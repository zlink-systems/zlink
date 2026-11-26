/* SPDX-License-Identifier: Elastic-2.0 */

#include "actor_maintenance_join_actors.hpp"
#include "actor_maintenance_join_host_fixture.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include <zlink/framework.hpp>
#include <gtest/gtest.h>
#include <atomic>
#include <iostream>
#include <string_view>

namespace zlink::framework::test::actor_maintenance_join
{
namespace
{
constexpr auto disabled_type = actor_type;
constexpr auto enabled_type = "relocatable-member";
constexpr auto enabled_id = "enabled-member";
constexpr auto target_id = "join-policy-room";

class policy_spot_t final : public fw::spot_t<actor_t>
{
  public:
    policy_spot_t (fw::spot_context_t context, std::shared_ptr<std::atomic<int>> callbacks) :
        _context (std::move (context)), _callbacks (std::move (callbacks))
    {
    }

    fw::spot_context_t &context () noexcept override { return _context; }
    const fw::spot_context_t &context () const noexcept override { return _context; }
    void configure () override {}
    fw::task_t<void> on_actor_joined (actor_t &) override { co_return; }
    fw::task_t<void> on_leave_actor (actor_t &) override { co_return; }

    fw::task_t<fw::spot_actor_join_result_t> on_actor_join (std::string_view id,
                                                            const fw::message_t &) override
    {
        ++*_callbacks;
        co_return id == enabled_id ? fw::spot_actor_join_result_t::reject ()
                                   : fw::spot_actor_join_result_t::accept ();
    }

  private:
    fw::spot_context_t _context;
    std::shared_ptr<std::atomic<int>> _callbacks;
};

void configure_policy_host (host_t &host,
                            std::string_view rid,
                            bool mixed,
                            const std::shared_ptr<fw::location_store_t> &locations,
                            const std::shared_ptr<fw::relocation_store_t> &relocations,
                            const std::shared_ptr<observations_t> &observations,
                            const std::shared_ptr<std::atomic<int>> &callbacks)
{
    auto &options = host.app.add_zlink_framework ();
    host.app.logging ()
      .use_file (std::string (rid) + "-join-policy.flow")
      .set_min_level (fw::log_level_t::debug);
    options.configure_dispatch ().message_flow (fw::message_flow_log_mode_t::normal);
    options.configure_locations ().owner_lease_renew_interval = 10ms;
    options.configure_locations ().owner_lease_fencing_margin = 1ms;
    options.add_location_store (locations);
    options.add_relocation_store (relocations);
    auto mesh = options.add_route_mesh (mesh_name);
    mesh.set_object_role (fw::object_role_t::server)
      .set_routing_id (zlink::routing_id_t::from (std::string (rid)))
      .listen ("tcp://127.0.0.1:0")
      .add_entry_spot<entry_t> ([observations] (fw::entry_spot_context_t context) {
          return std::make_shared<entry_t> (std::move (context), observations);
      })
      .add_spot_factory<policy_spot_t> (
        room_type,
        [callbacks] (fw::spot_context_t context) {
            return std::make_shared<policy_spot_t> (std::move (context), callbacks);
        },
        [] (auto &factory) { factory.disable_relocation (); })
      .add_actor_factory<actor_t, factory_t> (
        disabled_type, std::make_shared<factory_t> (observations),
        [] (auto &factory) { factory.disable_relocation (); });
    if (mixed)
        mesh.add_actor_factory<actor_t, factory_t> (
          enabled_type, std::make_shared<factory_t> (observations),
          [] (auto &factory) { factory.recreate_on_relocation (); });
}

template <typename Join> auto observe_remote_requests (Join join)
{
    testing::internal::CaptureStderr ();
    auto outcome = join ();
    const auto trace = testing::internal::GetCapturedStderr ();
    constexpr std::string_view receive = "receive kind=28 parts=";
    int requests = 0;
    for (auto offset = trace.find (receive); offset != std::string::npos;
         offset = trace.find (receive, offset + receive.size ()))
        ++requests;
    return std::pair{std::move (outcome), requests};
}

std::optional<fw::actor_join_completion_t> deferred_join (fw::service_provider_t &services,
                                                          observations_t &observations,
                                                          const char *id,
                                                          std::size_t expected_completions)
{
    const auto sent = services.get_required<fw::actor_client_t> ()
                        .send (fw::actor_id_t (id), move_t{target_id})
                        .async ()
                        .result ();
    if (!sent) {
        ADD_FAILURE () << (sent.error () ? sent.error ()->what () : "Actor send failed");
        return std::nullopt;
    }
    std::unique_lock lock (observations.mutex);
    if (!observations.changed.wait_for (lock, wait_budget, [&] {
            return observations.completions.size () >= expected_completions;
        }))
        return std::nullopt;
    EXPECT_EQ (observations.completions.size (), expected_completions);
    return observations.completions.back ();
}

class ActorJoinRelocationPolicy : public testing::TestWithParam<bool>
{
};

TEST_P (ActorJoinRelocationPolicy, DisabledActorCanJoinLocallyAndFailsBeforeRemoteAdmission)
{
    const auto mixed = GetParam ();
    auto locations = std::make_shared<fw::runtime::in_memory_location_store_t> ();
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    auto source_observations = std::make_shared<observations_t> ();
    auto target_observations = std::make_shared<observations_t> ();
    auto callbacks = std::make_shared<std::atomic<int>> (0);
    host_t source, target;
    configure_policy_host (target, second_rid, mixed, locations, relocations, target_observations,
                           callbacks);
    target.start ();
    ASSERT_TRUE (ready (target));
    auto target_services = target.app.advanced ().services ().build_provider ();
    auto &target_actors = target_services.get_required<fw::actor_manager_t> ();
    const auto local = target_actors.get_or_create (fw::actor_id_t ("local-member"), disabled_type)
                         .timeout (wait_budget)
                         .async ()
                         .result ();
    ASSERT_TRUE (local) << local.error ()->what ();
    ASSERT_TRUE (std::holds_alternative<fw::actor_create_created_t> (local.value ()));
    EXPECT_EQ (std::get<fw::actor_create_created_t> (local.value ()).actor.node_rid ().value (),
               second_rid);

    const auto spot = target_services.get_required<fw::spot_manager_t> ()
                        .get_or_create (fw::spot_id_t (target_id), room_type)
                        .timeout (wait_budget)
                        .async ()
                        .result ();
    ASSERT_TRUE (spot) << spot.error ()->what ();
    ASSERT_EQ (spot.value ().spot.node_rid ().value (), second_rid);

    configure_policy_host (source, first_rid, mixed, locations, relocations, source_observations,
                           callbacks);
    source.start ();
    ASSERT_TRUE (ready (source));
    auto source_services = source.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (
      peer_ready (source_services.get_required<fw::route_mesh_runtime_t> (), second_rid));
    ASSERT_TRUE (peer_ready (target_services.get_required<fw::route_mesh_runtime_t> (), first_rid));
    ASSERT_TRUE (set_published_placement_weight (target_services, 0, &source_services));

    const auto [same_node, same_node_requests] = observe_remote_requests (
      [&] { return deferred_join (target_services, *target_observations, "local-member", 1); });
    ASSERT_TRUE (same_node);
    EXPECT_TRUE (std::holds_alternative<fw::actor_join_accepted_t> (*same_node));
    const auto local_callbacks = callbacks->exchange (0);
    EXPECT_EQ (local_callbacks, 1);
    EXPECT_EQ (same_node_requests, 0);
    std::cout << "same-node: callbacks=" << local_callbacks << ", command 28=" << same_node_requests
              << '\n';

    auto &source_actors = source_services.get_required<fw::actor_manager_t> ();
    const auto remote = source_actors.get_or_create (fw::actor_id_t (actor_id), disabled_type)
                          .timeout (wait_budget)
                          .async ()
                          .result ();
    ASSERT_TRUE (remote) << remote.error ()->what ();
    ASSERT_TRUE (std::holds_alternative<fw::actor_create_created_t> (remote.value ()));
    const auto &actor = std::get<fw::actor_create_created_t> (remote.value ()).actor;
    ASSERT_EQ (actor.node_rid ().value (), first_rid);
    const auto gateway =
      source_services.get_required<fw::detail::actor_gateway_runtime_t> ().weak_state ().lock ();
    ASSERT_NE (gateway, nullptr);
    const auto join = gateway->sync ([&] { return gateway->join_spot_dispatcher; });
    ASSERT_TRUE (join);
    const auto [immediate, immediate_requests] = observe_remote_requests ([&] {
        const zlink::message_t request;
        return join (actor, fw::spot_id_t (target_id), request, {}, {}, wait_budget).result ();
    });
    const auto immediate_callbacks = callbacks->exchange (0);
    std::cout << "remote immediate: kind=" << static_cast<int> (immediate.error_kind ())
              << ", callbacks=" << immediate_callbacks << ", command 28=" << immediate_requests
              << '\n';
    EXPECT_FALSE (immediate);
    EXPECT_EQ (immediate.error_kind (), fw::framework_error_kind_t::rejected)
      << (immediate.error () ? immediate.error ()->what () : "");
    EXPECT_EQ (immediate_callbacks, 0);
    EXPECT_EQ (immediate_requests, 0);

    const auto [deferred, deferred_requests] = observe_remote_requests (
      [&] { return deferred_join (source_services, *source_observations, actor_id, 2); });
    ASSERT_TRUE (deferred);
    const auto *failed = std::get_if<fw::actor_join_failed_t> (&*deferred);
    ASSERT_NE (failed, nullptr);
    const auto deferred_callbacks = callbacks->exchange (0);
    std::cout << "remote deferred: kind=" << static_cast<int> (failed->error_kind)
              << ", callbacks=" << deferred_callbacks << ", command 28=" << deferred_requests
              << '\n';
    EXPECT_EQ (failed->error_kind, fw::framework_error_kind_t::rejected);
    EXPECT_EQ (deferred_callbacks, 0);
    EXPECT_EQ (deferred_requests, 0);

    if (mixed) {
        const auto enabled = source_actors.get_or_create (fw::actor_id_t (enabled_id), enabled_type)
                               .timeout (wait_budget)
                               .async ()
                               .result ();
        ASSERT_TRUE (enabled) << enabled.error ()->what ();
        ASSERT_TRUE (std::holds_alternative<fw::actor_create_created_t> (enabled.value ()));
        ASSERT_EQ (
          std::get<fw::actor_create_created_t> (enabled.value ()).actor.node_rid ().value (),
          first_rid);
        const auto [control, control_requests] = observe_remote_requests (
          [&] { return deferred_join (source_services, *source_observations, enabled_id, 3); });
        ASSERT_TRUE (control);
        EXPECT_TRUE (std::holds_alternative<fw::actor_join_rejected_t> (*control));
        const auto control_callbacks = callbacks->load ();
        std::cout << "remote enabled: callbacks=" << control_callbacks
                  << ", command 28=" << control_requests << '\n';
        EXPECT_EQ (control_callbacks, 1);
        EXPECT_EQ (control_requests, 1);
    }
}

INSTANTIATE_TEST_SUITE_P (FactoryConfigurations, ActorJoinRelocationPolicy, testing::Bool ());
} // namespace
} // namespace zlink::framework::test::actor_maintenance_join
