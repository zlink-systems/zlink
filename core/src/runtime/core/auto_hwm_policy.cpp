/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "core/auto_hwm_policy.hpp"
#include "core/internal_defs.hpp"
#include "utils/err.hpp"
#include "zlink.h"

#include <algorithm>
#include <limits.h>

namespace
{
const uint64_t auto_hwm_stream_message_bytes = 1024ull;
const uint64_t auto_hwm_message_bytes = 4096ull;

struct profile_hwm_t
{
    uint32_t message_hwm;
    uint32_t stream_hwm;
    uint32_t control_hwm;
    uint32_t message_cap;
    uint32_t stream_cap;
};

struct connection_bucket_hwm_t
{
    uint32_t max_peers;
    uint32_t compact_hwm;
    uint32_t low_latency_hwm;
    uint32_t balanced_hwm;
    uint32_t throughput_hwm;
};

const uint32_t unlimited_peer_bucket = UINT32_MAX;

const connection_bucket_hwm_t connection_buckets[] = {
  {64, 64, 128, 256, 512},
  {128, 64, 64, 128, 256},
  {512, 32, 32, 64, 128},
  {2048, 16, 16, 32, 64},
  {unlimited_peer_bucket, 8, 8, 16, 32}};

const uint32_t connection_bucket_count =
  sizeof connection_buckets / sizeof connection_buckets[0];

uint32_t clamp_size_to_u32 (size_t value_)
{
    return value_ > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t> (value_);
}

uint64_t saturating_multiply_u64 (uint64_t left_, uint64_t right_)
{
    if (left_ != 0 && right_ > UINT64_MAX / left_)
        return UINT64_MAX;
    return left_ * right_;
}

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

profile_hwm_t profile_hwm (zlink_auto_hwm_profile_t profile_)
{
    switch (normalize_profile (profile_)) {
        case ZLINK_AUTO_HWM_PROFILE_COMPACT:
            return profile_hwm_t{64, 8, 8, 256, 32};
        case ZLINK_AUTO_HWM_PROFILE_LOW_LATENCY:
            return profile_hwm_t{128, 16, 16, 512, 64};
        case ZLINK_AUTO_HWM_PROFILE_THROUGHPUT:
            return profile_hwm_t{512, 256, 32, 4096, 512};
        case ZLINK_AUTO_HWM_PROFILE_BALANCED:
        default:
            return profile_hwm_t{256, 64, 16, 1024, 128};
    }
}

bool stream_policy_class (zlink::auto_hwm_policy_class_t policy_class_)
{
    return policy_class_ == zlink::auto_hwm_policy_stream;
}

uint32_t basis_hwm_for_class (zlink_auto_hwm_profile_t profile_,
                              zlink::auto_hwm_policy_class_t policy_class_)
{
    const profile_hwm_t hwm = profile_hwm (profile_);
    switch (policy_class_) {
        case zlink::auto_hwm_policy_fanout:
        case zlink::auto_hwm_policy_connection_data:
        case zlink::auto_hwm_policy_routed:
        case zlink::auto_hwm_policy_recv_ingress:
        case zlink::auto_hwm_policy_peer_queue:
            return hwm.message_hwm;
        case zlink::auto_hwm_policy_stream:
            return hwm.stream_hwm;
        case zlink::auto_hwm_policy_control:
            return hwm.control_hwm;
        default:
            return 0;
    }
}

uint32_t size_cap_for_class (zlink_auto_hwm_profile_t profile_,
                             zlink::auto_hwm_policy_class_t policy_class_)
{
    if (policy_class_ == zlink::auto_hwm_policy_none)
        return 0;
    const profile_hwm_t hwm = profile_hwm (profile_);
    return stream_policy_class (policy_class_) ? hwm.stream_cap : hwm.message_cap;
}

uint32_t routed_small_message_cap_for_profile (zlink_auto_hwm_profile_t profile_)
{
    switch (normalize_profile (profile_)) {
        case ZLINK_AUTO_HWM_PROFILE_COMPACT:
            return 32;
        case ZLINK_AUTO_HWM_PROFILE_LOW_LATENCY:
            return 64;
        case ZLINK_AUTO_HWM_PROFILE_THROUGHPUT:
            return 128;
        case ZLINK_AUTO_HWM_PROFILE_BALANCED:
        default:
            return 128;
    }
}

bool connection_bucket_policy_class (zlink::auto_hwm_policy_class_t policy_class_)
{
    return policy_class_ == zlink::auto_hwm_policy_connection_data
           || policy_class_ == zlink::auto_hwm_policy_recv_ingress
           || policy_class_ == zlink::auto_hwm_policy_routed;
}

uint32_t bucket_hwm_for_profile (const connection_bucket_hwm_t &bucket_,
                                 zlink_auto_hwm_profile_t profile_)
{
    switch (normalize_profile (profile_)) {
        case ZLINK_AUTO_HWM_PROFILE_COMPACT:
            return bucket_.compact_hwm;
        case ZLINK_AUTO_HWM_PROFILE_LOW_LATENCY:
            return bucket_.low_latency_hwm;
        case ZLINK_AUTO_HWM_PROFILE_THROUGHPUT:
            return bucket_.throughput_hwm;
        case ZLINK_AUTO_HWM_PROFILE_BALANCED:
        default:
            return bucket_.balanced_hwm;
    }
}

uint32_t connection_bucket_hwm_4k (zlink_auto_hwm_profile_t profile_, uint32_t connections_)
{
    const uint32_t peers = std::max<uint32_t> (connections_, 1u);
    for (uint32_t i = 0; i != connection_bucket_count; ++i) {
        if (peers <= connection_buckets[i].max_peers)
            return bucket_hwm_for_profile (connection_buckets[i], profile_);
    }
    return bucket_hwm_for_profile (
      connection_buckets[connection_bucket_count - 1],
      profile_);
}

uint32_t connection_bucket_index_for_connections (uint32_t connections_)
{
    const uint32_t peers = std::max<uint32_t> (connections_, 1u);
    for (uint32_t i = 0; i != connection_bucket_count; ++i) {
        if (peers <= connection_buckets[i].max_peers)
            return i;
    }
    return connection_bucket_count - 1;
}

uint32_t connection_bucket_hwm_4k_by_index (zlink_auto_hwm_profile_t profile_,
                                            uint32_t bucket_index_)
{
    if (bucket_index_ >= connection_bucket_count)
        bucket_index_ = connection_bucket_count - 1;
    return bucket_hwm_for_profile (connection_buckets[bucket_index_], profile_);
}

uint32_t connection_bucket_upper_hysteresis_threshold (uint32_t bucket_index_)
{
    if (bucket_index_ >= connection_bucket_count
        || connection_buckets[bucket_index_].max_peers == unlimited_peer_bucket) {
        return unlimited_peer_bucket;
    }
    const uint32_t max_peers = connection_buckets[bucket_index_].max_peers;
    return max_peers + ((max_peers + 3u) / 4u);
}

uint32_t connection_bucket_lower_hysteresis_threshold (uint32_t bucket_index_)
{
    if (bucket_index_ == 0 || bucket_index_ >= connection_bucket_count)
        return 0;
    const uint32_t previous_max_peers = connection_buckets[bucket_index_ - 1].max_peers;
    return (previous_max_peers * 3u) / 4u;
}

uint32_t connection_bucket_index_with_hysteresis (uint32_t connections_,
                                                 uint32_t previous_bucket_index_,
                                                 bool *retained_out_)
{
    if (retained_out_)
        *retained_out_ = false;

    const uint32_t peers = std::max<uint32_t> (connections_, 1u);
    const uint32_t normal_index = connection_bucket_index_for_connections (peers);
    if (previous_bucket_index_ >= connection_bucket_count)
        return normal_index;
    if (previous_bucket_index_ == normal_index)
        return normal_index;

    bool retain_previous = false;
    if (normal_index > previous_bucket_index_) {
        const uint32_t threshold =
          connection_bucket_upper_hysteresis_threshold (previous_bucket_index_);
        retain_previous = peers < threshold;
    } else {
        const uint32_t threshold =
          connection_bucket_lower_hysteresis_threshold (previous_bucket_index_);
        retain_previous = peers > threshold;
    }

    if (!retain_previous)
        return normal_index;
    if (retained_out_)
        *retained_out_ = true;
    return previous_bucket_index_;
}

uint64_t effective_message_bytes (int socket_type_, uint64_t override_)
{
    if (override_ > 0)
        return override_;
    return socket_type_ == ZLINK_CORE_SOCKET_STREAM ? auto_hwm_stream_message_bytes
                                                    : auto_hwm_message_bytes;
}

uint64_t ceil_div (uint64_t numerator_, uint64_t denominator_)
{
    if (denominator_ == 0)
        return 0;
    return (numerator_ + denominator_ - 1) / denominator_;
}

uint64_t clamp_hwm_to_cap (uint64_t slots_, uint32_t size_cap_)
{
    if (size_cap_ == 0)
        return 0;
    uint64_t hwm = std::min<uint64_t> (slots_, size_cap_);
    if (hwm == 0)
        return 1;
    return hwm;
}

}

zlink::auto_hwm_context_plan_t::auto_hwm_context_plan_t () :
    enabled (false), profile (ZLINK_AUTO_HWM_PROFILE_BALANCED), message_unit_bytes (0)
{
}

zlink::auto_hwm_socket_plan_t::auto_hwm_socket_plan_t () :
    role (auto_hwm_role_none),
    policy_class (auto_hwm_policy_none),
    scope (auto_hwm_scope_none),
    scope_count (1),
    socket_message_slots (0),
    effective_message_bytes (auto_hwm_message_bytes),
    unit_budget_bytes (0),
    size_cap (0),
    pending_messages (0),
    pending_bytes (0),
    connection_bucket_enabled (false),
    connection_bucket_count (1),
    connection_bucket_hwm_4k (0),
    connection_bucket_index (auto_hwm_connection_bucket_none),
    connection_bucket_hysteresis_enabled (false),
    previous_connection_bucket_index (auto_hwm_connection_bucket_none),
    connection_bucket_hysteresis_retained (false),
    sndhwm (0),
    rcvhwm (0),
    manual_sndbuf (false),
    manual_rcvbuf (false),
    requested_sndbuf (-1),
    requested_rcvbuf (-1),
    effective_sndbuf (-1),
    effective_rcvbuf (-1)
{
}

zlink_auto_hwm_profile_t
zlink::auto_hwm_normalize_profile (zlink_auto_hwm_profile_t profile_)
{
    return normalize_profile (profile_);
}

bool zlink::auto_hwm_valid_profile (int profile_)
{
    return profile_ == ZLINK_AUTO_HWM_PROFILE_COMPACT
           || profile_ == ZLINK_AUTO_HWM_PROFILE_LOW_LATENCY
           || profile_ == ZLINK_AUTO_HWM_PROFILE_BALANCED
           || profile_ == ZLINK_AUTO_HWM_PROFILE_THROUGHPUT;
}

uint32_t zlink::auto_hwm_profile_message_hwm (zlink_auto_hwm_profile_t profile_)
{
    return profile_hwm (profile_).message_hwm;
}

uint32_t zlink::auto_hwm_profile_stream_hwm (zlink_auto_hwm_profile_t profile_)
{
    return profile_hwm (profile_).stream_hwm;
}

uint32_t zlink::auto_hwm_profile_control_hwm (zlink_auto_hwm_profile_t profile_)
{
    return profile_hwm (profile_).control_hwm;
}

uint32_t zlink::auto_hwm_profile_message_cap (zlink_auto_hwm_profile_t profile_)
{
    return profile_hwm (profile_).message_cap;
}

uint32_t zlink::auto_hwm_profile_stream_cap (zlink_auto_hwm_profile_t profile_)
{
    return profile_hwm (profile_).stream_cap;
}

uint32_t
zlink::auto_hwm_profile_routed_small_message_cap (zlink_auto_hwm_profile_t profile_)
{
    return routed_small_message_cap_for_profile (profile_);
}

zlink::auto_hwm_role_t zlink::auto_hwm_default_role_for_socket_type (int socket_type_)
{
    switch (socket_type_) {
        case ZLINK_CORE_SOCKET_PAIR:
            return auto_hwm_role_peer_queue;
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

zlink::auto_hwm_policy_class_t zlink::auto_hwm_policy_class_for_role (auto_hwm_role_t role_,
                                                                      int socket_type_)
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

    switch (socket_type_) {
        case ZLINK_CORE_SOCKET_PUB:
        case ZLINK_CORE_SOCKET_XPUB:
            return auto_hwm_policy_fanout;
        case ZLINK_CORE_SOCKET_SUB:
        case ZLINK_CORE_SOCKET_XSUB:
            return auto_hwm_policy_recv_ingress;
        case ZLINK_CORE_SOCKET_ROUTER:
            return auto_hwm_policy_routed;
        case ZLINK_CORE_SOCKET_DEALER:
        case ZLINK_CORE_SOCKET_PAIR:
            return auto_hwm_policy_peer_queue;
        case ZLINK_CORE_SOCKET_STREAM:
            return auto_hwm_policy_stream;
        default:
            return auto_hwm_policy_none;
    }
}

void zlink::auto_hwm_context_plan_make (bool enabled_,
                                        zlink_auto_hwm_profile_t profile_,
                                        auto_hwm_context_plan_t *out_,
                                        uint64_t message_unit_bytes_)
{
    if (!out_)
        return;

    *out_ = auto_hwm_context_plan_t ();
    out_->enabled = enabled_;
    out_->profile = normalize_profile (profile_);
    out_->message_unit_bytes = message_unit_bytes_;
}

void zlink::auto_hwm_socket_plan_prepare (auto_hwm_role_t role_,
                                          int socket_type_,
                                          size_t managed_connections_,
                                          size_t active_hwm_connections_,
                                          auto_hwm_socket_plan_t *out_,
                                          uint64_t message_unit_bytes_,
                                          int sndbuf_,
                                          int rcvbuf_,
                                          bool manual_sndbuf_,
                                          bool manual_rcvbuf_,
                                          auto_hwm_scope_t scope_,
                                          size_t scope_count_,
                                          bool buffer_cost_enabled_,
                                          bool connection_bucket_enabled_,
                                          bool connection_bucket_hysteresis_enabled_,
                                          uint32_t previous_connection_bucket_index_)
{
    if (!out_)
        return;

    *out_ = auto_hwm_socket_plan_t ();
    out_->role = role_;
    out_->policy_class = auto_hwm_policy_class_for_role (role_, socket_type_);
    out_->scope = scope_;
    out_->connection_bucket_enabled = connection_bucket_enabled_;
    out_->connection_bucket_hysteresis_enabled = connection_bucket_hysteresis_enabled_;
    out_->previous_connection_bucket_index = previous_connection_bucket_index_;
    out_->manual_sndbuf = manual_sndbuf_;
    out_->manual_rcvbuf = manual_rcvbuf_;
    out_->effective_message_bytes = effective_message_bytes (socket_type_, message_unit_bytes_);
    const uint32_t buffer_connections = std::max<uint32_t> (
      clamp_size_to_u32 (std::max (managed_connections_, active_hwm_connections_)), 1u);
    out_->connection_bucket_count = buffer_connections;

    out_->requested_sndbuf = manual_sndbuf_ ? sndbuf_ : -1;
    out_->requested_rcvbuf = manual_rcvbuf_ ? rcvbuf_ : -1;
    out_->effective_sndbuf = out_->requested_sndbuf;
    out_->effective_rcvbuf = out_->requested_rcvbuf;

    out_->scope_count = clamp_size_to_u32 (scope_count_ > 0 ? scope_count_ : 1);
    out_->unit_budget_bytes = static_cast<uint64_t> (basis_hwm_for_class (
                                ZLINK_AUTO_HWM_PROFILE_BALANCED, out_->policy_class))
                              * effective_message_bytes (socket_type_, 0);
    out_->size_cap = size_cap_for_class (ZLINK_AUTO_HWM_PROFILE_BALANCED, out_->policy_class);
    if (!buffer_cost_enabled_ || !connection_bucket_policy_class (out_->policy_class)) {
        out_->connection_bucket_enabled = false;
        out_->connection_bucket_hysteresis_enabled = false;
        out_->previous_connection_bucket_index = auto_hwm_connection_bucket_none;
    }
}

void zlink::auto_hwm_context_finalize (auto_hwm_context_plan_t *context_,
                                       auto_hwm_socket_plan_t *plans_,
                                       size_t plan_count_)
{
    if (!context_ || !plans_)
        return;

    for (size_t i = 0; i != plan_count_; ++i) {
        auto_hwm_socket_plan_t &plan = plans_[i];
        const uint32_t basis_hwm = basis_hwm_for_class (context_->profile, plan.policy_class);
        uint32_t budget_hwm = basis_hwm;
        plan.connection_bucket_hwm_4k = 0;
        plan.connection_bucket_index = auto_hwm_connection_bucket_none;
        plan.connection_bucket_hysteresis_retained = false;
        if (plan.connection_bucket_enabled && !stream_policy_class (plan.policy_class)) {
            const uint32_t bucket_index =
              plan.connection_bucket_hysteresis_enabled
                  ? connection_bucket_index_with_hysteresis (
                      plan.connection_bucket_count, plan.previous_connection_bucket_index,
                      &plan.connection_bucket_hysteresis_retained)
                  : connection_bucket_index_for_connections (plan.connection_bucket_count);
            plan.connection_bucket_index = bucket_index;
            plan.connection_bucket_hwm_4k =
              connection_bucket_hwm_4k_by_index (context_->profile, bucket_index);
            budget_hwm = std::min<uint32_t> (basis_hwm, plan.connection_bucket_hwm_4k);
        }
        const uint64_t planning_unit_bytes =
          plan.effective_message_bytes > 0
            ? plan.effective_message_bytes
            : (stream_policy_class (plan.policy_class) ? auto_hwm_stream_message_bytes
                                                       : auto_hwm_message_bytes);
        plan.unit_budget_bytes = saturating_multiply_u64 (
          static_cast<uint64_t> (budget_hwm), planning_unit_bytes);
        plan.size_cap = size_cap_for_class (context_->profile, plan.policy_class);
        if (!plan.manual_sndbuf) {
            plan.requested_sndbuf = -1;
            plan.effective_sndbuf = plan.requested_sndbuf;
        }
        if (!plan.manual_rcvbuf) {
            plan.requested_rcvbuf = -1;
            plan.effective_rcvbuf = plan.requested_rcvbuf;
        }
        plan.socket_message_slots =
          clamp_hwm_to_cap (static_cast<uint64_t> (budget_hwm), plan.size_cap);

        plan.sndhwm = plan.unit_budget_bytes;
        plan.rcvhwm = plan.unit_budget_bytes;
    }
}

void zlink::auto_hwm_socket_plan_for_role (const auto_hwm_context_plan_t &context_,
                                           auto_hwm_role_t role_,
                                           int socket_type_,
                                           size_t managed_connections_,
                                           size_t active_hwm_connections_,
                                           auto_hwm_socket_plan_t *out_,
                                           uint64_t message_unit_bytes_,
                                           int sndbuf_,
                                           int rcvbuf_,
                                           bool manual_sndbuf_,
                                           bool manual_rcvbuf_,
                                           auto_hwm_scope_t scope_,
                                           size_t scope_count_,
                                           bool buffer_cost_enabled_,
                                           bool connection_bucket_enabled_,
                                           bool connection_bucket_hysteresis_enabled_,
                                           uint32_t previous_connection_bucket_index_)
{
    if (!out_)
        return;

    auto_hwm_socket_plan_prepare (
      role_, socket_type_, managed_connections_, active_hwm_connections_, out_, message_unit_bytes_,
      sndbuf_, rcvbuf_, manual_sndbuf_, manual_rcvbuf_, scope_, scope_count_, buffer_cost_enabled_,
      connection_bucket_enabled_, connection_bucket_hysteresis_enabled_,
      previous_connection_bucket_index_);

    auto_hwm_context_plan_t adjusted_context = context_;
    auto_hwm_context_finalize (&adjusted_context, out_, 1);
}
