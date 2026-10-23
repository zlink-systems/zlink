/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "allocation_counter.hpp"
#include "runtime/stateful/public_host_runtime.hpp"
#include "runtime/stateful/raw_stateful_dispatch.hpp"
#include "runtime/actors/actor_ref_access.hpp"
#include <algorithm>
#include <iterator>
#include <tuple>
#include <array>
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/live_location_reader.hpp"
#include "runtime/locations/actor_authority_payload.hpp"
#include "runtime/execution/serial_application_record.hpp"
#include "runtime/execution/serial_execution_queue.hpp"
#include <gtest/gtest.h>
#include <atomic>
#include <cstdio>
#include <thread>

namespace zlink::framework::runtime::host
{
struct public_host_runtime_test_access_t
{
    static stateful::raw_stateful_dispatch_t &stateful_dispatch (public_host_runtime_t &host)
    {
        return *host._stateful_dispatch;
    }
    // A retained source returns on the first line, so the call allocates the
    // coroutine frame of dispatch_native_spot on this ABI, the largest of its
    // allocations, and nothing of the record path.
    static std::size_t native_spot_frame_size (public_host_runtime_t &host,
                                               const stateful::object_ref_t &object)
    {
        const mesh::service_mailbox_t::application_dispatch_t dispatch;
        const auto frame = ::zlink::framework::tests::measure_allocations ([&] {
            auto native = host.dispatch_native_spot (
              object, {}, {}, nullptr, stateful::retained_relocation_source_t{}, dispatch);
            EXPECT_TRUE (native.await_ready ());
            EXPECT_FALSE (native.result ().value ());
        });
        return frame.largest;
    }
};
} // namespace zlink::framework::runtime::host

namespace
{
namespace fw = zlink::framework;
namespace host = fw::runtime::host;

std::shared_ptr<host::public_host_runtime_t> make_host ()
{
    auto runtime = std::make_shared<host::public_host_runtime_t> (host::host_options_t{
      .mesh = {.descriptor = {.mesh_name = "local-allocation",
                              .node_routing_id = {'n'},
                              .lifecycle_generation = 1,
                              .descriptor_revision = 1,
                              .advertised_endpoint = "tcp://127.0.0.1:7182"},
               .runtime_failures = std::make_shared<fw::runtime::runtime_failure_collector_t> ()},
      .object_stable_types = {"framework.spot", "player"}});
    runtime->configure_spot_route_fence_resolver (
      [] (auto, auto, auto) -> fw::task_t<std::optional<host::route_fence_t>> {
          return fw::task_t<std::optional<host::route_fence_t>> (
            fw::result_t<std::optional<host::route_fence_t>>::success (host::route_fence_t{1, 1}));
      });
    return runtime;
}

fw::tests::allocation_measurement_t caller_baseline (host::actor_handle_t &actor,
                                                     unsigned scenario,
                                                     const std::vector<zlink::message_t> &parts)
{
    // Calibrate the unchanged public caller frames without entering local enqueue.
    const auto remote = host::public_host_runtime_t::remote_actor_ref (
      zlink::routing_id_t::from ("remote"), "missing", 1);
    host::spot_handle_t invalid;
    host::pending_operation_t operation;
    return fw::tests::measure_allocations ([&] {
        auto pending = [&] {
            switch (scenario) {
                case 0:
                    return actor.send_to (remote, parts);
                case 1:
                    return actor.request_to (remote, parts, operation, zlink::send_flags_t::none,
                                             std::chrono::seconds (30));
                case 2:
                    return invalid.send_to_spot (zlink::routing_id_t::from ("n"), "missing", 1,
                                                 parts);
                default:
                    return invalid.request_to_spot (zlink::routing_id_t::from ("n"), "missing", 1,
                                                    parts, operation, zlink::send_flags_t::none,
                                                    std::chrono::seconds (30));
            }
        }();
        EXPECT_EQ (pending.result ().value (), scenario < 2
                                                 ? zlink::submit_result_t::not_connected
                                                 : zlink::submit_result_t::invalid_handle);
    });
}

TEST (LocalEnqueueAllocations, MissingLocalTargetsDoNotAllocateDispatchRecordStorage)
{
    auto runtime = make_host ();
    runtime->start ();
    auto spot = runtime->get_or_create_spot ("source");
    auto actor = runtime->create_actor ("player", "source");
    const auto absent = fw::detail::actor_ref_access_t::make (fw::node_rid_t::from_string ("n"),
                                                              "player", "missing", 1);
    const auto local = runtime->status ().routing_id ();
    std::vector<zlink::message_t> parts;
    parts.push_back (zlink::message_t::from ("payload"));
    for (unsigned scenario = 0; scenario != 3; ++scenario) {
        host::pending_operation_t operation;
        auto submit = [&] {
            switch (scenario) {
                case 0:
                    return actor.send_to (absent, parts);
                case 1:
                    return spot.send_to_spot (local, "missing", 1, parts);
                default:
                    return spot.request_to_spot (local, "missing", 1, parts, operation,
                                                 zlink::send_flags_t::none,
                                                 std::chrono::seconds (30));
            }
        };
        ASSERT_EQ (submit ().result ().value (), zlink::submit_result_t::not_found);
        zlink::submit_result_t submitted = zlink::submit_result_t::ok;
        const auto caller = caller_baseline (actor, scenario == 0 ? 0 : scenario + 1, parts);
        const auto measured = fw::tests::measure_allocations ([&] {
            auto pending = submit ();
            EXPECT_TRUE (pending.await_ready ());
            submitted = pending.result ().value ();
        });
        EXPECT_EQ (submitted, zlink::submit_result_t::not_found);
        // A rejected target never needs the storage for an application record.
        // sizeof follows the compiler/ABI instead of fixing a platform byte count.
        EXPECT_LE (measured.largest,
                   std::max (caller.largest,
                             sizeof (host::receive_record_t) + sizeof (host::ready_record_t) - 1));
        std::printf ("local enqueue %u: allocations=%zu bytes=%zu largest=%zu record_storage=%zu\n",
                     scenario, measured.count, measured.bytes, measured.largest,
                     sizeof (host::receive_record_t) + sizeof (host::ready_record_t));
    }
    runtime->close ();
}
TEST (LocalEnqueueAllocations, SuccessfulLocalMessagesOnlyAllocateTheirPublishedRecordStorage)
{
    auto runtime = make_host ();
    runtime->start ();
    auto spot = runtime->get_or_create_spot ("target");
    auto actor = runtime->create_actor ("player", "target");
    const auto local = runtime->status ().routing_id ();
    std::vector<zlink::message_t> parts;
    parts.push_back (zlink::message_t::from ("payload"));
    const auto storage = fw::tests::measure_allocations ([] {
        auto application = std::make_shared<host::local_application_dispatch_t> ();
        auto mailbox = std::make_shared<fw::runtime::mesh::service_mailbox_record_t> ();
        EXPECT_NE (application.get (), nullptr);
        EXPECT_NE (mailbox.get (), nullptr);
    });
    for (unsigned scenario = 0; scenario != 4; ++scenario) {
        host::pending_operation_t operation;
        auto submit = [&] {
            switch (scenario) {
                case 0:
                    return actor.send_to (actor.ref (), parts);
                case 1:
                    return actor.request_to (actor.ref (), parts, operation,
                                             zlink::send_flags_t::none, std::chrono::seconds (30));
                case 2:
                    return spot.send_to_spot (local, "target", 1, parts);
                default:
                    return spot.request_to_spot (local, "target", 1, parts, operation,
                                                 zlink::send_flags_t::none,
                                                 std::chrono::seconds (30));
            }
        };
        auto drain = [&] {
            unsigned handled = 0;
            while (runtime
                     ->dispatch_ready ([&] (const auto &, const auto &record, auto) {
                         ++handled;
                         if (record.reply_token.local_reply)
                             EXPECT_TRUE (record.reply_token.local_reply (parts));
                     })
                     .result ()
                     .value ()
                   != 0) {
            }
            EXPECT_EQ (handled, 1u);
            if (scenario == 1 || scenario == 3) {
                const auto complete =
                  operation.completion->task ().result_for (std::chrono::seconds (5));
                ASSERT_TRUE (complete.has_value ());
                EXPECT_TRUE (*complete);
            }
        };
        ASSERT_EQ (submit ().result ().value (), zlink::submit_result_t::ok);
        drain ();
        const auto caller = caller_baseline (actor, scenario, parts);
        std::array<fw::tests::allocation_event_t, 128> events{};
        fw::tests::allocation_event_log_t log{events.data (), events.size ()};
        const auto previous_log = fw::tests::allocation_event_log;
        fw::tests::allocation_event_log = &log;
        const auto measured = fw::tests::measure_allocations ([&] {
            auto pending = submit ();
            EXPECT_TRUE (pending.await_ready ());
            EXPECT_EQ (pending.result ().value (), zlink::submit_result_t::ok);
        });
        fw::tests::allocation_event_log = previous_log;
        ASSERT_FALSE (log.overflow);
        std::size_t frame_allocations = 0, frame_bytes = 0;
        for (std::size_t index = 0; index != log.count; ++index)
            if (events[index].size > std::max (storage.largest, caller.largest)) {
                ++frame_allocations;
                frame_bytes += events[index].size;
            }
        EXPECT_EQ (frame_allocations, 0u);
        std::printf ("successful local enqueue %u: allocations=%zu bytes=%zu largest=%zu "
                     "record_storage=%zu caller_storage=%zu frame_allocs=%zu frame_bytes=%zu\n",
                     scenario, measured.count, measured.bytes, measured.largest, storage.largest,
                     caller.largest, frame_allocations, frame_bytes);
        drain ();
    }
    runtime->close ();
}
// A Ready Store authority for one native Spot, as the stateful drain fixture.
struct stateful_spot_fixture_t
{
    std::shared_ptr<fw::runtime::in_memory_location_repository_t> store =
      std::make_shared<fw::runtime::in_memory_location_repository_t> ();
    fw::runtime::live_location_reader_t reader{*store};
    fw::runtime::offload_executor_t worker{1, "local-spot-owner"};
    std::shared_ptr<host::public_host_runtime_t> runtime;
    fw::runtime::stateful::object_ref_t object;
    zlink::routing_id_t rid = zlink::routing_id_t::from ("n");
    // Every gate lookup of the drain: one precedes each gate admission.
    std::shared_ptr<std::atomic_size_t> gate_lookups = std::make_shared<std::atomic_size_t> (0);

    explicit stateful_spot_fixture_t (fw::runtime::serial_lane_policy_t policy)
    {
        using namespace fw;
        using namespace fw::runtime;
        const auto lease = std::make_shared<const location_owner_token_t> (
          std::get<owner_lease_claimed_t> (
            store->claim_owner_lease ("local-owner", std::chrono::seconds (60)).result ().value ())
            .token);
        mesh_node_descriptor_t descriptor;
        descriptor.mesh_name = "local-allocation";
        descriptor.rid = rid;
        descriptor.lifecycle_generation = 1;
        descriptor.descriptor_revision = 1;
        descriptor.endpoint = "tcp://127.0.0.1:7188";
        descriptor.owner_id = lease->owner_id;
        descriptor.lease_generation = lease->lease_generation;
        descriptor.object_role = object_role_t::server;
        descriptor.state = framework_runtime_state_t::serving;
        descriptor.application_version = 1;
        descriptor.security_identity = "n";
        descriptor.activation_concurrency.limit = 1;
        descriptor.entry_spot_id = "entry";
        descriptor.object_capabilities.push_back ({placement_object_kind_t::user_spot,
                                                   "framework.spot",
                                                   maintenance_policy_kind_t::disabled, false, 0});
        descriptor.capacity.spots.limit = 1;
        descriptor.capacity.spot_types.push_back (
          {placement_object_kind_t::user_spot, "framework.spot", {1, 0, 0}});
        EXPECT_EQ (store->update_mesh_node (descriptor, location_write_intent_t::new_claim)
                     .result ()
                     .value ()
                     .status,
                   location_write_status_t::stored);
        object_reserve_request_t request;
        request.key = {placement_object_kind_t::user_spot, "stateful"};
        request.intent.stable_type = "framework.spot";
        request.target = {descriptor.mesh_name, node_rid_t::from_string ("n"), 1, *lease};
        request.creating_payload = {std::byte{'c'}};
        request.capacity_bundle = {
          0, 1,
          spot_type_capacity_delta_t{placement_object_kind_t::user_spot, "framework.spot", 1}};
        request.relocation_policy = detail::factory_relocation_kind_t::disabled;
        const auto reserved = store->reserve (request).result ().value ();
        EXPECT_TRUE (std::holds_alternative<object_reserved_t> (reserved));
        const auto fence = std::get<object_reserved_t> (reserved).fence;
        const auto ready = encode_user_spot_authority_payload (user_spot_authority_payload_t{
          .state = user_spot_authority_state_t::ready,
          .stable_type = "framework.spot",
          .spot_id = "stateful",
          .owner_id = lease->owner_id,
          .owner_lease_generation = static_cast<std::uint64_t> (lease->lease_generation),
          .mesh_name = descriptor.mesh_name,
          .node_rid = node_rid_t::from_string ("n"),
          .node_generation = 1});
        EXPECT_TRUE (std::holds_alternative<object_committed_t> (
          store->commit ({request.key, fence, ready}).result ().value ()));
        runtime = make_host ();
        runtime->configure_spot_route_fence_resolver ({});
        runtime->configure_user_spot_operations (store,
                                                 [] (const auto &, const auto &, const auto &) {
                                                     return host::user_spot_materialize_result_t{};
                                                 });
        runtime->configure_stateful_dispatch ([lease] { return lease; }, &reader);
        auto gate = std::make_shared<serial_execution_queue_t> (
          worker, serial_execution_queue_options_t{}, serial_execution_queue_t::error_handler_t{},
          std::move (policy));
        runtime->configure_relocation_gate ([gate, lookups = gate_lookups] (const auto &) {
            ++*lookups;
            return task_t<std::shared_ptr<serial_execution_queue_t>> (
              result_t<std::shared_ptr<serial_execution_queue_t>>::success (gate));
        });
        object = {stateful::object_kind_t::user_spot, "stateful",           fence.object_generation,
                  fence.authority_owner_generation,   descriptor.mesh_name, "n"};
        const auto created =
          runtime->objects ().begin_reserved_object (object, "framework.spot", {});
        EXPECT_EQ (created.error, stateful::stateful_error_t::none);
        EXPECT_EQ (runtime->objects ().commit_create (created.attempt),
                   stateful::stateful_error_t::none);
        runtime->start ();
    }

    ~stateful_spot_fixture_t ()
    {
        runtime->close ();
        worker.drain ();
    }
};

TEST (LocalEnqueueAllocations, StatefulSpotSendAndRequestDrainWithStoreAuthority)
{
    using namespace fw;
    using namespace fw::runtime;
    stateful_spot_fixture_t fixture (serial_lane_policy_t::spot_wide ());
    auto &runtime = fixture.runtime;
    const auto &object = fixture.object;
    auto spot = runtime->get_or_create_spot ("stateful");
    std::vector<zlink::message_t> parts;
    parts.push_back (zlink::message_t::from ("payload"));
    for (const bool request_message : {false, true}) {
        std::size_t drain_count = 0, drain_bytes = 0;
        std::chrono::nanoseconds drain_time{};
        for (unsigned iteration = 0; iteration != 1000; ++iteration) {
            host::pending_operation_t operation;
            const auto submitted =
              request_message
                ? spot
                    .request_to_spot (fixture.rid, "stateful", object.object_generation, parts,
                                      operation, zlink::send_flags_t::none,
                                      std::chrono::seconds (30))
                    .result ()
                    .value ()
                : spot.send_to_spot (fixture.rid, "stateful", object.object_generation, parts)
                    .result ()
                    .value ();
            ASSERT_EQ (submitted, zlink::submit_result_t::ok);
            unsigned handled = 0;
            std::optional<result_t<std::size_t>> drained;
            const auto before_drain = std::chrono::steady_clock::now ();
            const auto cost = tests::measure_allocations ([&] {
                drained = runtime
                            ->dispatch_ready ([&] (const auto &, const auto &record, auto) {
                                ++handled;
                                EXPECT_NE (record.application_record, nullptr);
                                if (record.reply_token.local_reply)
                                    EXPECT_TRUE (record.reply_token.local_reply (parts));
                                if (record.application_record)
                                    record.application_record->release_native_claim ();
                            })
                            .result_for (std::chrono::seconds (5));
            });
            drain_time += std::chrono::steady_clock::now () - before_drain;
            drain_count += cost.count;
            drain_bytes += cost.bytes;
            ASSERT_TRUE (drained.has_value ());
            ASSERT_TRUE (*drained);
            EXPECT_EQ (handled, 1u);
            while (
              runtime->dispatch_ready ([] (const auto &, const auto &, auto) {}).result ().value ()
              != 0) {
            }
            if (request_message) {
                const auto complete =
                  operation.completion->task ().result_for (std::chrono::seconds (5));
                ASSERT_TRUE (complete.has_value ());
                EXPECT_TRUE (*complete);
            }
        }
        std::printf ("stateful Spot %s drain: allocations/op=%.3f bytes/op=%.3f ns/op=%.3f\n",
                     request_message ? "request" : "send", drain_count / 1000.0,
                     drain_bytes / 1000.0, drain_time.count () / 1000.0);
    }
}
TEST (LocalEnqueueAllocations, StatefulOwnerClaimWithoutQueuedTurnNeedsNoContinuationFrame)
{
    using namespace fw::runtime;
    using delivery_result_t = stateful::raw_stateful_dispatch_t::owner_delivery_t;
    stateful_spot_fixture_t fixture (serial_lane_policy_t::spot_wide ());
    auto &objects = fixture.runtime->objects ();
    auto &dispatch = host::public_host_runtime_test_access_t::stateful_dispatch (*fixture.runtime);
    // A ready claim stores at most its delivery result; a continuation frame holds that
    // result and every local live across its awaits, so it is larger on any ABI.
    const auto result_storage = fw::tests::measure_allocations ([] {
        const fw::task_t<delivery_result_t> ready (fw::result_t<delivery_result_t>::success ({}));
        EXPECT_TRUE (ready.await_ready ());
    });
    const auto claim_object = [&] {
        auto claim = objects.try_claim_async (fixture.object, stateful::turn_domain_t::application);
        ASSERT_TRUE (claim.await_ready ());
        EXPECT_EQ (claim.result ().value ().error, stateful::stateful_error_t::none);
        EXPECT_FALSE (claim.result ().value ().turn.has_value ());
    };
    const auto claim_dispatch = [&] {
        auto claim = dispatch.try_claim_async (fixture.object);
        ASSERT_TRUE (claim.await_ready ());
        EXPECT_EQ (claim.result ().value ().error, stateful::stateful_error_t::none);
        EXPECT_FALSE (claim.result ().value ().delivery.has_value ());
    };
    claim_object ();
    claim_dispatch ();
    const auto object_cost = fw::tests::measure_allocations (claim_object);
    const auto dispatch_cost = fw::tests::measure_allocations (claim_dispatch);
    std::printf ("empty owner claim: object allocations=%zu bytes=%zu largest=%zu; "
                 "dispatch allocations=%zu bytes=%zu largest=%zu; result_storage=%zu\n",
                 object_cost.count, object_cost.bytes, object_cost.largest, dispatch_cost.count,
                 dispatch_cost.bytes, dispatch_cost.largest, result_storage.largest);
    EXPECT_LE (object_cost.largest, result_storage.largest);
    EXPECT_LE (dispatch_cost.largest, result_storage.largest);
}

TEST (LocalEnqueueAllocations, RecordedAuthorityUsesOnlyReadyResultStorage)
{
    using namespace fw;
    using namespace fw::runtime;
    using authority_t = std::optional<stateful::accepted_record_authority_t>;
    auto runtime = make_host ();
    const auto lease =
      std::make_shared<const location_owner_token_t> (location_owner_token_t{"local-owner", 3});
    stateful::accepted_record_authority_query_t query;
    query.target = {stateful::object_kind_t::user_spot, "spot", 1, 1, "local-allocation", "n"};
    query.source_node_routing_id = {'n'};
    query.source_node_generation = 1;
    query.target_node_routing_id = {'n'};
    query.target_node_generation = 1;
    query.source_original = mesh::local_request_source_t{lease, {}};
    const authority_t expected =
      stateful::accepted_record_authority_t{{lease->owner_id, 3, {'n'}, 1}, 3};
    const auto storage = tests::measure_allocations ([&] {
        const task_t<authority_t> result (result_t<authority_t>::success (expected));
        EXPECT_TRUE (result.await_ready ());
    });
    const auto input = tests::measure_allocations ([&] {
        const auto copy = query;
        EXPECT_EQ (copy.target, query.target);
    });
    const auto resolve = [&] {
        auto result = runtime->accepted_record_authority (query, lease);
        ASSERT_TRUE (result.await_ready ());
        const auto &authority = result.result ().value ();
        ASSERT_TRUE (authority.has_value ());
        EXPECT_EQ (authority->source.owner_id, lease->owner_id);
        EXPECT_EQ (authority->source.node_routing_id, query.source_node_routing_id);
        EXPECT_EQ (authority->target_owner_lease_generation, 3u);
    };
    resolve ();
    const auto measured = tests::measure_allocations (resolve);
    std::printf ("recorded authority: allocations=%zu bytes=%zu largest=%zu; ready storage=%zu\n",
                 measured.count, measured.bytes, measured.largest, storage.largest);
    EXPECT_LE (measured.largest, storage.largest);
    EXPECT_LE (measured.count, storage.count + input.count);
    EXPECT_LE (measured.bytes, storage.bytes + input.bytes);
}

TEST (LocalEnqueueAllocations, ReadyAuthorityResolverAddsOnlyItsLaneResult)
{
    using namespace fw;
    using namespace fw::runtime;
    using authority_t = std::optional<stateful::accepted_record_authority_t>;
    auto runtime = make_host ();
    const stateful::object_ref_t object{
      stateful::object_kind_t::user_spot, "spot", 1, 1, "local-allocation", "n"};
    protocol::spot_message_header_t header;
    header.target = {object.key, 1, {'n'}, 1, 1, 3};
    unsigned calls = 0;
    const stateful::accepted_record_authority_resolver_t resolver = [&] (const auto &) {
        ++calls;
        return task_t<authority_t> (result_t<authority_t>::success (
          stateful::accepted_record_authority_t{{"source", 2, {'s'}, 1}, 3}));
    };
    runtime->configure_stateful_dispatch (resolver);
    stateful::raw_stateful_dispatch_t direct (runtime->objects (), runtime->transport (), resolver);
    auto &wrapped = host::public_host_runtime_test_access_t::stateful_dispatch (*runtime);
    const auto resolve = [&] (stateful::raw_stateful_dispatch_t &dispatch) {
        const auto previous_calls = calls;
        auto result =
          dispatch.accepted_spot_authority (object, header, {'s'}, 1, std::nullopt, std::nullopt);
        ASSERT_TRUE (result.await_ready ());
        const auto &authority = result.result ().value ();
        EXPECT_EQ (authority.first, stateful::stateful_error_t::none);
        ASSERT_TRUE (authority.second.has_value ());
        EXPECT_EQ (authority.second->source.owner_id, "source");
        EXPECT_EQ (authority.second->target_owner_lease_generation, 3u);
        EXPECT_EQ (calls - previous_calls, 1u);
    };
    resolve (direct);
    resolve (wrapped);
    const auto baseline = tests::measure_allocations ([&] { resolve (direct); });
    const auto lane_result = tests::measure_allocations ([] {
        const task_t<authority_t> result (result_t<authority_t>::success (std::nullopt));
        EXPECT_TRUE (result.await_ready ());
    });
    const auto measured = tests::measure_allocations ([&] { resolve (wrapped); });
    std::printf ("authority resolver: direct=%zu/%zu lane=%zu/%zu wrapped=%zu/%zu\n",
                 baseline.count, baseline.bytes, lane_result.count, lane_result.bytes,
                 measured.count, measured.bytes);
    EXPECT_LE (measured.count, baseline.count + lane_result.count);
    EXPECT_LE (measured.bytes, baseline.bytes + lane_result.bytes);
}

TEST (LocalEnqueueAllocations, PendingAuthorityResolverKeepsItsInputAndErrorClassification)
{
    using namespace fw;
    using namespace fw::runtime;
    using authority_t = std::optional<stateful::accepted_record_authority_t>;
    for (unsigned outcome = 0; outcome != 4; ++outcome) {
        auto runtime = make_host ();
        task_completion_source_t<authority_t> completion;
        unsigned calls = 0;
        const stateful::accepted_record_authority_query_t *observed = nullptr;
        runtime->configure_stateful_dispatch (
          [&] (const stateful::accepted_record_authority_query_t &query) {
              ++calls;
              observed = &query;
              return completion.task ();
          });
        auto &dispatch = host::public_host_runtime_test_access_t::stateful_dispatch (*runtime);
        // The original arguments are gone while the configured resolver still awaits.
        auto pending = [&] {
            const stateful::object_ref_t object{stateful::object_kind_t::user_spot,
                                                "spot-with-owned-query-storage",
                                                1,
                                                1,
                                                "local-allocation",
                                                "n"};
            protocol::spot_message_header_t header;
            header.target = {object.key, 1, {'n'}, 1, 1, 3};
            return dispatch.accepted_spot_authority (object, header, {'s'}, 1, std::nullopt,
                                                     std::nullopt);
        }();
        ASSERT_FALSE (pending.await_ready ());
        ASSERT_EQ (calls, 1u);
        ASSERT_NE (observed, nullptr);
        EXPECT_EQ (observed->target.key, "spot-with-owned-query-storage");
        EXPECT_EQ (observed->source_node_routing_id, std::vector<std::uint8_t>{'s'});
        if (outcome == 0)
            completion.complete (result_t<authority_t>::success (
              stateful::accepted_record_authority_t{{"source", 2, {'s'}, 1}, 3}));
        else if (outcome == 3)
            completion.complete (detail::result_access_t::failure<authority_t> (
              std::make_exception_ptr (detail::make_boundary_exception (
                detail::boundary_error_t::stale_generation, "pending stale generation"))));
        else
            completion.complete (result_t<authority_t>::failure (
              outcome == 1 ? framework_error_kind_t::unavailable
                           : framework_error_kind_t::internal_failure,
              "pending authority failure"));
        const auto terminal = pending.result_for (std::chrono::seconds (5));
        ASSERT_TRUE (terminal.has_value ());
        if (outcome == 2) {
            EXPECT_FALSE (*terminal);
            EXPECT_EQ (terminal->error_kind (), framework_error_kind_t::internal_failure);
        } else {
            ASSERT_TRUE (*terminal);
            EXPECT_EQ (terminal->value ().first, outcome == 0 ? stateful::stateful_error_t::none
                                                 : outcome == 3
                                                   ? stateful::stateful_error_t::conflict
                                                   : stateful::stateful_error_t::unavailable);
            EXPECT_EQ (terminal->value ().second.has_value (), outcome == 0);
        }
        EXPECT_EQ (calls, 1u);
    }
}

TEST (LocalEnqueueAllocations, PendingRecordedSourceKeepsItsInputAndFailure)
{
    using namespace fw;
    using namespace fw::runtime;
    using descriptor_t = std::shared_ptr<const mesh_node_descriptor_t>;
    for (const bool fail : {false, true}) {
        auto runtime = make_host ();
        const auto lookup = std::make_shared<mesh::request_source_lookup_state_t> ();
        unsigned starts = 0;
        lookup->start = [&] (const auto &) { ++starts; };
        auto pending = [&] {
            stateful::accepted_record_authority_query_t query;
            query.target = {
              stateful::object_kind_t::user_spot, "spot", 1, 1, "local-allocation", "n"};
            query.target_node_routing_id = {'n'};
            query.target_node_generation = 1;
            query.source_node_routing_id = {'s'};
            query.source_node_generation = 7;
            query.source_original = lookup;
            return runtime->accepted_record_authority (
              std::move (query),
              std::make_shared<const location_owner_token_t> (location_owner_token_t{"target", 3}));
        }();
        ASSERT_FALSE (pending.await_ready ());
        EXPECT_EQ (starts, 1u);
        if (fail)
            lookup->completion.complete (result_t<descriptor_t>::failure (
              framework_error_kind_t::unavailable, "pending source failure"));
        else {
            auto source = std::make_shared<mesh_node_descriptor_t> ();
            source->owner_id = "source";
            source->lease_generation = 2;
            lookup->completion.complete (result_t<descriptor_t>::success (std::move (source)));
        }
        const auto terminal = pending.result_for (std::chrono::seconds (5));
        ASSERT_TRUE (terminal.has_value ());
        if (fail) {
            EXPECT_FALSE (*terminal);
            EXPECT_EQ (terminal->error_kind (), framework_error_kind_t::unavailable);
        } else {
            ASSERT_TRUE (*terminal);
            ASSERT_TRUE (terminal->value ().has_value ());
            EXPECT_EQ (terminal->value ()->source.owner_id, "source");
            EXPECT_EQ (terminal->value ()->source.node_routing_id, std::vector<std::uint8_t>{'s'});
            EXPECT_EQ (terminal->value ()->source.node_generation, 7u);
            EXPECT_EQ (terminal->value ()->target_owner_lease_generation, 3u);
        }
        EXPECT_EQ (starts, 1u);
    }
}

TEST (LocalEnqueueAllocations, IdleActorNotificationAddsOnlyMailboxAndClaimStorage)
{
    using namespace fw;
    using namespace fw::runtime;
    stateful_spot_fixture_t fixture (serial_lane_policy_t::spot_wide ());
    auto &runtime = fixture.runtime;
    auto &objects = runtime->objects ();
    auto &dispatch = host::public_host_runtime_test_access_t::stateful_dispatch (*runtime);
    const auto idle = [&] {
        auto pending = runtime->dispatch_ready ([] (const auto &, const auto &, auto) {});
        ASSERT_TRUE (pending.await_ready ());
        EXPECT_EQ (pending.result ().value (), 0u);
    };
    const auto inventory = [&] {
        auto pending = objects.inventory_async ();
        ASSERT_TRUE (pending.await_ready ());
        const auto owners = pending.await_resume ();
        EXPECT_FALSE (owners.empty ());
    };
    idle ();
    inventory ();
    const auto empty = tests::measure_allocations (idle);
    const auto inventory_before = tests::measure_allocations (inventory);
    auto actor = runtime->create_actor ("player", "actor");
    const auto owners = objects.inventory_async ().result ().value ();
    const auto found = std::find_if (owners.begin (), owners.end (), [] (const auto &owner) {
        return owner.owner.kind == stateful::object_kind_t::actor && owner.owner.key == "actor";
    });
    ASSERT_NE (found, owners.end ());
    const auto object = found->owner;
    idle ();
    inventory ();
    const auto inventory_after = tests::measure_allocations (inventory);
    const auto measured = tests::measure_allocations (idle);
    // Calibrate the owning APIs and the callback's captured input on this ABI.
    // Forwarding their ready result must not add another coroutine or result state.
    const auto claim = tests::measure_allocations ([&] {
        std::function<void ()> before;
        std::string stable_type;
        const auto result = dispatch.try_claim (object, &before, &stable_type);
        EXPECT_EQ (result.first, stateful::stateful_error_t::none);
        EXPECT_FALSE (result.second.has_value ());
    });
    const auto notification = tests::measure_allocations ([&] {
        auto pending = runtime->transport ().mailbox ().notify_stateful_ready (
          object.key, [weak = runtime->weak_from_this (), object] (const auto &callback) {
              return weak.lock () && !object.key.empty () && static_cast<bool> (callback);
          });
        ASSERT_TRUE (pending.await_ready ());
        EXPECT_FALSE (pending.result ().value ());
    });
    const auto result = tests::measure_allocations ([] {
        const task_t<bool> ready (result_t<bool>::success (false));
        EXPECT_TRUE (ready.await_ready ());
    });
    const auto expected_count =
      empty.count + inventory_after.count + claim.count + notification.count + result.count;
    const auto expected_bytes =
      empty.bytes + inventory_after.bytes + claim.bytes + notification.bytes + result.bytes;
    std::printf ("idle Actor notification: actual=%zu/%zu expected=%zu/%zu; "
                 "inventory delta=%zu/%zu claim=%zu/%zu mailbox=%zu/%zu result=%zu/%zu\n",
                 measured.count, measured.bytes, expected_count - inventory_before.count,
                 expected_bytes - inventory_before.bytes,
                 inventory_after.count - inventory_before.count,
                 inventory_after.bytes - inventory_before.bytes, claim.count, claim.bytes,
                 notification.count, notification.bytes, result.count, result.bytes);
    EXPECT_LE (measured.count + inventory_before.count, expected_count);
    EXPECT_LE (measured.bytes + inventory_before.bytes, expected_bytes);
}

TEST (LocalEnqueueAllocations, IdleManagementDoesNotAllocateInfrastructureBatchStorage)
{
    using namespace fw;
    using namespace fw::runtime;
    auto runtime = make_host ();
    std::array<unsigned, 8> seen{};
    std::size_t seen_count = 0;
    runtime->configure_message_follow_handler ([&] (const auto &notice) {
        ASSERT_LT (seen_count, seen.size ());
        seen[seen_count++] = notice.queued_messages;
    });
    const auto enqueue = [&] (unsigned sequence) {
        const protocol::spot_route_fence_t source{"source", 1, {'n'}, 1, 1, 1};
        const protocol::spot_route_fence_t target{"target", 1, {'n'}, 1, 1, 1};
        const protocol::message_follow_notice_t notice{source, target,        1, sequence,
                                                       1,      {1, sequence}, 0};
        mesh::service_mailbox_record_t record;
        record.domain = mesh::service_mailbox_domain_t::infrastructure;
        record.owner = "management";
        record.source_routing_id = {'n'};
        record.source_node_generation = 1;
        record.parts.push_back (zlink::message_t::from (protocol::encode_message_follow (notice)));
        ASSERT_TRUE (runtime->transport ().mailbox ().try_enqueue (std::move (record)));
    };
    std::array<tests::allocation_event_t, 256> events{};
    const auto drain = [&] (std::size_t expected_records) {
        tests::allocation_event_log_t log{events.data (), events.size ()};
        const auto previous = tests::allocation_event_log;
        tests::allocation_event_log = &log;
        const auto measured = tests::measure_allocations ([&] {
            auto task = runtime->dispatch_ready ([] (const auto &, const auto &, auto) {}, false);
            ASSERT_TRUE (task.await_ready ());
            EXPECT_EQ (task.result ().value (), expected_records);
        });
        tests::allocation_event_log = previous;
        EXPECT_FALSE (log.overflow);
        const auto largest_count =
          std::count_if (events.begin (), events.begin () + log.count,
                         [&] (const auto &event) { return event.size == measured.largest; });
        EXPECT_EQ (runtime->transport ().mailbox ().pending_messages (
                     mesh::service_mailbox_domain_t::infrastructure),
                   0u);
        return std::pair{measured, largest_count};
    };
    (void) drain (0);
    enqueue (1);
    (void) drain (1);
    seen_count = 0;
    const auto [idle, idle_largest_count] = drain (0);
    enqueue (2);
    const auto [single, single_largest_count] = drain (1);
    enqueue (3);
    enqueue (4);
    const auto [multiple, multiple_largest_count] = drain (2);
    ASSERT_EQ (seen_count, 3u);
    EXPECT_EQ (seen[0], 2u);
    EXPECT_EQ (seen[1], 3u);
    EXPECT_EQ (seen[2], 4u);
    std::printf ("management allocation count/bytes/largest: idle=%zu/%zu/%zu "
                 "one record=%zu/%zu/%zu two records=%zu/%zu/%zu; largest counts=%zu/%zu/%zu\n",
                 idle.count, idle.bytes, idle.largest, single.count, single.bytes, single.largest,
                 multiple.count, multiple.bytes, multiple.largest,
                 static_cast<std::size_t> (idle_largest_count),
                 static_cast<std::size_t> (single_largest_count),
                 static_cast<std::size_t> (multiple_largest_count));
    // The actual cold batch supplies the frame-size baseline on this ABI.
    // An empty management turn must not allocate that record-processing storage.
    EXPECT_LT (idle.largest, single.largest);
    EXPECT_LE (idle.bytes + single.largest, single.bytes);
    EXPECT_EQ (single.largest, multiple.largest);
    EXPECT_EQ (single_largest_count, 1);
    EXPECT_EQ (multiple_largest_count, 1);
}

TEST (LocalEnqueueAllocations, StatefulSpotDrainRecordContinuationStaysWithinRecordStorage)
{
    using namespace fw;
    using namespace fw::runtime;
    // A gate whose admission completes in the caller's turn keeps the whole drain on
    // this thread, so every frame of the record path is counted.
    stateful_spot_fixture_t fixture (serial_lane_policy_t::session ());
    auto &runtime = fixture.runtime;
    auto spot = runtime->get_or_create_spot ("stateful");
    std::vector<zlink::message_t> parts;
    parts.push_back (zlink::message_t::from ("payload"));
    unsigned handled = 0;
    std::array<tests::allocation_event_t, 256> events{};
    const auto drain_once = [&] {
        std::size_t count = 0;
        tests::allocation_event_log_t log{events.data (), events.size ()};
        const auto previous_log = tests::allocation_event_log;
        tests::allocation_event_log = &log;
        const auto cost = tests::measure_allocations ([&] {
            auto drained = runtime->dispatch_ready ([&] (const auto &, const auto &record, auto) {
                ++handled;
                if (record.reply_token.local_reply)
                    EXPECT_TRUE (record.reply_token.local_reply (parts));
                if (record.application_record)
                    record.application_record->release_native_claim ();
            });
            ASSERT_TRUE (drained.await_ready ());
            count = drained.result ().value ();
        });
        tests::allocation_event_log = previous_log;
        EXPECT_FALSE (log.overflow);
        std::vector<std::size_t> sizes;
        for (std::size_t index = 0; index != log.count; ++index)
            sizes.push_back (events[index].size);
        std::sort (sizes.begin (), sizes.end ());
        return std::tuple{count, cost, sizes};
    };
    const auto request = [&] {
        host::pending_operation_t operation;
        EXPECT_EQ (spot
                     .request_to_spot (fixture.rid, "stateful", fixture.object.object_generation,
                                       parts, operation, zlink::send_flags_t::none,
                                       std::chrono::seconds (30))
                     .result ()
                     .value (),
                   zlink::submit_result_t::ok);
        return operation;
    };
    {
        auto warm = request ();
        while (std::get<0> (drain_once ()) != 0) {
        }
        ASSERT_TRUE (warm.completion->task ().result_for (std::chrono::seconds (5)).has_value ());
    }
    // The receive turn alone: dispatch_ready with no record pending.
    const auto [idle_count, idle, idle_sizes] = drain_once ();
    EXPECT_EQ (idle_count, 0u);
    handled = 0;
    auto operation = request ();
    const auto [record_count, record, record_sizes] = drain_once ();
    EXPECT_EQ (record_count, 1u);
    EXPECT_EQ (handled, 1u);
    const auto complete = operation.completion->task ().result_for (std::chrono::seconds (5));
    ASSERT_TRUE (complete.has_value ());
    EXPECT_TRUE (*complete);
    // The record's own storage is what the drain allocates beyond the idle receive turn.
    std::vector<std::size_t> extra;
    std::set_difference (record_sizes.begin (), record_sizes.end (), idle_sizes.begin (),
                         idle_sizes.end (), std::back_inserter (extra));
    std::printf ("stateful Spot request drain on caller turn: allocations=%zu bytes=%zu; "
                 "idle allocations=%zu bytes=%zu; record allocations:",
                 record.count, record.bytes, idle.count, idle.bytes);
    for (const auto size : extra)
        std::printf (" %zu", size);
    std::printf ("\n");
    // dispatch_native_spot stays a coroutine because gate admission can wait; its
    // frame size relative to the record storage depends on the ABI, so the bound
    // leaves out that one frame as measured on this ABI.
    const auto native_frame =
      host::public_host_runtime_test_access_t::native_spot_frame_size (*runtime, fixture.object);
    const auto native = std::find (extra.begin (), extra.end (), native_frame);
    ASSERT_NE (native, extra.end ());
    extra.erase (native);
    // Published storage the record needs anyway; a continuation frame that holds
    // a whole record path is larger, so it shows above this bound.
    const auto storage = tests::measure_allocations ([] {
        auto application = std::make_shared<host::local_application_dispatch_t> ();
        auto mailbox = std::make_shared<mesh::service_mailbox_record_t> ();
        auto accepted = std::make_shared<serial_application_record_t> ();
        EXPECT_NE (application.get (), nullptr);
        EXPECT_NE (mailbox.get (), nullptr);
        EXPECT_NE (accepted.get (), nullptr);
    });
    ASSERT_FALSE (extra.empty ());
    std::printf ("record storage=%zu\n", storage.largest);
    EXPECT_LE (extra.back (), storage.largest);
}
TEST (LocalEnqueueAllocations, StatefulSpotDrainAdmitsOnceAndStaysOnIdleGateCaller)
{
    using namespace fw;
    using namespace fw::runtime;
    stateful_spot_fixture_t fixture (serial_lane_policy_t::spot_wide ());
    auto &runtime = fixture.runtime;
    auto spot = runtime->get_or_create_spot ("stateful");
    std::vector<zlink::message_t> parts;
    parts.push_back (zlink::message_t::from ("payload"));
    const auto caller = std::this_thread::get_id ();
    for (unsigned round = 0; round != 3; ++round) {
        host::pending_operation_t operation;
        ASSERT_EQ (spot
                     .request_to_spot (fixture.rid, "stateful", fixture.object.object_generation,
                                       parts, operation, zlink::send_flags_t::none,
                                       std::chrono::seconds (30))
                     .result ()
                     .value (),
                   zlink::submit_result_t::ok);
        const auto lookups_before = fixture.gate_lookups->load ();
        unsigned handled = 0, off_caller = 0;
        auto drained = runtime->dispatch_ready ([&] (const auto &, const auto &record, auto) {
            ++handled;
            if (std::this_thread::get_id () != caller)
                ++off_caller;
            if (record.reply_token.local_reply)
                EXPECT_TRUE (record.reply_token.local_reply (parts));
            if (record.application_record)
                record.application_record->release_native_claim ();
        });
        // An idle gate admits in the receive turn: the drain completes without a
        // continuation on another thread.
        const bool ready = drained.await_ready ();
        const auto count = drained.result_for (std::chrono::seconds (5));
        ASSERT_TRUE (count.has_value ());
        EXPECT_EQ (count->value (), 1u);
        EXPECT_EQ (handled, 1u);
        EXPECT_TRUE (ready);
        EXPECT_EQ (off_caller, 0u);
        // The native publication asks the gate; the owner claim does not ask it again.
        EXPECT_EQ (fixture.gate_lookups->load () - lookups_before, 1u);
        const auto idle_before = fixture.gate_lookups->load ();
        EXPECT_EQ (runtime->dispatch_ready ([] (const auto &, const auto &, auto) {})
                     .result_for (std::chrono::seconds (5))
                     ->value (),
                   0u);
        EXPECT_EQ (fixture.gate_lookups->load (), idle_before);
        const auto complete = operation.completion->task ().result_for (std::chrono::seconds (5));
        ASSERT_TRUE (complete.has_value ());
        EXPECT_TRUE (*complete);
        std::printf ("stateful Spot request drain: gate lookups=%zu idle lookups=%zu ready=%d "
                     "off_caller=%u\n",
                     idle_before - lookups_before, fixture.gate_lookups->load () - idle_before,
                     ready, off_caller);
    }
}
} // namespace
