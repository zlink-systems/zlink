/* SPDX-License-Identifier: Elastic-2.0 */

#include "m6a_runtime_tests.hpp"
#include "../support/m6a_mesh_fixture.hpp"
#include "../support/m6a_relocation_prepare_request.hpp"

#include "mailbox_claim_fixture.hpp"

#include "../support/runtime_failure_fixture.hpp"


#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include <limits>
#include <zlink/framework/contracts/dispatch/request_task.hpp>
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/backend/raw_binding_adapter.hpp"
#include "runtime/mesh/user_spot_terminal_mapping.hpp"
#include "runtime/locations/service_descriptor_registry.hpp"
#include "runtime/fanout/raw_fanout_owner.hpp"
#include "runtime/client_server/raw_client_server_owner.hpp"
#include "runtime/messaging/envelope_codec.hpp"
#include "runtime/locations/authority_key_codec.hpp"
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/live_location_reader.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include <zlink/Contracts/Core/context.hpp>
#include <zlink/Contracts/Core/routing_id.hpp>
#include <zlink/Contracts/Sockets/message_socket_contracts.hpp>
#include <zlink/Contracts/Sockets/routed_socket_contracts.hpp>
#include <zlink/socket/api.h>
#include "runtime/client_server/weighted_selector.hpp"
#include "runtime/protocol/service_wire_codec.hpp"

#include <service_wire_pilot_codec.hpp>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <future>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <map>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace zlink::framework::test::m6a
{
namespace mesh = zlink::framework::runtime::mesh;
namespace locations = zlink::framework::runtime::locations;
namespace fanout = zlink::framework::runtime::fanout;
namespace client_server = zlink::framework::runtime::client_server;
namespace foundation = zlink::framework::runtime::foundation;
namespace protocol = zlink::framework::runtime::protocol;
namespace runtime = zlink::framework::runtime;
using namespace std::chrono_literals;

// Spec 15/28 + node's own cross-language audit: an explicit, identity-
// matched relocationFailed(53) reply must resolve request_relocation_prepare
// promptly with its own outcome, not be silently dropped into the same "no
// result" a genuine timeout produces. Before this fix, request_relocation_
// prepare only ever extracted relocation_ready_t from the decoded reply —
// a relocation_failed_t reply fell through untouched, discarding both the
// fast explicit rejection and its wire failure_code.
void verify_relocation_prepare_failed_reply_resolves_promptly_with_identity_fencing ()
{
    mesh::raw_mesh_node_owner_t source (mesh::raw_mesh_node_options_t{
      .descriptor = descriptor ("relocation-fence-source"),
      .runtime_failures =
        std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()});
    mesh::raw_mesh_node_owner_t target (mesh::raw_mesh_node_options_t{
      .descriptor = descriptor ("relocation-fence-target"),
      .runtime_failures =
        std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()});
    source.start ();
    target.start ();
    const auto source_descriptor = source.topology ().local_descriptor ();
    const auto target_descriptor = target.topology ().local_descriptor ();
    admit_pair (source, target, target_descriptor);

    const auto prepare = relocation_prepare_request (source_descriptor, target_descriptor);

    mesh::relocation_prepare_response_t response;
    std::atomic_bool source_settled{false};
    const auto started = std::chrono::steady_clock::now ();
    std::thread prepare_thread ([&] {
        response = await_task (
          source.request_relocation_prepare (target_descriptor.node_routing_id, prepare, 5s));
        source_settled.store (true, std::memory_order_release);
    });

    std::shared_ptr<mesh::service_mailbox_claim_t> claim;
    const auto receive_deadline = std::chrono::steady_clock::now () + 2s;
    while (!claim && std::chrono::steady_clock::now () < receive_deadline) {
        claim =
          target.mailbox ().try_claim (mesh::service_mailbox_domain_t::infrastructure, 1, 4096);
        if (!claim) {
            (void) await_task (
              target.pump_one (mesh::service_liveness_registry_t::clock_t::now ()));
            (void) await_task (
              source.pump_one (mesh::service_liveness_registry_t::clock_t::now ()));
        }
    }
    assert (claim && claim->records.size () == 1);
    const auto &record = (*claim->records.front ());
    const auto control =
      protocol::decode_relocation_control (protocol::message_bytes (record.parts ().front ()));
    const auto *decoded_prepare = std::get_if<protocol::relocation_prepare_t> (&control);
    assert (decoded_prepare && *decoded_prepare == prepare);

    assert (target.reply_relocation_failed (
      record, protocol::relocation_failed_t{
                prepare.relocation, prepare.target_attempt_generation, prepare.coordinator,
                prepare.target, prepare.object, protocol::relocation_role_t::target,
                static_cast<std::uint32_t> (protocol::framework_error_code::relocationDataLost)}));
    assert (target.mailbox ().release (*claim));

    const auto settle_deadline = std::chrono::steady_clock::now () + 2s;
    while (!source_settled.load (std::memory_order_acquire)
           && std::chrono::steady_clock::now () < settle_deadline) {
        (void) await_task (source.pump_one (mesh::service_liveness_registry_t::clock_t::now ()));
        std::this_thread::sleep_for (1ms);
    }
    prepare_thread.join ();
    const auto elapsed = std::chrono::steady_clock::now () - started;

    assert (source_settled.load (std::memory_order_acquire));
    assert (!response.ready);
    assert (response.failed);
    assert (response.failed->relocation == prepare.relocation);
    assert (response.failed->target_attempt_generation == prepare.target_attempt_generation);
    assert (response.failed->coordinator == prepare.coordinator);
    assert (response.failed->target == prepare.target);
    assert (response.failed->object == prepare.object);
    assert (response.failed->failure_code
            == static_cast<std::uint32_t> (protocol::framework_error_code::relocationDataLost));
    // Resolved by the explicit reply well inside the 5s request timeout —
    // proves this is its own prompt outcome, not the same "no result" a
    // timeout would also produce.
    assert (elapsed < 2s);

    source.close ();
    target.close ();
}

// The same exact-identity fencing that already protects relocation_ready_t
// must also protect relocation_failed_t: a reply whose identity does not
// match the sent prepare (a stale or wrong-attempt reply) must resolve
// neither ready nor failed.
void verify_relocation_prepare_failed_reply_with_mismatched_identity_is_fenced ()
{
    mesh::raw_mesh_node_owner_t source (mesh::raw_mesh_node_options_t{
      .descriptor = descriptor ("relocation-mismatch-source"),
      .runtime_failures =
        std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()});
    mesh::raw_mesh_node_owner_t target (mesh::raw_mesh_node_options_t{
      .descriptor = descriptor ("relocation-mismatch-target"),
      .runtime_failures =
        std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()});
    source.start ();
    target.start ();
    const auto source_descriptor = source.topology ().local_descriptor ();
    const auto target_descriptor = target.topology ().local_descriptor ();
    admit_pair (source, target, target_descriptor);

    const auto prepare = relocation_prepare_request (source_descriptor, target_descriptor);

    mesh::relocation_prepare_response_t response;
    std::atomic_bool source_settled{false};
    std::thread prepare_thread ([&] {
        response = await_task (
          source.request_relocation_prepare (target_descriptor.node_routing_id, prepare, 300ms));
        source_settled.store (true, std::memory_order_release);
    });

    std::shared_ptr<mesh::service_mailbox_claim_t> claim;
    const auto receive_deadline = std::chrono::steady_clock::now () + 2s;
    while (!claim && std::chrono::steady_clock::now () < receive_deadline) {
        claim =
          target.mailbox ().try_claim (mesh::service_mailbox_domain_t::infrastructure, 1, 4096);
        if (!claim) {
            (void) await_task (
              target.pump_one (mesh::service_liveness_registry_t::clock_t::now ()));
            (void) await_task (
              source.pump_one (mesh::service_liveness_registry_t::clock_t::now ()));
        }
    }
    assert (claim && claim->records.size () == 1);
    const auto &record = (*claim->records.front ());

    // A different exact identity (target_attempt_generation) than the one
    // this prepare sent — the framework's own "newest attempt wins /
    // stale identity discarded" rule (spec 15 §4.2) applies here too.
    auto mismatched = prepare;
    ++mismatched.target_attempt_generation;
    assert (target.reply_relocation_failed (
      record, protocol::relocation_failed_t{
                mismatched.relocation, mismatched.target_attempt_generation, mismatched.coordinator,
                mismatched.target, mismatched.object, protocol::relocation_role_t::target,
                static_cast<std::uint32_t> (protocol::framework_error_code::relocationDataLost)}));
    assert (target.mailbox ().release (*claim));

    const auto settle_deadline = std::chrono::steady_clock::now () + 2s;
    while (!source_settled.load (std::memory_order_acquire)
           && std::chrono::steady_clock::now () < settle_deadline) {
        (void) await_task (source.pump_one (mesh::service_liveness_registry_t::clock_t::now ()));
        std::this_thread::sleep_for (1ms);
    }
    prepare_thread.join ();

    assert (source_settled.load (std::memory_order_acquire));
    assert (!response.ready);
    assert (!response.failed);

    source.close ();
    target.close ();
}

void verify_prepare_does_not_replay_on_selected_route ()
{
    mesh::raw_mesh_node_owner_t source (zlink::framework::test::runtime_failure_options (
      mesh::raw_mesh_node_options_t{descriptor ("Z-prepare-source")}));
    mesh::raw_mesh_node_owner_t target (zlink::framework::test::runtime_failure_options (
      mesh::raw_mesh_node_options_t{descriptor ("A-prepare-target")}));
    source.start ();
    target.start ();
    const auto local = source.topology ().local_descriptor ();
    const auto remote = target.topology ().local_descriptor ();
    source.expect_peer (remote);
    const auto prepare = relocation_prepare_request (local, remote);
    auto pending = source.request_relocation_prepare (remote.node_routing_id, prepare, 2s);
    assert (!pending.await_ready ());
    assert (source.connect_peer (target.endpoint (), remote));
    const auto deadline = std::chrono::steady_clock::now () + 2s;
    const auto receive = [&] {
        std::shared_ptr<mesh::service_mailbox_claim_t> claim;
        while (!claim && std::chrono::steady_clock::now () < deadline) {
            const auto now = std::chrono::steady_clock::now ();
            (void) await_task (source.pump_one (now));
            (void) await_task (target.pump_one (now));
            claim =
              target.mailbox ().try_claim (mesh::service_mailbox_domain_t::infrastructure, 1, 4096);
        }
        assert (claim && claim->records.size () == 1);
        assert (std::get<protocol::relocation_prepare_t> (protocol::decode_relocation_control (
                  protocol::message_bytes (claim->records.front ()->parts ().front ())))
                == prepare);
        return claim;
    };
    auto first = receive ();
    const auto first_record = first->records.front ()->reply_source ();
    assert (first->records.size () == 1);
    assert (target.mailbox ().release (*first));
    assert (target.connect_peer (source.endpoint (), local));
    while (!pending.await_ready () && std::chrono::steady_clock::now () < deadline) {
        const auto now = std::chrono::steady_clock::now ();
        (void) source.expire_requests (now).result ().value ();
        (void) await_task (source.pump_one (now));
        (void) await_task (target.pump_one (now));
        assert (
          !target.mailbox ().try_claim (mesh::service_mailbox_domain_t::infrastructure, 1, 4096));
    }
    assert (pending.await_ready ());
    const auto &terminal = pending.result ();
    assert (terminal && !terminal.value ().ready && !terminal.value ().failed);
    (void) target.reply_relocation_failed (
      *first_record,
      protocol::relocation_failed_t{
        prepare.relocation, prepare.target_attempt_generation, prepare.coordinator, prepare.target,
        prepare.object, protocol::relocation_role_t::target,
        static_cast<std::uint32_t> (protocol::framework_error_code::relocationDataLost)});
    for (int turn = 0; turn < 32; ++turn) {
        const auto now = std::chrono::steady_clock::now ();
        (void) await_task (source.pump_one (now));
        (void) await_task (target.pump_one (now));
        assert (
          !target.mailbox ().try_claim (mesh::service_mailbox_domain_t::infrastructure, 1, 4096));
    }
    assert (&pending.result () == &terminal);
    assert (!pending.result ().value ().ready && !pending.result ().value ().failed);
    assert (std::chrono::steady_clock::now () < deadline);
    source.close ();
    target.close ();
}
}
