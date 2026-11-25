/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/stateful_ingress_fixture.hpp"
#include "runtime/backend/raw_binding_adapter.hpp"
#include "runtime/locations/store_location_resolvers.hpp"
#include "../support/m6b_cases.hpp"
#include "../support/m6b_host_access.hpp"
#include "../support/m6b_task.hpp"
#include "../support/m6b_mesh_fixture.hpp"
#include "../support/m6b_stateful_fixture.hpp"
#include "../support/stateful_object_sync.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/stateful/raw_stateful_dispatch.hpp"
#include "runtime/stateful/public_host_runtime.hpp"
#include "runtime/stateful/stateful_object_runtime.hpp"
#include <zlink/Contracts/Core/context.hpp>
#include <zlink/Contracts/Core/routing_id.hpp>
#include <zlink/Contracts/Messaging/message.hpp>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <future>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace foundation = zlink::framework::runtime::foundation;
namespace mesh = zlink::framework::runtime::mesh;
namespace protocol = zlink::framework::runtime::protocol;
namespace stateful = zlink::framework::runtime::stateful;
namespace host = zlink::framework::runtime::host;
namespace spots = zlink::framework::detail;
using namespace std::chrono_literals;
using zlink::framework::test::begin_reserved_object;

namespace zlink::framework::test::m6b
{
// The public Instance call payload of the Spot Close ready-route cases.
void verify_spot_id_contract ()
{
    using namespace zlink::framework;
    static_assert (std::is_same_v<spot_id_t, std::string>);

    const auto user = detail::new_user_spot_id ();
    assert (user.size () == 36);
    assert (user[8] == '-' && user[13] == '-' && user[18] == '-' && user[23] == '-'
            && user[14] == '4');
    assert (user[19] == '8' || user[19] == '9' || user[19] == 'a' || user[19] == 'b');
    assert (detail::valid_spot_id (user));

    const auto entry = detail::new_entry_spot_id ("server");
    assert (entry.starts_with ("server-entry-"));
    assert (detail::is_framework_entry_spot_id (entry));
    assert (entry != detail::new_entry_spot_id ("server"));

    assert (!detail::valid_spot_id (""));
    assert (!detail::valid_spot_id (std::string (256, 'x')));
    assert (!detail::valid_spot_id (std::string ("\xc3\x28", 2)));
    assert (detail::valid_spot_id ("Room") && detail::valid_spot_id ("room"));

    bool rejected = false;
    try {
        (void) spot_ref_t (std::string (256, 'x'), 1, "mesh", node_rid_t::from_string ("node"));
    }
    catch (const std::invalid_argument &) {
        rejected = true;
    }
    assert (rejected);

    const auto max_generation =
      static_cast<std::uint64_t> (std::numeric_limits<std::int64_t>::max ());
    const auto max_ref =
      spot_ref_t ("room", max_generation, "mesh", node_rid_t::from_string ("node"));
    assert (max_ref.object_generation () == max_generation);
    rejected = false;
    try {
        (void) spot_ref_t ("room", max_generation + 1, "mesh", node_rid_t::from_string ("node"));
    }
    catch (const std::invalid_argument &) {
        rejected = true;
    }
    assert (rejected);
}

void verify_spot_route_fence_admission_precedes_body_decode ()
{
    auto state = std::make_shared<spots::spot_context_state_t> ();
    state->spot_id = "room";
    state->object_generation = 7;
    state->authority_owner_generation = 11;
    const zlink::framework::location_owner_token_t owner{"node-owner", 31};
    const protocol::spot_route_fence_t valid{"room", 7, {}, 3, 11, 31};

    assert (state->accepts_route_fence (valid, owner));

    auto stale = valid;
    stale.object_generation++;
    // General Spot messages address the logical Spot. Recreating the Spot on
    // the same live owner must not turn an already-routed message stale.
    assert (state->accepts_route_fence (stale, owner));
    stale = valid;
    stale.authority_owner_generation++;
    assert (!state->accepts_route_fence (stale, owner));
    stale = valid;
    stale.owner_lease_generation++;
    assert (!state->accepts_route_fence (stale, owner));
    stale = valid;
    stale.spot_id = "other";
    assert (!state->accepts_route_fence (stale, owner));
    assert (!state->accepts_route_fence (
      valid, std::optional<zlink::framework::location_owner_token_t>{}));
}

void verify_public_host_route_fence_reads_store_without_second_cache (bool release_lease)
{
    using namespace zlink::framework;
    auto store = std::make_shared<runtime::in_memory_location_repository_t> ();
    store->configure_reclaim_margin (std::chrono::milliseconds::zero ());
    const auto owner = std::get<owner_lease_claimed_t> (
                         store->claim_owner_lease ("route-cache-owner", 5s).result ().value ())
                         .token;
    mesh_node_descriptor_t location{
      .mesh_name = "m6b-mesh",
      .rid = zlink::routing_id_t::from ("route-cache-target"),
      .lifecycle_generation = 1,
      .descriptor_revision = 1,
      .endpoint = "tcp://127.0.0.1:1",
      .application_version = 1,
      .object_capabilities = {{.object_kind = placement_object_kind_t::user_spot,
                               .stable_type = "room"}},
      .object_role = object_role_t::server,
      .capacity = {.spots = {.limit = 8}},
      .state = framework_runtime_state_t::serving,
      .security_identity = "route-cache-test",
      .owner_id = owner.owner_id,
      .lease_generation = owner.lease_generation};
    assert (store->update_mesh_node (location, location_write_intent_t::new_claim)
              .result ()
              .value ()
              .status
            == location_write_status_t::stored);
    const object_reserve_request_t reserve{
      .key = {placement_object_kind_t::user_spot, "route-cache-spot"},
      .intent = {.stable_type = "room"},
      .target = {.mesh_name = location.mesh_name,
                 .node_rid = node_rid_t::from_string (location.rid.to_string ()),
                 .node_lifecycle_generation = location.lifecycle_generation,
                 .owner = owner},
      .capacity_bundle = {
        .spot_slots = 1,
        .spot_type = spot_type_capacity_delta_t{placement_object_kind_t::user_spot, "room", 1}}};
    const auto reserved = std::get<object_reserved_t> (store->reserve (reserve).result ().value ());
    const auto committed = std::get<object_committed_t> (
      store->commit ({reserve.key, reserved.fence, {std::byte{1}}}).result ().value ());

    auto source = std::make_shared<host::public_host_runtime_t> (host::host_options_t{
      mesh::raw_mesh_node_options_t{
        .descriptor = descriptor ("route-cache-source"),
        .runtime_failures =
          std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()},
      "entry",
      {"framework.spot"},
      3s});
    auto target = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = descriptor ("route-cache-target"),
        .runtime_failures =
          std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()}});
    source->configure_user_spot_operations (
      store,
      [] (const stateful::object_ref_t &, const std::string &, const std::vector<std::byte> &)
        -> zlink::framework::task_t<
          zlink::framework::runtime::host::user_spot_materialize_result_t> {
          co_return host::user_spot_materialize_result_t{true, std::nullopt};
      });
    source->start ();
    target->start ();
    assert (
      source->connect_peer (target->status ().local_endpoint (), target->status ().routing_id ()));
    const auto noop_dispatch = [] (const host::ready_record_t &, const host::receive_record_t &,
                                   std::vector<zlink::message_t>, auto) {};
    const auto connect_deadline = std::chrono::steady_clock::now () + 5s;
    while (
      !ready_peer (source->transport ().topology (), target->status ().routing_id ().to_bytes ())
      && std::chrono::steady_clock::now () < connect_deadline) {
        (void) source->dispatch_ready (noop_dispatch);
        (void) target->dispatch_ready (noop_dispatch);
        std::this_thread::sleep_for (1ms);
    }
    assert (source->transport ().topology ().peer (target->status ().routing_id ().to_bytes ()));

    auto caller = source->entry_spot ();
    const auto send = [&] {
        return caller
          .send_to_spot (target->status ().routing_id (), reserve.key.global_id,
                         committed.ready.object_generation,
                         {zlink::message_t::from (std::string ("payload"))})
          .result ()
          .value ();
    };
    assert (send () == zlink::submit_result_t::ok);
    if (release_lease) {
        // The Location Store owns the fence. A lease released after the first
        // send must reach the next send, not be hidden by a host-side copy.
        assert (std::holds_alternative<owner_lease_released_t> (
          store->release_owner_lease (owner).result ().value ()));
    } else
        std::this_thread::sleep_for (3s);
    assert (send () == zlink::submit_result_t::not_found);

    source->close ();
    target->close ();
}

// Submit/completion section 5: a public Spot or Actor send writes the owner
// generation G its Location authority resolved, never the admitted peer's.
// Authority G1 against a ready peer admitted at G0 is NotConnected.
void verify_public_host_send_uses_authority_generation ()
{
    std::atomic<std::uint64_t> spot_node_generation{0};
    auto source = std::make_shared<host::public_host_runtime_t> (host::host_options_t{
      mesh::raw_mesh_node_options_t{
        .descriptor = descriptor ("authority-send-host-source"),
        .runtime_failures =
          std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()},
      "entry",
      {"framework.spot"}});
    auto target = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = descriptor ("authority-send-host-target"),
        .runtime_failures =
          std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()}});
    source->configure_spot_route_fence_resolver (
      [&spot_node_generation] (
        auto, auto, auto) -> zlink::framework::task_t<std::optional<host::route_fence_t>> {
          return zlink::framework::task_t<std::optional<host::route_fence_t>> (
            zlink::framework::result_t<std::optional<host::route_fence_t>>::success (
              host::route_fence_t{1, 1, spot_node_generation.load ()}));
      });
    source->start ();
    target->start ();
    assert (
      source->connect_peer (target->status ().local_endpoint (), target->status ().routing_id ()));
    const auto noop_dispatch = [] (const host::ready_record_t &, const host::receive_record_t &,
                                   std::vector<zlink::message_t>, auto) {};
    const auto connect_deadline = std::chrono::steady_clock::now () + 5s;
    while (
      !ready_peer (source->transport ().topology (), target->status ().routing_id ().to_bytes ())
      && std::chrono::steady_clock::now () < connect_deadline) {
        (void) source->dispatch_ready (noop_dispatch);
        (void) target->dispatch_ready (noop_dispatch);
        std::this_thread::sleep_for (1ms);
    }
    assert (
      ready_peer (source->transport ().topology (), target->status ().routing_id ().to_bytes ()));

    const auto admitted_generation = target->status ().lifecycle_generation ();
    const auto authority_generation = admitted_generation + 1;
    const std::vector<zlink::message_t> parts{zlink::message_t::from (std::string ("payload"))};
    const auto actor = host::public_host_runtime_t::remote_actor_ref (
      target->status ().routing_id (), "authority-send-actor", 1);
    const auto send_actor = [&] (std::uint64_t node_generation) {
        return source->send_to_actor (actor, parts, {}, 1, 1, std::nullopt, node_generation)
          .result ()
          .value ();
    };
    auto caller = source->entry_spot ();
    const auto send_spot = [&] (std::uint64_t node_generation) {
        spot_node_generation.store (node_generation);
        return caller
          .send_to_spot (target->status ().routing_id (), "authority-send-spot", 1, parts)
          .result ()
          .value ();
    };

    // Authority G1, admitted peer G0: the admitted generation is not a source of G.
    assert (send_actor (authority_generation) == zlink::submit_result_t::not_connected);
    assert (send_spot (authority_generation) == zlink::submit_result_t::not_connected);
    // An Actor send without an authority generation has no route to name.
    assert (send_actor (0) == zlink::submit_result_t::not_found);
    assert (
      ready_peer (source->transport ().topology (), target->status ().routing_id ().to_bytes ()));
    // The authority generation of the ready peer is its route.
    assert (send_actor (admitted_generation) == zlink::submit_result_t::ok);
    assert (send_spot (admitted_generation) == zlink::submit_result_t::ok);

    source->close ();
    target->close ();
}

void verify_entry_spot_descriptor_mapping_survives_node_removal ()
{
    using namespace zlink::framework;
    runtime::in_memory_location_repository_t store;
    store.configure_reclaim_margin (std::chrono::milliseconds::zero ());
    const auto first_owner = std::get<owner_lease_claimed_t> (
                               store.claim_owner_lease ("entry-owner-a", 15s).result ().value ())
                               .token;
    const auto second_owner = std::get<owner_lease_claimed_t> (
                                store.claim_owner_lease ("entry-owner-b", 15s).result ().value ())
                                .token;
    const auto entry_id = detail::new_entry_spot_id ("entry-node");
    const auto descriptor = [&] (std::string rid, std::uint64_t lifecycle,
                                 const location_owner_token_t &owner, std::string id) {
        return mesh_node_descriptor_t{
          .mesh_name = "entry-mesh",
          .rid = zlink::routing_id_t::from (std::move (rid)),
          .lifecycle_generation = lifecycle,
          .descriptor_revision = 1,
          .endpoint = "tcp://127.0.0.1:1",
          .entry_spot_id = std::move (id),
          .application_version = 1,
          .object_capabilities = {{.object_kind = placement_object_kind_t::user_spot,
                                   .stable_type = "room"}},
          .object_role = object_role_t::server,
          .capacity = {.spots = {.limit = 8}},
          .state = framework_runtime_state_t::serving,
          .security_identity = "entry-test",
          .owner_id = owner.owner_id,
          .lease_generation = owner.lease_generation};
    };
    const auto first = descriptor ("entry-node-a", 41, first_owner, entry_id);
    const auto second = descriptor ("entry-node-b", 42, second_owner, entry_id);
    assert (
      store.update_mesh_node (first, location_write_intent_t::new_claim).result ().value ().status
      == location_write_status_t::stored);
    runtime::store_location_resolvers_t resolver (store);
    const auto first_route =
      resolver.resolve_spot_address (first.mesh_name, entry_id).result ().value ();
    assert (first_route);
    assert (first_route->node_rid.to_string () == first.rid.to_string ());
    assert (
      store.update_mesh_node (second, location_write_intent_t::new_claim).result ().value ().status
      == location_write_status_t::stored);

    assert (store.remove_mesh_node ({first.mesh_name, first.rid}, first_owner).result ().value ()
            == location_write_status_t::stored);
    resolver.invalidate_spot_address (entry_id);
    const auto replacement_route =
      resolver.resolve_spot_address (second.mesh_name, entry_id).result ().value ();
    assert (replacement_route);
    assert (replacement_route->node_rid.to_string () == second.rid.to_string ());
    assert (store.remove_mesh_node ({first.mesh_name, first.rid}, first_owner).result ().value ()
            == location_write_status_t::ignored_stale);

    object_reserve_request_t reserved_entry{
      .key = {placement_object_kind_t::user_spot, entry_id},
      .intent = {.stable_type = "room"},
      .target = {.mesh_name = second.mesh_name,
                 .node_rid = node_rid_t::from_string (second.rid.to_string ()),
                 .node_lifecycle_generation = second.lifecycle_generation,
                 .owner = second_owner},
      .capacity_bundle = {
        .spot_slots = 1,
        .spot_type = spot_type_capacity_delta_t{placement_object_kind_t::user_spot, "room", 1}}};
    assert (std::holds_alternative<object_reserved_t> (
      store.reserve (reserved_entry).result ().value ()));

    const auto occupied_id = detail::new_user_spot_id ();
    auto occupied_target = descriptor ("occupied-target", 43, first_owner,
                                       detail::new_entry_spot_id ("occupied-target"));
    assert (store.update_mesh_node (occupied_target, location_write_intent_t::new_claim)
              .result ()
              .value ()
              .status
            == location_write_status_t::stored);
    object_reserve_request_t occupied_request{
      .key = {placement_object_kind_t::user_spot, occupied_id},
      .intent = {.stable_type = "room"},
      .target = {.mesh_name = occupied_target.mesh_name,
                 .node_rid = node_rid_t::from_string (occupied_target.rid.to_string ()),
                 .node_lifecycle_generation = occupied_target.lifecycle_generation,
                 .owner = first_owner},
      .capacity_bundle = {
        .spot_slots = 1,
        .spot_type = spot_type_capacity_delta_t{placement_object_kind_t::user_spot, "room", 1}}};
    assert (std::holds_alternative<object_reserved_t> (
      store.reserve (occupied_request).result ().value ()));
    auto descriptor_with_occupied_id = descriptor ("occupied-entry", 44, second_owner, occupied_id);
    assert (store.update_mesh_node (descriptor_with_occupied_id, location_write_intent_t::new_claim)
              .result ()
              .value ()
              .status
            == location_write_status_t::stored);
}
void verify_raw_spot_and_actor_routing ()
{
    mesh::raw_mesh_node_owner_t source (mesh::raw_mesh_node_options_t{
      .descriptor = descriptor ("raw-source"),
      .runtime_failures =
        std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()});
    mesh::raw_mesh_node_owner_t target (mesh::raw_mesh_node_options_t{
      .descriptor = descriptor ("raw-target"),
      .runtime_failures =
        std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()});
    source.start ();
    target.start ();
    const auto source_descriptor = source.topology ().local_descriptor ();
    const auto target_descriptor = target.topology ().local_descriptor ();
    const auto deadline = mesh::service_liveness_registry_t::clock_t::now () + 5s;
    assert (source.connect_peer (target.endpoint (), target_descriptor));
    while ((!ready_peer (source.topology (), target_descriptor.node_routing_id)
            || !ready_peer (target.topology (), source_descriptor.node_routing_id))
           && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
        const auto now = mesh::service_liveness_registry_t::clock_t::now ();
        (void) source.observe_routes ();
        (void) target.observe_routes ();
        (void) source.pump_one (now).result ().value ();
        (void) target.pump_one (now).result ().value ();
        std::this_thread::sleep_for (1ms);
    }
    assert (source.topology ().peer (target_descriptor.node_routing_id));
    assert (target.topology ().peer (source_descriptor.node_routing_id));

    const protocol::spot_route_fence_t follow_source{"follow-spot",
                                                     1,
                                                     source_descriptor.node_routing_id,
                                                     source_descriptor.lifecycle_generation,
                                                     2,
                                                     3};
    const protocol::spot_route_fence_t follow_target{"follow-spot",
                                                     1,
                                                     target_descriptor.node_routing_id,
                                                     target_descriptor.lifecycle_generation,
                                                     3,
                                                     3};
    // The diagnostic is allowed to exceed 16 MiB because that value limits
    // one encoded control envelope, not the relayed payload or queue storage.
    const protocol::message_follow_notice_t follow_notice{
      follow_source, follow_target, 1, 1, 16u * 1024u * 1024u + 1u, {1, 1}, 0};
    assert (source.send_message_follow (target_descriptor.node_routing_id, follow_notice)
              .result ()
              .value ());
    bool follow_received = false;
    while (!follow_received && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
        const auto pumped =
          target.pump_one (mesh::service_liveness_registry_t::clock_t::now ()).result ().value ();
        assert (pumped != mesh::raw_mesh_pump_result_t::protocol_error);
        auto claim = target.mailbox ().try_claim (mesh::service_mailbox_domain_t::infrastructure,
                                                  16, 64 * 1024);
        if (!claim)
            continue;
        for (const auto &slot : claim->records) {
            const auto &record = *slot;
            if (protocol::decode_header (protocol::message_bytes (record.parts ().front ())).kind
                == protocol::command::messageFollow) {
                assert (protocol::decode_message_follow (
                          protocol::message_bytes (record.parts ().front ()))
                        == follow_notice);
                follow_received = true;
            }
        }
        assert (target.mailbox ().release (*claim));
    }
    assert (follow_received);

    auto non_increasing_follow = follow_notice;
    std::get<protocol::spot_route_fence_t> (non_increasing_follow.target)
      .authority_owner_generation =
      std::get<protocol::spot_route_fence_t> (non_increasing_follow.source)
        .authority_owner_generation;
    assert (source.send_message_follow (target_descriptor.node_routing_id, non_increasing_follow)
              .result ()
              .value ());
    mesh::raw_mesh_pump_result_t non_increasing_follow_pump = mesh::raw_mesh_pump_result_t::no_data;
    while (non_increasing_follow_pump != mesh::raw_mesh_pump_result_t::protocol_error
           && mesh::service_liveness_registry_t::clock_t::now () < deadline)
        non_increasing_follow_pump =
          target.pump_one (mesh::service_liveness_registry_t::clock_t::now ()).result ().value ();
    assert (non_increasing_follow_pump == mesh::raw_mesh_pump_result_t::protocol_error);

    auto stale_follow = follow_notice;
    std::get<protocol::spot_route_fence_t> (stale_follow.target).target_node_generation++;
    assert (source.send_message_follow (target_descriptor.node_routing_id, stale_follow)
              .result ()
              .value ());
    mesh::raw_mesh_pump_result_t stale_follow_pump = mesh::raw_mesh_pump_result_t::no_data;
    while (stale_follow_pump != mesh::raw_mesh_pump_result_t::protocol_error
           && mesh::service_liveness_registry_t::clock_t::now () < deadline)
        stale_follow_pump =
          target.pump_one (mesh::service_liveness_registry_t::clock_t::now ()).result ().value ();
    assert (stale_follow_pump == mesh::raw_mesh_pump_result_t::protocol_error);

    stateful::stateful_object_runtime_t objects;
    objects.replace_placement_candidates (
      {{"m6b-mesh", "raw-target", {"room", "player"}, 100, 16, 0, 8, 0}});
    const auto spot = create_ready (
      objects,
      {stateful::object_kind_t::user_spot, "spot-1", "room", std::nullopt, {}, false, false});
    const auto actor = create_ready (
      objects,
      {stateful::object_kind_t::actor, "actor-1", "player", std::nullopt, {}, false, false});
    bool authority_live = true;
    stateful::raw_stateful_dispatch_t dispatch (
      objects, target,
      [&] (stateful::accepted_record_authority_query_t query)
        -> zlink::framework::task_t<std::optional<stateful::accepted_record_authority_t>> {
          assert (query.source_node_routing_id == source_descriptor.node_routing_id);
          assert (query.source_node_generation == source_descriptor.lifecycle_generation);
          if (!authority_live)
              co_return std::nullopt;
          co_return stateful::accepted_record_authority_t{
            {"source-owner", 17, source_descriptor.node_routing_id,
             source_descriptor.lifecycle_generation},
            query.target.kind == stateful::object_kind_t::actor ? 37u : 31u};
      });

    const protocol::spot_route_fence_t spot_fence{"spot-1",
                                                  spot.object_generation,
                                                  target_descriptor.node_routing_id,
                                                  target_descriptor.lifecycle_generation,
                                                  spot.authority_owner_generation,
                                                  31};
    assert (source
              .send_to_spot (target_descriptor.node_routing_id, "source-spot", spot_fence,
                             {"SpotPacket", "application/json", bytes ("spot")})
              .result ()
              .value ());
    mesh::raw_mesh_pump_result_t spot_pump = mesh::raw_mesh_pump_result_t::no_data;
    while (spot_pump != mesh::raw_mesh_pump_result_t::application
           && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
        spot_pump =
          target.pump_one (mesh::service_liveness_registry_t::clock_t::now ()).result ().value ();
        assert (spot_pump != mesh::raw_mesh_pump_result_t::protocol_error);
    }
    assert (spot_pump == mesh::raw_mesh_pump_result_t::application);
    assert (zlink::framework::test::ingest_stateful_mailbox_record (objects, target, dispatch, spot)
              .result ()
              .value ()
            == stateful::stateful_error_t::none);
    const auto [spot_delivery_error, spot_delivery] = dispatch.try_claim (spot);
    assert (spot_delivery_error == stateful::stateful_error_t::none);
    assert (spot_delivery);
    stateful::with_accepted_application (spot_delivery->turn, [] (auto, const auto &application) {
        assert (application.payload_bytes () == bytes ("spot"));
    });
    const auto &frozen_spot = spot_delivery->frozen;
    assert (spot_delivery->turn.payload.empty ());
    assert (spot_delivery->turn.application_record.has_value ());
    assert (frozen_spot.kind == protocol::frozen_record_kind_t::spot_send);
    assert (frozen_spot.source.owner_id == "source-owner");
    assert (frozen_spot.source.lease_generation == 17);
    assert (frozen_spot.operation.high == source_descriptor.lifecycle_generation);
    assert (frozen_spot.operation.low != 0);
    assert (dispatch.complete_async (*spot_delivery).result ().value ()
            == stateful::stateful_error_t::none);

    const protocol::wire_operation_id_t relocated_one_way_operation{0x91, 0x92};
    assert (target.mailbox ().try_enqueue (mesh::service_mailbox_record_t::from_parts (
      "spot:spot-1", mesh::service_mailbox_domain_t::application,
      zlink::framework::detail::backend::materialize_binding_parts (
        {protocol::encode_spot_message_header (protocol::command::spotSend, "source-spot",
                                               spot_fence, relocated_one_way_operation),
         protocol::encode_application_payload (
           {"SpotPacket", "application/json", bytes ("relocated")})}),
      source_descriptor.node_routing_id, std::nullopt, source_descriptor.lifecycle_generation,
      std::make_pair (relocated_one_way_operation.high, relocated_one_way_operation.low))));
    assert (zlink::framework::test::ingest_stateful_mailbox_record (objects, target, dispatch, spot)
              .result ()
              .value ()
            == stateful::stateful_error_t::none);
    const auto [relocated_error, relocated_delivery] = dispatch.try_claim (spot);
    assert (relocated_error == stateful::stateful_error_t::none);
    assert (relocated_delivery && !relocated_delivery->request);
    stateful::with_accepted_application (
      relocated_delivery->turn, [] (auto, const auto &application) {
          assert (application.payload_bytes () == bytes ("relocated"));
      });
    assert (dispatch.complete_async (*relocated_delivery).result ().value ()
            == stateful::stateful_error_t::none);

    const protocol::actor_route_fence_t actor_fence{"actor-1",
                                                    actor.object_generation,
                                                    target_descriptor.node_routing_id,
                                                    target_descriptor.lifecycle_generation,
                                                    actor.authority_owner_generation,
                                                    37};
    using request_result_t = std::pair<foundation::operation_terminal_t, std::vector<std::uint8_t>>;
    std::promise<request_result_t> promise;
    auto future = promise.get_future ();
    assert (source
              .request_to_actor (
                target_descriptor.node_routing_id, std::nullopt, actor_fence,
                {"ActorPacket", "application/json", bytes ("request")}, 2s,
                [&promise] (foundation::operation_terminal_t terminal,
                            ::zlink::framework::result_t<zlink::message_t> completion_payload) {
                    auto native_payload = std::move (completion_payload.value ());
                    auto payload = native_payload.to_bytes ();
                    promise.set_value ({terminal, std::move (payload)});
                },
                protocol::wire_operation_id_t{77, 88})
              .result ()
              .value ());
    mesh::raw_mesh_pump_result_t actor_pump = mesh::raw_mesh_pump_result_t::no_data;
    while (actor_pump != mesh::raw_mesh_pump_result_t::application
           && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
        const auto pump_now = mesh::service_liveness_registry_t::clock_t::now ();
        const auto source_pump = source.pump_one (pump_now).result ().value ();
        assert (source_pump != mesh::raw_mesh_pump_result_t::protocol_error);
        actor_pump = target.pump_one (pump_now).result ().value ();
        assert (actor_pump != mesh::raw_mesh_pump_result_t::protocol_error);
    }
    assert (actor_pump == mesh::raw_mesh_pump_result_t::application);
    assert (
      zlink::framework::test::ingest_stateful_mailbox_record (objects, target, dispatch, actor)
        .result ()
        .value ()
      == stateful::stateful_error_t::none);
    const auto [actor_delivery_error, actor_delivery] = dispatch.try_claim (actor);
    assert (actor_delivery_error == stateful::stateful_error_t::none);
    assert (actor_delivery && actor_delivery->request);
    stateful::with_accepted_application (actor_delivery->turn, [] (auto, const auto &application) {
        assert (application.payload_bytes () == bytes ("request"));
    });
    const auto &frozen_actor = actor_delivery->frozen;
    assert (actor_delivery->turn.payload.empty ());
    assert (actor_delivery->turn.application_record.has_value ());
    assert (frozen_actor.kind == protocol::frozen_record_kind_t::actor_request);
    assert ((frozen_actor.operation == protocol::wire_operation_id_t{77, 88}));
    assert (frozen_actor.reply_route_id && *frozen_actor.reply_route_id != 88);
    assert (frozen_actor.source.owner_id == "source-owner");
    assert (dispatch
              .complete_async (
                *actor_delivery,
                protocol::application_payload_t{"ActorReply", "application/json", bytes ("reply")})
              .result ()
              .value ()
            == stateful::stateful_error_t::none);

    while (future.wait_for (0ms) != std::future_status::ready
           && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
        const auto pump =
          source.pump_one (mesh::service_liveness_registry_t::clock_t::now ()).result ().value ();
        assert (pump != mesh::raw_mesh_pump_result_t::protocol_error);
        std::this_thread::sleep_for (1ms);
    }
    assert (future.wait_for (0ms) == std::future_status::ready);
    const auto result = future.get ();
    assert (result.first == foundation::operation_terminal_t::completed);
    assert (protocol::decode_application_payload (result.second).payload_bytes ()
            == bytes ("reply"));

    // A bound-session actorRequest journals the frozen source with the
    // session triple and the owning Actor identity of the current binding
    // (the fenced ingest target), instead of stripping the identity down to
    // a plain node source.
    const protocol::actor_message_header_t::bound_session_source_t bound_session_tail{
      {'s', 'e', 's', 's', '-', '1'}, 5, 9};
    const protocol::wire_operation_id_t bound_operation{0xa1, 0xa2};
    std::promise<request_result_t> bound_promise;
    auto bound_future = bound_promise.get_future ();
    assert (
      source
        .request_to_actor (
          target_descriptor.node_routing_id, std::nullopt, actor_fence,
          {"ActorPacket", "application/json", bytes ("bound")}, 2s,
          [&bound_promise] (foundation::operation_terminal_t terminal,
                            ::zlink::framework::result_t<zlink::message_t> completion_payload) {
              auto native_payload = std::move (completion_payload.value ());
              auto payload = native_payload.to_bytes ();
              bound_promise.set_value ({terminal, std::move (payload)});
          },
          bound_operation, bound_session_tail, 91)
        .result ()
        .value ());
    mesh::raw_mesh_pump_result_t bound_pump = mesh::raw_mesh_pump_result_t::no_data;
    while (bound_pump != mesh::raw_mesh_pump_result_t::application
           && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
        const auto pump_now = mesh::service_liveness_registry_t::clock_t::now ();
        const auto source_pump = source.pump_one (pump_now).result ().value ();
        assert (source_pump != mesh::raw_mesh_pump_result_t::protocol_error);
        bound_pump = target.pump_one (pump_now).result ().value ();
        assert (bound_pump != mesh::raw_mesh_pump_result_t::protocol_error);
    }
    assert (bound_pump == mesh::raw_mesh_pump_result_t::application);
    assert (
      zlink::framework::test::ingest_stateful_mailbox_record (objects, target, dispatch, actor)
        .result ()
        .value ()
      == stateful::stateful_error_t::none);
    const auto [bound_delivery_error, bound_delivery] = dispatch.try_claim (actor);
    assert (bound_delivery_error == stateful::stateful_error_t::none);
    assert (bound_delivery && bound_delivery->request);
    const auto &frozen_bound = bound_delivery->frozen;
    assert (frozen_bound.kind == protocol::frozen_record_kind_t::actor_request);
    assert (frozen_bound.source_kind == protocol::frozen_source_kind_t::bound_session);
    assert (frozen_bound.source_session_routing_id
            && *frozen_bound.source_session_routing_id == bound_session_tail.session_routing_id);
    assert (frozen_bound.source_binding_generation == bound_session_tail.binding_generation);
    assert (frozen_bound.source_session_sequence == bound_session_tail.session_sequence);
    assert (frozen_bound.source_actor && frozen_bound.source_actor->first == "actor-1"
            && frozen_bound.source_actor->second == actor.object_generation);
    assert (frozen_bound.reply_route_id && *frozen_bound.reply_route_id != 0);
    // The canonical journal bytes decode back to the same bound-session
    // identity, so replay after relocation keeps the fence intact.
    {
        assert (bound_delivery->turn.application_record.has_value ());
        const auto encoded_bound =
          protocol::encode_frozen_application_record (*bound_delivery->turn.application_record);
        assert (!encoded_bound.canonical_bytes.empty ());
        const auto canonical = protocol::decode_frozen_record (encoded_bound.canonical_bytes);
        assert (canonical.source_kind == protocol::frozen_source_kind_t::bound_session);
        assert (canonical.source_session_routing_id
                && *canonical.source_session_routing_id == bound_session_tail.session_routing_id);
        assert (canonical.source_binding_generation == bound_session_tail.binding_generation);
        assert (canonical.source_session_sequence == bound_session_tail.session_sequence);
        assert (canonical.source_actor && canonical.source_actor->first == "actor-1"
                && canonical.source_actor->second == actor.object_generation);
    }
    assert (dispatch
              .complete_async (*bound_delivery,
                               protocol::application_payload_t{"ActorReply", "application/json",
                                                               bytes ("bound-reply")})
              .result ()
              .value ()
            == stateful::stateful_error_t::none);
    while (bound_future.wait_for (0ms) != std::future_status::ready
           && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
        const auto pump =
          source.pump_one (mesh::service_liveness_registry_t::clock_t::now ()).result ().value ();
        assert (pump != mesh::raw_mesh_pump_result_t::protocol_error);
    }
    assert (bound_future.wait_for (0ms) == std::future_status::ready);
    const auto bound_result = bound_future.get ();
    assert (bound_result.first == foundation::operation_terminal_t::completed);
    assert (protocol::decode_application_payload (bound_result.second).payload_bytes ()
            == bytes ("bound-reply"));

    // A bound-session-routed actorRequest that is missing its exact fence
    // (here: zero binding generation and session sequence) is pre-Captured.
    // Ingest answers with the retryable moving rejection and never freezes
    // the frame under a fallback node/actor identity.
    const protocol::wire_operation_id_t unfenced_operation{0xb1, 0xb2};
    assert (target.mailbox ().try_enqueue (mesh::service_mailbox_record_t::from_parts (
      "actor:actor-1", mesh::service_mailbox_domain_t::application,
      zlink::framework::detail::backend::materialize_binding_parts (
        {protocol::encode_actor_message_header (protocol::command::actorRequest, std::nullopt,
                                                actor_fence, unfenced_operation, 92, 0),
         protocol::encode_application_payload (
           {"ActorPacket", "application/json", bytes ("unfenced")})}),
      source_descriptor.node_routing_id, 92, source_descriptor.lifecycle_generation,
      std::make_pair (unfenced_operation.high, unfenced_operation.low),
      mesh::service_bound_session_source_t{bound_session_tail.session_routing_id, 0, 0})));
    assert (
      zlink::framework::test::ingest_stateful_mailbox_record (objects, target, dispatch, actor)
        .result ()
        .value ()
      == stateful::stateful_error_t::moving);
    {
        const auto [unfenced_error, unfenced_delivery] = dispatch.try_claim (actor);
        (void) unfenced_error;
        assert (!unfenced_delivery);
    }

    // A binding that requires the fence while the frame carries no
    // bound-session tail at all is the same pre-Captured condition.
    const protocol::wire_operation_id_t tailless_operation{0xb3, 0xb4};
    assert (target.mailbox ().try_enqueue (mesh::service_mailbox_record_t::from_parts (
      "actor:actor-1", mesh::service_mailbox_domain_t::application,
      zlink::framework::detail::backend::materialize_binding_parts (
        {protocol::encode_actor_message_header (protocol::command::actorRequest, std::nullopt,
                                                actor_fence, tailless_operation, 93, 0),
         protocol::encode_application_payload (
           {"ActorPacket", "application/json", bytes ("tailless")})}),
      source_descriptor.node_routing_id, 93, source_descriptor.lifecycle_generation,
      std::make_pair (tailless_operation.high, tailless_operation.low),
      mesh::service_bound_session_source_t{bound_session_tail.session_routing_id,
                                           bound_session_tail.binding_generation,
                                           bound_session_tail.session_sequence})));
    assert (
      zlink::framework::test::ingest_stateful_mailbox_record (objects, target, dispatch, actor)
        .result ()
        .value ()
      == stateful::stateful_error_t::moving);
    {
        const auto [tailless_error, tailless_delivery] = dispatch.try_claim (actor);
        (void) tailless_error;
        assert (!tailless_delivery);
    }

    authority_live = false;
    assert (source
              .send_to_actor (target_descriptor.node_routing_id, std::nullopt, actor_fence,
                              {"ActorPacket", "application/json", bytes ("stale-owner")})
              .result ()
              .value ());
    mesh::raw_mesh_pump_result_t stale_owner_pump = mesh::raw_mesh_pump_result_t::no_data;
    while (stale_owner_pump != mesh::raw_mesh_pump_result_t::application
           && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
        stale_owner_pump =
          target.pump_one (mesh::service_liveness_registry_t::clock_t::now ()).result ().value ();
    }
    assert (stale_owner_pump == mesh::raw_mesh_pump_result_t::application);
    assert (
      zlink::framework::test::ingest_stateful_mailbox_record (objects, target, dispatch, actor)
        .result ()
        .value ()
      == stateful::stateful_error_t::unavailable);
    authority_live = true;

    auto stale_owner_lease_fence = actor_fence;
    ++stale_owner_lease_fence.owner_lease_generation;
    assert (source
              .send_to_actor (target_descriptor.node_routing_id, std::nullopt,
                              stale_owner_lease_fence,
                              {"ActorPacket", "application/json", bytes ("stale-lease")})
              .result ()
              .value ());
    stale_owner_pump = mesh::raw_mesh_pump_result_t::no_data;
    while (stale_owner_pump != mesh::raw_mesh_pump_result_t::application
           && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
        stale_owner_pump =
          target.pump_one (mesh::service_liveness_registry_t::clock_t::now ()).result ().value ();
    }
    assert (stale_owner_pump == mesh::raw_mesh_pump_result_t::application);
    assert (
      zlink::framework::test::ingest_stateful_mailbox_record (objects, target, dispatch, actor)
        .result ()
        .value ()
      == stateful::stateful_error_t::conflict);

    auto stale_fence = actor_fence;
    ++stale_fence.object_generation;
    std::promise<request_result_t> stale_promise;
    auto stale_future = stale_promise.get_future ();
    std::size_t stale_terminal_count = 0;
    assert (
      source
        .request_to_actor (target_descriptor.node_routing_id, std::nullopt, stale_fence,
                           {"ActorPacket", "application/json", bytes ("stale")}, 2s,
                           [&stale_promise, &stale_terminal_count] (
                             foundation::operation_terminal_t terminal,
                             ::zlink::framework::result_t<zlink::message_t> completion_payload) {
                               auto native_payload = std::move (completion_payload.value ());
                               auto payload = native_payload.to_bytes ();
                               ++stale_terminal_count;
                               stale_promise.set_value ({terminal, std::move (payload)});
                           })
        .result ()
        .value ());
    actor_pump = mesh::raw_mesh_pump_result_t::no_data;
    while (actor_pump != mesh::raw_mesh_pump_result_t::application
           && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
        const auto pump_now = mesh::service_liveness_registry_t::clock_t::now ();
        const auto source_pump = source.pump_one (pump_now).result ().value ();
        assert (source_pump != mesh::raw_mesh_pump_result_t::protocol_error);
        actor_pump = target.pump_one (pump_now).result ().value ();
        assert (actor_pump != mesh::raw_mesh_pump_result_t::protocol_error);
    }
    assert (actor_pump == mesh::raw_mesh_pump_result_t::application);
    assert (
      zlink::framework::test::ingest_stateful_mailbox_record (objects, target, dispatch, actor)
        .result ()
        .value ()
      == stateful::stateful_error_t::none);
    const auto [recreated_delivery_error, recreated_delivery] = dispatch.try_claim (actor);
    assert (recreated_delivery_error == stateful::stateful_error_t::none);
    assert (recreated_delivery && recreated_delivery->request);
    const auto &recreated_frozen = recreated_delivery->frozen;
    assert (recreated_delivery->turn.payload.empty ());
    assert (recreated_delivery->turn.application_record.has_value ());
    assert (recreated_frozen.target);
    assert (recreated_frozen.target->object_generation == actor.object_generation);
    assert (dispatch
              .complete_async (*recreated_delivery,
                               protocol::application_payload_t{"ActorReply", "application/json",
                                                               bytes ("recreated")})
              .result ()
              .value ()
            == stateful::stateful_error_t::none);
    while (stale_future.wait_for (0ms) != std::future_status::ready
           && mesh::service_liveness_registry_t::clock_t::now () < deadline) {
        const auto pump =
          source.pump_one (mesh::service_liveness_registry_t::clock_t::now ()).result ().value ();
        assert (pump != mesh::raw_mesh_pump_result_t::protocol_error);
        std::this_thread::sleep_for (1ms);
    }
    assert (stale_future.wait_for (0ms) == std::future_status::ready);
    const auto recreated_result = stale_future.get ();
    assert (recreated_result.first == foundation::operation_terminal_t::completed);
    assert (protocol::decode_application_payload (recreated_result.second).payload_bytes ()
            == bytes ("recreated"));
    assert (stale_terminal_count == 1);
    source.close ();
    target.close ();
}
void verify_dispatch_callbacks_survive_suspension (bool through_mesh, bool dispatch_marker)
{
    using namespace zlink::framework;
    auto state = test::runtime_failure_fixture<detail::mesh_node_builder_state_t> ("m6b-mesh");
    state->listen_endpoint = "tcp://127.0.0.1:0";
    state->routing_id = zlink::routing_id_t::from ("callback-lifetime");
    state->core_context = std::make_shared<zlink::context_t> ();
    detail::mesh_node_runtime_t mesh_node (state);
    auto release =
      std::make_shared<task_completion_source_t<std::optional<location_owner_token_t>>> ();
    auto entered = std::make_shared<std::atomic_bool> (false);
    mesh_node.configure_bound_session_operations (
      {.session_owner =
         [release, entered] (const auto &, std::uint64_t) {
             entered->store (true);
             return release->task ();
         },
       .bind =
         [] (const auto &, const auto &, std::uint64_t, const auto &, auto) {
             using bind_result_t = host::bound_session_bind_operation_result_t;
             return task_t<bind_result_t> (result_t<bind_result_t>::success (bind_result_t{}));
         },
       .send = [] (const auto &, auto) { return stateful::stateful_error_t::conflict; },
       .replaced = [] (const auto &) {},
       .commit_relocation_route = [] (const auto &, const auto &, const auto &) { return false; },
       .prepare_relocation_target_route = [] (const auto &, std::uint64_t) { return false; }});
    mesh_node.start ().result ().value ();
    auto &target = mesh_node.native_node ();
    const auto local = target.status ();
    const stateful::object_ref_t actor{
      stateful::object_kind_t::actor,  "lifetime-actor", 7, 11, "m6b-mesh",
      local.routing_id ().to_string ()};
    const auto creation = begin_reserved_object (target.objects (), actor, "lifetime.actor", {});
    assert (creation.error == stateful::stateful_error_t::none);
    assert (target.objects ().commit_create (creation.attempt) == stateful::stateful_error_t::none);
    const protocol::bound_session_bind_t bind{
      1,
      {actor.key, 7, local.routing_id ().to_bytes (), local.lifecycle_generation (), 11, 13},
      zlink::routing_id_t::from ("lifetime-session").to_bytes (),
      {protocol::bound_session_binding_state_t::active, 17}};
    assert (target.transport ().mailbox ().try_enqueue (mesh::service_mailbox_record_t::from_parts (
      "lifetime-bind", mesh::service_mailbox_domain_t::infrastructure,
      {zlink::message_t::from (protocol::encode_bound_session_bind (bind))},
      local.routing_id ().to_bytes (), std::nullopt, local.lifecycle_generation ())));
    host::public_host_runtime_test_access_t::enqueue_lifetime_probe (target);
    std::optional<task_t<std::size_t>> pending;
    auto calls = std::make_shared<std::array<int, 2>> ();
    {
        auto marker = std::make_shared<int> (42);
        auto dispatch = [marker = dispatch_marker ? marker : nullptr,
                         calls] (const auto &, const auto &, auto, auto) {
            assert (!marker || *marker == 42);
            ++(*calls)[0];
        };
        auto next = [marker = dispatch_marker ? nullptr : marker, calls] {
            assert (!marker || *marker == 42);
            ++(*calls)[1];
            return false;
        };
        pending.emplace (through_mesh ? mesh_node.dispatch_ready (dispatch, true, next)
                                      : target.dispatch_ready (dispatch, true, next));
        assert (entered->load ());
    }
    release->complete (result_t<std::optional<location_owner_token_t>>::success (std::nullopt));
    await_task (std::move (*pending));
    assert ((*calls)[0] == 1 && (*calls)[1] == 1);
    mesh_node.stop ();
}
}
