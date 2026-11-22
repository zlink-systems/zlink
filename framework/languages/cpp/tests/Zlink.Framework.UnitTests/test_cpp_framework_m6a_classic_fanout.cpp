/* SPDX-License-Identifier: Elastic-2.0 */

#include "m6a_runtime_tests.hpp"
#include "../support/m6a_mesh_fixture.hpp"


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

void verify_manual_and_automatic_classic_fanout ()
{
    fanout::raw_fanout_publisher_t publisher (
      "tcp://127.0.0.1:0", {}, false, std::nullopt,
      std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ());
    publisher.start ();
    fanout::raw_fanout_subscriber_t manual (
      nullptr, {}, std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ());
    const auto publisher_id = bytes ("publisher-a");
    assert (manual.connect_manual (publisher_id, publisher.endpoint ()));

    auto receive_now = std::chrono::steady_clock::now ();
    bool beacon_received = false;
    const auto deadline = receive_now + 2s;
    std::size_t beacon_tick = 1;
    while (!beacon_received && std::chrono::steady_clock::now () < deadline) {
        (void) publisher.tick (receive_now
                               + fanout::fanout_beacon_interval * static_cast<int> (beacon_tick++));
        const auto [status, received] = manual.try_receive (receive_now);
        static_cast<void> (received);
        beacon_received = status == fanout::fanout_receive_status_t::beacon;
        if (!beacon_received) {
            std::this_thread::sleep_for (2ms);
        }
    }
    assert (beacon_received);
    assert (manual.ready (publisher_id));
    bool application_received = false;
    for (std::size_t attempt = 0; attempt < 100 && !application_received; ++attempt) {
        await_task (publisher.publish ("fanout-alpha", "topic-a",
                                       {"FanoutProbe", "application/json", bytes ("fanout")}));
        const auto [status, received] = manual.try_receive (receive_now);
        if (status == fanout::fanout_receive_status_t::application) {
            assert (received);
            assert (received->publisher_routing_id == publisher_id);
            assert (received->topic == "topic-a");
            const protocol::application_payload_t expected{"FanoutProbe", "application/json",
                                                           bytes ("fanout")};
            assert (received->payload == expected);
            application_received = true;
        } else {
            std::this_thread::sleep_for (2ms);
        }
    }
    assert (application_received);
    assert (manual.tick (receive_now + fanout::fanout_receive_deadline)
            == std::vector<std::vector<std::uint8_t>>{publisher_id});
    assert (!manual.ready (publisher_id));

    fanout::raw_fanout_subscriber_t automatic (
      nullptr, {}, std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ());
    fanout::fanout_publisher_intent_t automatic_descriptor{publisher_id, 1, publisher.endpoint (),
                                                           mesh::service_node_state_t::serving};
    automatic.reconcile_automatic ({automatic_descriptor});
    assert (automatic.publisher_count () == 1);
    bool automatic_ready = false;
    for (std::size_t attempt = 0; attempt < 100 && !automatic_ready; ++attempt) {
        (void) publisher.tick (receive_now
                               + fanout::fanout_beacon_interval * static_cast<int> (beacon_tick++));
        const auto [status, received] = automatic.try_receive (receive_now);
        static_cast<void> (received);
        automatic_ready = status == fanout::fanout_receive_status_t::beacon;
        if (!automatic_ready) {
            std::this_thread::sleep_for (2ms);
        }
    }
    assert (automatic_ready);
    assert (automatic.ready (publisher_id));

    automatic_descriptor.lifecycle_generation = 2;
    automatic.reconcile_automatic ({automatic_descriptor});
    assert (automatic.publisher_count () == 1);
    assert (!automatic.ready (publisher_id));

    automatic_descriptor.state = mesh::service_node_state_t::draining;
    automatic.reconcile_automatic ({automatic_descriptor});
    assert (automatic.publisher_count () == 0);

    bool reserved_rejected = false;
    try {
        await_task (publisher.publish ("fanout-alpha",
                                       fanout::raw_fanout_publisher_t::reserved_topic (),
                                       {"Reserved", "application/json", {}}));
    }
    catch (const zlink::framework::framework_exception_t &error) {
        reserved_rejected =
          error.kind () == zlink::framework::framework_error_kind_t::protocol_error;
    }
    assert (reserved_rejected);
}
}
