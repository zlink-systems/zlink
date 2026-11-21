/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/m6a_actor_join_request.hpp"
#include "m6a_runtime_tests.hpp"
#include "../support/m6a_mesh_fixture.hpp"

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

// actorJoin(28) wire-level request/reply, mirroring the relocationPrepare
// harness pattern above: originate via request_actor_join, the target claims
// the infrastructure mailbox record (standing in for the real admission
// handler, which is a separate, not-yet-landed increment — see
// runtime/mesh/raw_mesh_node_owner.{hpp,cpp} and the C-5 report), and replies
// via reply_actor_join.

std::shared_ptr<mesh::service_mailbox_claim_t>
claim_actor_join (mesh::raw_mesh_node_owner_t &target)
{
    std::shared_ptr<mesh::service_mailbox_claim_t> claim;
    const auto receive_deadline = std::chrono::steady_clock::now () + 2s;
    while (!claim && std::chrono::steady_clock::now () < receive_deadline) {
        claim =
          target.mailbox ().try_claim (mesh::service_mailbox_domain_t::infrastructure, 1, 4096);
        if (!claim) {
            (void) await_task (
              target.pump_one (mesh::service_liveness_registry_t::clock_t::now ()));
        }
    }
    return claim;
}

void verify_actor_join_waits_for_admit_without_replay ()
{
    for (const bool handover : {false, true}) {
        mesh::raw_mesh_node_owner_t source (zlink::framework::test::runtime_failure_options (
          zlink::framework::runtime::mesh::raw_mesh_node_options_t{
            descriptor ("Z-join-replay-source")}));
        mesh::raw_mesh_node_owner_t target (zlink::framework::test::runtime_failure_options (
          zlink::framework::runtime::mesh::raw_mesh_node_options_t{
            descriptor ("A-join-replay-target")}));
        source.start ();
        target.start ();
        const auto local = source.topology ().local_descriptor ();
        const auto remote = target.topology ().local_descriptor ();
        source.expect_peer (remote);
        const auto pump = [&] {
            const auto now = std::chrono::steady_clock::now ();
            (void) source.expire_requests (now).result ().value ();
            (void) source.observe_routes ();
            (void) target.observe_routes ();
            assert (await_task (source.pump_one (now))
                    != mesh::raw_mesh_pump_result_t::protocol_error);
            assert (await_task (target.pump_one (now))
                    != mesh::raw_mesh_pump_result_t::protocol_error);
        };
        if (handover) {
            assert (source.connect_peer (target.endpoint (), remote));
            const auto deadline = std::chrono::steady_clock::now () + 2s;
            while ((!source.topology ().peer (remote.node_routing_id)
                    || !target.topology ().peer (local.node_routing_id))
                   && std::chrono::steady_clock::now () < deadline)
                pump ();
            assert (source.topology ().peer (remote.node_routing_id));
            assert (target.topology ().peer (local.node_routing_id));
        }
        const auto request = actor_join_request (local, remote, 81, "join-actor", "join-spot");
        const std::optional<protocol::application_payload_t> payload{
          {"Join", "application/json", bytes ("{\"value\":7}")}};
        const auto deadline = std::chrono::steady_clock::now () + 2s;
        auto pending = source.request_actor_join (remote.node_routing_id, request, payload,
                                                  handover ? 200ms : 2s);
        assert (!pending.await_ready ());
        const auto receive = [&] {
            std::shared_ptr<mesh::service_mailbox_claim_t> claim;
            while (!claim && std::chrono::steady_clock::now () < deadline) {
                pump ();
                claim = target.mailbox ().try_claim (mesh::service_mailbox_domain_t::infrastructure,
                                                     1, 4096);
            }
            assert (claim && claim->records.size () == 1);
            return claim;
        };
        std::shared_ptr<const mesh::service_mailbox_reply_target_t> first;
        std::vector<zlink::message_t> first_parts;
        if (handover) {
            auto claim = receive ();
            first = claim->records.front ()->reply_source ();
            first_parts = claim->records.front ()->parts ();
            assert (first->reply_token ());
            assert (target.mailbox ().release (*claim));
            assert (!pending.await_ready ());
            assert (target.connect_peer (source.endpoint (), local));
        } else {
            assert (source.connect_peer (target.endpoint (), remote));
        }
        if (handover) {
            while (!pending.await_ready () && std::chrono::steady_clock::now () < deadline) {
                pump ();
                assert (!target.mailbox ().try_claim (
                  mesh::service_mailbox_domain_t::infrastructure, 1, 4096));
            }
            assert (pending.await_ready ());
            const auto &terminal = pending.result ();
            assert (terminal && !terminal.value ().reply);
            const auto failure = terminal.value ().failure;
            (void) target.reply_actor_join (*first, protocol::actor_join_result_t::accepted,
                                            protocol::actor_join_reply_spot_ref_t{"join-spot", 9},
                                            3, 8192, 0, 0, payload);
            for (int turn = 0; turn < 32; ++turn) {
                pump ();
                assert (!target.mailbox ().try_claim (
                  mesh::service_mailbox_domain_t::infrastructure, 1, 4096));
            }
            assert (&pending.result () == &terminal);
            assert (!pending.result ().value ().reply
                    && pending.result ().value ().failure == failure);
            source.close ();
            target.close ();
            continue;
        }
        auto claim = receive ();
        const auto &record = (*claim->records.front ());
        const auto decoded = protocol::decode_actor_join_28 (
          zlink::framework::detail::backend::copy_binding_parts (record.parts ()));
        assert (decoded.correlation == request.correlation);
        if (first)
            assert (zlink::framework::detail::backend::copy_binding_parts (record.parts ())
                    == zlink::framework::detail::backend::copy_binding_parts (first_parts));
        assert (!pending.await_ready ());
        assert (target.reply_actor_join (record, protocol::actor_join_result_t::accepted,
                                         protocol::actor_join_reply_spot_ref_t{"join-spot", 9}, 3,
                                         8192, 0, 0, payload));
        assert (target.mailbox ().release (*claim));
        while (!pending.await_ready () && std::chrono::steady_clock::now () < deadline)
            pump ();
        assert (pending.await_ready ());
        const auto outcome = await_task (std::move (pending));
        assert (outcome.reply
                && outcome.reply->join_result == protocol::actor_join_result_t::accepted);
        assert (outcome.application_reply == payload);
        assert (std::chrono::steady_clock::now () < deadline);
        source.close ();
        target.close ();
    }
}

void verify_actor_join_durable_terminals ()
{
    mesh::raw_mesh_node_owner_t source (zlink::framework::test::runtime_failure_options (
      zlink::framework::runtime::mesh::raw_mesh_node_options_t{
        descriptor ("join-terminal-source")}));
    source.start ();
    const auto local = source.topology ().local_descriptor ();
    const auto remote = descriptor ("join-terminal-target", "tcp://127.0.0.1:1");
    const auto request = actor_join_request (local, remote, 93, "join-actor", "join-spot");
    source.expect_peer (remote);
    const auto started = std::chrono::steady_clock::now ();
    auto expiring = source.request_actor_join (remote.node_routing_id, request, std::nullopt, 50ms);
    const auto expiry_deadline = started + 500ms;
    while (!expiring.await_ready () && std::chrono::steady_clock::now () < expiry_deadline) {
        (void) source.expire_requests (std::chrono::steady_clock::now ()).result ().value ();
        std::this_thread::yield ();
    }
    assert (expiring.await_ready ());
    const auto expired = await_task (std::move (expiring));
    assert (std::chrono::steady_clock::now () - started >= 40ms);
    assert (!expired.reply
            && expired.failure == mesh::actor_join_wire_failure_t::deadline_exceeded);
    auto pending = source.request_actor_join (remote.node_routing_id, request, std::nullopt, 2s);
    assert (!pending.await_ready ());
    source.forget_peer (remote.node_routing_id, remote.advertised_endpoint);
    const auto deadline = std::chrono::steady_clock::now () + 250ms;
    while (!pending.await_ready () && std::chrono::steady_clock::now () < deadline)
        std::this_thread::yield ();
    assert (pending.await_ready ());
    const auto ended = await_task (std::move (pending));
    assert (!ended.reply && ended.failure == mesh::actor_join_wire_failure_t::unavailable);
    mesh::raw_mesh_node_owner_t target (zlink::framework::test::runtime_failure_options (
      zlink::framework::runtime::mesh::raw_mesh_node_options_t{
        descriptor ("join-admitted-target")}));
    target.start ();
    const auto target_descriptor = target.topology ().local_descriptor ();
    admit_pair (source, target, target_descriptor);
    const auto admitted_request =
      actor_join_request (local, target_descriptor, 95, "join-actor", "join-spot");
    const auto admitted_at = std::chrono::steady_clock::now ();
    auto admitted = source.request_actor_join (target_descriptor.node_routing_id, admitted_request,
                                               std::nullopt, 200ms);
    const auto claim = claim_actor_join (target);
    assert (claim && claim->records.front ()->reply_token ());
    assert (target.mailbox ().release (*claim));
    // Physical loss preserves the source's logical connection intent.
    // Local disconnect_rid instead retires issued wait tokens with a typed
    // non-replayable ENOENT (covered by the raw port contract test).
    target.close ();
    const auto completion_deadline = admitted_at + 500ms;
    while (!admitted.await_ready () && std::chrono::steady_clock::now () < completion_deadline) {
        const auto now = std::chrono::steady_clock::now ();
        (void) source.expire_requests (now).result ().value ();
        (void) source.observe_routes ();
        (void) await_task (source.pump_one (now));
    }
    assert (admitted.await_ready ());
    const auto lost_reply = await_task (std::move (admitted));
    // Submit §5 delivers binding connection loss without waiting for the
    // logical deadline or submitting the admitted request again.
    assert (!lost_reply.reply
            && lost_reply.failure == mesh::actor_join_wire_failure_t::unavailable);
    assert (source.pending_operation_count () == 0);
    source.close ();
}

void verify_actor_join_ends_after_unexpected_admitted_peer_loss (bool monitor_disconnect)
{
    mesh::raw_mesh_node_owner_t source (zlink::framework::test::runtime_failure_options (
      zlink::framework::runtime::mesh::raw_mesh_node_options_t{
        descriptor (monitor_disconnect ? "join-owner-loss-disconnect-source"
                                       : "join-owner-loss-liveness-source")}));
    mesh::raw_mesh_node_owner_t target (zlink::framework::test::runtime_failure_options (
      zlink::framework::runtime::mesh::raw_mesh_node_options_t{
        descriptor (monitor_disconnect ? "join-owner-loss-disconnect-target"
                                       : "join-owner-loss-liveness-target")}));
    source.start ();
    target.start ();
    const auto local = source.topology ().local_descriptor ();
    const auto remote = target.topology ().local_descriptor ();
    admit_pair (source, target, remote);

    const auto request = actor_join_request (local, remote, monitor_disconnect ? 97 : 99,
                                             "join-owner-loss-actor", "join-owner-loss-spot");
    auto pending = source.request_actor_join (remote.node_routing_id, request, std::nullopt, 2s);
    const auto claim = claim_actor_join (target);
    assert (claim && claim->records.front ()->reply_token ());
    assert (target.mailbox ().release (*claim));

    source.forget_peer (remote.node_routing_id, target.endpoint ());
    assert (source.topology ().peer (remote.node_routing_id));
    assert (!pending.await_ready ());

    if (monitor_disconnect) {
        target.close ();
        const auto disconnect_deadline = std::chrono::steady_clock::now () + 1s;
        while (source.topology ().peer (remote.node_routing_id)
               && std::chrono::steady_clock::now () < disconnect_deadline) {
            const auto now = mesh::service_liveness_registry_t::clock_t::now ();
            (void) source.observe_routes ();
            (void) await_task (source.pump_one (now));
        }
    } else {
        (void) await_task (
          source.tick_liveness (mesh::service_liveness_registry_t::clock_t::now () + 16s));
    }

    assert (!source.topology ().peer (remote.node_routing_id));
    const auto completion_deadline = std::chrono::steady_clock::now () + 250ms;
    while (!pending.await_ready () && std::chrono::steady_clock::now () < completion_deadline)
        std::this_thread::yield ();
    assert (pending.await_ready ());
    const auto outcome = await_task (std::move (pending));
    assert (!outcome.reply && outcome.failure == mesh::actor_join_wire_failure_t::unavailable);
    source.close ();
    target.close ();
}

// Accepted case: the tail's spot ref, membershipEpoch, and
// receiveChunkLimitBytes must all thread through unchanged from the
// receiver's reply_actor_join call to the originate side's decoded tail —
// this is the exact information the ORIGINATE side's completion bookkeeping
// (dd23fb0b36's receiveChunkLimitBytes consumption) depends on.
void verify_actor_join_accepted_reply_threads_chunk_limit_and_epoch ()
{
    mesh::raw_mesh_node_owner_t source (mesh::raw_mesh_node_options_t{
      .descriptor = descriptor ("actor-join-accept-source"),
      .runtime_failures =
        std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()});
    mesh::raw_mesh_node_owner_t target (mesh::raw_mesh_node_options_t{
      .descriptor = descriptor ("actor-join-accept-target"),
      .runtime_failures =
        std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()});
    source.start ();
    target.start ();
    const auto source_descriptor = source.topology ().local_descriptor ();
    const auto target_descriptor = target.topology ().local_descriptor ();
    admit_pair (source, target, target_descriptor);

    const auto request =
      actor_join_request (source_descriptor, target_descriptor, 4242, "actor-1", "spot-1");

    mesh::actor_join_wire_outcome_t outcome;
    std::atomic_bool source_settled{false};
    std::thread join_thread ([&] {
        outcome = await_task (
          source.request_actor_join (target_descriptor.node_routing_id, request, std::nullopt, 5s));
        source_settled.store (true, std::memory_order_release);
    });

    const auto claim = claim_actor_join (target);
    assert (claim && claim->records.size () == 1);
    const auto &record = (*claim->records.front ());
    const auto decoded = protocol::decode_actor_join_28 (
      zlink::framework::detail::backend::copy_binding_parts (record.parts ()));
    assert (decoded.correlation == request.correlation);
    assert (decoded.actor.id == request.actor.actor_id);
    assert (decoded.actor.generation == request.actor.object_generation);
    assert (decoded.actor.target_node_rid == request.actor.target_node_routing_id);
    assert (decoded.actor.target_node_generation == request.actor.target_node_generation);
    assert (decoded.actor.expected_authority_owner_generation
            == request.actor.authority_owner_generation);
    assert (decoded.actor.expected_owner_lease_generation == request.actor.owner_lease_generation);
    assert (decoded.entry == request.entry);
    assert (decoded.target_spot.id == request.target_spot.spot_id);
    assert (decoded.target_spot.generation == request.target_spot.object_generation);
    assert (decoded.target_spot.target_node_rid == request.target_spot.target_node_routing_id);
    assert (decoded.target_spot.target_node_generation
            == request.target_spot.target_node_generation);
    assert (decoded.target_spot.expected_authority_owner_generation
            == request.target_spot.authority_owner_generation);
    assert (decoded.target_spot.expected_owner_lease_generation
            == request.target_spot.owner_lease_generation);
    assert (!decoded.payload);
    assert (record.correlation && *record.correlation == request.correlation);

    assert (target.reply_actor_join (record, protocol::actor_join_result_t::accepted,
                                     protocol::actor_join_reply_spot_ref_t{"spot-1", 9}, 3, 8192));
    assert (target.mailbox ().release (*claim));

    const auto settle_deadline = std::chrono::steady_clock::now () + 2s;
    while (!source_settled.load (std::memory_order_acquire)
           && std::chrono::steady_clock::now () < settle_deadline) {
        (void) await_task (source.pump_one (mesh::service_liveness_registry_t::clock_t::now ()));
        std::this_thread::sleep_for (1ms);
    }
    join_thread.join ();

    assert (source_settled.load (std::memory_order_acquire));
    const auto &response = outcome.reply;
    assert (response);
    assert (response->join_result == protocol::actor_join_result_t::accepted);
    assert (response->spot && response->spot->spot_id == "spot-1");
    assert (response->spot->object_generation == 9);
    assert (response->membership_epoch == 3);
    assert (response->receive_chunk_limit_bytes == 8192);

    source.close ();
    target.close ();
}

// Rejected case: a typed rejection (no spot) must resolve as rejected, not
// as a timeout/no-result and not as accepted.
void verify_actor_join_rejected_reply_completes_typed_failure ()
{
    mesh::raw_mesh_node_owner_t source (mesh::raw_mesh_node_options_t{
      .descriptor = descriptor ("actor-join-reject-source"),
      .runtime_failures =
        std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()});
    mesh::raw_mesh_node_owner_t target (mesh::raw_mesh_node_options_t{
      .descriptor = descriptor ("actor-join-reject-target"),
      .runtime_failures =
        std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()});
    source.start ();
    target.start ();
    const auto source_descriptor = source.topology ().local_descriptor ();
    const auto target_descriptor = target.topology ().local_descriptor ();
    admit_pair (source, target, target_descriptor);

    const auto request =
      actor_join_request (source_descriptor, target_descriptor, 4343, "actor-2", "spot-2");

    mesh::actor_join_wire_outcome_t outcome;
    std::atomic_bool source_settled{false};
    std::thread join_thread ([&] {
        outcome = await_task (
          source.request_actor_join (target_descriptor.node_routing_id, request, std::nullopt, 5s));
        source_settled.store (true, std::memory_order_release);
    });

    const auto claim = claim_actor_join (target);
    assert (claim && claim->records.size () == 1);
    const auto &record = (*claim->records.front ());
    const protocol::application_payload_t application_reply{
      "actor-join-rejected", "application/octet-stream", bytes ("not-approved")};
    bool terminal_payload_rejected = false;
    try {
        (void) target.reply_actor_join (record, protocol::actor_join_result_t::rejected,
                                        std::nullopt, 0, 0, 105, 17, application_reply);
    }
    catch (const std::invalid_argument &) {
        terminal_payload_rejected = true;
    }
    assert (terminal_payload_rejected);
    assert (target.reply_actor_join (record, protocol::actor_join_result_t::rejected, std::nullopt,
                                     0, 0, 0, 0, application_reply));
    assert (target.mailbox ().release (*claim));

    const auto settle_deadline = std::chrono::steady_clock::now () + 2s;
    while (!source_settled.load (std::memory_order_acquire)
           && std::chrono::steady_clock::now () < settle_deadline) {
        (void) await_task (source.pump_one (mesh::service_liveness_registry_t::clock_t::now ()));
        std::this_thread::sleep_for (1ms);
    }
    join_thread.join ();

    assert (source_settled.load (std::memory_order_acquire));
    const auto &response = outcome.reply;
    assert (response);
    assert (response->join_result == protocol::actor_join_result_t::rejected);
    assert (!response->spot);
    assert (outcome.application_reply == application_reply);

    source.close ();
    target.close ();
}

// Identity-fence case: the receiver must reject an inbound actorJoin(28)
// whose "actor" fence claims a source node lifecycle generation different
// from the one this transport connection was actually admitted under — a
// wrong-generation sender is a protocol error, exactly like the existing
// actorCreate/userSpotCreate/userSpotClose/relocationPrepare fence checks.
void verify_actor_join_wrong_source_generation_is_fenced ()
{
    mesh::raw_mesh_node_owner_t source (mesh::raw_mesh_node_options_t{
      .descriptor = descriptor ("actor-join-fence-source"),
      .runtime_failures =
        std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()});
    mesh::raw_mesh_node_owner_t target (mesh::raw_mesh_node_options_t{
      .descriptor = descriptor ("actor-join-fence-target"),
      .runtime_failures =
        std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()});
    source.start ();
    target.start ();
    const auto source_descriptor = source.topology ().local_descriptor ();
    const auto target_descriptor = target.topology ().local_descriptor ();
    admit_pair (source, target, target_descriptor);

    auto request =
      actor_join_request (source_descriptor, target_descriptor, 4444, "actor-3", "spot-3");
    // Claim a source node generation the connection was not actually
    // admitted under.
    request.actor.target_node_generation = source_descriptor.lifecycle_generation + 1;

    mesh::actor_join_wire_outcome_t outcome;
    std::atomic_bool source_settled{false};
    std::thread join_thread ([&] {
        outcome = await_task (source.request_actor_join (target_descriptor.node_routing_id, request,
                                                         std::nullopt, 300ms));
        source_settled.store (true, std::memory_order_release);
    });

    mesh::raw_mesh_pump_result_t pumped = mesh::raw_mesh_pump_result_t::no_data;
    const auto receive_deadline = std::chrono::steady_clock::now () + 2s;
    while (pumped != mesh::raw_mesh_pump_result_t::protocol_error
           && std::chrono::steady_clock::now () < receive_deadline) {
        pumped = await_task (target.pump_one (mesh::service_liveness_registry_t::clock_t::now ()));
    }
    assert (pumped == mesh::raw_mesh_pump_result_t::protocol_error);
    // A fenced inbound frame must not have reached the infrastructure
    // mailbox at all.
    assert (!target.mailbox ().try_claim (mesh::service_mailbox_domain_t::infrastructure, 1, 4096));

    const auto settle_deadline = std::chrono::steady_clock::now () + 2s;
    while (!source_settled.load (std::memory_order_acquire)
           && std::chrono::steady_clock::now () < settle_deadline) {
        (void) await_task (source.pump_one (mesh::service_liveness_registry_t::clock_t::now ()));
        std::this_thread::sleep_for (1ms);
    }
    join_thread.join ();

    assert (source_settled.load (std::memory_order_acquire));
    // Wire protocol §2 terminates this protocol rejection on the original
    // request reply path; the source fence still prevents admission.
    assert (outcome.reply);
    assert (outcome.reply->header.correlation == request.correlation);
    assert (outcome.reply->header.terminal_result
            == static_cast<std::uint32_t> (protocol::request_terminal_result::protocolError));
    assert (outcome.reply->header.failure_code
            == static_cast<std::uint32_t> (protocol::framework_error_code::requestProtocolError));
    assert (!outcome.reply->spot);
    assert (!outcome.application_reply);

    source.close ();
    target.close ();
}

// Malformed-reply classification (spec 32 §5): a decodable reply whose
// correlation does not match the request this call sent is a stale or
// misrouted reply — the typed outcome must classify it as protocol_error,
// never as accepted/rejected and never as a plain timeout.
void verify_actor_join_mismatched_correlation_reply_classifies_protocol_error ()
{
    mesh::raw_mesh_node_owner_t source (mesh::raw_mesh_node_options_t{
      .descriptor = descriptor ("actor-join-mismatch-source"),
      .runtime_failures =
        std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()});
    mesh::raw_mesh_node_owner_t target (mesh::raw_mesh_node_options_t{
      .descriptor = descriptor ("actor-join-mismatch-target"),
      .runtime_failures =
        std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()});
    source.start ();
    target.start ();
    const auto source_descriptor = source.topology ().local_descriptor ();
    const auto target_descriptor = target.topology ().local_descriptor ();
    admit_pair (source, target, target_descriptor);

    const auto request =
      actor_join_request (source_descriptor, target_descriptor, 4545, "actor-4", "spot-4");

    mesh::actor_join_wire_outcome_t outcome;
    std::atomic_bool source_settled{false};
    std::thread join_thread ([&] {
        outcome = await_task (
          source.request_actor_join (target_descriptor.node_routing_id, request, std::nullopt, 5s));
        source_settled.store (true, std::memory_order_release);
    });

    const auto claim = claim_actor_join (target);
    assert (claim && claim->records.size () == 1);
    // Reply through the raw route (so the frame reaches the pending
    // request), but stamp a correlation that does not identify it.
    auto misrouted = std::const_pointer_cast<mesh::service_mailbox_reply_target_t> (
      claim->records.front ()->reply_source ());
    misrouted->correlation = request.correlation + 2;
    assert (target.reply_actor_join (*misrouted, protocol::actor_join_result_t::accepted,
                                     protocol::actor_join_reply_spot_ref_t{"spot-4", 9}, 3, 8192));
    assert (target.mailbox ().release (*claim));

    const auto settle_deadline = std::chrono::steady_clock::now () + 2s;
    while (!source_settled.load (std::memory_order_acquire)
           && std::chrono::steady_clock::now () < settle_deadline) {
        (void) await_task (source.pump_one (mesh::service_liveness_registry_t::clock_t::now ()));
        std::this_thread::sleep_for (1ms);
    }
    join_thread.join ();

    assert (source_settled.load (std::memory_order_acquire));
    assert (!outcome.reply);
    assert (outcome.failure == mesh::actor_join_wire_failure_t::protocol_error);

    source.close ();
    target.close ();
}

// The receiving admission owner reads this target Spot Authority row. Only
// the target Spot key resolves; an Actor Authority read finds no row.
class actor_join_target_authority_store_t final : public runtime::in_memory_location_repository_t
{
  public:
    explicit actor_join_target_authority_store_t (zlink::framework::authority_snapshot_t target) :
        _target (std::move (target))
    {
    }

    zlink::framework::task_t<zlink::framework::authority_read_result_t>
    read_authority_core (zlink::framework::authority_key_t key, std::stop_token) override
    {
        using read_t = zlink::framework::authority_read_result_t;
        if (key.value == runtime::spot_authority_key ("spot-5").value)
            return zlink::framework::task_t<read_t> (
              zlink::framework::result_t<read_t>::success (read_t{_target}));
        return zlink::framework::task_t<read_t> (zlink::framework::result_t<read_t>::success (
          read_t{zlink::framework::authority_missing_t{std::chrono::system_clock::now ()}}));
    }

  private:
    zlink::framework::authority_snapshot_t _target;
};

// Matching target RID and lifecycle generation permits sender admission.
// The receiving owner independently rejects a stale target authority fence;
// raw ingress must preserve that request for the receiving admission owner.
void verify_actor_join_target_fence_reaches_admission ()
{
    namespace zf = zlink::framework;
    mesh::raw_mesh_node_owner_t source (mesh::raw_mesh_node_options_t{
      .descriptor = descriptor ("actor-join-thin-source"),
      .runtime_failures =
        std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()});
    mesh::raw_mesh_node_owner_t target (mesh::raw_mesh_node_options_t{
      .descriptor = descriptor ("actor-join-thin-target"),
      .runtime_failures =
        std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()});
    source.start ();
    target.start ();
    const auto source_descriptor = source.topology ().local_descriptor ();
    const auto target_descriptor = target.topology ().local_descriptor ();
    admit_pair (source, target, target_descriptor);

    auto request =
      actor_join_request (source_descriptor, target_descriptor, 4646, "actor-5", "spot-5");

    mesh::actor_join_wire_outcome_t outcome;
    std::atomic_bool settled{false};
    std::thread join_thread ([&] {
        outcome = await_task (
          source.request_actor_join (target_descriptor.node_routing_id, request, std::nullopt, 5s));
        settled.store (true, std::memory_order_release);
    });
    const auto claim = claim_actor_join (target);
    assert (claim && claim->records.size () == 1);
    assert (claim->records.front ()->correlation
            && *claim->records.front ()->correlation == request.correlation);
    const auto incoming = protocol::decode_actor_join_28 (
      zlink::framework::detail::backend::copy_binding_parts (claim->records.front ()->parts ()));
    assert (incoming.target_spot.target_node_rid == target_descriptor.node_routing_id);
    assert (incoming.target_spot.target_node_generation == target_descriptor.lifecycle_generation);
    // The receiving admission owner's Store holds a newer target authority than the request.
    const auto target_rid = zlink::routing_id_t::from (target_descriptor.node_routing_id);
    actor_join_target_authority_store_t store (zf::authority_snapshot_t{
      .store_version = "actor-join-thin-target-v1",
      .object_generation = request.target_spot.object_generation,
      .authority_owner_generation = request.target_spot.authority_owner_generation + 1,
      .owner = zf::location_owner_token_t{"actor-join-thin-target-owner",
                                          static_cast<std::int64_t> (
                                            request.target_spot.owner_lease_generation)},
      .store_now = std::chrono::system_clock::now (),
      .allocation = {
        .state = zf::placement_allocation_state_t::active,
        .object_kind = zf::placement_object_kind_t::user_spot,
        .stable_type = "target",
        .target = {.mesh_name = "m6a-mesh",
                   .node_rid = zf::node_rid_t::from_string (target_rid.to_string ()),
                   .node_lifecycle_generation = target_descriptor.lifecycle_generation,
                   .owner = zf::location_owner_token_t{
                     "actor-join-thin-target-owner",
                     static_cast<std::int64_t> (request.target_spot.owner_lease_generation)}}}});
    auto spot_state = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::spot_node_builder_state_t> ("actor-join-thin-target");
    zlink::framework::detail::spot_node_runtime_t spots (spot_state);
    zf::service_collection_t services;
    services.add_factory<runtime::live_location_reader_t> (
      [&store] (zf::service_provider_t &) {
          return std::make_unique<runtime::live_location_reader_t> (store);
      },
      zf::service_lifetime_t::singleton);
    auto provider = services.build_provider ();
    spots.bind_service_provider (provider);
    // The receiving owner judges the fence the wire carried, not the sender's copy.
    const protocol::actor_join_request_t received{
      incoming.correlation,
      protocol::actor_route_fence_t{
        incoming.actor.id, incoming.actor.generation, incoming.actor.target_node_rid,
        incoming.actor.target_node_generation, incoming.actor.expected_authority_owner_generation,
        incoming.actor.expected_owner_lease_generation},
      incoming.entry,
      protocol::spot_route_fence_t{incoming.target_spot.id, incoming.target_spot.generation,
                                   incoming.target_spot.target_node_rid,
                                   incoming.target_spot.target_node_generation,
                                   incoming.target_spot.expected_authority_owner_generation,
                                   incoming.target_spot.expected_owner_lease_generation}};
    const auto admitted = zlink::framework::detail::admit_wire_actor_join (
      spot_state, target_rid, received, std::nullopt, nullptr);
    assert (admitted.join_result != protocol::actor_join_result_t::accepted);
    assert (!admitted.spot);
    assert (admitted.terminal_result
            == static_cast<std::uint32_t> (protocol::request_terminal_result::protocolError));
    assert (admitted.failure_code
            == static_cast<std::uint32_t> (protocol::framework_error_code::requestProtocolError));
    assert (target.reply_actor_join ((*claim->records.front ()), admitted.join_result,
                                     admitted.spot, admitted.membership_epoch,
                                     admitted.receive_chunk_limit_bytes, admitted.terminal_result,
                                     admitted.failure_code, admitted.application_reply));
    assert (target.mailbox ().release (*claim));

    const auto deadline = std::chrono::steady_clock::now () + 2s;
    while (!settled.load (std::memory_order_acquire)
           && std::chrono::steady_clock::now () < deadline) {
        (void) await_task (source.pump_one (mesh::service_liveness_registry_t::clock_t::now ()));
        std::this_thread::sleep_for (1ms);
    }
    join_thread.join ();
    assert (settled.load (std::memory_order_acquire));
    // The source observes the receiving owner's rejection, not a fixture-computed one.
    assert (outcome.reply);
    assert (outcome.reply->header.correlation == request.correlation);
    assert (outcome.reply->header.terminal_result == admitted.terminal_result);
    assert (outcome.reply->header.failure_code == admitted.failure_code);
    assert (!outcome.reply->spot);
    spots.release_native_handles ();
    source.close ();
    target.close ();
}
}
