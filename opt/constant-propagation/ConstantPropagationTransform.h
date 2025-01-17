/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "ConstantEnvironment.h"
#include "ConstantPropagationAnalysis.h"
#include "ConstantPropagationWholeProgramState.h"
#include "IRCode.h"

namespace constant_propagation {

/**
 * Optimize the given code by:
 *   - removing dead branches
 *   - converting instructions to `const` when the values are known
 *   - removing field writes if they all write the same constant value
 */
class Transform final {
 public:
  struct Config {
    bool replace_moves_with_consts{true};
    const DexType* class_under_init{nullptr};
    Config() {}
  };

  struct Stats {
    size_t branches_removed{0};
    size_t materialized_consts{0};
    Stats operator+(const Stats& that) const {
      Stats result;
      result.branches_removed = branches_removed + that.branches_removed;
      result.materialized_consts =
          materialized_consts + that.materialized_consts;
      return result;
    }
  };

  explicit Transform(Config config = Config()) : m_config(config) {}

  Stats apply(const intraprocedural::FixpointIterator&,
              const WholeProgramState&,
              IRCode*);

 private:
  /*
   * The methods in this class queue up their transformations. After they are
   * all done, the apply_changes() method does the actual modification of the
   * IRCode.
   */
  void apply_changes(IRCode*);

  void simplify_instruction(const ConstantEnvironment&,
                            const WholeProgramState& wps,
                            IRList::iterator);

  void replace_with_const(const ConstantEnvironment&, IRList::iterator);

  void eliminate_redundant_put(const ConstantEnvironment&,
                               const WholeProgramState& wps,
                               IRList::iterator);

  void eliminate_dead_branch(const intraprocedural::FixpointIterator&,
                             const ConstantEnvironment&,
                             cfg::Block*);

  const Config m_config;
  std::vector<std::pair<IRInstruction*, std::vector<IRInstruction*>>>
      m_replacements;
  std::vector<IRList::iterator> m_deletes;
  Stats m_stats;
};

/*
 * Generates an appropriate const-* instruction for a given ConstantValue.
 */
class value_to_instruction_visitor final
    : public boost::static_visitor<std::vector<IRInstruction*>> {
 public:
  value_to_instruction_visitor(const IRInstruction* original)
      : m_original(original) {}

  std::vector<IRInstruction*> operator()(
      const SignedConstantDomain& dom) const {
    auto cst = dom.get_constant();
    if (!cst) {
      return {};
    }
    IRInstruction* insn = new IRInstruction(
        m_original->dest_is_wide() ? OPCODE_CONST_WIDE : OPCODE_CONST);
    insn->set_literal(*cst);
    insn->set_dest(m_original->dest());
    return {insn};
  }

  std::vector<IRInstruction*> operator()(const StringDomain& dom) const {
    auto cst = dom.get_constant();
    if (!cst) {
      return {};
    }
    IRInstruction* insn = new IRInstruction(OPCODE_CONST_STRING);
    insn->set_string(const_cast<DexString*>(*cst));
    return {insn, (new IRInstruction(IOPCODE_MOVE_RESULT_PSEUDO_OBJECT))
                      ->set_dest(m_original->dest())};
  }

  template <typename Domain>
  std::vector<IRInstruction*> operator()(const Domain& dom) const {
    return {};
  }

 private:
  const IRInstruction* m_original;
};

} // namespace constant_propagation
