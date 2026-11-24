/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/actor_gateway_cases.hpp"
#include "../support/actor_gateway_fixture.hpp"
#include "../support/actor_gateway_task_result.hpp"
#include "../support/stateful_object_sync.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/mesh/mesh_node_host_service.hpp"
#include "runtime/protocol/service_wire_codec.hpp"
#include "runtime/spots/spot_route_packets.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include <zlink/framework/contracts/dispatch/task.hpp>
#include <zlink/framework/contracts/streams/stream.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <typeindex>
#include <utility>
#include <vector>

namespace zlink::framework::test::actor_gateway
{
using zlink::framework::test::begin_reserved_object;

// Shared stub: the committed relocation record cleanup_expired_actor_admissions_at
// reads at reconcile-deadline expiry to reconcile against the Location Store's
// authority (spec 28 relay-ready irreversibility) instead of blindly replaying
// locally. Its record is nulled or filled per test to select the branch under
// test.
class fixed_reconcile_authority_store_t final
    : public zlink::framework::runtime::stateful::authority_relocation_port_t
{
  public:
    zlink::framework::runtime::stateful::source_relocation_settlement_t settle_source_relocation (
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

    std::optional<zlink::framework::runtime::stateful::authority_relocation_reference_t> record;

    zlink::framework::runtime::stateful::authority_publish_result_t
    publish (const zlink::framework::runtime::stateful::object_ref_t &,
             const zlink::framework::runtime::stateful::object_ref_t &,
             zlink::framework::location_owner_token_t,
             zlink::framework::object_creation_target_t,
             std::string,
             std::uint32_t,
             zlink::framework::runtime::stateful::inventory_digest_t,
             std::vector<std::byte> = {},
             std::string = {},
             zlink::framework::runtime::protocol::relocation_id_t = {},
             zlink::framework::location_owner_token_t = {},
             const zlink::framework::runtime::protocol::relocation_prepare_t * = nullptr) override
    {
        return {};
    }
    std::optional<zlink::framework::runtime::stateful::authority_relocation_reference_t>
    read (zlink::framework::runtime::stateful::object_kind_t, const std::string &) override
    {
        return record;
    }
};

int reconcile_deadline_adopts_target_when_store_shows_committed ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace runtime = zlink::framework::runtime;
    namespace stateful = zlink::framework::runtime::stateful;

    // 04-relocation-flow.ko.md §9, paragraph after the failure table:
    // after accepted relay-ready and an unknown cutover result, a visible
    // target commit adopts the target route and forward-drains the backlog.
    // Source dispatch must never reopen.
    auto node = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::spot_node_builder_state_t> ("actor-a");
    node->worker_executor =
      std::make_shared<runtime::offload_executor_t> (1, "reconcile-adopt-worker");
    service_collection_t services;
    node->root_services = services.build_provider ();

    const auto key = std::string ("player:reconcile-actor");
    node->actor_generations.emplace (key, 1);

    const auto source_fence =
      runtime::protocol::actor_route_fence_t{"reconcile-actor", 1, {}, 0, 5, 1};
    const auto target_fence =
      runtime::protocol::actor_route_fence_t{"reconcile-actor", 1, {}, 0, 6, 2};
    const auto target_actor = test_actor_ref ("actor-b", "player", "reconcile-actor", 1);
    const auto target_route =
      spot_route_t{node_rid_t::from_string ("actor-b"), spot_id_t ("target-spot"), ""};

    auto authority = std::make_shared<fixed_reconcile_authority_store_t> ();
    authority->record = stateful::authority_relocation_reference_t{
      .source = stateful::object_ref_t{stateful::object_kind_t::actor, "reconcile-actor", 1, 5,
                                       "mesh", "actor-a"},
      .target = stateful::object_ref_t{stateful::object_kind_t::actor, "reconcile-actor", 1, 6,
                                       "mesh", "actor-b"}};
    node->relocation_authority = authority;

    const auto before_reconcile = std::chrono::steady_clock::now ();
    node->actor_transfer_coordinator->mark_reconcile (
      key, std::chrono::milliseconds (20),
      reconcile_target_context_t{target_route, target_actor, source_fence, target_fence,
                                 "transfer-reconcile"});
    const auto after_reconcile = std::chrono::steady_clock::now ();

    // A packet parked while reconcile was pending must actually be drained
    // by forwarding to the adopted target -- not merely have its route
    // adopted with the backlog left stranded.
    // The adoption task can run after an early return, so the handler owns its counter.
    auto relay_calls = std::make_shared<std::atomic_int> (0);
    spot_node_runtime_t spots (node);
    spots.on_actor_message_follow (
      [relay_calls] (const actor_ref_t &, const runtime::messaging::envelope_header_t &,
                     const zlink::message_t &, std::chrono::milliseconds,
                     const zlink::routing_id_t &, const runtime::protocol::actor_route_fence_t &,
                     std::uint8_t, const runtime::protocol::wire_operation_id_t &,
                     std::uint64_t) -> task_t<std::optional<zlink::message_t>> {
          relay_calls->fetch_add (1, std::memory_order_acq_rel);
          co_return result_t<std::optional<zlink::message_t>>::success (std::nullopt);
      });
    (void) node->actor_transfer_coordinator->try_append_backlog (
      key, handoff_packet_t{"AdoptedProbe",
                            std::vector<std::uint8_t> ({'p', 'r', 'o', 'b', 'e'}),
                            "text/plain",
                            {},
                            false});

    if (spots.cleanup_expired_actor_admissions_at (before_reconcile
                                                   + std::chrono::milliseconds (19))
          != 0
        || relay_calls->load (std::memory_order_acquire) != 0
        || !node->actor_transfer_coordinator->blocks_dispatch (key))
        return 6;
    if (spots.cleanup_expired_actor_admissions_at (after_reconcile + std::chrono::milliseconds (20))
        != 1)
        return 7;

    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (2);
    while (node->actor_transfer_coordinator->phase (key).has_value ()
           && std::chrono::steady_clock::now () < deadline) {
        std::this_thread::yield ();
    }
    if (node->actor_transfer_coordinator->phase (key).has_value ())
        return 1;
    const auto route = node->actor_routes.find (key);
    const auto found_target_route = route != node->actor_routes.end ();
    const auto spot_matches =
      found_target_route && route->second.spot_id == std::string ("target-spot");
    const auto node_matches =
      found_target_route && route->second.node_rid.value () == std::string_view ("actor-b");
    // The async adoption task (worker_executor's own thread) is the sole
    // other owner of `node` at this point; wait for it to fully settle and
    // detach the executor here, on the main thread, before `node` goes out
    // of scope -- otherwise the worker thread could end up dropping the
    // last reference to its own executor and self-joining.
    node->worker_executor->drain ();
    node->worker_executor.reset ();
    if (relay_calls->load (std::memory_order_acquire) != 1)
        return 5;
    if (!found_target_route)
        return 2;
    if (!spot_matches)
        return 3;
    if (!node_matches)
        return 4;
    return 0;
}

int reconcile_deadline_fast_fails_without_target_commit (
  std::shared_ptr<fixed_reconcile_authority_store_t> authority)
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace runtime = zlink::framework::runtime;

    // 04-relocation-flow.ko.md §9, paragraph after the failure table:
    // after relay-ready is accepted, an unknown cutover cannot reopen source
    // dispatch even when the Store still shows the source. The target may
    // continue its CAS within Restore validity. At the reconciliation deadline,
    // held requests must end Unavailable and the source must stay sealed.
    serializer_registry_t serializers;
    auto node = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::spot_node_builder_state_t> ("actor-a");
    node->worker_executor =
      std::make_shared<runtime::offload_executor_t> (1, "reconcile-indeterminate-worker");
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

    spot_node_builder_state_t::actor_factory_registration_t factory;
    factory.actor_type = std::type_index (typeid (int));
    factory.create_instance = [] (std::string) { return std::make_shared<int> (7); };
    factory.configure_instance = [] (void *, const actor_ref_t &, void *) {};
    node->actor_factories.emplace ("player", std::move (factory));

    const auto actor = test_actor_ref ("actor-a", "player", "reconcile-actor", 1);
    const auto key = std::string ("player:reconcile-actor");
    node->actor_instances.emplace (key, std::make_shared<int> (7));
    node->actor_spot_ids.emplace (key, spot->spot_id);
    node->actor_generations.emplace (key, 1);

    // The handlers and sinks below can run after an early return skips the drain, so they own
    // their flags and counters.
    auto handler_ran = std::make_shared<std::atomic_bool> (false);
    spot->handlers.push_back (
      spot_handler_descriptor_t{spot_handler_kind_t::actor_send, "ReconcileProbe", "",
                                std::type_index (typeid (int)), std::type_index (typeid (void)),
                                std::type_index (typeid (int)), std::type_index (typeid (void))});
    spot->handler_invokers.push_back (
      [handler_ran] (void *, void *, service_provider_t &, serializer_registry_t &,
                     const zlink::message_t &,
                     const spot_inbound_message_t &) -> task_t<zlink::message_t> {
          handler_ran->store (true, std::memory_order_release);
          co_return zlink::message_t{};
      });

    const auto source_fence =
      runtime::protocol::actor_route_fence_t{"reconcile-actor", 1, {}, 0, 5, 1};
    const auto target_fence =
      runtime::protocol::actor_route_fence_t{"reconcile-actor", 1, {}, 0, 6, 2};
    const auto target_actor = test_actor_ref ("actor-b", "player", "reconcile-actor", 1);
    const auto target_route =
      spot_route_t{node_rid_t::from_string ("actor-b"), spot_id_t ("target-spot"), ""};
    node->relocation_authority = std::move (authority);
    const auto before_reconcile = std::chrono::steady_clock::now ();
    node->actor_transfer_coordinator->mark_reconcile (
      key, std::chrono::milliseconds (20),
      reconcile_target_context_t{target_route, target_actor, source_fence, target_fence,
                                 "transfer-reconcile"});

    const auto after_reconcile = std::chrono::steady_clock::now ();

    // A parked *request* (not just a send) must actually receive an
    // explicit failure reply -- "bounded wait then requestFailed", not
    // just silently dropped or left to time out on its own.
    auto terminal_calls = std::make_shared<std::atomic_int> (0);
    auto terminal_was_failure = std::make_shared<std::atomic_bool> (false);
    node->actor_handoff_terminal_sender =
      [terminal_calls,
       terminal_was_failure] (const zlink::routing_id_t &, const zlink::routing_id_t &,
                              const runtime::protocol::wire_operation_id_t &, std::uint64_t,
                              const runtime::protocol::actor_route_fence_t &,
                              const result_t<zlink::message_t> &completed) -> task_t<bool> {
        terminal_calls->fetch_add (1, std::memory_order_acq_rel);
        terminal_was_failure->store (!completed, std::memory_order_release);
        co_return true;
    };
    std::map<std::string, std::string> terminal_metadata;
    terminal_metadata[std::string (actor_handoff_source_node_key)] =
      zlink::routing_id_t::from ("remote-caller-node").to_hex ();
    terminal_metadata[std::string (actor_handoff_parking_node_key)] =
      zlink::routing_id_t::from ("actor-a").to_hex ();
    terminal_metadata[std::string (actor_handoff_operation_high_key)] = "111";
    terminal_metadata[std::string (actor_handoff_operation_low_key)] = "222";
    terminal_metadata[std::string (actor_handoff_reply_route_key)] = "333";
    // This backlog belongs to actor-a, so its terminal is delivered through
    // actor-a's parked reply entry rather than re-sent to the remote caller.
    // Keep a real local reply token here: after terminal identity gained the
    // parking-node and initiating-fence dimensions, a hand-written backlog
    // with no matching pending entry only exercises the intentional drop.
    auto local_reply_calls = std::make_shared<std::atomic_int> (0);
    auto local_reply_was_unavailable = std::make_shared<std::atomic_bool> (false);
    auto reply_host = std::make_shared<host::public_host_runtime_t> (host::host_options_t{
      .mesh = {.descriptor = {.mesh_name = "reconcile-indeterminate",
                              .node_routing_id =
                                zlink::routing_id_t::from (std::string ("actor-a")).to_bytes (),
                              .lifecycle_generation = 1,
                              .descriptor_revision = 1,
                              .advertised_endpoint = "tcp://127.0.0.1:0"},
               .runtime_failures =
                 std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()}});
    const auto make_reply_token = [reply_host] (std::shared_ptr<std::atomic_int> calls,
                                                std::shared_ptr<std::atomic_bool> was_unavailable) {
        host::reply_token_t token;
        token.host = reply_host;
        token.local_reply = [calls, was_unavailable] (const std::vector<zlink::message_t> &parts) {
            const auto reply =
              detail::decode_actor_reply (runtime::messaging::message_parts_t (parts));
            was_unavailable->store (!reply
                                      && reply.error_kind () == framework_error_kind_t::unavailable,
                                    std::memory_order_release);
            calls->fetch_add (1, std::memory_order_acq_rel);
            return true;
        };
        return token;
    };
    auto local_reply = make_reply_token (local_reply_calls, local_reply_was_unavailable);
    node->pending_handoff_requests.emplace (
      spot_node_builder_state_t::pending_handoff_request_key_t{
        zlink::routing_id_t::from ("remote-caller-node").to_hex (), 111, 222, {}},
      spot_node_builder_state_t::pending_handoff_request_t{actor,
                                                           {},
                                                           333,
                                                           std::move (local_reply),
                                                           {},
                                                           std::chrono::steady_clock::now ()
                                                             + std::chrono::seconds (30)});
    auto second_reply_calls = std::make_shared<std::atomic_int> (0);
    auto second_reply_was_unavailable = std::make_shared<std::atomic_bool> (false);
    auto second_metadata = terminal_metadata;
    second_metadata[std::string (actor_handoff_operation_high_key)] = "112";
    second_metadata[std::string (actor_handoff_operation_low_key)] = "223";
    second_metadata[std::string (actor_handoff_reply_route_key)] = "334";
    node->pending_handoff_requests.emplace (
      spot_node_builder_state_t::pending_handoff_request_key_t{
        zlink::routing_id_t::from ("remote-caller-node").to_hex (), 112, 223, {}},
      spot_node_builder_state_t::pending_handoff_request_t{
        actor,
        {},
        334,
        make_reply_token (second_reply_calls, second_reply_was_unavailable),
        {},
        std::chrono::steady_clock::now () + std::chrono::seconds (30)});
    (void) node->actor_transfer_coordinator->try_append_backlog (
      key,
      handoff_packet_t{"FastFailProbe", {}, "application/octet-stream", terminal_metadata, true});
    (void) node->actor_transfer_coordinator->try_append_backlog (
      key,
      handoff_packet_t{"FastFailProbe", {}, "application/octet-stream", second_metadata, true});

    service_collection_t services;
    auto provider = services.build_provider ();
    actor_gateway_runtime_t gateway;
    spot_inbound_message_t metadata;

    spot_node_runtime_t spots (node);
    auto sent =
      spots.relay_actor_packet (actor, gateway.actor_context (actor), stream_message_kind_t::send,
                                "ReconcileProbe", zlink::message_t::from (std::string ("probe")),
                                provider, serializers, std::move (metadata), nullptr);
    const auto sent_result = finite_task_result (std::move (sent));
    if (!sent_result || !*sent_result)
        return 1;
    if (handler_ran->load (std::memory_order_acquire))
        return 2;

    // Drive the existing monotonic sweep clock across the deadline without
    // sleeping. Before expiry the reply must remain parked.
    if (spots.cleanup_expired_actor_admissions_at (before_reconcile
                                                   + std::chrono::milliseconds (19))
          != 0
        || local_reply_calls->load (std::memory_order_acquire) != 0
        || second_reply_calls->load (std::memory_order_acquire) != 0)
        return 9;
    const auto expired = after_reconcile + std::chrono::milliseconds (20);
    if (spots.cleanup_expired_actor_admissions_at (expired) != 1)
        return 10;
    // No target commit is visible: keep the move sealed, regardless of
    // whether the Store returned source evidence, no record, or no answer.
    if (!node->actor_transfer_coordinator->phase (key).has_value ())
        return 3;
    if (*node->actor_transfer_coordinator->phase (key) != actor_move_phase_t::reconcile)
        return 4;
    if (terminal_calls->load (std::memory_order_acquire) != 0
        || local_reply_calls->load (std::memory_order_acquire) != 1
        || second_reply_calls->load (std::memory_order_acquire) != 1)
        return 7;
    if (terminal_was_failure->load (std::memory_order_acquire)
        || !local_reply_was_unavailable->load (std::memory_order_acquire)
        || !second_reply_was_unavailable->load (std::memory_order_acquire))
        return 8;
    if (!node->pending_handoff_requests.empty ()
        || !node->actor_transfer_coordinator->blocks_dispatch (key)
        || node->actor_routes.contains (key))
        return 11;

    // A fresh request still cannot reach the handler -- it parks again
    // rather than being served, because the Actor genuinely stays
    // unavailable.
    spot_inbound_message_t follow_up_metadata;
    auto follow_up =
      spots.relay_actor_packet (actor, gateway.actor_context (actor), stream_message_kind_t::send,
                                "ReconcileProbe", zlink::message_t::from (std::string ("probe-2")),
                                provider, serializers, std::move (follow_up_metadata), nullptr);
    const auto follow_up_result = finite_task_result (std::move (follow_up));
    if (!follow_up_result || !*follow_up_result)
        return 5;
    (void) spots.cleanup_expired_actor_admissions_at (expired);
    node->worker_executor->drain ();
    if (handler_ran->load (std::memory_order_acquire))
        return 6;
    if (local_reply_calls->load (std::memory_order_acquire) != 1
        || second_reply_calls->load (std::memory_order_acquire) != 1
        || !node->actor_transfer_coordinator->blocks_dispatch (key)
        || node->actor_routes.contains (key))
        return 12;
    zlink::framework::detail::spot_node_runtime_t (node).release_native_handles ();
    return 0;
}

int reconcile_deadline_fast_fails_when_store_shows_source ()
{
    namespace stateful = zlink::framework::runtime::stateful;
    auto authority = std::make_shared<fixed_reconcile_authority_store_t> ();
    // This record still names actor-a, not the expected actor-b commit. Its
    // matching source fence used to authorize the unsafe source_owns replay.
    const auto source = stateful::object_ref_t{
      stateful::object_kind_t::actor, "reconcile-actor", 1, 5, "mesh", "actor-a"};
    authority->record =
      stateful::authority_relocation_reference_t{.source = source, .target = source};
    return reconcile_deadline_fast_fails_without_target_commit (std::move (authority));
}

int reconcile_deadline_fast_fails_when_store_has_no_commit ()
{
    // A missing relocation record also used to authorize source_owns replay.
    return reconcile_deadline_fast_fails_without_target_commit (
      std::make_shared<fixed_reconcile_authority_store_t> ());
}

int reconcile_deadline_fast_fails_when_store_is_indeterminate ()
{
    return reconcile_deadline_fast_fails_without_target_commit (nullptr);
}

int known_generation_source_leave_claims_transfer ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace stateful = zlink::framework::runtime::stateful;

    auto node = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::spot_node_builder_state_t> ("actor-a");
    auto entry_spot = std::make_shared<spot_context_state_t> ();
    entry_spot->node = node;
    entry_spot->node_rid = node_rid_t::from_string ("actor-a");
    entry_spot->spot_id = spot_id_t ("actor-a-entry");
    entry_spot->spot_name = "entry";
    entry_spot->spot_instance = std::make_shared<int> (1);
    entry_spot->actor_count = 1;
    auto native_spot = std::make_shared<service::spot_t> (
      nullptr, stateful::object_ref_t{stateful::object_kind_t::user_spot, "actor-a-entry", 7, 0,
                                      "mesh", "actor-a"});
    entry_spot->native_spot = native_spot;
    node->spot_contexts_by_id.emplace (entry_spot->spot_id,
                                       spot_context_access_t::create (entry_spot));

    const auto actor = test_actor_ref ("actor-a", "player", "known-leave-actor", 7);
    const auto key = std::string ("player:known-leave-actor");
    constexpr auto transfer_id = "transfer-known-leave";
    node->actor_instances.emplace (key, std::make_shared<int> (1));
    node->actor_spot_ids.emplace (key, entry_spot->spot_id);
    spot_node_builder_state_t::actor_factory_registration_t factory;
    factory.actor_type = std::type_index (typeid (int));
    node->actor_factories.emplace ("player", std::move (factory));
    if (!node->actor_transfer_coordinator->try_reserve_source (key, transfer_id)
        || !node->actor_transfer_coordinator->try_begin_source_remote (key, transfer_id)) {
        return 1;
    }

    auto authority = std::make_shared<fixed_reconcile_authority_store_t> ();
    authority->record = stateful::authority_relocation_reference_t{
      .source = stateful::object_ref_t{stateful::object_kind_t::actor, "known-leave-actor", 7, 0,
                                       "", "actor-a"},
      .target = stateful::object_ref_t{stateful::object_kind_t::actor, "known-leave-actor", 7, 28,
                                       "", "actor-b"},
      .relocation_reference = transfer_id,
      .target_owner = location_owner_token_t{"owner-b", 9}};
    node->relocation_authority = authority;

    spot_node_runtime_t spots (node);
    const auto fence = runtime::protocol::actor_route_fence_t{
      "known-leave-actor", 7, zlink::routing_id_t::from ("actor-b").to_bytes (), 1, 5, 9};
    const auto result = spots
                          .submit_remote_actor_leave_async (transfer_id, actor, entry_spot->spot_id,
                                                            7, spot_id_t ("spot-b-target"), fence)
                          .result ()
                          .value ();
    if (!result || node->pending_remote_actor_leaves.size () != 1
        || !node->actor_spot_ids.contains (key))
        return 2;
    const auto source_fence = runtime::protocol::actor_route_fence_t{
      "known-leave-actor", 7, zlink::routing_id_t::from ("actor-a").to_bytes (), 1, 1, 8};
    // The original B notification carries generation 5 even when B has
    // returned to current authority generation 28 before source settlement.
    const auto completed = finite_task_result (spots.complete_remote_actor_transfer (
      actor, test_actor_ref ("actor-b", "player", "known-leave-actor", 7),
      {node_rid_t::from_string ("actor-b"), spot_id_t ("spot-b-target"), {}}, source_fence,
      {fence.target_node_routing_id, fence.target_node_generation, "owner-b",
       fence.owner_lease_generation},
      28, transfer_id));
    if (!completed || !*completed || node->pending_remote_source_cleanups.size () != 1
        || !node->pending_remote_source_cleanups.front ().leave_submitted)
        return 3;
    const auto cleanup_at = node->pending_remote_source_cleanups.front ().not_before;
    for (auto duplicate = 0; duplicate < 2; ++duplicate) {
        if (!node->lane
               .run ([&] {
                   const auto &cleanup = node->pending_remote_source_cleanups.front ();
                   return cleanup.leave_submitted && cleanup.leave_completed
                          && node->pending_remote_actor_leaves.empty ()
                          && entry_spot->actor_count == 0;
               })
               .get ())
            return 4;
        if (!node->lane
               .run ([&] {
                   return node->actor_spot_ids.contains (key)
                          && node->actor_spot_ids.at (key) == spot_id_t ("spot-b-target")
                          && node->actor_routes.contains (key)
                          && node->actor_routes.at (key).node_rid.value () == "actor-b"
                          && node->actor_routes.at (key).spot_id == spot_id_t ("spot-b-target");
               })
               .get ())
            return 5;
        const auto follow =
          node->actor_transfer_coordinator->message_follow_target (key, source_fence);
        if (!follow || follow->route.node_rid.value () != "actor-b"
            || follow->route.spot_id != spot_id_t ("spot-b-target")
            || follow->target_fence.authority_owner_generation != 28)
            return 6;
        if (duplicate == 0
            && !spots
                  .submit_remote_actor_leave_async (transfer_id, actor, entry_spot->spot_id, 7,
                                                    spot_id_t ("spot-b-target"), fence)
                  .result ()
                  .value ())
            return 7;
    }

    // Advance the existing cleanup boundary without waiting for wall-clock time.
    const auto cleaned =
      finite_task_result (spots.cleanup_expired_actor_admissions_at_async (cleanup_at));
    if (!cleaned || !*cleaned || cleaned->value () != 1)
        return 8;
    if (!node->lane
           .run ([&] {
               return !node->actor_instances.contains (key)
                      && node->pending_remote_source_cleanups.empty ()
                      && node->pending_remote_actor_leaves.empty () && entry_spot->actor_count == 0
                      && node->actor_spot_ids.contains (key)
                      && node->actor_spot_ids.at (key) == spot_id_t ("spot-b-target")
                      && node->actor_routes.contains (key)
                      && node->actor_routes.at (key).node_rid.value () == "actor-b"
                      && node->actor_routes.at (key).spot_id == spot_id_t ("spot-b-target");
           })
           .get ())
        return 9;
    const auto follow = node->actor_transfer_coordinator->message_follow_target (key, source_fence);
    const auto passed = follow && follow->route.node_rid.value () == "actor-b"
                            && follow->route.spot_id == spot_id_t ("spot-b-target")
                            && follow->target_fence.authority_owner_generation == 28
                          ? 0
                          : 10;
    zlink::framework::detail::spot_node_runtime_t (node).release_native_handles ();
    return passed;
}

int early_zero_generation_leave_waits_for_source_transfer_completion (bool no_follow)
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace runtime = zlink::framework::runtime;
    namespace stateful = zlink::framework::runtime::stateful;

    // The target-side materialization path cannot project the source Spot
    // lifecycle generation, so its node-level OnLeave command legitimately
    // carries zero. Pin the ordering in which that one-way command reaches
    // the source before complete_remote_actor_transfer has installed the
    // source-cleanup record that resolves the generation. The command must
    // be retained and applied exactly once after the completion registration.
    serializer_registry_t serializers;
    auto node = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::spot_node_builder_state_t> ("actor-a");
    // Message Follow may be disabled independently of membership lifecycle.
    // The early OnLeave must still survive until the source transfer completes.
    node->message_follow_duration = std::chrono::milliseconds (0);
    node->worker_executor = std::make_shared<runtime::offload_executor_t> (1, "early-actor-leave");
    std::mutex blocker_mutex;
    std::condition_variable blocker_changed;
    std::optional<runtime::serial_execution_queue_t::async_completion_t> complete_blocker;
    bool blocker_entered = false;
    const auto release_blocker = [&] {
        std::optional<runtime::serial_execution_queue_t::async_completion_t> completion;
        {
            std::lock_guard lock (blocker_mutex);
            if (complete_blocker) {
                completion.emplace (std::move (*complete_blocker));
                complete_blocker.reset ();
            }
        }
        if (completion)
            (*completion) ([] {});
    };
    const auto finish = [&] (int code) {
        release_blocker ();
        if (node->worker_executor) {
            node->worker_executor->drain ();
            node->worker_executor.reset ();
        }
        return code;
    };
    node->channel_runtime =
      zlink::framework::test::runtime_failure_fixture<channel_runtime_state_t> ();
    node->channel_runtime->serializers = &serializers;

    auto entry_spot = std::make_shared<spot_context_state_t> ();
    entry_spot->node = node;
    entry_spot->node_rid = node_rid_t::from_string ("actor-a");
    entry_spot->spot_id = spot_id_t ("actor-a-entry");
    entry_spot->spot_name = "entry";
    entry_spot->spot_instance = std::make_shared<int> (1);
    entry_spot->channel_runtime =
      zlink::framework::test::runtime_failure_fixture<channel_runtime_state_t> ();
    entry_spot->channel_runtime->serializers = &serializers;
    entry_spot->serial_executor = node->worker_executor;
    runtime::serial_execution_queue_options_t serial_options;
    entry_spot->serial_queue = std::make_shared<runtime::serial_execution_queue_t> (
      *entry_spot->serial_executor, serial_options);
    const auto native_entry_spot = std::make_shared<service::spot_t> (
      nullptr, stateful::object_ref_t{stateful::object_kind_t::user_spot, "actor-a-entry", 7, 0,
                                      "mesh", "actor-a"});
    entry_spot->native_spot = native_entry_spot;
    node->spot_contexts_by_id.emplace (entry_spot->spot_id,
                                       spot_context_access_t::create (entry_spot));

    std::atomic_int leave_calls{0};
    spot_actor_admission_callbacks_t callbacks;
    callbacks.on_leave_actor = [&leave_calls] (void *, void *) -> task_t<void> {
        leave_calls.fetch_add (1, std::memory_order_acq_rel);
        co_return;
    };
    entry_spot->actor_admissions.emplace (std::type_index (typeid (int)), std::move (callbacks));

    spot_node_builder_state_t::actor_factory_registration_t factory;
    factory.actor_type = std::type_index (typeid (int));
    factory.create_instance = [] (std::string) { return std::make_shared<int> (1); };
    factory.configure_instance = [] (void *, const actor_ref_t &, void *) {};
    node->actor_factories.emplace ("player", std::move (factory));

    std::atomic_int source_handler_calls{0};
    entry_spot->handlers.push_back (
      spot_handler_descriptor_t{spot_handler_kind_t::actor_request, "NoFollowProbe", "",
                                std::type_index (typeid (int)), std::type_index (typeid (void)),
                                std::type_index (typeid (int)), std::type_index (typeid (void))});
    entry_spot->handler_invokers.push_back (
      [&] (void *, void *, service_provider_t &, serializer_registry_t &, const zlink::message_t &,
           const spot_inbound_message_t &) -> task_t<zlink::message_t> {
          ++source_handler_calls;
          co_return zlink::message_t{};
      });
    const auto native_source = std::make_shared<host::public_host_runtime_t> (host::host_options_t{
      .mesh = {.descriptor = {.mesh_name = "no-follow",
                              .node_routing_id = zlink::routing_id_t::from ("actor-a").to_bytes (),
                              .lifecycle_generation = 1,
                              .descriptor_revision = 1,
                              .advertised_endpoint = "tcp://127.0.0.1:0"},
               .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()}});
    if (no_follow) {
        node->native_node = native_source;
        auto &objects = native_source->objects ();
        objects.configure_relocation_state (
          [] (const stateful::object_ref_t &, const std::string &, std::stop_token)
            -> zlink::framework::task_t<
              zlink::framework::runtime::stateful::frozen_object_state_t> {
              stateful::frozen_object_state_t captured;
              captured.application_state = std::vector<std::uint8_t>{1};
              co_return captured;
          },
          [] (const stateful::frozen_object_state_t &, const stateful::object_ref_t &,
              std::stop_token) -> zlink::framework::task_t<bool> { co_return true; });
        const stateful::object_ref_t source_object{
          stateful::object_kind_t::actor, "early-leave-actor", 7, 1, "no-follow", "actor-a"};
        const auto reserved = begin_reserved_object (objects, source_object, "player", {});
        if (objects.commit_create (reserved.attempt) != stateful::stateful_error_t::none)
            return finish (20);
        const auto seal =
          objects.try_seal_relocation_aggregate ({source_object}).result ().value ();
        if (seal.error != stateful::stateful_error_t::none
            || objects.begin_relocation_boundary (seal.seal.token, {}).first
                 != stateful::stateful_error_t::none
            || objects.finalize_relocation_cutover (seal.seal.token).result ().value ()
                 != stateful::stateful_error_t::none)
            return finish (21);
    }
    constexpr auto transfer_id = "transfer-early-leave";
    const auto actor = test_actor_ref ("actor-a", "player", "early-leave-actor", 7);
    const auto target_actor = test_actor_ref ("actor-b", "player", "early-leave-actor", 7);
    const auto key = std::string ("player:early-leave-actor");
    node->actor_instances.emplace (key, std::make_shared<int> (1));
    node->actor_spot_ids.emplace (key, entry_spot->spot_id);
    node->actor_generations.emplace (key, 7);
    if (!node->actor_transfer_coordinator->try_reserve_source (key, transfer_id)
        || !node->actor_transfer_coordinator->try_begin_source_remote (key, transfer_id)) {
        return finish (1);
    }

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
      .source = stateful::object_ref_t{stateful::object_kind_t::actor, "early-leave-actor", 7, 0,
                                       "", "actor-a"},
      .target = stateful::object_ref_t{stateful::object_kind_t::actor, "early-leave-actor", 7, 5,
                                       "", "actor-b"},
      .relocation_reference = transfer_id,
      .checksum_crc32c = 0,
      .inventory_digest = {},
      .target_owner = location_owner_token_t{"owner-b", 9},
      .application_payload = {}};
    node->relocation_authority = authority;

    const auto source_fence = runtime::protocol::actor_route_fence_t{
      "early-leave-actor", 7, zlink::routing_id_t::from ("actor-a").to_bytes (), 1, 1, 1};
    const auto target_fence = runtime::protocol::actor_route_fence_t{
      "early-leave-actor", 7, zlink::routing_id_t::from ("actor-b").to_bytes (), 1, 5, 9};
    const auto leave_command =
      spot_actor_leave_route_command_t{.transfer_id = transfer_id,
                                       .actor_node_rid = "actor-a",
                                       .actor_type = "player",
                                       .actor_id = "early-leave-actor",
                                       .actor_generation = 7,
                                       .source_spot_id = "actor-a-entry",
                                       .source_spot_generation = 0,
                                       .target_spot_id = "spot-b-target",
                                       .target_node_rid = "actor-b",
                                       .target_node_generation = 1,
                                       .target_authority_owner_generation = 5,
                                       .target_owner_lease_generation = 9};
    runtime::messaging::envelope_header_t leave_header;
    leave_header.kind = runtime::messaging::message_kind_t::command;
    leave_header.channel_name = "node";
    leave_header.message_name = spot_actor_leave_route_command_t::packet_name;

    spot_node_runtime_t spots (node);
    service_collection_t services;
    services.add_singleton<actor_gateway_runtime_t> ();
    auto provider = services.build_provider ();
    spots.set_route_client (route_client_t{});
    const runtime::host::ready_record_t owner{.owner_kind = runtime::host::owner_kind_t::node,
                                              .domain = runtime::host::ready_domain_t::application};
    const auto dispatch_leave = [&] (const spot_actor_leave_route_command_t &command,
                                     std::function<void ()> transfer_owner_reservation = {},
                                     std::size_t transferred_owner_byte_cost = 0) {
        auto encoded =
          runtime::messaging::envelope_codec_t{}.encode_parts (leave_header, command, serializers);
        auto parts = std::move (encoded).take_items ();
        runtime::host::receive_record_t record{.kind = runtime::host::record_kind_t::node_send,
                                               .domain =
                                                 runtime::host::ready_domain_t::application};
        record.source_node_rid = zlink::routing_id_t::from ("actor-b");
        record.transferred_owner_byte_cost = transferred_owner_byte_cost;
        const bool owns_terminal = static_cast<bool> (transfer_owner_reservation);
        bool terminal_deferred = false;
        const auto handled = spots.dispatch_mesh_record (
          owner, record, parts,
          zlink::framework::runtime::decode_mesh_record_header (record, parts, false).value (),
          provider, serializers, std::move (transfer_owner_reservation), &terminal_deferred);
        return handled && (!owns_terminal || terminal_deferred);
    };

    const auto lifecycle =
      runtime::serial_work_options_t{runtime::serial_work_lane_t::lifecycle,
                                     runtime::serial_execution_queue_t::fixed_work_byte_cost};
    if (!entry_spot->serial_queue->try_post_async (
          "block-source-lifecycle",
          [&] (auto complete) {
              std::lock_guard lock (blocker_mutex);
              complete_blocker.emplace (std::move (complete));
              blocker_entered = true;
              blocker_changed.notify_all ();
          },
          lifecycle)) {
        return finish (10);
    }
    {
        std::unique_lock lock (blocker_mutex);
        if (!blocker_changed.wait_for (lock, std::chrono::seconds (2),
                                       [&] { return blocker_entered; })) {
            return finish (11);
        }
    }
    if (!entry_spot->serial_queue->try_post (
          "untransferred-source-lifecycle-behind-blocker", [] {}, lifecycle)) {
        return finish (12);
    }

    std::atomic_int owner_reservation_settles{0};
    constexpr auto transferred_owner_byte_cost =
      runtime::serial_execution_queue_t::fixed_work_byte_cost * 2;
    const auto early = dispatch_leave (
      leave_command, [&] { owner_reservation_settles.fetch_add (1, std::memory_order_release); },
      transferred_owner_byte_cost);
    const auto early_state =
      node->lane
        .run ([&] {
            const auto reservation_retained =
              !node->pending_remote_actor_leaves.empty ()
              && static_cast<bool> (
                node->pending_remote_actor_leaves.front ().transfer_owner_reservation)
              && node->pending_remote_actor_leaves.front ().transferred_owner_byte_cost
                   == transferred_owner_byte_cost;
            return std::make_tuple (node->pending_remote_source_cleanups.size (),
                                    node->pending_remote_actor_leaves.size (),
                                    reservation_retained);
        })
        .get ();
    if (!early || leave_calls.load (std::memory_order_acquire) != 0
        || owner_reservation_settles.load (std::memory_order_acquire) != 0
        || std::get<0> (early_state) != 0 || std::get<1> (early_state) != 1
        || !std::get<2> (early_state)) {
        return finish (2);
    }

    std::atomic_int exact_duplicate_reservation_settles{0};
    const auto exact_duplicate = dispatch_leave (
      leave_command,
      [&] { exact_duplicate_reservation_settles.fetch_add (1, std::memory_order_release); },
      transferred_owner_byte_cost);
    if (!exact_duplicate
        || exact_duplicate_reservation_settles.load (std::memory_order_acquire) != 1
        || owner_reservation_settles.load (std::memory_order_acquire) != 0) {
        return finish (13);
    }

    auto conflicting_command = leave_command;
    conflicting_command.target_spot_id = "spot-b-conflict";
    conflicting_command.target_node_generation = 2;
    std::atomic_int conflicting_reservation_settles{0};
    const auto conflict = dispatch_leave (
      conflicting_command,
      [&] { conflicting_reservation_settles.fetch_add (1, std::memory_order_release); },
      transferred_owner_byte_cost);
    const auto pending_after_conflict =
      node->lane.run ([&] { return node->pending_remote_actor_leaves.size (); }).get ();
    if (!conflict || pending_after_conflict != 1
        || conflicting_reservation_settles.load (std::memory_order_acquire) != 1)
        return finish (8);

    const auto swept_active = spots.cleanup_expired_actor_admissions_at (
      std::chrono::steady_clock::now () + std::chrono::seconds (31));
    const auto pending_after_active_sweep =
      node->lane.run ([&] { return node->pending_remote_actor_leaves.size (); }).get ();
    if (swept_active != 0 || pending_after_active_sweep != 1)
        return finish (9);

    const auto completed = finite_task_result (spots.complete_remote_actor_transfer (
      actor, target_actor,
      spot_route_t{node_rid_t::from_string ("actor-b"), spot_id_t ("spot-b-target"), "game"},
      source_fence,
      {target_fence.target_node_routing_id,
       target_fence.target_node_generation,
       {},
       target_fence.owner_lease_generation},
      no_follow ? std::nullopt
                : std::optional<std::uint64_t>{target_fence.authority_owner_generation},
      transfer_id));
    if (!completed || !*completed)
        return finish (3);

    if (no_follow) {
        service_collection_t services;
        auto provider = services.build_provider ();
        actor_gateway_runtime_t gateway;
        const auto late = finite_task_result (spots.relay_actor_packet (
          actor, gateway.actor_context (actor), stream_message_kind_t::request, "NoFollowProbe",
          zlink::message_t{}, provider, serializers));
        if (!late || *late
            || (!late->error () || late->error ()->kind () != framework_error_kind_t::unavailable)
            || source_handler_calls.load () != 0
            || node->actor_transfer_coordinator->phase (key).has_value ())
            return finish (22);
    }
    const auto admitted_leave_state =
      node->lane
        .run ([&] {
            const auto cleanup = std::find_if (
              node->pending_remote_source_cleanups.begin (),
              node->pending_remote_source_cleanups.end (),
              [&] (const auto &candidate) { return candidate.transfer_id == transfer_id; });
            return std::make_tuple (
              cleanup != node->pending_remote_source_cleanups.end () && cleanup->leave_submitted,
              cleanup != node->pending_remote_source_cleanups.end () && cleanup->leave_completed,
              node->pending_remote_actor_leaves.size ());
        })
        .get ();
    if (owner_reservation_settles.load (std::memory_order_acquire) != 1
        || leave_calls.load (std::memory_order_acquire) != 0
        || entry_spot->serial_queue->pending_count (runtime::serial_work_lane_t::lifecycle) != 3
        || !std::get<0> (admitted_leave_state) || std::get<1> (admitted_leave_state)
        || std::get<2> (admitted_leave_state) != 0) {
        return finish (14);
    }
    release_blocker ();

    const auto leave_deadline = std::chrono::steady_clock::now () + std::chrono::seconds (2);
    struct leave_state_t
    {
        bool cleanup_present = false;
        bool leave_submitted = false;
        bool leave_completed = false;
        std::size_t pending_leaves = 0;
    };
    leave_state_t leave_state;
    do {
        leave_state =
          node->lane
            .run ([&] {
                leave_state_t state;
                const auto cleanup = std::find_if (
                  node->pending_remote_source_cleanups.begin (),
                  node->pending_remote_source_cleanups.end (),
                  [&] (const auto &candidate) { return candidate.transfer_id == transfer_id; });
                state.cleanup_present = cleanup != node->pending_remote_source_cleanups.end ();
                if (state.cleanup_present) {
                    state.leave_submitted = cleanup->leave_submitted;
                    state.leave_completed = cleanup->leave_completed;
                }
                state.pending_leaves = node->pending_remote_actor_leaves.size ();
                return state;
            })
            .get ();
        if (leave_calls.load (std::memory_order_acquire) == 1 && leave_state.cleanup_present
            && leave_state.leave_submitted && leave_state.leave_completed
            && leave_state.pending_leaves == 0) {
            break;
        }
        std::this_thread::yield ();
    } while (std::chrono::steady_clock::now () < leave_deadline);
    if (leave_calls.load (std::memory_order_acquire) != 1 || !leave_state.cleanup_present
        || !leave_state.leave_submitted || !leave_state.leave_completed
        || leave_state.pending_leaves != 0) {
        return finish (4);
    }

    const auto duplicate = dispatch_leave (leave_command);
    if (!duplicate)
        return finish (5);
    node->worker_executor->drain ();
    if (leave_calls.load (std::memory_order_acquire) != 1)
        return finish (6);

    (void) spots.cleanup_expired_actor_admissions_at (std::chrono::steady_clock::now ()
                                                      + std::chrono::seconds (31));
    const auto late_duplicate = dispatch_leave (leave_command);
    const auto pending_after_late_duplicate =
      node->lane.run ([&] { return node->pending_remote_actor_leaves.size (); }).get ();
    const auto passed = finish (late_duplicate && pending_after_late_duplicate == 0 ? 0 : 7);
    zlink::framework::detail::spot_node_runtime_t (node).release_native_handles ();
    return passed;
}
} // namespace zlink::framework::test::actor_gateway
