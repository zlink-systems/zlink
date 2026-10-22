/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "allocation_counter.hpp"
#include "runtime/stateful/public_host_runtime.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include <gtest/gtest.h>

#include <array>
#include <future>
#include <thread>
#include <cstdio>
#include <limits>
#include <memory>
#include <optional>

namespace
{
namespace fw = zlink::framework;
namespace host = fw::runtime::host;
namespace mesh = fw::runtime::mesh;
namespace wire = fw::runtime::protocol;
namespace stateful = fw::runtime::stateful;

std::shared_ptr<host::public_host_runtime_t> make_host ()
{
    auto runtime = std::make_shared<host::public_host_runtime_t> (host::host_options_t{
      .mesh = {.descriptor = {.mesh_name = "receive-allocation",
                              .node_routing_id = {'n'},
                              .lifecycle_generation = 1,
                              .descriptor_revision = 1,
                              .advertised_endpoint = "tcp://127.0.0.1:0"},
               .runtime_failures = std::make_shared<fw::runtime::runtime_failure_collector_t> ()}});
    return runtime;
}

mesh::service_mailbox_record_t infrastructure_record (std::vector<std::uint8_t> bytes)
{
    mesh::service_mailbox_record_t record;
    record.owner = "control";
    record.domain = mesh::service_mailbox_domain_t::infrastructure;
    record.parts.push_back (zlink::message_t::from (bytes));
    record.source_routing_id = {'s'};
    record.source_node_generation = 1;
    return record;
}

wire::message_follow_notice_t notice ()
{
    wire::actor_route_fence_t source{.actor_id = "actor",
                                     .object_generation = 1,
                                     .target_node_routing_id = {'s'},
                                     .target_node_generation = 1,
                                     .authority_owner_generation = 1,
                                     .owner_lease_generation = 1};
    auto target = source;
    target.target_node_routing_id = {'n'};
    target.authority_owner_generation = 2;
    return {.source = source, .target = target, .hop_count = 1, .original_operation = {1, 1}};
}

TEST (ReceiveAllocations, EmptyTurnDoesNotBuildInfrastructureDispatchStorage)
{
    auto runtime = make_host ();
    unsigned callbacks = 0;
    const auto expected = notice ();
    runtime->configure_message_follow_handler ([&] (const auto &received) {
        EXPECT_EQ (received, expected);
        ++callbacks;
    });
    runtime->start ();
    const auto enqueue = [&] {
        ASSERT_TRUE (runtime->transport ().mailbox ().try_enqueue (
          infrastructure_record (wire::encode_message_follow (expected))));
    };
    const auto dispatch = [&] {
        return runtime->dispatch_ready ([] (const auto &, const auto &, auto) {}, false).result ();
    };
    enqueue ();
    ASSERT_EQ (dispatch ().value (), 1u);
    ASSERT_EQ (dispatch ().value (), 0u);
    enqueue ();
    std::optional<fw::result_t<std::size_t>> result;
    const auto infrastructure =
      fw::tests::measure_allocations ([&] { result.emplace (dispatch ()); });
    ASSERT_TRUE (*result);
    EXPECT_EQ (result->value (), 1u);
    const auto empty = fw::tests::measure_allocations ([&] { result.emplace (dispatch ()); });
    ASSERT_TRUE (*result);
    EXPECT_EQ (result->value (), 0u);
    EXPECT_EQ (callbacks, 2u);
    EXPECT_EQ (runtime->transport ().mailbox ().pending_messages (
                 mesh::service_mailbox_domain_t::infrastructure),
               0u);
    // The real control-record path supplies the platform's storage baseline.
    // An empty receive turn has no infrastructure operation to retain.
    EXPECT_LT (empty.largest, infrastructure.largest);
    std::printf ("receive allocation bytes: infrastructure=%zu/%zu/%zu "
                 "empty=%zu/%zu/%zu\n",
                 infrastructure.count, infrastructure.bytes, infrastructure.largest, empty.count,
                 empty.bytes, empty.largest);
    runtime->close ();
}

TEST (ReceiveAllocations, InfrastructureDispatchRetainsItsClaimAcrossSuspension)
{
    auto runtime = make_host ();
    const auto rid = zlink::routing_id_t::from (std::vector<std::uint8_t>{'n'});
    const stateful::object_ref_t actor{stateful::object_kind_t::actor, "actor",         1, 1,
                                       "receive-allocation",           rid.to_string ()};
    const auto created = runtime->objects ().begin_reserved_object (actor, "actor", {});
    ASSERT_EQ (created.error, stateful::stateful_error_t::none);
    ASSERT_EQ (runtime->objects ().commit_create (created.attempt),
               stateful::stateful_error_t::none);
    fw::task_completion_source_t<std::optional<fw::location_owner_token_t>> owner;
    unsigned owner_calls = 0;
    unsigned bind_calls = 0;
    unsigned follow_calls = 0;
    runtime->configure_message_follow_handler ([&] (const auto &) {
        EXPECT_EQ (bind_calls, 1u);
        ++follow_calls;
    });
    runtime->configure_bound_session_operations (
      {.session_owner =
         [&] (const auto &, auto) {
             ++owner_calls;
             return owner.task ();
         },
       .bind =
         [&] (const auto &, const auto &, auto, const auto &, auto) {
             ++bind_calls;
             return host::bound_session_bind_operation_result_t{stateful::stateful_error_t::none,
                                                                std::nullopt};
         },
       .send =
         [] (const auto &, auto) {
             ADD_FAILURE () << "unexpected bound Session send";
             return stateful::stateful_error_t::conflict;
         },
       .replaced = [] (const auto &) { ADD_FAILURE () << "unexpected Session replacement"; },
       .commit_relocation_route =
         [] (const auto &, const auto &, const auto &) {
             ADD_FAILURE () << "unexpected relocation commit";
             return false;
         },
       .prepare_relocation_target_route =
         [] (const auto &, auto) {
             ADD_FAILURE () << "unexpected relocation preparation";
             return false;
         }});
    runtime->start ();
    const wire::bound_session_bind_t bind{.correlation = 1,
                                          .actor = {.actor_id = "actor",
                                                    .object_generation = 1,
                                                    .target_node_routing_id = {'n'},
                                                    .target_node_generation = 1,
                                                    .authority_owner_generation = 1,
                                                    .owner_lease_generation = 1},
                                          .session_routing_id = {'s'},
                                          .binding = {.generation = 1}};
    ASSERT_TRUE (runtime->transport ().mailbox ().try_enqueue (
      infrastructure_record (wire::encode_bound_session_bind (bind))));
    auto running = runtime->dispatch_ready ([] (const auto &, const auto &, auto) {}, false);
    EXPECT_FALSE (running.await_ready ());
    EXPECT_EQ (owner_calls, 1u);
    EXPECT_EQ (bind_calls, 0u);
    auto &mailbox = runtime->transport ().mailbox ();
    ASSERT_TRUE (
      mailbox.try_enqueue (infrastructure_record (wire::encode_message_follow (notice ()))));
    EXPECT_EQ (mailbox.pending_messages (mesh::service_mailbox_domain_t::infrastructure), 1u);
    EXPECT_FALSE (mailbox.try_claim_owner (mesh::service_mailbox_domain_t::infrastructure,
                                           "control", 1, std::numeric_limits<std::size_t>::max ()));
    ASSERT_TRUE (owner.complete (fw::result_t<std::optional<fw::location_owner_token_t>>::success (
      fw::location_owner_token_t{"session-owner", 1})));
    const auto settled = running.result ();
    ASSERT_TRUE (settled);
    EXPECT_EQ (settled.value (), 2u);
    EXPECT_EQ (bind_calls, 1u);
    EXPECT_EQ (follow_calls, 1u);
    EXPECT_EQ (runtime->transport ().mailbox ().pending_messages (
                 mesh::service_mailbox_domain_t::infrastructure),
               0u);
    runtime->close ();
}
TEST (ReceiveAllocations, InfrastructureFrameFailureReleasesTheClaim)
{
    for (const std::size_t offset : {0u, 1u}) {
        auto runtime = make_host ();
        runtime->start ();
        const auto enqueue = [&] {
            ASSERT_TRUE (runtime->transport ().mailbox ().try_enqueue (
              infrastructure_record (wire::encode_message_follow (notice ()))));
        };
        const auto dispatch = [&] {
            return runtime->dispatch_ready ([] (const auto &, const auto &, auto) {}, false)
              .result ();
        };
        enqueue ();
        ASSERT_EQ (dispatch ().value (), 1u);
        enqueue ();
        std::array<fw::tests::allocation_event_t, 128> events{};
        fw::tests::allocation_event_log_t log{events.data (), events.size ()};
        fw::tests::allocation_event_log = &log;
        std::optional<fw::result_t<std::size_t>> calibrated;
        const auto measurement =
          fw::tests::measure_allocations ([&] { calibrated.emplace (dispatch ()); });
        fw::tests::allocation_event_log = nullptr;
        ASSERT_TRUE (*calibrated);
        ASSERT_EQ (calibrated->value (), 1u);
        ASSERT_FALSE (log.overflow);
        std::size_t frame = 0;
        while (frame < log.count && events[frame].size != measurement.largest)
            ++frame;
        ASSERT_LT (frame + offset, log.count);
        // Calibrate the actual active infrastructure workspace, then fail its
        // frame allocation and the following promise-state allocation separately.
        enqueue ();
        fw::tests::allocations_before_failure = frame + offset;
        const auto failed = dispatch ();
        const bool injected = !fw::tests::allocations_before_failure.has_value ();
        fw::tests::allocations_before_failure.reset ();
        ASSERT_TRUE (injected);
        EXPECT_FALSE (failed);
        enqueue ();
        auto &mailbox = runtime->transport ().mailbox ();
        auto next =
          mailbox.try_claim_owner (mesh::service_mailbox_domain_t::infrastructure, "control", 1,
                                   std::numeric_limits<std::size_t>::max ());
        ASSERT_TRUE (next.has_value ()) << "allocation failure retained the previous claim";
        EXPECT_TRUE (mailbox.release (*next));
        runtime->close ();
    }
}
TEST (ReceiveAllocations, IdleSpotManagementOwnsOnlyItsReadyResult)
{
    using next_t = std::optional<std::chrono::steady_clock::time_point>;
    auto state = fw::test::runtime_failure_fixture<fw::detail::spot_node_builder_state_t> (
      "receive-management");
    fw::detail::spot_node_runtime_t runtime (state);
    ASSERT_FALSE (runtime.advance_management_async (false).result ().value ());
    for (const bool include_next : {false, true}) {
        std::optional<fw::result_t<next_t>> expected;
        const auto baseline = fw::tests::measure_allocations ([&] {
            fw::task_t<next_t> ready (fw::result_t<next_t>::success (std::nullopt));
            expected.emplace (ready.result ());
        });
        std::optional<fw::result_t<next_t>> result;
        const auto actual = fw::tests::measure_allocations ([&] {
            auto pending = runtime.advance_management_async (include_next);
            EXPECT_TRUE (pending.await_ready ());
            result.emplace (pending.result ());
        });
        ASSERT_TRUE (*result);
        EXPECT_EQ (result->value (), expected->value ());
        EXPECT_EQ (actual.count, baseline.count);
        EXPECT_EQ (actual.bytes, baseline.bytes);
        std::printf ("idle management next=%d actual=%zu/%zu ready=%zu/%zu\n", include_next,
                     actual.count, actual.bytes, baseline.count, baseline.bytes);
    }
}

TEST (ReceiveAllocations, PendingManagementKeepsTemporaryOwnerState)
{
    auto state = fw::test::runtime_failure_fixture<fw::detail::spot_node_builder_state_t> (
      "pending-management");
    std::promise<void> entered, release;
    auto released = release.get_future ();
    std::thread blocker ([&] {
        state->lane
          .run ([&] {
              entered.set_value ();
              released.wait ();
          })
          .get ();
    });
    entered.get_future ().get ();
    // The temporary facade disappears while the state-lane snapshot is pending.
    auto pending = fw::detail::spot_node_runtime_t (state).next_management_activity_async ();
    const bool suspended = !pending.await_ready ();
    release.set_value ();
    blocker.join ();
    const auto result = pending.result ();
    EXPECT_TRUE (suspended);
    ASSERT_TRUE (result);
    EXPECT_FALSE (result.value ());
}
} // namespace
