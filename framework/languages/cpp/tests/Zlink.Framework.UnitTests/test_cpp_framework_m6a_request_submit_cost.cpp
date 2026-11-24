/* SPDX-License-Identifier: Elastic-2.0 */

#include "m6a_runtime_tests.hpp"
#include "../support/m6a_mesh_fixture.hpp"


#include "../support/allocation_counter.hpp"


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

void verify_node_request_submit_cost ()
{
    namespace fw = zlink::framework;
    using fw::tests::count_allocations;
    bool no_target_copies = true;
    bool no_extra_lane_allocations = true;
    for (const std::size_t target_size : {127u, 193u}) {
        std::size_t missing_peer_allocations = 0;
        for (const bool ready_peer : {false, true}) {
            mesh::raw_mesh_node_owner_t source (mesh::raw_mesh_node_options_t{
              .descriptor = descriptor ("request-submit-source"),
              .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()});
            const std::vector<std::uint8_t> target (target_size, 't');
            if (ready_peer) {
                auto remote = descriptor ("request-submit-target");
                remote.node_routing_id = target;
                remote.state = mesh::service_node_state_t::serving;
                assert (
                  source.topology ().admit (remote, bytes ("request-submit-connection"), remote)
                  == mesh::peer_admission_result_t::admitted);
                const auto peer = source.topology ().peer (target);
                assert (peer && peer->ready);
            }
            std::vector<std::uint8_t> copied;
            const auto copied_allocations =
              count_allocations ([&] { copied = target; }, target.size ());
            assert (copied == target && copied_allocations > 0);
            const auto borrowed_baseline = count_allocations ([] {}, target.size ());
            protocol::application_payload_t payload{"RequestSubmit", "x/y", {}};
            bool callback_called = false;
            std::optional<fw::task_t<bool>> submission;
            const auto observed = count_allocations (
              [&] {
                  submission.emplace (source.request_to_node (
                    target, payload, 1s, [&] (auto, auto) { callback_called = true; }));
              },
              target.size ());
            assert (submission->await_ready ());
            assert (submission->result () && !submission->result ().value ());
            std::optional<zlink::message_t> encoded;
            const auto encode_allocations = count_allocations (
              [&] { encoded.emplace (protocol::encode_application_payload_message (payload)); });
            assert (encoded && encoded->size () != 0);
            const auto full_allocations = count_allocations ([&] {
                submission.emplace (source.request_to_node (
                  target, payload, 1s, [&] (auto, auto) { callback_called = true; }));
            });
            assert (submission->await_ready ());
            assert (submission->result () && !submission->result ().value ());
            if (!ready_peer) {
                missing_peer_allocations = full_allocations;
            } else {
                // Both public calls create the same request task layers. The
                // Ready/no-port path additionally encodes the payload and enters
                // the owner lane; that synchronous turn needs no future state.
                const auto expected = missing_peer_allocations + encode_allocations;
                std::fprintf (
                  stderr,
                  "node request lane target=%zu missing=%zu ready=%zu encode=%zu expected=%zu\n",
                  target_size, missing_peer_allocations, full_allocations, encode_allocations,
                  expected);
                no_extra_lane_allocations =
                  no_extra_lane_allocations && full_allocations == expected;
            }
            assert (!callback_called);
            assert (source.expire_requests (foundation::operation_registry_t::clock_t::now ())
                      .result ()
                      .value ()
                    == 0);
            std::fprintf (stderr,
                          "node request target=%zu ready=%d copied=%zu borrowed=%zu observed=%zu\n",
                          target_size, ready_peer, copied_allocations, borrowed_baseline, observed);
            no_target_copies = no_target_copies && observed == borrowed_baseline;
        }
    }
    assert (no_extra_lane_allocations);
    assert (no_target_copies);
}

void verify_request_submission_results ()
{
    using zlink::framework::runtime::messaging::map_submit_request_result;
    assert (map_submit_request_result (zlink::submit_result_t::not_connected, false)
            == zlink::request_result_t::not_connected);
    assert (map_submit_request_result (zlink::submit_result_t::not_admitted, false)
            == zlink::request_result_t::rejected);
    // Submit §7 distinguishes an absent initial target from a removed pending route.
    assert (map_submit_request_result (zlink::submit_result_t::not_found, false)
            == zlink::request_result_t::not_found);
    assert (map_submit_request_result (zlink::submit_result_t::not_found, true)
            == zlink::request_result_t::not_connected);
    assert (map_submit_request_result (zlink::submit_result_t::not_connected, true)
            == zlink::request_result_t::not_connected);
}
}
