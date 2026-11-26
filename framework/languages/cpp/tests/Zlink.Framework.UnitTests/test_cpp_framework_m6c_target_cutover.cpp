/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/m6c_relocation_store_fixture.hpp"
#include "../support/m6c_runtime_cases.hpp"
#include "../support/m6c_task.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/mesh/raw_mesh_node_owner.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace m6c_test
{

using namespace zlink::framework::runtime::stateful;

void test_relocation_target_cutover_and_authority_settlement (test_context_t &test)
{
    using namespace std::chrono_literals;
    namespace detail = zlink::framework::detail;
    namespace framework = zlink::framework;
    namespace protocol = zlink::framework::runtime::protocol;
    namespace mesh = zlink::framework::runtime::mesh;
    const auto text_bytes = [] (std::string_view value) {
        return std::vector<std::uint8_t> (value.begin (), value.end ());
    };

    const auto core_context = std::make_shared<zlink::context_t> ();
    auto state = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::mesh_node_builder_state_t> ("production-relocation-mesh");
    state->core_context = core_context;
    state->listen_endpoint = "tcp://127.0.0.1:0";
    state->routing_id = zlink::routing_id_t::from ("settlement-target");
    state->spot_state->dispatch.message_flow (framework::message_flow_log_mode_t::detailed);
    framework::logging_builder_t logging;
    logging.use_file ((std::filesystem::temp_directory_path () / "target-cutover.flow").string ())
      .set_min_level (framework::log_level_t::debug);
    detail::dispatch_options_access_t::set_logger (state->spot_state->dispatch,
                                                   logging.create_logger ("target-cutover"));
    state->spot_state->snapshot.actor_types.push_back ("production.actor");
    auto roots = std::make_shared<memory_relocation_repository_t> ();
    auto authority = std::make_shared<memory_authority_store_t> ();
    detail::mesh_node_runtime_t target (state);
    target.configure_relocation_runtime (authority, roots);
    target.configure_stateful_dispatch (
      [] (accepted_record_authority_query_t query)
        -> zlink::framework::task_t<std::optional<accepted_record_authority_t>> {
          co_return std::
            nullopt; // The Store still names the source; the exact attempt owns admission.
      });
    target.configure_session_route_owner (
      [] { return std::optional<framework::location_owner_token_t>{{"target-owner", 9}}; });
    target.start ().result ().value ();
    mesh::raw_mesh_node_owner_t source (mesh::raw_mesh_node_options_t{
      .descriptor = {"production-relocation-mesh",
                     text_bytes ("settlement-source"),
                     1,
                     1,
                     "tcp://127.0.0.1:0",
                     {},
                     mesh::service_node_state_t::preparing},
      .runtime_failures =
        std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()});
    source.start ();
    const auto source_descriptor = source.topology ().local_descriptor ();
    const auto target_descriptor =
      target.native_node ().transport ().topology ().local_descriptor ();
    (void) source.connect_peer (target.status ().local_endpoint (), target_descriptor);

    std::mutex delivered_mutex;
    std::vector<std::uint64_t> delivered;
    std::atomic<bool> stop{false};
    std::thread target_dispatch ([&] {
        while (!stop.load (std::memory_order_acquire)) {
            // Await so the suspended coroutine cannot outlive the callback and its captures.
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
                 && target.native_node ()
                      .transport ()
                      .peer_route (source_descriptor.node_routing_id)
                      .second;
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
        const auto objects = target.native_node ().objects ().inventory_async ().result ().value ();
        return std::any_of (objects.begin (), objects.end (), [&] (const object_inventory_t &item) {
            return item.owner.key == actor_id;
        });
    };

    // FW19: without a verified cutover the wait is a Warning only.
    const auto [silent, silent_ready] = prepare ("cutover-silent", {0x71, 0x01});
    (void) pump_until ([] { return false; }, 1500ms);
    test.require (silent_ready && publishes () == 0 && staged ("cutover-silent"),
                  "the cutover wait must never start the target CAS");

    // FW20: two accepted records with identical bytes are two records, and a
    // retransmitted whole batch replaces the partial copy before cutover.
    const auto [twin, twin_ready] = prepare ("cutover-twins", {0x71, 0x02});
    const auto target_actor = [&] () -> std::optional<object_ref_t> {
        const auto inventory =
          target.native_node ().objects ().inventory_async ().result ().value ();
        for (const auto &item : inventory)
            if (item.owner.key == "cutover-twins")
                return item.owner;
        return std::nullopt;
    }();
    test.require (target_actor.has_value (), "prepared target must expose its staged identity");
    if (target_actor) {
        protocol::application_payload_t::multipart_t parts;
        parts.push_back (zlink::message_t::from (std::string ("late")));
        (void) await_task (source.request_to_actor (
          target_descriptor.node_routing_id, std::nullopt,
          {target_actor->key, target_actor->object_generation, target_descriptor.node_routing_id,
           target_descriptor.lifecycle_generation, target_actor->authority_owner_generation, 9},
          protocol::application_payload_t::from_parts (std::move (parts)), 5s, [] (auto, auto) {},
          protocol::wire_operation_id_t{0x1087, 77}));
        test.require (pump_until (
                        [&] {
                            return target.native_node ().objects ().pending (
                                     *target_actor, turn_domain_t::application)
                                   == 1;
                        },
                        3s),
                      "target attempt must accept late application before the Store owner changes");
        std::lock_guard lock (delivered_mutex);
        test.require (delivered.empty (),
                      "late application cannot execute before verified cutover");
    }
    const auto before_twins = publishes ();
    const auto first = record (twin, {0x71, 0x02}, 41);
    const auto second = record (twin, {0x71, 0x02}, 41);
    (void) await_task (source.send_relocation_control (target_descriptor.node_routing_id, first));
    for (const auto &data : {first, second})
        (void) await_task (
          source.send_relocation_control (target_descriptor.node_routing_id, data));
    cutover (twin, {0x71, 0x02}, {first, second});
    const auto twin_committed = pump_until ([&] { return publishes () > before_twins; }, 3s);
    test.require (twin_ready && twin_committed,
                  "a verified cutover over identical accepted records must start the target CAS");

    test.require (pump_until (
                    [&] {
                        std::lock_guard lock (delivered_mutex);
                        return delivered.size () == 3;
                    },
                    3s),
                  "verified boundary and late application must all execute");
    {
        std::lock_guard lock (delivered_mutex);
        test.require (delivered == std::vector<std::uint64_t>{41, 41, 77},
                      "batch retransmission must execute once, followed by late ingress");
    }

    // FW22: an indeterminate CAS is resubmitted with no count or timer while
    // the Store names the source; a confirmed source Preserve ends staging.
    {
        std::lock_guard lock (authority->mutex);
        authority->throw_before_publish = true;
    }
    const auto [fenced, fenced_ready] = prepare ("cutover-fenced", {0x71, 0x03});
    const auto base = publishes ();
    cutover (fenced, {0x71, 0x03}, {});
    const auto resubmitted = pump_until ([&] { return publishes () >= base + 3; }, 3s);
    const auto retained = staged ("cutover-fenced");
    {
        std::lock_guard lock (authority->mutex);
        authority->source_preserved = true;
    }
    const auto discarded = pump_until ([&] { return !staged ("cutover-fenced"); }, 3s);
    test.require (fenced_ready && resubmitted && retained && discarded,
                  "the target resubmits an indeterminate CAS and discards staging only on a "
                  "confirmed source Preserve fence");

    stop.store (true, std::memory_order_release);
    target_dispatch.join ();
    source.close ();
    target.stop ();
}

} // namespace m6c_test
