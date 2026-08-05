/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "spot_runtime.hpp"

#include <zlink/framework/contracts/configuration/zlink_builder.hpp>

#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/channels/channel_reply_writer.hpp"
#include "runtime/channels/channel_runtime.hpp"
#include "runtime/channels/channel_runtime_manager.hpp"
#include "runtime/channels/route_channel_runtime.hpp"
#include "runtime/diagnostics/dispatch_error_reporter.hpp"
#include "runtime/diagnostics/monitoring_runtime.hpp"
#include "runtime/diagnostics/runtime_metrics.hpp"
#include "runtime/diagnostics/message_flow_tracer.hpp"
#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/execution/actor_execution_context.hpp"
#include "runtime/execution/serial_execution_queue.hpp"
#include "runtime/locations/actor_authority_payload.hpp"
#include "runtime/locations/live_location_reader.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/mesh/mesh_metadata_codec.hpp"
#include "runtime/messaging/envelope_codec.hpp"
#include "runtime/messaging/request_failure_mapper.hpp"
#include "runtime/messaging/submit_result_mapper.hpp"
#include "runtime/spots/spot_route_internal_dispatcher.hpp"
#include "runtime/diagnostics/flow_context.hpp"
#include "runtime/spots/spot_route_packets.hpp"
#include "runtime/streams/stream_runtime.hpp"
#include "runtime/timers/timer_runtime.hpp"

#include <zlink/framework/contracts/channels/call.hpp>

#include <zlink/Contracts/Core/routing_id.hpp>
#include <zlink/Contracts/Eventing/timers.hpp>
#include <zlink/Contracts/Errors/errors.hpp>
#include <zlink/Contracts/Messaging/message.hpp>
#include <zlink/Contracts/Messaging/received.hpp>
#include <zlink/Contracts/Messaging/topic_message.hpp>
#include <zlink/Contracts/Sockets/results.hpp>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

namespace zlink::framework
{

namespace service = runtime::host;

namespace
{
constexpr std::size_t max_spot_relocation_state_bytes =
  64u * 1024u * 1024u;

constexpr std::uint32_t actor_recv_info_no_bind_flag = 1u;

std::string actor_request_dedup_prefix (std::string_view actor_key)
{
    return std::to_string (actor_key.size ()) + ":" + std::string (actor_key);
}

std::string actor_request_dedup_key (std::string_view actor_key,
                                     std::string_view request_id)
{
    auto result = actor_request_dedup_prefix (actor_key);
    result.append (request_id);
    return result;
}

std::optional<std::uint64_t> handoff_u64 (
  const std::map<std::string, std::string> &metadata,
  std::string_view key)
{
    const auto found = metadata.find (std::string (key));
    if (found == metadata.end () || found->second.empty ())
        return std::nullopt;
    std::uint64_t value = 0;
    const auto parsed = std::from_chars (
      found->second.data (), found->second.data () + found->second.size (), value);
    if (parsed.ec != std::errc{} || parsed.ptr != found->second.data () + found->second.size ())
        return std::nullopt;
    return value;
}

std::optional<zlink::routing_id_t> handoff_routing_id (
  const std::map<std::string, std::string> &metadata,
  std::string_view key)
{
    const auto found = metadata.find (std::string (key));
    if (found == metadata.end () || found->second.empty ())
        return std::nullopt;
    try {
        auto value = zlink::routing_id_t::from_hex (found->second);
        const auto bytes = value.to_bytes ();
        if (bytes.empty () || std::all_of (bytes.begin (), bytes.end (), [] (auto byte) {
                return byte == 0;
            })) {
            return std::nullopt;
        }
        return value;
    }
    catch (...) {
        return std::nullopt;
    }
}

std::optional<runtime::protocol::actor_route_fence_t> handoff_actor_route (
  const std::map<std::string, std::string> &metadata)
{
    const auto actor_id = metadata.find (
      std::string (detail::actor_handoff_route_actor_id_key));
    const auto target_node = handoff_routing_id (
      metadata, detail::actor_handoff_route_target_node_key);
    const auto object_generation = handoff_u64 (
      metadata, detail::actor_handoff_route_object_generation_key);
    const auto target_node_generation = handoff_u64 (
      metadata, detail::actor_handoff_route_target_node_generation_key);
    const auto authority_generation = handoff_u64 (
      metadata, detail::actor_handoff_route_authority_generation_key);
    const auto lease_generation = handoff_u64 (
      metadata, detail::actor_handoff_route_lease_generation_key);
    if (actor_id == metadata.end () || actor_id->second.empty () || !target_node
        || !object_generation || !target_node_generation || !authority_generation
        || !lease_generation || *object_generation == 0 || *target_node_generation == 0
        || *authority_generation == 0 || *lease_generation == 0) {
        return std::nullopt;
    }
    return runtime::protocol::actor_route_fence_t{
      actor_id->second,
      *object_generation,
      target_node->to_bytes (),
      *target_node_generation,
      *authority_generation,
      *lease_generation};
}

std::optional<std::string>
actor_type_from_authority (runtime::live_location_reader_t &store,
                           std::string_view actor_id)
{
    const auto read = store
      .read_authority (authority_key_t{"1:" + std::string (actor_id)})
      .result ();
    if (!read)
        return std::nullopt;
    const auto *snapshot = std::get_if<authority_snapshot_t> (&read.value ());
    if (!snapshot || snapshot->allocation.state != placement_allocation_state_t::active
        || snapshot->allocation.object_kind != placement_object_kind_t::actor)
        return std::nullopt;
    const auto projection = runtime::decode_actor_authority_payload (snapshot->payload);
    if (!projection || projection->actor.actor_id ().value () != actor_id)
        return std::nullopt;
    return std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (
      projection->actor));
}

void trace_actor_dispatch (std::string_view stage,
                           std::string_view actor_id)
{
    const auto *enabled = std::getenv ("ZLINK_CPP_AUTO_CONNECT_TRACE");
    if (enabled == nullptr || *enabled == '\0')
        return;
    std::cerr << "zlink actor-dispatch stage=" << stage
              << " actor=" << actor_id << '\n';
}

bool is_blank (const std::string &value)
{
    return std::all_of (value.begin (), value.end (),
                        [] (unsigned char ch) { return std::isspace (ch) != 0; });
}

std::size_t handler_work_byte_cost (
  const zlink::message_t &message,
  const spot_inbound_message_t &metadata) noexcept
{
    std::size_t total = runtime::serial_execution_queue_t::fixed_work_byte_cost;
    const auto add = [&total] (std::size_t value) {
        if (value > std::numeric_limits<std::size_t>::max () - total)
            total = std::numeric_limits<std::size_t>::max ();
        else
            total += value;
    };
    add (message.size ());
    add (metadata.content_type.size ());
    for (const auto &[key, value] : metadata.values) {
        add (key.size ());
        add (value.size ());
    }
    if (metadata.mesh_name)
        add (metadata.mesh_name->size ());
    if (metadata.correlation_id)
        add (metadata.correlation_id->size ());
    if (metadata.source)
        add (metadata.source->size ());
    return total;
}

class spot_worker_scheduler_t final : public detail::worker_scheduler_t
{
  public:
    spot_worker_scheduler_t (std::shared_ptr<runtime::offload_executor_t> workers,
                             std::weak_ptr<detail::spot_context_state_t> owner) :
        _workers (std::move (workers)), _owner (std::move (owner))
    {
    }

    bool try_schedule (std::function<void (std::stop_token)> work) override
    {
        return _workers && _workers->try_submit_cancellable (std::move (work));
    }

    void post_owner (std::function<void ()> work) override
    {
        if (auto owner = _owner.lock ()) {
            (void) owner->try_post_serial ("worker-completion", std::move (work));
        }
    }

    std::stop_token stop_token () const noexcept override
    {
        if (auto owner = _owner.lock (); owner && owner->node) {
            return owner->node->worker_cancellation.get_token ();
        }
        return {};
    }

  private:
    std::shared_ptr<runtime::offload_executor_t> _workers;
    std::weak_ptr<detail::spot_context_state_t> _owner;
};

std::shared_ptr<runtime::offload_executor_t>
framework_worker_executor (const std::shared_ptr<detail::spot_node_builder_state_t> &node)
{
    if (node && node->worker_executor) {
        return node->worker_executor;
    }
    const auto &options = node->worker_options;
    auto executor = std::make_shared<runtime::offload_executor_t> (
      options.min_threads (), options.max_threads (), options.max_queue_length (),
      options.idle_timeout (), "zlink-spot-wrk");
    if (node) {
        node->worker_executor = executor;
    }
    return executor;
}

std::shared_ptr<detail::worker_scheduler_t>
make_spot_worker_scheduler (const std::shared_ptr<detail::spot_context_state_t> &owner)
{
    return std::make_shared<spot_worker_scheduler_t> (framework_worker_executor (owner->node),
                                                      owner);
}

void configure_spot_execution (const std::shared_ptr<detail::spot_context_state_t> &state)
{
    /* The queue owns turn ordering. Execution resources are shared by the
     * node; a Spot must not allocate its own worker pool. A yielded turn is
     * resumed through this same queue, so it does not require a Spot-local
     * executor. */
    state->serial_executor = framework_worker_executor (state->node);
    state->serial_queue =
      std::make_shared<runtime::serial_execution_queue_t> (
        *state->serial_executor, runtime::serial_execution_queue_options_t{},
        runtime::serial_execution_queue_t::error_handler_t{},
        !state->is_entry_spot ()
          && state->execution_mode == user_spot_execution_mode_t::spot_wide);
    state->worker_scheduler = make_spot_worker_scheduler (state);
}

} // namespace

namespace detail
{

spot_node_builder_state_t::~spot_node_builder_state_t () = default;

void drain_spot_node_executors (spot_node_builder_state_t &node)
{
    for (auto &[_, context] : node.spot_contexts_by_id) {
        auto state = context._state;
        if (state) {
            for (auto &timer : state->timers) {
                if (timer && timer->lane) {
                    timer->lane->cancel_pending ();
                }
            }
        }
        if (state && state->serial_queue) {
            state->serial_queue->cancel_pending ();
        }
    }
    for (auto &[_, context] : node.spot_contexts_by_id) {
        auto state = context._state;
        if (state) {
            for (auto &timer : state->timers) {
                if (timer && timer->lane) {
                    timer->lane->drain ();
                    timer->lane.reset ();
                }
            }
            if (state->serial_queue) {
                state->serial_queue->cancel_pending ();
                state->serial_queue->drain ();
                state->serial_queue.reset ();
            }
            state->serial_executor.reset ();
            state->worker_scheduler.reset ();
        }
    }
    if (node.worker_executor) {
        node.worker_executor->drain ();
        node.worker_executor.reset ();
    }
}

void cancel_spot_node_dispatch_queues (spot_node_builder_state_t &node)
{
    for (auto &[_, context] : node.spot_contexts_by_id) {
        auto state = context._state;
        if (state && state->serial_queue) {
            state->serial_queue->cancel_pending ();
        }
    }
}

} // namespace detail

namespace
{

zlink::message_t framework_reply_or_empty (const std::optional<message_t> &reply,
                                           serializer_registry_t &serializers)
{
    return reply ? detail::message_to_raw (*reply, serializers) : zlink::message_t{};
}

void attach_native_spot_locked (const std::shared_ptr<detail::spot_context_state_t> &state)
{
    if (!state || !state->node) {
        return;
    }
    auto native_node = state->node->native_node.lock ();
    if (!native_node) {
        return;
    }

    const auto rid = std::string (state->spot_id);
    auto native = state->native_spot.lock ();
    if (!native) {
        const auto found = state->node->native_spots_by_id.find (rid);
        if (found == state->node->native_spots_by_id.end ()) {
            if (state->node->snapshot.entry_spot_name
                && *state->node->snapshot.entry_spot_name == state->spot_name) {
                native = std::make_shared<service::spot_t> (native_node->entry_spot ());
            } else {
                try {
                    auto spot = native_node->get_or_create_spot (rid);
                    native = std::make_shared<service::spot_t> (std::move (spot));
                }
                catch (const std::exception &error) {
                    throw framework_exception_t (
                      framework_error_kind_t::internal_failure,
                      "native spot facade creation failed for '" + state->spot_name + "' (rid='"
                        + rid + "'): " + error.what ());
                }
            }
            state->node->native_spots_by_id.emplace (rid, native);
        } else {
            native = found->second;
        }
        state->native_spot = native;
    }

    for (const auto &handler : state->handlers) {
        if (handler.kind == spot_handler_kind_t::subscription && !handler.topic.empty ()) {
            /* The node creates its subscription receiver lazily on the first
               subscription, and that creation waits a bounded time for the
               inproc attachment pipe. Under congestion the wait can expire, and
               the node reports every creation failure as "not supported"
               (ledger CPP-SPOT-SUB-ACT-001), so a spot that merely arrived at a
               busy moment would fail to be created at all. The failure is
               transient by nature: retry a few times before giving up. */
            constexpr int activation_attempts = 5;
            std::string last_error;
            bool activated = false;
            for (int attempt = 0; attempt < activation_attempts && !activated; ++attempt) {
                try {
                    native->set_subscription (
                                              state->node->snapshot.discovery_channel_name.value_or (
                                                state->node->snapshot.name),
                                              handler.topic);
                    activated = true;
                }
                catch (const std::exception &error) {
                    last_error = error.what ();
                    std::this_thread::sleep_for (std::chrono::milliseconds (100));
                }
            }
            if (!activated) {
                throw framework_exception_t (
                  framework_error_kind_t::internal_failure,
                  "native spot subscription activation failed for '" + state->spot_name
                    + "' (rid='" + rid + "', topic='" + handler.topic + "'): " + last_error);
            }
        }
    }
}

void report_spot_dispatch_error (const std::shared_ptr<detail::spot_node_builder_state_t> &state,
                                 dispatch_error_surface_t surface,
                                 dispatch_message_kind_t message_kind,
                                 dispatch_error_reason_t reason,
                                 dispatch_error_action_t action,
                                 std::optional<std::string> packet_name = std::nullopt,
                                 std::optional<std::string> topic = std::nullopt,
                                 std::optional<std::string> spot_id = std::nullopt,
                                 std::optional<std::string> actor_id = std::nullopt,
                                 std::exception_ptr exception = nullptr,
                                 std::optional<std::string> correlation_id = std::nullopt)
{
    if (!state) {
        return;
    }
    detail::dispatch_error_reporter_t (state->dispatch)
      .report (message_dispatch_error_event_t{
        surface, message_kind, reason, action, std::move (packet_name), std::nullopt,
        std::move (topic), std::move (spot_id), std::move (actor_id), std::nullopt,
        std::move (correlation_id), std::move (exception), std::nullopt, std::nullopt});
}

void report_spot_dispatch_trace (const std::shared_ptr<detail::spot_node_builder_state_t> &state,
                                 message_flow_outcome_t outcome,
                                 dispatch_error_surface_t surface,
                                 dispatch_message_kind_t message_kind,
                                 std::string_view packet_name = {},
                                 std::string_view topic = {},
                                 std::string_view spot_id = {},
                                 std::string_view actor_id = {},
                                 std::string_view correlation_id = {})
{
    if (!state) {
        return;
    }
    // string_view params + lazy build: callers pass cheap views; std::string is
    // only allocated inside the lambda after the gate passes (zero cost when off).
    detail::message_flow_tracer_t (state->dispatch).trace (outcome, [&] {
        auto field = [] (std::string_view value) -> std::optional<std::string> {
            if (value.empty ()) {
                return std::nullopt;
            }
            return std::string (value);
        };
        return message_flow_event_t{
          outcome,          surface,          message_kind, field (packet_name),
          std::nullopt,     field (topic),    field (correlation_id), std::nullopt,
          field (spot_id), field (actor_id), std::nullopt};
    });
}

zlink::message_t encode_spot_publish_frame (std::string channel_name,
                                            std::string packet_name,
                                            const std::string &topic,
                                            const zlink::message_t &payload)
{
    runtime::messaging::envelope_header_t header;
    header.kind = runtime::messaging::message_kind_t::publish;
    header.channel_name = std::move (channel_name);
    header.message_name = std::move (packet_name);
    header.topic = topic;
    header.source = header.channel_name;

    const auto header_bytes =
      runtime::messaging::envelope_codec_t{}.encode_header (header).to_bytes ();
    const auto body_bytes = payload.to_bytes ();
    std::vector<std::uint8_t> frame;
    frame.reserve (8 + header_bytes.size () + body_bytes.size ());
    frame.push_back (static_cast<std::uint8_t> ('Z'));
    frame.push_back (static_cast<std::uint8_t> ('L'));
    frame.push_back (static_cast<std::uint8_t> ('F'));
    frame.push_back (static_cast<std::uint8_t> ('E'));
    const auto header_size = static_cast<std::uint32_t> (header_bytes.size ());
    frame.push_back (static_cast<std::uint8_t> (header_size >> 24));
    frame.push_back (static_cast<std::uint8_t> (header_size >> 16));
    frame.push_back (static_cast<std::uint8_t> (header_size >> 8));
    frame.push_back (static_cast<std::uint8_t> (header_size));
    frame.insert (frame.end (), header_bytes.begin (), header_bytes.end ());
    frame.insert (frame.end (), body_bytes.begin (), body_bytes.end ());
    return zlink::message_t::from (frame);
}

void report_actor_handoff_request_trace (
  const std::shared_ptr<detail::spot_node_builder_state_t> &state,
  std::string marker,
  const actor_ref_t &actor_ref,
  std::string request_id,
  std::string transfer_id)
{
    if (!state || request_id.empty () || transfer_id.empty ()) {
        return;
    }
    detail::message_flow_tracer_t (state->dispatch).trace (
      message_flow_outcome_t::dispatched,
      [marker = std::move (marker), actor_ref, request_id = std::move (request_id),
       transfer_id = std::move (transfer_id)] () mutable {
          return message_flow_event_t{
            .outcome = message_flow_outcome_t::dispatched,
            .surface = dispatch_error_surface_t::spot_actor,
            .message_kind = dispatch_message_kind_t::actor_request,
            .packet_name = std::move (marker),
            .channel_name = "request",
            .correlation_id = std::move (request_id),
            .actor_id = std::string (actor_ref.actor_id ().value ()),
            .flow_id = std::move (transfer_id),
            .flow_origin = flow_origin_t::lifecycle};
      });
}

void decrement_actor_count_unlocked (detail::spot_context_state_t &state)
{
    if (state.actor_count > 0) {
        state.actor_count--;
    }
}

void erase_actor_route_unlocked (detail::spot_node_builder_state_t &state, const std::string &key)
{
    state.actor_spot_ids.erase (key);
    state.actor_routes.erase (key);
    state.actor_generations.erase (key);
    state.native_actors.erase (key);
}

std::uint64_t actor_generation_from_location (const actor_location_t &location)
{
    return location.actor_ref ? location.actor_ref->object_generation () : 0;
}

std::uint64_t owner_node_generation (const detail::spot_context_state_t &context)
{
    if (context.node) {
        if (const auto native = context.node->native_node.lock ()) {
            return native->status ().lifecycle_generation ();
        }
    }
    return 0;
}

std::uint64_t spot_generation (const detail::spot_context_state_t &context)
{
    if (const auto native = context.native_spot.lock ()) {
        return native->status ().lifecycle_generation ();
    }
    return 0;
}

std::uint64_t actor_membership_epoch (const actor_ref_t &actor,
                                      const detail::spot_context_state_t &context)
{
    if (!context.node) {
        return 0;
    }
    const auto found =
      context.node->core_actor_membership_epochs.find (std::string (actor.actor_id ().value ()));
    return found == context.node->core_actor_membership_epochs.end () ? 0 : found->second;
}

actor_location_t make_actor_location (const actor_ref_t &actor,
                                      const detail::spot_context_state_t &context)
{
    return actor_location_t{
      .mesh_name = context.node->snapshot.name,
      .actor_id = std::string (actor.actor_id ().value ()),
      .actor_type = std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor)),
      .actor_ref = actor,
      .owner_node_rid = zlink::routing_id_t::from (std::string (actor.node_rid ().value ())),
      .owner_node_generation = owner_node_generation (context),
      .spot_id = context.spot_id,
      .spot_generation = spot_generation (context),
      .spot_kind = zlink::spot_kind::user,
      .membership_epoch = actor_membership_epoch (actor, context)};
}

actor_location_t make_entry_actor_location (const actor_ref_t &actor,
                                            const detail::spot_context_state_t &context)
{
    return actor_location_t{.mesh_name = context.node->snapshot.name,
                            .actor_id = std::string (actor.actor_id ().value ()),
                            .actor_type = std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor)),
                            .actor_ref = actor,
                            .owner_node_rid =
                              zlink::routing_id_t::from (std::string (context.node_rid.value ())),
                            .owner_node_generation = owner_node_generation (context),
                            .spot_id = context.spot_id,
                            .spot_generation = spot_generation (context),
                            .spot_kind = zlink::spot_kind::entry,
                            .membership_epoch = actor_membership_epoch (actor, context)};
}

spot_location_t make_spot_location (const detail::spot_node_builder_state_t &state,
                                    const std::string &spot_name,
                                    const spot_id_t &spot_id)
{
    const auto kind = state.snapshot.entry_spot_name && *state.snapshot.entry_spot_name == spot_name
                        ? zlink::spot_kind::entry
                        : zlink::spot_kind::user;
    return spot_location_t{
      .mesh_name = state.snapshot.name,
      .spot_id = spot_id,
      .spot_type = spot_name,
      .node_rid = zlink::routing_id_t::from (detail::effective_spot_node_rid (state.snapshot)),
      .spot_kind = kind,
      .route_endpoint = state.snapshot.router_bind_endpoint};
}

void deactivate_actor_location (std::weak_ptr<detail::spot_node_builder_state_t> weak_state,
                                const actor_location_t &location)
{
    auto state = weak_state.lock ();
    if (!state) {
        return;
    }

    std::function<result_t<void> (const actor_ref_t &)> destroy_actor_registry;
    if (!location.actor_ref) {
        return;
    }
    const auto &actor = *location.actor_ref;
    const auto key = std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor)) + ":" + std::string (actor.actor_id ().value ());
    {
        std::lock_guard<std::recursive_mutex> node_lock (state->mutex);
        // A lost claim races with a completed transfer: after this node hands the
        // actor to another node it records the newer generation and Message
        // Message Follow route. A loss notification for an older generation is stale
        // and must not erase that newer record.
        const auto recorded = state->actor_generations.find (key);
        if (recorded != state->actor_generations.end ()
            && recorded->second > actor.object_generation ()) {
            return;
        }
        erase_actor_route_unlocked (*state, key);
        state->actor_created_keys.erase (key);
        state->destroyed_actor_keys.insert (key);
        state->actor_instances.erase (key);
        detail::erase_actor_instance_index_unlocked (
          *state, ::zlink::framework::detail::actor_ref_access_t::actor_type (actor),
          actor.actor_id ().value ());
        state->actor_mailboxes.erase (key);
        (void) state->dispatched_request_replies.erase_if ([&] (const auto &request_key) {
            return request_key.starts_with (actor_request_dedup_prefix (key));
        });
        destroy_actor_registry = state->destroy_actor_registry;
    }
    if (destroy_actor_registry) {
        (void) destroy_actor_registry (actor);
    }
}

result_t<void> claim_actor_location_before_activation (
  const std::shared_ptr<detail::spot_node_builder_state_t> &state,
  const actor_ref_t &committed,
  const detail::spot_context_state_t &context,
  bool &claimed,
  bool takeover = false)
{
    claimed = false;
    if (!state->location_lifecycle) {
        return result_t<void>::success ();
    }

    if (state->location_lifecycle->owns_actor (
          actor_location_key_t{context.node->snapshot.name,
                               std::string (committed.actor_id ().value ())})) {
        return result_t<void>::success ();
    }

    auto location = make_actor_location (committed, context);
    const auto claim_result = state->location_lifecycle->claim_actor (
      location,
      [weak_state = std::weak_ptr<detail::spot_node_builder_state_t> (state)] (
        const actor_location_t &lost) { deactivate_actor_location (weak_state, lost); },
      takeover);
    if (claim_result.status != location_write_status_t::stored) {
        return result_t<void>::failure (claim_result.status
                                            == location_write_status_t::rejected_conflict
                                          ? framework_error_kind_t::already_exists
                                          : framework_error_kind_t::internal_failure,
                                        "actor location claim failed");
    }
    claimed = true;
    return result_t<void>::success ();
}

result_t<void> claim_pending_actor_location_before_activation (
  const std::shared_ptr<detail::spot_node_builder_state_t> &state,
  const actor_ref_t &source_actor,
  const spot_id_t &source_spot_id,
  const actor_ref_t &committed,
  const detail::spot_context_state_t &target,
  bool &claimed)
{
    claimed = false;
    if (!state->location_lifecycle) {
        return result_t<void>::success ();
    }
    const bool source_is_local =
      !source_actor.node_rid ().empty ()
      && source_actor.node_rid ().value () == detail::effective_spot_node_rid (state->snapshot);
    if (source_is_local
        && state->location_lifecycle->owns_actor (
          actor_location_key_t{target.node->snapshot.name,
                               std::string (committed.actor_id ().value ())})) {
        return result_t<void>::success ();
    }
    auto location = make_actor_location (committed, target);
    location.actor_ref = source_actor;
    location.owner_node_rid =
      zlink::routing_id_t::from (std::string (source_actor.node_rid ().value ()));
    location.spot_id =
      source_spot_id.empty () ? std::string (source_actor.node_rid ().value ())
                              : source_spot_id;
    location.spot_kind =
      source_spot_id.empty () ? zlink::spot_kind::entry : zlink::spot_kind::user;
    const auto result = state->location_lifecycle->claim_actor (
      std::move (location),
      [weak_state = std::weak_ptr<detail::spot_node_builder_state_t> (state)] (
        const actor_location_t &lost) { deactivate_actor_location (weak_state, lost); },
      true);
    if (result.status != location_write_status_t::stored) {
        return result_t<void>::failure (result.status == location_write_status_t::rejected_conflict
                                          ? framework_error_kind_t::already_exists
                                          : framework_error_kind_t::internal_failure,
                                        "pending actor location claim failed");
    }
    claimed = true;
    return result_t<void>::success ();
}

void release_actor_location (detail::spot_node_builder_state_t &state, const actor_ref_t &actor)
{
    if (!state.location_lifecycle || ::zlink::framework::detail::actor_ref_access_t::empty (actor)) {
        return;
    }
    (void) state.location_lifecycle->release_actor (
      actor_location_key_t{state.snapshot.name, std::string (actor.actor_id ().value ())});
}

result_t<void> update_actor_location_after_move (detail::spot_node_builder_state_t &state,
                                                 const actor_ref_t &actor,
                                                 const detail::spot_context_state_t &context,
                                                 bool entry)
{
    if (!state.location_lifecycle || ::zlink::framework::detail::actor_ref_access_t::empty (actor)) {
        return result_t<void>::success ();
    }
    auto location =
      entry ? make_entry_actor_location (actor, context) : make_actor_location (actor, context);
    const auto tracked = state.location_lifecycle->owns_actor (
      actor_location_key_t{state.snapshot.name, std::string (actor.actor_id ().value ())});
    const auto updated = state.location_lifecycle->update_actor_location (std::move (location));
    if (updated.status != location_write_status_t::stored) {
        return result_t<void>::failure (framework_error_kind_t::internal_failure,
                                        "actor committed location update failed: status="
                                          + std::to_string (static_cast<int> (updated.status))
                                          + " tracked=" + (tracked ? "true" : "false"));
    }
    return result_t<void>::success ();
}

std::string spot_mesh_channel_name (const std::shared_ptr<detail::spot_context_state_t> &state)
{
    if (state && !state->spot_name.empty ()) {
        return state->spot_name;
    }
    return "spot-mesh";
}

std::optional<std::string>
optional_spot_route_channel_name (const std::shared_ptr<detail::spot_context_state_t> &state)
{
    if (!state || !state->node || !state->channel_runtime) {
        return std::nullopt;
    }
    std::lock_guard lock (state->channel_runtime->mutex);
    if (state->node->snapshot.spot_route_channel_name) {
        const auto &route_channel_name = *state->node->snapshot.spot_route_channel_name;
        if (state->channel_runtime->route_channels.find (route_channel_name)
            != state->channel_runtime->route_channels.end ()) {
            return route_channel_name;
        }
    }
    if (state->channel_runtime->route_channels.size () == 1) {
        return state->channel_runtime->route_channels.begin ()->first;
    }
    if (state->node->snapshot.accepted_route_channels.size () == 1) {
        const auto &route_channel_name =
          state->node->snapshot.accepted_route_channels.front ().channel_name;
        if (state->channel_runtime->route_channels.find (route_channel_name)
            != state->channel_runtime->route_channels.end ()) {
            return route_channel_name;
        }
    }
    return std::nullopt;
}

runtime::messaging::message_parts_t
encode_spot_route_parts (runtime::messaging::message_kind_t kind,
                         const std::string &route_channel_name,
                         const std::string &packet_name,
                         zlink::message_t payload,
                         std::chrono::milliseconds timeout,
                         std::map<std::string, std::string> metadata)
{
    runtime::messaging::client_call_codec_t codec;
    auto header = codec.create_envelope (kind, route_channel_name, packet_name, timeout);
    header.metadata = std::move (metadata);
    runtime::messaging::envelope_codec_t envelope;
    return envelope.encode_raw_body_parts (header, std::move (payload));
}

std::optional<std::uint64_t> resolve_target_spot_generation (
  const std::shared_ptr<detail::spot_node_builder_state_t> &state,
  const zlink::routing_id_t &target_node_rid,
  const spot_id_t &target_spot_id)
{
    if (!state) {
        return std::nullopt;
    }
    runtime::spot_address_resolver_t *resolver = nullptr;
    std::string mesh_name;
    std::shared_ptr<service::spot_t> local_spot;
    {
        std::lock_guard<std::recursive_mutex> lock (state->mutex);
        mesh_name = state->snapshot.name;
        resolver = state->spot_location_resolver;
        const auto local = state->native_spots_by_id.find (target_spot_id);
        if (local != state->native_spots_by_id.end ()
            && detail::effective_spot_node_rid (state->snapshot)
                 == target_node_rid.to_string ()) {
            local_spot = local->second;
        }
    }
    if (local_spot) {
        const auto generation = local_spot->status ().lifecycle_generation ();
        if (generation != 0) {
            return generation;
        }
    }
    if (!resolver) {
        return std::nullopt;
    }
    const auto address =
      resolver->resolve_spot_address (std::move (mesh_name), target_spot_id)
        .result ()
        .value ();
    if (!address || address->node_rid != target_node_rid
        || address->spot_generation == 0) {
        return std::nullopt;
    }
    return address->spot_generation;
}

framework_exception_t spot_request_terminal_exception (
  runtime::foundation::operation_terminal_t terminal)
{
    switch (terminal) {
        case runtime::foundation::operation_terminal_t::timed_out:
            return detail::make_boundary_exception (
              detail::boundary_error_t::timed_out, "SPOT mesh request timed out");
        case runtime::foundation::operation_terminal_t::cancelled:
            return detail::make_boundary_exception (
              detail::boundary_error_t::cancelled, "SPOT mesh request was cancelled");
        case runtime::foundation::operation_terminal_t::transport_failed:
            return detail::make_boundary_exception (
              detail::boundary_error_t::disconnected,
              "SPOT mesh request lost its connection");
        case runtime::foundation::operation_terminal_t::shutdown:
            return detail::make_boundary_exception (
              detail::boundary_error_t::shutdown,
              "SPOT mesh request stopped because the runtime is shutting down");
        case runtime::foundation::operation_terminal_t::completed:
            break;
    }
    return framework_exception_t (
      framework_error_kind_t::internal_failure,
      "SPOT mesh request completed without a terminal result");
}

task_t<runtime::messaging::message_parts_t> request_spot_parts_async (
  service::spot_handle_t egress,
  const zlink::routing_id_t &target_node_rid,
  const spot_id_t &target_spot_id,
  std::uint64_t target_generation,
  runtime::messaging::message_parts_t parts,
  std::chrono::milliseconds timeout)
{
    auto source = std::make_shared<
      detail::task_completion_source_t<runtime::messaging::message_parts_t>> ();
    auto output = source->task ();
    try {
        const auto &native_parts = parts.items ();
        if (native_parts.empty ()) {
            source->complete (result_t<runtime::messaging::message_parts_t>::failure (
              framework_error_kind_t::protocol_error,
              "SPOT mesh request requires at least one message part"));
            return output;
        }
        service::operation_id_t operation_id;
        const auto submitted = egress.request_to_spot (
          target_node_rid, target_spot_id, target_generation,
          native_parts, operation_id, zlink::send_flags_t::none, timeout, {},
          [source] (runtime::foundation::operation_terminal_t terminal,
                    result_t<std::vector<zlink::message_t>> decoded) mutable {
              if (terminal != runtime::foundation::operation_terminal_t::completed) {
                  source->complete (detail::result_access_t::failure<
                    runtime::messaging::message_parts_t> (
                    spot_request_terminal_exception (terminal)));
                  return;
              }
              if (!decoded) {
                  source->complete (detail::propagate_failure<
                    runtime::messaging::message_parts_t> (
                    decoded, "SPOT mesh request reply decode failed"));
                  return;
              }
              source->complete (result_t<runtime::messaging::message_parts_t>::success (
                runtime::messaging::message_parts_t (std::move (decoded.value()))));
          });
        if (submitted != zlink::submit_result_t::ok) {
            source->complete (detail::result_access_t::failure<
              runtime::messaging::message_parts_t> (
              runtime::messaging::map_submit_result_exception (
                submitted, "SPOT mesh request was not submitted")));
            return output;
        }
    }
    catch (const framework_exception_t &error) {
        source->complete (detail::result_access_t::failure<
          runtime::messaging::message_parts_t> (error));
    }
    catch (const zlink::request_error_t &error) {
        source->complete (detail::result_access_t::failure<
          runtime::messaging::message_parts_t> (
          runtime::messaging::map_request_result_exception (
            error.result (), error.what ())));
    }
    catch (const zlink::submit_error_t &error) {
        source->complete (detail::result_access_t::failure<
          runtime::messaging::message_parts_t> (
          runtime::messaging::map_submit_result_exception (
            error.result (), error.what ())));
    }
    catch (const std::exception &error) {
        source->complete (result_t<runtime::messaging::message_parts_t>::failure (
          framework_error_kind_t::internal_failure, error.what ()));
    }
    return output;
}

task_t<runtime::messaging::message_parts_t>
request_spot_mesh_parts (const std::shared_ptr<detail::spot_context_state_t> &state,
                         node_rid_t node_rid,
                         spot_id_t spot_id,
                         runtime::messaging::message_parts_t parts,
                         std::chrono::milliseconds timeout)
{
    if (!state) {
        return task_t<runtime::messaging::message_parts_t> (
          result_t<runtime::messaging::message_parts_t>::failure (
            framework_error_kind_t::protocol_error, "SPOT context is not configured"));
    }
    auto native = state->native_spot.lock ();
    if (!native) {
        return task_t<runtime::messaging::message_parts_t> (
          result_t<runtime::messaging::message_parts_t>::failure (
            framework_error_kind_t::not_found,
            "SPOT mesh route requires a running native Spot"));
    }
    try {
        const auto target_node_rid =
          zlink::routing_id_t::from (std::string (node_rid.value ()));
        const auto target_generation =
          resolve_target_spot_generation (state->node, target_node_rid, spot_id);
        if (!target_generation) {
            return task_t<runtime::messaging::message_parts_t> (
              result_t<runtime::messaging::message_parts_t>::failure (
                framework_error_kind_t::not_found,
                "SPOT mesh request target generation is unavailable"));
        }
        return request_spot_parts_async (
          *native, target_node_rid, spot_id, *target_generation, std::move (parts), timeout);
    }
    catch (const framework_exception_t &error) {
        return task_t<runtime::messaging::message_parts_t> (
          detail::result_access_t::failure<runtime::messaging::message_parts_t> (error));
    }
    catch (const std::exception &error) {
        return task_t<runtime::messaging::message_parts_t> (
          result_t<runtime::messaging::message_parts_t>::failure (
            framework_error_kind_t::internal_failure, error.what ()));
    }
}

task_t<zlink::message_t>
request_spot_mesh_message (const std::shared_ptr<detail::spot_context_state_t> &state,
                           node_rid_t node_rid,
                           spot_id_t spot_id,
                           runtime::messaging::message_parts_t parts,
                           std::chrono::milliseconds timeout)
{
    auto source = std::make_shared<detail::task_completion_source_t<zlink::message_t>> ();
    auto output = source->task ();
    auto reply = request_spot_mesh_parts (
      state, std::move (node_rid), std::move (spot_id), std::move (parts), timeout);
    detail::observe_task_completion (
      reply,
      [source] (const result_t<runtime::messaging::message_parts_t> &reply_result) mutable {
          try {
          if (!reply_result) {
              source->complete (detail::propagate_failure<zlink::message_t> (
                reply_result, "SPOT mesh request failed"));
              return;
          }
          runtime::messaging::envelope_codec_t envelope;
          auto reply_header = envelope.decode_header (reply_result.value ());
          if (!reply_header) {
              source->complete (result_t<zlink::message_t>::failure (
                reply_header.error_kind (),
                reply_header.error () ? reply_header.error ()->what ()
                                      : "SPOT route reply header decode failed"));
              return;
          }
          if (reply_header.value ().kind == runtime::messaging::message_kind_t::error) {
              runtime::messaging::request_failure_mapper_t failure_mapper;
              source->complete (detail::result_access_t::failure<zlink::message_t> (
                failure_mapper.error_header_exception (
                  reply_header.value ().error_code.value_or ("request_failed"),
                  reply_header.value ().error_message.value_or ("SPOT route request failed"),
                  "SPOT route request")));
              return;
          }
          auto body = envelope.decode_body (reply_result.value ());
          if (!body) {
              source->complete (detail::propagate_failure<zlink::message_t> (
                body, "SPOT route reply body decode failed"));
              return;
          }
          source->complete (result_t<zlink::message_t>::success (body.value ()));
          }
          catch (const framework_exception_t &error) {
              source->complete (
                detail::result_access_t::failure<zlink::message_t> (error));
          }
          catch (const std::exception &error) {
              source->complete (result_t<zlink::message_t>::failure (
                framework_error_kind_t::internal_failure, error.what ()));
          }
          catch (...) {
              source->complete (result_t<zlink::message_t>::failure (
                framework_error_kind_t::internal_failure,
                "SPOT route reply processing failed"));
          }
      });
    return output;
}

} // namespace

namespace detail
{

bool spot_context_state_t::idle_quiescent () const
{
    if (actor_count != 0 || relocation_boundary_active
        || relocation_ready_deferred || !queued_routed_packets.empty ()) {
        return false;
    }
    if (!serial_queue
        || serial_queue->pending_count (runtime::serial_work_lane_t::application) != 0
        || serial_queue->pending_count (runtime::serial_work_lane_t::lifecycle) != 0) {
        return false;
    }
    for (const auto &timer : timers) {
        if (!timer)
            continue;
        std::lock_guard<std::mutex> timer_lock (timer->mutex);
        if (!timer->disposed
            && (timer->running || timer->pending_fire
                || timer->pending_fire_count != 0)) {
            return false;
        }
    }
    return true;
}

bool spot_context_state_t::enter_callback ()
{
    std::lock_guard<std::mutex> lock (callback_mutex);
    if (callback_admission_closed || idle_eviction_in_progress) {
        return false;
    }
    if (callback_depth == 0) {
        callback_thread = std::this_thread::get_id ();
    }
    ++callback_depth;
    return true;
}

void spot_context_state_t::leave_callback () noexcept
{
    bool should_close = false;
    {
        std::lock_guard<std::mutex> lock (callback_mutex);
        if (callback_depth > 0) {
            --callback_depth;
        }
        if (callback_depth == 0) {
            callback_thread = std::thread::id ();
            should_close = close_requested;
        }
    }
    if (should_close) {
        try {
            (void) close_now ();
        }
        catch (...) {
        }
    }
}

bool spot_context_state_t::is_current_callback_thread () const
{
    std::lock_guard<std::mutex> lock (callback_mutex);
    return callback_depth > 0 && callback_thread == std::this_thread::get_id ();
}

bool spot_context_state_t::try_post_serial (
  std::string name,
  std::function<void ()> work,
  runtime::serial_work_options_t options)
{
    if (admission_blocked ()) {
        return false;
    }
    if (!serial_queue) {
        work ();
        return true;
    }
    return serial_queue->try_post (
      std::move (name), std::move (work), std::move (options));
}

bool spot_context_state_t::try_post_serial_after_current_turn (
  std::string name,
  std::function<void ()> work,
  runtime::serial_work_options_t options)
{
    if (admission_blocked ()) {
        return false;
    }
    if (!serial_queue) {
        work ();
        return true;
    }
    /* A handler may defer relocation readiness before its turn completes.
     * Work posted as a normal next turn then observes that readiness fence and
     * is rejected. Run lifecycle cleanup in the queue's after-active phase so
     * it executes after the borrowed handler reference is released but before
     * the normal next-turn continuation. */
    const auto current_turn = detail::capture_current_serial_turn ();
    if (owns_current_serial_turn ()
        && current_turn && !current_turn->is_after_active_phase ()) {
        return serial_queue->try_post_deferred (std::move (name), std::move (work));
    }
    return serial_queue->try_post (
      std::move (name), std::move (work), std::move (options));
}

bool spot_context_state_t::try_post_serial_async (
  std::string name,
  runtime::serial_execution_queue_t::async_work_t work,
  runtime::serial_work_options_t options)
{
    if (admission_blocked ()) {
        return false;
    }
    if (!serial_queue) {
        work ([] (std::function<void ()> completion) {
            if (completion) {
                completion ();
            }
        });
        return true;
    }
    return serial_queue->try_post_async (
      std::move (name), std::move (work), std::move (options));
}

result_t<void> spot_context_state_t::run_serial_task (
  std::string name, std::function<task_t<void> ()> work)
{
    if (!work) {
        return result_t<void>::success ();
    }
    if (admission_blocked ()) {
        return result_t<void>::failure (
          framework_error_kind_t::unavailable,
          "spot is closing for idle eviction");
    }
    if (!serial_queue || is_current_callback_thread () || owns_current_serial_turn ()) {
        try {
            auto task = work ();
            return task.result ();
        }
        catch (const framework_exception_t &error) {
            return detail::result_access_t::failure<void> (error);
        }
        catch (const std::exception &error) {
            return result_t<void>::failure (framework_error_kind_t::internal_failure,
                                            error.what ());
        }
        catch (...) {
            return result_t<void>::failure (framework_error_kind_t::internal_failure,
                                            "spot lifecycle callback failed");
        }
    }

    detail::task_completion_source_t<void> completion;
    auto result = completion.task ();
    const auto posted = try_post_serial_async (
      std::move (name), [this, work = std::move (work), completion] (auto complete) mutable {
          if (!enter_callback ()) {
              complete ([completion] () mutable {
                  completion.complete (
                    detail::boundary_failure<void> (
                      detail::boundary_error_t::closed,
                      "spot activation is closed"));
              });
              return;
          }
          auto turn = detail::capture_current_serial_turn ();
          try {
              auto task = work ();
              detail::observe_task_completion (
                task, [this, completion, turn, complete] (const result_t<void> &value) mutable {
                    const auto final_result =
                      value ? result_t<void>::success ()
                            : result_t<void>::failure (
                                value.error_kind (),
                                value.error () != nullptr ? value.error ()->what ()
                                                          : "spot lifecycle callback failed");
                    auto finish = [this, completion, final_result] () mutable {
                        leave_callback ();
                        completion.complete (final_result);
                    };
                    if (turn && turn->released ()) {
                        finish ();
                    } else {
                        complete (std::move (finish));
                    }
                });
          }
          catch (const framework_exception_t &error) {
              complete ([this, completion, error] () mutable {
                  leave_callback ();
                  completion.complete (detail::result_access_t::failure<void> (error));
              });
          }
          catch (const std::exception &error) {
              const auto message = std::string (error.what ());
              complete ([this, completion, message] () mutable {
                  leave_callback ();
                  completion.complete (result_t<void>::failure (
                    framework_error_kind_t::internal_failure, message));
              });
          }
          catch (...) {
              complete ([this, completion] () mutable {
                  leave_callback ();
                  completion.complete (result_t<void>::failure (
                    framework_error_kind_t::internal_failure,
                    "spot lifecycle callback failed"));
              });
          }
      },
      runtime::serial_work_options_t{
        runtime::serial_work_lane_t::lifecycle,
        runtime::serial_execution_queue_t::fixed_work_byte_cost});
    if (!posted) {
        return result_t<void>::failure (framework_error_kind_t::capacity_exceeded,
                                        "spot serial queue is full");
    }
    return result.result ();
}

bool spot_context_state_t::run_serial_sync (std::string name, std::function<void ()> work)
{
    if (!work) {
        return true;
    }
    if (admission_blocked ()) {
        return false;
    }
    if (is_current_callback_thread () || owns_current_serial_turn ()) {
        work ();
        return true;
    }

    std::exception_ptr error;
    bool callback_admitted = false;
    const bool posted = try_post_serial (std::move (name), [&] {
        callback_admitted = enter_callback ();
        if (!callback_admitted) {
            return;
        }
        try {
            work ();
        }
        catch (...) {
            error = std::current_exception ();
        }
        leave_callback ();
    }, runtime::serial_work_options_t{
      runtime::serial_work_lane_t::lifecycle,
      runtime::serial_execution_queue_t::fixed_work_byte_cost});
    if (!posted) {
        return false;
    }
    drain_serial ();
    if (error) {
        std::rethrow_exception (error);
    }
    return callback_admitted;
}

bool spot_context_state_t::owns_current_serial_turn () const
{
    const auto turn = detail::capture_current_serial_turn ();
    return turn && serial_queue && turn->belongs_to (serial_queue.get ());
}

void spot_context_state_t::defer_relocation_ready ()
{
    if (execution_mode != user_spot_execution_mode_t::spot_wide
        || relocation_readiness
             != spot_relocation_readiness_mode_t::application_signaled
        || is_entry_spot () || is_instance_spot ()
        || !owns_current_serial_turn ()) {
        throw framework_exception_t (
          framework_error_kind_t::not_configured,
          "relocation readiness can be deferred only from an "
          "application-signaled SpotWide User Spot turn");
    }
    bool complete_without_relocation = false;
    {
        std::lock_guard lock (callback_mutex);
        if (relocation_ready_deferred) {
            throw framework_exception_t (
              framework_error_kind_t::not_configured,
              "relocation readiness is already deferred");
        }
        relocation_ready_deferred = true;
        complete_without_relocation =
          !relocation_boundary_active;
    }
    if (complete_without_relocation
        && !try_post_serial (
          "relocation-ready-continued", [this] {
              complete_relocation_ready (
                spot_relocation_ready_outcome_t::continued);
          },
          runtime::serial_work_options_t{
            runtime::serial_work_lane_t::lifecycle,
            runtime::serial_execution_queue_t::fixed_work_byte_cost})) {
        std::lock_guard lock (callback_mutex);
        relocation_ready_deferred = false;
        throw framework_exception_t (
          framework_error_kind_t::capacity_exceeded,
          "relocation readiness completion queue is full");
    }
}

void spot_context_state_t::ensure_relocation_turn_open () const
{
    const auto current_turn = detail::capture_current_serial_turn ();
    if (!owns_current_serial_turn ()
        || !current_turn || current_turn->is_after_active_phase ())
        return;
    std::lock_guard lock (callback_mutex);
    if (relocation_ready_deferred) {
        throw framework_exception_t (
          framework_error_kind_t::not_configured,
          "Framework operations are not allowed after relocation "
          "readiness is deferred in the current Spot turn");
    }
}

void spot_context_state_t::complete_relocation_ready (
  spot_relocation_ready_outcome_t outcome)
{
    bool pending = false;
    {
        std::lock_guard lock (callback_mutex);
        pending = relocation_ready_deferred;
    }
    if (!pending)
        return;
    (void) run_serial_sync (
      "relocation-ready-completed", [this, outcome] {
          std::shared_ptr<void> instance;
          std::function<void (
            void *, const spot_relocation_ready_completion_t &)>
            callback;
          {
              std::lock_guard lock (callback_mutex);
              if (!relocation_ready_deferred)
                  return;
              relocation_ready_deferred = false;
              instance = spot_instance;
              callback =
                lifecycle.on_relocation_ready_completed;
          }
          if (instance && callback) {
              callback (
                instance.get (),
                spot_relocation_ready_completion_t{outcome});
          }
      });
}

void spot_context_state_t::drain_serial ()
{
    if (serial_queue) {
        serial_queue->drain ();
    }
}

} // namespace detail

node_rid_t::node_rid_t (std::string value) : _value (std::move (value))
{
}

node_rid_t node_rid_t::from_string (std::string value)
{
    return node_rid_t (std::move (value));
}

std::string_view node_rid_t::value () const noexcept
{
    return _value;
}

bool node_rid_t::empty () const noexcept
{
    return _value.empty ();
}

spot_context_t::erased_request_call_t::erased_request_call_t (framework_exception_t error) :
    _error (std::move (error))
{
}

spot_context_t::erased_request_call_t::erased_request_call_t (
  std::string packet_name,
  serializer_registry_t *serializers,
  std::function<task_t<zlink::message_t> (const std::string &,
                                          std::chrono::milliseconds,
                                          const request_call_t<zlink::message_t>::metadata_map_t &)>
    submit,
  request_call_t<zlink::message_t>::preflight_fn_t preflight) :
    _packet_name (std::move (packet_name)), _serializers (serializers),
    _submit (std::move (submit)), _preflight (std::move (preflight))
{
}

spot_context_t::spot_context_t () : _state (std::make_shared<detail::spot_context_state_t> ())
{
}

spot_context_t::spot_context_t (std::shared_ptr<detail::spot_context_state_t> state) :
    _state (std::move (state))
{
    if (_state) {
        _worker_scheduler = _state->worker_scheduler;
    }
}

spot_context_t::~spot_context_t () = default;
spot_context_t::spot_context_t (spot_context_t &&) noexcept = default;

spot_relocation_ready_call_t::spot_relocation_ready_call_t (
  std::shared_ptr<detail::spot_context_state_t> state) :
    _state (std::move (state))
{
}

spot_relocation_ready_call_t::~spot_relocation_ready_call_t () = default;
spot_relocation_ready_call_t::spot_relocation_ready_call_t (
  spot_relocation_ready_call_t &&) noexcept = default;

void spot_relocation_ready_call_t::defer ()
{
    if (!_state) {
        throw framework_exception_t (
          framework_error_kind_t::not_configured,
          "relocation readiness call is not bound to a Spot");
    }
    _state->defer_relocation_ready ();
}

entry_spot_context_t::entry_spot_context_t () = default;

entry_spot_context_t::entry_spot_context_t (const spot_context_t &context) :
    spot_context_t (context._state)
{
}

entry_spot_context_t::entry_spot_context_t (std::shared_ptr<detail::spot_context_state_t> state) :
    spot_context_t (std::move (state))
{
}

entry_spot_context_t::~entry_spot_context_t () = default;
entry_spot_context_t::entry_spot_context_t (entry_spot_context_t &&) noexcept = default;

instance_spot_context_t::instance_spot_context_t () = default;

instance_spot_context_t::instance_spot_context_t (const spot_context_t &context) :
    spot_context_t (context._state)
{
}

instance_spot_context_t::instance_spot_context_t (
  std::shared_ptr<detail::spot_context_state_t> state) :
    spot_context_t (std::move (state))
{
}

instance_spot_context_t::~instance_spot_context_t () = default;
instance_spot_context_t::instance_spot_context_t (instance_spot_context_t &&) noexcept = default;

node_rid_t spot_context_t::node_rid () const
{
    return _state->node_rid;
}

std::string_view spot_context_t::mesh_name () const
{
    return _state->mesh_name;
}

spot_id_t spot_context_t::spot_id () const
{
    return _state->spot_id;
}

std::uint64_t spot_context_t::object_generation () const noexcept
{
    return _state ? _state->object_generation : 0;
}

bool spot_context_t::has_same_source_fence (const spot_context_t &other) const noexcept
{
    return _state && _state == other._state;
}

std::string spot_context_t::spot_name () const
{
    return _state->spot_name;
}

spot_handler_registry_t spot_context_t::handlers ()
{
    return spot_handler_registry_t (_state);
}

spot_relocation_ready_call_t spot_context_t::relocation_ready ()
{
    return spot_relocation_ready_call_t (_state);
}

spot_manager_t spot_context_t::manager () const
{
    return spot_manager_t (_state->node, _state);
}

channel_client_t spot_context_t::outbound () const
{
    if (!_state->channel_runtime) {
        throw framework_exception_t (framework_error_kind_t::internal_failure,
                                     "SPOT channel outbound runtime is not configured");
    }
    auto state = std::weak_ptr<detail::spot_context_state_t> (_state);
    return channel_client_t (message_bus_t (
      _state->channel_runtime,
      [state] {
          const auto locked = state.lock ();
          if (!locked)
              return result_t<void>::failure (
                framework_error_kind_t::not_configured,
                "Spot execution context is no longer available");
          try {
              locked->ensure_relocation_turn_open ();
              return result_t<void>::success ();
          }
          catch (const framework_exception_t &error) {
              return detail::result_access_t::failure<void> (
                error);
          }
      }));
}

void spot_context_t::ensure_submission_open () const
{
    if (!_state) {
        throw framework_exception_t (
          framework_error_kind_t::not_configured,
          "Spot execution context is not configured");
    }
    _state->ensure_relocation_turn_open ();
}

std::function<result_t<void> ()>
spot_context_t::submission_preflight () const
{
    auto state =
      std::weak_ptr<detail::spot_context_state_t> (_state);
    return [state] {
        const auto locked = state.lock ();
        if (!locked) {
            return result_t<void>::failure (
              framework_error_kind_t::not_configured,
              "Spot execution context is no longer available");
        }
        try {
            locked->ensure_relocation_turn_open ();
            return result_t<void>::success ();
        }
        catch (const framework_exception_t &error) {
            return detail::result_access_t::failure<void> (
              error);
        }
    };
}

task_t<bool> spot_context_t::close ()
{
    return close_erased ();
}

task_t<bool> spot_context_t::close_erased ()
{
    _state->ensure_relocation_turn_open ();
    if (!_state || !_state->node) {
        co_return result_t<bool>::success (false);
    }
    {
        std::lock_guard<std::recursive_mutex> node_lock (_state->node->mutex);
        if (_state->closed || _state->actor_count != 0) {
            co_return result_t<bool>::success (false);
        }
        co_return result_t<bool>::success (_state->close_now ());
    }
}

void detail::spot_context_state_t::cancel_timers () noexcept
{
    detail::timer_runtime_t::cancel_all (*this);
}

task_t<actor_ref_t> spot_context_t::leave_actor_erased (
  const actor_ref_t &actor_ref,
  std::type_index actor_type,
  void *actor,
  std::function<void (void *, const actor_ref_t &)> update_actor_ref)
{
    report_spot_dispatch_trace (
      _state ? _state->node : nullptr, message_flow_outcome_t::received,
      dispatch_error_surface_t::spot_actor, dispatch_message_kind_t::actor_request,
      "actor_leave", {}, _state ? _state->spot_id : std::string_view{},
      ::zlink::framework::detail::actor_ref_access_t::empty (actor_ref)
        ? std::string_view{}
        : actor_ref.actor_id ().value ());
    ensure_submission_open ();
    if (!_state || !_state->node || ::zlink::framework::detail::actor_ref_access_t::empty (actor_ref)) {
        return task_t<actor_ref_t> (result_t<actor_ref_t>::failure (
          framework_error_kind_t::not_found, "actor ref is empty"));
    }
    if (_state->is_current_callback_thread ()) {
        report_spot_dispatch_trace (
          _state->node, message_flow_outcome_t::dispatched,
          dispatch_error_surface_t::spot_actor, dispatch_message_kind_t::actor_request,
          "actor_leave_deferred", {}, _state->spot_id, actor_ref.actor_id ().value ());
        auto state = _state;
        const auto deferred_ref = actor_ref;
        const auto posted = state->try_post_serial_after_current_turn (
          "spot-actor-leave-after-handler",
          [state, deferred_ref, actor_type, actor,
           update_actor_ref = std::move (update_actor_ref)] () mutable {
              report_spot_dispatch_trace (
                state->node, message_flow_outcome_t::dispatched,
                dispatch_error_surface_t::spot_actor,
                dispatch_message_kind_t::actor_request,
                "actor_leave_deferred_execute", {}, state->spot_id,
                deferred_ref.actor_id ().value ());
              /* The deferred queue item is only an admission trigger. Waiting
               * for the full leave operation here can consume every serial
               * worker while the leave callback waits for an outbound reply.
               * Run that blocking boundary on the Framework call executor so
               * the serial queue remains available for the callback and its
               * continuation. */
              const auto submitted = detail::submit_blocking_call (
                [state, deferred_ref, actor_type, actor,
                 update_actor_ref = std::move (update_actor_ref)] () mutable {
                    try {
                        const auto completed =
                          spot_context_t (state)
                            .leave_actor_erased (deferred_ref, actor_type, actor,
                                                 std::move (update_actor_ref))
                            .result ();
                        report_spot_dispatch_trace (
                          state->node,
                          completed ? message_flow_outcome_t::replied
                                    : message_flow_outcome_t::error,
                          dispatch_error_surface_t::spot_actor,
                          dispatch_message_kind_t::actor_request,
                          completed ? "actor_leave_deferred_complete"
                                    : "actor_leave_deferred_failed",
                          {}, state->spot_id, deferred_ref.actor_id ().value ());
                    }
                    catch (...) {
                        report_spot_dispatch_trace (
                          state->node, message_flow_outcome_t::error,
                          dispatch_error_surface_t::spot_actor,
                          dispatch_message_kind_t::actor_request,
                          "actor_leave_deferred_exception", {}, state->spot_id,
                          deferred_ref.actor_id ().value ());
                    }
                });
              if (!submitted) {
                  report_spot_dispatch_trace (
                    state->node, message_flow_outcome_t::error,
                    dispatch_error_surface_t::spot_actor,
                    dispatch_message_kind_t::actor_request,
                    "actor_leave_deferred_rejected", {}, state->spot_id,
                    deferred_ref.actor_id ().value ());
              }
          },
          runtime::serial_work_options_t{
            runtime::serial_work_lane_t::lifecycle,
            runtime::serial_execution_queue_t::fixed_work_byte_cost});
        if (!posted) {
            return task_t<actor_ref_t> (result_t<actor_ref_t>::failure (
              framework_error_kind_t::capacity_exceeded,
              "Actor leave could not be scheduled after the current handler"));
        }
        return task_t<actor_ref_t> (
          result_t<actor_ref_t>::success (actor_ref));
    }
    /* on_leave_actor may close the source Spot, which detaches its context
     * from the node while this operation has released the node lock. Keep the
     * node owner independently of the source context for the whole move. */
    auto node = _state->node;
    std::unique_lock<std::recursive_mutex> node_lock (node->mutex);
    const auto key =
      std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor_ref)) + ":" + std::string (actor_ref.actor_id ().value ());
    const auto found_location = node->actor_spot_ids.find (key);
    if (found_location == node->actor_spot_ids.end ()) {
        report_spot_dispatch_trace (
          node, message_flow_outcome_t::replied,
          dispatch_error_surface_t::spot_actor, dispatch_message_kind_t::actor_request,
          "actor_leave_noop", {}, _state->spot_id, actor_ref.actor_id ().value ());
        return task_t<actor_ref_t> (result_t<actor_ref_t>::success (actor_ref));
    }
    if (found_location->second != _state->spot_id) {
        return task_t<actor_ref_t> (result_t<actor_ref_t>::failure (
          framework_error_kind_t::not_found, "actor is not joined to this SPOT"));
    }

    const auto found_generation = node->actor_generations.find (key);
    if (found_generation != node->actor_generations.end ()
        && found_generation->second != actor_ref.object_generation ()) {
        return task_t<actor_ref_t> (detail::boundary_failure<actor_ref_t> (detail::boundary_error_t::stale_generation, "actor generation is stale"));
    }

    if (_state->node_rid.empty () || actor_ref.node_rid ().value () != _state->node_rid.value ()) {
        try {
            auto entry_join = node->actor_entry_spot_join;
            auto &source_state = *_state;
            decrement_actor_count_unlocked (source_state);
            erase_actor_route_unlocked (*node, key);
            const auto source_admission = source_state.actor_admissions.find (actor_type);
            if (source_admission != source_state.actor_admissions.end ()
                && source_admission->second.on_leave_actor && source_state.spot_instance) {
                node_lock.unlock ();
                const auto completed = source_state.run_serial_task (
                  "spot-lifecycle-leave", [&] {
                      return source_admission->second.on_leave_actor (
                        source_state.spot_instance.get (), actor);
                  });
                node_lock.lock ();
                if (!completed) {
                    return task_t<actor_ref_t> (result_t<actor_ref_t>::failure (
                      completed.error_kind (), completed.error () != nullptr
                                                  ? completed.error ()->what ()
                                                  : "spot actor leave callback failed"));
                }
            }
            if (!entry_join) {
                return task_t<actor_ref_t> (result_t<actor_ref_t>::success (actor_ref));
            }
            std::optional<zlink::message_t> actor_snapshot;
            const auto actor_factory =
              node->actor_factories.find (std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor_ref)));
            if (actor_factory != node->actor_factories.end ()
                && source_state.channel_runtime && source_state.channel_runtime->serializers) {
                actor_snapshot = actor_factory->second.serialize_instance (
                  actor, *source_state.channel_runtime->serializers);
            }
            node_lock.unlock ();
            auto joined =
              entry_join (actor_ref, actor_ref.node_rid (), zlink::message_t{}, actor_snapshot);
            node_lock.lock ();
            if (!joined) {
                const auto *error = joined.error ();
                return task_t<actor_ref_t> (result_t<actor_ref_t>::failure (
                  joined.error_kind (),
                  error != nullptr ? error->what () : "remote entry spot join failed"));
            }
            if (update_actor_ref) {
                update_actor_ref (actor, joined.value ().actor);
            }
            return task_t<actor_ref_t> (result_t<actor_ref_t>::success (joined.value ().actor));
        }
        catch (const framework_exception_t &error) {
            return task_t<actor_ref_t> (
              detail::result_access_t::failure<actor_ref_t> (error));
        }
        catch (const std::exception &error) {
            return task_t<actor_ref_t> (result_t<actor_ref_t>::failure (
              framework_error_kind_t::internal_failure, error.what ()));
        }
        catch (...) {
            return task_t<actor_ref_t> (result_t<actor_ref_t>::failure (
              framework_error_kind_t::internal_failure, "remote actor leave callback failed"));
        }
    }

    if (!node->snapshot.entry_spot_name) {
        return task_t<actor_ref_t> (result_t<actor_ref_t>::failure (
          framework_error_kind_t::not_found, "entry spot is not registered"));
    }

    const auto entry_id =
      node->spot_ids_by_name.find (*node->snapshot.entry_spot_name);
    if (entry_id == node->spot_ids_by_name.end ()) {
        return task_t<actor_ref_t> (result_t<actor_ref_t>::failure (
          framework_error_kind_t::not_found, "entry spot is not created"));
    }
    const auto entry_context =
      node->spot_contexts_by_id.find (entry_id->second);
    if (entry_context == node->spot_contexts_by_id.end ()
        || !entry_context->second._state->spot_instance) {
        return task_t<actor_ref_t> (result_t<actor_ref_t>::failure (
          framework_error_kind_t::not_found, "entry spot context is not registered"));
    }
    /* Lifecycle callbacks below temporarily release the node lock and may
     * create or close Spot contexts. Keep the Entry Spot state itself alive;
     * retaining an unordered_map iterator across that boundary is invalid. */
    auto entry_state_owner = entry_context->second._state;

    try {
        auto &source_state = *_state;
        decrement_actor_count_unlocked (source_state);
        erase_actor_route_unlocked (*node, key);
        const auto source_admission = source_state.actor_admissions.find (actor_type);
        if (source_admission != source_state.actor_admissions.end ()
            && source_admission->second.on_leave_actor && source_state.spot_instance) {
            node_lock.unlock ();
            const auto completed = source_state.run_serial_task (
              "spot-lifecycle-leave", [&] {
                  return source_admission->second.on_leave_actor (
                    source_state.spot_instance.get (), actor);
              });
            node_lock.lock ();
            if (!completed) {
                return task_t<actor_ref_t> (result_t<actor_ref_t>::failure (
                  completed.error_kind (), completed.error () != nullptr
                                              ? completed.error ()->what ()
                                              : "spot actor leave callback failed"));
            }
        } else {
            const auto source_left = source_state.on_leave_actor_callbacks.find (actor_type);
            if (source_left != source_state.on_leave_actor_callbacks.end ()
                && source_state.spot_instance) {
                node_lock.unlock ();
                const auto completed = source_state.run_serial_task (
                  "spot-lifecycle-leave", [&] {
                      return source_left->second (source_state.spot_instance.get (), actor);
                  });
                node_lock.lock ();
                if (!completed) {
                    return task_t<actor_ref_t> (result_t<actor_ref_t>::failure (
                      completed.error_kind (), completed.error () != nullptr
                                                  ? completed.error ()->what ()
                                                  : "spot actor leave callback failed"));
                }
            }
        }

        auto &entry_state = *entry_state_owner;
        const auto committed =
          ::zlink::framework::detail::actor_ref_access_t::make (node_rid_t::from_string (std::string (_state->node_rid.value ())),
                       std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor_ref)), std::string (actor_ref.actor_id ().value ()),
                       actor_ref.object_generation ());
        (void) update_actor_location_after_move (*node, committed, entry_state, true);
        detail::record_actor_context_route_unlocked (*node, key,
                                                     std::string (_state->node_rid.value ()),
                                                     entry_state, committed.object_generation ());
        if (update_actor_ref) {
            update_actor_ref (actor, committed);
        }
        if (node->update_actor_registry_ref) {
            auto updated = node->update_actor_registry_ref (committed);
            if (!updated) {
                const auto *error = updated.error ();
                return task_t<actor_ref_t> (result_t<actor_ref_t>::failure (
                  updated.error_kind (),
                  error != nullptr ? error->what () : "actor registry ref update failed"));
            }
        }

        const auto entry_admission = entry_state.actor_admissions.find (actor_type);
        if (entry_admission != entry_state.actor_admissions.end ()
            && entry_admission->second.on_actor_joined && entry_state.spot_instance) {
            node_lock.unlock ();
            const auto completed = entry_state.run_serial_task (
              "spot-lifecycle-join", [&] {
                  return entry_admission->second.on_actor_joined (
                    entry_state.spot_instance.get (), actor);
              });
            node_lock.lock ();
            if (!completed) {
                return task_t<actor_ref_t> (result_t<actor_ref_t>::failure (
                  completed.error_kind (), completed.error () != nullptr
                                              ? completed.error ()->what ()
                                              : "spot actor joined callback failed"));
            }
        } else {
            const auto entry_joined = entry_state.on_actor_joined_callbacks.find (actor_type);
            if (entry_joined != entry_state.on_actor_joined_callbacks.end ()
                && entry_state.spot_instance) {
                node_lock.unlock ();
                const auto completed = entry_state.run_serial_task (
                  "spot-lifecycle-join", [&] {
                      return entry_joined->second (entry_state.spot_instance.get (), actor);
                  });
                node_lock.lock ();
                if (!completed) {
                    return task_t<actor_ref_t> (result_t<actor_ref_t>::failure (
                      completed.error_kind (), completed.error () != nullptr
                                                  ? completed.error ()->what ()
                                                  : "spot actor joined callback failed"));
                }
            }
        }
        return task_t<actor_ref_t> (result_t<actor_ref_t>::success (committed));
    }
    catch (const framework_exception_t &error) {
        return task_t<actor_ref_t> (
          detail::result_access_t::failure<actor_ref_t> (error));
    }
    catch (const std::exception &error) {
        return task_t<actor_ref_t> (
          result_t<actor_ref_t>::failure (framework_error_kind_t::internal_failure, error.what ()));
    }
    catch (...) {
        return task_t<actor_ref_t> (result_t<actor_ref_t>::failure (
          framework_error_kind_t::internal_failure, "actor leave callback failed"));
    }
}

task_t<void> entry_spot_context_t::destroy_actor_instance_erased (const void *instance)
{
    if (!_state || !_state->node || instance == nullptr) {
        return task_t<void> (result_t<void>::failure (framework_error_kind_t::not_found,
                                                      "actor instance is not registered"));
    }
    /* Resolution and destruction stay under one node lock (the mutex is
     * recursive), so a concurrent transfer cannot move the actor between
     * the identity lookup and the location decision. */
    std::lock_guard<std::recursive_mutex> node_lock (_state->node->mutex);
    const auto found = _state->node->actor_instance_index.find (instance);
    if (found == _state->node->actor_instance_index.end ()) {
        /* Instance not registered on this node: already destroyed or
         * superseded — duplicate destroy is a successful no-op. */
        return task_t<void> (result_t<void>::success ());
    }
    const auto key = found->second.first + ":" + found->second.second;
    const auto found_generation = _state->node->actor_generations.find (key);
    return destroy_actor_erased (
      ::zlink::framework::detail::actor_ref_access_t::make (_state->node_rid, found->second.first, found->second.second,
                   found_generation != _state->node->actor_generations.end ()
                     ? found_generation->second
                     : 1));
}

task_t<void> entry_spot_context_t::destroy_actor_erased (const actor_ref_t &actor)
{
    if (!_state || !_state->node || ::zlink::framework::detail::actor_ref_access_t::empty (actor)) {
        return task_t<void> (result_t<void>::failure (framework_error_kind_t::not_found,
                                                      "actor ref is empty"));
    }
    /* A lifecycle callback receives a reference to the actor instance. Erasing
     * actor_instances from that callback would destroy the object before the
     * callback (and its coroutine frame) has returned. Queue the destructive
     * part behind the current serial turn so the borrowed reference remains
     * valid for the complete callback. */
    if (_state->is_current_callback_thread ()) {
        auto state = _state;
        const auto deferred_actor = actor;
        const auto posted = state->try_post_serial_after_current_turn (
          "entry-spot-actor-destroy-after-handler",
          [state, deferred_actor] {
              (void) entry_spot_context_t (state)
                .destroy_actor_erased (deferred_actor)
                .result ();
          },
          runtime::serial_work_options_t{
            runtime::serial_work_lane_t::lifecycle,
            runtime::serial_execution_queue_t::fixed_work_byte_cost});
        if (!posted) {
            return task_t<void> (result_t<void>::failure (
              framework_error_kind_t::capacity_exceeded,
              "Actor destroy could not be scheduled after the current handler"));
        }
        return task_t<void> (result_t<void>::success ());
    }
    std::lock_guard<std::recursive_mutex> node_lock (_state->node->mutex);
    if (actor.node_rid ().empty () || actor.node_rid ().value () != _state->node_rid.value ()) {
        return task_t<void> (result_t<void>::failure (framework_error_kind_t::not_found,
                                                      "actor is not owned by this Entry SPOT"));
    }

    const auto key = std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor)) + ":" + std::string (actor.actor_id ().value ());
    const auto found_location = _state->node->actor_spot_ids.find (key);
    if (found_location != _state->node->actor_spot_ids.end ()
        && found_location->second != _state->spot_id) {
        return task_t<void> (
          result_t<void>::failure (framework_error_kind_t::not_found,
                                   "actor must leave its current SPOT before destroy"));
    }

    const auto found_generation = _state->node->actor_generations.find (key);
    if (found_generation != _state->node->actor_generations.end ()
        && found_generation->second != actor.object_generation ()) {
        return task_t<void> (result_t<void>::success ());
    }
    if (_state->node->destroying_actors.contains (key)) {
        return task_t<void> (result_t<void>::success ());
    }

    if (found_location != _state->node->actor_spot_ids.end ()) {
        _state->node->destroying_actors.insert (key);
        release_actor_location (*_state->node, actor);
        erase_actor_route_unlocked (*_state->node, key);
        _state->node->actor_created_keys.erase (key);
        _state->node->destroyed_actor_keys.insert (key);
        _state->node->actor_instances.erase (key);
        detail::erase_actor_instance_index_unlocked (*_state->node, ::zlink::framework::detail::actor_ref_access_t::actor_type (actor),
                                                     actor.actor_id ().value ());
        _state->node->actor_mailboxes.erase (key);
        (void) _state->node->dispatched_request_replies.erase_if (
          [&] (const auto &request_key) {
              return request_key.starts_with (actor_request_dedup_prefix (key));
          });
        decrement_actor_count_unlocked (*_state);
        if (_state->node->destroy_actor_registry) {
            auto cleanup = _state->node->destroy_actor_registry (actor);
            _state->node->destroying_actors.erase (key);
            if (!cleanup) {
                const auto *error = cleanup.error ();
                return task_t<void> (result_t<void>::failure (
                  cleanup.error_kind (),
                  error != nullptr ? error->what () : "actor registry cleanup failed"));
            }
        } else {
            _state->node->destroying_actors.erase (key);
        }
    }

    return task_t<void> (result_t<void>::success ());
}

send_call_t spot_context_t::publish_erased (std::string topic,
                                            std::string packet_name,
                                            zlink::message_t payload)
{
    auto state = _state;
    return send_call_t (
      std::move (packet_name), [state, topic = std::move (topic), payload = std::move (payload)] (
                                 const std::string &submitted_packet_name,
                                 const send_call_t::metadata_map_t &) {
          if (!state) {
              return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                              "spot context is not configured");
          }
          try {
              state->ensure_relocation_turn_open ();
          }
          catch (const framework_exception_t &error) {
              return detail::result_access_t::failure<void> (
                error);
          }
          state->ordering_log.push_back ("publish:" + topic + ":" + submitted_packet_name + ":"
                                         + payload.to_string ());
          auto native = state->native_spot.lock ();
          if (native) {
              try {
                  /* Fan-out wire envelope (flow-correlation §4.1, .NET
                   * ZLinkSpotPublishEnvelope 동형): the header carries the
                   * ambient flow pair so every subscriber line shares one
                   * flow id across the tree. */
                  const bool capture_enabled =
                    state->node
                    && detail::message_flow_tracer_t (state->node->dispatch).capture_enabled ();
                  auto flow_scope = runtime::flow_context_t::enter_current_or_create (
                    flow_origin_t::application, capture_enabled);
                  /* Self-delimited single frame: ['Z''L''F''E'][u32 BE
                   * header_len][header JSON][body]. The node-attached fanout
                   * path does not keep multipart boundaries end to end, so
                   * the envelope frames itself; the decode side also accepts
                   * a true two-part frame from peers whose wire preserves
                   * parts. The magic makes the format discriminable from a
                   * legacy raw payload, so a validation failure after a
                   * magic match is definitively a corrupted framework frame. */
                  auto frame_part = encode_spot_publish_frame (
                    state->node ? state->node->snapshot.name : std::string{},
                    submitted_packet_name, topic, payload);
                  if (!state->node || !state->node->channel_runtime
                      || !state->node->channel_runtime->serializers) {
                      return result_t<void>::failure (
                        framework_error_kind_t::internal_failure,
                        "spot publish serializer registry is unavailable");
                  }
                  runtime::messaging::client_call_codec_t route_codec;
                  auto route_header = route_codec.create_envelope (
                    runtime::messaging::message_kind_t::command, "spot",
                    detail::spot_multicast_route_send_t::packet_name,
                    std::chrono::seconds (30));
                  const auto route = detail::spot_multicast_route_send_t{
                    topic, frame_part.to_bytes ()};
                  const auto encoded = route_codec.encode_envelope_parts (
                    route_header, route,
                    *state->node->channel_runtime->serializers);
                  if (native->publish (
                        state->node->snapshot.discovery_channel_name.value_or (
                          state->node->snapshot.name),
                        topic, encoded.items ())
                      != zlink::submit_result_t::ok) {
                      return result_t<void>::failure (framework_error_kind_t::internal_failure,
                                                      "spot publish failed");
                  }
              }
              catch (const std::exception &error) {
                  return result_t<void>::failure (framework_error_kind_t::internal_failure,
                                                  error.what ());
              }
              if (state->node) {
                  detail::message_flow_tracer_t (state->node->dispatch)
                    .trace (message_flow_outcome_t::sent, [&] {
                        return message_flow_event_t{message_flow_outcome_t::sent,
                                                    dispatch_error_surface_t::spot_subscription,
                                                    dispatch_message_kind_t::publish,
                                                    submitted_packet_name,
                                                    std::nullopt,
                                                    topic,
                                                    std::nullopt,
                                                    std::nullopt,
                                                    std::string (state->spot_id),
                                                    std::nullopt,
                                                    std::nullopt};
                    });
              }
          }
          return result_t<void>::success ();
      });
}

serializer_registry_t *spot_context_t::serializer_registry () const noexcept
{
    if (!_state || !_state->channel_runtime) {
        return nullptr;
    }
    return _state->channel_runtime->serializers;
}

send_call_t spot_context_t::send_to_erased (node_rid_t node_rid,
                                            spot_id_t spot_id,
                                            std::string packet_name,
                                            zlink::message_t payload)
{
    if (node_rid.empty () || spot_id.empty ()) {
        return send_call_t (result_t<void>::failure (framework_error_kind_t::not_found,
                                                     "target spot route is empty"));
    }
    auto state = _state;
    return send_call_t (
      std::move (packet_name),
      [state, node_rid = std::move (node_rid), spot_id = std::move (spot_id),
       payload = std::move (payload)] (
        const std::string &submitted_packet_name,
        const send_call_t::metadata_map_t &metadata) mutable -> result_t<void> {
          if (!state) {
              return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                              "SPOT context is not configured");
          }
          try {
              state->ensure_relocation_turn_open ();
          }
          catch (const framework_exception_t &error) {
              return detail::result_access_t::failure<void> (
                error);
          }
          if (auto route_channel_name = optional_spot_route_channel_name (state)) {
              detail::channel_runtime_manager_t manager (state->channel_runtime);
              auto &runtime = manager.get_route_channel (*route_channel_name);
              auto parts = encode_spot_route_parts (
                runtime::messaging::message_kind_t::command, *route_channel_name,
                submitted_packet_name, payload, std::chrono::milliseconds::zero (), metadata);
              auto submitted = runtime.submit_spot_send_parts (
                zlink::routing_id_t::from (std::string (node_rid.value ())),
                spot_id, std::move (parts));
              if (submitted) {
                  state->ordering_log.push_back ("send_to:" + std::string (spot_id));
              }
              return submitted;
          }
          auto native = state->native_spot.lock ();
          if (!native) {
              return result_t<void>::failure (framework_error_kind_t::not_found,
                                              "SPOT mesh route requires a running native Spot");
          }
          try {
              const auto channel_name = spot_mesh_channel_name (state);
              auto parts = encode_spot_route_parts (runtime::messaging::message_kind_t::command,
                                                    channel_name, submitted_packet_name, payload,
                                                    std::chrono::milliseconds::zero (), metadata);
              auto native_parts = parts.items ();
              if (native_parts.empty ()) {
                  return result_t<void>::failure (
                    framework_error_kind_t::protocol_error,
                    "SPOT mesh send requires at least one message part");
              }
              const auto target_node_rid =
                zlink::routing_id_t::from (std::string (node_rid.value ()));
              const auto target_spot_id = spot_id;
              const auto target_generation = resolve_target_spot_generation (
                state->node, target_node_rid, target_spot_id);
              if (!target_generation) {
                  return result_t<void>::failure (
                    framework_error_kind_t::not_found,
                    "SPOT mesh send target generation is unavailable");
              }
              if (native->send_to_spot (target_node_rid, target_spot_id,
                                        *target_generation, native_parts)
                  != zlink::submit_result_t::ok) {
                  return result_t<void>::failure (framework_error_kind_t::internal_failure,
                                                  "SPOT mesh send was not submitted");
              }
              if (state) {
                  state->ordering_log.push_back ("send_to:" + std::string (spot_id));
              }
              return result_t<void>::success ();
          }
          catch (const framework_exception_t &error) {
              return detail::result_access_t::failure<void> (error);
          }
          catch (const std::exception &error) {
              return result_t<void>::failure (framework_error_kind_t::internal_failure,
                                              error.what ());
          }
      });
}

spot_context_t::erased_request_call_t spot_context_t::request_to_erased (node_rid_t node_rid,
                                                                         spot_id_t spot_id,
                                                                         std::string packet_name,
                                                                         zlink::message_t payload)
{
    if (node_rid.empty () || spot_id.empty ()) {
        return erased_request_call_t (framework_exception_t (
          framework_error_kind_t::not_found, "target spot route is empty"));
    }
    auto state = _state;
    const auto target_spot_id = spot_id;
    return erased_request_call_t (
      std::move (packet_name), serializer_registry (),
      [state, node_rid = std::move (node_rid), spot_id = std::move (spot_id),
       payload = std::move (payload)] (
        const std::string &submitted_packet_name, std::chrono::milliseconds timeout,
        const request_call_t<zlink::message_t>::metadata_map_t &metadata) mutable
      -> task_t<zlink::message_t> {
          if (!state) {
              return task_t<zlink::message_t> (result_t<zlink::message_t>::failure (
                framework_error_kind_t::protocol_error, "SPOT context is not configured"));
          }
          try {
              if (auto route_channel_name = optional_spot_route_channel_name (state)) {
                  detail::channel_runtime_manager_t manager (state->channel_runtime);
                  auto &runtime = manager.get_route_channel (*route_channel_name);
                  const auto effective_timeout = timeout > std::chrono::milliseconds::zero ()
                                                   ? timeout
                                                   : runtime.default_request_timeout ();
                  auto parts = encode_spot_route_parts (runtime::messaging::message_kind_t::request,
                                                        *route_channel_name, submitted_packet_name,
                                                        payload, effective_timeout, metadata);
                  state->ordering_log.push_back ("request_to:" + std::string (spot_id));
                  auto reply = runtime.request_reply_spot_parts (
                    zlink::routing_id_t::from (std::string (node_rid.value ())),
                    spot_id, std::move (parts), effective_timeout);
                  if (!reply) {
                      if (reply.error_kind ()
                          == framework_error_kind_t::not_found) {
                          report_spot_dispatch_error (
                            state->node, dispatch_error_surface_t::spot_route,
                            dispatch_message_kind_t::request,
                            dispatch_error_reason_t::handler_missing,
                            dispatch_error_action_t::reply_error, submitted_packet_name,
                            *route_channel_name, std::string (spot_id), std::nullopt,
                            reply.error () ? std::make_exception_ptr (*reply.error ())
                                           : std::exception_ptr {});
                      }
                      return task_t<zlink::message_t> (detail::propagate_failure<zlink::message_t> (reply, "SPOT route request failed"));
                  }
                  runtime::messaging::envelope_codec_t envelope;
                  auto body = envelope.decode_body (reply.value ());
                  if (!body) {
                      return task_t<zlink::message_t> (result_t<zlink::message_t>::failure (
                        body.error_kind (), body.error () ? body.error ()->what ()
                                                          : "SPOT route reply body decode failed"));
                  }
                  return task_t<zlink::message_t> (
                    result_t<zlink::message_t>::success (body.value ()));
              }
              auto native = state->native_spot.lock ();
              if (!native) {
                  return task_t<zlink::message_t> (result_t<zlink::message_t>::failure (
                    framework_error_kind_t::not_found,
                    "SPOT mesh route requires a running native Spot"));
              }
              const auto effective_timeout =
                timeout > std::chrono::milliseconds::zero () ? timeout
                                                              : std::chrono::seconds (30);
              const auto channel_name = spot_mesh_channel_name (state);
              auto parts = encode_spot_route_parts (runtime::messaging::message_kind_t::request,
                                                    channel_name, submitted_packet_name, payload,
                                                    effective_timeout, metadata);
              state->ordering_log.push_back ("request_to:" + std::string (spot_id));
              return request_spot_mesh_message (state, std::move (node_rid), std::move (spot_id),
                                                std::move (parts), effective_timeout);
          }
          catch (const framework_exception_t &error) {
              return task_t<zlink::message_t> (detail::result_access_t::failure<zlink::message_t> (error));
          }
      },
      [state, target_spot_id] (bool release_turn) {
          if (state) {
              try {
                  state->ensure_relocation_turn_open ();
              }
              catch (const framework_exception_t &error) {
                  return detail::result_access_t::failure<void> (
                    error);
              }
          }
          if (!release_turn && state
              && state->spot_id == target_spot_id
              && state->owns_current_serial_turn ()) {
              return result_t<void>::failure (
                framework_error_kind_t::not_configured,
                "awaited request requires the current Spot execution gate");
          }
          return result_t<void>::success ();
      });
}

spot_context_t &spot_context_t::register_packet_erased (std::string packet_name,
                                                        std::type_index payload_type)
{
    const auto duplicate = std::any_of (_state->packets.begin (), _state->packets.end (),
                                        [&] (const spot_packet_descriptor_t &descriptor) {
                                            return descriptor.packet_name == packet_name;
                                        });
    if (duplicate) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "duplicate spot packet registration");
    }
    _state->packets.push_back (spot_packet_descriptor_t{std::move (packet_name), payload_type});
    return *this;
}

std::vector<spot_packet_descriptor_t> spot_context_t::packet_registry () const
{
    return _state->packets;
}

spot_handler_registry_t::spot_handler_registry_t () :
    _state (std::make_shared<detail::spot_context_state_t> ())
{
}

spot_handler_registry_t::spot_handler_registry_t (
  std::shared_ptr<detail::spot_context_state_t> state) :
    _state (std::move (state))
{
}

spot_handler_registry_t::~spot_handler_registry_t () = default;
spot_handler_registry_t::spot_handler_registry_t (spot_handler_registry_t &&) noexcept = default;
spot_handler_registry_t &
spot_handler_registry_t::operator= (spot_handler_registry_t &&) noexcept = default;

spot_handler_registry_t &spot_handler_registry_t::add_handler_erased (spot_handler_kind_t kind,
                                                                      std::string packet_name,
                                                                      std::string topic,
                                                                      std::type_index handler_type,
                                                                      std::type_index payload_type,
                                                                      std::type_index actor_type,
                                                                      std::type_index reply_type,
                                                                      invoker_t invoker)
{
    const auto duplicate =
      std::any_of (_state->handlers.begin (), _state->handlers.end (),
                   [&] (const spot_handler_descriptor_t &descriptor) {
                       return descriptor.kind == kind && descriptor.packet_name == packet_name
                              && descriptor.topic == topic && descriptor.actor_type == actor_type;
                   });
    if (duplicate) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "duplicate spot handler registration");
    }

    _state->handlers.push_back (spot_handler_descriptor_t{kind, std::move (packet_name),
                                                          std::move (topic), handler_type,
                                                          payload_type, actor_type, reply_type});
    _state->handler_invokers.push_back (std::move (invoker));
    const auto &descriptor = _state->handlers.back ();
    if (descriptor.kind == spot_handler_kind_t::subscription && !descriptor.topic.empty ()) {
        if (auto native = _state->native_spot.lock ()) {
            native->set_subscription (
                                      _state->node->snapshot.discovery_channel_name.value_or (
                                        _state->node->snapshot.name),
                                      descriptor.topic);
        }
    }
    return *this;
}

void spot_handler_registry_t::register_actor_admission_erased (
  std::type_index actor_type, detail::spot_actor_admission_callbacks_t callbacks)
{
    _state->actor_admissions[actor_type] = std::move (callbacks);
}

std::vector<spot_handler_descriptor_t> spot_handler_registry_t::descriptors () const
{
    return _state->handlers;
}

spot_handler_kind_t
spot_handler_registry_t::resolve_actor_packet_kind (std::string_view packet_name,
                                                    std::type_index actor_type) const
{
    for (const auto &descriptor : _state->handlers) {
        if (descriptor.kind == spot_handler_kind_t::actor_request
            && descriptor.packet_name == packet_name && descriptor.actor_type == actor_type) {
            return spot_handler_kind_t::actor_request;
        }
    }
    for (const auto &descriptor : _state->handlers) {
        if (descriptor.kind == spot_handler_kind_t::actor_send
            && descriptor.packet_name == packet_name && descriptor.actor_type == actor_type) {
            return spot_handler_kind_t::actor_send;
        }
    }
    return spot_handler_kind_t::actor_request;
}

task_t<zlink::message_t>
spot_handler_registry_t::invoke_erased (spot_handler_kind_t kind,
                                        std::string_view packet_name,
                                        std::string_view topic,
                                        std::type_index actor_type,
                                        void *spot,
                                        void *actor,
                                        service_provider_t &services,
                                        serializer_registry_t &serializers,
                                        zlink::message_t message,
                                        spot_inbound_message_t metadata,
                                        bool serial_dispatch,
                                        std::string actor_execution_key,
                                        std::string actor_execution_spot_id) const
{
    for (std::size_t index = 0; index < _state->handlers.size (); ++index) {
        const auto &descriptor = _state->handlers[index];
        if (descriptor.kind == kind && descriptor.packet_name == packet_name
            && descriptor.topic == topic && descriptor.actor_type == actor_type) {
            const auto handler_index = index;
            detail::task_completion_source_t<zlink::message_t> completion;
            auto task = completion.task ();
            auto state = _state;
            const bool requires_spot_serial = serial_dispatch;
            std::shared_ptr<runtime::serial_execution_queue_t>
              actor_serial_queue;
            if (!actor_execution_key.empty ()
                && state->node) {
                std::lock_guard<std::recursive_mutex> node_lock (
                  state->node->mutex);
                auto &slot =
                  state->node
                    ->actor_execution_queues[actor_execution_key];
                if (!slot) {
                    slot =
                      std::make_shared<
                        runtime::serial_execution_queue_t> (
                        *framework_worker_executor (
                          state->node),
                        runtime::serial_execution_queue_options_t{});
                }
                actor_serial_queue = slot;
                serial_dispatch = true;
            }
            if (!serial_dispatch) {
                if (!state->enter_callback ()) {
                    return task_t<zlink::message_t> (
                      detail::boundary_failure<zlink::message_t> (
                        detail::boundary_error_t::closed,
                        "spot activation is closed"));
                }
                try {
                    runtime::actor_execution_scope_t actor_execution (
                      std::move (actor_execution_key),
                      std::move (actor_execution_spot_id));
                    auto handler_task = state->handler_invokers[handler_index](
                      spot, actor, services, serializers, message, std::move (metadata));
                    detail::observe_task_completion (
                      handler_task,
                      [state, completion] (const result_t<zlink::message_t> &result) mutable {
                          state->leave_callback ();
                          if (result) {
                              completion.complete (
                                result_t<zlink::message_t>::success (result.value ()));
                              return;
                          }
                          completion.complete (result_t<zlink::message_t>::failure (
                            result.error_kind (),
                            result.error () != nullptr ? result.error ()->what ()
                                                       : "spot handler failed"));
                      });
                }
                catch (const framework_exception_t &error) {
                    state->leave_callback ();
                    completion.complete (detail::result_access_t::failure<zlink::message_t> (error));
                }
                catch (const std::exception &error) {
                    state->leave_callback ();
                    completion.complete (result_t<zlink::message_t>::failure (
                      framework_error_kind_t::internal_failure, error.what ()));
                }
                catch (...) {
                    state->leave_callback ();
                    completion.complete (result_t<zlink::message_t>::failure (
                      framework_error_kind_t::internal_failure, "spot handler threw an exception"));
                }
                return task;
            }
            const runtime::serial_work_options_t work_options{
              runtime::serial_work_lane_t::application,
              handler_work_byte_cost (message, metadata)};
            auto dispatch_flow = runtime::flow_context_t::current ();
            std::function<bool (
              std::string,
              runtime::serial_execution_queue_t::async_work_t,
              runtime::serial_work_options_t)> post_serial;
            if (actor_serial_queue && requires_spot_serial) {
                post_serial =
                  [queue = std::move (actor_serial_queue),
                   state, completion] (
                    std::string name,
                    runtime::serial_execution_queue_t::async_work_t work,
                    runtime::serial_work_options_t options) {
                      return queue->try_post_async (
                        name + "-actor",
                        [state, name = std::move (name),
                         work = std::move (work),
                         completion] (auto actor_complete) mutable {
                            const auto posted =
                              state->try_post_serial_async (
                                std::move (name),
                                [work = std::move (work),
                                 actor_complete] (
                                  auto spot_complete) mutable {
                                    work (
                                      [spot_complete = std::move (
                                         spot_complete),
                                       actor_complete] (
                                        std::function<void ()>
                                          finish) mutable {
                                          spot_complete (
                                            [finish = std::move (
                                               finish),
                                             actor_complete] () mutable {
                                                std::exception_ptr error;
                                                try {
                                                    if (finish)
                                                        finish ();
                                                }
                                                catch (...) {
                                                    error =
                                                      std::current_exception ();
                                                }
                                                actor_complete (
                                                  [error] {
                                                      if (error)
                                                          std::rethrow_exception (
                                                            error);
                                                  });
                                            });
                                      });
                                });
                            if (!posted) {
                                actor_complete (
                                  [completion] () mutable {
                                      completion.complete (
                                        result_t<
                                          zlink::message_t>::
                                          failure (
                                            framework_error_kind_t::capacity_exceeded,
                                            "spot serial queue is full"));
                                  });
                            }
                        }, options);
                  };
            } else if (actor_serial_queue) {
                post_serial =
                  [queue = std::move (actor_serial_queue)] (
                    std::string name,
                    runtime::serial_execution_queue_t::async_work_t work,
                    runtime::serial_work_options_t options) {
                      return queue->try_post_async (
                        std::move (name), std::move (work), options);
                  };
            } else {
                post_serial =
                  [state] (
                    std::string name,
                    runtime::serial_execution_queue_t::async_work_t work,
                    runtime::serial_work_options_t options) {
                      return state->try_post_serial_async (
                        std::move (name), std::move (work), options);
                  };
            }
            const auto posted = post_serial (
              "spot-handler",
              [state, handler_index, spot, actor, &services, &serializers,
               owned_message = std::move (message), metadata = std::move (metadata),
               completion, dispatch_flow = std::move (dispatch_flow),
               actor_execution_key = std::move (actor_execution_key),
               actor_execution_spot_id = std::move (actor_execution_spot_id)] (
                auto complete) mutable {
                  runtime::flow_context_t::scope_t callback_flow (std::move (dispatch_flow));
                  runtime::actor_execution_scope_t actor_execution (
                    std::move (actor_execution_key),
                    std::move (actor_execution_spot_id));
                  if (!state->enter_callback ()) {
                      complete ([completion] () mutable {
                          completion.complete (
                            detail::boundary_failure<zlink::message_t> (
                              detail::boundary_error_t::closed,
                              "spot activation is closed"));
                      });
                      return;
                  }
                  auto turn = detail::capture_current_serial_turn ();
                  try {
                      auto handler_task = state->handler_invokers[handler_index](
                        spot, actor, services, serializers, owned_message, metadata);
                      detail::observe_task_completion (
                        handler_task, [state, completion, turn, complete] (
                                        const result_t<zlink::message_t> &result) mutable {
                            result_t<zlink::message_t> final_result =
                              result
                                ? result_t<zlink::message_t>::success (result.value ())
                                : result_t<zlink::message_t>::failure (
                                    result.error_kind (),
                                    result.error () != nullptr ? result.error ()->what ()
                                                               : "spot handler failed");
                            auto finish = [state, completion,
                                           final_result = std::move (final_result)] () mutable {
                                state->leave_callback ();
                                completion.complete (std::move (final_result));
                            };
                            if (turn && turn->released ()) {
                                finish ();
                                return;
                            }
                            complete (std::move (finish));
                        });
                  }
                  catch (const framework_exception_t &error) {
                      complete ([state, completion, error] () mutable {
                          state->leave_callback ();
                          completion.complete (detail::result_access_t::failure<zlink::message_t> (error));
                      });
                  }
                  catch (const std::exception &error) {
                      const auto message = std::string (error.what ());
                      complete ([state, completion, message = std::move (message)] () mutable {
                          state->leave_callback ();
                          completion.complete (result_t<zlink::message_t>::failure (
                            framework_error_kind_t::internal_failure, std::move (message)));
                      });
                  }
                  catch (...) {
                      complete ([state, completion] () mutable {
                          state->leave_callback ();
                          completion.complete (result_t<zlink::message_t>::failure (
                            framework_error_kind_t::internal_failure,
                            "spot handler threw an exception"));
                      });
                  }
              }, work_options);
            if (!posted) {
                return task_t<zlink::message_t> (result_t<zlink::message_t>::failure (
                  framework_error_kind_t::capacity_exceeded,
                  "spot serial queue is full"));
            }
            return task;
        }
    }
    std::ostringstream error_message;
    error_message << "spot handler is not registered: packet='" << packet_name << "', topic='"
                  << topic << "', actor_type='" << actor_type.name () << "', registered=[";
    for (std::size_t index = 0; index < _state->handlers.size (); ++index) {
        if (index > 0) {
            error_message << "; ";
        }
        const auto &descriptor = _state->handlers[index];
        error_message << "packet='" << descriptor.packet_name << "', topic='" << descriptor.topic
                      << "', actor_type='" << descriptor.actor_type.name () << "'";
    }
    error_message << "]";
    return task_t<zlink::message_t> (result_t<zlink::message_t>::failure (
      framework_error_kind_t::not_found, error_message.str ()));
}

spot_node_builder_t::spot_node_builder_t () :
    _state (std::make_shared<detail::spot_node_builder_state_t> (""))
{
}

spot_node_builder_t::spot_node_builder_t (
  std::shared_ptr<detail::spot_node_builder_state_t> state) :
    _state (std::move (state))
{
}

spot_node_builder_t::~spot_node_builder_t () = default;
spot_node_builder_t::spot_node_builder_t (spot_node_builder_t &&) noexcept = default;
spot_node_builder_t &spot_node_builder_t::operator= (spot_node_builder_t &&) noexcept = default;

spot_node_builder_t &
spot_node_builder_t::set_message_follow_duration (std::chrono::milliseconds duration)
{
    _state->message_follow_duration = duration;
    return *this;
}

spot_node_builder_t &
spot_node_builder_t::accept_implicit_route_mesh (std::string route_channel_name,
                                                 std::vector<std::string> manual_connections)
{
    if (route_channel_name.empty () || is_blank (route_channel_name)) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "accepted SPOT route channel name is required");
    }
    for (const auto &endpoint : manual_connections) {
        if (endpoint.empty () || is_blank (endpoint)) {
            throw framework_exception_t (framework_error_kind_t::protocol_error,
                                         "accepted SPOT route manual endpoint is required");
        }
    }
    const auto duplicate =
      std::any_of (_state->snapshot.accepted_route_channels.begin (),
                   _state->snapshot.accepted_route_channels.end (), [&] (const auto &accepted) {
                       return accepted.channel_name == route_channel_name;
                   });
    if (duplicate) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "duplicate accepted SPOT route channel");
    }
    _state->snapshot.accepted_route_channels.push_back (accepted_spot_route_channel_t{
      std::move (route_channel_name), std::move (manual_connections)});
    return *this;
}

spot_node_builder_t &spot_node_builder_t::add_spot_factory_erased (
  std::string spot_name,
  std::type_index spot_type,
  detail::spot_runtime_kind_t kind,
  user_spot_execution_mode_t execution_mode,
  std::int32_t stable_type_limit,
  spot_relocation_readiness_mode_t relocation_readiness,
  detail::factory_relocation_configuration_t relocation)
{
    const bool entry_spot = kind == detail::spot_runtime_kind_t::entry;
    const bool instance_spot = kind == detail::spot_runtime_kind_t::instance;
    if (entry_spot && execution_mode != user_spot_execution_mode_t::spot_wide) {
        throw framework_exception_t (
          framework_error_kind_t::not_configured,
          "Entry Spot execution mode is fixed by the Framework");
    }
    if (relocation.kind
          == detail::factory_relocation_kind_t::preserve_state
        && (!relocation.capture || !relocation.restore)) {
        throw framework_exception_t (
          framework_error_kind_t::not_configured,
          "Spot preserve-state relocation callbacks must not be empty");
    }
    const auto [_, inserted] = _state->spot_factories.emplace (spot_name, spot_type);
    if (!inserted) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "duplicate spot factory registration");
    }
    if (entry_spot) {
        if (_state->snapshot.entry_spot_name) {
            throw framework_exception_t (framework_error_kind_t::protocol_error,
                                         "entry spot is already registered");
        }
        _state->snapshot.entry_spot_name = spot_name;
    }
    if (instance_spot) {
        _state->snapshot.instance_spot_names.push_back (spot_name);
    }
    if (!entry_spot) {
        _state->spot_factory_relocations.emplace (spot_name, relocation);
        _state->spot_stable_type_limits.emplace (spot_name, stable_type_limit);
        _state->spot_relocation_readiness.emplace (
          spot_name, relocation_readiness);
    }
    _state->snapshot.spot_execution_modes.emplace (spot_name, execution_mode);
    _state->snapshot.spot_names.push_back (std::move (spot_name));
    return *this;
}

spot_node_builder_t &spot_node_builder_t::add_actor_factory_erased (
  std::string actor_type,
  std::type_index actor_instance_type,
  std::function<std::shared_ptr<void> (std::string)> create_instance,
  std::function<void (void *, const actor_ref_t &, void *)> configure_instance,
  std::function<std::optional<zlink::message_t> (void *, serializer_registry_t &)>
    serialize_instance,
  std::function<void (void *, const zlink::message_t &, serializer_registry_t &)>
    deserialize_instance,
  std::function<std::shared_ptr<void> (actor_context_t)> create_context_instance,
  detail::actor_join_completion_callback_t on_join_completed,
  detail::factory_relocation_configuration_t relocation,
  std::function<task_t<std::vector<std::byte>> (
    void *, std::stop_token)> capture,
  std::function<task_t<void> (
    void *, std::vector<std::byte>, std::stop_token)> restore)
{
    if ((!create_instance && !create_context_instance)
        || !configure_instance || !serialize_instance || !deserialize_instance) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "actor factory callback must not be empty");
    }
    if (relocation.kind
          == detail::factory_relocation_kind_t::preserve_state
        && (!capture || !restore)) {
        throw framework_exception_t (
          framework_error_kind_t::not_configured,
          "Actor preserve-state relocation callbacks must not be empty");
    }
    const auto [_, inserted] = _state->actor_factories.emplace (
      actor_type,
      detail::spot_node_builder_state_t::actor_factory_registration_t{
        actor_instance_type, relocation,
        std::move (create_instance), std::move (configure_instance),
        std::move (serialize_instance), std::move (deserialize_instance),
        std::move (create_context_instance), std::move (on_join_completed),
        std::move (capture), std::move (restore)});
    if (!inserted) {
        throw framework_exception_t (framework_error_kind_t::already_exists,
                                     "duplicate actor factory registration");
    }
    _state->snapshot.actor_types.push_back (std::move (actor_type));
    return *this;
}

void spot_node_builder_t::register_lifecycle_erased (std::string spot_name,
                                                     detail::spot_lifecycle_callbacks_t callbacks)
{
    _state->spot_lifecycles[std::move (spot_name)] = std::move (callbacks);
}

spot_node_builder_t &spot_node_builder_t::add_spot_resolver (
  std::string name, std::function<std::optional<spot_route_t> (spot_id_t)> resolver)
{
    if (name.empty () || !resolver) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "spot resolver requires a name and callback");
    }
    const auto [_, inserted] = _state->resolvers.emplace (std::move (name), std::move (resolver));
    if (!inserted) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "duplicate spot resolver registration");
    }
    return *this;
}

spot_node_snapshot_t spot_node_builder_t::snapshot () const
{
    return _state->snapshot;
}

detail::local_spot_create_result_t spot_node_builder_t::create_spot (std::string spot_name)
{
    return detail::spot_node_runtime_t (_state).create_spot (std::move (spot_name));
}

detail::local_spot_create_result_t spot_node_builder_t::create_spot_raw (std::string spot_name,
                                                           zlink::message_t request)
{
    return detail::spot_node_runtime_t (_state).create_spot (std::move (spot_name),
                                                             std::move (request));
}

detail::local_spot_create_result_t spot_node_builder_t::create_spot (std::string spot_name,
                                                       const message_t &request)
{
    if (!_state || !_state->channel_runtime || !_state->channel_runtime->serializers) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "spot create requires a serializer registry");
    }
    return create_spot_raw (
      std::move (spot_name),
      detail::message_to_raw (request, *_state->channel_runtime->serializers));
}

detail::local_spot_create_result_t spot_node_builder_t::get_or_create_spot (std::string spot_name,
                                                              spot_id_t spot_id)
{
    return detail::spot_node_runtime_t (_state).get_or_create_spot (std::move (spot_name),
                                                                    std::move (spot_id));
}

detail::local_spot_create_result_t spot_node_builder_t::get_or_create_spot_raw (std::string spot_name,
                                                                  spot_id_t spot_id,
                                                                  zlink::message_t request)
{
    return detail::spot_node_runtime_t (_state).get_or_create_spot (
      std::move (spot_name), std::move (spot_id), std::move (request));
}

detail::local_spot_create_result_t spot_node_builder_t::get_or_create_spot (std::string spot_name,
                                                              spot_id_t spot_id,
                                                              const message_t &request)
{
    if (!_state || !_state->channel_runtime || !_state->channel_runtime->serializers) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "spot get or create requires a serializer registry");
    }
    return get_or_create_spot_raw (
      std::move (spot_name), std::move (spot_id),
      detail::message_to_raw (request, *_state->channel_runtime->serializers));
}

task_t<std::optional<spot_info_t>> spot_node_builder_t::find_spot (spot_id_t spot_id) const
{
    return task_t<std::optional<spot_info_t>> (result_t<std::optional<spot_info_t>>::success (
      detail::spot_node_runtime_t (_state).find_spot (std::move (spot_id))));
}

task_t<std::vector<spot_info_t>> spot_node_builder_t::list_spots () const
{
    return task_t<std::vector<spot_info_t>> (result_t<std::vector<spot_info_t>>::success (
      detail::spot_node_runtime_t (_state).list_spots ()));
}

task_t<bool> spot_node_builder_t::close_spot (spot_id_t spot_id)
{
    return detail::spot_node_runtime_t (_state).close_spot (std::move (spot_id));
}

void spot_node_builder_t::retain_factory_builder (std::shared_ptr<void> builder)
{
    std::lock_guard<std::recursive_mutex> lock (_state->mutex);
    _state->factory_builder_lifetimes.push_back (std::move (builder));
}

std::optional<std::string> spot_node_builder_t::spot_name_for (spot_id_t spot_id) const
{
    return detail::spot_node_runtime_t (_state).spot_name_for (std::move (spot_id));
}

std::optional<spot_route_t> spot_node_builder_t::resolve_spot (spot_id_t spot_id) const
{
    return detail::spot_node_runtime_t (_state).resolve_spot (std::move (spot_id));
}

} // namespace zlink::framework

namespace zlink::framework::detail
{

spot_node_runtime_t::spot_node_runtime_t (std::shared_ptr<spot_node_builder_state_t> state) :
    _state (std::move (state))
{
}

} // namespace zlink::framework::detail

namespace zlink::framework
{

spot_manager_t::spot_manager_t () :
    _state (std::make_shared<detail::spot_node_builder_state_t> (""))
{
}

spot_manager_t::spot_manager_t (
  std::shared_ptr<detail::spot_node_builder_state_t> state) :
    _state (std::move (state))
{
}

spot_manager_t::spot_manager_t (
  std::shared_ptr<detail::spot_node_builder_state_t> state,
  std::weak_ptr<detail::spot_context_state_t> source) :
    _state (std::move (state)), _source (std::move (source))
{
}

spot_manager_t::~spot_manager_t () = default;
spot_manager_t::spot_manager_t (spot_manager_t &&) noexcept = default;
spot_manager_t &spot_manager_t::operator= (spot_manager_t &&) noexcept = default;

spot_create_call_t::spot_create_call_t (
  std::shared_ptr<detail::spot_create_call_state_t> state) :
    _state (std::move (state))
{
}

spot_create_call_t::~spot_create_call_t () = default;
spot_create_call_t::spot_create_call_t (spot_create_call_t &&) noexcept = default;
spot_create_call_t &
spot_create_call_t::operator= (spot_create_call_t &&) noexcept = default;

spot_create_call_t &spot_create_call_t::in_mesh (std::string mesh_name)
{
    if (!_state || _state->mesh_name)
        throw framework_exception_t (
          framework_error_kind_t::not_configured,
          "Spot create Mesh can be set only once");
    _state->mesh_name = std::move (mesh_name);
    return *this;
}

spot_create_call_t &spot_create_call_t::creation_request (message_t request)
{
    if (!_state || _state->request)
        throw framework_exception_t (
          framework_error_kind_t::not_configured,
          "Spot creation request can be set only once");
    _state->request = std::move (request);
    return *this;
}

spot_create_call_t &
spot_create_call_t::timeout (std::chrono::milliseconds timeout)
{
    if (!_state || _state->timeout || timeout <= std::chrono::milliseconds::zero ())
        throw framework_exception_t (
          framework_error_kind_t::not_configured,
          "Spot create timeout must be positive and set only once");
    _state->timeout = timeout;
    return *this;
}

task_t<spot_create_result_t> spot_create_call_t::submit ()
{
    if (!_state || _state->submitted)
        return task_t<spot_create_result_t> (
          result_t<spot_create_result_t>::failure (
            framework_error_kind_t::invalid_operation,
            "Spot create call was already submitted"));
    _state->submitted = true;
    if (const auto source = _state->source.lock ()) {
        try {
            source->ensure_relocation_turn_open ();
        }
        catch (const framework_exception_t &error) {
            return task_t<spot_create_result_t> (
              detail::result_access_t::failure<
                spot_create_result_t> (error));
        }
    }
    if (!_state->node || !_state->node->create_user_spot)
        return task_t<spot_create_result_t> (
          result_t<spot_create_result_t>::failure (
            framework_error_kind_t::not_configured,
            "Spot manager is not connected to a Location runtime"));
    auto task = _state->node->create_user_spot (
      _state->exclusive, _state->spot_id, std::move (_state->stable_type),
      std::move (_state->mesh_name), std::move (_state->request),
      _state->timeout.value_or (std::chrono::seconds (30)));
    const auto turn_plan = detail::prepare_serial_turn_await (false);
    if (!turn_plan)
        return task;
    return detail::reschedule_task (std::move (task),
                                    std::move (turn_plan->scheduler));
}

task_t<spot_create_result_t> spot_create_call_t::yield ()
{
    if (!detail::current_serial_turn_allows_yield ()) {
        return detail::unsupported_yield_task<spot_create_result_t> ();
    }
    auto turn_plan = detail::prepare_serial_turn_await (true);
    auto task = submit ();
    if (!turn_plan) {
        return task;
    }
    return detail::reschedule_task (
      std::move (task), std::move (turn_plan->scheduler));
}

spot_create_call_t spot_manager_t::create (std::string stable_type)
{
    auto state = std::make_shared<detail::spot_create_call_state_t> ();
    state->node = _state;
    state->source = _source;
    state->exclusive = true;
    state->stable_type = std::move (stable_type);
    return spot_create_call_t (std::move (state));
}

spot_create_call_t spot_manager_t::get_or_create (
  spot_id_t spot_id,
  std::string stable_type)
{
    auto state = std::make_shared<detail::spot_create_call_state_t> ();
    state->node = _state;
    state->source = _source;
    state->spot_id = std::move (spot_id);
    state->stable_type = std::move (stable_type);
    return spot_create_call_t (std::move (state));
}

task_t<std::optional<spot_ref_t>>
spot_manager_t::find (spot_id_t spot_id) const
{
    if (const auto source = _source.lock ()) {
        try {
            source->ensure_relocation_turn_open ();
        }
        catch (const framework_exception_t &error) {
            return task_t<std::optional<spot_ref_t>> (
              detail::result_access_t::failure<
                std::optional<spot_ref_t>> (error));
        }
    }
    if (!_state || !_state->find_user_spot)
        return task_t<std::optional<spot_ref_t>> (
          result_t<std::optional<spot_ref_t>>::failure (
            framework_error_kind_t::not_configured,
            "Spot manager is not connected to a Location runtime"));
    return _state->find_user_spot (std::move (spot_id));
}

task_t<bool> spot_manager_t::close (spot_ref_t spot)
{
    if (const auto source = _source.lock ()) {
        try {
            source->ensure_relocation_turn_open ();
        }
        catch (const framework_exception_t &error) {
            return task_t<bool> (
              detail::result_access_t::failure<bool> (error));
        }
    }
    if (!_state || !_state->close_user_spot)
        return task_t<bool> (result_t<bool>::failure (
          framework_error_kind_t::not_configured,
          "Spot manager is not connected to a Location runtime"));
    return _state->close_user_spot (std::move (spot));
}

std::optional<actor_ref_t>
spot_manager_t::current_actor_ref (const actor_ref_t &actor_ref) const
{
    return detail::spot_node_runtime_t (_state).current_actor_ref (actor_ref);
}

result_t<std::optional<zlink::message_t>>
spot_manager_t::relay_actor_packet (const actor_ref_t &actor_ref,
                                         actor_context_t actor_context,
                                         std::string_view packet_name,
                                         const zlink::message_t &message,
                                         service_provider_t &services,
                                         serializer_registry_t &serializers,
                                         spot_inbound_message_t metadata)
{
    return relay_actor_packet (actor_ref, std::move (actor_context),
                               detail::stream_message_kind_t::request, packet_name, message,
                               services, serializers, std::move (metadata));
}

result_t<std::optional<zlink::message_t>>
spot_manager_t::relay_actor_packet (const actor_ref_t &actor_ref,
                                         actor_context_t actor_context,
                                         detail::stream_message_kind_t message_kind,
                                         std::string_view packet_name,
                                         const zlink::message_t &message,
                                         service_provider_t &services,
                                         serializer_registry_t &serializers,
                                         spot_inbound_message_t metadata)
{
    if (_state->actor_packet_relay) {
        return _state->actor_packet_relay (actor_ref, std::move (actor_context), message_kind,
                                           packet_name, message, services, serializers,
                                           std::move (metadata));
    }
    return detail::spot_node_runtime_t (_state).relay_actor_packet (
      actor_ref, std::move (actor_context), message_kind, packet_name, message, services,
      serializers, std::move (metadata));
}

spot_publisher_client_t::spot_publisher_client_t (spot_manager_t manager,
                                                  serializer_registry_t &serializers) :
    _manager (std::move (manager)), _serializers (&serializers)
{
}

publish_call_t spot_publisher_client_t::publish_raw (std::string channel_name,
                                                     std::string topic,
                                                     std::string packet_name,
                                                     zlink::message_t payload) const
{
    if (!_serializers) {
        return publish_call_t (result_t<void>::failure (
          framework_error_kind_t::internal_failure,
          "logical multicast serializer registry is unavailable"));
    }
    if (channel_name.empty () || is_blank (channel_name)) {
        return publish_call_t (result_t<void>::failure (
          framework_error_kind_t::protocol_error,
          "logical multicast channel name must not be empty"));
    }
    if (topic.empty ()) {
        return publish_call_t (result_t<void>::failure (
          framework_error_kind_t::protocol_error,
          "logical multicast topic must not be empty"));
    }

    std::shared_ptr<service::mesh_node_t> native_node;
    {
        std::lock_guard<std::recursive_mutex> node_lock (_manager._state->mutex);
        native_node = _manager._state->native_node.lock ();
    }
    if (!native_node) {
        return publish_call_t (result_t<void>::failure (
          framework_error_kind_t::unavailable,
          "logical multicast route mesh is not connected"));
    }

    const bool capture_enabled =
      detail::message_flow_tracer_t (_manager._state->dispatch).capture_enabled ();
    auto frame = encode_spot_publish_frame (
      channel_name, std::move (packet_name), topic, payload);
    return publish_call_t (
      [native_node = std::move (native_node),
       channel_name = std::move (channel_name),
       topic = std::move (topic), frame = std::move (frame), capture_enabled] (
        const publish_call_t::metadata_map_t &metadata) {
          auto flow_scope = runtime::flow_context_t::enter_current_or_create (
            flow_origin_t::application, capture_enabled);
          std::vector<zlink::message_t> parts{frame};
          auto publisher = native_node->entry_spot ();
          const auto encoded_metadata =
            detail::mesh_metadata_codec_t::encode (metadata);
          const auto submitted = publisher.publish (
            channel_name, topic, parts, zlink::send_flags_t::none,
            encoded_metadata);
          if (submitted == zlink::submit_result_t::ok)
              return result_t<void>::success ();
          return result_t<void>::failure (
            runtime::messaging::map_submit_result_error_kind (submitted),
            "logical multicast could not enter the source transport queue");
      });
}

} // namespace zlink::framework

namespace zlink::framework::detail
{

spot_manager_t spot_node_runtime_t::manager () const
{
    return spot_manager_t (_state);
}

result_t<spot_context_t>
spot_node_runtime_t::actor_join_context_unlocked (spot_id_t spot_id,
                                                  const zlink::message_t &request)
{
    auto context = find_context (spot_id);
    if (!context || !context->_state->spot_instance) {
        std::optional<std::string> dynamic_spot_name;
        for (const auto &[spot_name, _] : _state->spot_factories) {
            if (_state->snapshot.entry_spot_name
                && spot_name == *_state->snapshot.entry_spot_name) {
                continue;
            }
            if (dynamic_spot_name) {
                dynamic_spot_name.reset ();
                break;
            }
            dynamic_spot_name = spot_name;
        }
        if (dynamic_spot_name) {
            (void) get_or_create_spot (*dynamic_spot_name, spot_id, request);
            auto refreshed = find_context (spot_id);
            context.reset ();
            if (refreshed)
                context.emplace (std::move (*refreshed));
        }
    }
    if (!context || !context->_state->spot_instance) {
        return result_t<spot_context_t>::failure (framework_error_kind_t::not_found,
                                                  "target spot is not registered");
    }
    return result_t<spot_context_t>::success (std::move (*context));
}

result_t<std::reference_wrapper<spot_node_builder_state_t::actor_factory_registration_t>>
spot_node_runtime_t::actor_factory_unlocked (const actor_ref_t &actor_ref) const
{
    const auto found = _state->actor_factories.find (std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor_ref)));
    if (found == _state->actor_factories.end ()) {
        return result_t<
          std::reference_wrapper<spot_node_builder_state_t::actor_factory_registration_t>>::
          failure (framework_error_kind_t::not_found,
                   "actor factory is not registered");
    }
    return result_t<std::reference_wrapper<
      spot_node_builder_state_t::actor_factory_registration_t>>::success (found->second);
}

result_t<std::reference_wrapper<spot_actor_admission_callbacks_t>>
spot_node_runtime_t::actor_admission_unlocked (spot_context_t &context,
                                               std::type_index actor_type,
                                               spot_id_t spot_id,
                                               const actor_ref_t &actor_ref)
{
    const auto admission = context._state->actor_admissions.find (actor_type);
    if (admission == context._state->actor_admissions.end () || !admission->second.join) {
        report_spot_dispatch_error (
          _state, dispatch_error_surface_t::spot_actor, dispatch_message_kind_t::actor_request,
          dispatch_error_reason_t::handler_missing, dispatch_error_action_t::reply_error,
          "actor.join", std::nullopt, std::string (spot_id),
          std::string (actor_ref.actor_id ().value ()));
        return result_t<std::reference_wrapper<spot_actor_admission_callbacks_t>>::failure (
          framework_error_kind_t::not_found, "spot actor join callback is not registered");
    }
    return result_t<std::reference_wrapper<spot_actor_admission_callbacks_t>>::success (
      admission->second);
}

void spot_node_runtime_t::leave_previous_actor_route (
  const std::string &key,
  std::type_index actor_type,
  void *actor,
  std::unique_lock<std::recursive_mutex> &node_lock)
{
    const auto previous = _state->actor_spot_ids.find (key);
    if (previous == _state->actor_spot_ids.end ()) {
        return;
    }
    if (auto previous_context = find_context (previous->second)) {
        auto &previous_state = *previous_context->_state;
        if (const auto previous_admission = previous_state.actor_admissions.find (actor_type);
            previous_admission != previous_state.actor_admissions.end ()
            && previous_admission->second.on_leave_actor && previous_state.spot_instance) {
            // The leave callback is user code on the spot serial queue. It may call back
            // into the framework (sends, joins) that need the node mutex from the serial
            // thread, so the node mutex must not be held across this wait.
            node_lock.unlock ();
            result_t<void> completed = result_t<void>::success ();
            try {
                completed = previous_state.run_serial_task (
                  "spot-actor-leave", [&] {
                      return previous_admission->second.on_leave_actor (
                        previous_state.spot_instance.get (), actor);
                  });
            }
            catch (...) {
                node_lock.lock ();
                throw;
            }
            node_lock.lock ();
            if (!completed) {
                throw framework_exception_t (
                  completed.error_kind (), completed.error () != nullptr
                                              ? completed.error ()->what ()
                                              : "spot actor leave callback failed");
            }
        }
        decrement_actor_count_unlocked (previous_state);
    }
    erase_actor_route_unlocked (*_state, key);
}

void spot_node_runtime_t::commit_accepted_actor_join_unlocked (
  const std::string &key,
  spot_context_t &context,
  const actor_ref_t &committed,
  std::type_index actor_type,
  void *actor,
  const spot_actor_admission_callbacks_t &admission,
  bool create_entry_actor,
  const zlink::message_t &create_request,
  std::string operation_id,
  bool &authority_committed)
{
    authority_committed = false;
    std::optional<spot_context_t> previous_context;
    if (const auto previous = _state->actor_spot_ids.find (key);
        previous != _state->actor_spot_ids.end ()) {
        auto found_previous = find_context (previous->second);
        if (found_previous)
            previous_context.emplace (std::move (*found_previous));
    }
    const auto caller_owns_source_turn =
      previous_context && previous_context->_state->owns_current_serial_turn ();
    auto &target_state = *context._state;
    bool created_entry_actor = false;
    if (create_entry_actor
        && admission.kind == detail::spot_runtime_kind_t::entry
        && !_state->actor_created_keys.contains (key)
        && admission.on_create_actor) {
        auto &serializers = *target_state.channel_runtime->serializers;
        result_t<actor_create_response_t> create_result =
          result_t<actor_create_response_t>::failure (
            framework_error_kind_t::internal_failure,
            "Entry Spot actor creation callback did not complete");
        auto create_actor = [&] {
            create_result =
              admission
                .on_create_actor (target_state.spot_instance.get (), actor,
                                  create_request, serializers)
                .result ();
        };
        if (caller_owns_source_turn) {
            create_actor ();
        } else if (!target_state.run_serial_sync ("spot-actor-create", create_actor)) {
            throw framework_exception_t (framework_error_kind_t::capacity_exceeded,
                                         "spot serial queue is full");
        }
        if (!create_result)
            throw framework_exception_t (
              create_result.error_kind (),
              create_result.error () ? create_result.error ()->what ()
                                     : "Entry Spot actor creation callback failed");
        if (!create_result.value ().accepted)
            throw framework_exception_t (
              framework_error_kind_t::rejected,
              "Entry Spot rejected Actor creation");
        _state->actor_created_keys.insert (key);
        created_entry_actor = true;
    }
    /*
     * Publish membership before lifecycle callbacks. Application callbacks
     * observe the committed target Context, and a callback failure must not
     * roll authority back to the previous owner.
     */
    const auto location_updated =
      update_actor_location_after_move (*_state, committed, target_state, false);
    if (!location_updated) {
        throw framework_exception_t (location_updated.error_kind (),
                                     location_updated.error ()
                                       ? location_updated.error ()->what ()
                                       : "actor committed location update failed");
    }
    authority_committed = true;

    if (previous_context) {
        decrement_actor_count_unlocked (*previous_context->_state);
    }
    erase_actor_route_unlocked (*_state, key);
    _state->destroyed_actor_keys.erase (key);
    if (auto native = _state->native_node.lock ();
        native && !_state->native_actors.contains (key)
        && !_state->mesh_runtime_owned_native_actor_ids.contains (
          std::string (committed.actor_id ().value ()))) {
        _state->native_actors.emplace (
          key, std::make_unique<service::actor_t> (
                 native->create_actor (std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (committed)),
                                       std::string (committed.actor_id ().value ()))));
    }
    detail::record_actor_context_route_unlocked (*_state, key,
                                                 detail::effective_spot_node_rid (_state->snapshot),
                                                 target_state, committed.object_generation ());
    if (_state->update_actor_registry_ref) {
        const auto updated = _state->update_actor_registry_ref (committed);
        if (!updated) {
            throw framework_exception_t (updated.error_kind (), updated.error ()
                                                                  ? updated.error ()->what ()
                                                                  : "actor ref update failed");
        }
    }
    if (operation_id.empty ())
        operation_id = key;
    emit_actor_transfer_marker (
      "location_committed", committed, std::move (operation_id),
      context.spot_id (), node_rid ());

    if (!created_entry_actor && admission.on_actor_joined) {
        const auto completed = target_state.run_serial_task (
          "spot-actor-joined", [&] {
              return admission.on_actor_joined (target_state.spot_instance.get (), actor);
          });
        if (!completed) {
            throw framework_exception_t (
              completed.error_kind (), completed.error () != nullptr
                                          ? completed.error ()->what ()
                                          : "spot actor joined callback failed");
        }
    }
    if (previous_context) {
        auto &previous_state = *previous_context->_state;
        if (const auto previous_admission = previous_state.actor_admissions.find (actor_type);
            previous_admission != previous_state.actor_admissions.end ()
            && previous_admission->second.on_leave_actor && previous_state.spot_instance) {
            const auto completed = previous_state.run_serial_task (
              "spot-actor-leave", [&] {
                  return previous_admission->second.on_leave_actor (
                    previous_state.spot_instance.get (), actor);
              });
            if (!completed) {
                throw framework_exception_t (
                  completed.error_kind (), completed.error () != nullptr
                                              ? completed.error ()->what ()
                                              : "spot actor leave callback failed");
            }
        }
    }
}

void spot_node_runtime_t::enqueue_actor_handoff_replay (
  const actor_ref_t &actor_ref,
  std::vector<handoff_packet_t> backlog,
  service_provider_t &services,
  std::string transfer_id)
{
    if (backlog.empty ())
        return;
    const auto key = actor_key (actor_ref);
    const auto location = _state->actor_spot_ids.find (key);
    const auto factory = _state->actor_factories.find (
      std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor_ref)));
    const auto actor = _state->actor_instances.find (key);
    if (location == _state->actor_spot_ids.end ()
        || factory == _state->actor_factories.end ()
        || actor == _state->actor_instances.end () || !actor->second) {
        emit_actor_transfer_marker ("handoff_replay_unavailable", actor_ref,
                                    transfer_id);
        return;
    }
    const auto context = find_context (location->second);
    if (!context || !context->_state->spot_instance) {
        emit_actor_transfer_marker ("handoff_replay_unavailable", actor_ref,
                                    transfer_id, location->second);
        return;
    }
    auto &serializers = *context->_state->channel_runtime->serializers;
    for (auto &packet : backlog) {
        emit_actor_transfer_marker ("handoff_replay_enqueued", actor_ref,
                                    transfer_id, location->second);
        spot_inbound_message_t metadata;
        metadata.content_type = std::move (packet.content_type);
        metadata.values = std::move (packet.metadata);
        std::string replay_request_id;
        if (packet.is_request) {
            const auto id_it = metadata.values.find ("__zlink.actorRequestId");
            if (id_it != metadata.values.end () && !id_it->second.empty ()) {
                replay_request_id = id_it->second;
                const auto claim = _state->dispatched_request_replies.claim (
                  actor_request_dedup_key (key, replay_request_id));
                if (claim.state != runtime::exactly_once_claim_state::claimed)
                    continue;
            }
        }
        const auto kind = packet.is_request ? spot_handler_kind_t::actor_request
                                            : spot_handler_kind_t::actor_send;
        auto task = spot_handler_registry_t (context->_state)
                      .invoke_erased (
                        kind, packet.packet_name, {}, factory->second.actor_type,
                        context->_state->spot_instance.get (), actor->second.get (), services,
                        serializers, zlink::message_t::from (std::move (packet.payload)),
                        std::move (metadata), true, key, std::string (location->second));
        auto task_holder = std::make_shared<task_t<zlink::message_t>> (std::move (task));
        detail::observe_task_completion (
          *task_holder,
          [task_holder, state = _state, key, replay_request_id,
           actor_ref, transfer_id] (const result_t<zlink::message_t> &completed) mutable {
              if (replay_request_id.empty ())
                  return;
              const auto dedup_key = actor_request_dedup_key (key, replay_request_id);
              if (completed) {
                  (void) state->dispatched_request_replies.complete (
                    dedup_key, completed.value ());
              } else {
                  (void) state->dispatched_request_replies.erase (dedup_key);
                  detail::spot_node_runtime_t (state).emit_actor_transfer_marker (
                    "handoff_replay_failed", actor_ref, transfer_id);
              }
          });
    }
}

void spot_node_runtime_t::replay_actor_handoff_until_move_closed (
  const actor_ref_t &actor_ref,
  std::string transfer_id)
{
    const auto key = actor_key (actor_ref);
    const auto replay_id = _state->actor_transfer_coordinator.transfer_id (key)
                             .value_or (transfer_id.empty () ? key : transfer_id);
    for (;;) {
        auto replay = _state->actor_transfer_coordinator.finish_move_replay (key);
        if (!replay.backlog.empty ()) {
            if (_state->root_services) {
                enqueue_actor_handoff_replay (
                  actor_ref, std::move (replay.backlog), *_state->root_services, replay_id);
            } else {
                emit_actor_transfer_marker (
                  "handoff_replay_unavailable", actor_ref, replay_id);
            }
        }
        if (replay.completed)
            break;
    }
}

result_t<actor_join_reply_t> spot_node_runtime_t::join_actor_to_spot_erased (
  const actor_ref_t &actor_ref,
  spot_id_t spot_id,
  const zlink::message_t &request,
  const std::optional<zlink::message_t> &actor_snapshot,
  actor_context_t actor_context,
  std::uint64_t operation_id_high,
  std::uint64_t operation_id_low)
{
    if (::zlink::framework::detail::actor_ref_access_t::empty (actor_ref)) {
        return result_t<actor_join_reply_t>::failure (framework_error_kind_t::not_found,
                                                      "actor ref is empty");
    }
    auto context = actor_join_context_unlocked (spot_id, request);
    if (!context) {
        return detail::propagate_failure<actor_join_reply_t> (context, "target spot is not registered");
    }
    auto actor_factory = actor_factory_unlocked (actor_ref);
    if (!actor_factory) {
        return detail::propagate_failure<actor_join_reply_t> (actor_factory, "actor factory failed");
    }
    const auto key = actor_key (actor_ref);
    const auto committed =
      ::zlink::framework::detail::actor_ref_access_t::make (node_rid_t::from_string (detail::effective_spot_node_rid (_state->snapshot)),
                   std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor_ref)), std::string (actor_ref.actor_id ().value ()),
                   actor_ref.object_generation ());
    /* Registration is double-checked: an already-registered actor is taken
     * under the node mutex without touching the factory, and only a first
     * registration constructs — outside the mutex, because the factory is user
     * code and must not be able to invert lock order. The map entry and its
     * identity index entry are then installed together under the mutex, so a
     * concurrent destroy never sees one without the other. */
    std::shared_ptr<void> actor_instance = registered_actor_instance (actor_ref, key);
    if (!actor_instance) {
        auto &registration = actor_factory.value ().get ();
        std::shared_ptr<void> created_instance;
        if (registration.create_context_instance) {
            if (!actor_context._state) {
                return result_t<actor_join_reply_t>::failure (
                  framework_error_kind_t::not_configured,
                  "Actor factory requires a Framework Actor context");
            }
            if (actor_context.serializer_registry () == nullptr) {
                return result_t<actor_join_reply_t>::failure (
                  framework_error_kind_t::protocol_error,
                  "Actor materialization Context has no serializer registry");
            }
            auto committed_context = actor_context_t (
              actor_context._state, committed, 0,
              context.value ()._state->mesh_name);
            created_instance = registration.create_context_instance (
              std::move (committed_context));
        } else {
            created_instance = registration.create_instance (
              std::string (actor_ref.actor_id ().value ()));
        }
        if (!created_instance) {
            return result_t<actor_join_reply_t>::failure (
              framework_error_kind_t::not_found, "actor factory returned null");
        }
        actor_instance = install_actor_instance (actor_ref, key, std::move (created_instance));
        if (!actor_instance) {
            return result_t<actor_join_reply_t>::failure (
              framework_error_kind_t::not_found, "actor has been destroyed");
        }
    }
    if (actor_snapshot) {
        auto &serializers = *context.value ()._state->channel_runtime->serializers;
        actor_factory.value ().get ().deserialize_instance (actor_instance.get (), *actor_snapshot,
                                                            serializers);
    }
    auto admission = actor_admission_unlocked (
      context.value (), actor_factory.value ().get ().actor_type, spot_id, actor_ref);
    if (!admission) {
        return detail::propagate_failure<actor_join_reply_t> (admission, "actor admission failed");
    }

    const auto source_spot = _state->actor_spot_ids.find (key);
    const auto source_spot_id =
      source_spot == _state->actor_spot_ids.end () ? spot_id_t{} : source_spot->second;
    auto &serializers = *context.value ()._state->channel_runtime->serializers;
    const auto response = admission.value ().get ().join (
      context.value ()._state->spot_instance.get (), actor_ref.actor_id ().value (), request,
      serializers);
    if (!response.accepted) {
        return result_t<actor_join_reply_t>::success (
          actor_join_reply_t{1, actor_ref, framework_reply_or_empty (response.reply, serializers)});
    }

    if (!_state->actor_transfer_coordinator.try_begin_local (key)) {
        return result_t<actor_join_reply_t>::failure (framework_error_kind_t::rejected,
                                                      "actor move is already in progress");
    }
    bool claimed_location = false;
    const auto location_claim = claim_pending_actor_location_before_activation (
      _state, actor_ref, source_spot_id, committed, *context.value ()._state,
      claimed_location);
    if (!location_claim) {
        replay_actor_handoff_until_move_closed (actor_ref, key);
        return result_t<actor_join_reply_t>::failure (
          location_claim.error_kind (), location_claim.error () ? location_claim.error ()->what ()
                                                                : "actor location claim failed");
    }
    bool authority_committed = false;
    auto fail_local_commit = [&] {
        if (!authority_committed && (claimed_location || source_spot_id.empty ())) {
            release_actor_location (*_state, committed);
        }
        if (authority_committed) {
            _state->actor_transfer_coordinator.mark_reconcile (key);
            replay_actor_handoff_until_move_closed (committed, key);
        } else {
            replay_actor_handoff_until_move_closed (actor_ref, key);
        }
    };
    try {
        if (!actor_factory.value ().get ().create_context_instance) {
            actor_factory.value ().get ().configure_instance (
              actor_instance.get (), committed, nullptr);
        }
        commit_accepted_actor_join_unlocked (
          key, context.value (), committed, actor_factory.value ().get ().actor_type,
          actor_instance.get (), admission.value ().get (), true, request,
          operation_id_high == 0 && operation_id_low == 0
            ? std::string{}
            : std::to_string (operation_id_high) + ":"
                + std::to_string (operation_id_low),
          authority_committed);
        if (_state->root_services) {
            for (;;) {
                auto replay = _state->actor_transfer_coordinator.finish_move_replay (key);
                enqueue_actor_handoff_replay (
                  committed, std::move (replay.backlog), *_state->root_services, key);
                if (replay.completed)
                    break;
            }
        } else {
            const auto completed =
              _state->actor_transfer_coordinator.complete_move_and_take_backlog (key);
            if (!completed.backlog.empty ()) {
                emit_actor_transfer_marker ("handoff_replay_unavailable", committed, key);
            }
        }
    }
    catch (const framework_exception_t &error) {
        fail_local_commit ();
        return detail::result_access_t::failure<actor_join_reply_t> (error);
    }
    catch (const std::exception &error) {
        fail_local_commit ();
        return result_t<actor_join_reply_t>::failure (framework_error_kind_t::internal_failure,
                                                      error.what ());
    }
    catch (...) {
        fail_local_commit ();
        return result_t<actor_join_reply_t>::failure (framework_error_kind_t::internal_failure,
                                                      "actor join commit failed");
    }
    return result_t<actor_join_reply_t>::success (
      actor_join_reply_t{0, committed, framework_reply_or_empty (response.reply, serializers)});
}

result_t<actor_join_reply_t>
spot_node_runtime_t::join_remote_actor_to_spot_erased (const actor_ref_t &actor_ref,
                                                       spot_id_t spot_id,
                                                       const zlink::message_t &request,
                                                       actor_context_t actor_context)
{
    /* graceful-drain-handoff §4-2/§5.2: a draining node rejects new actor
    * admission and joins; already-admitted transfer commits stay accepted. */
    if (_state->drain_flag && _state->drain_flag->load (std::memory_order_acquire)) {
        return result_t<actor_join_reply_t>::failure (framework_error_kind_t::rejected,
                                                      "spot node is draining and rejects new actor joins");
    }
    if (::zlink::framework::detail::actor_ref_access_t::empty (actor_ref)) {
        return result_t<actor_join_reply_t>::failure (framework_error_kind_t::not_found,
                                                      "actor ref is empty");
    }
    auto context = actor_join_context_unlocked (spot_id, request);
    if (!context) {
        return detail::propagate_failure<actor_join_reply_t> (context, "target spot is not registered");
    }

    auto actor_factory = actor_factory_unlocked (actor_ref);
    if (!actor_factory) {
        return detail::propagate_failure<actor_join_reply_t> (actor_factory, "actor factory failed");
    }

    const auto key = actor_key (actor_ref);
    const auto committed =
      ::zlink::framework::detail::actor_ref_access_t::make (node_rid_t::from_string (detail::effective_spot_node_rid (_state->snapshot)),
                   std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor_ref)), std::string (actor_ref.actor_id ().value ()),
                   actor_ref.object_generation ());
    /* Registration is double-checked: an already-registered actor is taken
     * under the node mutex without touching the factory, and only a first
     * registration constructs — outside the mutex, because the factory is user
     * code and must not be able to invert lock order. The map entry and its
     * identity index entry are then installed together under the mutex, so a
     * concurrent destroy never sees one without the other. */
    std::shared_ptr<void> actor_instance = registered_actor_instance (actor_ref, key);
    if (!actor_instance) {
        auto &registration = actor_factory.value ().get ();
        std::shared_ptr<void> created_instance;
        if (registration.create_context_instance) {
            if (!actor_context._state) {
                return result_t<actor_join_reply_t>::failure (
                  framework_error_kind_t::not_configured,
                  "Actor factory requires a Framework Actor context");
            }
            auto committed_context = actor_context_t (
              actor_context._state, committed, 0,
              context.value ()._state->mesh_name);
            created_instance = registration.create_context_instance (
              std::move (committed_context));
        } else {
            created_instance = registration.create_instance (
              std::string (actor_ref.actor_id ().value ()));
        }
        if (!created_instance) {
            return result_t<actor_join_reply_t>::failure (
              framework_error_kind_t::not_found, "actor factory returned null");
        }
        actor_instance = install_actor_instance (actor_ref, key, std::move (created_instance));
        if (!actor_instance) {
            return result_t<actor_join_reply_t>::failure (
              framework_error_kind_t::not_found, "actor has been destroyed");
        }
    }
    if (!actor_factory.value ().get ().create_context_instance) {
        auto committed_context = actor_context_t (
          actor_context._state, committed, 0,
          context.value ()._state->mesh_name);
        actor_factory.value ().get ().configure_instance (
          actor_instance.get (), committed, &committed_context);
    }

    auto admission = actor_admission_unlocked (
      context.value (), actor_factory.value ().get ().actor_type, spot_id, actor_ref);
    if (!admission) {
        return detail::propagate_failure<actor_join_reply_t> (admission, "actor admission failed");
    }

    auto &serializers = *context.value ()._state->channel_runtime->serializers;
    const auto response = admission.value ().get ().join (
      context.value ()._state->spot_instance.get (), actor_ref.actor_id ().value (), request,
      serializers);
    if (!response.accepted) {
        if (!actor_factory.value ().get ().create_context_instance) {
            actor_factory.value ().get ().configure_instance (
              actor_instance.get (), actor_ref, &actor_context);
        }
        return result_t<actor_join_reply_t>::success (
          actor_join_reply_t{1, actor_ref, framework_reply_or_empty (response.reply, serializers)});
    }

    bool claimed_location = false;
    const auto location_claim = claim_pending_actor_location_before_activation (
      _state, actor_ref, spot_id_t{}, committed, *context.value ()._state, claimed_location);
    if (!location_claim) {
        return result_t<actor_join_reply_t>::failure (
          location_claim.error_kind (), location_claim.error () ? location_claim.error ()->what ()
                                                                : "actor location claim failed");
    }
    bool authority_committed = false;
    try {
        commit_accepted_actor_join_unlocked (
          key, context.value (), committed, actor_factory.value ().get ().actor_type,
          actor_instance.get (), admission.value ().get (), false, request, key,
          authority_committed);
    }
    catch (...) {
        if (claimed_location && !authority_committed) {
            release_actor_location (*_state, committed);
        }
        throw;
    }
    return result_t<actor_join_reply_t>::success (
      actor_join_reply_t{0, committed, framework_reply_or_empty (response.reply, serializers)});
}

std::size_t spot_node_runtime_t::cleanup_expired_actor_admissions ()
{
    return cleanup_expired_actor_admissions_at (std::chrono::steady_clock::now ());
}

std::size_t spot_node_runtime_t::cleanup_expired_actor_admissions_at (
  std::chrono::steady_clock::time_point now)
{
    const auto expired = _state->actor_transfer_coordinator.cleanup_expired (now);
    for (const auto &entry : expired) {
        emit_actor_transfer_marker ("pending_admission_expired", entry.admission.source_actor,
                                    entry.transfer_id, entry.admission.target_spot_id);
    }
    std::size_t removed = expired.size ();
    std::vector<spot_node_builder_state_t::pending_remote_source_cleanup_t> cleaned_sources;
    {
        std::lock_guard<std::recursive_mutex> node_lock (_state->mutex);
        for (auto found = _state->pending_remote_source_cleanups.begin ();
             found != _state->pending_remote_source_cleanups.end ();) {
            if (found->not_before > now) {
                ++found;
                continue;
            }
            const auto key = actor_key (found->source_actor);
            const auto generation = _state->actor_generations.find (key);
            const auto route = _state->actor_routes.find (key);
            const bool actor_has_newer_local_incarnation =
              generation != _state->actor_generations.end ()
              && generation->second > found->source_actor.object_generation ()
              && route != _state->actor_routes.end ()
              && route->second.node_rid.value ()
                   == detail::effective_spot_node_rid (_state->snapshot);
            if (!actor_has_newer_local_incarnation) {
                _state->actor_instances.erase (key);
                detail::erase_actor_instance_index_unlocked (
                  *_state,
                  ::zlink::framework::detail::actor_ref_access_t::actor_type (
                    found->source_actor),
                  found->source_actor.actor_id ().value ());
                _state->actor_mailboxes.erase (key);
                release_actor_location (*_state, found->source_actor);
            }
            cleaned_sources.push_back (std::move (*found));
            found = _state->pending_remote_source_cleanups.erase (found);
            ++removed;
        }
    }
    for (const auto &cleanup : cleaned_sources) {
        emit_actor_transfer_marker ("source_cleanup", cleanup.source_actor,
                                    cleanup.transfer_id, cleanup.target_spot_id);
    }
    const auto removed_message_follow_routes =
      _state->actor_transfer_coordinator.remove_expired_message_follow (now);
    if (!removed_message_follow_routes.empty ()) {
        std::lock_guard<std::recursive_mutex> node_lock (_state->mutex);
        for (const auto &entry : removed_message_follow_routes) {
            const auto &key = entry.actor_key;
            // Message Follow ended (§10.4-3): remove the retained route. The
            // generation record remains so stale refs fail fast instead of
            // recreating the actor on this node.
            const auto generation = _state->actor_generations.find (key);
            if (generation == _state->actor_generations.end ()
                || generation->second <= entry.old_generation + 1) {
                _state->actor_routes.erase (key);
                _state->native_actors.erase (key);
            }
            const auto separator = key.find (':');
            if (separator != std::string::npos) {
                const auto actor_ref = ::zlink::framework::detail::actor_ref_access_t::make (
                  node_rid (), key.substr (0, separator), key.substr (separator + 1),
                  entry.old_generation);
                emit_actor_transfer_marker (
                  "message_follow_route_removed", actor_ref,
                  entry.transfer_id.empty () ? key : entry.transfer_id);
            }
            ++removed;
        }
    }
    return removed;
}

std::vector<handoff_packet_t>
spot_node_runtime_t::take_actor_handoff_backlog (const actor_ref_t &actor_ref)
{
    return _state->actor_transfer_coordinator.take_backlog (actor_key (actor_ref));
}

bool spot_node_runtime_t::actor_transfer_in_progress (const actor_ref_t &actor_ref) const
{
    return !::zlink::framework::detail::actor_ref_access_t::empty (actor_ref)
           && _state->actor_transfer_coordinator.blocks_dispatch (actor_key (actor_ref));
}

bool spot_node_runtime_t::actor_transfer_in_progress (std::string_view actor_id) const
{
    std::lock_guard<std::recursive_mutex> node_lock (_state->mutex);
    const auto type = _state->actor_types_by_id.find (std::string (actor_id));
    if (type == _state->actor_types_by_id.end ())
        return false;
    return _state->actor_transfer_coordinator.blocks_dispatch (
      type->second + ":" + std::string (actor_id));
}

void spot_node_runtime_t::set_message_follow_duration (std::chrono::milliseconds duration)
{
    _state->message_follow_duration = duration;
}

void spot_node_runtime_t::bind_relocation_store (
  std::shared_ptr<runtime::stateful::relocation_store_port_t> store)
{
    std::lock_guard<std::recursive_mutex> node_lock (_state->mutex);
    _state->relocation_store = std::move (store);
}

void spot_node_runtime_t::bind_relocation_authority (
  std::shared_ptr<runtime::stateful::authority_relocation_port_t> authority)
{
    std::lock_guard<std::recursive_mutex> node_lock (_state->mutex);
    _state->relocation_authority = std::move (authority);
}

std::vector<std::uint8_t>
spot_node_runtime_t::capture_spot_relocation_state (
  const runtime::stateful::object_ref_t &spot,
  const std::string &stable_type,
  std::stop_token cancellation) const
{
    std::shared_ptr<spot_context_state_t> context;
    detail::factory_relocation_configuration_t relocation;
    {
        std::lock_guard<std::recursive_mutex> lock (_state->mutex);
        const auto found =
          _state->spot_contexts_by_id.find (spot.key);
        const auto configured =
          _state->spot_factory_relocations.find (stable_type);
        if (found == _state->spot_contexts_by_id.end ()
            || found->second._state->spot_name != stable_type
            || configured == _state->spot_factory_relocations.end ()) {
            throw framework_exception_t (
              framework_error_kind_t::not_found,
              "Relocation source Spot is not materialized");
        }
        context = found->second._state;
        relocation = configured->second;
    }
    if (relocation.kind
          != detail::factory_relocation_kind_t::preserve_state)
        return {};
    if (!relocation.capture || !context->spot_instance) {
        throw framework_exception_t (
          framework_error_kind_t::not_configured,
          "State-preserving Spot relocation has no capture callback");
    }
    std::vector<std::byte> payload;
    const auto captured = context->run_serial_task (
      "spot-relocation-capture",
      [&] () -> task_t<void> {
          payload = co_await relocation.capture (
            context->spot_instance.get (), cancellation);
      });
    if (!captured)
        throw framework_exception_t (
          captured.error_kind (),
          captured.error () != nullptr
            ? captured.error ()->what ()
            : "Spot relocation capture failed");
    std::vector<std::uint8_t> output;
    if (payload.size () > max_spot_relocation_state_bytes) {
        throw framework_exception_t (
          framework_error_kind_t::rejected,
          "Spot relocation state exceeds the 64 MiB limit");
    }
    output.reserve (payload.size ());
    for (const auto value : payload)
        output.push_back (std::to_integer<std::uint8_t> (value));
    return output;
}

bool spot_node_runtime_t::restore_spot_relocation_state (
  const runtime::stateful::frozen_object_state_t &frozen,
  const runtime::stateful::object_ref_t &target,
  std::stop_token cancellation)
{
    std::uint64_t reservation = 0;
    std::shared_ptr<std::promise<void>> completion;
    try {
        detail::factory_relocation_configuration_t relocation;
        {
            std::lock_guard<std::recursive_mutex> lock (_state->mutex);
            const auto configured =
              _state->spot_factory_relocations.find (
                frozen.stable_type);
            if (configured
                == _state->spot_factory_relocations.end ())
                return false;
            if (_state->spot_contexts_by_id.contains (target.key)
                || _state->pending_spot_creations_by_id
                     .contains (target.key)
                || _state->next_pending_spot_creation_reservation
                     == 0)
                return false;
            reservation =
              _state->next_pending_spot_creation_reservation++;
            completion =
              std::make_shared<std::promise<void>> ();
            const auto inserted =
              _state->pending_spot_creations_by_id.emplace (
                target.key,
                detail::spot_node_builder_state_t::
                  pending_spot_creation_t{
                    frozen.stable_type,
                    completion->get_future ().share (),
                    reservation});
            if (!inserted.second)
                return false;
            relocation = configured->second;
        }
        if (frozen.application_state.size ()
            > max_spot_relocation_state_bytes)
            throw std::length_error (
              "Spot relocation state exceeds 64 MiB");
        std::function<task_t<void> (void *)> staged_restore;
        if (relocation.kind
            == detail::factory_relocation_kind_t::preserve_state) {
            if (!relocation.restore)
                throw std::logic_error (
                  "Spot relocation restore callback is missing");
            std::vector<std::byte> payload;
            payload.reserve (frozen.application_state.size ());
            for (const auto value : frozen.application_state)
                payload.push_back (static_cast<std::byte> (value));
            staged_restore =
              [relocation = std::move (relocation),
               payload = std::move (payload),
               cancellation] (void *instance) mutable {
                  return relocation.restore (
                    instance, std::move (payload), cancellation);
              };
        } else if (
          relocation.kind
          == detail::factory_relocation_kind_t::recreate) {
            if (!frozen.application_state.empty ())
                throw std::logic_error (
                  "Recreated Spot has application state");
            staged_restore = [] (void *) -> task_t<void> {
                co_return;
            };
        } else {
            throw std::logic_error (
              "Spot relocation policy does not permit restore");
        }
        std::unique_lock<std::recursive_mutex> node_lock (
          _state->mutex);
        const auto owned =
          _state->pending_spot_creations_by_id.find (
            target.key);
        if (owned
              == _state->pending_spot_creations_by_id.end ()
            || owned->second.reservation != reservation)
            throw std::logic_error (
              "Relocation Spot reservation ownership was lost");
        auto materialized = create_spot_context_unlocked (
          frozen.stable_type,
          spot_id_t (target.key),
          zlink::message_t{},
          node_lock,
          target.object_generation,
          target.mesh_name,
          std::move (staged_restore));
        const auto created =
          materialized.state == spot_create_state_t::created;
        const auto current =
          _state->pending_spot_creations_by_id.find (
            target.key);
        if (current
              != _state->pending_spot_creations_by_id.end ()
            && current->second.reservation == reservation) {
            _state->pending_spot_creations_by_id.erase (
              current);
            if (created)
                completion->set_value ();
            else
                completion->set_exception (
                  std::make_exception_ptr (
                    framework_exception_t (
                      framework_error_kind_t::internal_failure,
                      "Relocation Spot activation was rejected")));
        }
        return created;
    }
    catch (...) {
        if (reservation != 0) {
            std::lock_guard<std::recursive_mutex> lock (
              _state->mutex);
            const auto current =
              _state->pending_spot_creations_by_id.find (
                target.key);
            if (current
                  != _state->pending_spot_creations_by_id.end ()
                && current->second.reservation == reservation) {
                _state->pending_spot_creations_by_id.erase (
                  current);
                completion->set_exception (
                  std::current_exception ());
            }
        }
    }
    return false;
}

std::optional<runtime::stateful::durable_join_completion_root_t>
spot_node_runtime_t::pending_join_completion_root (const std::string &transfer_id) const
{
    const auto admission = _state->actor_transfer_coordinator.admission (transfer_id);
    if (!admission || admission->completion_root_reference.empty ())
        return std::nullopt;
    return runtime::stateful::durable_join_completion_root_t{
      admission->completion_root_reference, admission->completion_root_checksum};
}

result_t<void> spot_node_runtime_t::restore_pending_join_completion (
  const std::string &transfer_id,
  const actor_ref_t &actor,
  const spot_id_t &target_spot_id,
  runtime::stateful::durable_join_completion_root_t root)
{
    const auto admission = _state->actor_transfer_coordinator.admission (transfer_id);
    if (!admission || admission->source_actor.actor_id () != actor.actor_id ()
        || admission->source_actor.object_generation () != actor.object_generation ()
        || admission->target_spot_id != target_spot_id) {
        return result_t<void>::failure (
          framework_error_kind_t::protocol_error,
          "Join completion root does not match the pending Actor admission");
    }
    if (!_state->relocation_store || root.reference.empty ()
        || root.checksum_crc32c == 0) {
        return result_t<void>::failure (
          framework_error_kind_t::data_lost,
          "Join completion root cannot be restored without relocation data");
    }
    runtime::stateful::durable_join_completion_store_t store (
      _state->relocation_store);
    const auto record = store.recover (root);
    if (!record
        || record->operation_id_high != admission->completion_operation_id_high
        || record->operation_id_low != admission->completion_operation_id_low
        || record->actor.kind != runtime::stateful::object_kind_t::actor
        || record->actor.key != actor.actor_id ().value ()
        || record->actor.object_generation != actor.object_generation ()) {
        return result_t<void>::failure (
          framework_error_kind_t::data_lost,
          "Join completion root failed identity or checksum validation");
    }
    if (!_state->actor_transfer_coordinator.update_completion_root (
          transfer_id, std::move (root.reference), root.checksum_crc32c)) {
        return result_t<void>::failure (
          framework_error_kind_t::protocol_error,
          "Join completion root no longer has a pending Actor admission");
    }
    return result_t<void>::success ();
}

result_t<spot_actor_join_result_t>
spot_node_runtime_t::admit_remote_actor_to_spot (std::string transfer_id,
                                                 const actor_ref_t &actor_ref,
                                                 spot_id_t source_spot_id,
                                                 spot_id_t target_spot_id,
                                                 const zlink::message_t &request,
                                                 std::uint64_t completion_operation_id_high,
                                                 std::uint64_t completion_operation_id_low)
{
    /* graceful-drain-handoff §4-2/§5.2: a draining node rejects new actor
    * admission and joins; already-admitted transfer commits stay accepted. */
    if (_state->drain_flag && _state->drain_flag->load (std::memory_order_acquire)) {
        return result_t<spot_actor_join_result_t>::failure (
          framework_error_kind_t::rejected,
          "spot node is draining and rejects new actor admission");
    }
    std::unique_lock<std::recursive_mutex> node_lock (_state->mutex);
    cleanup_expired_actor_admissions ();
    if (transfer_id.empty () || ::zlink::framework::detail::actor_ref_access_t::empty (actor_ref)) {
        return result_t<spot_actor_join_result_t>::failure (
          framework_error_kind_t::protocol_error,
          "remote actor admission requires transfer and actor identity");
    }
    auto context = actor_join_context_unlocked (target_spot_id, request);
    if (!context) {
        return detail::propagate_failure<spot_actor_join_result_t> (context, "target spot is not registered");
    }
    auto actor_factory = actor_factory_unlocked (actor_ref);
    if (!actor_factory) {
        return detail::propagate_failure<spot_actor_join_result_t> (actor_factory, "actor factory failed");
    }
    auto admission = actor_admission_unlocked (context.value (),
                                               actor_factory.value ().get ().actor_type,
                                               target_spot_id, actor_ref);
    if (!admission) {
        return detail::propagate_failure<spot_actor_join_result_t> (admission, "actor admission failed");
    }

    auto &target = *context.value ()._state;
    auto &serializers = *target.channel_runtime->serializers;
    spot_actor_join_result_t response;
    // The admission callback is user code on the spot serial queue. It may call back
    // into the framework (sends, joins) that need the node mutex from the serial
    // thread, so the node mutex must not be held across this wait.
    node_lock.unlock ();
    if (!target.run_serial_sync ("spot-actor-admission", [&] {
            response = admission.value ().get ().join (target.spot_instance.get (),
                                                       actor_ref.actor_id ().value (),
                                                       request, serializers);
        })) {
        return result_t<spot_actor_join_result_t>::failure (
          framework_error_kind_t::capacity_exceeded, "spot serial queue is full");
    }
    node_lock.lock ();
    if (response.accepted) {
        const auto timeout =
          _state->channel_runtime
            ? _state->channel_runtime->default_request_timeout
            : std::chrono::duration_cast<std::chrono::milliseconds> (std::chrono::seconds (30));
        if (!_state->actor_transfer_coordinator.try_add_admission (
              std::move (transfer_id),
              pending_actor_admission_t{actor_key (actor_ref), actor_ref,
                                        std::move (source_spot_id), std::move (target_spot_id),
                                        std::chrono::steady_clock::now () + timeout,
                                        completion_operation_id_high,
                                        completion_operation_id_low})) {
            return result_t<spot_actor_join_result_t>::failure (
              framework_error_kind_t::protocol_error,
              "remote actor admission is already pending");
        }
    }
    return result_t<spot_actor_join_result_t>::success (std::move (response));
}

result_t<spot_node_runtime_t::remote_actor_transfer_t>
spot_node_runtime_t::transfer_actor_out (const actor_ref_t &actor_ref,
                                         std::string transfer_id)
{
    std::lock_guard<std::recursive_mutex> node_lock (_state->mutex);
    const auto key = actor_key (actor_ref);
    const auto factory =
      _state->actor_factories.find (std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor_ref)));
    const auto actor = _state->actor_instances.find (key);
    const auto source_spot = _state->actor_spot_ids.find (key);
    if (actor == _state->actor_instances.end () || !actor->second
        || source_spot == _state->actor_spot_ids.end ()) {
        return result_t<remote_actor_transfer_t>::failure (
          framework_error_kind_t::not_found,
          "source actor is not joined to a local spot");
    }
    if (factory == _state->actor_factories.end ()) {
        return result_t<remote_actor_transfer_t>::failure (
          framework_error_kind_t::not_found,
          "source actor factory is not registered");
    }
    if (factory->second.relocation.kind
        == detail::factory_relocation_kind_t::disabled) {
        return result_t<remote_actor_transfer_t>::failure (
          framework_error_kind_t::not_configured,
          "Actor relocation is disabled");
    }
    if (transfer_id.empty ()) {
        transfer_id = key;
    }
    if (!_state->actor_transfer_coordinator.try_begin_source_remote (key,
                                                                     std::move (transfer_id))) {
        return result_t<remote_actor_transfer_t>::failure (framework_error_kind_t::rejected,
                                                           "actor transfer is already in progress");
    }
    // One histogram sample per transfer, taken right at the moving transition
    // (runtime-metrics §4.3): the coordinator now blocks new dispatches, so the
    // counter is exactly the requests still in flight across the move.
    {
        runtime::runtime_metrics_t metrics (_state->monitoring);
        if (metrics.enabled ()) {
            std::size_t pending = 0;
            {
                const std::lock_guard<std::mutex> pending_lock (
                  _state->actor_pending_requests_mutex);
                const auto found = _state->actor_pending_requests.find (key);
                if (found != _state->actor_pending_requests.end ()) {
                    pending = found->second;
                }
            }
            metrics.histogram ("zlink.actor.transfer.pending_requests.count", "{request}",
                               static_cast<double> (pending));
        }
    }
    if (factory->second.relocation.kind
        != detail::factory_relocation_kind_t::preserve_state) {
        return result_t<remote_actor_transfer_t>::success (
          remote_actor_transfer_t{source_spot->second, zlink::message_t{}});
    }
    try {
        auto state = factory->second.capture (
          actor->second.get (), {}).result ();
        if (!state) {
            replay_actor_handoff_until_move_closed (actor_ref, transfer_id);
            return detail::propagate_failure<remote_actor_transfer_t> (state, "actor transfer-out failed");
        }
        std::string payload;
        payload.resize (state.value ().size ());
        std::transform (
          state.value ().begin (), state.value ().end (), payload.begin (),
          [] (std::byte value) { return static_cast<char> (value); });
        return result_t<remote_actor_transfer_t>::success (remote_actor_transfer_t{
          source_spot->second,
          zlink::message_t::from (payload)});
    }
    catch (const framework_exception_t &error) {
        replay_actor_handoff_until_move_closed (actor_ref, transfer_id);
        return detail::result_access_t::failure<remote_actor_transfer_t> (error);
    }
    catch (const std::exception &error) {
        replay_actor_handoff_until_move_closed (actor_ref, transfer_id);
        return result_t<remote_actor_transfer_t>::failure (framework_error_kind_t::internal_failure,
                                                           error.what ());
    }
    catch (...) {
        replay_actor_handoff_until_move_closed (actor_ref, transfer_id);
        return result_t<remote_actor_transfer_t>::failure (framework_error_kind_t::internal_failure,
                                                           "actor transfer-out failed");
    }
}

std::string spot_node_runtime_t::next_actor_transfer_id ()
{
    return _state->actor_transfer_coordinator.next_transfer_id (
      detail::effective_spot_node_rid (_state->snapshot));
}

std::pair<std::uint64_t, std::uint64_t>
spot_node_runtime_t::actor_join_operation_id (
  std::string_view transfer_id) const
{
    /*
     * Actor Join completion replay needs an ID that is independent from the
     * relocation ID. Derive two 64-bit words with independent FNV-1a domains
     * from the source-generated transfer ID. The transfer ID already includes
     * the source node identity and a monotonically increasing sequence.
     */
    constexpr std::uint64_t fnv_prime = 1099511628211ULL;
    auto hash = [] (std::string_view value,
                    std::uint64_t seed) {
        constexpr std::uint64_t prime = 1099511628211ULL;
        auto result = seed;
        for (const auto byte : value) {
            result ^= static_cast<unsigned char> (byte);
            result *= prime;
        }
        return result;
    };
    auto high = hash (transfer_id, 14695981039346656037ULL);
    auto low = hash (transfer_id, 1099511628211ULL);
    low ^= static_cast<std::uint64_t> (transfer_id.size ());
    low *= fnv_prime;
    if (high == 0 && low == 0)
        low = 1;
    return {high, low};
}

result_t<std::shared_ptr<deferred_barrier_t>>
spot_node_runtime_t::reserve_actor_join_barrier (
  const actor_ref_t &actor_ref)
{
    if (::zlink::framework::detail::actor_ref_access_t::empty (actor_ref)) {
        return result_t<std::shared_ptr<deferred_barrier_t>>::failure (
          framework_error_kind_t::not_found,
          "Actor join barrier source is empty");
    }

    std::shared_ptr<runtime::serial_execution_queue_t> queue;
    {
        std::lock_guard<std::recursive_mutex> node_lock (_state->mutex);
        const auto key = actor_key (actor_ref);
        auto &slot = _state->actor_execution_queues[key];
        if (!slot) {
            slot = std::make_shared<runtime::serial_execution_queue_t> (
              *framework_worker_executor (_state),
              runtime::serial_execution_queue_options_t{});
        }
        queue = slot;
    }
    return queue->reserve_barrier_next ("deferred-actor-join");
}

result_t<void>
spot_node_runtime_t::deliver_actor_join_completion (
  const actor_ref_t &actor_ref,
  const actor_join_completion_t &completion,
  std::optional<spot_id_t> source_spot_id)
{
    /*
     * A commit-preceding failure can arrive after the source membership has
     * been sealed or removed. The Actor instance and generation are the
     * completion fence; source_spot_id is only a caller routing hint.
     */
    (void) source_spot_id;
    const auto operation = std::visit (
      [] (const auto &value) {
          return std::pair<std::uint64_t, std::uint64_t>{
            value.operation_id_high, value.operation_id_low};
      },
      completion);
    if (operation.first == 0 && operation.second == 0) {
        return result_t<void>::failure (
          framework_error_kind_t::protocol_error,
          "Actor Join completion operation ID must be non-zero");
    }

    actor_join_completion_callback_t callback;
    std::shared_ptr<void> actor;
    std::shared_ptr<std::mutex> mailbox;
    {
        std::lock_guard<std::recursive_mutex> node_lock (_state->mutex);
        if (_state->delivered_join_completions.contains (operation)
            || _state->delivering_join_completions.contains (operation)) {
            return result_t<void>::success ();
        }

        const auto key = actor_key (actor_ref);
        const auto generation = _state->actor_generations.find (key);
        if (generation != _state->actor_generations.end ()
            && generation->second != actor_ref.object_generation ()) {
            return result_t<void>::failure (
              framework_error_kind_t::invalid_operation,
              "Actor Join completion generation is stale");
        }
        const auto factory =
          _state->actor_factories.find (std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor_ref)));
        const auto instance = _state->actor_instances.find (key);
        if (factory == _state->actor_factories.end ()
            || instance == _state->actor_instances.end ()
            || !instance->second) {
            return result_t<void>::failure (
              framework_error_kind_t::not_found,
              "Actor Join completion Actor is not registered");
        }
        callback = factory->second.on_join_completed;
        actor = instance->second;
        auto &mailbox_slot = _state->actor_mailboxes[key];
        if (!mailbox_slot)
            mailbox_slot = std::make_shared<std::mutex> ();
        mailbox = mailbox_slot;
        _state->delivering_join_completions.insert (operation);
    }

    auto release_delivery = [&] (bool delivered) {
        std::lock_guard<std::recursive_mutex> node_lock (_state->mutex);
        _state->delivering_join_completions.erase (operation);
        if (delivered)
            _state->delivered_join_completions.insert (operation);
    };

    try {
        std::unique_lock actor_mailbox_lock (*mailbox);
        if (callback) {
            auto completed = callback (
              actor.get (),
              std::holds_alternative<actor_join_accepted_t> (completion)
                ? actor_join_completion_outcome_t::accepted
                : (std::holds_alternative<actor_join_rejected_t> (completion)
                     ? actor_join_completion_outcome_t::rejected
                     : actor_join_completion_outcome_t::failed),
              operation.first, operation.second,
              std::get_if<actor_join_accepted_t> (&completion)
                ? &std::get<actor_join_accepted_t> (completion).actor
                : nullptr,
              std::holds_alternative<actor_join_accepted_t> (completion)
                ? std::get<actor_join_accepted_t> (completion).reply
                : (std::holds_alternative<actor_join_rejected_t> (completion)
                     ? std::get<actor_join_rejected_t> (completion).reply
                     : std::optional<message_t>{}),
              std::holds_alternative<actor_join_failed_t> (completion)
                ? std::get<actor_join_failed_t> (completion).error_kind
                : framework_error_kind_t::internal_failure,
              std::holds_alternative<actor_join_failed_t> (completion)
                && detail::is_transient_error (
                  std::get<actor_join_failed_t> (completion).error_kind))
                               .result ();
            if (!completed) {
                release_delivery (false);
                return result_t<void>::failure (
                  completed.error_kind (),
                  completed.error () != nullptr
                    ? completed.error ()->what ()
                    : "Actor Join completion callback failed");
            }
        }
        release_delivery (true);
        return result_t<void>::success ();
    }
    catch (const framework_exception_t &error) {
        release_delivery (false);
        return detail::result_access_t::failure<void> (error);
    }
    catch (const std::exception &error) {
        release_delivery (false);
        return result_t<void>::failure (
          framework_error_kind_t::internal_failure, error.what ());
    }
    catch (...) {
        release_delivery (false);
        return result_t<void>::failure (
          framework_error_kind_t::internal_failure,
          "Actor Join completion callback failed");
    }
}

result_t<void> spot_node_runtime_t::leave_actor_for_remote_transfer (const actor_ref_t &actor_ref)
{
    std::unique_lock<std::recursive_mutex> node_lock (_state->mutex);
    const auto key = actor_key (actor_ref);
    const auto actor = _state->actor_instances.find (key);
    const auto factory = _state->actor_factories.find (std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor_ref)));
    if (actor == _state->actor_instances.end () || !actor->second
        || factory == _state->actor_factories.end ()
        || _state->actor_spot_ids.find (key) == _state->actor_spot_ids.end ()) {
        return result_t<void>::failure (framework_error_kind_t::not_found,
                                        "source actor is not joined to a local spot");
    }
    if (_state->actor_transfer_coordinator.phase (key)
        != std::make_optional (actor_move_phase_t::source_remote)) {
        return result_t<void>::failure (framework_error_kind_t::rejected,
                                        "actor transfer has not been prepared");
    }
    try {
        leave_previous_actor_route (key, factory->second.actor_type, actor->second.get (),
                                    node_lock);
        return result_t<void>::success ();
    }
    catch (const framework_exception_t &error) {
        replay_actor_handoff_until_move_closed (actor_ref, key);
        return detail::result_access_t::failure<void> (error);
    }
    catch (const std::exception &error) {
        replay_actor_handoff_until_move_closed (actor_ref, key);
        return result_t<void>::failure (framework_error_kind_t::internal_failure, error.what ());
    }
    catch (...) {
        replay_actor_handoff_until_move_closed (actor_ref, key);
        return result_t<void>::failure (framework_error_kind_t::internal_failure,
                                        "source actor leave failed");
    }
}

void spot_node_runtime_t::fail_remote_actor_transfer (const actor_ref_t &actor_ref, bool reconcile)
{
    const auto key = actor_key (actor_ref);
    if (reconcile) {
        _state->actor_transfer_coordinator.mark_reconcile (key);
    } else {
        replay_actor_handoff_until_move_closed (actor_ref, key);
    }
}

void spot_node_runtime_t::complete_remote_actor_transfer (const actor_ref_t &source_actor,
                                                          const actor_ref_t &target_actor,
                                                          spot_route_t target_route,
                                                          std::string transfer_id)
{
    std::vector<handoff_packet_t> late_handoff;
    std::function<result_t<std::optional<zlink::message_t>> (
      const actor_ref_t &,
      const runtime::messaging::envelope_header_t &,
      const zlink::message_t &,
      std::chrono::milliseconds,
      const zlink::routing_id_t &,
      const runtime::protocol::actor_route_fence_t &,
      std::uint8_t,
      const runtime::protocol::wire_operation_id_t &,
      std::uint64_t)> late_handoff_relay;
    std::unique_lock<std::recursive_mutex> node_lock (_state->mutex);
    const auto key = actor_key (source_actor);
    if (transfer_id.empty ()) {
        transfer_id = key;
    }
    const auto late_transfer_id = transfer_id;
    const auto target_spot_id = target_route.spot_id;
    const auto move_completion =
      _state->actor_transfer_coordinator.complete_move_and_take_backlog (key);
    const auto transfer_elapsed = move_completion.elapsed;
    if (transfer_elapsed) {
        // Commit ack confirmed: one transfers count and one duration sample per
        // completed out→commit-ack move (runtime-metrics §4.3, RMETRIC-004).
        runtime::runtime_metrics_t metrics (_state->monitoring);
        if (metrics.enabled ()) {
            metrics.counter ("zlink.actor.transfers", "{transfer}", 1.0);
            metrics.histogram ("zlink.actor.transfer.duration", "s",
                               std::chrono::duration<double> (*transfer_elapsed).count ());
        }
    }
    // Keep the old-generation Message Follow route independent from the actor's
    // current route. On A→B→A, recording the final local route must not erase
    // the generation-1 A→B hop that still chains through B to generation 3.
    _state->actor_transfer_coordinator.activate_message_follow (
      key, source_actor.object_generation (), target_actor, target_route,
      std::chrono::steady_clock::now () + _state->message_follow_duration,
      transfer_id);
    detail::record_actor_route_unlocked (*_state, key, std::move (target_route),
                                         target_actor.object_generation ());
    // Commit acknowledgement fixes the new owner and Message Follow route first.
    // Releasing stale source ownership is post-commit housekeeping: losing the
    // source process now cannot roll back the accepted target generation.
    _state->pending_remote_source_cleanups.push_back (
      spot_node_builder_state_t::pending_remote_source_cleanup_t{
        source_actor, std::move (transfer_id), target_spot_id,
        std::chrono::steady_clock::now () + std::chrono::seconds (1)});
    // Packets that arrive after the first backlog snapshot but before the
    // commit acknowledgement belong to the target generation as well. The
    // source no longer owns an execution queue after this point, so extract
    // them before releasing the coordinator state and relay them through the
    // already authenticated Message Follow route.
    late_handoff = std::move (move_completion.backlog);
    late_handoff_relay = _state->actor_message_follow_relay;
    node_lock.unlock ();

    if (late_handoff.empty ())
        return;
    if (!late_handoff_relay) {
        emit_actor_transfer_marker ("handoff_late_relay_unavailable", source_actor,
                                    late_transfer_id);
        return;
    }
    for (auto &packet : late_handoff) {
        const auto handoff_source = handoff_routing_id (
          packet.metadata, actor_handoff_source_node_key);
        const auto handoff_route = handoff_actor_route (packet.metadata);
        const auto handoff_hop = handoff_u64 (
          packet.metadata, actor_handoff_hop_count_key);
        const auto handoff_operation_high = handoff_u64 (
          packet.metadata, actor_handoff_operation_high_key);
        const auto handoff_operation_low = handoff_u64 (
          packet.metadata, actor_handoff_operation_low_key);
        const auto handoff_reply_route = handoff_u64 (
          packet.metadata, actor_handoff_reply_route_key);
        const bool preserves_route_context =
          handoff_source && handoff_route && handoff_hop && *handoff_hop <= 8
          && handoff_operation_high && handoff_operation_low && handoff_reply_route;
        runtime::messaging::envelope_header_t header;
        header.kind = packet.is_request
                        ? runtime::messaging::message_kind_t::request
                        : runtime::messaging::message_kind_t::command;
        header.channel_name = "actor";
        header.message_name = std::move (packet.packet_name);
        header.content_type = std::move (packet.content_type);
        header.metadata = std::move (packet.metadata);
        const auto relayed = late_handoff_relay (
          source_actor, header, zlink::message_t::from (std::move (packet.payload)),
          std::chrono::seconds (30),
          preserves_route_context
            ? *handoff_source
            : zlink::routing_id_t::from (std::uint32_t{0}),
          preserves_route_context
            ? *handoff_route
            : runtime::protocol::actor_route_fence_t{},
          preserves_route_context
            ? static_cast<std::uint8_t> (*handoff_hop)
            : 0,
          preserves_route_context
            ? runtime::protocol::wire_operation_id_t{
                *handoff_operation_high, *handoff_operation_low}
            : runtime::protocol::wire_operation_id_t{},
          preserves_route_context ? *handoff_reply_route : 0);
        if (!relayed) {
            emit_actor_transfer_marker ("handoff_late_relay_failed", source_actor,
                                        key);
        }
    }
}

void spot_node_runtime_t::emit_actor_transfer_marker (
  std::string marker,
  const actor_ref_t &actor_ref,
  std::string transfer_id,
  std::optional<spot_id_t> spot_id,
  std::optional<node_rid_t> target_node_rid) const
{
    message_flow_tracer_t (_state->dispatch).trace (
      message_flow_outcome_t::dispatched,
      [marker = std::move (marker), actor_ref, transfer_id = std::move (transfer_id),
       spot_id = std::move (spot_id), target_node_rid = std::move (target_node_rid),
       node_rid = node_rid ()] () mutable {
          return message_flow_event_t{
            .outcome = message_flow_outcome_t::dispatched,
            .surface = dispatch_error_surface_t::spot_actor,
            .message_kind = dispatch_message_kind_t::actor_request,
            .packet_name = std::move (marker),
            .channel_name = target_node_rid
                              ? std::make_optional (
                                  std::string (target_node_rid->value ()))
                              : std::nullopt,
            .correlation_id = transfer_id,
            .source_rid = std::string (node_rid.value ()),
            .spot_id = spot_id,
            .actor_id = std::string (actor_ref.actor_id ().value ()),
            .flow_id = std::move (transfer_id),
            .flow_origin = flow_origin_t::lifecycle};
      });
}

result_t<actor_join_reply_t>
spot_node_runtime_t::prepare_remote_actor_to_spot (std::string transfer_id,
                                                   const actor_ref_t &actor_ref,
                                                   spot_id_t target_spot_id,
                                                   zlink::message_t transfer_state,
                                                   actor_context_t actor_context,
                                                   bool defer_joined_callback)
{
    std::unique_lock<std::recursive_mutex> node_lock (_state->mutex);
    cleanup_expired_actor_admissions ();
    const auto pending =
      _state->actor_transfer_coordinator.begin_commit (transfer_id, actor_ref, target_spot_id);
    if (!pending) {
        return result_t<actor_join_reply_t>::failure (
          framework_error_kind_t::protocol_error,
          "remote actor commit has no matching admission");
    }
    auto factory = actor_factory_unlocked (actor_ref);
    auto context = find_context (target_spot_id);
    if (!factory || !context || !context->_state->spot_instance) {
        _state->actor_transfer_coordinator.fail_commit (transfer_id, false);
        return result_t<actor_join_reply_t>::failure (
          framework_error_kind_t::not_found,
          "remote actor commit dependencies are not registered");
    }

    const auto committed =
      ::zlink::framework::detail::actor_ref_access_t::make (node_rid_t::from_string (detail::effective_spot_node_rid (_state->snapshot)),
                   std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor_ref)), std::string (actor_ref.actor_id ().value ()),
                   actor_ref.object_generation ());
    bool claimed_location = false;
    auto location_claim = claim_pending_actor_location_before_activation (
      _state, pending->source_actor, pending->source_spot_id, committed, *context->_state,
      claimed_location);
    if (!location_claim) {
        _state->actor_transfer_coordinator.fail_commit (transfer_id, false);
        return result_t<actor_join_reply_t>::failure (
          location_claim.error_kind (), location_claim.error () ? location_claim.error ()->what ()
                                                                : "actor location claim failed");
    }

    auto fail_target_commit = [&] (bool reconcile) {
        if (claimed_location) {
            release_actor_location (*_state, committed);
        }
        _state->actor_transfer_coordinator.fail_commit (transfer_id, reconcile);
    };

    auto committed_context = actor_context_t (
      actor_context._state, committed, 0,
      context->_state->mesh_name);
    std::shared_ptr<void> actor;
    try {
        auto &registration = factory.value ().get ();
        actor = registration.create_context_instance
                  ? registration.create_context_instance (
                      std::move (committed_context))
                  : registration.create_instance (
                      std::string (actor_ref.actor_id ().value ()));
        if (actor && !registration.create_context_instance) {
            registration.configure_instance (
              actor.get (), committed, &committed_context);
        }
        if (actor
            && registration.relocation.kind
                 == detail::factory_relocation_kind_t::preserve_state) {
            const auto bytes = transfer_state.to_bytes ();
            std::vector<std::byte> payload;
            payload.reserve (bytes.size ());
            std::transform (
              bytes.begin (), bytes.end (), std::back_inserter (payload),
              [] (std::uint8_t value) {
                  return static_cast<std::byte> (value);
              });
            auto restored = registration.restore (
              actor.get (), std::move (payload), {}).result ();
            if (!restored) {
                fail_target_commit (false);
                return result_t<actor_join_reply_t>::failure (
                  restored.error_kind (),
                  restored.error () ? restored.error ()->what ()
                                     : "actor restore failed");
            }
        }
    }
    catch (const framework_exception_t &error) {
        fail_target_commit (false);
        return detail::result_access_t::failure<actor_join_reply_t> (error);
    }
    catch (const std::exception &error) {
        fail_target_commit (false);
        return result_t<actor_join_reply_t>::failure (framework_error_kind_t::internal_failure,
                                                      error.what ());
    }
    catch (...) {
        fail_target_commit (false);
        return result_t<actor_join_reply_t>::failure (framework_error_kind_t::internal_failure,
                                                      "actor transfer-in failed");
    }
    if (!actor) {
        fail_target_commit (false);
        return result_t<actor_join_reply_t>::failure (framework_error_kind_t::not_found,
                                                      "actor materialization returned no actor");
    }

    const auto admission =
      context->_state->actor_admissions.find (factory.value ().get ().actor_type);
    if (admission == context->_state->actor_admissions.end ()) {
        fail_target_commit (false);
        return result_t<actor_join_reply_t>::failure (
          framework_error_kind_t::not_found,
          "target spot actor lifecycle is not registered");
    }

    auto &target = *context->_state;
    // The joined callback is user code on the spot serial queue. It may call back
    // into the framework (sends, joins) that need the node mutex from the serial
    // thread, so the node mutex must not be held across this wait.
    node_lock.unlock ();
    try {
        if (!defer_joined_callback && admission->second.on_actor_joined) {
            const auto completed = target.run_serial_task (
              "spot-actor-transfer-joined", [&] () -> task_t<void> {
                  const auto updated =
                    actor_gateway_runtime_t (actor_context._state).update_actor_ref (committed);
                  if (!updated) {
                      throw framework_exception_t (
                        updated.error_kind (), updated.error () != nullptr
                                                  ? updated.error ()->what ()
                                                  : "target actor gateway ref update failed");
                  }
                  co_await admission->second.on_actor_joined (target.spot_instance.get (),
                                                               actor.get ());
              });
            if (!completed) {
                node_lock.lock ();
                fail_target_commit (false);
                return result_t<actor_join_reply_t>::failure (
                  completed.error_kind (), completed.error () != nullptr
                                              ? completed.error ()->what ()
                                              : "spot actor joined callback failed");
            }
        }
        node_lock.lock ();
    }
    catch (const framework_exception_t &error) {
        node_lock.lock ();
        detail::record_actor_instance_index_unlocked (*_state, committed, actor.get ());
        _state->actor_instances[actor_key (committed)] = actor;
        fail_target_commit (true);
        return detail::result_access_t::failure<actor_join_reply_t> (error);
    }
    catch (const std::exception &error) {
        node_lock.lock ();
        detail::record_actor_instance_index_unlocked (*_state, committed, actor.get ());
        _state->actor_instances[actor_key (committed)] = actor;
        fail_target_commit (true);
        return result_t<actor_join_reply_t>::failure (framework_error_kind_t::internal_failure,
                                                      error.what ());
    }
    catch (...) {
        node_lock.lock ();
        detail::record_actor_instance_index_unlocked (*_state, committed, actor.get ());
        _state->actor_instances[actor_key (committed)] = actor;
        fail_target_commit (true);
        return result_t<actor_join_reply_t>::failure (framework_error_kind_t::internal_failure,
                                                      "target joined callback failed");
    }

    const auto key = actor_key (committed);
    detail::record_actor_instance_index_unlocked (*_state, committed, actor.get ());
    _state->actor_instances[key] = std::move (actor);
    _state->destroyed_actor_keys.erase (key);
    // Core installs the target Actor during the finalize transfer fence.
    detail::record_actor_context_route_unlocked (*_state, key,
                                                 detail::effective_spot_node_rid (_state->snapshot),
                                                 target, committed.object_generation ());
    return result_t<actor_join_reply_t>::success (
      actor_join_reply_t{0, committed, zlink::message_t{}});
}

result_t<actor_join_reply_t>
spot_node_runtime_t::commit_remote_actor_to_spot (
  std::string transfer_id,
  const actor_ref_t &actor_ref,
  spot_id_t target_spot_id,
  zlink::message_t transfer_state,
  actor_context_t actor_context,
  std::vector<handoff_packet_t> handoff_backlog,
  service_provider_t *services)
{
    auto prepared = prepare_remote_actor_to_spot (
      transfer_id, actor_ref, target_spot_id, std::move (transfer_state),
      std::move (actor_context));
    if (!prepared) {
        return prepared;
    }
    if (!handoff_backlog.empty () && services == nullptr) {
        _state->actor_transfer_coordinator.fail_commit (transfer_id, true);
        return result_t<actor_join_reply_t>::failure (
          framework_error_kind_t::protocol_error,
          "remote actor handoff backlog requires a service provider");
    }
    if (services != nullptr) {
        return finalize_remote_actor_to_spot (
          std::move (transfer_id), actor_ref, std::move (target_spot_id),
          std::move (handoff_backlog), *services);
    }
    service_collection_t empty_services;
    auto empty_provider = empty_services.build_provider ();
    return finalize_remote_actor_to_spot (
      std::move (transfer_id), actor_ref, std::move (target_spot_id), {}, empty_provider);
}

result_t<actor_join_reply_t>
spot_node_runtime_t::finalize_remote_actor_to_spot (
  std::string transfer_id,
  const actor_ref_t &actor_ref,
  spot_id_t target_spot_id,
  std::vector<handoff_packet_t> handoff_backlog,
  service_provider_t &services,
  actor_gateway_runtime_t *actor_gateway)
{
    std::unique_lock<std::recursive_mutex> node_lock (_state->mutex);
    const auto pending = _state->actor_transfer_coordinator.pending_commit (
      transfer_id, actor_ref, target_spot_id);
    if (!pending) {
        return result_t<actor_join_reply_t>::failure (
          framework_error_kind_t::protocol_error,
          "remote actor finalize has no matching prepared commit");
    }
    const auto committed =
      ::zlink::framework::detail::actor_ref_access_t::make (node_rid_t::from_string (detail::effective_spot_node_rid (_state->snapshot)),
                   std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor_ref)), std::string (actor_ref.actor_id ().value ()),
                   actor_ref.object_generation ());
    const auto key = actor_key (committed);
    auto context = find_context (target_spot_id);
    auto factory = actor_factory_unlocked (committed);
    const auto actor = _state->actor_instances.find (key);
    if (!context || !context->_state->spot_instance || !factory
        || actor == _state->actor_instances.end () || !actor->second) {
        _state->actor_transfer_coordinator.fail_commit (transfer_id, true);
        return result_t<actor_join_reply_t>::failure (
          framework_error_kind_t::not_found,
          "prepared remote actor commit dependencies are unavailable");
    }

    auto &target = *context->_state;
    if (actor_gateway != nullptr) {
        const auto admission =
          target.actor_admissions.find (factory.value ().get ().actor_type);
        if (admission == target.actor_admissions.end ()) {
            _state->actor_transfer_coordinator.fail_commit (transfer_id, true);
            return result_t<actor_join_reply_t>::failure (
              framework_error_kind_t::not_found,
              "target spot actor lifecycle is not registered");
        }
        const auto actor_instance = actor->second;
        const auto joined_callback = admission->second.on_actor_joined;
        node_lock.unlock ();
        const auto updated = actor_gateway->update_actor_ref (committed);
        if (!updated) {
            node_lock.lock ();
            _state->actor_transfer_coordinator.fail_commit (transfer_id, true);
            return result_t<actor_join_reply_t>::failure (
              updated.error_kind (), updated.error () ? updated.error ()->what ()
                                                       : "target actor gateway ref update failed");
        }
        if (joined_callback) {
            const auto completed = target.run_serial_task (
              "spot-actor-transfer-joined",
              [&] { return joined_callback (target.spot_instance.get (), actor_instance.get ()); });
            if (!completed) {
                node_lock.lock ();
                _state->actor_transfer_coordinator.fail_commit (transfer_id, true);
                return result_t<actor_join_reply_t>::failure (
                  completed.error_kind (), completed.error () ? completed.error ()->what ()
                                                               : "spot actor joined callback failed");
            }
        }
        node_lock.lock ();
    }
    auto &target_serializers = *target.channel_runtime->serializers;
    for (auto &packet : handoff_backlog) {
        emit_actor_transfer_marker ("backlog_enqueued", committed, transfer_id,
                                    target_spot_id);
        const auto message = zlink::message_t::from (packet.payload);
        spot_inbound_message_t metadata;
        metadata.content_type = std::move (packet.content_type);
        metadata.values = std::move (packet.metadata);
        std::string replay_request_id;
        if (packet.is_request) {
            const auto id_it = metadata.values.find ("__zlink.actorRequestId");
            if (id_it != metadata.values.end () && !id_it->second.empty ()) {
                replay_request_id = id_it->second;
                report_actor_handoff_request_trace (
                  _state, "backlog_request_frame", committed, replay_request_id, transfer_id);
                const auto claim = _state->dispatched_request_replies.claim (
                  actor_request_dedup_key (key, replay_request_id));
                if (claim.state != runtime::exactly_once_claim_state::claimed) {
                    continue;
                }
            }
        }
        const auto handler_kind = packet.is_request ? spot_handler_kind_t::actor_request
                                                    : spot_handler_kind_t::actor_send;
        auto task = spot_handler_registry_t (context->_state)
                      .invoke_erased (handler_kind, packet.packet_name, {},
                                      factory.value ().get ().actor_type,
                                      target.spot_instance.get (), actor->second.get (), services,
                                      target_serializers, message, std::move (metadata), true,
                                      key, std::string (target_spot_id));
        auto task_holder = std::make_shared<task_t<zlink::message_t>> (std::move (task));
        detail::observe_task_completion (
          *task_holder, [task_holder, node_state = _state, key, replay_request_id] (
                          const result_t<zlink::message_t> &completed) {
              if (replay_request_id.empty ()) {
                  return;
              }
              const auto dedup_key = actor_request_dedup_key (key, replay_request_id);
              if (completed) {
                  (void) node_state->dispatched_request_replies.complete (
                    dedup_key, completed.value ());
              } else {
                  (void) node_state->dispatched_request_replies.erase (dedup_key);
              }
          });
    }

    const auto location_updated =
      update_actor_location_after_move (*_state, committed, target, false);
    if (!location_updated) {
        _state->actor_transfer_coordinator.fail_commit (transfer_id, true);
        return result_t<actor_join_reply_t>::failure (location_updated.error_kind (),
                                                      location_updated.error ()
                                                        ? location_updated.error ()->what ()
                                                        : "actor committed location update failed");
    }
    emit_actor_transfer_marker ("location_committed", committed, transfer_id,
                                target_spot_id);
    if (_state->update_actor_registry_ref) {
        const auto updated = _state->update_actor_registry_ref (committed);
        if (!updated) {
            _state->actor_transfer_coordinator.fail_commit (transfer_id, true);
            return detail::propagate_failure<actor_join_reply_t> (updated, "actor ref update failed");
        }
    }
    _state->actor_transfer_coordinator.complete_commit (transfer_id);
    return result_t<actor_join_reply_t>::success (
      actor_join_reply_t{0, committed, zlink::message_t{}});
}

result_t<actor_join_reply_t> spot_node_runtime_t::join_actor_to_entry_spot_erased (
  const actor_ref_t &actor_ref,
  node_rid_t spot_node_rid,
  const zlink::message_t &request,
  const std::optional<zlink::message_t> &actor_snapshot,
  actor_context_t actor_context)
{
    /* graceful-drain-handoff §4-2/§5.2: a draining node rejects new actor
    * admission and joins; already-admitted transfer commits stay accepted. */
    if (_state->drain_flag && _state->drain_flag->load (std::memory_order_acquire)) {
        return result_t<actor_join_reply_t>::failure (framework_error_kind_t::rejected,
                                                      "spot node is draining and rejects new actor joins");
    }
    if (spot_node_rid.empty ()
        || spot_node_rid.value () != detail::effective_spot_node_rid (_state->snapshot)) {
        return result_t<actor_join_reply_t>::failure (framework_error_kind_t::not_found,
                                                      "spot node rid does not match this node");
    }
    if (!_state->snapshot.entry_spot_name) {
        return result_t<actor_join_reply_t>::failure (framework_error_kind_t::not_found,
                                                      "entry spot is not registered");
    }
    const auto entry_id = _state->spot_ids_by_name.find (*_state->snapshot.entry_spot_name);
    if (entry_id == _state->spot_ids_by_name.end ()) {
        return result_t<actor_join_reply_t>::failure (framework_error_kind_t::not_found,
                                                      "entry spot is not created");
    }
    return join_actor_to_spot_erased (actor_ref, entry_id->second, request, actor_snapshot,
                                      std::move (actor_context));
}

void spot_node_runtime_t::on_destroy_actor (
  std::function<result_t<void> (const actor_ref_t &)> destroy_actor)
{
    _state->destroy_actor_registry = std::move (destroy_actor);
}

void spot_node_runtime_t::on_actor_ref_updated (
  std::function<result_t<void> (const actor_ref_t &)> update_actor)
{
    _state->update_actor_registry_ref = std::move (update_actor);
}

void spot_node_runtime_t::on_actor_entry_spot_join (
  std::function<result_t<actor_join_reply_t> (const actor_ref_t &,
                                              node_rid_t,
                                              const zlink::message_t &,
                                              const std::optional<zlink::message_t> &)> join)
{
    _state->actor_entry_spot_join = std::move (join);
}

void spot_node_runtime_t::on_actor_packet_relay (
  std::function<result_t<std::optional<zlink::message_t>> (const actor_ref_t &,
                                                           actor_context_t,
                                                           stream_message_kind_t,
                                                           std::string_view,
                                                           const zlink::message_t &,
                                                           service_provider_t &,
                                                           serializer_registry_t &,
                                                           spot_inbound_message_t)> relay)
{
    _state->actor_packet_relay = std::move (relay);
}

void spot_node_runtime_t::on_actor_message_follow (
  std::function<result_t<std::optional<zlink::message_t>> (
    const actor_ref_t &,
    const runtime::messaging::envelope_header_t &,
    const zlink::message_t &,
    std::chrono::milliseconds,
    const zlink::routing_id_t &,
    const runtime::protocol::actor_route_fence_t &,
    std::uint8_t,
    const runtime::protocol::wire_operation_id_t &,
    std::uint64_t)> relay)
{
    _state->actor_message_follow_relay = std::move (relay);
}

void spot_node_runtime_t::invalidate_message_follow_route (
  const runtime::protocol::message_follow_notice_t &notice)
{
    const auto *source = std::get_if<runtime::protocol::spot_route_fence_t> (
      &notice.source);
    if (!source || !_state->spot_location_resolver)
        return;
    runtime::spot_address_t expected;
    expected.spot_id = source->spot_id;
    expected.node_rid = zlink::routing_id_t::from (
      source->target_node_routing_id);
    expected.node_generation = source->target_node_generation;
    expected.object_generation = source->object_generation;
    expected.authority_owner_generation =
      source->authority_owner_generation;
    expected.owner.lease_generation =
      static_cast<std::int64_t> (source->owner_lease_generation);
    (void) _state->spot_location_resolver
      ->invalidate_spot_address_if_matches (source->spot_id, expected);
}

result_t<std::optional<zlink::message_t>>
spot_node_runtime_t::relay_actor_packet (const actor_ref_t &actor_ref,
                                         actor_context_t actor_context,
                                         std::string_view packet_name,
                                         const zlink::message_t &message,
                                         service_provider_t &services,
                                         serializer_registry_t &serializers,
                                         spot_inbound_message_t metadata)
{
    return relay_actor_packet (actor_ref, std::move (actor_context), stream_message_kind_t::request,
                               packet_name, message, services, serializers, std::move (metadata));
}

result_t<std::optional<zlink::message_t>>
spot_node_runtime_t::relay_actor_packet (const actor_ref_t &actor_ref,
                                         actor_context_t actor_context,
                                         stream_message_kind_t message_kind,
                                         std::string_view packet_name,
                                         const zlink::message_t &message,
                                         service_provider_t &services,
                                         serializer_registry_t &serializers,
                                         spot_inbound_message_t metadata)
{
    if (::zlink::framework::detail::actor_ref_access_t::empty (actor_ref)) {
        return result_t<std::optional<zlink::message_t>>::failure (
          framework_error_kind_t::not_found, "actor ref is empty");
    }

    const auto key = actor_key (actor_ref);
    const auto handoff = metadata.values.find ("__zlink.actorHandoffBacklog");
    const auto handoff_transfer_id = metadata.values.find ("__zlink.actorTransferId");
    if (handoff != metadata.values.end () && handoff->second == "true"
        && handoff_transfer_id != metadata.values.end ()
        && !handoff_transfer_id->second.empty ()) {
        emit_actor_transfer_marker ("backlog_enqueued", actor_ref,
                                    handoff_transfer_id->second,
                                    actor_spot (actor_ref));
    }
    if (_state->actor_transfer_coordinator.blocks_dispatch (key)) {
        // In-flight handoff (§10.2-1): actor packets that arrive while the actor
        // is moving are preserved in arrival order and travel to the target with
        // the commit while the bounded temporary queue has room. Sends return
        // the empty success shape so preservation is indistinguishable from
        // immediate dispatch. A request's reply channel cannot move with the
        // actor, so it fails fast as retriable and the caller re-resolves
        // (§10.2-5) or times out. When the temporary queue is full, a request
        // receives Unavailable and a one-way operation is dropped, as required by
        // the relocation contract.
        const bool is_request = message_kind == stream_message_kind_t::request;
        const auto append_result = _state->actor_transfer_coordinator.try_append_backlog (
          key, detail::handoff_packet_t{std::string (packet_name), message.to_bytes (),
                                        metadata.content_type, metadata.values, is_request});
        if (append_result == detail::handoff_append_result_t::appended) {
            emit_actor_transfer_marker (
              "handoff_backlog", actor_ref,
              _state->actor_transfer_coordinator.transfer_id (key).value_or (key));
            if (is_request) {
                const auto request_id = metadata.values.find ("__zlink.actorRequestId");
                if (request_id != metadata.values.end ()) {
                    report_actor_handoff_request_trace (
                      _state, "handoff_request_frame", actor_ref, request_id->second,
                      _state->actor_transfer_coordinator.transfer_id (key).value_or (key));
                }
            }
            if (!is_request) {
                return result_t<std::optional<zlink::message_t>>::success (zlink::message_t{});
            }
        }
        if (append_result == detail::handoff_append_result_t::capacity_exceeded && !is_request) {
            return result_t<std::optional<zlink::message_t>>::success (zlink::message_t{});
        }
        return result_t<std::optional<zlink::message_t>>::failure (
          framework_error_kind_t::unavailable, "actor transfer is in progress");
    }

    const auto actor_type_key = std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor_ref));
    const auto found_factory = _state->actor_factories.find (actor_type_key);
    if (found_factory == _state->actor_factories.end ()) {
        return result_t<std::optional<zlink::message_t>>::failure (
          framework_error_kind_t::not_found, "actor factory is not registered");
    }

    auto &mailbox_slot = _state->actor_mailboxes[key];
    if (!mailbox_slot) {
        mailbox_slot = std::make_shared<std::mutex> ();
    }
    auto actor_mailbox = mailbox_slot;
    auto found_location = _state->actor_spot_ids.find (key);
    if (found_location == _state->actor_spot_ids.end ()
        && _state->destroyed_actor_keys.contains (key)) {
        return result_t<std::optional<zlink::message_t>>::failure (
          framework_error_kind_t::not_found, "actor has been destroyed");
    }
    /* Same double-checked registration as the join paths. */
    std::shared_ptr<void> actor_instance = registered_actor_instance (actor_ref, key);
    if (!actor_instance) {
        auto &registration = found_factory->second;
        std::shared_ptr<void> created_instance;
        if (registration.create_context_instance) {
            auto actor_mesh_name = _state->snapshot.name;
            if (found_location != _state->actor_spot_ids.end ()) {
                const auto actor_spot_context =
                  find_context (found_location->second);
                if (actor_spot_context)
                    actor_mesh_name =
                      actor_spot_context->_state->mesh_name;
            }
            auto materialization_context = actor_context_t (
              actor_context._state, actor_ref,
              actor_context._source_binding_generation,
              std::move (actor_mesh_name));
            created_instance =
              registration.create_context_instance (
                std::move (materialization_context));
        } else {
            created_instance =
              registration.create_instance (
                std::string (actor_ref.actor_id ().value ()));
        }
        if (!created_instance) {
            return result_t<std::optional<zlink::message_t>>::failure (
              framework_error_kind_t::not_found, "actor factory returned null");
        }
        actor_instance = install_actor_instance (actor_ref, key, std::move (created_instance), true);
        if (!actor_instance) {
            return result_t<std::optional<zlink::message_t>>::failure (
              framework_error_kind_t::not_found, "actor has been destroyed");
        }
    }

    if (found_location == _state->actor_spot_ids.end ()) {
        if (!_state->snapshot.entry_spot_name) {
            return result_t<std::optional<zlink::message_t>>::failure (
              framework_error_kind_t::not_found, "entry spot is not registered");
        }
        const auto entry_id = _state->spot_ids_by_name.find (*_state->snapshot.entry_spot_name);
        if (entry_id == _state->spot_ids_by_name.end ()) {
            return result_t<std::optional<zlink::message_t>>::failure (
              framework_error_kind_t::not_found, "entry spot is not created");
        }
        auto entry_context = find_context (entry_id->second);
        if (!entry_context || !entry_context->_state->spot_instance) {
            return result_t<std::optional<zlink::message_t>>::failure (
              framework_error_kind_t::not_found, "entry spot context is not registered");
        }
        auto &entry_state = *entry_context->_state;
        detail::record_actor_context_route_unlocked (
          *_state, key, detail::effective_spot_node_rid (_state->snapshot), entry_state,
          actor_ref.object_generation ());
        bool created_entry_actor = false;
        if (const auto admission =
              entry_state.actor_admissions.find (found_factory->second.actor_type);
            admission != entry_state.actor_admissions.end ()) {
            if (admission->second.on_create_actor && _state->actor_created_keys.insert (key).second) {
                const auto create_request =
                  actor_context.create_payload ().value_or (zlink::message_t{});
                const auto created =
                  admission->second
                    .on_create_actor (entry_state.spot_instance.get (),
                                      actor_instance.get (), create_request,
                                      serializers)
                    .result ();
                if (!created || !created.value ().accepted) {
                    _state->actor_created_keys.erase (key);
                    erase_actor_route_unlocked (*_state, key);
                    return result_t<std::optional<zlink::message_t>>::failure (
                      created ? framework_error_kind_t::rejected
                              : created.error_kind (),
                      created && !created.value ().accepted
                        ? "Entry Spot rejected Actor creation"
                        : (created.error () ? created.error ()->what ()
                                           : "Entry Spot Actor creation failed"));
                }
                created_entry_actor = true;
            }
            if (!created_entry_actor && admission->second.on_actor_joined) {
                const auto completed = entry_state.run_serial_task (
                  "spot-lifecycle-join", [&] {
                      return admission->second.on_actor_joined (
                        entry_state.spot_instance.get (), actor_instance.get ());
                  });
                if (!completed) {
                    return result_t<std::optional<zlink::message_t>>::failure (
                      completed.error_kind (), completed.error () != nullptr
                                                  ? completed.error ()->what ()
                                                  : "spot actor joined callback failed");
                }
            }
        }
        found_location = _state->actor_spot_ids.find (key);
    }
    if (found_location == _state->actor_spot_ids.end ()) {
        return result_t<std::optional<zlink::message_t>>::failure (
          framework_error_kind_t::not_found,
          "actor spot route disappeared before dispatch");
    }
    // Actor movement can replace or erase the route while the handler is
    // suspended. Keep a value snapshot; a map iterator must not cross the
    // user callback boundary.
    const auto current_spot_id = found_location->second;

    const auto found_generation = _state->actor_generations.find (key);
    if (found_generation != _state->actor_generations.end ()
        && found_generation->second != actor_ref.object_generation ()) {
        // The dispatched ref's generation does not match the actor's current
        // incarnation (§10.4-3). Retriable: for a still-committing local move the
        // published record lags and re-resolving lands the committed generation
        // (ST-A3); for a genuinely stale record the client re-resolves the same
        // answer and eventually surfaces this stale on its own budget timeout.
        emit_actor_transfer_marker (
          "message_follow_expired", actor_ref,
          std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor_ref)) + ":" + std::string (actor_ref.actor_id ().value ()),
          current_spot_id);
        return result_t<std::optional<zlink::message_t>>::failure (
          framework_error_kind_t::unavailable,
          "actor generation is stale. actor=" + std::string (actor_ref.actor_id ().value ())
            + ", current=" + std::to_string (found_generation->second)
            + ", received=" + std::to_string (actor_ref.object_generation ()));
    }
    const auto current_actor_node_rid = actor_ref.node_rid ().empty ()
                                          ? detail::effective_spot_node_rid (_state->snapshot)
                                          : std::string (actor_ref.node_rid ().value ());
    const auto current_actor_ref =
      ::zlink::framework::detail::actor_ref_access_t::make (node_rid_t::from_string (current_actor_node_rid),
                   std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor_ref)), std::string (actor_ref.actor_id ().value ()),
                   found_generation != _state->actor_generations.end () ? found_generation->second
                                                                        : actor_ref.object_generation ());
    auto context = find_context (current_spot_id);
    if (!context || !context->_state->spot_instance) {
        return result_t<std::optional<zlink::message_t>>::failure (
          framework_error_kind_t::not_found,
          "actor spot context is not registered. node=" + _state->snapshot.name
            + ", actor=" + std::string (actor_ref.actor_id ().value ())
            + ", spot=" + current_spot_id);
    }
    if (!found_factory->second.create_context_instance) {
        auto current_actor_context = actor_context_t (
          actor_context._state, current_actor_ref,
          actor_context._source_binding_generation,
          context->_state->mesh_name);
        found_factory->second.configure_instance (
          actor_instance.get (), current_actor_ref,
          &current_actor_context);
    }
    bool dispatch_on_spot_serial = true;
    if (_state->snapshot.entry_spot_name) {
        const auto entry_id = _state->spot_ids_by_name.find (*_state->snapshot.entry_spot_name);
        dispatch_on_spot_serial = entry_id == _state->spot_ids_by_name.end ()
                                  || entry_id->second != current_spot_id;
    }
    if (context->_state->execution_mode
        == user_spot_execution_mode_t::per_actor) {
        dispatch_on_spot_serial = false;
    }

    const auto handler_kind = message_kind == stream_message_kind_t::send
                                ? spot_handler_kind_t::actor_send
                                : spot_handler_kind_t::actor_request;
    const auto dispatch_kind = message_kind == stream_message_kind_t::send
                                 ? dispatch_message_kind_t::actor_send
                                 : dispatch_message_kind_t::actor_request;

    // §10.2-1 exactly-once: a request preserved during the move and also retried
    // by the sender (or replayed by the commit) carries a stable id. The first
    // arrival dispatches; a repeat returns the cached reply (or fails retriable
    // while that first dispatch is still in flight so the sender re-polls).
    std::string dedup_request_id;
    if (message_kind == stream_message_kind_t::request) {
        const auto id_it = metadata.values.find ("__zlink.actorRequestId");
        if (id_it != metadata.values.end () && !id_it->second.empty ()) {
            dedup_request_id = id_it->second;
            const auto claim = _state->dispatched_request_replies.claim (
              actor_request_dedup_key (key, dedup_request_id));
            if (claim.state == runtime::exactly_once_claim_state::completed) {
                if (claim.value) {
                    return result_t<std::optional<zlink::message_t>>::success (
                      *claim.value);
                }
            }
            if (claim.state == runtime::exactly_once_claim_state::pending) {
                return result_t<std::optional<zlink::message_t>>::failure (
                  framework_error_kind_t::unavailable,
                  "actor request dispatch is in flight");
            }
        }
    }
    // In-flight request window for the transfer pending sample (runtime-metrics
    // §4.3): counted from dispatch start until the reply (or error) is produced,
    // so a moving transition that lands mid-dispatch sees this request.
    struct pending_request_scope_t
    {
        std::shared_ptr<detail::spot_node_builder_state_t> state;
        std::string key;

        pending_request_scope_t (std::shared_ptr<detail::spot_node_builder_state_t> state_,
                                 std::string key_) :
            state (std::move (state_)), key (std::move (key_))
        {
        }
        pending_request_scope_t (const pending_request_scope_t &) = delete;
        pending_request_scope_t &operator= (const pending_request_scope_t &) = delete;
        ~pending_request_scope_t ()
        {
            const std::lock_guard<std::mutex> lock (state->actor_pending_requests_mutex);
            const auto found = state->actor_pending_requests.find (key);
            if (found != state->actor_pending_requests.end () && --found->second == 0) {
                state->actor_pending_requests.erase (found);
            }
        }
    };
    std::optional<pending_request_scope_t> pending_request_scope;
    if (message_kind == stream_message_kind_t::request) {
        {
            const std::lock_guard<std::mutex> lock (_state->actor_pending_requests_mutex);
            _state->actor_pending_requests[key]++;
        }
        pending_request_scope.emplace (_state, key);
    }
    report_spot_dispatch_trace (_state, message_flow_outcome_t::received,
                                dispatch_error_surface_t::spot_actor, dispatch_kind, packet_name,
                                {}, current_spot_id, actor_ref.actor_id ().value ());
    std::unique_lock actor_mailbox_lock (*actor_mailbox);
    auto reply =
      spot_handler_registry_t (context->_state)
        .invoke_erased (handler_kind, packet_name, {}, found_factory->second.actor_type,
                        context->_state->spot_instance.get (), actor_instance.get (), services,
                        serializers, message, std::move (metadata),
                        dispatch_on_spot_serial, key, current_spot_id)
        .result ();
    if (!reply) {
        if (!dedup_request_id.empty ()) {
            (void) _state->dispatched_request_replies.erase (
              actor_request_dedup_key (key, dedup_request_id));
        }
        const auto *error = reply.error ();
        const framework_exception_t exception (
          reply.error_kind (), error != nullptr ? error->what () : "actor packet relay failed");
        report_spot_dispatch_error (
          _state, dispatch_error_surface_t::spot_actor, dispatch_kind,
          dispatch_reason_from_error (exception.kind ()), dispatch_error_action_t::reply_error,
          std::string (packet_name), std::nullopt, current_spot_id,
          std::string (actor_ref.actor_id ().value ()), std::make_exception_ptr (exception));
        return detail::result_access_t::failure<std::optional<zlink::message_t>> (exception);
    }
    report_spot_dispatch_trace (_state, message_flow_outcome_t::replied,
                                dispatch_error_surface_t::spot_actor, dispatch_kind, packet_name,
                                {}, current_spot_id, actor_ref.actor_id ().value ());
    if (!dedup_request_id.empty ()) {
        (void) _state->dispatched_request_replies.complete (
          actor_request_dedup_key (key, dedup_request_id), reply.value ());
    }
    return result_t<std::optional<zlink::message_t>>::success (std::move (reply.value ()));
}

result_t<void>
spot_node_runtime_t::notify_actor_disconnected_erased (const actor_ref_t &actor_ref) const
{
    std::lock_guard<std::recursive_mutex> node_lock (_state->mutex);
    if (::zlink::framework::detail::actor_ref_access_t::empty (actor_ref)) {
        return result_t<void>::failure (framework_error_kind_t::not_found,
                                        "actor ref is empty");
    }

    const auto key = actor_key (actor_ref);
    const auto found_generation = _state->actor_generations.find (key);
    if (found_generation != _state->actor_generations.end ()
        && found_generation->second != actor_ref.object_generation ()) {
        return detail::boundary_failure<void> (detail::boundary_error_t::stale_generation,
                                        "actor generation is stale");
    }

    const auto found_location = _state->actor_spot_ids.find (key);
    if (found_location == _state->actor_spot_ids.end ()) {
        return result_t<void>::success ();
    }
    auto context = find_context (found_location->second);
    if (!context || !context->_state->spot_instance) {
        return result_t<void>::success ();
    }

    const auto actor_factory = actor_factory_unlocked (actor_ref);
    if (!actor_factory) {
        return result_t<void>::failure (actor_factory.error_kind (),
                                        actor_factory.error () ? actor_factory.error ()->what ()
                                                               : "actor factory failed");
    }
    const auto actor = _state->actor_instances.find (key);
    if (actor == _state->actor_instances.end () || !actor->second) {
        return result_t<void>::success ();
    }

    const auto admission =
      context->_state->actor_admissions.find (actor_factory.value ().get ().actor_type);
    if (admission == context->_state->actor_admissions.end ()
        || !admission->second.on_disconnect_actor) {
        return result_t<void>::success ();
    }

    try {
        const auto completed = context->_state->run_serial_task (
          "spot-lifecycle-disconnect", [&] {
              return admission->second.on_disconnect_actor (
                context->_state->spot_instance.get (), actor->second.get ());
          });
        if (!completed) {
            return completed;
        }
        return result_t<void>::success ();
    }
    catch (const framework_exception_t &error) {
        return detail::result_access_t::failure<void> (error);
    }
    catch (const std::exception &error) {
        return result_t<void>::failure (framework_error_kind_t::internal_failure, error.what ());
    }
    catch (...) {
        return result_t<void>::failure (framework_error_kind_t::internal_failure,
                                        "spot actor disconnected callback failed");
    }
}

spot_node_runtime_t spot_node_runtime_t::from (const spot_node_builder_t &builder)
{
    return spot_node_runtime_t (builder._state);
}

std::optional<spot_node_runtime_t> spot_node_runtime_t::from (const zlink_builder_t &builder,
                                                              const std::string &spot_node_name)
{
    if (!builder._state)
        return std::nullopt;
    const auto found = builder._state->mesh_nodes.find (spot_node_name);
    if (found != builder._state->mesh_nodes.end () && found->second
        && found->second->spot_state) {
        return spot_node_runtime_t (found->second->spot_state);
    }
    return std::nullopt;
}

std::vector<spot_node_snapshot_t> spot_node_runtime_t::snapshots (
  const zlink_builder_t &builder)
{
    std::vector<spot_node_snapshot_t> result;
    if (!builder._state)
        return result;
    result.reserve (builder._state->mesh_nodes.size ());
    for (const auto &[_, registration] : builder._state->mesh_nodes) {
        if (!registration || !registration->spot_state)
            continue;
        std::lock_guard<std::recursive_mutex> lock (registration->spot_state->mutex);
        result.push_back (registration->spot_state->snapshot);
    }
    return result;
}

local_spot_create_result_t spot_node_runtime_t::create_spot (std::string spot_name)
{
    return create_spot (std::move (spot_name), zlink::message_t{});
}

local_spot_create_result_t spot_node_runtime_t::create_spot_context_unlocked (
  std::string spot_name,
  spot_id_t spot_id,
  zlink::message_t request,
  std::unique_lock<std::recursive_mutex> &node_lock,
  std::uint64_t object_generation,
  std::string mesh_name,
  std::function<task_t<void> (void *)> staged_restore,
  std::uint64_t authority_owner_generation)
{
    /* graceful-drain-handoff §4-2: a draining node blocks new spot creation.
     * Existing spots (and in-progress transfer commits) keep running. */
    if (_state->drain_flag && _state->drain_flag->load (std::memory_order_acquire)) {
        throw framework_exception_t (framework_error_kind_t::rejected,
                                     "spot node is draining and rejects new spot creation");
    }
    const auto found = _state->spot_factories.find (spot_name);
    if (found == _state->spot_factories.end ()) {
        throw framework_exception_t (framework_error_kind_t::internal_failure,
                                     "spot factory is not registered");
    }
    const auto lifecycle =
      _state->spot_lifecycles.find (spot_name) != _state->spot_lifecycles.end ()
        ? _state->spot_lifecycles.at (spot_name)
        : spot_lifecycle_callbacks_t{};
    const auto instance_spot =
      std::find (_state->snapshot.instance_spot_names.begin (),
                 _state->snapshot.instance_spot_names.end (),
                 spot_name)
      != _state->snapshot.instance_spot_names.end ();

    auto context_state = std::make_shared<spot_context_state_t> ();
    context_state->node = _state;
    context_state->channel_runtime = _state->channel_runtime;
    context_state->node_rid =
      node_rid_t::from_string (detail::effective_spot_node_rid (_state->snapshot));
    context_state->mesh_name = mesh_name.empty () ? _state->snapshot.name
                                                   : std::move (mesh_name);
    context_state->spot_id = spot_id;
    context_state->object_generation = object_generation;
    context_state->authority_owner_generation = authority_owner_generation;
    context_state->last_application_work_completed_ns.store (
      std::chrono::duration_cast<std::chrono::nanoseconds> (
        std::chrono::steady_clock::now ().time_since_epoch ())
        .count (),
      std::memory_order_relaxed);
    context_state->spot_name = spot_name;
    const auto entry_spot =
      _state->snapshot.entry_spot_name
      && *_state->snapshot.entry_spot_name == spot_name;
    context_state->kind = entry_spot
                            ? detail::spot_runtime_kind_t::entry
                            : instance_spot
                                ? detail::spot_runtime_kind_t::instance
                                : detail::spot_runtime_kind_t::user;
    if (const auto mode = _state->snapshot.spot_execution_modes.find (spot_name);
        mode != _state->snapshot.spot_execution_modes.end ()) {
        context_state->execution_mode = mode->second;
    }
    if (const auto readiness =
          _state->spot_relocation_readiness.find (spot_name);
        readiness != _state->spot_relocation_readiness.end ()) {
        context_state->relocation_readiness = readiness->second;
    }
    context_state->lifecycle = lifecycle;
    if (_state->root_services) {
        context_state->activation_scope =
          std::make_shared<service_scope_t> (
            service_scope_t::create (
              *_state->root_services,
              context_state->is_entry_spot ()
                ? service_scope_kind_t::entry_spot
                : service_scope_kind_t::spot_activation));
    }
    configure_spot_execution (context_state);
    spot_context_t context (context_state);
    std::optional<message_t> create_reply;
    const auto id_value = std::string (spot_id);

    if (lifecycle.create_entry_context_instance
        || lifecycle.create_instance_context_instance
        || lifecycle.create_spot_context_instance) {
        if (lifecycle.create_entry_context_instance) {
            context_state->spot_instance =
              lifecycle.create_entry_context_instance (
                entry_spot_context_t (context_state));
        } else if (lifecycle.create_instance_context_instance) {
            context_state->spot_instance =
              lifecycle.create_instance_context_instance (
                instance_spot_context_t (context_state));
        } else {
            context_state->spot_instance =
              lifecycle.create_spot_context_instance (
                spot_context_t (context_state));
        }
        if (!context_state->spot_instance) {
            throw framework_exception_t (framework_error_kind_t::internal_failure,
                                         "SPOT factory returned null");
        }
    }

    auto remove_activation = [&] {
        auto native = context_state->native_spot.lock ();
        context_state->native_spot.reset ();
        _state->native_spots_by_id.erase (id_value);
        if (native) {
            node_lock.unlock ();
            try {
                native->close ();
            }
            catch (...) {
            }
            node_lock.lock ();
        }
        context_state->closed = true;
        context_state->detach_application_instance (false);
    };

    if (context_state->spot_instance) {
        spot_create_response_t response;
        try {
            if (staged_restore) {
                node_lock.unlock ();
                auto restored =
                  staged_restore (
                    context_state->spot_instance.get ()).result ();
                if (!restored) {
                    throw framework_exception_t (
                      restored.error_kind (),
                      restored.error () != nullptr
                        ? restored.error ()->what ()
                        : "Spot relocation restore failed");
                }
                node_lock.lock ();
                response = spot_create_response_t::accept ();
            } else {
                auto &serializers =
                  *context_state->channel_runtime->serializers;
                attach_native_spot_locked (context_state);
                node_lock.unlock ();
                response =
                  !context_state->is_instance_spot () && lifecycle.on_create
                    ? lifecycle.on_create (
                        context_state->spot_instance.get (),
                        request, serializers)
                        .result ()
                        .value ()
                    : spot_create_response_t::accept ();
                node_lock.lock ();
            }
        }
        catch (...) {
            if (!node_lock.owns_lock ()) {
                node_lock.lock ();
            }
            remove_activation ();
            throw;
        }
        if (!response.accepted) {
            remove_activation ();
            return local_spot_create_result_t{spot_id, spot_create_state_t::rejected, response.reply,
                                        std::move (context)};
        }
        create_reply = response.reply;
        if (lifecycle.on_initialize) {
            try {
                node_lock.unlock ();
                lifecycle.on_initialize (context_state->spot_instance.get ());
                node_lock.lock ();
            }
            catch (...) {
                if (!node_lock.owns_lock ()) {
                    node_lock.lock ();
                }
                remove_activation ();
                throw;
            }
        }
        if (staged_restore)
            attach_native_spot_locked (context_state);
    }
    else
        attach_native_spot_locked (context_state);

    const auto context_inserted =
      _state->spot_contexts_by_id.emplace (
        id_value, spot_context_t (context_state));
    if (!context_inserted.second) {
        remove_activation ();
        return local_spot_create_result_t{
          spot_id, spot_create_state_t::rejected,
          std::nullopt, std::move (context)};
    }
    _state->spot_ids_by_name[spot_name] = spot_id;
    _state->spot_names_by_id[id_value] = spot_name;
    auto remove_ready_activation = [&] {
        _state->spot_ids_by_name.erase (spot_name);
        _state->spot_names_by_id.erase (id_value);
        _state->spot_contexts_by_id.erase (id_value);
        remove_activation ();
    };

    if (_state->location_lifecycle) {
        auto location = make_spot_location (*_state, spot_name, spot_id);
        if (const auto native = context_state->native_spot.lock ())
            location.spot_generation = native->status ().lifecycle_generation ();
        const auto claimed =
          _state->location_lifecycle->claim_spot (std::move (location));
        if (claimed.status != location_write_status_t::stored) {
            remove_ready_activation ();
            return local_spot_create_result_t{spot_id, spot_create_state_t::rejected, std::nullopt,
                                        std::move (context)};
        }
    }

    if (_state->monitoring) {
        runtime::runtime_metrics_t metrics (_state->monitoring);
        if (metrics.enabled ()) {
            const auto kind = _state->snapshot.entry_spot_name
                                  && *_state->snapshot.entry_spot_name == spot_name
                                ? "entry"
                                : "user";
            metrics.counter ("zlink.spot.created", "{spot}", 1, {{"kind", kind}});
            metrics.updown ("zlink.spot.count", "{spot}", 1, {{"kind", kind}});
        }
    }
    return local_spot_create_result_t{
      spot_id, spot_create_state_t::created, create_reply, std::move (context)};
}

local_spot_create_result_t spot_node_runtime_t::create_spot (std::string spot_name,
                                                       zlink::message_t request)
{
    std::unique_lock<std::recursive_mutex> node_lock (_state->mutex);
    const auto is_entry_spot =
      _state->snapshot.entry_spot_name && *_state->snapshot.entry_spot_name == spot_name;
    auto spot_id = is_entry_spot
                     ? detail::new_entry_spot_id (_state->snapshot.name)
                     : detail::new_user_spot_id ();
    return create_spot_context_unlocked (std::move (spot_name), std::move (spot_id),
                                         std::move (request), node_lock);
}

local_spot_create_result_t spot_node_runtime_t::get_or_create_spot (std::string spot_name,
                                                              spot_id_t spot_id)
{
    return get_or_create_spot (std::move (spot_name), std::move (spot_id), zlink::message_t{});
}

local_spot_create_result_t spot_node_runtime_t::get_or_create_spot (std::string spot_name,
                                                              spot_id_t spot_id,
                                                              zlink::message_t request,
                                                              std::uint64_t object_generation,
                                                              std::string mesh_name,
                                                              std::uint64_t authority_owner_generation)
{
    std::unique_lock<std::recursive_mutex> node_lock (_state->mutex);
    const auto id_value = std::string (spot_id);
    auto same_spot_type = [&] (const std::string &existing_name) {
        const auto existing_factory = _state->spot_factories.find (existing_name);
        const auto requested_factory = _state->spot_factories.find (spot_name);
        return existing_factory == _state->spot_factories.end ()
               || requested_factory == _state->spot_factories.end ()
               || existing_factory->second == requested_factory->second;
    };
    if (const auto existing = _state->spot_contexts_by_id.find (id_value);
        existing != _state->spot_contexts_by_id.end ()) {
        const auto existing_name = _state->spot_names_by_id.find (id_value);
        if (existing_name != _state->spot_names_by_id.end ()
            && !same_spot_type (existing_name->second)) {
            throw framework_exception_t (framework_error_kind_t::type_mismatch,
                                         "spot id is already bound to a different spot type");
        }
        return local_spot_create_result_t{spot_id, spot_create_state_t::existing, std::nullopt,
                                    spot_context_t (existing->second._state)};
    }
    if (const auto pending = _state->pending_spot_creations_by_id.find (id_value);
        pending != _state->pending_spot_creations_by_id.end ()) {
        if (!same_spot_type (pending->second.spot_name)) {
            throw framework_exception_t (framework_error_kind_t::type_mismatch,
                                         "spot id is already bound to a different spot type");
        }
        auto future = pending->second.future;
        node_lock.unlock ();
        future.get ();
        node_lock.lock ();
        const auto created = _state->spot_contexts_by_id.find (id_value);
        if (created == _state->spot_contexts_by_id.end ()) {
            throw framework_exception_t (framework_error_kind_t::internal_failure,
                                         "concurrent spot creation completed without a context");
        }
        return local_spot_create_result_t{
          spot_id_t (id_value), spot_create_state_t::existing, std::nullopt,
          spot_context_t (created->second._state)};
    }

    auto promise = std::make_shared<std::promise<void>> ();
    if (_state->next_pending_spot_creation_reservation == 0)
        throw framework_exception_t (
          framework_error_kind_t::internal_failure,
          "spot creation reservation sequence is exhausted");
    const auto reservation =
      _state->next_pending_spot_creation_reservation++;
    const auto pending_inserted =
      _state->pending_spot_creations_by_id.emplace (
      id_value, detail::spot_node_builder_state_t::pending_spot_creation_t{
                   spot_name, promise->get_future ().share (),
                   reservation});
    if (!pending_inserted.second)
        throw framework_exception_t (
          framework_error_kind_t::internal_failure,
          "spot creation reservation was not acquired");
    try {
        auto result = create_spot_context_unlocked (std::move (spot_name), std::move (spot_id),
                                                    std::move (request), node_lock,
                                                    object_generation, std::move (mesh_name),
                                                    {}, authority_owner_generation);
        const auto owned =
          _state->pending_spot_creations_by_id.find (id_value);
        if (owned == _state->pending_spot_creations_by_id.end ()
            || owned->second.reservation != reservation)
            throw framework_exception_t (
              framework_error_kind_t::internal_failure,
              "spot creation reservation ownership was lost");
        _state->pending_spot_creations_by_id.erase (owned);
        promise->set_value ();
        return std::move (result);
    }
    catch (...) {
        if (!node_lock.owns_lock ()) {
            node_lock.lock ();
        }
        const auto owned =
          _state->pending_spot_creations_by_id.find (id_value);
        if (owned != _state->pending_spot_creations_by_id.end ()
            && owned->second.reservation == reservation) {
            _state->pending_spot_creations_by_id.erase (owned);
            promise->set_exception (std::current_exception ());
        }
        throw;
    }
}

task_t<zlink::message_t> spot_node_runtime_t::dispatch_instance_activation (
  const spot_id_t &spot_id,
  std::string packet_name,
  std::string content_type,
  std::vector<std::uint8_t> payload,
  std::map<std::string, std::string> metadata,
  bool request,
  std::string correlation_id,
  service_provider_t &services,
  serializer_registry_t &serializers,
  std::optional<std::string> flow_id,
  std::optional<flow_origin_t> flow_origin)
{
    /* Instance Spot cold activation carries the first application message
     * outside the normal Spot route record. Keep it in the same diagnostic
     * stream as a Ready Spot dispatch so an operator can follow activation,
     * handler completion, and a reply in one file. The activation payload
     * carries the framework-owned flow pair when tracing is enabled; a target
     * creates a new inbound flow only when the source did not carry one. */
    const auto capture_flow =
      detail::message_flow_tracer_t (_state->dispatch).capture_enabled ();
    auto flow_scope = capture_flow
                        ? runtime::flow_context_t::enter (
                            std::move (flow_id), flow_origin, true,
                            flow_origin_t::inbound)
                        : runtime::flow_context_t::enter_current_or_create (
                            flow_origin_t::inbound, false);
    auto context = find_context (spot_id);
    if (!context || !context->_state->spot_instance
        || std::find (_state->snapshot.instance_spot_names.begin (),
                      _state->snapshot.instance_spot_names.end (),
                      context->_state->spot_name)
             == _state->snapshot.instance_spot_names.end ()) {
        const framework_exception_t error (
          framework_error_kind_t::not_found,
          "Instance Spot activation target is not materialized");
        report_spot_dispatch_error (
          _state, dispatch_error_surface_t::spot_route,
          request ? dispatch_message_kind_t::request : dispatch_message_kind_t::send,
          dispatch_reason_from_error (error.kind ()),
          request ? dispatch_error_action_t::reply_error : dispatch_error_action_t::drop,
          packet_name, std::nullopt, std::string (spot_id), std::nullopt,
          std::make_exception_ptr (error), correlation_id);
        return task_t<zlink::message_t> (
          detail::result_access_t::failure<zlink::message_t> (error));
    }
    auto state = context->_state;
    const auto message_kind = request ? dispatch_message_kind_t::request
                                      : dispatch_message_kind_t::send;
    report_spot_dispatch_trace (
      _state, message_flow_outcome_t::received,
      dispatch_error_surface_t::spot_route, message_kind, packet_name, {},
      std::string (spot_id), {}, correlation_id);
    const auto trace_packet_name = packet_name;
    const auto trace_spot_id = std::string (spot_id);
    const auto trace_correlation_id = correlation_id;
    auto handler_task = spot_handler_registry_t (state).invoke_erased (
      spot_handler_kind_t::packet, packet_name, {}, std::type_index (typeid (void)),
      state->spot_instance.get (), nullptr, services, serializers,
      zlink::message_t::from (payload),
      spot_inbound_message_t{.content_type = std::move (content_type),
                             .values = std::move (metadata),
                             .mesh_name = state->mesh_name,
                             .correlation_id = std::move (correlation_id)});
    detail::observe_task_completion (
      handler_task,
      [node = _state, state, request, message_kind, trace_packet_name, trace_spot_id,
       trace_correlation_id] (const result_t<zlink::message_t> &result) {
          state->last_application_work_completed_ns.store (
            std::chrono::duration_cast<std::chrono::nanoseconds> (
              std::chrono::steady_clock::now ().time_since_epoch ())
              .count (),
            std::memory_order_relaxed);
          if (result) {
              report_spot_dispatch_trace (
                node,
                request ? message_flow_outcome_t::replied
                        : message_flow_outcome_t::dispatched,
                dispatch_error_surface_t::spot_route,
                request ? dispatch_message_kind_t::response : message_kind,
                trace_packet_name, {}, trace_spot_id, {}, trace_correlation_id);
              return;
          }
          const auto *error = result.error ();
          const framework_exception_t exception (
            result.error_kind (),
            error != nullptr ? error->what () : "Instance Spot handler failed");
          report_spot_dispatch_error (
            node, dispatch_error_surface_t::spot_route, message_kind,
            dispatch_reason_from_error (exception.kind ()),
            request ? dispatch_error_action_t::reply_error : dispatch_error_action_t::drop,
            trace_packet_name, std::nullopt, trace_spot_id, std::nullopt,
            std::make_exception_ptr (exception), trace_correlation_id);
      });
    return handler_task;
}

std::optional<spot_info_t> spot_node_runtime_t::find_spot (spot_id_t spot_id) const
{
    if (const auto name = spot_name_for (spot_id)) {
        return spot_info_t{std::move (spot_id), *name};
    }
    return std::nullopt;
}

std::vector<spot_info_t> spot_node_runtime_t::list_spots () const
{
    std::lock_guard<std::recursive_mutex> node_lock (_state->mutex);
    std::vector<spot_info_t> spots;
    spots.reserve (_state->spot_names_by_id.size ());
    for (const auto &[rid, name] : _state->spot_names_by_id) {
        spots.push_back (spot_info_t{spot_id_t (rid), name});
    }
    return spots;
}

task_t<bool> spot_node_runtime_t::close_spot (spot_id_t spot_id)
{
    std::optional<spot_context_t> context;
    {
        std::lock_guard<std::recursive_mutex> node_lock (_state->mutex);
        const auto found = _state->spot_contexts_by_id.find (std::string (spot_id));
        if (found == _state->spot_contexts_by_id.end ()) {
            co_return result_t<bool>::success (false);
        }
        context.emplace (spot_context_t (found->second._state));
    }
    const bool closed = context->close ().result ().value ();
    if (closed && _state->monitoring) {
        runtime::runtime_metrics_t metrics (_state->monitoring);
        if (metrics.enabled ()) {
            const auto spot_name = spot_name_for (spot_id);
            const auto kind = _state->snapshot.entry_spot_name && spot_name
                                  && *_state->snapshot.entry_spot_name == *spot_name
                                ? "entry"
                                : "user";
            metrics.counter ("zlink.spot.closed", "{spot}", 1, {{"kind", kind}});
            metrics.updown ("zlink.spot.count", "{spot}", -1, {{"kind", kind}});
        }
    }
    co_return result_t<bool>::success (closed);
}

bool spot_node_runtime_t::close_all_user_spots ()
{
    std::vector<spot_id_t> user_spots;
    {
        std::lock_guard<std::recursive_mutex> node_lock (_state->mutex);
        user_spots.reserve (_state->spot_contexts_by_id.size ());
        for (const auto &[rid, context] : _state->spot_contexts_by_id) {
            if (!context._state || context._state->closed
                || context._state->native_spot.expired ()) {
                continue;
            }
            if (_state->snapshot.entry_spot_name
                && context._state->spot_name == *_state->snapshot.entry_spot_name) {
                continue;
            }
            user_spots.push_back (spot_id_t (rid));
        }
    }
    for (auto &spot_id : user_spots) {
        try {
            if (!close_spot (std::move (spot_id)).result ().value ())
                return false;
        }
        catch (...) {
            return false;
        }
    }
    return active_user_spot_count () == 0;
}

node_rid_t spot_node_runtime_t::node_rid () const
{
    std::lock_guard<std::recursive_mutex> node_lock (_state->mutex);
    return node_rid_t::from_string (detail::effective_spot_node_rid (_state->snapshot));
}

std::optional<std::string> spot_node_runtime_t::spot_name_for (spot_id_t spot_id) const
{
    std::lock_guard<std::recursive_mutex> node_lock (_state->mutex);
    const auto found = _state->spot_names_by_id.find (std::string (spot_id));
    if (found == _state->spot_names_by_id.end ()) {
        return std::nullopt;
    }
    return found->second;
}

std::optional<spot_route_t> spot_node_runtime_t::resolve_spot (spot_id_t spot_id) const
{
    std::lock_guard<std::recursive_mutex> node_lock (_state->mutex);
    const auto found = _state->spot_names_by_id.find (std::string (spot_id));
    if (found != _state->spot_names_by_id.end ()) {
        return spot_route_t{
          node_rid_t::from_string (detail::effective_spot_node_rid (_state->snapshot)),
          std::move (spot_id), found->second};
    }
    for (const auto &[_, resolver] : _state->resolvers) {
        if (auto route = resolver (spot_id)) {
            return route;
        }
    }
    if (_state->spot_location_resolver) {
        const auto address =
          _state->spot_location_resolver
            ->resolve_spot_address (_state->snapshot.name, spot_id)
            .result ()
            .value ();
        if (address) {
            return spot_route_t{node_rid_t::from_string (address->node_rid.to_string ()),
                                spot_id_t (address->spot_id),
                                {}};
        }
    }
    return std::nullopt;
}

std::optional<spot_id_t> spot_node_runtime_t::actor_spot (const actor_ref_t &actor_ref) const
{
    std::lock_guard<std::recursive_mutex> node_lock (_state->mutex);
    const auto found = _state->actor_spot_ids.find (actor_key (actor_ref));
    if (found == _state->actor_spot_ids.end ()) {
        return std::nullopt;
    }
    return found->second;
}

result_t<bool> spot_node_runtime_t::destroy_actor (const actor_ref_t &actor_ref)
{
    if (::zlink::framework::detail::actor_ref_access_t::empty (actor_ref)) {
        return result_t<bool>::failure (
          framework_error_kind_t::invalid_operation,
          "ActorRef must identify an actor to destroy");
    }

    const auto local_node_rid = detail::effective_spot_node_rid (_state->snapshot);
    if (actor_ref.node_rid ().empty ()
        || actor_ref.node_rid ().value () != local_node_rid) {
        return result_t<bool>::failure (
          framework_error_kind_t::invalid_operation,
          "ActorRef does not identify an actor on this node");
    }

    const auto key = actor_key (actor_ref);
    std::function<result_t<void> (const actor_ref_t &)> destroy_registry;
    {
        std::lock_guard<std::recursive_mutex> node_lock (_state->mutex);
        if (_state->actor_transfer_coordinator.blocks_dispatch (key)) {
            return result_t<bool>::failure (
              framework_error_kind_t::unavailable,
              "Actor transfer is in progress");
        }

        const auto generation = _state->actor_generations.find (key);
        if (generation == _state->actor_generations.end ()) {
            return result_t<bool>::success (false);
        }
        if (generation->second != actor_ref.object_generation ()) {
            return result_t<bool>::failure (
              framework_error_kind_t::invalid_operation,
              "ActorRef generation does not match the current actor");
        }
        if (_state->destroying_actors.contains (key)) {
            return result_t<bool>::success (false);
        }

        _state->destroying_actors.insert (key);
        const auto location = _state->actor_spot_ids.find (key);
        if (location != _state->actor_spot_ids.end ()) {
            const auto context = _state->spot_contexts_by_id.find (
              std::string (location->second));
            if (context != _state->spot_contexts_by_id.end ()
                && context->second._state) {
                decrement_actor_count_unlocked (*context->second._state);
            }
        }

        erase_actor_route_unlocked (*_state, key);
        _state->actor_created_keys.erase (key);
        _state->destroyed_actor_keys.insert (key);
        _state->actor_instances.erase (key);
        detail::erase_actor_instance_index_unlocked (
          *_state, ::zlink::framework::detail::actor_ref_access_t::actor_type (actor_ref),
          actor_ref.actor_id ().value ());
        _state->actor_mailboxes.erase (key);
        _state->actor_execution_queues.erase (key);
        _state->actor_types_by_id.erase (std::string (actor_ref.actor_id ().value ()));
        _state->mesh_runtime_owned_native_actor_ids.erase (
          std::string (actor_ref.actor_id ().value ()));
        _state->core_actor_membership_epochs.erase (
          std::string (actor_ref.actor_id ().value ()));
        (void) _state->dispatched_request_replies.erase_if (
          [&] (const auto &request_key) {
              return request_key.starts_with (actor_request_dedup_prefix (key));
          });
        {
            const std::lock_guard<std::mutex> pending_lock (
              _state->actor_pending_requests_mutex);
            _state->actor_pending_requests.erase (key);
        }
        destroy_registry = _state->destroy_actor_registry;
        _state->destroying_actors.erase (key);
    }

    release_actor_location (*_state, actor_ref);
    if (destroy_registry) {
        auto destroyed = destroy_registry (actor_ref);
        if (!destroyed) {
            return result_t<bool>::failure (
              destroyed.error_kind (),
              destroyed.error () ? destroyed.error ()->what ()
                                  : "Actor registry cleanup failed");
        }
    }
    return result_t<bool>::success (true);
}

void spot_node_runtime_t::record_actor_spot (const actor_ref_t &actor_ref, spot_id_t spot_id)
{
    std::lock_guard<std::recursive_mutex> node_lock (_state->mutex);
    const auto key = actor_key (actor_ref);
    auto name = spot_name_for (spot_id).value_or ("");
    detail::record_actor_route_unlocked (
      *_state, key,
      spot_route_t{node_rid_t::from_string (detail::effective_spot_node_rid (_state->snapshot)),
                   std::move (spot_id), std::move (name)},
      actor_ref.object_generation ());
}

std::optional<spot_route_t> spot_node_runtime_t::actor_route (const actor_ref_t &actor_ref) const
{
    std::lock_guard<std::recursive_mutex> node_lock (_state->mutex);
    const auto found = _state->actor_routes.find (actor_key (actor_ref));
    if (found == _state->actor_routes.end ()) {
        return std::nullopt;
    }
    return found->second;
}

std::optional<actor_message_follow_target_t>
spot_node_runtime_t::actor_message_follow_target (const actor_ref_t &actor_ref) const
{
    return _state->actor_transfer_coordinator.message_follow_target (
      actor_key (actor_ref), actor_ref.object_generation ());
}

std::optional<actor_message_follow_target_t>
spot_node_runtime_t::try_acquire_actor_message_follow (
  const actor_ref_t &actor_ref,
  std::size_t payload_bytes,
  std::size_t hop_count)
{
    return _state->actor_transfer_coordinator.try_acquire_message_follow (
      actor_key (actor_ref), actor_ref.object_generation (), payload_bytes,
      hop_count);
}

void spot_node_runtime_t::release_actor_message_follow (
  const actor_ref_t &actor_ref,
  std::size_t payload_bytes) noexcept
{
    _state->actor_transfer_coordinator.release_message_follow (
      actor_key (actor_ref), actor_ref.object_generation (), payload_bytes);
}

bool spot_node_runtime_t::mark_actor_message_follow_notified (
  const actor_ref_t &actor_ref,
  const zlink::routing_id_t &source_node)
{
    return _state->actor_transfer_coordinator
      .mark_message_follow_notified (
        actor_key (actor_ref), actor_ref.object_generation (),
        source_node.to_bytes ());
}

void spot_node_runtime_t::record_actor_route (const actor_ref_t &actor_ref, spot_route_t route)
{
    std::lock_guard<std::recursive_mutex> node_lock (_state->mutex);
    const auto key = actor_key (actor_ref);
    detail::record_actor_route_unlocked (*_state, key, std::move (route), actor_ref.object_generation ());
}

std::optional<std::string> spot_node_runtime_t::actor_route_transport_name () const
{
    std::lock_guard<std::recursive_mutex> node_lock (_state->mutex);
    if (_state->snapshot.spot_route_channel_name
        && !_state->snapshot.spot_route_channel_name->empty ()) {
        return _state->snapshot.spot_route_channel_name;
    }
    if (_state->snapshot.accepted_route_channels.size () == 1) {
        return _state->snapshot.accepted_route_channels.front ().channel_name;
    }
    if (_state->snapshot.discovery_channel_name
        && !_state->snapshot.discovery_channel_name->empty ()) {
        return _state->snapshot.discovery_channel_name;
    }
    return std::nullopt;
}

void spot_node_runtime_t::cancel_pending_work () noexcept
{
    try {
        detail::drain_spot_node_executors (*_state);
    }
    catch (...) {
    }
}

void spot_node_runtime_t::release_native_handles () noexcept
{
    std::vector<std::shared_ptr<detail::spot_context_state_t>> contexts;
    try {
        {
            std::lock_guard<std::recursive_mutex> node_lock (_state->mutex);
            contexts.reserve (_state->spot_contexts_by_id.size ());
            for (auto &[_, context] : _state->spot_contexts_by_id) {
                if (context._state) {
                    context._state->closed = true;
                    contexts.push_back (context._state);
                }
            }
            _state->spot_contexts_by_id.clear ();
            _state->spot_ids_by_name.clear ();
            _state->spot_names_by_id.clear ();
            _state->native_actors.clear ();
            _state->native_spots_by_id.clear ();
            _state->routed_control_spot.reset ();
        }
        for (auto &context : contexts) {
            try {
                context->detach_application_instance (
                  true, spot_close_reason_t::host_shutdown);
            }
            catch (...) {
            }
        }
    }
    catch (...) {
    }
}

void spot_node_runtime_t::request_stop () noexcept
{
    _state->stopping.store (true, std::memory_order_release);
    _state->worker_cancellation.request_stop ();
    if (_state->worker_executor) {
        _state->worker_executor->request_stop ();
    }
}

void spot_node_runtime_t::bind_service_provider (
  service_provider_t &services)
{
    std::lock_guard<std::recursive_mutex> lock (_state->mutex);
    _state->root_services = services;
}

bool spot_node_runtime_t::stopping () const noexcept
{
    return _state->stopping.load (std::memory_order_acquire);
}

void spot_node_runtime_t::cancel_pending_dispatch () noexcept
{
    try {
        detail::cancel_spot_node_dispatch_queues (*_state);
    }
    catch (...) {
    }
}

void spot_node_runtime_t::cancel_timers () noexcept
{
    try {
        for (auto &[_, context] : _state->spot_contexts_by_id) {
            if (context._state) {
                detail::timer_runtime_t (context._state).cancel_all ();
            }
        }
    }
    catch (...) {
    }
}

std::optional<actor_ref_t>
spot_node_runtime_t::current_actor_ref (const actor_ref_t &actor_ref) const
{
    std::lock_guard<std::recursive_mutex> node_lock (_state->mutex);
    const auto found = _state->actor_generations.find (actor_key (actor_ref));
    if (found == _state->actor_generations.end ()) {
        return std::nullopt;
    }
    return ::zlink::framework::detail::actor_ref_access_t::make (
      node_rid_t::from_string (detail::effective_spot_node_rid (_state->snapshot)),
      std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor_ref)), std::string (actor_ref.actor_id ().value ()), found->second);
}

const std::vector<std::string> &
spot_node_runtime_t::ordering_log (const spot_context_t &context) const
{
    return context._state->ordering_log;
}

void spot_node_runtime_t::attach_native_node (std::shared_ptr<service::mesh_node_t> node)
{
    std::lock_guard<std::recursive_mutex> node_lock (_state->mutex);
    _state->stopping.store (false, std::memory_order_release);
    _state->worker_cancellation = std::stop_source{};
    _state->native_node = std::move (node);
    if (_state->spot_contexts_by_id.empty ()) {
        if (auto native = _state->native_node.lock ()) {
            _state->routed_control_spot =
              std::make_shared<service::spot_t> (native->entry_spot ());
        }
    }
    for (auto &[_, context] : _state->spot_contexts_by_id) {
        attach_native_spot_locked (context._state);
    }
    if (_state->instance_spot_idle_timeout
          > std::chrono::milliseconds::zero ()
        && !_state->instance_spot_idle_timer) {
        auto weak_state = std::weak_ptr<spot_node_builder_state_t> (_state);
        auto timer = std::make_unique<zlink::timer_t> ();
        timer->on_fire ([weak_state] (std::uint64_t) {
            if (auto state = weak_state.lock ()) {
                if (!state->stopping.load (std::memory_order_acquire))
                    spot_node_runtime_t (std::move (state)).evict_idle_spots ();
            }
        });
        timer->start (_state->instance_spot_idle_timeout,
                      std::numeric_limits<std::uint64_t>::max ());
        _state->instance_spot_idle_timer = std::move (timer);
    }
}

void spot_node_runtime_t::detach_native_node ()
{
    std::vector<std::shared_ptr<service::spot_t>> native_spots;
    {
        std::lock_guard<std::recursive_mutex> node_lock (_state->mutex);
        if (_state->instance_spot_idle_timer) {
            try {
                _state->instance_spot_idle_timer->stop ();
            }
            catch (...) {
            }
        }
        native_spots.reserve (_state->native_spots_by_id.size ());
        for (const auto &[_, native] : _state->native_spots_by_id) {
            if (native) {
                native_spots.push_back (native);
            }
        }
        _state->native_node.reset ();
        _state->native_spots_by_id.clear ();
        _state->routed_control_spot.reset ();
        for (auto &[_, context] : _state->spot_contexts_by_id) {
            context._state->native_spot.reset ();
        }
    }

    std::exception_ptr close_error;
    for (const auto &native : native_spots) {
        try {
            native->close ();
        }
        catch (...) {
            if (!close_error) {
                close_error = std::current_exception ();
            }
        }
    }
    if (close_error) {
        std::rethrow_exception (close_error);
    }
}

void spot_node_runtime_t::evict_idle_spots () noexcept
{
    try {
        if (_state->instance_spot_idle_timeout
              <= std::chrono::milliseconds::zero ()
            || !_state->admit_instance_spot_idle_eviction
            || _state->stopping.load (std::memory_order_acquire))
            return;

        const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds> (
          std::chrono::steady_clock::now ().time_since_epoch ())
                              .count ();
        const auto timeout_ns = std::chrono::duration_cast<std::chrono::nanoseconds> (
          _state->instance_spot_idle_timeout)
                                  .count ();
        std::vector<std::shared_ptr<spot_context_state_t>> candidates;
        {
            std::lock_guard<std::recursive_mutex> node_lock (_state->mutex);
            for (auto &[rid, context] : _state->spot_contexts_by_id) {
                (void) rid;
                auto state = context._state;
                if (!state || state->closed || !state->is_instance_spot ()
                    || !state->spot_instance || state->actor_count != 0
                    || state->relocation_boundary_active
                    || state->relocation_ready_deferred
                    || state->queued_routed_packets.size () != 0
                    || state->authority_owner_generation == 0)
                    continue;
                const auto pending_creation =
                  _state->pending_spot_creations_by_id.find (
                    std::string (state->spot_id));
                if (pending_creation != _state->pending_spot_creations_by_id.end ())
                    continue;
                if (!state->serial_queue
                    || state->serial_queue->pending_count (
                         runtime::serial_work_lane_t::application) != 0
                    || state->serial_queue->pending_count (
                         runtime::serial_work_lane_t::lifecycle) != 0)
                    continue;
                if (state->has_active_callback ())
                    continue;
                bool timer_busy = false;
                for (const auto &timer : state->timers) {
                    if (!timer)
                        continue;
                    std::lock_guard timer_lock (timer->mutex);
                    if (!timer->disposed
                        && (timer->running || timer->pending_fire
                            || timer->pending_fire_count != 0)) {
                        timer_busy = true;
                        break;
                    }
                }
                if (timer_busy)
                    continue;
                const auto last_ns = state->last_application_work_completed_ns.load (
                  std::memory_order_relaxed);
                if (last_ns <= 0 || now_ns < last_ns
                    || now_ns - last_ns < timeout_ns)
                    continue;
                {
                    std::lock_guard callback_lock (state->callback_mutex);
                    if (state->callback_admission_closed
                        || state->callback_depth != 0
                        || state->close_requested
                        || state->idle_eviction_in_progress)
                        continue;
                    /* Seal admission only after the candidate checks above.
                     * try_close_idle repeats the quiescence and idle-age
                     * checks after the Location Store transaction. */
                    state->idle_eviction_in_progress = true;
                }
                candidates.push_back (std::move (state));
            }
        }

        for (const auto &state : candidates) {
            bool evicted = false;
            try {
                evicted = _state->admit_instance_spot_idle_eviction (
                  state->spot_id, state->spot_name,
                  state->object_generation,
                  state->authority_owner_generation,
                  [state] { return state->try_close_idle (); });
            }
            catch (...) {
                evicted = false;
            }
            if (!evicted) {
                std::lock_guard<std::recursive_mutex> node_lock (_state->mutex);
                if (!state->closed) {
                    std::lock_guard callback_lock (state->callback_mutex);
                    state->idle_eviction_in_progress = false;
                }
            }
        }
    }
    catch (...) {
    }
}

void spot_node_runtime_t::record_core_actor_transfer_activation (
  std::string actor_id,
  std::uint64_t membership_epoch)
{
    std::lock_guard<std::recursive_mutex> node_lock (_state->mutex);
    _state->mesh_runtime_owned_native_actor_ids.insert (actor_id);
    _state->core_actor_membership_epochs[std::move (actor_id)] = membership_epoch;
}

void spot_node_runtime_t::bind_location_lifecycle (runtime::location_lifecycle_t &lifecycle)
{
    std::lock_guard<std::recursive_mutex> node_lock (_state->mutex);
    _state->location_lifecycle = &lifecycle;
}

bool spot_node_runtime_t::has_active_callbacks () const
{
    std::lock_guard<std::recursive_mutex> node_lock (_state->mutex);
    for (const auto &[_, context] : _state->spot_contexts_by_id) {
        if (context._state && context._state->has_active_callback ()) {
            return true;
        }
    }
    return false;
}

std::vector<actor_ref_t> spot_node_runtime_t::local_actor_refs () const
{
    std::lock_guard<std::recursive_mutex> node_lock (_state->mutex);
    std::vector<actor_ref_t> refs;
    refs.reserve (_state->actor_spot_ids.size ());
    const auto node_rid = detail::effective_spot_node_rid (_state->snapshot);
    for (const auto &[key, spot_id] : _state->actor_spot_ids) {
        const auto split = key.find (':');
        if (split == std::string::npos) {
            continue;
        }
        const auto generation = _state->actor_generations.find (key);
        if (generation == _state->actor_generations.end () || generation->second == 0) {
            continue;
        }
        refs.push_back (
          ::zlink::framework::detail::actor_ref_access_t::make (
            node_rid_t::from_string (node_rid), key.substr (0, split),
            key.substr (split + 1), generation->second));
    }
    return refs;
}

std::optional<zlink::message_t>
spot_node_runtime_t::serialize_actor_snapshot (const actor_ref_t &actor_ref) const
{
    std::lock_guard<std::recursive_mutex> node_lock (_state->mutex);
    const auto actor = _state->actor_instances.find (actor_key (actor_ref));
    const auto factory = _state->actor_factories.find (std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor_ref)));
    if (actor == _state->actor_instances.end () || !actor->second
        || factory == _state->actor_factories.end () || !_state->channel_runtime
        || !_state->channel_runtime->serializers) {
        return std::nullopt;
    }
    return factory->second.serialize_instance (actor->second.get (),
                                               *_state->channel_runtime->serializers);
}

void spot_node_runtime_t::bind_drain_flag (std::shared_ptr<std::atomic_bool> flag)
{
    std::lock_guard<std::recursive_mutex> node_lock (_state->mutex);
    _state->drain_flag = std::move (flag);
}

void spot_node_runtime_t::bind_spot_location_resolver (runtime::spot_address_resolver_t &resolver)
{
    std::lock_guard<std::recursive_mutex> node_lock (_state->mutex);
    _state->spot_location_resolver = &resolver;
}

std::shared_ptr<service::mesh_node_t> spot_node_runtime_t::native_node () const
{
    std::lock_guard<std::recursive_mutex> node_lock (_state->mutex);
    return _state->native_node.lock ();
}

result_t<void> spot_node_runtime_t::send_spot_mesh_parts (
  const zlink::routing_id_t &target_node_rid,
  const spot_id_t &target_spot_id,
  runtime::messaging::message_parts_t parts) const
{
    auto node = native_node ();
    if (!node) {
        return result_t<void>::failure (framework_error_kind_t::not_found,
                                        "SPOT mesh route requires a running native node");
    }
    try {
        auto native_parts = parts.items ();
        if (native_parts.empty ()) {
            return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                            "SPOT mesh send requires at least one message part");
        }
        auto egress = node->entry_spot ();
        const auto target_generation =
          resolve_target_spot_generation (_state, target_node_rid, target_spot_id);
        if (!target_generation) {
            return result_t<void>::failure (
              framework_error_kind_t::not_found,
              "SPOT mesh send target generation is unavailable");
        }
        if (egress.send_to_spot (target_node_rid, target_spot_id,
                                 *target_generation, native_parts)
            != zlink::submit_result_t::ok) {
            return result_t<void>::failure (framework_error_kind_t::internal_failure,
                                            "SPOT mesh send was not submitted");
        }
        return result_t<void>::success ();
    }
    catch (const framework_exception_t &error) {
        return detail::result_access_t::failure<void> (error);
    }
    catch (const std::exception &error) {
        return result_t<void>::failure (framework_error_kind_t::internal_failure, error.what ());
    }
}

std::optional<std::uint64_t> spot_node_runtime_t::resolve_spot_generation (
  const zlink::routing_id_t &target_node_rid,
  const spot_id_t &target_spot_id) const
{
    return resolve_target_spot_generation (
      _state, target_node_rid, target_spot_id);
}

void spot_node_runtime_t::set_route_client (route_client_t route_client)
{
    std::lock_guard<std::recursive_mutex> node_lock (_state->mutex);
    _state->route_client = std::move (route_client);
}

std::vector<spot_context_t> spot_node_runtime_t::active_contexts () const
{
    std::lock_guard<std::recursive_mutex> node_lock (_state->mutex);
    std::vector<spot_context_t> contexts;
    contexts.reserve (_state->spot_contexts_by_id.size ());
    for (const auto &[_, context] : _state->spot_contexts_by_id) {
        if (!context._state->closed && !context._state->native_spot.expired ()) {
            contexts.push_back (spot_context_t (context._state));
        }
    }
    return contexts;
}

std::vector<spot_id_t>
spot_node_runtime_t::deferred_relocation_ready_spots () const
{
    std::vector<spot_id_t> result;
    std::lock_guard<std::recursive_mutex> node_lock (
      _state->mutex);
    for (const auto &[_, context] :
         _state->spot_contexts_by_id) {
        const auto state = context._state;
        if (!state || state->is_entry_spot () || state->is_instance_spot ())
            continue;
        std::lock_guard callback_lock (state->callback_mutex);
        if (state->relocation_ready_deferred)
            result.push_back (state->spot_id);
    }
    return result;
}

std::vector<spot_node_runtime_t::application_relocation_unit_t>
spot_node_runtime_t::application_relocation_units () const
{
    std::vector<application_relocation_unit_t> result;
    std::lock_guard<std::recursive_mutex> node_lock (
      _state->mutex);
    const auto node_rid =
      detail::effective_spot_node_rid (_state->snapshot);
    for (const auto &[_, context] :
         _state->spot_contexts_by_id) {
        const auto state = context._state;
        if (!state || state->is_entry_spot () || state->is_instance_spot ()
            || state->execution_mode
                 != user_spot_execution_mode_t::spot_wide
            || state->relocation_readiness
                 != spot_relocation_readiness_mode_t::
                   application_signaled)
            continue;
        application_relocation_unit_t unit{
          state->spot_id, state->spot_name, false, {}};
        {
            std::lock_guard callback_lock (
              state->callback_mutex);
            unit.ready = state->relocation_ready_deferred;
        }
        for (const auto &[key, actor_spot_id] :
             _state->actor_spot_ids) {
            if (actor_spot_id != state->spot_id)
                continue;
            const auto split = key.find (':');
            if (split == std::string::npos)
                continue;
            const auto generation =
              _state->actor_generations.find (key);
            if (generation == _state->actor_generations.end ()
                || generation->second == 0) {
                continue;
            }
            unit.actors.push_back (
              ::zlink::framework::detail::actor_ref_access_t::make (
                node_rid_t::from_string (node_rid), key.substr (0, split),
                key.substr (split + 1), generation->second));
        }
        result.push_back (std::move (unit));
    }
    return result;
}

void spot_node_runtime_t::begin_relocation_readiness ()
{
    std::lock_guard<std::recursive_mutex> node_lock (
      _state->mutex);
    for (const auto &[_, context] :
         _state->spot_contexts_by_id) {
        const auto state = context._state;
        if (!state || state->is_entry_spot () || state->is_instance_spot ()
            || state->execution_mode
                 != user_spot_execution_mode_t::spot_wide
            || state->relocation_readiness
                 != spot_relocation_readiness_mode_t::
                   application_signaled)
            continue;
        std::lock_guard callback_lock (state->callback_mutex);
        state->relocation_boundary_active = true;
    }
}

void spot_node_runtime_t::end_relocation_readiness (
  const std::vector<spot_id_t> &relocated_spots)
{
    std::vector<std::shared_ptr<spot_context_state_t>> states;
    {
        std::lock_guard<std::recursive_mutex> node_lock (
          _state->mutex);
        for (const auto &[_, context] :
             _state->spot_contexts_by_id) {
            const auto state = context._state;
            if (!state)
                continue;
            {
                std::lock_guard callback_lock (
                  state->callback_mutex);
                if (!state->relocation_boundary_active)
                    continue;
                state->relocation_boundary_active = false;
            }
            states.push_back (state);
        }
    }
    for (const auto &state : states) {
        const auto relocated =
          std::find (
            relocated_spots.begin (), relocated_spots.end (),
            state->spot_id)
          != relocated_spots.end ();
        state->complete_relocation_ready (
          relocated
            ? spot_relocation_ready_outcome_t::relocated
            : spot_relocation_ready_outcome_t::continued);
    }
}

bool spot_node_runtime_t::complete_relocation_ready (
  const spot_id_t &spot_id,
  spot_relocation_ready_outcome_t outcome)
{
    std::shared_ptr<spot_context_state_t> state;
    {
        std::lock_guard<std::recursive_mutex> node_lock (
          _state->mutex);
        const auto found =
          _state->spot_contexts_by_id.find (
            std::string (spot_id));
        if (found == _state->spot_contexts_by_id.end ())
            return false;
        state = found->second._state;
    }
    if (!state)
        return false;
    state->complete_relocation_ready (outcome);
    return true;
}

std::size_t spot_node_runtime_t::active_user_spot_count () const
{
    std::lock_guard<std::recursive_mutex> node_lock (_state->mutex);
    return static_cast<std::size_t> (std::count_if (
      _state->spot_contexts_by_id.begin (), _state->spot_contexts_by_id.end (),
      [&] (const auto &entry) {
          const auto &context = entry.second;
          return !context._state->closed && !context._state->native_spot.expired ()
                 && (!_state->snapshot.entry_spot_name
                     || context._state->spot_name != *_state->snapshot.entry_spot_name);
      }));
}

bool spot_node_runtime_t::dispatch_mesh_record (
  const service::ready_record_t &owner,
  const service::receive_record_t &record,
  std::vector<zlink::message_t> &parts,
  service_provider_t &services,
  serializer_registry_t &serializers)
{
    if (owner.owner_kind == service::owner_kind_t::spot
        && record.kind == service::record_kind_t::spot_multicast) {
        const auto context = find_context (
          spot_id_t (owner.spot_id));
        if (!context)
            return true;
        (void) dispatch_subscription (
          *context, record.topic, parts, services, serializers);
        return true;
    }
    const bool spot_record =
      owner.owner_kind == service::owner_kind_t::spot
      && (record.kind == service::record_kind_t::spot_send
          || record.kind == service::record_kind_t::spot_request);
    const bool node_record =
      owner.owner_kind == service::owner_kind_t::node
      && (record.kind == service::record_kind_t::node_send
          || record.kind == service::record_kind_t::node_request);
    if (spot_record || node_record) {
        std::optional<route_client_t> route_client;
        {
            std::lock_guard<std::recursive_mutex> lock (_state->mutex);
            route_client = _state->route_client;
        }
        if (!route_client)
            return false;

        runtime::messaging::message_parts_t encoded (std::move (parts));
        runtime::messaging::envelope_codec_t codec;
        auto header = codec.decode_header (encoded);
        if (!header) {
            parts = std::move (encoded).take_items ();
            return false;
        }

        /* Native SPOT delivery bypasses the RouteMesh packet dispatcher after
         * the envelope has been decoded. Re-enter the wire flow here so the
         * remote Spot handler observes the same flow as the originating
         * STREAM/session request. */
        auto flow_scope = runtime::flow_context_t::enter (
          header.value ().flow_id, header.value ().flow_origin,
          detail::message_flow_tracer_t (_state->dispatch).capture_enabled (),
          flow_origin_t::inbound);

        auto &actor_gateway = services.get_required<actor_gateway_runtime_t> ();
        spot_route_internal_dispatcher_t dispatcher (
          *this, actor_gateway, *route_client, serializers);
        if ((record.kind == service::record_kind_t::spot_send
             || record.kind == service::record_kind_t::node_send)
            && dispatcher.can_handle_send (header.value ().message_name)) {
            route_received_packet_t received{
              record.source_node_rid,
              record.operation_id.low == 0
                ? std::nullopt
                : std::make_optional (record.operation_id.low),
              std::move (encoded),
              std::nullopt};
            const auto dispatched = dispatcher.dispatch_send (received, services);
            if (!dispatched) {
                const framework_exception_t error (
                  dispatched.error_kind (),
                  dispatched.error () != nullptr ? dispatched.error ()->what ()
                                                 : "SPOT route send failed");
                report_spot_dispatch_error (
                  _state, dispatch_error_surface_t::spot_route,
                  dispatch_message_kind_t::send,
                  dispatch_reason_from_error (error.kind ()),
                  dispatch_error_action_t::drop,
                  header.value ().message_name,
                  std::nullopt,
                  owner.spot_id.empty ()
                    ? std::nullopt
                    : std::make_optional (owner.spot_id),
                  std::nullopt,
                  std::make_exception_ptr (error),
                  record.operation_id.low == 0
                    ? std::nullopt
                    : std::make_optional (
                        std::to_string (record.operation_id.low)));
            }
            return true;
        }
        if ((record.kind == service::record_kind_t::spot_request
             || record.kind == service::record_kind_t::node_request)
            && dispatcher.can_handle_request (header.value ().message_name)) {
            route_received_packet_t received{
              record.source_node_rid,
              record.operation_id.low == 0
                ? std::nullopt
                : std::make_optional (record.operation_id.low),
              std::move (encoded),
              std::nullopt};
            detail::channel_reply_writer_t replies;
            auto response =
              dispatcher.dispatch_request (received, header.value (), services);
            const auto reply_parts =
              response
                ? replies.reply_raw_envelope (
                    replies.create_reply_header (
                      runtime::messaging::message_kind_t::response,
                      header.value ().channel_name, header.value ()),
                    std::move (response.value ()))
                : replies.reply_raw_envelope (
                    replies.create_error_header (
                      header.value ().channel_name, header.value (),
                      framework_exception_t (
                        response.error_kind (),
                        response.error () ? response.error ()->what ()
                                          : "SPOT route request failed")),
                    zlink::message_t::from (""));
            (void) service::reply (record.reply_token, reply_parts.items ());
            return true;
        }
        if (spot_record) {
            const auto context = find_context (
              spot_id_t (owner.spot_id));
            if (!context || !context->_state || !context->_state->spot_instance) {
                parts = std::move (encoded).take_items ();
                return false;
            }
            detail::channel_reply_writer_t replies;
            auto reply_error = [&] (const framework_exception_t &error) {
                if (record.kind != service::record_kind_t::spot_request) {
                    return;
                }
                auto reply = replies.reply_raw_envelope (
                  replies.create_error_header (header.value ().channel_name,
                                               header.value (), error),
                  zlink::message_t::from (""));
                (void) service::reply (record.reply_token, reply.items ());
            };
            if (record.spot_route) {
                const auto &target = *record.spot_route;
                const auto &state = *context->_state;
                std::optional<location_owner_token_t> owner_token;
                if (state.node && state.node->location_lifecycle)
                    owner_token = state.node->location_lifecycle
                                    ->current_owner_token ();
                if (!state.accepts_route_fence (target, owner_token)) {
                    reply_error (framework_exception_t (
                      framework_error_kind_t::unavailable,
                      "Spot route fence is stale"));
                    return true;
                }
            }
            try {
                /* Complete route admission before creating the typed body. */
                auto body = codec.decode_body (encoded);
                if (!body) {
                    reply_error (framework_exception_t (
                      body.error_kind (),
                      body.error () ? body.error ()->what ()
                                     : "Spot request envelope body is invalid"));
                    return true;
                }
                report_spot_dispatch_trace (
                  _state, message_flow_outcome_t::received,
                  dispatch_error_surface_t::spot_route,
                  record.kind == service::record_kind_t::spot_request
                    ? dispatch_message_kind_t::request
                    : dispatch_message_kind_t::send,
                  header.value ().message_name, {}, owner.spot_id);
                auto handled = spot_handler_registry_t (context->_state)
                                 .invoke_erased (
                                   spot_handler_kind_t::packet,
                                   header.value ().message_name, {},
                                   std::type_index (typeid (void)),
                                   context->_state->spot_instance.get (), nullptr,
                                   services, serializers, body.value (),
                                   spot_inbound_message_t{
                                     .content_type = header.value ().content_type,
                                     .values = header.value ().metadata})
                                 .result ();
                if (!handled) {
                    const auto *error = handled.error ();
                    reply_error (framework_exception_t (
                      handled.error_kind (),
                      error != nullptr ? error->what () : "Spot handler failed"));
                    return true;
                }
                if (record.kind == service::record_kind_t::spot_request) {
                    auto reply = replies.reply_raw_envelope (
                      replies.create_reply_header (
                        runtime::messaging::message_kind_t::response,
                        header.value ().channel_name, header.value ()),
                      std::move (handled.value ()));
                    (void) service::reply (record.reply_token, reply.items ());
                }
                report_spot_dispatch_trace (
                  _state,
                  record.kind == service::record_kind_t::spot_request
                    ? message_flow_outcome_t::replied
                    : message_flow_outcome_t::dispatched,
                  dispatch_error_surface_t::spot_route,
                  record.kind == service::record_kind_t::spot_request
                    ? dispatch_message_kind_t::response
                    : dispatch_message_kind_t::send,
                  header.value ().message_name, {}, owner.spot_id);
                return true;
            }
            catch (const framework_exception_t &error) {
                reply_error (error);
                return true;
            }
            catch (const std::exception &error) {
                reply_error (framework_exception_t (
                  framework_error_kind_t::internal_failure, error.what ()));
                return true;
            }
            catch (...) {
                reply_error (framework_exception_t (
                  framework_error_kind_t::internal_failure,
                  "Spot handler dispatch failed"));
                return true;
            }
        }
        parts = std::move (encoded).take_items ();
        return false;
    }
    if (owner.owner_kind == service::owner_kind_t::actor
        && (record.kind == service::record_kind_t::actor_send
            || record.kind == service::record_kind_t::actor_request)) {
        std::string actor_type;
        {
            std::lock_guard<std::recursive_mutex> lock (_state->mutex);
            const auto found =
              _state->actor_types_by_id.find (
                std::string (owner.actor->actor_id ().value ()));
            if (found != _state->actor_types_by_id.end ())
                actor_type = found->second;
        }
        if (actor_type.empty ()) {
            const auto actor_id = std::string (owner.actor->actor_id ().value ());
            const auto located = actor_type_from_authority (
              services.get_required<runtime::live_location_reader_t> (), actor_id);
            if (located && !located->empty ()) {
                actor_type = *located;
                std::lock_guard<std::recursive_mutex> lock (_state->mutex);
                _state->actor_types_by_id[actor_id] = actor_type;
            }
        }
        runtime::messaging::message_parts_t encoded (std::move (parts));
        runtime::messaging::envelope_codec_t codec;
        auto header = codec.decode_header (encoded);
        detail::channel_reply_writer_t replies;
        auto reply_error = [&] (const framework_exception_t &error) {
            if (record.kind != service::record_kind_t::actor_request || !header)
                return;
            auto reply = replies.reply_raw_envelope (
              replies.create_error_header (header.value ().channel_name, header.value (), error),
              zlink::message_t::from (""));
            (void) service::reply (record.reply_token, reply.items ());
        };
        if (actor_type.empty () || !header) {
            reply_error (framework_exception_t (
              actor_type.empty () ? framework_error_kind_t::not_found
                                  : framework_error_kind_t::protocol_error,
              actor_type.empty () ? "Actor type is not registered"
                                  : "Actor request envelope header is invalid"));
            return true;
        }
        const actor_ref_t actor =
          ::zlink::framework::detail::actor_ref_access_t::make (
            node_rid_t::from_string (
              std::string (owner.actor->node_rid ().value ())),
            actor_type, std::string (owner.actor->actor_id ().value ()),
            owner.actor->object_generation ());
        if (record.actor_route) {
            const auto &route = *record.actor_route;
            const auto native = _state->native_node.lock ();
            const auto local = native
                                 ? native->status ()
                                 : runtime::host::node_status_t{};
            const bool targets_local_node =
              native
              && route.actor_id == actor.actor_id ().value ()
              && route.object_generation == actor.object_generation ()
              && route.target_node_routing_id == local.routing_id ().to_bytes ()
              && route.target_node_generation == local.lifecycle_generation ();
            const bool follows_committed_source =
              targets_local_node
              && actor_message_follow_target (actor).has_value ();
            const bool admitted =
              targets_local_node
              && (follows_committed_source
                  || (_state->actor_route_admission
                      && _state->actor_route_admission (route)));
            if (!admitted) {
                reply_error (framework_exception_t (
                  framework_error_kind_t::unavailable,
                  "Actor route fence is stale or not admitted"));
                return true;
            }
        }
        /* Route admission is complete before creating the typed body. This
         * keeps stale authority, owner, and node generations out of the
         * application deserializer and handler path. */
        auto body = codec.decode_body (encoded);
        if (!body) {
            reply_error (framework_exception_t (
              framework_error_kind_t::protocol_error,
              "Actor request envelope body is invalid"));
            return true;
        }
        if (header.value ().message_name
            == actor_bound_session_bind_route_request_t::packet_name) {
            trace_actor_dispatch (
              "remote-session-bind-received", owner.actor->actor_id ().value ());
            std::optional<route_client_t> route_client;
            {
                std::lock_guard<std::recursive_mutex> lock (_state->mutex);
                route_client = _state->route_client;
            }
            if (!route_client) {
                reply_error (framework_exception_t (
                  framework_error_kind_t::unavailable,
                  "Remote Actor session binding requires a RouteMesh client"));
                return true;
            }
            auto &actor_gateway =
              services.get_required<actor_gateway_runtime_t> ();
            spot_route_internal_dispatcher_t dispatcher (
              *this, actor_gateway, *route_client, serializers);
            route_received_packet_t received{
              record.source_node_rid,
              record.operation_id.low == 0
                ? std::nullopt
                : std::make_optional (record.operation_id.low),
              std::move (encoded),
              std::nullopt};
            auto response =
              dispatcher.dispatch_request (received, header.value (), services);
            const auto reply_parts =
              response
                ? replies.reply_raw_envelope (
                    replies.create_reply_header (
                      runtime::messaging::message_kind_t::response,
                      header.value ().channel_name, header.value ()),
                    std::move (response.value ()))
                : replies.reply_raw_envelope (
                    replies.create_error_header (
                      header.value ().channel_name, header.value (),
                      framework_exception_t (
                        response.error_kind (),
                        response.error ()
                          ? response.error ()->what ()
                          : "Remote Actor session binding failed")),
                    zlink::message_t::from (""));
            (void) service::reply (
              record.reply_token, reply_parts.items ());
            return true;
        }
        auto &actor_gateway =
          services.get_required<actor_gateway_runtime_t> ();
        auto actor_context = actor_gateway.actor_context (
          actor, record.source_binding_generation);
        auto relayed = [&] {
            if ((record.kind == service::record_kind_t::actor_request
                 || record.kind == service::record_kind_t::actor_send)
                && record.actor_route
                && _state->actor_message_follow_relay) {
                return _state->actor_message_follow_relay (
                  actor, header.value (), body.value (),
                  std::chrono::seconds (30), record.source_node_rid,
                  *record.actor_route,
                  record.message_follow_hop_count,
                  runtime::protocol::wire_operation_id_t{
                    record.operation_id.high, record.operation_id.low},
                  record.reply_route_id);
            }
            auto relay_metadata = spot_inbound_message_t{
              .content_type = header.value ().content_type,
              .values = header.value ().metadata};
            if (record.actor_route) {
                const auto &route = *record.actor_route;
                relay_metadata.values[std::string (detail::actor_handoff_source_node_key)] =
                  record.source_node_rid.to_hex ();
                relay_metadata.values[
                  std::string (detail::actor_handoff_route_actor_id_key)] = route.actor_id;
                relay_metadata.values[std::string (
                  detail::actor_handoff_route_object_generation_key)] =
                  std::to_string (route.object_generation);
                relay_metadata.values[std::string (
                  detail::actor_handoff_route_target_node_key)] =
                  zlink::routing_id_t::from (route.target_node_routing_id).to_hex ();
                relay_metadata.values[std::string (
                  detail::actor_handoff_route_target_node_generation_key)] =
                  std::to_string (route.target_node_generation);
                relay_metadata.values[std::string (
                  detail::actor_handoff_route_authority_generation_key)] =
                  std::to_string (route.authority_owner_generation);
                relay_metadata.values[std::string (
                  detail::actor_handoff_route_lease_generation_key)] =
                  std::to_string (route.owner_lease_generation);
                relay_metadata.values[std::string (detail::actor_handoff_hop_count_key)] =
                  std::to_string (record.message_follow_hop_count);
                relay_metadata.values[std::string (
                  detail::actor_handoff_operation_high_key)] =
                  std::to_string (record.operation_id.high);
                relay_metadata.values[std::string (
                  detail::actor_handoff_operation_low_key)] =
                  std::to_string (record.operation_id.low);
                relay_metadata.values[std::string (detail::actor_handoff_reply_route_key)] =
                  std::to_string (record.reply_route_id);
            }
            return relay_actor_packet (
              actor, std::move (actor_context),
              record.kind == service::record_kind_t::actor_send
                ? stream_message_kind_t::send
                : stream_message_kind_t::request,
              header.value ().message_name, body.value (), services, serializers,
              std::move (relay_metadata));
        } ();
        if (!relayed) {
            reply_error (framework_exception_t (
              relayed.error_kind (),
              relayed.error () ? relayed.error ()->what () : "Actor handler failed"));
            return true;
        }
        if (record.kind == service::record_kind_t::actor_request && relayed.value ()) {
            auto reply = replies.reply_raw_envelope (
              replies.create_reply_header (
                runtime::messaging::message_kind_t::response,
                header.value ().channel_name, header.value ()),
              std::move (*relayed.value ()));
            (void) service::reply (record.reply_token, reply.items ());
        }
        return true;
    }
    if (owner.owner_kind != service::owner_kind_t::spot
        || record.kind != service::record_kind_t::spot_control
        || !record.actor_control) {
        return false;
    }
    const auto &control = *record.actor_control;
    if (control.kind != service::lifecycle_kind_t::joined) {
        return true;
    }
    if (record.operation_kind != service::operation_kind_t::actor_join) {
        // The remote source emits a post-commit JOINED lifecycle notification
        // after the target has already admitted and materialized the actor.
        // It has no admission payload or reply token and must not re-enter the
        // admission handler.
        return true;
    }

    std::string actor_type;
    {
        std::lock_guard<std::recursive_mutex> lock (_state->mutex);
        const auto found =
          _state->actor_types_by_id.find (
            std::string (control.current_actor.actor_id ().value ()));
        if (found != _state->actor_types_by_id.end ())
            actor_type = found->second;
    }
    if (actor_type.empty ()) {
        const auto actor_id = std::string (control.current_actor.actor_id ().value ());
        const auto located = actor_type_from_authority (
          services.get_required<runtime::live_location_reader_t> (), actor_id);
        if (located && !located->empty ()) {
            actor_type = *located;
            std::lock_guard<std::recursive_mutex> lock (_state->mutex);
            _state->actor_types_by_id[actor_id] = actor_type;
        }
    }
    if (actor_type.empty ()) {
        (void) service::actor_join_reply (
          record.reply_token, service::actor_join_result_t::rejected, {});
        return true;
    }

    const actor_ref_t actor =
      ::zlink::framework::detail::actor_ref_access_t::make (
        node_rid_t::from_string (
          std::string (control.current_actor.node_rid ().value ())),
        actor_type, std::string (control.current_actor.actor_id ().value ()),
        control.current_actor.object_generation ());
    const zlink::message_t request = parts.empty () ? zlink::message_t{} : parts.front ();
    bool targets_entry_spot = false;
    {
        std::lock_guard<std::recursive_mutex> lock (_state->mutex);
        if (_state->snapshot.entry_spot_name) {
            const auto entry =
              _state->spot_ids_by_name.find (
                *_state->snapshot.entry_spot_name);
            targets_entry_spot =
              entry != _state->spot_ids_by_name.end ()
              && entry->second == owner.spot_id;
        }
    }
    auto joined = [&] () -> result_t<actor_join_reply_t> {
        if (parts.size () >= 10
            && owner.spot_id
                 != detail::effective_spot_node_rid (_state->snapshot)) {
            const auto target_spot =
              spot_id_t (owner.spot_id);
            const auto transfer_id = parts[2].to_string ();
            auto admitted = admit_remote_actor_to_spot (
              transfer_id, actor, spot_id_t (parts[3].to_string ()),
              target_spot, request);
            if (!admitted) {
                return detail::propagate_failure<actor_join_reply_t> (
                  admitted, "remote actor admission failed");
            }
            const auto admission_reply =
              framework_reply_or_empty (admitted.value ().reply, serializers);
            if (!admitted.value ().accepted) {
                return result_t<actor_join_reply_t>::success (
                  actor_join_reply_t{1, actor, admission_reply});
            }
            auto native = native_node ();
            if (!native) {
                return result_t<actor_join_reply_t>::failure (
                  framework_error_kind_t::internal_failure,
                  "target Core MeshNode is unavailable");
            }
            service::actor_transfer_prepare_t transfer_prepare{
              .role = service::actor_transfer_role_t::target,
              .transfer_id = transfer_id,
              .actor = control.current_actor,
              .source_spot_id = parts[3].to_string (),
              .target_spot_id = owner.spot_id,
              .target_node_rid = native->status ().routing_id ()};
            service::actor_transfer_token_t transfer_token;
            service::actor_transfer_prepare_result_t transfer_result{
              actor, 0};
            if (!native->prepare_actor_transfer (
                  transfer_prepare, transfer_token, transfer_result)) {
                return result_t<actor_join_reply_t>::failure (
                  framework_error_kind_t::internal_failure,
                  "target Framework Actor relocation prepare failed");
            }
            {
                std::lock_guard<std::recursive_mutex> lock (_state->mutex);
                // Framework target prepare installed the transferred Actor with the
                // source generation. Application materialization must reuse it.
                _state->mesh_runtime_owned_native_actor_ids.insert (
                  std::string (actor.actor_id ().value ()));
            }
            auto committed = commit_remote_actor_to_spot (
              transfer_id, actor, target_spot, parts[1],
              services.get_required<actor_gateway_runtime_t> ().actor_context (actor),
              {}, &services);
            if (!committed) {
                std::lock_guard<std::recursive_mutex> lock (_state->mutex);
                _state->mesh_runtime_owned_native_actor_ids.erase (
                  std::string (actor.actor_id ().value ()));
                return committed;
            }
            const auto new_membership_epoch = transfer_result.membership_epoch + 1;
            if (!transfer_token.commit (new_membership_epoch)
                || !transfer_token.activate ()) {
                return result_t<actor_join_reply_t>::failure (
                  framework_error_kind_t::internal_failure,
                  "target Framework Actor relocation activation failed");
            }
            emit_actor_transfer_marker (
              "location_committed", committed.value ().actor, transfer_id,
              target_spot);
            return result_t<actor_join_reply_t>::success (
              actor_join_reply_t{
                committed.value ().result_code, committed.value ().actor,
                admission_reply});
        }
        return targets_entry_spot
                 ? [&] {
                       auto &actor_gateway =
                         services.get_required<
                           actor_gateway_runtime_t> ();
                       auto actor_context =
                         actor_gateway.actor_context (actor);
                       if (actor_context.serializer_registry ()
                           == nullptr) {
                           return result_t<
                             actor_join_reply_t>::failure (
                             framework_error_kind_t::protocol_error,
                             "Actor gateway Context has no serializer registry");
                       }
                       return join_actor_to_entry_spot_erased (
                         actor,
                         node_rid_t::from_string (
                           detail::effective_spot_node_rid (
                             _state->snapshot)),
                         request, std::nullopt,
                         std::move (actor_context));
                   } ()
                 : join_actor_to_spot_erased (
                     actor, spot_id_t (owner.spot_id),
                     request);
    } ();
    if (!joined) {
        (void) service::actor_join_reply (
          record.reply_token, service::actor_join_result_t::rejected, {});
        return true;
    }
    const std::vector<zlink::message_t> reply_parts =
      joined.value ().reply.to_string ().empty ()
        ? std::vector<zlink::message_t>{}
        : std::vector<zlink::message_t>{joined.value ().reply};
    (void) service::actor_join_reply (
      record.reply_token,
      joined.value ().result_code == 0
        ? service::actor_join_result_t::accepted
        : service::actor_join_result_t::rejected,
      reply_parts);
    return true;
}

result_t<void> spot_node_runtime_t::dispatch_subscription (const spot_context_t &context,
                                                           std::string topic,
                                                           const zlink::message_t &message,
                                                           service_provider_t &services,
                                                           serializer_registry_t &serializers) const
{
    return dispatch_subscription (context, std::move (topic),
                                  std::vector<zlink::message_t>{message}, services, serializers);
}

result_t<void>
spot_node_runtime_t::dispatch_subscription (const spot_context_t &context,
                                            std::string topic,
                                            const std::vector<zlink::message_t> &parts,
                                            service_provider_t &services,
                                            serializer_registry_t &serializers) const
{
    if (!context._state || !context._state->spot_instance) {
        return result_t<void>::failure (framework_error_kind_t::not_found,
                                        "spot context is not registered");
    }
    if (parts.empty ()) {
        return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                        "spot subscription frame is empty");
    }
    /* Fan-out wire envelope (flow-correlation §4.1): the decoded header
     * carries the publisher's flow pair, so every subscriber line — including
     * skip/drop lines — shares the tree's flow id. The frame is either the
     * framework's self-delimited single part (['Z''L''F''E'][u32 BE
     * header_len][header JSON][body]), a true two-part envelope from a
     * parts-preserving wire, or a bare payload from a non-framework publisher
     * (dispatched without a flow pair). */
    const runtime::messaging::envelope_codec_t codec;
    zlink::message_t body = parts.front ();
    std::optional<std::string> flow_id;
    std::optional<flow_origin_t> flow_origin;
    std::optional<std::string> packet_name;
    bool report_decode_failure = false;
    if (parts.size () >= 2) {
        const runtime::messaging::message_parts_t envelope_parts{std::vector (parts)};
        auto header = codec.decode_header (envelope_parts);
        auto decoded_body = codec.decode_body (envelope_parts);
        if (header && decoded_body) {
            body = decoded_body.value ();
            flow_id = header.value ().flow_id;
            flow_origin = header.value ().flow_origin;
            if (!header.value ().message_name.empty ()) {
                packet_name = header.value ().message_name;
            }
        } else {
            report_decode_failure = true;
        }
    } else {
        const auto &frame = parts.front ();
        const auto bytes = frame.to_bytes ();
        const bool framed = bytes.size () >= 8 && bytes[0] == static_cast<std::uint8_t> ('Z')
                            && bytes[1] == static_cast<std::uint8_t> ('L')
                            && bytes[2] == static_cast<std::uint8_t> ('F')
                            && bytes[3] == static_cast<std::uint8_t> ('E');
        if (framed) {
            /* The 'ZLFE' prefix is reserved by the fanout wire contract for
             * framework frames — a raw publisher whose payload begins with
             * it is out of contract (CPP-FANOUT-WIRE-001 tracks the final
             * cross-language wire). On a magic match any failure past this
             * point drops the message instead of handing a corrupted body
             * to the application handler. header_size is compared against
             * the remainder (never added to the prefix width) so an
             * adversarial length cannot wrap std::size_t. */
            const std::size_t header_size = (static_cast<std::size_t> (bytes[4]) << 24)
                                            | (static_cast<std::size_t> (bytes[5]) << 16)
                                            | (static_cast<std::size_t> (bytes[6]) << 8)
                                            | static_cast<std::size_t> (bytes[7]);
            report_decode_failure = true;
            if (header_size > 0 && header_size <= bytes.size () - 8) {
                auto header = codec.decode_header (zlink::message_t::from (
                  std::vector<std::uint8_t> (bytes.begin () + 8,
                                             bytes.begin () + 8
                                               + static_cast<std::ptrdiff_t> (header_size))));
                if (header) {
                    body = zlink::message_t::from (std::vector<std::uint8_t> (
                      bytes.begin () + 8 + static_cast<std::ptrdiff_t> (header_size),
                      bytes.end ()));
                    flow_id = header.value ().flow_id;
                    flow_origin = header.value ().flow_origin;
                    if (!header.value ().message_name.empty ()) {
                        packet_name = header.value ().message_name;
                    }
                    report_decode_failure = false;
                }
            }
        }
    }
    if (report_decode_failure) {
        report_spot_dispatch_error (
          _state, dispatch_error_surface_t::spot_subscription, dispatch_message_kind_t::publish,
          dispatch_error_reason_t::payload_decode_failed, dispatch_error_action_t::drop,
          std::nullopt, topic, std::string (context._state->spot_id));
        return result_t<void>::success ();
    }
    const bool capture_enabled =
      detail::message_flow_tracer_t (_state->dispatch).capture_enabled ();
    auto flow_scope = runtime::flow_context_t::enter (std::move (flow_id), flow_origin,
                                                      capture_enabled, flow_origin_t::inbound);
    const auto &message = body;
    report_spot_dispatch_trace (
      _state, message_flow_outcome_t::received, dispatch_error_surface_t::spot_subscription,
      dispatch_message_kind_t::publish, {}, topic, context._state->spot_id);
    bool handler_found = false;
    for (const auto &descriptor : context._state->handlers) {
        if (packet_name && descriptor.kind == spot_handler_kind_t::subscription
            && descriptor.topic == topic && descriptor.packet_name == *packet_name) {
            handler_found = true;
            break;
        }
    }
    if (!handler_found) {
        report_spot_dispatch_error (
          _state, dispatch_error_surface_t::spot_subscription, dispatch_message_kind_t::publish,
          dispatch_error_reason_t::handler_missing, dispatch_error_action_t::drop, packet_name,
          topic, std::string (context._state->spot_id));
        return result_t<void>::success ();
    }
    auto result =
      spot_handler_registry_t (context._state)
        .invoke_erased (spot_handler_kind_t::subscription, *packet_name, topic,
                        std::type_index (typeid (void)), context._state->spot_instance.get (),
                        nullptr, services, serializers, message)
        .result ();
    if (!result) {
        const auto *error = result.error ();
        const framework_exception_t exception (
          result.error_kind (), error != nullptr ? error->what () : "spot subscription failed");
        report_spot_dispatch_error (
          _state, dispatch_error_surface_t::spot_subscription, dispatch_message_kind_t::publish,
          dispatch_reason_from_error (exception.kind ()), dispatch_error_action_t::drop,
          *packet_name, topic, std::string (context._state->spot_id), std::nullopt,
          std::make_exception_ptr (exception));
        return detail::result_access_t::failure<void> (exception);
    }
    report_spot_dispatch_trace (
      _state, message_flow_outcome_t::dispatched, dispatch_error_surface_t::spot_subscription,
      dispatch_message_kind_t::publish, *packet_name, topic, context._state->spot_id);
    return result_t<void>::success ();
}

result_t<std::size_t> spot_node_runtime_t::dispatch_multicast (
  std::string topic,
  const std::vector<zlink::message_t> &parts,
  service_provider_t &services,
  serializer_registry_t &serializers) const
{
    std::size_t dispatched = 0;
    for (const auto &context : active_contexts ()) {
        const auto subscribed = std::any_of (
          context._state->handlers.begin (), context._state->handlers.end (),
          [&topic] (const spot_handler_descriptor_t &handler) {
              return handler.kind == spot_handler_kind_t::subscription
                     && handler.topic == topic;
          });
        if (!subscribed)
            continue;
        auto delivered = dispatch_subscription (
          context, topic, parts, services, serializers);
        if (!delivered) {
            return detail::propagate_failure<std::size_t> (
              delivered, "spot multicast dispatch failed");
        }
        ++dispatched;
    }
    return result_t<std::size_t>::success (dispatched);
}


} // namespace zlink::framework::detail
