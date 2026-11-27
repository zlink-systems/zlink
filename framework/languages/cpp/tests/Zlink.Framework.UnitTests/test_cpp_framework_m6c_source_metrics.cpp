/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/m6c_public_authority_fixture.hpp"
#include "../support/m6c_relocation_store_fixture.hpp"
#include "../support/m6c_runtime_cases.hpp"
#include "../support/m6c_stateful_fixture.hpp"
#include "../support/m6c_task.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "../support/runtime_metric_contract.hpp"
#include "../support/telemetry_observation.hpp"
#include "metric_test_reader.hpp"
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/diagnostics/flow_context.hpp"
#include "runtime/stateful/public_store_adapters.hpp"
#include "runtime/timers/async_delay.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace m6c_test
{

using namespace zlink::framework::runtime::stateful;

void test_source_relocation_metric_outcomes (test_context_t &test)
{
    namespace fw = zlink::framework;
    namespace protocol = fw::runtime::protocol;
    for (const std::string_view scenario :
         {"commit", "preserve", "missing", "shutdown", "cleanup-error"}) {
        metric_test::provider_t provider;
        auto monitoring = zlink::framework::test::runtime_failure_monitoring ();
        stateful_object_runtime_t objects;
        objects.configure_relocation_state (
          [] (const object_ref_t &, const std::string &,
              std::stop_token) -> fw::task_t<frozen_object_state_t> {
              frozen_object_state_t captured;
              captured.application_state = {0x41};
              co_return captured;
          },
          [] (const frozen_object_state_t &, const object_ref_t &,
              std::stop_token) -> zlink::framework::task_t<bool> { co_return true; });
        const auto actor = create_actor (objects, "metric-actor");
        public_memory_authority_store_t store;
        const auto store_now = std::chrono::system_clock::time_point{std::chrono::seconds{100}};
        store.snapshot = source_relocation_authority (actor, "outcome-contract", store_now);
        store.owner_lease_read = fw::owner_lease_found_t{
          {"source-owner", 1}, store_now + std::chrono::seconds{1}, store_now};
        auto authority = std::make_shared<public_authority_store_adapter_t> (store);
        auto roots = std::make_shared<memory_relocation_repository_t> ();
        fw::test::telemetry_observations_t<fw::message_flow_event_t> flow_events;
        fw::dispatch_options_t flow_dispatch;
        flow_dispatch.message_flow (fw::message_flow_log_mode_t::normal);
        fw::detail::dispatch_options_access_t::set_observer_for_tests (
          flow_dispatch, [&] (const auto &event) { flow_events.record (event); });
        std::mutex delay_mutex;
        fw::detail::delay_owner_t delays (delay_mutex);
        maintenance_runtime_t relocation (objects, authority, roots);
        relocation.bind_delay_owner (delays);
        if (scenario == "cleanup-error")
            relocation.attach_committed_terminal ([] (const object_ref_t &, const auto &) {
                throw std::runtime_error ("source cleanup failed");
            });
        eligible_relocation_unit_t::canonical_wire_context_t wire{
          .relocation = {0x2278, 1},
          .target_attempt_generation = 1,
          .coordinator = {"source-owner", 1, {'s'}, 1, "1"},
          .target_node_routing_id = {'t'},
          .target_node_generation = 1,
          .application_version = 1,
          .prepare_target =
            [&] (const std::vector<frozen_object_state_t> &,
                 const relocation_payload_manifest_t &) -> fw::task_t<relocation_reason_t> {
              const auto &aggregate = store.snapshot->aggregate;
              test.require (
                aggregate && aggregate->relocation && aggregate->relocation->phase == "captured"
                  && aggregate->relocation->expected_store_version == "1"
                  && store.snapshot->store_version != "1",
                "source must publish Captured against the original fence before PREPARE");
              co_return relocation_reason_t::none;
          },
          .send_state_chunk = [] (const protocol::relocation_state_t &) -> fw::task_t<bool> {
              co_return true;
          },
          .send_relocation_data = [] (const std::vector<protocol::relocation_data_t> &,
                                      const relocation_ingress_batch_t &) -> fw::task_t<bool> {
              co_return true;
          },
          .send_cutover = [&] (const protocol::relocation_cutover_t &) -> fw::task_t<bool> {
              if (scenario == "missing")
                  store.snapshot.reset ();
              else if (scenario == "shutdown")
                  delays.request_stop ();
              else if (scenario != "preserve") {
                  store.snapshot->store_version = "committed";
                  store.snapshot->owner = {"target-owner", 2};
                  ++store.snapshot->authority_owner_generation;
              }
              co_return true;
          },
          .restore_deadline = scenario == "preserve"
                                ? std::chrono::steady_clock::time_point{}
                                : std::chrono::steady_clock::time_point::max (),
          .abort_target_before_cutover = [] { return true; },
          .monitoring = monitoring,
          .dispatch = &flow_dispatch,
          .mesh_name = "outcome-contract",
          .policy = scenario == "cleanup-error"
                      ? fw::detail::factory_relocation_kind_t::preserve_state
                      : fw::detail::factory_relocation_kind_t::recreate};
        try {
            const std::string flow_id = "01930000-0000-7000-8000-000000000002";
            fw::runtime::flow_context_t::scope_t flow_scope (fw::runtime::flow_value_t{
              flow_id, fw::flow_origin_t::application, fw::message_flow_log_mode_t::normal});
            const auto result = await_task (relocation.relocate (
              actor, "target-owner", {"target-owner", 2}, 1024 * 1024, digest_with (0x41), wire));
            const auto observed =
              scenario == "preserve"   ? relocation_authority_t::source_preserved
              : scenario == "missing"  ? relocation_authority_t::source_lease_expired
              : scenario == "shutdown" ? relocation_authority_t::unsettled
                                       : relocation_authority_t::target_committed;
            test.require (result.settlement.outcome == observed,
                          "fixture must exercise the requested source settlement");
            if (scenario == "preserve")
                test.require (
                  objects.enqueue (actor, turn_domain_t::application, {1, {0x41}})
                    == stateful_error_t::none,
                  "Preserve must resume the retained source work before aborted is recorded");
            test.require (scenario != "cleanup-error",
                          "fixture must exercise the committed cleanup error");
        }
        catch (const std::runtime_error &) {
            test.require (scenario == "cleanup-error", "only the controlled cleanup may fail");
        }
        fw::test::acknowledge_delivery ();
        const auto observed_flows = flow_events.snapshot ();
        test.require (observed_flows.size () == 1,
                      "each source relocation must write one terminal message-flow record");
        if (observed_flows.size () == 1) {
            const auto &event = observed_flows.front ();
            const auto flow_outcome = scenario == "preserve"   ? "aborted"
                                      : scenario == "shutdown" ? "shutdown"
                                      : scenario == "missing"  ? "failed"
                                                               : "completed";
            test.require (
              event.outcome == fw::message_flow_outcome_t::completed
                && event.surface == fw::dispatch_error_surface_t::actor_relocation
                && event.message_kind == fw::dispatch_message_kind_t::control
                && event.actor_id == actor.key
                && event.flow_id == "01930000-0000-7000-8000-000000000002"
                && event.flow_origin == fw::flow_origin_t::application
                && event.detail_result == flow_outcome,
              "source terminal record must retain the actor, flow pair, and confirmed result");
        }
        const auto expected = scenario == "preserve"   ? "aborted"
                              : scenario == "shutdown" ? "shutdown"
                              : scenario == "missing"  ? "failed"
                                                       : "completed";
        int terminals = 0, durations = 0, starts = 0, sizes = 0;
        for (const auto &metric : provider.collect ()) {
            const auto &name = metric.instrument_descriptor.name_;
            for (const auto &point : metric.point_data_attr_) {
                if (name == "zlink.relocation.started" || name == "zlink.relocation.completed") {
                    const auto *sum = opentelemetry::nostd::get_if<metric_test::sdk::SumPointData> (
                      &point.point_data);
                    test.require (sum && opentelemetry::nostd::get<double> (sum->value_) == 1,
                                  "a source operation must increment each counter exactly once");
                } else if (name == "zlink.relocation.bytes"
                           || name == "zlink.relocation.duration") {
                    const auto *histogram =
                      opentelemetry::nostd::get_if<metric_test::sdk::HistogramPointData> (
                        &point.point_data);
                    test.require (histogram && histogram->count_ == 1,
                                  "a source operation must record each histogram exactly once");
                }
                test.require (
                  point.attributes.size ()
                    == (name == "zlink.relocation.started" || name == "zlink.relocation.bytes" ? 3
                                                                                               : 4),
                  "object metric labels must remain bounded by the contract");
                const auto policy = point.attributes.find ("policy");
                test.require (policy != point.attributes.end ()
                                && opentelemetry::nostd::get<std::string> (policy->second)
                                     == (scenario == "cleanup-error" ? "snapshot" : "recreate"),
                              "the source registration owns the metric policy");
                if (name == "zlink.relocation.started")
                    ++starts;
                if (name == "zlink.relocation.bytes")
                    ++sizes;
                if (name != "zlink.relocation.completed" && name != "zlink.relocation.duration")
                    continue;
                if (name == "zlink.relocation.completed")
                    ++terminals;
                else
                    ++durations;
                const auto outcome = point.attributes.find ("outcome");
                test.require (outcome != point.attributes.end ()
                                && opentelemetry::nostd::get<std::string> (outcome->second)
                                     == expected,
                              "source metric must use its confirmed authority outcome");
            }
        }
        test.require (terminals == 1 && durations == 1 && starts == 1 && sizes == 1,
                      "each source relocation must emit started, bytes and exactly one "
                      "completed/duration pair");
    }
}

void test_relocation_metric_starts_only_after_unit_admission (test_context_t &test)
{
    namespace fw = zlink::framework;
    for (const bool aggregate : {false, true}) {
        metric_test::provider_t provider;
        stateful_object_runtime_t objects;
        objects.configure_relocation_state (
          [] (const object_ref_t &, const std::string &,
              std::stop_token) -> fw::task_t<frozen_object_state_t> {
              frozen_object_state_t captured;
              captured.application_state = {0x41};
              co_return captured;
          },
          [] (const frozen_object_state_t &, const object_ref_t &,
              std::stop_token) -> zlink::framework::task_t<bool> { co_return true; });
        const auto actor = create_actor (objects, "metric-admission-a");
        const auto other = create_actor (objects, "metric-admission-b");
        auto authority = std::make_shared<memory_authority_store_t> ();
        maintenance_runtime_t relocation (
          objects, authority, std::make_shared<memory_relocation_repository_t> (),
          relocation_limits_t{.outbound_units = 1}, {},
          std::make_shared<memory_aggregate_authority_t> (authority));
        fw::task_completion_source_t<relocation_reason_t> prepared;
        eligible_relocation_unit_t::canonical_wire_context_t first_wire{
          .relocation = {0x2278, 2},
          .target_attempt_generation = 1,
          .coordinator = {"source-owner", 1, {'s'}, 1, "1"},
          .target_node_routing_id = {'t'},
          .target_node_generation = 1,
          .application_version = 1,
          .prepare_target =
            [&] (const std::vector<frozen_object_state_t> &,
                 const relocation_payload_manifest_t &) { return prepared.task (); },
          .send_state_chunk = [] (const fw::runtime::protocol::relocation_state_t &)
            -> fw::task_t<bool> { co_return true; }};
        auto occupied = relocation.relocate_to_ready (
          actor, "target", {"target-owner", 1}, 1024 * 1024, digest_with (1), first_wire, {}, 0);
        test.require (!occupied.await_ready (),
                      "fixture must hold an admitted preparation pending");
        eligible_relocation_unit_t::canonical_wire_context_t wire{
          .monitoring = zlink::framework::test::runtime_failure_monitoring (),
          .mesh_name = "admission-contract",
          .policy = fw::detail::factory_relocation_kind_t::recreate};
        const auto reason =
          aggregate ? await_task (relocation.relocate_aggregate ({actor, other}, "target",
                                                                 {"target-owner", 1}, 1024,
                                                                 digest_with (1), wire))
                        .reason
                    : await_task (relocation.relocate (actor, "target", {"target-owner", 1}, 1024,
                                                       digest_with (1), wire))
                        .reason;
        test.require (reason == relocation_reason_t::permit_unavailable,
                      "fixture must reject the unit before preparation starts");
        test.require (provider.collect ().empty (),
                      "a unit not admitted to preparation must emit no relocation metrics");
        prepared.complete (
          fw::result_t<relocation_reason_t>::success (relocation_reason_t::restore_failed));
        (void) await_task (std::move (occupied));
    }
}

void test_source_relocation_failure_stops_state_chunks_and_unseals (test_context_t &test)
{
    namespace framework = zlink::framework;
    namespace protocol = zlink::framework::runtime::protocol;

    metric_test::provider_t metric_provider;
    stateful_object_runtime_t objects;
    objects.configure_relocation_state (
      [] (const object_ref_t &, const std::string &, std::stop_token)
        -> zlink::framework::task_t<zlink::framework::runtime::stateful::frozen_object_state_t> {
          frozen_object_state_t captured;
          captured.application_state = std::vector<std::uint8_t> (256, 0x5a);
          co_return captured;
      },
      [] (const frozen_object_state_t &, const object_ref_t &,
          std::stop_token) -> zlink::framework::task_t<bool> { co_return true; });
    const auto actor = create_actor (objects, "live-failure-actor");
    auto roots = std::make_shared<memory_relocation_repository_t> ();
    auto authority = std::make_shared<memory_authority_store_t> ();
    std::mutex delay_mutex;
    zlink::framework::detail::delay_owner_t delays (delay_mutex);
    maintenance_runtime_t relocation (objects, authority, roots);
    relocation.bind_delay_owner (delays);
    relocation.configure_metrics (
      std::make_shared<framework::runtime::relocation_metrics_t> (metric_provider.api ()));

    auto target_terminal =
      std::make_shared<framework::task_completion_source_t<relocation_reason_t>> ();
    std::uint32_t planned_chunks = 0;
    std::uint32_t sent_chunks = 0;
    auto send_start = std::make_shared<framework::task_completion_source_t<void>> ();
    eligible_relocation_unit_t::canonical_wire_context_t wire{
      .relocation = {0x51, 0x52},
      .target_attempt_generation = 7,
      .coordinator = {"source-owner", 1, {'s', 'r', 'c'}, 3, "authority-v1"},
      .target_node_routing_id = {'d', 's', 't'},
      .target_node_generation = 7,
      .application_version = 1,
      .prepare_target = [&] (const std::vector<frozen_object_state_t> &,
                             const relocation_payload_manifest_t &manifest)
        -> framework::task_t<relocation_reason_t> {
          planned_chunks = manifest.chunk_count;
          co_return co_await target_terminal->task ();
      },
      .send_state_chunk = [&] (const protocol::relocation_state_t &) -> framework::task_t<bool> {
          ++sent_chunks;
          if (sent_chunks == 1) {
              co_await send_start->task ();
              // A wire reply completes in the registry's existing completion turn.
              auto delivered = std::make_shared<framework::task_completion_source_t<void>> ();
              auto delivery = delivered->task ();
              framework::detail::dispatch_completion ([target_terminal, delivered] {
                  target_terminal->complete (framework::result_t<relocation_reason_t>::success (
                    relocation_reason_t::checksum_mismatch));
                  delivered->complete (framework::result_t<void>::success ());
              });
              co_await delivery;
          }
          co_return true;
      },
      .send_relocation_data = [] (const std::vector<protocol::relocation_data_t> &,
                                  const relocation_ingress_batch_t &) -> framework::task_t<bool> {
          co_return true;
      },
      .send_cutover = [] (const protocol::relocation_cutover_t &) -> framework::task_t<bool> {
          co_return true;
      },
      .abort_target_before_cutover = [] { return true; }};

    wire.monitoring = zlink::framework::test::runtime_failure_monitoring ();
    wire.mesh_name = "source-metric-mesh";
    wire.policy = framework::detail::factory_relocation_kind_t::preserve_state;
    auto operation = relocation.relocate (actor, "target-node", {"target-owner", 9}, 1024 * 1024,
                                          digest_with (0x31), wire, {}, 16);
    std::vector<metric_test::sdk::MetricData> terminal_metrics;
    framework::task_completion_source_t<relocation_result_t> observed;
    framework::detail::observe_task_terminal (
      operation, [&] (const framework::result_t<relocation_result_t> &result) {
          terminal_metrics = metric_provider.collect ();
          observed.complete (result);
      });
    send_start->complete (framework::result_t<void>::success ());
    const auto result = await_task (observed.task ());
    const auto source_resumed = objects.enqueue (actor, turn_domain_t::application, {1, {0x41}});
    test.require (planned_chunks > 1 && sent_chunks == 1
                    && result.terminal == relocation_terminal_t::blocked
                    && result.reason == relocation_reason_t::checksum_mismatch
                    && source_resumed == stateful_error_t::none,
                  "an explicit target relocation failure must stop remaining state chunks, "
                  "preserve its reason, and unseal the source");
    const auto &data = terminal_metrics;
    metric_test::validate_metric_contract (data);
    const auto count = [&] (const std::string &name) {
        double value = 0;
        for (const auto &metric : data) {
            if (metric.instrument_descriptor.name_ != name)
                continue;
            for (const auto &point : metric.point_data_attr_) {
                if (const auto *sum = opentelemetry::nostd::get_if<metric_test::sdk::SumPointData> (
                      &point.point_data))
                    value += opentelemetry::nostd::get<double> (sum->value_);
                if (const auto *histogram =
                      opentelemetry::nostd::get_if<metric_test::sdk::HistogramPointData> (
                        &point.point_data))
                    value += histogram->count_;
            }
        }
        return value;
    };
    test.require (
      count ("zlink.relocation.started") == 1 && count ("zlink.relocation.completed") == 1
        && count ("zlink.relocation.duration") == 1 && count ("zlink.relocation.bytes") == 1,
      "one relocation terminal must record exactly one metric sample");
}

} // namespace m6c_test
