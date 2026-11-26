/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/m6c_relocation_store_fixture.hpp"
#include "../support/m6c_runtime_cases.hpp"
#include "../support/m6c_stateful_fixture.hpp"
#include "../support/m6c_task.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include "runtime/stateful/raw_stateful_dispatch.hpp"
#include "runtime/timers/async_delay.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace m6c_test
{

using namespace zlink::framework::runtime::stateful;

void test_cutover_replays_over_selected_network_route (test_context_t &test)
{
    using namespace std::chrono_literals;
    namespace framework = zlink::framework;
    namespace mesh = framework::runtime::mesh;
    namespace protocol = framework::runtime::protocol;
    const auto descriptor = [] (std::string rid) {
        return mesh::service_node_descriptor_t{
          "mesh", {rid.begin (), rid.end ()},           1, 1, "tcp://127.0.0.1:0",
          {},     mesh::service_node_state_t::preparing};
    };
    mesh::raw_mesh_node_owner_t source (framework::test::runtime_failure_options (
      mesh::raw_mesh_node_options_t{descriptor ("Z-network-source")}));
    mesh::raw_mesh_node_owner_t target (framework::test::runtime_failure_options (
      mesh::raw_mesh_node_options_t{descriptor ("A-network-target")}));
    source.start ();
    target.start ();
    const auto local = source.topology ().local_descriptor ();
    const auto remote = target.topology ().local_descriptor ();
    assert (source.connect_peer (target.endpoint (), remote));
    const auto ready = [&] {
        return source.peer_route (remote.node_routing_id).second
               && target.peer_route (local.node_routing_id).second;
    };
    const auto pump = [&] {
        const auto now = std::chrono::steady_clock::now ();
        (void) await_task (source.pump_one (now));
        (void) await_task (target.pump_one (now));
        (void) await_task (source.tick_liveness (now));
        (void) await_task (target.tick_liveness (now));
    };
    const auto admission_deadline = std::chrono::steady_clock::now () + 2s;
    while (!ready () && std::chrono::steady_clock::now () < admission_deadline)
        pump ();
    assert (ready ());
    const auto original_source_route = source.peer_route (remote.node_routing_id).first;
    const auto original_target_route = target.peer_route (local.node_routing_id).first;

    stateful_object_runtime_t objects;
    objects.configure_relocation_state (
      [] (const object_ref_t &, const std::string &, std::stop_token)
        -> zlink::framework::task_t<zlink::framework::runtime::stateful::frozen_object_state_t> {
          frozen_object_state_t captured;
          captured.application_state = std::vector<std::uint8_t>{0x41};
          co_return captured;
      },
      [] (const frozen_object_state_t &, const object_ref_t &,
          std::stop_token) -> zlink::framework::task_t<bool> { co_return true; });
    const auto actor = create_actor (objects, "network-actor", "Z-network-source");
    auto roots = std::make_shared<memory_relocation_repository_t> ();
    auto authority = std::make_shared<settlement_authority_t> ();
    authority->preserved_authority = relocation_authority_t::source_preserved;
    std::mutex delay_mutex;
    zlink::framework::detail::delay_owner_t delays (delay_mutex);
    maintenance_runtime_t relocation (objects, authority, roots);
    relocation.bind_delay_owner (delays);
    int data_sends = 0;
    int cutover_sends = 0;
    std::atomic<bool> stop{false};
    std::optional<std::thread> receiver;
    std::vector<std::vector<std::uint8_t>> received_batch;
    int verified_cutovers = 0;
    const auto deadline = std::chrono::steady_clock::now () + 2s;
    eligible_relocation_unit_t::canonical_wire_context_t wire{
      .relocation = {0x1761, 0x41},
      .target_attempt_generation = 7,
      .coordinator = {"source-owner", 1, local.node_routing_id, local.lifecycle_generation,
                      "authority-v1"},
      .target_node_routing_id = remote.node_routing_id,
      .target_node_generation = remote.lifecycle_generation,
      .application_version = 1,
      .prepare_target =
        [&] (const std::vector<frozen_object_state_t> &,
             const relocation_payload_manifest_t &) -> framework::task_t<relocation_reason_t> {
          protocol::frozen_application_record_t held;
          held.kind = protocol::frozen_record_kind_t::actor_send;
          held.source_kind = protocol::frozen_source_kind_t::node;
          held.source = {"source-owner", 1, local.node_routing_id, local.lifecycle_generation};
          held.operation = {0x1761, 1};
          held.body = protocol::frozen_actor_application_body_t{
            {actor.key, actor.object_generation, remote.node_routing_id,
             remote.lifecycle_generation, actor.authority_owner_generation, 9},
            {"NetworkBoundary", "application/json", {0x42}}};
          assert (objects.enqueue (actor, turn_domain_t::application, {1, {}, 1, std::move (held)})
                  == stateful_error_t::none);
          co_return relocation_reason_t::none;
      },
      .send_state_chunk = [] (const protocol::relocation_state_t &) -> framework::task_t<bool> {
          co_return true;
      },
      .send_relocation_data = [&] (const std::vector<protocol::relocation_data_t> &records,
                                   const relocation_ingress_batch_t &) -> framework::task_t<bool> {
          ++data_sends;
          assert (records.size () == 1);
          for (const auto &record : records)
              if (!co_await source.send_relocation_control (remote.node_routing_id, record))
                  co_return false;
          co_return true;
      },
      .send_application =
        [] (const object_ref_t &, const turn_record_t &, std::optional<std::uint64_t>) {
            assert (false && "network fixture submits no post-boundary application record");
        },
      .send_cutover =
        [&] (const protocol::relocation_cutover_t &cutover) -> framework::task_t<bool> {
          ++cutover_sends;
          const bool submitted =
            co_await source.send_relocation_control (remote.node_routing_id, cutover);
          assert (submitted);
          if (cutover_sends == 1) {
              // Leave the original batch at Core, then replace its selected pipe.
              assert (target.connect_peer (source.endpoint (), local));
              while ((source.peer_route (remote.node_routing_id).first == original_source_route
                      || target.peer_route (local.node_routing_id).first == original_target_route)
                     && std::chrono::steady_clock::now () < deadline) {
                  (void) source.observe_routes ();
                  (void) target.observe_routes ();
              }
              assert (source.peer_route (remote.node_routing_id).first != original_source_route);
              assert (target.peer_route (local.node_routing_id).first != original_target_route);
              while (!ready () && std::chrono::steady_clock::now () < deadline)
                  pump ();
              assert (ready ());
              assert (!target.mailbox ().try_claim (mesh::service_mailbox_domain_t::infrastructure,
                                                    1, 4096));
              // Settlement first observes the replacement already ready. A
              // connected boolean therefore never has a false-to-true edge.
              receiver.emplace ([&] {
                  while (!stop.load (std::memory_order_acquire)) {
                      pump ();
                      auto claim = target.mailbox ().try_claim (
                        mesh::service_mailbox_domain_t::infrastructure, 1, 4096);
                      if (!claim) {
                          std::this_thread::yield ();
                          continue;
                      }
                      const auto control = protocol::decode_relocation_control (
                        protocol::message_bytes (claim->records.front ()->parts ().front ()));
                      if (const auto *data = std::get_if<protocol::relocation_data_t> (&control)) {
                          received_batch.push_back (protocol::encode_relocation_control (*data));
                      } else {
                          const auto &boundary = std::get<protocol::relocation_cutover_t> (control);
                          relocation_crc32c_accumulator_t checksum;
                          for (const auto &record : received_batch)
                              checksum.update (record);
                          assert (boundary.boundary_record_count == received_batch.size ());
                          assert (boundary.boundary_checksum_crc32c == checksum.value ());
                          ++verified_cutovers;
                          std::lock_guard lock (authority->mutex);
                          authority->observed_authority = relocation_authority_t::target_committed;
                      }
                      assert (target.mailbox ().release (*claim));
                  }
              });
          }
          co_return submitted;
      },
      .restore_deadline = deadline,
      .target_route = [&] { return source.peer_route (remote.node_routing_id); },
      .abort_target_before_cutover = [] { return true; }};
    const auto result = await_task (relocation.relocate (
      actor, "A-network-target", {"target-owner", 9}, 1024 * 1024, digest_with (0x41), wire));
    stop.store (true, std::memory_order_release);
    if (receiver)
        receiver->join ();
    if (result.terminal != relocation_terminal_t::completed || data_sends != 2 || cutover_sends != 2
        || received_batch.size () != 1 || verified_cutovers != 1)
        std::cerr << "network cutover terminal=" << static_cast<int> (result.terminal)
                  << " reason=" << static_cast<int> (result.reason) << " batchSends=" << data_sends
                  << " cutoverSends=" << cutover_sends << " records=" << received_batch.size ()
                  << " verified=" << verified_cutovers << '\n';
    test.require (result.terminal == relocation_terminal_t::completed && data_sends == 2
                    && cutover_sends == 2 && received_batch.size () == 1 && verified_cutovers == 1
                    && std::chrono::steady_clock::now () < deadline,
                  "the original relocation terminal must follow a verified complete batch on the "
                  "new ready Core route");
    source.close ();
    target.close ();
}

} // namespace m6c_test
