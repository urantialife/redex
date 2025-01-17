/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "SideEffectSummary.h"

#include "CallGraph.h"
#include "ConcurrentContainers.h"
#include "Walkers.h"

using namespace side_effects;
using namespace sparta;

namespace ptrs = local_pointers;

namespace {

using reg_t = uint32_t;

using ParamInstructionMap =
    std::unordered_map<const IRInstruction*, param_idx_t>;

using PointersFixpointIteratorMap =
    ConcurrentMap<const DexMethodRef*, ptrs::FixpointIterator*>;

using SummaryConcurrentMap = ConcurrentMap<const DexMethodRef*, Summary>;

class SummaryBuilder final {
 public:
  SummaryBuilder(const InvokeToSummaryMap& invoke_to_summary_cmap,
                 const ptrs::FixpointIterator& ptrs_fp_iter,
                 const IRCode* code)
      : m_invoke_to_summary_cmap(invoke_to_summary_cmap),
        m_ptrs_fp_iter(ptrs_fp_iter),
        m_code(code) {
    auto idx = 0;
    for (auto& mie : InstructionIterable(code->get_param_instructions())) {
      auto insn = mie.insn;
      m_param_insn_map.emplace(insn, idx++);
    }
  }

  Summary build() {
    Summary summary;

    // Aggregate the effects of each individual instruction in the code object.
    auto& cfg = m_code->cfg();
    for (auto* block : cfg.blocks()) {
      auto env = m_ptrs_fp_iter.get_entry_state_at(block);
      if (env.is_bottom()) {
        continue;
      }
      for (auto& mie : InstructionIterable(block)) {
        auto* insn = mie.insn;
        analyze_instruction_effects(env, insn, &summary);
        m_ptrs_fp_iter.analyze_instruction(insn, &env);
      }
    }

    return summary;
  }

 private:
  void analyze_instruction_effects(const ptrs::Environment& env,
                                   const IRInstruction* insn,
                                   Summary* summary) {
    auto op = insn->opcode();
    switch (op) {
    case OPCODE_THROW: {
      summary->effects |= EFF_THROWS;
      break;
    }
    case OPCODE_MONITOR_ENTER:
    case OPCODE_MONITOR_EXIT: {
      summary->effects |= EFF_LOCKS;
      break;
    }
    case OPCODE_SPUT:
    case OPCODE_SPUT_WIDE:
    case OPCODE_SPUT_BOOLEAN:
    case OPCODE_SPUT_BYTE:
    case OPCODE_SPUT_CHAR:
    case OPCODE_SPUT_SHORT:
    case OPCODE_SPUT_OBJECT: {
      summary->effects |= EFF_WRITE_MAY_ESCAPE;
      break;
    }
    case OPCODE_IPUT:
    case OPCODE_IPUT_WIDE:
    case OPCODE_IPUT_BOOLEAN:
    case OPCODE_IPUT_BYTE:
    case OPCODE_IPUT_CHAR:
    case OPCODE_IPUT_SHORT:
    case OPCODE_IPUT_OBJECT:

    case OPCODE_APUT:
    case OPCODE_APUT_WIDE:
    case OPCODE_APUT_BOOLEAN:
    case OPCODE_APUT_BYTE:
    case OPCODE_APUT_CHAR:
    case OPCODE_APUT_SHORT:
    case OPCODE_APUT_OBJECT: {
      classify_heap_write(env, insn->src(1), summary);
      break;
    }

    case OPCODE_FILL_ARRAY_DATA: {
      classify_heap_write(env, insn->src(0), summary);
      break;
    }

    case OPCODE_INVOKE_SUPER:
    case OPCODE_INVOKE_INTERFACE: {
      TRACE(OSDCE, 3, "Unknown invoke: %s", SHOW(insn));
      summary->effects |= EFF_UNKNOWN_INVOKE;
      break;
    }
    case OPCODE_INVOKE_STATIC:
    case OPCODE_INVOKE_DIRECT:
    case OPCODE_INVOKE_VIRTUAL: {
      if (m_invoke_to_summary_cmap.count(insn)) {
        const auto& callee_summary = m_invoke_to_summary_cmap.at(insn);
        summary->effects |= callee_summary.effects;
        for (auto idx : callee_summary.modified_params) {
          classify_heap_write(env, insn->src(idx), summary);
        }
      } else {
        TRACE(OSDCE, 3, "Unknown invoke: %s", SHOW(insn));
        summary->effects |= EFF_UNKNOWN_INVOKE;
      }
      break;
    }
    default: {
      break;
    }
    }
  }

  /*
   * Given a write to the heap, classify it as one of the following:
   *   - Write to a locally-allocated non-escaping object
   *   - Write to an object passed in as a parameter
   *   - Write to an escaping and/or unknown object
   */
  void classify_heap_write(const ptrs::Environment& env,
                           reg_t modified_ptr_reg,
                           Summary* summary) {
    auto pointers = env.get_pointers(modified_ptr_reg);
    if (!pointers.is_value()) {
      summary->effects |= EFF_WRITE_MAY_ESCAPE;
      return;
    }
    for (auto insn : pointers.elements()) {
      if (!env.may_have_escaped(insn)) {
        if (insn->opcode() == IOPCODE_LOAD_PARAM_OBJECT) {
          summary->modified_params.emplace(m_param_insn_map.at(insn));
        }
      } else {
        TRACE(OSDCE, 3, "Escaping write to value allocated by %s",
              SHOW(insn));
        summary->effects |= EFF_WRITE_MAY_ESCAPE;
      }
    }
  }

  // Map of load-param instruction -> parameter index
  ParamInstructionMap m_param_insn_map;
  const InvokeToSummaryMap& m_invoke_to_summary_cmap;
  const ptrs::FixpointIterator& m_ptrs_fp_iter;
  const IRCode* m_code;
};

/*
 * Analyze :method and insert its summary into :summary_cmap. Recursively
 * analyze the callees if necessary. This method is thread-safe.
 */
void analyze_method_recursive(const DexMethod* method,
                              const call_graph::Graph& call_graph,
                              const ptrs::FixpointIteratorMap& ptrs_fp_iter_map,
                              PatriciaTreeSet<const DexMethodRef*> visiting,
                              SummaryConcurrentMap* summary_cmap) {
  if (summary_cmap->count(method) != 0 || visiting.contains(method) ||
      method->get_code() == nullptr) {
    return;
  }
  visiting.insert(method);

  std::unordered_map<const IRInstruction*, Summary> invoke_to_summary_cmap;
  if (call_graph.has_node(method)) {
    const auto& callee_edges = call_graph.node(method).callees();
    for (const auto& edge : callee_edges) {
      auto* callee = edge->callee();
      analyze_method_recursive(callee, call_graph, ptrs_fp_iter_map, visiting,
                               summary_cmap);
      if (summary_cmap->count(callee) != 0) {
        invoke_to_summary_cmap.emplace(edge->invoke_iterator()->insn,
                                       summary_cmap->at(callee));
      }
    }
  }

  const auto* ptrs_fp_iter = ptrs_fp_iter_map.find(method)->second;
  auto summary =
      SummaryBuilder(invoke_to_summary_cmap, *ptrs_fp_iter, method->get_code())
          .build();
  if (method->rstate.no_optimizations()) {
    summary.effects |= EFF_NO_OPTIMIZE;
  }
  summary_cmap->emplace(method, summary);

  if (traceEnabled(OSDCE, 3)) {
    TRACE(OSDCE, 3, "%s %s unknown side effects (%u)", SHOW(method),
          summary.effects != EFF_NONE ? "has" : "does not have",
          summary.effects);
    if (summary.modified_params.size() != 0) {
      TRACE(OSDCE, 3, "Modified params: ");
      for (auto idx : summary.modified_params) {
        TRACE(OSDCE, 3, "%u ", idx);
      }
      TRACE(OSDCE, 3, "");
    }
  }
}

} // namespace

namespace side_effects {

Summary analyze_code(const InvokeToSummaryMap& invoke_to_summary_cmap,
                     const ptrs::FixpointIterator& ptrs_fp_iter,
                     const IRCode* code) {
  return SummaryBuilder(invoke_to_summary_cmap, ptrs_fp_iter, code).build();
}

void analyze_scope(
    const Scope& scope,
    const call_graph::Graph& call_graph,
    const ConcurrentMap<const DexMethodRef*, ptrs::FixpointIterator*>&
        ptrs_fp_iter_map,
    SummaryMap* summary_map) {
  // This method is special: the bytecode verifier requires that this method
  // be called before a newly-allocated object gets used in any way. We can
  // model this by treating the method as modifying its `this` parameter --
  // changing it from uninitialized to initialized.
  (*summary_map)[DexMethod::get_method("Ljava/lang/Object;.<init>:()V")] =
      Summary({0});

  SummaryConcurrentMap summary_cmap;
  for (auto& pair : *summary_map) {
    summary_cmap.insert(pair);
  }

  walk::parallel::code(scope, [&](const DexMethod* method, IRCode& code) {
    PatriciaTreeSet<const DexMethodRef*> visiting;
    analyze_method_recursive(method, call_graph, ptrs_fp_iter_map, visiting,
                             &summary_cmap);
  });

  for (auto& pair : summary_cmap) {
    summary_map->insert(pair);
  }
}

s_expr to_s_expr(const Summary& summary) {
  std::vector<s_expr> s_exprs;
  s_exprs.emplace_back(std::to_string(summary.effects));
  std::vector<s_expr> mod_param_s_exprs;
  for (auto idx : summary.modified_params) {
    mod_param_s_exprs.emplace_back(idx);
  }
  s_exprs.emplace_back(mod_param_s_exprs);
  return s_expr(s_exprs);
}

Summary Summary::from_s_expr(const s_expr& expr) {
  Summary summary;
  always_assert(expr.size() == 2);
  always_assert(expr[0].is_string());
  summary.effects = std::stoi(expr[0].str());
  always_assert(expr[1].is_list());
  for (size_t i = 0; i < expr[1].size(); ++i) {
    summary.modified_params.emplace(expr[1][i].get_int32());
  }
  return summary;
}

} // namespace side_effects
