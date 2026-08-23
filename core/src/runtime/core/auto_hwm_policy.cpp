/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "core/auto_hwm_policy.hpp"
#include "zlink.h"

#include <algorithm>
#include <vector>

namespace
{
const uint64_t kib = 1024ull;
const uint64_t mib = 1024ull * kib;

struct profile_budget_t
{
    uint32_t percent;
    uint64_t data_minimum;
    uint64_t data_maximum;
    uint64_t stream_minimum;
    uint64_t stream_maximum;
};

zlink_auto_hwm_profile_t normalize_profile (zlink_auto_hwm_profile_t profile_)
{
    switch (profile_) {
        case ZLINK_AUTO_HWM_PROFILE_COMPACT:
        case ZLINK_AUTO_HWM_PROFILE_LOW_LATENCY:
        case ZLINK_AUTO_HWM_PROFILE_BALANCED:
        case ZLINK_AUTO_HWM_PROFILE_THROUGHPUT:
            return profile_;
        default:
            return ZLINK_AUTO_HWM_PROFILE_BALANCED;
    }
}

profile_budget_t profile_budget (zlink_auto_hwm_profile_t profile_)
{
    switch (normalize_profile (profile_)) {
        case ZLINK_AUTO_HWM_PROFILE_COMPACT:
            return profile_budget_t{2, 32 * kib, 1 * mib, 8 * kib, 32 * kib};
        case ZLINK_AUTO_HWM_PROFILE_LOW_LATENCY:
            return profile_budget_t{5, 32 * kib, 2 * mib, 16 * kib, 64 * kib};
        case ZLINK_AUTO_HWM_PROFILE_THROUGHPUT:
            return profile_budget_t{20, 128 * kib, 16 * mib, 256 * kib, 512 * kib};
        case ZLINK_AUTO_HWM_PROFILE_BALANCED:
        default:
            return profile_budget_t{20, 64 * kib, 1 * mib, 64 * kib, 128 * kib};
    }
}

bool stream_role (zlink::auto_hwm_role_t role_)
{
    return role_ == zlink::auto_hwm_role_stream;
}

uint64_t saturating_add (uint64_t left_, uint64_t right_, bool *overflow_)
{
    if (UINT64_MAX - left_ < right_) {
        if (overflow_)
            *overflow_ = true;
        return UINT64_MAX;
    }
    return left_ + right_;
}

uint64_t saturating_multiply (uint64_t left_, uint64_t right_, bool *overflow_)
{
    if (left_ != 0 && right_ > UINT64_MAX / left_) {
        if (overflow_)
            *overflow_ = true;
        return UINT64_MAX;
    }
    return left_ * right_;
}

uint64_t apply_profile_percent (uint64_t memory_bytes_, uint32_t percent_)
{
    return (memory_bytes_ / 100u) * percent_
           + ((memory_bytes_ % 100u) * percent_) / 100u;
}

uint64_t resolved_memory_limit (const zlink::auto_hwm_budget_input_t &input_)
{
    if (input_.configured_memory_limit_bytes > 0)
        return input_.configured_memory_limit_bytes;

    if (input_.runtime_memory_limit_bytes > 0 && input_.detected_hard_limit_bytes > 0) {
        return std::min (input_.runtime_memory_limit_bytes,
                         input_.detected_hard_limit_bytes);
    }
    if (input_.runtime_memory_limit_bytes > 0)
        return input_.runtime_memory_limit_bytes;
    if (input_.detected_hard_limit_bytes > 0)
        return input_.detected_hard_limit_bytes;
    return input_.detected_physical_memory_bytes;
}

struct direction_plan_ref_t
{
    uint64_t *hwm;
    uint64_t queue_count;
    uint64_t maximum;
};

void append_direction (std::vector<direction_plan_ref_t> *directions_,
                       uint64_t *hwm_,
                       uint64_t queue_count_,
                       uint64_t maximum_)
{
    if (directions_ && hwm_ && queue_count_ > 0 && *hwm_ < maximum_)
        directions_->push_back (direction_plan_ref_t{hwm_, queue_count_, maximum_});
}
}

zlink::auto_hwm_budget_input_t::auto_hwm_budget_input_t () :
    enabled (false),
    profile (ZLINK_AUTO_HWM_PROFILE_BALANCED),
    configured_memory_limit_bytes (0),
    runtime_memory_limit_bytes (0),
    configured_core_budget_bytes (0),
    detected_hard_limit_bytes (0),
    detected_physical_memory_bytes (0)
{
}

zlink::auto_hwm_context_plan_t::auto_hwm_context_plan_t () :
    enabled (false),
    profile (ZLINK_AUTO_HWM_PROFILE_BALANCED),
    configured_memory_limit_bytes (0),
    runtime_memory_limit_bytes (0),
    resolved_memory_limit_bytes (0),
    configured_core_budget_bytes (0),
    effective_core_budget_bytes (0),
    total_planned_hwm_bytes (0),
    total_applied_hwm_bytes (0),
    manual_reserved_hwm_bytes (0),
    active_directional_queue_count (0),
    active_send_queue_count (0),
    active_receive_queue_count (0),
    unlimited_manual_queue_count (0),
    budget_insufficient (false),
    aggregate_hwm_valid (true),
    aggregate_overflow (false)
{
}

zlink::auto_hwm_socket_plan_t::auto_hwm_socket_plan_t () :
    role (auto_hwm_role_none),
    policy_class (auto_hwm_policy_none),
    planning_enabled (false),
    send_queue_count (0),
    receive_queue_count (0),
    minimum_hwm_bytes (0),
    maximum_hwm_bytes (0),
    pending_bytes (0),
    manual_sndhwm (false),
    manual_rcvhwm (false),
    sndhwm (0),
    rcvhwm (0)
{
}

bool zlink::auto_hwm_valid_profile (int profile_)
{
    return profile_ == ZLINK_AUTO_HWM_PROFILE_COMPACT
           || profile_ == ZLINK_AUTO_HWM_PROFILE_LOW_LATENCY
           || profile_ == ZLINK_AUTO_HWM_PROFILE_BALANCED
           || profile_ == ZLINK_AUTO_HWM_PROFILE_THROUGHPUT;
}

uint64_t zlink::auto_hwm_profile_minimum_bytes (zlink_auto_hwm_profile_t profile_,
                                                auto_hwm_role_t role_)
{
    if (role_ == auto_hwm_role_none)
        return 0;
    const profile_budget_t budget = profile_budget (profile_);
    return stream_role (role_) ? budget.stream_minimum : budget.data_minimum;
}

uint64_t zlink::auto_hwm_profile_maximum_bytes (zlink_auto_hwm_profile_t profile_,
                                                auto_hwm_role_t role_)
{
    if (role_ == auto_hwm_role_none)
        return 0;
    const profile_budget_t budget = profile_budget (profile_);
    if (normalize_profile (profile_) == ZLINK_AUTO_HWM_PROFILE_BALANCED
        && role_ == auto_hwm_role_recv_ingress)
        return 2 * mib;
    return stream_role (role_) ? budget.stream_maximum : budget.data_maximum;
}

zlink::auto_hwm_role_t zlink::auto_hwm_default_role_for_socket_type (int socket_type_)
{
    switch (socket_type_) {
        case ZLINK_CORE_SOCKET_PAIR:
        case ZLINK_CORE_SOCKET_DEALER:
            return auto_hwm_role_peer_queue;
        case ZLINK_CORE_SOCKET_ROUTER:
            return auto_hwm_role_routed;
        case ZLINK_CORE_SOCKET_STREAM:
            return auto_hwm_role_stream;
        case ZLINK_CORE_SOCKET_PUB:
        case ZLINK_CORE_SOCKET_XPUB:
            return auto_hwm_role_fanout;
        case ZLINK_CORE_SOCKET_SUB:
        case ZLINK_CORE_SOCKET_XSUB:
            return auto_hwm_role_recv_ingress;
        default:
            return auto_hwm_role_none;
    }
}

zlink::auto_hwm_policy_class_t zlink::auto_hwm_policy_class_for_role (
  auto_hwm_role_t role_, int socket_type_)
{
    switch (role_) {
        case auto_hwm_role_control:
            return auto_hwm_policy_control;
        case auto_hwm_role_routed:
            return auto_hwm_policy_routed;
        case auto_hwm_role_fanout:
            return auto_hwm_policy_fanout;
        case auto_hwm_role_connection_data:
            return auto_hwm_policy_connection_data;
        case auto_hwm_role_recv_ingress:
            return auto_hwm_policy_recv_ingress;
        case auto_hwm_role_peer_queue:
            return auto_hwm_policy_peer_queue;
        case auto_hwm_role_stream:
            return auto_hwm_policy_stream;
        case auto_hwm_role_none:
        default:
            break;
    }

    const auto_hwm_role_t default_role =
      auto_hwm_default_role_for_socket_type (socket_type_);
    if (default_role == auto_hwm_role_none)
        return auto_hwm_policy_none;
    return auto_hwm_policy_class_for_role (default_role, 0);
}

void zlink::auto_hwm_context_plan_make (const auto_hwm_budget_input_t &input_,
                                        auto_hwm_context_plan_t *out_)
{
    if (!out_)
        return;

    *out_ = auto_hwm_context_plan_t ();
    out_->enabled = input_.enabled;
    out_->profile = normalize_profile (input_.profile);
    out_->configured_memory_limit_bytes = input_.configured_memory_limit_bytes;
    out_->runtime_memory_limit_bytes = input_.runtime_memory_limit_bytes;
    out_->configured_core_budget_bytes = input_.configured_core_budget_bytes;
    out_->resolved_memory_limit_bytes = resolved_memory_limit (input_);
    out_->effective_core_budget_bytes =
      input_.configured_core_budget_bytes > 0
        ? input_.configured_core_budget_bytes
        : apply_profile_percent (out_->resolved_memory_limit_bytes,
                                 profile_budget (out_->profile).percent);
}

void zlink::auto_hwm_socket_plan_prepare (auto_hwm_role_t role_,
                                          uint64_t send_queue_count_,
                                          uint64_t receive_queue_count_,
                                          bool manual_sndhwm_,
                                          uint64_t sndhwm_,
                                          bool manual_rcvhwm_,
                                          uint64_t rcvhwm_,
                                          bool planning_enabled_,
                                          auto_hwm_socket_plan_t *out_)
{
    if (!out_)
        return;

    *out_ = auto_hwm_socket_plan_t ();
    out_->role = role_;
    out_->policy_class = auto_hwm_policy_class_for_role (role_, 0);
    out_->planning_enabled = planning_enabled_ && role_ != auto_hwm_role_none;
    out_->send_queue_count = send_queue_count_;
    out_->receive_queue_count = receive_queue_count_;
    out_->manual_sndhwm = manual_sndhwm_;
    out_->manual_rcvhwm = manual_rcvhwm_;
    out_->sndhwm = sndhwm_;
    out_->rcvhwm = rcvhwm_;
}

void zlink::auto_hwm_context_finalize (auto_hwm_context_plan_t *context_,
                                       auto_hwm_socket_plan_t *plans_,
                                       size_t plan_count_)
{
    if (!context_ || (!plans_ && plan_count_ != 0))
        return;

    context_->total_planned_hwm_bytes = 0;
    context_->total_applied_hwm_bytes = 0;
    context_->manual_reserved_hwm_bytes = 0;
    context_->active_directional_queue_count = 0;
    context_->active_send_queue_count = 0;
    context_->active_receive_queue_count = 0;
    context_->unlimited_manual_queue_count = 0;
    context_->budget_insufficient = false;
    context_->aggregate_hwm_valid = true;
    context_->aggregate_overflow = false;

    uint64_t auto_minimum_total = 0;
    std::vector<direction_plan_ref_t> auto_directions;
    auto_directions.reserve (plan_count_ * 2);

    for (size_t i = 0; i != plan_count_; ++i) {
        auto_hwm_socket_plan_t &plan = plans_[i];
        plan.minimum_hwm_bytes =
          auto_hwm_profile_minimum_bytes (context_->profile, plan.role);
        plan.maximum_hwm_bytes =
          auto_hwm_profile_maximum_bytes (context_->profile, plan.role);

        if (!plan.planning_enabled)
            continue;

        context_->active_send_queue_count = saturating_add (
          context_->active_send_queue_count, plan.send_queue_count,
          &context_->aggregate_overflow);
        context_->active_receive_queue_count = saturating_add (
          context_->active_receive_queue_count, plan.receive_queue_count,
          &context_->aggregate_overflow);
        context_->active_directional_queue_count = saturating_add (
          context_->active_directional_queue_count,
          saturating_add (plan.send_queue_count, plan.receive_queue_count,
                          &context_->aggregate_overflow),
          &context_->aggregate_overflow);

        if (!plan.manual_sndhwm) {
            plan.sndhwm = plan.minimum_hwm_bytes;
            const uint64_t minimum = saturating_multiply (
              plan.minimum_hwm_bytes, plan.send_queue_count,
              &context_->aggregate_overflow);
            auto_minimum_total = saturating_add (
              auto_minimum_total, minimum, &context_->aggregate_overflow);
            append_direction (&auto_directions, &plan.sndhwm,
                              plan.send_queue_count, plan.maximum_hwm_bytes);
        } else if (plan.send_queue_count > 0) {
            const uint64_t reservation =
              plan.sndhwm == 0 ? plan.maximum_hwm_bytes : plan.sndhwm;
            context_->manual_reserved_hwm_bytes = saturating_add (
              context_->manual_reserved_hwm_bytes,
              saturating_multiply (reservation, plan.send_queue_count,
                                   &context_->aggregate_overflow),
              &context_->aggregate_overflow);
            if (plan.sndhwm == 0) {
                context_->aggregate_hwm_valid = false;
                context_->unlimited_manual_queue_count = saturating_add (
                  context_->unlimited_manual_queue_count, plan.send_queue_count,
                  &context_->aggregate_overflow);
            }
        }

        if (!plan.manual_rcvhwm) {
            plan.rcvhwm = plan.minimum_hwm_bytes;
            const uint64_t minimum = saturating_multiply (
              plan.minimum_hwm_bytes, plan.receive_queue_count,
              &context_->aggregate_overflow);
            auto_minimum_total = saturating_add (
              auto_minimum_total, minimum, &context_->aggregate_overflow);
            append_direction (&auto_directions, &plan.rcvhwm,
                              plan.receive_queue_count, plan.maximum_hwm_bytes);
        } else if (plan.receive_queue_count > 0) {
            const uint64_t reservation =
              plan.rcvhwm == 0 ? plan.maximum_hwm_bytes : plan.rcvhwm;
            context_->manual_reserved_hwm_bytes = saturating_add (
              context_->manual_reserved_hwm_bytes,
              saturating_multiply (reservation, plan.receive_queue_count,
                                   &context_->aggregate_overflow),
              &context_->aggregate_overflow);
            if (plan.rcvhwm == 0) {
                context_->aggregate_hwm_valid = false;
                context_->unlimited_manual_queue_count = saturating_add (
                  context_->unlimited_manual_queue_count, plan.receive_queue_count,
                  &context_->aggregate_overflow);
            }
        }
    }

    const uint64_t data_budget =
      context_->manual_reserved_hwm_bytes >= context_->effective_core_budget_bytes
        ? 0
        : context_->effective_core_budget_bytes
            - context_->manual_reserved_hwm_bytes;

    if (context_->manual_reserved_hwm_bytes > context_->effective_core_budget_bytes
        || auto_minimum_total > data_budget || context_->aggregate_overflow) {
        context_->budget_insufficient = true;
    } else if (context_->enabled) {
        uint64_t remaining = data_budget - auto_minimum_total;
        while (remaining > 0) {
            uint64_t unsaturated_count = 0;
            for (size_t i = 0; i != auto_directions.size (); ++i) {
                if (*auto_directions[i].hwm < auto_directions[i].maximum) {
                    unsaturated_count = saturating_add (
                      unsaturated_count, auto_directions[i].queue_count,
                      &context_->aggregate_overflow);
                }
            }
            if (unsaturated_count == 0 || context_->aggregate_overflow)
                break;

            const uint64_t share = remaining / unsaturated_count;
            bool granted = false;
            if (share > 0) {
                for (size_t i = 0; i != auto_directions.size (); ++i) {
                    direction_plan_ref_t &direction = auto_directions[i];
                    if (*direction.hwm >= direction.maximum)
                        continue;
                    const uint64_t headroom = direction.maximum - *direction.hwm;
                    const uint64_t grant_per_queue = std::min (
                      std::min (share, headroom),
                      remaining / direction.queue_count);
                    if (grant_per_queue == 0)
                        continue;
                    *direction.hwm += grant_per_queue;
                    remaining -= grant_per_queue * direction.queue_count;
                    granted = true;
                }
            } else {
                for (size_t i = 0; i != auto_directions.size (); ++i) {
                    direction_plan_ref_t &direction = auto_directions[i];
                    if (*direction.hwm >= direction.maximum
                        || remaining < direction.queue_count) {
                        continue;
                    }
                    ++*direction.hwm;
                    remaining -= direction.queue_count;
                    granted = true;
                }
            }
            if (!granted)
                break;
        }
    }

    uint64_t planned_auto_total = 0;
    for (size_t i = 0; i != plan_count_; ++i) {
        const auto_hwm_socket_plan_t &plan = plans_[i];
        if (!plan.planning_enabled)
            continue;
        if (!plan.manual_sndhwm) {
            planned_auto_total = saturating_add (
              planned_auto_total,
              saturating_multiply (plan.sndhwm, plan.send_queue_count,
                                   &context_->aggregate_overflow),
              &context_->aggregate_overflow);
        }
        if (!plan.manual_rcvhwm) {
            planned_auto_total = saturating_add (
              planned_auto_total,
              saturating_multiply (plan.rcvhwm, plan.receive_queue_count,
                                   &context_->aggregate_overflow),
              &context_->aggregate_overflow);
        }
    }
    context_->total_planned_hwm_bytes = saturating_add (
      planned_auto_total, context_->manual_reserved_hwm_bytes,
      &context_->aggregate_overflow);
    context_->total_applied_hwm_bytes = context_->total_planned_hwm_bytes;
    if (context_->aggregate_overflow)
        context_->budget_insufficient = true;
}
