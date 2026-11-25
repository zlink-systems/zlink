/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/actor_maintenance_join_host_fixture.hpp"
#include "../support/actor_maintenance_join_packets.hpp"
#include "../support/actor_maintenance_join_location_store_fixtures.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/locations/authority_key_codec.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include <zlink/framework.hpp>
#include <gtest/gtest.h>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace zlink::framework::test::actor_maintenance_join
{
// Actor model §6.3 (#1772): a Create or GetOrCreate that makes a new Actor keeps only the nodes
// whose Entry Spot accepts that Actor type (descriptor capability `entryAccepted`).
constexpr auto entryless_weight = 1000;
constexpr auto entry_weight = 1;
constexpr std::size_t descriptor_scan_limit = 1000;

std::string entryless_target_id (std::string seed)
{
    for (;;) {
        const auto choice = std::hash<std::string>{}(seed) % (entryless_weight + entry_weight);
        if (choice > 0 && choice < static_cast<std::size_t> (entryless_weight))
            return seed;
        seed.push_back ('_');
    }
}

std::vector<std::string> factory_nodes (observations_t &evidence)
{
    std::lock_guard lock (evidence.mutex);
    return evidence.factory_nodes;
}

// The `entryAccepted` value of each actor capability this mesh's descriptors publish, keyed by
// routing ID and Actor stable type. A capability without the field reads as `nullopt`.
std::map<std::pair<std::string, std::string>, std::optional<bool>>
published_entry_accepted (fw::location_store_t &store)
{
    std::map<std::pair<std::string, std::string>, std::optional<bool>> result;
    const auto scanned =
      store.scan (fw::store_scan_request_t{{}, std::nullopt, descriptor_scan_limit}).result ();
    const auto &page = std::get<fw::store_scan_page_t> (scanned.value ());
    for (const auto &item : page.items) {
        const auto bytes = std::string (reinterpret_cast<const char *> (item.value.bytes.data ()),
                                        item.value.bytes.size ());
        const auto record = nlohmann::json::parse (bytes, nullptr, false);
        if (!record.is_object () || !record.contains ("descriptor")
            || !record.at ("descriptor").contains ("objectCapabilities"))
            continue;
        const auto &descriptor = record.at ("descriptor");
        if (descriptor.value ("meshName", std::string{}) != mesh_name)
            continue;
        for (const auto &capability : descriptor.at ("objectCapabilities")) {
            if (capability.at ("objectKind") != "actor")
                continue;
            result[{descriptor.at ("routingIdHex").get<std::string> (),
                    capability.at ("stableType").get<std::string> ()}] =
              capability.contains ("entryAccepted")
                ? std::make_optional (capability.at ("entryAccepted").get<bool> ())
                : std::nullopt;
        }
    }
    return result;
}

std::string rid_hex (std::string_view rid)
{
    return zlink::routing_id_t::from (std::string (rid)).to_hex ();
}

// Location Store view of a requester that read its descriptors before a node dropped its Entry
// Spot: every actor capability it lists claims `entryAccepted`.
class stale_entry_descriptor_store_t final : public fw::location_store_t
{
  public:
    explicit stale_entry_descriptor_store_t (
      std::shared_ptr<fw::location_store_t> inner,
      std::optional<std::string> missing_owner = std::nullopt) :
        _inner (std::move (inner)), _missing_owner (std::move (missing_owner))
    {
    }

    fw::task_t<fw::store_read_result_t> read (fw::store_key_t key) override
    {
        auto result = co_await _inner->read (std::move (key));
        if (_missing_owner) {
            if (const auto *found = std::get_if<fw::store_found_t> (&result)) {
                const auto record = nlohmann::json::parse (
                  std::string (reinterpret_cast<const char *> (found->value.bytes.data ()),
                               found->value.bytes.size ()),
                  nullptr, false);
                // The canonical owner lease row has these three fields; its expiry is Store metadata.
                if (record.is_object () && record.size () == owner_lease_field_count
                    && record.contains ("recordVersion") && record.contains ("leaseGeneration")
                    && record.value ("ownerId", std::string{}) == *_missing_owner)
                    co_return fw::store_missing_t{found->value.store_now};
            }
        }
        co_return result;
    }

    fw::task_t<fw::store_write_result_t> write (fw::store_write_request_t request) override
    {
        return _inner->write (std::move (request));
    }

    fw::task_t<fw::store_scan_result_t> scan (fw::store_scan_request_t request) override
    {
        auto scanned = co_await _inner->scan (std::move (request));
        if (auto *page = std::get_if<fw::store_scan_page_t> (&scanned)) {
            for (auto &item : page->items) {
                auto record = nlohmann::json::parse (
                  std::string (reinterpret_cast<const char *> (item.value.bytes.data ()),
                               item.value.bytes.size ()),
                  nullptr, false);
                if (!record.is_object () || !record.contains ("descriptor")
                    || !record.at ("descriptor").contains ("objectCapabilities"))
                    continue;
                for (auto &capability : record["descriptor"]["objectCapabilities"])
                    if (capability.at ("objectKind") == "actor")
                        capability["entryAccepted"] = true;
                const auto text = record.dump ();
                item.value.bytes.assign (reinterpret_cast<const std::byte *> (text.data ()),
                                         reinterpret_cast<const std::byte *> (text.data ())
                                           + text.size ());
            }
        }
        co_return scanned;
    }

  private:
    static constexpr std::size_t owner_lease_field_count = 3;
    std::shared_ptr<fw::location_store_t> _inner;
    const std::optional<std::string> _missing_owner;
};

class ActorEntryPlacement : public testing::TestWithParam<entry_kind_t>
{
};

// A node without an Entry Spot for the Actor type (none, or one for another type) is not a
// creation candidate even when its placement weight dominates; the Actor is created on the node
// whose Entry Spot accepts it, and the Entry-less node never runs the factory.
TEST_P (ActorEntryPlacement, CreatesOnlyWhereEntryAcceptsActorType)
{
    auto evidence_owner = std::make_shared<observations_t> ();
    auto &evidence = *evidence_owner;
    release_callbacks_t release{evidence};
    auto store = std::make_shared<fw::runtime::in_memory_location_store_t> ();
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    host_t accepting, entryless;
    configure (accepting, evidence_owner, first_rid, store, relocations, default_follow_duration, 0,
               false, entry_kind_t::member, entry_weight);
    accepting.start ();
    ASSERT_TRUE (ready (accepting));
    configure (entryless, evidence_owner, second_rid, store, relocations, default_follow_duration,
               0, false, GetParam (), entryless_weight);
    entryless.start ();
    ASSERT_TRUE (ready (entryless));
    auto services = entryless.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (peer_ready (services.get_required<fw::route_mesh_runtime_t> ()));
    auto &actors = services.get_required<fw::actor_manager_t> ();
    for (const auto *seed : {"placement-a", "placement-b", "placement-c"}) {
        // Either descriptor order selects the Entry-less node before the fix.
        const auto id = entryless_target_id (seed);
        auto call = std::string_view (seed) == "placement-b"
                      ? actors.create (fw::actor_id_t (id), actor_type)
                      : actors.get_or_create (fw::actor_id_t (id), actor_type);
        const auto created = call.async ().result ();
        ASSERT_TRUE (created) << id << ": "
                              << (created.error () ? created.error ()->what () : "no error");
        const auto *made = std::get_if<fw::actor_create_created_t> (&created.value ());
        ASSERT_NE (made, nullptr) << id;
        EXPECT_EQ (made->actor.node_rid ().value (), first_rid) << id;
    }
    EXPECT_EQ (factory_nodes (evidence), (std::vector<std::string> (3, first_rid)));
}

INSTANTIATE_TEST_SUITE_P (NoEntryAndOtherTypeEntry,
                          ActorEntryPlacement,
                          testing::Values (entry_kind_t::none, entry_kind_t::other));

TEST (ActorEntryPlacementUnavailable, NoAcceptingEntryReturnsUnavailableBeforeFactory)
{
    auto evidence_owner = std::make_shared<observations_t> ();
    auto &evidence = *evidence_owner;
    host_t entryless;
    configure (entryless, evidence_owner, first_rid,
               std::make_shared<fw::runtime::in_memory_location_store_t> (),
               std::make_shared<fw::runtime::in_memory_relocation_store_t> (),
               default_follow_duration, 0, false, entry_kind_t::none);
    entryless.start ();
    ASSERT_TRUE (ready (entryless));
    auto services = entryless.app.advanced ().services ().build_provider ();
    const auto created = services.get_required<fw::actor_manager_t> ()
                           .create (fw::actor_id_t (actor_id), actor_type)
                           .async ()
                           .result ();
    ASSERT_FALSE (created);
    EXPECT_EQ (created.error_kind (), fw::framework_error_kind_t::unavailable);
    EXPECT_TRUE (factory_nodes (evidence).empty ());
}

// Location runtime canonical descriptor: `entryAccepted` is true only for an actor capability
// whose Actor type the node's Entry Spot accepts.
TEST (ActorEntryPlacementDescriptor, PublishesEntryAccepted)
{
    auto evidence_owner = std::make_shared<observations_t> ();
    auto &evidence = *evidence_owner;
    release_callbacks_t release{evidence};
    auto store = std::make_shared<fw::runtime::in_memory_location_store_t> ();
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    host_t accepting, entryless, other;
    configure (accepting, evidence_owner, first_rid, store, relocations);
    accepting.start ();
    ASSERT_TRUE (ready (accepting));
    configure (entryless, evidence_owner, second_rid, store, relocations, default_follow_duration,
               0, false, entry_kind_t::none);
    entryless.start ();
    ASSERT_TRUE (ready (entryless));
    configure (other, evidence_owner, third_rid, store, relocations, default_follow_duration, 0,
               false, entry_kind_t::other);
    other.start ();
    ASSERT_TRUE (ready (other));
    const auto published = published_entry_accepted (*store);
    const std::map<std::pair<std::string, std::string>, std::optional<bool>> expected{
      {{rid_hex (first_rid), actor_type}, true},
      {{rid_hex (second_rid), actor_type}, false},
      {{rid_hex (third_rid), actor_type}, false},
      {{rid_hex (third_rid), other_actor_type}, true}};
    EXPECT_EQ (published, expected);
}

enum class placement_requester_t
{
    accepting,
    entryless,
    third_node
};

class ActorEntryPlacementStaleDescriptor : public testing::TestWithParam<placement_requester_t>
{
};

// A requester whose descriptor read still claims `entryAccepted` for an Entry-less node selects
// it. That target ends the creation before the factory, and the requester excludes that host
// execution generation and creates the Actor on the accepting node within the same call.
// The requester is the Entry-less target, the accepting target, or a third node. These exercise
// local-to-remote, remote-to-local, and remote-to-remote reselection with one operation identity.
TEST_P (ActorEntryPlacementStaleDescriptor, TargetRejectsBeforeFactoryAndRequesterReselects)
{
    auto evidence_owner = std::make_shared<observations_t> ();
    auto &evidence = *evidence_owner;
    release_callbacks_t release{evidence};
    auto store = std::make_shared<fw::runtime::in_memory_location_store_t> ();
    std::shared_ptr<fw::location_store_t> actual_store = store;
    std::shared_ptr<fw::location_store_t> stale =
      std::make_shared<stale_entry_descriptor_store_t> (store);
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    const bool local_target = GetParam () == placement_requester_t::entryless;
    host_t accepting, entryless, third;
    configure (accepting, evidence_owner, first_rid, local_target ? actual_store : stale,
               relocations, default_follow_duration, 0, false, entry_kind_t::member, entry_weight);
    accepting.start ();
    ASSERT_TRUE (ready (accepting));
    configure (entryless, evidence_owner, second_rid, local_target ? stale : actual_store,
               relocations, default_follow_duration, 0, false, entry_kind_t::none,
               entryless_weight);
    entryless.start ();
    ASSERT_TRUE (ready (entryless));
    if (GetParam () == placement_requester_t::third_node) {
        configure (third, evidence_owner, third_rid, stale, relocations, default_follow_duration, 0,
                   false, entry_kind_t::none, 0);
        third.start ();
        ASSERT_TRUE (ready (third));
    }
    auto &requester = GetParam () == placement_requester_t::third_node ? third
                      : local_target                                   ? entryless
                                                                       : accepting;
    auto services = requester.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (peer_ready (services.get_required<fw::route_mesh_runtime_t> (), {},
                             GetParam () == placement_requester_t::third_node ? 2 : 1));
    const auto id = entryless_target_id (actor_id);
    const auto created = services.get_required<fw::actor_manager_t> ()
                           .get_or_create (fw::actor_id_t (id), actor_type)
                           .async ()
                           .result ();
    ASSERT_TRUE (created) << (created.error () ? created.error ()->what () : "no error");
    const auto *made = std::get_if<fw::actor_create_created_t> (&created.value ());
    ASSERT_NE (made, nullptr);
    EXPECT_EQ (made->actor.node_rid ().value (), first_rid);
    EXPECT_EQ (factory_nodes (evidence), (std::vector<std::string>{first_rid}));
}

INSTANTIATE_TEST_SUITE_P (LocalAndRemoteTarget,
                          ActorEntryPlacementStaleDescriptor,
                          testing::Values (placement_requester_t::accepting,
                                           placement_requester_t::entryless,
                                           placement_requester_t::third_node));

TEST (ActorEntryPlacementExisting, RequiresLiveOwnerWithoutNewCreationCandidates)
{
    auto evidence_owner = std::make_shared<observations_t> ();
    auto &evidence = *evidence_owner;
    auto store = std::make_shared<fw::runtime::in_memory_location_store_t> ();
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    host_t accepting, live_view, missing_owner_view;
    configure (accepting, evidence_owner, first_rid, store, relocations);
    accepting.start ();
    ASSERT_TRUE (ready (accepting));
    auto accepting_services = accepting.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (accepting_services.get_required<fw::actor_manager_t> ()
                   .get_or_create (fw::actor_id_t (actor_id), actor_type)
                   .async ()
                   .result ());
    ASSERT_TRUE (set_published_placement_weight (accepting_services, 0));
    const auto current = accepting_services.get_required<fw::location_repository_t> ()
                           .read_authority (fw::runtime::actor_authority_key (actor_id))
                           .result ();
    ASSERT_TRUE (current);
    const auto *snapshot = std::get_if<fw::authority_snapshot_t> (&current.value ());
    ASSERT_NE (snapshot, nullptr);
    auto missing =
      std::make_shared<stale_entry_descriptor_store_t> (store, snapshot->owner.owner_id);
    configure (live_view, evidence_owner, second_rid, store, relocations, default_follow_duration,
               0, false, entry_kind_t::none, 0);
    live_view.start ();
    ASSERT_TRUE (ready (live_view));
    configure (missing_owner_view, evidence_owner, third_rid, missing, relocations,
               default_follow_duration, 0, false, entry_kind_t::none, 0);
    missing_owner_view.start ();
    ASSERT_TRUE (ready (missing_owner_view));
    auto live_services = live_view.app.advanced ().services ().build_provider ();
    const auto existing = live_services.get_required<fw::actor_manager_t> ()
                            .get_or_create (fw::actor_id_t (actor_id), actor_type)
                            .async ()
                            .result ();
    ASSERT_TRUE (existing);
    EXPECT_TRUE (std::holds_alternative<fw::actor_create_existing_t> (existing.value ()));
    auto missing_services = missing_owner_view.app.advanced ().services ().build_provider ();
    const auto unavailable = missing_services.get_required<fw::actor_manager_t> ()
                               .get_or_create (fw::actor_id_t (actor_id), actor_type)
                               .async ()
                               .result ();
    ASSERT_FALSE (unavailable);
    EXPECT_EQ (unavailable.error_kind (), fw::framework_error_kind_t::unavailable);
    EXPECT_EQ (factory_nodes (evidence), (std::vector<std::string>{first_rid}));
}

// Regression: an Entry-less node still takes an existing Actor as a join target and, with its
// User Spot, as a relocation target.
TEST (ActorEntryPlacementRegression, EntrylessNodeTakesJoinAndRelocation)
{
    auto evidence_owner = std::make_shared<observations_t> ();
    auto &evidence = *evidence_owner;
    release_callbacks_t release{evidence};
    auto store = std::make_shared<fw::runtime::in_memory_location_store_t> ();
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    host_t accepting, source, target;
    configure (accepting, evidence_owner, first_rid, store, relocations, source_follow_duration, 0,
               true);
    accepting.start ();
    ASSERT_TRUE (ready (accepting));
    auto accepting_services = accepting.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (accepting_services.get_required<fw::actor_manager_t> ()
                   .get_or_create (fw::actor_id_t (actor_id), actor_type)
                   .async ()
                   .result ());
    ASSERT_TRUE (set_published_placement_weight (accepting_services, 0));
    configure (source, evidence_owner, second_rid, store, relocations, source_follow_duration, 0,
               true, entry_kind_t::none);
    source.start ();
    ASSERT_TRUE (ready (source));
    auto services = source.app.advanced ().services ().build_provider ();
    // Existing does not need an eligible new placement candidate.
    const auto existing = services.get_required<fw::actor_manager_t> ()
                            .get_or_create (fw::actor_id_t (actor_id), actor_type)
                            .async ()
                            .result ();
    ASSERT_TRUE (existing);
    ASSERT_TRUE (std::holds_alternative<fw::actor_create_existing_t> (existing.value ()));
    ASSERT_TRUE (services.get_required<fw::spot_manager_t> ()
                   .get_or_create (fw::spot_id_t (local_spot), room_type)
                   .async ()
                   .result ());
    ASSERT_TRUE (peer_ready (accepting_services.get_required<fw::route_mesh_runtime_t> ()));
    ASSERT_TRUE (
      move_to (accepting_services.get_required<fw::actor_client_t> (), evidence, local_spot, 1));
    {
        std::lock_guard lock (evidence.mutex);
        ASSERT_TRUE (
          std::holds_alternative<fw::actor_join_accepted_t> (evidence.completions.back ()));
        ASSERT_EQ (std::get<fw::actor_join_accepted_t> (evidence.completions.back ())
                     .actor.node_rid ()
                     .value (),
                   second_rid);
    }
    configure (target, evidence_owner, third_rid, store, relocations, source_follow_duration, 0,
               true, entry_kind_t::none);
    target.start ();
    ASSERT_TRUE (ready (target));
    auto relocation = source.app.relocate (
      {.mode = fw::relocation_mode_t::planned_maintenance, .deadline = wait_budget});
    auto &runtime = services.get_required<fw::framework_runtime_t> ();
    const auto readiness_deadline = std::chrono::steady_clock::now () + wait_budget;
    while (runtime.status ().state != fw::framework_runtime_state_t::relocating
           && std::chrono::steady_clock::now () < readiness_deadline)
        std::this_thread::yield ();
    ASSERT_EQ (runtime.status ().state, fw::framework_runtime_state_t::relocating);
    auto client = source.app.advanced ().zlink ().route_client (
      services.get_required<fw::serializer_registry_t> ());
    ASSERT_TRUE (client.send_to_spot (fw::spot_id_t (local_spot), ready_t{}).async ().result ());
    const auto result = relocation.result_for (wait_budget);
    ASSERT_TRUE (result.has_value ());
    ASSERT_TRUE (*result);
    EXPECT_EQ (result->value ().outcome, fw::relocation_outcome_t::relocated)
      << "reason=" << static_cast<int> (result->value ().reason);
    // The actor now lives on the target; a request before the accepting node admits that
    // peer is RouteNotConnected (Unavailable), so wait for the route as the other cases do.
    ASSERT_TRUE (
      peer_ready (accepting_services.get_required<fw::route_mesh_runtime_t> (), third_rid));
    auto probe = accepting_services.get_required<fw::actor_client_t> ()
                   .request (fw::actor_id_t (actor_id), probe_t{7})
                   .timeout (wait_budget)
                   .async<probe_t> ()
                   .result ();
    ASSERT_TRUE (probe) << (probe.error () ? probe.error ()->what () : "no error");
    EXPECT_EQ (probe.value ().value, 7);
}

TEST (ActorMaintenanceJoin, HostRelocationAcquiresUnitsAfterEarlierDeferredJoin)
{
    constexpr auto remaining_actor = "remaining-member";
    auto evidence_owner = std::make_shared<observations_t> ();
    auto &evidence = *evidence_owner;
    auto store = std::make_shared<observed_location_store_t> ();
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    host_t source, joined, target;
    release_callbacks_t release{evidence};
    configure (source, evidence_owner, first_rid, store, relocations);
    source.start ();
    ASSERT_TRUE (ready (source));
    auto services = source.app.advanced ().services ().build_provider ();
    auto &actors = services.get_required<fw::actor_manager_t> ();
    auto &client = services.get_required<fw::actor_client_t> ();
    std::map<std::string, std::uint64_t> generations;
    for (const auto *id : {actor_id, remaining_actor}) {
        const auto created =
          actors.get_or_create (fw::actor_id_t (id), actor_type).async ().result ();
        ASSERT_TRUE (created);
        const auto *new_actor = std::get_if<fw::actor_create_created_t> (&created.value ());
        ASSERT_NE (new_actor, nullptr);
        generations.emplace (id, new_actor->actor.object_generation ());
    }
    ASSERT_TRUE (set_published_placement_weight (services, 0));
    configure (joined, evidence_owner, second_rid, store, relocations, default_follow_duration, 0,
               true);
    joined.start ();
    ASSERT_TRUE (ready (joined));
    auto joined_services = joined.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (joined_services.get_required<fw::spot_manager_t> ()
                   .get_or_create (fw::spot_id_t (local_spot), room_type)
                   .async ()
                   .result ());
    ASSERT_TRUE (set_published_placement_weight (joined_services, 0));
    ASSERT_TRUE (peer_ready (services.get_required<fw::route_mesh_runtime_t> (), second_rid));
    configure (target, evidence_owner, third_rid, store, relocations);
    target.start ();
    ASSERT_TRUE (ready (target));
    ASSERT_TRUE (peer_ready (services.get_required<fw::route_mesh_runtime_t> (), third_rid, 2));
    evidence.hold_join_admission = true;
    ASSERT_TRUE (client.send (fw::actor_id_t (actor_id), move_t{local_spot}).async ().result ());
    ASSERT_TRUE (evidence.join_admission_entered.task ().result_for (wait_budget));
    // Relocation continuations can run on an existing worker. Release the
    // earlier Join at the first Store scan after Host admission closes. The
    // one-shot hook lets the Join's own Store calls make progress while it waits.
    store->before_scan_when (
      [&] { return source.app.runtime_state () == fw::framework_runtime_state_t::relocating; },
      [&] () -> fw::task_t<void> {
          evidence.release_join_admission.complete (fw::result_t<void>::success ());
          co_await evidence.join_completed.task ();
      });
    const auto relocated =
      source.app
        .relocate ({.mode = fw::relocation_mode_t::planned_maintenance, .deadline = wait_budget})
        .result ();
    store->before_scan_when ({}, {});
    EXPECT_TRUE (evidence.release_join_admission.task ().await_ready ());
    EXPECT_TRUE (evidence.join_completed.task ().await_ready ());
    ASSERT_TRUE (relocated);
    ASSERT_EQ (relocated.value ().outcome, fw::relocation_outcome_t::relocated)
      << "reason=" << static_cast<int> (relocated.value ().reason);
    for (const auto &[id, owner] :
         {std::pair{actor_id, second_rid}, std::pair{remaining_actor, third_rid}}) {
        const auto resolved = joined_services.get_required<fw::actor_manager_t> ()
                                .get_or_create (fw::actor_id_t (id), actor_type)
                                .async ()
                                .result ();
        ASSERT_TRUE (resolved);
        const auto *existing = std::get_if<fw::actor_create_existing_t> (&resolved.value ());
        ASSERT_NE (existing, nullptr);
        EXPECT_EQ (existing->actor.node_rid ().value (), owner);
        EXPECT_EQ (existing->actor.object_generation (), generations.at (id));
        const auto reply = joined_services.get_required<fw::actor_client_t> ()
                             .request (fw::actor_id_t (id), probe_t{7})
                             .timeout (wait_budget)
                             .async<probe_t> ()
                             .result ();
        ASSERT_TRUE (reply);
        EXPECT_EQ (reply.value ().value, 7);
    }
}

TEST (ActorMaintenanceJoin, RemoteJoinDoesNotLeaveLocalRelocationWork)
{
    auto evidence_owner = std::make_shared<observations_t> ();
    auto &evidence = *evidence_owner;
    auto store = std::make_shared<fw::runtime::in_memory_location_store_t> ();
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    host_t source, target;
    release_callbacks_t release{evidence};
    configure (source, evidence_owner, first_rid, store, relocations, default_follow_duration, 0,
               true);
    source.start ();
    ASSERT_TRUE (ready (source));
    auto services = source.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (services.get_required<fw::actor_manager_t> ()
                   .get_or_create (fw::actor_id_t (actor_id), actor_type)
                   .async ()
                   .result ());
    ASSERT_TRUE (set_published_placement_weight (services, 0));
    configure (target, evidence_owner, second_rid, store, relocations, default_follow_duration, 0,
               true);
    target.start ();
    ASSERT_TRUE (ready (target));
    auto target_services = target.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (target_services.get_required<fw::spot_manager_t> ()
                   .get_or_create (fw::spot_id_t (local_spot), room_type)
                   .async ()
                   .result ());
    ASSERT_TRUE (peer_ready (services.get_required<fw::route_mesh_runtime_t> (), second_rid));
    ASSERT_TRUE (move_to (services.get_required<fw::actor_client_t> (), evidence, local_spot, 1));
    {
        std::lock_guard lock (evidence.mutex);
        ASSERT_TRUE (
          std::holds_alternative<fw::actor_join_accepted_t> (evidence.completions.back ()));
        ASSERT_EQ (std::get<fw::actor_join_accepted_t> (evidence.completions.back ())
                     .actor.node_rid ()
                     .value (),
                   second_rid);
    }
    auto registration =
      fw::detail::mesh_node_runtime_t::registrations (source.app.advanced ().zlink ()).front ();
    const auto source_released_authority = [&] {
        return registration->spot_state->lane
          .run ([&] {
              for (const auto &[key, route] : registration->spot_state->actor_routes) {
                  if (route.node_rid.value () == second_rid
                      && !registration->spot_state->actor_authority_fences.contains (key))
                      return true;
              }
              return false;
          })
          .get ();
    };
    const auto source_completion_deadline = std::chrono::steady_clock::now () + wait_budget;
    while (!source_released_authority ()
           && std::chrono::steady_clock::now () < source_completion_deadline)
        std::this_thread::yield ();
    ASSERT_TRUE (source_released_authority ());
    const auto refs =
      fw::detail::spot_node_runtime_t (registration->spot_state).local_actor_refs ();
    const auto authority_read = services.get_required<fw::location_repository_t> ()
                                  .read_authority (fw::runtime::actor_authority_key (actor_id))
                                  .result ()
                                  .value ();
    const auto *authority = std::get_if<fw::authority_snapshot_t> (&authority_read);
    ASSERT_NE (authority, nullptr);
    EXPECT_EQ (authority->allocation.target.node_rid.value (), second_rid);
    EXPECT_TRUE (refs.empty ());
    EXPECT_TRUE (source_released_authority ());
    const auto result =
      source.app
        .relocate ({.mode = fw::relocation_mode_t::planned_maintenance, .deadline = wait_budget})
        .result_for (wait_budget);
    ASSERT_TRUE (result && *result);
    EXPECT_EQ (result->value ().outcome, fw::relocation_outcome_t::relocated);
    EXPECT_EQ (result->value ().reason, fw::relocation_reason_t::none);
}
} // namespace zlink::framework::test::actor_maintenance_join
