/* SPDX-License-Identifier: Elastic-2.0 */

#include "runtime/backend/raw_binding_adapter.hpp"
#include "../support/m6b_cases.hpp"
#include "../support/m6b_host_access.hpp"
#include "../support/m6b_task.hpp"
#include "../support/m6b_mesh_fixture.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include "runtime/locations/actor_authority_payload.hpp"
#include "runtime/stateful/raw_stateful_dispatch.hpp"
#include "runtime/stateful/public_host_runtime.hpp"
#include "runtime/stateful/stateful_object_runtime.hpp"
#include "runtime/spots/actor_transfer_coordinator.hpp"
#include "runtime/utils/relocation_id_generator.hpp"
#include <zlink/Contracts/Messaging/message.hpp>
#include <nlohmann/json.hpp>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <fstream>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mesh = zlink::framework::runtime::mesh;
namespace protocol = zlink::framework::runtime::protocol;
namespace stateful = zlink::framework::runtime::stateful;
namespace host = zlink::framework::runtime::host;
namespace spots = zlink::framework::detail;
using namespace std::chrono_literals;

namespace zlink::framework::test::m6b
{
void verify_actor_commit_is_replayable_until_deadline ()
{
    using namespace zlink::framework;
    spots::actor_transfer_coordinator_t coordinator;
    std::size_t activity = 0;
    coordinator.set_activity_handler ([&] { ++activity; });
    assert (!coordinator.next_activity ());
    const auto source = detail::actor_ref_access_t::make (node_rid_t::from_string ("node-a"),
                                                          "player", "actor-commit-replay", 7);
    detail::pending_actor_admission_t admission{.actor_key = "player:actor-commit-replay",
                                                .source_actor = source,
                                                .source_spot_id = "spot-a",
                                                .target_spot_id = "spot-b",
                                                .deadline = std::chrono::steady_clock::now () + 30s,
                                                .completion_operation_id_high = 11,
                                                .completion_operation_id_low = 12};
    assert (coordinator.try_add_admission ("transfer-replay", admission));
    assert (activity == 1);
    assert (coordinator.next_activity () == admission.deadline);
    assert (coordinator.stage_session_relocation_route ("transfer-replay", {0x41}, "player", 17));
    assert (coordinator.commit_session_relocation_route_authority ("transfer-replay", 40, 41));
    assert (coordinator.commit_session_relocation_route_authority ("transfer-replay", 40, 41));
    assert (!coordinator.commit_session_relocation_route_authority ("transfer-replay", 40, 42));
    std::vector<detail::handoff_packet_t> discarded;
    assert (coordinator.begin_commit ("transfer-replay", source, "spot-b", discarded));
    assert (discarded.empty ());
    assert (!coordinator.next_activity ());
    coordinator.complete_commit ("transfer-replay");
    assert (coordinator.next_activity () == admission.deadline);
    assert (!coordinator.pending_commit ("transfer-replay", source, "spot-b"));
    const auto completed = coordinator.completed_commit ("transfer-replay", source, "spot-b");
    assert (completed);
    assert (completed->session_relocation_committed_previous_authority_owner_generation == 40);
    assert (completed->session_relocation_committed_target_authority_owner_generation == 41);
    assert (!coordinator.completed_commit ("transfer-replay", source, "different-spot"));
    assert (!coordinator.try_add_admission ("transfer-replay", std::move (admission)));
    assert (!coordinator.phase ("player:actor-commit-replay"));

    // Session route update is one-way. Once the target commit closes, a
    // successor move is not gated on an application-level terminal ACK.
    assert (coordinator.try_reserve_source ("player:actor-commit-replay", "transfer-successor"));
    assert (coordinator.phase ("player:actor-commit-replay")
            == spots::actor_move_phase_t::source_reserved);
    coordinator.cancel_move ("player:actor-commit-replay");

    auto return_admission = *completed;
    return_admission.source_spot_id = "spot-b";
    return_admission.target_spot_id = "spot-a";
    assert (coordinator.try_add_admission ("transfer-return", return_admission));
    assert (coordinator.phase ("player:actor-commit-replay")
            == spots::actor_move_phase_t::target_pending);
    coordinator.fail_commit ("transfer-return", false);
    assert (!coordinator.phase ("player:actor-commit-replay"));

    (void) await_task (
      coordinator.cleanup_expired_async (std::chrono::steady_clock::now () + 31s, {}));
    assert (!coordinator.next_activity ());
}

void verify_reconcile_management_deadline_advances_after_each_attempt ()
{
    spots::actor_transfer_coordinator_t coordinator;
    coordinator.mark_reconcile ("player:reconcile-deadline", 1s);
    const auto first = coordinator.next_activity ();
    assert (first);
    assert (coordinator.take_due_reconciles (*first - 1ms).empty ());
    assert (coordinator.take_due_reconciles (*first).size () == 1);
    const auto next = coordinator.next_activity ();
    assert (next == *first + std::chrono::milliseconds (100));
    assert (coordinator.take_due_reconciles (*first).empty ());
    assert (coordinator.take_due_reconciles (*next).size () == 1);
    coordinator.cancel_move ("player:reconcile-deadline");
    assert (!coordinator.next_activity ());
}

void verify_terminal_journal_preserves_outstanding_entries ()
{
    using journal_t = host::relocation_detail::terminal_journal_t<std::string, int, int>;
    using admission_kind_t = journal_t::admission_kind_t;
    const auto now = journal_t::clock_t::now ();
    journal_t journal (10ms);

    assert (journal.try_begin ("outstanding", 1, now).kind == admission_kind_t::admitted);
    assert (journal.try_begin ("completed", 2, now).kind == admission_kind_t::admitted);
    assert (journal.complete ("completed", 2, 20, now));
    assert (journal.try_begin ("beyond-former-capacity", 3, now).kind
            == admission_kind_t::admitted);
    assert (journal.size () == 3);
    assert (journal.try_begin ("outstanding", 1, now).kind == admission_kind_t::pending);
    assert (journal.try_begin ("outstanding", 9, now).kind == admission_kind_t::conflicting);

    assert (journal.try_begin ("after-expiry", 3, now + 11ms).kind == admission_kind_t::admitted);
    assert (journal.size () == 3);
    assert (journal.try_begin ("outstanding", 1, now + 11ms).kind == admission_kind_t::pending);
    assert (journal.complete ("after-expiry", 3, 30, now + 11ms));
    const auto replay = journal.try_begin ("after-expiry", 3, now + 11ms);
    assert (replay.kind == admission_kind_t::replay && replay.terminal == 30);
}

void verify_unbounded_actor_handoff_backlog ()
{
    using namespace zlink::framework;
    using detail::handoff_append_result_t;
    using detail::handoff_packet_t;

    const std::string actor_key = "player:actor-handoff-backlog";
    spots::actor_transfer_coordinator_t coordinator;
    assert (coordinator.try_begin_source_remote (actor_key, "transfer-1"));

    const auto packet_with_payload = [] (std::size_t bytes) {
        handoff_packet_t packet;
        packet.payload.resize (bytes);
        return packet;
    };
    //  The backlog has no record-count or stored-size bound.
    for (std::size_t index = 0; index != 2048; ++index) {
        assert (coordinator.try_append_backlog (actor_key, packet_with_payload (1))
                == handoff_append_result_t::appended);
    }
    assert (coordinator.take_backlog (actor_key).size () == 2048);

    assert (coordinator.try_append_backlog (actor_key, packet_with_payload (64u * 1024u * 1024u))
            == handoff_append_result_t::appended);
    assert (coordinator.try_append_backlog (actor_key, packet_with_payload (64u * 1024u * 1024u))
            == handoff_append_result_t::appended);
    assert (coordinator.take_backlog (actor_key).size () == 2);
    assert (coordinator.complete_move (actor_key).has_value ());

    const std::string replay_key = "player:actor-handoff-replay";
    assert (coordinator.try_begin_local (replay_key));
    assert (coordinator.try_append_backlog (replay_key, packet_with_payload (1))
            == handoff_append_result_t::appended);
    auto first_replay = coordinator.finish_move_replay (replay_key);
    assert (!first_replay.completed && first_replay.backlog.size () == 1);
    assert (coordinator.try_append_backlog (replay_key, packet_with_payload (2))
            == handoff_append_result_t::appended);
    auto late_replay = coordinator.finish_move_replay (replay_key);
    assert (!late_replay.completed && late_replay.backlog.size () == 1);
    assert (coordinator.finish_move_replay (replay_key).completed);

    const std::string reserved_key = "player:actor-handoff-reserved";
    assert (coordinator.try_reserve_source (reserved_key));
    assert (!coordinator.try_reserve_source (reserved_key));
    assert (!coordinator.blocks_dispatch (reserved_key));
    std::vector<handoff_packet_t> reserved_queue;
    for (const auto size : {1u, 2u}) {
        auto packet = packet_with_payload (size);
        assert (coordinator.try_append_backlog (reserved_key, packet)
                == handoff_append_result_t::not_moving);
        reserved_queue.push_back (std::move (packet));
    }
    assert (coordinator.take_backlog (reserved_key).empty ());
    assert (coordinator.try_begin_source_remote (reserved_key, "transfer-reserved"));
    assert (coordinator.blocks_dispatch (reserved_key));
    coordinator.prepend_source_suffix (reserved_key, std::move (reserved_queue));
    assert (coordinator.try_append_backlog (reserved_key, packet_with_payload (3))
            == handoff_append_result_t::appended);
    const auto reserved_backlog = coordinator.take_backlog (reserved_key);
    assert (reserved_backlog.size () == 3);
    assert (reserved_backlog[0].payload.size () == 1);
    assert (reserved_backlog[1].payload.size () == 2);
    assert (reserved_backlog[2].payload.size () == 3);
    assert (coordinator.complete_move (reserved_key).has_value ());

    const std::string cancelled_key = "player:actor-handoff-cancelled";
    assert (coordinator.try_reserve_source (cancelled_key));
    auto cancelled_packet = packet_with_payload (1);
    assert (coordinator.try_append_backlog (cancelled_key, cancelled_packet)
            == handoff_append_result_t::not_moving);
    assert (coordinator.take_backlog (cancelled_key).empty ());
    assert (coordinator.try_begin_local (cancelled_key));
    assert (coordinator.try_append_backlog (cancelled_key, cancelled_packet)
            == handoff_append_result_t::appended);
    coordinator.cancel_move (cancelled_key);
    assert (!coordinator.phase (cancelled_key));
    assert (coordinator.take_backlog (cancelled_key).empty ());
}
void verify_relocation_finalize_write_lowers_published_bound ()
{
    // An attempt inserted unscheduled is excluded from a management pass that
    // runs before its first finalize instant is written. That write must make
    // the attempt due again; without it the bound stays never-due.
    auto host = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = descriptor ("finalize-bound"),
        .runtime_failures =
          std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()}});
    using access = host::public_host_runtime_test_access_t;
    const access::key_t key{1, 2, 3};
    const auto now = std::chrono::steady_clock::now ();
    access::insert_unscheduled_target_attempt (*host, key);
    assert (access::relocation_due (*host, now));
    access::publish_relocation_bound (*host);
    assert (!access::relocation_due (*host, now + 24h));
    access::schedule_finalize (*host, key, now + 1h);
    assert (!access::relocation_due (*host, now));
    assert (access::relocation_due (*host, now + 1h));
}

void verify_pending_relocation_management_bounds_dispatch_wait ()
{
    auto host = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = descriptor ("management-deadline"),
        .runtime_failures =
          std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()}});
    host->start ();
    const auto local = host->transport ().topology ().local_descriptor ();
    const protocol::relocation_coordinator_fence_t coordinator{
      "management-owner", 7, local.node_routing_id, local.lifecycle_generation, "management-store"};
    const protocol::request_source_fence_t source{"request-owner", 9, bytes ("source"), 11};
    const protocol::reply_relay_t relay{
      {1, 2}, 3, {4, 5}, 6, coordinator, 7, 8, 0, protocol::framework_error_code::none};
    assert (host->relocation_wire ().register_terminal_target (
      {relay, source, std::nullopt, [] (protocol::reply_relay_ack_status_t) { return true; },
       [] { return true; }}));
    assert (host->relocation_wire ().pending_terminal_relays () == 1);
    const auto next_activity = host->relocation_wire ().next_activity ();
    assert (next_activity
            == stateful::raw_relocation_replay_coordinator_t::clock_t::time_point::min ());
    // A zero-time poll can run before Core publishes the registration wake.
    // Wait on the transport to consume that exact notification, independently
    // of the host's already-due relocation deadline.
    assert (host->transport ().wait_for_activity (-1ms, false));
    // An unbounded caller wait must become nonblocking at the due deadline.
    // Without the bound this hangs and the existing CTest watchdog fails it;
    // there is no worker scheduling race or elapsed-time assertion.
    assert (!host->wait_for_dispatch_activity (-1ms, false, next_activity));
    assert (host->relocation_wire ().pending_terminal_relays () == 1);
    host->close ();
}

void verify_failed_relay_persistence_rearms_dispatch_wait ()
{
    for (const bool source_lease_expiry : {false, true}) {
        auto host = std::make_shared<host::public_host_runtime_t> (
          host::host_options_t{mesh::raw_mesh_node_options_t{
            .descriptor = descriptor ("relay-persist-wake"),
            .runtime_failures =
              std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()}});
        host->start ();
        const auto local = host->transport ().topology ().local_descriptor ();
        const protocol::relocation_coordinator_fence_t coordinator{
          "management-owner", 7, local.node_routing_id, local.lifecycle_generation,
          "management-store"};
        const protocol::request_source_fence_t source{"request-owner", 9, bytes ("source"), 11};
        const protocol::reply_relay_t relay{
          {1, 2}, 3, {4, 5}, 6, coordinator, 7, 8, 0, protocol::framework_error_code::none};
        std::promise<void> persist_entered;
        auto entered = persist_entered.get_future ();
        std::promise<void> release_persist;
        auto released = release_persist.get_future ().share ();
        const auto fail_persist = [&] {
            persist_entered.set_value ();
            released.wait ();
            return false;
        };
        assert (host->relocation_wire ().register_terminal_target (
          {relay, source, std::nullopt,
           [&] (protocol::reply_relay_ack_status_t) { return fail_persist (); }, fail_persist}));
        const protocol::reply_relay_ack_t ack{
          relay.relocation,     coordinator, relay.operation,
          relay.reply_route_id, source,      protocol::reply_relay_ack_status_t::terminal_received};
        const auto record = mesh::service_mailbox_record_t::from_parts (
          "relay", mesh::service_mailbox_domain_t::infrastructure,
          zlink::framework::detail::backend::materialize_binding_parts (
            {protocol::encode_reply_relay_ack (ack)}),
          source.node_routing_id, std::nullopt, source.node_generation);
        auto persisting = std::async (std::launch::async, [&] {
            if (source_lease_expiry)
                return host->relocation_wire ().confirm_terminal_source_lease_expired (
                  relay.relocation, relay.operation, source);
            return await_task (host->relocation_wire ().process (record))
                   != stateful::raw_relocation_replay_result_t::persistence_failed;
        });
        entered.wait ();
        assert (!host->relocation_wire ().next_activity ());
        // Drain the original registration wake while persistence has made
        // the relay temporarily ineligible for retry.
        assert (host->transport ().wait_for_activity (-1ms, false));
        assert (!host->relocation_wire ().next_activity ());
        release_persist.set_value ();
        assert (!persisting.get ());
        const auto next_activity = host->relocation_wire ().next_activity ();
        assert (next_activity
                == stateful::raw_relocation_replay_coordinator_t::clock_t::time_point::min ());
        // Observe the new notification separately from the due deadline:
        // either alone must not stand in for the other.
        assert (host->transport ().wait_for_activity (-1ms, false));
        assert (!host->wait_for_dispatch_activity (-1ms, false, next_activity));
        assert (host->relocation_wire ().pending_terminal_relays () == 1);
        host->close ();
    }
}
// Spec 28 §3/§12: the exact identity is RelocationId + targetAttemptGeneration
// + coordinator fence (+ object). A chunk carrying a different exact
// identity than the in-progress assembly is discarded without being linked
// to that assembly -- it must not corrupt, complete, or fail the assembly
// that owns the identity actually in progress.
void verify_relocation_assembly_rejects_mismatched_identity_chunk ()
{
    const protocol::relocation_id_t relocation{0x51, 0x52};
    const protocol::relocation_coordinator_fence_t coordinator{
      "coordinator", 7, bytes ("coordinator-node"), 9, "store-v1"};
    const protocol::relocation_object_t object{
      protocol::relocation_object_kind_t::actor, {}, "assembly-actor", 2, 4};
    const std::vector<std::uint8_t> payload = bytes ("assembly-payload");
    const auto manifest = stateful::plan_relocation_payload (payload, payload.size ());
    assert (manifest.chunk_count == 1);

    stateful::relocation_state_assembly_t assembly (relocation, 31, coordinator, object, manifest);

    // A different RelocationId is a mismatched exact identity.
    auto wrong_relocation = stateful::make_relocation_state_chunk (
      protocol::relocation_id_t{0x61, 0x62}, 31, coordinator, object, payload, 0, payload.size ());
    assert (assembly.accept (wrong_relocation) == stateful::relocation_assembly_result_t::ignored);

    // A different targetAttemptGeneration is a mismatched exact identity.
    auto wrong_generation = stateful::make_relocation_state_chunk (
      relocation, 32, coordinator, object, payload, 0, payload.size ());
    assert (assembly.accept (wrong_generation) == stateful::relocation_assembly_result_t::ignored);

    // A different coordinator fence is a mismatched exact identity.
    protocol::relocation_coordinator_fence_t wrong_coordinator = coordinator;
    wrong_coordinator.lease_generation = coordinator.lease_generation + 1;
    auto wrong_fence = stateful::make_relocation_state_chunk (relocation, 31, wrong_coordinator,
                                                              object, payload, 0, payload.size ());
    assert (assembly.accept (wrong_fence) == stateful::relocation_assembly_result_t::ignored);

    // None of the mismatched-identity arrivals touched the assembly bound
    // to the exact identity actually in progress: the matching chunk still
    // completes it cleanly afterward.
    assert (!assembly.complete ());
    assert (!assembly.failed ());
    auto matching = stateful::make_relocation_state_chunk (relocation, 31, coordinator, object,
                                                           payload, 0, payload.size ());
    assert (assembly.accept (matching) == stateful::relocation_assembly_result_t::completed);
    assert (assembly.complete ());
    assert (!assembly.failed ());
    assert (assembly.take_payload () == payload);
}

// Spec 28 §12: "On a checksum mismatch, the target doesn't proceed to CAS,
// doesn't restore from a partially assembled payload, and responds with an
// explicit failure reply." The manifest's declared checksum_crc32c came
// from the source's own payload (relocation_transfer.hpp's
// plan_relocation_payload); a chunk whose exact identity matches but whose
// bytes don't hash to that checksum must fail the assembly explicitly, and
// the partially assembled payload must not survive for restore.
void verify_relocation_assembly_rejects_checksum_mismatch ()
{
    const protocol::relocation_id_t relocation{0x53, 0x54};
    const protocol::relocation_coordinator_fence_t coordinator{
      "coordinator", 7, bytes ("coordinator-node"), 9, "store-v1"};
    const protocol::relocation_object_t object{
      protocol::relocation_object_kind_t::actor, {}, "checksum-actor", 2, 4};
    const std::vector<std::uint8_t> honest_payload = bytes ("honest-payload");
    // The manifest declares the checksum of the honest payload, exactly as
    // the source's plan_relocation_payload would when the sender computes
    // it from its own captured bytes.
    const auto manifest =
      stateful::plan_relocation_payload (honest_payload, honest_payload.size ());
    assert (manifest.chunk_count == 1);

    stateful::relocation_state_assembly_t assembly (relocation, 41, coordinator, object, manifest);

    // A single chunk carrying the exact identity in the manifest, but with
    // bytes that don't hash to the manifest's checksum -- the contracted
    // fault-injection point spec 28 §12 describes: the wire bytes a target
    // actually receives diverge from what its own manifest declares.
    const std::vector<std::uint8_t> corrupted_payload = bytes ("dishonest-payloadX");
    auto corrupted_chunk = stateful::make_relocation_state_chunk (
      relocation, 41, coordinator, object, corrupted_payload, 0, honest_payload.size ());
    assert (corrupted_chunk.chunk_data != honest_payload);
    // Pin that this exercises the CRC compare (accept()'s last check), not
    // an earlier length guard: the chunk must land exactly at the
    // manifest's declared total_length so every length/ordinal guard
    // passes and only the checksum can fail it.
    assert (corrupted_chunk.chunk_data.size () == manifest.total_length);
    assert (assembly.accept (corrupted_chunk) == stateful::relocation_assembly_result_t::conflict);

    // No CAS, no restore from the partial payload: the assembly is failed
    // and holds nothing an accidental subsequent read could restore from.
    assert (assembly.failed ());
    assert (!assembly.complete ());
    assert (assembly.take_payload ().empty ());
}

void verify_relocation_id_generation_retries_collisions ()
{
    const std::vector<protocol::relocation_id_t> candidates{
      {0, 0}, {0x11, 0x22}, {0x11, 0x22}, {0x33, 0x44}};
    std::size_t next = 0;
    zlink::framework::runtime::relocation_id_generator_t generator (
      [&] { return candidates.at (next++); });
    assert ((generator.issue () == protocol::relocation_id_t{0x11, 0x22}));
    assert ((generator.issue () == protocol::relocation_id_t{0x33, 0x44}));
    assert (next == candidates.size ());
}

void verify_draining_target_relocation_prepare (bool already_accepted)
{
    auto source = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = descriptor ("prepare-source"),
        .runtime_failures =
          std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()}});
    auto target = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = descriptor ("prepare-target"),
        .runtime_failures =
          std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()}});
    int prepares = 0;
    target->configure_actor_join_relocation (
      [&] (const protocol::relocation_prepare_t &) -> std::optional<bool> {
          ++prepares;
          return std::nullopt;
      },
      [] (stateful::frozen_object_state_t &, const stateful::object_ref_t &,
          const protocol::relocation_prepare_t &) -> zlink::framework::task_t<bool> {
          co_return true;
      },
      [] (const stateful::object_ref_t &)
        -> std::optional<zlink::framework::runtime::actor_authority_payload_t> {
          return std::nullopt;
      },
      [] (const std::string &, const stateful::object_ref_t &) -> zlink::framework::task_t<void> {
          co_return;
      },
      [] (const stateful::object_ref_t &, std::uint64_t, std::uint64_t) { return true; });
    source->start ();
    target->start ();
    target->configure_session_route_owner ([] {
        return std::make_optional (zlink::framework::location_owner_token_t{"prepare-owner", 7});
    });
    const auto source_status = source->status ();
    const auto target_status = target->status ();
    assert (source->connect_peer (target->transport ().endpoint (), target_status.routing_id ()));
    const auto dispatch = [] (const host::ready_record_t &, const host::receive_record_t &,
                              std::vector<zlink::message_t>, auto) {};
    const auto deadline = std::chrono::steady_clock::now () + 5s;
    while (
      (!ready_peer (source->transport ().topology (), target_status.routing_id ().to_bytes ())
       || !ready_peer (target->transport ().topology (), source_status.routing_id ().to_bytes ()))
      && std::chrono::steady_clock::now () < deadline) {
        (void) source->dispatch_ready (dispatch);
        (void) target->dispatch_ready (dispatch);
    }
    const protocol::relocation_prepare_t prepare{
      {801, 802},
      1,
      {"prepare-coordinator", 1, source_status.routing_id ().to_bytes (),
       source_status.lifecycle_generation (), "prepare-store"},
      {target_status.routing_id ().to_bytes (), target_status.lifecycle_generation (),
       "prepare-owner", 7},
      protocol::relocation_role_t::source,
      {protocol::relocation_object_kind_t::actor, {}, "prepare-actor", 1, 1},
      source_status.routing_id ().to_bytes (),
      source_status.lifecycle_generation (),
      1,
      1,
      1,
      1,
      static_cast<std::uint64_t> (std::numeric_limits<std::int64_t>::max ())};
    std::optional<zlink::framework::task_t<mesh::relocation_prepare_response_t>> initial;
    if (already_accepted) {
        initial.emplace (source->transport ().request_relocation_prepare (
          target_status.routing_id ().to_bytes (), prepare, 2s));
        while (prepares < 1 && std::chrono::steady_clock::now () < deadline) {
            (void) await_task (target->dispatch_ready (dispatch));
            (void) source->dispatch_ready (dispatch);
        }
        assert (prepares == 1);
        assert (!initial->await_ready ());
    }
    await_task (target->transport ().publish_draining ());
    assert (target->status ().state == host::node_status_t::state_t::draining);
    auto pending = source->transport ().request_relocation_prepare (
      target_status.routing_id ().to_bytes (), prepare, 2s);
    if (already_accepted) {
        while (prepares < 2 && std::chrono::steady_clock::now () < deadline) {
            (void) await_task (target->dispatch_ready (dispatch));
            (void) source->dispatch_ready (dispatch);
        }
        assert (prepares == 2);
        const auto chunk = stateful::make_relocation_state_chunk (
          prepare.relocation, prepare.target_attempt_generation, prepare.coordinator,
          prepare.object, std::vector<std::uint8_t>{1}, 0, 1);
        assert (await_task (source->transport ().send_relocation_control (
          target_status.routing_id ().to_bytes (), chunk)));
    }
    // Messaging hot path §4.1: a reply completes in a receive rotation, which
    // this test drives with dispatch_ready. Both replies can arrive in
    // different rotations, so keep rotating until both requests complete.
    while ((!pending.await_ready () || (initial && !initial->await_ready ()))
           && std::chrono::steady_clock::now () < deadline) {
        (void) target->dispatch_ready (dispatch);
        (void) source->dispatch_ready (dispatch);
    }
    assert (pending.await_ready ());
    assert (!initial || initial->await_ready ());
    const auto response = await_task (std::move (pending));
    assert (!response.ready);
    if (already_accepted) {
        assert (response.failed);
        const auto accepted = await_task (std::move (*initial));
        assert (accepted.failed);
        assert (accepted.failed->failure_code
                == static_cast<std::uint32_t> (protocol::framework_error_code::relocationDataLost));
        assert (response.failed->failure_code == accepted.failed->failure_code);
    } else {
        assert (response.failed);
        assert (response.failed->failure_code
                == static_cast<std::uint32_t> (protocol::framework_error_code::routeNotConnected));
    }
    target->close ();
    source->close ();
}

// Code-only receive classification preserves verified integrity failures and
// keeps InternalFailure distinct from DataLost, including unknown-code fallback.
void verify_relocation_failure_code_classification_is_distinct ()
{
    using zlink::framework::framework_error_kind_t;
    assert (host::classify_relocation_failure_code (
              static_cast<std::uint32_t> (protocol::framework_error_code::relocationDataLost))
            == zlink::framework::framework_error_kind_t::data_lost);
    assert (host::classify_relocation_failure_code (
              static_cast<std::uint32_t> (protocol::framework_error_code::requestFailed))
            == zlink::framework::framework_error_kind_t::internal_failure);
    assert (host::classify_relocation_failure_reason (
              static_cast<std::uint32_t> (protocol::framework_error_code::relocationDataLost))
            == stateful::relocation_reason_t::checksum_mismatch);
    // A remote node's full queue is not a source permit failure (spec 32:
    // foreign capacity classifies as Unavailable).
    assert (host::classify_relocation_failure_reason (
              static_cast<std::uint32_t> (protocol::framework_error_code::workerQueueFull))
            == stateful::relocation_reason_t::restore_failed);
    assert (host::classify_relocation_failure_code (
              static_cast<std::uint32_t> (protocol::framework_error_code::relocationDataLost))
            != host::classify_relocation_failure_code (
              static_cast<std::uint32_t> (protocol::framework_error_code::requestFailed)));
    std::ifstream mapping_input (ZLINK_ERROR_MAPPING_CONFORMANCE_PATH);
    const auto fixture = nlohmann::json::parse (mapping_input);
    const std::map<std::string, framework_error_kind_t> kinds{
      {"NotFound", framework_error_kind_t::not_found},
      {"AlreadyExists", framework_error_kind_t::already_exists},
      {"TypeMismatch", framework_error_kind_t::type_mismatch},
      {"Rejected", framework_error_kind_t::rejected},
      {"Unavailable", framework_error_kind_t::unavailable},
      {"DeadlineExceeded", framework_error_kind_t::deadline_exceeded},
      {"ProtocolError", framework_error_kind_t::protocol_error},
      {"InvalidOperation", framework_error_kind_t::invalid_operation},
      {"DataLost", framework_error_kind_t::data_lost},
      {"InternalFailure", framework_error_kind_t::internal_failure}};
    for (const auto &row : fixture.at ("receive"))
        assert (host::classify_relocation_failure_code (row.at ("failureCode"))
                == kinds.at (row.at ("kind")));
    // An unrecognized wire code is internal_failure, not ignored/dropped.
    assert (host::classify_relocation_failure_code (999999u)
            == zlink::framework::framework_error_kind_t::internal_failure);
}

/* #2295: dispatch_ready suspends on the bound-session owner lookup, the
 * caller's callbacks go out of scope, and the coroutine resumes and calls
 * them. The coroutine must own the callbacks; ASan reports the use after
 * scope when it only holds references. */
}
