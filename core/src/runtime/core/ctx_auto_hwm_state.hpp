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
    //  Arms the debounce deadline without making a new request. An attach
    //  extension uses it: the plan it recorded answers every request already
    //  made, and the replan it still owes is the deadline itself — an armed
    //  deadline *is* the obligation to run a full pass, so nothing else has
    //  to record that a pass is owed.
    //  A deadline that is already armed is left alone, so the caller wakes
    //  the recalculation task exactly once per wait no matter how many
    //  extensions join it. Returns true only for that first arm.
    bool arm_debounce (uint64_t now_ms_, int debounce_ms_);
    //  Releases the wait. Only a completed full pass may call it: the pass is
    //  what the deadline was waiting for. A request that arrived while the
    //  pass ran keeps its own wait.
    void clear_debounce ();
    //  Remaining wait for an armed deadline, 0 when none is armed or it has
    //  already passed.
    uint64_t debounce_remaining_ms (uint64_t now_ms_) const;
    uint64_t pending_generation () const;
    uint64_t last_applied_generation () const;
    const auto_hwm_context_plan_t &applied_plan () const;
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
