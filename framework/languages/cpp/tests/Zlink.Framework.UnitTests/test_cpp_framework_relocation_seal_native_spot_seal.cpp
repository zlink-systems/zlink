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

// Relocation flow §4.2 step 10: when the second saved record fails its conversion, the attempt
// aborts and every original returns to the same source FIFO in order, with no reply and with
// its own mailbox credit still outstanding.
void verify_native_spot_seal (bool fail_second,
                              bool per_actor = false,
                              bool detached = false,
                              bool without_ingress = false)
{
    using namespace zlink::framework::runtime;
    using access_t = host::public_host_runtime_test_access_t;
    auto host = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = test_descriptor ("native-cut"),
        .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()}});
    const protocol::request_source_fence_t source{"requester-A", 3, {2}, 1};
    host->configure_stateful_dispatch (
      [source] (const auto &) -> task_t<std::optional<stateful::accepted_record_authority_t>> {
          co_return stateful::accepted_record_authority_t{source, 3};
      });
    const stateful::object_ref_t spot{
      stateful::object_kind_t::user_spot, "native-spot", 1, 1, "mesh", "source"};
    auto &objects = host->objects ();
    const auto created = begin_reserved_object (objects, spot, "room", {});
    ASSERT_EQ (created.error, stateful::stateful_error_t::none);
    ASSERT_EQ (objects.commit_create (created.attempt), stateful::stateful_error_t::none);
    offload_executor_t worker (1, "native-cut-test");
    auto gate = std::make_shared<serial_execution_queue_t> (
      worker, serial_execution_queue_options_t{}, serial_execution_queue_t::error_handler_t{},
      per_actor ? serial_lane_policy_t::per_actor_spot () : serial_lane_policy_t::spot_wide ());
    gate->set_application_start_policy ([] (const auto &) { return false; });
    auto located_gate = std::make_shared<std::shared_ptr<serial_execution_queue_t>> (gate);
    std::vector<std::shared_ptr<serial_application_record_t>> accepted;
    int handlers = 0;
    std::vector<std::uint64_t> executed;
    const auto native_wire = [&] (std::uint64_t operation,
                                  std::optional<std::uint64_t> correlation) {
        return native_spot_record (*host, spot, operation, correlation);
    };
    const mesh::service_mailbox_t::application_dispatch_t publish =
      [&] (const auto &, const host::receive_record_t &record, std::vector<zlink::message_t> parts,
           auto permit) {
          ASSERT_TRUE (record.application_record);
          auto owner = record.application_record;
          if (record.before_application_handler || permit)
              owner->admission.retain_initial (
                runtime::handler_entry_t (record.before_application_handler, std::move (permit)));
          if (fail_second && record.operation_id.low == 2) {
              auto invalid = protocol::decode_spot_message_header (
                protocol::message_bytes (native_wire (2, 77).parts ().front ()),
                protocol::command::spotRequest);
              invalid.correlation = 0;
              owner->set_spot_identity ({std::move (invalid), source}, parts.front ().copy ());
          }
          owner->set_invocation (parts.back ().copy (), {});
          accepted.push_back (owner);
          serial_work_options_t options;
          options.application_record = owner;
          ASSERT_TRUE (gate->try_post (
            "typed-native",
            [&, owner, operation = record.operation_id.low] {
                owner->admission.enter_handler ();
                ++handlers;
                executed.push_back (operation);
                (void) owner->complete_native (std::nullopt);
            },
            std::move (options)));
      };
    host->configure_relocation_gate (
      [located_gate] (const auto &) -> task_t<std::shared_ptr<serial_execution_queue_t>> {
          co_return *located_gate;
      },
      [located_gate] (std::string, std::optional<protocol::spot_route_fence_t>)
        -> task_t<std::shared_ptr<serial_application_target_t>> {
          if (!*located_gate)
              throw detail::make_framework_origin_exception (framework_error_kind_t::not_found,
                                                             "source activation was retired");
          co_return nullptr;
      },
      without_ingress ? publish : mesh::service_mailbox_t::application_dispatch_t{});
    for (std::uint64_t operation : {1u, 2u}) {
        const auto claim = zlink::framework::test::publish_stateful_claim (
          host->transport (), spot,
          native_wire (operation,
                       operation == 2 ? std::optional<std::uint64_t>{77} : std::nullopt));
        ASSERT_EQ (access_t::dispatch_claim (*host, claim.claim, publish).result ().value (), 1u);
        // Admission can publish before the gate imports its FIFO. The next claim
        // requires that import to return this original's native credit first.
        (void) gate->admit_native_application ({}).result ().value ();
        EXPECT_EQ (claim.claim->outstanding (), 0u);
    }
    ASSERT_EQ (accepted.size (), 2u);
    for (const auto &record : accepted)
        EXPECT_EQ (record->identity (), nullptr);
    EXPECT_EQ (objects.pending (spot, stateful::turn_domain_t::application), 0u);
    EXPECT_EQ (handlers, 0);
    const auto sealed = objects.try_seal_relocation_aggregate ({spot}).result ().value ();
    if (fail_second) {
        EXPECT_EQ (sealed.error, stateful::stateful_error_t::conflict);
    } else {
        ASSERT_EQ (sealed.error, stateful::stateful_error_t::none);
        ASSERT_EQ (sealed.seal.participants.size (), 1u);
        const auto &saved = sealed.seal.participants.front ().pending_application;
        ASSERT_EQ (saved.size (), 2u);
        EXPECT_EQ (operations_of (saved), (std::vector<std::uint64_t>{1, 2}));
        for (const auto &turn : saved) {
            ASSERT_TRUE (turn.frozen_record);
            EXPECT_FALSE (turn.payload.empty ());
            EXPECT_EQ (turn.frozen_record->source, source);
        }
        EXPECT_EQ (saved.back ().frozen_record->reply_route_id, 77u);
        std::vector<std::uint64_t> forwarded;
        if (detached) {
            const protocol::request_source_fence_t retained{"source-owner", 3, {1}, 1};
            ASSERT_EQ (objects
                         .begin_relocation_boundary (
                           sealed.seal.token,
                           [&] (const auto &, const auto &turn) {
                               forwarded.push_back (operation_of (turn));
                           },
                           retained)
                         .first,
                       stateful::stateful_error_t::none);
            located_gate->reset ();
        }
        const auto late = zlink::framework::test::publish_stateful_claim (host->transport (), spot,
                                                                          native_wire (3, 78));
        ASSERT_EQ (access_t::dispatch_claim (*host, late.claim, publish).result ().value (), 1u);
        EXPECT_EQ (accepted.size (), 2u);
        if (detached) {
            EXPECT_EQ (forwarded, (std::vector<std::uint64_t>{3}));
            *located_gate = gate;
        } else {
            const auto [error, boundary] = objects.begin_relocation_boundary (sealed.seal.token);
            ASSERT_EQ (error, stateful::stateful_error_t::none);
            EXPECT_EQ (operations_of (boundary.participants.front ().records),
                       (std::vector<std::uint64_t>{3}));
        }
        EXPECT_EQ (handlers, 0);
        ASSERT_EQ (objects.abort_relocation_before_cutover (sealed.seal.token).result ().value (),
                   stateful::stateful_error_t::none);
    }
    if (without_ingress) {
        // A production host binds its mailbox dispatcher and has no inventory scan.
        host->transport ().mailbox ().bind_application_dispatch ([] (auto &) {},
                                                                 [] (const auto &) {});
        (void) host->dispatch_ready (publish).result ().value ();
    } else {
        // Fresh native ingress may not pass the originals restored by this abort.
        const auto fresh = zlink::framework::test::publish_stateful_claim (
          host->transport (), spot, native_wire (4, std::nullopt));
        ASSERT_EQ (access_t::dispatch_claim (*host, fresh.claim, publish).result ().value (), 1u);
        while (access_t::dispatch_owner (*host, spot, publish).result ().value ()) {
        }
    }
    gate->set_application_start_policy ([] (const auto &) { return true; });
    gate->drain ();
    EXPECT_EQ (executed, without_ingress ? (std::vector<std::uint64_t>{1, 2, 3})
                         : fail_second   ? (std::vector<std::uint64_t>{1, 2, 4})
                                         : (std::vector<std::uint64_t>{1, 2, 3, 4}));
    EXPECT_EQ (objects.pending (spot, stateful::turn_domain_t::application), 0u);
    gate->close ();
    gate->drain ();
    worker.request_stop ();
    worker.drain ();
    host->close ();
}

TEST (FrameworkRelocationSealContract, native_spot_abort_executes_prefix_without_new_ingress)
{
    verify_native_spot_seal (false, false, false, true);
}

TEST (FrameworkRelocationSealContract,
      native_spot_materializes_only_at_seal_and_holds_later_ingress)
{
    verify_native_spot_seal (false);
}

TEST (FrameworkRelocationSealContract,
      native_spot_retained_ingress_survives_activation_gate_removal)
{
    verify_native_spot_seal (false, false, true);
    verify_native_spot_seal (false, true, true);
}

TEST (FrameworkRelocationSealContract, native_spot_second_conversion_failure_restores_originals)
{
    verify_native_spot_seal (true);
}

TEST (FrameworkRelocationSealContract, per_actor_spot_abort_restores_originals_before_fresh_native)
{
    verify_native_spot_seal (false, true);
    verify_native_spot_seal (true, true);
}

TEST (FrameworkRelocationSealContract, native_spot_commit_releases_source_gate)
{
    using namespace zlink::framework::runtime;
    auto host = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = test_descriptor ("native-commit"),
        .runtime_failures = std::make_shared<runtime_failure_collector_t> ()}});
    host->configure_stateful_dispatch (
      [] (const auto &) -> task_t<std::optional<stateful::accepted_record_authority_t>> {
          co_return std::nullopt;
      });
    const stateful::object_ref_t spot{
      stateful::object_kind_t::user_spot, "committed-spot", 1, 1, "mesh", "source"};
    const auto created = begin_reserved_object (host->objects (), spot, "room", {});
    ASSERT_EQ (host->objects ().commit_create (created.attempt), stateful::stateful_error_t::none);
    offload_executor_t worker (1, "source-commit");
    auto gate = std::make_shared<serial_execution_queue_t> (
      worker, serial_execution_queue_options_t{}, serial_execution_queue_t::error_handler_t{},
      serial_lane_policy_t::spot_wide ());
    host->configure_relocation_gate (
      [gate] (const auto &) -> task_t<std::shared_ptr<serial_execution_queue_t>> {
          co_return gate;
      });
    const auto sealed = host->objects ().try_seal_relocation_aggregate ({spot}).result ().value ();
    ASSERT_EQ (sealed.error, stateful::stateful_error_t::none);
    ASSERT_EQ (host->objects ().begin_relocation_boundary (sealed.seal.token).first,
               stateful::stateful_error_t::none);
    ASSERT_EQ (host->objects ().finalize_relocation_cutover (sealed.seal.token).result ().value (),
               stateful::stateful_error_t::none);
    int executed = 0;
    ASSERT_TRUE (gate->try_post ("ordinary-after-commit", [&] { ++executed; }));
    std::promise<void> observed;
    auto completed = observed.get_future ();
    ASSERT_TRUE (worker.try_submit_internal ([&] { observed.set_value (); }));
    completed.get ();
    EXPECT_EQ (executed, 1);
    gate->cancel_pending ();
    gate->close ();
    gate->drain ();
    worker.request_stop ();
    worker.drain ();
    host->close ();
}

TEST (FrameworkRelocationSealContract, native_spot_fallback_rereads_remote_and_local_original)
{
    using namespace zlink::framework::runtime;
    for (const bool local : {false, true}) {
        for (const bool reject_first_authority : {false, true}) {
            SCOPED_TRACE (local);
            SCOPED_TRACE (reject_first_authority);
            auto host = std::make_shared<host::public_host_runtime_t> (
              host::host_options_t{mesh::raw_mesh_node_options_t{
                .descriptor = test_descriptor ("native-fallback"),
                .runtime_failures = std::make_shared<runtime_failure_collector_t> ()}});
            const protocol::request_source_fence_t source{"requester", 3, {2}, 1};
            int resolutions = 0;
            host->configure_stateful_dispatch (
              [&] (const stateful::accepted_record_authority_query_t &query)
                -> task_t<std::optional<stateful::accepted_record_authority_t>> {
                  EXPECT_EQ (query.source_node_routing_id, (std::vector<std::uint8_t>{2}));
                  ++resolutions;
                  if (reject_first_authority && resolutions == 1)
                      co_return std::nullopt;
                  co_return stateful::accepted_record_authority_t{source, 3};
              });
            const stateful::object_ref_t spot{
              stateful::object_kind_t::user_spot, "fallback-spot", 1, 1, "mesh", "source"};
            const auto created = begin_reserved_object (host->objects (), spot, "room", {});
            ASSERT_EQ (host->objects ().commit_create (created.attempt),
                       stateful::stateful_error_t::none);
            auto original = native_spot_record (*host, spot, 91, std::nullopt);
            if (local) {
                auto application = std::make_shared<host::local_application_dispatch_t> ();
                application->owner.owner_kind = host::owner_kind_t::spot;
                application->owner.spot_id = spot.key;
                application->record.kind = host::record_kind_t::spot_send;
                application->record.operation_id = {1, 91};
                application->record.source_node_rid =
                  zlink::routing_id_t::from (original.source_routing_id);
                application->record.spot_route =
                  protocol::spot_route_fence_t{spot.key, 1, {1}, 1, 1, 3};
                application->parts =
                  protocol::decode_application_parts (protocol::decode_application_payload (
                    protocol::message_bytes (original.parts ().back ())));
                original.application.emplace (std::move (*application));
                original.replace_parts ({});
            }
            const auto slot = zlink::framework::test::publish_stateful_claim (
              host->transport (), spot, std::move (original));
            int dispatched = 0;
            EXPECT_EQ (host::public_host_runtime_test_access_t::dispatch_claim (
                         *host, slot.claim,
                         [&] (const auto &, const host::receive_record_t &record,
                              std::vector<zlink::message_t> parts, auto) {
                             ++dispatched;
                             EXPECT_EQ (record.operation_id.low, 91u);
                             ASSERT_EQ (parts.size (), 2u);
                             EXPECT_EQ (parts.back ().to_string (), "{}");
                             if (record.complete_stateful_dispatch)
                                 record.complete_stateful_dispatch ();
                             if (record.release_mailbox_reservation)
                                 record.release_mailbox_reservation ();
                         })
                         .result ()
                         .value (),
                       1u);
            EXPECT_EQ (resolutions, 2);
            EXPECT_EQ (dispatched, 1);
            EXPECT_EQ (slot.claim->outstanding (), 0u);
            host->close ();
        }
    }
}

TEST (FrameworkRelocationSealContract, native_spot_target_wait_keeps_original_and_gate_free)
{
    using namespace zlink::framework::runtime;
    auto host = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = test_descriptor ("native-target"),
        .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()}});
    const protocol::request_source_fence_t source{"requester", 3, {2}, 1};
    host->configure_stateful_dispatch (
      [source] (const auto &) -> task_t<std::optional<stateful::accepted_record_authority_t>> {
          co_return stateful::accepted_record_authority_t{source, 3};
      });
    const stateful::object_ref_t spot{
      stateful::object_kind_t::user_spot, "target-spot", 1, 1, "mesh", "source"};
    const auto created = begin_reserved_object (host->objects (), spot, "room", {});
    ASSERT_EQ (created.error, stateful::stateful_error_t::none);
    ASSERT_EQ (host->objects ().commit_create (created.attempt), stateful::stateful_error_t::none);
    offload_executor_t worker (1, "target-owner");
    auto gate = std::make_shared<serial_execution_queue_t> (
      worker, serial_execution_queue_options_t{}, serial_execution_queue_t::error_handler_t{},
      serial_lane_policy_t::spot_wide ());
    task_completion_source_t<void> entered;
    task_completion_source_t<std::shared_ptr<serial_application_target_t>> release;
    host->configure_relocation_gate (
      [gate] (const auto &) -> task_t<std::shared_ptr<serial_execution_queue_t>> {
          co_return gate;
      },
      [&] (std::string, std::optional<protocol::spot_route_fence_t>)
        -> task_t<std::shared_ptr<serial_application_target_t>> {
          entered.complete (result_t<void>::success ());
          co_return co_await release.task ();
      });
    const auto slot = zlink::framework::test::publish_stateful_claim (
      host->transport (), spot, native_spot_record (*host, spot, 1, std::nullopt));
    int dispatched = 0;
    auto pending = host::public_host_runtime_test_access_t::dispatch_claim (
      *host, slot.claim,
      [&] (const auto &, const host::receive_record_t &record, std::vector<zlink::message_t>,
           auto) {
          ++dispatched;
          record.release_mailbox_reservation ();
      });
    entered.task ().result ().value ();
    EXPECT_FALSE (pending.await_ready ());
    EXPECT_TRUE (
      host->transport ()
        .mailbox ()
        .prepare (slot, [] (const auto &original) { return !original.parts ().empty (); })
        .value_or (false));
    auto pulse = gate->admit_native_application ({});
    // A pending target projection must not retain the execution gate.
    EXPECT_TRUE (pulse.result ());
    EXPECT_EQ (dispatched, 0);
    release.complete (result_t<std::shared_ptr<serial_application_target_t>>::success (nullptr));
    EXPECT_EQ (pending.result ().value (), 1u);
    EXPECT_EQ (dispatched, 1);
    EXPECT_EQ (slot.claim->outstanding (), 0u);
    gate->close ();
    gate->drain ();
    worker.request_stop ();
    worker.drain ();
    host->close ();
}
