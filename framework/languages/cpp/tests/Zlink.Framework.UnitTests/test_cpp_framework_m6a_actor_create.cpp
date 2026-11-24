/* SPDX-License-Identifier: Elastic-2.0 */

#include "m6a_runtime_tests.hpp"
#include "../support/m6a_mesh_fixture.hpp"
#include "../support/m6a_actor_create_request.hpp"

#include "mailbox_claim_fixture.hpp"

#include "../support/runtime_failure_fixture.hpp"


#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include <limits>
#include <zlink/framework/contracts/dispatch/request_task.hpp>
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/backend/raw_binding_adapter.hpp"
#include "runtime/mesh/user_spot_terminal_mapping.hpp"
#include "runtime/locations/service_descriptor_registry.hpp"
#include "runtime/fanout/raw_fanout_owner.hpp"
#include "runtime/client_server/raw_client_server_owner.hpp"
#include "runtime/messaging/envelope_codec.hpp"
#include "runtime/locations/authority_key_codec.hpp"
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/live_location_reader.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include <zlink/Contracts/Core/context.hpp>
#include <zlink/Contracts/Core/routing_id.hpp>
#include <zlink/Contracts/Sockets/message_socket_contracts.hpp>
#include <zlink/Contracts/Sockets/routed_socket_contracts.hpp>
#include <zlink/socket/api.h>
#include "runtime/client_server/weighted_selector.hpp"
#include "runtime/protocol/service_wire_codec.hpp"

#include <service_wire_pilot_codec.hpp>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <future>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <map>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace zlink::framework::test::m6a
{
namespace mesh = zlink::framework::runtime::mesh;
namespace locations = zlink::framework::runtime::locations;
namespace fanout = zlink::framework::runtime::fanout;
namespace client_server = zlink::framework::runtime::client_server;
namespace foundation = zlink::framework::runtime::foundation;
namespace protocol = zlink::framework::runtime::protocol;
namespace runtime = zlink::framework::runtime;
using namespace std::chrono_literals;

void verify_actor_create_command_49_roundtrip ()
{
    const protocol::actor_create_header_t request{
      19,
      {20, 21},
      bytes ("source"),
      22,
      "actor-1",
      "player",
      {"reservation-1", "store-1", 23, 24, bytes ("target"), 25, "owner-1", 26, 1},
      27};
    assert (protocol::decode_actor_create_header (protocol::encode_actor_create_header (request))
            == request);

    const auto created = protocol::decode_actor_create_reply (protocol::encode_actor_create_reply (
      19, 0, 0, protocol::actor_create_result_t::created, bytes ("target"), "actor-1", 23));
    assert (created.header.correlation == 19);
    assert (created.result == protocol::actor_create_result_t::created);
    assert (created.node_routing_id == bytes ("target"));
    assert (created.actor_id == "actor-1");
    assert (created.object_generation == 23);

    const auto rejected = protocol::decode_actor_create_reply (protocol::encode_actor_create_reply (
      19, 0, 0, protocol::actor_create_result_t::rejected, {}, {}, 0));
    assert (rejected.result == protocol::actor_create_result_t::rejected);
    assert (rejected.node_routing_id.empty ());
}

void verify_actor_create_retries_until_route_is_admitted ()
{
    using completion_t = std::pair<foundation::operation_terminal_t, std::vector<std::uint8_t>>;
    mesh::raw_mesh_node_owner_t source (mesh::raw_mesh_node_options_t{
      .descriptor = descriptor ("startup-source"),
      .runtime_failures =
        std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()});
    mesh::raw_mesh_node_owner_t target (mesh::raw_mesh_node_options_t{
      .descriptor = descriptor ("startup-target"),
      .runtime_failures =
        std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()});
    source.start ();
    target.start ();
    const auto source_descriptor = source.topology ().local_descriptor ();
    const auto target_descriptor = target.topology ().local_descriptor ();
    auto request = actor_create_request (source_descriptor, target_descriptor, "startup-actor");
    std::promise<completion_t> completion_promise;
    auto completion = completion_promise.get_future ();
    auto submitted = source.request_actor_create (
      target_descriptor.node_routing_id, std::move (request), 2s,
      [&completion_promise] (
        foundation::operation_terminal_t terminal,
        ::zlink::framework::result_t<zlink::message_t> completion_payload) mutable {
          auto native_payload = std::move (completion_payload.value ());
          auto payload = native_payload.to_bytes ();
          completion_promise.set_value ({terminal, std::move (payload)});
      });
    assert (await_task (std::move (submitted)));
    assert (completion.wait_for (0ms) == std::future_status::timeout);
    admit_pair (source, target, target_descriptor);

    std::shared_ptr<mesh::service_mailbox_claim_t> claim;
    const auto receive_deadline = std::chrono::steady_clock::now () + 2s;
    while (!claim && std::chrono::steady_clock::now () < receive_deadline) {
        claim =
          target.mailbox ().try_claim (mesh::service_mailbox_domain_t::infrastructure, 1, 4096);
        if (claim)
            break;
        const auto received =
          await_task (target.pump_one (mesh::service_liveness_registry_t::clock_t::now ()));
        assert (received != mesh::raw_mesh_pump_result_t::protocol_error);
    }
    assert (claim && claim->records.size () == 1);
    const auto &record = (*claim->records.front ());
    const auto decoded =
      protocol::decode_actor_create_header (protocol::message_bytes (record.parts ().front ()));
    assert (decoded.actor_id == "startup-actor");
    assert (target.reply_actor_create (
      record, protocol::actor_create_reply_t{{*record.correlation, 0, 0},
                                             protocol::actor_create_result_t::created,
                                             target_descriptor.node_routing_id,
                                             decoded.actor_id,
                                             1}));
    assert (target.mailbox ().release (*claim));
    const auto completion_deadline = std::chrono::steady_clock::now () + 2s;
    while (completion.wait_for (0ms) != std::future_status::ready
           && std::chrono::steady_clock::now () < completion_deadline) {
        (void) await_task (source.pump_one (mesh::service_liveness_registry_t::clock_t::now ()));
    }
    assert (completion.wait_for (0ms) == std::future_status::ready);
    assert (completion.get ().first == foundation::operation_terminal_t::completed);
    source.close ();
    target.close ();
}

void verify_actor_create_intent_removal_ends_operation ()
{
    // Exercise both logical removal orders, including a request already
    // admitted by the binding. Neither transport loss nor forgetting an
    // expectation while an admitted peer remains is lifecycle termination.
    for (const int removal_order : {0, 1, 2}) {
        mesh::raw_mesh_node_owner_t source (zlink::framework::test::runtime_failure_options (
          zlink::framework::runtime::mesh::raw_mesh_node_options_t{descriptor ("intent-source")}));
        mesh::raw_mesh_node_owner_t target (zlink::framework::test::runtime_failure_options (
          zlink::framework::runtime::mesh::raw_mesh_node_options_t{descriptor ("intent-target")}));
        source.start ();
        target.start ();
        const auto local = source.topology ().local_descriptor ();
        const auto remote = target.topology ().local_descriptor ();
        source.expect_peer (remote);
        const auto pump = [&] {
            const auto now = std::chrono::steady_clock::now ();
            (void) source.observe_routes ();
            (void) target.observe_routes ();
            assert (await_task (source.pump_one (now))
                    != mesh::raw_mesh_pump_result_t::protocol_error);
            assert (await_task (target.pump_one (now))
                    != mesh::raw_mesh_pump_result_t::protocol_error);
        };
        if (removal_order != 0) {
            assert (source.connect_peer (target.endpoint (), remote));
            const auto deadline = std::chrono::steady_clock::now () + 2s;
            while (!source.topology ().peer (remote.node_routing_id)
                   && std::chrono::steady_clock::now () < deadline)
                pump ();
            assert (source.topology ().peer (remote.node_routing_id));
        }
        std::promise<foundation::operation_terminal_t> promise;
        auto completion = promise.get_future ();
        std::atomic<unsigned> completions{0};
        const auto deadline = std::chrono::steady_clock::now () + 2s;
        assert (await_task (source.request_actor_create (
          remote.node_routing_id, actor_create_request (local, remote, "intent-actor"), 2s,
          [&] (auto terminal, auto) {
              assert (completions.fetch_add (1) == 0);
              promise.set_value (terminal);
          })));
        if (removal_order != 0) {
            std::shared_ptr<mesh::service_mailbox_claim_t> claim;
            while (!claim && std::chrono::steady_clock::now () < deadline) {
                pump ();
                claim = target.mailbox ().try_claim (mesh::service_mailbox_domain_t::infrastructure,
                                                     1, 4096);
            }
            assert (claim && claim->records.front ()->reply_token ());
            assert (target.mailbox ().release (*claim));
        }
        if (removal_order == 1) {
            source.forget_peer (remote.node_routing_id, target.endpoint ());
            assert (completion.wait_for (0ms) == std::future_status::timeout);
            (void) source.disconnect_peer (remote.node_routing_id, target.endpoint ());
        } else {
            if (removal_order == 2) {
                (void) source.disconnect_peer (remote.node_routing_id, target.endpoint ());
                assert (completion.wait_for (100ms) == std::future_status::timeout);
            }
            source.forget_peer (remote.node_routing_id, target.endpoint ());
        }
        assert (completion.wait_for (250ms) == std::future_status::ready);
        assert (std::chrono::steady_clock::now () < deadline);
        assert (completion.get () == foundation::operation_terminal_t::route_unavailable);
        source.close ();
        target.close ();
        assert (completions.load () == 1);
    }
}

void verify_actor_create_readiness_deadline_is_timed_out ()
{
    using completion_t = std::pair<foundation::operation_terminal_t, std::vector<std::uint8_t>>;
    mesh::raw_mesh_node_owner_t source (mesh::raw_mesh_node_options_t{
      .descriptor = descriptor ("timeout-source"),
      .runtime_failures =
        std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()});
    source.start ();
    const auto source_descriptor = source.topology ().local_descriptor ();
    auto target_descriptor = descriptor ("timeout-target", "tcp://127.0.0.1:1");
    target_descriptor.state = mesh::service_node_state_t::serving;
    auto request = actor_create_request (source_descriptor, target_descriptor, "timeout-actor");
    std::promise<completion_t> completion_promise;
    auto completion = completion_promise.get_future ();
    const auto submitted_at = std::chrono::steady_clock::now ();
    auto submitted = source.request_actor_create (
      target_descriptor.node_routing_id, std::move (request), 50ms,
      [&completion_promise] (
        foundation::operation_terminal_t terminal,
        ::zlink::framework::result_t<zlink::message_t> completion_payload) mutable {
          auto native_payload = std::move (completion_payload.value ());
          auto payload = native_payload.to_bytes ();
          completion_promise.set_value ({terminal, std::move (payload)});
      });
    assert (submitted.await_ready ());
    assert (await_task (std::move (submitted)));
    const auto deadline = submitted_at + 500ms;
    while (completion.wait_for (0ms) != std::future_status::ready
           && std::chrono::steady_clock::now () < deadline) {
        (void) source.expire_requests (std::chrono::steady_clock::now ()).result ().value ();
        std::this_thread::yield ();
    }
    assert (completion.wait_for (0ms) == std::future_status::ready);
    assert (std::chrono::steady_clock::now () - submitted_at >= 40ms);
    const auto terminal = completion.get ().first;
    assert (terminal == foundation::operation_terminal_t::timed_out);
    assert (runtime::user_spot_terminal::map_user_spot_operation_failure (terminal, {}, true)
            == zlink::framework::framework_error_kind_t::deadline_exceeded);
    source.close ();
}

void verify_actor_create_from_dispatch_thread_does_not_block ()
{
    mesh::raw_mesh_node_owner_t source (mesh::raw_mesh_node_options_t{
      .descriptor = descriptor ("dispatch-source"),
      .runtime_failures =
        std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()});
    source.start ();
    const auto source_descriptor = source.topology ().local_descriptor ();
    auto target_descriptor = descriptor ("dispatch-target", "tcp://127.0.0.1:1");
    target_descriptor.state = mesh::service_node_state_t::serving;
    source.expect_peer (target_descriptor);
    auto request = actor_create_request (source_descriptor, target_descriptor, "dispatch-actor");
    std::atomic<bool> callback_called{false};
    std::atomic<foundation::operation_terminal_t> callback_terminal{
      foundation::operation_terminal_t::transport_failed};

    const auto submitted_at = std::chrono::steady_clock::now ();
    auto submitted = source.request_actor_create (
      target_descriptor.node_routing_id, std::move (request), 500ms,
      [&callback_called, &callback_terminal] (foundation::operation_terminal_t terminal,
                                              ::zlink::framework::result_t<zlink::message_t>) {
          callback_terminal.store (terminal, std::memory_order_release);
          callback_called.store (true, std::memory_order_release);
      });
    assert (await_task (std::move (submitted)));
    assert (std::chrono::steady_clock::now () - submitted_at < 20ms);
    assert (!callback_called.load (std::memory_order_acquire));
    source.close ();
    const auto callback_deadline = std::chrono::steady_clock::now () + 500ms;
    while (!callback_called.load (std::memory_order_acquire)
           && std::chrono::steady_clock::now () < callback_deadline) {
        std::this_thread::yield ();
    }
    assert (callback_called.load (std::memory_order_acquire));
    assert (callback_terminal.load (std::memory_order_acquire)
            == foundation::operation_terminal_t::shutdown);
}
}
