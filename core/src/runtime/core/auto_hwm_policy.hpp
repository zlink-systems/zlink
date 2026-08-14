/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_AUTO_HWM_POLICY_HPP_INCLUDED__
#define __ZLINK_AUTO_HWM_POLICY_HPP_INCLUDED__

#include <stddef.h>
#include <stdint.h>

#include "zlink_enum.h"

namespace zlink
{
enum auto_hwm_role_t
{
    auto_hwm_role_none = 0,
    auto_hwm_role_control = 1,
    auto_hwm_role_routed = 2,
    auto_hwm_role_fanout = 3,
    auto_hwm_role_recv_ingress = 4,
    auto_hwm_role_connection_data = 5,
    auto_hwm_role_peer_queue = 6,
    auto_hwm_role_stream = 7
};

enum auto_hwm_policy_class_t
{
    auto_hwm_policy_none = 0,
    auto_hwm_policy_fanout = 1,
    auto_hwm_policy_connection_data = 2,
    auto_hwm_policy_recv_ingress = 3,
    auto_hwm_policy_routed = 4,
    auto_hwm_policy_peer_queue = 5,
    auto_hwm_policy_stream = 6,
    auto_hwm_policy_control = 7
};

struct auto_hwm_budget_input_t
{
    auto_hwm_budget_input_t ();

    bool enabled;
    zlink_auto_hwm_profile_t profile;
    uint64_t configured_memory_limit_bytes;
    uint64_t runtime_memory_limit_bytes;
    uint64_t configured_core_budget_bytes;
    uint64_t detected_hard_limit_bytes;
    uint64_t detected_physical_memory_bytes;
};

struct auto_hwm_context_plan_t
{
    auto_hwm_context_plan_t ();

    bool enabled;
    zlink_auto_hwm_profile_t profile;
    uint64_t configured_memory_limit_bytes;
    uint64_t runtime_memory_limit_bytes;
    uint64_t resolved_memory_limit_bytes;
    uint64_t configured_core_budget_bytes;
    uint64_t effective_core_budget_bytes;
    uint64_t total_planned_hwm_bytes;
    uint64_t total_applied_hwm_bytes;
    uint64_t manual_reserved_hwm_bytes;
    uint64_t active_directional_queue_count;
    uint64_t active_send_queue_count;
    uint64_t active_receive_queue_count;
    uint64_t unlimited_manual_queue_count;
    bool budget_insufficient;
    bool aggregate_hwm_valid;
    bool aggregate_overflow;
};

struct auto_hwm_socket_plan_t
{
    auto_hwm_socket_plan_t ();

    auto_hwm_role_t role;
    auto_hwm_policy_class_t policy_class;
    bool planning_enabled;
    uint64_t send_queue_count;
    uint64_t receive_queue_count;
    uint64_t minimum_hwm_bytes;
    uint64_t maximum_hwm_bytes;
    uint64_t pending_bytes;
    bool manual_sndhwm;
    bool manual_rcvhwm;
    uint64_t sndhwm;
    uint64_t rcvhwm;
};

bool auto_hwm_valid_profile (int profile_);
uint64_t auto_hwm_profile_minimum_bytes (zlink_auto_hwm_profile_t profile_,
                                         auto_hwm_role_t role_);
uint64_t auto_hwm_profile_maximum_bytes (zlink_auto_hwm_profile_t profile_,
                                         auto_hwm_role_t role_);
auto_hwm_role_t auto_hwm_default_role_for_socket_type (int socket_type_);
auto_hwm_policy_class_t auto_hwm_policy_class_for_role (auto_hwm_role_t role_, int socket_type_);
void auto_hwm_context_plan_make (const auto_hwm_budget_input_t &input_,
                                 auto_hwm_context_plan_t *out_);
void auto_hwm_socket_plan_prepare (auto_hwm_role_t role_,
                                   uint64_t send_queue_count_,
                                   uint64_t receive_queue_count_,
                                   bool manual_sndhwm_,
                                   uint64_t sndhwm_,
                                   bool manual_rcvhwm_,
                                   uint64_t rcvhwm_,
                                   bool planning_enabled_,
                                   auto_hwm_socket_plan_t *out_);
void auto_hwm_context_finalize (auto_hwm_context_plan_t *context_,
                                auto_hwm_socket_plan_t *plans_,
                                size_t plan_count_);
}

#endif
