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

namespace
{
mesh::service_mailbox_record_t local_native_spot_record (host::public_host_runtime_t &host,
                                                         const stateful::object_ref_t &spot,
                                                         std::uint64_t operation)
{
    auto original = native_spot_record (host, spot, operation, std::nullopt);
    const auto bytes = protocol::message_bytes (original.parts ().front ());
    const auto header =
      protocol::decode_spot_message_header (bytes, protocol::decode_header (bytes).kind);
    original.application.emplace ();
    original.application->owner.owner_kind = host::owner_kind_t::spot;
    original.application->owner.spot_id = spot.key;
    original.application->record.kind = host::record_kind_t::spot_send;
    original.application->record.operation_id = {1, operation};
    original.application->record.source_node_rid =
      zlink::routing_id_t::from (original.source_routing_id);
    original.application->record.spot_route = header.target;
    original.application->parts =
      protocol::decode_application_parts (std::move (original.body_parts ().back ()));
    original.replace_parts ({});
    return original;
}
} // namespace

TEST (FrameworkRelocationSealContract, native_preparation_outcomes_preserve_context_and_settlement)
{
    using namespace zlink::framework::runtime;
    for (const bool local : {false, true}) {
        for (const bool deferred : {false, true}) {
            for (const bool failed : {false, true}) {
                SCOPED_TRACE (::testing::Message () << local << "/" << deferred << "/" << failed);
                auto host = std::make_shared<host::public_host_runtime_t> (
                  host::host_options_t{mesh::raw_mesh_node_options_t{
                    .descriptor = test_descriptor ("prepare-outcome"),
                    .runtime_failures =
                      std::make_shared<runtime::runtime_failure_collector_t> ()}});
                host->configure_stateful_dispatch ([] (const auto &) {
                    return task_t<std::optional<stateful::accepted_record_authority_t>> (
                      result_t<std::optional<stateful::accepted_record_authority_t>>::success (
                        stateful::accepted_record_authority_t{{"requester", 3, {2}, 1}, 3}));
                });
                const stateful::object_ref_t spot{
                  stateful::object_kind_t::user_spot, "target", 1, 1, "mesh", "source"};
                const auto created = begin_reserved_object (host->objects (), spot, "room", {});
                ASSERT_EQ (created.error, stateful::stateful_error_t::none);
                ASSERT_EQ (host->objects ().commit_create (created.attempt),
                           stateful::stateful_error_t::none);
                using target_t = std::shared_ptr<serial_application_target_t>;
                task_completion_source_t<target_t> release;
                auto expected = std::make_shared<serial_application_target_t> ();
                auto context = std::make_shared<int> (17);
                expected->context = context;
                unsigned preparations = 0;
                host->configure_relocation_gate (
                  {}, [&] (std::string, std::optional<protocol::spot_route_fence_t>) {
                      ++preparations;
                      if (deferred)
                          return release.task ();
                      if (failed)
                          return task_t<target_t> (result_t<target_t>::failure (
                            framework_error_kind_t::not_found, "target no longer exists"));
                      return task_t<target_t> (result_t<target_t>::success (expected));
                  });
                const auto slot = zlink::framework::test::publish_stateful_claim (
                  host->transport (), spot,
                  local ? local_native_spot_record (*host, spot, 1)
                        : native_spot_record (*host, spot, 1, std::nullopt));
                unsigned dispatched = 0;
                auto pending = host::public_host_runtime_test_access_t::dispatch_claim (
                  *host, slot.claim, [&] (const auto &, const auto &record, auto, auto) {
                      ++dispatched;
                      ASSERT_TRUE (record.application_record);
                      const auto target = record.application_record->target;
                      ASSERT_TRUE (target);
                      if (failed) {
                          const auto error = std::get_if<framework_exception_t> (&target->context);
                          ASSERT_NE (error, nullptr);
                          EXPECT_EQ (error->kind (), framework_error_kind_t::not_found);
                      } else {
                          EXPECT_EQ (target, expected);
                          EXPECT_EQ (std::get<std::shared_ptr<void>> (target->context), context);
                      }
                      record.application_record->release_native_claim ();
                  });
                EXPECT_EQ (preparations, 1u);
                if (deferred) {
                    EXPECT_FALSE (pending.await_ready ());
                    EXPECT_EQ (dispatched, 0u);
                    EXPECT_EQ (slot.claim->outstanding (), 1u);
                    EXPECT_TRUE (host->transport ()
                                   .mailbox ()
                                   .prepare (slot,
                                             [] (const auto &record) {
                                                 return record.application
                                                          ? !record.application->parts.empty ()
                                                          : !record.parts ().empty ();
                                             })
                                   .value_or (false));
                    release.complete (
                      failed ? result_t<target_t>::failure (framework_error_kind_t::not_found,
                                                            "target no longer exists")
                             : result_t<target_t>::success (expected));
                }
                EXPECT_EQ (pending.result ().value (), 1u);
                EXPECT_EQ (dispatched, 1u);
                EXPECT_EQ (slot.claim->outstanding (), 0u);
                EXPECT_FALSE (host->transport ().mailbox ().release (*slot.claim));
                host->close ();
            }
        }
    }
}

TEST (FrameworkRelocationSealContract, native_claim_target_failure_settles_only_failed_record)
{
    using namespace zlink::framework::runtime;
    for (const bool local : {false, true}) {
        for (const bool deferred : {false, true}) {
            for (const unsigned failed : {1u, 2u, 3u}) {
                for (const unsigned failure_kind : {0u, 1u, 2u}) {
                    const bool cancelled = failure_kind == 1;
                    const bool shutdown = failure_kind == 2;
                    SCOPED_TRACE (::testing::Message ()
                                  << "local=" << local << " deferred=" << deferred
                                  << " failed=" << failed << " failure_kind=" << failure_kind);
                    auto failures = std::make_shared<runtime_failure_collector_t> ();
                    std::vector<std::exception_ptr> reported;
                    failures->bind_runtime_reporter (
                      [&] (auto error) { reported.push_back (error); });
                    const auto marker = std::make_shared<const int> (369304);
                    using failed_t = zlink::framework::test::marked_exception_t<std::runtime_error>;
                    using cancelled_t =
                      zlink::framework::test::marked_exception_t<std::system_error>;
                    const cancelled_t cancellation (
                      marker, std::make_error_code (std::errc::operation_canceled),
                      "target preparation cancelled");
                    const auto error =
                      shutdown
                        ? std::make_exception_ptr (detail::make_framework_origin_exception (
                            framework_error_kind_t::shutting_down, "target admission closed"))
                      : cancelled
                        ? std::make_exception_ptr (cancellation)
                        : std::make_exception_ptr (failed_t (marker, "target preparation failed"));
                    auto host = std::make_shared<host::public_host_runtime_t> (
                      host::host_options_t{mesh::raw_mesh_node_options_t{
                        .descriptor = test_descriptor ("native-target-failure"),
                        .runtime_failures = failures}});
                    host->configure_stateful_dispatch ([] (const auto &) {
                        return task_t<std::optional<stateful::accepted_record_authority_t>> (
                          result_t<std::optional<stateful::accepted_record_authority_t>>::success (
                            stateful::accepted_record_authority_t{{"requester", 3, {2}, 1}, 3}));
                    });
                    const stateful::object_ref_t spot{
                      stateful::object_kind_t::user_spot, "suffix", 1, 1, "mesh", "source"};
                    const auto created = begin_reserved_object (host->objects (), spot, "room", {});
                    ASSERT_EQ (created.error, stateful::stateful_error_t::none);
                    ASSERT_EQ (host->objects ().commit_create (created.attempt),
                               stateful::stateful_error_t::none);
                    using target_t = std::shared_ptr<serial_application_target_t>;
                    task_completion_source_t<target_t> release;
                    unsigned preparations = 0;
                    host->configure_relocation_gate (
                      {},
                      [&] (std::string,
                           std::optional<protocol::spot_route_fence_t>) -> task_t<target_t> {
                          if (++preparations == failed || (shutdown && preparations > failed)) {
                              if (deferred)
                                  return release.task ();
                              std::rethrow_exception (error);
                          }
                          return task_t<target_t> (result_t<target_t>::success (
                            std::make_shared<serial_application_target_t> ()));
                      });
                    std::vector<mesh::service_mailbox_record_t> originals;
                    std::vector<std::uint64_t> expected;
                    for (unsigned index = 1; index <= 3; ++index) {
                        originals.push_back (
                          local ? local_native_spot_record (*host, spot, index)
                                : native_spot_record (*host, spot, index, std::nullopt));
                        if (shutdown ? index < failed : index != failed)
                            expected.push_back (index);
                    }
                    const auto claim = zlink::framework::test::publish_stateful_claims (
                      host->transport (), spot, std::move (originals));
                    std::vector<std::uint64_t> dispatched;
                    unsigned rejected = 0;
                    auto pending = host::public_host_runtime_test_access_t::dispatch_claim (
                      *host, claim, [&] (const auto &, const auto &record, auto, auto) {
                          ASSERT_TRUE (record.application_record);
                          const auto *failure = std::get_if<framework_exception_t> (
                            &record.application_record->target->context);
                          if (failure) {
                              EXPECT_TRUE (shutdown);
                              EXPECT_EQ (failure->kind (), framework_error_kind_t::shutting_down);
                              ++rejected;
                          } else {
                              dispatched.push_back (record.operation_id.low);
                          }
                          record.application_record->release_native_claim ();
                      });
                    if (deferred) {
                        EXPECT_FALSE (pending.await_ready ());
                        EXPECT_EQ (preparations, failed);
                        EXPECT_EQ (dispatched.size (), failed - 1);
                        EXPECT_EQ (claim->outstanding (), 4u - failed);
                        release.complete (detail::result_access_t::failure<target_t> (error));
                    }
                    EXPECT_EQ (pending.result ().value (), shutdown ? 3u : 2u);
                    EXPECT_EQ (preparations, 3u);
                    EXPECT_EQ (dispatched, expected);
                    EXPECT_EQ (rejected, shutdown ? 4u - failed : 0u);
                    ASSERT_EQ (reported.size (), shutdown ? 0u : 1u);
                    if (!shutdown) {
                        if (cancelled)
                            EXPECT_TRUE (
                              zlink::framework::test::exception_payload_matches<cancelled_t> (
                                reported.front (), [&] (const auto &cause) {
                                    return cause.marker == marker
                                           && cause.code () == cancellation.code ()
                                           && std::string (cause.what ()) == cancellation.what ();
                                }));
                        else
                            EXPECT_TRUE (
                              zlink::framework::test::exception_payload_matches<failed_t> (
                                reported.front (), [&] (const auto &cause) {
                                    return cause.marker == marker
                                           && std::string (cause.what ())
                                                == "target preparation failed";
                                }));
                    }
                    EXPECT_EQ (claim->outstanding (), 0u);
                    EXPECT_FALSE (host->transport ().mailbox ().release (*claim));
                    EXPECT_EQ (host->transport ().mailbox ().pending_messages (
                                 mesh::service_mailbox_domain_t::application),
                               0u);
                    host->close ();
                }
            }
        }
    }
}

TEST (FrameworkRelocationSealContract, per_actor_native_publication_and_cut_share_one_owner)
{
    using namespace zlink::framework::runtime;
    using namespace std::chrono_literals;
    auto host = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = test_descriptor ("native-publication"),
        .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()}});
    const protocol::request_source_fence_t source{"requester", 3, {2}, 1};
    host->configure_stateful_dispatch (
      [source] (const auto &) -> task_t<std::optional<stateful::accepted_record_authority_t>> {
          co_return stateful::accepted_record_authority_t{source, 3};
      });
    const stateful::object_ref_t spot{
      stateful::object_kind_t::user_spot, "publication-spot", 1, 1, "mesh", "source"};
    const auto created = begin_reserved_object (host->objects (), spot, "room", {});
    ASSERT_EQ (created.error, stateful::stateful_error_t::none);
    ASSERT_EQ (host->objects ().commit_create (created.attempt), stateful::stateful_error_t::none);
    offload_executor_t worker (1, "publication-owner");
    auto gate = std::make_shared<serial_execution_queue_t> (
      worker, serial_execution_queue_options_t{}, serial_execution_queue_t::error_handler_t{},
      serial_lane_policy_t::per_actor_spot ());
    gate->set_application_start_policy ([] (const auto &) { return false; });
    host->configure_relocation_gate (
      [gate] (const auto &) -> task_t<std::shared_ptr<serial_execution_queue_t>> {
          co_return gate;
      });
    std::promise<void> entered, release;
    auto entering = entered.get_future ();
    auto held = release.get_future ().share ();
    const auto slot = zlink::framework::test::publish_stateful_claim (
      host->transport (), spot, native_spot_record (*host, spot, 1, std::nullopt));
    auto publishing = std::async (std::launch::async, [&] {
        return host::public_host_runtime_test_access_t::dispatch_claim (
                 *host, slot.claim,
                 [&] (const auto &, const host::receive_record_t &record,
                      std::vector<zlink::message_t> parts, auto permit) {
                     auto original = record.application_record;
                     if (record.before_application_handler || permit)
                         original->admission.retain_initial (runtime::handler_entry_t (
                           record.before_application_handler, std::move (permit)));
                     original->set_invocation (parts.back ().copy (), {});
                     entered.set_value ();
                     held.wait ();
                     serial_work_options_t options;
                     options.application_record = std::move (original);
                     EXPECT_TRUE (gate->try_post ("accepted-native", [] {}, std::move (options)));
                 })
          .result ()
          .value ();
    });
    const auto observed = entering.wait_for (5s) == std::future_status::ready;
    EXPECT_TRUE (observed);
    if (!observed) {
        release.set_value ();
        (void) publishing.get ();
        return;
    }
    auto binding = std::make_shared<stateful::relocation_seal_binding_t> ();
    auto cut = gate->take_application_suffix (
      [binding] { binding->bound.store (true, std::memory_order_release); },
      [] (const std::function<void ()> &step) {
          step ();
          return true;
      },
      binding);
    EXPECT_FALSE (cut.await_ready ());
    release.set_value ();
    EXPECT_EQ (publishing.get (), 1u);
    const auto saved = cut.result ().value ();
    EXPECT_EQ (saved.size (), 1u);
    gate->cancel_pending ();
    gate->close ();
    gate->drain ();
    worker.request_stop ();
    worker.drain ();
    host->close ();
}

TEST (FrameworkRelocationSealContract, native_public_requester_terminal_uses_registry_after_seal)
{
    using namespace zlink::framework::runtime;
    using namespace std::chrono_literals;
    auto host = std::make_shared<host::public_host_runtime_t> (host::host_options_t{
      .mesh =
        mesh::raw_mesh_node_options_t{.descriptor = test_descriptor ("native-requester"),
                                      .runtime_failures =
                                        std::make_shared<runtime::runtime_failure_collector_t> ()},
      .object_stable_types = {"framework.spot"}});
    auto lease = std::make_shared<const location_owner_token_t> (
      location_owner_token_t{"native-requester-owner", 3});
    host->configure_stateful_dispatch ([lease] { return lease; });
    host->configure_spot_route_fence_resolver (
      [] (const auto &, const auto &, auto) -> task_t<std::optional<host::route_fence_t>> {
          co_return host::route_fence_t{1, 3, 1};
      });
    const stateful::object_ref_t spot{
      stateful::object_kind_t::user_spot, "native-local", 1, 1, "mesh", "native-requester"};
    const auto created = begin_reserved_object (host->objects (), spot, "framework.spot", {});
    ASSERT_EQ (created.error, stateful::stateful_error_t::none);
    ASSERT_EQ (host->objects ().commit_create (created.attempt), stateful::stateful_error_t::none);
    offload_executor_t worker (1, "native-requester-owner");
    auto gate = std::make_shared<serial_execution_queue_t> (
      worker, serial_execution_queue_options_t{}, serial_execution_queue_t::error_handler_t{},
      serial_lane_policy_t::spot_wide ());
    gate->set_application_start_policy ([] (const auto &) { return false; });
    host->configure_relocation_gate (
      [gate] (const auto &) -> task_t<std::shared_ptr<serial_execution_queue_t>> {
          co_return gate;
      });
    host->start ();
    EXPECT_THROW (host->configure_relocation_gate ({}), std::logic_error);
    messaging::envelope_header_t header;
    header.kind = messaging::message_kind_t::request;
    header.channel_name = "mesh";
    header.message_name = "NativeSourceRequest";
    std::vector<zlink::message_t> parts;
    parts.push_back (messaging::envelope_codec_t{}.encode_header (header));
    parts.push_back (zlink::message_t::from ("{}"));
    host::pending_operation_t pending;
    ASSERT_EQ (host->entry_spot ()
                 .request_to_spot (host->status ().routing_id (), spot.key, spot.object_generation,
                                   parts, pending, zlink::send_flags_t::none, 15s)
                 .result ()
                 .value (),
               zlink::submit_result_t::ok);
    const auto claim = host->transport ().mailbox ().try_claim_owner (
      mesh::service_mailbox_domain_t::application,
      mesh::service_mailbox_t::application_owner (host::owner_kind_t::spot, spot.key), 1, 65536);
    ASSERT_TRUE (claim);
    ASSERT_TRUE (claim->records.front ()->application);
    const auto *original_parts = claim->records.front ()->application->parts.data ();
    ASSERT_EQ (
      host::public_host_runtime_test_access_t::dispatch_claim (
        *host, claim,
        [gate, original_parts] (const auto &, const host::receive_record_t &record,
                                std::vector<zlink::message_t> application, auto permit) {
            EXPECT_EQ (application.data (), original_parts);
            if (record.before_application_handler || permit)
                record.application_record->admission.retain_initial (
                  runtime::handler_entry_t (record.before_application_handler, std::move (permit)));
            record.application_record->set_invocation (application.back ().copy (), {});
            serial_work_options_t options;
            options.application_record = record.application_record;
            EXPECT_TRUE (gate->try_post ("native-local", [] {}, std::move (options)));
        })
        .result ()
        .value (),
      1u);
    const auto sealed = host->objects ().try_seal_relocation_aggregate ({spot}).result ().value ();
    ASSERT_EQ (sealed.error, stateful::stateful_error_t::none);
    const auto &saved = sealed.seal.participants.front ().pending_application;
    ASSERT_EQ (saved.size (), 1u);
    EXPECT_EQ (saved.front ().sequence, 0u);
    ASSERT_TRUE (saved.front ().frozen_record);
    const protocol::relocation_coordinator_fence_t coordinator{
      lease->owner_id, 3, host->status ().routing_id ().to_bytes (),
      host->status ().lifecycle_generation (), "native-local-authority"};
    const auto relay = stateful::maintenance_runtime_t::terminal_identity (
      {2119, 6}, 1, coordinator, 1, *saved.front ().frozen_record);
    header.kind = messaging::message_kind_t::response;
    std::vector<zlink::message_t> response;
    response.push_back (messaging::envelope_codec_t{}.encode_header (header));
    response.push_back (zlink::message_t::from ("{}"));
    // The production source registration uses this completion port. Native originals
    // have no raw pending entry; their original public operation owns the reply.
    const bool completed =
      host::public_host_runtime_test_access_t::dispatch (*host)
        .complete_relocated_source_async (spot, saved.front ().sequence, relay,
                                          protocol::application_payload_t::from_parts (response))
        .result ()
        .value ();
    EXPECT_TRUE (completed);
    const bool ready = completed && eventually ([&] {
                           (void) host
                             ->dispatch_ready ([] (const auto &, const auto &,
                                                   std::vector<zlink::message_t>, auto) {})
                             .result ();
                           return pending.completion->task ().await_ready ();
                       });
    EXPECT_TRUE (ready);
    if (ready) {
        const auto result = pending.completion->task ().result ();
        ASSERT_TRUE (result);
        EXPECT_EQ (result.value ().parts.back ().to_string (), "{}");
        EXPECT_EQ (host->transport ().pending_operation_count (), 0u);
    }
    EXPECT_EQ (
      host->objects ().abort_relocation_before_cutover (sealed.seal.token).result ().value (),
      stateful::stateful_error_t::none);
    EXPECT_EQ (host->objects ().discard_application (spot, 0), stateful::stateful_error_t::none);
    gate->close ();
    gate->drain ();
    worker.request_stop ();
    worker.drain ();
    host->close ();
}

TEST (FrameworkRelocationSealContract,
      public_requester_receives_relocated_terminal_without_old_owner)
{
    using namespace zlink::framework::runtime;
    using namespace std::chrono_literals;
    const auto make_host = [] (std::string rid) {
        return std::make_shared<host::public_host_runtime_t> (host::host_options_t{
          .mesh =
            mesh::raw_mesh_node_options_t{
              .descriptor = test_descriptor (std::move (rid)),
              .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()},
          .object_stable_types = {"framework.spot"}});
    };
    auto requester = make_host ("requester-A");
    auto previous = make_host ("previous-B");
    auto target = make_host ("target-C");
    auto lease =
      std::make_shared<const location_owner_token_t> (location_owner_token_t{"requester-owner", 3});
    requester->configure_stateful_dispatch ([lease] { return lease; });
    requester->configure_spot_route_fence_resolver (
      [] (const auto &, const auto &, auto) -> task_t<std::optional<host::route_fence_t>> {
          co_return host::route_fence_t{1, 1, 1};
      });
    requester->start ();
    previous->start ();
    target->start ();
    ASSERT_TRUE (requester->connect_peer (previous->transport ().endpoint (),
                                          previous->status ().routing_id ()));
    ASSERT_TRUE (
      requester->connect_peer (target->transport ().endpoint (), target->status ().routing_id ()));
    ASSERT_TRUE (eventually ([&] {
        for (auto node : {requester, previous, target})
            (void) node->transport ().pump_one (std::chrono::steady_clock::now ()).result ();
        auto b =
          requester->transport ().topology ().peer (previous->status ().routing_id ().to_bytes ());
        auto c =
          requester->transport ().topology ().peer (target->status ().routing_id ().to_bytes ());
        return b && b->ready && c && c->ready;
    }));
    messaging::envelope_header_t envelope;
    envelope.kind = messaging::message_kind_t::request;
    envelope.channel_name = "mesh";
    envelope.message_name = "OriginalRequest";
    std::vector<zlink::message_t> parts;
    parts.push_back (messaging::envelope_codec_t{}.encode_header (envelope));
    parts.push_back (zlink::message_t::from ("{}"));
    host::pending_operation_t pending;
    auto submitted =
      requester->entry_spot ().request_to_spot (previous->status ().routing_id (), "relocated-spot",
                                                1, parts, pending, zlink::send_flags_t::none, 15s);
    ASSERT_EQ (submitted.result ().value (), zlink::submit_result_t::ok);
    ASSERT_TRUE (eventually ([&] {
        (void) previous->transport ().pump_one (std::chrono::steady_clock::now ()).result ();
        return previous->transport ().mailbox ().pending_messages (
                 mesh::service_mailbox_domain_t::application)
               != 0;
    }));
    auto claim = previous->transport ().mailbox ().try_claim (
      mesh::service_mailbox_domain_t::application, 1, 65536);
    ASSERT_TRUE (claim);
    auto accepted = previous->transport ().mailbox ().take ({claim, 0});
    ASSERT_TRUE (accepted);
    auto header = protocol::decode_spot_message_header (
      protocol::message_bytes (accepted->parts ().front ()), protocol::command::spotRequest);
    ASSERT_TRUE (header.correlation);
    EXPECT_EQ (header.operation.high, pending.id.high);
    EXPECT_EQ (header.operation.low, pending.id.low);
    const protocol::request_source_fence_t source{lease->owner_id, 3,
                                                  requester->status ().routing_id ().to_bytes (),
                                                  requester->status ().lifecycle_generation ()};
    const protocol::relocation_coordinator_fence_t coordinator{
      "previous-owner", 4, previous->status ().routing_id ().to_bytes (),
      previous->status ().lifecycle_generation (), "authority-v1"};
    protocol::reply_relay_t relay{header.operation,
                                  *header.correlation,
                                  {2119, 5},
                                  1,
                                  coordinator,
                                  1,
                                  1,
                                  0,
                                  protocol::framework_error_code::none};
    const auto reply = protocol::application_payload_t::from_parts (parts);
    const auto relay_record = [&] (const protocol::reply_relay_t &value) {
        return mesh::service_mailbox_record_t::from_parts (
          {}, mesh::service_mailbox_domain_t::infrastructure,
          detail::backend::materialize_binding_parts (
            {protocol::encode_reply_relay (value), protocol::encode_application_payload (reply)}),
          target->status ().routing_id ().to_bytes (), std::nullopt,
          target->status ().lifecycle_generation ());
    };
    auto wrong = relay;
    ++wrong.operation.high;
    EXPECT_EQ (requester->relocation_wire ().process (relay_record (wrong)).result ().value (),
               stateful::raw_relocation_replay_result_t::not_registered);
    wrong = relay;
    ++wrong.operation.low;
    EXPECT_EQ (requester->relocation_wire ().process (relay_record (wrong)).result ().value (),
               stateful::raw_relocation_replay_result_t::not_registered);
    wrong = relay;
    ++wrong.reply_route_id;
    EXPECT_EQ (requester->relocation_wire ().process (relay_record (wrong)).result ().value (),
               stateful::raw_relocation_replay_result_t::not_registered);
    std::atomic_int acknowledgements{0};
    ASSERT_TRUE (target->relocation_wire ().register_terminal_target ({relay, source, reply,
                                                                       [&] (auto) {
                                                                           ++acknowledgements;
                                                                           return true;
                                                                       },
                                                                       [] { return true; }}));
    // Only A and C run: B cannot proxy the reply or acknowledge on A's behalf.
    const auto dispatch = [] (const auto &, const auto &, std::vector<zlink::message_t>, auto) {};
    const bool received = eventually ([&] {
        (void) target->dispatch_ready (dispatch).result ();
        (void) requester->dispatch_ready (dispatch).result ();
        return pending.completion->task ().await_ready () && acknowledgements == 1;
    });
    EXPECT_TRUE (received);
    if (received) {
        const auto result = pending.completion->task ().result ();
        ASSERT_TRUE (result);
        ASSERT_EQ (result.value ().parts.size (), 2u);
        EXPECT_EQ (result.value ().parts.back ().to_string (), "{}");
        EXPECT_EQ (target->relocation_wire ().pending_terminal_relays (), 0u);
        EXPECT_EQ (requester->relocation_wire ().process (relay_record (relay)).result ().value (),
                   stateful::raw_relocation_replay_result_t::terminal_duplicate);
        EXPECT_EQ (requester->transport ().pending_operation_count (), 0u);
        (void) target->dispatch_ready (dispatch).result ();
        EXPECT_EQ (acknowledgements, 1);
    }
    if (accepted->transferred)
        accepted->transferred ();
    requester->close ();
    previous->close ();
    target->close ();
}

TEST (FrameworkRelocationSealContract, native_protocol_rejection_preserves_following_claim_record)
{
    using namespace zlink::framework::runtime;
    auto host = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = test_descriptor ("native-protocol"),
        .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()}});
    host->configure_stateful_dispatch ([] (const auto &) {
        return task_t<std::optional<stateful::accepted_record_authority_t>> (
          result_t<std::optional<stateful::accepted_record_authority_t>>::success (
            stateful::accepted_record_authority_t{{"requester", 3, {2}, 1}, 3}));
    });
    const stateful::object_ref_t spot{
      stateful::object_kind_t::user_spot, "protocol", 1, 1, "mesh", "source"};
    const auto created = begin_reserved_object (host->objects (), spot, "room", {});
    ASSERT_EQ (created.error, stateful::stateful_error_t::none);
    ASSERT_EQ (host->objects ().commit_create (created.attempt), stateful::stateful_error_t::none);
    unsigned preparations = 0;
    host->configure_relocation_gate (
      {}, [&] (std::string, std::optional<protocol::spot_route_fence_t>) {
          ++preparations;
          return task_t<std::shared_ptr<serial_application_target_t>> (
            result_t<std::shared_ptr<serial_application_target_t>>::success (
              std::make_shared<serial_application_target_t> ()));
      });
    auto invalid = native_spot_record (*host, spot, 1, std::nullopt);
    invalid.body_parts ().back () = zlink::message_t::from ("invalid wire payload");
    std::vector<mesh::service_mailbox_record_t> originals;
    originals.push_back (std::move (invalid));
    originals.push_back (native_spot_record (*host, spot, 2, std::nullopt));
    const auto claim = zlink::framework::test::publish_stateful_claims (host->transport (), spot,
                                                                        std::move (originals));
    unsigned dispatched = 0;
    auto pending = host::public_host_runtime_test_access_t::dispatch_claim (
      *host, claim, [&] (const auto &, const auto &record, auto, auto) {
          ++dispatched;
          EXPECT_EQ (record.operation_id.low, 2u);
          ASSERT_TRUE (record.application_record);
          record.application_record->release_native_claim ();
      });
    EXPECT_EQ (pending.result ().value (), 1u);
    EXPECT_EQ (preparations, 1u);
    EXPECT_EQ (dispatched, 1u);
    EXPECT_EQ (claim->outstanding (), 0u);
    EXPECT_FALSE (host->transport ().mailbox ().release (*claim));
    host->close ();
}

TEST (FrameworkRelocationSealContract, native_pending_locator_retains_prepared_target_and_original)
{
    using namespace zlink::framework::runtime;
    for (const bool local : {false, true}) {
        SCOPED_TRACE (local);
        auto host = std::make_shared<host::public_host_runtime_t> (
          host::host_options_t{mesh::raw_mesh_node_options_t{
            .descriptor = test_descriptor ("pending-locate"),
            .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()}});
        host->configure_stateful_dispatch ([] (const auto &) {
            return task_t<std::optional<stateful::accepted_record_authority_t>> (
              result_t<std::optional<stateful::accepted_record_authority_t>>::success (
                stateful::accepted_record_authority_t{{"requester", 3, {2}, 1}, 3}));
        });
        const stateful::object_ref_t spot{
          stateful::object_kind_t::user_spot, std::string (197, 'p'), 1, 1, "mesh", "source"};
        const auto created = begin_reserved_object (host->objects (), spot, "room", {});
        ASSERT_EQ (created.error, stateful::stateful_error_t::none);
        ASSERT_EQ (host->objects ().commit_create (created.attempt),
                   stateful::stateful_error_t::none);
        auto expected = std::make_shared<serial_application_target_t> ();
        auto context = std::make_shared<int> (17);
        expected->context = context;
        task_completion_source_t<std::shared_ptr<serial_execution_queue_t>> release;
        unsigned preparations = 0, locations = 0, dispatched = 0;
        host->configure_relocation_gate (
          [&] (const auto &object) {
              ++locations;
              EXPECT_EQ (object.key, spot.key);
              EXPECT_EQ (object.object_generation, spot.object_generation);
              return release.task ();
          },
          [&] (std::string key, std::optional<protocol::spot_route_fence_t> route) {
              ++preparations;
              EXPECT_EQ (key, spot.key);
              EXPECT_TRUE (route);
              if (route)
                  EXPECT_EQ (route->spot_id, spot.key);
              return task_t<std::shared_ptr<serial_application_target_t>> (
                result_t<std::shared_ptr<serial_application_target_t>>::success (expected));
          });
        const auto slot = zlink::framework::test::publish_stateful_claim (
          host->transport (), spot,
          local ? local_native_spot_record (*host, spot, 91)
                : native_spot_record (*host, spot, 91, std::nullopt));
        // The initiating callback object and its stack have returned before locate completes.
        auto pending = [&] {
            mesh::service_mailbox_t::application_dispatch_t dispatch =
              [&] (const auto &owner, const auto &record, auto parts, auto) {
                  ++dispatched;
                  EXPECT_EQ (owner.spot_id, spot.key);
                  EXPECT_EQ (record.operation_id.low, 91u);
                  ASSERT_TRUE (record.spot_route);
                  EXPECT_EQ (record.spot_route->spot_id, spot.key);
                  EXPECT_EQ (record.spot_route->owner_lease_generation, 3u);
                  ASSERT_EQ (parts.size (), 2u);
                  EXPECT_EQ (parts.back ().to_bytes (), (std::vector<std::uint8_t>{'{', '}'}));
                  ASSERT_TRUE (record.application_record);
                  ASSERT_TRUE (record.application_record->target);
                  EXPECT_EQ (record.application_record->target, expected);
                  EXPECT_EQ (
                    std::get<std::shared_ptr<void>> (record.application_record->target->context),
                    context);
                  record.application_record->release_native_claim ();
              };
            return host::public_host_runtime_test_access_t::dispatch_claim (*host, slot.claim,
                                                                            std::move (dispatch));
        }();
        EXPECT_FALSE (pending.await_ready ());
        EXPECT_EQ (preparations, 1u);
        EXPECT_EQ (locations, 1u);
        EXPECT_EQ (dispatched, 0u);
        EXPECT_EQ (slot.claim->outstanding (), 1u);
        EXPECT_TRUE (host->transport ()
                       .mailbox ()
                       .prepare (slot,
                                 [] (const auto &record) {
                                     return record.application ? !record.application->parts.empty ()
                                                               : !record.parts ().empty ();
                                 })
                       .value_or (false));
        release.complete (result_t<std::shared_ptr<serial_execution_queue_t>>::success (nullptr));
        EXPECT_EQ (pending.result ().value (), 1u);
        EXPECT_EQ (dispatched, 1u);
        EXPECT_EQ (preparations, 1u);
        EXPECT_EQ (locations, 1u);
        EXPECT_EQ (slot.claim->outstanding (), 0u);
        EXPECT_FALSE (host->transport ().mailbox ().release (*slot.claim));
        host->close ();
    }
}
