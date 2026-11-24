/* SPDX-License-Identifier: Elastic-2.0 */

#pragma once

#include "runtime/stateful/public_host_runtime.hpp"
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace zlink::framework::runtime::host
{
// Drives the relocation terminal lane directly so a test can build the state
// between two management passes without waiting for time to pass.
struct public_host_runtime_test_access_t
{
    static std::pair<bool, std::optional<protocol::session_relocation_sealed_t>>
    admit_seal (public_host_runtime_t &host, const protocol::session_relocation_seal_t &seal);
    static void expire_seal_results (public_host_runtime_t &host);

    static void set_dispatch (public_host_runtime_t &host, dispatch_options_t options);
    static bool post_terminal_turn (public_host_runtime_t &host, std::function<void ()> work);
    static bool on_terminal_lane (public_host_runtime_t &host);
    static void enqueue_lifetime_probe (public_host_runtime_t &host);
    using key_t = public_host_runtime_t::relocation_attempt_key_t;

    static bool restore_saved (public_host_runtime_t &host,
                               const protocol::relocation_prepare_t &prepare,
                               std::vector<stateful::frozen_object_state_t> frozen,
                               std::vector<stateful::object_ref_t> targets,
                               const stateful::relocation_restore_identity_t &identity);

    static bool complete_saved (public_host_runtime_t &host,
                                const stateful::object_ref_t &owner,
                                const protocol::application_payload_t &reply);

    static bool dispatch_saved (public_host_runtime_t &host,
                                stateful::stateful_delivery_t delivery,
                                const mesh::service_mailbox_t::application_dispatch_t &dispatch);

    static void insert_unscheduled_target_attempt (public_host_runtime_t &host, const key_t &key);

    static void publish_relocation_bound (public_host_runtime_t &host);

    static void schedule_finalize (public_host_runtime_t &host,
                                   const key_t &key,
                                   std::chrono::steady_clock::time_point at);

    static bool relocation_due (const public_host_runtime_t &host,
                                std::chrono::steady_clock::time_point now);
};
} // namespace zlink::framework::runtime::host
