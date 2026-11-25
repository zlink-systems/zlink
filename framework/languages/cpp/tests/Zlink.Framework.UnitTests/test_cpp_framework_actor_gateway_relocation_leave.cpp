/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/actor_gateway_cases.hpp"
#include "../support/actor_gateway_fixture.hpp"
#include "../support/actor_gateway_task_result.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/protocol/service_wire_codec.hpp"
#include "runtime/spots/spot_route_internal_dispatcher.hpp"
#include "runtime/spots/spot_route_packets.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include <zlink/framework/contracts/dispatch/task.hpp>
#include <zlink/framework/contracts/streams/stream.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
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
int source_cleanup_waits_for_leave_completion_before_erasing_actor ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace runtime = zlink::framework::runtime;

    // Regression pin (ST-B1): cleanup_expired_actor_admissions_at must not
    // erase the source Actor instance (spec 15 source membership cleanup)
    // until the OnLeave callback submit_remote_actor_leave queued has
    // actually finished (leave_completed), not merely been queued
    // (leave_submitted). submit_remote_actor_leave only enqueues OnLeave
    // onto the source Spot's serial executor and returns immediately, so on
    // some environments/timings the sweep raced ahead of that queued task
    // and erased the instance first; OnLeave's own dispatch then found it
    // gone and silently no-op'd -- relocation still completed end to end,
    // but the "leave" evidence never fired.
    auto node = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::spot_node_builder_state_t> ("actor-a");

    const auto actor = test_actor_ref ("actor-a", "player", "leave-race-actor", 1);
    const auto key = std::string ("player:leave-race-actor");
    node->actor_instances.emplace (key, std::make_shared<int> (7));

    const auto now = std::chrono::steady_clock::now ();
    node->pending_remote_source_cleanups.push_back (
      spot_node_builder_state_t::pending_remote_source_cleanup_t{
        .source_actor = actor,
        .source_fence = runtime::protocol::actor_route_fence_t{},
        .transfer_id = "transfer-leave-race",
        .target_spot_id = spot_id_t ("target-spot"),
        .not_before = now - std::chrono::seconds (1),
        .leave_submitted = true,
        .leave_completed = false,
        .leave_deadline = now + std::chrono::seconds (30)});

    spot_node_runtime_t spots (node);
    if (spots.next_management_activity_async ().result ().value ()
        != now + std::chrono::seconds (30))
        return 12;
    // not_before has already passed, but OnLeave is only queued
    // (leave_submitted), not finished (leave_completed), and the last-resort
    // deadline is far off: the sweep must hold the erase.
    const auto swept_while_pending = spots.cleanup_expired_actor_admissions_at (now);
    if (swept_while_pending != 0)
        return 1;
    if (!node->actor_instances.contains (key))
        return 2;
    if (node->pending_remote_source_cleanups.size () != 1)
        return 3;

    // OnLeave's queued task finishes and marks completion, mirroring the
    // completion continuation submit_remote_actor_leave installs.
    node->pending_remote_source_cleanups.front ().leave_completed = true;
    const auto swept_after_completion = spots.cleanup_expired_actor_admissions_at (now);
    if (swept_after_completion == 0)
        return 4;
    if (node->actor_instances.contains (key))
        return 5;
    if (!node->pending_remote_source_cleanups.empty ())
        return 6;

    // A return admission can begin after OnLeave completed but before this
    // sweep. The retained cleanup fence is the proof materialization needs to
    // distinguish the old A->B source instance from a live conflict. Preserve
    // both while dispatch is blocked by that in-flight move, then clean them
    // normally once the move closes.
    node->actor_instances.emplace (key, std::make_shared<int> (8));
    node->pending_remote_source_cleanups.push_back (
      spot_node_builder_state_t::pending_remote_source_cleanup_t{
        .source_actor = actor,
        .source_fence = runtime::protocol::actor_route_fence_t{},
        .transfer_id = "transfer-return-race",
        .target_spot_id = spot_id_t ("target-spot"),
        .not_before = now - std::chrono::seconds (1),
        .leave_submitted = true,
        .leave_completed = true,
        .leave_deadline = now - std::chrono::seconds (1)});
    if (!node->actor_transfer_coordinator->try_reserve_source (key, "return-admission"))
        return 7;
    if (spots.cleanup_expired_actor_admissions_at (now) != 0)
        return 8;
    if (!node->actor_instances.contains (key) || node->pending_remote_source_cleanups.size () != 1)
        return 9;
    node->actor_transfer_coordinator->cancel_move (key);
    if (spots.cleanup_expired_actor_admissions_at (now) == 0)
        return 10;
    if (node->actor_instances.contains (key) || !node->pending_remote_source_cleanups.empty ())
        return 11;
    return 0;
}

int sweep_preserves_pending_leave_added_after_snapshot ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace runtime = zlink::framework::runtime;

    auto node = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::spot_node_builder_state_t> ("actor-a");
    const auto actor = test_actor_ref ("actor-a", "player", "sweep-race-actor", 7);
    const auto pending = [&] (std::string transfer_id) {
        return spot_node_builder_state_t::pending_remote_actor_leave_t{
          .transfer_id = std::move (transfer_id),
          .source_actor = actor,
          .source_spot_id = spot_id_t ("source-spot"),
          .target_spot_id = spot_id_t ("target-spot"),
          .target_fence = runtime::protocol::actor_route_fence_t{}};
    };
    node->pending_remote_actor_leaves.push_back (pending ("old-transfer"));
    spot_node_runtime_t spots (node);
    const auto removed =
      spots.cleanup_expired_actor_admissions_at (std::chrono::steady_clock::now (), [&] {
          if (!node->lane.try_post ([node, &pending] {
                  node->pending_remote_actor_leaves.push_back (pending ("new-transfer"));
              }))
              throw std::runtime_error ("node lane closed before pending leave insertion");
      });
    const auto remaining = node->lane
                             .run ([&] {
                                 std::vector<std::string> transfers;
                                 for (const auto &leave : node->pending_remote_actor_leaves)
                                     transfers.push_back (leave.transfer_id);
                                 return transfers;
                             })
                             .get ();
    return removed == 1 && remaining == std::vector<std::string>{"new-transfer"} ? 0 : 1;
}

int leave_notification_travels_node_level_and_reaches_source_entry_spot_once ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace runtime = zlink::framework::runtime;
    namespace stateful = zlink::framework::runtime::stateful;

    // Regression pin: the OnLeave notification travels node-level (not
    // spot-addressed -- the source's Entry Spot is fixed to node lifecycle
    // and is never published into mesh spot routing, so a spot-addressed
    // send can never resolve it; that was ST-B1's "leave" marker never
    // firing). This drives the actual production sender
    // (spot_node_runtime_t::send_actor_leave_notification, reached via the
    // same on_actor_leave_notification hook app.cpp wires to
    // application_mesh->send_to_node) and the actual production receiver
    // (spot_route_internal_dispatcher_t::dispatch_send ->
    // submit_remote_actor_leave), connected by a stand-in for the wire hop.
    // Asserts OnLeave reaches the source Entry Spot exactly once and that
    // the just-landed completion-ordering gate (leave_completed) becomes
    // reachable and true afterward.
    serializer_registry_t serializers;

    // SOURCE node ("actor-a"): the Entry Spot the Actor left from.
    auto source_node = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::spot_node_builder_state_t> ("actor-a");
    source_node->worker_executor =
      std::make_shared<runtime::offload_executor_t> (1, "leave-notify-source");
    source_node->channel_runtime =
      zlink::framework::test::runtime_failure_fixture<channel_runtime_state_t> ();
    source_node->channel_runtime->serializers = &serializers;

    auto entry_spot = std::make_shared<spot_context_state_t> ();
    entry_spot->node = source_node;
    entry_spot->node_rid = node_rid_t::from_string ("actor-a");
    entry_spot->spot_id = spot_id_t ("actor-a-entry");
    entry_spot->spot_name = "entry";
    entry_spot->spot_instance = std::make_shared<int> (1);
    entry_spot->channel_runtime =
      zlink::framework::test::runtime_failure_fixture<channel_runtime_state_t> ();
    entry_spot->channel_runtime->serializers = &serializers;
    entry_spot->serial_executor = source_node->worker_executor;
    // spot_handle_t::status() only reads the object_ref_t it was
    // constructed with, never the host, so a null host is a faithful stand
    // -in here for the lifecycle_generation() check
    // submit_remote_actor_leave makes.
    const auto native_entry_spot = std::make_shared<service::spot_t> (
      nullptr, stateful::object_ref_t{stateful::object_kind_t::user_spot, "actor-a-entry", 7, 0,
                                      "mesh", "actor-a"});
    entry_spot->native_spot = native_entry_spot;
    source_node->spot_contexts_by_id.emplace (entry_spot->spot_id,
                                              spot_context_access_t::create (entry_spot));

    std::atomic_int leave_calls{0};
    spot_actor_admission_callbacks_t admission_callbacks;
    admission_callbacks.on_leave_actor = [&leave_calls] (void *, void *) -> task_t<void> {
        leave_calls.fetch_add (1, std::memory_order_release);
        co_return;
    };
    entry_spot->actor_admissions.emplace (std::type_index (typeid (int)),
                                          std::move (admission_callbacks));

    spot_node_builder_state_t::actor_factory_registration_t factory;
    factory.actor_type = std::type_index (typeid (int));
    factory.create_instance = [] (std::string) { return std::make_shared<int> (1); };
    factory.configure_instance = [] (void *, const actor_ref_t &, void *) {};
    source_node->actor_factories.emplace ("player", std::move (factory));

    const auto actor = test_actor_ref ("actor-a", "player", "leave-notify-actor", 7);
    const auto key = std::string ("player:leave-notify-actor");
    source_node->actor_instances.emplace (key, std::make_shared<int> (1));
    source_node->actor_spot_ids.emplace (key, entry_spot->spot_id);
    source_node->actor_generations.emplace (key, 7);

    // Authority store: the committed relocation record
    // submit_remote_actor_leave validates the OnLeave command against.
    class fixed_authority_store_t final : public stateful::authority_relocation_port_t
    {
      public:
        zlink::framework::runtime::stateful::source_relocation_settlement_t
        settle_source_relocation (
          const zlink::framework::runtime::stateful::relocation_authority_fence_t &fence,
          const std::vector<zlink::framework::runtime::stateful::object_ref_t> &participants,
          bool preserve) override
        {
            namespace sf = zlink::framework::runtime::stateful;
            (void) preserve;
            sf::source_relocation_settlement_t settled{observe_relocation (fence), {}};
            if (settled.outcome == sf::relocation_authority_t::target_committed) {
                for (const auto &participant : participants) {
                    if (const auto current = read (participant.kind, participant.key))
                        settled.follow_generations.emplace (
                          std::pair{participant.kind, participant.key},
                          current->target.authority_owner_generation);
                }
            }
            return settled;
        }

        std::optional<stateful::authority_relocation_reference_t> record;

        stateful::authority_publish_result_t publish (
          const stateful::object_ref_t &,
          const stateful::object_ref_t &,
          location_owner_token_t,
          object_creation_target_t,
          std::string,
          std::uint32_t,
          stateful::inventory_digest_t,
          std::vector<std::byte> = {},
          std::string = {},
          zlink::framework::runtime::protocol::relocation_id_t = {},
          location_owner_token_t = {},
          const zlink::framework::runtime::protocol::relocation_prepare_t * = nullptr) override
        {
            return {};
        }
        std::optional<stateful::authority_relocation_reference_t>
        read (stateful::object_kind_t, const std::string &) override
        {
            return record;
        }
    };
    auto authority = std::make_shared<fixed_authority_store_t> ();
    authority->record = stateful::authority_relocation_reference_t{
      .source = stateful::object_ref_t{stateful::object_kind_t::actor, "leave-notify-actor", 7, 0,
                                       "", "actor-a"},
      .target = stateful::object_ref_t{stateful::object_kind_t::actor, "leave-notify-actor", 7, 5,
                                       "", "actor-b"},
      .relocation_reference = "transfer-leave-notify",
      .checksum_crc32c = 0,
      .inventory_digest = {},
      .target_owner = location_owner_token_t{"owner-b", 9},
      .application_payload = {}};
    source_node->relocation_authority = authority;

    // A pending source cleanup for this transfer, exactly as
    // complete_remote_actor_transfer would have pushed at commit -- gated
    // on leave_completed (this session's completion-ordering fix).
    const auto source_fence = runtime::protocol::actor_route_fence_t{
      "leave-notify-actor", 7, zlink::routing_id_t::from ("actor-a").to_bytes (), 1, 1, 1};
    const auto target_fence = runtime::protocol::actor_route_fence_t{
      "leave-notify-actor", 7, zlink::routing_id_t::from ("actor-b").to_bytes (), 1, 5, 9};
    source_node->pending_remote_source_cleanups.push_back (
      spot_node_builder_state_t::pending_remote_source_cleanup_t{
        .source_actor = actor,
        .source_fence = source_fence,
        .transfer_id = "transfer-leave-notify",
        .source_spot_id = spot_id_t ("actor-a-entry"),
        .source_spot_generation = 7,
        .target_spot_id = spot_id_t ("spot-b-target"),
        .target_identity = {target_fence.target_node_routing_id,
                            target_fence.target_node_generation,
                            {},
                            target_fence.owner_lease_generation},
        .not_before = std::chrono::steady_clock::now (),
        .leave_submitted = false,
        .leave_completed = false,
        .leave_deadline = std::chrono::steady_clock::now () + std::chrono::seconds (30)});
    source_node->actor_transfer_coordinator->activate_message_follow (
      key, source_fence, test_actor_ref ("actor-b", "player", "leave-notify-actor", 7),
      spot_route_t{node_rid_t::from_string ("actor-b"), spot_id_t ("spot-b-target"), "game"},
      target_fence, std::chrono::steady_clock::now () + std::chrono::seconds (30),
      "transfer-leave-notify");

    // TARGET node ("actor-b"): only needs enough to hold the
    // on_actor_leave_notification sender under test.
    auto target_node = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::spot_node_builder_state_t> ("actor-b");

    spot_node_runtime_t source_spots (source_node);
    spot_node_runtime_t target_spots (target_node);

    service_collection_t services;
    auto provider = services.build_provider ();
    actor_gateway_runtime_t source_gateway;

    std::atomic_int notification_sends{0};
    target_spots.on_actor_leave_notification (
      [&] (const zlink::routing_id_t &target_node_rid,
           std::vector<zlink::message_t> parts) -> task_t<zlink::submit_result_t> {
          notification_sends.fetch_add (1, std::memory_order_release);
          if (target_node_rid.to_string () != "actor-a")
              co_return zlink::submit_result_t::not_found;
          // Stand-in for the wire hop: RouteMesh delivers a node-level send
          // to the receiving node's own dispatch_send exactly like this.
          spot_route_internal_dispatcher_t dispatcher (source_spots, source_gateway,
                                                       route_client_t{}, serializers);
          runtime::messaging::message_parts_t encoded (std::move (parts));
          route_received_packet_t received{zlink::routing_id_t::from ("actor-b"), std::nullopt,
                                           std::move (encoded), std::nullopt};
          auto header =
            runtime::messaging::envelope_codec_t{}.decode_header (received.parts, false);
          if (!header)
              co_return zlink::submit_result_t::internal_error;
          const auto dispatched =
            co_await dispatcher.dispatch_send (received, header.value (), provider);
          co_return dispatched ? zlink::submit_result_t::ok
                               : zlink::submit_result_t::internal_error;
      });

    // Build the leave command exactly as dispatch_actor_commit_request's
    // finalize branch does, and submit it through the production sender.
    const auto leave_command =
      spot_actor_leave_route_command_t{.transfer_id = "transfer-leave-notify",
                                       .actor_node_rid = "actor-a",
                                       .actor_type = "player",
                                       .actor_id = "leave-notify-actor",
                                       .actor_generation = 7,
                                       .source_spot_id = "actor-a-entry",
                                       .source_spot_generation = 7,
                                       .target_spot_id = "spot-b-target",
                                       .target_node_rid = "actor-b",
                                       .target_node_generation = 1,
                                       .target_authority_owner_generation = 5,
                                       .target_owner_lease_generation = 9};
    runtime::messaging::envelope_header_t leave_header;
    leave_header.kind = runtime::messaging::message_kind_t::command;
    leave_header.channel_name = "node";
    leave_header.message_name = spot_actor_leave_route_command_t::packet_name;
    auto leave_parts = runtime::messaging::envelope_codec_t{}.encode_parts (
      leave_header, leave_command, serializers);

    auto sent = target_spots.send_actor_leave_notification (zlink::routing_id_t::from ("actor-a"),
                                                            std::move (leave_parts));
    const auto sent_result = finite_task_result (std::move (sent));
    if (!sent_result || !*sent_result || sent_result->value () != zlink::submit_result_t::ok)
        return 1;
    if (notification_sends.load (std::memory_order_acquire) != 1)
        return 2;

    const auto leave_deadline = std::chrono::steady_clock::now () + std::chrono::seconds (2);
    while (leave_calls.load (std::memory_order_acquire) == 0
           && std::chrono::steady_clock::now () < leave_deadline) {
        std::this_thread::yield ();
    }
    if (leave_calls.load (std::memory_order_acquire) != 1)
        return 3;

    // leave_completed is set from the queued callback's own completion
    // continuation, asynchronously; poll for it the same way.
    const auto completion_deadline = std::chrono::steady_clock::now () + std::chrono::seconds (2);
    bool completed = false;
    while (std::chrono::steady_clock::now () < completion_deadline) {
        const auto found = std::find_if (
          source_node->pending_remote_source_cleanups.begin (),
          source_node->pending_remote_source_cleanups.end (),
          [] (const auto &candidate) { return candidate.transfer_id == "transfer-leave-notify"; });
        if (found != source_node->pending_remote_source_cleanups.end () && found->leave_completed) {
            completed = true;
            break;
        }
        std::this_thread::yield ();
    }
    if (!completed)
        return 4;

    // A duplicate delivery of the same command must not re-invoke OnLeave.
    auto duplicate_parts = runtime::messaging::envelope_codec_t{}.encode_parts (
      leave_header, leave_command, serializers);
    auto duplicate_sent = target_spots.send_actor_leave_notification (
      zlink::routing_id_t::from ("actor-a"), std::move (duplicate_parts));
    const auto duplicate_result = finite_task_result (std::move (duplicate_sent));
    if (!duplicate_result || !*duplicate_result)
        return 5;
    std::this_thread::sleep_for (std::chrono::milliseconds (50));
    if (leave_calls.load (std::memory_order_acquire) != 1)
        return 6;
    const auto passed = 0;
    zlink::framework::detail::spot_node_runtime_t (source_node).release_native_handles ();
    return passed;
}
} // namespace zlink::framework::test::actor_gateway
