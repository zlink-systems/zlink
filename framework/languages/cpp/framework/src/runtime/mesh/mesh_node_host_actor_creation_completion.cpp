/* SPDX-License-Identifier: Elastic-2.0 */

#include "runtime/mesh/mesh_node_host_service.hpp"
#include "runtime/mesh/mesh_node_host_service_internal.hpp"
#include "runtime/mesh/mesh_node_host_actor_creation_internal.hpp"

#include "runtime/execution/task_result.hpp"
#include "runtime/locations/actor_authority_payload.hpp"
#include "runtime/locations/authority_key_codec.hpp"
#include "runtime/locations/location_runtime.hpp"
#include "runtime/locations/pending_creation_projection.hpp"
#include "runtime/mesh/user_spot_terminal_mapping.hpp"
#include "runtime/messaging/envelope_codec.hpp"
#include "runtime/messaging/request_failure_mapper.hpp"

#include <chrono>
#include <functional>
#include <utility>

namespace zlink::framework::runtime
{

using mesh_node_host_service_detail::actor_result_from_terminal;
using mesh_node_host_service_detail::actor_terminal_envelope;
using mesh_node_host_service_detail::complete_actor_creation;
using mesh_node_host_service_detail::target_actor_authority_payload;

namespace
{
namespace terminal_codec = zlink::framework::runtime::protocol::generated::detail;

/* The reservation's immutable creation intent is an Application payload
 * envelope (packet name + content type + serialized body), not the bare
 * serialized request.  .NET, Java and Node all write and read it that way, and
 * Node fences the packet name on read, so a bare body is refused by every
 * decoding target (#549). */
constexpr std::string_view framework_creation_packet_name = "ZLinkFrameworkCreationRequest";

std::optional<std::vector<std::byte>>
decode_actor_creation_intent (const std::vector<std::byte> &stored)
{
    try {
        std::vector<std::uint8_t> bytes;
        bytes.reserve (stored.size ());
        for (const auto value : stored)
            bytes.push_back (std::to_integer<std::uint8_t> (value));
        const auto application = protocol::decode_application_payload (bytes);
        if (application.packet_name () != framework_creation_packet_name)
            return std::nullopt;
        const auto body = application.payload_bytes ();
        std::vector<std::byte> result;
        result.reserve (body.size ());
        for (const auto value : body)
            result.push_back (static_cast<std::byte> (value));
        return result;
    }
    catch (...) {
        return std::nullopt;
    }
}
} // namespace

namespace mesh_node_host_service_detail
{
std::vector<std::byte> encode_actor_creation_intent (const std::string &content_type,
                                                     std::vector<std::uint8_t> body)
{
    const auto encoded = protocol::encode_application_payload (protocol::application_payload_t{
      std::string (framework_creation_packet_name),
      content_type.empty () ? std::string (messaging::envelope_codec_t::default_content_type)
                            : content_type,
      std::move (body)});
    std::vector<std::byte> result;
    result.reserve (encoded.size ());
    for (const auto value : encoded)
        result.push_back (static_cast<std::byte> (value));
    return result;
}

std::optional<std::vector<std::byte>>
read_actor_creation_request (const std::shared_ptr<location_repository_t> &store,
                             const protocol::actor_create_header_t &request)
{
    if (!store)
        return std::nullopt;
    const auto read = infrastructure_result ([&] {
                          return store->read_authority (actor_authority_key (request.actor_id));
                      }).value ();
    const auto *snapshot = std::get_if<authority_snapshot_t> (&read);
    if (!snapshot || snapshot->allocation.object_kind != placement_object_kind_t::actor
        || snapshot->allocation.state != placement_allocation_state_t::reserved
        || snapshot->allocation.stable_type != request.stable_type
        || snapshot->object_generation != request.reservation.object_generation
        || snapshot->authority_owner_generation != request.reservation.authority_owner_generation
        || snapshot->allocation.target.node_lifecycle_generation
             != request.reservation.target_node_generation
        || snapshot->allocation.target.owner.owner_id != request.reservation.target_owner_id
        || snapshot->allocation.target.owner.lease_generation
             != request.reservation.target_owner_lease_generation
        || snapshot->allocation.capacity_bundle.actor_slots
             != request.reservation.pending_capacity_delta
        || snapshot->allocation.capacity_bundle.spot_slots != 0 || !snapshot->pending_creation
        || snapshot->pending_creation->reservation_id != request.reservation.reservation_id)
        return std::nullopt;
    const auto target_rid = zlink::routing_id_t::from (request.reservation.target_node_routing_id);
    if (snapshot->allocation.target.node_rid.value () != target_rid.to_string ())
        return std::nullopt;
    const auto payload = decode_inline_creation_content (
      snapshot->pending_creation->request_content_reference,
      snapshot->pending_creation->request_sha256, snapshot->pending_creation->request_encoded_size);
    if (!payload)
        return std::nullopt;
    return decode_actor_creation_intent (*payload);
}

std::vector<std::byte>
actor_terminal_envelope (terminal_codec::request_terminal_result_t terminal_result,
                         terminal_codec::framework_error_code_t failure_code,
                         std::optional<terminal_codec::actor_create_result_t> creation_result,
                         const std::optional<actor_ref_t> &actor,
                         const std::optional<protocol::application_payload_t> &reply)
{
    terminal_codec::creation_operation_terminal_v1_t terminal{
      .terminalResult = terminal_result,
      .failureCode = failure_code,
      .hasCreation =
        creation_result ? terminal_codec::bool8_t::true_ : terminal_codec::bool8_t::false_,
      .hasApplicationPayload =
        reply ? terminal_codec::bool8_t::true_ : terminal_codec::bool8_t::false_};
    if (creation_result) {
        if ((*creation_result == terminal_codec::actor_create_result_t::existing
             || *creation_result == terminal_codec::actor_create_result_t::created)
            && !actor)
            throw std::invalid_argument ("creation terminal actor is required");
        terminal_codec::actor_create_terminal_t creation;
        creation.createResult = *creation_result;
        if (*creation_result == terminal_codec::actor_create_result_t::existing) {
            creation.tag = terminal_codec::actor_create_terminal_t::tag_t::case_0;
            creation.value.emplace<terminal_codec::actor_create_terminal_t::case_0_t> (
              terminal_codec::actor_create_terminal_t::case_0_t{
                {terminal_codec::text8_t{std::string (actor->actor_id ().value ())},
                 terminal_codec::nonzero_u64_t{actor->object_generation ()}}});
        } else if (*creation_result == terminal_codec::actor_create_result_t::created) {
            creation.tag = terminal_codec::actor_create_terminal_t::tag_t::case_1;
            creation.value.emplace<terminal_codec::actor_create_terminal_t::case_1_t> (
              terminal_codec::actor_create_terminal_t::case_1_t{
                {terminal_codec::text8_t{std::string (actor->actor_id ().value ())},
                 terminal_codec::nonzero_u64_t{actor->object_generation ()}}});
        } else {
            creation.tag = terminal_codec::actor_create_terminal_t::tag_t::case_2;
            creation.value.emplace<terminal_codec::actor_create_terminal_t::case_2_t> ();
        }
        terminal.creation = std::move (creation);
    }
    if (reply) {
        terminal.applicationPayload = terminal_codec::application_payload_envelope_v1_t{
          terminal_codec::packet_name_t{reply->packet_name ()},
          terminal_codec::content_type_t{reply->content_type ()},
          terminal_codec::application_payload_bytes_t{reply->payload_bytes ()}};
    }
    const terminal_codec::codec_context_t context{1024 * 1024, 1024 * 1024};
    terminal_codec::writer_t writer;
    if (terminal_codec::encode_value_creation_operation_terminal_v1 (writer, terminal, context)
        != terminal_codec::error_code::ok)
        throw std::invalid_argument ("creation terminal schema encoding failed");
    std::vector<std::byte> result;
    result.reserve (writer.data.size ());
    for (const auto byte : writer.data)
        result.push_back (static_cast<std::byte> (byte));
    return result;
}

/* The Location Store reservation record keeps the Application's creation
 * intent (requestContentReference/requestSha256/requestEncodedSize) and the
 * Framework's Actor authority payload in two separate slots.  Both the
 * Creating reservation and the Ready completion write the same canonical
 * authority body for the chosen placement target; only the state differs.
 * The target decodes this slot before it materializes the Actor, so the
 * Application request must never be written here (#549). */
task_t<object_complete_creation_result_t>
complete_actor_creation (std::shared_ptr<host::public_host_runtime_t> node,
                         std::shared_ptr<location_repository_t> store,
                         object_complete_creation_request_t request,
                         std::string stable_type)
{
    const auto fence = request.fence;
    const auto key = request.key;
    std::function<task_t<void> (std::optional<authority_snapshot_t>)> confirm;
    if (node && std::holds_alternative<object_creation_completed_t> (request.completion))
        confirm = co_await node->prepare_store_ready (
          {stateful::object_kind_t::actor, key.global_id, fence.object_generation,
           fence.authority_owner_generation, fence.target.mesh_name,
           std::string (fence.target.node_rid.value ())});
    auto result = co_await store->complete_creation (std::move (request));
    bool ready = false;
    if (const auto *completed = std::get_if<object_creation_completed_result_t> (&result))
        ready = completed->ready.has_value ();
    else if (const auto *already =
               std::get_if<object_creation_already_completed_result_t> (&result)) {
        const auto terminal =
          actor_result_from_terminal (already->terminal, fence.target.node_rid, stable_type,
                                      [] (zlink::message_t) { return message_t{}; });
        const auto *created = std::get_if<actor_create_created_t> (&terminal);
        ready = created && created->actor.object_generation () == fence.object_generation
                && already->ready.has_value ();
    }
    if (ready && confirm)
        co_await confirm (std::get_if<object_creation_completed_result_t> (&result)
                            ? std::get<object_creation_completed_result_t> (result).ready
                            : std::get<object_creation_already_completed_result_t> (result).ready);
    co_return result;
}

std::vector<std::byte> target_actor_authority_payload (actor_authority_state_t state,
                                                       const std::string &stable_type,
                                                       const actor_id_t &actor_id,
                                                       const mesh_node_descriptor_t &target)
{
    return encode_actor_authority_payload (actor_authority_payload_t{
      .state = state,
      .stable_type = stable_type,
      .actor_id = std::string (actor_id.value ()),
      .current_spot_id = target.entry_spot_id.value_or (target.rid.to_string ()),
      .current_spot_generation = target.lifecycle_generation,
      .current_spot_kind = actor_authority_spot_kind_t::entry,
      .owner_id = target.owner_id,
      .owner_lease_generation = static_cast<std::uint64_t> (target.lease_generation),
      .mesh_name = target.mesh_name,
      .node_rid = actor_authority_node_rid (target.rid),
      .node_generation = target.lifecycle_generation});
}
} // namespace mesh_node_host_service_detail

task_t<std::optional<actor_create_result_t>>
mesh_node_host_service_t::complete_remote_actor_creation (
  std::shared_ptr<detail::mesh_node_runtime_t> source,
  mesh_node_descriptor_t target,
  protocol::actor_create_header_t command,
  std::chrono::milliseconds timeout,
  actor_id_t actor_id,
  std::string stable_type,
  object_creation_key_t reserve_key,
  object_reservation_fence_t fence,
  creation_operation_identity_t operation)
{
    struct remote_actor_create_completion_t
    {
        foundation::operation_terminal_t terminal =
          foundation::operation_terminal_t::transport_failed;
        protocol::actor_create_reply_t reply;
        std::optional<protocol::application_payload_t> application_reply;
    };
    auto remote = std::make_shared<task_completion_source_t<remote_actor_create_completion_t>> ();
    using remote_result_t = result_t<std::optional<actor_create_result_t>>;
    std::optional<result_t<actor_create_result_t>> incomplete_result;
    try {
        const auto accepted = co_await source->native_node ().create_actor_remote (
          target.rid, std::move (command), timeout,
          [remote] (foundation::operation_terminal_t terminal, protocol::actor_create_reply_t reply,
                    std::optional<protocol::application_payload_t> application_reply) mutable {
              remote->complete (result_t<remote_actor_create_completion_t>::success (
                {terminal, std::move (reply), std::move (application_reply)}));
          });
        if (!accepted) {
            (void) co_await await_result (_location_store->abort ({reserve_key, fence}));
            incomplete_result = result_t<actor_create_result_t>::failure (
              framework_error_kind_t::rejected, "Actor creation operation was not admitted");
        } else {
            const auto completed_remote = co_await remote->task ();
            //  Spec 32-framework-error-model:87-120 — classify the remote creation
            //  outcome instead of collapsing every failure to InternalFailure.
            if (completed_remote.terminal != foundation::operation_terminal_t::completed) {
                //  Non-completed local operation terminals classify exactly like
                //  the sibling User Spot lifecycle path (timed_out ->
                //  DeadlineExceeded, transport_failed -> Unavailable, cancelled ->
                //  InvalidOperation, shutdown -> ShuttingDown).
                incomplete_result = result_t<actor_create_result_t>::failure (
                  user_spot_terminal::map_user_spot_operation_failure (completed_remote.terminal,
                                                                       {}, true),
                  "Remote Actor creation transport did not complete");
            } else if (completed_remote.reply.header.terminal_result != 0
                       && completed_remote.reply.header.failure_code
                            == static_cast<std::uint32_t> (
                              protocol::framework_error_code::routeNotConnected)) {
                //  Actor model §6.3: the target has no Entry Spot accepting the type; it ended
                //  the creation before the factory and cancelled the reservation.
                co_return remote_result_t::success (std::nullopt);
            } else if (completed_remote.reply.header.terminal_result != 0) {
                //  A carried wire terminal + fine failure code classifies via the
                //  shared ownership-aware remote-reply mapper (spec 32:83-118,
                //  99-108) — e.g. Busy+None -> Unavailable and workerTimedOut ->
                //  DeadlineExceeded.
                incomplete_result = result_t<actor_create_result_t>::failure (
                  messaging::request_failure_mapper_t{}
                    .reply_header_exception (completed_remote.reply.header.terminal_result,
                                             completed_remote.reply.header.failure_code,
                                             "Remote Actor creation")
                    .kind (),
                  "Remote Actor creation target rejected the operation");
            } else if (completed_remote.reply.result == protocol::actor_create_result_t::rejected) {
                std::optional<message_t> rejected_reply;
                if (completed_remote.application_reply)
                    rejected_reply = message_t::from_raw (
                      zlink::message_t::from (completed_remote.application_reply->payload_bytes ()),
                      _serializers);
                co_return remote_result_t::success (
                  actor_create_rejected_t{std::move (rejected_reply)});
            } else if (completed_remote.reply.result == protocol::actor_create_result_t::existing) {
                if (completed_remote.reply.actor_id == actor_id.value ()
                    && completed_remote.reply.node_routing_id == target.rid.to_bytes ()) {
                    co_return remote_result_t::success (
                      actor_create_existing_t{::zlink::framework::detail::actor_ref_access_t::make (
                        node_rid_t::from_string (target.rid.to_string ()), stable_type,
                        completed_remote.reply.actor_id,
                        completed_remote.reply.object_generation)});
                }
                incomplete_result = result_t<actor_create_result_t>::failure (
                  framework_error_kind_t::protocol_error,
                  "Remote Actor creation returned a malformed existing reply");
            } else if (completed_remote.reply.result != protocol::actor_create_result_t::created
                       || completed_remote.reply.actor_id != actor_id.value ()
                       || completed_remote.reply.node_routing_id != target.rid.to_bytes ()
                       || completed_remote.reply.object_generation != fence.object_generation) {
                //  Spec 32:91-92 — a reply that cannot be processed (unknown
                //  result discriminator or fields disagreeing with the request)
                //  is a ProtocolError, not InternalFailure.
                incomplete_result = result_t<actor_create_result_t>::failure (
                  framework_error_kind_t::protocol_error,
                  "Remote Actor creation returned a malformed reply");
            } else {
                const auto created = ::zlink::framework::detail::actor_ref_access_t::make (
                  node_rid_t::from_string (target.rid.to_string ()), stable_type,
                  completed_remote.reply.actor_id, completed_remote.reply.object_generation);
                std::optional<message_t> reply;
                if (completed_remote.application_reply)
                    reply = message_t::from_raw (
                      zlink::message_t::from (completed_remote.application_reply->payload_bytes ()),
                      _serializers);
                co_return remote_result_t::success (
                  actor_create_created_t{created, std::move (reply)});
            }
        }
    }
    catch (const framework_exception_t &error) {
        incomplete_result = detail::result_access_t::failure<actor_create_result_t> (error);
    }
    catch (const std::exception &error) {
        incomplete_result = result_t<actor_create_result_t>::failure (
          framework_error_kind_t::internal_failure, error.what ());
    }
    if (const auto terminal = co_await _location_store->read_creation_terminal (operation))
        co_return remote_result_t::success (actor_result_from_terminal (
          *terminal, node_rid_t::from_string (target.rid.to_string ()), stable_type,
          [this] (zlink::message_t raw) {
              return message_t::from_raw (std::move (raw), _serializers);
          }));
    if (!incomplete_result)
        co_return remote_result_t::failure (
          framework_error_kind_t::internal_failure,
          "Remote Actor creation did not produce a completed result");
    co_return detail::propagate_failure<std::optional<actor_create_result_t>> (
      *incomplete_result, "Remote Actor creation failed");
}

/* The local Actor's entry Spot join completes through its operation
 * completion; the creation terminal is published from there, not from a
 * caller that waits for the join. */
task_t<actor_create_result_t> mesh_node_host_service_t::complete_local_actor_creation (
  task_t<zlink::framework::detail::actor_join_reply_t> joining,
  mesh_node_descriptor_t target,
  actor_ref_t created,
  actor_id_t actor_id,
  std::string stable_type,
  object_creation_key_t reserve_key,
  object_reservation_fence_t fence,
  creation_operation_identity_t operation,
  std::chrono::system_clock::time_point operation_deadline)
{
    result_t<zlink::framework::detail::actor_join_reply_t> joined =
      result_t<zlink::framework::detail::actor_join_reply_t>::failure (
        framework_error_kind_t::internal_failure, "Actor creation callback failed");
    try {
        joined = result_t<zlink::framework::detail::actor_join_reply_t>::success (
          co_await std::move (joining));
    }
    catch (...) {
        joined = detail::current_exception_result<zlink::framework::detail::actor_join_reply_t> ();
    }
    if (!joined) {
        const auto failed_envelope =
          actor_terminal_envelope (terminal_codec::request_terminal_result_t::internalError,
                                   terminal_codec::framework_error_code_t::actorCreateFailed,
                                   std::nullopt, std::nullopt, std::nullopt);
        const creation_terminal_publication_t failed_publication{operation, failed_envelope,
                                                                 operation_deadline};
        (void) co_await await_result (_location_store->complete_creation (
          {reserve_key, fence, object_creation_failed_t{failed_publication}}));
        co_return detail::propagate_failure<actor_create_result_t> (
          joined, "Actor creation callback failed");
    }
    const bool accepted = joined.value ().result_code == 0;
    std::optional<message_t> reply;
    if (!joined.value ().reply.is_empty ())
        reply = message_t::from_raw (joined.value ().reply, _serializers);
    std::optional<protocol::application_payload_t> terminal_reply;
    if (!joined.value ().reply.is_empty ())
        terminal_reply = protocol::application_payload_t{stable_type, "application/octet-stream",
                                                         joined.value ().reply.to_bytes ()};
    const auto envelope = actor_terminal_envelope (
      terminal_codec::request_terminal_result_t::ok, terminal_codec::framework_error_code_t::none,
      accepted ? terminal_codec::actor_create_result_t::created
               : terminal_codec::actor_create_result_t::rejected,
      accepted ? std::make_optional (created) : std::nullopt, terminal_reply);
    const creation_terminal_publication_t publication{operation, envelope, operation_deadline};
    object_creation_completion_t completion;
    if (accepted)
        completion = object_creation_completed_t{
          target_actor_authority_payload (actor_authority_state_t::ready, stable_type, actor_id,
                                          target),
          publication};
    else
        completion = object_creation_rejected_t{publication};
    std::shared_ptr<host::public_host_runtime_t> owner;
    for (const auto &local : _nodes)
        if (target.rid == local->native_node ().status ().routing_id ())
            owner = local->native_node ().shared_from_this ();
    const auto completed = co_await await_result (
      complete_actor_creation (std::move (owner), _location_store,
                               {reserve_key, fence, std::move (completion)}, stable_type));
    if (!completed)
        co_return detail::propagate_failure<actor_create_result_t> (
          completed, "Actor creation completion failed");
    if (const auto *done = std::get_if<object_creation_completed_result_t> (&completed.value ())) {
        if (!done->ready || done->ready->allocation.state != placement_allocation_state_t::active)
            co_return (result_t<actor_create_result_t>::failure (
              framework_error_kind_t::unavailable, "Actor creation did not reach active state"));
    } else if (const auto *already =
                 std::get_if<object_creation_already_completed_result_t> (&completed.value ())) {
        co_return (result_t<actor_create_result_t>::success (actor_result_from_terminal (
          already->terminal, node_rid_t::from_string (target.rid.to_string ()), stable_type,
          [this] (zlink::message_t raw) {
              return message_t::from_raw (std::move (raw), _serializers);
          })));
    } else {
        co_return (result_t<actor_create_result_t>::failure (
          framework_error_kind_t::unavailable, "Actor creation completion was fenced"));
    }
    if (accepted)
        co_return (result_t<actor_create_result_t>::success (
          actor_create_created_t{created, std::move (reply)}));
    co_return (
      result_t<actor_create_result_t>::success (actor_create_rejected_t{std::move (reply)}));
}

} // namespace zlink::framework::runtime
