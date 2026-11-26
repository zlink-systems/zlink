/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/actor_maintenance_join_host_fixture.hpp"
#include "../support/actor_maintenance_join_packets.hpp"
#include "runtime/locations/authority_key_codec.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>

namespace zlink::framework::test::actor_maintenance_join
{
namespace
{
class actor_destroy_store_t final : public fw::location_store_t
{
  public:
    fw::task_t<fw::store_read_result_t> read (fw::store_key_t key) override
    {
        return inner->read (std::move (key));
    }

    fw::task_t<fw::store_write_result_t> write (fw::store_write_request_t request) override
    {
        const auto authority = authority_key.load ();
        if (reject_next_authority_put && authority
            && std::ranges::any_of (request.mutations,
                                    [&] (const auto &mutation) {
                                        const auto *put = std::get_if<fw::store_put_t> (&mutation);
                                        return put && put->key.value == authority->value;
                                    })
            && reject_next_authority_put.exchange (false))
            co_return fw::store_write_corrupt_t{};
        if (authority && std::ranges::any_of (request.mutations, [&] (const auto &mutation) {
                const auto *remove = std::get_if<fw::store_delete_t> (&mutation);
                return remove && remove->key.value == authority->value;
            })) {
            deleting.complete (fw::result_t<void>::success ());
            co_await proceed.task ();
            if (reject_delete)
                co_return fw::store_write_corrupt_t{};
        }
        co_return co_await inner->write (std::move (request));
    }

    fw::task_t<fw::store_scan_result_t> scan (fw::store_scan_request_t request) override
    {
        static const auto mesh_prefix = std::string ("mesh-node") + '\0' + mesh_name + '\0';
        if (hide_mesh_descriptors && request.prefix == mesh_prefix)
            co_return fw::store_scan_page_t{};
        co_return co_await inner->scan (std::move (request));
    }

    std::shared_ptr<fw::location_store_t> inner =
      std::make_shared<fw::runtime::in_memory_location_store_t> ();
    std::atomic<std::shared_ptr<const fw::store_key_t>> authority_key;
    std::atomic<bool> hide_mesh_descriptors{false};
    std::atomic<bool> reject_next_authority_put{false};
    std::atomic<bool> reject_delete{false};
    fw::task_completion_source_t<void> deleting;
    fw::task_completion_source_t<void> proceed;
};

struct release_destroy_t
{
    fw::task_completion_source_t<void> &gate;

    ~release_destroy_t () { gate.complete (fw::result_t<void>::success ()); }
};

class ActorDestroyContract : public testing::Test
{
  protected:
    void SetUp () override
    {
        configure (owner, std::make_shared<observations_t> (), first_rid, store,
                   std::make_shared<fw::runtime::in_memory_relocation_store_t> ());
        owner.start ();
        ASSERT_TRUE (ready (owner));
        services = owner.app.advanced ().services ().build_provider ();
        manager = services->get_required<fw::actor_manager_t> ();
        const auto created = manager.get_or_create (fw::actor_id_t (actor_id), actor_type)
                               .async ()
                               .result_for (wait_budget);
        ASSERT_TRUE (created && *created);
        actor_reference = std::get<fw::actor_create_created_t> (created->value ()).actor;
        // An application reply confirms that the initial Entry handoff released the move seal.
        const auto probe = services->get_required<fw::actor_client_t> ()
                             .request (fw::actor_id_t (actor_id), probe_t{17})
                             .async<probe_t> ()
                             .result_for (wait_budget);
        ASSERT_TRUE (probe && *probe);
        ASSERT_EQ (probe->value ().value, 17);
        identify_authority ();
    }

    void TearDown () override { store->reject_delete = false; }

    void identify_authority ()
    {
        const auto authority = services->get_required<fw::location_repository_t> ()
                                 .read_authority (fw::runtime::actor_authority_key (actor_id))
                                 .result ()
                                 .value ();
        before_destroy = std::get<fw::authority_snapshot_t> (authority);
        const auto scan = store->scan ({"", std::nullopt, 100}).result ().value ();
        for (const auto &entry : std::get<fw::store_scan_page_t> (scan).items)
            if (entry.value.version.value == before_destroy.store_version)
                store->authority_key.store (std::make_shared<const fw::store_key_t> (entry.key));
        ASSERT_TRUE (store->authority_key.load ());
    }

    std::shared_ptr<actor_destroy_store_t> store = std::make_shared<actor_destroy_store_t> ();
    host_t owner;
    std::optional<fw::service_provider_t> services;
    fw::actor_manager_t manager;
    const fw::actor_ref_t &actor () const { return *actor_reference; }
    std::optional<fw::actor_ref_t> actor_reference;
    fw::authority_snapshot_t before_destroy;
};
} // namespace

TEST_F (ActorDestroyContract, GetOrCreateWaitsForNewIncarnation)
{
    release_destroy_t release{store->proceed};
    const auto destroying = manager.destroy (actor ());
    ASSERT_TRUE (store->deleting.task ().result_for (wait_budget));
    const auto creating = manager.get_or_create (fw::actor_id_t (actor_id), actor_type).async ();
    EXPECT_FALSE (creating.result_for (100ms).has_value ());
    store->proceed.complete (fw::result_t<void>::success ());
    const auto destroyed = destroying.result_for (wait_budget);
    ASSERT_TRUE (destroyed && *destroyed && destroyed->value ());
    const auto created = creating.result_for (wait_budget);
    ASSERT_TRUE (created && *created);
    const auto *new_actor = std::get_if<fw::actor_create_created_t> (&created->value ());
    ASSERT_NE (new_actor, nullptr);
    EXPECT_GT (new_actor->actor.object_generation (), actor ().object_generation ());
}

TEST_F (ActorDestroyContract, FindOmitsClosingAuthority)
{
    release_destroy_t release{store->proceed};
    const auto destroying = manager.destroy (actor ());
    ASSERT_TRUE (store->deleting.task ().result_for (wait_budget));
    const auto authority = services->get_required<fw::location_repository_t> ()
                             .read_authority (fw::runtime::actor_authority_key (actor_id))
                             .result ()
                             .value ();
    const auto &closing = std::get<fw::authority_snapshot_t> (authority);
    const auto payload = fw::runtime::decode_canonical_authority_payload (closing.payload);
    ASSERT_TRUE (payload && !payload->body.empty ());
    EXPECT_EQ (payload->body.front (), std::byte{3});
    EXPECT_TRUE (closing.same_incarnation (before_destroy));
    EXPECT_EQ (closing.owner.lease_generation, before_destroy.owner.lease_generation);
    EXPECT_NE (closing.store_version, before_destroy.store_version);
    const auto found = manager.find (fw::actor_id_t (actor_id)).result_for (wait_budget);
    ASSERT_TRUE (found && *found);
    EXPECT_FALSE (found->value ().has_value ());
    store->proceed.complete (fw::result_t<void>::success ());
    const auto destroyed = destroying.result_for (wait_budget);
    ASSERT_TRUE (destroyed && *destroyed && destroyed->value ());
}

TEST_F (ActorDestroyContract, ClosingCasFailureRestoresActorAndAllowsDestroy)
{
    release_destroy_t release{store->proceed};
    // After Entry handoff, the first Put on this authority is the Closing CAS.
    store->reject_next_authority_put = true;
    const auto failed = manager.destroy (actor ()).result_for (wait_budget);
    ASSERT_TRUE (failed && !*failed);
    ASSERT_FALSE (store->reject_next_authority_put.load ());

    const auto authority = services->get_required<fw::location_repository_t> ()
                             .read_authority (fw::runtime::actor_authority_key (actor_id))
                             .result_for (wait_budget);
    ASSERT_TRUE (authority && *authority);
    const auto *ready = std::get_if<fw::authority_snapshot_t> (&authority->value ());
    ASSERT_NE (ready, nullptr);
    const auto payload = fw::runtime::decode_canonical_authority_payload (ready->payload);
    ASSERT_TRUE (payload && !payload->body.empty ());
    EXPECT_EQ (payload->body.front (), std::byte{0});
    EXPECT_EQ (ready->store_version, before_destroy.store_version);

    const auto probe = services->get_required<fw::actor_client_t> ()
                         .request (actor ().actor_id (), probe_t{23})
                         .async<probe_t> ()
                         .result_for (wait_budget);
    EXPECT_TRUE (probe && *probe && probe->value ().value == 23);

    store->proceed.complete (fw::result_t<void>::success ());
    const auto destroyed = manager.destroy (actor ()).result_for (wait_budget);
    ASSERT_TRUE (destroyed && *destroyed && destroyed->value ());
}

TEST_F (ActorDestroyContract, OwnerDestroyJoinsTerminal)
{
    release_destroy_t release{store->proceed};
    const auto first = manager.destroy (actor ());
    ASSERT_TRUE (store->deleting.task ().result_for (wait_budget));
    const auto second = manager.destroy (actor ());
    EXPECT_FALSE (second.result_for (100ms).has_value ());
    store->proceed.complete (fw::result_t<void>::success ());
    const auto first_result = first.result_for (wait_budget);
    const auto second_result = second.result_for (wait_budget);
    ASSERT_TRUE (first_result && *first_result && first_result->value ());
    ASSERT_TRUE (second_result && *second_result && second_result->value ());
}

TEST_F (ActorDestroyContract, OwnerDestroyJoinsFailedTerminal)
{
    release_destroy_t release{store->proceed};
    store->reject_delete = true;
    const auto first = manager.destroy (actor ());
    ASSERT_TRUE (store->deleting.task ().result_for (wait_budget));
    const auto second = manager.destroy (actor ());
    EXPECT_FALSE (second.result_for (100ms).has_value ());
    store->proceed.complete (fw::result_t<void>::success ());
    const auto failed = first.result_for (wait_budget);
    const auto joined = second.result_for (wait_budget);
    const auto later = manager.destroy (actor ()).result_for (wait_budget);
    ASSERT_TRUE (failed && !*failed);
    ASSERT_TRUE (joined && !*joined);
    ASSERT_TRUE (later && !*later);
    EXPECT_EQ (joined->error_kind (), failed->error_kind ());
    EXPECT_EQ (later->error_kind (), failed->error_kind ());
    const auto found = manager.find (fw::actor_id_t (actor_id)).result_for (wait_budget);
    ASSERT_TRUE (found && *found);
    EXPECT_FALSE (found->value ().has_value ());
}

TEST_F (ActorDestroyContract, OwnTurnReturnsAfterClosingAndOwnerJoinsTerminal)
{
    const auto registration =
      fw::detail::mesh_node_runtime_t::registrations (owner.app.advanced ().zlink ()).front ();
    const auto returned = fw::detail::make_task_completion_source<void> ();
    const auto finish_turn = fw::detail::make_task_completion_source<void> ();
    release_destroy_t release_turn{*finish_turn};
    release_destroy_t release_delete{store->proceed};
    const auto turn =
      fw::detail::spot_node_runtime_t (registration->spot_state)
        .run_actor_turn (
          actor (),
          [manager = manager, ref = actor (), returned,
           finish_turn] (fw::actor_t &, std::stop_token) mutable -> fw::task_t<fw::result_t<bool>> {
              const auto destroyed = co_await manager.destroy (ref);
              returned->complete (fw::result_t<void>::success ());
              co_await finish_turn->task ();
              co_return fw::result_t<bool>::success (destroyed);
          });
    ASSERT_TRUE (returned->task ().result_for (wait_budget));
    const auto found = manager.find (fw::actor_id_t (actor_id)).result_for (wait_budget);
    ASSERT_TRUE (found && *found);
    EXPECT_FALSE (found->value ().has_value ());
    EXPECT_FALSE (store->deleting.task ().await_ready ());
    const auto second = manager.destroy (actor ());
    EXPECT_FALSE (second.result_for (100ms).has_value ());
    finish_turn->complete (fw::result_t<void>::success ());
    const auto turn_result = turn.result_for (wait_budget);
    ASSERT_TRUE (turn_result && *turn_result && turn_result->value ()
                 && turn_result->value ().value ());
    ASSERT_TRUE (store->deleting.task ().result_for (wait_budget));
    store->proceed.complete (fw::result_t<void>::success ());
    const auto second_result = second.result_for (wait_budget);
    ASSERT_TRUE (second_result && *second_result && second_result->value ());
}

TEST_F (ActorDestroyContract, RemoteDestroyWaitsForOwnerAuthorityRemoval)
{
    host_t caller;
    configure (caller, std::make_shared<observations_t> (), second_rid, store,
               std::make_shared<fw::runtime::in_memory_relocation_store_t> ());
    caller.start ();
    ASSERT_TRUE (ready (caller));
    auto caller_services = caller.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (peer_ready (caller_services.get_required<fw::route_mesh_runtime_t> (), first_rid));
    auto &remote = caller_services.get_required<fw::actor_manager_t> ();
    release_destroy_t release{store->proceed};
    const auto pending = remote.destroy (actor ());
    ASSERT_TRUE (store->deleting.task ().result_for (wait_budget));
    const auto found = manager.find (fw::actor_id_t (actor_id)).result_for (wait_budget);
    ASSERT_TRUE (found && *found);
    EXPECT_FALSE (found->value ().has_value ());
    const auto owner_destroy = manager.destroy (actor ());
    EXPECT_FALSE (owner_destroy.result_for (100ms).has_value ());
    store->proceed.complete (fw::result_t<void>::success ());
    const auto removed = pending.result_for (wait_budget);
    const auto owner_result = owner_destroy.result_for (wait_budget);
    ASSERT_TRUE (removed && *removed && removed->value ());
    ASSERT_TRUE (owner_result && *owner_result && owner_result->value ());
}

TEST_F (ActorDestroyContract, RemoteDestroyWithoutAdmittedOwnerIsUnavailable)
{
    // Keep the Actor authority readable while preventing either host from discovering its peer.
    store->hide_mesh_descriptors = true;
    host_t caller;
    auto &options = caller.app.add_zlink_framework ();
    caller.app.logging ().use_file (std::string (second_rid) + ".flow");
    options.configure_dispatch ().message_flow (fw::message_flow_log_mode_t::detailed);
    options.configure_locations ().owner_lease_renew_interval = 10ms;
    options.configure_locations ().owner_lease_fencing_margin = 1ms;
    options.add_location_store (store);
    options.add_route_mesh (mesh_name)
      .set_object_role (fw::object_role_t::client)
      .set_routing_id (zlink::routing_id_t::from (second_rid))
      .listen ("tcp://127.0.0.1:0")
      .set_default_request_timeout (wait_budget);
    caller.start ();
    ASSERT_TRUE (ready (caller));
    auto caller_services = caller.app.advanced ().services ().build_provider ();
    auto &routes = caller_services.get_required<fw::route_mesh_runtime_t> ();
    ASSERT_EQ (routes.snapshot (mesh_name).ready_peer_count, 0u);

    release_destroy_t release{store->proceed};
    auto &remote = caller_services.get_required<fw::actor_manager_t> ();
    const auto deadline = std::chrono::steady_clock::now () + wait_budget;
    const auto destroyed = remote.destroy (actor ()).result_for (wait_budget);
    ASSERT_TRUE (destroyed && !*destroyed);
    EXPECT_LT (std::chrono::steady_clock::now (), deadline);
    EXPECT_EQ (destroyed->error_kind (), fw::framework_error_kind_t::unavailable);
    // A disconnected binding can also produce Unavailable after a submission attempt.
    ASSERT_NE (destroyed->error (), nullptr);
    EXPECT_STREQ (destroyed->error ()->what (), "Actor owner is not RouteMesh-admitted");

    const auto authority = services->get_required<fw::location_repository_t> ()
                             .read_authority (fw::runtime::actor_authority_key (actor_id))
                             .result_for (wait_budget);
    ASSERT_TRUE (authority && *authority);
    const auto *current = std::get_if<fw::authority_snapshot_t> (&authority->value ());
    ASSERT_NE (current, nullptr);
    EXPECT_EQ (current->store_version, before_destroy.store_version);

    const auto probe = services->get_required<fw::actor_client_t> ()
                         .request (actor ().actor_id (), probe_t{23})
                         .async<probe_t> ()
                         .result_for (wait_budget);
    ASSERT_TRUE (probe && *probe);
    EXPECT_EQ (probe->value ().value, 23);
}

TEST_F (ActorDestroyContract, StaleRefCannotDestroyReplacement)
{
    store->proceed.complete (fw::result_t<void>::success ());
    const auto destroyed = manager.destroy (actor ()).result_for (wait_budget);
    ASSERT_TRUE (destroyed && *destroyed && destroyed->value ());
    const auto missing = manager.destroy (actor ()).result_for (wait_budget);
    ASSERT_TRUE (missing && *missing);
    EXPECT_FALSE (missing->value ());
    const auto created = manager.get_or_create (fw::actor_id_t (actor_id), actor_type)
                           .async ()
                           .result_for (wait_budget);
    ASSERT_TRUE (created && *created);
    const auto replacement = std::get<fw::actor_create_created_t> (created->value ()).actor;
    ASSERT_GT (replacement.object_generation (), actor ().object_generation ());
    const auto stale = manager.destroy (actor ()).result_for (wait_budget);
    ASSERT_TRUE (stale);
    ASSERT_FALSE (*stale);
    EXPECT_EQ (stale->error_kind (), fw::framework_error_kind_t::invalid_operation);
    const auto found = manager.find (fw::actor_id_t (actor_id)).result_for (wait_budget);
    ASSERT_TRUE (found && *found && found->value ());
    EXPECT_EQ (found->value ()->object_generation (), replacement.object_generation ());
}
} // namespace zlink::framework::test::actor_maintenance_join
