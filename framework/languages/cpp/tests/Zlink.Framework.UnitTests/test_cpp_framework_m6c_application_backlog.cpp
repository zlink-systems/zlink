/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/m6c_runtime_cases.hpp"
#include "../support/m6c_stateful_fixture.hpp"
#include "../support/m6c_task.hpp"
#include "runtime/dispatch/dispatch_limits.hpp"
#include "runtime/stateful/maintenance_runtime.hpp"
#include "runtime/stateful/raw_stateful_dispatch.hpp"
#include "runtime/timers/async_delay.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace m6c_test
{

using namespace zlink::framework::runtime::stateful;

void test_stateful_application_queue_accepts_active_backlog (test_context_t &test)
{
    namespace limits = zlink::framework::runtime::dispatch_limits;
    const auto fixed = limits::fixed_work_byte_cost;
    stateful_object_runtime_t count_limited;
    const auto count_actor = create_actor (count_limited, "active-count-actor");
    test.require (count_limited.enqueue (count_actor, turn_domain_t::application, {1, {1}})
                    == stateful_error_t::none,
                  "active reservation test must enqueue its first application turn");
    const auto [count_claim_error, count_claim] =
      count_limited.try_claim (count_actor, turn_domain_t::application);
    test.require (count_claim_error == stateful_error_t::none && count_claim,
                  "active reservation test must claim the first application turn");
    test.require (count_limited.enqueue (count_actor, turn_domain_t::application, {2, {2}})
                    == stateful_error_t::none,
                  "an active turn must not impose a second application queue limit");
    if (count_claim) {
        test.require (count_limited.complete_claim (count_actor, turn_domain_t::application)
                        == stateful_error_t::none,
                      "active count reservation must release at handler completion");
    }
    const auto [count_second_error, count_second] =
      count_limited.try_claim (count_actor, turn_domain_t::application);
    test.require (count_second_error == stateful_error_t::none && count_second
                    && count_second->sequence == 2,
                  "queued work must remain available after the active turn completes");
    if (count_second)
        test.require (count_limited.complete_claim (count_actor, turn_domain_t::application)
                        == stateful_error_t::none,
                      "queued count-regression turn must complete");

    stateful_object_runtime_t byte_limited;
    const auto byte_actor = create_actor (byte_limited, "active-byte-actor");
    test.require (byte_limited.enqueue (byte_actor, turn_domain_t::application,
                                        {1, std::vector<std::uint8_t> (4, 0x41)})
                    == stateful_error_t::none,
                  "active byte reservation test must enqueue its first turn");
    const auto [byte_claim_error, byte_claim] =
      byte_limited.try_claim (byte_actor, turn_domain_t::application);
    test.require (byte_claim_error == stateful_error_t::none && byte_claim,
                  "active byte reservation test must claim the first turn");
    test.require (byte_limited.enqueue (byte_actor, turn_domain_t::application, {2, {}})
                    == stateful_error_t::none,
                  "payload bytes must not impose a Framework queue limit");
    if (byte_claim) {
        test.require (byte_limited.complete_claim (byte_actor, turn_domain_t::application)
                        == stateful_error_t::none,
                      "active byte reservation must release at handler completion");
    }
    const auto [byte_second_error, byte_second] =
      byte_limited.try_claim (byte_actor, turn_domain_t::application);
    test.require (byte_second_error == stateful_error_t::none && byte_second
                    && byte_second->sequence == 2,
                  "byte-regression work must remain queued after completion");
    if (byte_second)
        test.require (byte_limited.complete_claim (byte_actor, turn_domain_t::application)
                        == stateful_error_t::none,
                      "queued byte-regression turn must complete");

    stateful_object_runtime_t multipart_accounting;
    const auto multipart_actor = create_actor (multipart_accounting, "multipart-accounting-actor");
    test.require (multipart_accounting.enqueue (multipart_actor, turn_domain_t::application,
                                                {1, std::vector<std::uint8_t> (128, 0x43), 4})
                    == stateful_error_t::none,
                  "application HWM must exclude the canonical multipart envelope");
    const auto [multipart_claim_error, multipart_claim] =
      multipart_accounting.try_claim (multipart_actor, turn_domain_t::application);
    test.require (multipart_claim_error == stateful_error_t::none && multipart_claim,
                  "multipart accounting test must claim the admitted application turn");
    test.require (
      multipart_accounting.enqueue (multipart_actor, turn_domain_t::application, {2, {}})
        == stateful_error_t::none,
      "multipart payload accounting must not become a queue admission cap");
    if (multipart_claim) {
        test.require (
          multipart_accounting.complete_claim (multipart_actor, turn_domain_t::application)
            == stateful_error_t::none,
          "multipart payload reservation must release at completion");
    }
    const auto [multipart_second_error, multipart_second] =
      multipart_accounting.try_claim (multipart_actor, turn_domain_t::application);
    test.require (multipart_second_error == stateful_error_t::none && multipart_second
                    && multipart_second->sequence == 2,
                  "multipart backlog must survive the active turn");
    if (multipart_second)
        test.require (
          multipart_accounting.complete_claim (multipart_actor, turn_domain_t::application)
            == stateful_error_t::none,
          "multipart backlog turn must complete");

    stateful_object_runtime_t progressive_restore;
    bool materialized = false;
    bool lifecycle_committed = false;
    bool lifecycle_saw_closed_dispatch = false;
    bool relocation_aborted = false;
    object_ref_t restore_target{
      object_kind_t::actor, "progressive-restore-actor", 1, 2, "mesh", "node-b"};
    progressive_restore.configure_relocation_materialization (
      [&] (const frozen_object_state_t &, const object_ref_t &target,
           const std::optional<object_ref_t> &parent, std::stop_token) {
          materialized = target == restore_target && !parent;
          return zlink::framework::task_t<bool> (
            zlink::framework::result_t<bool>::success (materialized));
      },
      [&] (const std::vector<object_ref_t> &targets) {
          lifecycle_committed = targets == std::vector<object_ref_t>{restore_target};
          const auto [claim_error, claimed] =
            progressive_restore.try_claim (restore_target, turn_domain_t::application);
          lifecycle_saw_closed_dispatch = claim_error == stateful_error_t::moving && !claimed;
          return lifecycle_committed && lifecycle_saw_closed_dispatch;
      },
      [&] (const std::vector<object_ref_t> &) -> zlink::framework::task_t<void> {
          relocation_aborted = true;
          co_return;
      });
    const relocation_restore_identity_t restore_identity{"restore-byte-root", 1,
                                                         digest_with (0x61)};
    const frozen_object_state_t frozen{
      .owner = {.kind = object_kind_t::actor,
                .key = restore_target.key,
                .object_generation = restore_target.object_generation,
                .authority_owner_generation = 1,
                .mesh_name = restore_target.mesh_name,
                .node_id = "node-a"},
      .stable_type = "actor",
      .application_state = {},
      .pending_application = {{1, std::vector<std::uint8_t> (4, 0x41)},
                              {2, std::vector<std::uint8_t> (4, 0x42)},
                              {3, std::vector<std::uint8_t> (4, 0x43)}},
      .timers = {}};
    const auto restored =
      progressive_restore.restore_relocation (frozen, restore_target, restore_identity)
        .result ()
        .value ();
    stateful_error_t relay_ingress = stateful_error_t::conflict;
    stateful_error_t temporary_ingress = stateful_error_t::conflict;
    stateful_error_t pre_commit_claim_error = stateful_error_t::conflict;
    std::optional<turn_record_t> pre_commit_claim;
    stateful_error_t committed = stateful_error_t::conflict;
    std::size_t backlog_records_before_commit = 0;
    std::size_t backlog_bytes_before_commit = 0;
    bool progressive_fifo = false;
    std::size_t handler_turns = 0;
    if (restored == stateful_error_t::none) {
        relay_ingress =
          progressive_restore.enqueue (restore_target, turn_domain_t::application, {4, {0x44}});
        temporary_ingress =
          progressive_restore.enqueue (restore_target, turn_domain_t::application, {5, {0x45}});
        backlog_records_before_commit =
          progressive_restore.pending (restore_target, turn_domain_t::application);
        backlog_bytes_before_commit =
          progressive_restore.pending_bytes (restore_target, turn_domain_t::application);
        std::tie (pre_commit_claim_error, pre_commit_claim) =
          progressive_restore.try_claim (restore_target, turn_domain_t::application);
        committed =
          progressive_restore.commit_relocation_restore (restore_target, restore_identity);
        progressive_fifo = committed == stateful_error_t::none;
        for (std::uint64_t sequence = 1; progressive_fifo && sequence <= 5; ++sequence) {
            const auto [claim_error, claimed] =
              progressive_restore.try_claim (restore_target, turn_domain_t::application);
            const auto [concurrent_error, concurrent] =
              progressive_restore.try_claim (restore_target, turn_domain_t::application);
            progressive_fifo = claim_error == stateful_error_t::none && claimed
                               && claimed->sequence == sequence
                               && concurrent_error == stateful_error_t::none && !concurrent;
            if (claimed) {
                ++handler_turns;
                progressive_fifo =
                  progressive_restore.complete_claim (restore_target, turn_domain_t::application)
                    == stateful_error_t::none
                  && progressive_fifo;
            }
        }
    }
    test.require (
      restored == stateful_error_t::none && materialized && relay_ingress == stateful_error_t::none
        && temporary_ingress == stateful_error_t::none && backlog_records_before_commit == 5
        && backlog_bytes_before_commit > fixed + 1
        && pre_commit_claim_error == stateful_error_t::moving && !pre_commit_claim
        && progressive_restore.pending_bytes (restore_target, turn_domain_t::application) == 0
        && lifecycle_committed && lifecycle_saw_closed_dispatch && !relocation_aborted
        && progressive_fifo && handler_turns == 5,
      "relocation must retain a saved/relay/temporary durable backlog beyond "
      "the live queue, keep handlers closed through lifecycle, "
      "then admit one FIFO turn at a time");

    stateful_object_runtime_t aggregate_restore;
    std::size_t aggregate_materialized = 0;
    std::size_t aggregate_commit_count = 0;
    aggregate_restore.configure_relocation_materialization (
      [&] (const frozen_object_state_t &, const object_ref_t &, const std::optional<object_ref_t> &,
           std::stop_token) {
          ++aggregate_materialized;
          return zlink::framework::task_t<bool> (zlink::framework::result_t<bool>::success (true));
      },
      [&] (const std::vector<object_ref_t> &) {
          ++aggregate_commit_count;
          return true;
      },
      [] (const std::vector<object_ref_t> &) -> zlink::framework::task_t<void> { co_return; });
    std::vector<frozen_object_state_t> aggregate_frozen{
      {.owner = {.kind = object_kind_t::user_spot,
                 .key = "progressive-aggregate-spot",
                 .object_generation = 1,
                 .authority_owner_generation = 1,
                 .mesh_name = "mesh",
                 .node_id = "node-a"},
       .stable_type = "spot",
       .application_state = {},
       .pending_application = {{1, {0x51}}, {2, {0x52}}, {3, {0x53}}},
       .timers = {}},
      {.owner = {.kind = object_kind_t::actor,
                 .key = "progressive-aggregate-actor",
                 .object_generation = 1,
                 .authority_owner_generation = 1,
                 .mesh_name = "mesh",
                 .node_id = "node-a"},
       .stable_type = "actor",
       .application_state = {},
       .pending_application = {{1, {0x61}}, {2, {0x62}}, {3, {0x63}}},
       .timers = {}}};
    std::vector<object_ref_t> aggregate_targets{aggregate_frozen[0].owner,
                                                aggregate_frozen[1].owner};
    for (auto &target : aggregate_targets) {
        target.authority_owner_generation = 2;
        target.node_id = "node-b";
    }
    const relocation_restore_identity_t aggregate_identity{"aggregate-progressive-root", 2,
                                                           digest_with (0x62)};
    const auto aggregate_restored =
      aggregate_restore
        .restore_relocation_aggregate (aggregate_frozen, aggregate_targets, aggregate_identity)
        .result ()
        .value ();
    const auto aggregate_committed = aggregate_restored == stateful_error_t::none
                                       ? aggregate_restore.commit_relocation_restore_aggregate (
                                           aggregate_targets, aggregate_identity)
                                       : stateful_error_t::conflict;
    bool aggregate_fifo = aggregate_committed == stateful_error_t::none;
    for (const auto &target : aggregate_targets) {
        for (std::uint64_t sequence = 1; aggregate_fifo && sequence <= 3; ++sequence) {
            const auto [claim_error, claimed] =
              aggregate_restore.try_claim (target, turn_domain_t::application);
            aggregate_fifo =
              claim_error == stateful_error_t::none && claimed && claimed->sequence == sequence;
            if (claimed) {
                aggregate_fifo =
                  aggregate_restore.complete_claim (target, turn_domain_t::application)
                    == stateful_error_t::none
                  && aggregate_fifo;
            }
        }
    }
    test.require (aggregate_restored == stateful_error_t::none && aggregate_materialized == 2
                    && aggregate_commit_count == 1 && aggregate_fifo,
                  "aggregate relocation must restore and progressively drain every "
                  "participant backlog beyond the live count and byte limits");
}

void test_aggregate_seal_failure_preserves_earlier_application_work (test_context_t &test)
{
    stateful_object_runtime_t objects;
    const auto first = create_spot (objects, object_kind_t::user_spot, "seal-failure-first");
    const auto second = create_spot (objects, object_kind_t::user_spot, "seal-failure-second");
    test.require (
      objects.enqueue (first, turn_domain_t::application, {1, {1, 2}}) == stateful_error_t::none
        && objects.enqueue (first, turn_domain_t::application, {2, {3}}) == stateful_error_t::none
        && objects.enqueue (second, turn_domain_t::application, {1, {4}}) == stateful_error_t::none,
      "aggregate seal failure setup must retain application records");
    const auto first_bytes = objects.pending_bytes (first, turn_domain_t::application);
    const auto second_bytes = objects.pending_bytes (second, turn_domain_t::application);
    objects.configure_relocation_state (
      [&] (const object_ref_t &owner, const std::string &, std::stop_token)
        -> zlink::framework::task_t<zlink::framework::runtime::stateful::frozen_object_state_t> {
          if (owner == second)
              throw std::runtime_error ("capture failed for second participant");
          frozen_object_state_t captured;
          captured.application_state = std::vector<std::uint8_t>{0x41};
          co_return captured;
      },
      [] (const frozen_object_state_t &, const object_ref_t &,
          std::stop_token) -> zlink::framework::task_t<bool> { co_return true; });

    const auto [error, seal] = await_task (objects.try_seal_relocation_aggregate ({first, second}));
    test.require (error == stateful_error_t::conflict && seal.participants.empty (),
                  "aggregate capture failure must return conflict without a seal");
    test.require (objects.pending (first, turn_domain_t::application) == 2
                    && objects.pending_bytes (first, turn_domain_t::application) == first_bytes,
                  "aggregate capture failure must preserve the earlier participant queue");
    test.require (objects.pending (second, turn_domain_t::application) == 1
                    && objects.pending_bytes (second, turn_domain_t::application) == second_bytes,
                  "aggregate capture failure must preserve the failing participant queue");
}

void test_relocation_hold_restores_without_dedicated_limits (test_context_t &test)
{
    namespace limits = zlink::framework::runtime::dispatch_limits;
    namespace protocol = zlink::framework::runtime::protocol;

    // A bound-session request that arrives while the object is sealed for
    // relocation enters the ingress hold. When the hold merges back, the
    // record must travel with its canonical bytes unmodified so the session
    // fence survives the post-seal relay.
    const auto bound_session_bytes = [] {
        const std::vector<std::uint8_t> session_rid{'h', 'e', 'l', 'd'};
        const std::vector<std::uint8_t> node_rid{'s', 'r', 'c'};
        const auto multipart = [] (const std::vector<std::uint8_t> &part) {
            std::vector<std::uint8_t> encoded;
            const auto append_u32 = [&] (std::uint32_t value) {
                encoded.push_back ((value >> 24u) & 0xffu);
                encoded.push_back ((value >> 16u) & 0xffu);
                encoded.push_back ((value >> 8u) & 0xffu);
                encoded.push_back (value & 0xffu);
            };
            append_u32 (1);
            append_u32 (static_cast<std::uint32_t> (part.size ()));
            encoded.insert (encoded.end (), part.begin (), part.end ());
            return encoded;
        };
        protocol::frozen_application_record_t held;
        held.kind = protocol::frozen_record_kind_t::actor_request;
        held.source_kind = protocol::frozen_source_kind_t::bound_session;
        held.source = {"request-owner", 7, node_rid, 1};
        held.source_actor = std::make_pair (std::string ("held-actor"), 5u);
        held.source_session_routing_id = session_rid;
        held.source_binding_generation = 5;
        held.source_session_sequence = 9;
        held.operation = {0x991, 0x992};
        held.operation_kind = 4;
        held.reply_route_id = 61;
        held.body = protocol::frozen_actor_application_body_t{
          {"held-actor", 5, node_rid, 1, 1, 19},
          {protocol::framework_multipart_packet_name, protocol::framework_multipart_content_type,
           multipart ({'h', 'e', 'l', 'd'})}};
        return protocol::encode_frozen_record (protocol::encode_frozen_application_record (held));
    }();
    stateful_object_runtime_t failed_capture;
    const auto failed_spot =
      create_spot (failed_capture, object_kind_t::user_spot, "held-capture-failure");
    test.require (failed_capture.enqueue (failed_spot, turn_domain_t::application, {1, {1}})
                      == stateful_error_t::none
                    && failed_capture.enqueue (failed_spot, turn_domain_t::application, {2, {2}})
                         == stateful_error_t::none,
                  "held capture failure setup must retain the initial FIFO records");
    const auto [active_error, active] =
      failed_capture.try_claim (failed_spot, turn_domain_t::application);
    test.require (active_error == stateful_error_t::none && active && active->sequence == 1,
                  "held capture failure setup must keep the first record active");
    failed_capture.configure_relocation_state (
      [] (const object_ref_t &, const std::string &, std::stop_token)
        -> zlink::framework::task_t<zlink::framework::runtime::stateful::frozen_object_state_t> {
          throw std::runtime_error ("held capture failure");

          co_return zlink::framework::runtime::stateful::frozen_object_state_t{};
      },
      [] (const frozen_object_state_t &, const object_ref_t &,
          std::stop_token) -> zlink::framework::task_t<bool> { co_return true; });

    std::atomic<bool> capture_done = false;
    stateful_error_t capture_error = stateful_error_t::none;
    aggregate_relocation_seal_t capture_seal;
    std::thread capture ([&] {
        auto result = await_task (failed_capture.try_seal_relocation_aggregate ({failed_spot}));
        capture_error = result.error;
        capture_seal = std::move (result.seal);
        capture_done.store (true, std::memory_order_release);
    });
    const bool moving = wait_until_bounded (
      [&] {
          return failed_capture.register_timer (failed_spot, {91, 1000, 1000, 1})
                 == stateful_error_t::moving;
      },
      std::chrono::seconds (5));
    test.require (moving && !capture_done.load (std::memory_order_acquire),
                  "capture failure test must enqueue while the source is moving");
    test.require (
      failed_capture.enqueue (failed_spot, turn_domain_t::application, {3, bound_session_bytes})
        == stateful_error_t::none,
      "application ingress must enter the relocation hold before capture");
    if (active) {
        test.require (failed_capture.complete_claim (failed_spot, turn_domain_t::application)
                        == stateful_error_t::none,
                      "capture failure test must release the active turn");
    }
    capture.join ();
    test.require (capture_error == stateful_error_t::conflict && capture_seal.participants.empty (),
                  "capture failure must return conflict without a relocation seal");
    const auto [first_error, first] =
      failed_capture.try_claim (failed_spot, turn_domain_t::application);
    test.require (first_error == stateful_error_t::none && first && first->sequence == 2,
                  "capture failure must merge held work behind the captured queue");
    (void) failed_capture.complete_claim (failed_spot, turn_domain_t::application);
    const auto [second_error, second] =
      failed_capture.try_claim (failed_spot, turn_domain_t::application);
    test.require (second_error == stateful_error_t::none && second && second->sequence == 3,
                  "capture failure must not strand or reorder held work");
    {
        bool tail_intact = false;
        if (second && second->payload == bound_session_bytes) {
            const auto decoded = protocol::decode_frozen_record (second->payload);
            tail_intact =
              decoded.source_kind == protocol::frozen_source_kind_t::bound_session
              && decoded.source_session_routing_id
              && *decoded.source_session_routing_id == std::vector<std::uint8_t>{'h', 'e', 'l', 'd'}
              && decoded.source_binding_generation == 5 && decoded.source_session_sequence == 9
              && decoded.source_actor && decoded.source_actor->first == "held-actor"
              && decoded.source_actor->second == 5;
        }
        test.require (tail_intact, "held bound-session records must merge with the session tail "
                                   "intact");
    }
    (void) failed_capture.complete_claim (failed_spot, turn_domain_t::application);

    stateful_object_runtime_t count_limited;
    const auto count_first =
      create_spot (count_limited, object_kind_t::user_spot, "held-count-first");
    const auto count_second =
      create_spot (count_limited, object_kind_t::user_spot, "held-count-second");
    test.require (count_limited.enqueue (count_first, turn_domain_t::application, {0, {}})
                    == stateful_error_t::none,
                  "aggregate hold count setup must reserve an active turn");
    const auto [count_claim_error, count_claim] =
      count_limited.try_claim (count_first, turn_domain_t::application);
    test.require (count_claim_error == stateful_error_t::none && count_claim,
                  "aggregate hold count test must activate the source lane");
    std::atomic<bool> count_done = false;
    stateful_error_t count_error = stateful_error_t::conflict;
    aggregate_relocation_seal_t count_seal;
    std::thread count_sealing ([&] {
        auto result =
          await_task (count_limited.try_seal_relocation_aggregate ({count_first, count_second}));
        count_error = result.error;
        count_seal = std::move (result.seal);
        count_done.store (true, std::memory_order_release);
    });
    const bool count_moving = wait_until_bounded (
      [&] {
          return count_limited.register_timer (count_first, {92, 1000, 1000, 1})
                 == stateful_error_t::moving;
      },
      std::chrono::seconds (5));
    test.require (count_moving && !count_done.load (std::memory_order_acquire),
                  "aggregate hold count test must observe the moving barrier");
    const auto membership_actor = create_actor (count_limited, "membership-only-hold-actor");
    const object_ref_t membership_target{
      object_kind_t::user_spot, "membership-only-target", 1, 1, "mesh", "node-b"};
    const auto [membership_error, membership_move] =
      count_limited.begin_remote_membership_move_async (membership_actor, membership_target)
        .result ()
        .value ();
    test.require (
      membership_error == stateful_error_t::none
        && count_limited.enqueue (membership_actor, turn_domain_t::application, {1, {7}})
             == stateful_error_t::none,
      "membership-only movement must retain ingress without relocation accounting");
    bool count_records_accepted = true;
    for (std::uint64_t sequence = 1; sequence <= 600; ++sequence) {
        count_records_accepted =
          count_limited.enqueue (count_first, turn_domain_t::application, {sequence, {}})
            == stateful_error_t::none
          && count_records_accepted;
        count_records_accepted =
          count_limited.enqueue (count_second, turn_domain_t::application, {3000 + sequence, {}})
            == stateful_error_t::none
          && count_records_accepted;
    }
    test.require (count_records_accepted
                    && count_limited.pending (count_first, turn_domain_t::application)
                           + count_limited.pending (count_second, turn_domain_t::application)
                         == 1200,
                  "relocation hold must accept records beyond the former aggregate "
                  "1,024-record limit");
    for (std::uint64_t sequence = 601; sequence <= 2049; ++sequence) {
        count_records_accepted =
          count_limited.enqueue (count_first, turn_domain_t::application, {sequence, {}})
            == stateful_error_t::none
          && count_records_accepted;
    }
    test.require (count_records_accepted
                    && count_limited.pending (count_first, turn_domain_t::application) == 2049,
                  "source relocation ingress must remain held beyond the configured "
                  "application-lane record capacity");
    if (membership_error == stateful_error_t::none) {
        test.require (count_limited.abort_membership_move_async (membership_move).result ().value ()
                        == stateful_error_t::none,
                      "membership-only movement must release its independent hold");
    }
    if (count_claim) {
        test.require (count_limited.complete_claim (count_first, turn_domain_t::application)
                        == stateful_error_t::none,
                      "aggregate hold count test must release its active turn");
    }
    count_sealing.join ();
    test.require (
      count_error == stateful_error_t::none
        && count_limited.abort_relocation_before_cutover (count_seal.token).result ().value ()
             == stateful_error_t::none,
      "aggregate hold count test must close its relocation generation");
    bool source_abort_fifo = count_error == stateful_error_t::none;
    for (std::uint64_t sequence = 1; sequence <= 2049; ++sequence) {
        const auto [claim_error, claimed] =
          count_limited.try_claim (count_first, turn_domain_t::application);
        source_abort_fifo = source_abort_fifo && claim_error == stateful_error_t::none && claimed
                            && claimed->sequence == sequence;
        if (claimed) {
            source_abort_fifo =
              count_limited.complete_claim (count_first, turn_domain_t::application)
                == stateful_error_t::none
              && source_abort_fifo;
        }
    }
    test.require (
      source_abort_fifo,
      "source relocation abort must restore a hold beyond the former capacity in FIFO order");

    stateful_object_runtime_t byte_limited;
    const auto byte_first = create_spot (byte_limited, object_kind_t::user_spot, "held-byte-first");
    const auto byte_second =
      create_spot (byte_limited, object_kind_t::user_spot, "held-byte-second");
    test.require (byte_limited.enqueue (byte_first, turn_domain_t::application, {0, {}})
                    == stateful_error_t::none,
                  "aggregate hold byte setup must reserve an active turn");
    const auto [byte_claim_error, byte_claim] =
      byte_limited.try_claim (byte_first, turn_domain_t::application);
    test.require (byte_claim_error == stateful_error_t::none && byte_claim,
                  "aggregate hold byte test must activate the source lane");
    std::atomic<bool> byte_done = false;
    stateful_error_t byte_error = stateful_error_t::conflict;
    aggregate_relocation_seal_t byte_seal;
    std::thread byte_sealing ([&] {
        auto result =
          await_task (byte_limited.try_seal_relocation_aggregate ({byte_first, byte_second}));
        byte_error = result.error;
        byte_seal = std::move (result.seal);
        byte_done.store (true, std::memory_order_release);
    });
    const bool byte_moving = wait_until_bounded (
      [&] {
          return byte_limited.register_timer (byte_first, {93, 1000, 1000, 1})
                 == stateful_error_t::moving;
      },
      std::chrono::seconds (5));
    test.require (byte_moving && !byte_done.load (std::memory_order_acquire),
                  "aggregate hold byte test must observe the moving barrier");
    const auto held_payload_bytes = 8u * 1024u * 1024u - limits::fixed_work_byte_cost;
    test.require (
      byte_limited.enqueue (byte_first, turn_domain_t::application,
                            {1, std::vector<std::uint8_t> (held_payload_bytes, 0x41)})
          == stateful_error_t::none
        && byte_limited.enqueue (byte_second, turn_domain_t::application,
                                 {1, std::vector<std::uint8_t> (held_payload_bytes, 0x42)})
             == stateful_error_t::none,
      "relocation hold byte setup must retain 16 MiB across participants");
    const auto former_bound_excess_payload_bytes =
      1u * 1024u * 1024u - limits::fixed_work_byte_cost;
    test.require (byte_limited.enqueue (
                    byte_first, turn_domain_t::application,
                    {2, std::vector<std::uint8_t> (former_bound_excess_payload_bytes, 0x43)})
                    == stateful_error_t::none,
                  "relocation hold must accept bytes beyond the former aggregate "
                  "16 MiB limit");
    const auto application_capacity_fill_payload_bytes =
      11u * 1024u * 1024u - 2u * limits::fixed_work_byte_cost;
    test.require (byte_limited.enqueue (
                    byte_first, turn_domain_t::application,
                    {3, std::vector<std::uint8_t> (application_capacity_fill_payload_bytes, 0x44)})
                      == stateful_error_t::none
                    && byte_limited.enqueue (byte_first, turn_domain_t::application, {4, {}})
                         == stateful_error_t::none
                    && byte_limited.enqueue (byte_first, turn_domain_t::application, {5, {}})
                         == stateful_error_t::none
                    && byte_limited.pending_bytes (byte_first, turn_domain_t::application)
                         > 20u * 1024u * 1024u,
                  "source relocation ingress must remain held beyond the configured "
                  "application-lane byte capacity");
    if (byte_claim) {
        test.require (byte_limited.complete_claim (byte_first, turn_domain_t::application)
                        == stateful_error_t::none,
                      "aggregate hold byte test must release its active turn");
    }
    byte_sealing.join ();
    test.require (
      byte_error == stateful_error_t::none
        && byte_limited.abort_relocation_before_cutover (byte_seal.token).result ().value ()
             == stateful_error_t::none,
      "aggregate hold byte test must close its relocation generation");

    stateful_object_runtime_t target_hold;
    std::mutex target_mutex;
    std::condition_variable target_condition;
    bool target_restore_entered = false;
    bool release_target_restore = false;
    target_hold.configure_relocation_state (
      [] (const object_ref_t &, const std::string &, std::stop_token)
        -> zlink::framework::task_t<zlink::framework::runtime::stateful::frozen_object_state_t> {
          frozen_object_state_t captured;
          captured.application_state = std::vector<std::uint8_t>{};
          co_return captured;
      },
      [&] (const frozen_object_state_t &, const object_ref_t &,
           std::stop_token) -> zlink::framework::task_t<bool> {
          std::unique_lock lock (target_mutex);
          target_restore_entered = true;
          target_condition.notify_all ();
          target_condition.wait (lock, [&] { return release_target_restore; });
          co_return true;
      });
    const frozen_object_state_t target_frozen{.owner = {.kind = object_kind_t::user_spot,
                                                        .key = "target-unbounded-hold",
                                                        .object_generation = 1,
                                                        .authority_owner_generation = 1,
                                                        .mesh_name = "mesh",
                                                        .node_id = "source"},
                                              .stable_type = "spot",
                                              .application_state = {},
                                              .pending_application = {},
                                              .timers = {}};
    auto target_ref = target_frozen.owner;
    target_ref.authority_owner_generation = 2;
    target_ref.node_id = "target";
    const relocation_restore_identity_t target_identity{"target-unbounded-root", 7,
                                                        digest_with (0x71)};
    stateful_error_t target_restore_result = stateful_error_t::conflict;
    std::thread target_restoring ([&] {
        target_restore_result =
          target_hold.restore_relocation (target_frozen, target_ref, target_identity)
            .result ()
            .value ();
    });
    {
        std::unique_lock lock (target_mutex);
        test.require (target_condition.wait_for (lock, std::chrono::seconds (5),
                                                 [&] { return target_restore_entered; }),
                      "target temporary hold test must enter restore before ingress");
    }
    bool target_records_held =
      target_hold.enqueue (target_ref, turn_domain_t::application,
                           {1, std::vector<std::uint8_t> (17u * 1024u * 1024u, 0x51)})
      == stateful_error_t::none;
    for (std::uint64_t sequence = 2; sequence <= 1025; ++sequence) {
        target_records_held =
          target_hold.enqueue (target_ref, turn_domain_t::application, {sequence, {0x52}})
            == stateful_error_t::none
          && target_records_held;
    }
    {
        std::lock_guard lock (target_mutex);
        release_target_restore = true;
    }
    target_condition.notify_all ();
    target_restoring.join ();
    test.require (target_records_held && target_restore_result == stateful_error_t::none
                    && target_hold.pending (target_ref, turn_domain_t::application) == 1025
                    && target_hold.pending_bytes (target_ref, turn_domain_t::application)
                         > 16u * 1024u * 1024u
                    && target_hold.commit_relocation_restore (target_ref, target_identity)
                         == stateful_error_t::none,
                  "target relocation ingress must remain held beyond the configured lane "
                  "and former 1,024-record/16 MiB temporary limits");
    for (std::uint64_t sequence = 1; sequence <= 1025; ++sequence) {
        const auto [claim_error, claimed] =
          target_hold.try_claim (target_ref, turn_domain_t::application);
        test.require (claim_error == stateful_error_t::none && claimed
                        && claimed->sequence == sequence,
                      "target relocation hold must resume in FIFO order after commit");
        if (claimed) {
            (void) target_hold.complete_claim (target_ref, turn_domain_t::application);
        }
    }

    stateful_object_runtime_t target_abort;
    std::mutex abort_mutex;
    std::condition_variable abort_condition;
    bool abort_restore_entered = false;
    bool release_abort_restore = false;
    target_abort.configure_relocation_state (
      [] (const object_ref_t &, const std::string &, std::stop_token)
        -> zlink::framework::task_t<zlink::framework::runtime::stateful::frozen_object_state_t> {
          frozen_object_state_t captured;
          captured.application_state = std::vector<std::uint8_t>{};
          co_return captured;
      },
      [&] (const frozen_object_state_t &, const object_ref_t &,
           std::stop_token) -> zlink::framework::task_t<bool> {
          std::unique_lock lock (abort_mutex);
          abort_restore_entered = true;
          abort_condition.notify_all ();
          abort_condition.wait (lock, [&] { return release_abort_restore; });
          co_return true;
      });
    auto abort_frozen = target_frozen;
    abort_frozen.owner.key = "target-abort-hold";
    auto abort_target = abort_frozen.owner;
    abort_target.authority_owner_generation = 2;
    abort_target.node_id = "target";
    const relocation_restore_identity_t abort_identity{"target-abort-root", 8, digest_with (0x72)};
    stateful_error_t abort_restore_result = stateful_error_t::conflict;
    std::thread abort_restoring ([&] {
        abort_restore_result =
          target_abort.restore_relocation (abort_frozen, abort_target, abort_identity)
            .result ()
            .value ();
    });
    {
        std::unique_lock lock (abort_mutex);
        test.require (abort_condition.wait_for (lock, std::chrono::seconds (5),
                                                [&] { return abort_restore_entered; }),
                      "target abort test must enter restore before ingress");
    }
    test.require (target_abort.enqueue (abort_target, turn_domain_t::application, {1, {0x71}})
                      == stateful_error_t::none
                    && target_abort.enqueue (abort_target, turn_domain_t::application, {2, {0x72}})
                         == stateful_error_t::none,
                  "target abort test must hold records beyond its normal lane");
    {
        std::lock_guard lock (abort_mutex);
        release_abort_restore = true;
    }
    abort_condition.notify_all ();
    abort_restoring.join ();
    test.require (abort_restore_result == stateful_error_t::none
                    && target_abort.abort_relocation_restore_async (abort_target, abort_identity)
                           .result ()
                           .value ()
                         == stateful_error_t::none
                    && !target_abort.find (abort_target.kind, abort_target.key),
                  "target relocation abort must discard its temporary hold and object reservation");
    target_abort.configure_relocation_state (
      [] (const object_ref_t &, const std::string &, std::stop_token)
        -> zlink::framework::task_t<zlink::framework::runtime::stateful::frozen_object_state_t> {
          frozen_object_state_t captured;
          captured.application_state = std::vector<std::uint8_t>{};
          co_return captured;
      },
      [] (const frozen_object_state_t &, const object_ref_t &,
          std::stop_token) -> zlink::framework::task_t<bool> { co_return true; });
    test.require (target_abort.restore_relocation (abort_frozen, abort_target, abort_identity)
                        .result ()
                        .value ()
                      == stateful_error_t::none
                    && target_abort.commit_relocation_restore (abort_target, abort_identity)
                         == stateful_error_t::none
                    && target_abort.pending (abort_target, turn_domain_t::application) == 0,
                  "target relocation abort must leave no held records in a retried restore");

    stateful_object_runtime_t normal_lane_caps;
    const auto membership_capped = create_actor (normal_lane_caps, "membership-normal-cap");
    const object_ref_t remote_membership_target{
      object_kind_t::user_spot, "membership-normal-target", 1, 1, "mesh", "node-b"};
    const auto [normal_move_error, normal_move] =
      normal_lane_caps
        .begin_remote_membership_move_async (membership_capped, remote_membership_target)
        .result ()
        .value ();
    test.require (
      normal_move_error == stateful_error_t::none
        && normal_lane_caps.enqueue (
             membership_capped, turn_domain_t::application,
             {99, std::vector<std::uint8_t> (2u * limits::fixed_work_byte_cost + 5, 0x61)})
             == stateful_error_t::none
        && normal_lane_caps.enqueue (membership_capped, turn_domain_t::application, {1, {}})
             == stateful_error_t::none
        && normal_lane_caps.enqueue (membership_capped, turn_domain_t::application, {2, {}})
             == stateful_error_t::none
        && normal_lane_caps.enqueue (membership_capped, turn_domain_t::application, {3, {}})
             == stateful_error_t::none,
      "membership-only movement must retain work beyond former lane limits");
    if (normal_move_error == stateful_error_t::none)
        (void) normal_lane_caps.abort_membership_move_async (normal_move).result ().value ();

    const auto closing_capped =
      create_spot (normal_lane_caps, object_kind_t::user_spot, "closing-normal-cap");
    const auto [close_error, close_eligible] =
      normal_lane_caps.can_close_spot_async (closing_capped).result ().value ();
    test.require (
      close_error == stateful_error_t::none && close_eligible
        && normal_lane_caps.enqueue (
             closing_capped, turn_domain_t::application,
             {99, std::vector<std::uint8_t> (2u * limits::fixed_work_byte_cost + 5, 0x62)})
             == stateful_error_t::none
        && normal_lane_caps.enqueue (closing_capped, turn_domain_t::application, {1, {}})
             == stateful_error_t::none
        && normal_lane_caps.enqueue (closing_capped, turn_domain_t::application, {2, {}})
             == stateful_error_t::none
        && normal_lane_caps.enqueue (closing_capped, turn_domain_t::application, {3, {}})
             == stateful_error_t::none,
      "Close eligibility must preserve work beyond former lane limits");
    test.require (normal_lane_caps.close_spot (closing_capped)
                    == std::pair{stateful_error_t::none, true},
                  "eligible Spot local release must succeed");
}

} // namespace m6c_test
