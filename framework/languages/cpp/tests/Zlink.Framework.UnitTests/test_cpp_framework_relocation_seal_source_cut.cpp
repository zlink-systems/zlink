/* SPDX-License-Identifier: Elastic-2.0 */
#include "mailbox_claim_fixture.hpp"
#include "../support/telemetry_observation.hpp"
#include "../support/stateful_object_sync.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "../support/exception_payload.hpp"
#include "../support/manual_internal_scheduler.hpp"
#include "runtime/execution/serial_application_record.hpp"
#include "../support/stateful_ingress_fixture.hpp"
#include "runtime/backend/raw_binding_adapter.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/diagnostics/dispatch_error_reporter.hpp"
#include "runtime/diagnostics/monitoring_runtime.hpp"
#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/live_location_reader.hpp"
#include "runtime/protocol/service_wire_codec.hpp"
#include "runtime/stateful/public_host_runtime.hpp"
#include "runtime/stateful/accepted_actor_record.hpp"
#include "runtime/streams/stream_runtime.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include "runtime/timers/timer_runtime.hpp"
#include <gtest/gtest.h>
#include <service_wire_pilot_codec.hpp>
#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <tuple>
#include <string>
#include <utility>
#include <type_traits>
#include <variant>
#include <vector>
#include "../support/relocation_seal_host_access.hpp"
#include "../support/relocation_seal_execution_fixture.hpp"
#include "../support/relocation_seal_native_records.hpp"

using namespace framework_relocation_seal_fixture;

using zlink::framework::test::begin_reserved_object;

using namespace zlink::framework;

using namespace zlink::framework::detail;

namespace host = zlink::framework::runtime::host;

namespace mesh = zlink::framework::runtime::mesh;

namespace protocol = zlink::framework::runtime::protocol;

namespace stateful = zlink::framework::runtime::stateful;

TEST (FrameworkRelocationSealContract, sealed_scheduled_timer_keeps_pending_count_without_failure)
{
    auto context = std::make_shared<spot_context_state_t> ();
    context->node = zlink::framework::test::runtime_failure_fixture<spot_node_builder_state_t> (
      "sealed-timer-node");
    auto executor = std::make_shared<runtime::offload_executor_t> (1);
    context->serial_executor = executor;
    context->serial_queue = std::make_shared<runtime::serial_execution_queue_t> (
      *executor, runtime::serial_execution_queue_options_t{});
    serializer_registry_t serializers;
    context->channel_runtime = std::make_shared<channel_runtime_state_t> ();
    context->channel_runtime->serializers = &serializers;
    context->spot_instance = std::make_shared<int> (1);
    auto timer = std::make_shared<timer_state_t> ();
    timer->name = "pending-sealed-tick";
    timer->period = std::chrono::milliseconds (1);
    auto handler = std::make_shared<int> (1);
    timer->handler_instance = handler;
    std::vector<timer_tick_t> delivered;
    timer->handler_invoker = [&] (void *, void *, serializer_registry_t &,
                                  const timer_tick_t &tick) -> task_t<zlink::message_t> {
        delivered.push_back (tick);
        co_return zlink::message_t{};
    };
    context->node->lane.run ([&] { context->admission_sealed = true; }).get ();
    timer_runtime_t::post_fire_count (context, timer, context->serial_queue, 1);
    timer_runtime_t::post_fire_count (context, timer, context->serial_queue, 1);
    context->drain_serial ();
    EXPECT_TRUE (delivered.empty ());
    EXPECT_EQ (timer->pending_fire_count, 2u);
    EXPECT_EQ (timer->delivery_index, 0u);
    EXPECT_TRUE (timer->failure_events.empty ());

    context->node->lane.run ([&] { context->admission_sealed = false; }).get ();
    timer_runtime_t::post_fire_count (context, timer, context->serial_queue, 0);
    context->drain_serial ();
    ASSERT_EQ (delivered.size (), 1u);
    EXPECT_EQ (delivered.front ().delivery_index, 1u);
    EXPECT_EQ (delivered.front ().scheduled_index, 2u);
    EXPECT_EQ (delivered.front ().skipped_ticks, 1u);
    EXPECT_EQ (timer->pending_fire_count, 0u);
    EXPECT_TRUE (timer->failure_events.empty ());
}

TEST (FrameworkRelocationSealContract, relocation_callbacks_are_immutable_after_transport_start)
{
    auto host = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = test_descriptor ("startup-relocation-callbacks"),
        .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()}});
    EXPECT_NO_THROW (host->configure_relocation_gate ({}));
    host->start ();
    // Force the former write/read overlap boundary: transport is already running.
    // Even clearing callbacks must be rejected before mutating the published set.
    EXPECT_THROW (host->configure_relocation_gate ({}), std::logic_error);
    host->close ();
}

TEST (FrameworkRelocationSealContract, aggregate_cut_uses_spot_gate_after_actor)
{
    zlink::framework::test::manual_internal_scheduler_t worker;
    auto gate = std::make_shared<runtime::serial_execution_queue_t> (
      worker, runtime::serial_execution_queue_options_t{},
      runtime::serial_execution_queue_t::error_handler_t{},
      runtime::serial_lane_policy_t::spot_wide ());
    auto host = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = test_descriptor ("aggregate-cut"),
        .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()}});
    host->configure_relocation_gate (
      [gate] (const auto &owner) -> task_t<std::shared_ptr<runtime::serial_execution_queue_t>> {
          co_return owner.kind == stateful::object_kind_t::actor ? nullptr : gate;
      });
    runtime::serial_execution_queue_t::async_completion_t finish;
    ASSERT_TRUE (
      gate->try_post_async ("active", [&] (auto complete) { finish = std::move (complete); }));
    worker.run_all ();
    ASSERT_TRUE (finish);
    auto record = std::make_shared<runtime::serial_application_record_t> ();
    record->payload ().emplace (stateful::turn_record_t{42, {1}});
    runtime::serial_work_options_t options;
    options.application_record = record;
    bool ran = false;
    ASSERT_TRUE (gate->try_post ("suffix", [&] { ran = true; }, std::move (options)));
    worker.run_all ();
    const stateful::object_ref_t actor{
      stateful::object_kind_t::actor, "actor", 1, 1, "mesh", "source"};
    const stateful::object_ref_t spot{
      stateful::object_kind_t::user_spot, "spot", 1, 1, "mesh", "source"};
    auto cutting = host::public_host_runtime_test_access_t::cut (*host, {actor, spot});
    worker.run_until (cutting);
    ASSERT_TRUE (cutting.await_ready ());
    const auto exported = cutting.result ().value ();
    ASSERT_TRUE (exported);
    ASSERT_EQ (exported->size (), 2u);
    EXPECT_TRUE ((*exported)[0].empty ());
    ASSERT_EQ ((*exported)[1].size (), 1u);
    EXPECT_EQ ((*exported)[1][0].sequence, 42u);
    finish ([] {});
    worker.run_all ();
    EXPECT_FALSE (ran);
    gate->drain ();
    worker.run_all ();
    host->close ();
}

TEST (FrameworkRelocationSealContract, cut_exception_preserves_cause_after_abort)
{
    stateful::stateful_object_runtime_t objects;
    const stateful::object_ref_t spot{
      stateful::object_kind_t::user_spot, "spot", 1, 1, "mesh", "source"};
    const auto created = begin_reserved_object (objects, spot, "room", {});
    ASSERT_EQ (objects.commit_create (created.attempt), stateful::stateful_error_t::none);
    objects.configure_relocation_cut (
      [] (auto, auto,
          auto) -> task_t<std::optional<std::vector<std::vector<stateful::turn_record_t>>>> {
          throw std::runtime_error ("source cut failed");
          co_return std::nullopt;
      });
    const auto failed = objects.try_seal_relocation_aggregate ({spot}).result ();
    ASSERT_FALSE (failed.has_value ());
    try {
        (void) failed.value ();
        FAIL () << "cut failure must retain its exception";
    }
    catch (const std::runtime_error &error) {
        EXPECT_STREQ (error.what (), "source cut failed");
    }
    EXPECT_TRUE (objects.find (spot.kind, spot.key, stateful::object_state_t::ready));
}

TEST (FrameworkRelocationSealContract, cut_local_exception_reports_original_cause)
{
    auto failures = std::make_shared<runtime::runtime_failure_collector_t> ();
    int reports = 0;
    failures->bind_runtime_reporter ([&] (std::exception_ptr cause) {
        ++reports;
        try {
            std::rethrow_exception (cause);
        }
        catch (const std::runtime_error &error) {
            EXPECT_STREQ (error.what (), "source lookup failed");
        }
    });
    auto host = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = test_descriptor ("cut-error"), .runtime_failures = failures}});
    host->configure_stateful_dispatch (
      [] (const auto &) -> task_t<std::optional<stateful::accepted_record_authority_t>> {
          throw std::runtime_error ("source lookup failed");
          co_return std::nullopt;
      });
    const stateful::object_ref_t spot{
      stateful::object_kind_t::user_spot, "spot", 1, 1, "mesh", "source"};
    const auto created = begin_reserved_object (host->objects (), spot, "room", {});
    ASSERT_EQ (host->objects ().commit_create (created.attempt), stateful::stateful_error_t::none);
    auto application = std::make_shared<host::local_application_dispatch_t> ();
    application->owner.spot_id = spot.key;
    application->record.kind = host::record_kind_t::spot_request;
    application->record.spot_route = protocol::spot_route_fence_t{spot.key, 1, {1}, 1, 1, 3};
    application->record.operation_id = {1, 1};
    application->record.reply_route_id = 1;
    mesh::service_mailbox_record_t original;
    original.domain = mesh::service_mailbox_domain_t::application;
    original.application.emplace (std::move (*application));
    ASSERT_TRUE (
      host::public_host_runtime_test_access_t::ingest_cut (*host, spot, std::move (original))
        .result ());
    EXPECT_EQ (reports, 1);
    host->close ();
}

// Relocation flow §4.2 cut S: an accepted, unstarted original is saved exactly once wherever it
// waits — the source FIFO (B), a claim suspended on its source authority (C) or the mailbox
// (D). The started turn (A) reaches its terminal on the source first, and a record published
// after the cut (E) takes the ingress hold.
TEST (FrameworkRelocationSealContract, seal_cut_saves_each_unstarted_original_once)
{
    using namespace std::chrono_literals;
    using access_t = host::public_host_runtime_test_access_t;
    auto host = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = test_descriptor ("cut-owner"),
        .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()}});
    const protocol::request_source_fence_t fence{"source-owner", 3, {2}, 1};
    const auto claim_authority = std::make_shared<task_completion_source_t<void>> ();
    const auto suspended = std::make_shared<std::atomic_int> (0);
    host->configure_stateful_dispatch (
      [fence, claim_authority, suspended] (const stateful::accepted_record_authority_query_t &query)
        -> task_t<std::optional<stateful::accepted_record_authority_t>> {
          // C's claim, the first query from source 3, waits at an explicit barrier.
          if (query.source_node_routing_id == std::vector<std::uint8_t>{3}
              && suspended->fetch_add (1) == 0)
              co_await claim_authority->task ();
          co_return stateful::accepted_record_authority_t{fence, 3};
      });
    auto &objects = host->objects ();
    auto &dispatch = access_t::dispatch (*host);
    auto &mailbox = host->transport ().mailbox ();
    const stateful::object_ref_t spot{
      stateful::object_kind_t::user_spot, "cut-spot", 1, 1, "mesh", "source"};
    const auto created = begin_reserved_object (objects, spot, "room", {});
    ASSERT_EQ (created.error, stateful::stateful_error_t::none);
    ASSERT_EQ (objects.commit_create (created.attempt), stateful::stateful_error_t::none);
    std::vector<mesh::service_mailbox_record_t> claimed;
    claimed.push_back (spot_send (*host, spot, 1, 2));
    claimed.push_back (spot_send (*host, spot, 2, 2));
    claimed.push_back (spot_send (*host, spot, 3, 3));
    const auto claim = zlink::framework::test::publish_stateful_claims (host->transport (), spot,
                                                                        std::move (claimed));
    const auto retained = zlink::framework::test::retained_ingress_snapshot (objects, spot);
    const auto slot = [&] (std::size_t index) {
        return mesh::service_mailbox_t::claim_handle_t{claim, index};
    };
    ASSERT_EQ (dispatch.ingest_claimed (spot, slot (0), retained).result ().value (),
               stateful::stateful_error_t::none);
    // A starts: the state lane hands its turn to the execution owner.
    const auto [started_error, started] = dispatch.try_claim (spot);
    ASSERT_EQ (started_error, stateful::stateful_error_t::none);
    ASSERT_TRUE (started);
    // B waits in the source FIFO, C in its claim, D in the mailbox behind that claim.
    ASSERT_EQ (dispatch.ingest_claimed (spot, slot (1), retained).result ().value (),
               stateful::stateful_error_t::none);
    auto late = dispatch.ingest_claimed (spot, slot (2), retained);
    ASSERT_FALSE (late.await_ready ());
    ASSERT_TRUE (mailbox.try_enqueue (spot_send (*host, spot, 4, 2)));
    auto sealing = std::async (std::launch::async, [&] {
        return objects.try_seal_relocation_aggregate ({spot}).result ().value ();
    });
    // The cut moves C and D behind B in the source FIFO; the seal then waits for A.
    EXPECT_TRUE (eventually (
      [&] { return objects.pending (spot, stateful::turn_domain_t::application) == 3u; }));
    EXPECT_EQ (mailbox.pending_messages (mesh::service_mailbox_domain_t::application), 0u);
    ASSERT_TRUE (mailbox.try_enqueue (spot_send (*host, spot, 5, 2)));
    EXPECT_EQ (sealing.wait_for (100ms), std::future_status::timeout);
    // C's claim lost its take: no second admission and no settlement of the cut credit.
    const auto outstanding = claim->outstanding ();
    claim_authority->complete (result_t<void>::success ());
    EXPECT_EQ (late.result ().value (), stateful::stateful_error_t::not_found);
    EXPECT_EQ (claim->outstanding (), outstanding);
    // A's terminal releases the seal.
    ASSERT_EQ (dispatch.complete (*started, std::nullopt), stateful::stateful_error_t::none);
    const auto sealed = sealing.get ();
    ASSERT_EQ (sealed.error, stateful::stateful_error_t::none);
    ASSERT_EQ (sealed.seal.participants.size (), 1u);
    EXPECT_EQ (operations_of (sealed.seal.participants.front ().pending_application),
               (std::vector<std::uint64_t>{2, 3, 4}));
    // The saved credits return once relocation owns their payloads; E is claimed next and
    // takes the ingress hold.
    dispatch.release_relocated_payloads (spot,
                                         sealed.seal.participants.front ().pending_application);
    EXPECT_EQ (zlink::framework::test::ingest_stateful_mailbox_record (objects, host->transport (),
                                                                       dispatch, spot)
                 .result ()
                 .value (),
               stateful::stateful_error_t::none);
    const auto [boundary_error, batch] = objects.begin_relocation_boundary (sealed.seal.token);
    ASSERT_EQ (boundary_error, stateful::stateful_error_t::none);
    ASSERT_EQ (batch.participants.size (), 1u);
    EXPECT_EQ (operations_of (batch.participants.front ().records),
               (std::vector<std::uint64_t>{5}));
    host->close ();
}

TEST (FrameworkRelocationSealContract, seal_conversion_failure_restores_fifo_once)
{
    using access_t = host::public_host_runtime_test_access_t;
    auto host = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = test_descriptor ("cut-restore"),
        .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()}});
    const protocol::request_source_fence_t fence{"source-owner", 3, {2}, 1};
    host->configure_stateful_dispatch (
      [fence] (const auto &) -> task_t<std::optional<stateful::accepted_record_authority_t>> {
          co_return stateful::accepted_record_authority_t{fence, 3};
      });
    auto &objects = host->objects ();
    auto &dispatch = access_t::dispatch (*host);
    auto &mailbox = host->transport ().mailbox ();
    const stateful::object_ref_t spot{
      stateful::object_kind_t::user_spot, "restore-spot", 1, 1, "mesh", "source"};
    const auto created = begin_reserved_object (objects, spot, "room", {});
    ASSERT_EQ (created.error, stateful::stateful_error_t::none);
    ASSERT_EQ (objects.commit_create (created.attempt), stateful::stateful_error_t::none);
    std::vector<mesh::service_mailbox_record_t> claimed;
    claimed.push_back (spot_send (*host, spot, 1, 2, 11));
    const auto claim = zlink::framework::test::publish_stateful_claims (host->transport (), spot,
                                                                        std::move (claimed));
    ASSERT_EQ (dispatch
                 .ingest_claimed (spot, mesh::service_mailbox_t::claim_handle_t{claim, 0},
                                  zlink::framework::test::retained_ingress_snapshot (objects, spot))
                 .result ()
                 .value (),
               stateful::stateful_error_t::none);
    // The second record has no canonical form: its reply route ID is zero.
    protocol::spot_message_header_t header;
    header.target = {spot.key, spot.object_generation, {1}, 1, 1, 3};
    header.operation = {1, 2};
    header.correlation = 12;
    auto invalid = stateful::accepted_spot_record (header, fence,
                                                   {"Probe", "application/json", {'{', '}'}}, true);
    invalid.reply_route_id = 0;
    ASSERT_EQ (objects.enqueue (spot, stateful::turn_domain_t::application,
                                stateful::turn_record_t{1000, {}, 1, std::move (invalid)}),
               stateful::stateful_error_t::none);
    // D waits in the mailbox behind the active claim.
    ASSERT_TRUE (mailbox.try_enqueue (spot_send (*host, spot, 3, 2, 13)));
    const auto outstanding = claim->outstanding ();
    const auto sealed = objects.try_seal_relocation_aggregate ({spot}).result ().value ();
    EXPECT_EQ (sealed.error, stateful::stateful_error_t::conflict);
    // Abort restored the source FIFO with the cut original after the earlier ones; the claim
    // credit of B is still outstanding and the binding no longer holds new ingress.
    EXPECT_EQ (claim->outstanding (), outstanding);
    EXPECT_EQ (mailbox.pending_messages (mesh::service_mailbox_domain_t::application), 0u);
    std::vector<std::uint64_t> order;
    while (true) {
        const auto [error, turn] = objects.try_claim (spot, stateful::turn_domain_t::application);
        ASSERT_EQ (error, stateful::stateful_error_t::none);
        if (!turn)
            break;
        order.push_back (operation_of (*turn));
        ASSERT_EQ (objects.complete_claim (spot, stateful::turn_domain_t::application),
                   stateful::stateful_error_t::none);
    }
    EXPECT_EQ (order, (std::vector<std::uint64_t>{1, 2, 3}));
    host->close ();
}

// One failed participant capture aborts the aggregate: every participant returns to Ready
// with its own queued turn.
TEST (FrameworkRelocationSealContract, aggregate_participant_failure_aborts_every_participant)
{
    stateful::stateful_object_runtime_t objects;
    objects.replace_placement_candidates ({{.mesh_name = "mesh",
                                            .node_id = "source",
                                            .stable_types = {"actor"},
                                            .active_capacity = 2,
                                            .pending_capacity = 2}});
    std::vector<stateful::object_ref_t> participants;
    for (const auto *key : {"first", "second"}) {
        const auto created = objects.begin_create ({.kind = stateful::object_kind_t::actor,
                                                    .key = key,
                                                    .stable_type = "actor",
                                                    .mesh_name = std::string ("mesh"),
                                                    .exclusive = true});
        ASSERT_EQ (created.status, stateful::create_status_t::reserved);
        ASSERT_EQ (objects.commit_create (created.attempt), stateful::stateful_error_t::none);
        ASSERT_EQ (objects.enqueue (created.object, stateful::turn_domain_t::application, {1, {1}}),
                   stateful::stateful_error_t::none);
        participants.push_back (created.object);
    }
    int captures = 0;
    objects.configure_relocation_state (
      [&] (const stateful::object_ref_t &owner, const std::string &stable_type,
           std::stop_token) -> task_t<stateful::frozen_object_state_t> {
          ++captures;
          if (owner.key == "second")
              throw std::runtime_error ("second participant capture failed");
          co_return stateful::frozen_object_state_t{.owner = owner, .stable_type = stable_type};
      },
      [] (const auto &, const auto &, std::stop_token) -> zlink::framework::task_t<bool> {
          co_return true;
      });
    const auto sealed = objects.try_seal_relocation_aggregate (participants).result ().value ();
    EXPECT_EQ (sealed.error, stateful::stateful_error_t::conflict);
    EXPECT_EQ (captures, 2);
    for (const auto &participant : participants) {
        SCOPED_TRACE (participant.key);
        const auto [error, turn] =
          objects.try_claim (participant, stateful::turn_domain_t::application);
        EXPECT_EQ (error, stateful::stateful_error_t::none);
        ASSERT_TRUE (turn);
        EXPECT_EQ (turn->sequence, 1u);
    }
    for (const auto &participant : participants)
        EXPECT_EQ (objects.complete_claim (participant, stateful::turn_domain_t::application),
                   stateful::stateful_error_t::none);
}
