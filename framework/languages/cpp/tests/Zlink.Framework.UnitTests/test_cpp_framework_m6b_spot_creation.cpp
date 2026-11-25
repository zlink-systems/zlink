/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/m6b_cases.hpp"
#include "../support/m6b_task.hpp"
#include "../support/m6b_mesh_fixture.hpp"
#include "../support/m6b_stateful_fixture.hpp"
#include "../support/stateful_object_sync.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/stateful/public_host_runtime.hpp"
#include "runtime/stateful/stateful_object_runtime.hpp"
#include <zlink/Contracts/Core/routing_id.hpp>
#include <zlink/Contracts/Messaging/message.hpp>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace foundation = zlink::framework::runtime::foundation;
namespace mesh = zlink::framework::runtime::mesh;
namespace protocol = zlink::framework::runtime::protocol;
namespace stateful = zlink::framework::runtime::stateful;
namespace host = zlink::framework::runtime::host;
using namespace std::chrono_literals;
using zlink::framework::test::begin_reserved_object;

namespace zlink::framework::test::m6b
{
void verify_creation_terminal_operation_isolation ()
{
    using namespace zlink::framework;
    auto store = std::make_shared<zlink::framework::runtime::in_memory_location_repository_t> ();
    store->configure_reclaim_margin (std::chrono::milliseconds::zero ());
    const auto claimed = std::get<owner_lease_claimed_t> (
      store->claim_owner_lease ("terminal-owner", std::chrono::seconds (15)).result ().value ());
    mesh_node_descriptor_t target{
      .mesh_name = "m6b-mesh",
      .rid = zlink::routing_id_t::from ("terminal-target"),
      .lifecycle_generation = 1,
      .descriptor_revision = 1,
      .endpoint = "tcp://127.0.0.1:1",
      .application_version = 1,
      .object_capabilities = {{.object_kind = placement_object_kind_t::actor,
                               .stable_type = "player"}},
      .object_role = object_role_t::server,
      .capacity = {.actors = {.limit = 8}},
      .state = framework_runtime_state_t::serving,
      .security_identity = "terminal",
      .owner_id = claimed.token.owner_id,
      .lease_generation = claimed.token.lease_generation};
    assert (
      store->update_mesh_node (target, location_write_intent_t::new_claim).result ().value ().status
      == location_write_status_t::stored);
    object_reserve_request_t request{
      .key = {placement_object_kind_t::actor, "terminal-actor"},
      .intent = {.stable_type = "player"},
      .target = {.mesh_name = "m6b-mesh",
                 .node_rid = node_rid_t::from_string ("terminal-target"),
                 .node_lifecycle_generation = 1,
                 .owner = claimed.token},
      .capacity_bundle = {.actor_slots = 1}};
    const auto reserved = std::get<object_reserved_t> (store->reserve (request).result ().value ());
    const creation_operation_identity_t first{
      node_rid_t::from_string ("terminal-source"), 3, {5, 7}};
    const std::vector<std::byte> envelope{std::byte{'v'}, std::byte{'1'}};
    const creation_terminal_publication_t publication{
      first, envelope, std::chrono::system_clock::now () + std::chrono::seconds (30)};
    assert (std::holds_alternative<object_creation_completed_result_t> (
      store
        ->complete_creation ({request.key, reserved.fence, object_creation_rejected_t{publication}})
        .result ()
        .value ()));
    assert (store->read_creation_terminal (first).result ().value ().has_value ());
    const creation_operation_identity_t second{
      node_rid_t::from_string ("terminal-source"), 3, {5, 8}};
    assert (!store->read_creation_terminal (second).result ().value ().has_value ());
    assert (
      std::holds_alternative<object_reserved_t> (store->reserve (request).result ().value ()));
}

void verify_typed_capacity_retry_uses_second_candidate ()
{
    using namespace zlink::framework;
    auto store = std::make_shared<zlink::framework::runtime::in_memory_location_repository_t> ();
    store->configure_reclaim_margin (std::chrono::milliseconds::zero ());
    const auto first_owner = std::get<owner_lease_claimed_t> (
                               store->claim_owner_lease ("capacity-first", 15s).result ().value ())
                               .token;
    const auto second_owner =
      std::get<owner_lease_claimed_t> (
        store->claim_owner_lease ("capacity-second", 15s).result ().value ())
        .token;
    const auto publish = [&] (std::string rid, const location_owner_token_t &owner) {
        mesh_node_descriptor_t descriptor{
          .mesh_name = "capacity-mesh",
          .rid = zlink::routing_id_t::from (rid),
          .lifecycle_generation = 1,
          .descriptor_revision = 1,
          .endpoint = "tcp://127.0.0.1:1",
          .application_version = 1,
          .object_capabilities = {{.object_kind = placement_object_kind_t::user_spot,
                                   .stable_type = "room"}},
          .object_role = object_role_t::server,
          .capacity = {.spots = {.limit = 32},
                       .spot_types = {{.object_kind = placement_object_kind_t::user_spot,
                                       .stable_type = "room",
                                       .usage = {.limit = 1}}}},
          .state = framework_runtime_state_t::serving,
          .security_identity = "capacity",
          .owner_id = owner.owner_id,
          .lease_generation = owner.lease_generation};
        assert (store->update_mesh_node (std::move (descriptor), location_write_intent_t::new_claim)
                  .result ()
                  .value ()
                  .status
                == location_write_status_t::stored);
    };
    publish ("capacity-node-a", first_owner);
    publish ("capacity-node-b", second_owner);
    const auto target = [] (std::string rid, const location_owner_token_t &owner) {
        return object_creation_target_t{"capacity-mesh", node_rid_t::from_string (rid), 1, owner};
    };
    object_reserve_request_t occupied{
      .key = {placement_object_kind_t::user_spot, "occupied"},
      .intent = {.stable_type = "room"},
      .target = target ("capacity-node-a", first_owner),
      .capacity_bundle = {
        .spot_slots = 1,
        .spot_type = spot_type_capacity_delta_t{
          .object_kind = placement_object_kind_t::user_spot, .stable_type = "room", .slots = 1}}};
    assert (
      std::holds_alternative<object_reserved_t> (store->reserve (occupied).result ().value ()));

    object_reserve_request_t request{
      .key = {placement_object_kind_t::user_spot, "retry-room"},
      .intent = {.stable_type = "room"},
      .target = target ("capacity-node-a", first_owner),
      .capacity_bundle = {
        .spot_slots = 1,
        .spot_type = spot_type_capacity_delta_t{
          .object_kind = placement_object_kind_t::user_spot, .stable_type = "room", .slots = 1}}};
    assert (std::holds_alternative<object_placement_capacity_exhausted_t> (
      store->reserve (request).result ().value ()));
    request.target = target ("capacity-node-b", second_owner);
    assert (
      std::holds_alternative<object_reserved_t> (store->reserve (request).result ().value ()));
}
void verify_actor_create_does_not_replay_after_reciprocal_handover ()
{
    using completion_t = std::pair<foundation::operation_terminal_t, std::vector<std::uint8_t>>;
    mesh::raw_mesh_node_owner_t source (zlink::framework::test::runtime_failure_options (
      zlink::framework::runtime::mesh::raw_mesh_node_options_t{descriptor ("Z-handover-source")}));
    mesh::raw_mesh_node_owner_t target (zlink::framework::test::runtime_failure_options (
      zlink::framework::runtime::mesh::raw_mesh_node_options_t{descriptor ("A-handover-target")}));
    source.start ();
    target.start ();
    const auto source_descriptor = source.topology ().local_descriptor ();
    const auto target_descriptor = target.topology ().local_descriptor ();
    assert (source.connect_peer (target.endpoint (), target_descriptor));
    const auto pump = [&] {
        const auto now = mesh::service_liveness_registry_t::clock_t::now ();
        (void) source.observe_routes ();
        (void) target.observe_routes ();
        assert (await_task (source.pump_one (now)) != mesh::raw_mesh_pump_result_t::protocol_error);
        assert (await_task (target.pump_one (now)) != mesh::raw_mesh_pump_result_t::protocol_error);
    };
    const auto admission_deadline = std::chrono::steady_clock::now () + 2s;
    while ((!ready_peer (source.topology (), target_descriptor.node_routing_id)
            || !ready_peer (target.topology (), source_descriptor.node_routing_id))
           && std::chrono::steady_clock::now () < admission_deadline)
        pump ();
    assert (source.topology ().peer (target_descriptor.node_routing_id));
    assert (target.topology ().peer (source_descriptor.node_routing_id));

    const auto deadline = std::chrono::steady_clock::now () + 2s;
    const protocol::actor_create_header_t request{
      0,
      {source_descriptor.lifecycle_generation, 37},
      source_descriptor.node_routing_id,
      source_descriptor.lifecycle_generation,
      "handover-actor",
      "player",
      {"handover-reservation", "handover-store", 1, 1, target_descriptor.node_routing_id,
       target_descriptor.lifecycle_generation, "handover-owner", 1, 1},
      static_cast<std::uint64_t> (std::chrono::duration_cast<std::chrono::milliseconds> (
                                    std::chrono::system_clock::now ().time_since_epoch () + 2s)
                                    .count ())};
    std::promise<completion_t> promise;
    auto completion = promise.get_future ();
    assert (await_task (
      source.request_actor_create (target_descriptor.node_routing_id, request,
                                   std::chrono::duration_cast<std::chrono::milliseconds> (
                                     deadline - std::chrono::steady_clock::now ()),
                                   [&promise] (auto terminal, auto payload) {
                                       promise.set_value ({terminal, payload.value ().to_bytes ()});
                                   })));
    const auto receive = [&] {
        std::shared_ptr<mesh::service_mailbox_claim_t> claim;
        while (!claim && std::chrono::steady_clock::now () < deadline) {
            pump ();
            claim =
              target.mailbox ().try_claim (mesh::service_mailbox_domain_t::infrastructure, 1, 4096);
        }
        assert (claim && claim->records.size () == 1);
        return claim;
    };
    auto losing = receive ();
    const auto first_record = losing->records.front ()->reply_source ();
    const auto first = protocol::decode_actor_create_header (
      protocol::message_bytes (losing->records.front ()->parts ().front ()));
    assert (first.operation == request.operation);
    assert (first.correlation != 0);
    assert (losing->records.front ()->reply_token ());
    assert (completion.wait_for (0ms) == std::future_status::timeout);
    assert (target.mailbox ().release (*losing));

    // Submit §5: handover ends the binding request with its typed result.
    // A selected-route change cannot submit this operation a second time.
    assert (target.connect_peer (source.endpoint (), source_descriptor));
    const auto no_replay = [&] {
        assert (
          !target.mailbox ().try_claim (mesh::service_mailbox_domain_t::infrastructure, 1, 4096));
    };
    while (completion.wait_for (0ms) != std::future_status::ready
           && std::chrono::steady_clock::now () < deadline) {
        pump ();
        (void) source.expire_requests (std::chrono::steady_clock::now ()).result ().value ();
        no_replay ();
    }
    assert (completion.wait_for (0ms) == std::future_status::ready);
    const auto ended = completion.get ();
    assert (ended.first == foundation::operation_terminal_t::transport_failed);
    assert (ended.second.empty ());
    assert (source.pending_operation_count () == 0);
    (void) target.reply_actor_create (*first_record, {{first.correlation, 0, 0},
                                                      protocol::actor_create_result_t::created,
                                                      target_descriptor.node_routing_id,
                                                      first.actor_id,
                                                      1});
    for (int turn = 0; turn < 32; ++turn) {
        pump ();
        no_replay ();
    }
    assert (source.pending_operation_count () == 0);
    source.close ();
    target.close ();
}

void verify_global_identity_remote_create_and_generation_fence ()
{
    stateful::stateful_object_runtime_t runtime;
    runtime.replace_placement_candidates (
      {{"mesh-a", "node-a", {"player", "room", "transient"}, 100, 16, 0, 4, 0},
       {"mesh-b", "node-b", {"player", "room", "transient"}, 100, 16, 0, 4, 0}});

    auto first = runtime.begin_create ({stateful::object_kind_t::actor,
                                        "actor-1",
                                        "player",
                                        std::string ("mesh-a"),
                                        {},
                                        false,
                                        false});
    assert (first.status == stateful::create_status_t::reserved);
    assert (first.object.mesh_name == "mesh-a");

    const auto joined = runtime.begin_create ({stateful::object_kind_t::actor,
                                               "actor-1",
                                               "player",
                                               std::string ("mesh-b"),
                                               {},
                                               false,
                                               false});
    assert (joined.status == stateful::create_status_t::joined);
    assert (joined.attempt == first.attempt);
    assert (!joined.factory_owner);
    const auto actor_type_mismatch = runtime.begin_create ({stateful::object_kind_t::actor,
                                                            "actor-1",
                                                            "administrator",
                                                            std::string ("mesh-b"),
                                                            {},
                                                            false,
                                                            false});
    assert (actor_type_mismatch.status == stateful::create_status_t::failed);
    assert (actor_type_mismatch.error == stateful::stateful_error_t::type_mismatch);
    assert (runtime.commit_create (first.attempt) == stateful::stateful_error_t::none);

    const auto global_existing = runtime.begin_create ({stateful::object_kind_t::actor,
                                                        "actor-1",
                                                        "player",
                                                        std::string ("mesh-b"),
                                                        {},
                                                        false,
                                                        false});
    assert (global_existing.status == stateful::create_status_t::existing);
    assert (global_existing.object.mesh_name == "mesh-a");
    assert (global_existing.object.object_generation == first.object.object_generation);

    const stateful::object_ref_t reserved_spot{
      stateful::object_kind_t::user_spot, "spot-1", 1, 1, "mesh-a", "node-a"};
    auto first_spot = begin_reserved_object (runtime, reserved_spot, "lobby", {});
    assert (first_spot.status == stateful::create_status_t::reserved);
    const auto joined_spot = begin_reserved_object (runtime, reserved_spot, "lobby", {});
    assert (joined_spot.status == stateful::create_status_t::joined);
    assert (joined_spot.attempt == first_spot.attempt);
    const auto spot_type_mismatch = begin_reserved_object (runtime, reserved_spot, "match", {});
    assert (spot_type_mismatch.status == stateful::create_status_t::failed);
    assert (spot_type_mismatch.error == stateful::stateful_error_t::type_mismatch);
    assert (runtime.commit_create (first_spot.attempt) == stateful::stateful_error_t::none);
    const auto existing_spot = runtime.begin_create ({stateful::object_kind_t::user_spot,
                                                      "spot-1",
                                                      "lobby",
                                                      std::string ("mesh-b"),
                                                      {},
                                                      false,
                                                      false});
    assert (existing_spot.status == stateful::create_status_t::existing);
    assert (existing_spot.object.mesh_name == "mesh-a");
    assert (existing_spot.object.object_generation == first_spot.object.object_generation);

    const auto original = *runtime.find (stateful::object_kind_t::actor, "actor-1");
    assert (runtime.destroy_actor (original) == stateful::stateful_error_t::none);
    const auto replacement = create_ready (runtime, {stateful::object_kind_t::actor,
                                                     "actor-1",
                                                     "player",
                                                     std::string ("mesh-b"),
                                                     {},
                                                     false,
                                                     false});
    assert (replacement.object_generation == original.object_generation + 1);
    assert (runtime.destroy_actor (original) == stateful::stateful_error_t::generation_stale);

    // A node can retain an Actor reference while the Actor changes membership
    // on another node. When the Actor returns, the committed owner generation
    // may therefore advance by more than one from the retained local value.
    const auto returning_actor = create_ready (runtime, {stateful::object_kind_t::actor,
                                                         "returning-actor",
                                                         "player",
                                                         std::string ("mesh-a"),
                                                         {},
                                                         false,
                                                         false});
    auto remote_owner = returning_actor;
    remote_owner.authority_owner_generation++;
    remote_owner.node_id = "node-b";
    assert (runtime.adopt_reserved_actor_owner (remote_owner, "player").result ().value ()
            == stateful::stateful_error_t::none);
    auto returned_owner = remote_owner;
    returned_owner.authority_owner_generation += 3;
    returned_owner.node_id = "node-a";
    assert (runtime.adopt_reserved_actor_owner (returned_owner, "player").result ().value ()
            == stateful::stateful_error_t::none);
    auto stale_owner = returned_owner;
    stale_owner.authority_owner_generation--;
    stale_owner.node_id = "node-b";
    assert (runtime.adopt_reserved_actor_owner (stale_owner, "player").result ().value ()
            == stateful::stateful_error_t::generation_stale);

    // A User-Spot membership authority commit can advance the generation
    // without changing the local owner node. It is distinct from the
    // return-to-node adoption above and accepts exactly the next generation.
    auto local_membership_owner = returned_owner;
    ++local_membership_owner.authority_owner_generation;
    assert (
      runtime.advance_local_actor_authority (local_membership_owner, "player").result ().value ()
      == stateful::stateful_error_t::none);
    assert (runtime.find (stateful::object_kind_t::actor, local_membership_owner.key)
            == local_membership_owner);
    auto skipped_local_membership_owner = local_membership_owner;
    skipped_local_membership_owner.authority_owner_generation += 2;
    assert (runtime.advance_local_actor_authority (skipped_local_membership_owner, "player")
              .result ()
              .value ()
            == stateful::stateful_error_t::none);
    auto stale_local_membership_owner = skipped_local_membership_owner;
    --stale_local_membership_owner.authority_owner_generation;
    assert (runtime.advance_local_actor_authority (stale_local_membership_owner, "player")
              .result ()
              .value ()
            == stateful::stateful_error_t::generation_stale);

    // Application actors created from a globally reserved reference use the
    // same local placement capacity as actors created through begin_create.
    // Destroying that actor must release the capacity before the next
    // incarnation is reserved.
    const stateful::object_ref_t reserved_actor{
      stateful::object_kind_t::actor, "reserved-actor", 1, 1, "mesh-a", "node-a"};
    const auto reserved_create = begin_reserved_object (runtime, reserved_actor, "player", {});
    assert (reserved_create.status == stateful::create_status_t::reserved);
    assert (runtime.commit_create (reserved_create.attempt) == stateful::stateful_error_t::none);
    assert (runtime.destroy_actor (reserved_actor) == stateful::stateful_error_t::none);
    const auto reserved_recreated = create_ready (runtime, {stateful::object_kind_t::actor,
                                                            "reserved-actor",
                                                            "player",
                                                            std::string ("mesh-a"),
                                                            {},
                                                            false,
                                                            false});
    assert (reserved_recreated.object_generation == reserved_actor.object_generation + 1);
}
void verify_instance_cold_activation_only_from_intent ()
{
    stateful::stateful_object_runtime_t runtime;
    runtime.replace_placement_candidates ({{"mesh-a", "node-a", {"transient"}, 100, 16, 0, 8, 0}});

    const auto forbidden = runtime.begin_create ({stateful::object_kind_t::instance_spot,
                                                  "instance-1",
                                                  "transient",
                                                  std::nullopt,
                                                  {},
                                                  false,
                                                  false});
    assert (forbidden.error == stateful::stateful_error_t::instance_manager_create_forbidden);

    std::size_t factory_count = 0;
    stateful::create_request_t request{stateful::object_kind_t::instance_spot,
                                       "instance-1",
                                       "transient",
                                       std::nullopt,
                                       {},
                                       false,
                                       false};
    const auto activated =
      runtime.activate_instance (request, [&] (const stateful::object_ref_t &) {
          ++factory_count;
          return true;
      });
    assert (activated.status == stateful::create_status_t::reserved);
    assert (activated.error == stateful::stateful_error_t::none);
    assert (factory_count == 1);

    const auto existing = runtime.activate_instance (request, [&] (const stateful::object_ref_t &) {
        ++factory_count;
        return true;
    });
    assert (existing.status == stateful::create_status_t::existing);
    assert (factory_count == 1);
}
void verify_creating_spot_subscription_uses_native_object_lifetime ()
{
    auto local = descriptor ("creating-subscription");
    local.channels = {{"local", 0}};
    auto host = std::make_shared<host::public_host_runtime_t> (host::host_options_t{
      mesh::raw_mesh_node_options_t{
        .descriptor = std::move (local),
        .runtime_failures =
          std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()},
      "entry",
      {"framework.spot"}});
    host->start ();
    const stateful::object_ref_t reference{stateful::object_kind_t::user_spot,
                                           "creating-room",
                                           7,
                                           1,
                                           "m6b-mesh",
                                           "creating-subscription"};
    const auto creating = begin_reserved_object (host->objects (), reference, "room", {});
    assert (creating.factory_owner);
    assert (await_task (host->objects ().contains_async (reference)));
    assert (!host->objects ().find (reference.kind, reference.key));
    host::spot_handle_t spot (host, reference);
    // Configure registers subscriptions before the same native record becomes Ready.
    spot.set_subscription ("local", "creating-topic");
    assert (host->get_or_create_spot (reference.key).status ().lifecycle_generation ()
            == reference.object_generation);
    assert (host->objects ().commit_create (creating.attempt) == stateful::stateful_error_t::none);
    assert (host->objects ().find (reference.kind, reference.key) == reference);
    auto publisher = host->entry_spot ();
    std::vector<std::string> delivered;
    const auto dispatch = [&] (const host::ready_record_t &owner,
                               const host::receive_record_t &record,
                               std::vector<zlink::message_t> parts, auto permit) {
        assert (record.kind == host::record_kind_t::spot_multicast);
        assert (parts.size () == 1 && parts.front ().to_string () == "payload");
        zlink::framework::runtime::handler_entry_t before_application_handler (
          record.before_application_handler, std::move (permit));
        before_application_handler ();
        delivered.push_back (owner.spot_id);
    };
    const auto publish = [&] (const std::string &topic) {
        await_task (
          publisher.publish ("local", topic, {zlink::message_t::from (std::string ("payload"))}));
        while (await_task (host->dispatch_ready (dispatch)) != 0) {
        }
    };
    publish ("creating-topic");
    assert ((delivered == std::vector<std::string>{reference.key}));
    delivered.clear ();
    assert (spot.close ());
    publish ("creating-topic");
    assert (delivered.empty ());

    auto failed_reference = reference;
    ++failed_reference.object_generation;
    const auto failed = begin_reserved_object (host->objects (), failed_reference, "room", {});
    assert (failed.factory_owner);
    host::spot_handle_t failed_spot (host, failed_reference);
    failed_spot.set_subscription ("local", "aborted-topic");
    assert (host->objects ().abort_create (failed.attempt) == stateful::stateful_error_t::none);
    assert (!await_task (host->objects ().contains_async (failed_reference)));

    auto replacement_reference = failed_reference;
    ++replacement_reference.object_generation;
    const auto replacement =
      begin_reserved_object (host->objects (), replacement_reference, "room", {});
    assert (replacement.factory_owner);
    assert (host->objects ().commit_create (replacement.attempt)
            == stateful::stateful_error_t::none);
    publish ("aborted-topic");
    assert (delivered.empty ());
    host::spot_handle_t replacement_spot (host, replacement_reference);
    replacement_spot.set_subscription ("local", "aborted-topic");
    failed_spot.unset_subscription ("local", "aborted-topic");
    bool stale_subscription_rejected = false;
    try {
        failed_spot.set_subscription ("local", "aborted-topic");
    }
    catch (const zlink::framework::framework_exception_t &error) {
        assert (error.kind () == zlink::framework::framework_error_kind_t::invalid_operation);
        stale_subscription_rejected = true;
    }
    assert (stale_subscription_rejected);
    publish ("aborted-topic");
    assert ((delivered == std::vector<std::string>{reference.key}));
    assert (replacement_spot.close ());
    auto actor_reference = replacement_reference;
    actor_reference.kind = stateful::object_kind_t::actor;
    actor_reference.key = "not-a-subscriber";
    const auto actor = begin_reserved_object (host->objects (), actor_reference, "actor", {});
    assert (actor.factory_owner);
    host::spot_handle_t actor_as_spot (host, actor_reference);
    bool actor_subscription_rejected = false;
    try {
        actor_as_spot.set_subscription ("local", "aborted-topic");
    }
    catch (const zlink::framework::framework_exception_t &error) {
        assert (error.kind () == zlink::framework::framework_error_kind_t::invalid_operation);
        actor_subscription_rejected = true;
    }
    assert (actor_subscription_rejected);
    actor_as_spot.unset_subscription ("local", "aborted-topic");
    assert (host->objects ().abort_create (actor.attempt) == stateful::stateful_error_t::none);
    host->close ();
}
}
