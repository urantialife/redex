/**
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "RegAlloc.h"

#include <boost/functional/hash.hpp>

#include "Dataflow.h"
#include "DexUtil.h"
#include "GraphColoring.h"
#include "IRCode.h"
#include "IRInstruction.h"
#include "LiveRange.h"
#include "Transform.h"
#include "Walkers.h"

#include "JemallocUtil.h"

using namespace regalloc;

void RegAllocPass::run_pass(DexStoresVector& stores,
                            ConfigFiles&,
                            PassManager& mgr) {
  regalloc::graph_coloring::Allocator::Config allocator_config;
  const auto& jw = mgr.get_current_pass_info()->config;
  jw.get("live_range_splitting", false, allocator_config.use_splitting);
  allocator_config.no_overwrite_this =
      mgr.get_redex_options().no_overwrite_this();

  using Output = graph_coloring::Allocator::Stats;
  auto scope = build_class_scope(stores);
  auto stats = walk::parallel::reduce_methods<Output>(
      scope,
      [&](DexMethod* m) { // mapper
        graph_coloring::Allocator::Stats stats;
        if (m->get_code() == nullptr) {
          return stats;
        }
        auto& code = *m->get_code();

        TRACE(REG, 3, "Handling %s:", SHOW(m));
        TRACE(REG,
              5,
              "regs:%d code:\n%s",
              code.get_registers_size(),
              SHOW(&code));
        try {
          live_range::renumber_registers(&code, /* width_aware */ true);
          // The transformations below all require a CFG. Build it once
          // here instead of requiring each transform to build it.
          code.build_cfg(/* editable */ false);
          graph_coloring::Allocator allocator(allocator_config);
          allocator.allocate(m);
          stats.accumulate(allocator.get_stats());

          TRACE(REG,
                5,
                "After alloc: regs:%d code:\n%s",
                code.get_registers_size(),
                SHOW(&code));
        } catch (std::exception&) {
          fprintf(stderr, "Failed to allocate %s\n", SHOW(m));
          fprintf(stderr, "%s\n", SHOW(code.cfg()));
          throw;
        }
        return stats;
      },
      [](Output a, Output b) { // reducer
        a.accumulate(b);
        return a;
      });

  TRACE(REG, 1, "Total reiteration count: %lu", stats.reiteration_count);
  TRACE(REG, 1, "Total Params spilled early: %lu", stats.params_spill_early);
  TRACE(REG, 1, "Total spill count: %lu", stats.moves_inserted());
  TRACE(REG, 1, "  Total param spills: %lu", stats.param_spill_moves);
  TRACE(REG, 1, "  Total range spills: %lu", stats.range_spill_moves);
  TRACE(REG, 1, "  Total global spills: %lu", stats.global_spill_moves);
  TRACE(REG, 1, "  Total splits: %lu", stats.split_moves);
  TRACE(REG, 1, "Total coalesce count: %lu", stats.moves_coalesced);
  TRACE(REG, 1, "Total net moves: %ld", stats.net_moves());

  mgr.incr_metric("param spilled too early", stats.params_spill_early);
  mgr.incr_metric("reiteration_count", stats.reiteration_count);
  mgr.incr_metric("spill_count", stats.moves_inserted());
  mgr.incr_metric("coalesce_count", stats.moves_coalesced);
  mgr.incr_metric("net_moves", stats.net_moves());

  mgr.record_running_regalloc();
}

static RegAllocPass s_pass;
