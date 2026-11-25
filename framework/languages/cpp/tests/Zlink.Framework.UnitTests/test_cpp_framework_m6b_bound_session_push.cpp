/* SPDX-License-Identifier: Elastic-2.0 */

#include "runtime/backend/raw_binding_adapter.hpp"
#include "../support/m6b_cases.hpp"
#include "../support/m6b_mesh_fixture.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include "runtime/stateful/public_host_runtime.hpp"
#include "runtime/stateful/stateful_object_runtime.hpp"
#include "runtime/stateful/stream_session_registry.hpp"
#include <zlink/Contracts/Core/routing_id.hpp>
#include <zlink/Contracts/Messaging/message.hpp>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mesh = zlink::framework::runtime::mesh;
namespace protocol = zlink::framework::runtime::protocol;
namespace stateful = zlink::framework::runtime::stateful;
namespace host = zlink::framework::runtime::host;

namespace zlink::framework::test::m6b
{
void verify_bound_session_push_uses_session_registry_when_gateway_projection_rejects ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    auto session_owner = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = descriptor ("projection-session-owner"),
        .runtime_failures =
          std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()}});
    const auto source_node = zlink::routing_id_t::from ("projection-actor-owner");
    const stateful::object_ref_t previous{
      stateful::object_kind_t::actor, "projection-actor", 7, 11, "m6b-mesh", "projection-old-node"};
    const auto connection = session_owner->sessions ().open ("projection-session");
    const auto [bind_error, binding] =
      session_owner->sessions ().bind_remote (connection, previous, 13, 17);
    assert (bind_error == stateful::stateful_error_t::none);
    const auto sealed = session_owner->sessions ().seal_remote_route (
      connection.connection_id, binding.binding_generation, previous.key,
      previous.object_generation, binding.owner_lease_generation);
    assert (sealed.error == stateful::stateful_error_t::none);
    auto target = previous;
    target.node_id = source_node.to_string ();
    ++target.authority_owner_generation;
    const auto committed = session_owner->sessions ().commit_remote_route (
      connection.connection_id, binding.binding_generation, previous.key,
      previous.object_generation, target, 19, 23);
    assert (committed.error == stateful::stateful_error_t::none);
    assert (committed.binding && committed.binding->owner_lease_generation == 23);

    auto gateway_state = std::make_shared<actor_gateway_state_t> ();
    actor_gateway_runtime_t gateway (gateway_state);
    auto jobs = std::make_shared<runtime::application_job_queue_t> (
      runtime::application_job_queue_configuration_t{application_job_queue_profile_t::balanced,
                                                     std::uint32_t{1}, 1, 1});
    std::atomic_int deliveries{0};
    const auto previous_actor = actor_ref_access_t::make (
      node_rid_t::from_string (previous.node_id), {}, previous.key, previous.object_generation);
    assert (gateway.bind_session_sink (
      previous_actor, [&deliveries] (std::string, stream_codec_t, const zlink::message_t &) {
          deliveries.fetch_add (1, std::memory_order_acq_rel);
          return task_t<void> (result_t<void>::success ());
      }));
    assert (gateway.record_bound_session_route (
      previous_actor, session_owner->status ().routing_id (),
      zlink::routing_id_t::from (connection.connection_id),
      session_owner->status ().lifecycle_generation (), previous.authority_owner_generation, 17,
      binding.binding_generation, 0, 0));

    session_owner->configure_bound_session_operations (host::bound_session_operations_t{
      .bind =
        [] (const protocol::bound_session_bind_t &bind, const zlink::routing_id_t &, std::uint64_t,
            const zlink::framework::location_owner_token_t &,
            std::function<bool (std::uint32_t, std::uint32_t,
                                std::optional<protocol::stream_bind_reply_tail_t>)>
              submit_terminal_reply) {
            (void) submit_terminal_reply (
              static_cast<std::uint32_t> (protocol::request_terminal_result::ok),
              static_cast<std::uint32_t> (protocol::framework_error_code::none),
              protocol::stream_bind_reply_tail_t{bind.binding.generation,
                                                 bind.actor.authority_owner_generation});
            using bind_result_t = host::bound_session_bind_operation_result_t;
            return zlink::framework::task_t<bind_result_t> (
              zlink::framework::result_t<bind_result_t>::success (bind_result_t{}));
        },
      .send = [] (const protocol::bound_session_send_t &,
                  std::vector<zlink::message_t>) { return stateful::stateful_error_t::conflict; },
      .replaced = [] (const protocol::bound_session_replaced_t &) {},
      .commit_relocation_route = [] (const protocol::session_relocation_route_t &,
                                     const stateful::stream_binding_t &,
                                     const stateful::stream_binding_t &) { return false; },
      .prepare_relocation_target_route = [] (const protocol::session_relocation_route_t &,
                                             std::uint64_t) { return true; },
      .capture_send = [gateway, jobs] (const protocol::bound_session_send_t &send) mutable
      -> std::optional<host::bound_session_operations_t::delivery_capability_t> {
          assert (jobs->snapshot ().permits_in_use == 0);
          const auto actor = actor_ref_access_t::make (
            node_rid_t::from_string (
              zlink::routing_id_t::from (send.actor.target_node_routing_id).to_string ()),
            {}, send.actor.actor_id, send.actor.object_generation);
          auto admitted =
            gateway.admit_bound_session_delivery (actor, send.expected_binding_generation);
          if (!admitted)
              return std::nullopt;
          return [admitted = std::move (*admitted)] (std::vector<zlink::message_t> parts) mutable {
              if (parts.size () != 1)
                  return task_t<stateful::stateful_error_t> (
                    result_t<stateful::stateful_error_t>::success (
                      stateful::stateful_error_t::invalid));
              auto delivered =
                admitted ("ProjectionPush", stream_codec_t::message_pack, parts.front ());
              return project_task_terminal<stateful::stateful_error_t> (
                delivered, [] (const result_t<void> &terminal) {
                    return result_t<stateful::stateful_error_t>::success (
                      terminal ? stateful::stateful_error_t::none
                               : stateful::stateful_error_t::conflict);
                });
          };
      }});

    const protocol::bound_session_send_t send{
      protocol::actor_route_fence_t{target.key, target.object_generation, source_node.to_bytes (),
                                    19, target.authority_owner_generation, 23},
      binding.binding_generation};
    const protocol::application_payload_t application{
      std::string (protocol::framework_multipart_packet_name),
      std::string (protocol::framework_multipart_content_type),
      {0, 0, 0, 1, 0, 0, 0, 2, 0x7b, 0x7d}};
    auto queued = mesh::service_mailbox_record_t::from_parts (
      "bound-session:" + target.key, mesh::service_mailbox_domain_t::application,
      zlink::framework::detail::backend::materialize_binding_parts (
        {protocol::encode_bound_session_send (send),
         protocol::encode_application_payload (application)}),
      source_node.to_bytes (), std::nullopt, 19);
    auto permit = jobs->try_reserve_supply ();
    assert (permit);
    queued.application_permit.emplace (std::move (*permit));
    queued.application_permit->mark_queued ();
    assert (jobs->snapshot ().queued_application_jobs == 1);
    assert (session_owner->transport ().mailbox ().try_enqueue (std::move (queued)));
    const auto noop_dispatch = [] (const host::ready_record_t &, const host::receive_record_t &,
                                   std::vector<zlink::message_t>, auto) {};
    assert (session_owner->dispatch_ready (noop_dispatch).result ().value () == 1);
    assert (jobs->snapshot ().permits_in_use == 0);
    const auto current = session_owner->sessions ().current_binding (target.key);
    const auto lagging_projection =
      gateway.bound_session_route_async (previous_actor).result ().value ();
    assert (deliveries.load (std::memory_order_acquire) == 1);
    // Session-Actor binding §8.1: a push never rewrites the Session-owned route.
    assert (current && current->owner_lease_generation == 23);
    assert (lagging_projection
            && lagging_projection->authority_owner_generation == previous.authority_owner_generation
            && lagging_projection->owner_lease_generation == 17);
}

// Session-Actor binding §3 item 3 and §8.1: the Session owner admits a push by
// SessionRid, binding generation, ActorId and ObjectGeneration only. Owner
// lifecycle fields carried by the record (authority owner generation, owner
// lease) neither reject nor hold the push.
void verify_bound_session_push_admission_uses_only_session_owned_fields ()
{
    auto session_owner = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = descriptor ("fw06-session-owner"),
        .runtime_failures =
          std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()}});
    const auto actor_node = zlink::routing_id_t::from ("fw06-actor-owner");
    const stateful::object_ref_t actor{
      stateful::object_kind_t::actor, "fw06-actor", 7, 11, "m6b-mesh", actor_node.to_string ()};
    const auto connection = session_owner->sessions ().open ("fw06-session");
    const auto [bind_error, binding] =
      session_owner->sessions ().bind_remote (connection, actor, 13, 17);
    assert (bind_error == stateful::stateful_error_t::none);

    std::atomic_int deliveries{0};
    session_owner->configure_bound_session_operations (host::bound_session_operations_t{
      .bind =
        [] (const protocol::bound_session_bind_t &bind, const zlink::routing_id_t &, std::uint64_t,
            const zlink::framework::location_owner_token_t &,
            std::function<bool (std::uint32_t, std::uint32_t,
                                std::optional<protocol::stream_bind_reply_tail_t>)>
              submit_terminal_reply) {
            (void) submit_terminal_reply (
              static_cast<std::uint32_t> (protocol::request_terminal_result::ok),
              static_cast<std::uint32_t> (protocol::framework_error_code::none),
              protocol::stream_bind_reply_tail_t{bind.binding.generation,
                                                 bind.actor.authority_owner_generation});
            using bind_result_t = host::bound_session_bind_operation_result_t;
            return zlink::framework::task_t<bind_result_t> (
              zlink::framework::result_t<bind_result_t>::success (bind_result_t{}));
        },
      .send =
        [&deliveries] (const protocol::bound_session_send_t &, std::vector<zlink::message_t>) {
            deliveries.fetch_add (1, std::memory_order_acq_rel);
            return stateful::stateful_error_t::none;
        },
      .replaced = [] (const protocol::bound_session_replaced_t &) {},
      .commit_relocation_route = [] (const protocol::session_relocation_route_t &,
                                     const stateful::stream_binding_t &,
                                     const stateful::stream_binding_t &) { return false; },
      .prepare_relocation_target_route = [] (const protocol::session_relocation_route_t &,
                                             std::uint64_t) { return true; }});

    const protocol::application_payload_t application{
      std::string (protocol::framework_multipart_packet_name),
      std::string (protocol::framework_multipart_content_type),
      {0, 0, 0, 1, 0, 0, 0, 2, 0x7b, 0x7d}};
    const auto noop_dispatch = [] (const host::ready_record_t &, const host::receive_record_t &,
                                   std::vector<zlink::message_t>, auto) {};
    const auto push = [&] (std::uint64_t authority_owner_generation,
                           std::uint64_t owner_lease_generation, std::uint64_t binding_generation) {
        const protocol::bound_session_send_t send{
          protocol::actor_route_fence_t{actor.key, actor.object_generation, actor_node.to_bytes (),
                                        13, authority_owner_generation, owner_lease_generation},
          binding_generation};
        assert (session_owner->transport ().mailbox ().try_enqueue (
          mesh::service_mailbox_record_t::from_parts (
            "bound-session:" + actor.key, mesh::service_mailbox_domain_t::application,
            zlink::framework::detail::backend::materialize_binding_parts (
              {protocol::encode_bound_session_send (send),
               protocol::encode_application_payload (application)}),
            actor_node.to_bytes (), std::nullopt, 13)));
        (void) session_owner->dispatch_ready (noop_dispatch).result ();
    };

    // Owner lifecycle fields changed after the binding was made.
    push (actor.authority_owner_generation + 1, 31, binding.binding_generation);
    assert (deliveries.load (std::memory_order_acquire) == 1);
    // A source whose owner fence copies are older than the Session binding.
    push (actor.authority_owner_generation - 1, 5, binding.binding_generation);
    assert (deliveries.load (std::memory_order_acquire) == 2);
    // The binding generation is Session-owned: a stale one is rejected.
    push (actor.authority_owner_generation, 17, binding.binding_generation + 1);
    assert (deliveries.load (std::memory_order_acquire) == 2);
}

// Session-Actor binding §3 item 3: the push source sends by SessionRid and
// binding generation and does not pre-judge the push with its own copies.
// The Actor owner node is checked once, at the transport boundary, which
// refuses the frame; the host no longer answers `not_found` for it.
void verify_bound_session_push_source_does_not_prejudge_current_binding ()
{
    using namespace zlink::framework;

    auto actor_owner = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{
        .descriptor = descriptor ("fw06-push-source"),
        .runtime_failures =
          std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()}});
    const auto other_node =
      node_rid_t::from_string (zlink::routing_id_t::from ("fw06-other-node").to_string ());
    const auto actor =
      detail::actor_ref_access_t::make (other_node, "player", "fw06-source-actor", 7);
    const auto submitted =
      actor_owner
        ->send_bound_session (actor, zlink::routing_id_t::from ("fw06-session-owner"), 3, 11, 13,
                              {zlink::message_t::from (std::string ("{}"))})
        .result ();
    assert (!submitted || submitted.value () != zlink::submit_result_t::not_found);
}
}
