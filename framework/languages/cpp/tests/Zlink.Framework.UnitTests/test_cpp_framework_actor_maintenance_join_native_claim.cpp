/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/actor_maintenance_join_host_fixture.hpp"
#include "../support/actor_maintenance_join_packets.hpp"
#include "../support/actor_maintenance_join_native_fixture.hpp"
#include "../support/telemetry_observation.hpp"
#include "../support/stateful_object_sync.hpp"
#include "../support/manual_internal_scheduler.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "../support/stateful_ingress_fixture.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/stateful/public_host_runtime.hpp"
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/execution/serial_execution_queue.hpp"
#include "runtime/execution/serial_application_record.hpp"
#include "runtime/stateful/accepted_actor_record.hpp"
#include "runtime/stateful/raw_stateful_dispatch.hpp"
#include "runtime/execution/task_result.hpp"
#include <gtest/gtest.h>
#include <gtest/gtest-spi.h>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace zlink::framework::test::actor_maintenance_join
{
using zlink::framework::test::begin_reserved_object;

TEST (NativeIntake, CanonicalAdmissionReportsOnlyActualSuccessWhenEnabled)
{
    namespace rt = fw::runtime;
    namespace st = rt::stateful;
    for (const auto mode :
         {fw::message_flow_log_mode_t::off, fw::message_flow_log_mode_t::normal}) {
        SCOPED_TRACE (mode == fw::message_flow_log_mode_t::off ? "off" : "normal");
        auto observations =
          std::make_shared<fw::test::telemetry_observations_t<fw::message_flow_event_t>> ();
        std::vector<fw::message_flow_event_t> events;
        fw::dispatch_options_t dispatch;
        dispatch.message_flow (mode);
        fw::detail::dispatch_options_access_t::set_observer_for_tests (
          dispatch, [observations] (const auto &event) { observations->record (event); });
        native_intake_fixture_t fixture (std::move (dispatch));
        auto host = fixture.host;
        const st::object_ref_t object{st::object_kind_t::actor,
                                      actor_id,
                                      1,
                                      1,
                                      mesh_name,
                                      zlink::routing_id_t::from (fixture.rid).to_string ()};
        const auto created = begin_reserved_object (host->objects (), object, actor_type, {});
        ASSERT_EQ (created.error, st::stateful_error_t::none);
        ASSERT_EQ (host->objects ().commit_create (created.attempt), st::stateful_error_t::none);
        st::raw_stateful_dispatch_t raw (host->objects (), host->transport (), fixture.resolver);
        std::vector<rt::mesh::service_mailbox_record_t> records;
        records.push_back (fixture.actor_record (1, 2));
        records.push_back (fixture.actor_record (2));
        records.push_back (fixture.actor_record (3));
        auto claim =
          fw::test::publish_stateful_claims (host->transport (), object, std::move (records));
        std::shared_ptr<rt::mesh::service_mailbox_claim_t> following_claim;
        // Settled slot credits of both claims.
        const auto releases = [&] {
            std::size_t settled = claim->claimed_messages - claim->outstanding ();
            if (following_claim)
                settled += following_claim->claimed_messages - following_claim->outstanding ();
            return settled;
        };
        const auto ingest = [&] (auto slot, const st::object_ref_t &target) {
            const auto result =
              raw
                .ingest_claimed (target, std::move (slot),
                                 fw::test::retained_ingress_snapshot (host->objects (), target))
                .result ();
            EXPECT_TRUE (result);
            return result ? result.value () : st::stateful_error_t::invalid;
        };
        EXPECT_NE (ingest (rt::mesh::service_mailbox_slot_t{claim, 0}, object),
                   st::stateful_error_t::none);
        EXPECT_EQ (releases (), 1u);
        fw::test::acknowledge_delivery ();
        events = observations->snapshot ();
        EXPECT_TRUE (events.empty ());
        EXPECT_EQ (host->objects ().pending (object, st::turn_domain_t::application), 0u);
        EXPECT_EQ (ingest (rt::mesh::service_mailbox_slot_t{claim, 1}, object),
                   st::stateful_error_t::none);
        fw::test::acknowledge_delivery ();
        events = observations->snapshot ();
        EXPECT_EQ (events.size (), mode == fw::message_flow_log_mode_t::normal ? 1u : 0u);
        if (!events.empty ()) {
            EXPECT_EQ (events.front ().outcome, fw::message_flow_outcome_t::admitted);
            EXPECT_EQ (events.front ().actor_id, actor_id);
            EXPECT_EQ (events.front ().packet_name, probe_t::packet_name);
            EXPECT_FALSE (events.front ().detail_stage);
            EXPECT_FALSE (events.front ().detail_result);
        }
        auto stale_object = object;
        ++stale_object.object_generation;
        // The valid route/body reaches commit; the actual Kernel owner rejects enqueue.
        EXPECT_EQ (ingest (rt::mesh::service_mailbox_slot_t{claim, 2}, stale_object),
                   st::stateful_error_t::generation_stale);
        EXPECT_EQ (releases (), 2u);
        fw::test::acknowledge_delivery ();
        events = observations->snapshot ();
        EXPECT_EQ (events.size (), mode == fw::message_flow_log_mode_t::normal ? 1u : 0u);
        EXPECT_EQ (host->objects ().pending (object, st::turn_domain_t::application), 1u);
        auto [error, delivery] = raw.try_claim (object);
        EXPECT_EQ (error, st::stateful_error_t::none);
        EXPECT_TRUE (delivery);
        if (delivery) {
            EXPECT_EQ (delivery->turn.sequence, 1u);
            EXPECT_EQ (delivery->frozen.operation.low, 2u);
            EXPECT_EQ (raw.complete (*delivery), st::stateful_error_t::none);
        }
        EXPECT_EQ (releases (), 3u);
        const auto following_slot =
          fw::test::publish_stateful_claim (host->transport (), object, fixture.actor_record (4));
        following_claim = following_slot.claim;
        EXPECT_EQ (ingest (following_slot, object), st::stateful_error_t::none);
        auto [following_error, following] = raw.try_claim (object);
        EXPECT_EQ (following_error, st::stateful_error_t::none);
        EXPECT_TRUE (following);
        if (following) {
            EXPECT_EQ (following->turn.sequence, 2u);
            EXPECT_EQ (following->frozen.operation.low, 4u);
            EXPECT_EQ (raw.complete (*following), st::stateful_error_t::none);
        }
        fw::test::acknowledge_delivery ();
        events = observations->snapshot ();
        EXPECT_EQ (events.size (), mode == fw::message_flow_log_mode_t::normal ? 2u : 0u);
        EXPECT_EQ (raw.discard_pending (object), st::stateful_error_t::none);
        EXPECT_EQ (releases (), 4u);
        EXPECT_EQ (host->objects ().pending (object, st::turn_domain_t::application), 0u);
        host->close ();
    }
}

TEST (NativeIntake, JoinedBatchFenceExcludesInitialMoveAndOtherActor)
{
    auto evidence_owner = std::make_shared<observations_t> ();
    auto &evidence = *evidence_owner;
    evidence.joined_payloads = {1, 2};
    fw::message_flow_event_t event{};
    event.outcome = fw::message_flow_outcome_t::admitted;
    event.actor_id = actor_id;
    event.packet_name = move_t::packet_name;
    observe_joined_batch_admission (evidence, event);
    EXPECT_EQ (evidence.backlog_appends, 0);
    EXPECT_FALSE (evidence.joined_send_captured.task ().await_ready ());
    int completions = 0;
    evidence.joined_admission_completion = observations_t::joined_admission_batch_t{
      2, [&] {
          ++completions;
          evidence.joined_send_captured.complete (fw::result_t<void>::success ());
      }};
    event.packet_name = probe_t::packet_name;
    event.actor_id = "other-actor";
    observe_joined_batch_admission (evidence, event);
    event.actor_id = actor_id;
    event.outcome = fw::message_flow_outcome_t::received;
    observe_joined_batch_admission (evidence, event);
    EXPECT_EQ (evidence.backlog_appends, 0);
    event.outcome = fw::message_flow_outcome_t::admitted;
    event.detail_stage = "invoke_erased.post_serial";
    observe_joined_batch_admission (evidence, event);
    EXPECT_EQ (evidence.backlog_appends, 0);
    event.detail_stage = "canonical_application.commit_accepted_ingress";
    observe_joined_batch_admission (evidence, event);
    EXPECT_EQ (evidence.backlog_appends, 0);
    ASSERT_TRUE (evidence.joined_admission_completion);
    EXPECT_EQ (evidence.joined_admission_completion->remaining, 1u);
    EXPECT_FALSE (evidence.joined_send_captured.task ().await_ready ());

    event.outcome = fw::message_flow_outcome_t::received;
    event.detail_stage = "relay_actor_packet.try_append_backlog";
    event.detail_result = "result=appended";
    event.actor_id = "other-actor";
    observe_joined_batch_admission (evidence, event);
    EXPECT_EQ (evidence.backlog_appends, 0);
    event.actor_id = actor_id;
    observe_joined_batch_admission (evidence, event);
    EXPECT_EQ (evidence.backlog_appends, 1);
    observe_joined_batch_admission (evidence, event);
    EXPECT_EQ (evidence.backlog_appends, 2);
    EXPECT_EQ (evidence.joined_admission_completion->remaining, 1u);
    EXPECT_FALSE (evidence.joined_send_captured.task ().await_ready ());

    event.outcome = fw::message_flow_outcome_t::admitted;
    event.detail_stage = "canonical_application.commit_accepted_ingress";
    event.detail_result.reset ();
    observe_joined_batch_admission (evidence, event);
    EXPECT_EQ (evidence.backlog_appends, 2);
    EXPECT_TRUE (evidence.joined_send_captured.task ().await_ready ());
    observe_joined_batch_admission (evidence, event);
    EXPECT_EQ (evidence.backlog_appends, 2);
    EXPECT_EQ (completions, 1);
    EXPECT_FALSE (evidence.joined_admission_completion);

    event.outcome = fw::message_flow_outcome_t::received;
    event.detail_stage = "relay_actor_packet.try_append_backlog";
    event.detail_result = "result=appended";
    observe_joined_batch_admission (evidence, event);
    EXPECT_EQ (evidence.backlog_appends, 3);
    EXPECT_EQ (completions, 1);
}

TEST (NativeIntake, TypedControlMalformedWireAndKernelMetadata)
{
    namespace rt = fw::runtime;
    namespace st = fw::runtime::stateful;
    namespace mesh = fw::runtime::mesh;
    namespace wire = fw::runtime::protocol;
    native_intake_fixture_t fixture;
    auto host = fixture.host;
    const auto &rid = fixture.rid;
    const auto &resolver = fixture.resolver;
    // Restore only Kernel metadata: the facade Actor map intentionally has no entry.
    const st::object_ref_t object{st::object_kind_t::actor,
                                  actor_id,
                                  1,
                                  1,
                                  mesh_name,
                                  zlink::routing_id_t::from (rid).to_string ()};
    const auto created = begin_reserved_object (host->objects (), object, actor_type, {});
    ASSERT_EQ (created.error, st::stateful_error_t::none);
    ASSERT_EQ (host->objects ().commit_create (created.attempt), st::stateful_error_t::none);
    auto &mailbox = host->transport ().mailbox ();
    const auto owner =
      mesh::service_mailbox_t::application_owner (rt::host::owner_kind_t::actor, actor_id);
    std::size_t started = 0, rejected = 0, controls = 0, applications = 0;
    auto callback = [&] (const rt::host::ready_record_t &ready,
                         const rt::host::receive_record_t &record,
                         std::vector<zlink::message_t> parts, auto) {
        if (record.kind == rt::host::record_kind_t::spot_control) {
            EXPECT_TRUE (parts.empty ());
            ++controls;
        } else {
            ASSERT_TRUE (ready.actor);
            EXPECT_EQ (fw::detail::actor_ref_access_t::actor_type (*ready.actor), actor_type);
            EXPECT_EQ (parts.size (), 2u);
            const auto header = fw::detail::decode_actor_packet_header (
              rt::messaging::message_parts_t (parts), fw::detail::stream_message_kind_t::send,
              false);
            ASSERT_TRUE (header);
            EXPECT_EQ (header.value ().message_name, probe_t::packet_name);
            ++applications;
            ASSERT_TRUE (record.complete_stateful_dispatch);
            record.complete_stateful_dispatch ();
        }
    };
    const auto drain = [&] {
        EXPECT_TRUE (mailbox.begin_application_drain (owner));
        const auto result =
          zlink::framework::test::application_owner_result (host->dispatch_application_owner (
            owner, callback, [&] { ++started; }, [&] { ++rejected; }));
        EXPECT_TRUE (result);
        if (result)
            EXPECT_TRUE (result.value ());
        mailbox.end_application_drain (owner);
        EXPECT_EQ (mailbox.pending_messages (mesh::service_mailbox_domain_t::application), 0u);
        EXPECT_EQ (mailbox.pending_bytes (mesh::service_mailbox_domain_t::application), 0u);
    };
    mesh::service_mailbox_record_t typed;
    typed.owner = owner;
    typed.domain = mesh::service_mailbox_domain_t::application;
    typed.application.emplace ();
    typed.application->record.kind = rt::host::record_kind_t::spot_control;
    ASSERT_TRUE (mailbox.try_enqueue (std::move (typed)));
    drain ();
    EXPECT_EQ (controls, 1u);
    EXPECT_EQ (rejected, 0u);
    std::vector<zlink::message_t> malformed_parts;
    malformed_parts.push_back (zlink::message_t::from ("invalid"));
    auto malformed = mesh::service_mailbox_record_t::from_parts (
      owner, mesh::service_mailbox_domain_t::application, std::move (malformed_parts));
    ASSERT_TRUE (mailbox.try_enqueue (std::move (malformed)));
    drain ();
    EXPECT_EQ (controls, 1u);
    EXPECT_EQ (rejected, 1u);
    st::raw_stateful_dispatch_t raw (host->objects (), host->transport (), resolver);
    EXPECT_THROW (mailbox.try_enqueue (mesh::service_mailbox_record_t::from_parts (
                    owner, mesh::service_mailbox_domain_t::application, {})),
                  std::invalid_argument);
    auto invalid_accepted = mesh::service_mailbox_record_t::from_parts (
      owner, mesh::service_mailbox_domain_t::application, {zlink::message_t::from ("invalid")});
    const auto invalid_slot =
      fw::test::publish_stateful_claim (host->transport (), object, std::move (invalid_accepted));
    EXPECT_EQ (raw
                 .ingest_claimed (object, invalid_slot,
                                  fw::test::retained_ingress_snapshot (host->objects (), object))
                 .result ()
                 .value (),
               st::stateful_error_t::invalid);
    EXPECT_EQ (invalid_slot.claim->outstanding (), 0u);
    auto valid = fixture.actor_record (1);
    ASSERT_TRUE (mailbox.try_enqueue (std::move (valid)));
    drain ();
    EXPECT_EQ (applications, 1u);
    EXPECT_EQ (controls, 1u);
    EXPECT_EQ (started, 3u);
    EXPECT_EQ (rejected, 1u);
    EXPECT_EQ (host->objects ().pending (object, st::turn_domain_t::application), 0u);
    host->close ();
}

TEST (NativeIntake, CanonicalPrivateFifoReturnsNativeClaimBeforeTerminal)
{
    namespace rt = fw::runtime;
    namespace wire = rt::protocol;
    for (const bool restored : {false, true}) {
        SCOPED_TRACE (restored ? "restored" : "live");
        fw::test::manual_internal_scheduler_t worker;
        auto queue = std::make_shared<rt::serial_execution_queue_t> (worker);
        auto record = std::make_shared<rt::serial_application_record_t> ();
        record->payload ().emplace ();
        wire::application_payload_t::multipart_t parts;
        parts.push_back (zlink::message_t::from ("1"));
        wire::actor_message_header_t header;
        const auto rid = zlink::routing_id_t::from (first_rid).to_bytes ();
        header.target = {actor_id, 1, rid, 1, 1, 1};
        header.operation = {1, 1};
        record->payload ()->application_record = rt::stateful::accepted_actor_record (
          header, {"source", 1, rid, 1},
          wire::application_payload_t::from_parts (std::move (parts)), false);
        if (restored) {
            record->payload ()->frozen_record =
              wire::summarize_frozen_application_record (*record->payload ()->application_record);
            record->payload ()->application_record.reset ();
        }
        int native_returns = 0, handlers = 0;
        record->transferred = [&] { ++native_returns; };
        rt::serial_execution_queue_t::async_completion_t finish;
        rt::serial_work_options_t options;
        options.application_record = record;
        const bool accepted = queue->try_post_async (
          "native-private-owner",
          [&, record] (auto terminal) {
              record->admission.enter_handler ();
              ++handlers;
              finish = std::move (terminal);
          },
          options);
        EXPECT_TRUE (accepted);
        EXPECT_TRUE (queue->try_post ("private-successor", [&] { ++handlers; }));
        worker.run_all ();
        EXPECT_EQ (handlers, 1);
        EXPECT_EQ (native_returns, 1);
        // Settle the fixture's native source even on the failing input, then
        // end its actual private terminal and drain the successor normally.
        record->release_native_claim ();
        if (finish)
            finish ([] {});
        worker.run_all ();
        EXPECT_EQ (handlers, 2);
        EXPECT_EQ (native_returns, 1);
        queue->close ();
        worker.run_all ();
        queue->drain ();
        worker.retire ();
    }
}

TEST (NativeIntake, NativeMailboxTransferKeepsActualPrivateMClaim)
{
    namespace rt = fw::runtime;
    namespace st = rt::stateful;
    namespace mesh = rt::mesh;
    for (const bool kernel_only_producer : {false, true}) {
        SCOPED_TRACE (kernel_only_producer ? "Kernel-only negative" : "native handoff");
        native_intake_fixture_t fixture;
        auto host = fixture.host;
        const st::object_ref_t object{st::object_kind_t::actor,
                                      actor_id,
                                      1,
                                      1,
                                      mesh_name,
                                      zlink::routing_id_t::from (fixture.rid).to_string ()};
        const auto created = begin_reserved_object (host->objects (), object, actor_type, {});
        ASSERT_EQ (created.error, st::stateful_error_t::none);
        ASSERT_EQ (host->objects ().commit_create (created.attempt), st::stateful_error_t::none);
        auto &mailbox = host->transport ().mailbox ();
        const auto owner =
          mesh::service_mailbox_t::application_owner (rt::host::owner_kind_t::actor, actor_id);
        fw::test::manual_internal_scheduler_t worker;
        auto spot = std::make_shared<rt::serial_execution_queue_t> (
          worker, rt::serial_execution_queue_options_t{},
          rt::serial_execution_queue_t::error_handler_t{}, rt::serial_lane_policy_t::spot_wide ());
        auto queue = std::make_shared<rt::serial_execution_queue_t> (worker);
        queue->attach_spot_gate (spot);
        fw::task_completion_source_t<void> mailbox_terminal;
        int delivered = 0, handlers = 0, started = 0, rejected = 0;
        struct settle_fixture_t
        {
            fw::task_completion_source_t<void> &terminal;
            fw::test::manual_internal_scheduler_t &worker;
            std::shared_ptr<rt::serial_execution_queue_t> queue;
            std::shared_ptr<rt::serial_execution_queue_t> spot;
            std::shared_ptr<rt::host::public_host_runtime_t> host;
            ~settle_fixture_t ()
            {
                terminal.complete (fw::result_t<void>::success ());
                worker.run_all ();
                queue->close ();
                worker.run_all ();
                queue->drain ();
                spot->close ();
                worker.run_all ();
                spot->drain ();
                worker.retire ();
                host->close ();
            }
        } cleanup{mailbox_terminal, worker, queue, spot, host};
        const auto dispatch = [&] (const rt::host::ready_record_t &,
                                   const rt::host::receive_record_t &record,
                                   std::vector<zlink::message_t> parts, auto) {
            EXPECT_EQ (parts.size (), 2u);
            const auto header = fw::detail::decode_actor_packet_header (
              rt::messaging::message_parts_t (parts), fw::detail::stream_message_kind_t::send,
              false);
            ASSERT_TRUE (header);
            EXPECT_EQ (header.value ().message_name, probe_t::packet_name);
            ++delivered;
            EXPECT_EQ (record.operation_id.low, static_cast<std::uint64_t> (delivered));
            auto accepted = record.application_record;
            EXPECT_TRUE (accepted);
            if (!accepted) {
                if (record.complete_stateful_dispatch)
                    record.complete_stateful_dispatch ();
                return;
            }
            const bool first = delivered == 1;
            if (first && kernel_only_producer) {
                // The previous producer returned only the Kernel claim and
                // kept the actual native mailbox reservation until M.
                accepted->transferred = [host, object] {
                    (void) host->objects ().complete_claim (object, st::turn_domain_t::application);
                };
            }
            rt::serial_work_options_t options;
            options.application_record = accepted;
            EXPECT_TRUE (queue->try_post_async (
              "native-private-M",
              [&, accepted, first, operation = record.operation_id.low,
               terminal = std::function<void ()> (
                 [accepted] { (void) accepted->complete_native (std::nullopt); })] (auto complete) {
                  ++handlers;
                  EXPECT_EQ (operation, static_cast<std::uint64_t> (handlers));
                  accepted->admission.enter_handler ();
                  if (first) {
                      auto pending = std::make_shared<fw::task_t<void>> (mailbox_terminal.task ());
                      fw::detail::observe_task_terminal (*pending,
                                                         [pending, terminal] (const auto &) {
                                                             if (terminal)
                                                                 terminal ();
                                                         });
                      auto turn = fw::detail::capture_current_serial_turn ();
                      EXPECT_TRUE (turn);
                      if (turn)
                          EXPECT_TRUE (
                            turn->defer ([&] { return mailbox_terminal.task (); }, [] {}));
                      complete ([] {});
                  } else {
                      complete ([terminal] {
                          if (terminal)
                              terminal ();
                      });
                  }
              },
              options));
        };
        const auto drain = [&] {
            if (!mailbox.begin_application_drain (owner))
                return false;
            const auto result =
              zlink::framework::test::application_owner_result (host->dispatch_application_owner (
                owner, dispatch, [&] { ++started; }, [&] { ++rejected; }));
            mailbox.end_application_drain (owner);
            EXPECT_TRUE (result);
            return result && result.value ();
        };
        EXPECT_TRUE (mailbox.try_enqueue (fixture.actor_record (1)));
        EXPECT_TRUE (drain ());
        worker.run_all ();
        EXPECT_EQ (handlers, 1);
        EXPECT_FALSE (mailbox_terminal.task ().await_ready ());
        EXPECT_TRUE (mailbox.try_enqueue (fixture.actor_record (2)));
        const bool native_available = drain ();
        worker.run_all ();
        const auto require_native_transfer = [&] {
            EXPECT_TRUE (native_available && delivered == 2)
              << "native_available=" << native_available << " delivered=" << delivered;
        };
        int observed_rejections = 0;
        if (kernel_only_producer) {
            testing::TestPartResultArray failures;
            {
                testing::ScopedFakeTestPartResultReporter capture (
                  testing::ScopedFakeTestPartResultReporter::INTERCEPT_ONLY_CURRENT_THREAD,
                  &failures);
                require_native_transfer ();
            }
            observed_rejections = failures.size ();
            EXPECT_EQ (failures.size (), 1);
            if (failures.size () == 1)
                EXPECT_TRUE (failures.GetTestPartResult (0).nonfatally_failed ());
        } else {
            require_native_transfer ();
        }
        std::printf ("native-M kernel_only=%d native_available=%d delivered=%d handlers_while_M=%d "
                     "rejected_checks=%d\n",
                     kernel_only_producer, native_available, delivered, handlers,
                     observed_rejections);
        EXPECT_EQ (handlers, 1);
        mailbox_terminal.complete (fw::result_t<void>::success ());
        worker.run_all ();
        if (!native_available)
            EXPECT_TRUE (drain ());
        worker.run_all ();
        EXPECT_EQ (delivered, 2);
        EXPECT_EQ (handlers, 2);
        EXPECT_EQ (started, 2);
        EXPECT_EQ (rejected, 0);
        EXPECT_EQ (mailbox.pending_messages (mesh::service_mailbox_domain_t::application), 0u);
        EXPECT_EQ (mailbox.pending_bytes (mesh::service_mailbox_domain_t::application), 0u);
        EXPECT_EQ (host->objects ().pending (object, st::turn_domain_t::application), 0u);
        EXPECT_EQ (host->objects ().complete_claim (object, st::turn_domain_t::application),
                   st::stateful_error_t::conflict);
    }
}
} // namespace zlink::framework::test::actor_maintenance_join
