/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/m6a_actor_join_request.hpp"
#include "m6a_runtime_tests.hpp"
#include "../support/m6a_mesh_fixture.hpp"
#include "../support/m6a_actor_create_request.hpp"
#include "../support/m6a_relocation_prepare_request.hpp"
#include "../support/m6a_native_calls.hpp"

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

// Authority-selected lifecycle generations wait independently of an older admitted peer.
void verify_authority_generation_request (std::string_view path, bool invalidated)
{
    using namespace zlink::framework;
    mesh::raw_mesh_node_owner_t source (zlink::framework::test::runtime_failure_options (
      mesh::raw_mesh_node_options_t{descriptor ("authority-generation-source")}));
    mesh::raw_mesh_node_owner_t target (zlink::framework::test::runtime_failure_options (
      mesh::raw_mesh_node_options_t{descriptor ("authority-generation-target")}));
    source.start ();
    target.start ();
    auto remote = target.topology ().local_descriptor ();
    admit_pair (source, target, remote);
    const auto admitted_generation = remote.lifecycle_generation;
    remote.lifecycle_generation = admitted_generation == std::numeric_limits<std::uint64_t>::max ()
                                    ? 1
                                    : std::numeric_limits<std::uint64_t>::max ();
    if (path == "prepare")
        remote.lifecycle_generation = admitted_generation == 1 ? 2 : 1;
    source.expect_peer (remote);
#if !defined(_WIN32)
    request_submissions.store (0);
    count_request_submissions.store (true);
#endif
    const auto deadline = std::chrono::steady_clock::now () + 2s;
    auto verify_waiting = [&] {
        assert (source.pending_operation_count () == 1);
#if !defined(_WIN32)
        assert (request_submissions.load () == 0);
#endif
        assert (
          !target.mailbox ().try_claim (mesh::service_mailbox_domain_t::infrastructure, 1, 4096));
        assert (
          !target.mailbox ().try_claim (mesh::service_mailbox_domain_t::application, 1, 4096));
    };
    auto end_wait = [&] {
        verify_waiting ();
        if (invalidated) {
            source.forget_peer (remote.node_routing_id, remote.advertised_endpoint);
            const auto remaining = source.topology ().peer (remote.node_routing_id);
            assert (remaining && remaining->descriptor.lifecycle_generation == admitted_generation);
            assert (peer_ready (source, remote.node_routing_id));
        } else {
            assert (await_task (source.expire_requests (deadline + 1s)) == 1);
        }
    };
    if (path == "join") {
        auto pending =
          source.request_actor_join (remote.node_routing_id,
                                     actor_join_request (source.topology ().local_descriptor (),
                                                         remote, 8181, "actor", "spot"),
                                     std::nullopt, 2s);
        end_wait ();
        const auto result = await_task (std::move (pending));
        assert (!result.reply);
        assert (result.failure
                == (invalidated ? mesh::actor_join_wire_failure_t::unavailable
                                : mesh::actor_join_wire_failure_t::deadline_exceeded));
    } else if (path == "prepare") {
        auto pending = source.request_relocation_prepare (
          remote.node_routing_id,
          relocation_prepare_request (source.topology ().local_descriptor (), remote), 2s);
        end_wait ();
        const auto result = await_task (std::move (pending));
        assert (!result.ready && !result.failed);
        if (invalidated)
            assert (await_task (source.expire_requests (deadline + 1s)) == 0);
    } else if (path == "actor-create" || path == "instance" || path == "user-spot-create"
               || path == "user-spot-close") {
        std::promise<foundation::operation_terminal_t> promise;
        auto terminal = promise.get_future ();
        auto callback = [&promise] (auto result, auto) { promise.set_value (result); };
        const auto local = source.topology ().local_descriptor ();
        const auto unix_deadline = static_cast<std::uint64_t> (
          std::chrono::duration_cast<std::chrono::milliseconds> (
            (std::chrono::system_clock::now () + 2s).time_since_epoch ())
            .count ());
        // User Spot create and close carry the reservation or close target's owner G.
        task_t<bool> pending =
          path == "actor-create"
            ? source.request_actor_create (remote.node_routing_id,
                                           actor_create_request (local, remote, "authority-actor"),
                                           2s, callback)
          : path == "user-spot-create"
            ? source.request_user_spot_create (
                remote.node_routing_id,
                protocol::user_spot_create_header_t{
                  0,
                  {local.lifecycle_generation, 92},
                  local.node_routing_id,
                  local.lifecycle_generation,
                  "authority-spot",
                  "room",
                  {"authority-reservation", "authority-store", 1, 1, remote.node_routing_id,
                   remote.lifecycle_generation, "authority-owner", 1, 1},
                  unix_deadline},
                2s, callback)
          : path == "user-spot-close"
            ? source.request_user_spot_close (remote.node_routing_id,
                                              protocol::user_spot_close_header_t{
                                                0,
                                                {local.lifecycle_generation, 93},
                                                local.node_routing_id,
                                                local.lifecycle_generation,
                                                {"authority-spot", 1, remote.node_routing_id,
                                                 remote.lifecycle_generation, 1, "authority-store"},
                                                unix_deadline},
                                              std::chrono::steady_clock::now () + 2s, callback)
            : source.request_instance_spot_activation (
                remote.node_routing_id,
                protocol::instance_spot_activation_header_t{
                  {remote.node_routing_id, remote.lifecycle_generation, "instance-1", "admission",
                   "room", "descriptor-1",
                   static_cast<std::uint64_t> (
                     std::chrono::duration_cast<std::chrono::milliseconds> (
                       (std::chrono::system_clock::now () + 2s).time_since_epoch ())
                       .count ())},
                  local.lifecycle_generation,
                  local.node_routing_id,
                  std::string ("source-spot"),
                  true,
                  {local.lifecycle_generation, 91},
                  0,
                  false},
                std::nullopt,
                protocol::application_payload_t{"AuthorityGeneration", "application/json", {}}, 2s,
                callback);
        end_wait ();
        assert (terminal.wait_for (250ms) == std::future_status::ready);
        assert (terminal.get ()
                == (invalidated ? foundation::operation_terminal_t::route_unavailable
                                : foundation::operation_terminal_t::timed_out));
        // Actor and User Spot create/close report registration; Instance Spot reports the
        // failed admission.
        const auto submitted = pending.result ();
        if (path != "instance")
            assert (submitted && submitted.value ());
        else
            assert (!submitted);
    } else {
        assert (path == "spot" || path == "actor");
        std::promise<foundation::operation_terminal_t> promise;
        auto terminal = promise.get_future ();
        auto callback = [&promise] (auto result, auto) { promise.set_value (result); };
        protocol::application_payload_t payload{"AuthorityGeneration", "application/json", {}};
        task_t<bool> pending =
          path == "spot" ? source.request_to_spot (
                             remote.node_routing_id, "source-spot",
                             protocol::spot_route_fence_t{"spot", 1, remote.node_routing_id,
                                                          remote.lifecycle_generation, 1, 1},
                             payload, 2s, callback)
                         : source.request_to_actor (
                             remote.node_routing_id, std::nullopt,
                             protocol::actor_route_fence_t{"actor", 1, remote.node_routing_id,
                                                           remote.lifecycle_generation, 1, 1},
                             payload, 2s, callback);
        end_wait ();
        assert (terminal.wait_for (250ms) == std::future_status::ready);
        assert (terminal.get ()
                == (invalidated ? foundation::operation_terminal_t::route_unavailable
                                : foundation::operation_terminal_t::timed_out));
        try {
            (void) await_task (std::move (pending));
            assert (false);
        }
        catch (const framework_exception_t &error) {
            assert (error.kind ()
                    == (invalidated ? framework_error_kind_t::unavailable
                                    : framework_error_kind_t::deadline_exceeded));
        }
    }
    if (invalidated)
        assert (std::chrono::steady_clock::now () < deadline);
    assert (source.pending_operation_count () == 0);
#if !defined(_WIN32)
    assert (request_submissions.load () == 0);
    count_request_submissions.store (false);
#endif
    source.close ();
    target.close ();
}

// Submit/completion section 5: a Spot or Actor send names the owner generation G
// its authority resolved. A ready peer of another generation is not that route:
// the send is NotConnected and no frame reaches the peer.
void verify_authority_generation_send ()
{
    mesh::raw_mesh_node_owner_t source (zlink::framework::test::runtime_failure_options (
      mesh::raw_mesh_node_options_t{descriptor ("authority-send-source")}));
    mesh::raw_mesh_node_owner_t target (zlink::framework::test::runtime_failure_options (
      mesh::raw_mesh_node_options_t{descriptor ("authority-send-target")}));
    source.start ();
    target.start ();
    const auto remote = target.topology ().local_descriptor ();
    admit_pair (source, target, remote);
    const auto admitted_generation = remote.lifecycle_generation;
    const auto authority_generation = admitted_generation + 1;
    const protocol::application_payload_t payload{"AuthorityGenerationSend", "application/json",
                                                  bytes ("{}")};
    const auto send = [&] (bool spot, std::uint64_t generation) {
        auto pending =
          spot
            ? source.send_to_spot_result (
                remote.node_routing_id, "source-spot",
                protocol::spot_route_fence_t{"spot", 1, remote.node_routing_id, generation, 1, 1},
                payload)
            : source.send_to_actor_result (
                remote.node_routing_id, std::nullopt,
                protocol::actor_route_fence_t{"actor", 1, remote.node_routing_id, generation, 1, 1},
                payload);
        return await_task (std::move (pending));
    };
    const auto received_by = [&] (std::chrono::steady_clock::duration window) {
        std::size_t received = 0;
        const auto until = std::chrono::steady_clock::now () + window;
        while (std::chrono::steady_clock::now () < until) {
            if (await_task (target.pump_one (mesh::service_liveness_registry_t::clock_t::now ()))
                == mesh::raw_mesh_pump_result_t::application)
                ++received;
            std::this_thread::yield ();
        }
        return received;
    };

    // Authority G1, admitted peer G0: rejected before submission.
    assert (send (true, authority_generation) == zlink::submit_result_t::not_connected);
    assert (send (false, authority_generation) == zlink::submit_result_t::not_connected);
    assert (peer_ready (source, remote.node_routing_id));
    assert (received_by (200ms) == 0);
    assert (!target.mailbox ().try_claim (mesh::service_mailbox_domain_t::application, 1, 4096));

    // The same peer at the authority generation is the route; both frames arrive.
    assert (send (true, admitted_generation) == zlink::submit_result_t::ok);
    assert (send (false, admitted_generation) == zlink::submit_result_t::ok);
    std::size_t delivered = 0;
    const auto deadline = std::chrono::steady_clock::now () + 2s;
    while (delivered < 2 && std::chrono::steady_clock::now () < deadline)
        delivered += received_by (10ms);
    assert (delivered == 2);
    source.close ();
    target.close ();
}
}
