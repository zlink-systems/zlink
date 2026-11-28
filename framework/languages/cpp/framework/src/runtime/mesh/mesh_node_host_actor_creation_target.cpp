/* SPDX-License-Identifier: Elastic-2.0 */

#include "runtime/mesh/mesh_node_host_service_internal.hpp"

#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/execution/task_result.hpp"

#include <service_wire_codec.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <utility>

namespace zlink::framework::runtime
{
namespace terminal_codec = zlink::framework::runtime::protocol::generated::detail;

void mesh_node_host_service_detail::execute_actor_create_operation_target (
  std::shared_ptr<detail::mesh_node_runtime_t> node,
  std::shared_ptr<location_repository_t> store,
  std::shared_ptr<detail::mesh_node_builder_state_t> registration,
  const protocol::actor_create_header_t &request,
  host::actor_create_operation_target_completion_t completion)
{
    const auto failed = [request] {
        host::actor_create_operation_result_t result;
        result.reply.header = {
          request.correlation, 105u,
          static_cast<std::uint32_t> (protocol::framework_error_code::actorCreateFailed)};
        return result;
    };
    const auto status = node->status ();
    if (request.reservation.target_node_routing_id != status.routing_id ().to_bytes ()
        || request.reservation.target_node_generation != status.lifecycle_generation ()) {
        completion (failed ());
        return;
    }
    auto accepting = detail::spot_node_runtime_t (registration->spot_state)
                       .entry_accepts_actor (request.stable_type);
    detail::observe_task_completion (accepting, [request, node, store, status, completion,
                                                 failed] (const result_t<bool> &accepting) {
        if (!accepting) {
            completion (failed ());
            return;
        }
        mesh_node_descriptor_t target;
        target.mesh_name = node->mesh_name ();
        target.rid = status.routing_id ();
        target.lifecycle_generation = status.lifecycle_generation ();
        target.owner_id = request.reservation.target_owner_id;
        target.lease_generation =
          static_cast<std::int64_t> (request.reservation.target_owner_lease_generation);
        const object_reservation_fence_t fence{
          request.reservation.reservation_id,
          request.reservation.expected_store_version,
          request.reservation.object_generation,
          request.reservation.authority_owner_generation,
          {target.mesh_name,
           node_rid_t::from_string (status.routing_id ().to_string ()),
           target.lifecycle_generation,
           {target.owner_id, target.lease_generation}},
          {.actor_slots = request.reservation.pending_capacity_delta}};
        //  Actor model §6.3: without an Entry Spot accepting the type, the target ends
        //  the creation before the factory. It cancels the exact reservation, records
        //  no terminal and answers routeNotConnected, so the requester excludes this
        //  host execution generation.
        if (!accepting.value ()) {
            auto aborted =
              store->abort ({{placement_object_kind_t::actor, request.actor_id}, fence});
            detail::observe_task_completion (
              aborted, [completion, failed, correlation = request.correlation] (
                         const result_t<object_abort_result_t> &aborted) {
                  if (!aborted) {
                      completion (failed ());
                      return;
                  }
                  host::actor_create_operation_result_t result;
                  result.reply.header = {
                    correlation,
                    static_cast<std::uint32_t> (
                      terminal_codec::request_terminal_result_t::internalError),
                    static_cast<std::uint32_t> (protocol::framework_error_code::routeNotConnected)};
                  completion (std::move (result));
              });
            return;
        }
        const auto publish =
          [request, store, target, node,
           fence] (std::optional<terminal_codec::actor_create_result_t> creation_result,
                   std::optional<actor_ref_t> actor,
                   const std::optional<protocol::application_payload_t> &application_reply,
                   std::function<void (bool)> done) {
              const auto envelope = actor_terminal_envelope (
                creation_result ? terminal_codec::request_terminal_result_t::ok
                                : terminal_codec::request_terminal_result_t::internalError,
                creation_result ? terminal_codec::framework_error_code_t::none
                                : terminal_codec::framework_error_code_t::actorCreateFailed,
                creation_result, actor, application_reply);
              const auto deadline = std::chrono::system_clock::time_point (
                std::chrono::milliseconds (request.deadline_unix_ms));
              const creation_operation_identity_t operation{
                node_rid_t::from_string (
                  zlink::routing_id_t::from (request.source_node_routing_id).to_string ()),
                request.source_node_generation,
                {request.operation.high, request.operation.low}};
              const creation_terminal_publication_t terminal{operation, envelope, deadline};
              object_creation_completion_t target_completion;
              if (creation_result == terminal_codec::actor_create_result_t::created) {
                  target_completion = object_creation_completed_t{
                    target_actor_authority_payload (actor_authority_state_t::ready,
                                                    request.stable_type,
                                                    actor_id_t (request.actor_id), target),
                    terminal};
              } else if (creation_result == terminal_codec::actor_create_result_t::rejected) {
                  target_completion = object_creation_rejected_t{terminal};
              } else {
                  target_completion = object_creation_failed_t{terminal};
              }
              auto published =
                complete_actor_creation (node->native_node ().shared_from_this (), store,
                                         {{placement_object_kind_t::actor, request.actor_id},
                                          fence,
                                          std::move (target_completion)},
                                         request.stable_type);
              detail::observe_task_completion (
                published, [done = std::move (done)] (
                             const result_t<object_complete_creation_result_t> &result) {
                    done (
                      result
                      && (std::holds_alternative<object_creation_completed_result_t> (
                            result.value ())
                          || std::holds_alternative<object_creation_already_completed_result_t> (
                            result.value ())));
                });
          };
        const auto now = std::chrono::system_clock::now ();
        const auto deadline = std::chrono::system_clock::time_point (
          std::chrono::milliseconds (request.deadline_unix_ms));
        const auto finish_failed = [publish, completion, failed] {
            publish (std::nullopt, std::nullopt, std::nullopt,
                     [completion, failed] (bool) { completion (failed ()); });
        };
        if (deadline <= now) {
            finish_failed ();
            return;
        }
        const auto timeout = std::chrono::duration_cast<std::chrono::milliseconds> (deadline - now);
        const auto creation_bytes = read_actor_creation_request (store, request);
        if (!creation_bytes) {
            finish_failed ();
            return;
        }
        auto creation_request =
          std::make_shared<const zlink::message_t> (zlink::message_t::from (*creation_bytes));
        // MeshNode §5.1: admission remains held through terminal publication.
        std::shared_ptr<void> activation_admission;
        try {
            activation_admission = node->activation_admission ().enter_scoped (
              detail::activation_admission_t::actor_key (request.actor_id));
        }
        catch (const framework_exception_t &) {
            finish_failed ();
            return;
        }
        //  State lanes §blocking boundary (06:211-226): this callback runs on a completion
        //  worker, so the creation continues when its task completes instead of blocking it.
        auto creating = node->create_application_actor_async (
          request.stable_type, request.actor_id, request.reservation.object_generation,
          request.reservation.authority_owner_generation);
        detail::observe_task_completion (
          creating, [node, request, status, completion, failed, publish, activation_admission,
                     creation_request = std::move (creation_request),
                     timeout] (const result_t<actor_ref_t> &created) mutable {
              if (!created) {
                  publish (std::nullopt, std::nullopt, std::nullopt,
                           [completion, failed, activation_admission] (bool) mutable {
                               activation_admission.reset ();
                               completion (failed ());
                           });
                  return;
              }
              const auto actor = created.value ();
              auto finish_creation = [node, actor, completion, activation_admission] (
                                       host::actor_create_operation_result_t result,
                                       bool ready) mutable {
                  activation_admission.reset ();
                  if (!ready)
                      node->settle_application_actor_creation (actor, false);
                  completion (std::move (result));
                  if (ready)
                      node->settle_application_actor_creation (actor, true);
              };
              auto join_operation = std::make_shared<host::pending_operation_t> ();
              auto joined = node->submit_application_actor_entry_spot_join (
                actor, node_rid_t::from_string (status.routing_id ().to_string ()),
                *creation_request, timeout, *join_operation,
                [request, actor, finish_creation,
                 publish] (result_t<detail::actor_join_reply_t> joined) mutable {
                    host::actor_create_operation_result_t result;
                    result.reply.header = {request.correlation, 105u,
                                           static_cast<std::uint32_t> (
                                             protocol::framework_error_code::actorCreateFailed)};
                    if (!joined) {
                        publish (std::nullopt, std::nullopt, std::nullopt,
                                 [finish_creation, result = std::move (result)] (bool) mutable {
                                     finish_creation (std::move (result), false);
                                 });
                        return;
                    }
                    std::optional<protocol::application_payload_t> application_reply;
                    if (!joined.value ().reply.is_empty ()) {
                        application_reply = protocol::application_payload_t{
                          request.stable_type, "application/octet-stream",
                          joined.value ().reply.to_bytes ()};
                        result.application_reply = application_reply;
                    }
                    result.reply.header = {request.correlation, 0u, 0u};
                    const auto accepted = joined.value ().result_code == 0;
                    result.reply.result = accepted ? protocol::actor_create_result_t::created
                                                   : protocol::actor_create_result_t::rejected;
                    result.reply.node_routing_id =
                      zlink::routing_id_t::from (std::string (actor.node_rid ().value ()))
                        .to_bytes ();
                    result.reply.actor_id = std::string (actor.actor_id ().value ());
                    result.reply.object_generation = actor.object_generation ();
                    const auto creation_result =
                      accepted ? terminal_codec::actor_create_result_t::created
                               : terminal_codec::actor_create_result_t::rejected;
                    publish (creation_result, accepted ? std::make_optional (actor) : std::nullopt,
                             application_reply,
                             [request, result = std::move (result), finish_creation,
                              accepted] (bool published) mutable {
                                 if (!published) {
                                     result.reply.header = {
                                       request.correlation, 105u,
                                       static_cast<std::uint32_t> (
                                         protocol::framework_error_code::actorCreateFailed)};
                                     result.application_reply.reset ();
                                 }
                                 finish_creation (std::move (result), published && accepted);
                             });
                });
              detail::observe_task_completion (
                joined, [node, join_operation, publish, finish_creation,
                         failed] (const result_t<result_t<void>> &submitted) {
                    if (!submitted || !submitted.value ())
                        publish (std::nullopt, std::nullopt, std::nullopt,
                                 [finish_creation, failed] (bool) mutable {
                                     finish_creation (failed (), false);
                                 });
                });
          });
    });
}

} // namespace zlink::framework::runtime
