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
