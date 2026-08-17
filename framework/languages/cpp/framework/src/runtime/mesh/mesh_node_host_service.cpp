/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/mesh/mesh_node_host_service.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/actors/actor_manager_access.hpp"
#include "runtime/locations/sha256.hpp"
#include "runtime/locations/actor_authority_payload.hpp"
#include "runtime/locations/authority_key_codec.hpp"

#include "runtime/channels/route_handler_registry.hpp"
#include "runtime/channels/channel_reply_writer.hpp"
#include "runtime/diagnostics/dispatch_error_reporter.hpp"
#include "runtime/mesh/mesh_record_dispatcher.hpp"
#include "runtime/mesh/mesh_metadata_codec.hpp"
#include "runtime/mesh/user_spot_terminal_mapping.hpp"
#include "runtime/messaging/envelope_codec.hpp"
#include "runtime/messaging/request_failure_mapper.hpp"
#include "runtime/locations/pending_creation_projection.hpp"
#include "runtime/locations/location_runtime.hpp"
#include "runtime/locations/spot_address_resolvers.hpp"
#include "runtime/locations/sha256.hpp"
#include "runtime/locations/source_creation_cleanup.hpp"
#include "runtime/spots/spot_route_packets.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>

namespace zlink::framework::runtime
{

struct mesh_node_host_service_t::actor_destroy_callback_gate_t
{
    std::mutex mutex;
    std::condition_variable changed;
    bool stopping = false;
    std::size_t active = 0;

    bool try_enter ()
    {
        std::lock_guard lock (mutex);
        if (stopping)
            return false;
        ++active;
        return true;
    }

    void leave ()
    {
        std::lock_guard lock (mutex);
        --active;
        if (active == 0)
            changed.notify_all ();
    }

    void stop_and_wait () noexcept
    {
        std::unique_lock lock (mutex);
        stopping = true;
        changed.wait (lock, [this] { return active == 0; });
    }
};

application_dispatch_terminal_owner_t::application_dispatch_terminal_owner_t (
  std::shared_ptr<detail::mesh_node_runtime_t> node,
  std::function<void ()> complete_stateful_dispatch,
  std::function<void ()> release_mailbox_reservation) :
    _node (std::move (node)),
    _complete_stateful_dispatch (std::move (complete_stateful_dispatch)),
    _release_mailbox_reservation (std::move (release_mailbox_reservation))
{
}

application_dispatch_terminal_owner_t::~application_dispatch_terminal_owner_t () noexcept
{
    settle ();
}

void application_dispatch_terminal_owner_t::settle () noexcept
{
    if (_settled.exchange (true, std::memory_order_acq_rel))
        return;

    invoke (_complete_stateful_dispatch);
    invoke (_release_mailbox_reservation);
    if (const auto node = _node.lock ())
        node->application_work_finished ();

    _complete_stateful_dispatch = {};
    _release_mailbox_reservation = {};
    _node.reset ();
}

void application_dispatch_terminal_owner_t::invoke (
  const std::function<void ()> &callback) noexcept
{
    if (!callback)
        return;
    try {
        callback ();
    }
    catch (...) {
    }
}

namespace
{
std::atomic_uint64_t next_user_spot_operation{1};

class terminal_callback_guard_t final
{
  public:
    explicit terminal_callback_guard_t (std::function<void ()> release) :
        _release (std::move (release))
    {
    }

    ~terminal_callback_guard_t () noexcept
    {
        if (_release) {
            try {
                _release ();
            }
            catch (...) {
            }
        }
    }

    terminal_callback_guard_t (const terminal_callback_guard_t &) = delete;
    terminal_callback_guard_t &operator= (const terminal_callback_guard_t &) = delete;

    void dismiss () noexcept { _release = {}; }

  private:
    std::function<void ()> _release;
};

std::optional<std::vector<std::byte>> read_actor_creation_request (
  const std::shared_ptr<location_repository_t> &store,
  const protocol::actor_create_header_t &request)
{
    if (!store)
        return std::nullopt;
    const auto read = store
      ->read_authority (actor_authority_key (request.actor_id))
      .result ()
      .value ();
    const auto *snapshot = std::get_if<authority_snapshot_t> (&read);
    if (!snapshot
        || snapshot->allocation.object_kind
             != placement_object_kind_t::actor
        || snapshot->allocation.state
             != placement_allocation_state_t::reserved
        || snapshot->allocation.stable_type != request.stable_type
        || snapshot->object_generation
             != request.reservation.object_generation
        || snapshot->authority_owner_generation
             != request.reservation.authority_owner_generation
        || snapshot->allocation.target.node_lifecycle_generation
             != request.reservation.target_node_generation
        || snapshot->allocation.target.owner.owner_id
             != request.reservation.target_owner_id
        || snapshot->allocation.target.owner.lease_generation
             != request.reservation.target_owner_lease_generation
        || snapshot->allocation.capacity_bundle.actor_slots
             != request.reservation.pending_capacity_delta
        || snapshot->allocation.capacity_bundle.spot_slots != 0
        || !snapshot->pending_creation
        || snapshot->pending_creation->reservation_id
             != request.reservation.reservation_id)
        return std::nullopt;
    const auto target_rid = zlink::routing_id_t::from (
      request.reservation.target_node_routing_id);
    if (snapshot->allocation.target.node_rid.value ()
          != target_rid.to_string ())
        return std::nullopt;
    const auto payload = decode_inline_creation_content (
      snapshot->pending_creation->request_content_reference);
    if (!payload
        || snapshot->pending_creation->request_encoded_size
             != payload->size ()
        || snapshot->pending_creation->request_sha256 != sha256 (*payload))
        return std::nullopt;
    return payload;
}

bool capacity_available (const capacity_usage_t &usage)
{
    return usage.limit == 0
           || usage.active + usage.reserved
                < static_cast<std::uint64_t> (usage.limit);
}

bool placement_capacity_available (
  const mesh_node_descriptor_t &descriptor,
  placement_object_kind_t kind,
  const std::string &stable_type)
{
    if (kind == placement_object_kind_t::actor)
        return capacity_available (
          descriptor.capacity.actors);
    if (!capacity_available (
          descriptor.capacity.spots))
        return false;
    const auto typed = std::find_if (
      descriptor.capacity.spot_types.begin (),
      descriptor.capacity.spot_types.end (),
      [&] (const spot_type_capacity_t &candidate) {
          return candidate.object_kind == kind
                 && candidate.stable_type == stable_type;
      });
    return typed
             == descriptor.capacity.spot_types.end ()
           || capacity_available (typed->usage);
}

std::vector<std::byte> actor_terminal_envelope (
  creation_terminal_state_t state,
  const std::optional<actor_ref_t> &actor,
  const std::optional<message_t> &reply,
  serializer_registry_t &serializers)
{
    static constexpr std::string_view magic =
      "creation-operation-terminal-v1";
    std::vector<std::byte> result;
    auto append_u32 = [&] (std::uint32_t value) {
        for (int shift = 24; shift >= 0; shift -= 8)
            result.push_back (
              static_cast<std::byte> ((value >> shift) & 0xffu));
    };
    auto append_u64 = [&] (std::uint64_t value) {
        for (int shift = 56; shift >= 0; shift -= 8)
            result.push_back (
              static_cast<std::byte> ((value >> shift) & 0xffu));
    };
    auto append_text = [&] (std::string_view value) {
        append_u32 (static_cast<std::uint32_t> (value.size ()));
        for (const auto byte : value)
            result.push_back (
              static_cast<std::byte> (
                static_cast<unsigned char> (byte)));
    };
    append_text (magic);
    append_u32 (static_cast<std::uint32_t> (state));
    append_u32 (state == creation_terminal_state_t::failed ? 1u : 0u);
    append_text (actor ? actor->node_rid ().value () : std::string_view{});
    append_text (actor
                   ? ::zlink::framework::detail::actor_ref_access_t::actor_type (*actor)
                   : std::string_view{});
    append_text (actor ? actor->actor_id ().value () : std::string_view{});
    append_u64 (actor ? actor->object_generation () : 0);
    if (reply) {
        const auto raw = detail::message_to_raw (*reply, serializers);
        const auto bytes = raw.bytes ();
        append_u32 (static_cast<std::uint32_t> (bytes.size ()));
        result.insert (result.end (), bytes.begin (), bytes.end ());
    }
    else {
        append_u32 (0);
    }
    return result;
}

actor_create_result_t actor_result_from_terminal (
  const creation_terminal_record_t &terminal,
  auto &&wrap_message)
{
    const auto &bytes = terminal.terminal_envelope;
    std::size_t offset = 0;
    auto read_u32 = [&] {
        if (offset + 4 > bytes.size ())
            throw std::invalid_argument (
              "creation terminal envelope is truncated");
        std::uint32_t value = 0;
        for (int index = 0; index < 4; ++index)
            value = (value << 8)
                    | std::to_integer<std::uint8_t> (
                      bytes[offset++]);
        return value;
    };
    auto read_u64 = [&] {
        if (offset + 8 > bytes.size ())
            throw std::invalid_argument (
              "creation terminal envelope is truncated");
        std::uint64_t value = 0;
        for (int index = 0; index < 8; ++index)
            value = (value << 8)
                    | std::to_integer<std::uint8_t> (
                      bytes[offset++]);
        return value;
    };
    auto read_text = [&] {
        const auto size = read_u32 ();
        if (offset + size > bytes.size ())
            throw std::invalid_argument (
              "creation terminal envelope is truncated");
        std::string value;
        value.reserve (size);
        for (std::uint32_t index = 0; index < size; ++index)
            value.push_back (
              static_cast<char> (
                std::to_integer<unsigned char> (
                  bytes[offset++])));
        return value;
    };
    if (read_text () != "creation-operation-terminal-v1")
        throw std::invalid_argument (
          "creation terminal envelope version is invalid");
    const auto state =
      static_cast<creation_terminal_state_t> (read_u32 ());
    (void) read_u32 ();
    const auto node = read_text ();
    const auto type = read_text ();
    const auto id = read_text ();
    const auto generation = read_u64 ();
    const auto reply_size = read_u32 ();
    if (offset + reply_size != bytes.size ())
        throw std::invalid_argument (
          "creation terminal envelope has trailing data");
    std::optional<message_t> reply;
    if (reply_size != 0) {
        std::vector<std::uint8_t> raw;
        raw.reserve (reply_size);
        for (std::uint32_t index = 0; index < reply_size; ++index)
            raw.push_back (
              std::to_integer<std::uint8_t> (
                bytes[offset++]));
        reply = wrap_message (zlink::message_t::from (raw));
    }
    if (state == creation_terminal_state_t::created) {
        return actor_create_created_t{
          ::zlink::framework::detail::actor_ref_access_t::make (
            node_rid_t::from_string (node), type, id, generation),
          std::move (reply)};
    }
    if (state == creation_terminal_state_t::rejected)
        return actor_create_rejected_t{std::move (reply)};
    throw framework_exception_t (
      framework_error_kind_t::internal_failure,
      "Actor creation operation previously failed");
}

} // namespace

task_t<actor_create_result_t>
mesh_node_host_service_t::complete_remote_actor_creation (
  std::shared_ptr<detail::mesh_node_runtime_t> source,
  mesh_node_descriptor_t target,
  protocol::actor_create_header_t command,
  std::chrono::milliseconds timeout,
  actor_id_t actor_id,
  std::string stable_type,
  object_creation_key_t reserve_key,
  object_reservation_fence_t fence,
  creation_operation_identity_t operation,
  std::chrono::system_clock::time_point operation_deadline)
{
    struct remote_actor_create_completion_t
    {
        foundation::operation_terminal_t terminal =
          foundation::operation_terminal_t::transport_failed;
        protocol::actor_create_reply_t reply;
        std::optional<protocol::application_payload_t> application_reply;
    };
    auto remote = std::make_shared<
      detail::task_completion_source_t<remote_actor_create_completion_t>> ();
    const auto fail_creation = [&] {
        const auto failed_envelope = actor_terminal_envelope (
          creation_terminal_state_t::failed,
          std::nullopt, std::nullopt, *_serializers);
        return _location_store
          ->complete_creation (
            {reserve_key, fence,
             object_creation_failed_t{{operation, failed_envelope,
                                       sha256 (failed_envelope),
                                       operation_deadline}}})
          .result ();
    };
    try {
        const auto accepted = co_await source->native_node ().create_actor_remote (
          target.rid, std::move (command), timeout,
          [remote] (foundation::operation_terminal_t terminal,
                    protocol::actor_create_reply_t reply,
                    std::optional<protocol::application_payload_t>
                      application_reply) mutable {
              remote->complete (
                result_t<remote_actor_create_completion_t>::success (
                  {terminal, std::move (reply),
                   std::move (application_reply)}));
          });
        if (!accepted) {
            (void) fail_creation ();
            co_return result_t<actor_create_result_t>::failure (
              framework_error_kind_t::rejected,
              "Actor creation operation was not admitted");
        }

        const auto completed_remote = co_await remote->task ();
        //  Spec 32-framework-error-model:87-120 — classify the remote creation
        //  outcome instead of collapsing every failure to InternalFailure.
        if (completed_remote.terminal
            != foundation::operation_terminal_t::completed) {
            (void) fail_creation ();
            //  Non-completed local operation terminals classify exactly like
            //  the sibling User Spot lifecycle path (timed_out ->
            //  DeadlineExceeded, transport_failed -> Unavailable, cancelled ->
            //  InvalidOperation, shutdown -> ShuttingDown).
            co_return result_t<actor_create_result_t>::failure (
              user_spot_terminal::map_user_spot_operation_failure (
                completed_remote.terminal, {}, true),
              "Remote Actor creation transport did not complete");
        }
        if (completed_remote.reply.header.terminal_result != 0) {
            (void) fail_creation ();
            //  A carried wire terminal + fine failure code classifies via the
            //  shared ownership-aware remote-reply mapper (spec 32:83-118,
            //  99-108) — e.g. Busy+None -> Unavailable, Backpressured+None ->
            //  CapacityExceeded, workerTimedOut -> DeadlineExceeded.
            co_return result_t<actor_create_result_t>::failure (
              messaging::request_failure_mapper_t{}
                .reply_header_exception (
                  completed_remote.reply.header.terminal_result,
                  completed_remote.reply.header.failure_code,
                  "Remote Actor creation")
                .kind (),
              "Remote Actor creation target rejected the operation");
        }
        if (completed_remote.reply.result
            == protocol::actor_create_result_t::rejected) {
            //  Spec 32:129-136 — an Application rejection is a typed
            //  Actor-create result, not a framework failure. Persist the
            //  rejected terminal exactly like the local creation path.
            std::optional<message_t> rejected_reply;
            if (completed_remote.application_reply)
                rejected_reply = message_t::from_raw (
                  zlink::message_t::from (
                    completed_remote.application_reply->payload),
                  _serializers);
            const auto rejected_envelope = actor_terminal_envelope (
              creation_terminal_state_t::rejected,
              std::nullopt,
              rejected_reply,
              *_serializers);
            const auto rejected_completed =
              _location_store
                ->complete_creation (
                  {reserve_key, fence,
                   object_creation_rejected_t{
                     {operation, rejected_envelope,
                      sha256 (rejected_envelope), operation_deadline}}})
                .result ();
            if (!rejected_completed)
                co_return result_t<actor_create_result_t>::failure (
                  rejected_completed.error_kind (),
                  rejected_completed.error ()
                    ? rejected_completed.error ()->what ()
                    : "Actor creation rejection completion failed");
            co_return result_t<actor_create_result_t>::success (
              actor_create_rejected_t{std::move (rejected_reply)});
        }
        if (completed_remote.reply.result
            == protocol::actor_create_result_t::existing) {
            //  A peer reporting an existing Actor is a typed result carrying
            //  the existing ref; the local reservation is stale, so release it.
            (void) fail_creation ();
            co_return result_t<actor_create_result_t>::success (
              actor_create_existing_t{
                ::zlink::framework::detail::actor_ref_access_t::make (
                  node_rid_t::from_string (target.rid.to_string ()),
                  stable_type,
                  std::string (actor_id.value ()),
                  completed_remote.reply.object_generation)});
        }
        if (completed_remote.reply.result
              != protocol::actor_create_result_t::created
            || completed_remote.reply.actor_id != actor_id.value ()
            || completed_remote.reply.node_routing_id
                 != target.rid.to_bytes ()
            || completed_remote.reply.object_generation
                 != fence.object_generation) {
            (void) fail_creation ();
            //  Spec 32:91-92 — a reply that cannot be processed (unknown
            //  result discriminator or fields disagreeing with the request)
            //  is a ProtocolError, not InternalFailure.
            co_return result_t<actor_create_result_t>::failure (
              framework_error_kind_t::protocol_error,
              "Remote Actor creation returned a malformed reply");
        }

        const auto created =
          ::zlink::framework::detail::actor_ref_access_t::make (
            node_rid_t::from_string (target.rid.to_string ()),
            stable_type,
            std::string (actor_id.value ()),
            completed_remote.reply.object_generation);
        std::optional<message_t> reply;
        if (completed_remote.application_reply)
            reply = message_t::from_raw (
              zlink::message_t::from (
                completed_remote.application_reply->payload),
              _serializers);
        const auto envelope = actor_terminal_envelope (
          creation_terminal_state_t::created,
          created,
          reply,
          *_serializers);
        const creation_terminal_publication_t publication{
          operation,
          envelope,
          sha256 (envelope),
          operation_deadline};
        const auto completed = _location_store
          ->complete_creation (
            {reserve_key,
             fence,
             object_creation_completed_t{
               encode_actor_authority_payload (
                 created,
                 target.entry_spot_id.value_or (
                   target.rid.to_string ()),
                 target.lifecycle_generation),
               publication}})
          .result ();
        if (!completed)
            co_return result_t<actor_create_result_t>::failure (
              completed.error_kind (),
              completed.error ()
                ? completed.error ()->what ()
                : "Actor creation completion failed");
        if (const auto *done = std::get_if<
              object_creation_completed_result_t> (&completed.value ())) {
            if (!done->ready
                || done->ready->allocation.state
                     != placement_allocation_state_t::active)
                co_return result_t<actor_create_result_t>::failure (
                  framework_error_kind_t::unavailable,
                  "Actor creation did not reach active state");
        }
        else if (const auto *already = std::get_if<
                   object_creation_already_completed_result_t> (
                   &completed.value ())) {
            co_return result_t<actor_create_result_t>::success (
              actor_result_from_terminal (
                already->terminal,
                [this] (zlink::message_t raw) {
                    return message_t::from_raw (
                      std::move (raw), _serializers);
                }));
        }
        else {
            co_return result_t<actor_create_result_t>::failure (
              framework_error_kind_t::unavailable,
              "Actor creation completion was fenced");
        }
        co_return result_t<actor_create_result_t>::success (
          actor_create_created_t{created, std::move (reply)});
    }
    catch (const framework_exception_t &error) {
        (void) fail_creation ();
        co_return detail::result_access_t::failure<actor_create_result_t> (
          error);
    }
    catch (const std::exception &error) {
        (void) fail_creation ();
        co_return result_t<actor_create_result_t>::failure (
          framework_error_kind_t::internal_failure, error.what ());
    }
}

namespace
{

void trace_mesh_host_stop (const char *stage)
{
    const char *value = std::getenv ("ZLINK_CPP_HOST_STOP_TRACE");
    if (value != nullptr && std::string_view (value) != "" && std::string_view (value) != "0")
        std::cerr << "zlink-cpp-host-stop stage=mesh-host-" << stage << std::endl;
}

void trace_mesh_application (std::string_view stage,
                             const host::receive_record_t &record,
                             std::size_t parts,
                             std::string_view detail = {})
{
    const char *value = std::getenv ("ZLINK_CPP_MESH_TRACE");
    if (value == nullptr || std::string_view (value) == ""
        || std::string_view (value) == "0")
        return;
    std::cerr << "zlink mesh-host stage=" << stage
              << " kind=" << static_cast<int> (record.kind)
              << " source=" << record.source_node_rid.to_string ()
              << " parts=" << parts;
    if (!detail.empty ())
        std::cerr << " detail=" << detail;
    std::cerr << '\n';
}

void reject_application_request (
  const host::receive_record_t &record,
  std::vector<zlink::message_t> parts,
  framework_error_kind_t error_kind,
  std::string message)
{
    const bool request = record.kind == host::record_kind_t::node_request
                         || record.kind == host::record_kind_t::channel_request
                         || record.kind == host::record_kind_t::spot_request
                         || record.kind == host::record_kind_t::actor_request;
    if (!request)
        return;
    messaging::message_parts_t encoded (std::move (parts));
    messaging::envelope_codec_t codec;
    const auto header = codec.decode_header (encoded);
    if (!header)
        return;
    detail::channel_reply_writer_t replies;
    auto reply = replies.reply_raw_envelope (
      replies.create_error_header (
        header.value ().channel_name, header.value (),
        framework_exception_t (error_kind, std::move (message))),
      zlink::message_t::from (""));
    (void) host::reply (record.reply_token, reply.items ());
}

handler_registry_t &empty_handler_filters ()
{
    static handler_registry_t filters;
    return filters;
}

} // namespace

mesh_node_host_service_t::mesh_node_host_service_t (
  std::vector<std::shared_ptr<detail::mesh_node_builder_state_t>> registrations,
  serializer_registry_t &serializers,
  dispatch_options_t dispatch_options,
  std::shared_ptr<listener_status_registry_t> listener_statuses,
  std::shared_ptr<application_job_queue_t> application_jobs) :
    mesh_node_host_service_t (
      std::move (registrations), serializers, empty_handler_filters (),
      std::move (dispatch_options), std::move (listener_statuses),
      std::move (application_jobs))
{
}

mesh_node_host_service_t::mesh_node_host_service_t (
  std::vector<std::shared_ptr<detail::mesh_node_builder_state_t>> registrations,
  serializer_registry_t &serializers,
  handler_registry_t &filters,
  dispatch_options_t dispatch_options,
  std::shared_ptr<listener_status_registry_t> listener_statuses,
  std::shared_ptr<application_job_queue_t> application_jobs) :
    _registrations (std::move (registrations)),
    _serializers (&serializers),
    _filters (&filters),
    _dispatch_options (std::move (dispatch_options)),
    _application_dispatch (std::make_unique<offload_executor_t> (
      0, std::max<std::size_t> (2, std::thread::hardware_concurrency ()), 4096,
      std::chrono::milliseconds (100), "zlink-mesh-app")),
    _application_jobs (
      application_jobs
        ? std::move (application_jobs)
        : std::make_shared<application_job_queue_t> (
            application_job_queue_configuration_t{
              application_job_queue_profile_t::balanced,
              std::nullopt, 1,
              static_cast<std::uint32_t> (
                std::numeric_limits<std::int32_t>::max ())})),
    _listener_statuses (std::move (listener_statuses))
{
    _nodes.reserve (_registrations.size ());
    for (const auto &registration : _registrations) {
        auto node = std::make_shared<detail::mesh_node_runtime_t> (registration);
        node->bind_serializers (serializers);
        _nodes.push_back (std::move (node));
    }
}

mesh_node_host_service_t::~mesh_node_host_service_t ()
{
    stop ();
}

actor_manager_t mesh_node_host_service_t::actor_manager ()
{
    if (_nodes.empty ())
        return actor_manager_t{};
    return detail::actor_manager_access_t::create (
      [this] (bool exclusive, actor_id_t actor_id,
              std::string stable_type,
              std::optional<std::string> mesh_name,
              std::optional<message_t> request,
              std::chrono::milliseconds timeout,
              creation_operation_id_t operation_id) {
          const auto source_node =
            mesh_name
              ? std::find_if (
                  _nodes.begin (), _nodes.end (),
                  [&] (const auto &node) {
                      return node
                             && node->mesh_name ()
                                  == *mesh_name;
                  })
              : _nodes.begin ();
          if (source_node == _nodes.end ())
              return task_t<actor_create_result_t> (
                result_t<actor_create_result_t>::failure (
                  framework_error_kind_t::not_found,
                  "The selected Actor Mesh is not registered"));
          const auto source = (*source_node)->status ();
          return create_actor (
            exclusive, std::move (actor_id),
            std::move (stable_type), std::move (mesh_name),
            std::move (request), timeout,
            creation_operation_identity_t{
              node_rid_t::from_string (
                source.routing_id ().to_string ()),
              source.lifecycle_generation (),
              operation_id});
      },
      [this] (actor_id_t actor_id) {
          return find_actor (std::move (actor_id));
      },
      [this] (actor_id_t actor_id) {
          return find_actor_spot (std::move (actor_id));
      },
      [this] (actor_ref_t actor) {
          return destroy_actor (std::move (actor));
      });
}

task_t<actor_create_result_t>
mesh_node_host_service_t::create_actor (
  bool exclusive,
  actor_id_t actor_id,
  std::string stable_type,
  std::optional<std::string> mesh_name,
  std::optional<message_t> request,
  std::chrono::milliseconds timeout,
  creation_operation_identity_t operation)
{
    if (!_location_store || actor_id.value ().empty ()
        || stable_type.empty ())
        return task_t<actor_create_result_t> (
          result_t<actor_create_result_t>::failure (
            framework_error_kind_t::not_configured,
            "Actor creation requires Location Store, ActorId and stable type"));
    if (_nodes.empty ())
        return task_t<actor_create_result_t> (
          result_t<actor_create_result_t>::failure (
            framework_error_kind_t::not_configured,
            "No object Client or Server Mesh is registered"));
    const auto deadline =
      std::chrono::steady_clock::now () + timeout;
    const auto operation_deadline =
      std::chrono::system_clock::now () + timeout;
    if (const auto terminal =
          _location_store
            ->read_creation_terminal (operation)
            .result ()
            .value ())
        return task_t<actor_create_result_t> (
          result_t<actor_create_result_t>::success (
            actor_result_from_terminal (
              *terminal,
              [this] (zlink::message_t raw) {
                  return message_t::from_raw (
                    std::move (raw), _serializers);
              })));

    std::vector<mesh_node_descriptor_t> candidates;
    const auto selected_mesh = mesh_name.value_or (_nodes.front ()->mesh_name ());
    location_page_request_t page;
    do {
        auto listed = _location_store
                        ->list_mesh_nodes (selected_mesh, page)
                        .result ()
                        .value ();
        for (auto &descriptor : listed.items) {
            if (mesh_name && descriptor.mesh_name != *mesh_name)
                continue;
            const auto capability = std::find_if (
              descriptor.object_capabilities.begin (),
              descriptor.object_capabilities.end (),
              [&] (const object_capability_t &value) {
                  return value.object_kind
                           == placement_object_kind_t::actor
                         && value.stable_type == stable_type;
              });
            if (descriptor.state == framework_runtime_state_t::serving
                && descriptor.object_role == object_role_t::server
                && descriptor.placement_weight > 0
                && placement_capacity_available (
                     descriptor, placement_object_kind_t::actor,
                     stable_type)
                && capability
                     != descriptor.object_capabilities.end ())
                candidates.push_back (std::move (descriptor));
        }
        page.continuation_token = std::move (listed.continuation_token);
    } while (page.continuation_token);
    if (candidates.empty ())
    {
        return task_t<actor_create_result_t> (
          result_t<actor_create_result_t>::failure (
            mesh_name ? framework_error_kind_t::not_found
                      : framework_error_kind_t::capacity_exceeded,
            "No eligible Actor target is ready"));
    }
    const auto choose_target = [&] {
        const auto total_weight = std::accumulate (
          candidates.begin (), candidates.end (), std::uint64_t{0},
          [] (std::uint64_t total, const auto &candidate) {
              return total + static_cast<std::uint64_t> (
                candidate.placement_weight);
          });
        auto choice =
          std::hash<std::string>{} (std::string (actor_id.value ())) % total_weight;
        auto selected = candidates.front ();
        for (const auto &candidate : candidates) {
            const auto weight = static_cast<std::uint64_t> (
              candidate.placement_weight);
            if (choice < weight) {
                selected = candidate;
                break;
            }
            choice -= weight;
        }
        return selected;
    };
    auto target = choose_target ();
    const auto find_target_runtime = [&] {
        return std::find_if (
          _nodes.begin (), _nodes.end (),
          [&] (const auto &node) {
              const auto rid = node->routing_id ();
              return node->mesh_name () == target.mesh_name
                     && rid
                     && rid->to_hex () == target.rid.to_hex ();
          });
    };
    auto target_runtime = find_target_runtime ();
    const auto source_runtime = std::find_if (
      _nodes.begin (), _nodes.end (),
      [&] (const auto &node) {
          const auto rid = node->routing_id ();
          return rid
                 && rid->to_string () == operation.source_node_rid.value ();
      });

    std::vector<std::byte> request_bytes;
    if (request) {
        const auto raw = detail::message_to_raw (*request, *_serializers);
        const auto bytes = raw.bytes ();
        request_bytes.assign (bytes.begin (), bytes.end ());
    }
    object_reserve_request_t reserve{
      .key = {placement_object_kind_t::actor, std::string (actor_id.value ())},
      .intent =
        {.stable_type = stable_type,
         .request_content_reference =
           encode_inline_creation_content (request_bytes),
         .request_sha256 = sha256 (request_bytes),
         .request_encoded_size = request_bytes.size ()},
      .target =
        {.mesh_name = target.mesh_name,
         .node_rid = node_rid_t::from_string (
           target.rid.to_string ()),
         .node_lifecycle_generation =
           target.lifecycle_generation,
         .owner = {target.owner_id,
                   target.lease_generation}},
      .creating_payload = request_bytes,
      .capacity_bundle = {.actor_slots = 1}};
    while (std::chrono::steady_clock::now () < deadline) {
        const auto reserved =
          _location_store->reserve (reserve)
            .result ()
            .value ();
        if (const auto *existing =
              std::get_if<object_already_exists_t> (
                &reserved)) {
            if (exclusive)
                return task_t<actor_create_result_t> (
                  result_t<actor_create_result_t>::failure (
                    framework_error_kind_t::already_exists,
                    "Actor already exists"));
            const auto existing_ref = ::zlink::framework::detail::actor_ref_access_t::make (
              existing->current.allocation.target.node_rid,
              existing->current.allocation.stable_type,
              std::string (actor_id.value ()),
              existing->current.object_generation);
            return task_t<actor_create_result_t> (
              result_t<actor_create_result_t>::success (
                actor_create_existing_t{existing_ref}));
        }
        if (std::holds_alternative<object_type_mismatch_t> (
              reserved))
            return task_t<actor_create_result_t> (
              result_t<actor_create_result_t>::failure (
                framework_error_kind_t::type_mismatch,
                "Actor stable type does not match"));
        const auto *reserve_conflict =
          std::get_if<object_reserve_conflict_t> (&reserved);
        const bool target_unavailable =
          reserve_conflict
          && std::holds_alternative<authority_missing_t> (
            reserve_conflict->current);
        if (std::holds_alternative<object_placement_capacity_exhausted_t> (
              reserved)
            || target_unavailable) {
            candidates.erase (
              std::remove_if (
                candidates.begin (), candidates.end (),
                [&] (const auto &candidate) {
                    return candidate.mesh_name == target.mesh_name
                           && candidate.rid == target.rid
                           && candidate.lifecycle_generation
                                == target.lifecycle_generation;
                }),
              candidates.end ());
            if (candidates.empty ())
                return task_t<actor_create_result_t> (
                  result_t<actor_create_result_t>::failure (
                    framework_error_kind_t::capacity_exceeded,
                    "Actor placement candidates were exhausted"));
            target = choose_target ();
            target_runtime = find_target_runtime ();
            reserve.target = {
              .mesh_name = target.mesh_name,
              .node_rid = node_rid_t::from_string (
                target.rid.to_string ()),
              .node_lifecycle_generation =
                target.lifecycle_generation,
              .owner = {target.owner_id,
                        target.lease_generation}};
            continue;
        }
        if (const auto *winner =
              std::get_if<object_reserved_t> (&reserved)) {
            const auto fail_creation = [&] {
                const auto failed_envelope = actor_terminal_envelope (
                  creation_terminal_state_t::failed,
                  std::nullopt, std::nullopt, *_serializers);
                const creation_terminal_publication_t failed_publication{
                  operation, failed_envelope, sha256 (failed_envelope),
                  operation_deadline};
                return _location_store
                  ->complete_creation (
                    {reserve.key, winner->fence,
                     object_creation_failed_t{failed_publication}})
                  .result ();
            };
            if (target_runtime == _nodes.end ()) {
                if (source_runtime == _nodes.end ()) {
                    (void) fail_creation ();
                    return task_t<actor_create_result_t> (
                      result_t<actor_create_result_t>::failure (
                        framework_error_kind_t::unavailable,
                        "Actor creation source MeshNode is not available"));
                }
                const auto source_status = (*source_runtime)->native_node ().status ();
                protocol::actor_create_header_t command;
                command.operation = {
                  operation.operation_id.high,
                  operation.operation_id.low};
                command.source_node_routing_id =
                  source_status.routing_id ().to_bytes ();
                command.source_node_generation =
                  source_status.lifecycle_generation ();
                command.actor_id = std::string (actor_id.value ());
                command.stable_type = stable_type;
                command.reservation = {
                  winner->fence.reservation_id,
                  winner->fence.expected_store_version,
                  winner->fence.object_generation,
                  winner->fence.authority_owner_generation,
                  target.rid.to_bytes (),
                  target.lifecycle_generation,
                  winner->fence.target.owner.owner_id,
                  static_cast<std::uint64_t> (
                    winner->fence.target.owner.lease_generation),
                  static_cast<std::uint32_t> (
                    winner->fence.capacity_bundle.actor_slots)};
                command.deadline_unix_ms = static_cast<std::uint64_t> (
                  std::chrono::duration_cast<std::chrono::milliseconds> (
                    operation_deadline.time_since_epoch ())
                    .count ());
                return complete_remote_actor_creation (
                  *source_runtime,
                  target,
                  std::move (command),
                  timeout,
                  std::move (actor_id),
                  std::move (stable_type),
                  reserve.key,
                  winner->fence,
                  operation,
                  operation_deadline);
            }
            std::optional<zlink::message_t> raw_request;
            if (request)
                raw_request = detail::message_to_raw (
                  *request, *_serializers);
            const auto created =
              (*target_runtime)->create_application_actor (
                stable_type, std::string (actor_id.value ()), raw_request,
                winner->fence.object_generation,
                winner->fence.authority_owner_generation,
                timeout);
            if (!created) {
                const auto failed_envelope =
                  actor_terminal_envelope (
                    creation_terminal_state_t::failed,
                    std::nullopt, std::nullopt,
                    *_serializers);
                const creation_terminal_publication_t
                  failed_publication{
                    operation, failed_envelope,
                    sha256 (failed_envelope),
                    operation_deadline};
                (void) _location_store
                  ->complete_creation (
                    {reserve.key, winner->fence,
                     object_creation_failed_t{
                       failed_publication}})
                  .result ();
                return task_t<actor_create_result_t> (
                  result_t<actor_create_result_t>::failure (
                    created.error_kind (),
                    created.error ()
                      ? created.error ()->what ()
                      : "Actor factory failed"));
            }
            const auto joined =
              (*target_runtime)
                ->join_application_actor_to_entry_spot (
                  created.value (),
                  node_rid_t::from_string (
                    target.rid.to_string ()),
                  raw_request.value_or (zlink::message_t{}),
                  timeout);
            if (!joined) {
                const auto failed_envelope =
                  actor_terminal_envelope (
                    creation_terminal_state_t::failed,
                    std::nullopt, std::nullopt,
                    *_serializers);
                const creation_terminal_publication_t
                  failed_publication{
                    operation, failed_envelope,
                    sha256 (failed_envelope),
                    operation_deadline};
                (void) _location_store
                  ->complete_creation (
                    {reserve.key, winner->fence,
                     object_creation_failed_t{
                       failed_publication}})
                  .result ();
                return task_t<actor_create_result_t> (
                  result_t<actor_create_result_t>::failure (
                    joined.error_kind (),
                    joined.error ()
                      ? joined.error ()->what ()
                      : "Actor creation callback failed"));
            }
            const bool accepted =
              joined.value ().result_code == 0;
            std::optional<message_t> reply;
            if (!joined.value ().reply.is_empty ())
                reply = message_t::from_raw (
                  joined.value ().reply, _serializers);
            const auto state =
              accepted ? creation_terminal_state_t::created
                       : creation_terminal_state_t::rejected;
            const auto envelope = actor_terminal_envelope (
              state,
              accepted
                ? std::make_optional (created.value ())
                : std::nullopt,
              reply, *_serializers);
            const creation_terminal_publication_t publication{
              operation, envelope, sha256 (envelope),
              operation_deadline};
            object_creation_completion_t completion;
            if (accepted)
                completion =
                  object_creation_completed_t{
                    encode_actor_authority_payload (
                      created.value (),
                      target.entry_spot_id.value_or (
                        target.rid.to_string ()),
                      target.lifecycle_generation),
                    publication};
            else
                completion =
                  object_creation_rejected_t{publication};
            const auto completed =
              _location_store
                ->complete_creation (
                  {reserve.key, winner->fence,
                   std::move (completion)})
                .result ();
            if (!completed)
                return task_t<actor_create_result_t> (
                  result_t<actor_create_result_t>::failure (
                    completed.error_kind (),
                    completed.error ()
                      ? completed.error ()->what ()
                      : "Actor creation completion failed"));
            if (const auto *done = std::get_if<
                  object_creation_completed_result_t> (&completed.value ())) {
                if (!done->ready
                    || done->ready->allocation.state
                         != placement_allocation_state_t::active)
                    return task_t<actor_create_result_t> (
                      result_t<actor_create_result_t>::failure (
                        framework_error_kind_t::unavailable,
                        "Actor creation did not reach active state"));
            } else if (const auto *already = std::get_if<
                         object_creation_already_completed_result_t> (
                         &completed.value ())) {
                return task_t<actor_create_result_t> (
                  result_t<actor_create_result_t>::success (
                    actor_result_from_terminal (
                      already->terminal,
                      [this] (zlink::message_t raw) {
                          return message_t::from_raw (
                            std::move (raw), _serializers);
                      })));
            } else {
                return task_t<actor_create_result_t> (
                  result_t<actor_create_result_t>::failure (
                    framework_error_kind_t::unavailable,
                    "Actor creation completion was fenced"));
            }
            if (accepted)
                return task_t<actor_create_result_t> (
                  result_t<actor_create_result_t>::success (
                    actor_create_created_t{created.value (), std::move (reply)}));
            return task_t<actor_create_result_t> (
              result_t<actor_create_result_t>::success (
                actor_create_rejected_t{std::move (reply)}));
        }
        if (const auto terminal =
              _location_store
                ->read_creation_terminal (operation)
                .result ()
                .value ())
            return task_t<actor_create_result_t> (
              result_t<actor_create_result_t>::success (
                actor_result_from_terminal (
                  *terminal,
                  [this] (zlink::message_t raw) {
                      return message_t::from_raw (
                        std::move (raw), _serializers);
                  })));
        std::this_thread::sleep_for (
          std::chrono::milliseconds (1));
    }
    return task_t<actor_create_result_t> (
      result_t<actor_create_result_t>::failure (
        framework_error_kind_t::deadline_exceeded,
        "Actor creation deadline elapsed"));
}

task_t<std::optional<actor_ref_t>>
mesh_node_host_service_t::find_actor (
  actor_id_t actor_id)
{
    if (!_location_store)
        return task_t<std::optional<actor_ref_t>> (
          result_t<std::optional<actor_ref_t>>::success (
            std::nullopt));
    const auto current =
      _location_store
        ->read_authority (actor_authority_key (actor_id.value ()))
        .result ()
        .value ();
    const auto *snapshot =
      std::get_if<authority_snapshot_t> (&current);
    if (!snapshot
        || snapshot->allocation.state
             != placement_allocation_state_t::active)
        return task_t<std::optional<actor_ref_t>> (
          result_t<std::optional<actor_ref_t>>::success (
            std::nullopt));
    const auto projection = decode_actor_authority_payload (snapshot->payload);
    if (!projection) {
        return task_t<std::optional<actor_ref_t>> (
          result_t<std::optional<actor_ref_t>>::success (std::nullopt));
    }
    return task_t<std::optional<actor_ref_t>> (
      result_t<std::optional<actor_ref_t>>::success (
        ::zlink::framework::detail::actor_ref_access_t::make (
          snapshot->allocation.target.node_rid,
          snapshot->allocation.stable_type,
          std::string (actor_id.value ()), snapshot->object_generation)));
}

task_t<std::optional<spot_ref_t>>
mesh_node_host_service_t::find_actor_spot (
  actor_id_t actor_id)
{
    if (!_location_store)
        return task_t<std::optional<spot_ref_t>> (
          result_t<std::optional<spot_ref_t>>::success (
            std::nullopt));
    const auto read = _location_store
      ->read_authority (actor_authority_key (actor_id.value ()))
      .result ()
      .value ();
    const auto *snapshot = std::get_if<authority_snapshot_t> (&read);
    if (snapshot
        && snapshot->allocation.object_kind == placement_object_kind_t::actor
        && snapshot->allocation.state == placement_allocation_state_t::active) {
        const auto projection = decode_actor_authority_payload (snapshot->payload);
        if (projection && projection->actor.actor_id ().value () == actor_id.value ())
            return task_t<std::optional<spot_ref_t>> (
              result_t<std::optional<spot_ref_t>>::success (
                spot_ref_t{projection->spot_id, projection->spot_generation,
                           snapshot->allocation.target.mesh_name,
                           snapshot->allocation.target.node_rid}));
    }
    return task_t<std::optional<spot_ref_t>> (
      result_t<std::optional<spot_ref_t>>::success (
        std::nullopt));
}

result_t<void> mesh_node_host_service_t::finalize_local_actor_destroy (
  const actor_ref_t &actor)
{
    if (::zlink::framework::detail::actor_ref_access_t::empty (actor)) {
        return result_t<void>::failure (
          framework_error_kind_t::invalid_operation,
          "Actor destroy requires an exact ActorRef");
    }

    const auto node = std::find_if (
      _nodes.begin (), _nodes.end (), [&actor] (const auto &candidate) {
          const auto routing_id = candidate ? candidate->routing_id () : std::nullopt;
          return routing_id
                 && routing_id->to_string () == actor.node_rid ().value ();
      });
    if (node != _nodes.end ()) {
        const auto cleaned = (*node)->cleanup_application_actor_stateful (actor);
        if (!cleaned)
            return cleaned;
    }

    std::optional<authority_snapshot_t> removed_snapshot;
    if (_location_store) {
        const auto key = actor_authority_key (actor.actor_id ().value ());
        const auto current = _location_store->read_authority (key).result ();
        if (!current) {
            return detail::propagate_failure<void> (
              current, "Actor destroy authority read failed");
        }
        if (const auto *snapshot =
              std::get_if<authority_snapshot_t> (&current.value ())) {
            const bool exact =
              snapshot->allocation.object_kind
                == placement_object_kind_t::actor
              && snapshot->allocation.stable_type == ::zlink::framework::detail::actor_ref_access_t::actor_type (actor)
              && snapshot->object_generation == actor.object_generation ()
              && snapshot->allocation.target.node_rid.value ()
                   == actor.node_rid ().value ();
            if (exact
                && snapshot->allocation.state
                     == placement_allocation_state_t::active) {
                const auto removed = _location_store
                  ->compare_exchange_authority (
                    key, snapshot->store_version,
                    authority_delete_t{})
                  .result ();
                if (!removed) {
                    return detail::propagate_failure<void> (
                      removed, "Actor destroy authority delete failed");
                }
                if (std::holds_alternative<authority_deleted_t> (
                      removed.value ())) {
                    removed_snapshot = *snapshot;
                } else {
                    return result_t<void>::failure (
                      framework_error_kind_t::unavailable,
                      "Actor destroy authority changed before deletion");
                }
            }
        }
    }

    if (removed_snapshot && _services) {
        if (auto actor_resolver =
              _services->get<actor_address_resolver_t> ()) {
            const auto &snapshot = *removed_snapshot;
            const auto expected = spot_address_t{
              {},
              zlink::routing_id_t::from (
                std::string (snapshot.allocation.target.node_rid.value ())),
              {},
              0,
              {},
              snapshot.object_generation,
              snapshot.authority_owner_generation,
              location_owner_token_t{
                snapshot.allocation.target.owner.owner_id,
                snapshot.allocation.target.owner.lease_generation},
              snapshot.allocation.target.node_lifecycle_generation};
            (void) actor_resolver->get ().invalidate_actor_address_if_matches (
              actor.actor_id ().value (), expected);
        }
    }

    if (_services) {
        if (auto gateway =
              _services->get<detail::actor_gateway_runtime_t> ()) {
            const auto cleaned = gateway->get ().destroy_actor (actor);
            if (!cleaned) {
                return cleaned;
            }
        }
    }
    return result_t<void>::success ();
}

task_t<bool> mesh_node_host_service_t::destroy_actor (
  actor_ref_t actor)
{
    try {
        if (!_location_store || ::zlink::framework::detail::actor_ref_access_t::empty (actor)) {
            return task_t<bool> (result_t<bool>::failure (
              framework_error_kind_t::invalid_operation,
              "Actor destroy requires a Location Store and an exact ActorRef"));
        }

        const auto key = actor_authority_key (actor.actor_id ().value ());
        const auto read = _location_store->read_authority (key).result ().value ();
        const auto *snapshot = std::get_if<authority_snapshot_t> (&read);
        if (!snapshot) {
            return task_t<bool> (result_t<bool>::success (false));
        }
        if (snapshot->allocation.object_kind != placement_object_kind_t::actor
            || snapshot->allocation.stable_type != ::zlink::framework::detail::actor_ref_access_t::actor_type (actor)
            || snapshot->object_generation != actor.object_generation ()
            || snapshot->allocation.target.node_rid.value ()
                 != actor.node_rid ().value ()) {
            return task_t<bool> (result_t<bool>::failure (
              framework_error_kind_t::invalid_operation,
              "ActorRef does not match the current actor incarnation"));
        }
        if (snapshot->allocation.state != placement_allocation_state_t::active) {
            return task_t<bool> (result_t<bool>::failure (
              framework_error_kind_t::unavailable,
              "Actor creation or transfer is in progress"));
        }

        const auto node = std::find_if (
          _nodes.begin (), _nodes.end (), [&actor] (const auto &candidate) {
              const auto routing_id = candidate ? candidate->routing_id () : std::nullopt;
              return routing_id
                     && routing_id->to_string () == actor.node_rid ().value ();
          });
        if (node != _nodes.end ()) {
            if ((*node)->application_actor_transfer_in_progress (actor)) {
                return task_t<bool> (result_t<bool>::failure (
                  framework_error_kind_t::unavailable,
                  "Actor transfer is in progress"));
            }

            /* The local Spot owns the first destructive step. Its cleanup
             * callback runs after local state is detached and only then
             * removes Location authority. This keeps a failed local cleanup
             * from leaving a deleted authority with retained capacity. */
            const auto local_destroyed =
              (*node)->destroy_application_actor (actor);
            if (!local_destroyed) {
                return task_t<bool> (result_t<bool>::failure (
                  local_destroyed.error_kind (),
                  local_destroyed.error ()
                    ? local_destroyed.error ()->what ()
                    : "Actor runtime cleanup failed"));
            }
            /* The Spot destroy callback owns the authority and gateway
             * finalization for a local Actor. Keep this outer operation as
             * the result boundary so the finalizer runs exactly once. */
            return task_t<bool> (result_t<bool>::success (true));
        }

        const auto removed = _location_store
          ->compare_exchange_authority (
            key, snapshot->store_version, authority_delete_t{})
          .result ()
          .value ();
        if (!std::holds_alternative<authority_deleted_t> (removed)) {
            if (const auto *conflict =
                  std::get_if<authority_conflict_t> (&removed)) {
                if (std::holds_alternative<authority_missing_t> (
                      conflict->current)) {
                    return task_t<bool> (result_t<bool>::success (false));
                }
                const auto &current =
                  std::get<authority_snapshot_t> (conflict->current);
                if (current.object_generation != actor.object_generation ()) {
                    return task_t<bool> (result_t<bool>::failure (
                      framework_error_kind_t::invalid_operation,
                      "Actor generation changed while destroy was pending"));
                }
            }
            return task_t<bool> (result_t<bool>::failure (
              framework_error_kind_t::unavailable,
              "Actor authority changed while destroy was pending"));
        }
        /* A destroy removes the current authority before the next same-Id
         * incarnation can be created. Invalidate the resolver entry that was
         * derived from that authority so a new incarnation cannot reuse the
         * previous node route when its generation is reused. */
        if (_services) {
            if (auto actor_resolver =
                  _services->get<actor_address_resolver_t> ()) {
                const auto expected = spot_address_t{
                  {},
                  zlink::routing_id_t::from (
                    std::string (snapshot->allocation.target.node_rid.value ())),
                  {},
                  0,
                  {},
                  snapshot->object_generation,
                  snapshot->authority_owner_generation,
                  location_owner_token_t{
                    snapshot->allocation.target.owner.owner_id,
                    snapshot->allocation.target.owner.lease_generation},
                  snapshot->allocation.target.node_lifecycle_generation};
                (void) actor_resolver->get ().invalidate_actor_address_if_matches (
                  actor.actor_id ().value (), expected);
            }
        }

        const auto finalized = finalize_local_actor_destroy (actor);
        if (!finalized) {
            return task_t<bool> (detail::result_access_t::failure<bool> (
              framework_exception_t (
                finalized.error_kind (),
                finalized.error () ? finalized.error ()->what ()
                                   : "Actor destroy cleanup failed")));
        }
        return task_t<bool> (result_t<bool>::success (true));
    }
    catch (const framework_exception_t &error) {
        return task_t<bool> (detail::result_access_t::failure<bool> (error));
    }
    catch (const std::exception &error) {
        return task_t<bool> (result_t<bool>::failure (
          framework_error_kind_t::internal_failure, error.what ()));
    }
}

task_t<spot_create_result_t>
mesh_node_host_service_t::create_user_spot (
  const std::shared_ptr<detail::mesh_node_runtime_t> &,
  bool exclusive,
  std::optional<spot_id_t> spot_id,
  std::string stable_type,
  std::optional<std::string> mesh_name,
  std::optional<message_t> request,
  std::chrono::milliseconds timeout)
{
    const auto deadline =
      std::chrono::steady_clock::now () + timeout;
    if (!_location_store || stable_type.empty ())
        return task_t<spot_create_result_t> (
          result_t<spot_create_result_t>::failure (
            framework_error_kind_t::not_configured,
            "User Spot creation requires a Location Store and stable type"));
    std::shared_ptr<detail::mesh_node_runtime_t> source;
    if (mesh_name) {
        const auto found = std::find_if (
          _nodes.begin (), _nodes.end (),
          [&] (const auto &node) {
              return node && node->mesh_name () == *mesh_name;
          });
        if (found == _nodes.end ())
            return task_t<spot_create_result_t> (
              result_t<spot_create_result_t>::failure (
                framework_error_kind_t::not_found,
                "The selected Mesh is not registered in this process"));
        source = *found;
    } else {
        if (_nodes.empty ())
            return task_t<spot_create_result_t> (
              result_t<spot_create_result_t>::failure (
                framework_error_kind_t::not_configured,
                "No object Client or Server Mesh is registered"));
        if (_nodes.size () != 1)
            return task_t<spot_create_result_t> (
              result_t<spot_create_result_t>::failure (
                framework_error_kind_t::invalid_operation,
                "More than one object Mesh is registered; select one with in_mesh"));
        source = _nodes.front ();
    }
    if (!spot_id) {
        spot_id = detail::new_user_spot_id ();
    } else {
        try {
            detail::require_spot_id (*spot_id);
        }
        catch (const std::invalid_argument &error) {
            return task_t<spot_create_result_t> (
              result_t<spot_create_result_t>::failure (
                framework_error_kind_t::not_configured,
                error.what ()));
        }
        if (!exclusive
            && detail::is_framework_entry_spot_id (*spot_id)) {
            return task_t<spot_create_result_t> (
              result_t<spot_create_result_t>::failure (
                framework_error_kind_t::not_configured,
                "caller-provided SpotId uses the reserved Entry Spot format"));
        }
    }
    const auto selected_mesh = mesh_name.value_or (
      source->mesh_name ());
    std::vector<mesh_node_descriptor_t> candidates;
    location_page_request_t page;
    do {
        auto listed =
          _location_store->list_mesh_nodes (selected_mesh, page)
            .result ()
            .value ();
        for (auto &descriptor : listed.items) {
            const auto capable = std::any_of (
              descriptor.object_capabilities.begin (),
              descriptor.object_capabilities.end (),
              [&] (const object_capability_t &capability) {
                  return capability.object_kind
                           == placement_object_kind_t::user_spot
                    && capability.stable_type == stable_type;
              });
            if (descriptor.state == framework_runtime_state_t::serving
                && descriptor.object_role == object_role_t::server
                && descriptor.placement_weight > 0
                && placement_capacity_available (
                     descriptor,
                     placement_object_kind_t::user_spot,
                     stable_type)
                && capable)
                candidates.push_back (std::move (descriptor));
        }
        page.continuation_token = std::move (listed.continuation_token);
    } while (page.continuation_token);
    if (candidates.empty ())
        return task_t<spot_create_result_t> (
          result_t<spot_create_result_t>::failure (
            framework_error_kind_t::capacity_exceeded,
            "No eligible User Spot target is ready"));
    const auto choose_target = [&] {
        const auto total_weight = std::accumulate (
          candidates.begin (), candidates.end (), std::uint64_t{0},
          [] (std::uint64_t total, const auto &candidate) {
              return total + static_cast<std::uint64_t> (
                candidate.placement_weight);
          });
        auto choice =
          std::hash<std::string>{} (*spot_id) % total_weight;
        auto selected = candidates.front ();
        for (const auto &candidate : candidates) {
            const auto weight = static_cast<std::uint64_t> (
              candidate.placement_weight);
            if (choice < weight) {
                selected = candidate;
                break;
            }
            choice -= weight;
        }
        return selected;
    };
    auto target = choose_target ();
    std::vector<std::byte> application_bytes;
    if (request) {
        const auto raw = detail::message_to_raw (*request, *_serializers);
        const auto bytes = raw.bytes ();
        if (bytes.size () > 1024u * 1024u)
            return task_t<spot_create_result_t> (
              result_t<spot_create_result_t>::failure (
                framework_error_kind_t::not_configured,
                "User Spot creation request exceeds 1 MiB"));
        application_bytes.assign (bytes.begin (), bytes.end ());
    }
    const object_creation_key_t key{
      placement_object_kind_t::user_spot,
      *spot_id};
    object_reservation_fence_t fence;
    bool source_created_reservation = false;
    const std::string creating_text =
      "zlink:user-spot:creating:v1\n" + stable_type + "\n"
      + *spot_id;
    std::vector<std::byte> creating_payload;
    creating_payload.reserve (creating_text.size ());
    for (const auto value : creating_text)
        creating_payload.push_back (static_cast<std::byte> (
          static_cast<unsigned char> (value)));
    object_reserve_request_t reserve_request;
    reserve_request.key = key;
    reserve_request.intent.stable_type = stable_type;
    reserve_request.intent.request_content_reference =
      encode_inline_creation_content (application_bytes);
    reserve_request.intent.request_sha256 = sha256 (application_bytes);
    reserve_request.intent.request_encoded_size =
      static_cast<std::uint64_t> (application_bytes.size ());
    reserve_request.creating_payload = creating_payload;
    reserve_request.capacity_bundle = {
      0,
      1,
      spot_type_capacity_delta_t{
        placement_object_kind_t::user_spot,
        stable_type,
        1}};
    object_reserve_result_t reserved;
    while (true) {
        reserve_request.target = {
          selected_mesh,
          node_rid_t::from_string (target.rid.to_string ()),
          target.lifecycle_generation,
          {target.owner_id, target.lease_generation}};
        reserved =
          _location_store->reserve (reserve_request)
            .result ()
            .value ();
        const auto *conflict =
          std::get_if<object_reserve_conflict_t> (&reserved);
        const bool target_unavailable =
          conflict
          && std::holds_alternative<authority_missing_t> (
            conflict->current);
        if (!std::holds_alternative<object_placement_capacity_exhausted_t> (
              reserved)
            && !target_unavailable)
            break;
        candidates.erase (
          std::remove_if (
            candidates.begin (), candidates.end (),
            [&] (const auto &candidate) {
                return candidate.mesh_name == target.mesh_name
                       && candidate.rid == target.rid
                       && candidate.lifecycle_generation
                            == target.lifecycle_generation;
            }),
          candidates.end ());
        if (candidates.empty ()
            || std::chrono::steady_clock::now () >= deadline)
            return task_t<spot_create_result_t> (
              result_t<spot_create_result_t>::failure (
                framework_error_kind_t::capacity_exceeded,
                "User Spot placement candidates were exhausted"));
        target = choose_target ();
    }
    if (const auto *ready =
          std::get_if<object_already_exists_t> (&reserved)) {
        if (exclusive)
            return task_t<spot_create_result_t> (
              result_t<spot_create_result_t>::failure (
                framework_error_kind_t::already_exists,
                "Framework-generated User SpotId already exists"));
        return task_t<spot_create_result_t> (
          result_t<spot_create_result_t>::success (
            {spot_ref_t{
               *spot_id,
               ready->current.object_generation,
               ready->current.allocation.target.mesh_name,
               ready->current.allocation.target.node_rid},
             spot_create_state_t::existing,
             std::nullopt}));
    }
    if (const auto *mismatch =
          std::get_if<object_type_mismatch_t> (&reserved)) {
        if (exclusive
            && mismatch->current.allocation.state
                 == placement_allocation_state_t::active)
            return task_t<spot_create_result_t> (
              result_t<spot_create_result_t>::failure (
                framework_error_kind_t::already_exists,
                "Framework-generated User SpotId already exists"));
        return task_t<spot_create_result_t> (
          result_t<spot_create_result_t>::failure (
            framework_error_kind_t::type_mismatch,
            "User Spot stable type does not match"));
    }
    if (std::holds_alternative<
          object_placement_capacity_exhausted_t> (reserved))
        return task_t<spot_create_result_t> (
          result_t<spot_create_result_t>::failure (
            framework_error_kind_t::capacity_exceeded,
            "User Spot placement capacity is exhausted"));
    if (const auto *created =
          std::get_if<object_reserved_t> (&reserved)) {
        fence = created->fence;
        source_created_reservation = true;
    } else if (const auto *conflict =
                 std::get_if<object_reserve_conflict_t> (&reserved)) {
        if (exclusive)
            return task_t<spot_create_result_t> (
              result_t<spot_create_result_t>::failure (
                framework_error_kind_t::already_exists,
                "Framework-generated User SpotId already has an authority claim"));
        const auto *snapshot =
          std::get_if<authority_snapshot_t> (&conflict->current);
        if (!snapshot || !snapshot->pending_creation
            || snapshot->allocation.stable_type != stable_type)
            return task_t<spot_create_result_t> (
              result_t<spot_create_result_t>::failure (
                framework_error_kind_t::internal_failure,
                "User Spot Creating attempt cannot be joined"));
        fence = {
          snapshot->pending_creation->reservation_id,
          snapshot->store_version,
          snapshot->object_generation,
          snapshot->authority_owner_generation,
          {snapshot->allocation.target.mesh_name,
           snapshot->allocation.target.node_rid,
           snapshot->allocation.target.node_lifecycle_generation,
           snapshot->owner},
          snapshot->allocation.capacity_bundle};
        target.mesh_name = fence.target.mesh_name;
        target.rid = zlink::routing_id_t::from (
          std::string (fence.target.node_rid.value ()));
        target.lifecycle_generation =
          fence.target.node_lifecycle_generation;
        target.owner_id = fence.target.owner.owner_id;
        target.lease_generation = fence.target.owner.lease_generation;
    } else {
        return task_t<spot_create_result_t> (
          result_t<spot_create_result_t>::failure (
            framework_error_kind_t::internal_failure,
            "User Spot reservation failed"));
    }
    const auto source_status = source->native_node ().status ();
    protocol::user_spot_create_header_t command{
      0,
      {source_status.lifecycle_generation (),
       next_user_spot_operation.fetch_add (
         1, std::memory_order_relaxed)},
      source_status.routing_id ().to_bytes (),
      source_status.lifecycle_generation (),
      *spot_id,
      stable_type,
      {fence.reservation_id,
       fence.expected_store_version,
       fence.object_generation,
       fence.authority_owner_generation,
       target.rid.to_bytes (),
       target.lifecycle_generation,
       fence.target.owner.owner_id,
       static_cast<std::uint64_t> (
         fence.target.owner.lease_generation),
       fence.capacity_bundle.spot_slots},
      static_cast<std::uint64_t> (
        std::chrono::duration_cast<std::chrono::milliseconds> (
          std::chrono::system_clock::now ().time_since_epoch ()
          + timeout)
          .count ())};
    auto completion =
      std::make_shared<detail::task_completion_source_t<
        spot_create_result_t>> ();
    auto output = completion->task ();
    auto on_complete =
      [completion, target, serializers = _serializers,
       store = _location_store, key, fence,
       source_created_reservation] (
        foundation::operation_terminal_t terminal,
        protocol::user_spot_create_reply_t reply,
        std::optional<protocol::application_payload_t>
          application_reply) {
        if (terminal
              != foundation::operation_terminal_t::completed
            || reply.header.terminal_result != 0) {
            if (terminal
                == foundation::operation_terminal_t::completed)
                (void) cleanup_source_created_reservation (
                  store, key, fence,
                  source_created_reservation);
            //  Spec 16 §9 / 26 — preserve the mapped kind and the exact
            //  operation/wire terminal in the message instead of
            //  collapsing every failure into one opaque string.
            const auto mapped_kind = user_spot_terminal::
              map_user_spot_operation_failure (
                terminal, reply.header, true);
            completion->complete (
              result_t<spot_create_result_t>::failure (
                mapped_kind,
                "Remote User Spot creation failed (kind="
                  + std::to_string (static_cast<int> (mapped_kind))
                  + ", operationTerminal="
                  + std::to_string (static_cast<int> (terminal))
                  + ", terminalResult="
                  + std::to_string (reply.header.terminal_result)
                  + ", failureCode="
                  + std::to_string (reply.header.failure_code) + ")"));
            return;
        }
        if (reply.result
            == protocol::user_spot_create_result_t::rejected)
            (void) cleanup_source_created_reservation (
              store, key, fence,
              source_created_reservation);
        std::optional<message_t> decoded_reply;
        if (application_reply)
            decoded_reply = message_t::from_raw (
              zlink::message_t::from (
                application_reply->payload),
              serializers);
        completion->complete (
          result_t<spot_create_result_t>::success (
            {{spot_id_t (reply.spot_id),
              reply.object_generation,
              target.mesh_name,
              node_rid_t::from_string (
                target.rid.to_string ())},
             reply.result
                   == protocol::user_spot_create_result_t::existing
               ? spot_create_state_t::existing
               : reply.result
                     == protocol::user_spot_create_result_t::created
                 ? spot_create_state_t::created
                 : spot_create_state_t::rejected,
             std::move (decoded_reply)}));
      };
    try {
        auto accepted = source->native_node ().create_user_spot_remote (
          target.rid, command, timeout, on_complete);
        detail::observe_task_completion (
          accepted,
          [completion, store = _location_store, key, fence,
           source_created_reservation] (const result_t<bool> &result) mutable {
              if (result && result.value ())
                  return;
              (void) cleanup_source_created_reservation (
                store, key, fence, source_created_reservation);
              completion->complete (result_t<spot_create_result_t>::failure (
                result ? framework_error_kind_t::rejected : result.error_kind (),
                result && !result.value ()
                  ? "User Spot create operation was not admitted"
                  : (result.error () ? result.error ()->what ()
                                    : "User Spot create submission failed")));
          });
    }
    catch (...) {
        (void) cleanup_source_created_reservation (
          _location_store, key, fence,
          source_created_reservation);
        completion->complete (
          result_t<spot_create_result_t>::failure (
            framework_error_kind_t::internal_failure,
            "User Spot create submission failed"));
        return output;
    }
    return output;
}

task_t<std::optional<spot_ref_t>>
mesh_node_host_service_t::find_user_spot (spot_id_t spot_id)
{
    if (!_location_store)
        return task_t<std::optional<spot_ref_t>> (
          result_t<std::optional<spot_ref_t>>::failure (
            framework_error_kind_t::not_configured,
            "Spot manager requires a Location Store"));
    const auto read = _location_store
      ->read_authority (spot_authority_key (spot_id))
      .result ()
      .value ();
    const auto *snapshot = std::get_if<authority_snapshot_t> (&read);
    if (!snapshot
        || snapshot->allocation.object_kind
             != placement_object_kind_t::user_spot
        || snapshot->allocation.state
             != placement_allocation_state_t::active)
        return task_t<std::optional<spot_ref_t>> (
          result_t<std::optional<spot_ref_t>>::success (
            std::nullopt));
    return task_t<std::optional<spot_ref_t>> (
      result_t<std::optional<spot_ref_t>>::success (
        spot_ref_t{
          std::move (spot_id),
          snapshot->object_generation,
          snapshot->allocation.target.mesh_name,
          snapshot->allocation.target.node_rid}));
}

task_t<bool> mesh_node_host_service_t::close_user_spot (
  const std::shared_ptr<detail::mesh_node_runtime_t> &,
  spot_ref_t spot)
{
    if (!_location_store)
        return task_t<bool> (result_t<bool>::failure (
          framework_error_kind_t::not_configured,
          "Spot manager requires a Location Store"));
    const auto source_node = std::find_if (
      _nodes.begin (), _nodes.end (),
      [&] (const auto &node) {
          return node && node->mesh_name () == spot.mesh_name ();
      });
    if (source_node == _nodes.end ())
        return task_t<bool> (result_t<bool>::failure (
          framework_error_kind_t::not_found,
          "The SpotRef Mesh is not registered in this process"));
    const auto source = *source_node;
    const auto read = _location_store
      ->read_authority (spot_authority_key (spot.spot_id ()))
      .result ()
      .value ();
    const auto *snapshot = std::get_if<authority_snapshot_t> (&read);
    if (!snapshot)
        return task_t<bool> (result_t<bool>::success (false));
    if (snapshot->object_generation != spot.object_generation ())
        return task_t<bool> (result_t<bool>::failure (
          framework_error_kind_t::invalid_operation,
          "User Spot generation is stale"));
    if (snapshot->allocation.object_kind
          != placement_object_kind_t::user_spot
        || snapshot->allocation.state
             != placement_allocation_state_t::active
        || snapshot->allocation.target.mesh_name != spot.mesh_name ()
        || snapshot->allocation.target.node_rid.value ()
             != spot.node_rid ().value ())
        return task_t<bool> (result_t<bool>::failure (
          framework_error_kind_t::unavailable,
          "User Spot owner is moving"));
    const auto source_status = source->native_node ().status ();
    protocol::user_spot_close_header_t command{
      0,
      {source_status.lifecycle_generation (),
       next_user_spot_operation.fetch_add (
         1, std::memory_order_relaxed)},
      source_status.routing_id ().to_bytes (),
      source_status.lifecycle_generation (),
      {spot.spot_id (),
       spot.object_generation (),
       zlink::routing_id_t::from (
         std::string (snapshot->allocation.target.node_rid.value ()))
         .to_bytes (),
       snapshot->allocation.target.node_lifecycle_generation,
       snapshot->authority_owner_generation,
       snapshot->store_version},
      static_cast<std::uint64_t> (
        std::chrono::duration_cast<std::chrono::milliseconds> (
          std::chrono::system_clock::now ().time_since_epoch ()
          + std::chrono::seconds (30))
          .count ())};
    auto completion =
      std::make_shared<detail::task_completion_source_t<bool>> ();
    auto output = completion->task ();
    auto accepted =
      source->native_node ().close_user_spot_remote (
        zlink::routing_id_t::from (
          std::string (snapshot->allocation.target.node_rid.value ())),
        std::move (command), std::chrono::seconds (30),
        [completion] (
          foundation::operation_terminal_t terminal,
          protocol::user_spot_close_reply_t reply) {
            if (terminal
                  != foundation::operation_terminal_t::completed
                || reply.header.terminal_result != 0) {
                completion->complete (
                  result_t<bool>::failure (
                    user_spot_terminal::
                      map_user_spot_operation_failure (
                      terminal, reply.header, false),
                    "Remote User Spot close failed"));
                return;
            }
            completion->complete (
              result_t<bool>::success (reply.closed));
        });
    detail::observe_task_completion (
      accepted, [completion] (const result_t<bool> &result) mutable {
          if (result && result.value ())
              return;
          completion->complete (result_t<bool>::failure (
            result ? framework_error_kind_t::rejected : result.error_kind (),
            result && !result.value ()
              ? "User Spot close operation was not admitted"
              : (result.error () ? result.error ()->what ()
                                : "User Spot close submission failed")));
      });
    return output;
}

void mesh_node_host_service_t::start (service_provider_t &services)
{
    try {
    _services = &services;
    const auto actor_gateway =
      services.get<detail::actor_gateway_runtime_t> ();
    if (actor_gateway) {
        actor_gateway->get ().bind_serializers (*_serializers);
    }
    _stop.store (false, std::memory_order_release);
    _accept_application_dispatch.store (true, std::memory_order_release);
    _actor_destroy_gate =
      std::make_shared<actor_destroy_callback_gate_t> ();
    auto store = std::shared_ptr<location_repository_t> (
      &services.get_required<location_repository_t> (),
      [] (location_repository_t *) noexcept {});
    _location_store = store;
    for (const auto &registration : _registrations) {
        if (!registration || !registration->spot_state)
            continue;
        const auto gate = _actor_destroy_gate;
        detail::spot_node_runtime_t spot_runtime (registration->spot_state);
        spot_runtime.on_destroy_actor ([this, gate] (const actor_ref_t &actor) {
              if (!gate->try_enter ())
                  return result_t<void>::failure (
                    framework_error_kind_t::shutting_down,
                    "Actor destroy cleanup was rejected during Mesh shutdown");
              try {
                  auto result = finalize_local_actor_destroy (actor);
                  gate->leave ();
                  return result;
              }
              catch (...) {
                  gate->leave ();
                  throw;
              }
        });
        if (actor_gateway) {
            spot_runtime.on_actor_ref_updated (
              [gateway = &actor_gateway->get ()] (const actor_ref_t &actor) {
                  return gateway->update_actor_ref (actor);
              });
        }
    }
    auto &location_runtime =
      services.get_required<location_runtime_t> ();
    _location_runtime = &location_runtime;
    _location_owner = location_runtime.current_owner_token ();
    if (!_location_owner)
        throw framework_exception_t (
          framework_error_kind_t::not_configured,
          "MeshNode publication requires an active Location owner lease");
    std::shared_ptr<stateful::relocation_store_port_t>
      instance_relocations;
    const auto has_instance_factories = std::any_of (
      _registrations.begin (), _registrations.end (),
      [] (const auto &registration) {
          return registration && registration->spot_state
                 && !registration->spot_state->snapshot
                       .instance_spot_names.empty ();
      });
    if (has_instance_factories) {
        auto &relocations = services.get_required<
          stateful::relocation_store_port_t> ();
        instance_relocations = std::shared_ptr<
          stateful::relocation_store_port_t> (
            &relocations, [] (auto *) noexcept {});
    }
    for (std::size_t index = 0; index < _nodes.size (); ++index) {
        const auto registration = _registrations[index];
        const auto source = _nodes[index];
        registration->spot_state->create_user_spot =
          [this, source] (
            bool exclusive,
            std::optional<spot_id_t> spot_id,
            std::string stable_type,
            std::optional<std::string> mesh_name,
            std::optional<message_t> request,
            std::chrono::milliseconds timeout) {
              return create_user_spot (
                source, exclusive, std::move (spot_id),
                std::move (stable_type), std::move (mesh_name),
                std::move (request), timeout);
          };
        registration->spot_state->find_user_spot =
          [this] (spot_id_t spot_id) {
              return find_user_spot (std::move (spot_id));
          };
        registration->spot_state->close_user_spot =
          [this, source] (spot_ref_t spot) {
              return close_user_spot (source, std::move (spot));
          };
        _nodes[index]->configure_user_spot_operations (
          store,
          [this, registration, source] (
            const stateful::object_ref_t &object,
            const std::string &stable_type,
            const std::vector<std::byte> &payload) {
              std::vector<std::uint8_t> rid_bytes;
              rid_bytes.reserve (object.key.size ());
              for (const auto value : object.key)
                  rid_bytes.push_back (
                    static_cast<std::uint8_t> (
                      static_cast<unsigned char> (value)));
              std::vector<std::uint8_t> request_bytes;
              request_bytes.reserve (payload.size ());
              for (const auto value : payload)
                  request_bytes.push_back (
                    std::to_integer<std::uint8_t> (value));
              auto spot_id = spot_id_t (
                zlink::routing_id_t::from (rid_bytes)
                  .to_string ());
              const auto native_id = std::string (spot_id);
              auto native_spot =
                std::make_shared<host::spot_handle_t> (
                  source->native_node ().shared_from_this (),
                  object);
              {
                  std::lock_guard<std::recursive_mutex> lock (
                    registration->spot_state->mutex);
                  registration->spot_state->native_spots_by_id
                    .insert_or_assign (
                      native_id, native_spot);
              }
              detail::spot_node_runtime_t spots (
                registration->spot_state);
              std::optional<detail::local_spot_create_result_t> created;
              try {
                  created.emplace (spots.get_or_create_spot (
                    stable_type, std::move (spot_id),
                    zlink::message_t::from (request_bytes),
                    object.object_generation,
                    object.mesh_name));
              }
              catch (...) {
                  std::lock_guard<std::recursive_mutex> lock (
                    registration->spot_state->mutex);
                  const auto found =
                    registration->spot_state
                      ->native_spots_by_id.find (native_id);
                  if (found
                        != registration->spot_state
                             ->native_spots_by_id.end ()
                      && found->second == native_spot)
                      registration->spot_state
                        ->native_spots_by_id.erase (found);
                  throw;
              }
              std::optional<protocol::application_payload_t>
                application_reply;
              if (created->reply) {
                  application_reply =
                    protocol::application_payload_t{
                      stable_type,
                      "application/octet-stream",
                      detail::message_to_raw (
                        *created->reply, *_serializers)
                        .to_bytes ()};
              }
              return host::user_spot_materialize_result_t{
                created->state
                  == spot_create_state_t::created
                  || created->state
                       == spot_create_state_t::existing,
                std::move (application_reply)};
          });
        if (!registration->spot_state->snapshot.instance_spot_names.empty ()) {
            if (!instance_relocations)
                throw framework_exception_t (
                  framework_error_kind_t::not_configured,
                  "Instance Spot factories require a Relocation Store");
            registration->spot_state->admit_instance_spot_idle_eviction =
              [source] (const spot_id_t &spot_id,
                        std::string_view stable_type,
                        std::uint64_t object_generation,
                        std::uint64_t authority_owner_generation,
                        std::function<bool ()> close_local) {
                  return source->native_node ().evict_instance_spot (
                    std::string (stable_type), std::string (spot_id),
                    object_generation, authority_owner_generation,
                    std::move (close_local));
              };
            registration->spot_state->begin_instance_spot_close =
              [source] (const spot_id_t &spot_id,
                        std::string_view stable_type,
                        std::uint64_t object_generation,
                        std::uint64_t authority_owner_generation) {
                  return source->native_node ().begin_instance_spot_close (
                    std::string (stable_type), std::string (spot_id),
                    object_generation, authority_owner_generation);
              };
            _nodes[index]->configure_instance_spot_operations (
              store, instance_relocations, *_location_owner,
              host::instance_spot_activation_materializer_t{
                [registration, store] (
                  const protocol::instance_spot_activation_header_t &request) {
                    const auto authority = store
                      ->read_authority (
                        spot_authority_key (request.target.spot_id))
                      .result ().value ();
                    const auto *snapshot =
                      std::get_if<authority_snapshot_t> (&authority);
                    if (!snapshot
                        || snapshot->allocation.object_kind
                             != placement_object_kind_t::instance_spot
                        || snapshot->allocation.stable_type
                             != request.target.stable_type
                        || snapshot->allocation.target.mesh_name
                             != request.target.mesh_name
                        || snapshot->allocation.target.node_rid.value ()
                             != zlink::routing_id_t::from (
                                  request.target.target_node_routing_id)
                                  .to_string ()
                        || snapshot->allocation.target
                             .node_lifecycle_generation
                             != request.target.target_node_generation)
                        return false;
                    const auto created = detail::spot_node_runtime_t (
                      registration->spot_state)
                      .get_or_create_spot (
                        request.target.stable_type,
                        spot_id_t (request.target.spot_id),
                        zlink::message_t{}, snapshot->object_generation,
                        request.target.mesh_name,
                        snapshot->authority_owner_generation);
                    return created.state
                           == spot_create_state_t::created
                           || created.state
                                == spot_create_state_t::existing;
                },
                [this, registration, &services] (
                  const protocol::instance_spot_activation_header_t &request,
                  const std::optional<std::vector<std::uint8_t>> &metadata,
                  const protocol::application_payload_t &application) {
                    try {
                        std::map<std::string, std::string> decoded_metadata;
                        if (metadata
                            && !detail::mesh_metadata_codec_t::decode (
                              *metadata, decoded_metadata))
                            return host::instance_spot_activation_result_t{
                              107,
                              static_cast<std::uint32_t> (
                                protocol::framework_error_code::requestFailed),
                              std::nullopt};
                        auto reply = detail::spot_node_runtime_t (
                          registration->spot_state)
                          .dispatch_instance_activation (
                            spot_id_t (request.target.spot_id),
                            application.packet_name,
                            application.content_type,
                            application.payload,
                            std::move (decoded_metadata),
                            request.request,
                            std::to_string (request.operation.high)
                              + ":"
                              + std::to_string (request.operation.low),
                            services, *_serializers, application.flow_id,
                            application.flow_origin)
                          .result ().value ();
                        std::optional<protocol::application_payload_t>
                          application_reply;
                        if (request.request) {
                            application_reply =
                              protocol::application_payload_t{
                                application.packet_name,
                                "application/octet-stream",
                                reply.to_bytes ()};
                            application_reply->flow_id = application.flow_id;
                            application_reply->flow_origin = application.flow_origin;
                        }
                        return host::instance_spot_activation_result_t{
                          0, 0, std::move (application_reply)};
                    }
                    catch (const framework_exception_t &error) {
                        detail::dispatch_error_reporter_t (_dispatch_options)
                          .report_lazy ([&] { return message_dispatch_error_event_t{
                            .surface = dispatch_error_surface_t::spot_route,
                            .message_kind = dispatch_message_kind_t::request,
                            .reason = detail::dispatch_reason_from_error (&error),
                            .action = dispatch_error_action_t::reply_error,
                            .packet_name = application.packet_name,
                            .spot_id = request.target.spot_id,
                            .exception = std::make_exception_ptr (error)}; });
                        return host::instance_spot_activation_result_t{
                          105,
                          static_cast<std::uint32_t> (
                            protocol::framework_error_code::requestFailed),
                          std::nullopt};
                    }
                }});
        }
    }
    auto &spot_resolver =
      services.get_required<spot_address_resolver_t> ();
    auto &actor_resolver =
      services.get_required<actor_address_resolver_t> ();
    const auto location_options_at_startup =
      location_runtime.options ();
    for (std::size_t index = 0; index < _nodes.size (); ++index) {
        const auto &node = _nodes[index];
        const auto registration = _registrations[index];
        const auto mesh_name = node->mesh_name ();
        node->configure_spot_route_fence_resolver (
          [&spot_resolver, mesh_name] (
            const zlink::routing_id_t &target_node_rid,
            std::string_view target_spot_id,
            std::uint64_t target_spot_generation)
            -> std::optional<host::route_fence_t> {
              try {
                  const auto resolved = spot_resolver
                    .resolve_spot_address (
                      mesh_name, std::string (target_spot_id))
                    .result ();
                  if (!resolved || !resolved.value ())
                      return std::nullopt;
                  const auto &address = *resolved.value ();
                  if (address.node_rid != target_node_rid
                      || address.object_generation
                           != target_spot_generation
                      || address.authority_owner_generation == 0
                      || address.owner.lease_generation <= 0)
                      return std::nullopt;
                  return host::route_fence_t{
                    address.authority_owner_generation,
                    static_cast<std::uint64_t> (
                      address.owner.lease_generation)};
              }
              catch (...) {
                  return std::nullopt;
              }
          },
          location_options_at_startup.route_cache_max_age,
          location_options_at_startup.owner_lease_fencing_margin,
          location_options_at_startup.session_relocation_seal_timeout);
        node->configure_actor_route_resolver (
          [&actor_resolver] (const actor_ref_t &actor)
            -> std::optional<spot_address_t> {
              try {
                  const auto resolved = actor_resolver
                    .resolve_actor_address (
                      std::string (actor.actor_id ().value ()))
                    .result ();
                  if (!resolved || !resolved.value ())
                      return std::nullopt;
                  const auto &address = *resolved.value ();
                  if (address.object_generation != actor.object_generation ()
                      || address.authority_owner_generation == 0
                      || address.owner.lease_generation <= 0)
                      return std::nullopt;
                  return address;
              }
              catch (...) {
                  return std::nullopt;
              }
          },
          [&actor_resolver] (
            const protocol::actor_route_fence_t &source) {
              const auto expected = spot_address_t{
                {},
                zlink::routing_id_t::from (
                  source.target_node_routing_id),
                {},
                0,
                {},
                source.object_generation,
                source.authority_owner_generation,
                location_owner_token_t{
                  {},
                  static_cast<std::int64_t> (
                    source.owner_lease_generation)},
                source.target_node_generation};
              (void) actor_resolver.invalidate_actor_address_if_matches (
                source.actor_id, expected);
          });
        registration->spot_state->actor_route_admission =
          [&actor_resolver] (const protocol::actor_route_fence_t &route) {
              try {
                  const auto resolved = actor_resolver
                    .resolve_actor_address (route.actor_id).result ();
                  if (!resolved || !resolved.value ())
                      return false;
                  const auto &address = *resolved.value ();
                  return address.node_rid
                           == zlink::routing_id_t::from (
                             route.target_node_routing_id)
                         && address.node_generation
                              == route.target_node_generation
                         && address.authority_owner_generation
                              == route.authority_owner_generation
                         && address.owner.lease_generation > 0
                         && static_cast<std::uint64_t> (
                              address.owner.lease_generation)
                              == route.owner_lease_generation;
              }
              catch (...) {
                  return false;
              }
          };
        node->configure_actor_create_operations (
          [node, store] (const protocol::actor_create_header_t &request,
                  host::actor_create_operation_target_completion_t completion) {
              const auto failed = [&] {
                  host::actor_create_operation_result_t result;
                  result.reply.header = {
                    request.correlation,
                    105u,
                    static_cast<std::uint32_t> (
                      protocol::framework_error_code::actorCreateFailed)};
                  return result;
              };
              const auto status = node->status ();
              if (request.reservation.target_node_routing_id
                    != status.routing_id ().to_bytes ()
                  || request.reservation.target_node_generation
                       != status.lifecycle_generation ()) {
                  completion (failed ());
                  return;
              }
              const auto now = std::chrono::system_clock::now ();
              const auto deadline =
                std::chrono::system_clock::time_point (
                  std::chrono::milliseconds (request.deadline_unix_ms));
              if (deadline <= now) {
                  completion (failed ());
                  return;
              }
              const auto timeout = std::chrono::duration_cast<
                std::chrono::milliseconds> (deadline - now);
              const auto creation_bytes = read_actor_creation_request (
                store, request);
              if (!creation_bytes) {
                  completion (failed ());
                  return;
              }
              const auto creation_request = zlink::message_t::from (
                *creation_bytes);
              const auto created = node->create_application_actor (
                request.stable_type, request.actor_id, creation_request,
                request.reservation.object_generation,
                request.reservation.authority_owner_generation,
                timeout);
              if (!created) {
                  completion (failed ());
                  return;
              }
              const auto actor = created.value ();
              const auto joined = node->submit_application_actor_entry_spot_join (
                actor,
                node_rid_t::from_string (status.routing_id ().to_string ()),
                creation_request, timeout,
                [request, actor, completion] (
                  result_t<detail::actor_join_reply_t> joined) mutable {
                    host::actor_create_operation_result_t result;
                    result.reply.header = {
                      request.correlation,
                      105u,
                      static_cast<std::uint32_t> (
                        protocol::framework_error_code::actorCreateFailed)};
                    if (!joined || joined.value ().result_code != 0) {
                        completion (std::move (result));
                        return;
                    }
                    result.reply.header = {request.correlation, 0u, 0u};
                    result.reply.result = protocol::actor_create_result_t::created;
                    result.reply.node_routing_id =
                      zlink::routing_id_t::from (
                        std::string (actor.node_rid ().value ())).to_bytes ();
                    result.reply.actor_id = std::string (actor.actor_id ().value ());
                    result.reply.object_generation = actor.object_generation ();
                    completion (std::move (result));
                });
              if (!joined) {
                  completion (failed ());
                  return;
              }
          });
        node->start ();
        if (_listener_statuses) {
            const auto status = node->status ();
            _listener_statuses->update (
              listener_kind_t::route_mesh,
              node->mesh_name (),
              status.local_endpoint ());
        }
        node->native_node ().objects ().configure_relocation_state (
          [registration] (
            const stateful::object_ref_t &spot,
            const std::string &stable_type,
            std::stop_token cancellation) {
              return detail::spot_node_runtime_t (
                registration->spot_state)
                .capture_spot_relocation_state (
                  spot, stable_type, cancellation);
          },
          [registration] (
            const stateful::frozen_object_state_t &frozen,
            const stateful::object_ref_t &target,
            std::stop_token cancellation) {
              return detail::spot_node_runtime_t (
                registration->spot_state)
                .restore_spot_relocation_state (
                  frozen, target, cancellation);
          });
        node->native_node ().objects ().configure_relocation_materialization (
          [registration] (
            const stateful::frozen_object_state_t &frozen,
            const stateful::object_ref_t &target,
            const std::optional<stateful::object_ref_t> &target_spot,
            std::stop_token cancellation) {
              return detail::spot_node_runtime_t (registration->spot_state)
                .materialize_relocation_state (
                  frozen, target, target_spot, cancellation);
          },
          [registration] (const std::vector<stateful::object_ref_t> &targets) {
              return detail::spot_node_runtime_t (registration->spot_state)
                .commit_relocation_materialization (targets);
          },
          [registration] (const std::vector<stateful::object_ref_t> &targets) {
              detail::spot_node_runtime_t (registration->spot_state)
                .abort_relocation_materialization (targets);
          });
    }
    _published_mesh_nodes.clear ();
    _published_mesh_descriptors.clear ();
    for (std::size_t index = 0; index < _nodes.size (); ++index) {
        const auto &node = _nodes[index];
        const auto &registration = _registrations[index];
        const auto status = node->status ();
        mesh_node_descriptor_t descriptor;
        const auto owner = current_location_owner ();
        if (!owner)
            throw framework_exception_t (
              framework_error_kind_t::not_configured,
              "MeshNode publication requires an active Location owner lease");
        descriptor.mesh_name = node->mesh_name ();
        descriptor.rid = status.routing_id ();
        descriptor.lifecycle_generation =
          status.lifecycle_generation ();
        descriptor.descriptor_revision = 1;
        descriptor.endpoint = status.local_endpoint ();
        if (registration->spot_state->snapshot.entry_spot_name) {
            const auto entry = registration->spot_state->spot_ids_by_name.find (
              *registration->spot_state->snapshot.entry_spot_name);
            if (entry != registration->spot_state->spot_ids_by_name.end ())
                descriptor.entry_spot_id = entry->second;
        }
        descriptor.channel_weights = node->channel_weights ();
        descriptor.object_role = registration->object_role;
        descriptor.placement_weight =
          node->placement_weight ();
        descriptor.capacity.actors.limit =
          node->actor_limit ();
        descriptor.capacity.spots.limit =
          node->spot_limit ();
        descriptor.activation_concurrency.limit =
          node->activation_concurrency_limit ();
        descriptor.state = framework_runtime_state_t::serving;
        descriptor.security_identity = "default";
        descriptor.owner_id = owner->owner_id;
        descriptor.lease_generation = owner->lease_generation;
        for (const auto &stable_type :
             registration->spot_state->snapshot.actor_types) {
            const auto configured =
              registration->spot_state->actor_factories.find (
                stable_type);
            const auto relocation =
              configured
                  != registration->spot_state->actor_factories.end ()
                ? configured->second.relocation.kind
                : detail::factory_relocation_kind_t::disabled;
            const auto has_state_adapter =
              relocation
                == detail::factory_relocation_kind_t::preserve_state;
            descriptor.object_capabilities.push_back (
              object_capability_t{
                .object_kind =
                  placement_object_kind_t::actor,
                .stable_type = stable_type,
                .policy =
                  has_state_adapter
                    ? maintenance_policy_kind_t::snapshot
                    : relocation
                          == detail::factory_relocation_kind_t::recreate
                        ? maintenance_policy_kind_t::recreate
                        : maintenance_policy_kind_t::disabled,
                .has_snapshot_adapter =
                  has_state_adapter});
        }
        for (const auto &stable_type :
             registration->spot_state->snapshot.spot_names) {
            if (registration->spot_state->snapshot.entry_spot_name
                == stable_type)
                continue;
            if (std::find (
                  registration->spot_state->snapshot.instance_spot_names.begin (),
                  registration->spot_state->snapshot.instance_spot_names.end (),
                  stable_type)
                != registration->spot_state->snapshot.instance_spot_names.end ())
                continue;
            const auto configured =
              registration->spot_state->spot_factory_relocations.find (
                stable_type);
            const auto relocation =
              configured
                  != registration->spot_state->spot_factory_relocations.end ()
                ? configured->second.kind
                : detail::factory_relocation_kind_t::disabled;
            descriptor.object_capabilities.push_back (
              object_capability_t{
                .object_kind =
                  placement_object_kind_t::user_spot,
                .stable_type = stable_type,
                .policy =
                  relocation
                      == detail::factory_relocation_kind_t::preserve_state
                    ? maintenance_policy_kind_t::snapshot
                    : relocation
                          == detail::factory_relocation_kind_t::recreate
                        ? maintenance_policy_kind_t::recreate
                        : maintenance_policy_kind_t::disabled,
                .has_snapshot_adapter =
                  relocation
                  == detail::factory_relocation_kind_t::preserve_state});
            descriptor.capacity.spot_types.push_back (
              spot_type_capacity_t{
                .object_kind =
                  placement_object_kind_t::user_spot,
                .stable_type = stable_type,
                .usage = {
                  .limit = registration->spot_state->spot_stable_type_limits.at (
                    stable_type)}});
        }
        for (const auto &stable_type :
             registration->spot_state->snapshot.instance_spot_names) {
            const auto configured =
              registration->spot_state->spot_factory_relocations.find (
                stable_type);
            const auto relocation =
              configured
                  != registration->spot_state->spot_factory_relocations.end ()
                ? configured->second.kind
                : detail::factory_relocation_kind_t::disabled;
            descriptor.object_capabilities.push_back (
              object_capability_t{
                .object_kind = placement_object_kind_t::instance_spot,
                .stable_type = stable_type,
                .policy =
                  relocation
                      == detail::factory_relocation_kind_t::preserve_state
                    ? maintenance_policy_kind_t::snapshot
                    : relocation
                          == detail::factory_relocation_kind_t::recreate
                        ? maintenance_policy_kind_t::recreate
                        : maintenance_policy_kind_t::disabled,
                .has_snapshot_adapter =
                  relocation
                  == detail::factory_relocation_kind_t::preserve_state});
            descriptor.capacity.spot_types.push_back (
              spot_type_capacity_t{
                .object_kind = placement_object_kind_t::instance_spot,
                .stable_type = stable_type,
                .usage = {
                  .limit = registration->spot_state->spot_stable_type_limits.at (
                    stable_type)}});
        }
        std::sort (
          descriptor.object_capabilities.begin (),
          descriptor.object_capabilities.end (),
          [] (const object_capability_t &left,
              const object_capability_t &right) {
              return std::tie (left.object_kind,
                               left.stable_type)
                     < std::tie (right.object_kind,
                                 right.stable_type);
          });
        std::sort (
          descriptor.capacity.spot_types.begin (),
          descriptor.capacity.spot_types.end (),
          [] (const spot_type_capacity_t &left,
              const spot_type_capacity_t &right) {
              return std::tie (left.object_kind,
                               left.stable_type)
                     < std::tie (right.object_kind,
                                 right.stable_type);
          });
        const auto written =
          _location_store
            ->update_mesh_node (
              descriptor, location_write_intent_t::new_claim)
            .result ()
            .value ();
        if (written.status != location_write_status_t::stored)
            throw framework_exception_t (
              framework_error_kind_t::not_configured,
              "MeshNode Location descriptor publication was fenced");
        _published_mesh_nodes.push_back (
          {descriptor.mesh_name, descriptor.rid});
        _published_mesh_descriptors.push_back (descriptor);
        node->bind_descriptor_publisher (
          [this, index] (const std::map<std::string, int> &channel_weights,
                         int placement_weight,
                         std::uint64_t descriptor_revision) {
              std::lock_guard lock (_descriptor_publish_mutex);
              const auto owner = current_location_owner ();
              if (!_location_store || !owner
                  || index >= _published_mesh_descriptors.size ())
                  throw framework_exception_t (
                    framework_error_kind_t::protocol_error,
                    "MeshNode Location descriptor publisher is not active");
              auto descriptor = _published_mesh_descriptors[index];
              descriptor.owner_id = owner->owner_id;
              descriptor.lease_generation = owner->lease_generation;
              descriptor.channel_weights = channel_weights;
              descriptor.placement_weight = placement_weight;
              descriptor.descriptor_revision = descriptor_revision;
              const auto written =
                _location_store
                  ->update_mesh_node (
                    descriptor, location_write_intent_t::renew)
                  .result ()
                  .value ();
              if (written.status != location_write_status_t::stored)
                  throw framework_exception_t (
                    framework_error_kind_t::protocol_error,
                    "MeshNode Location descriptor update was fenced");
              _published_mesh_descriptors[index] = std::move (descriptor);
          });
    }
    for (std::size_t index = 0; index < _nodes.size (); ++index) {
        const auto node = _nodes[index];
        const auto registration = _registrations[index];
        _threads.emplace_back ([this, node, registration] {
            application_supply_slot_t supply (
              _application_jobs,
              [node] {
                  node->native_node ().signal_dispatch_activity ();
              });
            while (!_stop.load (std::memory_order_acquire)) {
                supply.ensure_waiter ();
                auto application_permit = supply.take ();
                const bool accept_application_receive =
                  static_cast<bool> (application_permit);
                const auto count =
                  std::move (node->dispatch_ready (
                  [&] (const host::ready_record_t &owner,
                       const host::receive_record_t &record,
                       std::vector<zlink::message_t> parts) {
                      trace_mesh_application ("callback", record, parts.size ());
                      if (record.kind == host::record_kind_t::completion
                          && record.operation_kind
                               == host::operation_kind_t::actor_join
                          && node->complete_application_actor_entry_spot_join (
                            record, parts))
                          return;
                      detail::spot_node_runtime_t spot_runtime (registration->spot_state);
                      const bool transfer_dispatch =
                        owner.owner_kind == host::owner_kind_t::actor
                        && (record.kind == host::record_kind_t::actor_send
                            || record.kind == host::record_kind_t::actor_request)
                        && owner.actor
                        && spot_runtime.actor_transfer_in_progress (*owner.actor);
                      const bool framework_object_dispatch =
                        (owner.owner_kind == host::owner_kind_t::actor
                         && (record.kind == host::record_kind_t::actor_send
                             || record.kind == host::record_kind_t::actor_request))
                        || (owner.owner_kind == host::owner_kind_t::spot
                            && (record.kind == host::record_kind_t::spot_send
                                || record.kind == host::record_kind_t::spot_request
                                || record.kind == host::record_kind_t::spot_multicast
                                || record.kind == host::record_kind_t::spot_control));
                      const auto retain_mailbox_reservation =
                        record.retain_mailbox_reservation;
                      const auto complete_stateful_dispatch =
                        record.complete_stateful_dispatch;
                      const auto release_mailbox_reservation =
                        record.release_mailbox_reservation;
                      const auto release_mailbox = [&] {
                          if (release_mailbox_reservation) {
                              release_mailbox_reservation ();
                          }
                      };
                      terminal_callback_guard_t release_guard (
                        release_mailbox_reservation);
                      terminal_callback_guard_t stateful_guard (
                        complete_stateful_dispatch);
                      auto run_direct = [this] (auto &&work) {
                          {
                              std::lock_guard lock (_dispatch_gate_mutex);
                              ++_active_direct_dispatch;
                          }
                          try {
                              work ();
                          }
                          catch (...) {
                              {
                                  std::lock_guard lock (_dispatch_gate_mutex);
                                  --_active_direct_dispatch;
                              }
                              _dispatch_gate_changed.notify_all ();
                              throw;
                          }
                          {
                              std::lock_guard lock (_dispatch_gate_mutex);
                              --_active_direct_dispatch;
                          }
                          _dispatch_gate_changed.notify_all ();
                      };
                      if (framework_object_dispatch && transfer_dispatch) {
                          bool handled = false;
                          try {
                              run_direct ([&] {
                                  handled = spot_runtime.dispatch_mesh_record (
                                    owner, record, parts, *_services, *_serializers);
                              });
                          }
                          catch (...) {
                              release_mailbox ();
                              throw;
                          }
                          if (handled || transfer_dispatch) {
                              release_mailbox ();
                              return;
                          }
                      }
                      if (owner.domain == host::ready_domain_t::application) {
                          bool accepted = false;
                          {
                              std::lock_guard lock (_dispatch_gate_mutex);
                              if (_accept_application_dispatch.load (
                                    std::memory_order_relaxed)) {
                                  node->application_work_enqueued ();
                                  accepted = true;
                              }
                          }
                          if (!accepted) {
                              reject_application_request (
                                record, std::move (parts),
                                framework_error_kind_t::rejected,
                                "MeshNode is draining and rejects new application work");
                              release_mailbox ();
                              return;
                          }
                          if (!application_permit) {
                              reject_application_request (
                                record, std::move (parts),
                                framework_error_kind_t::shutting_down,
                                "Application Job Queue supply is unavailable");
                              release_mailbox ();
                              return;
                          }
                          auto application_job = std::make_shared<
                            application_job_queue_t::permit_t> (
                              std::move (*application_permit));
                          application_permit.reset ();
                          application_job->mark_queued ();
                          trace_mesh_application (
                            "submit", record, parts.size ());
                          if (retain_mailbox_reservation) {
                              retain_mailbox_reservation ();
                          }
                          auto dispatch_parts =
                            std::make_shared<std::vector<zlink::message_t>> (
                              std::move (parts));
                          const auto submitted =
                            _application_dispatch->try_submit_internal (
                                [this, node, registration, owner, record,
                                 release_mailbox_reservation,
                                 complete_stateful_dispatch,
                                 dispatch_parts,
                                 application_job] () mutable {
                                    terminal_callback_guard_t release_guard (
                                      release_mailbox_reservation);
                                    terminal_callback_guard_t stateful_guard (
                                      complete_stateful_dispatch);
                                    auto parts = std::move (*dispatch_parts);
                                    trace_mesh_application (
                                      "start", record, parts.size ());
                                    node->application_work_started ();
                                    const auto before_application_handler =
                                      [application_job] {
                                          application_job
                                            ->release_for_handler_entry ();
                                      };
                                    const auto terminal = std::make_shared<
                                      application_dispatch_terminal_owner_t> (
                                      node,
                                      complete_stateful_dispatch,
                                      release_mailbox_reservation);
                                    stateful_guard.dismiss ();
                                    release_guard.dismiss ();
                                    try {
                                        detail::spot_node_runtime_t
                                          application_spot_runtime (
                                            registration->spot_state);
                                        bool terminal_deferred = false;
                                        const auto framework_handled =
                                            application_spot_runtime
                                              .dispatch_mesh_record (
                                                owner, record, parts,
                                                *_services, *_serializers,
                                                [terminal] {
                                                    terminal->settle ();
                                                },
                                                &terminal_deferred,
                                                before_application_handler);
                                        trace_mesh_application (
                                          "framework-dispatch", record,
                                          parts.size (),
                                          framework_handled ? "handled"
                                                             : "not-handled");
                                        if (!framework_handled
                                            && !terminal_deferred) {
                                            detail::mesh_record_dispatcher_t dispatcher (
                                              *_services, *_serializers,
                                              registration->handlers, *_filters,
                                              _dispatch_options,
                                              before_application_handler);
                                            auto dispatched = dispatcher.dispatch (
                                              record, std::move (parts));
                                            if (dispatched) {
                                                trace_mesh_application (
                                                  "route-dispatch", record, 0,
                                                  "success");
                                            } else {
                                                trace_mesh_application (
                                                  "route-dispatch", record, 0,
                                                  "failure");
                                            }
                                        }
                                        if (terminal_deferred)
                                            return;
                                    }
                                    catch (const std::exception &error) {
                                        trace_mesh_application (
                                          "exception", record, parts.size (),
                                          error.what ());
                                        terminal->settle ();
                                        return;
                                    }
                                    catch (...) {
                                        trace_mesh_application (
                                          "exception", record, parts.size (),
                                          "unknown");
                                        terminal->settle ();
                                        return;
                                    }
                                    terminal->settle ();
                                });
                          if (!submitted) {
                              trace_mesh_application (
                                "reject", record, dispatch_parts->size (),
                                "application executor is stopping");
                              reject_application_request (
                                record, std::move (*dispatch_parts),
                                framework_error_kind_t::shutting_down,
                                "MeshNode application executor is stopping");
                              node->application_work_started ();
                              node->application_work_finished ();
                              _dispatch_gate_changed.notify_all ();
                              release_mailbox ();
                              return;
                          }
                          stateful_guard.dismiss ();
                          release_guard.dismiss ();
                          return;
                      }
                      run_direct ([&] {
                          if (spot_runtime.dispatch_mesh_record (
                                owner, record, parts, *_services, *_serializers)) {
                              return;
                          }
                          detail::mesh_record_dispatcher_t dispatcher (
                            *_services, *_serializers, registration->handlers, *_filters,
                            _dispatch_options);
                          (void) dispatcher.dispatch (record, std::move (parts));
                      });
                  },
                  accept_application_receive))
                    .result ()
                    .value ();
                application_permit.reset ();
                detail::spot_node_runtime_t maintenance (registration->spot_state);
                (void) maintenance.cleanup_expired_actor_admissions ();
                if (count == 0)
                    (void) node->native_node ().wait_for_dispatch_activity (
                      std::chrono::milliseconds (100),
                      false);
            }
            supply.close ();
        });
    }
    }
    catch (...) {
        stop ();
        throw;
    }
}

void mesh_node_host_service_t::request_stop () noexcept
{
    seal_application_dispatch ();
    if (_application_jobs)
        _application_jobs->stop ();
}

void mesh_node_host_service_t::seal_application_dispatch () noexcept
{
    {
        std::lock_guard lock (_dispatch_gate_mutex);
        _accept_application_dispatch.store (false, std::memory_order_release);
    }
    _dispatch_gate_changed.notify_all ();
}

bool mesh_node_host_service_t::wait_for_accepted_callbacks_until (
  std::chrono::steady_clock::time_point deadline) noexcept
{
    std::unique_lock lock (_dispatch_gate_mutex);
    auto settled = [this] {
        if (_active_direct_dispatch != 0)
            return false;
        return std::all_of (_nodes.begin (), _nodes.end (), [] (const auto &node) {
            return node->pending_application_callbacks () == 0
                   && node->active_application_callbacks () == 0
                   && node->pending_transport_operations () == 0
                   && node->active_completion_waiters () == 0;
        });
    };
    while (!settled () && std::chrono::steady_clock::now () < deadline) {
        _dispatch_gate_changed.wait_until (
          lock, std::min (deadline, std::chrono::steady_clock::now ()
                                     + std::chrono::milliseconds (1)));
    }
    return settled ();
}

void mesh_node_host_service_t::visit_relocation_nodes (
  const std::function<void (
    const std::shared_ptr<detail::mesh_node_runtime_t> &)> &visitor)
  const
{
    if (!visitor)
        return;
    for (const auto &node : _nodes)
        visitor (node);
}

bool mesh_node_host_service_t::publish_descriptor_state (
  framework_runtime_state_t state) noexcept
{
    std::lock_guard lock (_descriptor_publish_mutex);
    const auto owner = current_location_owner ();
    if (!_location_store || !owner) {
        return _published_mesh_descriptors.empty ();
    }
    try {
        for (std::size_t index = 0;
             index < _published_mesh_descriptors.size ();) {
            auto current = _published_mesh_descriptors[index];
            location_page_request_t page;
            bool found = false;
            do {
                auto listed = _location_store
                                ->list_mesh_nodes (current.mesh_name, page)
                                .result ()
                                .value ();
                const auto stored = std::find_if (
                  listed.items.begin (), listed.items.end (),
                  [&] (const mesh_node_descriptor_t &candidate) {
                      return candidate.rid.to_hex ()
                               == current.rid.to_hex ()
                             && candidate.lifecycle_generation
                                  == current.lifecycle_generation;
                  });
                if (stored != listed.items.end ()) {
                    current = *stored;
                    found = true;
                    break;
                }
                page.continuation_token =
                  std::move (listed.continuation_token);
            } while (page.continuation_token);
            if (!found) {
                /* A draining host may already have lost its descriptor when
                 * an earlier lease cleanup fenced the row. Shutdown is
                 * idempotent in that state; relocation still requires the
                 * descriptor to be present so it keeps the store failure
                 * signal. */
                if (state == framework_runtime_state_t::draining) {
                    _published_mesh_descriptors.erase (
                      _published_mesh_descriptors.begin () + index);
                    _published_mesh_nodes.erase (
                      _published_mesh_nodes.begin () + index);
                    continue;
                }
                return false;
            }
            if (current.state == state) {
                _published_mesh_descriptors[index] = std::move (current);
                ++index;
                continue;
            }
            if (current.descriptor_revision
                == std::numeric_limits<std::uint64_t>::max ()) {
                return false;
            }
            current.state = state;
            current.owner_id = owner->owner_id;
            current.lease_generation = owner->lease_generation;
            ++current.descriptor_revision;
            const auto written = _location_store
                                   ->update_mesh_node (
                                     current,
                                     location_write_intent_t::renew)
                                   .result ()
                                   .value ();
            if (written.status != location_write_status_t::stored) {
                return false;
            }
            _published_mesh_descriptors[index] = std::move (current);
            ++index;
        }
        return true;
    }
    catch (...) {
        return false;
    }
}

bool mesh_node_host_service_t::republish_after_store_recovery () noexcept
{
    std::lock_guard lock (_descriptor_publish_mutex);
    const auto owner = current_location_owner ();
    if (!_location_store || !owner)
        return _published_mesh_descriptors.empty ();
    try {
        for (std::size_t index = 0;
             index < _published_mesh_descriptors.size ();
             ++index) {
            auto descriptor = _published_mesh_descriptors[index];
            descriptor.owner_id = owner->owner_id;
            descriptor.lease_generation = owner->lease_generation;
            if (descriptor.descriptor_revision
                == std::numeric_limits<std::uint64_t>::max ()) {
                return false;
            }
            ++descriptor.descriptor_revision;
            const auto written =
              _location_store
                ->update_mesh_node (
                  descriptor, location_write_intent_t::renew)
                .result ()
                .value ();
            if (written.status != location_write_status_t::stored)
                return false;
            _published_mesh_descriptors[index] =
              std::move (descriptor);
        }
        return true;
    }
    catch (...) {
        return false;
    }
}

void mesh_node_host_service_t::stop () noexcept
{
    if (_actor_destroy_gate)
        _actor_destroy_gate->stop_and_wait ();
    request_stop ();
    if (_application_dispatch)
        _application_dispatch->drain ();
    trace_mesh_host_stop ("application-drained");
    _stop.store (true, std::memory_order_release);
    trace_mesh_host_stop ("pump-join-begin");
    for (auto &thread : _threads) {
        if (thread.joinable ())
            thread.join ();
    }
    trace_mesh_host_stop ("pump-join-end");
    _threads.clear ();
    for (auto &node : _nodes)
        node->bind_descriptor_publisher ({});
    const auto owner = current_location_owner ();
    if (_location_store && owner) {
        for (const auto &key : _published_mesh_nodes) {
            try {
                const auto removed = _location_store
                                       ->remove_mesh_node (key, *owner)
                                       .result ()
                                       .value ();
                const char *trace = std::getenv ("ZLINK_CPP_HOST_STOP_TRACE");
                if (trace != nullptr && *trace != '\0' && std::string_view (trace) != "0")
                    std::cerr << "zlink-cpp-host-stop mesh-descriptor-remove mesh="
                              << key.mesh_name << " status="
                              << static_cast<int> (removed) << std::endl;
            }
            catch (...) {
            }
        }
    }
    _published_mesh_nodes.clear ();
    _published_mesh_descriptors.clear ();
    _location_owner.reset ();
    _location_runtime = nullptr;
    for (auto &node : _nodes) {
        trace_mesh_host_stop ("node-stop-begin");
        node->stop ();
        if (_listener_statuses)
            _listener_statuses->remove (
              listener_kind_t::route_mesh, node->mesh_name ());
        trace_mesh_host_stop ("node-stop-end");
    }
    _actor_destroy_gate.reset ();
}

std::optional<location_owner_token_t>
mesh_node_host_service_t::current_location_owner () const
{
    if (!_location_runtime)
        return std::nullopt;
    return _location_runtime->current_owner_token ();
}

std::vector<std::shared_ptr<detail::mesh_node_runtime_t>>
mesh_node_host_service_t::nodes () const
{
    return _nodes;
}

zlink::submit_result_t mesh_node_host_service_t::submit_local_node_send (
  const std::shared_ptr<detail::mesh_node_runtime_t> &node,
  std::vector<zlink::message_t> parts)
{
    const auto found = std::find (_nodes.begin (), _nodes.end (), node);
    if (found == _nodes.end () || _services == nullptr || _serializers == nullptr)
        return zlink::submit_result_t::not_found;
    const auto index = static_cast<std::size_t> (std::distance (_nodes.begin (), found));
    const auto registration = _registrations[index];
    const auto source_rid = node->routing_id ();
    if (!source_rid)
        return zlink::submit_result_t::not_found;
    node->note_local_node_submit_attempt ();

    {
        std::lock_guard lock (_dispatch_gate_mutex);
        if (!_accept_application_dispatch.load (std::memory_order_relaxed))
            return zlink::submit_result_t::terminated;
    }
    auto application_permit = _application_jobs->wait_for_supply_blocking ();
    if (!application_permit)
        return zlink::submit_result_t::terminated;
    auto application_job =
      std::make_shared<application_job_queue_t::permit_t> (
        std::move (*application_permit));
    {
        std::lock_guard lock (_dispatch_gate_mutex);
        if (!_accept_application_dispatch.load (std::memory_order_relaxed))
            return zlink::submit_result_t::terminated;
        node->application_work_enqueued ();
        application_job->mark_queued ();
    }

    const auto submitted = _application_dispatch->try_submit_internal (
          [this, node, registration, source_rid,
           parts = std::move (parts), application_job] () mutable {
              node->application_work_started ();
              try {
                  host::receive_record_t record;
                  record.kind = host::record_kind_t::node_send;
                  record.domain = host::ready_domain_t::application;
                  record.source_node_rid = *source_rid;
                  const auto before_application_handler =
                    [application_job] {
                        application_job->release_for_handler_entry ();
                    };
                  detail::mesh_record_dispatcher_t dispatcher (
                    *_services, *_serializers, registration->handlers, *_filters,
                    _dispatch_options, before_application_handler);
                  (void) dispatcher.dispatch (record, std::move (parts));
              }
              catch (...) {
                  node->local_application_work_finished ();
                  _dispatch_gate_changed.notify_all ();
                  return;
              }
              node->local_application_work_finished ();
              _dispatch_gate_changed.notify_all ();
          });
    if (!submitted) {
        node->application_work_started ();
        node->local_application_work_finished ();
        _dispatch_gate_changed.notify_all ();
        return zlink::submit_result_t::terminated;
    }
    return zlink::submit_result_t::ok;
}

} // namespace zlink::framework::runtime
