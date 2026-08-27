/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_CTX_AUTO_HWM_STATE_HPP_INCLUDED__
#define __ZLINK_CTX_AUTO_HWM_STATE_HPP_INCLUDED__

#include "core/auto_hwm_policy.hpp"
#include "utils/stdint.hpp"
#include "zlink.h"

namespace zlink
{
class ctx_auto_hwm_state_t
{
  public:
    ctx_auto_hwm_state_t ();

    void set_enabled (bool enabled_);
    void set_recalc_debounce_ms (int debounce_ms_);
    void set_profile (zlink_auto_hwm_profile_t profile_);
    bool set_memory_limit_bytes (uint64_t memory_limit_bytes_);
    void set_runtime_memory_limit_bytes (uint64_t memory_limit_bytes_);
    bool set_core_budget_bytes (uint64_t budget_bytes_);

    bool enabled () const;
    int recalc_debounce_ms () const;
    zlink_auto_hwm_profile_t profile () const;
    uint64_t memory_limit_bytes () const;
    uint64_t runtime_memory_limit_bytes () const;
    uint64_t core_budget_bytes () const;
    auto_hwm_budget_input_t budget_input () const;

    uint64_t recalc_task_id () const;
    void set_recalc_task_id (uint64_t task_id_);
    uint64_t clear_recalc_task_id ();

    void schedule (uint64_t now_ms_, int debounce_ms_);
    uint64_t pending_generation () const;
    void record_applied_plan (const auto_hwm_context_plan_t &plan_,
                              uint64_t applied_generation_);
    bool recalc_due (uint64_t now_ms_) const;

    void copy_budget_snapshot (zlink_auto_hwm_budget_snapshot_t *out_) const;
    void reset_budget_metrics ();

  private:
    bool valid_explicit_limit (uint64_t value_) const;

    auto_hwm_budget_input_t _input;
    int _recalc_debounce_ms;
    bool _recalc_pending;
    uint64_t _recalc_deadline_ms;
    uint64_t _pending_generation;
    uint64_t _last_applied_generation;
    uint64_t _recalc_task_id;
    uint64_t _budget_generation;
    uint64_t _measurement_epoch;
    auto_hwm_context_plan_t _applied_plan;
};
}

#endif
