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
#include "../support/mesh_node_vertical_location_store_fixture.hpp"
#include "../support/mesh_node_vertical_transport_fixture.hpp"

using namespace framework_mesh_node_vertical_fixture;

namespace zlink::framework::runtime
{
// Reaches the two steps of one status query so a test can order two queries between them.
struct route_mesh_runtime_service_test_access_t
{
    static mesh_node_snapshot_t query_projection (const route_mesh_runtime_service_t &runtime,
                                                  const std::string &mesh_name)
    {
        return runtime.query_projection (mesh_name);
    }

    static mesh_node_snapshot_t complete_query (const route_mesh_runtime_service_t &runtime,
                                                const std::string &mesh_name,
                                                mesh_node_snapshot_t projection)
    {
        return runtime.complete_query (mesh_name, std::move (projection));
    }
};
} // namespace zlink::framework::runtime

using namespace std::chrono_literals;

namespace
{
template <typename T>
concept exposes_descriptor_revision = requires (const T &value) { value.descriptor_revision; };

template <typename T>
concept exposes_endpoint = requires (const T &value) { value.endpoint; };

static_assert (!exposes_descriptor_revision<zlink::framework::mesh_node_snapshot_t>);

static_assert (!exposes_endpoint<zlink::framework::mesh_node_snapshot_t>);

static_assert (!exposes_descriptor_revision<zlink::framework::mesh_peer_snapshot_t>);

static_assert (!exposes_endpoint<zlink::framework::mesh_peer_snapshot_t>);
} // namespace

namespace framework_mesh_node_vertical_tests
{
void verify_observation_starts_with_weight_changed_before_subscription ()
{
    namespace fw = zlink::framework;
    auto registration = make_node ("tcp://127.0.0.1:0", "observe-initial-weight");
    registration->placement_weight = 100;
    auto node = std::make_shared<fw::detail::mesh_node_runtime_t> (registration);
    node->start ().result ().value ();
    monitoring_mesh_store_t monitoring_store;
    fw::runtime::route_mesh_runtime_service_t runtime ({node}, nullptr, &monitoring_store);
    runtime.start ();
    fw::runtime::route_mesh_runtime_options_service_t runtime_options ({node});
    std::mutex event_mutex;
    std::condition_variable event_ready;
    std::vector<fw::observed_status_t<fw::mesh_node_snapshot_t>> received;
    const auto on_status = [&] (const auto &observed) {
        {
            std::lock_guard lock (event_mutex);
            received.push_back (observed);
        }
        event_ready.notify_one ();
    };
    auto before_subscription = runtime.observe ("vertical-mesh", 1, on_status);
    {
        std::unique_lock lock (event_mutex);
        assert (event_ready.wait_for (lock, 2s, [&] {
            return !received.empty () && received.back ().status.placement.is_available;
        }));
    }
    runtime_options.mesh ("vertical-mesh").placement_weight (0);
    {
        std::unique_lock lock (event_mutex);
        assert (event_ready.wait_for (lock, 2s, [&] {
            return !received.empty () && !received.back ().status.placement.is_available;
        }));
    }
    before_subscription->close ();
    received.clear ();

    auto observation = runtime.observe ("vertical-mesh", 1, on_status);
    {
        std::unique_lock lock (event_mutex);
        assert (event_ready.wait_for (lock, 2s, [&] { return !received.empty (); }));
        const auto &observed = received.front ();
        assert (!observed.status.placement.is_available);
        assert (observed.status.placement.unavailable_reason
                == fw::topology_reason_t::capacity_exceeded);
        assert (observed.loss.coalesced_count == 0);
        assert (observed.loss.discarded_terminal_count == 0);
    }
    observation->close ();
    runtime.stop ();
    node->stop ();
}

void verify_public_runtime_surface ()
{
    auto registration = make_node ("tcp://127.0.0.1:0", "runtime-a");
    registration->channels.emplace (
      "client-only", zlink::framework::detail::mesh_channel_registration_t{100, {}, true, false});
    registration->actor_limit = 0;
    registration->spot_limit = 17;
    registration->spot_state->activation_admission->set_limit (5);
    registration->placement_weight = 42;
    auto node = std::make_shared<zlink::framework::detail::mesh_node_runtime_t> (registration);
    node->start ().result ().value ();
    monitoring_mesh_store_t monitoring_store;
    const auto node_status = node->status ();
    zlink::framework::mesh_node_descriptor_t local_descriptor;
    local_descriptor.mesh_name = "vertical-mesh";
    local_descriptor.rid = node_status.routing_id ();
    local_descriptor.lifecycle_generation = node_status.lifecycle_generation ();
    local_descriptor.descriptor_revision = 3;
    local_descriptor.object_role = zlink::framework::object_role_t::server;
    local_descriptor.channel_weights.emplace ("work", 100);
    local_descriptor.state = zlink::framework::framework_runtime_state_t::serving;
    local_descriptor.placement_weight = 42;
    local_descriptor.capacity = {
      .actors = {.active = 4, .reserved = 2, .limit = 0},
      .spots = {.active = 3, .reserved = 1, .limit = 17},
      .spot_types = {{.object_kind = zlink::framework::placement_object_kind_t::user_spot,
                      .stable_type = "room",
                      .usage = {.active = 3, .reserved = 1, .limit = 9}}}};
    local_descriptor.activation_concurrency = {.active = 2, .limit = 5};
    monitoring_store.set_local (local_descriptor);
    assert (registration->listen_endpoint == node->status ().local_endpoint ());
    assert (registration->listen_endpoint != "tcp://127.0.0.1:0");
    std::atomic host_state{zlink::framework::framework_runtime_state_t::serving};
    auto runtime = std::make_shared<zlink::framework::runtime::route_mesh_runtime_service_t> (
      std::vector<std::shared_ptr<zlink::framework::detail::mesh_node_runtime_t>>{node}, nullptr,
      &monitoring_store, nullptr, [&] { return host_state.load (); });
    runtime->start ();
    zlink::framework::runtime::route_mesh_runtime_options_service_t runtime_options ({node});

    const auto first = runtime->snapshot ("vertical-mesh");
    const auto second = runtime->snapshot ("vertical-mesh");
    assert (first.mesh_name == "vertical-mesh");
    assert (first.state == zlink::framework::topology_state_t::ready);
    assert (first.is_ready);
    // Status counts come from this MeshNode's activation records, not from the
    // Location Store projection (runtime monitoring §5): nothing is active here.
    assert (first.placement.active_actor_count == 0);
    assert (first.placement.active_spot_count == 0);
    assert (first.placement.is_available);
    assert (first.channels.size () == 2);
    const auto client_only_initial =
      std::find_if (first.channels.begin (), first.channels.end (),
                    [] (const auto &channel) { return channel.channel_name == "client-only"; });
    assert (client_only_initial != first.channels.end ());
    assert (!client_only_initial->is_ready);
    assert (client_only_initial->ready_target_count == 0);
    const auto work_initial =
      std::find_if (first.channels.begin (), first.channels.end (),
                    [] (const auto &channel) { return channel.channel_name == "work"; });
    assert (work_initial != first.channels.end ());
    assert (work_initial->ready_target_count == 1);
    assert (work_initial->is_ready);
    assert (first.sequence == 1);
    assert (second.sequence == first.sequence);
    assert (second.observed_at == first.observed_at);
    auto &channel_options = runtime_options.channel ("work");
    channel_options.weight (0);
    assert (channel_options.weight () == 0);
    const auto without_work = runtime->snapshot ("vertical-mesh");
    assert (!without_work.channels.front ().is_ready);
    assert (without_work.sequence == second.sequence + 1);
    assert (runtime->snapshot ("vertical-mesh").sequence == without_work.sequence);
    channel_options.weight (100);
    assert (channel_options.weight () == 100);
    runtime_options.mesh ("vertical-mesh").placement_weight (0);
    assert (runtime_options.mesh ("vertical-mesh").placement_weight () == 0);
    runtime_options.mesh ("vertical-mesh").placement_weight (100);
    assert (runtime_options.mesh ("vertical-mesh").placement_weight () == 100);

    // Runtime monitoring §4 and §6: a query returns the value at that time. Two queries project
    // from the same published status. The older one (it reports a ready peer the node no longer
    // has) wins the publication turn; the later query must still return the current state.
    {
        using access = zlink::framework::runtime::route_mesh_runtime_service_test_access_t;
        const auto base = runtime->snapshot ("vertical-mesh");
        auto older = access::query_projection (*runtime, "vertical-mesh");
        auto newer = access::query_projection (*runtime, "vertical-mesh");
        assert (older.sequence == base.sequence);
        assert (newer.sequence == base.sequence);
        older.ready_peer_count = base.ready_peer_count + 1;
        const auto older_published =
          access::complete_query (*runtime, "vertical-mesh", std::move (older));
        assert (older_published.ready_peer_count == base.ready_peer_count + 1);
        assert (older_published.sequence == base.sequence + 1);
        const auto returned = access::complete_query (*runtime, "vertical-mesh", std::move (newer));
        if (returned.ready_peer_count != base.ready_peer_count
            || returned.sequence != base.sequence + 2) {
            std::cerr << "status query returned the older projection: sequence "
                      << returned.sequence << " ready peers " << returned.ready_peer_count << '\n';
            std::abort ();
        }
    }

    std::mutex event_mutex;
    std::condition_variable event_ready;
    std::vector<zlink::framework::mesh_node_snapshot_t> received;
    std::this_thread::sleep_for (150ms);
    auto observation = runtime->observe (
      "vertical-mesh", 1,
      [&] (const zlink::framework::observed_status_t<zlink::framework::mesh_node_snapshot_t>
             &observed) {
          const auto &status = observed.status;
          {
              std::lock_guard lock (event_mutex);
              received.push_back (status);
          }
          event_ready.notify_one ();
      });
    auto wait_for_snapshot = [&] (const auto &predicate) {
        std::unique_lock lock (event_mutex);
        return event_ready.wait_for (
          lock, 2s, [&] { return !received.empty () && predicate (received.back ()); });
    };
    {
        std::unique_lock lock (event_mutex);
        assert (event_ready.wait_for (lock, 2s, [&] { return !received.empty (); }));
        assert (received.back ().mesh_name == "vertical-mesh");
        assert (received.back ().state == zlink::framework::topology_state_t::ready);
    }

    auto remote_location = local_descriptor;
    remote_location.rid = zlink::routing_id_t::from (std::string ("runtime-peer"));
    remote_location.lifecycle_generation = 99;
    remote_location.descriptor_revision = 1;
    remote_location.endpoint = "tcp://127.0.0.1:39999";
    remote_location.channel_weights.emplace ("client-only", 100);
    monitoring_store.set_remote (remote_location);
    assert (wait_for_snapshot ([] (const auto &snapshot) {
        return snapshot.state == zlink::framework::topology_state_t::degraded && !snapshot.is_ready
               && std::any_of (
                 snapshot.peers.begin (), snapshot.peers.end (), [] (const auto &peer) {
                     return peer.state == zlink::framework::peer_state_t::not_connected;
                 });
    }));
    assert (!runtime->is_ready ("vertical-mesh"));

    auto remote_service = node->native_node ().transport ().topology ().local_descriptor ();
    remote_service.node_routing_id = remote_location.rid.to_bytes ();
    remote_service.lifecycle_generation = remote_location.lifecycle_generation;
    remote_service.descriptor_revision = remote_location.descriptor_revision;
    remote_service.advertised_endpoint = remote_location.endpoint;
    remote_service.channels.push_back (
      zlink::framework::runtime::mesh::service_channel_descriptor_t{"client-only", 100});
    std::sort (remote_service.channels.begin (), remote_service.channels.end (),
               [] (const auto &left, const auto &right) { return left.name < right.name; });
    const std::vector<std::uint8_t> connection_id{0x41, 0x42};
    assert (node->native_node ().transport ().topology ().admit (remote_service, connection_id)
            == zlink::framework::runtime::mesh::peer_admission_result_t::admitted);
    assert (wait_for_snapshot ([] (const auto &snapshot) {
        return snapshot.state == zlink::framework::topology_state_t::ready
               && snapshot.ready_peer_count == 1;
    }));
    assert (runtime->is_ready ("vertical-mesh"));
    {
        const auto current = runtime->snapshot ("vertical-mesh");
        const auto client_only =
          std::find_if (current.channels.begin (), current.channels.end (),
                        [] (const auto &channel) { return channel.channel_name == "client-only"; });
        assert (client_only != current.channels.end ());
        assert (client_only->is_ready);
        assert (client_only->ready_target_count == 1);
    }

    channel_options.weight (0);
    assert (wait_for_snapshot ([] (const auto &snapshot) {
        const auto work =
          std::find_if (snapshot.channels.begin (), snapshot.channels.end (),
                        [] (const auto &channel) { return channel.channel_name == "work"; });
        return work != snapshot.channels.end () && work->ready_target_count == 1;
    }));
    channel_options.weight (100);
    assert (wait_for_snapshot ([] (const auto &snapshot) {
        const auto work =
          std::find_if (snapshot.channels.begin (), snapshot.channels.end (),
                        [] (const auto &channel) { return channel.channel_name == "work"; });
        return work != snapshot.channels.end () && work->ready_target_count == 2;
    }));

    assert (node->native_node ().transport ().topology ().disconnect (
      remote_service.node_routing_id, connection_id));
    assert (wait_for_snapshot ([] (const auto &snapshot) {
        return snapshot.state == zlink::framework::topology_state_t::degraded;
    }));
    assert (!runtime->is_ready ("vertical-mesh"));
    monitoring_store.set_remote (std::nullopt);
    assert (wait_for_snapshot ([] (const auto &snapshot) {
        return snapshot.state == zlink::framework::topology_state_t::ready
               && snapshot.peers.empty ();
    }));
    assert (runtime->is_ready ("vertical-mesh"));

    // Local weight changes enter through the same owner as runtime option reads.
    runtime_options.mesh ("vertical-mesh").placement_weight (0);
    auto unavailable = monitoring_store.local ();
    ++unavailable.descriptor_revision;
    unavailable.placement_weight = 0;
    (void) monitoring_store.update_mesh_node (unavailable,
                                              zlink::framework::location_write_intent_t::renew);
    assert (wait_for_snapshot ([] (const auto &snapshot) {
        return !snapshot.placement.is_available
               && snapshot.placement.unavailable_reason
                    == zlink::framework::topology_reason_t::capacity_exceeded;
    }));

    runtime_options.mesh ("vertical-mesh").placement_weight (100);
    unavailable.placement_weight = 100;
    // The reserved slots alone exhaust both limits (MeshNode §5.1).
    unavailable.capacity.actors.limit = 2;
    unavailable.capacity.spots.limit = 1;
    ++unavailable.descriptor_revision;
    (void) monitoring_store.update_mesh_node (unavailable,
                                              zlink::framework::location_write_intent_t::renew);
    assert (
      wait_for_snapshot ([] (const auto &snapshot) { return !snapshot.placement.is_available; }));

    unavailable.capacity.spots.limit = 17;
    ++unavailable.descriptor_revision;
    (void) monitoring_store.update_mesh_node (unavailable,
                                              zlink::framework::location_write_intent_t::renew);
    assert (
      wait_for_snapshot ([] (const auto &snapshot) { return snapshot.placement.is_available; }));

    host_state.store (zlink::framework::framework_runtime_state_t::relocating);
    unavailable.state = zlink::framework::framework_runtime_state_t::relocating;
    ++unavailable.descriptor_revision;
    (void) monitoring_store.update_mesh_node (unavailable,
                                              zlink::framework::location_write_intent_t::renew);
    assert (wait_for_snapshot ([] (const auto &snapshot) {
        return snapshot.state == zlink::framework::topology_state_t::stopping && !snapshot.is_ready
               && std::all_of (snapshot.channels.begin (), snapshot.channels.end (),
                               [] (const auto &channel) { return !channel.is_ready; })
               && std::any_of (snapshot.channels.begin (), snapshot.channels.end (),
                               [] (const auto &channel) { return channel.ready_target_count > 0; });
    }));
    assert (!runtime->is_ready ("vertical-mesh"));

    host_state.store (zlink::framework::framework_runtime_state_t::serving);
    unavailable.state = zlink::framework::framework_runtime_state_t::serving;
    ++unavailable.descriptor_revision;
    (void) monitoring_store.update_mesh_node (unavailable,
                                              zlink::framework::location_write_intent_t::renew);
    assert (wait_for_snapshot ([] (const auto &snapshot) {
        return snapshot.state == zlink::framework::topology_state_t::ready && snapshot.is_ready;
    }));

    bool rejected_capacity = false;
    try {
        (void) runtime->observe (
          "vertical-mesh", 0,
          [] (const zlink::framework::observed_status_t<zlink::framework::mesh_node_snapshot_t> &) {
          });
    }
    catch (const zlink::framework::framework_exception_t &) {
        rejected_capacity = true;
    }
    assert (rejected_capacity);

    bool rejected_mesh = false;
    try {
        (void) runtime->snapshot ("missing");
    }
    catch (const zlink::framework::framework_exception_t &) {
        rejected_mesh = true;
    }
    assert (rejected_mesh);
    assert (runtime->is_ready ("vertical-mesh"));

    runtime->stop ();
    assert (wait_for_snapshot ([] (const auto &snapshot) {
        return snapshot.state == zlink::framework::topology_state_t::stopped && !snapshot.is_ready
               && snapshot.ready_peer_count == 0
               && std::all_of (snapshot.channels.begin (), snapshot.channels.end (),
                               [] (const auto &channel) {
                                   return !channel.is_ready && channel.ready_target_count == 0;
                               });
    }));
    assert (!runtime->is_ready ("vertical-mesh"));
    assert (runtime->snapshot ("vertical-mesh").state
            == zlink::framework::topology_state_t::stopped);
    const auto terminal = runtime->snapshot ("vertical-mesh");
    assert (runtime->snapshot ("vertical-mesh").sequence == terminal.sequence);
    assert (runtime->snapshot ("vertical-mesh").observed_at == terminal.observed_at);
    {
        std::lock_guard lock (event_mutex);
        assert (received.back ().sequence == terminal.sequence);
        assert (received.back ().observed_at == terminal.observed_at);
    }
    observation->close ();
    node->stop ();
}

void verify_manual_peer_public_status ()
{
    auto local_registration = make_node ("tcp://127.0.0.1:0", "manual-status-local");
    local_registration->channels.emplace (
      "manual-channel",
      zlink::framework::detail::mesh_channel_registration_t{100, {}, true, false});
    auto remote_registration = make_node ("tcp://127.0.0.1:0", "manual-status-remote");
    remote_registration->channels.emplace (
      "manual-channel", zlink::framework::detail::mesh_channel_registration_t{100, {}, true, true});
    auto local =
      std::make_shared<zlink::framework::detail::mesh_node_runtime_t> (local_registration);
    auto remote =
      std::make_shared<zlink::framework::detail::mesh_node_runtime_t> (remote_registration);
    local->start ().result ().value ();
    remote->start ().result ().value ();
    const auto remote_endpoint = remote->listen_endpoint ();
    const auto remote_status = remote->status ();
    auto runtime = std::make_shared<zlink::framework::runtime::route_mesh_runtime_service_t> (
      std::vector<std::shared_ptr<zlink::framework::detail::mesh_node_runtime_t>>{local}, nullptr);
    zlink::framework::route_mesh_runtime_t &public_runtime = *runtime;
    runtime->start ();
    local->connect_peer (remote_endpoint);

    const auto dispatch_ready = [] (zlink::framework::detail::mesh_node_runtime_t &node) {
        (void) std::move (
          node.dispatch_ready ([] (const zlink::framework::runtime::host::ready_record_t &,
                                   const zlink::framework::runtime::host::receive_record_t &,
                                   std::vector<zlink::message_t>, auto) {}))
          .result ()
          .value ();
    };
    const auto deadline = std::chrono::steady_clock::now () + 5s;
    bool admitted = false;
    while (std::chrono::steady_clock::now () < deadline) {
        dispatch_ready (*local);
        dispatch_ready (*remote);
        admitted = local->has_admitted_peer (remote_status.routing_id (),
                                             remote_status.lifecycle_generation ());
        if (admitted)
            break;
        std::this_thread::sleep_for (1ms);
    }
    if (!admitted)
        std::fprintf (stderr, "[manual-status] manual endpoint handshake was not admitted\n");
    assert (admitted);

    const auto status = public_runtime.snapshot ("vertical-mesh");
    const auto channel =
      std::find_if (status.channels.begin (), status.channels.end (),
                    [] (const auto &value) { return value.channel_name == "manual-channel"; });
    const auto ready_target_count =
      channel == status.channels.end () ? 0 : channel->ready_target_count;
    if (status.peers.size () != 1 || status.ready_peer_count != 1 || ready_target_count != 1) {
        std::fprintf (stderr,
                      "[manual-status] peerCount=%zu readyPeerCount=%u readyTargetCount=%u\n",
                      status.peers.size (), status.ready_peer_count, ready_target_count);
    }
    assert (status.peers.size () == 1);
    assert (status.ready_peer_count == 1);
    assert (ready_target_count == 1);

    runtime->stop ();
    local->disconnect_peer (remote_endpoint);
    remote->stop ();
    local->stop ();
}

void verify_monitor_pump_does_not_wait_for_spot_lane ()
{
    auto registration = make_node ("tcp://127.0.0.1:0", "monitor-pump-node");
    auto node = std::make_shared<zlink::framework::detail::mesh_node_runtime_t> (registration);
    node->start ().result ().value ();

    const auto status = node->status ();
    zlink::framework::mesh_node_descriptor_t descriptor;
    descriptor.mesh_name = "vertical-mesh";
    descriptor.rid = status.routing_id ();
    descriptor.lifecycle_generation = status.lifecycle_generation ();
    descriptor.object_role = zlink::framework::object_role_t::server;
    descriptor.state = zlink::framework::framework_runtime_state_t::serving;
    monitoring_mesh_store_t store;
    store.set_local (descriptor);
    monitoring_location_query_t location_query;
    std::promise<void> unhealthy_polled;
    location_query.on_unhealthy_status = [&] { unhealthy_polled.set_value (); };
    zlink::framework::runtime::route_mesh_runtime_service_t runtime ({node}, &location_query,
                                                                     &store);
    runtime.start ();
    auto observation = runtime.observe (
      "vertical-mesh", 1,
      [] (const zlink::framework::observed_status_t<zlink::framework::mesh_node_snapshot_t> &) {});

    std::promise<void> lane_entered;
    std::promise<void> release_lane;
    auto release = release_lane.get_future ().share ();
    assert (registration->spot_state->lane.try_post ([&] {
        lane_entered.set_value ();
        release.wait ();
    }));
    lane_entered.get_future ().wait ();
    location_query.set_store_healthy (false);
    unhealthy_polled.get_future ().wait ();
    runtime.stop ();
    release_lane.set_value ();
    observation->close ();
    node->stop ();
}

void verify_slow_observer_does_not_block_stop ()
{
    auto registration = make_node ("tcp://127.0.0.1:0", "slow-observer");
    auto node = std::make_shared<zlink::framework::detail::mesh_node_runtime_t> (registration);
    node->start ().result ().value ();
    auto runtime = std::make_shared<zlink::framework::runtime::route_mesh_runtime_service_t> (
      std::vector<std::shared_ptr<zlink::framework::detail::mesh_node_runtime_t>>{node}, nullptr);
    runtime->start ();
    zlink::framework::runtime::route_mesh_runtime_options_service_t runtime_options ({node});

    std::mutex gate;
    std::condition_variable changed;
    bool entered = false;
    bool released = false;
    std::size_t stopped_count = 0;
    auto observation = runtime->observe (
      "vertical-mesh", 4,
      [&] (const zlink::framework::observed_status_t<zlink::framework::mesh_node_snapshot_t>
             &observed) {
          const auto &snapshot = observed.status;
          std::unique_lock lock (gate);
          if (!entered) {
              entered = true;
              changed.notify_all ();
              changed.wait (lock, [&] { return released; });
          }
          if (snapshot.state == zlink::framework::topology_state_t::stopped) {
              ++stopped_count;
              changed.notify_all ();
          }
      });
    {
        std::unique_lock lock (gate);
        assert (changed.wait_for (lock, 1s, [&] { return entered; }));
    }

    // Queue a non-terminal change while the callback is blocked. Stop must
    // replace it with one immutable terminal snapshot before the runtime is
    // destroyed; the callback must never read the destroyed runtime.
    runtime_options.channel ("work").weight (0);
    auto stopping = std::async (std::launch::async, [&] { runtime->stop (); });
    assert (stopping.wait_for (1s) == std::future_status::ready);
    stopping.get ();
    runtime.reset ();
    {
        std::lock_guard lock (gate);
        released = true;
    }
    changed.notify_all ();
    {
        std::unique_lock lock (gate);
        assert (changed.wait_for (lock, 1s, [&] { return stopped_count == 1; }));
    }
    std::this_thread::sleep_for (50ms);
    {
        std::lock_guard lock (gate);
        assert (stopped_count == 1);
    }
    observation->close ();
    node->stop ();
}
} // namespace framework_mesh_node_vertical_tests
