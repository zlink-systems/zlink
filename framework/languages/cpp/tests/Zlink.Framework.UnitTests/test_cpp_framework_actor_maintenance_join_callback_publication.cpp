/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/actor_maintenance_join_host_fixture.hpp"
#include "../support/actor_maintenance_join_location_store_fixtures.hpp"
#include "../support/actor_maintenance_join_packets.hpp"
#include "../support/host_lifecycle_wait.hpp"
#include "runtime/locations/authority_aggregate_relocation.hpp"
#include "runtime/locations/authority_key_codec.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/provider_location_repository.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <tuple>

namespace zlink::framework::test::actor_maintenance_join
{
namespace
{
struct publication_observation_t
{
    fw::task_completion_source_t<fw::authority_snapshot_t> at_callback;
    fw::task_completion_source_t<void> handler_entered;
    fw::task_completion_source_t<void> release_handler;
    fw::task_completion_source_t<void> normalization_started;
    fw::task_completion_source_t<void> release_normalization;
};

class ActorJoinCallbackPublication : public testing::TestWithParam<std::tuple<bool, bool>>
{
};

TEST_P (ActorJoinCallbackPublication, NormalizesAtCapturedRequestBoundary)
{
    const auto [remote, capture_request] = GetParam ();
    auto evidence = std::make_shared<observations_t> ();
    evidence->hold_join_admission = capture_request;
    auto store = std::make_shared<observed_location_store_t> ();
    auto repository = std::make_shared<fw::runtime::provider_location_repository_t> (*store);
    auto relocations = std::make_shared<fw::runtime::in_memory_relocation_store_t> ();
    auto publication = std::make_shared<publication_observation_t> ();
    host_t source, target;
    release_callbacks_t release_callbacks{*evidence};
    const auto release_publication = std::shared_ptr<void> (nullptr, [publication] (void *) {
        publication->release_handler.complete (fw::result_t<void>::success ());
        publication->release_normalization.complete (fw::result_t<void>::success ());
    });
    configure (source, evidence, first_rid, store, relocations);
    source.start ();
    ASSERT_TRUE (ready (source));
    auto services = source.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (services.get_required<fw::actor_manager_t> ()
                   .get_or_create (fw::actor_id_t (actor_id), actor_type)
                   .async ()
                   .result ());
    auto target_services = services;
    if (remote) {
        ASSERT_TRUE (set_published_placement_weight (services, 0));
        configure (target, evidence, second_rid, store, relocations);
        target.start ();
        ASSERT_TRUE (ready (target));
        target_services = target.app.advanced ().services ().build_provider ();
        ASSERT_TRUE (peer_ready (services.get_required<fw::route_mesh_runtime_t> (), second_rid));
    }
    const auto created = target_services.get_required<fw::spot_manager_t> ()
                           .get_or_create (fw::spot_id_t (local_spot), room_type)
                           .async ()
                           .result ();
    ASSERT_TRUE (created);
    const auto &target_rid = remote ? second_rid : first_rid;
    ASSERT_EQ (created.value ().spot.node_rid ().value (), target_rid);

    const auto key = fw::runtime::actor_authority_key (actor_id);
    evidence->join_completion_view = [repository, key, publication] () -> fw::task_t<void> {
        auto row = co_await repository->read_authority (key);
        publication->at_callback.complete (fw::result_t<fw::authority_snapshot_t>::success (
          std::get<fw::authority_snapshot_t> (std::move (row))));
    };
    evidence->before_room_probe_reply = [publication] () -> fw::task_t<void> {
        EXPECT_TRUE (publication->at_callback.task ().await_ready ());
        publication->handler_entered.complete (fw::result_t<void>::success ());
        co_await publication->release_handler.task ();
    };

    if (remote && !capture_request) {
        // Keep the target publication visible until the Store lets the finishing CAS proceed.
        store->before_next_write_when (
          [repository, key] (const fw::store_write_request_t &request) -> fw::task_t<bool> {
              auto row = co_await repository->read_authority (key);
              const auto *snapshot = std::get_if<fw::authority_snapshot_t> (&row);
              if (!snapshot || !fw::runtime::finishing_relocation (*snapshot)
                  || snapshot->allocation.target.node_rid.value () != second_rid)
                  co_return false;
              co_return std::ranges::any_of (request.conditions, [&] (const auto &condition) {
                  const auto *version = std::get_if<fw::store_version_condition_t> (&condition);
                  return version && version->expected.value == snapshot->store_version;
              });
          },
          [publication] () -> fw::task_t<void> {
              publication->normalization_started.complete (fw::result_t<void>::success ());
              co_await publication->release_normalization.task ();
          });
    }

    auto &client = services.get_required<fw::actor_client_t> ();
    ASSERT_TRUE (client.send (fw::actor_id_t (actor_id), move_t{local_spot}).async ().result ());
    std::optional<fw::task_t<probe_t>> request;
    if (capture_request) {
        const auto admission = evidence->join_admission_entered.task ().result_for (wait_budget);
        ASSERT_TRUE (admission && *admission);
        request.emplace (client.request (fw::actor_id_t (actor_id), probe_t{71})
                           .timeout (wait_budget)
                           .async<probe_t> ());
        {
            std::unique_lock lock (evidence->mutex);
            ASSERT_TRUE (evidence->changed.wait_for (
              lock, wait_budget, [&] { return evidence->ordinary_posts == 1; }));
        }
        evidence->release_join_admission.complete (fw::result_t<void>::success ());
    }

    if (remote && !capture_request) {
        const auto normalization =
          publication->normalization_started.task ().result_for (wait_budget);
        ASSERT_TRUE (normalization && *normalization);
        EXPECT_FALSE (publication->at_callback.task ().result_for (150ms))
          << "Join completion ran before the finishing CAS";
        publication->release_normalization.complete (fw::result_t<void>::success ());
    }

    const auto callback = publication->at_callback.task ().result_for (wait_budget);
    ASSERT_TRUE (callback && *callback);
    const auto &at_callback = callback->value ();
    ASSERT_EQ (at_callback.allocation.target.node_rid.value (), target_rid);
    const auto finishing = fw::runtime::finishing_relocation (at_callback);
    EXPECT_EQ (finishing.has_value (), remote && capture_request)
      << "Actor authority is finishing at Join completion";

    if (request) {
        const auto entered = publication->handler_entered.task ().result_for (wait_budget);
        ASSERT_TRUE (entered && *entered);
        EXPECT_FALSE (request->await_ready ());
        auto row = repository->read_authority (key).result ().value ();
        EXPECT_EQ (
          fw::runtime::finishing_relocation (std::get<fw::authority_snapshot_t> (row)).has_value (),
          remote);
        publication->release_handler.complete (fw::result_t<void>::success ());
        const auto reply = request->result_for (wait_budget);
        ASSERT_TRUE (reply && *reply);
        EXPECT_EQ (reply->value ().value, 71);
    } else {
        EXPECT_FALSE (publication->handler_entered.task ().await_ready ());
    }

    ASSERT_TRUE (evidence->join_completed.task ().result_for (wait_budget));
    {
        std::lock_guard lock (evidence->mutex);
        ASSERT_EQ (evidence->completions.size (), 1u);
        EXPECT_TRUE (std::holds_alternative<fw::actor_join_accepted_t> (evidence->completions[0]));
        EXPECT_EQ (evidence->completion_owners[0], target_rid);
    }
    EXPECT_TRUE (host_lifecycle::wait_until (
      [&] {
          auto row = repository->read_authority (key).result ().value ();
          const auto &snapshot = std::get<fw::authority_snapshot_t> (row);
          return !snapshot.aggregate || !snapshot.aggregate->relocation;
      },
      wait_budget));
}

INSTANTIATE_TEST_SUITE_P (LocalAndRemote,
                          ActorJoinCallbackPublication,
                          testing::Combine (testing::Bool (), testing::Bool ()),
                          [] (const auto &info) {
                              const bool remote = std::get<0> (info.param);
                              const bool capture_request = std::get<1> (info.param);
                              return std::string (remote ? "Remote" : "Local")
                                     + (capture_request ? "OneRequest" : "ZeroRequests");
                          });
} // namespace
} // namespace zlink::framework::test::actor_maintenance_join
