/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "core/auto_hwm_policy.hpp"
#include "core/ctx.hpp"
#include "runtime/core/control_runtime.hpp"
#include "sockets/common/socket_base.hpp"
#include "utils/clock.hpp"

namespace
{
uint64_t add_counter_saturating (uint64_t current_, uint64_t amount_,
                                 bool *overflow_)
{
    if (UINT64_MAX - current_ < amount_) {
        if (overflow_)
            *overflow_ = true;
        return UINT64_MAX;
    }
    return current_ + amount_;
}

uint32_t admission_ratio_ppm (uint64_t attempts_, uint64_t blocked_)
{
    if (attempts_ == 0 || blocked_ == 0)
        return 0;
    if (blocked_ >= attempts_)
        return 1000000;
    return static_cast<uint32_t> (
      static_cast<long double> (blocked_) * 1000000.0L
      / static_cast<long double> (attempts_));
}
}

void zlink::ctx_t::auto_hwm_recalc_task_main (void *arg_)
{
    static_cast<ctx_t *> (arg_)->auto_hwm_recalc_task ();
}

void zlink::ctx_t::ensure_auto_hwm_recalc_task_started ()
{
    if (_auto_hwm.recalc_task_id () != 0)
        return;

    control_runtime_t *runtime = control_runtime ();
    if (!runtime)
        return;

    _auto_hwm.set_recalc_task_id (
      runtime->add_periodic_task (&ctx_t::auto_hwm_recalc_task_main, this, 10, false));
}

void zlink::ctx_t::stop_auto_hwm_recalc_task ()
{
    const uint64_t task_id = _auto_hwm.clear_recalc_task_id ();
    if (task_id == 0)
        return;

    control_runtime_t *runtime = _runtime_resources.control_runtime ();
    if (runtime)
        (void) runtime->remove_task (task_id);
}

void zlink::ctx_t::schedule_auto_hwm_recalculate ()
{
    const uint64_t now_ms = zlink::clock_t ().now_ms ();
    int debounce_ms = 0;
    {
        scoped_lock_t locker (_opt_sync);
        debounce_ms = _auto_hwm.recalc_debounce_ms ();
    }

    {
        scoped_lock_t lock (_slot_sync);
        _auto_hwm.schedule (now_ms, debounce_ms);

        if (debounce_ms > 0) {
            ensure_auto_hwm_recalc_task_started ();
            if (_auto_hwm.recalc_task_id () != 0) {
                control_runtime_t *runtime = _runtime_resources.control_runtime ();
                if (runtime)
                    (void) runtime->wakeup_task (_auto_hwm.recalc_task_id ());
            }
        }
    }

    if (debounce_ms <= 0)
        (void) auto_hwm_recalculate_now ();
}

int zlink::ctx_t::auto_hwm_recalculate_now ()
{
    auto_hwm_budget_input_t input;
    {
        scoped_lock_t locker (_opt_sync);
        input = _auto_hwm.budget_input ();
    }

    scoped_lock_t runtime_lock (_slot_sync);

    std::vector<socket_base_t *> sockets;
    _socket_registry.collect_sockets (&sockets);

    auto_hwm_context_plan_t context_plan;
    auto_hwm_context_plan_make (input, &context_plan);

    std::vector<physical_queue_endpoint_policy_t> queue_policies;
    for (size_t i = 0; i < sockets.size (); ++i) {
        socket_base_t *socket = sockets[i];
        if (!socket)
            continue;
        socket->collect_auto_hwm_queue_policies (&queue_policies);
    }

    _physical_queue_registry.plan_application_queues (&context_plan,
                                                       queue_policies);

    for (size_t i = 0; i < sockets.size (); ++i) {
        if (!sockets[i])
            continue;
        sockets[i]->apply_physical_auto_hwm_plan (
          context_plan, ZLINK_AUTO_HWM_RECALC_REASON_REFRESH);
    }
    _auto_hwm.record_applied_plan (context_plan);
    return 0;
}

void zlink::ctx_t::auto_hwm_recalc_task ()
{
    bool should_run = false;
    {
        scoped_lock_t lock (_slot_sync);
        if (_auto_hwm.recalc_due (zlink::clock_t ().now_ms ()))
            should_run = true;
    }

    if (should_run)
        (void) auto_hwm_recalculate_now ();
}

zlink::auto_hwm_budget_input_t zlink::ctx_t::auto_hwm_budget_input () const
{
    scoped_lock_t locker (const_cast<mutex_t &> (_opt_sync));
    return _auto_hwm.budget_input ();
}

int zlink::ctx_t::create_pipepair_queues (
  uint64_t first_direction_hwm_, uint64_t second_direction_hwm_,
  physical_queue_class_t queue_class_, auto_hwm_role_t role_,
  bool planning_enabled_,
  physical_queue_handle_t *first_direction_,
  physical_queue_handle_t *second_direction_)
{
    scoped_lock_t locker (_opt_sync);
    auto_hwm_context_plan_t context_plan;
    auto_hwm_context_plan_make (_auto_hwm.budget_input (), &context_plan);
    return _physical_queue_registry.create_pipepair_queues (
      first_direction_hwm_, second_direction_hwm_, queue_class_, role_,
      planning_enabled_, context_plan, first_direction_, second_direction_);
}

void zlink::ctx_t::record_auto_hwm_endpoint_policy (
  const physical_queue_endpoint_policy_t &policy_)
{
    _physical_queue_registry.record_endpoint_policy (policy_);
}

void zlink::ctx_t::record_auto_hwm_admission_attempt (
  bool blocked_by_target_hwm_)
{
    _physical_queue_registry.record_admission_attempt (
      blocked_by_target_hwm_);
}

int zlink::ctx_t::auto_hwm_budget_snapshot (zlink_auto_hwm_budget_snapshot_t *out_)
{
    scoped_lock_t locker (_slot_sync);
    if (_terminating) {
        errno = ETERM;
        return -1;
    }
    _auto_hwm.copy_budget_snapshot (out_);
    physical_queue_registry_snapshot_t queues;
    _physical_queue_registry.snapshot (&queues);
    std::vector<socket_base_t *> sockets;
    _socket_registry.collect_sockets (&sockets);
    uint64_t sampled_oversize_count = 0;
    uint64_t sampled_oversize_max = 0;
    uint64_t attempts = 0;
    uint64_t blocked = 0;
    bool sampled_overflow = false;
    for (std::vector<socket_base_t *>::const_iterator it = sockets.begin ();
         it != sockets.end (); ++it) {
        uint64_t socket_oversize_count = 0;
        uint64_t socket_oversize_max = 0;
        (*it)->auto_hwm_queue_counters (NULL,
                                        &socket_oversize_count,
                                        &socket_oversize_max);
        sampled_oversize_count = add_counter_saturating (
          sampled_oversize_count, socket_oversize_count, &sampled_overflow);
        sampled_oversize_max =
          std::max (sampled_oversize_max, socket_oversize_max);
        uint64_t socket_attempts = 0;
        uint64_t socket_blocked = 0;
        (*it)->auto_hwm_admission_counters (&socket_attempts,
                                            &socket_blocked);
        attempts = add_counter_saturating (attempts, socket_attempts,
                                           &sampled_overflow);
        blocked = add_counter_saturating (blocked, socket_blocked,
                                          &sampled_overflow);
    }
    out_->active_directional_queue_count =
      queues.active_application_direction_count;
    out_->active_completion_directional_queue_count =
      queues.active_completion_direction_count;
    out_->retired_queue_count = queues.retired_direction_count;
    out_->application_accounted_bytes =
      queues.application_lease_accounted_bytes;
    out_->core_queue_accounted_bytes =
      queues.application_current_accounted_bytes;
    out_->outstanding_application_lease_count =
      queues.outstanding_application_lease_count;
    out_->deferred_origin_credit_bytes =
      queues.deferred_origin_credit_bytes;
    bool application_sum_overflow = false;
    out_->current_accounted_bytes = add_counter_saturating (
      out_->core_queue_accounted_bytes, out_->application_accounted_bytes,
      &application_sum_overflow);
    out_->provisional_accounted_bytes =
      queues.application_provisional_accounted_bytes;
    out_->peak_accounted_bytes = std::max (
      queues.application_peak_accounted_bytes, out_->current_accounted_bytes);
    out_->completion_current_accounted_bytes =
      queues.completion_current_accounted_bytes;
    out_->completion_peak_accounted_bytes =
      queues.completion_peak_accounted_bytes;
    out_->completion_pending_message_count =
      queues.completion_pending_message_count;
    const bool messaging_sum_overflow =
      UINT64_MAX - out_->current_accounted_bytes
      < out_->completion_current_accounted_bytes;
    out_->total_messaging_accounted_bytes =
      messaging_sum_overflow
        ? UINT64_MAX
        : out_->current_accounted_bytes
            + out_->completion_current_accounted_bytes;
    out_->monitor_queue_applied_hwm_bytes =
      queues.monitor_applied_hwm_bytes;
    out_->monitor_queue_accounted_bytes =
      queues.monitor_current_accounted_bytes;
    const bool instance_applied_sum_overflow =
      UINT64_MAX - out_->total_applied_hwm_bytes
      < out_->monitor_queue_applied_hwm_bytes;
    out_->total_instance_applied_hwm_bytes =
      instance_applied_sum_overflow
        ? UINT64_MAX
        : out_->total_applied_hwm_bytes
            + out_->monitor_queue_applied_hwm_bytes;
    const bool instance_accounted_sum_overflow =
      UINT64_MAX - out_->total_messaging_accounted_bytes
      < out_->monitor_queue_accounted_bytes;
    out_->total_instance_accounted_bytes =
      instance_accounted_sum_overflow
        ? UINT64_MAX
        : out_->total_messaging_accounted_bytes
            + out_->monitor_queue_accounted_bytes;
    out_->oversize_admission_count = sampled_oversize_count;
    out_->largest_oversize_message_bytes = sampled_oversize_max;
    out_->blocked_ratio_ppm = admission_ratio_ppm (attempts, blocked);
    if (queues.aggregate_overflow || application_sum_overflow
        || messaging_sum_overflow || instance_applied_sum_overflow
        || instance_accounted_sum_overflow || sampled_overflow)
        out_->flags |= ZLINK_AUTO_HWM_BUDGET_FLAG_AGGREGATE_OVERFLOW;
    return 0;
}

int zlink::ctx_t::reset_auto_hwm_budget_metrics ()
{
    scoped_lock_t locker (_slot_sync);
    if (_terminating) {
        errno = ETERM;
        return -1;
    }
    _auto_hwm.reset_budget_metrics ();
    _physical_queue_registry.reset_metrics ();
    std::vector<socket_base_t *> sockets;
    _socket_registry.collect_sockets (&sockets);
    for (std::vector<socket_base_t *>::const_iterator it = sockets.begin ();
         it != sockets.end (); ++it)
        (*it)->reset_auto_hwm_admission_counters ();
    return 0;
}
