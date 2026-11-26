/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/m6c_runtime_cases.hpp"
#include "../support/m6c_stateful_fixture.hpp"
#include "../support/m6c_task.hpp"
#include "../support/request_admission_fixture.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "../support/stateful_ingress_fixture.hpp"
#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include "runtime/stateful/raw_stateful_dispatch.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace m6c_test
{

using namespace zlink::framework::runtime::stateful;

void test_boundary_application_preserves_original_reply (test_context_t &test,
                                                         bool no_follow,
                                                         bool source_expired)
{
    using namespace std::chrono_literals;
    namespace framework = zlink::framework;
    namespace protocol = framework::runtime::protocol;
    namespace mesh = framework::runtime::mesh;
    namespace foundation = framework::runtime::foundation;
    const auto descriptor = [] (std::string id) {
        return mesh::service_node_descriptor_t{"mesh",
                                               {id.begin (), id.end ()},
                                               1,
                                               1,
                                               "tcp://127.0.0.1:0",
                                               {},
                                               mesh::service_node_state_t::preparing};
    };
    mesh::raw_mesh_node_owner_t source (mesh::raw_mesh_node_options_t{
      .descriptor = descriptor ("late-source"),
      .runtime_failures =
        std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()});
    mesh::raw_mesh_node_owner_t target (mesh::raw_mesh_node_options_t{
      .descriptor = descriptor ("late-target"),
      .runtime_failures =
        std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()});
    source.start ();
    target.start ();
    const auto src = source.topology ().local_descriptor ();
    const auto dst = target.topology ().local_descriptor ();
    (void) source.connect_peer (dst.advertised_endpoint, dst);
    const auto pump_until = [&] (const std::function<bool ()> &done) {
        const auto deadline = std::chrono::steady_clock::now () + 5s;
        while (!done () && std::chrono::steady_clock::now () < deadline) {
            const auto now = std::chrono::steady_clock::now ();
            (void) await_task (source.pump_one (now));
            (void) await_task (target.pump_one (now));
            std::this_thread::yield ();
        }
        return done ();
    };
    test.require (pump_until ([&] {
                      const auto source_peer = source.topology ().peer (dst.node_routing_id);
                      const auto target_peer = target.topology ().peer (src.node_routing_id);
                      return source_peer && source_peer->ready && target_peer && target_peer->ready;
                  }),
                  "late ingress peers must be admitted");
    stateful_object_runtime_t objects;
    stateful_object_runtime_t restored;
    const auto configure = [] (stateful_object_runtime_t &runtime) {
        runtime.configure_relocation_state (
          [] (const object_ref_t &, const std::string &, std::stop_token)
            -> zlink::framework::task_t<
              zlink::framework::runtime::stateful::frozen_object_state_t> {
              frozen_object_state_t captured;
              captured.application_state = std::vector<std::uint8_t>{1};
              co_return captured;
          },
          [] (const frozen_object_state_t &, const object_ref_t &,
              std::stop_token) -> zlink::framework::task_t<bool> { co_return true; });
    };
    configure (objects);
    configure (restored);
    const auto actor = create_actor (objects, "late-reply", "late-source");
    auto relocated = actor;
    relocated.node_id = "late-target";
    relocated.authority_owner_generation += 8;
    // Relocation-flow §4.3: the temporary queue is fenced by the captured source.
    const auto resolver = [] (accepted_record_authority_query_t query)
      -> zlink::framework::task_t<std::optional<accepted_record_authority_t>> {
        co_return accepted_record_authority_t{
          {"owner", 1, query.source_node_routing_id, query.source_node_generation}, 1};
    };
    raw_stateful_dispatch_t ingress (objects, source, resolver);
    raw_stateful_dispatch_t destination (restored, target, resolver);
    const auto sealed = await_task (objects.try_seal_relocation_aggregate ({actor}));
    const relocation_restore_identity_t identity{"late-reply", 1, {}};
    test.require (
      sealed.error == stateful_error_t::none
        && restored.restore_relocation (sealed.seal.participants.front (), relocated, identity, {})
               .result ()
               .value ()
             == stateful_error_t::none,
      "late ingress target must restore before boundary");
    const foundation::call_id_t occupied{src.lifecycle_generation, 77};
    test.require (source
                    .register_local_operation_async (
                      std::chrono::steady_clock::now () + 5s, [] (auto, auto) {}, occupied)
                    .result ()
                    .value ()
                    .has_value (),
                  "source must reserve its independent local operation before forwarding");
    std::vector<framework::task_t<bool>> submissions;
    const auto boundary = objects.begin_relocation_boundary (
      sealed.seal.token, [&] (const object_ref_t &owner, const turn_record_t &turn) {
          // Forwarding starts only after the original reply route is registered.
          if (turn.application_record && turn.application_record->reply_route_id)
              test.require (*turn.application_record->reply_route_id
                              == turn.application_record->operation.low,
                            "accepted ingress must retain its original reply route");
          submissions.push_back (ingress.forward_accepted (owner, turn, dst.node_routing_id,
                                                           dst.lifecycle_generation, 1, 5s,
                                                           relocated.authority_owner_generation));
      });
    test.require (boundary.first == stateful_error_t::none, "late ingress boundary must open");
    std::atomic<int> completed{0};
    bool reply_matches = true;
    const auto pump_ingress = [&] (stateful_object_runtime_t &runtime,
                                   mesh::raw_mesh_node_owner_t &transport,
                                   raw_stateful_dispatch_t &dispatch, const object_ref_t &owner) {
        stateful_error_t error = stateful_error_t::not_found;
        return pump_until ([&] {
                   if (error == stateful_error_t::not_found)
                       error = zlink::framework::test::ingest_stateful_mailbox_record (
                                 runtime, transport, dispatch, owner)
                                 .result ()
                                 .value ();
                   return error != stateful_error_t::not_found;
               })
               && error == stateful_error_t::none;
    };
    const auto request = [&] (std::uint64_t id) {
        const auto submitted_before = submissions.size ();

        test.require (
          await_task (target.request_to_actor (
            src.node_routing_id, std::nullopt,
            {actor.key, actor.object_generation, src.node_routing_id, src.lifecycle_generation,
             actor.authority_owner_generation, 1},
            {"ActorPacket", "application/json", {static_cast<std::uint8_t> (id)}}, 5s,
            [&, id] (foundation::operation_terminal_t terminal,
                     ::zlink::framework::result_t<zlink::message_t> completion_payload) {
                auto native_bytes = std::move (completion_payload.value ());
                auto bytes = native_bytes.to_bytes ();
                if (id == 92 || (source_expired && id == 77)) {
                    if (terminal != foundation::operation_terminal_t::transport_failed
                        || bytes.empty ())
                        reply_matches = false;
                    else {
                        const auto reply = protocol::decode_reply_header (bytes);
                        reply_matches =
                          reply_matches
                          && framework::runtime::messaging::request_failure_mapper_t{}
                                 .reply_header_exception (reply.terminal_result, reply.failure_code,
                                                          "source relocation")
                                 .kind ()
                               == framework::framework_error_kind_t::unavailable;
                    }
                } else if (id == 91) {
                    reply_matches =
                      reply_matches
                      && terminal == foundation::operation_terminal_t::transport_failed
                      && protocol::decode_reply_header (bytes).failure_code
                           == static_cast<std::uint32_t> (
                             protocol::framework_error_code::requestFailed);
                } else {
                    reply_matches = reply_matches
                                    && terminal == foundation::operation_terminal_t::completed
                                    && protocol::decode_application_payload (bytes).payload_bytes ()
                                         == std::vector<std::uint8_t>{0x7a};
                }
                ++completed;
            },
            protocol::wire_operation_id_t{0x1087, id}, std::nullopt, id)),
          "original request must submit");
        const auto accepted = pump_ingress (objects, source, ingress, actor);
        test.require (accepted, "boundary ingress must be accepted with its pending reply route");
        if (!accepted || submissions.size () != submitted_before + 1)
            return false;
        test.require (await_task (std::move (submissions.back ())),
                      "accepted ingress must use the application transport");
        if (id == 92)
            return true;
        const auto delivered = pump_ingress (restored, target, destination, relocated);
        test.require (delivered, "forwarded application must retain the target fence");
        return delivered;
    };
    const auto stage = [&] (std::uint64_t id) {
        protocol::frozen_application_record_t record;
        record.kind = protocol::frozen_record_kind_t::actor_send;
        record.source_kind = protocol::frozen_source_kind_t::node;
        record.source = {"owner", 1, src.node_routing_id, src.lifecycle_generation};
        record.operation = {0x1087, id};
        record.body = protocol::frozen_actor_application_body_t{
          {actor.key, actor.object_generation, dst.node_routing_id, dst.lifecycle_generation,
           relocated.authority_owner_generation, 1},
          {"ActorPacket", "application/json", {static_cast<std::uint8_t> (id)}}};
        return destination
          .stage_relocated (
            relocated, {0, protocol::encode_frozen_application_record (record).canonical_bytes},
            [] (const auto &) { return true; })
          .result ()
          .value ();
    };
    test.require (stage (1) == stateful_error_t::none, "saved work must stage first");
    if (!request (77))
        return;
    test.require (await_task (target.send_to_actor (
                    src.node_routing_id, std::nullopt,
                    {actor.key, actor.object_generation, src.node_routing_id,
                     src.lifecycle_generation, actor.authority_owner_generation, 1},
                    {"ActorPacket", "application/json", {80}}, std::nullopt,
                    protocol::wire_operation_id_t{0x1087, 80})),
                  "original one-way send must submit");
    const auto send_accepted = pump_ingress (objects, source, ingress, actor);
    test.require (send_accepted, "source must accept the original one-way send");
    if (!send_accepted)
        return;
    test.require (await_task (std::move (submissions.back ()))
                    && pump_ingress (restored, target, destination, relocated),
                  "target must accept the forwarded one-way send");
    test.require (stage (2) == stateful_error_t::none,
                  "verified boundary must stage after late arrival");
    test.require (restored.try_claim (relocated, turn_domain_t::application).first
                    == stateful_error_t::moving,
                  "late ingress before owner commit must remain gated");
    relocation_ingress_batch_t committed;
    test.require (
      restored.commit_relocation_restore (relocated, identity) == stateful_error_t::none
        && objects
               .finalize_relocation_cutover (
                 sealed.seal.token,
                 [&, no_follow] (const object_ref_t &owner, const turn_record_t &turn) {
                     submissions.push_back (ingress.forward_accepted (
                       owner, turn, dst.node_routing_id, dst.lifecycle_generation, 1, 5s,
                       no_follow
                         ? std::nullopt
                         : std::optional<std::uint64_t>{relocated.authority_owner_generation}));
                 },
                 &committed)
               .result ()
               .value ()
             == stateful_error_t::none,
      "owner commit must release the target queue and source retained copy");
    test.require (committed.participants.size () == 1, "source must return its committed prefix");
    if (committed.participants.empty ())
        return;
    ingress.release_relocated_payloads (actor, committed.participants.front ().records);
    if (source_expired) {
        test.require (completed.load () == 0,
                      "source expiry must precede the original request terminal");
        test.require (ingress.fail_pending_unavailable (actor) == stateful_error_t::none,
                      "expired source must terminate its accepted requests without mailbox claims");
        test.require (pump_until ([&] { return completed.load () == 1; }),
                      "expired source must reply on the original request route");
        test.require (reply_matches && completed == 1
                        && objects.pending (actor, turn_domain_t::application) == 0,
                      "expired source must complete the original request with Unavailable once");
        (void) source.unregister_local_operation (occupied);
        source.close ();
        target.close ();
        return;
    }
    if (no_follow) {
        test.require (request (92) && pump_until ([&] { return completed.load () == 1; }),
                      "late noFollow request must receive Unavailable before the earlier target "
                      "request completes");
        test.require (await_task (target.send_to_actor (
                        src.node_routing_id, std::nullopt,
                        {actor.key, actor.object_generation, src.node_routing_id,
                         src.lifecycle_generation, actor.authority_owner_generation, 1},
                        {"ActorPacket", "application/json", {93}}, std::nullopt,
                        protocol::wire_operation_id_t{0x1087, 93}))
                        && pump_ingress (objects, source, ingress, actor)
                        && await_task (std::move (submissions.back ())),
                      "late noFollow send must discard only its own accepted record");
    }
    for (std::uint64_t id : {1u, 2u}) {
        const auto [error, delivery] = destination.try_claim (relocated);
        test.require (error == stateful_error_t::none && delivery
                        && delivery->frozen.operation.low == id,
                      "saved work and verified boundary must precede late ingress");
        if (delivery)
            (void) await_task (destination.complete_async (*delivery));
    }
    for (std::uint64_t id : {77u, 80u, 90u, 91u}) {
        if (no_follow && id >= 90)
            break;
        if ((id == 90 || id == 91) && !request (id))
            return;
        const auto [error, delivery] = destination.try_claim (relocated);
        test.require (
          error == stateful_error_t::none && delivery
            && delivery->frozen.operation == protocol::wire_operation_id_t{0x1087, id}
            && (id == 80 ? !delivery->frozen.reply_route_id
                         : delivery->frozen.reply_route_id.has_value ()),
          "forwarding must preserve the original operation and use a hop-local reply route");
        if (delivery)
            test.require (
              await_task (destination.complete_async (
                *delivery, id == 91
                             ? std::nullopt
                             : std::optional<protocol::application_payload_t>{{"ActorReply",
                                                                               "application/json",
                                                                               {0x7a}}}))
                == stateful_error_t::none,
              "target reply must complete");
        test.require (pump_until ([&] {
                          return completed.load ()
                                 == static_cast<int> ((id == 77 || id == 80) ? (no_follow ? 2 : 1)
                                                      : id == 90             ? 2
                                                                             : 3);
                      }),
                      "forwarded reply must complete the original requester once");
    }
    test.require (reply_matches && completed == (no_follow ? 2 : 3)
                    && objects.pending (actor, turn_domain_t::application) == 0,
                  "original reply payloads must survive forwarding without retained source work");
    (void) source.unregister_local_operation (occupied);
    source.close ();
    target.close ();
}

void test_boundary_preserve_restores_acceptance_order (test_context_t &test)
{
    stateful_object_runtime_t objects;
    objects.configure_relocation_state (
      [] (const object_ref_t &, const std::string &, std::stop_token)
        -> zlink::framework::task_t<zlink::framework::runtime::stateful::frozen_object_state_t> {
          frozen_object_state_t captured;
          captured.application_state = std::vector<std::uint8_t>{1};
          co_return captured;
      },
      [] (const frozen_object_state_t &, const object_ref_t &,
          std::stop_token) -> zlink::framework::task_t<bool> { co_return true; });
    const auto actor = create_actor (objects, "preserve-late");
    (void) objects.enqueue (actor, turn_domain_t::application, {1, {1}});
    const auto sealed = await_task (objects.try_seal_relocation_aggregate ({actor}));
    (void) objects.enqueue (actor, turn_domain_t::application, {2, {2}});
    std::vector<std::uint64_t> forwarded;
    const auto boundary = objects.begin_relocation_boundary (
      sealed.seal.token, [&] (const object_ref_t &, const turn_record_t &turn) {
          forwarded.push_back (turn.sequence);
      });
    (void) objects.enqueue (actor, turn_domain_t::application, {3, {3}});
    (void) objects.enqueue (actor, turn_domain_t::application, {4, {4}});
    test.require (boundary.first == stateful_error_t::none
                    && boundary.second.participants.front ().records.size () == 1
                    && boundary.second.participants.front ().records.front ().sequence == 2
                    && forwarded == std::vector<std::uint64_t>{3, 4},
                  "boundary prefix must remain immutable while late acceptance forwards once");
    test.require (objects.abort_relocation_before_cutover (sealed.seal.token).result ().value ()
                    == stateful_error_t::none,
                  "Preserve must restore accepted ingress");
    for (std::uint64_t expected : {1u, 2u, 3u, 4u}) {
        const auto [error, turn] = objects.try_claim (actor, turn_domain_t::application);
        test.require (
          error == stateful_error_t::none && turn && turn->sequence == expected,
          "Preserve must restore saved work, boundary prefix and late ingress in acceptance order");
        if (turn)
            (void) objects.complete_claim (actor, turn_domain_t::application);
    }
    test.require (objects.pending (actor, turn_domain_t::application) == 0
                    && objects.pending_bytes (actor, turn_domain_t::application) == 0,
                  "Preserve drain must release all retained accounting");
}

} // namespace m6c_test
