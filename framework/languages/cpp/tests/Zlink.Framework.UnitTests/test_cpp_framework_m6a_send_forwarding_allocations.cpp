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

void verify_node_send_forwarding_allocations ()
{
    namespace fw = zlink::framework;
    using fw::tests::count_allocations;
    static_assert (std::is_nothrow_move_constructible_v<protocol::application_payload_t>);
    mesh::raw_mesh_node_owner_t source (mesh::raw_mesh_node_options_t{
      .descriptor = descriptor ("forwarding-source"),
      .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()});
    const auto target = bytes ("missing-forwarding-target");
    protocol::application_payload_t payload{"Forwarding", "application/octet-stream", {}};
    assert (count_allocations ([] {}) == 0);
    assert (count_allocations ([] {
                auto *allocation = ::operator new (64);
                ::operator delete (allocation);
            })
            == 1);
    std::vector<std::uint8_t> copied_target;
    std::optional<zlink::message_t> encoded_header;
    const auto target_copy_allocations = count_allocations ([&] { copied_target = target; });
    std::vector<std::uint8_t> ().swap (copied_target);
    std::shared_ptr<const mesh::admitted_peer_t> peer;
    const auto argument_allocations = count_allocations ([&] {
        copied_target = target;
        encoded_header.emplace (protocol::encode_node_send_header_message ());
        peer = source.topology ().peer (target);
    });
    assert (copied_target == target && encoded_header && encoded_header->size () != 0 && !peer);
    std::optional<fw::task_t<zlink::submit_result_t>> ready;
    const auto result_allocations = count_allocations ([&] {
        ready.emplace (
          fw::result_t<zlink::submit_result_t>::success (zlink::submit_result_t::not_connected));
    });
    std::optional<fw::task_t<zlink::submit_result_t>> submission;
    const auto observed = count_allocations (
      [&] { submission.emplace (source.send_to_node_result (target, std::move (payload))); });
    assert (submission->await_ready ());
    assert (submission->result ().value () == zlink::submit_result_t::not_connected);
    // The terminal owner needs at most one coroutine frame in addition to its
    // result state; forwarding layers do not need another asynchronous result.
    constexpr std::size_t maximum_terminal_frames = 1;
    const auto allowed = argument_allocations + result_allocations + maximum_terminal_frames;
    std::fprintf (stderr,
                  "node send allocations observed=%zu allowed=%zu arguments=%zu result=%zu\n",
                  observed, allowed, argument_allocations, result_allocations);
    assert (observed <= allowed);
    // Synchronous forwarding borrows the routing id until native submission;
    // only the encoded header and the terminal task own new storage here.
    const auto borrowed_arguments = argument_allocations - target_copy_allocations;
    assert (observed == borrowed_arguments + result_allocations);
    auto temporary_target = source.send_to_node_result (
      bytes ("missing-forwarding-temporary"), {"Forwarding", "application/octet-stream", {}});
    assert (temporary_target.await_ready ());
    assert (temporary_target.result ().value () == zlink::submit_result_t::not_connected);
    const auto admitted = descriptor ("temporary-admitted-target");
    assert (source.topology ().admit (admitted, admitted.node_routing_id)
            == mesh::peer_admission_result_t::admitted);
    auto temporary_admitted_target = source.send_to_node_result (
      bytes ("temporary-admitted-target"), {"Forwarding", "application/octet-stream", {}});
    assert (temporary_admitted_target.await_ready ());
    assert (temporary_admitted_target.result ().value () == zlink::submit_result_t::terminated);
}
}
