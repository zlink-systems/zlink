/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/discarded_authority_fixture.hpp"
#include "../support/host_lifecycle_remote_create_spot.hpp"
#include "../support/host_lifecycle_wait.hpp"

#include <algorithm>
#include <atomic>
#include <span>
#include <thread>

namespace
{
using namespace zlink::framework::test::host_lifecycle;

class CreationReclaimTargetPolicy : public DiscardedAuthority
{
  protected:
    object_reserve_request_t target_request (const std::string &id,
                                             bool spot,
                                             std::optional<maintenance_policy_kind_t> policy)
    {
        auto r = request (id, spot);
        auto target = descriptor;
        const auto rid = "replacement-" + id;
        target.rid = zlink::routing_id_t::from (rid);
        target.entry_spot_id = rid + "-entry";
        target.security_identity = rid;
        const auto next = std::get<owner_lease_claimed_t> (
          repository->claim_owner_lease (rid + "-owner", 1h).result ().value ());
        target.owner_id = next.token.owner_id;
        target.lease_generation = next.token.lease_generation;
        const auto matches_type = [&] (const auto &entry) {
            return entry.object_kind == r.key.kind && entry.stable_type == r.intent.stable_type;
        };
        if (policy)
            std::find_if (target.object_capabilities.begin (), target.object_capabilities.end (),
                          matches_type)
              ->policy = *policy;
        else {
            std::erase_if (target.object_capabilities, matches_type);
            std::erase_if (target.capacity.spot_types, matches_type);
        }
        EXPECT_EQ (location_write_status_t::stored,
                   repository->update_mesh_node (target, location_write_intent_t::new_claim)
                     .result ()
                     .value ()
                     .status);
        r.target = {target.mesh_name, node_rid_t::from_string (target.rid.to_string ()),
                    target.lifecycle_generation, next.token};
        return r;
    }
};

TEST_P (CreationReclaimTargetPolicy, DisabledTargetOverridesRequesterRecreate)
{
    const auto actor = create ("ended-actor");
    const auto spot = create ("ended-spot", nullptr, true);
    ASSERT_TRUE (std::holds_alternative<owner_lease_released_t> (
      repository->release_owner_lease (owner).result ().value ()));
    const auto verify = [&] (const char *id, bool is_spot, const authority_snapshot_t &before) {
        SCOPED_TRACE (id);
        auto r = target_request (id, is_spot, maintenance_policy_kind_t::disabled);
        r.relocation_policy = detail::factory_relocation_kind_t::recreate;
        const auto reserved = repository->reserve (r).result ();
        ASSERT_TRUE (reserved) << reserved.error ()->what ();
        const auto *winner = std::get_if<object_reserved_t> (&reserved.value ());
        ASSERT_NE (winner, nullptr);
        EXPECT_GT (winner->fence.object_generation, before.object_generation);
    };
    verify ("ended-actor", false, actor);
    verify ("ended-spot", true, spot);
}

TEST_P (CreationReclaimTargetPolicy, RequesterDisabledDoesNotAuthorizeRecreateOrMissingCapability)
{
    struct case_t
    {
        const char *id;
        bool is_spot;
        std::optional<maintenance_policy_kind_t> policy;
    };
    const case_t cases[] = {{"recreate-actor", false, maintenance_policy_kind_t::recreate},
                            {"missing-actor", false, std::nullopt},
                            {"recreate-spot", true, maintenance_policy_kind_t::recreate},
                            {"missing-spot", true, std::nullopt}};
    std::vector<authority_snapshot_t> old_authorities;
    for (const auto &scenario : cases)
        old_authorities.push_back (create (scenario.id, nullptr, scenario.is_spot));
    ASSERT_TRUE (std::holds_alternative<owner_lease_released_t> (
      repository->release_owner_lease (owner).result ().value ()));
    for (std::size_t i = 0; i < std::size (cases); ++i) {
        const auto &[id, is_spot, policy] = cases[i];
        SCOPED_TRACE (id);
        auto r = target_request (id, is_spot, policy);
        r.relocation_policy = detail::factory_relocation_kind_t::disabled;
        const auto result = repository->reserve (r).result ();
        EXPECT_FALSE (result);
        if (!result)
            EXPECT_EQ (framework_error_kind_t::unavailable, result.error_kind ());
        const auto key = is_spot ? spot_authority_key (id) : actor_authority_key (id);
        const auto current = repository->read_authority_core (key).result ().value ();
        const auto *retained = std::get_if<authority_snapshot_t> (&current);
        EXPECT_NE (retained, nullptr);
        if (retained)
            EXPECT_EQ (old_authorities[i].store_version, retained->store_version);
    }
}

TEST_P (CreationReclaimTargetPolicy, TargetDescriptorRepublishRejectsProviderRelease)
{
    if (!GetParam ())
        GTEST_SKIP () << "The in-memory repository reads and releases in one lane turn";
    const auto actor = create ("ended-actor");
    const auto spot = create ("ended-spot", nullptr, true);
    ASSERT_TRUE (std::holds_alternative<owner_lease_released_t> (
      repository->release_owner_lease (owner).result ().value ()));
    const auto verify = [&] (const char *id, bool is_spot, const authority_snapshot_t &before) {
        SCOPED_TRACE (id);
        auto r = target_request (id, is_spot, maintenance_policy_kind_t::disabled);
        r.relocation_policy = detail::factory_relocation_kind_t::disabled;
        const store_key_t target_key{
          std::string ("mesh-node") + '\0' + r.target.mesh_name + '\0'
          + zlink::routing_id_t::from (std::string (r.target.node_rid.value ())).to_hex ()};
        const auto descriptor_read = opaque.inner.read (target_key).result ().value ();
        const auto *published = std::get_if<store_found_t> (&descriptor_read);
        ASSERT_NE (published, nullptr);
        auto updated = nlohmann::json::parse (published->value.bytes);
        auto &updated_descriptor = updated.at ("descriptor");
        updated_descriptor["descriptorRevision"] = "2";
        for (auto &capability : updated_descriptor.at ("objectCapabilities")) {
            if (capability.at ("stableType") == r.intent.stable_type)
                capability["policy"] = "recreate";
        }
        const auto encoded = updated.dump ();
        const auto bytes = std::as_bytes (std::span (encoded));
        const auto writes_before_release = opaque.writes.size ();
        bool republished = false;
        opaque.before_write = [&] () -> task_t<void> {
            store_write_request_t update;
            update.conditions.push_back (
              store_version_condition_t{target_key, published->value.version});
            update.mutations.push_back (
              store_put_t{target_key, {bytes.begin (), bytes.end ()}, std::nullopt});
            republished = std::holds_alternative<store_write_applied_t> (
              co_await opaque.inner.write (std::move (update)));
        };
        const auto result = repository->reserve (r).result ();
        EXPECT_TRUE (republished);
        ASSERT_GT (opaque.writes.size (), writes_before_release);
        EXPECT_TRUE (std::any_of (
          opaque.writes[writes_before_release].conditions.begin (),
          opaque.writes[writes_before_release].conditions.end (), [&] (const auto &condition) {
              const auto *version = std::get_if<store_version_condition_t> (&condition);
              return version && version->key.value == target_key.value
                     && version->expected.value == published->value.version.value;
          }));
        EXPECT_FALSE (result);
        if (!result)
            EXPECT_EQ (framework_error_kind_t::unavailable, result.error_kind ());
        const auto key = is_spot ? spot_authority_key (id) : actor_authority_key (id);
        const auto current = repository->read_authority_core (key).result ().value ();
        const auto *retained = std::get_if<authority_snapshot_t> (&current);
        ASSERT_NE (retained, nullptr);
        EXPECT_EQ (before.store_version, retained->store_version);
    };
    verify ("ended-actor", false, actor);
    verify ("ended-spot", true, spot);
}

class reclaim_user_spot_t final : public spot_t<configuration_actor_t>
{
  public:
    explicit reclaim_user_spot_t (spot_context_t context) : _context (std::move (context)) {}
    spot_context_t &context () noexcept override { return _context; }
    const spot_context_t &context () const noexcept override { return _context; }
    void configure () override {}
    task_t<spot_create_response_t> on_create (const message_t &) override
    {
        co_return spot_create_response_t::accept ();
    }
    task_t<spot_actor_join_result_t> on_actor_join (std::string_view, const message_t &) override
    {
        co_return spot_actor_join_result_t::accept ();
    }
    task_t<void> on_actor_joined (configuration_actor_t &) override { co_return; }
    task_t<void> on_leave_actor (configuration_actor_t &) override { co_return; }

  private:
    spot_context_t _context;
};

struct running_reclaim_host_t
{
    app_t app = app_t::create ();
    std::thread worker;

    ~running_reclaim_host_t ()
    {
        if (worker.joinable ()) {
            app.request_stop ();
            worker.join ();
        }
    }

    void start ()
    {
        worker = std::thread ([this] { EXPECT_EQ (0, app.run (0, nullptr)); });
    }
};

TEST_P (CreationReclaimTargetPolicy, SplitPlacementCreatesActorAndUserSpotWithNewGenerations)
{
    const auto runtime_store = std::make_shared<in_memory_location_store_t> ();
    provider_location_repository_t runtime_repository (*runtime_store);
    in_memory_location_repository_t runtime_memory;
    repository =
      GetParam () ? static_cast<location_repository_t *> (&runtime_repository) : &runtime_memory;
    std::atomic_int spot_factories{0};
    configuration_actor_factory_t::create_count = 0;
    running_reclaim_host_t target;
    running_reclaim_host_t source;
    const auto configure = [&] (app_t &app, const char *rid, bool target_node) {
        auto &options = app.add_zlink_framework ();
        options.configure_locations ().polling_interval = 10ms;
        options.configure_locations ().owner_lease_renew_interval = 10ms;
        options.configure_locations ().owner_lease_fencing_margin = 1ms;
        options.add_location_store (runtime_store);
        options.add_relocation_store (std::make_shared<in_memory_relocation_store_t> ());
        if (!GetParam ())
            app.advanced ().services ().add_factory<location_repository_t> (
              [this] (service_provider_t &) {
                  return std::shared_ptr<location_repository_t> (repository,
                                                                 [] (location_repository_t *) {});
              },
              service_lifetime_t::singleton);
        auto mesh = options.add_route_mesh (descriptor.mesh_name);
        mesh.set_object_role (object_role_t::server)
          .set_routing_id (zlink::routing_id_t::from (rid))
          .listen ("tcp://127.0.0.1:0");
        if (target_node) {
            mesh.add_entry_spot<remote_create_entry_spot_t> ([] (entry_spot_context_t context) {
                return std::make_shared<remote_create_entry_spot_t> (std::move (context));
            });
            mesh.add_actor_factory<configuration_actor_t, configuration_actor_factory_t> (
              "player", std::make_shared<configuration_actor_factory_t> (),
              [] (auto &factory) { factory.disable_relocation (); });
            mesh.add_spot_factory<reclaim_user_spot_t> (
              "room",
              [&] (spot_context_t context) {
                  ++spot_factories;
                  return std::make_shared<reclaim_user_spot_t> (std::move (context));
              },
              [] (auto &factory) { factory.disable_relocation (); });
        }
    };
    configure (target.app, "target", true);
    configure (source.app, "requester", false);
    target.start ();
    ASSERT_TRUE (wait_until ([&] { return target.app.is_ready (); }, 5s))
      << static_cast<int> (target.app.runtime_state ());
    source.start ();
    ASSERT_TRUE (wait_until ([&] { return source.app.is_ready (); }, 5s));
    auto services = source.app.advanced ().services ().build_provider ();
    auto &routes = services.get_required<route_mesh_runtime_t> ();
    ASSERT_TRUE (wait_until (
      [&] { return routes.snapshot (descriptor.mesh_name).ready_peer_count == 1; }, 5s));

    seed_owner ();
    const auto old_actor = create ("old-actor");
    const auto old_spot = create ("old-spot", nullptr, true);
    ASSERT_EQ (location_write_status_t::stored,
               repository->remove_mesh_node ({descriptor.mesh_name, descriptor.rid}, owner)
                 .result ()
                 .value ());
    ASSERT_TRUE (std::holds_alternative<owner_lease_released_t> (
      repository->release_owner_lease (owner).result ().value ()));
    services.get_required<location_repository_t> ().configure_reclaim_margin (0ms);

    const auto actor = services.get_required<actor_manager_t> ()
                         .get_or_create (actor_id_t ("old-actor"), "player")
                         .timeout (3s)
                         .async ()
                         .result ();
    EXPECT_TRUE (actor) << (actor.error () ? actor.error ()->what () : "");
    if (actor) {
        const auto *created = std::get_if<actor_create_created_t> (&actor.value ());
        ASSERT_NE (created, nullptr);
        EXPECT_GT (created->actor.object_generation (), old_actor.object_generation);
        EXPECT_EQ (created->actor.node_rid ().value (), "target");
    }
    EXPECT_EQ (1, configuration_actor_factory_t::create_count);

    const auto spot = services.get_required<spot_manager_t> ()
                        .get_or_create (spot_id_t ("old-spot"), "room")
                        .timeout (3s)
                        .async ()
                        .result ();
    EXPECT_TRUE (spot) << (spot.error () ? spot.error ()->what () : "");
    if (spot) {
        EXPECT_EQ (spot_create_state_t::created, spot.value ().state);
        EXPECT_GT (spot.value ().spot.object_generation (), old_spot.object_generation);
        EXPECT_EQ (spot.value ().spot.node_rid ().value (), "target");
    }
    EXPECT_EQ (1, spot_factories);
}

INSTANTIATE_TEST_SUITE_P (ProviderAndMemory, CreationReclaimTargetPolicy, ::testing::Bool ());
}
