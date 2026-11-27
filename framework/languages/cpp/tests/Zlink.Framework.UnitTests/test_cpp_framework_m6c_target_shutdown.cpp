/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/m6c_relocation_store_fixture.hpp"
#include "../support/m6c_runtime_cases.hpp"
#include "../support/m6c_task.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include "runtime/stateful/public_host_runtime.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace m6c_test
{

using namespace zlink::framework::runtime::stateful;

/* A relocation target driven by a raw source peer, so the test controls the
 * relay batch and the cutover (28 §4.4-4.5, 01 §10). */
void test_relocation_target_shutdown (test_context_t &test, int mode)
{
    const auto initial_failures = test.failures;
    using namespace std::chrono_literals;
    namespace detail = zlink::framework::detail;
    namespace framework = zlink::framework;
    namespace protocol = zlink::framework::runtime::protocol;
    namespace mesh = zlink::framework::runtime::mesh;
    const auto text_bytes = [] (std::string_view value) {
        return std::vector<std::uint8_t> (value.begin (), value.end ());
    };

    namespace host = framework::runtime::host;
    auto roots = std::make_shared<memory_relocation_repository_t> ();
    auto authority = std::make_shared<memory_authority_store_t> ();
    host::host_options_t options;
    options.mesh.runtime_failures =
      std::make_shared<framework::runtime::runtime_failure_collector_t> ();
    options.mesh.descriptor = {"production-relocation-mesh",
                               text_bytes ("settlement-target"),
                               1,
                               1,
                               "tcp://127.0.0.1:0",
                               {},
                               mesh::service_node_state_t::preparing};
    options.object_stable_types = {"production.actor"};
    auto target_owner = std::make_shared<host::public_host_runtime_t> (options);
    auto &target = *target_owner;
    target.objects ().configure_relocation_state (
      [] (const auto &, const auto &, std::stop_token)
        -> zlink::framework::task_t<zlink::framework::runtime::stateful::frozen_object_state_t> {
          frozen_object_state_t captured;
          captured.application_state = std::vector<std::uint8_t>{};
          co_return captured;
      },
      [] (const auto &, const auto &, std::stop_token) -> zlink::framework::task_t<bool> {
          co_return true;
      });
    target.configure_maintenance (
      {authority, std::make_shared<memory_aggregate_authority_t> (authority), roots});
    target.configure_stateful_dispatch (
      [] (accepted_record_authority_query_t query)
        -> zlink::framework::task_t<std::optional<accepted_record_authority_t>> {
          co_return std::
            nullopt; // The Store still names the source; the exact attempt owns admission.
      });
    target.configure_session_route_owner (
      [] { return std::optional<framework::location_owner_token_t>{{"target-owner", 9}}; });
    target.start ();
    mesh::raw_mesh_node_owner_t source (zlink::framework::test::runtime_failure_options (
      mesh::raw_mesh_node_options_t{{"production-relocation-mesh",
                                     text_bytes ("settlement-source"),
                                     1,
                                     1,
                                     "tcp://127.0.0.1:0",
                                     {},
                                     mesh::service_node_state_t::preparing}}));
    source.start ();
    const auto source_descriptor = source.topology ().local_descriptor ();
    const auto target_descriptor = target.transport ().topology ().local_descriptor ();
    (void) source.connect_peer (target.status ().local_endpoint (), target_descriptor);

    std::mutex delivered_mutex;
    std::vector<std::uint64_t> delivered;
    std::atomic<bool> stop{false};
    std::thread target_dispatch ([&] {
        while (!stop.load (std::memory_order_acquire)) {
            (void) target
              .dispatch_ready ([&] (const auto &, const auto &record, auto, auto) {
                  {
                      std::lock_guard lock (delivered_mutex);
                      delivered.push_back (record.operation_id.low);
                  }
                  if (record.complete_stateful_dispatch)
                      record.complete_stateful_dispatch ();
              })
              .result ();
            std::this_thread::yield ();
        }
    });
    const auto pump_until = [&] (const std::function<bool ()> &done,
                                 std::chrono::milliseconds limit) {
        const auto deadline = std::chrono::steady_clock::now () + limit;
        while (!done () && std::chrono::steady_clock::now () < deadline) {
            const auto now = mesh::service_liveness_registry_t::clock_t::now ();
            (void) await_task (source.pump_one (now));
            std::this_thread::yield ();
        }
        return done ();
    };
    const auto admitted = pump_until (
      [&] {
          return source.peer_route (target_descriptor.node_routing_id).second
                 && target.transport ().peer_route (source_descriptor.node_routing_id).second;
      },
      5s);
    test.require (admitted, "both relocation peers must be ready before PREPARE and STATE_CHUNK");

    const protocol::relocation_coordinator_fence_t coordinator{
      "source-owner", 1, source_descriptor.node_routing_id, source_descriptor.lifecycle_generation,
      "authority-v1"};
    /* Restores one Actor on the target and returns the attempt's wire
     * identity once the target replied relay-ready. */
    const auto prepare = [&] (std::string actor_id, protocol::relocation_id_t relocation) {
        const object_ref_t source_actor{
          object_kind_t::actor,
          actor_id,
          1,
          1,
          "production-relocation-mesh",
          zlink::routing_id_t::from ("settlement-source").to_string ()};
        {
            std::lock_guard lock (authority->mutex);
            authority->participant_identities = {
              relocation_participant_identity_t{source_actor, "production.actor", std::nullopt}};
        }
        const protocol::relocation_object_t object{protocol::relocation_object_kind_t::actor,
                                                   "production.actor", actor_id, 1, 1};
        const auto payload = maintenance_runtime_t::encode_envelope (
          {frozen_object_state_t{source_actor, "production.actor", {}, {}, {}}}, relocation, 1);
        const auto manifest = plan_relocation_payload (payload, 1024);
        const protocol::relocation_prepare_t request{
          relocation,
          target_descriptor.lifecycle_generation,
          coordinator,
          {target_descriptor.node_routing_id, target_descriptor.lifecycle_generation,
           "target-owner", 9},
          protocol::relocation_role_t::source,
          object,
          source_descriptor.node_routing_id,
          source_descriptor.lifecycle_generation,
          manifest.total_length,
          manifest.chunk_count,
          manifest.checksum_crc32c,
          1,
          static_cast<std::uint64_t> (std::numeric_limits<std::int64_t>::max ())};
        auto ready =
          source.request_relocation_prepare (target_descriptor.node_routing_id, request, 5s);
        for (std::uint32_t ordinal = 0; ordinal != manifest.chunk_count; ++ordinal) {
            (void) await_task (source.send_relocation_control (
              target_descriptor.node_routing_id,
              make_relocation_state_chunk (relocation, target_descriptor.lifecycle_generation,
                                           coordinator, object, payload, ordinal, 1024)));
        }
        (void) pump_until ([&] { return ready.await_ready (); }, 5s);
        const auto &response = ready.result ();
        return std::pair{object, response && response.value ().ready.has_value ()};
    };
    const auto record = [&] (const protocol::relocation_object_t &object,
                             protocol::relocation_id_t relocation, std::uint64_t operation) {
        protocol::frozen_application_record_t accepted;
        accepted.kind = protocol::frozen_record_kind_t::actor_send;
        accepted.source_kind = protocol::frozen_source_kind_t::node;
        accepted.source = {"source-owner", 1, source_descriptor.node_routing_id,
                           source_descriptor.lifecycle_generation};
        accepted.operation = {operation, operation};
        accepted.body = protocol::frozen_actor_application_body_t{
          {object.object_id, object.object_generation, target_descriptor.node_routing_id,
           target_descriptor.lifecycle_generation, object.expected_authority_owner_generation, 9},
          protocol::application_payload_t::from_parts ([] {
              protocol::application_payload_t::multipart_t parts;
              parts.push_back (zlink::message_t::from (std::string ("same bytes")));
              return parts;
          }())};
        return protocol::relocation_data_t{
          relocation,  target_descriptor.lifecycle_generation,
          coordinator, protocol::relocation_role_t::source,
          object,      protocol::encode_frozen_application_record (accepted)};
    };
    const auto cutover = [&] (const protocol::relocation_object_t &object,
                              protocol::relocation_id_t relocation,
                              const std::vector<protocol::relocation_data_t> &batch) {
        relocation_crc32c_accumulator_t checksum;
        for (const auto &data : batch)
            checksum.update (protocol::encode_relocation_control (data));
        protocol::relocation_cutover_t value{relocation, target_descriptor.lifecycle_generation,
                                             coordinator, protocol::relocation_role_t::source,
                                             object};
        value.boundary_record_count = batch.size ();
        value.boundary_checksum_crc32c = checksum.value ();
        (void) await_task (
          source.send_relocation_control (target_descriptor.node_routing_id, value));
    };
    const auto publishes = [&] {
        std::lock_guard lock (authority->mutex);
        return authority->publish_count;
    };
    const auto staged = [&] (const std::string &actor_id) {
        const auto objects = target.objects ().inventory_async ().result ().value ();
        return std::any_of (objects.begin (), objects.end (), [&] (const object_inventory_t &item) {
            return item.owner.key == actor_id;
        });
    };

    const auto [object, ready] = prepare ("shutdown-target", {0x71, 0x10});
    test.require (ready && staged ("shutdown-target"), "shutdown fixture must reach READY");
    if (mode != 0) {
        {
            std::lock_guard lock (authority->mutex);
            authority->throw_before_publish = mode == 1;
            authority->throw_after_publish = mode == 2;
            authority->fail_read_after_publish = mode == 2;
        }
        cutover (object, {0x71, 0x10}, {});
        test.require (pump_until ([&] { return publishes () != 0; }, 3s),
                      "verified cutover must enter CAS before seal");
    }
    auto shutdown = target.termination ()->terminate ();
    if (mode == 0) {
        test.require (pump_until ([&] { return shutdown.await_ready (); }, 2s),
                      "seal before cutover must finish shutdown within deadline");
        test.require (!staged ("shutdown-target"), "seal must discard READY staging");
        cutover (object, {0x71, 0x10}, {});
        (void) pump_until ([&] { return publishes () != 0; }, 100ms);
        test.require (publishes () == 0, "late cutover after seal must never start CAS");
    } else {
        test.require (!shutdown.await_ready (), "shutdown must wait for verified target terminal");
        test.require (staged ("shutdown-target"), "seal must retain verified staging");
        {
            std::lock_guard lock (authority->mutex);
            authority->throw_before_publish = false;
            authority->throw_after_publish = false;
            authority->throw_on_read = false;
            authority->fail_read_after_publish = false;
        }
        test.require (pump_until ([&] { return shutdown.await_ready (); }, 3s),
                      "CAS settlement must finish target shutdown");
        const auto active = target.objects ().inventory_async ().result ().value ();
        test.require (std::any_of (active.begin (), active.end (),
                                   [] (const auto &item) {
                                       return item.owner.key == "shutdown-target"
                                              && item.state == object_state_t::ready;
                                   }),
                      "verified target must publish after seal");
    }
    if (shutdown.await_ready ()) {
        const auto result = shutdown.result ();
        test.require (result && result.value ().outcome == termination_outcome_t::stopped,
                      "target terminal must finish as Stopped");
    }
    stop.store (true, std::memory_order_release);
    target_dispatch.join ();
    source.close ();
    target.close ();
    std::cout << "relocation-shutdown mode=" << mode
              << " failures=" << test.failures - initial_failures << '\n';
}

} // namespace m6c_test
