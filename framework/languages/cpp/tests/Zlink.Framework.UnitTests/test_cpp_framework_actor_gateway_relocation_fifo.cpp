/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/actor_gateway_cases.hpp"
#include "../support/actor_gateway_fixture.hpp"
#include "../support/actor_gateway_task_result.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "../support/manual_internal_scheduler.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/protocol/service_wire_codec.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include <zlink/framework/contracts/dispatch/task.hpp>
#include <zlink/framework/contracts/streams/stream.hpp>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <typeindex>
#include <utility>
#include <vector>

namespace zlink::framework::test::actor_gateway
{
// Regression pin for the relocation admission TOCTOU (B-2):
// spot-actor membership :903 ("Defer() 뒤 source seal 전 message는 barrier 뒤
// Actor queue에 두고 capture 대상") and relocation-flow :108/:155.
//
// Configure injects Deferred Join registration between coordinator admission
// and the Actor FIFO post. The native accepted record must remain visible in
// the queue suffix when seal starts; a committed source instead follows it.
// A manual scheduler fixes these boundaries without a timing race.
int relocation_barrier_fences_late_actor_fifo_admission (bool commit_before_post)
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace runtime = zlink::framework::runtime;

    serializer_registry_t serializers;
    auto node = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::spot_node_builder_state_t> ("actor-a");
    auto scheduler = std::make_shared<zlink::framework::test::manual_internal_scheduler_t> ();
    scheduler->publication_fence (zlink::framework::test::state_lane_publication_fence (
      node->lane, *::zlink::framework::runtime::infrastructure_executor ()));
    node->worker_executor = scheduler;
    node->channel_runtime =
      zlink::framework::test::runtime_failure_fixture<channel_runtime_state_t> ();
    node->channel_runtime->serializers = &serializers;

    auto spot = std::make_shared<spot_context_state_t> ();
    spot->node = node;
    spot->node_rid = node_rid_t::from_string ("actor-a");
    spot->spot_id = spot_id_t ("actor-a-entry");
    spot->spot_name = "entry";
    spot->spot_instance = std::make_shared<int> (1);
    spot->channel_runtime =
      zlink::framework::test::runtime_failure_fixture<channel_runtime_state_t> ();
    spot->channel_runtime->serializers = &serializers;
    spot->serial_executor = node->worker_executor;
    node->spot_contexts_by_id.emplace (spot->spot_id, spot_context_access_t::create (spot));

    const auto actor = test_actor_ref ("actor-a", "player", "fence-actor", 1);
    const auto key = std::string ("player:fence-actor");
    spot_node_runtime_t spots (node);

    const runtime::protocol::actor_route_fence_t source_fence{
      "fence-actor", 1, zlink::routing_id_t::from ("actor-a").to_bytes (), 1, 1, 1};
    auto target_fence = source_fence;
    target_fence.target_node_routing_id = zlink::routing_id_t::from ("actor-b").to_bytes ();
    target_fence.authority_owner_generation = 2;
    std::atomic_int follow_calls{0};
    spots.on_actor_message_follow (
      [&] (const actor_ref_t &, const runtime::messaging::envelope_header_t &header,
           const zlink::message_t &payload, std::chrono::milliseconds, const zlink::routing_id_t &,
           const runtime::protocol::actor_route_fence_t &fence, std::uint8_t,
           runtime::protocol::wire_operation_id_t,
           std::uint64_t) -> task_t<std::optional<zlink::message_t>> {
          ++follow_calls;
          if (fence != source_fence || header.message_name != "FencedProbe"
              || header.metadata.at ("sequence") != "LATE" || payload.to_string () != "late")
              co_return result_t<std::optional<zlink::message_t>>::failure (
                framework_error_kind_t::internal_failure, "late packet follow arguments changed");
          co_return result_t<std::optional<zlink::message_t>>::success (zlink::message_t{});
      });
    std::atomic_int configure_calls{0};
    std::shared_ptr<deferred_barrier_t> barrier;
    std::atomic_bool barrier_reserved{false};
    spot_node_builder_state_t::actor_factory_registration_t factory;
    factory.actor_type = std::type_index (typeid (int));
    factory.create_instance = [] (std::string) { return std::make_shared<int> (7); };
    factory.configure_instance = [&] (void *, const actor_ref_t &current, void *) {
        if (configure_calls.fetch_add (1, std::memory_order_acq_rel) != 0)
            return;
        // Registration retains this Actor mailbox. Until seal, accepted
        // application records still belong to the Actor FIFO.
        auto reserved = spots.reserve_actor_join_barrier (current);
        if (!reserved)
            return;
        barrier = std::move (reserved.value ());
        barrier_reserved.store (true, std::memory_order_release);
        if (commit_before_post) {
            node->actor_transfer_coordinator->activate_message_follow (
              key, source_fence, test_actor_ref ("actor-b", "player", "fence-actor", 1),
              spot_route_t{node_rid_t::from_string ("actor-b"), spot_id_t ("target"), "game"},
              target_fence, std::chrono::steady_clock::now () + std::chrono::seconds (30),
              "commit-before-post");
            node->actor_transfer_coordinator->cancel_move (key);
        }
    };
    node->actor_factories.emplace ("player", std::move (factory));

    node->actor_instances.emplace (key, std::make_shared<int> (7));
    node->actor_spot_ids.emplace (key, spot->spot_id);
    node->actor_generations.emplace (key, 1);
    node->actor_types_by_id.emplace ("fence-actor", "player");

    std::atomic_int source_handler_runs{0};
    std::atomic_int admission_notifications{0};
    std::atomic_int owner_transfers{0};
    spot->handlers.push_back (
      spot_handler_descriptor_t{spot_handler_kind_t::actor_send, "FencedProbe", "",
                                std::type_index (typeid (int)), std::type_index (typeid (void)),
                                std::type_index (typeid (int)), std::type_index (typeid (void))});
    spot->handler_invokers.push_back (
      [&source_handler_runs] (void *, void *, service_provider_t &, serializer_registry_t &,
                              const zlink::message_t &,
                              const spot_inbound_message_t &) -> task_t<zlink::message_t> {
          source_handler_runs.fetch_add (1, std::memory_order_acq_rel);
          co_return zlink::message_t{};
      });

    service_collection_t services;
    auto provider = services.build_provider ();
    actor_gateway_runtime_t gateway;

    runtime::messaging::envelope_header_t envelope;
    envelope.kind = runtime::messaging::message_kind_t::command;
    envelope.channel_name = "actor";
    envelope.message_name = "FencedProbe";
    envelope.content_type = "application/json";
    envelope.metadata.emplace ("sequence", "LATE");
    auto late_record =
      commit_before_post ? nullptr : accepted_actor_fifo_record (source_fence, 1, envelope, "late");
    envelope.metadata.clear ();
    auto resumed_record = accepted_actor_fifo_record (source_fence, 2, envelope, "resumed");
    std::vector<const runtime::serial_application_record_t *> transferred_records;
    spot_inbound_message_t metadata;
    metadata.values.emplace ("sequence", "LATE");
    auto sent = spots.relay_actor_packet (
      actor, gateway.actor_context (actor), stream_message_kind_t::send, "FencedProbe",
      zlink::message_t::from (std::string ("late")), provider, serializers, std::move (metadata),
      nullptr, {}, [&] { ++admission_notifications; }, zlink::routing_id_t::from (std::uint32_t{0}),
      {}, 0, std::nullopt,
      [&] {
          ++owner_transfers;
          transferred_records.push_back (late_record.get ());
      },
      0, {}, late_record);

    const auto cleanup = [&] {
        if (barrier)
            barrier->cancel ();
        node->actor_transfer_coordinator->cancel_move (key);
        scheduler->run_all ();
        node->worker_executor->drain ();
        node->worker_executor.reset ();
        spots.release_native_handles ();
    };

    if (!commit_before_post && barrier_reserved.load (std::memory_order_acquire)) {
        auto sealed = spots.seal_actor_application_suffix (actor, [&] {
            if (!node->actor_transfer_coordinator->try_begin_source_remote (key, "fifo-fence"))
                throw std::runtime_error ("reserved Actor did not enter source handoff");
        });
        scheduler->run_all ();
        const auto sealed_result = finite_task_result (std::move (sealed));
        if (!sealed_result || !*sealed_result) {
            cleanup ();
            return 10;
        }
    }
    scheduler->run_all ();
    const auto sent_result = finite_task_result (std::move (sent));

    if (!barrier_reserved.load (std::memory_order_acquire)) {
        cleanup ();
        return 1;
    }
    // A one-way send preserved for the move is indistinguishable from
    // immediate dispatch on the wire (spot-actor §10.2-1).
    if (!sent_result || !*sent_result) {
        cleanup ();
        return 2;
    }
    // The contract, not just the symptom: the old owner must never run it.
    if (source_handler_runs.load (std::memory_order_acquire) != 0
        || admission_notifications != 1
        // Queue acceptance transfers this exact record once before seal;
        // a committed-source relay does not submit to the old Actor queue.
        || owner_transfers != (commit_before_post ? 0 : 1)) {
        cleanup ();
        return 3;
    }
    if (commit_before_post) {
        const bool forwarded =
          follow_calls == 1 && node->actor_transfer_coordinator->take_backlog (key).empty ();
        cleanup ();
        return forwarded ? 0 : 9;
    }
    if (!node->actor_transfer_coordinator->blocks_dispatch (key)) {
        cleanup ();
        return 4;
    }
    // It must be in the capture set that travels with the commit, exactly once.
    auto backlog = node->actor_transfer_coordinator->take_backlog (key);
    if (backlog.size () != 1 || backlog.front ().packet_name != "FencedProbe"
        || backlog.front ().is_request || backlog.front ().application_record != late_record
        || transferred_records
             != std::vector<const runtime::serial_application_record_t *>{late_record.get ()}
        || !late_record->saved_work ()) {
        cleanup ();
        return 5;
    }
    const auto sequence = backlog.front ().metadata.find ("sequence");
    if (sequence == backlog.front ().metadata.end () || sequence->second != "LATE") {
        cleanup ();
        return 6;
    }

    // The barrier is released and the move closed: ordinary ingress resumes on
    // the same queue rather than staying fenced forever.
    barrier->cancel ();
    barrier.reset ();
    node->actor_transfer_coordinator->cancel_move (key);
    spot_inbound_message_t resumed_metadata;
    auto resumed = spots.relay_actor_packet (
      actor, gateway.actor_context (actor), stream_message_kind_t::send, "FencedProbe",
      zlink::message_t::from (std::string ("resumed")), provider, serializers,
      std::move (resumed_metadata), nullptr, {}, [&] { ++admission_notifications; },
      zlink::routing_id_t::from (std::uint32_t{0}), {}, 0, std::nullopt,
      [&] {
          ++owner_transfers;
          transferred_records.push_back (resumed_record.get ());
      },
      0, {}, resumed_record);
    scheduler->run_all ();
    const auto resumed_result = finite_task_result (std::move (resumed));
    const auto resumed_deadline = std::chrono::steady_clock::now () + std::chrono::seconds (2);
    while (source_handler_runs.load (std::memory_order_acquire) == 0
           && std::chrono::steady_clock::now () < resumed_deadline) {
        std::this_thread::yield ();
    }
    const auto resumed_runs = source_handler_runs.load (std::memory_order_acquire);
    cleanup ();
    if (!resumed_result || !*resumed_result)
        return 7;
    // Resumed ingress is a second record with its own single queue transfer.
    if (resumed_runs != 1 || admission_notifications != 2 || owner_transfers != 2
        || transferred_records
             != std::vector<const runtime::serial_application_record_t *>{late_record.get (),
                                                                          resumed_record.get ()})
        return 8;
    const auto passed = 0;
    return passed;
}

// A request caught in the same window must park too, and must not deadlock
// against the barrier that is waiting for the relocation to finish.
int relocation_barrier_fences_late_actor_request_without_deadlock ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace runtime = zlink::framework::runtime;

    serializer_registry_t serializers;
    auto node = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::spot_node_builder_state_t> ("actor-a");
    auto scheduler = std::make_shared<zlink::framework::test::manual_internal_scheduler_t> ();
    scheduler->publication_fence (zlink::framework::test::state_lane_publication_fence (
      node->lane, *::zlink::framework::runtime::infrastructure_executor ()));
    node->worker_executor = scheduler;
    node->channel_runtime =
      zlink::framework::test::runtime_failure_fixture<channel_runtime_state_t> ();
    node->channel_runtime->serializers = &serializers;

    auto spot = std::make_shared<spot_context_state_t> ();
    spot->node = node;
    spot->node_rid = node_rid_t::from_string ("actor-a");
    spot->spot_id = spot_id_t ("actor-a-entry");
    spot->spot_name = "entry";
    spot->spot_instance = std::make_shared<int> (1);
    spot->channel_runtime =
      zlink::framework::test::runtime_failure_fixture<channel_runtime_state_t> ();
    spot->channel_runtime->serializers = &serializers;
    spot->serial_executor = node->worker_executor;
    node->spot_contexts_by_id.emplace (spot->spot_id, spot_context_access_t::create (spot));

    const auto actor = test_actor_ref ("actor-a", "player", "fence-request-actor", 1);
    const auto key = std::string ("player:fence-request-actor");
    spot_node_runtime_t spots (node);

    std::atomic_int configure_calls{0};
    std::shared_ptr<deferred_barrier_t> barrier;
    std::atomic_bool barrier_reserved{false};
    spot_node_builder_state_t::actor_factory_registration_t factory;
    factory.actor_type = std::type_index (typeid (int));
    factory.create_instance = [] (std::string) { return std::make_shared<int> (7); };
    factory.configure_instance = [&] (void *, const actor_ref_t &current, void *) {
        if (configure_calls.fetch_add (1, std::memory_order_acq_rel) != 0)
            return;
        auto reserved = spots.reserve_actor_join_barrier (current);
        if (!reserved)
            return;
        barrier = std::move (reserved.value ());
        barrier_reserved.store (true, std::memory_order_release);
    };
    node->actor_factories.emplace ("player", std::move (factory));

    node->actor_instances.emplace (key, std::make_shared<int> (7));
    node->actor_spot_ids.emplace (key, spot->spot_id);
    node->actor_generations.emplace (key, 1);
    node->actor_types_by_id.emplace ("fence-request-actor", "player");

    std::atomic_int source_handler_runs{0};
    std::atomic_int admission_notifications{0};
    std::atomic_int owner_transfers{0};
    spot->handlers.push_back (
      spot_handler_descriptor_t{spot_handler_kind_t::actor_request, "FencedAsk", "",
                                std::type_index (typeid (int)), std::type_index (typeid (void)),
                                std::type_index (typeid (int)), std::type_index (typeid (void))});
    spot->handler_invokers.push_back (
      [&source_handler_runs] (void *, void *, service_provider_t &, serializer_registry_t &,
                              const zlink::message_t &,
                              const spot_inbound_message_t &) -> task_t<zlink::message_t> {
          source_handler_runs.fetch_add (1, std::memory_order_acq_rel);
          co_return zlink::message_t{};
      });

    service_collection_t services;
    auto provider = services.build_provider ();
    actor_gateway_runtime_t gateway;

    const runtime::protocol::actor_route_fence_t source_fence{
      "fence-request-actor", 1, zlink::routing_id_t::from ("actor-a").to_bytes (), 1, 1, 1};
    runtime::messaging::envelope_header_t envelope;
    envelope.kind = runtime::messaging::message_kind_t::request;
    envelope.channel_name = "actor";
    envelope.message_name = "FencedAsk";
    envelope.content_type = "application/json";
    envelope.metadata.emplace ("__zlink.actorRequestId", "fence-request-1");
    auto request_record = accepted_actor_fifo_record (source_fence, 1, envelope, "ask");
    const runtime::serial_application_record_t *transferred_record = nullptr;
    spot_inbound_message_t metadata;
    metadata.values = envelope.metadata.values ();
    auto asked = spots.relay_actor_packet (
      actor, gateway.actor_context (actor), stream_message_kind_t::request, "FencedAsk",
      zlink::message_t::from (std::string ("ask")), provider, serializers, std::move (metadata),
      nullptr, {}, [&] { ++admission_notifications; }, zlink::routing_id_t::from (std::uint32_t{0}),
      {}, 0, std::nullopt,
      [&] {
          ++owner_transfers;
          transferred_record = request_record.get ();
      },
      0, {}, request_record);

    const auto cleanup = [&] {
        if (barrier)
            barrier->cancel ();
        node->actor_transfer_coordinator->cancel_move (key);
        scheduler->run_all ();
        node->worker_executor->drain ();
        node->worker_executor.reset ();
    };

    if (barrier_reserved.load (std::memory_order_acquire)) {
        auto sealed = spots.seal_actor_application_suffix (actor, [&] {
            if (!node->actor_transfer_coordinator->try_begin_source_remote (key, "request-fence"))
                throw std::runtime_error ("reserved request did not enter source handoff");
        });
        scheduler->run_all ();
        const auto sealed_result = finite_task_result (std::move (sealed));
        if (!sealed_result || !*sealed_result) {
            cleanup ();
            return 5;
        }
    }
    scheduler->run_all ();
    // Export ends the old invocation, while the accepted request retains its
    // original operation and reply capability for replay.
    const auto asked_result = finite_task_result (std::move (asked));

    if (!barrier_reserved.load (std::memory_order_acquire)) {
        cleanup ();
        return 1;
    }
    // Preserved request: success with no reply yet (the target replies after
    // replay), never a silent local dispatch.
    if (!asked_result || !*asked_result || (*asked_result).value ().has_value ()) {
        cleanup ();
        return 2;
    }
    if (source_handler_runs.load (std::memory_order_acquire) != 0 || admission_notifications != 1
        || owner_transfers != 1 || transferred_record != request_record.get ()) {
        cleanup ();
        return 3;
    }
    auto backlog = node->actor_transfer_coordinator->take_backlog (key);
    if (backlog.size () != 1 || !backlog.front ().is_request
        || backlog.front ().packet_name != "FencedAsk"
        || backlog.front ().metadata.at ("__zlink.actorRequestId") != "fence-request-1"
        || backlog.front ().application_record != request_record
        || !request_record->saved_work ()) {
        cleanup ();
        return 4;
    }
    bool payload_preserved = false;
    runtime::stateful::with_accepted_application (
      *request_record->payload (), [&] (auto kind, const auto &application) {
          const runtime::messaging::message_parts_t parts (
            runtime::protocol::decode_application_parts (application));
          const auto body = runtime::messaging::envelope_codec_t{}.decode_body (parts);
          payload_preserved = kind == runtime::protocol::frozen_record_kind_t::actor_request && body
                              && body.value ().to_string () == "ask";
      });
    cleanup ();
    const auto passed = payload_preserved ? 0 : 6;
    zlink::framework::detail::spot_node_runtime_t (node).release_native_handles ();
    return passed;
}
} // namespace zlink::framework::test::actor_gateway
