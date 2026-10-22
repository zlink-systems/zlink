/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "allocation_counter.hpp"
#include "runtime/stateful/public_host_runtime.hpp"
#include "runtime/actors/actor_ref_access.hpp"
#include <algorithm>
#include <array>
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/live_location_reader.hpp"
#include "runtime/locations/actor_authority_payload.hpp"
#include "runtime/execution/serial_application_record.hpp"
#include "runtime/execution/serial_execution_queue.hpp"
#include <gtest/gtest.h>
#include <cstdio>

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
TEST (LocalEnqueueAllocations, StatefulSpotSendAndRequestDrainWithStoreAuthority)
{
    using namespace fw;
    using namespace fw::runtime;
    auto store = std::make_shared<in_memory_location_repository_t> ();
    live_location_reader_t reader (*store);
    const auto lease = std::make_shared<const location_owner_token_t> (
      std::get<owner_lease_claimed_t> (
        store->claim_owner_lease ("local-owner", std::chrono::seconds (60)).result ().value ())
        .token);
    mesh_node_descriptor_t descriptor;
    descriptor.mesh_name = "local-allocation";
    descriptor.rid = zlink::routing_id_t::from ("n");
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
    descriptor.object_capabilities.push_back ({placement_object_kind_t::user_spot, "framework.spot",
                                               maintenance_policy_kind_t::disabled, false, 0});
    descriptor.capacity.spots.limit = 1;
    descriptor.capacity.spot_types.push_back (
      {placement_object_kind_t::user_spot, "framework.spot", {1, 0, 0}});
    ASSERT_EQ (store->update_mesh_node (descriptor, location_write_intent_t::new_claim)
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
      0, 1, spot_type_capacity_delta_t{placement_object_kind_t::user_spot, "framework.spot", 1}};
    request.relocation_policy = detail::factory_relocation_kind_t::disabled;
    const auto reserved = store->reserve (request).result ().value ();
    ASSERT_TRUE (std::holds_alternative<object_reserved_t> (reserved));
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
    ASSERT_TRUE (std::holds_alternative<object_committed_t> (
      store->commit ({request.key, fence, ready}).result ().value ()));
    auto runtime = make_host ();
    runtime->configure_spot_route_fence_resolver ({});
    runtime->configure_user_spot_operations (store, [] (const auto &, const auto &, const auto &) {
        return host::user_spot_materialize_result_t{};
    });
    runtime->configure_stateful_dispatch ([lease] { return lease; }, &reader);
    offload_executor_t worker (1, "local-spot-owner");
    auto gate = std::make_shared<serial_execution_queue_t> (
      worker, serial_execution_queue_options_t{}, serial_execution_queue_t::error_handler_t{},
      serial_lane_policy_t::spot_wide ());
    runtime->configure_relocation_gate ([gate] (const auto &) {
        return task_t<std::shared_ptr<serial_execution_queue_t>> (
          result_t<std::shared_ptr<serial_execution_queue_t>>::success (gate));
    });
    const stateful::object_ref_t object{
      stateful::object_kind_t::user_spot, "stateful",           fence.object_generation,
      fence.authority_owner_generation,   descriptor.mesh_name, "n"};
    const auto created = runtime->objects ().begin_reserved_object (object, "framework.spot", {});
    ASSERT_EQ (created.error, stateful::stateful_error_t::none);
    ASSERT_EQ (runtime->objects ().commit_create (created.attempt),
               stateful::stateful_error_t::none);
    runtime->start ();
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
                    .request_to_spot (descriptor.rid, "stateful", object.object_generation, parts,
                                      operation, zlink::send_flags_t::none,
                                      std::chrono::seconds (30))
                    .result ()
                    .value ()
                : spot.send_to_spot (descriptor.rid, "stateful", object.object_generation, parts)
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
    runtime->close ();
    worker.drain ();
}
} // namespace
