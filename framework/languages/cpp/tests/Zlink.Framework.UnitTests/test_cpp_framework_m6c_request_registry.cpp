/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/m6c_relocation_store_fixture.hpp"
#include "../support/m6c_runtime_cases.hpp"
#include "../support/m6c_stateful_fixture.hpp"
#include "../support/m6c_task.hpp"
#include "../support/request_admission_fixture.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "metric_test_reader.hpp"
#include "runtime/timers/async_delay.hpp"

#include <iostream>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <variant>
#include <vector>

namespace m6c_test
{

using namespace zlink::framework::runtime::stateful;

// Ready and cutover callbacks establish the boundary; no clock advances the test.
void test_relocation_request_source_registration (test_context_t &test,
                                                  bool boundary,
                                                  bool aggregate)
{
    namespace framework = zlink::framework;
    namespace protocol = framework::runtime::protocol;
    metric_test::provider_t provider;
    auto monitoring = zlink::framework::test::runtime_failure_monitoring ();
    stateful_object_runtime_t objects;
    objects.configure_relocation_state (
      [] (const auto &, const auto &, std::stop_token)
        -> zlink::framework::task_t<zlink::framework::runtime::stateful::frozen_object_state_t> {
          co_return frozen_object_state_t{};
      },
      [] (const auto &, const auto &, std::stop_token) -> zlink::framework::task_t<bool> {
          co_return true;
      });
    auto actor = create_actor (objects, "registered-request");
    std::vector<object_ref_t> participants;
    if (aggregate) {
        auto peer = create_actor (objects, "a-peer");
        const auto spot = create_spot (objects, object_kind_t::user_spot, "registry-spot");
        for (auto *member : {&actor, &peer}) {
            const auto move =
              objects.begin_membership_move_async (*member, spot).result ().value ();
            test.require (move.first == stateful_error_t::none, "aggregate membership must begin");
            const auto joined =
              objects.commit_membership_move_async (move.second).result ().value ();
            test.require (joined.first == stateful_error_t::none,
                          "aggregate membership must commit");
            *member = joined.second;
        }
        participants = {spot, actor, peer};
    }
    auto authority = std::make_shared<settlement_authority_t> ();
    authority->observed_authority = relocation_authority_t::source_preserved;
    authority->preserved_authority = relocation_authority_t::source_preserved;
    auto roots = std::make_shared<memory_relocation_repository_t> ();
    maintenance_runtime_t relocation (objects, authority, roots, {}, {},
                                      std::make_shared<memory_aggregate_authority_t> (
                                        std::make_shared<memory_authority_store_t> ()));
    protocol::frozen_application_record_t request;
    request.kind = protocol::frozen_record_kind_t::actor_request;
    request.source_kind = protocol::frozen_source_kind_t::node;
    request.source = {"source-owner", 1, {'s'}, 1};
    request.operation = boundary ? protocol::wire_operation_id_t{0x1786, 0}
                                 : protocol::wire_operation_id_t{0x1786, 0x1771};
    request.operation_kind = 4;
    request.reply_route_id = 77;
    request.body = protocol::frozen_actor_application_body_t{
      {actor.key, actor.object_generation, {'s'}, 1, actor.authority_owner_generation, 1},
      {"Request", "application/json", {0x41}}};
    const auto accept = [&] {
        test.require (objects.enqueue (actor, turn_domain_t::application, {1, {}, 0, request})
                        == stateful_error_t::none,
                      "request must enter the source's accepted queue");
    };
    if (!boundary && !aggregate)
        accept ();
    std::vector<protocol::wire_operation_id_t> accepted;
    eligible_relocation_unit_t::canonical_wire_context_t wire{
      .relocation = {0x1786, 2},
      .target_attempt_generation = 1,
      .coordinator = {"source-owner", 1, {'s'}, 1, "v1"},
      .target_node_routing_id = {'t'},
      .target_node_generation = 1,
      .application_version = 1,
      .augment_frozen =
        [&] (std::vector<frozen_object_state_t> &frozen) {
            if (aggregate)
                for (auto &participant : frozen)
                    if (participant.owner == actor)
                        participant.pending_application.push_back ({1, {}, 0, request});
            return true;
        },
      .prepare_target = [&] (const std::vector<frozen_object_state_t> &frozen,
                             const auto &) -> framework::task_t<relocation_reason_t> {
          if (boundary)
              accept ();
          for (const auto &participant : frozen)
              for (const auto &turn : participant.pending_application) {
                  if (turn.frozen_record)
                      accepted.push_back (turn.frozen_record->operation);
                  else if (turn.application_record)
                      accepted.push_back (turn.application_record->operation);
              }
          co_return relocation_reason_t::none;
      },
      .send_state_chunk = [] (const auto &) -> framework::task_t<bool> { co_return true; },
      .send_relocation_data = [&] (const std::vector<protocol::relocation_data_t> &records,
                                   const auto &) -> framework::task_t<bool> {
          for (const auto &data : records)
              accepted.push_back (data.record.operation);
          co_return true;
      },
      .send_cutover = [&] (const protocol::relocation_cutover_t &) -> framework::task_t<bool> {
          test.require (accepted == std::vector<protocol::wire_operation_id_t>{request.operation},
                        "one original request must reach the target before cutover");
          co_return true;
      },
      .abort_target_before_cutover = [] { return true; },
      .monitoring = monitoring,
      .mesh_name = "request-registry",
      .policy = framework::detail::factory_relocation_kind_t::recreate};
    const auto terminal =
      aggregate
        ? await_task (relocation.relocate_aggregate (participants, "target", {"target-owner", 2},
                                                     1024 * 1024, digest_with (0x41), wire))
            .terminal
        : await_task (relocation.relocate (actor, "target", {"target-owner", 2}, 1024 * 1024,
                                           digest_with (0x41), wire))
            .terminal;
    test.require (terminal == relocation_terminal_t::blocked,
                  "source authority must restore after Ready");
    test.require (objects.enqueue (actor, turn_domain_t::application, {2, {0x42}})
                    == stateful_error_t::none,
                  "source Preserve must resume the original application queue");
    int terminals = 0;
    for (const auto &metric : provider.collect ()) {
        if (metric.instrument_descriptor.name_ != "zlink.relocation.completed")
            continue;
        for (const auto &point : metric.point_data_attr_) {
            ++terminals;
            const auto *sum =
              opentelemetry::nostd::get_if<metric_test::sdk::SumPointData> (&point.point_data);
            test.require (sum && opentelemetry::nostd::get<double> (sum->value_) == 1,
                          "source Preserve must record its terminal exactly once");
            const auto outcome = point.attributes.find ("outcome");
            test.require (outcome != point.attributes.end ()
                            && opentelemetry::nostd::get<std::string> (outcome->second)
                                 == "aborted",
                          "resumed source authority must retain the aborted metric outcome");
        }
    }
    test.require (terminals == 1, "source Preserve must emit one completed metric");
}

} // namespace m6c_test
