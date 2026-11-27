/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/m6c_public_authority_fixture.hpp"
#include "../support/m6c_relocation_store_fixture.hpp"
#include "../support/m6c_runtime_cases.hpp"
#include "../support/m6c_stateful_fixture.hpp"
#include "../support/m6c_task.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "../support/runtime_metric_contract.hpp"
#include "metric_test_reader.hpp"
#include "runtime/locations/actor_authority_payload.hpp"
#include "runtime/stateful/public_store_adapters.hpp"
#include "runtime/timers/async_delay.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace m6c_test
{

using namespace zlink::framework::runtime::stateful;

/* Scripted Location Store reading of one relocation unit (01 §6.1, §10). */
void test_source_settlement_preserve_and_members (test_context_t &test)
{
    namespace framework = zlink::framework;
    using outcome = relocation_authority_t;
    const auto now = std::chrono::system_clock::time_point{std::chrono::seconds{100}};
    const object_ref_t actor{object_kind_t::actor, "settlement-actor", 7, 11, "mesh", "a"};
    const relocation_authority_fence_t fence{actor.kind,          actor.key,           "1",
                                             {"source-owner", 1}, {"target-owner", 2}, {0x1878, 2}};
    public_memory_authority_store_t store;
    public_authority_store_adapter_t adapter (store);
    store.snapshot = framework::authority_snapshot_t{"2", {}, 7, 11, {"source-owner", 1}, now};
    store.owner_lease_read =
      framework::owner_lease_found_t{{"source-owner", 1}, now + std::chrono::seconds{1}, now};
    test.require (adapter.settle_source_relocation (fence, {actor}, false).outcome
                    == outcome::source_preserved,
                  "changed source StoreVersion requires a live lease before resuming");
    store.owner_lease_read = framework::owner_lease_missing_t{};
    test.require (adapter.settle_source_relocation (fence, {actor}, false).outcome
                    == outcome::source_lease_expired,
                  "changed source StoreVersion cannot reopen an expired source");
    store.fail_owner_lease_read = true;
    test.require (adapter.settle_source_relocation (fence, {actor}, false).outcome
                    == outcome::unsettled,
                  "indeterminate source lease must keep dispatch sealed");
    store.fail_owner_lease_read = false;
    store.owner_lease_read =
      framework::owner_lease_found_t{{"source-owner", 1}, now + std::chrono::seconds{1}, now};
    store.snapshot->store_version = "1";
    store.remaining_authority_conflicts = 1;
    store.conflict_snapshot =
      framework::authority_snapshot_t{"3", {}, 7, 29, {"next-owner", 3}, now};
    const auto conflict = adapter.settle_source_relocation (fence, {actor}, true);
    test.require (
      conflict.outcome == outcome::target_committed && conflict.follow_generations.empty (),
      "Preserve conflict must confirm a later owner without inventing first-target Follow");
    const object_ref_t spot{object_kind_t::user_spot, "settlement-spot", 9, 13, "mesh", "a"};
    const relocation_authority_fence_t whole{
      spot.kind, spot.key, "1", {"source-owner", 1}, {"target-owner", 2}, {0x1878, 3}};
    store.authority_rows.emplace (
      framework::runtime::spot_authority_key (spot.key).value,
      framework::authority_snapshot_t{"4", {}, 9, 31, {"target-owner", 2}, now});
    store.authority_rows.emplace (
      framework::runtime::actor_authority_key (actor.key).value,
      framework::authority_snapshot_t{"5", {}, 7, 37, {"next-owner", 3}, now});
    const auto aggregate = adapter.settle_source_relocation (whole, {actor, spot}, false);
    test.require (aggregate.outcome == outcome::target_committed
                    && aggregate.follow_generations.size () == 1
                    && aggregate.follow_generations.at ({spot.kind, spot.key}) == 31,
                  "whole-unit primary confirms commit while each member retains only its own "
                  "current first-target Follow");
}

void test_source_settlement_after_subsequent_moves (test_context_t &test)
{
    namespace framework = zlink::framework;
    namespace protocol = framework::runtime::protocol;
    for (const std::string_view scenario :
         {"target", "target-normalized", "chain", "chain-normalized", "return-target",
          "source-restarted", "target-restarted", "different-object", "same-generation",
          "lower-generation", "source-owner", "target-same-generation",
          "target-different-object"}) {
        const bool target_current =
          scenario == "target" || scenario == "target-normalized" || scenario == "return-target";
        const bool invalid = scenario == "different-object" || scenario == "same-generation"
                             || scenario == "lower-generation" || scenario == "source-owner"
                             || scenario == "target-same-generation"
                             || scenario == "target-different-object";
        metric_test::provider_t metric_provider;
        stateful_object_runtime_t objects;
        objects.configure_relocation_state (
          [] (const object_ref_t &, const std::string &, std::stop_token)
            -> zlink::framework::task_t<
              zlink::framework::runtime::stateful::frozen_object_state_t> {
              frozen_object_state_t captured;
              captured.application_state = std::vector<std::uint8_t>{0x41};
              co_return captured;
          },
          [] (const frozen_object_state_t &, const object_ref_t &,
              std::stop_token) -> zlink::framework::task_t<bool> { co_return true; });
        const auto actor = create_actor (objects, "chain-actor");
        public_memory_authority_store_t store;
        store.snapshot = source_relocation_authority (actor, "settlement-metric-mesh",
                                                      std::chrono::system_clock::now ());
        store.retarget_authority_generation_advance = 9;
        auto authority = std::make_shared<public_authority_store_adapter_t> (store);
        auto roots = std::make_shared<memory_relocation_repository_t> ();
        std::mutex delay_mutex;
        framework::detail::delay_owner_t delays (delay_mutex);
        maintenance_runtime_t relocation (objects, authority, roots);
        relocation.bind_delay_owner (delays);
        relocation.configure_metrics (
          std::make_shared<framework::runtime::relocation_metrics_t> (metric_provider.api ()));
        int committed = 0;
        int unavailable = 0;
        int cutover_calls = 0;
        std::vector<std::uint64_t> forwarded_generations;
        relocation.attach_committed_terminal (
          [&] (const object_ref_t &, const auto &) { ++committed; });
        relocation.attach_unavailable_terminal ([&] (const object_ref_t &) { ++unavailable; });
        eligible_relocation_unit_t::canonical_wire_context_t wire{
          .relocation = {0x1878, 1},
          .target_attempt_generation = 1,
          .coordinator = {"source-owner", 1, {'s'}, 1, "1"},
          .target_node_routing_id = {'b'},
          .target_node_generation = 1,
          .application_version = 1,
          .prepare_target = [] (const std::vector<frozen_object_state_t> &,
                                const relocation_payload_manifest_t &)
            -> framework::task_t<relocation_reason_t> { co_return relocation_reason_t::none; },
          .send_state_chunk = [] (const protocol::relocation_state_t &) -> framework::task_t<bool> {
              co_return true;
          },
          .send_relocation_data =
            [] (const std::vector<protocol::relocation_data_t> &,
                const relocation_ingress_batch_t &) -> framework::task_t<bool> { co_return true; },
          .send_application =
            [&] (const object_ref_t &, const turn_record_t &,
                 std::optional<std::uint64_t> generation) {
                if (generation)
                    forwarded_generations.push_back (*generation);
            },
          .send_cutover = [&] (const protocol::relocation_cutover_t &) -> framework::task_t<bool> {
              ++cutover_calls;
              // Complete A->B->C before the source's first authority read.
              // Provider CAS terminals, rather than elapsed time, establish the order.
              std::vector<framework::location_owner_token_t> owners{{"target-owner", 2}};
              if (!target_current || scenario == "return-target")
                  owners.push_back (scenario == "source-restarted"
                                      ? framework::location_owner_token_t{"source-owner", 4}
                                    : scenario == "target-restarted"
                                      ? framework::location_owner_token_t{"target-owner", 4}
                                      : framework::location_owner_token_t{"next-owner", 3});
              if (scenario == "return-target")
                  owners.push_back ({"target-owner", 2});
              for (const auto &owner : owners) {
                  const auto exchanged = await_task (store.compare_exchange_authority (
                    framework::runtime::actor_authority_key (actor.key),
                    store.snapshot->store_version,
                    framework::authority_retarget_t{
                      {}, {"mesh", framework::node_rid_t::from_string (owner.owner_id), 1, owner}},
                    {}));
                  test.require (std::holds_alternative<framework::authority_stored_t> (exchanged),
                                "chain fixture must commit each owner transition");
              }
              if (scenario == "target-normalized" || scenario == "chain-normalized") {
                  const auto normalized = await_task (store.compare_exchange_authority (
                    framework::runtime::actor_authority_key (actor.key),
                    store.snapshot->store_version, framework::authority_put_t{{}}, {}));
                  test.require (std::holds_alternative<framework::authority_stored_t> (normalized),
                                "chain fixture must normalize authority before source read");
              }
              if (scenario == "different-object")
                  ++store.snapshot->object_generation;
              if (scenario == "same-generation")
                  store.snapshot->authority_owner_generation = actor.authority_owner_generation;
              if (scenario == "lower-generation")
                  store.snapshot->authority_owner_generation = 0;
              if (scenario == "source-owner")
                  store.snapshot->owner = {"source-owner", 1};
              if (scenario == "target-same-generation" || scenario == "target-different-object") {
                  store.snapshot->owner = {"target-owner", 2};
                  if (scenario == "target-same-generation")
                      store.snapshot->authority_owner_generation = actor.authority_owner_generation;
                  else
                      ++store.snapshot->object_generation;
              }
              co_return true;
          },
          .restore_deadline = std::chrono::steady_clock::time_point::max (),
          .abort_target_before_cutover = [] { return true; }};
        wire.monitoring = zlink::framework::test::runtime_failure_monitoring ();
        wire.mesh_name = "settlement-metric-mesh";
        wire.policy = framework::detail::factory_relocation_kind_t::preserve_state;
        const auto result = await_task (relocation.relocate (
          actor, "target-owner", {"target-owner", 2}, 1024 * 1024, digest_with (0x41), wire));
        std::cerr << "source-settlement " << scenario
                  << " terminal=" << static_cast<int> (result.terminal)
                  << " committed=" << committed << " unavailable=" << unavailable
                  << " cutovers=" << cutover_calls
                  << " observed-owner=" << store.snapshot->owner.owner_id << '/'
                  << store.snapshot->owner.lease_generation
                  << " object-generation=" << store.snapshot->object_generation
                  << " source-authority=" << actor.authority_owner_generation
                  << " observed-authority=" << store.snapshot->authority_owner_generation << '\n';
        test.require (cutover_calls == 1 && store.snapshot->store_version != "1",
                      "source settlement fixture must reach the completed target CAS chain");
        test.require (
          result.terminal
            == (invalid ? relocation_terminal_t::data_lost : relocation_terminal_t::completed),
          "source settlement must confirm a chain and reject an invalid tenure");
        test.require (committed == (invalid ? 0 : 1) && unavailable == (invalid ? 1 : 0),
                      "source settlement must apply exactly one committed or failed cleanup");
        const auto metric_data = metric_provider.collect ();
        metric_test::validate_metric_contract (metric_data);
        bool terminal_recorded = false;
        bool interruption_recorded = false;
        for (const auto &metric : metric_data) {
            for (const auto &point : metric.point_data_attr_) {
                if (metric.instrument_descriptor.name_ == "zlink.relocation.completed") {
                    const auto &sum =
                      opentelemetry::nostd::get<metric_test::sdk::SumPointData> (point.point_data);
                    terminal_recorded =
                      opentelemetry::nostd::get<double> (sum.value_) == 1
                      && opentelemetry::nostd::get<std::string> (point.attributes.at ("outcome"))
                           == (invalid ? "failed" : "completed");
                }
                if (metric.instrument_descriptor.name_ == "zlink.relocation.interruption") {
                    const auto &histogram =
                      opentelemetry::nostd::get<metric_test::sdk::HistogramPointData> (
                        point.point_data);
                    interruption_recorded =
                      histogram.count_ == 1
                      && opentelemetry::nostd::get<double> (histogram.sum_) >= 0;
                }
            }
        }
        test.require (terminal_recorded && interruption_recorded,
                      "settlement and cutover must record their own single terminal metric");
        (void) objects.enqueue (actor, turn_domain_t::application, {1, {0x42}});
        const auto claim = objects.try_claim (actor, turn_domain_t::application);
        test.require (claim.first != stateful_error_t::none || !claim.second,
                      "a subsequent owner move must never reopen source dispatch");
        if (claim.second)
            (void) objects.complete_claim (actor, turn_domain_t::application);
        if (!invalid) {
            test.require (forwarded_generations.size () == (target_current ? 1u : 0u),
                          "only the first target's current owner tenure may receive Follow");
            if (target_current && !forwarded_generations.empty ())
                test.require (forwarded_generations.front ()
                                == store.snapshot->authority_owner_generation,
                              "Follow must use the observed current target generation");
        }
        delays.stop_and_join ();
    }
}

} // namespace m6c_test
