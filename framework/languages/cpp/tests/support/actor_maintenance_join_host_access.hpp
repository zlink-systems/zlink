/* SPDX-License-Identifier: Elastic-2.0 */

#pragma once

#include "runtime/stateful/public_host_runtime.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>

namespace zlink::framework::runtime::host
{
// Reproduce a superseded assembly at the activation boundary without timing races.
struct public_host_runtime_test_access_t
{
    static task_t<stateful::stateful_error_t> advance_authority (public_host_runtime_t &host,
                                                                 stateful::object_ref_t committed);

    enum class authority_interleaving_t
    {
        unchanged,
        superseded,
        erased,
        duplicate
    };

    static std::future<std::optional<stateful::object_ref_t>>
    probe_actor_index (public_host_runtime_t &host,
                       stateful::object_ref_t replacement,
                       authority_interleaving_t change);

    static std::promise<void> hold_objects (public_host_runtime_t &host);

    static std::promise<void> hold_actor_index (public_host_runtime_t &host);

    static std::string entry_id (public_host_runtime_t &host);

    static std::promise<void> hold_dispatch (public_host_runtime_t &host);

    static void lifecycle_turn (public_host_runtime_t &host, std::function<void ()> work);

    static task_t<zlink::submit_result_t> join (actor_handle_t &actor,
                                                const zlink::routing_id_t &rid,
                                                const std::string &spot,
                                                std::uint64_t generation,
                                                pending_operation_t &operation,
                                                std::chrono::milliseconds timeout);

    static task_t<std::size_t>
    dispatch_claim (public_host_runtime_t &host,
                    std::shared_ptr<mesh::service_mailbox_claim_t> claim,
                    const mesh::service_mailbox_t::application_dispatch_t &dispatch);

    static task_t<void> ingest_cut (public_host_runtime_t &host,
                                    stateful::object_ref_t owner,
                                    mesh::service_mailbox_record_t original);

    static std::array<std::size_t, 3> spot_records (public_host_runtime_t &host);
    static task_t<bool> restore_accepted_spot (public_host_runtime_t &host,
                                               std::shared_ptr<location_repository_t> store,
                                               const protocol::request_source_fence_t &source,
                                               std::shared_ptr<std::atomic<int>> materialized);
    static task_t<bool> activate_stale_actor_attempt (public_host_runtime_t &host,
                                                      bool replaced,
                                                      bool activation_started,
                                                      const stateful::object_ref_t &target,
                                                      std::string stable_type);
};
} // namespace zlink::framework::runtime::host
