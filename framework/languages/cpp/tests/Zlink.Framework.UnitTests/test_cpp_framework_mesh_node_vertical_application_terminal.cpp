/* SPDX-License-Identifier: Elastic-2.0 */
#include "../support/mesh_node_vertical_cases.hpp"
#include <zlink.hpp>
#include <atomic>
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/dispatch/completion_turn.hpp"
#include "runtime/dispatch/coroutine_executor.hpp"
#include <runtime/locations/location_repository.hpp>
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/live_location_reader.hpp"
#include "runtime/locations/location_runtime.hpp"
#include "runtime/locations/store_location_resolvers.hpp"
#include "runtime/dispatch/dispatch_limits.hpp"
#include "runtime/execution/infrastructure_wait_guard.hpp"
#include "runtime/mesh/mesh_node_host_service.hpp"
#include "runtime/mesh/route_mesh_runtime_options_service.hpp"
#include "runtime/mesh/route_mesh_runtime_service.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "../support/loopback_tcp_endpoint.hpp"
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <latch>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>
#include "../support/mesh_node_vertical_transport_fixture.hpp"

using namespace framework_mesh_node_vertical_fixture;

using namespace std::chrono_literals;

namespace framework_mesh_node_vertical_tests
{
void verify_local_and_wire_application_owner_keys_match ()
{
    using namespace zlink::framework;
    namespace runtime = zlink::framework::runtime;
    using runtime::host::owner_kind_t;
    runtime::host::ready_record_t local;
    assert (runtime::mesh::service_mailbox_t::application_owner (local)
            == runtime::mesh::service_mailbox_t::application_owner (owner_kind_t::node));
    local.owner_kind = owner_kind_t::channel;
    local.channel_name = "same";
    const auto channel = runtime::mesh::service_mailbox_t::application_owner (local);
    assert (channel
            == runtime::mesh::service_mailbox_t::application_owner (owner_kind_t::channel, "same"));
    local.owner_kind = owner_kind_t::spot;
    local.spot_id = "same";
    const auto spot = runtime::mesh::service_mailbox_t::application_owner (local);
    assert (spot == "spot:same");
    assert (spot
            == runtime::mesh::service_mailbox_t::application_owner (owner_kind_t::spot, "same"));
    local.owner_kind = owner_kind_t::actor;
    local.actor.emplace (actor_id_t ("same"), 1, "mesh", node_rid_t::from_string ("node"));
    const auto actor = runtime::mesh::service_mailbox_t::application_owner (local);
    assert (actor
            == runtime::mesh::service_mailbox_t::application_owner (owner_kind_t::actor, "same"));
    assert (channel != spot && spot != actor && actor != channel);
}

void verify_owner_drain_continuation_during_executor_shutdown ()
{
    namespace runtime = zlink::framework::runtime;
    runtime::offload_executor_t executor (1);
    std::promise<void> started;
    std::promise<void> release;
    auto release_wait = release.get_future ().share ();
    std::atomic_int completed{0};
    assert (executor.try_submit_internal ([&] {
        started.set_value ();
        release_wait.wait ();
        assert (executor.try_submit_internal ([&] { ++completed; }));
    }));
    started.get_future ().wait ();
    assert (!executor.drain_until (std::chrono::steady_clock::now ()));
    assert (!executor.try_submit ([] {}));
    release.set_value ();
    executor.drain ();
    assert (completed.load () == 1);
    assert (!executor.try_submit_internal ([] {}));
}

void verify_deferred_application_terminal_ownership ()
{
    using namespace zlink::framework;
    namespace runtime = zlink::framework::runtime;

    auto registration = make_node ("tcp://127.0.0.1:0", "deferred-terminal-owner");
    auto node = std::make_shared<detail::mesh_node_runtime_t> (registration);
    node->application_work_enqueued ();
    node->application_work_started ();

    auto stateful_completed = std::make_shared<std::atomic_bool> (false);
    std::atomic_int stateful_null_completions{0};
    std::atomic_int stateful_replies{0};
    std::atomic_int mailbox_releases{0};
    auto complete_stateful = [stateful_completed, &stateful_null_completions] {
        if (!stateful_completed->exchange (true, std::memory_order_acq_rel))
            ++stateful_null_completions;
    };
    auto late_reply = [stateful_completed, &stateful_replies] {
        if (stateful_completed->exchange (true, std::memory_order_acq_rel))
            return false;
        ++stateful_replies;
        return true;
    };
    auto terminal = std::make_shared<runtime::application_dispatch_terminal_owner_t> (
      node, complete_stateful, [&mailbox_releases] { ++mailbox_releases; });

    assert (node->pending_application_callbacks () == 0);
    assert (node->active_application_callbacks () == 1);
    assert (late_reply ());
    assert (stateful_replies.load () == 1);
    assert (stateful_null_completions.load () == 0);

    terminal->settle ();
    terminal->settle ();
    terminal.reset ();
    assert (node->pending_application_callbacks () == 0);
    assert (node->active_application_callbacks () == 0);
    assert (mailbox_releases.load () == 1);
    assert (stateful_replies.load () == 1);
    assert (stateful_null_completions.load () == 0);

    // An inline terminal uses the same owner and therefore settles every
    // obligation once even when its destructor runs immediately afterwards.
    node->application_work_enqueued ();
    node->application_work_started ();
    auto inline_completed = std::make_shared<std::atomic_bool> (false);
    std::atomic_int inline_stateful{0};
    std::atomic_int inline_mailbox{0};
    {
        auto inline_terminal = std::make_shared<runtime::application_dispatch_terminal_owner_t> (
          node,
          [inline_completed, &inline_stateful] {
              if (!inline_completed->exchange (true, std::memory_order_acq_rel)) {
                  ++inline_stateful;
              }
          },
          [&inline_mailbox] { ++inline_mailbox; });
        inline_terminal->settle ();
    }
    assert (inline_stateful.load () == 1);
    assert (inline_mailbox.load () == 1);
    assert (node->active_application_callbacks () == 0);

    node->application_work_enqueued ();
    node->application_work_started ();
    std::atomic_int moved_mailbox{0};
    std::shared_ptr<runtime::application_dispatch_terminal_owner_t> deferred;
    {
        runtime::application_dispatch_terminal_owner_t inline_owner (
          node, {}, [&moved_mailbox] { ++moved_mailbox; });
        deferred = std::make_shared<runtime::application_dispatch_terminal_owner_t> (
          std::move (inline_owner));
        inline_owner.settle ();
        assert (moved_mailbox.load () == 0);
    }
    assert (node->active_application_callbacks () == 1);
    deferred.reset ();
    assert (moved_mailbox.load () == 1);
    assert (node->active_application_callbacks () == 0);
}

bool verify_application_terminal_drain_waits_for_capture_destruction ()
{
    using namespace zlink::framework;
    namespace runtime = zlink::framework::runtime;

    auto registration = make_node ("tcp://127.0.0.1:0", "terminal-capture-drain");
    auto node = std::make_shared<detail::mesh_node_runtime_t> (registration);
    node->application_work_enqueued ();
    node->application_work_started ();

    struct destruction_gate_t
    {
        std::promise<void> entered;
        std::promise<void> release;
        std::shared_future<void> released = release.get_future ().share ();
    };
    struct blocking_capture_t
    {
        std::shared_ptr<destruction_gate_t> gate;

        ~blocking_capture_t ()
        {
            gate->entered.set_value ();
            gate->released.wait ();
        }
    };

    auto gate = std::make_shared<destruction_gate_t> ();
    std::future<void> capture_destruction_entered = gate->entered.get_future ();
    auto capture = std::make_shared<blocking_capture_t> ();
    capture->gate = gate;
    runtime::application_dispatch_terminal_owner_t terminal (
      node, [capture = std::move (capture)] {}, {});
    capture.reset ();

    std::thread settlement ([&] { terminal.settle (); });
    const bool capture_destruction_started =
      capture_destruction_entered.wait_for (2s) == std::future_status::ready;
    const auto active_during_capture_destruction = node->active_application_callbacks ();
    gate->release.set_value ();
    settlement.join ();

    const auto active_after_settlement = node->active_application_callbacks ();
    const bool passed = capture_destruction_started && active_during_capture_destruction == 1
                        && active_after_settlement == 0;
    if (!passed)
        std::cerr << "terminal capture destruction started=" << capture_destruction_started
                  << " active during destruction=" << active_during_capture_destruction
                  << " active after settlement=" << active_after_settlement << '\n';
    return passed;
}
} // namespace framework_mesh_node_vertical_tests
