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
#include "runtime/locations/authority_key_codec.hpp"
#include "runtime/locations/live_location_reader.hpp"
#include "runtime/locations/sha256.hpp"
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
#include <condition_variable>
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
constexpr std::size_t max_spot_relocation_state_bytes = 64u * 1024u * 1024u;

constexpr std::uint32_t actor_recv_info_no_bind_flag = 1u;

class remote_actor_commit_turn_state_t
{
  public:
    enum class phase_t
    {
        queued,
        active,
        committing,
        committed,
        terminal
    };

    struct outcome_t
    {
        bool success = false;
        framework_error_kind_t error_kind = framework_error_kind_t::internal_failure;
        std::string error;
    };

    bool begin_active ()
    {
        std::lock_guard lock (_mutex);
        if (_phase == phase_t::terminal)
            return false;
        _phase = phase_t::active;
        return true;
    }

    bool try_begin_commit ()
    {
        std::lock_guard lock (_mutex);
        if (_phase == phase_t::terminal || _stop_requested)
            return false;
        _phase = phase_t::committing;
        return true;
    }

    void mark_committed ()
    {
        std::lock_guard lock (_mutex);
        if (_phase != phase_t::terminal)
            _phase = phase_t::committed;
    }

    void select_cancel_error (framework_error_kind_t kind, std::string error)
    {
        std::lock_guard lock (_mutex);
        _cancel_kind = kind;
        _cancel_error = std::move (error);
    }

    void request_stop ()
    {
        bool notify = false;
        std::function<void (outcome_t)> terminal;
        outcome_t outcome;
        {
            std::lock_guard lock (_mutex);
            _stop_requested = true;
            if (_phase == phase_t::queued) {
                _phase = phase_t::terminal;
                _outcome = outcome_t{false, _cancel_kind, _cancel_error};
                outcome = _outcome;
                terminal = std::move (_terminal);
                notify = true;
            }
        }
        if (notify)
            _condition.notify_all ();
        if (terminal)
            terminal (std::move (outcome));
    }

    void settle (outcome_t value)
    {
        std::function<void (outcome_t)> terminal;
        outcome_t outcome;
        {
            std::lock_guard lock (_mutex);
            if (_phase == phase_t::terminal)
                return;
            _phase = phase_t::terminal;
            _outcome = std::move (value);
            outcome = _outcome;
            terminal = std::move (_terminal);
        }
        _condition.notify_all ();
        if (terminal)
            terminal (std::move (outcome));
    }

    void on_terminal (std::function<void (outcome_t)> terminal)
    {
        outcome_t outcome;
        bool invoke = false;
        {
            std::lock_guard lock (_mutex);
            if (_phase == phase_t::terminal) {
                outcome = _outcome;
                invoke = true;
            } else {
                _terminal = std::move (terminal);
            }
        }
        if (invoke && terminal)
            terminal (std::move (outcome));
    }

    bool wait_until (std::chrono::steady_clock::time_point until)
    {
        std::unique_lock lock (_mutex);
        return _condition.wait_until (lock, until, [&] { return _phase == phase_t::terminal; });
    }

    void wait ()
    {
        std::unique_lock lock (_mutex);
        _condition.wait (lock, [&] { return _phase == phase_t::terminal; });
    }

    outcome_t result () const
    {
        std::lock_guard lock (_mutex);
        return _outcome;
    }

    outcome_t cancel_outcome () const
    {
        std::lock_guard lock (_mutex);
        return outcome_t{false, _cancel_kind, _cancel_error};
    }

    bool terminal () const
    {
        std::lock_guard lock (_mutex);
        return _phase == phase_t::terminal;
    }

  private:
    mutable std::mutex _mutex;
    std::condition_variable _condition;
    phase_t _phase = phase_t::queued;
    bool _stop_requested = false;
    framework_error_kind_t _cancel_kind = framework_error_kind_t::shutting_down;
    std::string _cancel_error = "remote Actor handoff replay was cancelled";
    outcome_t _outcome;
    std::function<void (outcome_t)> _terminal;
};

class remote_actor_commit_deadline_t final
    : public std::enable_shared_from_this<remote_actor_commit_deadline_t>
{
  public:
    static std::shared_ptr<remote_actor_commit_deadline_t>
    start (std::chrono::steady_clock::time_point deadline,
           std::shared_ptr<runtime::offload_executor_t> executor,
           std::function<void ()> expired)
    {
        auto owner = std::shared_ptr<remote_actor_commit_deadline_t> (
          new remote_actor_commit_deadline_t (std::move (executor), std::move (expired)));
        std::weak_ptr<remote_actor_commit_deadline_t> weak_owner = owner;
        owner->_timer.on_fire ([weak_owner] (std::uint64_t) {
            if (auto active = weak_owner.lock (); active && !active->post_fire ()) {
                /* The node executor only rejects after shutdown begins. Move
                 * the last callback owner to a different thread so native
                 * timer destruction never waits on its own callback stack. */
                std::thread ([active = std::move (active)] () mutable {
                    active->fire ();
                }).detach ();
            }
        });
        const auto now = std::chrono::steady_clock::now ();
        owner->_timer.start (deadline > now ? deadline - now : std::chrono::nanoseconds (1), 1);
        return owner;
    }

    ~remote_actor_commit_deadline_t () noexcept
    {
        cancel ();
        try {
            _timer.close ();
        }
        catch (...) {
        }
    }

    void cancel () noexcept
    {
        bool stop = false;
        {
            std::lock_guard lock (_mutex);
            if (_cancelled)
                return;
            _cancelled = true;
            _expired = {};
            stop = !_firing;
        }
        if (!stop)
            return;
        try {
            _timer.stop ();
        }
        catch (...) {
        }
    }

  private:
    explicit remote_actor_commit_deadline_t (std::shared_ptr<runtime::offload_executor_t> executor,
                                             std::function<void ()> expired) :
        _executor (std::move (executor)), _expired (std::move (expired))
    {
    }

    bool post_fire ()
    {
        auto executor = _executor.lock ();
        if (!executor)
            return false;
        auto owner = shared_from_this ();
        return executor->try_submit_internal (
          [owner = std::move (owner)] () mutable { owner->fire (); });
    }

    void fire () noexcept
    {
        std::function<void ()> expired;
        {
            std::lock_guard lock (_mutex);
            if (_cancelled || _firing)
                return;
            _firing = true;
            expired = std::move (_expired);
        }
        if (expired) {
            try {
                expired ();
            }
            catch (...) {
            }
        }
        {
            std::lock_guard lock (_mutex);
            _firing = false;
            _cancelled = true;
        }
    }

    std::mutex _mutex;
    zlink::timer_t _timer;
    std::weak_ptr<runtime::offload_executor_t> _executor;
    std::function<void ()> _expired;
    bool _cancelled = false;
    bool _firing = false;
};

class actor_handoff_barrier_t final : public detail::deferred_barrier_t
{
  public:
    actor_handoff_barrier_t (std::shared_ptr<detail::deferred_barrier_t> inner,
                             std::function<void ()> cancel_reservation,
                             std::function<void ()> settle_reservation) :
        _inner (std::move (inner)),
        _cancel_reservation (std::move (cancel_reservation)),
        _settle_reservation (std::move (settle_reservation))
    {
    }

    result_t<void> activate (std::function<void ()> work) override
    {
        {
            std::lock_guard lock (_mutex);
            if (_activated)
                return result_t<void>::failure (framework_error_kind_t::invalid_operation,
                                                "Actor handoff barrier is already activated");
            _activated = true;
        }
        auto activated =
          _inner->activate ([work = std::move (work), settle = _settle_reservation] () mutable {
              try {
                  work ();
              }
              catch (...) {
                  if (settle)
                      settle ();
                  throw;
              }
              if (settle)
                  settle ();
          });
        if (!activated && _cancel_reservation)
            _cancel_reservation ();
        return activated;
    }

    result_t<void> activate_async (async_work_t work) override
    {
        {
            std::lock_guard lock (_mutex);
            if (_activated)
                return result_t<void>::failure (framework_error_kind_t::invalid_operation,
                                                "Actor handoff barrier is already activated");
            _activated = true;
        }
        auto activated =
          _inner->activate_async ([work = std::move (work), settle = _settle_reservation] (
                                    async_completion_t complete) mutable {
              try {
                  work ([complete = std::move (complete), settle] (result_t<void> result) mutable {
                      if (settle)
                          settle ();
                      complete (std::move (result));
                  });
              }
              catch (const framework_exception_t &error) {
                  if (settle)
                      settle ();
                  complete (detail::result_access_t::failure<void> (error));
              }
              catch (const std::exception &error) {
                  if (settle)
                      settle ();
                  complete (result_t<void>::failure (framework_error_kind_t::internal_failure,
                                                     error.what ()));
              }
          });
        if (!activated && _cancel_reservation)
            _cancel_reservation ();
        return activated;
    }

    void cancel () noexcept override
    {
        bool rollback = false;
        {
            std::lock_guard lock (_mutex);
            rollback = !_activated;
            _activated = true;
        }
        _inner->cancel ();
        if (rollback && _cancel_reservation) {
            try {
                _cancel_reservation ();
            }
            catch (...) {
            }
        }
    }

  private:
    std::shared_ptr<detail::deferred_barrier_t> _inner;
    std::function<void ()> _cancel_reservation;
    std::function<void ()> _settle_reservation;
    std::mutex _mutex;
    bool _activated = false;
};

std::string actor_request_dedup_prefix (std::string_view actor_key)
{
    return std::to_string (actor_key.size ()) + ":" + std::string (actor_key);
}

std::string actor_request_dedup_key (std::string_view actor_key, std::string_view request_id)
{
    auto result = actor_request_dedup_prefix (actor_key);
    result.append (request_id);
    return result;
}

std::optional<std::uint64_t> handoff_u64 (const std::map<std::string, std::string> &metadata,
                                          std::string_view key)
{
    const auto found = metadata.find (std::string (key));
    if (found == metadata.end () || found->second.empty ())
        return std::nullopt;
    std::uint64_t value = 0;
    const auto parsed =
      std::from_chars (found->second.data (), found->second.data () + found->second.size (), value);
    if (parsed.ec != std::errc{} || parsed.ptr != found->second.data () + found->second.size ())
        return std::nullopt;
    return value;
}

std::optional<zlink::routing_id_t>
handoff_routing_id (const std::map<std::string, std::string> &metadata, std::string_view key);

inline constexpr std::string_view actor_handoff_terminal_packet = "__zlink.actorHandoffTerminal";
inline constexpr std::string_view actor_handoff_terminal_success_key =
  "__zlink.actorHandoffTerminalSuccess";
inline constexpr std::string_view actor_handoff_terminal_error_kind_key =
  "__zlink.actorHandoffTerminalErrorKind";
inline constexpr std::string_view actor_handoff_terminal_error_message_key =
  "__zlink.actorHandoffTerminalErrorMessage";

struct handoff_terminal_route_t
{
    // Destination: the node that recorded the pending reply token.
    zlink::routing_id_t source_node;
    // Initiating owner identity, used only to select that pending entry.
    zlink::routing_id_t source_owner_node;
    runtime::protocol::wire_operation_id_t operation;
    std::uint64_t reply_route_id = 0;
    runtime::protocol::actor_route_fence_t source_fence;
    zlink::routing_id_t parking_node;
};

std::optional<runtime::protocol::actor_route_fence_t>
handoff_actor_route (const std::map<std::string, std::string> &metadata);

detail::spot_node_builder_state_t::pending_handoff_request_key_t
handoff_pending_key (const zlink::routing_id_t &source_node,
                     const runtime::protocol::wire_operation_id_t &operation,
                     runtime::protocol::actor_route_fence_t source_fence)
{
    return {source_node.to_hex (), operation.high, operation.low, std::move (source_fence)};
}

void report_handoff_terminal_drop (const std::shared_ptr<detail::spot_node_builder_state_t> &state,
                                   std::string_view reason)
{
    auto monitoring = state->lane.run ([&] { return state->monitoring; }).get ();
    runtime::runtime_metrics_t metrics (std::move (monitoring));
    metrics.counter ("zlink.actor.handoff_terminal.dropped", "{terminal}", 1,
                     {{"reason", std::string (reason)}});
    if (std::getenv ("ZLINK_CPP_HANDOFF_TRACE")) {
        std::cerr << "zlink-cpp-handoff-terminal dropped reason=" << reason << '\n';
    }
}

std::optional<handoff_terminal_route_t>
handoff_terminal_route (const std::map<std::string, std::string> &metadata)
{
    const auto parking = handoff_routing_id (metadata, detail::actor_handoff_parking_node_key);
    const auto source = handoff_routing_id (metadata, detail::actor_handoff_source_node_key);
    const auto high = handoff_u64 (metadata, detail::actor_handoff_operation_high_key);
    const auto low = handoff_u64 (metadata, detail::actor_handoff_operation_low_key);
    const auto reply_route = handoff_u64 (metadata, detail::actor_handoff_reply_route_key);
    const auto source_fence =
      handoff_actor_route (metadata).value_or (runtime::protocol::actor_route_fence_t{});
    /* Clean-break policy: a terminal without the recording parking-node key
     * is explicitly dropped. It must never be redirected to the requester. */
    if (!parking || !source || !high || !low || (!*high && !*low) || !reply_route)
        return std::nullopt;
    return handoff_terminal_route_t{
      *parking, *source, {*high, *low}, *reply_route, std::move (source_fence), *parking};
}

task_t<bool> send_handoff_terminal (const std::shared_ptr<detail::spot_node_builder_state_t> &state,
                                    const std::optional<handoff_terminal_route_t> &route,
                                    const result_t<zlink::message_t> &completed)
{
    if (!route) {
        report_handoff_terminal_drop (state, "missing_parking_node");
        co_return true;
    }
    const auto local_node_rid = state->lane.run ([&] {
        return detail::effective_spot_node_rid (state->snapshot);
    }).get ();
    if (route->source_node.to_string () == local_node_rid) {
        const auto pending = state->pending_handoff_requests_lane
                               .run ([&] () -> std::optional<
                                 detail::spot_node_builder_state_t::pending_handoff_request_t> {
            const auto found = state->pending_handoff_requests.find (handoff_pending_key (
              route->source_owner_node, route->operation, route->source_fence));
            if (found == state->pending_handoff_requests.end ()
                || found->second.reply_route_id != route->reply_route_id
                || route->parking_node.to_string () != local_node_rid) {
                return std::nullopt;
            }
            auto pending = std::move (found->second);
            state->pending_handoff_requests.erase (found);
            return pending;
        })
                               .get ();
        if (!pending) {
            co_return true;
        }
        detail::channel_reply_writer_t replies;
        auto terminal =
          completed
            ? replies.reply_raw_envelope (
                replies.create_reply_header (runtime::messaging::message_kind_t::response,
                                             pending->request_header.channel_name,
                                             pending->request_header),
                completed.value ())
            : replies.reply_raw_envelope (
                replies.create_error_header (
                  pending->request_header.channel_name, pending->request_header,
                  completed.error () ? *completed.error ()
                                     : framework_exception_t (completed.error_kind (),
                                                              "Actor handoff request failed")),
                zlink::message_t{});
        (void) service::reply (pending->reply_token, terminal.items ());
        co_return true;
    }
    const auto terminal_sender = state->lane.run ([&] {
        return state->actor_handoff_terminal_sender;
    }).get ();
    if (!terminal_sender)
        co_return false;
    co_return co_await terminal_sender (
      route->source_node, route->source_owner_node, route->operation, route->reply_route_id,
      route->source_fence, completed);
}

void order_bound_session_handoff (std::vector<detail::handoff_packet_t> &backlog)
{
    std::map<std::uint64_t, std::vector<std::size_t>> positions_by_binding;
    for (std::size_t index = 0; index < backlog.size (); ++index) {
        const auto binding =
          handoff_u64 (backlog[index].metadata, detail::bound_session_relay_binding_key);
        const auto sequence =
          handoff_u64 (backlog[index].metadata, detail::bound_session_relay_sequence_key);
        if (sequence && *sequence != 0)
            positions_by_binding[binding.value_or (0)].push_back (index);
    }
    for (const auto &[binding, positions] : positions_by_binding) {
        (void) binding;
        if (positions.size () < 2)
            continue;
        std::vector<std::pair<std::uint64_t, detail::handoff_packet_t>> ordered;
        ordered.reserve (positions.size ());
        for (const auto position : positions) {
            const auto sequence =
              handoff_u64 (backlog[position].metadata, detail::bound_session_relay_sequence_key);
            ordered.emplace_back (*sequence, std::move (backlog[position]));
        }
        std::stable_sort (
          ordered.begin (), ordered.end (),
          [] (const auto &left, const auto &right) { return left.first < right.first; });
        for (std::size_t index = 0; index < positions.size (); ++index)
            backlog[positions[index]] = std::move (ordered[index].second);
    }
}

std::optional<zlink::routing_id_t>
handoff_routing_id (const std::map<std::string, std::string> &metadata, std::string_view key)
{
    const auto found = metadata.find (std::string (key));
    if (found == metadata.end () || found->second.empty ())
        return std::nullopt;
    try {
        auto value = zlink::routing_id_t::from_hex (found->second);
        const auto bytes = value.to_bytes ();
        if (bytes.empty ()
            || std::all_of (bytes.begin (), bytes.end (), [] (auto byte) { return byte == 0; })) {
            return std::nullopt;
        }
        return value;
    }
    catch (...) {
        return std::nullopt;
    }
}

std::optional<runtime::protocol::actor_route_fence_t>
handoff_actor_route (const std::map<std::string, std::string> &metadata)
{
    const auto actor_id = metadata.find (std::string (detail::actor_handoff_route_actor_id_key));
    const auto target_node =
      handoff_routing_id (metadata, detail::actor_handoff_route_target_node_key);
    const auto object_generation =
      handoff_u64 (metadata, detail::actor_handoff_route_object_generation_key);
    const auto target_node_generation =
      handoff_u64 (metadata, detail::actor_handoff_route_target_node_generation_key);
    const auto authority_generation =
      handoff_u64 (metadata, detail::actor_handoff_route_authority_generation_key);
    const auto lease_generation =
      handoff_u64 (metadata, detail::actor_handoff_route_lease_generation_key);
    if (actor_id == metadata.end () || actor_id->second.empty () || !target_node
        || !object_generation || !target_node_generation || !authority_generation
        || !lease_generation || *object_generation == 0 || *target_node_generation == 0
        || *authority_generation == 0 || *lease_generation == 0) {
        return std::nullopt;
    }
    return runtime::protocol::actor_route_fence_t{actor_id->second,         *object_generation,
                                                  target_node->to_bytes (), *target_node_generation,
                                                  *authority_generation,    *lease_generation};
}

std::optional<std::string> actor_type_from_authority (runtime::live_location_reader_t &store,
                                                      std::string_view actor_id)
{
    const auto read = store.read_authority (runtime::actor_authority_key (actor_id)).result ();
    if (!read)
        return std::nullopt;
    const auto *snapshot = std::get_if<authority_snapshot_t> (&read.value ());
    if (!snapshot || snapshot->allocation.state != placement_allocation_state_t::active
        || snapshot->allocation.object_kind != placement_object_kind_t::actor)
        return std::nullopt;
    const auto projection =
      runtime::decode_actor_authority_payload (snapshot->payload, snapshot->object_generation);
    if (!projection || projection->actor.actor_id ().value () != actor_id)
        return std::nullopt;
    return std::string (
      ::zlink::framework::detail::actor_ref_access_t::actor_type (projection->actor));
}

struct actor_join_authority_fence_t
{
    std::uint64_t object_generation = 0;
    std::uint64_t node_generation = 0;
    std::uint64_t authority_owner_generation = 0;
    std::uint64_t owner_lease_generation = 0;
};

result_t<std::string> actor_type_from_authority (runtime::live_location_reader_t &store,
                                                 const actor_ref_t &wire_actor,
                                                 const actor_join_authority_fence_t &fence,
                                                 bool actor_type_from_authority_only)
{
    const auto actor_id = wire_actor.actor_id ().value ();
    // Object/authority-owner/owner-lease generations are bounded counters;
    // the MeshNode lifecycle generation is an opaque full-range token.
    if (actor_id.empty () || fence.object_generation == 0 || fence.node_generation == 0
        || fence.authority_owner_generation == 0 || fence.owner_lease_generation == 0) {
        return result_t<std::string>::failure (framework_error_kind_t::protocol_error,
                                               "remote Actor Join authority fence is incomplete");
    }
    try {
        const auto read = store.read_authority (runtime::actor_authority_key (actor_id)).result ();
        if (!read) {
            return result_t<std::string>::failure (
              framework_error_kind_t::unavailable,
              "remote Actor Join could not read its Authority row");
        }
        const auto *snapshot = std::get_if<authority_snapshot_t> (&read.value ());
        if (snapshot == nullptr) {
            return result_t<std::string>::failure (
              framework_error_kind_t::not_found,
              "remote Actor Join Actor Authority row is missing");
        }
        if (snapshot->allocation.state != placement_allocation_state_t::active
            || snapshot->allocation.object_kind != placement_object_kind_t::actor
            || snapshot->allocation.stable_type.empty ()
            || snapshot->object_generation != fence.object_generation
            || snapshot->allocation.target.node_rid.value () != wire_actor.node_rid ().value ()
            || snapshot->allocation.target.node_lifecycle_generation != fence.node_generation
            || snapshot->authority_owner_generation != fence.authority_owner_generation
            || snapshot->owner.lease_generation <= 0
            || static_cast<std::uint64_t> (snapshot->owner.lease_generation)
                 != fence.owner_lease_generation) {
            return result_t<std::string>::failure (
              framework_error_kind_t::protocol_error,
              "remote Actor Join Authority row does not exactly match its route fence");
        }
        const auto projection =
          runtime::decode_actor_authority_payload (snapshot->payload, snapshot->object_generation);
        if (!projection || projection->actor.actor_id ().value () != actor_id) {
            return result_t<std::string>::failure (
              framework_error_kind_t::protocol_error,
              "remote Actor Join Actor Authority row is incomplete");
        }
        const auto stable_type = snapshot->allocation.stable_type;
        // Canonical actorJoin(28) intentionally has no wire stable type.
        // The legacy/JSON path still supplies one and remains protected by
        // this forgery cross-check; the canonical path obtains its type only
        // from the Authority row above.
        const auto wire_type =
          ::zlink::framework::detail::actor_ref_access_t::actor_type (wire_actor);
        if ((!actor_type_from_authority_only || !wire_type.empty ()) && wire_type != stable_type) {
            return result_t<std::string>::failure (
              framework_error_kind_t::type_mismatch,
              "remote Actor Join wire type does not match its Authority row");
        }
        return result_t<std::string>::success (stable_type);
    }
    catch (const std::exception &) {
        return result_t<std::string>::failure (
          framework_error_kind_t::unavailable,
          "remote Actor Join could not read its Authority row");
    }
    catch (...) {
        return result_t<std::string>::failure (
          framework_error_kind_t::unavailable,
          "remote Actor Join could not read its Authority row");
    }
}

bool is_blank (const std::string &value)
{
    return std::all_of (value.begin (), value.end (),
                        [] (unsigned char ch) { return std::isspace (ch) != 0; });
}

std::size_t handler_work_byte_cost (const zlink::message_t &message,
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
framework_worker_executor_core (const std::shared_ptr<detail::spot_node_builder_state_t> &node)
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

std::shared_ptr<runtime::offload_executor_t>
framework_worker_executor (const std::shared_ptr<detail::spot_node_builder_state_t> &node)
{
    return node->lane.run ([&] { return framework_worker_executor_core (node); }).get ();
}

std::shared_ptr<runtime::offload_executor_t>
framework_deadline_executor_core (const std::shared_ptr<detail::spot_node_builder_state_t> &node)
{
    if (node && node->deadline_executor)
        return node->deadline_executor;
    auto executor = std::make_shared<runtime::offload_executor_t> (1, 0, "zlink-spot-deadline");
    if (node)
        node->deadline_executor = executor;
    return executor;
}

std::shared_ptr<runtime::offload_executor_t>
framework_deadline_executor (const std::shared_ptr<detail::spot_node_builder_state_t> &node)
{
    return node->lane.run ([&] { return framework_deadline_executor_core (node); }).get ();
}

void configure_spot_execution (
  const std::shared_ptr<detail::spot_context_state_t> &state,
  const std::shared_ptr<runtime::offload_executor_t> &worker_executor)
{
    /* The queue owns turn ordering. Execution resources are shared by the
     * node; a Spot must not allocate its own worker pool. A yielded turn is
     * resumed through this same queue, so it does not require a Spot-local
     * executor. */
    state->serial_executor = worker_executor;
    state->serial_queue = std::make_shared<runtime::serial_execution_queue_t> (
      *state->serial_executor, runtime::serial_execution_queue_options_t{},
      runtime::serial_execution_queue_t::error_handler_t{},
      state->is_entry_spot () ? runtime::serial_lane_policy_t::entry_spot ()
      : state->execution_mode == user_spot_execution_mode_t::spot_wide
        ? runtime::serial_lane_policy_t::spot_wide ()
        : runtime::serial_lane_policy_t::per_actor_spot ());
    state->spot_serial_executor = std::make_shared<detail::spot_serial_executor_t> (
      worker_executor,
      state->node->lane,
      state->is_entry_spot () ? runtime::serial_lane_policy_t::entry_spot ()
      : state->execution_mode == user_spot_execution_mode_t::spot_wide
        ? runtime::serial_lane_policy_t::spot_wide ()
        : runtime::serial_lane_policy_t::per_actor_spot (),
      state->serial_queue);
    state->worker_scheduler =
      std::make_shared<spot_worker_scheduler_t> (worker_executor, state);
}

} // namespace

namespace detail
{

namespace
{

thread_local const spot_context_state_t *current_callback_context = nullptr;

class callback_context_scope_t final
{
  public:
    explicit callback_context_scope_t (const spot_context_state_t *owner) noexcept :
        _previous (std::exchange (current_callback_context, owner))
    {
    }

    ~callback_context_scope_t () { current_callback_context = _previous; }

  private:
    const spot_context_state_t *_previous;
};

} // namespace

spot_node_builder_state_t::~spot_node_builder_state_t () = default;

void drain_spot_node_executors (spot_node_builder_state_t &node)
{
    struct drain_plan_t
    {
        struct context_t
        {
            std::shared_ptr<spot_context_state_t> state;
            std::vector<std::shared_ptr<runtime::serial_execution_queue_t>> timer_lanes;
            std::shared_ptr<runtime::serial_execution_queue_t> serial_queue;
        };
        std::vector<context_t> contexts;
        std::shared_ptr<runtime::offload_executor_t> deadline_executor;
        std::shared_ptr<runtime::offload_executor_t> worker_executor;
    };
    const auto plan = node.lane.run ([&] {
        drain_plan_t result;
        result.contexts.reserve (node.spot_contexts_by_id.size ());
        for (const auto &[_, context] : node.spot_contexts_by_id) {
            const auto state = context._state;
            if (!state)
                continue;
            result.contexts.push_back (drain_plan_t::context_t{state, {}, state->serial_queue});
            auto &context_plan = result.contexts.back ();
            if (state->spot_serial_executor)
                context_plan.timer_lanes = state->spot_serial_executor->timer_queues ();
        }
        result.deadline_executor = node.deadline_executor;
        result.worker_executor = node.worker_executor;
        return result;
    }).get ();

    for (const auto &context : plan.contexts) {
        for (const auto &timer_lane : context.timer_lanes)
            if (timer_lane)
                timer_lane->cancel_pending ();
        if (context.serial_queue)
            context.serial_queue->cancel_pending ();
    }
    for (const auto &context : plan.contexts) {
        for (const auto &timer_lane : context.timer_lanes)
            if (timer_lane)
                timer_lane->drain ();
        if (context.serial_queue) {
            context.serial_queue->cancel_pending ();
            context.serial_queue->drain ();
        }
    }
    if (plan.deadline_executor)
        plan.deadline_executor->drain ();
    if (plan.worker_executor)
        plan.worker_executor->drain ();

    node.lane.run ([&] {
        for (const auto &context : plan.contexts) {
            context.state->serial_executor.reset ();
            if (context.state->spot_serial_executor)
                context.state->spot_serial_executor->close ();
            context.state->spot_serial_executor.reset ();
            context.state->worker_scheduler.reset ();
        }
        node.deadline_executor.reset ();
        node.worker_executor.reset ();
    }).get ();
}

void cancel_spot_node_dispatch_queues (spot_node_builder_state_t &node)
{
    const auto queues = node.lane.run ([&] {
        std::vector<std::shared_ptr<runtime::serial_execution_queue_t>> result;
        result.reserve (node.spot_contexts_by_id.size ());
        for (const auto &[_, context] : node.spot_contexts_by_id) {
            if (const auto queue = context._state ? context._state->serial_queue : nullptr)
                result.push_back (queue);
        }
        return result;
    }).get ();
    for (const auto &queue : queues)
        if (queue)
            queue->cancel_pending ();
}

} // namespace detail

namespace
{

zlink::message_t framework_reply_or_empty (const std::optional<message_t> &reply,
                                           serializer_registry_t &serializers)
{
    return reply ? detail::message_to_raw (*reply, serializers) : zlink::message_t{};
}

} // namespace

std::shared_ptr<service::spot_t> detail::spot_node_runtime_t::attach_native_spot (
  const std::shared_ptr<detail::spot_context_state_t> &state,
  bool relocation_restore,
  bool publish)
{
    if (!state || !state->node) {
        return {};
    }
    struct attachment_plan_t
    {
        std::shared_ptr<service::mesh_node_t> native_node;
        std::shared_ptr<service::spot_t> native;
        std::string rid;
        std::string spot_name;
        std::string mesh_name;
        std::string node_rid;
        std::uint64_t object_generation = 0;
        std::uint64_t authority_owner_generation = 0;
        bool entry_spot = false;
        bool instance_spot = false;
        std::string subscription_channel;
        std::vector<spot_handler_descriptor_t> handlers;
    };
    auto owner = state->node;
    auto plan = owner->lane.run ([&] {
        attachment_plan_t result;
        result.native_node = owner->native_node.lock ();
        result.rid = std::string (state->spot_id);
        result.spot_name = state->spot_name;
        result.mesh_name = state->mesh_name;
        result.node_rid = std::string (state->node_rid.value ());
        result.object_generation = state->object_generation;
        result.authority_owner_generation = state->authority_owner_generation;
        result.entry_spot = owner->snapshot.entry_spot_name
                            && *owner->snapshot.entry_spot_name == state->spot_name;
        result.instance_spot = state->is_instance_spot ();
        result.subscription_channel =
          owner->snapshot.discovery_channel_name.value_or (owner->snapshot.name);
        result.handlers = state->handlers;
        result.native = state->native_spot.lock ();
        if (!result.native) {
            const auto found = owner->native_spots_by_id.find (result.rid);
            if (found != owner->native_spots_by_id.end ())
                result.native = found->second;
        }
        return result;
    }).get ();
    if (!plan.native_node)
        return {};

    auto native = plan.native;
    if (!native) {
        if (plan.entry_spot) {
            native = std::make_shared<service::spot_t> (plan.native_node->entry_spot ());
        } else {
            try {
                auto spot = relocation_restore
                              ? plan.native_node->bind_relocation_spot (
                                  runtime::stateful::object_ref_t{
                                    plan.instance_spot
                                      ? runtime::stateful::object_kind_t::instance_spot
                                      : runtime::stateful::object_kind_t::user_spot,
                                    plan.rid, plan.object_generation,
                                    plan.authority_owner_generation, plan.mesh_name, plan.node_rid})
                              : plan.native_node->get_or_create_spot (plan.rid);
                native = std::make_shared<service::spot_t> (std::move (spot));
            }
            catch (const std::exception &error) {
                throw framework_exception_t (framework_error_kind_t::internal_failure,
                                             "native spot facade creation failed for '"
                                               + plan.spot_name + "' (rid='" + plan.rid
                                               + "'): " + error.what ());
            }
        }
    }
    if (publish) {
        native = owner->lane.run ([&] {
            auto [found, inserted] = owner->native_spots_by_id.emplace (plan.rid, native);
            if (!inserted)
                native = found->second;
            state->native_spot = native;
            return native;
        }).get ();
    } else {
        // A creating context is not published yet, so its facade can stay
        // private until the activation and Location Store claim both succeed.
        state->native_spot = native;
    }

    for (const auto &handler : plan.handlers) {
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
                      plan.subscription_channel, handler.topic);
                    activated = true;
                }
                catch (const std::exception &error) {
                    last_error = error.what ();
                    std::this_thread::sleep_for (std::chrono::milliseconds (100));
                }
            }
            if (!activated) {
                throw framework_exception_t (framework_error_kind_t::internal_failure,
                                             "native spot subscription activation failed for '"
                                               + plan.spot_name + "' (rid='" + plan.rid + "', topic='"
                                               + handler.topic + "'): " + last_error);
            }
        }
    }
    return native;
}

namespace
{

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
    detail::dispatch_error_reporter_t (state->dispatch).report_lazy ([&] {
        return message_dispatch_error_event_t{surface,
                                              message_kind,
                                              reason,
                                              action,
                                              std::move (packet_name),
                                              std::nullopt,
                                              std::move (topic),
                                              std::move (spot_id),
                                              std::move (actor_id),
                                              std::nullopt,
                                              std::move (correlation_id),
                                              std::move (exception),
                                              std::nullopt,
                                              std::nullopt};
    });
}

void report_spot_dispatch_trace (const std::shared_ptr<detail::spot_node_builder_state_t> &state,
                                 message_flow_outcome_t outcome,
                                 dispatch_error_surface_t surface,
                                 dispatch_message_kind_t message_kind,
                                 std::string_view packet_name = {},
                                 std::string_view topic = {},
                                 std::string_view spot_id = {},
                                 std::string_view actor_id = {},
                                 std::string_view correlation_id = {},
                                 std::optional<message_flow_result_t> result = std::nullopt,
                                 std::optional<message_flow_reason_t> reason = std::nullopt)
{
    if (!state) {
        return;
    }
    // string_view params + lazy build: callers pass cheap views; std::string is
    // only allocated inside the lambda after the gate passes (zero cost when off).
    detail::message_flow_tracer_t (state->dispatch).trace (outcome, result, [&] {
        auto field = [] (std::string_view value) -> std::optional<std::string> {
            if (value.empty ()) {
                return std::nullopt;
            }
            return std::string (value);
        };
        auto event = message_flow_event_t{outcome,
                                          surface,
                                          message_kind,
                                          field (packet_name),
                                          std::nullopt,
                                          field (topic),
                                          field (correlation_id),
                                          std::nullopt,
                                          field (spot_id),
                                          field (actor_id),
                                          std::nullopt};
        event.result = result;
        event.reason = reason;
        return event;
    });
}

template <typename BuildResult>
void report_actor_dispatch_stage_trace_lazy (
  const std::shared_ptr<detail::spot_node_builder_state_t> &state,
  message_flow_outcome_t outcome,
  dispatch_message_kind_t message_kind,
  std::string_view packet_name,
  std::string_view spot_id,
  std::string_view actor_id,
  std::string_view stage,
  BuildResult &&build_result)
{
    if (!state) {
        return;
    }
    detail::message_flow_tracer_t tracer (state->dispatch);
    tracer.trace (message_flow_log_mode_t::detailed, outcome, [&] {
        return message_flow_event_t{
          .outcome = outcome,
          .surface = dispatch_error_surface_t::spot_actor,
          .message_kind = message_kind,
          .packet_name =
            packet_name.empty () ? std::nullopt : std::make_optional (std::string (packet_name)),
          .spot_id = spot_id.empty () ? std::nullopt : std::make_optional (std::string (spot_id)),
          .actor_id =
            actor_id.empty () ? std::nullopt : std::make_optional (std::string (actor_id)),
          .detail_stage = std::string (stage),
          .detail_result = std::forward<BuildResult> (build_result) ()};
    });
}

void report_actor_dispatch_stage_trace (
  const std::shared_ptr<detail::spot_node_builder_state_t> &state,
  message_flow_outcome_t outcome,
  dispatch_message_kind_t message_kind,
  std::string_view packet_name,
  std::string_view spot_id,
  std::string_view actor_id,
  std::string_view stage,
  std::string_view result)
{
    report_actor_dispatch_stage_trace_lazy (state, outcome, message_kind, packet_name, spot_id,
                                            actor_id, stage, [&] { return std::string (result); });
}

zlink::message_t encode_spot_publish_frame (std::string channel_name,
                                            std::string packet_name,
                                            const std::string &topic,
                                            std::string content_type,
                                            const zlink::message_t &payload)
{
    runtime::messaging::envelope_header_t header;
    header.kind = runtime::messaging::message_kind_t::publish;
    header.channel_name = std::move (channel_name);
    header.message_name = std::move (packet_name);
    header.content_type = std::move (content_type);
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
    detail::message_flow_tracer_t (state->dispatch)
      .trace (message_flow_outcome_t::dispatched, transfer_id, [&] {
          return message_flow_event_t{.outcome = message_flow_outcome_t::dispatched,
                                      .surface = dispatch_error_surface_t::spot_actor,
                                      .message_kind = dispatch_message_kind_t::actor_request,
                                      .packet_name = marker,
                                      .channel_name = "request",
                                      .correlation_id = request_id,
                                      .actor_id = std::string (actor_ref.actor_id ().value ()),
                                      .flow_id = transfer_id,
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
    state.actor_authority_fences.erase (key);
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
      .actor_type =
        std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor)),
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
    return actor_location_t{
      .mesh_name = context.node->snapshot.name,
      .actor_id = std::string (actor.actor_id ().value ()),
      .actor_type =
        std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor)),
      .actor_ref = actor,
      .owner_node_rid = zlink::routing_id_t::from (std::string (context.node_rid.value ())),
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
    const auto key =
      std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor)) + ":"
      + std::string (actor.actor_id ().value ());
    const auto deactivated = state->lane.run ([&] {
        // A lost claim races with a completed transfer: after this node hands the
        // actor to another node it records the newer generation and Message
        // Message Follow route. A loss notification for an older generation is stale
        // and must not erase that newer record.
        const auto recorded = state->actor_generations.find (key);
        if (recorded != state->actor_generations.end ()
            && recorded->second > actor.object_generation ()) {
            return false;
        }
        // ObjectGeneration alone cannot distinguish a returning incarnation
        // (A→B→A keeps the generation while the owner fences advance). While
        // a transfer for this Actor is in flight on this node, a location
        // loss can only refer to the superseded claim: the transfer commit
        // owns the record lifecycle, so the stale loss must not erase the
        // admission being established.
        if (state->actor_transfer_coordinator.blocks_dispatch (key)) {
            return false;
        }
        erase_actor_route_unlocked (*state, key);
        state->actor_created_keys.erase (key);
        state->destroyed_actor_keys.insert (key);
        state->actor_instances.erase (key);
        detail::erase_actor_instance_index_unlocked (
          *state, ::zlink::framework::detail::actor_ref_access_t::actor_type (actor),
          actor.actor_id ().value ());
        (void) state->dispatched_request_replies.erase_if ([&] (const auto &request_key) {
            return request_key.starts_with (actor_request_dedup_prefix (key));
        });
        destroy_actor_registry = state->destroy_actor_registry;
        return true;
    }).get ();
    if (!deactivated)
        return;
    if (destroy_actor_registry) {
        (void) destroy_actor_registry (actor);
    }
}

struct actor_location_plan_t
{
    runtime::location_lifecycle_t *lifecycle = nullptr;
    std::shared_ptr<service::mesh_node_t> native_node;
    std::shared_ptr<service::spot_t> native_spot;
    std::string mesh_name;
    node_rid_t context_node_rid;
    spot_id_t spot_id;
    std::uint64_t membership_epoch = 0;
    bool attached = false;
};

actor_location_plan_t capture_actor_location_plan (
  const std::shared_ptr<detail::spot_node_builder_state_t> &state,
  const actor_ref_t &actor,
  const detail::spot_context_state_t &context)
{
    return state->lane.run ([&] {
        actor_location_plan_t result;
        result.lifecycle = state->location_lifecycle;
        result.attached = context.node.get () == state.get ();
        if (!result.attached)
            return result;
        result.native_node = state->native_node.lock ();
        result.native_spot = context.native_spot.lock ();
        result.mesh_name = state->snapshot.name;
        result.context_node_rid = context.node_rid;
        result.spot_id = context.spot_id;
        const auto epoch = state->core_actor_membership_epochs.find (
          std::string (actor.actor_id ().value ()));
        if (epoch != state->core_actor_membership_epochs.end ())
            result.membership_epoch = epoch->second;
        return result;
    }).get ();
}

actor_location_t materialize_actor_location (const actor_ref_t &actor,
                                              const actor_location_plan_t &plan,
                                              bool entry)
{
    return actor_location_t{
      .mesh_name = plan.mesh_name,
      .actor_id = std::string (actor.actor_id ().value ()),
      .actor_type =
        std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor)),
      .actor_ref = actor,
      .owner_node_rid = entry
                          ? zlink::routing_id_t::from (std::string (plan.context_node_rid.value ()))
                          : zlink::routing_id_t::from (
                              std::string (actor.node_rid ().value ())),
      .owner_node_generation =
        plan.native_node ? plan.native_node->status ().lifecycle_generation () : 0,
      .spot_id = plan.spot_id,
      .spot_generation =
        plan.native_spot ? plan.native_spot->status ().lifecycle_generation () : 0,
      .spot_kind = entry ? zlink::spot_kind::entry : zlink::spot_kind::user,
      .membership_epoch = plan.membership_epoch};
}

result_t<void> claim_actor_location_before_activation (
  const std::shared_ptr<detail::spot_node_builder_state_t> &state,
  const actor_ref_t &committed,
  const detail::spot_context_state_t &context,
  bool &claimed,
  bool takeover = false)
{
    claimed = false;
    const auto plan = capture_actor_location_plan (state, committed, context);
    if (!plan.lifecycle) {
        return result_t<void>::success ();
    }
    if (!plan.attached) {
        return result_t<void>::failure (
          framework_error_kind_t::unavailable,
          "actor location claim raced Spot context teardown");
    }
    if (plan.lifecycle->owns_actor (actor_location_key_t{
          plan.mesh_name, std::string (committed.actor_id ().value ())})) {
        return result_t<void>::success ();
    }

    auto location = materialize_actor_location (committed, plan, false);
    const auto claim_result = plan.lifecycle->claim_actor (
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
    const auto plan = capture_actor_location_plan (state, committed, target);
    if (!plan.lifecycle) {
        return result_t<void>::success ();
    }
    if (!plan.attached) {
        return result_t<void>::failure (
          framework_error_kind_t::unavailable,
          "pending actor location claim raced Spot context teardown");
    }
    const bool source_is_local =
      !source_actor.node_rid ().empty ()
      && source_actor.node_rid ().value () == plan.context_node_rid.value ();
    if (source_is_local
        && plan.lifecycle->owns_actor (actor_location_key_t{
          plan.mesh_name, std::string (committed.actor_id ().value ())})) {
        return result_t<void>::success ();
    }
    auto location = materialize_actor_location (committed, plan, false);
    location.actor_ref = source_actor;
    location.owner_node_rid =
      zlink::routing_id_t::from (std::string (source_actor.node_rid ().value ()));
    location.spot_id =
      source_spot_id.empty () ? std::string (source_actor.node_rid ().value ()) : source_spot_id;
    location.spot_kind = source_spot_id.empty () ? zlink::spot_kind::entry : zlink::spot_kind::user;
    const auto result = plan.lifecycle->claim_actor (
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

void release_actor_location (
  const std::shared_ptr<detail::spot_node_builder_state_t> &state, const actor_ref_t &actor)
{
    if (!state || ::zlink::framework::detail::actor_ref_access_t::empty (actor)) {
        return;
    }
    auto [lifecycle, mesh_name] = state->lane.run ([&] {
        return std::make_pair (state->location_lifecycle, state->snapshot.name);
    }).get ();
    if (!lifecycle)
        return;
    (void) lifecycle->release_actor (
      actor_location_key_t{std::move (mesh_name), std::string (actor.actor_id ().value ())});
}

result_t<void> update_actor_location_after_move (
                                                 const std::shared_ptr<detail::spot_node_builder_state_t> &state,
                                                 const actor_ref_t &actor,
                                                 const detail::spot_context_state_t &context,
                                                 bool entry)
{
    if (::zlink::framework::detail::actor_ref_access_t::empty (actor)) {
        return result_t<void>::success ();
    }
    //  Spot context teardown can detach `node` while a membership commit is
    //  still completing on another boundary. Building the location would
    //  dereference the detached pointer; report a typed lifecycle failure
    //  instead (spec 15 §3 — membership/lifecycle stay ordered on the Spot
    //  turn boundary; a detached context means that boundary has ended).
    const auto plan = capture_actor_location_plan (state, actor, context);
    if (!plan.lifecycle)
        return result_t<void>::success ();
    if (!plan.attached) {
        return result_t<void>::failure (
          framework_error_kind_t::unavailable,
          "actor committed location update raced Spot context teardown");
    }
    auto location = materialize_actor_location (actor, plan, entry);
    const auto tracked = plan.lifecycle->owns_actor (
      actor_location_key_t{plan.mesh_name, std::string (actor.actor_id ().value ())});
    const auto updated = plan.lifecycle->update_actor_location (std::move (location));
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
    struct route_channel_projection_t
    {
        std::shared_ptr<detail::channel_runtime_state_t> channel_runtime;
        std::optional<std::string> configured;
        std::optional<std::string> accepted;
    };
    const auto projection = state->node->lane.run ([&] {
        route_channel_projection_t result{.channel_runtime = state->channel_runtime,
                                          .configured =
                                            state->node->snapshot.spot_route_channel_name};
        if (state->node->snapshot.accepted_route_channels.size () == 1) {
            result.accepted =
              state->node->snapshot.accepted_route_channels.front ().channel_name;
        }
        return result;
    }).get ();
    if (!projection.channel_runtime)
        return std::nullopt;
    return projection.channel_runtime->lane.run ([&] () -> std::optional<std::string> {
        if (projection.configured) {
            if (projection.channel_runtime->route_channels.find (*projection.configured)
                != projection.channel_runtime->route_channels.end ()) {
                return projection.configured;
            }
        }
        if (projection.channel_runtime->route_channels.size () == 1) {
            return projection.channel_runtime->route_channels.begin ()->first;
        }
        if (projection.accepted) {
            if (projection.channel_runtime->route_channels.find (*projection.accepted)
                != projection.channel_runtime->route_channels.end ()) {
                return projection.accepted;
            }
        }
        return std::nullopt;
    }).get ();
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

std::optional<std::uint64_t>
resolve_target_spot_generation (const std::shared_ptr<detail::spot_node_builder_state_t> &state,
                                const zlink::routing_id_t &target_node_rid,
                                const spot_id_t &target_spot_id)
{
    if (!state) {
        return std::nullopt;
    }
    runtime::spot_address_resolver_t *resolver = nullptr;
    std::string mesh_name;
    std::shared_ptr<service::spot_t> local_spot;
    state->lane.run ([&] {
        mesh_name = state->snapshot.name;
        resolver = state->spot_location_resolver;
        const auto local = state->native_spots_by_id.find (target_spot_id);
        if (local != state->native_spots_by_id.end ()
            && detail::effective_spot_node_rid (state->snapshot) == target_node_rid.to_string ()) {
            local_spot = local->second;
        }
    }).get ();
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
      resolver->resolve_spot_address (std::move (mesh_name), target_spot_id).result ().value ();
    if (!address || address->node_rid != target_node_rid || address->spot_generation == 0) {
        return std::nullopt;
    }
    return address->spot_generation;
}

framework_exception_t
spot_request_terminal_exception (runtime::foundation::operation_terminal_t terminal)
{
    switch (terminal) {
        case runtime::foundation::operation_terminal_t::timed_out:
            return detail::make_boundary_exception (detail::boundary_error_t::timed_out,
                                                    "SPOT mesh request timed out");
        case runtime::foundation::operation_terminal_t::cancelled:
            return detail::make_boundary_exception (detail::boundary_error_t::cancelled,
                                                    "SPOT mesh request was cancelled");
        case runtime::foundation::operation_terminal_t::transport_failed:
            return detail::make_boundary_exception (detail::boundary_error_t::disconnected,
                                                    "SPOT mesh request lost its connection");
        case runtime::foundation::operation_terminal_t::shutdown:
            return detail::make_boundary_exception (
              detail::boundary_error_t::shutdown,
              "SPOT mesh request stopped because the runtime is shutting down");
        case runtime::foundation::operation_terminal_t::completed:
            break;
    }
    return framework_exception_t (framework_error_kind_t::internal_failure,
                                  "SPOT mesh request completed without a terminal result");
}

task_t<runtime::messaging::message_parts_t>
request_spot_parts_async (service::spot_handle_t egress,
                          const zlink::routing_id_t &target_node_rid,
                          const spot_id_t &target_spot_id,
                          std::uint64_t target_generation,
                          runtime::messaging::message_parts_t parts,
                          std::chrono::milliseconds timeout)
{
    auto source =
      std::make_shared<detail::task_completion_source_t<runtime::messaging::message_parts_t>> ();
    auto output = source->task ();
    try {
        const auto &native_parts = parts.items ();
        if (native_parts.empty ()) {
            source->complete (result_t<runtime::messaging::message_parts_t>::failure (
              framework_error_kind_t::protocol_error,
              "SPOT mesh request requires at least one message part"));
            co_return co_await output;
        }
        service::call_id_t operation_id;
        const auto submitted = co_await egress.request_to_spot (
          target_node_rid, target_spot_id, target_generation, native_parts, operation_id,
          zlink::send_flags_t::none, timeout, {},
          [source] (runtime::foundation::operation_terminal_t terminal,
                    result_t<std::vector<zlink::message_t>> decoded) mutable {
              if (terminal != runtime::foundation::operation_terminal_t::completed) {
                  source->complete (
                    detail::result_access_t::failure<runtime::messaging::message_parts_t> (
                      spot_request_terminal_exception (terminal)));
                  return;
              }
              if (!decoded) {
                  source->complete (detail::propagate_failure<runtime::messaging::message_parts_t> (
                    decoded, "SPOT mesh request reply decode failed"));
                  return;
              }
              source->complete (result_t<runtime::messaging::message_parts_t>::success (
                runtime::messaging::message_parts_t (std::move (decoded.value ()))));
          });
        if (submitted != zlink::submit_result_t::ok) {
            source->complete (
              detail::result_access_t::failure<runtime::messaging::message_parts_t> (
                runtime::messaging::map_submit_result_exception (
                  submitted, "SPOT mesh request was not submitted")));
            co_return co_await output;
        }
    }
    catch (const framework_exception_t &error) {
        source->complete (
          detail::result_access_t::failure<runtime::messaging::message_parts_t> (error));
    }
    catch (const zlink::request_error_t &error) {
        source->complete (detail::result_access_t::failure<runtime::messaging::message_parts_t> (
          runtime::messaging::map_request_result_exception (error.result (), error.what ())));
    }
    catch (const zlink::submit_error_t &error) {
        source->complete (detail::result_access_t::failure<runtime::messaging::message_parts_t> (
          runtime::messaging::map_submit_result_exception (error.result (), error.what ())));
    }
    catch (const std::exception &error) {
        source->complete (result_t<runtime::messaging::message_parts_t>::failure (
          framework_error_kind_t::internal_failure, error.what ()));
    }
    co_return co_await output;
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
            framework_error_kind_t::not_found, "SPOT mesh route requires a running native Spot"));
    }
    try {
        const auto target_node_rid = zlink::routing_id_t::from (std::string (node_rid.value ()));
        const auto target_generation =
          resolve_target_spot_generation (state->node, target_node_rid, spot_id);
        if (!target_generation) {
            return task_t<runtime::messaging::message_parts_t> (
              result_t<runtime::messaging::message_parts_t>::failure (
                framework_error_kind_t::not_found,
                "SPOT mesh request target generation is unavailable"));
        }
        return request_spot_parts_async (*native, target_node_rid, spot_id, *target_generation,
                                         std::move (parts), timeout);
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
    auto reply = request_spot_mesh_parts (state, std::move (node_rid), std::move (spot_id),
                                          std::move (parts), timeout);
    detail::observe_task_completion (
      reply, [source] (const result_t<runtime::messaging::message_parts_t> &reply_result) mutable {
          try {
              if (!reply_result) {
                  source->complete (detail::propagate_failure<zlink::message_t> (
                    reply_result, "SPOT mesh request failed"));
                  return;
              }
              runtime::messaging::envelope_codec_t envelope;
              auto reply_header = envelope.decode_header (reply_result.value (), false);
              if (!reply_header) {
                  source->complete (result_t<zlink::message_t>::failure (
                    reply_header.error_kind (), reply_header.error ()
                                                  ? reply_header.error ()->what ()
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
              source->complete (detail::result_access_t::failure<zlink::message_t> (error));
          }
          catch (const std::exception &error) {
              source->complete (result_t<zlink::message_t>::failure (
                framework_error_kind_t::internal_failure, error.what ()));
          }
          catch (...) {
              source->complete (result_t<zlink::message_t>::failure (
                framework_error_kind_t::internal_failure, "SPOT route reply processing failed"));
          }
      });
    return output;
}

} // namespace

namespace detail
{

bool spot_context_state_t::idle_quiescent () const
{
    auto owner = state_lane_owner ();
    if (!owner)
        return false;

    struct snapshot_t
    {
        bool state_quiescent = false;
        std::shared_ptr<runtime::serial_execution_queue_t> queue;
        std::vector<std::shared_ptr<timer_state_t>> timers;
    };
    auto snapshot = owner->lane.run ([this] {
        return snapshot_t{
          actor_count == 0 && !relocation_boundary_active && !relocation_ready_deferred
            && queued_routed_packets.empty (),
          serial_queue,
          timers};
    }).get ();
    if (!snapshot.state_quiescent || !snapshot.queue)
        return false;
    if (snapshot.queue->pending_count (runtime::serial_work_lane_t::application) != 0
        || snapshot.queue->pending_count (runtime::serial_work_lane_t::lifecycle) != 0) {
        return false;
    }
    for (const auto &timer : snapshot.timers) {
        if (!timer)
            continue;
        std::lock_guard<std::mutex> timer_lock (timer->mutex);
        if (!timer->disposed
            && (timer->running || timer->pending_fire || timer->pending_fire_count != 0)) {
            return false;
        }
    }
    return true;
}

bool spot_context_state_t::enter_callback ()
{
    return callback_lane.run ([this] {
        if (callback_admission_closed || idle_eviction_in_progress)
            return false;
        ++callback_depth;
        return true;
    }).get ();
}

spot_context_state_t::timer_fire_state_snapshot_t
spot_context_state_t::enter_timer_callback ()
{
    return callback_lane.run ([this] {
        timer_fire_state_snapshot_t result;
        result.configured = spot_instance && channel_runtime && channel_runtime->serializers;
        if (!result.configured || callback_admission_closed || idle_eviction_in_progress)
            return result;
        ++callback_depth;
        result.spot_instance = spot_instance;
        result.channel_runtime = channel_runtime;
        result.admitted = true;
        return result;
    }).get ();
}

void spot_context_state_t::leave_callback () noexcept
{
    bool should_close = false;
    service::instance_spot_close_completion_t completion;
    std::tie (should_close, completion) = callback_lane.run ([this] {
        bool close = false;
        service::instance_spot_close_completion_t pending;
        if (callback_depth > 0) {
            --callback_depth;
        }
        if (callback_depth == 0) {
            close = close_requested;
            if (close)
                pending = std::move (pending_instance_spot_close_completion);
        }
        return std::make_pair (close, std::move (pending));
    }).get ();
    if (should_close) {
        bool local_closed = false;
        try {
            auto owner = state_lane_owner ();
            if (owner) {
                const auto token = owner->lane.run ([this, &owner] {
                    if (close_reservation == 0 || close_reservation_is_idle
                        || node.get () != owner.get () || closed || actor_count != 0) {
                        return std::uint64_t{0};
                    }
                    closed = true;
                    return close_reservation;
                }).get ();
                if (token != 0) {
                    callback_lane.run ([this] {
                        close_requested = false;
                        callback_admission_closed = true;
                    }).get ();
                    try {
                        close_application_then_release_location (
                          owner, spot_close_reason_t::explicit_close, token);
                    }
                    catch (...) {
                    }
                    local_closed = true;
                }
            }
        }
        catch (...) {
        }
        if (completion) {
            try {
                (void) completion (local_closed);
            }
            catch (...) {
            }
        }
    }
}

bool spot_context_state_t::is_current_callback_thread () const
{
    return current_callback_context == this;
}

bool spot_context_state_t::try_post_serial (std::string name,
                                            std::function<void ()> work,
                                            runtime::serial_work_options_t options)
{
    // Close and idle-eviction sealing cannot cross the queue admission point.
    auto queue = callback_lane.run ([this] {
        if (callback_admission_closed || idle_eviction_in_progress)
            return std::shared_ptr<runtime::serial_execution_queue_t>{};
        return serial_queue;
    }).get ();
    if (!queue) {
        if (admission_blocked ())
            return false;
        work ();
        return true;
    }
    return queue->try_post (std::move (name), std::move (work), std::move (options));
}

bool spot_context_state_t::try_post_serial_after_current_turn (
  std::string name, std::function<void ()> work, runtime::serial_work_options_t options)
{
    auto queue = callback_lane.run ([this] {
        if (callback_admission_closed || idle_eviction_in_progress)
            return std::shared_ptr<runtime::serial_execution_queue_t>{};
        return serial_queue;
    }).get ();
    if (!queue) {
        if (admission_blocked ())
            return false;
        work ();
        return true;
    }
    /* A handler may defer relocation readiness before its turn completes.
     * Work posted as a normal next turn then observes that readiness fence and
     * is rejected. Run lifecycle cleanup in the queue's after-active phase so
     * it executes after the borrowed handler reference is released but before
     * the normal next-turn continuation. */
    const auto current_turn = detail::capture_current_serial_turn ();
    if (owns_current_serial_turn () && current_turn && !current_turn->is_after_active_phase ()) {
        return queue->try_post_deferred (std::move (name), std::move (work));
    }
    return queue->try_post (std::move (name), std::move (work), std::move (options));
}

bool spot_context_state_t::try_post_serial_async (
  std::string name,
  runtime::serial_execution_queue_t::async_work_t work,
  runtime::serial_work_options_t options)
{
    auto queue = callback_lane.run ([this] {
        if (callback_admission_closed || idle_eviction_in_progress)
            return std::shared_ptr<runtime::serial_execution_queue_t>{};
        return serial_queue;
    }).get ();
    if (!queue) {
        if (admission_blocked ())
            return false;
        work ([] (std::function<void ()> completion) {
            if (completion) {
                completion ();
            }
        });
        return true;
    }
    return queue->try_post_async (std::move (name), std::move (work), std::move (options));
}

void spot_context_state_t::run_serial_task_async (
  std::string name,
  std::function<task_t<void> ()> work,
  std::function<void (result_t<void>)> completion,
  std::function<void (const std::shared_ptr<runtime::serial_execution_queue_t> &,
                      runtime::serial_submission_id_t)> submission_callback,
  std::function<void ()> activation_callback,
  std::function<void (bool)> cancellation_observed)
{
    if (!completion)
        return;
    if (!work) {
        completion (result_t<void>::success ());
        return;
    }

    const auto queue = callback_lane.run ([this] {
        if (callback_admission_closed || idle_eviction_in_progress)
            return std::shared_ptr<runtime::serial_execution_queue_t>{};
        return serial_queue;
    }).get ();
    if (!queue && admission_blocked ()) {
        completion (result_t<void>::failure (framework_error_kind_t::unavailable,
                                             "spot is closing for idle eviction"));
        return;
    }
    const auto current_turn = detail::capture_current_serial_turn ();
    const bool released_current_turn =
      queue && current_turn && current_turn->released ()
      && current_turn->belongs_to (queue.get ());
    const bool run_inline =
      !queue
      || (!released_current_turn
          && (is_current_callback_thread () || owns_current_serial_turn ()));
    if (run_inline) {
        try {
            if (activation_callback)
                activation_callback ();
            callback_context_scope_t callback_scope (this);
            auto observed = std::make_shared<task_t<void>> (work ());
            detail::observe_task_completion (*observed, [observed,
                                                         completion = std::move (completion)] (
                                                          const result_t<void> &value) mutable {
                completion (value ? result_t<void>::success ()
                                  : result_t<void>::failure (value.error_kind (),
                                                             value.error () != nullptr
                                                               ? value.error ()->what ()
                                                               : "spot lifecycle callback failed"));
            });
        }
        catch (const framework_exception_t &error) {
            completion (detail::result_access_t::failure<void> (error));
        }
        catch (const std::exception &error) {
            completion (
              result_t<void>::failure (framework_error_kind_t::internal_failure, error.what ()));
        }
        catch (...) {
            completion (result_t<void>::failure (framework_error_kind_t::internal_failure,
                                                 "spot lifecycle callback failed"));
        }
        return;
    }

    struct async_state_t
    {
        std::mutex mutex;
        bool started = false;
        bool cancellation_requested = false;
        bool settled = false;
        std::function<void (result_t<void>)> completion;
    };
    auto state = std::make_shared<async_state_t> ();
    state->completion = std::move (completion);
    auto settle = [state] (result_t<void> value) mutable {
        std::function<void (result_t<void>)> callback;
        {
            std::lock_guard lock (state->mutex);
            if (state->settled)
                return;
            state->settled = true;
            if (state->cancellation_requested) {
                value = result_t<void>::failure (
                  framework_error_kind_t::shutting_down,
                  "spot serial lifecycle task was cancelled during shutdown");
            }
            callback = std::move (state->completion);
        }
        if (callback)
            callback (std::move (value));
    };
    auto request_cancel = [state, settle,
                           cancellation_observed = std::move (cancellation_observed)] () mutable {
        bool settle_queued = false;
        {
            std::lock_guard lock (state->mutex);
            if (state->settled)
                return;
            state->cancellation_requested = true;
            settle_queued = !state->started;
            if (cancellation_observed)
                cancellation_observed (state->started);
        }
        if (settle_queued) {
            settle (result_t<void>::failure (
              framework_error_kind_t::shutting_down,
              "spot serial lifecycle task was cancelled before activation"));
        }
    };
    const std::weak_ptr<spot_context_state_t> weak_owner = weak_from_this ();
    const auto submitted = queue->try_post_cancellable_async (
      std::move (name),
      [weak_owner, work = std::move (work), state, settle,
       activation_callback = std::move (activation_callback)] (auto complete) mutable {
          bool cancelled_before_start = false;
          {
              std::lock_guard lock (state->mutex);
              cancelled_before_start = state->settled || state->cancellation_requested;
              if (!cancelled_before_start)
                  state->started = true;
          }
          if (cancelled_before_start) {
              complete ([] {});
              return;
          }
          if (activation_callback)
              activation_callback ();
          auto owner = weak_owner.lock ();
          if (!owner) {
              complete ([settle] () mutable {
                  settle (result_t<void>::failure (
                    framework_error_kind_t::shutting_down,
                    "spot lifecycle owner was released before activation"));
              });
              return;
          }
          if (!owner->enter_callback ()) {
              complete ([settle] () mutable {
                  settle (detail::boundary_failure<void> (detail::boundary_error_t::closed,
                                                          "spot activation is closed"));
              });
              return;
          }
          auto turn = detail::capture_current_serial_turn ();
          try {
              callback_context_scope_t callback_scope (owner.get ());
              auto observed = std::make_shared<task_t<void>> (work ());
              detail::observe_task_completion (
                *observed,
                [owner, observed, settle, turn, complete] (const result_t<void> &value) mutable {
                    const auto final_result =
                      value ? result_t<void>::success ()
                            : result_t<void>::failure (value.error_kind (),
                                                       value.error () != nullptr
                                                         ? value.error ()->what ()
                                                         : "spot lifecycle callback failed");
                    auto finish = [owner, settle, final_result] () mutable {
                        owner->leave_callback ();
                        settle (final_result);
                    };
                    if (turn && turn->released ()) {
                        finish ();
                    } else {
                        complete (std::move (finish));
                    }
                });
          }
          catch (const framework_exception_t &error) {
              complete ([owner, settle, error] () mutable {
                  owner->leave_callback ();
                  settle (detail::result_access_t::failure<void> (error));
              });
          }
          catch (const std::exception &error) {
              const auto message = std::string (error.what ());
              complete ([owner, settle, message] () mutable {
                  owner->leave_callback ();
                  settle (
                    result_t<void>::failure (framework_error_kind_t::internal_failure, message));
              });
          }
          catch (...) {
              complete ([owner, settle] () mutable {
                  owner->leave_callback ();
                  settle (result_t<void>::failure (framework_error_kind_t::internal_failure,
                                                   "spot lifecycle callback failed"));
              });
          }
      },
      std::move (request_cancel),
      runtime::serial_work_options_t{runtime::serial_work_lane_t::lifecycle,
                                     runtime::serial_execution_queue_t::fixed_work_byte_cost});
    if (submitted && submission_callback)
        submission_callback (queue, submitted.value ());
    if (!submitted) {
        settle (result_t<void>::failure (submitted.error_kind (), submitted.error () != nullptr
                                                                    ? submitted.error ()->what ()
                                                                    : "spot serial queue is full"));
    }
}

result_t<void> spot_context_state_t::run_serial_task (std::string name,
                                                      std::function<task_t<void> ()> work)
{
    detail::task_completion_source_t<void> completion;
    auto result = completion.task ();
    run_serial_task_async (
      std::move (name), std::move (work),
      [completion] (result_t<void> value) mutable { completion.complete (std::move (value)); });
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
        callback_context_scope_t callback_scope (this);
        work ();
        return true;
    }

    std::exception_ptr error;
    bool callback_admitted = false;
    const bool posted = try_post_serial (
      std::move (name),
      [&] {
          callback_admitted = enter_callback ();
          if (!callback_admitted) {
              return;
          }
          try {
              callback_context_scope_t callback_scope (this);
              work ();
          }
          catch (...) {
              error = std::current_exception ();
          }
          leave_callback ();
      },
      runtime::serial_work_options_t{runtime::serial_work_lane_t::lifecycle,
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
    const auto queue = serial_queue;
    return turn && queue && turn->belongs_to (queue.get ());
}

void spot_context_state_t::defer_relocation_ready ()
{
    if (execution_mode != user_spot_execution_mode_t::spot_wide
        || relocation_coordination_mode != spot_relocation_coordination_mode_t::application_signaled
        || is_entry_spot () || is_instance_spot () || !owns_current_serial_turn ()) {
        throw framework_exception_t (framework_error_kind_t::not_configured,
                                     "relocation readiness can be deferred only from an "
                                     "application-signaled SpotWide User Spot turn");
    }
    bool complete_without_relocation = false;
    complete_without_relocation = callback_lane.run ([this] {
        if (relocation_ready_deferred) {
            throw framework_exception_t (framework_error_kind_t::not_configured,
                                         "relocation readiness is already deferred");
        }
        relocation_ready_deferred = true;
        return !relocation_boundary_active;
    }).get ();
    if (complete_without_relocation
        && !try_post_serial (
          "relocation-ready-continued",
          [this] { complete_relocation_ready (spot_relocation_ready_outcome_t::continued); },
          runtime::serial_work_options_t{
            runtime::serial_work_lane_t::lifecycle,
            runtime::serial_execution_queue_t::fixed_work_byte_cost})) {
        callback_lane.run ([this] { relocation_ready_deferred = false; }).get ();
        throw framework_exception_t (framework_error_kind_t::capacity_exceeded,
                                     "relocation readiness completion queue is full");
    }
}

void spot_context_state_t::ensure_relocation_turn_open () const
{
    const auto current_turn = detail::capture_current_serial_turn ();
    if (!owns_current_serial_turn () || !current_turn || current_turn->is_after_active_phase ())
        return;
    const auto deferred = callback_lane.run ([this] {
        return relocation_ready_deferred;
    }).get ();
    if (deferred) {
        throw framework_exception_t (framework_error_kind_t::not_configured,
                                     "Framework operations are not allowed after relocation "
                                     "readiness is deferred in the current Spot turn");
    }
}

void spot_context_state_t::complete_relocation_ready (spot_relocation_ready_outcome_t outcome)
{
    const auto pending = callback_lane.run ([this] {
        return relocation_ready_deferred;
    }).get ();
    if (!pending)
        return;
    (void) run_serial_sync ("relocation-ready-completed", [this, outcome] {
        std::shared_ptr<void> instance;
        std::function<void (void *, const spot_relocation_ready_completion_t &)> callback;
        std::tie (instance, callback) = callback_lane.run ([this] {
            std::shared_ptr<void> current_instance;
            std::function<void (void *, const spot_relocation_ready_completion_t &)> current_callback;
            if (!relocation_ready_deferred)
                return std::make_pair (std::move (current_instance), std::move (current_callback));
            relocation_ready_deferred = false;
            current_instance = spot_instance;
            current_callback = lifecycle.on_relocation_ready_completed;
            return std::make_pair (std::move (current_instance), std::move (current_callback));
        }).get ();
        if (instance && callback) {
            callback (instance.get (), spot_relocation_ready_completion_t{outcome});
        }
    });
}

void spot_context_state_t::drain_serial ()
{
    if (const auto queue = serial_queue) {
        queue->drain ();
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
    _packet_name (std::move (packet_name)),
    _serializers (serializers),
    _submit (std::move (submit)),
    _preflight (std::move (preflight))
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
        throw framework_exception_t (framework_error_kind_t::not_configured,
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

route_client_t spot_context_t::spot_route_client () const
{
    if (!_state || !_state->node) {
        return route_client_t ();
    }
    const auto route_client = _state->node->route_client_lane
                                .run ([this] { return _state->node->route_client; })
                                .get ();
    return route_client.value_or (route_client_t ());
}

channel_client_t spot_context_t::outbound () const
{
    if (!_state->channel_runtime) {
        throw framework_exception_t (framework_error_kind_t::internal_failure,
                                     "SPOT channel outbound runtime is not configured");
    }
    auto state = std::weak_ptr<detail::spot_context_state_t> (_state);
    return channel_client_t (message_bus_t (_state->channel_runtime, [state] {
        const auto locked = state.lock ();
        if (!locked)
            return result_t<void>::failure (framework_error_kind_t::invalid_operation,
                                            "Spot execution context is no longer available");
        try {
            locked->ensure_relocation_turn_open ();
            return result_t<void>::success ();
        }
        catch (const framework_exception_t &error) {
            return detail::result_access_t::failure<void> (error);
        }
    }));
}

void spot_context_t::ensure_submission_open () const
{
    if (!_state) {
        throw framework_exception_t (framework_error_kind_t::not_configured,
                                     "Spot execution context is not configured");
    }
    _state->ensure_relocation_turn_open ();
}

std::function<result_t<void> ()> spot_context_t::submission_preflight () const
{
    auto state = std::weak_ptr<detail::spot_context_state_t> (_state);
    return [state] {
        const auto locked = state.lock ();
        if (!locked) {
            return result_t<void>::failure (framework_error_kind_t::not_configured,
                                            "Spot execution context is no longer available");
        }
        try {
            locked->ensure_relocation_turn_open ();
            return result_t<void>::success ();
        }
        catch (const framework_exception_t &error) {
            return detail::result_access_t::failure<void> (error);
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
    co_return result_t<bool>::success (_state->close_now ());
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
    report_spot_dispatch_trace (_state ? _state->node : nullptr, message_flow_outcome_t::received,
                                dispatch_error_surface_t::spot_actor,
                                dispatch_message_kind_t::actor_request, "actor_leave", {},
                                _state ? _state->spot_id : std::string_view{},
                                ::zlink::framework::detail::actor_ref_access_t::empty (actor_ref)
                                  ? std::string_view{}
                                  : actor_ref.actor_id ().value ());
    ensure_submission_open ();
    if (!_state || !_state->node
        || ::zlink::framework::detail::actor_ref_access_t::empty (actor_ref)) {
        return task_t<actor_ref_t> (
          result_t<actor_ref_t>::failure (framework_error_kind_t::not_found, "actor ref is empty"));
    }
    if (_state->is_current_callback_thread ()) {
        report_spot_dispatch_trace (_state->node, message_flow_outcome_t::dispatched,
                                    dispatch_error_surface_t::spot_actor,
                                    dispatch_message_kind_t::actor_request, "actor_leave_deferred",
                                    {}, _state->spot_id, actor_ref.actor_id ().value ());
        auto state = _state;
        const auto deferred_ref = actor_ref;
        const auto posted = state->try_post_serial_after_current_turn (
          "spot-actor-leave-after-handler",
          [state, deferred_ref, actor_type, actor,
           update_actor_ref = std::move (update_actor_ref)] () mutable {
              report_spot_dispatch_trace (state->node, message_flow_outcome_t::dispatched,
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
                        report_spot_dispatch_trace (state->node, message_flow_outcome_t::replied,
                                                    dispatch_error_surface_t::spot_actor,
                                                    dispatch_message_kind_t::actor_request,
                                                    completed ? "actor_leave_deferred_complete"
                                                              : "actor_leave_deferred_failed",
                                                    {}, state->spot_id,
                                                    deferred_ref.actor_id ().value (), {},
                                                    completed ? message_flow_result_t::succeeded
                                                              : message_flow_result_t::failed);
                    }
                    catch (...) {
                        report_spot_dispatch_trace (
                          state->node, message_flow_outcome_t::replied,
                          dispatch_error_surface_t::spot_actor,
                          dispatch_message_kind_t::actor_request, "actor_leave_deferred_exception",
                          {}, state->spot_id, deferred_ref.actor_id ().value (), {},
                          message_flow_result_t::failed);
                    }
                });
              if (!submitted) {
                  report_spot_dispatch_trace (
                    state->node, message_flow_outcome_t::backpressured,
                    dispatch_error_surface_t::spot_actor, dispatch_message_kind_t::actor_request,
                    "actor_leave_deferred_rejected", {}, state->spot_id,
                    deferred_ref.actor_id ().value (), {}, message_flow_result_t::backpressured,
                    message_flow_reason_t::backpressure);
              }
          },
          runtime::serial_work_options_t{runtime::serial_work_lane_t::lifecycle,
                                         runtime::serial_execution_queue_t::fixed_work_byte_cost});
        if (!posted) {
            return task_t<actor_ref_t> (result_t<actor_ref_t>::failure (
              framework_error_kind_t::capacity_exceeded,
              "Actor leave could not be scheduled after the current handler"));
        }
        return task_t<actor_ref_t> (result_t<actor_ref_t>::success (actor_ref));
    }
    /* Lifecycle callbacks may close the source Spot. Keep both the node and
     * each selected callback target alive after its state-lane turn ends. */
    auto node = _state->node;
    const auto stable_actor_type =
      std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor_ref));
    const auto key = stable_actor_type + ":" + std::string (actor_ref.actor_id ().value ());

    using entry_join_callback_t =
      std::function<result_t<detail::actor_join_reply_t> (
        const actor_ref_t &,
        node_rid_t,
        const zlink::message_t &,
        const std::optional<zlink::message_t> &)>;
    struct leave_plan_t
    {
        enum class destination_t
        {
            no_op,
            remote_entry,
            local_entry
        };

        destination_t destination = destination_t::no_op;
        std::shared_ptr<detail::spot_context_state_t> source_state;
        std::shared_ptr<detail::spot_context_state_t> entry_state;
        std::shared_ptr<void> source_spot_instance;
        std::function<task_t<void> (void *, void *)> source_leave;
        entry_join_callback_t entry_join;
    };

    auto planned = node->lane.run ([&] () -> result_t<leave_plan_t> {
        const auto found_location = node->actor_spot_ids.find (key);
        if (found_location == node->actor_spot_ids.end ())
            return result_t<leave_plan_t>::success (leave_plan_t{});
        if (found_location->second != _state->spot_id) {
            return result_t<leave_plan_t>::failure (framework_error_kind_t::not_found,
                                                    "actor is not joined to this SPOT");
        }

        const auto found_generation = node->actor_generations.find (key);
        if (found_generation != node->actor_generations.end ()
            && found_generation->second != actor_ref.object_generation ()) {
            return detail::boundary_failure<leave_plan_t> (
              detail::boundary_error_t::stale_generation, "actor generation is stale");
        }

        leave_plan_t plan;
        plan.source_state = _state;
        const bool remote_entry =
          _state->node_rid.empty ()
          || actor_ref.node_rid ().value () != _state->node_rid.value ();
        if (remote_entry) {
            plan.destination = leave_plan_t::destination_t::remote_entry;
            plan.entry_join = node->actor_entry_spot_join;
        } else {
            if (!node->snapshot.entry_spot_name) {
                return result_t<leave_plan_t>::failure (framework_error_kind_t::not_found,
                                                        "entry spot is not registered");
            }
            const auto entry_id = node->spot_ids_by_name.find (*node->snapshot.entry_spot_name);
            if (entry_id == node->spot_ids_by_name.end ()) {
                return result_t<leave_plan_t>::failure (framework_error_kind_t::not_found,
                                                        "entry spot is not created");
            }
            const auto entry_context = node->spot_contexts_by_id.find (entry_id->second);
            if (entry_context == node->spot_contexts_by_id.end ()
                || !entry_context->second._state
                || !entry_context->second._state->spot_instance) {
                return result_t<leave_plan_t>::failure (
                  framework_error_kind_t::not_found, "entry spot context is not registered");
            }
            plan.destination = leave_plan_t::destination_t::local_entry;
            plan.entry_state = entry_context->second._state;
        }

        auto &source_state = *_state;
        decrement_actor_count_unlocked (source_state);
        erase_actor_route_unlocked (*node, key);
        const auto source_admission = source_state.actor_admissions.find (actor_type);
        if (source_admission != source_state.actor_admissions.end ()
            && source_admission->second.on_leave_actor && source_state.spot_instance) {
            plan.source_leave = source_admission->second.on_leave_actor;
            plan.source_spot_instance = source_state.spot_instance;
        } else if (!remote_entry) {
            const auto source_left = source_state.on_leave_actor_callbacks.find (actor_type);
            if (source_left != source_state.on_leave_actor_callbacks.end ()
                && source_state.spot_instance) {
                plan.source_leave = source_left->second;
                plan.source_spot_instance = source_state.spot_instance;
            }
        }
        return result_t<leave_plan_t>::success (std::move (plan));
    }).get ();
    if (!planned) {
        return task_t<actor_ref_t> (
          detail::propagate_failure<actor_ref_t> (planned, "actor leave state admission failed"));
    }
    auto plan = std::move (planned.value ());
    if (plan.destination == leave_plan_t::destination_t::no_op) {
        report_spot_dispatch_trace (node, message_flow_outcome_t::replied,
                                    dispatch_error_surface_t::spot_actor,
                                    dispatch_message_kind_t::actor_request, "actor_leave_noop", {},
                                    _state->spot_id, actor_ref.actor_id ().value ());
        return task_t<actor_ref_t> (result_t<actor_ref_t>::success (actor_ref));
    }

    const auto run_source_leave = [&] {
        if (!plan.source_leave)
            return result_t<void>::success ();
        return plan.source_state->run_serial_task ("spot-lifecycle-leave", [&] {
            return plan.source_leave (plan.source_spot_instance.get (), actor);
        });
    };

    if (plan.destination == leave_plan_t::destination_t::remote_entry) {
        try {
            const auto completed = run_source_leave ();
            if (!completed) {
                return task_t<actor_ref_t> (result_t<actor_ref_t>::failure (
                  completed.error_kind (), completed.error () != nullptr
                                             ? completed.error ()->what ()
                                             : "spot actor leave callback failed"));
            }
            if (!plan.entry_join)
                return task_t<actor_ref_t> (result_t<actor_ref_t>::success (actor_ref));

            struct snapshot_plan_t
            {
                std::function<std::optional<zlink::message_t> (void *, serializer_registry_t &)>
                  serialize;
                std::shared_ptr<detail::channel_runtime_state_t> channel_runtime;
                serializer_registry_t *serializers = nullptr;
            };
            const auto snapshot_plan = node->lane.run ([&] {
                snapshot_plan_t selected;
                const auto actor_factory = node->actor_factories.find (stable_actor_type);
                if (actor_factory != node->actor_factories.end ()
                    && plan.source_state->channel_runtime
                    && plan.source_state->channel_runtime->serializers) {
                    selected.serialize = actor_factory->second.serialize_instance;
                    selected.channel_runtime = plan.source_state->channel_runtime;
                    selected.serializers = selected.channel_runtime->serializers;
                }
                return selected;
            }).get ();
            std::optional<zlink::message_t> actor_snapshot;
            if (snapshot_plan.serializers) {
                actor_snapshot = snapshot_plan.serialize (actor, *snapshot_plan.serializers);
            }
            auto joined = plan.entry_join (actor_ref, actor_ref.node_rid (), zlink::message_t{},
                                           actor_snapshot);
            if (!joined) {
                const auto *error = joined.error ();
                return task_t<actor_ref_t> (result_t<actor_ref_t>::failure (
                  joined.error_kind (),
                  error != nullptr ? error->what () : "remote entry spot join failed"));
            }
            if (update_actor_ref)
                update_actor_ref (actor, joined.value ().actor);
            return task_t<actor_ref_t> (result_t<actor_ref_t>::success (joined.value ().actor));
        }
        catch (const framework_exception_t &error) {
            return task_t<actor_ref_t> (detail::result_access_t::failure<actor_ref_t> (error));
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

    try {
        const auto completed = run_source_leave ();
        if (!completed) {
            return task_t<actor_ref_t> (result_t<actor_ref_t>::failure (
              completed.error_kind (), completed.error () != nullptr
                                         ? completed.error ()->what ()
                                         : "spot actor leave callback failed"));
        }

        struct location_update_plan_t
        {
            actor_ref_t committed;
            runtime::location_lifecycle_t *lifecycle = nullptr;
            std::shared_ptr<service::mesh_node_t> native_node;
            std::shared_ptr<service::spot_t> native_spot;
            std::string mesh_name;
            node_rid_t node_rid;
            spot_id_t spot_id;
            std::uint64_t membership_epoch = 0;
            bool context_attached = false;
        };
        auto location_plan = node->lane.run ([&] {
            location_update_plan_t selected{
              .committed = ::zlink::framework::detail::actor_ref_access_t::make (
                node_rid_t::from_string (std::string (_state->node_rid.value ())),
                stable_actor_type, std::string (actor_ref.actor_id ().value ()),
                actor_ref.object_generation ())};
            selected.lifecycle = node->location_lifecycle;
            selected.context_attached = plan.entry_state->node != nullptr;
            if (selected.lifecycle && selected.context_attached) {
                selected.native_node = node->native_node.lock ();
                selected.native_spot = plan.entry_state->native_spot.lock ();
                selected.mesh_name = node->snapshot.name;
                selected.node_rid = plan.entry_state->node_rid;
                selected.spot_id = plan.entry_state->spot_id;
                const auto membership = node->core_actor_membership_epochs.find (
                  std::string (actor_ref.actor_id ().value ()));
                if (membership != node->core_actor_membership_epochs.end ())
                    selected.membership_epoch = membership->second;
            }
            return selected;
        }).get ();

        if (location_plan.lifecycle && location_plan.context_attached) {
            const auto owner_generation = location_plan.native_node
                                            ? location_plan.native_node->status ().lifecycle_generation ()
                                            : 0;
            const auto spot_generation_value =
              location_plan.native_spot
                ? location_plan.native_spot->status ().lifecycle_generation ()
                : 0;
            auto location = actor_location_t{
              .mesh_name = location_plan.mesh_name,
              .actor_id = std::string (location_plan.committed.actor_id ().value ()),
              .actor_type = stable_actor_type,
              .actor_ref = location_plan.committed,
              .owner_node_rid =
                zlink::routing_id_t::from (std::string (location_plan.node_rid.value ())),
              .owner_node_generation = owner_generation,
              .spot_id = location_plan.spot_id,
              .spot_generation = spot_generation_value,
              .spot_kind = zlink::spot_kind::entry,
              .membership_epoch = location_plan.membership_epoch};
            (void) location_plan.lifecycle->owns_actor (actor_location_key_t{
              location.mesh_name, std::string (location_plan.committed.actor_id ().value ())});
            (void) location_plan.lifecycle->update_actor_location (std::move (location));
        }

        auto update_registry = node->lane.run ([&] {
            detail::record_actor_context_route_unlocked (
              *node, key, std::string (_state->node_rid.value ()), *plan.entry_state,
              location_plan.committed.object_generation ());
            return node->update_actor_registry_ref;
        }).get ();
        if (update_actor_ref)
            update_actor_ref (actor, location_plan.committed);
        if (update_registry) {
            auto updated = update_registry (location_plan.committed);
            if (!updated) {
                const auto *error = updated.error ();
                return task_t<actor_ref_t> (result_t<actor_ref_t>::failure (
                  updated.error_kind (),
                  error != nullptr ? error->what () : "actor registry ref update failed"));
            }
        }

        struct joined_callback_plan_t
        {
            std::function<task_t<void> (void *, void *)> callback;
            std::shared_ptr<void> spot_instance;
        };
        const auto joined_plan = node->lane.run ([&] {
            joined_callback_plan_t selected;
            const auto entry_admission = plan.entry_state->actor_admissions.find (actor_type);
            if (entry_admission != plan.entry_state->actor_admissions.end ()
                && entry_admission->second.on_actor_joined && plan.entry_state->spot_instance) {
                selected.callback = entry_admission->second.on_actor_joined;
                selected.spot_instance = plan.entry_state->spot_instance;
            } else {
                const auto entry_joined =
                  plan.entry_state->on_actor_joined_callbacks.find (actor_type);
                if (entry_joined != plan.entry_state->on_actor_joined_callbacks.end ()
                    && plan.entry_state->spot_instance) {
                    selected.callback = entry_joined->second;
                    selected.spot_instance = plan.entry_state->spot_instance;
                }
            }
            return selected;
        }).get ();
        if (joined_plan.callback) {
            const auto joined = plan.entry_state->run_serial_task ("spot-lifecycle-join", [&] {
                return joined_plan.callback (joined_plan.spot_instance.get (), actor);
            });
            if (!joined) {
                return task_t<actor_ref_t> (result_t<actor_ref_t>::failure (
                  joined.error_kind (), joined.error () != nullptr
                                           ? joined.error ()->what ()
                                           : "spot actor joined callback failed"));
            }
        }
        return task_t<actor_ref_t> (
          result_t<actor_ref_t>::success (location_plan.committed));
    }
    catch (const framework_exception_t &error) {
        return task_t<actor_ref_t> (detail::result_access_t::failure<actor_ref_t> (error));
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
    /* Resolve one immutable ref in a state turn, then enter the public destroy
     * surface after the turn has ended. */
    const auto actor = _state->node->lane.run ([&] () -> std::optional<actor_ref_t> {
        const auto found = _state->node->actor_instance_index.find (instance);
        if (found == _state->node->actor_instance_index.end ()) {
            return std::nullopt;
        }
        const auto key = found->second.first + ":" + found->second.second;
        const auto found_generation = _state->node->actor_generations.find (key);
        return ::zlink::framework::detail::actor_ref_access_t::make (
          _state->node_rid, found->second.first, found->second.second,
          found_generation != _state->node->actor_generations.end () ? found_generation->second
                                                                     : 1);
    }).get ();
    if (!actor) {
        /* Instance not registered on this node: already destroyed or
         * superseded — duplicate destroy is a successful no-op. */
        return task_t<void> (result_t<void>::success ());
    }
    return destroy_actor_erased (*actor);
}

task_t<void> entry_spot_context_t::destroy_actor_erased (const actor_ref_t &actor)
{
    if (!_state || !_state->node || ::zlink::framework::detail::actor_ref_access_t::empty (actor)) {
        return task_t<void> (
          result_t<void>::failure (framework_error_kind_t::not_found, "actor ref is empty"));
    }
    /* A lifecycle callback receives a reference to the actor instance. Erasing
     * actor_instances from that callback would destroy the object before the
     * callback (and its coroutine frame) has returned. Queue the destructive
     * part behind the current serial turn so the borrowed reference remains
     * valid for the complete callback. */
    if (_state->is_current_callback_thread ()) {
        auto state = _state;
        const auto deferred_actor = actor;
        const auto key =
          std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor)) + ":"
          + std::string (actor.actor_id ().value ());
        const auto accepted = state->node->lane.run ([&] {
            const auto found_location = state->node->actor_spot_ids.find (key);
            const auto found_generation = state->node->actor_generations.find (key);
            if (found_location == state->node->actor_spot_ids.end ()
                || (found_generation != state->node->actor_generations.end ()
                    && found_generation->second != actor.object_generation ())) {
                return false;
            }
            state->node->retiring_actor_keys.insert (key);
            return true;
        }).get ();
        if (!accepted)
            return task_t<void> (result_t<void>::success ());
        const auto posted = state->try_post_serial_after_current_turn (
          "entry-spot-actor-destroy-after-handler",
          [state, deferred_actor, key] {
              (void) entry_spot_context_t (state).destroy_actor_erased (deferred_actor).result ();
              state->node->lane.run ([&] { state->node->retiring_actor_keys.erase (key); }).get ();
          },
          runtime::serial_work_options_t{runtime::serial_work_lane_t::lifecycle,
                                         runtime::serial_execution_queue_t::fixed_work_byte_cost});
        if (!posted) {
            state->node->lane.run ([&] { state->node->retiring_actor_keys.erase (key); }).get ();
            return task_t<void> (result_t<void>::failure (
              framework_error_kind_t::capacity_exceeded,
              "Actor destroy could not be scheduled after the current handler"));
        }
        return task_t<void> (result_t<void>::success ());
    }
    std::function<result_t<void> (const actor_ref_t &)> destroy_registry;
    const auto key =
      std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor)) + ":"
      + std::string (actor.actor_id ().value ());
    const auto selected = _state->node->lane.run ([&] {
        if (actor.node_rid ().empty () || actor.node_rid ().value () != _state->node_rid.value ()) {
            return result_t<bool>::failure (framework_error_kind_t::not_found,
                                            "actor is not owned by this Entry SPOT");
        }

        const auto found_location = _state->node->actor_spot_ids.find (key);
        if (found_location != _state->node->actor_spot_ids.end ()
            && found_location->second != _state->spot_id) {
            return result_t<bool>::failure (framework_error_kind_t::not_found,
                                            "actor must leave its current SPOT before destroy");
        }

        const auto found_generation = _state->node->actor_generations.find (key);
        if (found_generation != _state->node->actor_generations.end ()
            && found_generation->second != actor.object_generation ()) {
            return result_t<bool>::success (false);
        }
        if (_state->node->destroying_actors.contains (key)) {
            return result_t<bool>::success (false);
        }
        // A destroy scheduled for the retained entry-spot record must not
        // race a transfer that is re-admitting the same incarnation on this
        // node (A→B→A): the transfer commit owns the instance lifecycle.
        if (_state->node->actor_transfer_coordinator.blocks_dispatch (key)) {
            return result_t<bool>::failure (framework_error_kind_t::unavailable,
                                            "Actor transfer is in progress");
        }

        const auto destroys_local_actor = found_location != _state->node->actor_spot_ids.end ();
        if (destroys_local_actor) {
            _state->node->destroying_actors.insert (key);
            destroy_registry = _state->node->destroy_actor_registry;
        }
        return result_t<bool>::success (destroys_local_actor);
    }).get ();
    if (!selected) {
        return task_t<void> (result_t<void>::failure (
          selected.error_kind (), selected.error () != nullptr ? selected.error ()->what ()
                                                               : "actor destroy failed"));
    }
    if (!selected.value ())
        return task_t<void> (result_t<void>::success ());

    release_actor_location (_state->node, actor);
    _state->node->lane.run ([&] {
        if (_state->node->destroying_actors.contains (key)) {
            erase_actor_route_unlocked (*_state->node, key);
            _state->node->actor_created_keys.erase (key);
            _state->node->destroyed_actor_keys.insert (key);
            _state->node->actor_instances.erase (key);
            detail::erase_actor_instance_index_unlocked (
              *_state->node, ::zlink::framework::detail::actor_ref_access_t::actor_type (actor),
              actor.actor_id ().value ());
            (void) _state->node->dispatched_request_replies.erase_if (
              [&] (const auto &request_key) {
                  return request_key.starts_with (actor_request_dedup_prefix (key));
              });
            decrement_actor_count_unlocked (*_state);
        }
    }).get ();

    if (destroy_registry) {
        auto cleanup = destroy_registry (actor);
        _state->node->lane.run ([&] { _state->node->destroying_actors.erase (key); }).get ();
        if (!cleanup) {
            const auto *error = cleanup.error ();
            return task_t<void> (result_t<void>::failure (
              cleanup.error_kind (),
              error != nullptr ? error->what () : "actor registry cleanup failed"));
        }
    } else {
        _state->node->lane.run ([&] { _state->node->destroying_actors.erase (key); }).get ();
    }

    return task_t<void> (result_t<void>::success ());
}

namespace
{
//  Coroutine parameters are copied into the frame; publish_tail borrows its
//  parts/metadata for the whole suspended fanout, so the owning copies must
//  live in this frame rather than in the caller's synchronous scope.
task_t<void> run_spot_publish_fanout (std::shared_ptr<service::spot_t> native,
                                      std::vector<zlink::message_t> parts,
                                      std::vector<std::uint8_t> metadata)
{
    co_await native->publish_tail (parts, metadata);
    co_return;
}
} // namespace

send_call_t spot_context_t::publish_erased (std::string topic,
                                            std::string packet_name,
                                            std::string content_type,
                                            zlink::message_t payload)
{
    auto state = _state;
    return send_call_t (std::move (packet_name), [state, topic = std::move (topic),
                                                  content_type = std::move (content_type),
                                                  payload = std::move (payload)] (
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
            return detail::result_access_t::failure<void> (error);
        }
        struct publish_projection_t
        {
            std::shared_ptr<service::spot_t> native;
            dispatch_options_t dispatch;
            std::string mesh_name;
            std::string discovery_channel_name;
            std::shared_ptr<detail::channel_runtime_state_t> channel_runtime;
        };
        const auto projection = state->node
                                  ? state->node->lane.run ([&] {
                                        return publish_projection_t{
                                          .native = state->native_spot.lock (),
                                          .dispatch = state->node->dispatch,
                                          .mesh_name = state->node->snapshot.name,
                                          .discovery_channel_name =
                                            state->node->snapshot.discovery_channel_name.value_or (
                                              state->node->snapshot.name),
                                          .channel_runtime = state->node->channel_runtime};
                                    }).get ()
                                  : publish_projection_t{};
        auto native = projection.native;
        if (native) {
            try {
                /* Fan-out wire envelope (flow-correlation §4.1, .NET
                   * ZLinkSpotPublishEnvelope 동형): the header carries the
                   * ambient flow pair so every subscriber line shares one
                   * flow id across the tree. */
                const auto diagnostics_mode =
                  detail::message_flow_tracer_t (projection.dispatch).mode ();
                auto flow_scope = runtime::flow_context_t::enter_current_or_create (
                  flow_origin_t::application, diagnostics_mode);
                /* Self-delimited single frame: ['Z''L''F''E'][u32 BE
                   * header_len][header JSON][body]. The node-attached fanout
                   * path does not keep multipart boundaries end to end, so
                   * the envelope frames itself; the decode side also accepts
                   * a true two-part frame from peers whose wire preserves
                   * parts. The magic makes the format discriminable from a
                   * legacy raw payload, so a validation failure after a
                   * magic match is definitively a corrupted framework frame. */
                auto frame_part = encode_spot_publish_frame (
                  projection.mesh_name, submitted_packet_name, topic, content_type, payload);
                if (!projection.channel_runtime || !projection.channel_runtime->serializers) {
                    return result_t<void>::failure (
                      framework_error_kind_t::internal_failure,
                      "spot publish serializer registry is unavailable");
                }
                runtime::messaging::client_call_codec_t route_codec;
                auto route_header = route_codec.create_envelope (
                  runtime::messaging::message_kind_t::command, "spot",
                  detail::spot_multicast_route_send_t::packet_name, std::chrono::seconds (30));
                const auto route =
                  detail::spot_multicast_route_send_t{topic, frame_part.to_bytes ()};
                const auto encoded = route_codec.encode_envelope_parts (
                  route_header, route, *projection.channel_runtime->serializers);
                const auto submitted = native->publish (
                  projection.discovery_channel_name, topic, encoded.items ());
                if (submitted != zlink::submit_result_t::ok) {
                    return result_t<void>::failure (
                      runtime::messaging::map_submit_result_error_kind (submitted),
                      "spot publish failed");
                }
                /* Spec 12 §1/§9 — a Logical Multicast publish is sent
                   * once per participating node and every node checks its
                   * own local subscriptions. The entry-spot publish() above
                   * owns only the LOCAL dequeue acceptance (its own
                   * comment: physical fanout is scheduled by the logical
                   * multicast executor). Run the same remote fanout tail
                   * the public spot publisher client already uses;
                   * failures are reported through the standard
                   * logical-multicast failure path and never alter the
                   * accepted publish result. */
                {
                    //  publish_tail takes its parts/metadata by reference
                    //  and suspends; feed it through a coroutine whose
                    //  PARAMETERS own copies for the frame's lifetime.
                    auto tail = run_spot_publish_fanout (
                      native, encoded.items (), detail::mesh_metadata_codec_t::encode ({}));
                    detail::observe_task_completion (tail, [node = state->node,
                                                            mesh_name = projection.mesh_name, topic,
                                                            packet = submitted_packet_name] (
                                                             const result_t<void> &fanout) {
                        if (fanout || !node)
                            return;
                        detail::report_logical_multicast_failure (
                          node, mesh_name, topic, packet,
                          fanout.error () != nullptr
                            ? framework_exception_t (fanout.error_kind (), fanout.error ()->what ())
                            : framework_exception_t (framework_error_kind_t::internal_failure,
                                                     "logical multicast fanout failed"));
                    });
                }
            }
            catch (const std::exception &error) {
                return result_t<void>::failure (framework_error_kind_t::internal_failure,
                                                error.what ());
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
        const send_call_t::metadata_map_t &metadata) mutable -> task_t<void> {
          if (!state) {
              throw framework_exception_t (framework_error_kind_t::protocol_error,
                                           "SPOT context is not configured");
          }
          try {
              state->ensure_relocation_turn_open ();
          }
          catch (const framework_exception_t &error) {
              throw error;
          }
          if (auto route_channel_name = optional_spot_route_channel_name (state)) {
              detail::channel_runtime_manager_t manager (state->channel_runtime);
              auto &runtime = manager.get_route_channel (*route_channel_name);
              auto parts = encode_spot_route_parts (
                runtime::messaging::message_kind_t::command, *route_channel_name,
                submitted_packet_name, payload, std::chrono::milliseconds::zero (), metadata);
              auto submitted = runtime.submit_spot_send_parts (
                zlink::routing_id_t::from (std::string (node_rid.value ())), spot_id,
                std::move (parts));
              if (!submitted) {
                  throw submitted.error ()
                    ? *submitted.error ()
                    : framework_exception_t (framework_error_kind_t::internal_failure,
                                             "SPOT mesh send was not submitted");
              }
              co_return;
          }
          auto native = state->native_spot.lock ();
          if (!native) {
              throw framework_exception_t (framework_error_kind_t::not_found,
                                           "SPOT mesh route requires a running native Spot");
          }
          try {
              const auto channel_name = spot_mesh_channel_name (state);
              auto parts = encode_spot_route_parts (runtime::messaging::message_kind_t::command,
                                                    channel_name, submitted_packet_name, payload,
                                                    std::chrono::milliseconds::zero (), metadata);
              auto native_parts = parts.items ();
              if (native_parts.empty ()) {
                  throw framework_exception_t (framework_error_kind_t::protocol_error,
                                               "SPOT mesh send requires at least one message part");
              }
              const auto target_node_rid =
                zlink::routing_id_t::from (std::string (node_rid.value ()));
              const auto target_spot_id = spot_id;
              const auto target_generation =
                resolve_target_spot_generation (state->node, target_node_rid, target_spot_id);
              if (!target_generation) {
                  throw framework_exception_t (framework_error_kind_t::not_found,
                                               "SPOT mesh send target generation is unavailable");
              }
              const auto submitted = co_await native->send_to_spot (
                target_node_rid, target_spot_id, *target_generation, native_parts);
              if (submitted != zlink::submit_result_t::ok) {
                  throw framework_exception_t (
                    runtime::messaging::map_submit_result_error_kind (submitted),
                    "SPOT mesh send was not submitted");
              }
              co_return;
          }
          catch (const framework_exception_t &error) {
              throw error;
          }
          catch (const std::exception &error) {
              throw framework_exception_t (framework_error_kind_t::internal_failure, error.what ());
          }
      });
}

spot_context_t::erased_request_call_t spot_context_t::request_to_erased (node_rid_t node_rid,
                                                                         spot_id_t spot_id,
                                                                         std::string packet_name,
                                                                         zlink::message_t payload)
{
    if (node_rid.empty () || spot_id.empty ()) {
        return erased_request_call_t (
          framework_exception_t (framework_error_kind_t::not_found, "target spot route is empty"));
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
                  auto reply = runtime.request_reply_spot_parts (
                    zlink::routing_id_t::from (std::string (node_rid.value ())), spot_id,
                    std::move (parts), effective_timeout);
                  if (!reply) {
                      if (reply.error_kind () == framework_error_kind_t::not_found) {
                          report_spot_dispatch_error (
                            state->node, dispatch_error_surface_t::spot_route,
                            dispatch_message_kind_t::request,
                            dispatch_error_reason_t::handler_missing,
                            dispatch_error_action_t::reply_error, submitted_packet_name,
                            *route_channel_name, std::string (spot_id), std::nullopt,
                            reply.error () ? std::make_exception_ptr (*reply.error ())
                                           : std::exception_ptr{});
                      }
                      return task_t<zlink::message_t> (detail::propagate_failure<zlink::message_t> (
                        reply, "SPOT route request failed"));
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
                timeout > std::chrono::milliseconds::zero () ? timeout : std::chrono::seconds (30);
              const auto channel_name = spot_mesh_channel_name (state);
              auto parts = encode_spot_route_parts (runtime::messaging::message_kind_t::request,
                                                    channel_name, submitted_packet_name, payload,
                                                    effective_timeout, metadata);
              return request_spot_mesh_message (state, std::move (node_rid), std::move (spot_id),
                                                std::move (parts), effective_timeout);
          }
          catch (const framework_exception_t &error) {
              return task_t<zlink::message_t> (
                detail::result_access_t::failure<zlink::message_t> (error));
          }
      },
      [state, target_spot_id] (bool release_turn) {
          if (state) {
              try {
                  state->ensure_relocation_turn_open ();
              }
              catch (const framework_exception_t &error) {
                  return detail::result_access_t::failure<void> (error);
              }
          }
          if (!release_turn && state && state->spot_id == target_spot_id
              && state->owns_current_serial_turn ()) {
              return result_t<void>::failure (
                framework_error_kind_t::invalid_operation,
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
        const auto topic = descriptor.topic;
        const auto subscription = _state->node
                                    ? _state->node->lane.run ([&] {
                                          return std::pair{
                                            _state->native_spot.lock (),
                                            _state->node->snapshot.discovery_channel_name.value_or (
                                              _state->node->snapshot.name)};
                                      }).get ()
                                    : std::pair<std::shared_ptr<service::spot_t>, std::string>{};
        if (subscription.first) {
            subscription.first->set_subscription (subscription.second, topic);
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

task_t<zlink::message_t> spot_handler_registry_t::invoke_erased (
  spot_handler_kind_t kind,
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
  std::string actor_execution_spot_id,
  std::function<std::optional<result_t<zlink::message_t>> (
    const zlink::message_t &, const spot_inbound_message_t &)> before_invoke,
  actor_queue_dispatch_t actor_queue_dispatch,
  std::function<void ()> before_application_handler,
  std::function<void ()> after_application_admission,
  std::function<void ()> transfer_owner_reservation,
  std::size_t transferred_owner_byte_cost) const
{
    for (std::size_t index = 0; index < _state->handlers.size (); ++index) {
        const auto &descriptor = _state->handlers[index];
        if (descriptor.kind == kind && descriptor.packet_name == packet_name
            && descriptor.topic == topic && descriptor.actor_type == actor_type) {
            const auto handler_index = index;
            const auto trace_message_kind = kind == spot_handler_kind_t::actor_send
                                              ? dispatch_message_kind_t::actor_send
                                              : dispatch_message_kind_t::actor_request;
            std::string trace_packet_name;
            std::string trace_actor_id;
            std::string trace_spot_id;
            if (_state->node) {
                detail::message_flow_tracer_t (_state->node->dispatch)
                  .trace (message_flow_log_mode_t::detailed, message_flow_outcome_t::received, [&] {
                      trace_packet_name = packet_name;
                      const auto actor_key_separator = actor_execution_key.rfind (':');
                      trace_actor_id = actor_key_separator == std::string::npos
                                         ? actor_execution_key
                                         : actor_execution_key.substr (actor_key_separator + 1);
                      trace_spot_id = actor_execution_spot_id;
                      return message_flow_event_t{
                        .outcome = message_flow_outcome_t::received,
                        .surface = dispatch_error_surface_t::spot_actor,
                        .message_kind = trace_message_kind,
                        .packet_name = trace_packet_name,
                        .spot_id = trace_spot_id.empty () ? std::nullopt
                                                          : std::make_optional (trace_spot_id),
                        .actor_id = trace_actor_id.empty () ? std::nullopt
                                                            : std::make_optional (trace_actor_id),
                        .detail_stage = "invoke_erased.enter",
                        .detail_result =
                          std::string ("serial_dispatch=") + (serial_dispatch ? "true" : "false")};
                  });
            }
            detail::task_completion_source_t<zlink::message_t> completion;
            auto task = completion.task ();
            auto state = _state;
            const auto coordinator = state->ensure_spot_serial_executor ();
            const bool requires_spot_serial = coordinator
                                               && coordinator->uses_spot_execution_gate ();
            if (actor_queue_dispatch == actor_queue_dispatch_t::acquire
                && !actor_execution_key.empty () && coordinator) {
                serial_dispatch = true;
            }
            report_actor_dispatch_stage_trace_lazy (
              state->node, message_flow_outcome_t::received, trace_message_kind, trace_packet_name,
              trace_spot_id, trace_actor_id, "invoke_erased.queue_select", [&] {
                  return std::string ("actor_queue=coordinator")
                         + " spot_serial=" + (requires_spot_serial ? "true" : "false");
              });
            if (!serial_dispatch) {
                if (!state->enter_callback ()) {
                    return task_t<zlink::message_t> (detail::boundary_failure<zlink::message_t> (
                      detail::boundary_error_t::closed, "spot activation is closed"));
                }
                try {
                    detail::callback_context_scope_t callback_scope (state.get ());
                    auto carrier =
                      std::make_shared<std::pair<zlink::message_t, spot_inbound_message_t>> (
                        std::move (message), std::move (metadata));
                    runtime::actor_execution_scope_t actor_execution (
                      std::move (actor_execution_key), std::move (actor_execution_spot_id));
                    if (after_application_admission)
                        after_application_admission ();
                    if (before_application_handler) {
                        before_application_handler ();
                    }
                    auto handler_task = state->handler_invokers[handler_index](
                      spot, actor, services, serializers, carrier->first, carrier->second);
                    detail::observe_task_completion (
                      handler_task, [state, completion, carrier, trace_message_kind,
                                     trace_packet_name, trace_spot_id, trace_actor_id] (
                                      const result_t<zlink::message_t> &result) mutable {
                          report_actor_dispatch_stage_trace_lazy (
                            state->node, message_flow_outcome_t::dispatched, trace_message_kind,
                            trace_packet_name, trace_spot_id, trace_actor_id,
                            "invoke_erased.application_handler", [&] {
                                return std::string ("result=finished success=")
                                       + (result ? "true" : "false");
                            });
                          state->leave_callback ();
                          if (result) {
                              completion.complete (
                                result_t<zlink::message_t>::success (result.value ()));
                              return;
                          }
                          completion.complete (result_t<zlink::message_t>::failure (
                            result.error_kind (), result.error () != nullptr
                                                    ? result.error ()->what ()
                                                    : "spot handler failed"));
                      });
                }
                catch (const framework_exception_t &error) {
                    report_actor_dispatch_stage_trace (
                      state->node, message_flow_outcome_t::dispatched, trace_message_kind,
                      trace_packet_name, trace_spot_id, trace_actor_id,
                      "invoke_erased.application_handler", "result=finished success=false");
                    state->leave_callback ();
                    completion.complete (
                      detail::result_access_t::failure<zlink::message_t> (error));
                }
                catch (const std::exception &error) {
                    report_actor_dispatch_stage_trace (
                      state->node, message_flow_outcome_t::dispatched, trace_message_kind,
                      trace_packet_name, trace_spot_id, trace_actor_id,
                      "invoke_erased.application_handler", "result=finished success=false");
                    state->leave_callback ();
                    completion.complete (result_t<zlink::message_t>::failure (
                      framework_error_kind_t::internal_failure, error.what ()));
                }
                catch (...) {
                    report_actor_dispatch_stage_trace (
                      state->node, message_flow_outcome_t::dispatched, trace_message_kind,
                      trace_packet_name, trace_spot_id, trace_actor_id,
                      "invoke_erased.application_handler", "result=finished success=false");
                    state->leave_callback ();
                    completion.complete (result_t<zlink::message_t>::failure (
                      framework_error_kind_t::internal_failure, "spot handler threw an exception"));
                }
                return task;
            }
            const bool has_transferred_owner_reservation =
              static_cast<bool> (transfer_owner_reservation);
            const runtime::serial_work_options_t work_options{
              runtime::serial_work_lane_t::application,
              has_transferred_owner_reservation
                ? transferred_owner_byte_cost
                : handler_work_byte_cost (message, metadata),
              transfer_owner_reservation};
            auto carrier = std::make_shared<std::pair<zlink::message_t, spot_inbound_message_t>> (
              std::move (message), std::move (metadata));
            auto dispatch_flow = runtime::flow_context_t::current ();
            auto trace_serial_queue = coordinator ? coordinator->spot_queue () : state->serial_queue;
            std::function<bool (std::string, runtime::serial_execution_queue_t::async_work_t,
                                runtime::serial_work_options_t)>
              post_serial;
            if (actor_queue_dispatch == actor_queue_dispatch_t::current_turn
                && state->serial_queue) {
                post_serial = [state] (std::string name,
                                       runtime::serial_execution_queue_t::async_work_t work,
                                       runtime::serial_work_options_t options) {
                    if (state->admission_blocked ())
                        return false;
                    return state->try_post_serial_async (std::move (name), std::move (work),
                                                         options);
                };
            } else if (coordinator && !actor_execution_key.empty ()) {
                const auto current_turn = actor_queue_dispatch == actor_queue_dispatch_t::current_turn;
                post_serial = [coordinator, actor_id = actor_execution_key, completion,
                               current_turn] (std::string name,
                                              runtime::serial_execution_queue_t::async_work_t work,
                                              runtime::serial_work_options_t options) {
                    return coordinator->execute_actor (
                      actor_id, std::move (name), std::move (work), options, current_turn,
                      [completion, options] () mutable {
                          completion.complete (result_t<zlink::message_t>::failure (
                            options.transfer_owner_reservation
                              ? framework_error_kind_t::shutting_down
                              : framework_error_kind_t::capacity_exceeded,
                            options.transfer_owner_reservation
                              ? "spot serial queue is closed or stopping"
                              : "spot serial queue is full"));
                      });
                };
            } else {
                post_serial = [state] (std::string name,
                                       runtime::serial_execution_queue_t::async_work_t work,
                                       runtime::serial_work_options_t options) {
                    return state->try_post_serial_async (std::move (name), std::move (work),
                                                         options);
                };
            }
            const bool finish_after_active =
              actor_queue_dispatch == actor_queue_dispatch_t::current_turn;
            const auto posted = post_serial (
              "spot-handler",
              [state, handler_index, spot, actor, &services, &serializers,
               carrier = std::move (carrier), completion, dispatch_flow = std::move (dispatch_flow),
               actor_execution_key = std::move (actor_execution_key),
               actor_execution_spot_id = std::move (actor_execution_spot_id),
               before_invoke = std::move (before_invoke),
               before_application_handler = std::move (before_application_handler),
               trace_message_kind, trace_packet_name, trace_actor_id, trace_spot_id,
               finish_after_active] (auto complete) mutable {
                  runtime::flow_context_t::scope_t callback_flow (std::move (dispatch_flow));
                  runtime::actor_execution_scope_t actor_execution (
                    std::move (actor_execution_key), std::move (actor_execution_spot_id));
                  report_actor_dispatch_stage_trace (
                    state->node, message_flow_outcome_t::dispatched, trace_message_kind,
                    trace_packet_name, trace_spot_id, trace_actor_id,
                    "invoke_erased.serial_queue_execute", "started=true");
                  if (before_invoke) {
                      auto redirected = before_invoke (carrier->first, carrier->second);
                      if (redirected) {
                          complete ([completion, result = std::move (*redirected)] () mutable {
                              completion.complete (std::move (result));
                          });
                          return;
                      }
                  }
                  if (!state->enter_callback ()) {
                      complete ([completion] () mutable {
                          completion.complete (detail::boundary_failure<zlink::message_t> (
                            detail::boundary_error_t::closed, "spot activation is closed"));
                      });
                      return;
                  }
                  auto turn = detail::capture_current_serial_turn ();
                  try {
                      detail::callback_context_scope_t callback_scope (state.get ());
                      if (before_application_handler) {
                          before_application_handler ();
                      }
                      auto handler_task = state->handler_invokers[handler_index](
                        spot, actor, services, serializers, carrier->first, carrier->second);
                      detail::observe_task_completion (
                        handler_task,
                        [state, completion, turn, complete, carrier, finish_after_active,
                         trace_message_kind, trace_packet_name, trace_spot_id,
                         trace_actor_id] (const result_t<zlink::message_t> &result) mutable {
                            report_actor_dispatch_stage_trace_lazy (
                              state->node, message_flow_outcome_t::dispatched, trace_message_kind,
                              trace_packet_name, trace_spot_id, trace_actor_id,
                              "invoke_erased.application_handler", [&] {
                                  return std::string ("result=finished success=")
                                         + (result ? "true" : "false");
                              });
                            result_t<zlink::message_t> final_result =
                              result ? result_t<zlink::message_t>::success (result.value ())
                                     : result_t<zlink::message_t>::failure (
                                         result.error_kind (), result.error () != nullptr
                                                                 ? result.error ()->what ()
                                                                 : "spot handler failed");
                            auto finish_owner = std::make_shared<std::function<void ()>> (
                              [state, completion,
                               final_result = std::move (final_result)] () mutable {
                                  state->leave_callback ();
                                  completion.complete (std::move (final_result));
                              });
                            auto finish = [finish_owner] () mutable {
                                auto owned = std::move (*finish_owner);
                                if (owned)
                                    owned ();
                            };
                            if (finish_after_active && turn && !turn->released ()
                                && turn->defer (finish, finish)) {
                                complete ([] {});
                                return;
                            }
                            if (turn && turn->released ()) {
                                finish ();
                                return;
                            }
                            complete (std::move (finish));
                        });
                  }
                  catch (const framework_exception_t &error) {
                      report_actor_dispatch_stage_trace (
                        state->node, message_flow_outcome_t::dispatched, trace_message_kind,
                        trace_packet_name, trace_spot_id, trace_actor_id,
                        "invoke_erased.application_handler", "result=finished success=false");
                      complete ([state, completion, error] () mutable {
                          state->leave_callback ();
                          completion.complete (
                            detail::result_access_t::failure<zlink::message_t> (error));
                      });
                  }
                  catch (const std::exception &error) {
                      report_actor_dispatch_stage_trace (
                        state->node, message_flow_outcome_t::dispatched, trace_message_kind,
                        trace_packet_name, trace_spot_id, trace_actor_id,
                        "invoke_erased.application_handler", "result=finished success=false");
                      const auto message = std::string (error.what ());
                      complete ([state, completion, message = std::move (message)] () mutable {
                          state->leave_callback ();
                          completion.complete (result_t<zlink::message_t>::failure (
                            framework_error_kind_t::internal_failure, std::move (message)));
                      });
                  }
                  catch (...) {
                      report_actor_dispatch_stage_trace (
                        state->node, message_flow_outcome_t::dispatched, trace_message_kind,
                        trace_packet_name, trace_spot_id, trace_actor_id,
                        "invoke_erased.application_handler", "result=finished success=false");
                      complete ([state, completion] () mutable {
                          state->leave_callback ();
                          completion.complete (result_t<zlink::message_t>::failure (
                            framework_error_kind_t::internal_failure,
                            "spot handler threw an exception"));
                      });
                  }
              },
              work_options);
            report_actor_dispatch_stage_trace_lazy (
              state->node,
              posted ? message_flow_outcome_t::admitted : message_flow_outcome_t::received,
              trace_message_kind, trace_packet_name, trace_spot_id, trace_actor_id,
              "invoke_erased.post_serial", [&] {
                  return std::string ("posted=") + (posted ? "true" : "false")
                         + (trace_serial_queue
                              ? " closed="
                                  + std::string (trace_serial_queue->closed () ? "true" : "false")
                                  + " pending="
                                  + std::to_string (trace_serial_queue->pending_count ())
                              : " queue=inline");
              });
            if (!posted) {
                /* Dispatch rejection is framework-generated (zlink.origin
                 * marker on the resulting error reply). */
                return task_t<zlink::message_t> (
                  detail::result_access_t::failure<zlink::message_t> (
                    detail::make_framework_origin_exception (
                      has_transferred_owner_reservation
                        ? framework_error_kind_t::shutting_down
                        : framework_error_kind_t::capacity_exceeded,
                      has_transferred_owner_reservation
                        ? "spot serial queue is closed or stopping"
                        : "spot serial queue is full")));
            }
            if (after_application_admission)
                after_application_admission ();
            return task;
        }
    }
    std::ostringstream error_message;
    error_message << "spot handler is not registered: packet='" << packet_name << "', topic='"
                  << topic << "', kind=" << static_cast<int> (kind) << ", actor_type='"
                  << actor_type.name () << "', registered=[";
    for (std::size_t index = 0; index < _state->handlers.size (); ++index) {
        if (index > 0) {
            error_message << "; ";
        }
        const auto &descriptor = _state->handlers[index];
        error_message << "packet='" << descriptor.packet_name << "', topic='" << descriptor.topic
                      << "', kind=" << static_cast<int> (descriptor.kind) << ", actor_type='"
                      << descriptor.actor_type.name () << "'";
    }
    error_message << "]";
    /* Route resolution failure is framework-generated (zlink.origin marker on
     * the resulting error reply); it must stay distinguishable from an
     * application handler's own not_found. */
    return task_t<zlink::message_t> (
      detail::result_access_t::failure<zlink::message_t> (detail::make_framework_origin_exception (
        framework_error_kind_t::not_found, error_message.str ())));
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
    _state->lane.run ([this, duration] { _state->message_follow_duration = duration; }).get ();
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
    _state->lane
      .run ([this, route_channel_name = std::move (route_channel_name),
             manual_connections = std::move (manual_connections)] () mutable {
          const auto duplicate = std::any_of (
            _state->snapshot.accepted_route_channels.begin (),
            _state->snapshot.accepted_route_channels.end (),
            [&] (const auto &accepted) { return accepted.channel_name == route_channel_name; });
          if (duplicate) {
              throw framework_exception_t (framework_error_kind_t::protocol_error,
                                           "duplicate accepted SPOT route channel");
          }
          _state->snapshot.accepted_route_channels.push_back (accepted_spot_route_channel_t{
            std::move (route_channel_name), std::move (manual_connections)});
      })
      .get ();
    return *this;
}

spot_node_builder_t &spot_node_builder_t::add_spot_factory_erased (
  std::string spot_name,
  std::type_index spot_type,
  detail::spot_runtime_kind_t kind,
  user_spot_execution_mode_t execution_mode,
  std::int32_t stable_type_limit,
  spot_relocation_coordination_mode_t relocation_coordination_mode,
  detail::factory_relocation_configuration_t relocation)
{
    const bool entry_spot = kind == detail::spot_runtime_kind_t::entry;
    const bool instance_spot = kind == detail::spot_runtime_kind_t::instance;
    if (entry_spot && execution_mode != user_spot_execution_mode_t::spot_wide) {
        throw framework_exception_t (framework_error_kind_t::not_configured,
                                     "Entry Spot execution mode is fixed by the Framework");
    }
    if (relocation.kind == detail::factory_relocation_kind_t::preserve_state
        && (!relocation.capture || !relocation.restore)) {
        throw framework_exception_t (framework_error_kind_t::not_configured,
                                     "Spot preserve-state relocation callbacks must not be empty");
    }
    _state->lane
      .run ([this, spot_name = std::move (spot_name), spot_type, kind, execution_mode,
             stable_type_limit, relocation_coordination_mode,
             relocation = std::move (relocation)] () mutable {
          const bool entry_spot = kind == detail::spot_runtime_kind_t::entry;
          const bool instance_spot = kind == detail::spot_runtime_kind_t::instance;
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
              _state->spot_relocation_coordination_modes.emplace (spot_name,
                                                                  relocation_coordination_mode);
          }
          _state->snapshot.spot_execution_modes.emplace (spot_name, execution_mode);
          _state->snapshot.spot_names.push_back (std::move (spot_name));
      })
      .get ();
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
  std::function<task_t<std::vector<std::byte>> (void *, std::stop_token)> capture,
  std::function<task_t<void> (void *, std::vector<std::byte>, std::stop_token)> restore)
{
    if ((!create_instance && !create_context_instance) || !configure_instance || !serialize_instance
        || !deserialize_instance) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "actor factory callback must not be empty");
    }
    if (relocation.kind == detail::factory_relocation_kind_t::preserve_state
        && (!capture || !restore)) {
        throw framework_exception_t (framework_error_kind_t::not_configured,
                                     "Actor preserve-state relocation callbacks must not be empty");
    }
    _state->lane
      .run ([this, actor_type = std::move (actor_type), actor_instance_type,
             create_instance = std::move (create_instance),
             configure_instance = std::move (configure_instance),
             serialize_instance = std::move (serialize_instance),
             deserialize_instance = std::move (deserialize_instance),
             create_context_instance = std::move (create_context_instance),
             on_join_completed = std::move (on_join_completed), relocation = std::move (relocation),
             capture = std::move (capture), restore = std::move (restore)] () mutable {
          const auto [_, inserted] = _state->actor_factories.emplace (
            actor_type, detail::spot_node_builder_state_t::actor_factory_registration_t{
                          actor_instance_type, relocation, std::move (create_instance),
                          std::move (configure_instance), std::move (serialize_instance),
                          std::move (deserialize_instance), std::move (create_context_instance),
                          std::move (on_join_completed), std::move (capture), std::move (restore)});
          if (!inserted) {
              throw framework_exception_t (framework_error_kind_t::already_exists,
                                           "duplicate actor factory registration");
          }
          _state->snapshot.actor_types.push_back (std::move (actor_type));
      })
      .get ();
    return *this;
}

void spot_node_builder_t::register_lifecycle_erased (std::string spot_name,
                                                     detail::spot_lifecycle_callbacks_t callbacks)
{
    _state->lane
      .run (
        [this, spot_name = std::move (spot_name), callbacks = std::move (callbacks)] () mutable {
            _state->spot_lifecycles[std::move (spot_name)] = std::move (callbacks);
        })
      .get ();
}

spot_node_builder_t &spot_node_builder_t::add_spot_resolver (
  std::string name, std::function<std::optional<spot_route_t> (spot_id_t)> resolver)
{
    if (name.empty () || !resolver) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "spot resolver requires a name and callback");
    }
    _state->lane
      .run ([this, name = std::move (name), resolver = std::move (resolver)] () mutable {
          const auto [_, inserted] =
            _state->resolvers.emplace (std::move (name), std::move (resolver));
          if (!inserted) {
              throw framework_exception_t (framework_error_kind_t::protocol_error,
                                           "duplicate spot resolver registration");
          }
      })
      .get ();
    return *this;
}

spot_node_snapshot_t spot_node_builder_t::snapshot () const
{
    return _state->lane.run ([this] { return _state->snapshot; }).get ();
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
    const auto channel_runtime = _state ? _state->lane.run ([this] {
        return _state->channel_runtime;
    }).get () : std::shared_ptr<detail::channel_runtime_state_t>{};
    if (!channel_runtime || !channel_runtime->serializers) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "spot create requires a serializer registry");
    }
    return create_spot_raw (
      std::move (spot_name),
      detail::message_to_raw (request, *channel_runtime->serializers));
}

detail::local_spot_create_result_t spot_node_builder_t::get_or_create_spot (std::string spot_name,
                                                                            spot_id_t spot_id)
{
    return detail::spot_node_runtime_t (_state).get_or_create_spot (std::move (spot_name),
                                                                    std::move (spot_id));
}

detail::local_spot_create_result_t spot_node_builder_t::get_or_create_spot_raw (
  std::string spot_name, spot_id_t spot_id, zlink::message_t request)
{
    return detail::spot_node_runtime_t (_state).get_or_create_spot (
      std::move (spot_name), std::move (spot_id), std::move (request));
}

detail::local_spot_create_result_t spot_node_builder_t::get_or_create_spot (
  std::string spot_name, spot_id_t spot_id, const message_t &request)
{
    const auto channel_runtime = _state ? _state->lane.run ([this] {
        return _state->channel_runtime;
    }).get () : std::shared_ptr<detail::channel_runtime_state_t>{};
    if (!channel_runtime || !channel_runtime->serializers) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "spot get or create requires a serializer registry");
    }
    return get_or_create_spot_raw (
      std::move (spot_name), std::move (spot_id),
      detail::message_to_raw (request, *channel_runtime->serializers));
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
    _state->lane
      .run ([this, builder = std::move (builder)] () mutable {
          _state->factory_builder_lifetimes.push_back (std::move (builder));
      })
      .get ();
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

void report_logical_multicast_failure (const std::shared_ptr<spot_node_builder_state_t> &state,
                                       std::string_view channel_name,
                                       std::string_view topic,
                                       std::string_view packet_name,
                                       const framework_exception_t &error) noexcept
{
    if (!state)
        return;
    try {
        dispatch_error_reporter_t (state->dispatch).report_lazy ([&] {
            return message_dispatch_error_event_t{.surface =
                                                    dispatch_error_surface_t::route_mesh_channel,
                                                  .message_kind = dispatch_message_kind_t::publish,
                                                  .reason = dispatch_reason_from_error (&error),
                                                  .action = dispatch_error_action_t::drop,
                                                  .packet_name = std::string (packet_name),
                                                  .channel_name = std::string (channel_name),
                                                  .topic = std::string (topic),
                                                  .exception = std::make_exception_ptr (error)};
        });
    }
    catch (...) {
        // Diagnostics cannot change the already-completed publish result.
    }
}

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

spot_manager_t::spot_manager_t (std::shared_ptr<detail::spot_node_builder_state_t> state) :
    _state (std::move (state))
{
}

spot_manager_t::spot_manager_t (std::shared_ptr<detail::spot_node_builder_state_t> state,
                                std::weak_ptr<detail::spot_context_state_t> source) :
    _state (std::move (state)), _source (std::move (source))
{
}

spot_manager_t::~spot_manager_t () = default;
spot_manager_t::spot_manager_t (spot_manager_t &&) noexcept = default;
spot_manager_t &spot_manager_t::operator= (spot_manager_t &&) noexcept = default;

spot_create_call_t::spot_create_call_t (std::shared_ptr<detail::spot_create_call_state_t> state) :
    _state (std::move (state))
{
}

spot_create_call_t::~spot_create_call_t () = default;
spot_create_call_t::spot_create_call_t (spot_create_call_t &&) noexcept = default;
spot_create_call_t &spot_create_call_t::operator= (spot_create_call_t &&) noexcept = default;

spot_create_call_t &spot_create_call_t::in_mesh (std::string mesh_name)
{
    if (!_state || _state->mesh_name)
        throw framework_exception_t (framework_error_kind_t::not_configured,
                                     "Spot create Mesh can be set only once");
    _state->mesh_name = std::move (mesh_name);
    return *this;
}

spot_create_call_t &spot_create_call_t::creation_request (message_t request)
{
    if (!_state || _state->request)
        throw framework_exception_t (framework_error_kind_t::not_configured,
                                     "Spot creation request can be set only once");
    _state->request = std::move (request);
    return *this;
}

spot_create_call_t &spot_create_call_t::timeout (std::chrono::milliseconds timeout)
{
    if (!_state || _state->timeout || timeout <= std::chrono::milliseconds::zero ())
        throw framework_exception_t (framework_error_kind_t::not_configured,
                                     "Spot create timeout must be positive and set only once");
    _state->timeout = timeout;
    return *this;
}

task_t<spot_create_result_t> spot_create_call_t::submit ()
{
    if (!_state || _state->submitted)
        return task_t<spot_create_result_t> (result_t<spot_create_result_t>::failure (
          framework_error_kind_t::invalid_operation, "Spot create call was already submitted"));
    _state->submitted = true;
    if (const auto source = _state->source.lock ()) {
        try {
            source->ensure_relocation_turn_open ();
        }
        catch (const framework_exception_t &error) {
            return task_t<spot_create_result_t> (
              detail::result_access_t::failure<spot_create_result_t> (error));
        }
    }
    if (!_state->node)
        return task_t<spot_create_result_t> (result_t<spot_create_result_t>::failure (
          framework_error_kind_t::not_configured,
          "Spot manager is not connected to a Location runtime"));
    const auto create_user_spot = _state->node->lane.run ([&] {
        return _state->node->create_user_spot;
    }).get ();
    if (!create_user_spot)
        return task_t<spot_create_result_t> (result_t<spot_create_result_t>::failure (
          framework_error_kind_t::not_configured,
          "Spot manager is not connected to a Location runtime"));
    auto task = create_user_spot (
      _state->exclusive, _state->spot_id, std::move (_state->stable_type),
      std::move (_state->mesh_name), std::move (_state->request),
      _state->timeout.value_or (std::chrono::seconds (30)));
    const auto turn_plan = detail::prepare_serial_turn_await (false);
    if (!turn_plan)
        return task;
    return detail::reschedule_task (std::move (task), std::move (turn_plan->scheduler));
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
    return detail::reschedule_task (std::move (task), std::move (turn_plan->scheduler));
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

spot_create_call_t spot_manager_t::get_or_create (spot_id_t spot_id, std::string stable_type)
{
    auto state = std::make_shared<detail::spot_create_call_state_t> ();
    state->node = _state;
    state->source = _source;
    state->spot_id = std::move (spot_id);
    state->stable_type = std::move (stable_type);
    return spot_create_call_t (std::move (state));
}

task_t<std::optional<spot_ref_t>> spot_manager_t::find (spot_id_t spot_id) const
{
    if (const auto source = _source.lock ()) {
        try {
            source->ensure_relocation_turn_open ();
        }
        catch (const framework_exception_t &error) {
            return task_t<std::optional<spot_ref_t>> (
              detail::result_access_t::failure<std::optional<spot_ref_t>> (error));
        }
    }
    if (!_state)
        return task_t<std::optional<spot_ref_t>> (result_t<std::optional<spot_ref_t>>::failure (
          framework_error_kind_t::not_configured,
          "Spot manager is not connected to a Location runtime"));
    const auto find_user_spot = _state->lane.run ([&] { return _state->find_user_spot; }).get ();
    if (!find_user_spot)
        return task_t<std::optional<spot_ref_t>> (result_t<std::optional<spot_ref_t>>::failure (
          framework_error_kind_t::not_configured,
          "Spot manager is not connected to a Location runtime"));
    return find_user_spot (std::move (spot_id));
}

task_t<bool> spot_manager_t::close (spot_ref_t spot)
{
    if (const auto source = _source.lock ()) {
        try {
            source->ensure_relocation_turn_open ();
        }
        catch (const framework_exception_t &error) {
            return task_t<bool> (detail::result_access_t::failure<bool> (error));
        }
    }
    if (!_state)
        return task_t<bool> (
          result_t<bool>::failure (framework_error_kind_t::not_configured,
                                   "Spot manager is not connected to a Location runtime"));
    const auto close_user_spot = _state->lane.run ([&] { return _state->close_user_spot; }).get ();
    if (!close_user_spot)
        return task_t<bool> (
          result_t<bool>::failure (framework_error_kind_t::not_configured,
                                   "Spot manager is not connected to a Location runtime"));
    return close_user_spot (std::move (spot));
}

std::optional<actor_ref_t> spot_manager_t::current_actor_ref (const actor_ref_t &actor_ref) const
{
    return detail::spot_node_runtime_t (_state).current_actor_ref (actor_ref);
}

task_t<std::optional<zlink::message_t>>
spot_manager_t::relay_actor_packet (const actor_ref_t &actor_ref,
                                    actor_context_t actor_context,
                                    std::string_view packet_name,
                                    const zlink::message_t &message,
                                    service_provider_t &services,
                                    serializer_registry_t &serializers,
                                    spot_inbound_message_t metadata)
{
    co_return co_await relay_actor_packet (actor_ref, std::move (actor_context),
                                           detail::stream_message_kind_t::request, packet_name,
                                           message, services, serializers, std::move (metadata));
}

task_t<std::optional<zlink::message_t>> spot_manager_t::relay_actor_packet (
  const actor_ref_t &actor_ref,
  actor_context_t actor_context,
  detail::stream_message_kind_t message_kind,
  std::string_view packet_name,
  const zlink::message_t &message,
  service_provider_t &services,
  serializer_registry_t &serializers,
  spot_inbound_message_t metadata,
  const runtime::protocol::actor_route_fence_t *admitted_message_follow_target)
{
    const auto actor_packet_relay = _state->lane.run ([&] {
        return _state->actor_packet_relay;
    }).get ();
    if (actor_packet_relay) {
        co_return co_await actor_packet_relay (
          actor_ref, std::move (actor_context), message_kind, packet_name, message, services,
          serializers, std::move (metadata), admitted_message_follow_target);
    }
    // `relay_actor_packet` is a coroutine.  Do not invoke that member on a
    // temporary: after its first suspension the coroutine frame retains the
    // receiver (`this`), not the temporary object.  In particular, a Join can
    // relocate the Actor before the handler task completes, so its completion
    // resumes this call after the full handler turn has unwound.  Keep the
    // runtime wrapper in this coroutine frame until that resume is finished.
    auto runtime = detail::spot_node_runtime_t (_state);
    co_return co_await runtime.relay_actor_packet (
      actor_ref, std::move (actor_context), message_kind, packet_name, message, services,
      serializers, std::move (metadata), admitted_message_follow_target);
}

spot_publisher_client_t::spot_publisher_client_t (spot_manager_t manager,
                                                  serializer_registry_t &serializers) :
    _manager (std::move (manager)), _serializers (&serializers)
{
}

publish_call_t spot_publisher_client_t::publish_raw (std::string channel_name,
                                                     std::string topic,
                                                     std::string packet_name,
                                                     std::string content_type,
                                                     zlink::message_t payload) const
{
    if (!_serializers) {
        return publish_call_t (
          result_t<void>::failure (framework_error_kind_t::internal_failure,
                                   "logical multicast serializer registry is unavailable"));
    }
    if (channel_name.empty () || is_blank (channel_name)) {
        return publish_call_t (
          result_t<void>::failure (framework_error_kind_t::protocol_error,
                                   "logical multicast channel name must not be empty"));
    }
    if (topic.empty ()) {
        return publish_call_t (result_t<void>::failure (
          framework_error_kind_t::protocol_error, "logical multicast topic must not be empty"));
    }

    auto native_node = _manager._state->lane.run ([&] {
        return _manager._state->native_node.lock ();
    }).get ();
    if (!native_node) {
        return publish_call_t (result_t<void>::failure (
          framework_error_kind_t::unavailable, "logical multicast route mesh is not connected"));
    }

    const auto diagnostics_mode = detail::message_flow_tracer_t (_manager._state->dispatch).mode ();
    auto frame = encode_spot_publish_frame (channel_name, packet_name, topic,
                                            std::move (content_type), payload);
    return publish_call_t (
      [native_node = std::move (native_node), state = _manager._state,
       channel_name = std::move (channel_name), topic = std::move (topic),
       packet_name = std::move (packet_name), frame = std::move (frame),
       diagnostics_mode] (const publish_call_t::metadata_map_t &metadata) -> task_t<void> {
          const auto fail = [&] (const framework_exception_t &error) {
              detail::report_logical_multicast_failure (state, channel_name, topic, packet_name,
                                                        error);
          };
          try {
              auto flow_scope = runtime::flow_context_t::enter_current_or_create (
                flow_origin_t::application, diagnostics_mode);
              std::vector<zlink::message_t> parts{frame};
              auto publisher = native_node->entry_spot ();
              const auto encoded_metadata = detail::mesh_metadata_codec_t::encode (metadata);
              const auto submitted = publisher.publish (
                channel_name, topic, parts, zlink::send_flags_t::none, encoded_metadata);
              if (submitted != zlink::submit_result_t::ok) {
                  const framework_exception_t error (
                    runtime::messaging::map_submit_result_error_kind (submitted),
                    "logical multicast could not enter the source transport queue");
                  throw error;
              }
              co_await publisher.publish_tail (parts, encoded_metadata);
              co_return;
          }
          catch (const framework_exception_t &error) {
              fail (error);
              throw;
          }
          catch (const std::exception &error) {
              const framework_exception_t failure (framework_error_kind_t::internal_failure,
                                                   error.what ());
              fail (failure);
              throw failure;
          }
          catch (...) {
              const framework_exception_t failure (framework_error_kind_t::internal_failure,
                                                   "logical multicast failed after admission");
              fail (failure);
              throw failure;
          }
      });
}

} // namespace zlink::framework

namespace zlink::framework::detail
{

spot_manager_t spot_node_runtime_t::manager () const
{
    return spot_manager_t (_state);
}

spot_node_runtime_t::actor_join_state_snapshot_t
spot_node_runtime_t::actor_join_state_snapshot (const actor_ref_t &actor_ref,
                                                spot_id_t spot_id,
                                                const zlink::message_t &request)
{
    struct selection_t
    {
        actor_join_state_snapshot_t snapshot;
        std::optional<std::string> dynamic_spot_name;
    };
    const auto key = actor_key (actor_ref);
    auto select = [&] {
        return _state->lane.run ([&] {
            selection_t result;
            auto context = find_context_core (spot_id);
            if (context && context->_state->node.get () == _state.get ()
                && !context->_state->closed && context->_state->close_reservation == 0
                && context->_state->spot_instance) {
                auto &snapshot = result.snapshot;
                snapshot.context.emplace (std::move (*context));
                const auto &context_state = snapshot.context->_state;
                snapshot.spot_instance = context_state->spot_instance;
                snapshot.serializers = context_state->channel_runtime
                                         ? context_state->channel_runtime->serializers
                                         : nullptr;
                snapshot.mesh_name = context_state->mesh_name;
                snapshot.node_rid = detail::effective_spot_node_rid (_state->snapshot);
                if (const auto source = _state->actor_spot_ids.find (key);
                    source != _state->actor_spot_ids.end ()) {
                    snapshot.source_spot_id = source->second;
                }
                snapshot.message_follow_duration = _state->message_follow_duration;
                snapshot.has_root_services = _state->root_services.has_value ();

                const auto factory = _state->actor_factories.find (
                  std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (
                    actor_ref)));
                if (factory != _state->actor_factories.end ()) {
                    snapshot.registration.emplace (factory->second);
                    const auto admission =
                      context_state->actor_admissions.find (factory->second.actor_type);
                    if (admission != context_state->actor_admissions.end ()
                        && admission->second.join) {
                        snapshot.admission.emplace (admission->second);
                    }
                }

                const auto instance = _state->actor_instances.find (key);
                if (instance != _state->actor_instances.end () && instance->second) {
                    snapshot.actor_instance = instance->second;
                    detail::record_actor_instance_index_unlocked (
                      *_state, actor_ref, snapshot.actor_instance.get ());
                }
                return result;
            }
            for (const auto &[spot_name, _] : _state->spot_factories) {
                if (_state->snapshot.entry_spot_name
                    && spot_name == *_state->snapshot.entry_spot_name) {
                    continue;
                }
                if (result.dynamic_spot_name) {
                    result.dynamic_spot_name.reset ();
                    break;
                }
                result.dynamic_spot_name = spot_name;
            }
            return result;
        }).get ();
    };
    auto selected = select ();
    if (!selected.snapshot.context && selected.dynamic_spot_name) {
        (void) get_or_create_spot (*selected.dynamic_spot_name, spot_id, request);
        auto refreshed = select ();
        return std::move (refreshed.snapshot);
    }
    return std::move (selected.snapshot);
}

result_t<spot_actor_admission_callbacks_t>
spot_node_runtime_t::actor_admission (spot_context_t &context,
                                      std::type_index actor_type,
                                      spot_id_t spot_id,
                                      const actor_ref_t &actor_ref)
{
    std::optional<spot_actor_admission_callbacks_t> selected;
    _state->lane.run ([&] {
        const auto current = find_context_core (context.spot_id ());
        if (!current || current->_state.get () != context._state.get ()
            || context._state->node.get () != _state.get () || context._state->closed
            || context._state->close_reservation != 0 || !context._state->spot_instance) {
            return;
        }
        const auto admission = context._state->actor_admissions.find (actor_type);
        if (admission != context._state->actor_admissions.end () && admission->second.join)
            selected.emplace (admission->second);
    }).get ();
    if (!selected) {
        report_spot_dispatch_error (
          _state, dispatch_error_surface_t::spot_actor, dispatch_message_kind_t::actor_request,
          dispatch_error_reason_t::handler_missing, dispatch_error_action_t::reply_error,
          "actor.join", std::nullopt, std::string (spot_id),
          std::string (actor_ref.actor_id ().value ()));
        return result_t<spot_actor_admission_callbacks_t>::failure (
          framework_error_kind_t::not_found, "spot actor join callback is not registered");
    }
    return result_t<spot_actor_admission_callbacks_t>::success (std::move (*selected));
}

void spot_node_runtime_t::commit_accepted_actor_join (
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
    struct commit_plan_t
    {
        std::optional<spot_context_t> previous_context;
        std::shared_ptr<service::mesh_node_t> native_node;
        std::shared_ptr<void> target_spot_instance;
        serializer_registry_t *serializers = nullptr;
        std::string node_rid;
        spot_id_t target_spot_id;
        bool create_entry_actor = false;
        bool create_native_actor = false;
        bool valid = false;
    };
    auto &target_state = *context._state;
    const auto plan = _state->lane.run ([&] {
        commit_plan_t result;
        const auto target = find_context_core (context.spot_id ());
        if (!target || target->_state.get () != context._state.get ()
            || target_state.node.get () != _state.get () || target_state.closed
            || target_state.close_reservation != 0 || !target_state.spot_instance) {
            return result;
        }
        if (const auto previous = _state->actor_spot_ids.find (key);
            previous != _state->actor_spot_ids.end ()) {
            auto previous_context = find_context_core (previous->second);
            if (previous_context)
                result.previous_context.emplace (std::move (*previous_context));
        }
        result.native_node = _state->native_node.lock ();
        result.target_spot_instance = target_state.spot_instance;
        result.serializers =
          target_state.channel_runtime ? target_state.channel_runtime->serializers : nullptr;
        result.node_rid = detail::effective_spot_node_rid (_state->snapshot);
        result.target_spot_id = target_state.spot_id;
        result.create_entry_actor =
          create_entry_actor && target_state.is_entry_spot ()
          && !_state->actor_created_keys.contains (key) && admission.on_create_actor;
        result.create_native_actor =
          result.native_node && !_state->native_actors.contains (key)
          && !_state->mesh_runtime_owned_native_actor_ids.contains (
            std::string (committed.actor_id ().value ()));
        result.valid = true;
        return result;
    }).get ();
    if (!plan.valid) {
        throw framework_exception_t (framework_error_kind_t::not_found,
                                     "target spot is not registered");
    }
    const auto caller_owns_source_turn =
      plan.previous_context && plan.previous_context->_state->owns_current_serial_turn ();
    bool created_entry_actor = false;
    if (plan.create_entry_actor) {
        if (!plan.serializers) {
            throw framework_exception_t (framework_error_kind_t::protocol_error,
                                         "spot create actor requires a serializer registry");
        }
        result_t<actor_create_response_t> create_result =
          result_t<actor_create_response_t>::failure (
            framework_error_kind_t::internal_failure,
            "Entry Spot actor creation callback did not complete");
        auto create_actor = [&] {
            create_result = admission
                              .on_create_actor (plan.target_spot_instance.get (), actor,
                                                create_request, *plan.serializers)
                              .result ();
        };
        const bool caller_owns_target_turn =
          caller_owns_source_turn
          && plan.previous_context->_state.get () == context._state.get ();
        if (caller_owns_target_turn) {
            create_actor ();
        } else if (!target_state.run_serial_sync ("spot-actor-create", create_actor)) {
            throw framework_exception_t (framework_error_kind_t::capacity_exceeded,
                                         "spot serial queue is full");
        }
        if (!create_result)
            throw framework_exception_t (create_result.error_kind (),
                                         create_result.error ()
                                           ? create_result.error ()->what ()
                                           : "Entry Spot actor creation callback failed");
        if (!create_result.value ().accepted)
            throw framework_exception_t (framework_error_kind_t::rejected,
                                         "Entry Spot rejected Actor creation");
        created_entry_actor = _state->lane.run ([&] {
            const auto target = find_context_core (plan.target_spot_id);
            if (!target || target->_state.get () != context._state.get ()
                || target_state.node.get () != _state.get () || target_state.closed
                || target_state.close_reservation != 0
                || target_state.spot_instance.get () != plan.target_spot_instance.get ()) {
                return false;
            }
            _state->actor_created_keys.insert (key);
            return true;
        }).get ();
        if (!created_entry_actor) {
            throw framework_exception_t (framework_error_kind_t::not_found,
                                         "target spot is not registered");
        }
    }
    /*
     * Publish membership before lifecycle callbacks. Application callbacks
     * observe the committed target Context, and a callback failure must not
     * roll authority back to the previous owner.
     */
    const auto location_updated =
      update_actor_location_after_move (_state, committed, target_state, false);
    if (!location_updated) {
        throw framework_exception_t (location_updated.error_kind (),
                                     location_updated.error ()
                                       ? location_updated.error ()->what ()
                                       : "actor committed location update failed");
    }
    authority_committed = true;

    std::unique_ptr<service::actor_t> native_actor;
    if (plan.create_native_actor) {
        native_actor = std::make_unique<service::actor_t> (plan.native_node->create_actor (
          std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (committed)),
          std::string (committed.actor_id ().value ())));
    }
    std::function<result_t<void> (const actor_ref_t &)> update_actor_registry_ref;
    struct leave_projection_t
    {
        std::shared_ptr<spot_context_state_t> context;
        std::shared_ptr<void> spot_instance;
        std::function<task_t<void> (void *, void *)> callback;
    } leave;
    const auto route_committed = _state->lane.run ([&] {
        const auto target = find_context_core (plan.target_spot_id);
        if (!target || target->_state.get () != context._state.get ()
            || target_state.node.get () != _state.get () || target_state.closed
            || target_state.close_reservation != 0
            || target_state.spot_instance.get () != plan.target_spot_instance.get ()) {
            return false;
        }
        std::optional<spot_context_t> previous_context;
        if (const auto previous = _state->actor_spot_ids.find (key);
            previous != _state->actor_spot_ids.end ()) {
            auto selected = find_context_core (previous->second);
            if (selected)
                previous_context.emplace (std::move (*selected));
        }
        if (previous_context)
            decrement_actor_count_unlocked (*previous_context->_state);
        erase_actor_route_unlocked (*_state, key);
        _state->destroyed_actor_keys.erase (key);
        if (native_actor && !_state->native_actors.contains (key)
            && !_state->mesh_runtime_owned_native_actor_ids.contains (
              std::string (committed.actor_id ().value ()))) {
            _state->native_actors.emplace (key, std::move (native_actor));
        }
        detail::record_actor_context_route_unlocked (*_state, key, plan.node_rid, target_state,
                                                     committed.object_generation ());
        update_actor_registry_ref = _state->update_actor_registry_ref;
        if (previous_context) {
            const auto previous_admission =
              previous_context->_state->actor_admissions.find (actor_type);
            if (previous_admission != previous_context->_state->actor_admissions.end ()
                && previous_admission->second.on_leave_actor
                && previous_context->_state->spot_instance) {
                leave.context = previous_context->_state;
                leave.spot_instance = previous_context->_state->spot_instance;
                leave.callback = previous_admission->second.on_leave_actor;
            }
        }
        return true;
    }).get ();
    if (!route_committed) {
        throw framework_exception_t (framework_error_kind_t::not_found,
                                     "target spot is not registered");
    }
    if (update_actor_registry_ref) {
        const auto updated = update_actor_registry_ref (committed);
        if (!updated) {
            throw framework_exception_t (updated.error_kind (), updated.error ()
                                                                  ? updated.error ()->what ()
                                                                  : "actor ref update failed");
        }
    }
    if (actor_transfer_marker_enabled ()) {
        emit_actor_transfer_marker ("location_committed", committed,
                                    operation_id.empty () ? key : std::move (operation_id),
                                    context.spot_id (), node_rid ());
    }

    if (!created_entry_actor && admission.on_actor_joined) {
        const auto completed = target_state.run_serial_task ("spot-actor-joined", [&] {
            return admission.on_actor_joined (plan.target_spot_instance.get (), actor);
        });
        if (!completed) {
            throw framework_exception_t (completed.error_kind (),
                                         completed.error () != nullptr
                                           ? completed.error ()->what ()
                                           : "spot actor joined callback failed");
        }
    }
    if (leave.callback) {
        const auto completed = leave.context->run_serial_task ("spot-actor-leave", [&] {
            return leave.callback (leave.spot_instance.get (), actor);
        });
        if (!completed) {
            throw framework_exception_t (completed.error_kind (),
                                         completed.error () != nullptr
                                           ? completed.error ()->what ()
                                           : "spot actor leave callback failed");
        }
    }
}

task_t<void> spot_node_runtime_t::replay_actor_handoff_batch (actor_ref_t actor_ref,
                                                              std::vector<handoff_packet_t> backlog,
                                                              service_provider_t services,
                                                              std::string transfer_id,
                                                              bool reuse_active_actor_queue,
                                                              std::function<bool ()> stop_requested)
{
    if (backlog.empty ())
        co_return;
    order_bound_session_handoff (backlog);
    const auto key = actor_key (actor_ref);
    std::optional<spot_id_t> location;
    std::shared_ptr<spot_context_state_t> context_state;
    std::shared_ptr<void> actor_instance;
    std::type_index actor_type{typeid (void)};
    _state->lane.run ([&] {
        if (const auto local = _state->actor_spot_ids.find (key);
            local != _state->actor_spot_ids.end ()) {
            location = local->second;
        } else if (const auto routed = _state->actor_routes.find (key);
                   routed != _state->actor_routes.end ()) {
            location = routed->second.spot_id;
        }
        const auto factory = _state->actor_factories.find (
          std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor_ref)));
        const auto actor = _state->actor_instances.find (key);
        if (location && factory != _state->actor_factories.end ()
            && actor != _state->actor_instances.end () && actor->second) {
            const auto context = find_context_core (*location);
            if (context && context->_state->spot_instance) {
                context_state = context->_state;
                actor_instance = actor->second;
                actor_type = factory->second.actor_type;
            }
        }
    }).get ();

    auto terminal_failure = [node = _state, actor_ref,
                             transfer_id] (handoff_packet_t &packet, framework_error_kind_t kind,
                                           std::string message) -> task_t<void> {
        spot_inbound_message_t metadata;
        metadata.content_type = std::move (packet.content_type);
        metadata.values = std::move (packet.metadata);
        const auto terminal_route =
          packet.is_request ? handoff_terminal_route (metadata.values) : std::nullopt;
        const auto failure = result_t<zlink::message_t>::failure (kind, std::move (message));
        if (spot_node_runtime_t (node).actor_transfer_marker_enabled ()) {
            spot_node_runtime_t (node).emit_actor_transfer_marker ("handoff_replay_failed",
                                                                   actor_ref, transfer_id);
        }
        if (!packet.is_request)
            co_return;
        try {
            if (!(co_await send_handoff_terminal (node, terminal_route, failure))) {
                if (spot_node_runtime_t (node).actor_transfer_marker_enabled ()) {
                    spot_node_runtime_t (node).emit_actor_transfer_marker (
                      "handoff_terminal_send_failed", actor_ref, transfer_id);
                }
            }
        }
        catch (...) {
            if (spot_node_runtime_t (node).actor_transfer_marker_enabled ()) {
                spot_node_runtime_t (node).emit_actor_transfer_marker (
                  "handoff_terminal_send_failed", actor_ref, transfer_id);
            }
        }
    };
    if (!context_state || !location || !context_state->channel_runtime
        || !context_state->channel_runtime->serializers) {
        for (auto &packet : backlog) {
            co_await terminal_failure (packet, framework_error_kind_t::unavailable,
                                       "target Actor handoff replay owner is unavailable");
        }
        co_return;
    }
    for (auto &packet : backlog) {
        if (stop_requested && stop_requested ()) {
            co_await terminal_failure (packet, framework_error_kind_t::shutting_down,
                                       "target Actor handoff replay was cancelled during shutdown");
            continue;
        }
        if (spot_node_runtime_t (_state).actor_transfer_marker_enabled ()) {
            spot_node_runtime_t (_state).emit_actor_transfer_marker (
              "handoff_replay_enqueued", actor_ref, transfer_id, *location);
        }
        spot_inbound_message_t metadata;
        metadata.content_type = std::move (packet.content_type);
        metadata.values = std::move (packet.metadata);
        const auto terminal_route =
          packet.is_request ? handoff_terminal_route (metadata.values) : std::nullopt;
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
        const auto kind =
          packet.is_request ? spot_handler_kind_t::actor_request : spot_handler_kind_t::actor_send;
        result_t<zlink::message_t> result = result_t<zlink::message_t>::failure (
          framework_error_kind_t::internal_failure, "Actor handoff replay did not complete");
        try {
            result = result_t<zlink::message_t>::success (
              co_await spot_handler_registry_t (context_state)
                .invoke_erased (kind, packet.packet_name, {}, actor_type,
                                context_state->spot_instance.get (), actor_instance.get (),
                                services, *context_state->channel_runtime->serializers,
                                zlink::message_t::from (std::move (packet.payload)),
                                std::move (metadata), true, key, std::string (*location), {},
                                reuse_active_actor_queue
                                  ? spot_handler_registry_t::actor_queue_dispatch_t::current_turn
                                  : spot_handler_registry_t::actor_queue_dispatch_t::acquire));
        }
        catch (const framework_exception_t &error) {
            result = result_t<zlink::message_t>::failure (error.kind (), error.what ());
        }
        catch (const std::exception &error) {
            result = result_t<zlink::message_t>::failure (framework_error_kind_t::internal_failure,
                                                          error.what ());
        }
        if (!replay_request_id.empty ()) {
            const auto dedup_key = actor_request_dedup_key (key, replay_request_id);
            if (result)
                (void) _state->dispatched_request_replies.complete (dedup_key, result.value ());
            else
                (void) _state->dispatched_request_replies.erase (dedup_key);
        }
        if (!result && spot_node_runtime_t (_state).actor_transfer_marker_enabled ()) {
            spot_node_runtime_t (_state).emit_actor_transfer_marker ("handoff_replay_failed",
                                                                     actor_ref, transfer_id);
        }
        if (terminal_route) {
            try {
                if (!(co_await send_handoff_terminal (_state, terminal_route, result))) {
                    if (spot_node_runtime_t (_state).actor_transfer_marker_enabled ()) {
                        spot_node_runtime_t (_state).emit_actor_transfer_marker (
                          "handoff_terminal_send_failed", actor_ref, transfer_id);
                    }
                }
            }
            catch (...) {
                if (spot_node_runtime_t (_state).actor_transfer_marker_enabled ()) {
                    spot_node_runtime_t (_state).emit_actor_transfer_marker (
                      "handoff_terminal_send_failed", actor_ref, transfer_id);
                }
            }
        }
    }
}

void spot_node_runtime_t::enqueue_actor_handoff_replay (const actor_ref_t &actor_ref,
                                                        std::vector<handoff_packet_t> backlog,
                                                        service_provider_t &services,
                                                        std::string transfer_id)
{
    auto replay_owner = std::make_shared<spot_node_runtime_t> (_state);
    auto replay = replay_owner->replay_actor_handoff_batch (
      actor_ref, std::move (backlog), services, std::move (transfer_id), false, {});
    // Register the terminal observer before this fire-and-forget entry returns:
    // the replay coroutine owns its serial handler and terminal-send continuations.
    detail::observe_task_completion (replay, [replay_owner] (const result_t<void> &) {});
}

void spot_node_runtime_t::replay_actor_handoff_until_move_closed (const actor_ref_t &actor_ref,
                                                                  std::string transfer_id)
{
    const auto key = actor_key (actor_ref);
    const auto replay_id = _state->actor_transfer_coordinator.transfer_id (key).value_or (
      transfer_id.empty () ? key : transfer_id);
    auto root_services = _state->lane.run ([&] { return _state->root_services; }).get ();
    for (;;) {
        auto replay = _state->actor_transfer_coordinator.finish_move_replay (key);
        if (!replay.backlog.empty ()) {
            if (root_services) {
                enqueue_actor_handoff_replay (actor_ref, std::move (replay.backlog),
                                              *root_services, replay_id);
            } else if (actor_transfer_marker_enabled ()) {
                emit_actor_transfer_marker ("handoff_replay_unavailable", actor_ref, replay_id);
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
    const auto key = actor_key (actor_ref);
    auto join_snapshot = actor_join_state_snapshot (actor_ref, spot_id, request);
    if (!join_snapshot.context) {
        return result_t<actor_join_reply_t>::failure (framework_error_kind_t::not_found,
                                                      "target spot is not registered");
    }
    if (!join_snapshot.registration) {
        return result_t<actor_join_reply_t>::failure (framework_error_kind_t::not_found,
                                                      "actor factory is not registered");
    }
    auto &context = *join_snapshot.context;
    auto &registration = *join_snapshot.registration;
    const auto committed = ::zlink::framework::detail::actor_ref_access_t::make (
      node_rid_t::from_string (join_snapshot.node_rid),
      std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor_ref)),
      std::string (actor_ref.actor_id ().value ()), actor_ref.object_generation ());
    /* Registration is double-checked: an already-registered actor is taken
     * in one node state turn without touching the factory, and only a first
     * registration constructs outside the lane because the factory is user
     * code. The map entry and its identity index entry are then installed
     * together in one turn, so a
     * concurrent destroy never sees one without the other. */
    std::shared_ptr<void> actor_instance = join_snapshot.actor_instance;
    bool application_callback_crossed = false;
    if (!actor_instance) {
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
            auto committed_context = actor_context_t (actor_context._state, committed, 0,
                                                      join_snapshot.mesh_name);
            created_instance = registration.create_context_instance (std::move (committed_context));
        } else {
            created_instance =
              registration.create_instance (std::string (actor_ref.actor_id ().value ()));
        }
        application_callback_crossed = true;
        if (!created_instance) {
            return result_t<actor_join_reply_t>::failure (framework_error_kind_t::not_found,
                                                          "actor factory returned null");
        }
        actor_instance = install_actor_instance (actor_ref, key, std::move (created_instance));
        if (!actor_instance) {
            return result_t<actor_join_reply_t>::failure (framework_error_kind_t::not_found,
                                                          "actor has been destroyed");
        }
    }
    if (actor_snapshot) {
        registration.deserialize_instance (actor_instance.get (), *actor_snapshot,
                                           *join_snapshot.serializers);
        application_callback_crossed = true;
    }
    if (application_callback_crossed) {
        auto refreshed_admission = actor_admission (
          context, registration.actor_type, spot_id, actor_ref);
        if (!refreshed_admission) {
            return detail::propagate_failure<actor_join_reply_t> (
              refreshed_admission, "actor admission failed");
        }
        join_snapshot.admission.emplace (std::move (refreshed_admission.value ()));
    }
    if (!join_snapshot.admission) {
        report_spot_dispatch_error (
          _state, dispatch_error_surface_t::spot_actor, dispatch_message_kind_t::actor_request,
          dispatch_error_reason_t::handler_missing, dispatch_error_action_t::reply_error,
          "actor.join", std::nullopt, std::string (spot_id),
          std::string (actor_ref.actor_id ().value ()));
        return result_t<actor_join_reply_t>::failure (
          framework_error_kind_t::not_found, "spot actor join callback is not registered");
    }

    auto &admission_callbacks = *join_snapshot.admission;
    auto &serializers = *join_snapshot.serializers;
    const auto response =
      admission_callbacks.join (join_snapshot.spot_instance.get (), actor_ref.actor_id ().value (),
                                request, serializers);
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
      _state, actor_ref, join_snapshot.source_spot_id, committed, *context._state,
      claimed_location);
    if (!location_claim) {
        replay_actor_handoff_until_move_closed (actor_ref, key);
        return result_t<actor_join_reply_t>::failure (
          location_claim.error_kind (), location_claim.error () ? location_claim.error ()->what ()
                                                                : "actor location claim failed");
    }
    bool authority_committed = false;
    auto fail_local_commit = [&] {
        if (!authority_committed
            && (claimed_location || join_snapshot.source_spot_id.empty ())) {
            release_actor_location (_state, committed);
        }
        if (authority_committed) {
            _state->actor_transfer_coordinator.mark_reconcile (key,
                                                               join_snapshot.message_follow_duration);
            replay_actor_handoff_until_move_closed (committed, key);
        } else {
            replay_actor_handoff_until_move_closed (actor_ref, key);
        }
    };
    try {
        if (!registration.create_context_instance) {
            registration.configure_instance (actor_instance.get (), committed, nullptr);
        }
        commit_accepted_actor_join (
          key, context, committed, registration.actor_type,
          actor_instance.get (), admission_callbacks, true, request,
          /* The operation id string only feeds the actor-transfer marker —
           * do not assemble it when the marker gate is off (spec 26 §4). */
          !actor_transfer_marker_enabled () || (operation_id_high == 0 && operation_id_low == 0)
            ? std::string{}
            : std::to_string (operation_id_high) + ":" + std::to_string (operation_id_low),
          authority_committed);
        if (join_snapshot.has_root_services) {
            const auto state = _state;
            if (!framework_worker_executor (_state)->try_submit_internal ([state, committed, key] {
                    spot_node_runtime_t (state).replay_actor_handoff_until_move_closed (committed,
                                                                                        key);
                })) {
                throw framework_exception_t (framework_error_kind_t::unavailable,
                                             "actor handoff replay executor is stopping");
            }
        } else {
            const auto completed =
              _state->actor_transfer_coordinator.complete_move_and_take_backlog (key);
            if (!completed.backlog.empty () && actor_transfer_marker_enabled ()) {
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
    auto drain_flag = _state->lane.run ([&] { return _state->drain_flag; }).get ();
    if (drain_flag && drain_flag->load (std::memory_order_acquire)) {
        return result_t<actor_join_reply_t>::failure (
          framework_error_kind_t::rejected, "spot node is draining and rejects new actor joins");
    }
    if (::zlink::framework::detail::actor_ref_access_t::empty (actor_ref)) {
        return result_t<actor_join_reply_t>::failure (framework_error_kind_t::not_found,
                                                      "actor ref is empty");
    }
    const auto key = actor_key (actor_ref);
    auto join_snapshot = actor_join_state_snapshot (actor_ref, spot_id, request);
    if (!join_snapshot.context) {
        return result_t<actor_join_reply_t>::failure (framework_error_kind_t::not_found,
                                                      "target spot is not registered");
    }
    if (!join_snapshot.registration) {
        return result_t<actor_join_reply_t>::failure (framework_error_kind_t::not_found,
                                                      "actor factory is not registered");
    }
    auto &context = *join_snapshot.context;
    auto &registration = *join_snapshot.registration;
    const auto committed = ::zlink::framework::detail::actor_ref_access_t::make (
      node_rid_t::from_string (join_snapshot.node_rid),
      std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor_ref)),
      std::string (actor_ref.actor_id ().value ()), actor_ref.object_generation ());
    /* Registration is double-checked: an already-registered actor is taken
     * in one node state turn without touching the factory, and only a first
     * registration constructs outside the lane because the factory is user
     * code. The map entry and its identity index entry are then installed
     * together in one turn, so a
     * concurrent destroy never sees one without the other. */
    std::shared_ptr<void> actor_instance = join_snapshot.actor_instance;
    bool application_callback_crossed = false;
    if (!actor_instance) {
        std::shared_ptr<void> created_instance;
        if (registration.create_context_instance) {
            if (!actor_context._state) {
                return result_t<actor_join_reply_t>::failure (
                  framework_error_kind_t::not_configured,
                  "Actor factory requires a Framework Actor context");
            }
            auto committed_context = actor_context_t (actor_context._state, committed, 0,
                                                      join_snapshot.mesh_name);
            created_instance = registration.create_context_instance (std::move (committed_context));
        } else {
            created_instance =
              registration.create_instance (std::string (actor_ref.actor_id ().value ()));
        }
        application_callback_crossed = true;
        if (!created_instance) {
            return result_t<actor_join_reply_t>::failure (framework_error_kind_t::not_found,
                                                          "actor factory returned null");
        }
        actor_instance = install_actor_instance (actor_ref, key, std::move (created_instance));
        if (!actor_instance) {
            return result_t<actor_join_reply_t>::failure (framework_error_kind_t::not_found,
                                                          "actor has been destroyed");
        }
    }
    if (!registration.create_context_instance) {
        auto committed_context =
          actor_context_t (actor_context._state, committed, 0, join_snapshot.mesh_name);
        registration.configure_instance (actor_instance.get (), committed, &committed_context);
        application_callback_crossed = true;
    }

    if (application_callback_crossed) {
        auto refreshed_admission = actor_admission (
          context, registration.actor_type, spot_id, actor_ref);
        if (!refreshed_admission) {
            return detail::propagate_failure<actor_join_reply_t> (
              refreshed_admission, "actor admission failed");
        }
        join_snapshot.admission.emplace (std::move (refreshed_admission.value ()));
    }
    if (!join_snapshot.admission) {
        report_spot_dispatch_error (
          _state, dispatch_error_surface_t::spot_actor, dispatch_message_kind_t::actor_request,
          dispatch_error_reason_t::handler_missing, dispatch_error_action_t::reply_error,
          "actor.join", std::nullopt, std::string (spot_id),
          std::string (actor_ref.actor_id ().value ()));
        return result_t<actor_join_reply_t>::failure (
          framework_error_kind_t::not_found, "spot actor join callback is not registered");
    }

    auto &admission_callbacks = *join_snapshot.admission;
    auto &serializers = *join_snapshot.serializers;
    const auto response =
      admission_callbacks.join (join_snapshot.spot_instance.get (), actor_ref.actor_id ().value (),
                                request, serializers);
    if (!response.accepted) {
        if (!registration.create_context_instance) {
            registration.configure_instance (actor_instance.get (), actor_ref, &actor_context);
        }
        return result_t<actor_join_reply_t>::success (
          actor_join_reply_t{1, actor_ref, framework_reply_or_empty (response.reply, serializers)});
    }

    bool claimed_location = false;
    const auto location_claim = claim_pending_actor_location_before_activation (
      _state, actor_ref, spot_id_t{}, committed, *context._state, claimed_location);
    if (!location_claim) {
        return result_t<actor_join_reply_t>::failure (
          location_claim.error_kind (), location_claim.error () ? location_claim.error ()->what ()
                                                                : "actor location claim failed");
    }
    bool authority_committed = false;
    try {
        commit_accepted_actor_join (key, context, committed, registration.actor_type,
                                    actor_instance.get (), admission_callbacks, false, request, key,
                                    authority_committed);
    }
    catch (...) {
        if (claimed_location && !authority_committed) {
            release_actor_location (_state, committed);
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

std::size_t
spot_node_runtime_t::cleanup_expired_actor_admissions_at (std::chrono::steady_clock::time_point now)
{
    const auto expired = _state->actor_transfer_coordinator.cleanup_expired (now);
    if (actor_transfer_marker_enabled ()) {
        for (const auto &entry : expired) {
            emit_actor_transfer_marker ("pending_admission_expired", entry.admission.source_actor,
                                        entry.transfer_id, entry.admission.target_spot_id);
        }
    }
    std::size_t removed = expired.size ();
    std::vector<spot_node_builder_state_t::pending_remote_source_cleanup_t> cleaned_sources;
    std::vector<actor_ref_t> released_sources;
    _state->lane.run ([&] {
        for (auto found = _state->pending_remote_source_cleanups.begin ();
             found != _state->pending_remote_source_cleanups.end ();) {
            if (found->not_before > now) {
                ++found;
                continue;
            }
            // Spec 15: source membership cleanup (erasing the local Actor
            // instance) must not run ahead of the OnLeave callback actually
            // executing. OnLeave arrives asynchronously from the target as a
            // one-way command (submit_remote_actor_leave) that only queues
            // the callback and returns; it needs this Actor instance to
            // still be registered when it runs. Hold the erase until the
            // callback has completed (leave_completed), bounded by
            // leave_deadline in case the notification is genuinely lost
            // (target crash, partition) or there is no OnLeave handler to
            // await, so this cannot leak forever.
            if (!found->leave_completed && found->leave_deadline > now) {
                ++found;
                continue;
            }
            const auto key = actor_key (found->source_actor);
            const auto current_fence = _state->actor_authority_fences.find (key);
            // An in-flight transfer for the key can be a return admission
            // (A→B→A). Keep both the retained source instance and this exact
            // cleanup/fence evidence until materialization consumes the
            // remnant; dropping only the cleanup here leaves an unprovable
            // actor_instances conflict on the returning target.
            if (_state->actor_transfer_coordinator.blocks_dispatch (key)) {
                ++found;
                continue;
            }
            const bool actor_has_newer_local_authority =
              (current_fence != _state->actor_authority_fences.end ()
               && current_fence->second != found->source_fence);
            if (!actor_has_newer_local_authority) {
                _state->actor_instances.erase (key);
                detail::erase_actor_instance_index_unlocked (
                  *_state,
                  ::zlink::framework::detail::actor_ref_access_t::actor_type (found->source_actor),
                  found->source_actor.actor_id ().value ());
                released_sources.push_back (found->source_actor);
                if (current_fence != _state->actor_authority_fences.end ()
                    && current_fence->second == found->source_fence) {
                    _state->actor_authority_fences.erase (current_fence);
                }
            }
            cleaned_sources.push_back (std::move (*found));
            found = _state->pending_remote_source_cleanups.erase (found);
            ++removed;
        }
    }).get ();
    for (const auto &actor : released_sources)
        release_actor_location (_state, actor);
    if (actor_transfer_marker_enabled ()) {
        for (const auto &cleanup : cleaned_sources) {
            emit_actor_transfer_marker ("source_cleanup", cleanup.source_actor, cleanup.transfer_id,
                                        cleanup.target_spot_id);
        }
    }
    const auto removed_message_follow_routes =
      _state->actor_transfer_coordinator.remove_expired_message_follow (now);
    std::vector<std::pair<actor_ref_t, std::string>> removed_route_markers;
    if (!removed_message_follow_routes.empty ()) {
        _state->lane.run ([&] {
            const auto local_node_rid =
              node_rid_t::from_string (detail::effective_spot_node_rid (_state->snapshot));
            for (const auto &entry : removed_message_follow_routes) {
                const auto &key = entry.actor_key;
                // Message Follow ended (§10.4-3): remove the retained route. The
                // current local authority and any other retained source fences
                // remain independent from this expired source route.
                if (!_state->actor_authority_fences.contains (key)
                    && !_state->actor_transfer_coordinator.has_message_follow_route (key)) {
                    _state->actor_routes.erase (key);
                    _state->native_actors.erase (key);
                }
                const auto separator = key.find (':');
                if (separator != std::string::npos) {
                    removed_route_markers.emplace_back (
                      ::zlink::framework::detail::actor_ref_access_t::make (
                        local_node_rid, key.substr (0, separator), key.substr (separator + 1),
                        entry.source_fence.object_generation),
                      entry.transfer_id.empty () ? key : entry.transfer_id);
                }
                ++removed;
            }
        }).get ();
    }
    if (actor_transfer_marker_enabled ()) {
        for (const auto &[actor_ref, transfer_id] : removed_route_markers)
            emit_actor_transfer_marker ("message_follow_route_removed", actor_ref, transfer_id);
    }
    // A reconcile-phase move (genuinely ambiguous FINALIZE outcome, spec 28)
    // is bounded by reconcile_deadline (move_state_t) so it cannot park
    // requests in the backlog forever. Once past relay-ready, FINALIZE may
    // already have reached the target, so a stuck reconcile must never
    // blindly replay locally on timer expiry (spec 28 relay-ready
    // irreversibility) -- reconcile each expired one against the Location
    // Store's actual authority instead: adopt the target route if it
    // committed, restore locally only if the store still shows the source
    // as owner, and otherwise fast-fail whatever is parked and remain
    // unavailable (never silent parking).
    const auto expired_reconciles = _state->actor_transfer_coordinator.reconcile_keys_expired (now);
    for (const auto &expired : expired_reconciles) {
        const auto &key = expired.actor_key;
        const auto separator = key.find (':');
        if (separator == std::string::npos)
            continue;
        const auto [generation, local_node_rid] = _state->lane.run ([&] {
            std::uint64_t value = 0;
            if (const auto found = _state->actor_generations.find (key);
                found != _state->actor_generations.end ()) {
                value = found->second;
            }
            return std::make_pair (
              value, node_rid_t::from_string (detail::effective_spot_node_rid (_state->snapshot)));
        }).get ();
        const auto actor_ref = ::zlink::framework::detail::actor_ref_access_t::make (
          local_node_rid, key.substr (0, separator), key.substr (separator + 1), generation);

        if (!expired.context) {
            // No target identity was captured for this reconcile (a call
            // site outside spec 28's relay-ready-ambiguous FINALIZE case,
            // e.g. a target-local post-authority-commit failure): unchanged
            // prior behavior, not the Location-Store-reconciled outcomes
            // below, which require the captured identity to compare against.
            replay_actor_handoff_until_move_closed (actor_ref, key);
            ++removed;
            continue;
        }

        enum class reconcile_outcome_t
        {
            target_committed,
            source_owns,
            indeterminate
        };
        auto outcome = reconcile_outcome_t::indeterminate;
        {
            const auto relocation_authority =
              _state->lane.run ([&] { return _state->relocation_authority; }).get ();
            if (!relocation_authority) {
                outcome = reconcile_outcome_t::indeterminate;
            } else {
                try {
                    const auto current = relocation_authority->read (
                      runtime::stateful::object_kind_t::actor, key.substr (separator + 1));
                    if (!current) {
                        outcome = reconcile_outcome_t::source_owns;
                    } else if (current->target.key == expired.context->target_fence.actor_id
                               && current->target.object_generation
                                    == expired.context->target_fence.object_generation
                               && current->target.node_id
                                    == std::string (
                                      expired.context->target_actor.node_rid ().value ())
                               && current->target.authority_owner_generation
                                    == expired.context->target_fence.authority_owner_generation) {
                        outcome = reconcile_outcome_t::target_committed;
                    } else if (current->source.key == expired.context->source_fence.actor_id
                               && current->source.object_generation
                                    == expired.context->source_fence.object_generation
                               && current->source.authority_owner_generation
                                    == expired.context->source_fence.authority_owner_generation) {
                        outcome = reconcile_outcome_t::source_owns;
                    } else {
                        outcome = reconcile_outcome_t::indeterminate;
                    }
                }
                catch (...) {
                    outcome = reconcile_outcome_t::indeterminate;
                }
            }
        }
        if (actor_transfer_marker_enabled ()) {
            emit_actor_transfer_marker (
              outcome == reconcile_outcome_t::target_committed ? "reconcile_deadline_adopted_target"
              : outcome == reconcile_outcome_t::source_owns    ? "reconcile_deadline_restored_local"
                                                               : "reconcile_deadline_fast_failed",
              actor_ref, key);
        }
        if (outcome == reconcile_outcome_t::target_committed) {
            const auto state = _state;
            const auto ctx = *expired.context;
            const auto transfer_id = ctx.transfer_id.empty () ? key : ctx.transfer_id;
            bool submitted = false;
            const auto has_root_services =
              _state->lane.run ([&] { return _state->root_services.has_value (); }).get ();
            if (has_root_services) {
                submitted = framework_worker_executor (_state)->try_submit_internal (
                  [state, actor_ref, ctx, transfer_id] {
                      auto owner = std::make_shared<spot_node_runtime_t> (state);
                      auto task = owner->complete_remote_actor_transfer (
                        actor_ref, ctx.target_actor, ctx.target_route, ctx.source_fence,
                        ctx.target_fence, transfer_id);
                      detail::observe_task_completion (task, [owner] (const result_t<void> &) {});
                  });
            }
            if (!submitted) {
                // Executor unavailable: cannot safely adopt the target route
                // without a place to drain the backlog through, so fall back
                // to the same fast-fail as the indeterminate case.
                auto task = fast_fail_reconcile_backlog (actor_ref, key);
                detail::observe_task_completion (task, [] (const result_t<void> &) {});
            }
        } else if (outcome == reconcile_outcome_t::source_owns) {
            replay_actor_handoff_until_move_closed (actor_ref, key);
        } else {
            auto task = fast_fail_reconcile_backlog (actor_ref, key);
            detail::observe_task_completion (task, [] (const result_t<void> &) {});
        }
        ++removed;
    }
    return removed;
}

bool spot_node_runtime_t::stage_session_relocation_route (
  const std::string &transfer_id,
  std::vector<std::uint8_t> route,
  std::string actor_type,
  std::uint64_t target_owner_lease_generation)
{
    return _state->actor_transfer_coordinator.stage_session_relocation_route (
      transfer_id, std::move (route), std::move (actor_type), target_owner_lease_generation);
}

bool spot_node_runtime_t::commit_session_relocation_route_authority (
  const std::string &transfer_id,
  std::uint64_t previous_authority_owner_generation,
  std::uint64_t target_authority_owner_generation)
{
    return _state->actor_transfer_coordinator.commit_session_relocation_route_authority (
      transfer_id, previous_authority_owner_generation, target_authority_owner_generation);
}

bool spot_node_runtime_t::adopt_committed_actor_relocation_authority (
  const runtime::stateful::object_ref_t &target,
  std::uint64_t target_node_generation,
  std::uint64_t target_owner_lease_generation)
{
    if (target.kind != runtime::stateful::object_kind_t::actor || target.key.empty ()
        || target.object_generation == 0 || target.authority_owner_generation == 0
        || target.node_id.empty () || target_node_generation == 0
        || target_owner_lease_generation == 0)
        return false;

    return _state->lane.run ([&] {
        const auto type = _state->actor_types_by_id.find (target.key);
        if (type == _state->actor_types_by_id.end ())
            return false;
        const auto fence = _state->actor_authority_fences.find (type->second + ":" + target.key);
        if (fence == _state->actor_authority_fences.end ())
            return false;
        const auto target_node = zlink::routing_id_t::from (target.node_id).to_bytes ();
        auto &current = fence->second;
        if (current.actor_id != target.key || current.object_generation != target.object_generation
            || current.target_node_routing_id != target_node
            || current.target_node_generation != target_node_generation
            || current.authority_owner_generation > target.authority_owner_generation
            || (current.authority_owner_generation == target.authority_owner_generation
                && current.owner_lease_generation != 0
                && current.owner_lease_generation != target_owner_lease_generation)) {
            return false;
        }
        current.authority_owner_generation = target.authority_owner_generation;
        current.owner_lease_generation = target_owner_lease_generation;
        return true;
    }).get ();
}

task_t<bool> spot_node_runtime_t::activate_session_relocation_route (const std::string &transfer_id)
{
    const auto admission =
      _state->actor_transfer_coordinator.session_relocation_admission (transfer_id);
    if (!admission || admission->session_relocation_route.empty ())
        co_return false;
    runtime::protocol::session_relocation_route_t route;
    try {
        route =
          runtime::protocol::decode_session_relocation_route (admission->session_relocation_route);
    }
    catch (...) {
        co_return false;
    }
    if (route.route.action != runtime::protocol::session_relocation_route_action_t::commit
        || admission->session_relocation_committed_previous_authority_owner_generation == 0
        || admission->session_relocation_committed_target_authority_owner_generation
             <= admission->session_relocation_committed_previous_authority_owner_generation) {
        co_return false;
    }
    route.route.previous_authority_owner_generation =
      admission->session_relocation_committed_previous_authority_owner_generation;
    route.route.target_authority_owner_generation =
      admission->session_relocation_committed_target_authority_owner_generation;
    const auto native = native_node ();
    if (!native)
        co_return false;
    co_return co_await native->route_session_remote (
      zlink::routing_id_t::from (route.session_owner_node_routing_id), std::move (route));
}

bool spot_node_runtime_t::remove_actor_message_follow (
  const actor_ref_t &actor_ref,
  const runtime::protocol::actor_route_fence_t &source_fence,
  const runtime::protocol::actor_route_fence_t &target_fence)
{
    const auto key = actor_key (actor_ref);
    const auto removed =
      _state->actor_transfer_coordinator.remove_message_follow (key, source_fence, target_fence);
    if (!removed)
        return true;
    _state->lane.run ([&] {
        if (!_state->actor_authority_fences.contains (key)
            && !_state->actor_transfer_coordinator.has_message_follow_route (key)) {
            _state->actor_routes.erase (key);
            _state->native_actors.erase (key);
        }
    }).get ();
    if (actor_transfer_marker_enabled ()) {
        emit_actor_transfer_marker ("message_follow_route_removed", actor_ref,
                                    removed->transfer_id.empty () ? key : removed->transfer_id);
    }
    return true;
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
    return _state->lane.run ([&] {
        const auto type = _state->actor_types_by_id.find (std::string (actor_id));
        if (type == _state->actor_types_by_id.end ())
            return false;
        return _state->actor_transfer_coordinator.blocks_dispatch (type->second + ":"
                                                                   + std::string (actor_id));
    }).get ();
}

std::optional<std::string> spot_node_runtime_t::resolve_actor_type (std::string_view actor_id) const
{
    return _state->lane.run ([&] () -> std::optional<std::string> {
        const auto found = _state->actor_types_by_id.find (std::string (actor_id));
        if (found == _state->actor_types_by_id.end ())
            return std::nullopt;
        return found->second;
    }).get ();
}

std::optional<std::uint64_t>
spot_node_runtime_t::resolve_actor_membership_epoch (std::string_view actor_id) const
{
    return _state->lane.run ([&] () -> std::optional<std::uint64_t> {
        const auto found = _state->core_actor_membership_epochs.find (std::string (actor_id));
        if (found == _state->core_actor_membership_epochs.end ())
            return std::nullopt;
        return found->second;
    }).get ();
}

actor_context_t spot_node_runtime_t::default_actor_context ()
{
    return actor_context_t{};
}

std::optional<spot_id_t> spot_node_runtime_t::resolve_entry_spot_id () const
{
    return _state->lane.run ([&] () -> std::optional<spot_id_t> {
        if (!_state->snapshot.entry_spot_name)
            return std::nullopt;
        const auto entry_id = _state->spot_ids_by_name.find (*_state->snapshot.entry_spot_name);
        if (entry_id == _state->spot_ids_by_name.end ())
            return std::nullopt;
        return entry_id->second;
    }).get ();
}

void spot_node_runtime_t::set_message_follow_duration (std::chrono::milliseconds duration)
{
    _state->lane.run ([&] { _state->message_follow_duration = duration; }).get ();
}

void spot_node_runtime_t::bind_relocation_store (
  std::shared_ptr<runtime::stateful::relocation_store_port_t> store)
{
    _state->lane.run ([&] { _state->relocation_store = std::move (store); }).get ();
}

void spot_node_runtime_t::bind_relocation_authority (
  std::shared_ptr<runtime::stateful::authority_relocation_port_t> authority)
{
    _state->lane.run ([&] { _state->relocation_authority = std::move (authority); }).get ();
}

std::vector<std::uint8_t>
spot_node_runtime_t::capture_spot_relocation_state (const runtime::stateful::object_ref_t &spot,
                                                    const std::string &stable_type,
                                                    std::stop_token cancellation) const
{
    if (spot.kind == runtime::stateful::object_kind_t::actor) {
        detail::spot_node_builder_state_t::actor_factory_registration_t factory;
        std::shared_ptr<void> instance;
        _state->lane.run ([&] {
            const auto configured = _state->actor_factories.find (stable_type);
            const auto materialized = _state->actor_instances.find (stable_type + ":" + spot.key);
            if (configured == _state->actor_factories.end ()
                || materialized == _state->actor_instances.end () || !materialized->second) {
                throw framework_exception_t (framework_error_kind_t::not_found,
                                             "Relocation source Actor is not materialized");
            }
            factory = configured->second;
            instance = materialized->second;
        }).get ();
        if (factory.relocation.kind != detail::factory_relocation_kind_t::preserve_state)
            return {};
        const auto capture = factory.relocation.capture;
        if (!capture) {
            throw framework_exception_t (
              framework_error_kind_t::not_configured,
              "State-preserving Actor relocation has no capture callback");
        }
        const auto captured = capture (instance.get (), cancellation).result ();
        if (!captured) {
            throw framework_exception_t (captured.error_kind (),
                                         captured.error () != nullptr
                                           ? captured.error ()->what ()
                                           : "Actor relocation capture failed");
        }
        if (captured.value ().size () > max_spot_relocation_state_bytes) {
            throw framework_exception_t (framework_error_kind_t::rejected,
                                         "Actor relocation state exceeds the 64 MiB limit");
        }
        std::vector<std::uint8_t> output;
        output.reserve (captured.value ().size ());
        for (const auto value : captured.value ())
            output.push_back (std::to_integer<std::uint8_t> (value));
        return output;
    }

    std::shared_ptr<spot_context_state_t> context;
    detail::factory_relocation_configuration_t relocation;
    _state->lane.run ([&] {
        const auto found = _state->spot_contexts_by_id.find (spot.key);
        const auto configured = _state->spot_factory_relocations.find (stable_type);
        if (found == _state->spot_contexts_by_id.end ()
            || found->second._state->spot_name != stable_type
            || configured == _state->spot_factory_relocations.end ()
            || !found->second._state->allows_relocation ()) {
            throw framework_exception_t (framework_error_kind_t::not_found,
                                         "Relocation source Spot is not materialized");
        }
        context = found->second._state;
        relocation = configured->second;
    }).get ();
    if (relocation.kind != detail::factory_relocation_kind_t::preserve_state)
        return {};
    const auto capture = relocation.capture;
    if (!capture || !context->spot_instance) {
        throw framework_exception_t (framework_error_kind_t::not_configured,
                                     "State-preserving Spot relocation has no capture callback");
    }
    std::vector<std::byte> payload;
    const auto captured =
      context->run_serial_task ("spot-relocation-capture", [&] () -> task_t<void> {
          payload = co_await capture (context->spot_instance.get (), cancellation);
      });
    if (!captured)
        throw framework_exception_t (captured.error_kind (), captured.error () != nullptr
                                                               ? captured.error ()->what ()
                                                               : "Spot relocation capture failed");
    std::vector<std::uint8_t> output;
    if (payload.size () > max_spot_relocation_state_bytes) {
        throw framework_exception_t (framework_error_kind_t::rejected,
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
        const auto admitted = _state->lane.run ([&] {
            const auto configured = _state->spot_factory_relocations.find (frozen.stable_type);
            if (configured == _state->spot_factory_relocations.end ())
                return false;
            if (_state->spot_contexts_by_id.contains (target.key)
                || _state->pending_spot_creations_by_id.contains (target.key)
                || _state->next_pending_spot_creation_reservation == 0)
                return false;
            reservation = _state->next_pending_spot_creation_reservation++;
            completion = std::make_shared<std::promise<void>> ();
            const auto inserted = _state->pending_spot_creations_by_id.emplace (
              target.key, detail::spot_node_builder_state_t::pending_spot_creation_t{
                            frozen.stable_type, completion->get_future ().share (), reservation});
            if (!inserted.second)
                return false;
            relocation = configured->second;
            return true;
        }).get ();
        if (!admitted)
            return false;
        if (frozen.application_state.size () > max_spot_relocation_state_bytes)
            throw std::length_error ("Spot relocation state exceeds 64 MiB");
        std::function<task_t<void> (void *)> staged_restore;
        if (relocation.kind == detail::factory_relocation_kind_t::preserve_state) {
            if (!relocation.restore)
                throw std::logic_error ("Spot relocation restore callback is missing");
            std::vector<std::byte> payload;
            payload.reserve (frozen.application_state.size ());
            for (const auto value : frozen.application_state)
                payload.push_back (static_cast<std::byte> (value));
            staged_restore = [relocation = std::move (relocation), payload = std::move (payload),
                              cancellation] (void *instance) mutable {
                return relocation.restore (instance, std::move (payload), cancellation);
            };
        } else if (relocation.kind == detail::factory_relocation_kind_t::recreate) {
            if (!frozen.application_state.empty ())
                throw std::logic_error ("Recreated Spot has application state");
            staged_restore = [] (void *) -> task_t<void> { co_return; };
        } else {
            throw std::logic_error ("Spot relocation policy does not permit restore");
        }
        const auto owned = _state->lane.run ([&] {
            const auto found = _state->pending_spot_creations_by_id.find (target.key);
            return found != _state->pending_spot_creations_by_id.end ()
                   && found->second.reservation == reservation;
        }).get ();
        if (!owned)
            throw std::logic_error ("Relocation Spot reservation ownership was lost");
        auto materialized = create_spot_context (
          frozen.stable_type, spot_id_t (target.key), zlink::message_t{},
          target.object_generation, target.mesh_name, std::move (staged_restore),
          target.authority_owner_generation);
        const auto created = materialized.state == spot_create_state_t::created;
        if (created && materialized.context._state
            && materialized.context._state->execution_mode == user_spot_execution_mode_t::spot_wide
            && materialized.context._state->relocation_coordination_mode
                 == spot_relocation_coordination_mode_t::application_signaled) {
            materialized.context._state->callback_lane.run ([state = materialized.context._state] {
                state->relocation_boundary_active = true;
                state->relocation_ready_deferred = true;
            }).get ();
        }
        const auto completed_reservation = _state->lane.run ([&] {
            const auto current = _state->pending_spot_creations_by_id.find (target.key);
            if (current == _state->pending_spot_creations_by_id.end ()
                || current->second.reservation != reservation) {
                return false;
            }
            _state->pending_spot_creations_by_id.erase (current);
            return true;
        }).get ();
        if (completed_reservation) {
            if (created)
                completion->set_value ();
            else
                completion->set_exception (std::make_exception_ptr (
                  framework_exception_t (framework_error_kind_t::internal_failure,
                                         "Relocation Spot activation was rejected")));
        }
        return created;
    }
    catch (...) {
        if (reservation != 0) {
            const auto error = std::current_exception ();
            const auto owned = _state->lane.run ([&] {
                const auto current = _state->pending_spot_creations_by_id.find (target.key);
                if (current == _state->pending_spot_creations_by_id.end ()
                    || current->second.reservation != reservation) {
                    return false;
                }
                _state->pending_spot_creations_by_id.erase (current);
                return true;
            }).get ();
            if (owned)
                completion->set_exception (error);
        }
    }
    return false;
}

std::optional<bool> spot_node_runtime_t::validate_actor_join_relocation_prepare (
  const runtime::protocol::relocation_prepare_t &prepare) const
{
    if (prepare.object.kind != runtime::protocol::relocation_object_kind_t::actor)
        return std::nullopt;
    const auto handoff_id = runtime::protocol::actor_join_handoff_id (prepare.relocation);
    const auto admission = _state->actor_transfer_coordinator.admission (handoff_id);
    if (!admission || !admission->canonical_user_spot_join)
        return std::nullopt;
    struct validation_projection_t
    {
        std::shared_ptr<spot_context_state_t> target;
        std::shared_ptr<service::mesh_node_t> native_node;
        bool matches = false;
    };
    const auto projection = _state->lane.run ([&] {
        validation_projection_t result;
        const auto target_context =
          _state->spot_contexts_by_id.find (std::string (admission->target_spot_id));
        if (target_context == _state->spot_contexts_by_id.end () || !target_context->second._state)
            return result;
        const auto &target = *target_context->second._state;
        result.target = target_context->second._state;
        if (target.node)
            result.native_node = target.node->native_node.lock ();
        const auto source_node =
          zlink::routing_id_t::from (std::string (admission->source_actor.node_rid ().value ()))
            .to_bytes ();
        const auto local_node = _state->snapshot.routing_id ? _state->snapshot.routing_id->to_bytes ()
                                                            : std::vector<std::uint8_t>{};
        result.matches =
          !local_node.empty () && !target.is_entry_spot () && !target.is_instance_spot ()
          && target.object_generation == admission->target_spot_generation
          && prepare.object.object_id == admission->source_actor.actor_id ().value ()
          && prepare.object.object_generation == admission->source_actor.object_generation ()
          && prepare.object.expected_authority_owner_generation
               == admission->source_actor_authority_owner_generation
          && prepare.source_node_routing_id == source_node
          && prepare.source_node_generation == admission->source_node_generation
          && prepare.coordinator.node_routing_id == source_node
          && prepare.coordinator.node_generation == admission->source_node_generation
          && prepare.coordinator.lease_generation == admission->source_owner_lease_generation
          && prepare.target.target_node_routing_id == local_node;
        return result;
    }).get ();
    if (!projection.target || !projection.matches)
        return false;
    const auto target_node_generation = projection.native_node
                                          ? projection.native_node->status ().lifecycle_generation ()
                                          : 0;
    return prepare.target.target_node_generation == target_node_generation;
}

bool spot_node_runtime_t::consume_actor_join_recovery (
  runtime::stateful::frozen_object_state_t &frozen,
  const runtime::stateful::object_ref_t &target,
  const runtime::protocol::relocation_prepare_t &prepare)
{
    std::optional<runtime::protocol::actor_join_recovery_t> recovery;
    auto recovery_record = frozen.pending_application.end ();
    try {
        for (auto current = frozen.pending_application.begin ();
             current != frozen.pending_application.end (); ++current) {
            std::optional<runtime::protocol::frozen_record_t> canonical;
            if (current->frozen_record)
                canonical = *current->frozen_record;
            else if (!current->payload.empty ())
                canonical = runtime::protocol::decode_frozen_record (current->payload);
            if (!canonical)
                continue;
            auto decoded = runtime::protocol::decode_actor_join_recovery_saved_work (*canonical);
            if (!decoded)
                continue;
            if (recovery)
                return false;
            recovery = std::move (*decoded);
            recovery_record = current;
        }
    }
    catch (...) {
        return false;
    }
    if (!recovery) {
        /* A matching command-40 admission is the canonical Join path.  It
         * cannot fall through to ordinary Actor relocation (and ultimately
         * the Entry Spot) without its durable ZLJR completion record.  A
         * non-Join/legacy relocation has no matching admission, so preserve
         * that path unchanged. */
        const auto join_prepare = validate_actor_join_relocation_prepare (prepare);
        return !join_prepare || !*join_prepare;
    }

    return _state->lane.run ([&] {
        const auto admission = _state->actor_transfer_coordinator.admission (recovery->handoff_id);
        const auto local_node =
          _state->snapshot.routing_id ? _state->snapshot.routing_id->to_bytes ()
                                      : std::vector<std::uint8_t>{};
        const auto valid =
          admission && !local_node.empty () && recovery->relocation == prepare.relocation
          && recovery->coordinator == prepare.coordinator
          && recovery->handoff_id == runtime::protocol::actor_join_handoff_id (prepare.relocation)
          && recovery->actor_id == frozen.owner.key && recovery->actor_type == frozen.stable_type
          && recovery->actor_generation == frozen.owner.object_generation
          && recovery->actor_authority_owner_generation == frozen.owner.authority_owner_generation
          && recovery->actor_authority_owner_generation
               != std::numeric_limits<std::uint64_t>::max ()
          && recovery->target_authority_owner_generation
               == recovery->actor_authority_owner_generation + 1
          && target.key == recovery->actor_id
          && target.object_generation == recovery->actor_generation
          && target.authority_owner_generation == recovery->target_authority_owner_generation
          && recovery->target_node_routing_id == local_node
          && recovery->target_node_generation == prepare.target.target_node_generation
          && recovery->target_spot_id == std::string (admission->target_spot_id)
          && recovery->target_spot_generation == admission->target_spot_generation
          && recovery->target_spot_authority_owner_generation
               == admission->target_spot_authority_owner_generation
          && recovery->reservation_token == admission->reservation_token
          && recovery->reserved_payload_bytes == admission->reserved_payload_bytes
          && recovery->actor_node_generation == admission->source_node_generation
          && recovery->expected_owner_lease_generation == admission->source_owner_lease_generation;
        if (!valid)
            return false;
        const auto key = actor_key (::zlink::framework::detail::actor_ref_access_t::make (
          node_rid_t::from_string (target.node_id), frozen.stable_type, target.key,
          target.object_generation));
        if (!_state->actor_join_relocation_recoveries
               .emplace (key,
                         detail::spot_node_builder_state_t::actor_join_relocation_recovery_t{
                           recovery->handoff_id, spot_id_t (recovery->source_spot_id),
                           admission->target_spot_id, recovery->target_node_generation,
                           recovery->target_spot_generation, admission->source_actor,
                           recovery->operation.high, recovery->operation.low, recovery->reply})
               .second)
            return false;
        frozen.pending_application.erase (recovery_record);
        return true;
    }).get ();
}

void spot_node_runtime_t::discard_actor_join_recovery (
  const std::string &stable_type, const runtime::stateful::object_ref_t &target) noexcept
{
    if (target.kind != runtime::stateful::object_kind_t::actor || stable_type.empty ()
        || target.key.empty ())
        return;
    try {
        _state->lane.run ([&] {
            const auto recovery =
              _state->actor_join_relocation_recoveries.find (stable_type + ":" + target.key);
            if (recovery == _state->actor_join_relocation_recoveries.end ())
                return;
            /* The admission is released with the entry so the coordinator does
             * not keep the reservation alive for a Join that never landed. */
            _state->actor_transfer_coordinator.fail_commit (recovery->second.handoff_id, false);
            _state->actor_join_relocation_recoveries.erase (recovery);
        }).get ();
    }
    catch (...) {
    }
}

std::optional<std::tuple<std::string, std::string, std::uint64_t>>
spot_node_runtime_t::actor_join_relocation_authority_spot (
  const runtime::stateful::object_ref_t &target) const
{
    if (target.kind != runtime::stateful::object_kind_t::actor)
        return std::nullopt;
    return _state->lane.run ([&] ()
                              -> std::optional<std::tuple<std::string, std::string, std::uint64_t>> {
        auto recovery = _state->actor_join_relocation_recoveries.end ();
        const auto type = _state->actor_types_by_id.find (target.key);
        if (type != _state->actor_types_by_id.end ()) {
            recovery =
              _state->actor_join_relocation_recoveries.find (type->second + ":" + target.key);
        }
        if (recovery == _state->actor_join_relocation_recoveries.end ()) {
            recovery = std::find_if (
              _state->actor_join_relocation_recoveries.begin (),
              _state->actor_join_relocation_recoveries.end (), [&target] (const auto &candidate) {
                  return candidate.second.source_actor.actor_id ().value () == target.key
                         && candidate.second.source_actor.object_generation ()
                              == target.object_generation;
              });
        }
        if (recovery == _state->actor_join_relocation_recoveries.end ())
            return std::nullopt;
        return std::tuple{std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (
                            recovery->second.source_actor)),
                          std::string (recovery->second.target_spot_id),
                          recovery->second.target_spot_generation};
    }).get ();
}

bool spot_node_runtime_t::materialize_relocation_state (
  const runtime::stateful::frozen_object_state_t &frozen,
  const runtime::stateful::object_ref_t &target,
  const std::optional<runtime::stateful::object_ref_t> &target_spot,
  std::stop_token cancellation)
{
    if (target.kind != runtime::stateful::object_kind_t::actor)
        return restore_spot_relocation_state (frozen, target, cancellation);

    detail::spot_node_builder_state_t::actor_factory_registration_t factory;
    std::shared_ptr<spot_context_state_t> context;
    actor_gateway_runtime_t *actor_gateway = nullptr;
    detail::spot_actor_admission_callbacks_t admission;
    bool deferred_join_completion = false;
    runtime::stateful::object_ref_t resolved_target_spot;
    std::optional<service_provider_t> root_services;
    const auto preparation_result = _state->lane.run ([&] () -> std::optional<bool> {
        // A single-Actor relocation unit (e.g. an Entry Spot Actor moving
        // alone) never carries a Spot on the wire (spec 28: the Entry Spot
        // is already present on the target node). Resolve this node's own
        // local Entry Spot as the target Spot in that case, the same way
        // the join path does.
        const auto actor_key_value = frozen.stable_type + ":" + target.key;
        const auto join_recovery = _state->actor_join_relocation_recoveries.find (actor_key_value);
        deferred_join_completion = join_recovery != _state->actor_join_relocation_recoveries.end ();
        if (target_spot) {
            if (target_spot->kind != runtime::stateful::object_kind_t::user_spot)
                return false;
            resolved_target_spot = *target_spot;
        } else if (join_recovery != _state->actor_join_relocation_recoveries.end ()) {
            const auto recovered_context =
              _state->spot_contexts_by_id.find (std::string (join_recovery->second.target_spot_id));
            if (recovered_context == _state->spot_contexts_by_id.end ()
                || !recovered_context->second._state)
                return false;
            const auto &spot = *recovered_context->second._state;
            resolved_target_spot =
              runtime::stateful::object_ref_t{runtime::stateful::object_kind_t::user_spot,
                                              std::string (spot.spot_id),
                                              spot.object_generation,
                                              spot.authority_owner_generation,
                                              spot.mesh_name,
                                              std::string (spot.node_rid.value ())};
        } else {
            if (!_state->snapshot.entry_spot_name) {
                // A maintenance/whole-node Actor import has no User-Spot
                // Join admission and no local Entry Spot ownership to
                // materialize. The stateful runtime already installed the
                // generic Actor record; leave that import on its established
                // path. A canonical routed Join may never take this branch.
                return std::make_optional (!deferred_join_completion
                                           && !_state->actor_factories.contains (
                                             frozen.stable_type));
            }
            const auto entry_id = _state->spot_ids_by_name.find (*_state->snapshot.entry_spot_name);
            if (entry_id == _state->spot_ids_by_name.end ())
                return false;
            const auto entry_context = _state->spot_contexts_by_id.find (entry_id->second);
            if (entry_context == _state->spot_contexts_by_id.end () || !entry_context->second._state
                || !entry_context->second._state->spot_instance)
                return false;
            const auto &entry_state = *entry_context->second._state;
            resolved_target_spot = runtime::stateful::object_ref_t{
              entry_state.is_instance_spot () ? runtime::stateful::object_kind_t::instance_spot
                                              : runtime::stateful::object_kind_t::user_spot,
              std::string (entry_state.spot_id),
              entry_state.object_generation,
              entry_state.authority_owner_generation,
              entry_state.mesh_name,
              std::string (entry_state.node_rid.value ())};
            if (resolved_target_spot.kind != runtime::stateful::object_kind_t::user_spot)
                return false;
        }
        if (deferred_join_completion) {
            if (resolved_target_spot.key != std::string (join_recovery->second.target_spot_id)
                || resolved_target_spot.object_generation
                     != join_recovery->second.target_spot_generation
                || !_state->actor_transfer_coordinator.begin_commit (
                  join_recovery->second.handoff_id, join_recovery->second.source_actor,
                  join_recovery->second.target_spot_id)) {
                return false;
            }
        }
        const auto configured = _state->actor_factories.find (frozen.stable_type);
        const auto found_context = _state->spot_contexts_by_id.find (resolved_target_spot.key);
        if (configured == _state->actor_factories.end ()
            || found_context == _state->spot_contexts_by_id.end () || !found_context->second._state
            || !found_context->second._state->spot_instance)
            return false;
        factory = configured->second;
        context = found_context->second._state;
        root_services = _state->root_services;
        return std::nullopt;
    }).get ();
    if (preparation_result)
        return *preparation_result;
    if (root_services)
        actor_gateway = &root_services->get_required<actor_gateway_runtime_t> ();

    if (frozen.application_state.size () > max_spot_relocation_state_bytes)
        return false;
    if (factory.relocation.kind == detail::factory_relocation_kind_t::disabled
        || factory.relocation.kind == detail::factory_relocation_kind_t::unspecified)
        return false;
    if (factory.relocation.kind == detail::factory_relocation_kind_t::recreate
        && !frozen.application_state.empty ())
        return false;
    if (factory.relocation.kind == detail::factory_relocation_kind_t::preserve_state
        && !factory.restore)
        return false;

    const auto committed = ::zlink::framework::detail::actor_ref_access_t::make (
      node_rid_t::from_string (target.node_id), frozen.stable_type, target.key,
      target.object_generation);
    actor_gateway_runtime_t local_actor_gateway;
    auto actor_context = actor_gateway != nullptr ? actor_gateway->actor_context (committed)
                                                  : local_actor_gateway.actor_context (committed);
    std::shared_ptr<void> actor;
    try {
        actor = factory.create_context_instance
                  ? factory.create_context_instance (std::move (actor_context))
                  : factory.create_instance (target.key);
        if (actor && !factory.create_context_instance && factory.configure_instance)
            factory.configure_instance (actor.get (), committed, &actor_context);
        if (!actor)
            return false;
        if (factory.relocation.kind == detail::factory_relocation_kind_t::preserve_state) {
            std::vector<std::byte> payload;
            payload.reserve (frozen.application_state.size ());
            for (const auto value : frozen.application_state)
                payload.push_back (static_cast<std::byte> (value));
            const auto restored =
              factory.restore (actor.get (), std::move (payload), cancellation).result ();
            if (!restored)
                return false;
        }
    }
    catch (...) {
        return false;
    }

    struct return_relocation_remnant_t
    {
        actor_ref_t source_actor;
        runtime::protocol::actor_route_fence_t source_fence;
        std::string transfer_id;
        std::shared_ptr<void> actor;
        std::shared_ptr<spot_context_state_t> context;
        std::function<task_t<void> (void *, void *)> leave_callback;
        bool submitted_leave = false;
    };

    std::function<result_t<void> (const actor_ref_t &)> update_registry;
    const auto key = actor_key (committed);
    std::optional<return_relocation_remnant_t> return_remnant;
    const auto remnant_prepared = _state->lane.run ([&] {
        const auto existing_actor = _state->actor_instances.find (key);
        const auto existing_spot = _state->actor_spot_ids.find (key);
        if (existing_actor != _state->actor_instances.end ()
            || existing_spot != _state->actor_spot_ids.end ()) {
            const auto current_fence = _state->actor_authority_fences.find (key);
            const auto existing_route = _state->actor_routes.find (key);
            const auto existing_generation = _state->actor_generations.find (key);
            const auto local_node = zlink::routing_id_t::from (target.node_id).to_bytes ();
            const bool has_source_cleanup = std::ranges::any_of (
              _state->pending_remote_source_cleanups, [&] (const auto &candidate) {
                  return candidate.source_actor.actor_id ().value () == target.key
                         && ::zlink::framework::detail::actor_ref_access_t::actor_type (
                              candidate.source_actor)
                              == frozen.stable_type
                         && candidate.source_actor.object_generation () == target.object_generation
                         && candidate.source_actor.node_rid ().value () == target.node_id
                         && candidate.source_fence.actor_id == target.key
                         && candidate.source_fence.object_generation == target.object_generation
                         && candidate.source_fence.target_node_routing_id == local_node
                         && candidate.source_fence.authority_owner_generation
                              < target.authority_owner_generation;
              });
            const bool exact_generation =
              existing_generation != _state->actor_generations.end ()
              && existing_generation->second == frozen.owner.object_generation;
            const bool replaceable_departed_cache =
              !has_source_cleanup && current_fence == _state->actor_authority_fences.end ()
              && exact_generation
              && ((existing_route != _state->actor_routes.end ()
                   && existing_route->second.node_rid.value () == frozen.owner.node_id)
                  || (existing_actor == _state->actor_instances.end ()
                      && existing_spot != _state->actor_spot_ids.end ()
                      && existing_route == _state->actor_routes.end ()));
            if (replaceable_departed_cache) {
                // Source membership cleanup already erased the old instance;
                // or its local map erase lagged behind the already-remote
                // route. With no local authority fence, the Store-validated
                // frozen source is the exact current-owner route, so the newer
                // returning target may replace both forms of departed cache.
                if (existing_actor != _state->actor_instances.end ()) {
                    detail::erase_actor_instance_index_unlocked (*_state, frozen.stable_type,
                                                                 target.key);
                    _state->actor_instances.erase (existing_actor);
                }
                erase_actor_route_unlocked (*_state, key);
            } else {
                auto cleanup = std::find_if (
                  _state->pending_remote_source_cleanups.begin (),
                  _state->pending_remote_source_cleanups.end (), [&] (const auto &candidate) {
                      return candidate.source_actor.actor_id ().value () == target.key
                             && ::zlink::framework::detail::actor_ref_access_t::actor_type (
                                  candidate.source_actor)
                                  == frozen.stable_type
                             && candidate.source_actor.object_generation ()
                                  == target.object_generation
                             && candidate.source_actor.node_rid ().value () == target.node_id
                             && candidate.source_fence.actor_id == target.key
                             && candidate.source_fence.object_generation == target.object_generation
                             && candidate.source_fence.target_node_routing_id == local_node
                             && candidate.source_fence.authority_owner_generation
                                  < target.authority_owner_generation;
                  });
                if (cleanup == _state->pending_remote_source_cleanups.end ()) {
                    return false;
                }
                const auto followed = _state->actor_transfer_coordinator.message_follow_target (
                  key, cleanup->source_fence);
                if (!followed || followed->actor.actor_id ().value () != frozen.owner.key
                    || ::zlink::framework::detail::actor_ref_access_t::actor_type (followed->actor)
                         != frozen.stable_type
                    || followed->actor.object_generation () != frozen.owner.object_generation
                    || followed->actor.node_rid ().value () != frozen.owner.node_id
                    || followed->target_fence.authority_owner_generation
                         != frozen.owner.authority_owner_generation
                    || frozen.owner.authority_owner_generation >= target.authority_owner_generation
                    || (current_fence != _state->actor_authority_fences.end ()
                        && current_fence->second != cleanup->source_fence)) {
                    return false;
                }

                return_remnant = return_relocation_remnant_t{
                  .source_actor = cleanup->source_actor,
                  .source_fence = cleanup->source_fence,
                  .transfer_id = cleanup->transfer_id,
                  .actor = existing_actor != _state->actor_instances.end ()
                             ? existing_actor->second
                             : std::shared_ptr<void>{},
                  .submitted_leave = cleanup->leave_submitted};
                if (!cleanup->leave_submitted) {
                    cleanup->leave_submitted = true;
                    const auto source_context =
                      _state->spot_contexts_by_id.find (std::string (cleanup->source_spot_id));
                    if (source_context != _state->spot_contexts_by_id.end ()
                        && source_context->second._state
                        && source_context->second._state->spot_instance) {
                        return_remnant->context = source_context->second._state;
                        decrement_actor_count_unlocked (*return_remnant->context);
                        const auto source_admission =
                          return_remnant->context->actor_admissions.find (factory.actor_type);
                        if (source_admission != return_remnant->context->actor_admissions.end ()) {
                            return_remnant->leave_callback =
                              source_admission->second.on_leave_actor;
                        }
                    }
                    if (!return_remnant->leave_callback)
                        cleanup->leave_completed = true;
                }
            }
        }
        return true;
    }).get ();
    if (!remnant_prepared)
        return false;

    if (return_remnant && !return_remnant->submitted_leave && return_remnant->leave_callback
        && return_remnant->context && return_remnant->actor) {
        (void) return_remnant->context->run_serial_task ("spot-actor-return-relocation-leave", [&] {
            return return_remnant->leave_callback (return_remnant->context->spot_instance.get (),
                                                   return_remnant->actor.get ());
        });
        _state->lane.run ([&] {
            const auto cleanup = std::find_if (
              _state->pending_remote_source_cleanups.begin (),
              _state->pending_remote_source_cleanups.end (), [&] (const auto &candidate) {
                  return candidate.transfer_id == return_remnant->transfer_id
                         && candidate.source_fence == return_remnant->source_fence;
              });
            if (cleanup != _state->pending_remote_source_cleanups.end ())
                cleanup->leave_completed = true;
        }).get ();
    }

    const auto target_native_node = _state->lane.run ([&] {
        return context->node ? context->node->native_node.lock ()
                             : std::shared_ptr<service::mesh_node_t>{};
    }).get ();
    const auto target_node_generation = target_native_node
                                          ? target_native_node->status ().lifecycle_generation ()
                                          : 0;
    const auto installed = _state->lane.run ([&] {
        const auto found_context = _state->spot_contexts_by_id.find (resolved_target_spot.key);
        const auto existing_actor = _state->actor_instances.find (key);
        if (return_remnant) {
            if (existing_actor != _state->actor_instances.end ()
                && existing_actor->second != return_remnant->actor) {
                return false;
            }
            const auto current_fence = _state->actor_authority_fences.find (key);
            if (current_fence != _state->actor_authority_fences.end ()
                && current_fence->second != return_remnant->source_fence) {
                return false;
            }
            if (existing_actor != _state->actor_instances.end ()) {
                detail::erase_actor_instance_index_unlocked (
                  *_state,
                  ::zlink::framework::detail::actor_ref_access_t::actor_type (
                    return_remnant->source_actor),
                  return_remnant->source_actor.actor_id ().value ());
                _state->actor_instances.erase (existing_actor);
            }
            erase_actor_route_unlocked (*_state, key);
        }
        if (found_context == _state->spot_contexts_by_id.end ()
            || found_context->second._state.get () != context.get ()
            || _state->actor_instances.contains (key) || _state->actor_spot_ids.contains (key)) {
            return false;
        }
        const auto found_admission = context->actor_admissions.find (factory.actor_type);
        if (found_admission == context->actor_admissions.end ())
            return false;
        admission = found_admission->second;
        detail::record_actor_instance_index_unlocked (*_state, committed, actor.get ());
        _state->actor_instances.emplace (key, actor);
        _state->actor_types_by_id.insert_or_assign (target.key, frozen.stable_type);
        _state->destroyed_actor_keys.erase (key);
        detail::record_actor_context_route_unlocked (*_state, key, target.node_id, *context,
                                                     committed.object_generation ());
        const auto staged_fence = runtime::protocol::actor_route_fence_t{
          target.key,
          target.object_generation,
          zlink::routing_id_t::from (target.node_id).to_bytes (),
          target_node_generation,
          target.authority_owner_generation,
          0};
        const auto current_fence = _state->actor_authority_fences.find (key);
        const bool current_is_newer_same_target =
          current_fence != _state->actor_authority_fences.end ()
          && current_fence->second.actor_id == staged_fence.actor_id
          && current_fence->second.object_generation == staged_fence.object_generation
          && current_fence->second.target_node_routing_id == staged_fence.target_node_routing_id
          && current_fence->second.target_node_generation == staged_fence.target_node_generation
          && (current_fence->second.authority_owner_generation
                > staged_fence.authority_owner_generation
              || (current_fence->second.authority_owner_generation
                    == staged_fence.authority_owner_generation
                  && current_fence->second.owner_lease_generation != 0));
        if (!current_is_newer_same_target)
            _state->actor_authority_fences.insert_or_assign (key, staged_fence);
        if (const auto coordinator = context->ensure_spot_serial_executor ())
            (void) coordinator->ensure_actor_queue (key);
        update_registry = _state->update_actor_registry_ref;
        return true;
    }).get ();
    if (!installed)
        return false;

    try {
        if (update_registry) {
            const auto updated = update_registry (committed);
            if (!updated)
                throw framework_exception_t (updated.error_kind (),
                                             updated.error () != nullptr
                                               ? updated.error ()->what ()
                                               : "relocated Actor registry update failed");
        }
    }
    catch (...) {
        abort_relocation_materialization ({target});
        return false;
    }
    // A normal maintenance/standalone import has no routed-Join recovery.
    // It retains the ordinary relocation lifecycle: membership is visible as
    // soon as materialization publishes the Actor, then OnActorJoined runs
    // before the target-only authority commit. Canonical User-Spot Join is
    // the sole deferred case because its ZLJR completion is fenced and
    // delivered by commit_relocation_materialization.
    if (!deferred_join_completion && admission.on_actor_joined) {
        const auto joined = context->run_serial_task ("spot-relocation-actor-joined", [&] {
            return admission.on_actor_joined (context->spot_instance.get (), actor.get ());
        });
        if (!joined) {
            abort_relocation_materialization ({target});
            return false;
        }
    }
    return true;
}

bool spot_node_runtime_t::commit_relocation_materialization (
  const std::vector<runtime::stateful::object_ref_t> &targets)
{
    std::vector<std::shared_ptr<spot_context_state_t>> ready;
    std::vector<std::pair<std::string, runtime::stateful::object_ref_t>> actor_fences;
    struct actor_join_completion_record_t
    {
        std::string handoff_id;
        actor_ref_t source_actor;
        actor_ref_t actor;
        spot_id_t source_spot_id;
        spot_id_t target_spot_id;
        std::uint64_t target_node_generation = 0;
        std::uint64_t target_authority_owner_generation = 0;
        std::uint64_t target_owner_lease_generation = 0;
        std::uint64_t operation_high = 0;
        std::uint64_t operation_low = 0;
        std::vector<std::uint8_t> reply;
        std::shared_ptr<spot_context_state_t> target_context;
        std::shared_ptr<void> actor_instance;
        detail::spot_actor_admission_callbacks_t admission;
    };
    std::vector<actor_join_completion_record_t> actor_join_completions;
    std::shared_ptr<runtime::stateful::authority_relocation_port_t> authority_store;
    const auto prepared = _state->lane.run ([&] {
        authority_store = _state->relocation_authority;
        for (const auto &target : targets) {
            if (target.kind == runtime::stateful::object_kind_t::actor) {
                const auto type = _state->actor_types_by_id.find (target.key);
                if (type == _state->actor_types_by_id.end ())
                    continue;
                const auto key = type->second + ":" + target.key;
                const auto fence = _state->actor_authority_fences.find (key);
                if (fence == _state->actor_authority_fences.end ())
                    return false;
                actor_fences.emplace_back (key, target);
                const auto recovery = _state->actor_join_relocation_recoveries.find (key);
                if (recovery != _state->actor_join_relocation_recoveries.end ()) {
                    const auto context = _state->spot_contexts_by_id.find (
                      std::string (recovery->second.target_spot_id));
                    const auto instance = _state->actor_instances.find (key);
                    const auto factory = _state->actor_factories.find (type->second);
                    if (context == _state->spot_contexts_by_id.end () || !context->second._state
                        || instance == _state->actor_instances.end ()
                        || factory == _state->actor_factories.end ())
                        return false;
                    const auto admission =
                      context->second._state->actor_admissions.find (factory->second.actor_type);
                    if (admission == context->second._state->actor_admissions.end ())
                        return false;
                    actor_join_completions.push_back (
                      {recovery->second.handoff_id, recovery->second.source_actor,
                       ::zlink::framework::detail::actor_ref_access_t::make (
                         node_rid_t::from_string (target.node_id), type->second, target.key,
                         target.object_generation),
                       recovery->second.source_spot_id, recovery->second.target_spot_id,
                       recovery->second.target_node_generation, target.authority_owner_generation,
                       0, recovery->second.completion_operation_id_high,
                       recovery->second.completion_operation_id_low,
                       recovery->second.admission_reply, context->second._state, instance->second,
                       admission->second});
                }
                continue;
            }
            if (target.kind != runtime::stateful::object_kind_t::user_spot)
                continue;
            const auto context = _state->spot_contexts_by_id.find (target.key);
            if (context != _state->spot_contexts_by_id.end () && context->second._state)
                ready.push_back (context->second._state);
        }
        return true;
    }).get ();
    if (!prepared)
        return false;
    if (!actor_fences.empty () && !authority_store)
        return false;
    for (const auto &[key, target] : actor_fences) {
        const auto authority = authority_store->read (target.kind, target.key);
        if (!authority || authority->target.kind != target.kind
            || authority->target.key != target.key
            || authority->target.object_generation != target.object_generation
            || authority->target.mesh_name != target.mesh_name
            || authority->target.node_id != target.node_id
            || authority->target.authority_owner_generation < target.authority_owner_generation
            || authority->target_owner.owner_id.empty ()
            || authority->target_owner.lease_generation <= 0)
            return false;
        const auto updated = _state->lane.run ([&] {
            const auto fence = _state->actor_authority_fences.find (key);
            if (fence == _state->actor_authority_fences.end ())
                return false;
            fence->second.authority_owner_generation = authority->target.authority_owner_generation;
            fence->second.owner_lease_generation = authority->target_owner.lease_generation;
            for (auto &completion : actor_join_completions) {
                if (completion.actor.actor_id ().value () == target.key
                    && completion.actor.object_generation () == target.object_generation) {
                    completion.target_authority_owner_generation =
                      authority->target.authority_owner_generation;
                    completion.target_owner_lease_generation =
                      authority->target_owner.lease_generation;
                    break;
                }
            }
            return true;
        }).get ();
        if (!updated)
            return false;
    }
    for (const auto &state : ready) {
        state->callback_lane.run ([state] {
            state->relocation_boundary_active = false;
        }).get ();
        state->complete_relocation_ready (spot_relocation_ready_outcome_t::relocated);
    }
    for (auto &completion : actor_join_completions) {
        if (actor_transfer_marker_enabled ()) {
            emit_actor_transfer_marker ("location_committed", completion.actor,
                                        completion.handoff_id, completion.target_spot_id);
        }
        auto complete_join = [node = _state, completion = std::move (completion)] () mutable {
            auto runtime = spot_node_runtime_t (node);
            if (completion.admission.on_actor_joined) {
                const auto joined =
                  completion.target_context->run_serial_task ("spot-relocation-actor-joined", [&] {
                      return completion.admission.on_actor_joined (
                        completion.target_context->spot_instance.get (),
                        completion.actor_instance.get ());
                  });
                if (!joined) {
                    node->actor_transfer_coordinator.fail_commit (completion.handoff_id, true);
                    return;
                }
            }
            serializer_registry_t *serializers = nullptr;
            serializers = node->lane.run ([&] {
                if (node->channel_runtime)
                    return node->channel_runtime->serializers;
                return static_cast<serializer_registry_t *> (nullptr);
            }).get ();
            if (serializers == nullptr) {
                node->actor_transfer_coordinator.fail_commit (completion.handoff_id, true);
                return;
            }
            try {
                runtime::messaging::envelope_header_t leave_header;
                leave_header.kind = runtime::messaging::message_kind_t::command;
                leave_header.channel_name = "node";
                leave_header.message_name = detail::spot_actor_leave_route_command_t::packet_name;
                auto leave_parts = runtime::messaging::envelope_codec_t{}.encode_parts (
                  leave_header,
                  detail::spot_actor_leave_route_command_t{
                    .transfer_id = completion.handoff_id,
                    .actor_node_rid = std::string (completion.source_actor.node_rid ().value ()),
                    .actor_type =
                      std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (
                        completion.source_actor)),
                    .actor_id = std::string (completion.source_actor.actor_id ().value ()),
                    .actor_generation = completion.source_actor.object_generation (),
                    .source_spot_id = std::string (completion.source_spot_id),
                    .source_spot_generation = 0,
                    .target_spot_id = std::string (completion.target_spot_id),
                    .target_node_rid = std::string (completion.actor.node_rid ().value ()),
                    .target_node_generation = completion.target_node_generation,
                    .target_authority_owner_generation =
                      completion.target_authority_owner_generation,
                    .target_owner_lease_generation = completion.target_owner_lease_generation},
                  *serializers);
                auto notification = std::make_shared<task_t<zlink::submit_result_t>> (
                  runtime.send_actor_leave_notification (
                    zlink::routing_id_t::from (
                      std::string (completion.source_actor.node_rid ().value ())),
                    std::move (leave_parts)));
                detail::observe_task_completion (
                  *notification, [notification] (const result_t<zlink::submit_result_t> &) {});
            }
            catch (...) {
                // Source OnLeave is post-commit, one-way housekeeping.
            }
            std::optional<message_t> reply;
            if (!completion.reply.empty ()) {
                reply = message_t::from_raw (zlink::message_t::from (std::move (completion.reply)),
                                             serializers);
            }
            const auto actor = completion.actor;
            const auto source_actor = completion.source_actor;
            const auto target_spot_id = completion.target_spot_id;
            const auto completion_handoff_id = completion.handoff_id;
            const auto key = actor_key (actor);
            runtime.deliver_actor_join_completion_async (
              actor,
              actor_join_accepted_t{completion.operation_high, completion.operation_low, actor,
                                    std::move (reply)},
              target_spot_id,
              [node, actor, source_actor, target_spot_id, completion_handoff_id,
               key] (result_t<void> delivered) mutable {
                  if (!delivered) {
                      node->actor_transfer_coordinator.fail_commit (completion_handoff_id, true);
                      return;
                  }
                  auto [replay, replay_services] = node->lane.run ([&] {
                      auto backlog =
                        node->actor_transfer_coordinator.complete_commit_and_take_backlog (
                        completion_handoff_id, source_actor, target_spot_id);
                      if (backlog)
                          node->actor_join_relocation_recoveries.erase (key);
                      return std::make_pair (std::move (backlog), node->root_services);
                  }).get ();
                  if (!replay) {
                      node->actor_transfer_coordinator.fail_commit (completion_handoff_id, true);
                      return;
                  }
                  if (replay->empty ())
                      return;
                  if (!replay_services) {
                      auto runtime = spot_node_runtime_t (node);
                      if (runtime.actor_transfer_marker_enabled ()) {
                          runtime.emit_actor_transfer_marker ("handoff_replay_unavailable", actor,
                                                              completion_handoff_id);
                      }
                      return;
                  }
                  spot_node_runtime_t (node).enqueue_actor_handoff_replay (
                    actor, std::move (*replay), *replay_services, completion_handoff_id);
              });
        };
        complete_join ();
    }
    return true;
}

void spot_node_runtime_t::abort_relocation_materialization (
  const std::vector<runtime::stateful::object_ref_t> &targets) noexcept
{
    std::vector<std::shared_ptr<spot_context_state_t>> spots;
    try {
        _state->lane.run ([&] {
            for (const auto &target : targets) {
                if (target.kind != runtime::stateful::object_kind_t::actor)
                    continue;
                const auto type = _state->actor_types_by_id.find (target.key);
                if (type == _state->actor_types_by_id.end ())
                    continue;
                const auto key = type->second + ":" + target.key;
                const auto recovery = _state->actor_join_relocation_recoveries.find (key);
                if (recovery != _state->actor_join_relocation_recoveries.end ()) {
                    _state->actor_transfer_coordinator.fail_commit (recovery->second.handoff_id,
                                                                    false);
                    _state->actor_join_relocation_recoveries.erase (recovery);
                }
                if (const auto spot = _state->actor_spot_ids.find (key);
                    spot != _state->actor_spot_ids.end ()) {
                    const auto context =
                      _state->spot_contexts_by_id.find (std::string (spot->second));
                    if (context != _state->spot_contexts_by_id.end () && context->second._state)
                        decrement_actor_count_unlocked (*context->second._state);
                }
                const auto location = _state->actor_spot_ids.find (key);
                if (location != _state->actor_spot_ids.end ()) {
                    const auto context = _state->spot_contexts_by_id.find (
                      std::string (location->second));
                    if (context != _state->spot_contexts_by_id.end () && context->second._state
                        && context->second._state->spot_serial_executor) {
                        context->second._state->spot_serial_executor->erase_actor_queue (key);
                    }
                }
                erase_actor_route_unlocked (*_state, key);
                _state->actor_instances.erase (key);
                detail::erase_actor_instance_index_unlocked (*_state, type->second, target.key);
                _state->actor_types_by_id.erase (type);
                _state->destroyed_actor_keys.erase (key);
            }
            for (const auto &target : targets) {
                if (target.kind != runtime::stateful::object_kind_t::user_spot
                    && target.kind != runtime::stateful::object_kind_t::instance_spot)
                    continue;
                const auto context = _state->spot_contexts_by_id.find (target.key);
                if (context != _state->spot_contexts_by_id.end () && context->second._state)
                    spots.push_back (context->second._state);
            }
        }).get ();
    }
    catch (...) {
        return;
    }
    for (const auto &spot : spots) {
        try {
            (void) spot->close_now ();
        }
        catch (...) {
        }
    }
}

result_t<spot_actor_join_result_t> spot_node_runtime_t::admit_remote_actor_to_spot (
  std::string transfer_id,
  const actor_ref_t &actor_ref,
  spot_id_t source_spot_id,
  spot_id_t target_spot_id,
  const zlink::message_t &request,
  std::uint64_t completion_operation_id_high,
  std::uint64_t completion_operation_id_low,
  std::uint64_t actor_authority_owner_generation,
  std::uint64_t actor_node_generation,
  std::uint64_t expected_owner_lease_generation,
  bool actor_type_from_authority_only,
  std::uint64_t target_spot_generation,
  std::uint64_t target_spot_authority_owner_generation)
{
    /* graceful-drain-handoff §4-2/§5.2: a draining node rejects new actor
    * admission and joins; already-admitted transfer commits stay accepted. */
    auto drain_flag = _state->lane.run ([&] { return _state->drain_flag; }).get ();
    if (drain_flag && drain_flag->load (std::memory_order_acquire)) {
        return result_t<spot_actor_join_result_t>::failure (
          framework_error_kind_t::rejected,
          "spot node is draining and rejects new actor admission");
    }
    if (transfer_id.empty () || ::zlink::framework::detail::actor_ref_access_t::empty (actor_ref)) {
        return result_t<spot_actor_join_result_t>::failure (
          framework_error_kind_t::protocol_error,
          "remote actor admission requires transfer and actor identity");
    }

    auto services = _state->lane.run ([&] { return _state->root_services; }).get ();
    if (!services) {
        return result_t<spot_actor_join_result_t>::failure (
          framework_error_kind_t::unavailable,
          "remote Actor Join Location Store is unavailable");
    }
    runtime::live_location_reader_t *store = nullptr;
    try {
        store = &services->get_required<runtime::live_location_reader_t> ();
    }
    catch (const std::exception &) {
        return result_t<spot_actor_join_result_t>::failure (
          framework_error_kind_t::unavailable,
          "remote Actor Join Location Store is unavailable");
    }
    const auto stable_type = actor_type_from_authority (
      *store, actor_ref,
      actor_join_authority_fence_t{actor_ref.object_generation (), actor_node_generation,
                                   actor_authority_owner_generation,
                                   expected_owner_lease_generation},
      actor_type_from_authority_only);
    if (!stable_type) {
        return detail::propagate_failure<spot_actor_join_result_t> (
          stable_type, "remote Actor Join authority resolution failed");
    }
    const auto store_actor = ::zlink::framework::detail::actor_ref_access_t::make (
      node_rid_t::from_string (std::string (actor_ref.node_rid ().value ())), stable_type.value (),
      std::string (actor_ref.actor_id ().value ()), actor_ref.object_generation ());

    cleanup_expired_actor_admissions ();
    auto target_state = _state->lane.run ([&] {
        const auto found = _state->spot_contexts_by_id.find (std::string (target_spot_id));
        if (found == _state->spot_contexts_by_id.end () || !found->second._state
            || !found->second._state->spot_instance) {
            return std::shared_ptr<spot_context_state_t>{};
        }
        return found->second._state;
    }).get ();
    if (!target_state) {
        const auto dynamic_spot_name = _state->lane.run ([&] {
            std::optional<std::string> result;
            for (const auto &[spot_name, _] : _state->spot_factories) {
                if (_state->snapshot.entry_spot_name
                    && spot_name == *_state->snapshot.entry_spot_name) {
                    continue;
                }
                if (result) {
                    result.reset ();
                    break;
                }
                result = spot_name;
            }
            return result;
        }).get ();
        if (dynamic_spot_name) {
            (void) get_or_create_spot (*dynamic_spot_name, target_spot_id, request);
            target_state = _state->lane.run ([&] {
                const auto found =
                  _state->spot_contexts_by_id.find (std::string (target_spot_id));
                if (found == _state->spot_contexts_by_id.end () || !found->second._state
                    || !found->second._state->spot_instance) {
                    return std::shared_ptr<spot_context_state_t>{};
                }
                return found->second._state;
            }).get ();
        }
    }
    if (!target_state) {
        return result_t<spot_actor_join_result_t>::failure (
          framework_error_kind_t::not_found,
          "target spot is not registered");
    }

    std::optional<spot_node_builder_state_t::actor_factory_registration_t> actor_factory;
    std::optional<spot_actor_admission_callbacks_t> admission;
    std::chrono::milliseconds admission_timeout;
    _state->lane.run ([&] {
        const auto factory = _state->actor_factories.find (
          std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (store_actor)));
        if (factory == _state->actor_factories.end ())
            return;
        actor_factory = factory->second;
        const auto found_admission = target_state->actor_admissions.find (factory->second.actor_type);
        if (found_admission != target_state->actor_admissions.end ()
            && found_admission->second.join) {
            admission = found_admission->second;
        }
        admission_timeout =
          _state->channel_runtime
            ? _state->channel_runtime->default_request_timeout
            : std::chrono::duration_cast<std::chrono::milliseconds> (std::chrono::seconds (30));
    }).get ();
    if (!actor_factory) {
        return result_t<spot_actor_join_result_t>::failure (
          framework_error_kind_t::rejected,
          "remote Actor Join Authority stable type is not registered locally");
    }
    if (!admission) {
        report_spot_dispatch_error (
          _state, dispatch_error_surface_t::spot_actor, dispatch_message_kind_t::actor_request,
          dispatch_error_reason_t::handler_missing, dispatch_error_action_t::reply_error,
          "actor.join", std::nullopt, std::string (target_spot_id),
          std::string (store_actor.actor_id ().value ()));
        return result_t<spot_actor_join_result_t>::failure (
          framework_error_kind_t::not_found,
          "spot actor join callback is not registered");
    }

    auto &target = *target_state;
    auto &serializers = *target.channel_runtime->serializers;
    spot_actor_join_result_t response;
    bool admission_conflict = false;
    // The admission callback is user code on the spot serial queue. It may call back
    // into the framework (sends, joins), so the state lane must not be held
    // across this wait.
    if (!target.run_serial_sync ("spot-actor-admission", [&] {
            const auto existing = _state->actor_transfer_coordinator.admission (transfer_id);
            if (existing) {
                if (!existing->matches_prepare (store_actor, source_spot_id, target_spot_id,
                                                completion_operation_id_high,
                                                completion_operation_id_low)) {
                    admission_conflict = true;
                    return;
                }
                response = spot_actor_join_result_t{true, existing->admission_reply};
                return;
            }
            response = admission->join (
              target.spot_instance.get (), actor_ref.actor_id ().value (), request, serializers);
            if (response.accepted
                && !_state->actor_transfer_coordinator.try_add_admission (
                  transfer_id,
                  pending_actor_admission_t{
                    .actor_key = actor_key (store_actor),
                    .source_actor = store_actor,
                    .source_spot_id = source_spot_id,
                    .target_spot_id = target_spot_id,
                    .canonical_user_spot_join = actor_type_from_authority_only
                                                && !target.is_entry_spot ()
                                                && !target.is_instance_spot (),
                    .deadline = std::chrono::steady_clock::now () + admission_timeout,
                    .completion_operation_id_high = completion_operation_id_high,
                    .completion_operation_id_low = completion_operation_id_low,
                    .reservation_token = transfer_id,
                    .reserved_payload_bytes = runtime::protocol::actor_join_reserved_payload_bytes (
                      request.size (), actor_factory->relocation.kind
                                           == detail::factory_relocation_kind_t::preserve_state
                                         ? runtime::protocol::actor_join_snapshot_content_type
                                         : runtime::protocol::actor_join_recreate_content_type),
                    .target_spot_generation = target_spot_generation,
                    .target_spot_authority_owner_generation =
                      target_spot_authority_owner_generation,
                    .source_actor_authority_owner_generation = actor_authority_owner_generation,
                    .source_node_generation = actor_node_generation,
                    .source_owner_lease_generation = expected_owner_lease_generation,
                    .admission_reply = response.reply})) {
                admission_conflict = true;
            }
        })) {
        return result_t<spot_actor_join_result_t>::failure (
          framework_error_kind_t::capacity_exceeded, "spot serial queue is full");
    }
    if (admission_conflict) {
        return result_t<spot_actor_join_result_t>::failure (
          framework_error_kind_t::protocol_error,
          "remote actor admission conflicts with the pending prepare");
    }
    if (response.accepted) {
        if ((completion_operation_id_high == 0 && completion_operation_id_low == 0)
            || actor_authority_owner_generation == 0
            || actor_authority_owner_generation == std::numeric_limits<std::uint64_t>::max ()) {
            _state->actor_transfer_coordinator.fail_commit (transfer_id, false);
            return result_t<spot_actor_join_result_t>::failure (
              framework_error_kind_t::protocol_error,
              "remote Actor Join completion identity is invalid");
        }
    }
    return result_t<spot_actor_join_result_t>::success (std::move (response));
}

result_t<spot_node_runtime_t::remote_actor_transfer_t> spot_node_runtime_t::transfer_actor_out (
  const actor_ref_t &actor_ref, std::string transfer_id, bool capture_state)
{
    struct transfer_plan_t
    {
        std::string key;
        spot_id_t source_spot;
        std::shared_ptr<void> actor;
        spot_node_builder_state_t::actor_factory_registration_t factory;
        std::shared_ptr<monitoring_runtime_state_t> monitoring;
    };
    auto plan = _state->lane.run ([&] () -> result_t<transfer_plan_t> {
        const auto key = actor_key (actor_ref);
        const auto factory = _state->actor_factories.find (
          std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor_ref)));
        const auto actor = _state->actor_instances.find (key);
        const auto source_spot = _state->actor_spot_ids.find (key);
        if (actor == _state->actor_instances.end () || !actor->second
            || source_spot == _state->actor_spot_ids.end ()) {
            return result_t<transfer_plan_t>::failure (
              framework_error_kind_t::not_found, "source actor is not joined to a local spot");
        }
        if (factory == _state->actor_factories.end ()) {
            return result_t<transfer_plan_t>::failure (
              framework_error_kind_t::not_found, "source actor factory is not registered");
        }
        if (factory->second.relocation.kind == detail::factory_relocation_kind_t::disabled) {
            return result_t<transfer_plan_t>::failure (framework_error_kind_t::not_configured,
                                                       "Actor relocation is disabled");
        }
        if (transfer_id.empty ()) {
            transfer_id = key;
        }
        if (!_state->actor_transfer_coordinator.try_begin_source_remote (
              key, std::move (transfer_id))) {
            return result_t<transfer_plan_t>::failure (
              framework_error_kind_t::rejected, "actor transfer is already in progress");
        }
        return result_t<transfer_plan_t>::success (
          transfer_plan_t{key, source_spot->second, actor->second, factory->second,
                          _state->monitoring});
    }).get ();
    if (!plan) {
        return detail::propagate_failure<remote_actor_transfer_t> (plan,
                                                                   "actor transfer-out failed");
    }
    auto prepared = std::move (plan.value ());
    // One histogram sample per transfer, taken right at the moving transition
    // (runtime-metrics §4.3): the coordinator now blocks new dispatches, so the
    // counter is exactly the requests still in flight across the move.
    {
        runtime::runtime_metrics_t metrics (prepared.monitoring);
        if (metrics.enabled ()) {
            const auto pending =
              _state->actor_pending_requests_lane
                .run ([this, &prepared] {
                const auto found = _state->actor_pending_requests.find (prepared.key);
                if (found != _state->actor_pending_requests.end ()) {
                    return found->second;
                }
                return std::size_t{0};
                })
                .get ();
            metrics.histogram ("zlink.actor.transfer.pending_requests.count", "{request}",
                               static_cast<double> (pending));
        }
    }
    if (!capture_state) {
        return result_t<remote_actor_transfer_t>::success (remote_actor_transfer_t{
          prepared.source_spot, zlink::message_t{},
          prepared.factory.relocation.kind == detail::factory_relocation_kind_t::preserve_state
            ? std::string (runtime::protocol::actor_join_snapshot_content_type)
            : std::string (runtime::protocol::actor_join_recreate_content_type)});
    }
    if (prepared.factory.relocation.kind != detail::factory_relocation_kind_t::preserve_state) {
        return result_t<remote_actor_transfer_t>::success (remote_actor_transfer_t{
          prepared.source_spot, zlink::message_t{},
          std::string (runtime::protocol::actor_join_recreate_content_type)});
    }
    try {
        auto state = prepared.factory.capture (prepared.actor.get (), {}).result ();
        if (!state) {
            replay_actor_handoff_until_move_closed (actor_ref, transfer_id);
            return detail::propagate_failure<remote_actor_transfer_t> (state,
                                                                       "actor transfer-out failed");
        }
        std::string payload;
        payload.resize (state.value ().size ());
        std::transform (state.value ().begin (), state.value ().end (), payload.begin (),
                        [] (std::byte value) { return static_cast<char> (value); });
        return result_t<remote_actor_transfer_t>::success (remote_actor_transfer_t{
          prepared.source_spot, zlink::message_t::from (payload),
          std::string (runtime::protocol::actor_join_snapshot_content_type)});
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
    const auto local_node_rid = _state->lane.run ([&] {
        return detail::effective_spot_node_rid (_state->snapshot);
    }).get ();
    return _state->actor_transfer_coordinator.next_transfer_id (
      local_node_rid);
}

std::optional<std::string>
spot_node_runtime_t::reserved_actor_transfer_id (const actor_ref_t &actor_ref) const
{
    const auto key = actor_key (actor_ref);
    if (_state->actor_transfer_coordinator.phase (key) != actor_move_phase_t::source_reserved) {
        return std::nullopt;
    }
    return _state->actor_transfer_coordinator.transfer_id (key);
}

std::pair<std::uint64_t, std::uint64_t>
spot_node_runtime_t::actor_join_operation_id (std::string_view transfer_id) const
{
    /*
     * Actor Join completion replay needs an ID that is independent from the
     * relocation ID. Derive two 64-bit words with independent FNV-1a domains
     * from the source-generated transfer ID. The transfer ID already includes
     * the source node identity and a monotonically increasing sequence.
     */
    constexpr std::uint64_t fnv_prime = 1099511628211ULL;
    auto hash = [] (std::string_view value, std::uint64_t seed) {
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
spot_node_runtime_t::reserve_actor_join_barrier (const actor_ref_t &actor_ref)
{
    if (::zlink::framework::detail::actor_ref_access_t::empty (actor_ref)) {
        return result_t<std::shared_ptr<deferred_barrier_t>>::failure (
          framework_error_kind_t::not_found, "Actor join barrier source is empty");
    }

    const auto key = actor_key (actor_ref);
    if (!_state->actor_transfer_coordinator.try_reserve_source (key, next_actor_transfer_id ())) {
        return result_t<std::shared_ptr<deferred_barrier_t>>::failure (
          framework_error_kind_t::rejected, "Actor join is already reserved or moving");
    }

    auto coordinator = _state->lane.run ([&] {
        const auto location = _state->actor_spot_ids.find (key);
        if (location == _state->actor_spot_ids.end ())
            return std::shared_ptr<detail::spot_serial_executor_t>{};
        const auto context = _state->spot_contexts_by_id.find (std::string (location->second));
        return context == _state->spot_contexts_by_id.end () || !context->second._state
                 ? std::shared_ptr<detail::spot_serial_executor_t>{}
                 : context->second._state->ensure_spot_serial_executor ();
    }).get ();
    if (!coordinator) {
        _state->actor_transfer_coordinator.cancel_move (key);
        return result_t<std::shared_ptr<deferred_barrier_t>>::failure (
          framework_error_kind_t::not_found, "Actor join barrier target is unavailable");
    }
    auto reserved = coordinator->reserve_actor_handoff_barrier (key, "deferred-actor-join");
    if (!reserved) {
        _state->actor_transfer_coordinator.cancel_move (key);
        return reserved;
    }
    auto cancel_reservation = [state = _state, key] {
        if (state->actor_transfer_coordinator.phase (key) == actor_move_phase_t::source_reserved) {
            state->actor_transfer_coordinator.cancel_move (key);
        }
    };
    auto settle_reservation = [state = _state, actor_ref, key] {
        if (state->actor_transfer_coordinator.phase (key) != actor_move_phase_t::source_reserved) {
            return;
        }
        const auto has_root_services =
          state->lane.run ([&] { return state->root_services.has_value (); }).get ();
        if (!has_root_services) {
            const auto completed =
              state->actor_transfer_coordinator.complete_move_and_take_backlog (key);
            if (!completed.backlog.empty ()
                && spot_node_runtime_t (state).actor_transfer_marker_enabled ()) {
                spot_node_runtime_t (state).emit_actor_transfer_marker (
                  "handoff_replay_unavailable", actor_ref, key);
            }
            return;
        }
        if (!framework_worker_executor (state)->try_submit_internal ([state, actor_ref, key] {
                spot_node_runtime_t (state).replay_actor_handoff_until_move_closed (actor_ref, key);
            })) {
            state->actor_transfer_coordinator.cancel_move (key);
        }
    };
    return result_t<std::shared_ptr<deferred_barrier_t>>::success (
      std::make_shared<actor_handoff_barrier_t> (std::move (reserved.value ()),
                                                 std::move (cancel_reservation),
                                                 std::move (settle_reservation)));
}

result_t<void>
spot_node_runtime_t::deliver_actor_join_completion (const actor_ref_t &actor_ref,
                                                    const actor_join_completion_t &completion,
                                                    std::optional<spot_id_t> source_spot_id)
{
    std::mutex mutex;
    std::condition_variable settled;
    std::optional<result_t<void>> result;
    deliver_actor_join_completion_async (actor_ref, completion, std::move (source_spot_id),
                                         [&] (result_t<void> value) {
                                             {
                                                 std::lock_guard lock (mutex);
                                                 result.emplace (std::move (value));
                                             }
                                             settled.notify_all ();
                                         });
    std::unique_lock lock (mutex);
    settled.wait (lock, [&] { return result.has_value (); });
    return std::move (*result);
}

void spot_node_runtime_t::deliver_actor_join_completion_async (
  actor_ref_t actor_ref,
  actor_join_completion_t completion,
  std::optional<spot_id_t> source_spot_id,
  std::function<void (result_t<void>)> completed)
{
    /* A commit-preceding failure can arrive after source membership removal.
     * The Actor instance and generation are the completion fence; the source
     * Spot is only a routing hint. */
    (void) source_spot_id;
    auto completion_owner = std::make_shared<actor_join_completion_t> (std::move (completion));
    const auto operation = std::visit (
      [] (const auto &value) {
          return std::pair<std::uint64_t, std::uint64_t>{value.operation_id_high,
                                                         value.operation_id_low};
      },
      *completion_owner);
    if (operation.first == 0 && operation.second == 0) {
        completed (result_t<void>::failure (framework_error_kind_t::protocol_error,
                                            "Actor Join completion operation ID must be non-zero"));
        return;
    }

    actor_join_completion_callback_t callback;
    std::shared_ptr<void> actor;
    std::optional<result_t<void>> immediate;
    _state->lane.run ([&] {
        if (_state->delivered_join_completions.contains (operation)
            || _state->delivering_join_completions.contains (operation)) {
            immediate.emplace (result_t<void>::success ());
        } else {
            const auto key = actor_key (actor_ref);
            const auto generation = _state->actor_generations.find (key);
            if (generation != _state->actor_generations.end ()
                && generation->second != actor_ref.object_generation ()) {
                immediate.emplace (
                  result_t<void>::failure (framework_error_kind_t::invalid_operation,
                                           "Actor Join completion generation is stale"));
            } else {
                const auto factory = _state->actor_factories.find (std::string (
                  ::zlink::framework::detail::actor_ref_access_t::actor_type (actor_ref)));
                const auto instance = _state->actor_instances.find (key);
                if (factory == _state->actor_factories.end ()
                    || instance == _state->actor_instances.end () || !instance->second) {
                    immediate.emplace (
                      result_t<void>::failure (framework_error_kind_t::not_found,
                                               "Actor Join completion Actor is not registered"));
                } else {
                    callback = factory->second.on_join_completed;
                    actor = instance->second;
                    _state->delivering_join_completions.insert (operation);
                }
            }
        }
    }).get ();
    if (immediate) {
        completed (std::move (*immediate));
        return;
    }

    actor_gateway_runtime_t *actor_gateway = nullptr;
    std::shared_ptr<bound_session_delivery_fence_t> delivery_fence;
    auto root_services = _state->lane.run ([&] { return _state->root_services; }).get ();
    if (callback && root_services) {
        try {
            actor_gateway = &root_services->get_required<actor_gateway_runtime_t> ();
            delivery_fence = actor_gateway->begin_join_completion_delivery_fence (actor_ref);
        }
        catch (...) {
            actor_gateway = nullptr;
            delivery_fence.reset ();
        }
    }

    auto settle = [node = _state, operation,
                   completed = std::move (completed)] (result_t<void> result) mutable {
        node->lane.run ([&] {
            node->delivering_join_completions.erase (operation);
            if (result)
                node->delivered_join_completions.insert (operation);
        }).get ();
        completed (std::move (result));
    };
    auto settle_after_delivery = [actor_gateway, delivery_fence, actor_ref,
                                  settle = std::move (settle)] (result_t<void> result) mutable {
        if (actor_gateway && delivery_fence) {
            actor_gateway->settle_join_completion_delivery_fence (
              actor_ref, delivery_fence, std::move (result), std::move (settle));
            return;
        }
        settle (std::move (result));
    };

    if (std::holds_alternative<actor_join_failed_t> (*completion_owner)) {
        const auto failed_kind = std::get<actor_join_failed_t> (*completion_owner).error_kind;
        detail::message_flow_tracer_t (_state->dispatch)
          .trace (message_flow_outcome_t::replied, message_flow_result_t::failed, [&] {
              auto event = message_flow_event_t{message_flow_outcome_t::replied,
                                                dispatch_error_surface_t::spot_actor,
                                                dispatch_message_kind_t::actor_request,
                                                std::string ("JoinSpot"),
                                                std::nullopt,
                                                std::nullopt,
                                                std::nullopt,
                                                std::nullopt,
                                                std::nullopt,
                                                std::string (actor_ref.actor_id ().value ()),
                                                std::nullopt,
                                                dispatch_error_reason_t::handler_exception,
                                                dispatch_error_action_t::reply_error,
                                                std::make_exception_ptr (framework_exception_t (
                                                  failed_kind, "deferred Actor Join failed"))};
              event.result = message_flow_result_t::failed;
              event.reason = message_flow_reason_t::activation_rejected;
              return event;
          });
    }

    if (!callback) {
        settle_after_delivery (result_t<void>::success ());
        return;
    }
    try {
        auto callback_task =
          callback (actor.get (),
                    std::holds_alternative<actor_join_accepted_t> (*completion_owner)
                      ? actor_join_completion_outcome_t::accepted
                      : (std::holds_alternative<actor_join_rejected_t> (*completion_owner)
                           ? actor_join_completion_outcome_t::rejected
                           : actor_join_completion_outcome_t::failed),
                    operation.first, operation.second,
                    std::get_if<actor_join_accepted_t> (completion_owner.get ())
                      ? &std::get<actor_join_accepted_t> (*completion_owner).actor
                      : nullptr,
                    std::holds_alternative<actor_join_accepted_t> (*completion_owner)
                      ? std::get<actor_join_accepted_t> (*completion_owner).reply
                      : (std::holds_alternative<actor_join_rejected_t> (*completion_owner)
                           ? std::get<actor_join_rejected_t> (*completion_owner).reply
                           : std::optional<message_t>{}),
                    std::holds_alternative<actor_join_failed_t> (*completion_owner)
                      ? std::get<actor_join_failed_t> (*completion_owner).error_kind
                      : framework_error_kind_t::internal_failure,
                    std::holds_alternative<actor_join_failed_t> (*completion_owner)
                      && detail::is_transient_error (
                        std::get<actor_join_failed_t> (*completion_owner).error_kind));
        auto observed = std::make_shared<task_t<void>> (std::move (callback_task));
        detail::observe_task_completion (
          *observed,
          [completion_owner, actor = std::move (actor), observed,
           settle = std::move (settle_after_delivery)] (const result_t<void> &result) mutable {
              (void) completion_owner;
              (void) actor;
              if (result) {
                  settle (result_t<void>::success ());
                  return;
              }
              settle (result_t<void>::failure (result.error_kind (),
                                               result.error () != nullptr
                                                 ? result.error ()->what ()
                                                 : "Actor Join completion callback failed"));
          });
    }
    catch (const framework_exception_t &error) {
        settle_after_delivery (detail::result_access_t::failure<void> (error));
    }
    catch (const std::exception &error) {
        settle_after_delivery (
          result_t<void>::failure (framework_error_kind_t::internal_failure, error.what ()));
    }
    catch (...) {
        settle_after_delivery (result_t<void>::failure (framework_error_kind_t::internal_failure,
                                                        "Actor Join completion callback failed"));
    }
}

result_t<void> spot_node_runtime_t::leave_actor_for_remote_transfer (const actor_ref_t &actor_ref)
{
    const auto key = actor_key (actor_ref);
    std::shared_ptr<void> actor;
    std::shared_ptr<spot_context_state_t> previous_state;
    std::function<task_t<void> (void *, void *)> leave_callback;
    const auto plan = _state->lane.run ([&] () -> result_t<void> {
        const auto found_actor = _state->actor_instances.find (key);
        const auto factory = _state->actor_factories.find (
          std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor_ref)));
        const auto previous = _state->actor_spot_ids.find (key);
        if (found_actor == _state->actor_instances.end () || !found_actor->second
            || factory == _state->actor_factories.end ()
            || previous == _state->actor_spot_ids.end ()) {
            return result_t<void>::failure (framework_error_kind_t::not_found,
                                            "source actor is not joined to a local spot");
        }
        if (_state->actor_transfer_coordinator.phase (key)
            != std::make_optional (actor_move_phase_t::source_remote)) {
            return result_t<void>::failure (framework_error_kind_t::rejected,
                                            "actor transfer has not been prepared");
        }
        actor = found_actor->second;
        const auto previous_context =
          _state->spot_contexts_by_id.find (std::string (previous->second));
        if (previous_context != _state->spot_contexts_by_id.end ()
            && previous_context->second._state) {
            previous_state = previous_context->second._state;
            const auto admission = previous_state->actor_admissions.find (factory->second.actor_type);
            if (admission != previous_state->actor_admissions.end ()
                && admission->second.on_leave_actor && previous_state->spot_instance) {
                leave_callback = admission->second.on_leave_actor;
            }
        }
        return result_t<void>::success ();
    }).get ();
    if (!plan)
        return plan;
    try {
        if (leave_callback) {
            const auto completed = previous_state->run_serial_task ("spot-actor-leave", [&] {
                return leave_callback (previous_state->spot_instance.get (), actor.get ());
            });
            if (!completed) {
                throw framework_exception_t (completed.error_kind (),
                                             completed.error () != nullptr
                                               ? completed.error ()->what ()
                                               : "spot actor leave callback failed");
            }
        }
        _state->lane.run ([&] {
            if (previous_state)
                decrement_actor_count_unlocked (*previous_state);
            erase_actor_route_unlocked (*_state, key);
        }).get ();
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

result_t<void> spot_node_runtime_t::submit_remote_actor_leave (
  const std::string &transfer_id,
  const actor_ref_t &source_actor,
  const spot_id_t &source_spot_id,
  std::uint64_t source_spot_generation,
  const spot_id_t &target_spot_id,
  const runtime::protocol::actor_route_fence_t &target_fence)
{
    if (transfer_id.empty () || source_spot_id.empty () || target_spot_id.empty ()
        || target_fence.actor_id != source_actor.actor_id ().value ()
        || target_fence.object_generation != source_actor.object_generation ()
        || target_fence.target_node_routing_id.empty () || target_fence.target_node_generation == 0
        || target_fence.authority_owner_generation == 0
        || target_fence.owner_lease_generation == 0) {
        return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                        "remote Actor leave command fence is invalid");
    }

    const auto authority =
      _state->lane.run ([&] { return _state->relocation_authority; }).get ();
    if (!authority) {
        return result_t<void>::failure (framework_error_kind_t::not_configured,
                                        "remote Actor leave command requires a Location Store");
    }
    const auto committed = authority->read (runtime::stateful::object_kind_t::actor,
                                            std::string (source_actor.actor_id ().value ()));
    const auto target_node =
      zlink::routing_id_t::from (target_fence.target_node_routing_id).to_string ();
    // The canonical post-retarget actor row describes its current target,
    // not the historical source node.  The actor key/generation plus the
    // target fence below are the durable one-way leave authorization.
    if (!committed || committed->source.key != source_actor.actor_id ().value ()
        || committed->source.object_generation != source_actor.object_generation ()
        || committed->target.key != source_actor.actor_id ().value ()
        || committed->target.object_generation != source_actor.object_generation ()
        || committed->target.node_id != target_node
        || committed->target.authority_owner_generation != target_fence.authority_owner_generation
        || static_cast<std::uint64_t> (committed->target_owner.lease_generation)
             != target_fence.owner_lease_generation) {
        // A duplicate, late, or stale command is one-way housekeeping. It
        // cannot alter source membership or the committed target route.
        return result_t<void>::success ();
    }

    std::shared_ptr<spot_context_state_t> source_state;
    std::shared_ptr<void> actor_instance;
    std::function<task_t<void> (void *, void *)> leave_callback;
    const auto key = actor_key (source_actor);
    const auto requested_source_spot_generation = source_spot_generation;
    const auto matches_cleanup = [&] (const auto &candidate) {
        return candidate.transfer_id == transfer_id
               && candidate.source_actor.actor_id () == source_actor.actor_id ()
               && ::zlink::framework::detail::actor_ref_access_t::actor_type (
                    candidate.source_actor)
                    == ::zlink::framework::detail::actor_ref_access_t::actor_type (source_actor)
               && candidate.source_actor.object_generation () == source_actor.object_generation ()
               && candidate.source_actor.node_rid ().value () == source_actor.node_rid ().value ()
               && candidate.target_spot_id == target_spot_id;
    };
    struct source_leave_projection_t
    {
        std::shared_ptr<spot_context_state_t> state;
        std::shared_ptr<service::spot_t> native_spot;
        std::uint64_t source_spot_generation = 0;
    };
    const auto projection = _state->lane.run ([&] () -> std::optional<source_leave_projection_t> {
        const auto cleanup = std::find_if (
          _state->pending_remote_source_cleanups.begin (),
          _state->pending_remote_source_cleanups.end (), matches_cleanup);
        auto resolved_generation = requested_source_spot_generation;
        if (cleanup != _state->pending_remote_source_cleanups.end ()) {
            if (cleanup->source_spot_id != source_spot_id || cleanup->source_spot_generation == 0
                || (requested_source_spot_generation != 0
                    && requested_source_spot_generation != cleanup->source_spot_generation)) {
                return std::nullopt;
            }
            resolved_generation = cleanup->source_spot_generation;
        } else if (resolved_generation == 0) {
            return std::nullopt;
        }
        const auto context = _state->spot_contexts_by_id.find (std::string (source_spot_id));
        const auto actor = _state->actor_instances.find (key);
        const auto factory = _state->actor_factories.find (
          std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (source_actor)));
        if (context == _state->spot_contexts_by_id.end () || !context->second._state
            || !context->second._state->spot_instance || actor == _state->actor_instances.end ()
            || !actor->second || factory == _state->actor_factories.end ()) {
            return std::nullopt;
        }
        auto state = context->second._state;
        auto native_spot = state->native_spot.lock ();
        if (!native_spot)
            return std::nullopt;
        return source_leave_projection_t{std::move (state), std::move (native_spot),
                                         resolved_generation};
    }).get ();
    if (!projection
        || projection->native_spot->status ().lifecycle_generation ()
             != projection->source_spot_generation) {
        return result_t<void>::success ();
    }

    const auto committed_leave = _state->lane.run ([&] {
        auto cleanup = std::find_if (
          _state->pending_remote_source_cleanups.begin (),
          _state->pending_remote_source_cleanups.end (), matches_cleanup);
        auto resolved_generation = requested_source_spot_generation;
        if (cleanup != _state->pending_remote_source_cleanups.end ()) {
            if (cleanup->source_spot_id != source_spot_id || cleanup->source_spot_generation == 0
                || (requested_source_spot_generation != 0
                    && requested_source_spot_generation != cleanup->source_spot_generation)) {
                return false;
            }
            resolved_generation = cleanup->source_spot_generation;
        } else if (resolved_generation == 0) {
            return false;
        }
        const auto context = _state->spot_contexts_by_id.find (std::string (source_spot_id));
        const auto actor = _state->actor_instances.find (key);
        const auto factory = _state->actor_factories.find (
          std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (source_actor)));
        if (resolved_generation != projection->source_spot_generation
            || context == _state->spot_contexts_by_id.end ()
            || context->second._state != projection->state || !context->second._state->spot_instance
            || context->second._state->native_spot.lock () != projection->native_spot
            || actor == _state->actor_instances.end () || !actor->second
            || factory == _state->actor_factories.end ()) {
            return false;
        }
        source_state = context->second._state;
        const auto admission = source_state->actor_admissions.find (factory->second.actor_type);
        if (admission != source_state->actor_admissions.end ())
            leave_callback = admission->second.on_leave_actor;

        if (cleanup != _state->pending_remote_source_cleanups.end ()) {
            const auto target =
              _state->actor_transfer_coordinator.message_follow_target (key, cleanup->source_fence);
            if (cleanup->leave_submitted || !target || target->target_fence != target_fence
                || target->route.spot_id != target_spot_id) {
                return false;
            }
            cleanup->leave_submitted = true;
            // No OnLeave handler registered: there is nothing to await, so
            // this transfer's source cleanup is already unblocked.
            if (!leave_callback)
                cleanup->leave_completed = true;
        } else if (!_state->actor_transfer_coordinator.try_submit_source_leave (key, transfer_id))
            return false;

        const auto local = _state->actor_spot_ids.find (key);
        if (local != _state->actor_spot_ids.end () && local->second == source_spot_id) {
            erase_actor_route_unlocked (*_state, key);
        }
        decrement_actor_count_unlocked (*source_state);
        actor_instance = actor->second;
        return true;
    }).get ();
    if (!committed_leave)
        return result_t<void>::success ();

    if (leave_callback) {
        auto state = _state;
        source_state->run_serial_task_async (
          "spot-actor-remote-leave",
          [source_state, actor_instance, leave_callback = std::move (leave_callback)] () mutable {
              return leave_callback (source_state->spot_instance.get (), actor_instance.get ());
          },
          [state = std::move (state), key, transfer_id] (result_t<void>) {
              // Source lifecycle is notification-only. Completion and
              // failure do not participate in the committed target's Join
              // terminal -- but the sweep in
              // cleanup_expired_actor_admissions_at waits for this callback
              // to actually finish (leave_completed) before erasing the
              // local Actor instance the callback just ran against.
              state->lane.run ([&] {
                  const auto found = std::find_if (
                    state->pending_remote_source_cleanups.begin (),
                    state->pending_remote_source_cleanups.end (),
                    [&] (const auto &candidate) { return candidate.transfer_id == transfer_id; });
                  if (found != state->pending_remote_source_cleanups.end ())
                      found->leave_completed = true;
              }).get ();
          });
    }
    return result_t<void>::success ();
}

void spot_node_runtime_t::fail_remote_actor_transfer (
  const actor_ref_t &actor_ref,
  bool reconcile,
  std::optional<reconcile_target_context_t> reconcile_context)
{
    const auto key = actor_key (actor_ref);
    if (reconcile) {
        // Genuinely ambiguous outcome (e.g. the cutover submission itself
        // failed, so whether the target received it is unknown): retain the
        // reconcile phase, but only up to message_follow_duration -- see
        // move_state_t::reconcile_deadline. The captured target identity
        // lets the deadline handler reconcile against Location Store
        // authority truth instead of blindly replaying locally (spec 28
        // relay-ready irreversibility).
        const auto message_follow_duration =
          _state->lane.run ([&] { return _state->message_follow_duration; }).get ();
        _state->actor_transfer_coordinator.mark_reconcile (key, message_follow_duration,
                                                           std::move (reconcile_context));
    } else {
        replay_actor_handoff_until_move_closed (actor_ref, key);
    }
}

task_t<void> spot_node_runtime_t::fast_fail_reconcile_backlog (actor_ref_t actor_ref,
                                                               std::string key)
{
    auto backlog = _state->actor_transfer_coordinator.take_backlog (key);
    for (auto &packet : backlog) {
        if (!packet.is_request)
            continue;
        spot_inbound_message_t metadata;
        metadata.content_type = std::move (packet.content_type);
        metadata.values = std::move (packet.metadata);
        const auto terminal_route = handoff_terminal_route (metadata.values);
        if (!terminal_route)
            continue;
        const auto failure = result_t<zlink::message_t>::failure (
          framework_error_kind_t::unavailable,
          "actor relocation outcome could not be reconciled against the Location "
          "Store; actor remains unavailable");
        try {
            (void) co_await send_handoff_terminal (_state, terminal_route, failure);
        }
        catch (...) {
        }
        if (actor_transfer_marker_enabled ()) {
            emit_actor_transfer_marker ("reconcile_deadline_fast_failed", actor_ref, key);
        }
    }
    co_return;
}

task_t<void> spot_node_runtime_t::complete_remote_actor_transfer (
  const actor_ref_t &source_actor,
  const actor_ref_t &target_actor,
  spot_route_t target_route,
  runtime::protocol::actor_route_fence_t source_fence,
  runtime::protocol::actor_route_fence_t target_fence,
  std::string transfer_id)
{
    std::function<task_t<std::optional<zlink::message_t>> (
      const actor_ref_t &, const runtime::messaging::envelope_header_t &, const zlink::message_t &,
      std::chrono::milliseconds, const zlink::routing_id_t &,
      const runtime::protocol::actor_route_fence_t &, std::uint8_t,
      const runtime::protocol::wire_operation_id_t &, std::uint64_t)>
      late_handoff_relay;
    const auto key = actor_key (source_actor);
    if (transfer_id.empty ()) {
        transfer_id = key;
    }
    const auto late_transfer_id = transfer_id;
    const auto target_spot_id = target_route.spot_id;
    struct source_spot_projection_t
    {
        spot_id_t spot_id;
        std::shared_ptr<service::spot_t> native;
    };
    auto source = _state->lane.run ([&] {
        source_spot_projection_t result;
        if (const auto spot = _state->actor_spot_ids.find (key);
            spot != _state->actor_spot_ids.end ()) {
            result.spot_id = spot->second;
            const auto context = _state->spot_contexts_by_id.find (std::string (result.spot_id));
            if (context != _state->spot_contexts_by_id.end () && context->second._state)
                result.native = context->second._state->native_spot.lock ();
        }
        return result;
    }).get ();
    const auto source_spot_generation =
      source.native ? source.native->status ().lifecycle_generation () : 0;
    _state->lane.run ([&] {
        // Keep the old-generation Message Follow route independent from the actor's
        // A later relocation can return the same Actor incarnation to this node.
        // Retain the committed source fence; the authority owner generation, node
        // lifecycle, and owner lease distinguish it from the newer local target.
        _state->actor_transfer_coordinator.activate_message_follow (
          key, source_fence, target_actor, target_route, target_fence,
          std::chrono::steady_clock::now () + _state->message_follow_duration, transfer_id);
        if (const auto current = _state->actor_authority_fences.find (key);
            current != _state->actor_authority_fences.end () && current->second == source_fence) {
            _state->actor_authority_fences.erase (current);
        }
        detail::record_actor_route_unlocked (*_state, key, std::move (target_route),
                                             target_actor.object_generation ());
        // Commit acknowledgement fixes the new owner and Message Follow route first.
        // Releasing stale source ownership is post-commit housekeeping: losing the
        // source process now cannot roll back the accepted target generation.
        const auto source_leave_submitted =
          _state->actor_transfer_coordinator.source_leave_submitted (key, transfer_id);
        _state->pending_remote_source_cleanups.push_back (
          spot_node_builder_state_t::pending_remote_source_cleanup_t{
            .source_actor = source_actor,
            .source_fence = std::move (source_fence),
            .transfer_id = std::move (transfer_id),
            .source_spot_id = std::move (source.spot_id),
            .source_spot_generation = source_spot_generation,
            .target_spot_id = target_spot_id,
            .not_before = std::chrono::steady_clock::now () + std::chrono::seconds (1),
            .leave_submitted = source_leave_submitted,
            .leave_completed = false,
            .leave_deadline =
              std::chrono::steady_clock::now () + _state->message_follow_duration});
        // Publish the Message Follow route before removing the moving state. A
        // packet that already selected the source route can still reach the
        // coordinator while this transition runs; finish_move_replay() removes the
        // moving state only after an atomic empty-backlog observation.
        late_handoff_relay = _state->actor_message_follow_relay;
    }).get ();

    std::optional<std::chrono::steady_clock::duration> transfer_elapsed;
    for (;;) {
        auto replay = _state->actor_transfer_coordinator.finish_move_replay (key);
        order_bound_session_handoff (replay.backlog);
        for (auto &packet : replay.backlog) {
            if (!late_handoff_relay) {
                if (actor_transfer_marker_enabled ()) {
                    emit_actor_transfer_marker ("handoff_late_relay_unavailable", source_actor,
                                                late_transfer_id);
                }
                continue;
            }
            const auto handoff_source =
              handoff_routing_id (packet.metadata, actor_handoff_source_node_key);
            const auto handoff_route = handoff_actor_route (packet.metadata);
            const auto handoff_hop = handoff_u64 (packet.metadata, actor_handoff_hop_count_key);
            const auto handoff_operation_high =
              handoff_u64 (packet.metadata, actor_handoff_operation_high_key);
            const auto handoff_operation_low =
              handoff_u64 (packet.metadata, actor_handoff_operation_low_key);
            const auto handoff_reply_route =
              handoff_u64 (packet.metadata, actor_handoff_reply_route_key);
            const bool preserves_terminal_route = handoff_source && handoff_operation_high
                                                  && handoff_operation_low && handoff_reply_route;
            const bool preserves_stale_route =
              preserves_terminal_route && handoff_route && handoff_hop && *handoff_hop <= 8;
            runtime::messaging::envelope_header_t header;
            header.kind = packet.is_request ? runtime::messaging::message_kind_t::request
                                            : runtime::messaging::message_kind_t::command;
            header.channel_name = "actor";
            header.message_name = std::move (packet.packet_name);
            header.content_type = std::move (packet.content_type);
            header.metadata = std::move (packet.metadata);
            header.metadata.insert_or_assign ("__zlink.actorHandoffLateReplay", "1");
            // Route through the just-published Message Follow target. The
            // target Actor owner does not admit application records until its
            // completion opens, while that completion waits for this source
            // backlog to drain. The exact target Spot route can stage the
            // packet and acknowledge admission without creating that cycle.
            bool relayed = false;
            try {
                (void) co_await late_handoff_relay (
                  source_actor, header, zlink::message_t::from (std::move (packet.payload)),
                  std::chrono::seconds (30),
                  preserves_terminal_route ? *handoff_source
                                           : zlink::routing_id_t::from (std::uint32_t{0}),
                  preserves_stale_route ? *handoff_route : runtime::protocol::actor_route_fence_t{},
                  preserves_stale_route ? static_cast<std::uint8_t> (*handoff_hop) : 0,
                  preserves_terminal_route
                    ? runtime::protocol::wire_operation_id_t{*handoff_operation_high,
                                                             *handoff_operation_low}
                    : runtime::protocol::wire_operation_id_t{},
                  preserves_terminal_route ? *handoff_reply_route : 0);
                relayed = true;
            }
            catch (...) {
            }
            if (!relayed && actor_transfer_marker_enabled ()) {
                emit_actor_transfer_marker ("handoff_late_relay_failed", source_actor, key);
            }
        }
        if (replay.completed) {
            transfer_elapsed = replay.elapsed;
            break;
        }
    }
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
    co_return;
}

bool spot_node_runtime_t::actor_transfer_marker_enabled () const noexcept
{
    return message_flow_tracer_t (_state->dispatch)
      .enabled_for (message_flow_outcome_t::dispatched);
}

void spot_node_runtime_t::emit_actor_transfer_marker (
  std::string marker,
  const actor_ref_t &actor_ref,
  std::string transfer_id,
  std::optional<spot_id_t> spot_id,
  std::optional<node_rid_t> target_node_rid) const
{
    message_flow_tracer_t (_state->dispatch)
      .trace (message_flow_outcome_t::dispatched, transfer_id, [&] {
          const auto source_node_rid = node_rid ();
          return message_flow_event_t{
            .outcome = message_flow_outcome_t::dispatched,
            .surface = dispatch_error_surface_t::spot_actor,
            .message_kind = dispatch_message_kind_t::actor_request,
            .packet_name = marker,
            .channel_name = target_node_rid
                              ? std::make_optional (std::string (target_node_rid->value ()))
                              : std::nullopt,
            .correlation_id = transfer_id,
            .source_rid = std::string (source_node_rid.value ()),
            .spot_id = spot_id,
            .actor_id = std::string (actor_ref.actor_id ().value ()),
            .flow_id = transfer_id,
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
    cleanup_expired_actor_admissions ();
    const auto pending =
      _state->actor_transfer_coordinator.begin_commit (transfer_id, actor_ref, target_spot_id);
    if (!pending) {
        return result_t<actor_join_reply_t>::failure (
          framework_error_kind_t::protocol_error, "remote actor commit has no matching admission");
    }
    const auto key = actor_key (actor_ref);
    struct prepare_plan_t
    {
        spot_node_builder_state_t::actor_factory_registration_t factory;
        spot_actor_admission_callbacks_t admission;
        std::shared_ptr<spot_context_state_t> context;
        std::string node_rid;
        bool available = false;
        bool lifecycle_available = false;
        bool context_reserved = false;
    };
    const auto plan = _state->lane.run ([&] {
        prepare_plan_t result;
        const auto factory = _state->actor_factories.find (
          std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor_ref)));
        const auto context =
          _state->spot_contexts_by_id.find (std::string (target_spot_id));
        if (factory == _state->actor_factories.end ()
            || context == _state->spot_contexts_by_id.end () || !context->second._state
            || !context->second._state->spot_instance) {
            return result;
        }
        result.factory = factory->second;
        result.context = context->second._state;
        result.node_rid = detail::effective_spot_node_rid (_state->snapshot);
        result.available = true;
        const auto admission = result.context->actor_admissions.find (result.factory.actor_type);
        if (admission != result.context->actor_admissions.end ()) {
            result.admission = admission->second;
            result.lifecycle_available = true;
        }
        if (!result.lifecycle_available)
            return result;
        if (result.context->closed || result.context->close_reservation != 0
            || _state->actor_instances.contains (key) || _state->actor_spot_ids.contains (key)
            || _state->pending_actor_contexts.contains (key)) {
            return result;
        }
        result.context->actor_count++;
        _state->pending_actor_contexts.emplace (key, target_spot_id);
        result.context_reserved = true;
        return result;
    }).get ();
    if (!plan.available) {
        _state->actor_transfer_coordinator.fail_commit (transfer_id, false);
        return result_t<actor_join_reply_t>::failure (
          framework_error_kind_t::not_found, "remote actor commit dependencies are not registered");
    }
    if (!plan.lifecycle_available) {
        _state->actor_transfer_coordinator.fail_commit (transfer_id, false);
        return result_t<actor_join_reply_t>::failure (
          framework_error_kind_t::not_found, "target spot actor lifecycle is not registered");
    }

    const auto committed = ::zlink::framework::detail::actor_ref_access_t::make (
      node_rid_t::from_string (plan.node_rid),
      std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor_ref)),
      std::string (actor_ref.actor_id ().value ()), actor_ref.object_generation ());
    if (!plan.context_reserved) {
        _state->actor_transfer_coordinator.fail_commit (transfer_id, false);
        return result_t<actor_join_reply_t>::failure (
          framework_error_kind_t::unavailable,
          "target Spot closed before the remote Actor could be prepared");
    }

    bool claimed_location = false;
    auto location_claim = claim_pending_actor_location_before_activation (
      _state, pending->source_actor, pending->source_spot_id, committed, *plan.context,
      claimed_location);
    auto release_context_reservation = [&] {
        _state->lane.run ([&] {
            const auto reservation = _state->pending_actor_contexts.find (key);
            if (reservation == _state->pending_actor_contexts.end ()
                || reservation->second != target_spot_id) {
                return;
            }
            decrement_actor_count_unlocked (*plan.context);
            _state->pending_actor_contexts.erase (reservation);
        }).get ();
    };
    if (!location_claim) {
        release_context_reservation ();
        _state->actor_transfer_coordinator.fail_commit (transfer_id, false);
        return result_t<actor_join_reply_t>::failure (
          location_claim.error_kind (), location_claim.error () ? location_claim.error ()->what ()
                                                                : "actor location claim failed");
    }

    auto fail_target_commit = [&] (bool reconcile) {
        release_context_reservation ();
        if (claimed_location) {
            release_actor_location (_state, committed);
        }
        _state->actor_transfer_coordinator.fail_commit (transfer_id, reconcile);
    };

    auto committed_context =
      actor_context_t (actor_context._state, committed, 0, plan.context->mesh_name);
    std::shared_ptr<void> actor;
    try {
        const auto &registration = plan.factory;
        actor = registration.create_context_instance
                  ? registration.create_context_instance (std::move (committed_context))
                  : registration.create_instance (std::string (actor_ref.actor_id ().value ()));
        if (actor && !registration.create_context_instance) {
            registration.configure_instance (actor.get (), committed, &committed_context);
        }
        if (actor
            && registration.relocation.kind == detail::factory_relocation_kind_t::preserve_state) {
            const auto bytes = transfer_state.to_bytes ();
            std::vector<std::byte> payload;
            payload.reserve (bytes.size ());
            std::transform (bytes.begin (), bytes.end (), std::back_inserter (payload),
                            [] (std::uint8_t value) { return static_cast<std::byte> (value); });
            auto restored = registration.restore (actor.get (), std::move (payload), {}).result ();
            if (!restored) {
                fail_target_commit (false);
                return result_t<actor_join_reply_t>::failure (
                  restored.error_kind (),
                  restored.error () ? restored.error ()->what () : "actor restore failed");
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

    auto &target = *plan.context;
    try {
        if (!defer_joined_callback && plan.admission.on_actor_joined) {
            const auto completed =
              target.run_serial_task ("spot-actor-transfer-joined", [&] () -> task_t<void> {
                  const auto updated =
                    actor_gateway_runtime_t (actor_context._state).update_actor_ref (committed);
                  if (!updated) {
                      throw framework_exception_t (updated.error_kind (),
                                                   updated.error () != nullptr
                                                     ? updated.error ()->what ()
                                                     : "target actor gateway ref update failed");
                  }
                  co_await plan.admission.on_actor_joined (target.spot_instance.get (), actor.get ());
              });
            if (!completed) {
                fail_target_commit (false);
                return result_t<actor_join_reply_t>::failure (
                  completed.error_kind (), completed.error () != nullptr
                                             ? completed.error ()->what ()
                                             : "spot actor joined callback failed");
            }
        }
    }
    catch (const framework_exception_t &error) {
        _state->lane.run ([&] {
            detail::record_actor_instance_index_unlocked (*_state, committed, actor.get ());
            _state->actor_instances[actor_key (committed)] = actor;
        }).get ();
        fail_target_commit (true);
        return detail::result_access_t::failure<actor_join_reply_t> (error);
    }
    catch (const std::exception &error) {
        _state->lane.run ([&] {
            detail::record_actor_instance_index_unlocked (*_state, committed, actor.get ());
            _state->actor_instances[actor_key (committed)] = actor;
        }).get ();
        fail_target_commit (true);
        return result_t<actor_join_reply_t>::failure (framework_error_kind_t::internal_failure,
                                                      error.what ());
    }
    catch (...) {
        _state->lane.run ([&] {
            detail::record_actor_instance_index_unlocked (*_state, committed, actor.get ());
            _state->actor_instances[actor_key (committed)] = actor;
        }).get ();
        fail_target_commit (true);
        return result_t<actor_join_reply_t>::failure (framework_error_kind_t::internal_failure,
                                                      "target joined callback failed");
    }

    // The node lane was released around the joined callback above — the
    // only window in this synchronous PREPARE where a newer exact identity
    // for the same object can run try_add_admission's target_committing
    // eviction (spec 15 §4.2 newest-attempt-wins) concurrently. Re-check
    // before publishing the staged Actor: installing it for an already
    // evicted transfer would let a dead identity's late PREPARE win the
    // actor_key slot instead of discarding it (spec 15 §4.2 "이전 identity의
    // 늦은 chunk와 Restore는 조립에 연결하지 않고 폐기한다").
    if (!_state->actor_transfer_coordinator.is_current (key, transfer_id)) {
        fail_target_commit (false);
        return result_t<actor_join_reply_t>::failure (
          framework_error_kind_t::rejected,
          "remote Actor Join admission was superseded by a newer attempt");
    }
    const auto installed = _state->lane.run ([&] {
        const auto context = _state->spot_contexts_by_id.find (std::string (target_spot_id));
        if (context == _state->spot_contexts_by_id.end ()
            || context->second._state != plan.context || target.closed
            || target.close_reservation != 0 || !target.spot_instance) {
            return false;
        }
        const auto reservation = _state->pending_actor_contexts.find (key);
        if (reservation == _state->pending_actor_contexts.end ()
            || reservation->second != target_spot_id) {
            return false;
        }
        const auto coordinator = target.ensure_spot_serial_executor ();
        if (!coordinator || !coordinator->ensure_actor_queue (key))
            return false;
        detail::record_actor_instance_index_unlocked (*_state, committed, actor.get ());
        _state->actor_instances[key] = std::move (actor);
        _state->destroyed_actor_keys.erase (key);
        // Core installs the target Actor during the finalize transfer fence.
        decrement_actor_count_unlocked (target);
        _state->pending_actor_contexts.erase (reservation);
        detail::record_actor_context_route_unlocked (*_state, key, plan.node_rid, target,
                                                     committed.object_generation ());
        return true;
    }).get ();
    if (!installed) {
        fail_target_commit (false);
        return result_t<actor_join_reply_t>::failure (
          framework_error_kind_t::unavailable,
          "target Spot closed before the remote Actor could be staged");
    }
    return result_t<actor_join_reply_t>::success (
      actor_join_reply_t{0, committed, zlink::message_t{}});
}

result_t<void> spot_node_runtime_t::stage_remote_actor_commit_backlog (
  const std::string &transfer_id, std::vector<handoff_packet_t> handoff_backlog)
{
    if (!_state->actor_transfer_coordinator.stage_commit_backlog (transfer_id,
                                                                  std::move (handoff_backlog))) {
        return result_t<void>::failure (
          framework_error_kind_t::protocol_error,
          "target Actor handoff backlog could not be staged atomically");
    }
    return result_t<void>::success ();
}

result_t<void> spot_node_runtime_t::commit_remote_actor_authority (
  const std::string &transfer_id,
  const actor_ref_t &actor_ref,
  const spot_id_t &target_spot_id,
  std::uint64_t target_spot_generation,
  std::uint64_t source_authority_owner_generation,
  std::string source_mesh_name,
  std::string target_mesh_name,
  std::uint64_t target_node_lifecycle_generation,
  location_owner_token_t target_owner,
  std::uint64_t *committed_previous_authority_owner_generation,
  std::uint64_t *committed_target_authority_owner_generation)
{
    const auto pending =
      _state->actor_transfer_coordinator.pending_commit (transfer_id, actor_ref, target_spot_id);
    if (!pending) {
        return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                        "remote Actor authority commit has no pending admission");
    }
    struct authority_commit_plan_t
    {
        std::shared_ptr<runtime::stateful::authority_relocation_port_t> relocation_authority;
        std::shared_ptr<service::mesh_node_t> native;
        runtime::location_lifecycle_t *location_lifecycle = nullptr;
        std::string target_node_id;
        std::string local_mesh_name;
        std::uint64_t membership_epoch = 0;
    };
    const auto plan = _state->lane.run ([&] {
        authority_commit_plan_t result;
        result.relocation_authority = _state->relocation_authority;
        result.native = _state->native_node.lock ();
        result.location_lifecycle = _state->location_lifecycle;
        result.target_node_id = detail::effective_spot_node_rid (_state->snapshot);
        result.local_mesh_name = _state->snapshot.name;
        const auto epoch = _state->core_actor_membership_epochs.find (
          std::string (actor_ref.actor_id ().value ()));
        if (epoch != _state->core_actor_membership_epochs.end ())
            result.membership_epoch = epoch->second;
        return result;
    }).get ();
    if (!plan.relocation_authority) {
        return result_t<void>::failure (framework_error_kind_t::not_configured,
                                        "remote Actor authority commit requires a Location Store");
    }
    if (source_authority_owner_generation == 0
        || source_authority_owner_generation == std::numeric_limits<std::uint64_t>::max ()
        || target_mesh_name.empty () || target_node_lifecycle_generation == 0
        || target_owner.owner_id.empty () || target_owner.lease_generation <= 0) {
        return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                        "remote Actor authority commit fence is invalid");
    }

    const auto &native = plan.native;
    const auto &target_node_id = plan.target_node_id;
    if (!native || native->status ().routing_id ().to_string () != target_node_id
        || native->status ().lifecycle_generation () != target_node_lifecycle_generation) {
        return result_t<void>::failure (framework_error_kind_t::unavailable,
                                        "remote Actor authority target lifecycle is stale");
    }
    if (target_spot_generation == 0 || source_mesh_name.empty () || target_mesh_name.empty ()) {
        return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                        "remote Actor authority route identity is incomplete");
    }
    const runtime::stateful::object_ref_t source{runtime::stateful::object_kind_t::actor,
                                                 std::string (actor_ref.actor_id ().value ()),
                                                 actor_ref.object_generation (),
                                                 source_authority_owner_generation,
                                                 std::move (source_mesh_name),
                                                 std::string (actor_ref.node_rid ().value ())};
    const runtime::stateful::object_ref_t target{
      runtime::stateful::object_kind_t::actor, source.key,       source.object_generation,
      source.authority_owner_generation + 1,   target_mesh_name, target_node_id};
    const auto target_actor = ::zlink::framework::detail::actor_ref_access_t::make (
      node_rid_t::from_string (target_node_id),
      std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor_ref)),
      std::string (actor_ref.actor_id ().value ()), actor_ref.object_generation ());

    std::vector<std::byte> inventory_bytes;
    inventory_bytes.reserve (source.key.size () + sizeof (source.object_generation)
                             + sizeof (source.authority_owner_generation));
    for (const auto value : source.key)
        inventory_bytes.push_back (static_cast<std::byte> (static_cast<unsigned char> (value)));
    for (int shift = 56; shift >= 0; shift -= 8) {
        inventory_bytes.push_back (
          static_cast<std::byte> ((source.object_generation >> shift) & 0xffu));
        inventory_bytes.push_back (
          static_cast<std::byte> ((source.authority_owner_generation >> shift) & 0xffu));
    }
    const auto digest = runtime::sha256 (inventory_bytes);
    runtime::stateful::inventory_digest_t inventory_digest{};
    for (std::size_t index = 0; index != inventory_digest.size (); ++index)
        inventory_digest[index] = std::to_integer<std::uint8_t> (digest[index]);

    runtime::stateful::authority_publish_result_t published;
    try {
        const object_creation_target_t target_placement{
          target_mesh_name, node_rid_t::from_string (target_node_id),
          target_node_lifecycle_generation, target_owner};
        published = plan.relocation_authority->publish (
          source, target, target_owner, target_placement, {}, 0, inventory_digest,
          runtime::encode_actor_authority_payload (runtime::actor_authority_payload_t{
            .state = runtime::actor_authority_state_t::ready,
            .stable_type = std::string (
              ::zlink::framework::detail::actor_ref_access_t::actor_type (target_actor)),
            .actor_id = std::string (target_actor.actor_id ().value ()),
            .current_spot_id = target_spot_id,
            .current_spot_generation = target_spot_generation,
            .current_spot_kind = runtime::actor_authority_spot_kind_t::user,
            .owner_id = target_owner.owner_id,
            .owner_lease_generation = static_cast<std::uint64_t> (target_owner.lease_generation),
            .mesh_name = target_placement.mesh_name,
            .node_rid = target_placement.node_rid,
            .node_generation = target_placement.node_lifecycle_generation}));
        if (published.status != runtime::stateful::authority_publish_status_t::published
            || !published.current) {
            published.current = plan.relocation_authority->read (source.kind, source.key);
        }
    }
    catch (const std::exception &error) {
        return result_t<void>::failure (framework_error_kind_t::internal_failure, error.what ());
    }
    const auto &current = published.current;
    if (!current) {
        return result_t<void>::failure (
          framework_error_kind_t::unavailable,
          "remote Actor authority commit conflicted with current authority");
    }
    if (current->source != source) {
        return result_t<void>::failure (
          framework_error_kind_t::unavailable,
          "remote Actor authority commit conflicted with current authority");
    }
    const bool target_identity_matches =
      current->target.kind == target.kind && current->target.key == target.key
      && current->target.object_generation == target.object_generation
      && current->target.mesh_name == target.mesh_name && current->target.node_id == target.node_id
      && current->target.authority_owner_generation > current->source.authority_owner_generation;
    if (!target_identity_matches) {
        return result_t<void>::failure (
          framework_error_kind_t::unavailable,
          "remote Actor authority commit conflicted with current authority");
    }
    if (current->target_owner.owner_id != target_owner.owner_id
        || current->target_owner.lease_generation != target_owner.lease_generation) {
        return result_t<void>::failure (
          framework_error_kind_t::unavailable,
          "remote Actor authority commit conflicted with current authority");
    }
    if (!current->relocation_reference.empty () || current->checksum_crc32c != 0) {
        return result_t<void>::failure (
          framework_error_kind_t::unavailable,
          "remote Actor authority commit conflicted with current authority");
    }
    if (current->inventory_digest != inventory_digest) {
        return result_t<void>::failure (
          framework_error_kind_t::unavailable,
          "remote Actor authority commit conflicted with current authority");
    }
    //  Spec 28-relocation-flow §8: the committed target authority row is the
    //  node's own local Actor authority from this point on. Adopt it into the
    //  local object record so a LATER Join originating from this node fences
    //  with the generation the Store actually holds. Adoption is advisory —
    //  a record that is not in a locally advanceable state keeps the
    //  committed authority in the fence map below and reconciles on its own
    //  lifecycle path, so it must never fail the committed transfer.
    const runtime::stateful::object_ref_t committed_target{
      runtime::stateful::object_kind_t::actor,
      current->target.key,
      current->target.object_generation,
      current->target.authority_owner_generation,
      current->target.mesh_name,
      current->target.node_id};
    (void) native->advance_local_actor_authority (committed_target);
    const auto local_node_routing_id = native->status ().routing_id ().to_bytes ();
    _state->lane.run ([&] {
        _state->actor_authority_fences.insert_or_assign (
          actor_key (target_actor),
          runtime::protocol::actor_route_fence_t{
            std::string (target_actor.actor_id ().value ()), target_actor.object_generation (),
            local_node_routing_id, target_node_lifecycle_generation,
            current->target.authority_owner_generation,
            static_cast<std::uint64_t> (target_owner.lease_generation)});
    }).get ();
    //  Spec 28-relocation-flow §8: the cross-node Join moved the Actor owner
    //  to this target in the same CAS, and the staged transfer skips the
    //  ordinary local claim. Adopt the committed authority into this node's
    //  location lifecycle now — otherwise update_actor_location_after_move
    //  and a later Spot leave renew against an untracked entry.
    if (plan.location_lifecycle
        && !plan.location_lifecycle->owns_actor (actor_location_key_t{
          plan.local_mesh_name, std::string (target_actor.actor_id ().value ())})) {
        const auto adopted = plan.location_lifecycle->claim_actor (
          actor_location_t{
            .mesh_name = plan.local_mesh_name,
            .actor_id = std::string (target_actor.actor_id ().value ()),
            .actor_type = std::string (
              ::zlink::framework::detail::actor_ref_access_t::actor_type (target_actor)),
            .actor_ref = target_actor,
            .owner_node_rid = zlink::routing_id_t::from (std::string (target_node_id)),
            .owner_node_generation = target_node_lifecycle_generation,
            .spot_id = target_spot_id,
            .spot_generation = target_spot_generation,
            .spot_kind = zlink::spot_kind::user,
            .membership_epoch = plan.membership_epoch},
          [weak_state = std::weak_ptr<detail::spot_node_builder_state_t> (_state)] (
            const actor_location_t &lost) { deactivate_actor_location (weak_state, lost); },
          true);
        if (adopted.status != location_write_status_t::stored) {
            return result_t<void>::failure (framework_error_kind_t::internal_failure,
                                            "committed remote Actor authority adoption failed");
        }
    }
    if (committed_previous_authority_owner_generation != nullptr)
        *committed_previous_authority_owner_generation = current->source.authority_owner_generation;
    if (committed_target_authority_owner_generation != nullptr)
        *committed_target_authority_owner_generation = current->target.authority_owner_generation;
    if (actor_transfer_marker_enabled ()) {
        emit_actor_transfer_marker ("location_committed", target_actor, transfer_id,
                                    target_spot_id);
    }
    return result_t<void>::success ();
}

std::optional<actor_join_reply_t>
spot_node_runtime_t::completed_remote_actor_commit (const std::string &transfer_id,
                                                    const actor_ref_t &source_actor,
                                                    const spot_id_t &target_spot_id) const
{
    const auto completed = _state->actor_transfer_coordinator.completed_commit (
      transfer_id, source_actor, target_spot_id);
    if (!completed)
        return std::nullopt;
    const auto target_actor = ::zlink::framework::detail::actor_ref_access_t::make (
      node_rid (),
      std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (source_actor)),
      std::string (source_actor.actor_id ().value ()), source_actor.object_generation ());
    return actor_join_reply_t{0, target_actor, zlink::message_t{}};
}

result_t<actor_join_reply_t>
spot_node_runtime_t::commit_remote_actor_to_spot (std::string transfer_id,
                                                  const actor_ref_t &actor_ref,
                                                  spot_id_t target_spot_id,
                                                  zlink::message_t transfer_state,
                                                  actor_context_t actor_context,
                                                  std::vector<handoff_packet_t> handoff_backlog,
                                                  service_provider_t *services)
{
    auto prepared =
      prepare_remote_actor_to_spot (transfer_id, actor_ref, target_spot_id,
                                    std::move (transfer_state), std::move (actor_context));
    if (!prepared) {
        return prepared;
    }
    if (!handoff_backlog.empty () && services == nullptr) {
        _state->actor_transfer_coordinator.fail_commit (transfer_id, true);
        return result_t<actor_join_reply_t>::failure (
          framework_error_kind_t::protocol_error,
          "remote actor handoff backlog requires a service provider");
    }
    const auto staged =
      stage_remote_actor_commit_backlog (transfer_id, std::move (handoff_backlog));
    if (!staged) {
        _state->actor_transfer_coordinator.fail_commit (transfer_id, true);
        return detail::propagate_failure<actor_join_reply_t> (
          staged, "remote actor handoff backlog staging failed");
    }
    if (services != nullptr) {
        return finalize_remote_actor_to_spot (std::move (transfer_id), actor_ref,
                                              std::move (target_spot_id), *services);
    }
    service_collection_t empty_services;
    auto empty_provider = empty_services.build_provider ();
    return finalize_remote_actor_to_spot (std::move (transfer_id), actor_ref,
                                          std::move (target_spot_id), empty_provider);
}

void spot_node_runtime_t::finalize_remote_actor_to_spot_async (
  std::string transfer_id,
  const actor_ref_t &actor_ref,
  spot_id_t target_spot_id,
  service_provider_t &services,
  actor_gateway_runtime_t *actor_gateway,
  std::optional<std::chrono::steady_clock::time_point> deadline,
  std::function<void (result_t<actor_join_reply_t>)> completion,
  std::function<task_t<void> ()> submit_source_leave)
{
    if (!completion)
        return;
    struct completion_state_t
    {
        std::mutex mutex;
        bool settled = false;
        std::function<void (result_t<actor_join_reply_t>)> callback;
    };
    auto completion_state = std::make_shared<completion_state_t> ();
    completion_state->callback = std::move (completion);
    auto complete_result = [completion_state] (result_t<actor_join_reply_t> result) mutable {
        std::function<void (result_t<actor_join_reply_t>)> callback;
        {
            std::lock_guard lock (completion_state->mutex);
            if (completion_state->settled)
                return;
            completion_state->settled = true;
            callback = std::move (completion_state->callback);
        }
        if (callback)
            callback (std::move (result));
    };
    const auto pending =
      _state->actor_transfer_coordinator.pending_commit (transfer_id, actor_ref, target_spot_id);
    if (!pending) {
        complete_result (result_t<actor_join_reply_t>::failure (
          framework_error_kind_t::protocol_error,
          "remote actor finalize has no matching prepared commit"));
        return;
    }
    if (pending->completion_operation_id_high == 0 && pending->completion_operation_id_low == 0) {
        _state->actor_transfer_coordinator.fail_commit (transfer_id, true);
        complete_result (result_t<actor_join_reply_t>::failure (
          framework_error_kind_t::protocol_error,
          "remote Actor Join completion OperationId is invalid"));
        return;
    }
    const auto key = actor_key (actor_ref);
    struct finalize_plan_t
    {
        std::shared_ptr<spot_context_state_t> target_state;
        std::shared_ptr<void> actor_instance;
        std::shared_ptr<spot_serial_executor_t> executor;
        const void *actor_queue_identity = nullptr;
        std::shared_ptr<runtime::offload_executor_t> deadline_executor;
        std::function<task_t<void> (void *, void *)> joined_callback;
        std::string node_rid;
        std::chrono::milliseconds request_timeout{
          std::chrono::duration_cast<std::chrono::milliseconds> (std::chrono::seconds (30))};
        bool dependencies_available = false;
        bool lifecycle_available = true;
    };
    const auto plan = _state->lane.run ([&] {
        finalize_plan_t result;
        result.node_rid = detail::effective_spot_node_rid (_state->snapshot);
        if (_state->channel_runtime)
            result.request_timeout = _state->channel_runtime->default_request_timeout;
        const auto context =
          _state->spot_contexts_by_id.find (std::string (target_spot_id));
        const auto factory = _state->actor_factories.find (
          std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor_ref)));
        const auto actor = _state->actor_instances.find (key);
        if (context == _state->spot_contexts_by_id.end () || !context->second._state
            || !context->second._state->spot_instance || factory == _state->actor_factories.end ()
            || actor == _state->actor_instances.end () || !actor->second) {
            return result;
        }
        result.target_state = context->second._state;
        result.actor_instance = actor->second;
        if (actor_gateway != nullptr) {
            const auto admission =
              result.target_state->actor_admissions.find (factory->second.actor_type);
            if (admission == result.target_state->actor_admissions.end ()) {
                result.lifecycle_available = false;
                return result;
            }
            result.joined_callback = admission->second.on_actor_joined;
        }
        result.executor = result.target_state->ensure_spot_serial_executor ();
        if (!result.executor)
            return result;
        result.actor_queue_identity = result.executor->actor_queue_identity (key);
        if (!result.actor_queue_identity)
            return result;
        result.deadline_executor = framework_deadline_executor_core (_state);
        result.dependencies_available = true;
        return result;
    }).get ();
    if (!plan.dependencies_available) {
        _state->actor_transfer_coordinator.fail_commit (transfer_id, true);
        complete_result (result_t<actor_join_reply_t>::failure (
          plan.lifecycle_available ? framework_error_kind_t::not_found
                                   : framework_error_kind_t::not_found,
          plan.lifecycle_available ? "prepared remote actor commit dependencies are unavailable"
                                   : "target spot actor lifecycle is not registered"));
        return;
    }
    if (!deadline)
        deadline = std::chrono::steady_clock::now () + plan.request_timeout;
    const auto committed = ::zlink::framework::detail::actor_ref_access_t::make (
      node_rid_t::from_string (plan.node_rid),
      std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor_ref)),
      std::string (actor_ref.actor_id ().value ()), actor_ref.object_generation ());
    const auto actor_instance = plan.actor_instance;
    const auto target_state = plan.target_state;
    const auto executor = plan.executor;
    std::optional<actor_gateway_runtime_t> actor_gateway_owner;
    if (actor_gateway != nullptr)
        actor_gateway_owner.emplace (*actor_gateway);
    auto joined_callback = plan.joined_callback;
    const auto actor_queue_identity = plan.actor_queue_identity;
    std::weak_ptr<spot_node_builder_state_t> node_owner = _state;
    std::weak_ptr<spot_context_state_t> target_state_owner = target_state;
    std::weak_ptr<void> actor_instance_owner = actor_instance;
    auto services_owner = services;

    auto commit_state = std::make_shared<remote_actor_commit_turn_state_t> ();
    struct submission_state_t
    {
        enum class phase_t
        {
            lifecycle,
            actor
        };

        std::mutex mutex;
        std::weak_ptr<runtime::serial_execution_queue_t> queue;
        std::optional<runtime::serial_submission_id_t> id;
        bool cancellation_requested = false;
        bool lifecycle_was_active = false;
        bool lifecycle_started = false;
        phase_t phase = phase_t::lifecycle;
    };
    struct deadline_state_t
    {
        std::mutex mutex;
        std::shared_ptr<remote_actor_commit_deadline_t> timer;
    };
    auto submission_state = std::make_shared<submission_state_t> ();
    auto track_submission =
      [submission_state] (const std::shared_ptr<runtime::serial_execution_queue_t> &queue,
                          runtime::serial_submission_id_t id) {
          bool cancel = false;
          {
              std::lock_guard lock (submission_state->mutex);
              submission_state->queue = queue;
              submission_state->id = id;
              cancel = submission_state->cancellation_requested;
          }
          if (cancel)
              (void) queue->cancel_submission (id);
      };
    auto deadline_state = std::make_shared<deadline_state_t> ();
    auto deadline_executor = plan.deadline_executor;
    std::weak_ptr<spot_node_builder_state_t> terminal_node = _state;
    commit_state->on_terminal (
      [terminal_node, transfer_id, committed, deadline_state,
       complete_result] (remote_actor_commit_turn_state_t::outcome_t outcome) mutable {
          std::shared_ptr<remote_actor_commit_deadline_t> timer;
          {
              std::lock_guard lock (deadline_state->mutex);
              timer = std::move (deadline_state->timer);
          }
          if (timer)
              timer->cancel ();
          auto node = terminal_node.lock ();
          if (!outcome.success && node)
              node->actor_transfer_coordinator.fail_commit (transfer_id, true);
          if (outcome.success) {
              complete_result (result_t<actor_join_reply_t>::success (
                actor_join_reply_t{0, committed, zlink::message_t{}}));
          } else {
              complete_result (result_t<actor_join_reply_t>::failure (
                outcome.error_kind,
                outcome.error.empty () ? "remote Actor handoff replay failed" : outcome.error));
          }
      });
    if (deadline) {
        std::weak_ptr<remote_actor_commit_turn_state_t> weak_commit = commit_state;
        auto timer = remote_actor_commit_deadline_t::start (
          *deadline, deadline_executor, [weak_commit, submission_state] {
              auto commit = weak_commit.lock ();
              if (!commit)
                  return;
              commit->select_cancel_error (
                framework_error_kind_t::deadline_exceeded,
                "remote Actor handoff replay exceeded the Join deadline");
              std::shared_ptr<runtime::serial_execution_queue_t> queue;
              std::optional<runtime::serial_submission_id_t> id;
              submission_state_t::phase_t phase;
              bool lifecycle_started = false;
              {
                  std::lock_guard lock (submission_state->mutex);
                  submission_state->cancellation_requested = true;
                  queue = submission_state->queue.lock ();
                  id = submission_state->id;
                  phase = submission_state->phase;
                  lifecycle_started = submission_state->lifecycle_started;
                  if (phase == submission_state_t::phase_t::lifecycle && (!queue || !id)) {
                      submission_state->lifecycle_was_active = lifecycle_started;
                  }
              }
              if (queue && id)
                  (void) queue->cancel_submission (*id);
              if (phase == submission_state_t::phase_t::lifecycle)
                  return;
              commit->request_stop ();
          });
        bool terminal = false;
        {
            std::lock_guard lock (deadline_state->mutex);
            deadline_state->timer = timer;
            terminal = commit_state->terminal ();
        }
        if (terminal) {
            timer->cancel ();
            std::lock_guard lock (deadline_state->mutex);
            deadline_state->timer.reset ();
        }
    }
    const auto completion_operation_high = pending->completion_operation_id_high;
    const auto completion_operation_low = pending->completion_operation_id_low;
    const auto admission_reply = pending->admission_reply;
    auto submit_actor_turn = [node_owner, actor_ref, committed, key, target_spot_id,
                              target_state_owner, actor_instance_owner, executor,
                              actor_queue_identity, services_owner = std::move (services_owner),
                              transfer_id, deadline, completion_operation_high,
                              completion_operation_low, admission_reply, commit_state,
                              submission_state,
                              submit_source_leave = std::move (submit_source_leave),
                              track_submission] (result_t<void> joined) mutable {
        bool cancellation_requested = false;
        bool lifecycle_was_active = false;
        {
            std::lock_guard lock (submission_state->mutex);
            submission_state->queue.reset ();
            submission_state->id.reset ();
            cancellation_requested = submission_state->cancellation_requested;
            lifecycle_was_active =
              submission_state->lifecycle_was_active
              || (cancellation_requested && submission_state->lifecycle_started);
            submission_state->phase = submission_state_t::phase_t::actor;
            if (lifecycle_was_active)
                submission_state->cancellation_requested = false;
        }
        if (cancellation_requested && !lifecycle_was_active) {
            commit_state->request_stop ();
            return;
        }
        if (commit_state->terminal ())
            return;
        if (cancellation_requested && lifecycle_was_active) {
            joined =
              result_t<void>::failure (framework_error_kind_t::deadline_exceeded,
                                       "spot actor joined callback exceeded the Join deadline");
        } else if (joined && deadline && std::chrono::steady_clock::now () >= *deadline) {
            joined =
              result_t<void>::failure (framework_error_kind_t::deadline_exceeded,
                                       "spot actor joined callback exceeded the Join deadline");
        }
        auto submitted = executor->execute_actor_cancellable (
          key,
          "remote-actor-commit-replay",
          [node_owner, actor_ref, committed, key, target_spot_id, target_state_owner,
           actor_instance_owner, actor_queue_identity, executor,
           services_owner = std::move (services_owner),
           joined = std::move (joined), transfer_id, deadline, completion_operation_high,
           completion_operation_low, admission_reply, commit_state,
           submit_source_leave = std::move (submit_source_leave)] (auto actor_complete) mutable {
              auto settle_turn = [commit_state, actor_complete] (
                                   remote_actor_commit_turn_state_t::outcome_t outcome) mutable {
                  actor_complete ([commit_state, outcome = std::move (outcome)] () mutable {
                      commit_state->settle (std::move (outcome));
                  });
              };
              if (!commit_state->begin_active ()) {
                  actor_complete ([] {});
                  return;
              }

              auto node = node_owner.lock ();
              auto target_state = target_state_owner.lock ();
              auto actor_instance = actor_instance_owner.lock ();
              if (!node || !target_state || !actor_instance) {
                  settle_turn (remote_actor_commit_turn_state_t::outcome_t{
                    false, framework_error_kind_t::shutting_down,
                    "remote Actor handoff owner was released before activation"});
                  return;
              }
              auto replay_services = std::move (services_owner);

              runtime::actor_execution_scope_t actor_scope (key, std::string (target_spot_id));
              auto fail_precommit = [node, transfer_id, settle_turn] (framework_error_kind_t kind,
                                                                      std::string error) mutable {
                  node->actor_transfer_coordinator.fail_commit (transfer_id, true);
                  settle_turn (
                    remote_actor_commit_turn_state_t::outcome_t{false, kind, std::move (error)});
              };
              try {
                  const auto exact_pending = node->actor_transfer_coordinator.pending_commit (
                    transfer_id, actor_ref, target_spot_id);
                  const auto owner_fence_changed = !exact_pending || !node->lane.run ([&] {
                      const auto context =
                        node->spot_contexts_by_id.find (std::string (target_spot_id));
                      const auto actor = node->actor_instances.find (key);
                      const auto generation = node->actor_generations.find (key);
                      if (!exact_pending || context == node->spot_contexts_by_id.end ()
                          || context->second._state != target_state || !target_state->spot_instance
                          || actor == node->actor_instances.end ()
                          || actor->second != actor_instance
                          || generation == node->actor_generations.end ()
                          || generation->second != committed.object_generation ()
                          || !executor->actor_queue_matches (key, actor_queue_identity)) {
                          return false;
                      }
                      return true;
                  }).get ();
                  if (owner_fence_changed) {
                      node->actor_transfer_coordinator.fail_commit (transfer_id, true);
                      settle_turn (remote_actor_commit_turn_state_t::outcome_t{
                        false, framework_error_kind_t::invalid_operation,
                        "remote Actor handoff owner fence changed before activation"});
                      return;
                  }
                  // This transition decides the deadline race before any Join
                  // completion or location publication. Once it succeeds, the
                  // accepted transfer owns the Actor turn until every retained
                  // packet settles; later cancellation cannot truncate replay.
                  if (!commit_state->try_begin_commit ()) {
                      const auto cancelled = commit_state->cancel_outcome ();
                      fail_precommit (cancelled.error_kind, cancelled.error);
                      return;
                  }

                  const bool target_joined = static_cast<bool> (joined);
                  auto continue_after_source_leave = [node, actor_ref, committed, key,
                                                      target_spot_id, target_state, actor_instance,
                                                      actor_queue_identity, executor,
                                                      services = std::move (replay_services),
                                                      transfer_id, deadline,
                                                      completion_operation_high,
                                                      completion_operation_low, admission_reply,
                                                      commit_state, joined = std::move (joined),
                                                      settle_turn] (
                                                       const result_t<void> &) mutable {
                      runtime::actor_execution_scope_t actor_scope (key,
                                                                    std::string (target_spot_id));
                      std::optional<std::pair<framework_error_kind_t, std::string>>
                        completion_failure;
                      if (!joined) {
                          completion_failure =
                            std::make_pair (joined.error_kind (),
                                            joined.error () != nullptr
                                              ? std::string (joined.error ()->what ())
                                              : std::string ("spot actor joined callback failed"));
                      }
                      actor_join_completion_t completion =
                        completion_failure
                          ? actor_join_completion_t (actor_join_failed_t{completion_operation_high,
                                                                         completion_operation_low,
                                                                         completion_failure->first})
                          : actor_join_completion_t (actor_join_accepted_t{
                              completion_operation_high, completion_operation_low, committed,
                              admission_reply});
                      spot_node_runtime_t (node).deliver_actor_join_completion_async (
                        committed, std::move (completion), target_spot_id,
                        [node, actor_ref, committed, key, target_spot_id, target_state,
                         actor_instance, actor_queue_identity, executor,
                         services = std::move (services),
                         transfer_id, completion_failure, commit_state,
                         settle_turn] (result_t<void> delivered) mutable {
                            runtime::actor_execution_scope_t actor_scope (
                              key, std::string (target_spot_id));
                            if (!delivered) {
                                node->actor_transfer_coordinator.fail_commit (transfer_id, true);
                                settle_turn (remote_actor_commit_turn_state_t::outcome_t{
                                  false, delivered.error_kind (),
                                  delivered.error () != nullptr
                                    ? delivered.error ()->what ()
                                    : "remote Actor Join completion callback failed"});
                                return;
                            }
                            if (completion_failure) {
                                node->actor_transfer_coordinator.fail_commit (transfer_id, true);
                                settle_turn (remote_actor_commit_turn_state_t::outcome_t{
                                  false, completion_failure->first, completion_failure->second});
                                return;
                            }

                            std::optional<std::vector<handoff_packet_t>> replay;
                            std::optional<remote_actor_commit_turn_state_t::outcome_t>
                              commit_failure;
                            const auto exact_pending =
                              node->actor_transfer_coordinator.pending_commit (
                                transfer_id, actor_ref, target_spot_id);
                            struct completion_plan_t
                            {
                                bool valid = false;
                                std::function<result_t<void> (const actor_ref_t &)> update_actor_ref;
                            };
                            const auto plan = node->lane.run ([&] {
                                completion_plan_t result;
                                const auto context =
                                  node->spot_contexts_by_id.find (std::string (target_spot_id));
                                const auto actor = node->actor_instances.find (key);
                                const auto generation = node->actor_generations.find (key);
                                if (!exact_pending || context == node->spot_contexts_by_id.end ()
                                    || context->second._state != target_state
                                    || !target_state->spot_instance
                                    || actor == node->actor_instances.end ()
                                    || actor->second != actor_instance
                                    || generation == node->actor_generations.end ()
                                    || generation->second != committed.object_generation ()
                                    || !executor->actor_queue_matches (key, actor_queue_identity)) {
                                    return result;
                                }
                                result.valid = true;
                                result.update_actor_ref = node->update_actor_registry_ref;
                                return result;
                            }).get ();
                            if (!exact_pending || !plan.valid) {
                                commit_failure.emplace (
                                  false, framework_error_kind_t::invalid_operation,
                                  "remote Actor handoff owner fence changed during completion");
                            } else {
                                const auto location_updated = update_actor_location_after_move (
                                  node, committed, *target_state, false);
                                if (!location_updated) {
                                    commit_failure.emplace (
                                      false, location_updated.error_kind (),
                                      location_updated.error () != nullptr
                                        ? location_updated.error ()->what ()
                                        : "actor committed location update failed");
                                } else if (plan.update_actor_ref) {
                                    const auto updated = plan.update_actor_ref (committed);
                                    if (!updated) {
                                        commit_failure.emplace (
                                          false, updated.error_kind (),
                                          updated.error () != nullptr ? updated.error ()->what ()
                                                                      : "actor ref update failed");
                                    }
                                }
                            }
                            if (!commit_failure) {
                                const auto still_pending =
                                  node->actor_transfer_coordinator.pending_commit (
                                    transfer_id, actor_ref, target_spot_id);
                                const auto still_owned = still_pending && node->lane.run ([&] {
                                    const auto context = node->spot_contexts_by_id.find (
                                      std::string (target_spot_id));
                                    const auto actor = node->actor_instances.find (key);
                                    const auto generation = node->actor_generations.find (key);
                                    return context != node->spot_contexts_by_id.end ()
                                           && context->second._state == target_state
                                           && target_state->spot_instance
                                           && actor != node->actor_instances.end ()
                                           && actor->second == actor_instance
                                           && generation != node->actor_generations.end ()
                                           && generation->second
                                                == committed.object_generation ()
                                           && executor->actor_queue_matches (key, actor_queue_identity);
                                }).get ();
                                if (still_owned) {
                                    replay = node->actor_transfer_coordinator
                                               .complete_commit_and_take_backlog (
                                                 transfer_id, actor_ref, target_spot_id);
                                } else {
                                    commit_failure.emplace (
                                      false, framework_error_kind_t::invalid_operation,
                                      "remote Actor handoff owner fence changed during completion");
                                }
                            }
                            if (commit_failure) {
                                node->actor_transfer_coordinator.fail_commit (transfer_id, true);
                            }
                            if (commit_failure) {
                                settle_turn (std::move (*commit_failure));
                                return;
                            }
                            if (!replay) {
                                node->actor_transfer_coordinator.fail_commit (transfer_id, true);
                                settle_turn (remote_actor_commit_turn_state_t::outcome_t{
                                  false, framework_error_kind_t::invalid_operation,
                                  "remote Actor commit fence changed before replay publication"});
                                return;
                            }
                            commit_state->mark_committed ();
                            auto replay_owner = std::make_shared<spot_node_runtime_t> (node);
                            auto replay_task = replay_owner->replay_actor_handoff_batch (
                              committed, std::move (*replay), services, transfer_id, true, {});
                            detail::observe_task_completion (
                              replay_task,
                              [settle_turn, replay_owner] (const result_t<void> &result) mutable {
                                  settle_turn (remote_actor_commit_turn_state_t::outcome_t{
                                    static_cast<bool> (result),
                                    result ? framework_error_kind_t::internal_failure
                                           : result.error_kind (),
                                    result.error () ? result.error ()->what () : std::string{}});
                              });
                        });
                  };

                  if (!target_joined || !submit_source_leave) {
                      continue_after_source_leave (result_t<void>::success ());
                      return;
                  }
                  try {
                      auto submitted_leave =
                        std::make_shared<task_t<void>> (submit_source_leave ());
                      detail::observe_task_terminal (
                        *submitted_leave,
                        [submitted_leave,
                         continue_after_source_leave = std::move (continue_after_source_leave)] (
                          const result_t<void> &submitted) mutable {
                            // OnLeave is a one-way notification. Its transport
                            // terminal is ordered before Accepted but cannot
                            // change the already-committed target outcome.
                            continue_after_source_leave (submitted);
                        });
                  }
                  catch (const framework_exception_t &error) {
                      continue_after_source_leave (detail::result_access_t::failure<void> (error));
                  }
                  catch (const std::exception &error) {
                      continue_after_source_leave (result_t<void>::failure (
                        framework_error_kind_t::internal_failure, error.what ()));
                  }
                  catch (...) {
                      continue_after_source_leave (
                        result_t<void>::failure (framework_error_kind_t::internal_failure,
                                                 "source Actor leave submission failed"));
                  }
              }
              catch (const framework_exception_t &error) {
                  fail_precommit (error.kind (), error.what ());
              }
              catch (const std::exception &error) {
                  fail_precommit (framework_error_kind_t::internal_failure, error.what ());
              }
              catch (...) {
                  fail_precommit (framework_error_kind_t::internal_failure,
                                  "remote Actor handoff owner failed during activation");
              }
          },
          [commit_state] { commit_state->request_stop (); },
          runtime::serial_work_options_t{runtime::serial_work_lane_t::application,
                                         runtime::serial_execution_queue_t::fixed_work_byte_cost});
        if (!submitted) {
            commit_state->settle (remote_actor_commit_turn_state_t::outcome_t{
              false, submitted.error_kind (),
              submitted.error () != nullptr
                ? submitted.error ()->what ()
                : "remote Actor handoff replay owner could not be reserved"});
            return;
        }
        track_submission (submitted.value ().queue, submitted.value ().id);
    };

    if (actor_gateway_owner) {
        const auto updated = actor_gateway_owner->update_actor_ref (committed);
        if (!updated) {
            commit_state->settle (remote_actor_commit_turn_state_t::outcome_t{
              false, updated.error_kind (),
              updated.error () != nullptr ? updated.error ()->what ()
                                          : "target actor gateway ref update failed"});
            return;
        }
    }
    if (joined_callback) {
        target_state->run_serial_task_async (
          "spot-actor-transfer-joined",
          [target_state, actor_instance, joined_callback = std::move (joined_callback)] () mutable {
              return joined_callback (target_state->spot_instance.get (), actor_instance.get ());
          },
          std::move (submit_actor_turn), track_submission,
          [submission_state] {
              std::lock_guard lock (submission_state->mutex);
              submission_state->lifecycle_started = true;
              if (submission_state->cancellation_requested)
                  submission_state->lifecycle_was_active = true;
          },
          [submission_state] (bool started) {
              std::lock_guard lock (submission_state->mutex);
              if (submission_state->phase == submission_state_t::phase_t::lifecycle) {
                  submission_state->lifecycle_was_active = started;
              }
          });
    } else {
        submit_actor_turn (result_t<void>::success ());
    }
}

result_t<actor_join_reply_t> spot_node_runtime_t::finalize_remote_actor_to_spot (
  std::string transfer_id,
  const actor_ref_t &actor_ref,
  spot_id_t target_spot_id,
  service_provider_t &services,
  actor_gateway_runtime_t *actor_gateway,
  std::optional<std::chrono::steady_clock::time_point> deadline)
{
    detail::task_completion_source_t<actor_join_reply_t> completion;
    auto result = completion.task ();
    finalize_remote_actor_to_spot_async (
      std::move (transfer_id), actor_ref, std::move (target_spot_id), services, actor_gateway,
      deadline, [completion] (result_t<actor_join_reply_t> value) mutable {
          completion.complete (std::move (value));
      });
    return result.result ();
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
        return result_t<actor_join_reply_t>::failure (
          framework_error_kind_t::rejected, "spot node is draining and rejects new actor joins");
    }
    enum class entry_selection_t
    {
        selected,
        node_mismatch,
        not_registered,
        not_created
    };
    struct entry_selection_result_t
    {
        entry_selection_t selection = entry_selection_t::not_created;
        std::optional<spot_id_t> entry_id;
    };
    const auto entry = _state->lane.run ([&] {
        entry_selection_result_t result;
        if (spot_node_rid.empty ()
            || spot_node_rid.value () != detail::effective_spot_node_rid (_state->snapshot)) {
            result.selection = entry_selection_t::node_mismatch;
            return result;
        }
        if (!_state->snapshot.entry_spot_name) {
            result.selection = entry_selection_t::not_registered;
            return result;
        }
        const auto entry_id = _state->spot_ids_by_name.find (*_state->snapshot.entry_spot_name);
        if (entry_id == _state->spot_ids_by_name.end ()) {
            result.selection = entry_selection_t::not_created;
            return result;
        }
        result.selection = entry_selection_t::selected;
        result.entry_id = entry_id->second;
        return result;
    }).get ();
    if (entry.selection == entry_selection_t::node_mismatch) {
        return result_t<actor_join_reply_t>::failure (framework_error_kind_t::not_found,
                                                      "spot node rid does not match this node");
    }
    if (entry.selection == entry_selection_t::not_registered) {
        return result_t<actor_join_reply_t>::failure (framework_error_kind_t::not_found,
                                                      "entry spot is not registered");
    }
    if (!entry.entry_id) {
        return result_t<actor_join_reply_t>::failure (framework_error_kind_t::not_found,
                                                      "entry spot is not created");
    }
    return join_actor_to_spot_erased (actor_ref, *entry.entry_id, request, actor_snapshot,
                                      std::move (actor_context));
}

void spot_node_runtime_t::on_destroy_actor (
  std::function<result_t<void> (const actor_ref_t &)> destroy_actor)
{
    _state->lane.run ([&] {
        _state->destroy_actor_registry = std::move (destroy_actor);
    }).get ();
}

void spot_node_runtime_t::on_actor_ref_updated (
  std::function<result_t<void> (const actor_ref_t &)> update_actor)
{
    _state->lane.run ([&] {
        _state->update_actor_registry_ref = std::move (update_actor);
    }).get ();
}

void spot_node_runtime_t::on_actor_entry_spot_join (
  std::function<result_t<actor_join_reply_t> (const actor_ref_t &,
                                              node_rid_t,
                                              const zlink::message_t &,
                                              const std::optional<zlink::message_t> &)> join)
{
    _state->lane.run ([&] {
        _state->actor_entry_spot_join = std::move (join);
    }).get ();
}

void spot_node_runtime_t::on_actor_packet_relay (
  std::function<
    task_t<std::optional<zlink::message_t>> (const actor_ref_t &,
                                             actor_context_t,
                                             stream_message_kind_t,
                                             std::string_view,
                                             const zlink::message_t &,
                                             service_provider_t &,
                                             serializer_registry_t &,
                                             spot_inbound_message_t,
                                             const runtime::protocol::actor_route_fence_t *)> relay)
{
    _state->lane.run ([&] {
        _state->actor_packet_relay = std::move (relay);
    }).get ();
}

void spot_node_runtime_t::on_actor_message_follow (
  std::function<
    task_t<std::optional<zlink::message_t>> (const actor_ref_t &,
                                             const runtime::messaging::envelope_header_t &,
                                             const zlink::message_t &,
                                             std::chrono::milliseconds,
                                             const zlink::routing_id_t &,
                                             const runtime::protocol::actor_route_fence_t &,
                                             std::uint8_t,
                                             const runtime::protocol::wire_operation_id_t &,
                                             std::uint64_t)> relay)
{
    _state->lane.run ([&] {
        _state->actor_message_follow_relay = std::move (relay);
    }).get ();
}

void spot_node_runtime_t::on_actor_handoff_terminal (
  std::function<task_t<bool> (const zlink::routing_id_t &,
                              const zlink::routing_id_t &,
                              const runtime::protocol::wire_operation_id_t &,
                              std::uint64_t,
                              const runtime::protocol::actor_route_fence_t &,
                              const result_t<zlink::message_t> &)> sender)
{
    _state->lane.run ([&] {
        _state->actor_handoff_terminal_sender = std::move (sender);
    }).get ();
}

void spot_node_runtime_t::on_actor_leave_notification (
  std::function<task_t<zlink::submit_result_t> (const zlink::routing_id_t &,
                                                std::vector<zlink::message_t>)> sender)
{
    _state->lane.run ([&] {
        _state->actor_leave_notification_sender = std::move (sender);
    }).get ();
}

void spot_node_runtime_t::invalidate_message_follow_route (
  const runtime::protocol::message_follow_notice_t &notice)
{
    const auto *source = std::get_if<runtime::protocol::spot_route_fence_t> (&notice.source);
    if (!source)
        return;
    auto *resolver =
      _state->lane.run ([&] { return _state->spot_location_resolver; }).get ();
    if (!resolver)
        return;
    runtime::spot_address_t expected;
    expected.spot_id = source->spot_id;
    expected.node_rid = zlink::routing_id_t::from (source->target_node_routing_id);
    expected.node_generation = source->target_node_generation;
    expected.object_generation = source->object_generation;
    expected.authority_owner_generation = source->authority_owner_generation;
    expected.owner.lease_generation = static_cast<std::int64_t> (source->owner_lease_generation);
    (void) resolver->invalidate_spot_address_if_matches (source->spot_id, expected);
}

task_t<std::optional<zlink::message_t>>
spot_node_runtime_t::relay_actor_packet (const actor_ref_t &actor_ref,
                                         actor_context_t actor_context,
                                         std::string_view packet_name,
                                         const zlink::message_t &message,
                                         service_provider_t &services,
                                         serializer_registry_t &serializers,
                                         spot_inbound_message_t metadata)
{
    co_return co_await relay_actor_packet (actor_ref, std::move (actor_context),
                                           stream_message_kind_t::request, packet_name, message,
                                           services, serializers, std::move (metadata));
}

task_t<std::optional<zlink::message_t>> spot_node_runtime_t::relay_actor_packet (
  const actor_ref_t &actor_ref,
  actor_context_t actor_context,
  stream_message_kind_t message_kind,
  std::string_view packet_name,
  const zlink::message_t &message,
  service_provider_t &services,
  serializer_registry_t &serializers,
  spot_inbound_message_t metadata,
  const runtime::protocol::actor_route_fence_t *admitted_message_follow_target,
  std::function<void ()> before_application_handler,
  std::function<void ()> after_application_admission,
  zlink::routing_id_t inbound_source_node_rid,
  runtime::protocol::wire_operation_id_t inbound_operation,
  std::uint64_t inbound_reply_route_id,
  std::optional<std::string> inbound_deadline,
  std::function<void ()> transfer_owner_reservation,
  std::size_t transferred_owner_byte_cost)
{
    // This member coroutine can be entered through a short-lived runtime
    // wrapper. Keep the node state in the frame for the terminal path below:
    // a handler may suspend for a relocating Join, after which completion
    // updates the request deduplication table.
    const auto state_owner = _state;
    if (::zlink::framework::detail::actor_ref_access_t::empty (actor_ref)) {
        co_return result_t<std::optional<zlink::message_t>>::failure (
          framework_error_kind_t::not_found, "actor ref is empty");
    }

    const auto key = actor_key (actor_ref);
    const auto dispatch_kind = message_kind == stream_message_kind_t::send
                                 ? dispatch_message_kind_t::actor_send
                                 : dispatch_message_kind_t::actor_request;
    report_actor_dispatch_stage_trace (
      _state, message_flow_outcome_t::received, dispatch_kind, packet_name, {},
      actor_ref.actor_id ().value (), "relay_actor_packet.enter",
      admitted_message_follow_target == nullptr ? "follow_fence=none" : "follow_fence=present");
    if (_state->lane.run ([&] { return _state->retiring_actor_keys.contains (key); }).get ()) {
        co_return result_t<std::optional<zlink::message_t>>::failure (
          framework_error_kind_t::not_found, "actor destruction is pending");
    }
    // A cold hit carries no follow fence (e.g. a client probing the actor's
    // former owner directly, unaware it already relocated). Relocation
    // preserves ObjectGeneration, so a retained Message Follow route for this
    // key at the actor's believed generation is this node's own source fence
    // for that route; borrow it so the fence-validated relay path below
    // (which normally only runs for an already-fenced hop) also covers the
    // very first hop instead of falling through to a local spot lookup that
    // can never resolve (the route points at the remote target's spot).
    runtime::protocol::actor_route_fence_t synthesized_follow_fence;
    if (admitted_message_follow_target == nullptr) {
        if (const auto found =
              _state->actor_transfer_coordinator.message_follow_source_for_generation (
                key, actor_ref.object_generation ())) {
            synthesized_follow_fence = *found;
            admitted_message_follow_target = &synthesized_follow_fence;
        }
    }
    if (admitted_message_follow_target != nullptr) {
        const auto current_fence = _state->lane.run ([&] {
            const auto current = _state->actor_authority_fences.find (key);
            return current == _state->actor_authority_fences.end ()
                     ? std::optional<runtime::protocol::actor_route_fence_t>{}
                     : std::make_optional (current->second);
        }).get ();
        // The exact adoption fence gates only when this node retains state
        // that could be confused with an older incarnation: a recorded
        // adoption fence or a retained Message Follow source route. A
        // fenced packet the mesh route admission already accepted for an
        // actor without either dispatches to the current local authority.
        bool targets_current_authority =
          current_fence ? *current_fence == *admitted_message_follow_target
                        : !_state->actor_transfer_coordinator.has_message_follow_route (key);
        if (!targets_current_authority && current_fence) {
            //  Spec 21 §6.3: a followed message is deliverable when the
            //  new owner's AuthorityOwnerGeneration is greater than the
            //  edge value the previous owner retained at move
            //  completion — forwarding can traverse several moves, so
            //  the final owner's fence is ordered against, not equal
            //  to, the followed edge's fence. The object incarnation
            //  and the fenced target node must still match exactly.
            const auto &m = *admitted_message_follow_target;
            targets_current_authority =
              current_fence->actor_id == m.actor_id
              && current_fence->object_generation == m.object_generation
              && current_fence->target_node_routing_id == m.target_node_routing_id
              && current_fence->authority_owner_generation > m.authority_owner_generation;
        }
        if (!targets_current_authority
            && !matches_actor_message_follow_source (actor_ref, *admitted_message_follow_target)) {
            report_actor_dispatch_stage_trace (
              _state, message_flow_outcome_t::received, dispatch_kind, packet_name, {},
              actor_ref.actor_id ().value (), "relay_actor_packet.follow_admission",
              "accepted=false");
            {
                //  Spec 26 Detailed-scope diagnostics: retain both fence
                //  tuples in the Detailed-only stage/result extension without
                //  overloading any standard address attribute.
                detail::message_flow_tracer_t (_state->dispatch)
                  .trace (message_flow_outcome_t::dropped, [&, current_fence] {
                      const auto describe = [] (
                                              const runtime::protocol::actor_route_fence_t &fence) {
                          return fence.actor_id + "/og=" + std::to_string (fence.object_generation)
                                 + "/ng=" + std::to_string (fence.target_node_generation)
                                 + "/ag=" + std::to_string (fence.authority_owner_generation)
                                 + "/lg=" + std::to_string (fence.owner_lease_generation);
                      };
                      auto event = message_flow_event_t{
                        message_flow_outcome_t::dropped, dispatch_error_surface_t::spot_actor,
                        dispatch_message_kind_t::actor_send, std::string ("follow_fence")};
                      event.detail_stage = "admission";
                      event.detail_result = !current_fence ? std::string ("local=none")
                                                           : "local=" + describe (*current_fence);
                      event.detail_result = *event.detail_result + " message="
                                            + describe (*admitted_message_follow_target);
                      event.actor_id = std::string (actor_ref.actor_id ().value ());
                      event.reason = message_flow_reason_t::stale_target;
                      return event;
                  });
            }
            co_return result_t<std::optional<zlink::message_t>>::failure (
              framework_error_kind_t::unavailable,
              "Actor Message Follow target fence is not admitted");
        }
        report_actor_dispatch_stage_trace (_state, message_flow_outcome_t::received, dispatch_kind,
                                           packet_name, {}, actor_ref.actor_id ().value (),
                                           "relay_actor_packet.follow_admission", "accepted=true");
    } else {
        report_actor_dispatch_stage_trace (_state, message_flow_outcome_t::received, dispatch_kind,
                                           packet_name, {}, actor_ref.actor_id ().value (),
                                           "relay_actor_packet.follow_admission", "required=false");
    }
    const auto handoff = metadata.values.find ("__zlink.actorHandoffBacklog");
    const auto handoff_transfer_id = metadata.values.find ("__zlink.actorTransferId");
    if (handoff != metadata.values.end () && handoff->second == "true"
        && handoff_transfer_id != metadata.values.end () && !handoff_transfer_id->second.empty ()) {
        if (actor_transfer_marker_enabled ()) {
            emit_actor_transfer_marker ("backlog_enqueued", actor_ref, handoff_transfer_id->second,
                                        actor_spot (actor_ref));
        }
    }
    // A node can become the current owner again while an older source route
    // for the same Actor incarnation is still retained. Resolve that exact
    // authority before choosing the handoff backlog: packets admitted through
    // a committed source Message Follow edge must relay immediately. Keeping
    // those post-commit arrivals in the source backlog lets a continuous
    // producer prevent finish_move_replay() from ever observing it empty.
    bool targets_current_authority = admitted_message_follow_target == nullptr;
    if (admitted_message_follow_target != nullptr) {
        const auto current_fence = _state->lane.run ([&] {
            const auto current = _state->actor_authority_fences.find (key);
            return current == _state->actor_authority_fences.end ()
                     ? std::optional<runtime::protocol::actor_route_fence_t>{}
                     : std::make_optional (current->second);
        }).get ();
        targets_current_authority =
          current_fence ? *current_fence == *admitted_message_follow_target
                        : !_state->actor_transfer_coordinator.has_message_follow_route (key);
        if (!targets_current_authority && current_fence) {
            const auto &m = *admitted_message_follow_target;
            targets_current_authority =
              current_fence->actor_id == m.actor_id
              && current_fence->object_generation == m.object_generation
              && current_fence->target_node_routing_id == m.target_node_routing_id
              && current_fence->authority_owner_generation > m.authority_owner_generation;
        }
    }
    const bool targets_committed_source =
      admitted_message_follow_target != nullptr
      && matches_actor_message_follow_source (actor_ref, *admitted_message_follow_target);
    {
        // In-flight handoff (§10.2-1): actor packets that arrive while the actor
        // is moving are preserved in arrival order and travel to the target with
        // the commit. Sends co_return
        // the empty success shape so preservation is indistinguishable from
        // immediate dispatch. A preserved request keeps its source reply token;
        // the target sends one internal terminal envelope after replay.
        const bool is_request = message_kind == stream_message_kind_t::request;
        const auto append_result =
          !targets_current_authority && targets_committed_source
            ? detail::handoff_append_result_t::not_moving
            : _state->actor_transfer_coordinator.try_append_backlog (
                key, detail::handoff_packet_t{std::string (packet_name), message.to_bytes (),
                                              metadata.content_type, metadata.values, is_request});
        const auto append_result_name = [&] {
            switch (append_result) {
                case detail::handoff_append_result_t::not_moving:
                    return "not_moving";
                case detail::handoff_append_result_t::appended:
                    return "appended";
                case detail::handoff_append_result_t::duplicate_request:
                    return "duplicate_request";
            }
            return "unknown";
        }();
        report_actor_dispatch_stage_trace_lazy (
          _state,
          append_result == detail::handoff_append_result_t::appended
            ? message_flow_outcome_t::admitted
            : message_flow_outcome_t::received,
          dispatch_kind, packet_name, {}, actor_ref.actor_id ().value (),
          "relay_actor_packet.try_append_backlog",
          [&] { return "result=" + std::string (append_result_name); });
        if (append_result == detail::handoff_append_result_t::appended) {
            if (actor_transfer_marker_enabled ()) {
                emit_actor_transfer_marker (
                  "handoff_backlog", actor_ref,
                  _state->actor_transfer_coordinator.transfer_id (key).value_or (key));
            }
            if (is_request) {
                const auto request_id = metadata.values.find ("__zlink.actorRequestId");
                if (request_id != metadata.values.end ()) {
                    report_actor_handoff_request_trace (
                      _state, "handoff_request_frame", actor_ref, request_id->second,
                      _state->actor_transfer_coordinator.transfer_id (key).value_or (key));
                }
            }
            if (!is_request) {
                if (after_application_admission)
                    after_application_admission ();
                co_return result_t<std::optional<zlink::message_t>>::success (zlink::message_t{});
            }
            co_return result_t<std::optional<zlink::message_t>>::success (std::nullopt);
        }
        if (append_result != detail::handoff_append_result_t::not_moving) {
            co_return detail::result_access_t::failure<std::optional<zlink::message_t>> (
              detail::make_origin_exception (framework_error_kind_t::unavailable,
                                             detail::failure_origin_t::actor_transfer_in_progress,
                                             "actor transfer is in progress"));
        }

        // Only the exact committed target fence may bypass an older Message
        // Follow route; matching the node or ObjectGeneration alone is
        // insufficient (Spec 21 §6.3).
        if (!targets_current_authority && !targets_committed_source) {
            co_return result_t<std::optional<zlink::message_t>>::failure (
              framework_error_kind_t::unavailable,
              "Actor Message Follow target fence is no longer current");
        }
        decltype (_state->actor_message_follow_relay) actor_message_follow_relay;
        if (!targets_current_authority && targets_committed_source) {
            actor_message_follow_relay = _state->lane.run ([&] {
                return _state->actor_message_follow_relay;
            }).get ();
        }
        if (!targets_current_authority && targets_committed_source
            && actor_message_follow_relay) {
            std::uint8_t incoming_hop_count = 0;
            if (const auto hop = metadata.values.find ("__zlink.messageFollowHopCount");
                hop != metadata.values.end ()) {
                const auto parsed = std::stoul (hop->second);
                if (parsed > runtime::protocol::messageFollowHopCount) {
                    co_return result_t<std::optional<zlink::message_t>>::failure (
                      framework_error_kind_t::protocol_error,
                      "Actor Message Follow hop count is invalid");
                }
                incoming_hop_count = static_cast<std::uint8_t> (parsed);
            }
            runtime::messaging::envelope_header_t header;
            header.kind = is_request ? runtime::messaging::message_kind_t::request
                                     : runtime::messaging::message_kind_t::command;
            header.channel_name = "actor";
            header.message_name = std::string (packet_name);
            header.content_type = metadata.content_type;
            header.metadata = metadata.values;
            // Spec 28 §10 keeps the client-managed absolute deadline on the
            // forwarded envelope while this hop uses its own local relay
            // window below.  In particular, a cold probe has no incoming
            // follow fence, so preserve the deadline supplied by the route
            // dispatcher instead of manufacturing a new client deadline.
            header.deadline = std::move (inbound_deadline);
            co_return co_await actor_message_follow_relay (
              actor_ref, header, message, std::chrono::seconds (30), inbound_source_node_rid,
              *admitted_message_follow_target, incoming_hop_count, inbound_operation,
              inbound_reply_route_id);
        }
    }

    using actor_factory_registration_t =
      detail::spot_node_builder_state_t::actor_factory_registration_t;
    struct actor_dispatch_plan_t
    {
        spot_id_t current_spot_id;
        std::uint64_t current_generation = 0;
        bool stale_generation = false;
        std::optional<actor_ref_t> current_actor_ref;
        std::shared_ptr<detail::spot_context_state_t> context_state;
        std::shared_ptr<void> spot_instance;
        std::string mesh_name;
        bool dispatch_on_spot_serial = true;
    };
    auto project_actor_dispatch_state = [&] (
                                          const std::optional<spot_id_t> &found_location)
      -> result_t<actor_dispatch_plan_t> {
        if (!found_location) {
            return result_t<actor_dispatch_plan_t>::failure (
              framework_error_kind_t::not_found, "actor spot route disappeared before dispatch");
        }

        actor_dispatch_plan_t plan;
        // Actor movement can replace or erase the route while a user callback
        // is suspended. Keep the complete derived dispatch projection in this turn.
        plan.current_spot_id = *found_location;
        const auto found_generation = _state->actor_generations.find (key);
        plan.current_generation = found_generation != _state->actor_generations.end ()
                                    ? found_generation->second
                                    : actor_ref.object_generation ();
        if (plan.current_generation != actor_ref.object_generation ()) {
            plan.stale_generation = true;
            return result_t<actor_dispatch_plan_t>::success (std::move (plan));
        }

        const auto current_actor_node_rid = actor_ref.node_rid ().empty ()
                                              ? detail::effective_spot_node_rid (_state->snapshot)
                                              : std::string (actor_ref.node_rid ().value ());
        plan.current_actor_ref = ::zlink::framework::detail::actor_ref_access_t::make (
          node_rid_t::from_string (current_actor_node_rid),
          std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor_ref)),
          std::string (actor_ref.actor_id ().value ()), plan.current_generation);
        const auto context = _state->spot_contexts_by_id.find (std::string (plan.current_spot_id));
        if (context == _state->spot_contexts_by_id.end () || !context->second._state
            || !context->second._state->spot_instance) {
            return result_t<actor_dispatch_plan_t>::failure (
              framework_error_kind_t::not_found,
              "actor spot context is not registered. node="
                + detail::effective_spot_node_rid (_state->snapshot) + ", actor="
                + std::string (actor_ref.actor_id ().value ()) + ", spot="
                + plan.current_spot_id);
        }
        plan.context_state = context->second._state;
        plan.spot_instance = plan.context_state->spot_instance;
        plan.mesh_name = plan.context_state->mesh_name;
        if (_state->snapshot.entry_spot_name) {
            const auto entry_id =
              _state->spot_ids_by_name.find (*_state->snapshot.entry_spot_name);
            plan.dispatch_on_spot_serial =
              entry_id == _state->spot_ids_by_name.end () || entry_id->second != plan.current_spot_id;
        }
        if (plan.context_state->execution_mode == user_spot_execution_mode_t::per_actor) {
            plan.dispatch_on_spot_serial = false;
        }
        return result_t<actor_dispatch_plan_t>::success (std::move (plan));
    };
    struct actor_state_snapshot_t
    {
        actor_factory_registration_t factory;
        std::optional<spot_id_t> found_location;
        std::shared_ptr<void> actor_instance;
        std::string actor_mesh_name;
        std::optional<result_t<actor_dispatch_plan_t>> dispatch;
    };
    auto materialized = _state->lane.run ([&] () -> result_t<actor_state_snapshot_t> {
        const auto actor_type_key =
          std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor_ref));
        const auto found_factory = _state->actor_factories.find (actor_type_key);
        if (found_factory == _state->actor_factories.end ()) {
            return result_t<actor_state_snapshot_t>::failure (
              framework_error_kind_t::not_found, "actor factory is not registered");
        }

        actor_state_snapshot_t plan;
        plan.factory = found_factory->second;
        const auto found_location = _state->actor_spot_ids.find (key);
        if (found_location != _state->actor_spot_ids.end ()) {
            plan.found_location = found_location->second;
        } else if (_state->destroyed_actor_keys.contains (key)) {
            return result_t<actor_state_snapshot_t>::failure (
              framework_error_kind_t::not_found, "actor has been destroyed");
        }

        const auto found_instance = _state->actor_instances.find (key);
        if (found_instance != _state->actor_instances.end () && found_instance->second) {
            plan.actor_instance = found_instance->second;
            detail::record_actor_instance_index_unlocked (*_state, actor_ref,
                                                          plan.actor_instance.get ());
            if (plan.found_location) {
                plan.dispatch = project_actor_dispatch_state (plan.found_location);
            }
        } else if (plan.factory.create_context_instance) {
            plan.actor_mesh_name = _state->snapshot.name;
            if (plan.found_location) {
                const auto actor_spot_context =
                  _state->spot_contexts_by_id.find (std::string (*plan.found_location));
                if (actor_spot_context != _state->spot_contexts_by_id.end ()
                    && actor_spot_context->second._state) {
                    plan.actor_mesh_name = actor_spot_context->second._state->mesh_name;
                }
            }
        }
        return result_t<actor_state_snapshot_t>::success (std::move (plan));
    }).get ();
    if (!materialized) {
        co_return detail::propagate_failure<std::optional<zlink::message_t>> (
          materialized, "actor materialization failed");
    }
    auto materialization = std::move (materialized.value ());
    auto factory = std::move (materialization.factory);
    const auto initial_location = materialization.found_location;

    /* Same double-checked registration as the join paths. */
    std::shared_ptr<void> actor_instance = std::move (materialization.actor_instance);
    if (!actor_instance) {
        std::shared_ptr<void> created_instance;
        if (factory.create_context_instance) {
            auto materialization_context = actor_context_t (
              actor_context._state, actor_ref, actor_context._source_binding_generation,
              std::move (materialization.actor_mesh_name));
            created_instance =
              factory.create_context_instance (std::move (materialization_context));
        } else {
            created_instance =
              factory.create_instance (std::string (actor_ref.actor_id ().value ()));
        }
        if (!created_instance) {
            co_return result_t<std::optional<zlink::message_t>>::failure (
              framework_error_kind_t::not_found, "actor factory returned null");
        }
        actor_instance = _state->lane.run ([&] {
            auto &slot = _state->actor_instances[key];
            if (!slot) {
                if (_state->destroyed_actor_keys.contains (key)) {
                    _state->actor_instances.erase (key);
                    return std::shared_ptr<void>{};
                }
                slot = std::move (created_instance);
            }
            detail::record_actor_instance_index_unlocked (*_state, actor_ref, slot.get ());
            return slot;
        }).get ();
        if (!actor_instance) {
            co_return result_t<std::optional<zlink::message_t>>::failure (
              framework_error_kind_t::not_found, "actor has been destroyed");
        }
    }

    if (!initial_location) {
        struct entry_admission_plan_t
        {
            std::shared_ptr<detail::spot_context_state_t> context_state;
            std::shared_ptr<void> spot_instance;
            std::optional<spot_actor_admission_callbacks_t> admission;
            bool creation_reserved = false;
        };
        auto entry_planned = _state->lane.run ([&] () -> result_t<entry_admission_plan_t> {
            if (!_state->snapshot.entry_spot_name) {
                return result_t<entry_admission_plan_t>::failure (
                  framework_error_kind_t::not_found, "entry spot is not registered");
            }
            const auto entry_id =
              _state->spot_ids_by_name.find (*_state->snapshot.entry_spot_name);
            if (entry_id == _state->spot_ids_by_name.end ()) {
                return result_t<entry_admission_plan_t>::failure (
                  framework_error_kind_t::not_found, "entry spot is not created");
            }
            const auto entry_context =
              _state->spot_contexts_by_id.find (std::string (entry_id->second));
            if (entry_context == _state->spot_contexts_by_id.end ()
                || !entry_context->second._state
                || !entry_context->second._state->spot_instance) {
                return result_t<entry_admission_plan_t>::failure (
                  framework_error_kind_t::not_found, "entry spot context is not registered");
            }

            entry_admission_plan_t plan;
            plan.context_state = entry_context->second._state;
            plan.spot_instance = plan.context_state->spot_instance;
            detail::record_actor_context_route_unlocked (
              *_state, key, detail::effective_spot_node_rid (_state->snapshot),
              *plan.context_state, actor_ref.object_generation ());
            const auto admission = plan.context_state->actor_admissions.find (factory.actor_type);
            if (admission != plan.context_state->actor_admissions.end ()) {
                plan.admission = admission->second;
                plan.creation_reserved = plan.admission->on_create_actor
                                         && _state->actor_created_keys.insert (key).second;
            }
            return result_t<entry_admission_plan_t>::success (std::move (plan));
        }).get ();
        if (!entry_planned) {
            co_return detail::propagate_failure<std::optional<zlink::message_t>> (
              entry_planned, "entry spot admission failed");
        }
        auto entry_plan = std::move (entry_planned.value ());
        bool created_entry_actor = false;
        if (entry_plan.admission) {
            if (entry_plan.creation_reserved) {
                const auto create_request =
                  actor_context.create_payload ().value_or (zlink::message_t{});
                const auto created =
                  entry_plan.admission
                    ->on_create_actor (entry_plan.spot_instance.get (), actor_instance.get (),
                                       create_request, serializers)
                    .result ();
                if (!created || !created.value ().accepted) {
                    _state->lane.run ([&] {
                        _state->actor_created_keys.erase (key);
                        erase_actor_route_unlocked (*_state, key);
                    }).get ();
                    co_return result_t<std::optional<zlink::message_t>>::failure (
                      created ? framework_error_kind_t::rejected : created.error_kind (),
                      created && !created.value ().accepted
                        ? "Entry Spot rejected Actor creation"
                        : (created.error () ? created.error ()->what ()
                                            : "Entry Spot Actor creation failed"));
                }
                created_entry_actor = true;
            }
            if (!created_entry_actor && entry_plan.admission->on_actor_joined) {
                const auto completed =
                  entry_plan.context_state->run_serial_task ("spot-lifecycle-join", [&] {
                    return entry_plan.admission->on_actor_joined (
                      entry_plan.spot_instance.get (), actor_instance.get ());
                });
                if (!completed) {
                    co_return result_t<std::optional<zlink::message_t>>::failure (
                      completed.error_kind (), completed.error () != nullptr
                                                 ? completed.error ()->what ()
                                                 : "spot actor joined callback failed");
                }
            }
        }
    }

    auto dispatch_planned = std::move (materialization.dispatch);
    if (!dispatch_planned) {
        dispatch_planned = _state->lane.run ([&] {
            const auto found_location = _state->actor_spot_ids.find (key);
            return project_actor_dispatch_state (
              found_location == _state->actor_spot_ids.end ()
                ? std::optional<spot_id_t>{}
                : std::make_optional (found_location->second));
        }).get ();
    }
    if (!*dispatch_planned) {
        co_return detail::propagate_failure<std::optional<zlink::message_t>> (
          *dispatch_planned, "actor dispatch state projection failed");
    }
    auto dispatch_plan = std::move (dispatch_planned->value ());
    const auto current_spot_id = dispatch_plan.current_spot_id;
    if (dispatch_plan.stale_generation) {
        // The dispatched ref's generation does not match the actor's current
        // incarnation (§10.4-3). Retriable: for a still-committing local move the
        // published record lags and re-resolving lands the committed generation
        // (ST-A3); for a genuinely stale record the client re-resolves the same
        // answer and eventually surfaces this stale on its own budget timeout.
        if (actor_transfer_marker_enabled ()) {
            emit_actor_transfer_marker (
              "message_follow_expired", actor_ref,
              std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor_ref))
                + ":" + std::string (actor_ref.actor_id ().value ()),
              current_spot_id);
        }
        co_return result_t<std::optional<zlink::message_t>>::failure (
          framework_error_kind_t::unavailable,
          "actor generation is stale. actor=" + std::string (actor_ref.actor_id ().value ())
            + ", current=" + std::to_string (dispatch_plan.current_generation)
            + ", received=" + std::to_string (actor_ref.object_generation ()));
    }
    const auto &current_actor_ref = *dispatch_plan.current_actor_ref;
    if (!factory.create_context_instance) {
        auto current_actor_context =
          actor_context_t (actor_context._state, current_actor_ref,
                           actor_context._source_binding_generation, dispatch_plan.mesh_name);
        factory.configure_instance (actor_instance.get (), current_actor_ref,
                                    &current_actor_context);
    }
    const bool dispatch_on_spot_serial = dispatch_plan.dispatch_on_spot_serial;

    const auto handler_kind = message_kind == stream_message_kind_t::send
                                ? spot_handler_kind_t::actor_send
                                : spot_handler_kind_t::actor_request;
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
                    co_return result_t<std::optional<zlink::message_t>>::success (*claim.value);
                }
            }
            if (claim.state == runtime::exactly_once_claim_state::pending) {
                co_return result_t<std::optional<zlink::message_t>>::failure (
                  framework_error_kind_t::unavailable, "actor request dispatch is in flight");
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
            try {
                state->actor_pending_requests_lane
                  .run ([this] {
                      const auto found = state->actor_pending_requests.find (key);
                      if (found != state->actor_pending_requests.end () && --found->second == 0) {
                          state->actor_pending_requests.erase (found);
                      }
                  })
                  .get ();
            }
            catch (...) {
                // Scope destruction must not turn a handler terminal into a process abort
                // while the node's state lane is closing.
            }
        }
    };
    std::optional<pending_request_scope_t> pending_request_scope;
    if (message_kind == stream_message_kind_t::request) {
        _state->actor_pending_requests_lane.run ([this, &key] {
            _state->actor_pending_requests[key]++;
        }).get ();
        pending_request_scope.emplace (_state, key);
    }
    report_spot_dispatch_trace (_state, message_flow_outcome_t::received,
                                dispatch_error_surface_t::spot_actor, dispatch_kind, packet_name,
                                {}, current_spot_id, actor_ref.actor_id ().value ());
    const bool request_delivery = message_kind == stream_message_kind_t::request;
    const auto admitted_source_fence = admitted_message_follow_target == nullptr
                                         ? std::optional<runtime::protocol::actor_route_fence_t>{}
                                         : std::make_optional (*admitted_message_follow_target);
    zlink::message_t reply;
    try {
        reply = co_await spot_handler_registry_t (dispatch_plan.context_state)
                  .invoke_erased (handler_kind, packet_name, {}, factory.actor_type,
                                  dispatch_plan.spot_instance.get (), actor_instance.get (),
                                  services, serializers, message, std::move (metadata),
                                  dispatch_on_spot_serial, key, current_spot_id, {},
                                  spot_handler_registry_t::actor_queue_dispatch_t::acquire,
                                  std::move (before_application_handler),
                                  std::move (after_application_admission),
                                  std::move (transfer_owner_reservation),
                                  transferred_owner_byte_cost);
    }
    catch (const framework_exception_t &error) {
        if (!dedup_request_id.empty ()) {
            (void) state_owner->dispatched_request_replies.erase (
              actor_request_dedup_key (key, dedup_request_id));
        }
        co_return detail::result_access_t::failure<std::optional<zlink::message_t>> (error);
    }
    catch (const std::exception &error) {
        if (!dedup_request_id.empty ()) {
            (void) state_owner->dispatched_request_replies.erase (
              actor_request_dedup_key (key, dedup_request_id));
        }
        co_return result_t<std::optional<zlink::message_t>>::failure (
          framework_error_kind_t::internal_failure, error.what ());
    }
    if (!dedup_request_id.empty ()) {
        (void) state_owner->dispatched_request_replies.complete (
          actor_request_dedup_key (key, dedup_request_id), reply);
    }
    co_return result_t<std::optional<zlink::message_t>>::success (std::move (reply));
}

result_t<void>
spot_node_runtime_t::notify_actor_disconnected_erased (const actor_ref_t &actor_ref) const
{
    if (::zlink::framework::detail::actor_ref_access_t::empty (actor_ref)) {
        return result_t<void>::failure (framework_error_kind_t::not_found, "actor ref is empty");
    }

    const auto key = actor_key (actor_ref);
    struct disconnect_plan_t
    {
        std::shared_ptr<spot_context_state_t> context_state;
        std::shared_ptr<void> spot_instance;
        std::shared_ptr<void> actor_instance;
        std::function<task_t<void> (void *, void *)> disconnect_callback;
        std::shared_ptr<spot_serial_executor_t> executor;
    };
    auto planned = _state->lane.run ([&] () -> result_t<std::optional<disconnect_plan_t>> {
        const auto found_generation = _state->actor_generations.find (key);
        if (found_generation != _state->actor_generations.end ()
            && found_generation->second != actor_ref.object_generation ()) {
            return detail::boundary_failure<std::optional<disconnect_plan_t>> (
              detail::boundary_error_t::stale_generation, "actor generation is stale");
        }

        const auto found_location = _state->actor_spot_ids.find (key);
        if (found_location == _state->actor_spot_ids.end ())
            return result_t<std::optional<disconnect_plan_t>>::success (std::nullopt);
        const auto context =
          _state->spot_contexts_by_id.find (std::string (found_location->second));
        if (context == _state->spot_contexts_by_id.end () || !context->second._state
            || !context->second._state->spot_instance) {
            return result_t<std::optional<disconnect_plan_t>>::success (std::nullopt);
        }

        const auto actor_type_key =
          std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor_ref));
        const auto actor_factory = _state->actor_factories.find (actor_type_key);
        if (actor_factory == _state->actor_factories.end ()) {
            return result_t<std::optional<disconnect_plan_t>>::failure (
              framework_error_kind_t::not_found, "actor factory is not registered");
        }
        const auto actor = _state->actor_instances.find (key);
        if (actor == _state->actor_instances.end () || !actor->second)
            return result_t<std::optional<disconnect_plan_t>>::success (std::nullopt);

        const auto context_state = context->second._state;
        const auto admission = context_state->actor_admissions.find (actor_factory->second.actor_type);
        if (admission == context_state->actor_admissions.end ()
            || !admission->second.on_disconnect_actor) {
            return result_t<std::optional<disconnect_plan_t>>::success (std::nullopt);
        }

        disconnect_plan_t plan;
        plan.context_state = context_state;
        plan.spot_instance = context_state->spot_instance;
        plan.actor_instance = actor->second;
        plan.disconnect_callback = admission->second.on_disconnect_actor;
        plan.executor = context_state->ensure_spot_serial_executor ();
        if (!plan.executor)
            return result_t<std::optional<disconnect_plan_t>>::success (std::nullopt);
        return result_t<std::optional<disconnect_plan_t>>::success (
          std::make_optional (std::move (plan)));
    }).get ();
    if (!planned) {
        return detail::propagate_failure<void> (planned,
                                                "actor disconnect state admission failed");
    }
    if (!planned.value ())
        return result_t<void>::success ();
    auto plan = std::move (*planned.value ());

    // Spec 20: Session cleanup submits the exact disconnect notification to
    // the Actor FIFO, and that turn enters the current Spot lifecycle lane.
    // Neither queue is dispatched or waited while the node state lane owns a turn.
    if (runtime::current_actor_execution.actor_key == key) {
        return plan.context_state->run_serial_task (
          "spot-lifecycle-disconnect",
          [context_state = plan.context_state, spot_instance = plan.spot_instance,
           actor_instance = plan.actor_instance, disconnect_callback = plan.disconnect_callback] {
              return disconnect_callback (spot_instance.get (), actor_instance.get ());
          });
    }
    detail::task_completion_source_t<void> completion;
    auto result = completion.task ();
    const auto posted = plan.executor->execute_actor (
      key, "actor-disconnected",
      [key, context_state = std::move (plan.context_state),
       spot_instance = std::move (plan.spot_instance),
       actor_instance = std::move (plan.actor_instance),
       disconnect_callback = std::move (plan.disconnect_callback),
       completion] (auto actor_complete) mutable {
          context_state->run_serial_task_async (
            "spot-lifecycle-disconnect",
            [key, context_state, spot_instance, actor_instance, disconnect_callback] {
                runtime::actor_execution_scope_t actor_execution (
                  key, std::string (context_state->spot_id));
                return disconnect_callback (spot_instance.get (), actor_instance.get ());
            },
            [actor_complete = std::move (actor_complete),
             completion] (result_t<void> callback_result) mutable {
                actor_complete (
                  [completion, callback_result = std::move (callback_result)] () mutable {
                      completion.complete (std::move (callback_result));
                  });
            });
      },
      runtime::serial_work_options_t{runtime::serial_work_lane_t::application,
                                     runtime::serial_execution_queue_t::fixed_work_byte_cost},
      false,
      [completion] () mutable {
          completion.complete (result_t<void>::failure (
            framework_error_kind_t::shutting_down,
            "Spot disconnect queue is closed or stopping"));
      });
    if (!posted) {
        const bool queue_closed = plan.executor->actor_queue_closed (key);
        return result_t<void>::failure (queue_closed
                                          ? framework_error_kind_t::shutting_down
                                          : framework_error_kind_t::capacity_exceeded,
                                        queue_closed
                                          ? "Actor disconnect queue is closed"
                                          : "Actor disconnect queue is full");
    }
    return result.result ();
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
    if (found != builder._state->mesh_nodes.end () && found->second && found->second->spot_state) {
        return spot_node_runtime_t (found->second->spot_state);
    }
    return std::nullopt;
}

std::vector<spot_node_snapshot_t> spot_node_runtime_t::snapshots (const zlink_builder_t &builder)
{
    std::vector<spot_node_snapshot_t> result;
    if (!builder._state)
        return result;
    result.reserve (builder._state->mesh_nodes.size ());
    for (const auto &[_, registration] : builder._state->mesh_nodes) {
        if (!registration || !registration->spot_state)
            continue;
        result.push_back (
          registration->spot_state->lane.run ([state = registration->spot_state] {
              return state->snapshot;
          }).get ());
    }
    return result;
}

local_spot_create_result_t spot_node_runtime_t::create_spot (std::string spot_name)
{
    return create_spot (std::move (spot_name), zlink::message_t{});
}

local_spot_create_result_t spot_node_runtime_t::create_spot_context (
  std::string spot_name,
  spot_id_t spot_id,
  zlink::message_t request,
  std::uint64_t object_generation,
  std::string mesh_name,
  std::function<task_t<void> (void *)> staged_restore,
  std::uint64_t authority_owner_generation)
{
    auto context_state = std::make_shared<spot_context_state_t> ();
    struct creation_plan_t
    {
        spot_lifecycle_callbacks_t lifecycle;
        std::optional<service_provider_t> root_services;
        std::shared_ptr<runtime::offload_executor_t> worker_executor;
        runtime::location_lifecycle_t *location_lifecycle = nullptr;
        std::shared_ptr<monitoring_runtime_state_t> monitoring;
        std::optional<spot_location_t> location;
        bool entry_spot = false;
    };
    auto plan = _state->lane.run ([&] {
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

        creation_plan_t result;
        if (const auto lifecycle = _state->spot_lifecycles.find (spot_name);
            lifecycle != _state->spot_lifecycles.end ()) {
            result.lifecycle = lifecycle->second;
        }
        const auto instance_spot =
          std::find (_state->snapshot.instance_spot_names.begin (),
                     _state->snapshot.instance_spot_names.end (), spot_name)
          != _state->snapshot.instance_spot_names.end ();
        result.entry_spot =
          _state->snapshot.entry_spot_name && *_state->snapshot.entry_spot_name == spot_name;
        result.root_services = _state->root_services;
        result.worker_executor = framework_worker_executor_core (_state);
        result.location_lifecycle = _state->location_lifecycle;
        result.monitoring = _state->monitoring;
        if (result.location_lifecycle)
            result.location.emplace (make_spot_location (*_state, spot_name, spot_id));

        context_state->node = _state;
        context_state->lane_owner = _state;
        context_state->channel_runtime = _state->channel_runtime;
        context_state->node_rid =
          node_rid_t::from_string (detail::effective_spot_node_rid (_state->snapshot));
        context_state->mesh_name =
          mesh_name.empty () ? _state->snapshot.name : std::move (mesh_name);
        context_state->spot_id = spot_id;
        context_state->object_generation = object_generation;
        context_state->authority_owner_generation = authority_owner_generation;
        context_state->spot_name = spot_name;
        context_state->lifecycle_domain = result.entry_spot
                                            ? spot_lifecycle_domain_t::entry ()
                                        : instance_spot
                                            ? spot_lifecycle_domain_t::instance ()
                                            : spot_lifecycle_domain_t::user ();
        if (const auto mode = _state->snapshot.spot_execution_modes.find (spot_name);
            mode != _state->snapshot.spot_execution_modes.end ()) {
            context_state->execution_mode = mode->second;
        }
        if (const auto mode = _state->spot_relocation_coordination_modes.find (spot_name);
            mode != _state->spot_relocation_coordination_modes.end ()) {
            context_state->relocation_coordination_mode = mode->second;
        }
        context_state->lifecycle = result.lifecycle;
        return result;
    }).get ();

    context_state->last_application_work_completed_ns.store (
      std::chrono::duration_cast<std::chrono::nanoseconds> (
        std::chrono::steady_clock::now ().time_since_epoch ())
        .count (),
      std::memory_order_relaxed);
    if (plan.root_services) {
        context_state->activation_scope =
          std::make_shared<service_scope_t> (service_scope_t::create (
            *plan.root_services, context_state->is_entry_spot ()
                                   ? service_scope_kind_t::entry_spot
                                   : service_scope_kind_t::spot_activation));
    }
    configure_spot_execution (context_state, plan.worker_executor);
    spot_context_t context (context_state);
    std::optional<message_t> create_reply;
    std::shared_ptr<service::spot_t> staged_native;
    const auto id_value = std::string (spot_id);

    if (plan.lifecycle.create_entry_context_instance
        || plan.lifecycle.create_instance_context_instance
        || plan.lifecycle.create_spot_context_instance) {
        service_provider_t empty_services;
        auto &activation_services = context_state->activation_scope
                                      ? context_state->activation_scope->provider ()
                                      : empty_services;
        if (plan.lifecycle.create_entry_context_instance) {
            context_state->spot_instance = plan.lifecycle.create_entry_context_instance (
              entry_spot_context_t (context_state), activation_services);
        } else if (plan.lifecycle.create_instance_context_instance) {
            context_state->spot_instance = plan.lifecycle.create_instance_context_instance (
              instance_spot_context_t (context_state), activation_services);
        } else {
            context_state->spot_instance = plan.lifecycle.create_spot_context_instance (
              spot_context_t (context_state), activation_services);
        }
        if (!context_state->spot_instance) {
            throw framework_exception_t (framework_error_kind_t::internal_failure,
                                         "SPOT factory returned null");
        }
    }

    auto remove_activation = [&] {
        auto native = context_state->native_spot.lock ();
        _state->lane.run ([&] {
            context_state->native_spot.reset ();
            if (const auto found = _state->native_spots_by_id.find (id_value);
                found != _state->native_spots_by_id.end () && found->second == native) {
                _state->native_spots_by_id.erase (found);
            }
            if (const auto found = _state->spot_contexts_by_id.find (id_value);
                found != _state->spot_contexts_by_id.end ()
                && found->second._state == context_state) {
                _state->spot_contexts_by_id.erase (found);
            }
            if (const auto found = _state->spot_names_by_id.find (id_value);
                found != _state->spot_names_by_id.end () && found->second == spot_name) {
                _state->spot_names_by_id.erase (found);
            }
            if (const auto found = _state->spot_ids_by_name.find (spot_name);
                found != _state->spot_ids_by_name.end () && found->second == spot_id) {
                _state->spot_ids_by_name.erase (found);
            }
            context_state->closed = true;
        }).get ();
        if (native) {
            try {
                native->close ();
            }
            catch (...) {
            }
        }
        context_state->detach_application_instance (false);
    };

    if (context_state->spot_instance) {
        spot_create_response_t response;
        try {
            if (staged_restore) {
                auto restored = staged_restore (context_state->spot_instance.get ()).result ();
                if (!restored) {
                    throw framework_exception_t (restored.error_kind (),
                                                 restored.error () != nullptr
                                                   ? restored.error ()->what ()
                                                   : "Spot relocation restore failed");
                }
                response = spot_create_response_t::accept ();
            } else {
                auto &serializers = *context_state->channel_runtime->serializers;
                staged_native = attach_native_spot (context_state, false, false);
                response =
                  !context_state->is_instance_spot () && plan.lifecycle.on_create
                    ? plan.lifecycle
                        .on_create (context_state->spot_instance.get (), request, serializers)
                        .result ()
                        .value ()
                    : spot_create_response_t::accept ();
            }
        }
        catch (...) {
            remove_activation ();
            throw;
        }
        if (!response.accepted) {
            remove_activation ();
            return local_spot_create_result_t{spot_id, spot_create_state_t::rejected,
                                              response.reply, std::move (context)};
        }
        create_reply = response.reply;
        if (plan.lifecycle.on_initialize) {
            try {
                plan.lifecycle.on_initialize (context_state->spot_instance.get ());
            }
            catch (...) {
                remove_activation ();
                throw;
            }
        }
        if (staged_restore)
            staged_native = attach_native_spot (context_state, true, false);
    } else
        staged_native = attach_native_spot (context_state, false, false);

    bool location_claimed = false;
    if (plan.location_lifecycle && plan.location) {
        auto location = std::move (*plan.location);
        if (const auto native = context_state->native_spot.lock ())
            location.spot_generation = native->status ().lifecycle_generation ();
        const auto claimed = plan.location_lifecycle->claim_spot (std::move (location));
        if (claimed.status != location_write_status_t::stored) {
            remove_activation ();
            return local_spot_create_result_t{spot_id, spot_create_state_t::rejected, std::nullopt,
                                              std::move (context)};
        }
        location_claimed = true;
    }

    const auto native = staged_native ? staged_native : context_state->native_spot.lock ();
    const auto published = _state->lane.run ([&] {
        if (_state->spot_contexts_by_id.contains (id_value)
            || (_state->native_spots_by_id.contains (id_value)
                && _state->native_spots_by_id.at (id_value) != native)) {
            return false;
        }
        _state->spot_contexts_by_id.emplace (id_value, spot_context_t (context_state));
        _state->spot_ids_by_name[spot_name] = spot_id;
        _state->spot_names_by_id[id_value] = spot_name;
        if (native)
            _state->native_spots_by_id[id_value] = native;
        return true;
    }).get ();
    if (!published) {
        if (location_claimed && plan.location_lifecycle)
            (void) plan.location_lifecycle->release_spot (spot_location_key_t{id_value});
        remove_activation ();
        return local_spot_create_result_t{spot_id, spot_create_state_t::rejected, std::nullopt,
                                          std::move (context)};
    }

    if (plan.monitoring) {
        runtime::runtime_metrics_t metrics (plan.monitoring);
        if (metrics.enabled ()) {
            const auto kind = plan.entry_spot ? "entry" : "user";
            metrics.counter ("zlink.spot.created", "{spot}", 1, {{"kind", kind}});
            metrics.updown ("zlink.spot.count", "{spot}", 1, {{"kind", kind}});
        }
    }
    return local_spot_create_result_t{spot_id, spot_create_state_t::created, create_reply,
                                      std::move (context)};
}

local_spot_create_result_t spot_node_runtime_t::create_spot (std::string spot_name,
                                                             zlink::message_t request)
{
    auto spot_id = _state->lane.run ([&] {
        const auto is_entry_spot =
          _state->snapshot.entry_spot_name && *_state->snapshot.entry_spot_name == spot_name;
        return is_entry_spot ? detail::new_entry_spot_id (_state->snapshot.name)
                             : detail::new_user_spot_id ();
    }).get ();
    return create_spot_context (std::move (spot_name), std::move (spot_id),
                                std::move (request));
}

local_spot_create_result_t spot_node_runtime_t::get_or_create_spot (std::string spot_name,
                                                                    spot_id_t spot_id)
{
    return get_or_create_spot (std::move (spot_name), std::move (spot_id), zlink::message_t{});
}

local_spot_create_result_t
spot_node_runtime_t::get_or_create_spot (std::string spot_name,
                                         spot_id_t spot_id,
                                         zlink::message_t request,
                                         std::uint64_t object_generation,
                                         std::string mesh_name,
                                         std::uint64_t authority_owner_generation)
{
    const auto id_value = std::string (spot_id);
    struct creation_admission_t
    {
        std::optional<local_spot_create_result_t> existing;
        std::shared_future<void> pending;
        std::shared_ptr<std::promise<void>> promise;
        std::uint64_t reservation = 0;
    };
    auto admission = _state->lane.run ([&] {
        auto same_spot_type = [&] (const std::string &existing_name) {
            const auto existing_factory = _state->spot_factories.find (existing_name);
            const auto requested_factory = _state->spot_factories.find (spot_name);
            return existing_factory == _state->spot_factories.end ()
                   || requested_factory == _state->spot_factories.end ()
                   || existing_factory->second == requested_factory->second;
        };
        creation_admission_t result;
        if (const auto existing = _state->spot_contexts_by_id.find (id_value);
            existing != _state->spot_contexts_by_id.end ()) {
            const auto existing_name = _state->spot_names_by_id.find (id_value);
            if (existing_name != _state->spot_names_by_id.end ()
                && !same_spot_type (existing_name->second)) {
                throw framework_exception_t (
                  framework_error_kind_t::type_mismatch,
                  "spot id is already bound to a different spot type");
            }
            result.existing.emplace (
              local_spot_create_result_t{spot_id, spot_create_state_t::existing, std::nullopt,
                                         spot_context_t (existing->second._state)});
            return result;
        }
        if (const auto pending = _state->pending_spot_creations_by_id.find (id_value);
            pending != _state->pending_spot_creations_by_id.end ()) {
            if (!same_spot_type (pending->second.spot_name)) {
                throw framework_exception_t (
                  framework_error_kind_t::type_mismatch,
                  "spot id is already bound to a different spot type");
            }
            result.pending = pending->second.future;
            return result;
        }
        result.promise = std::make_shared<std::promise<void>> ();
        if (_state->next_pending_spot_creation_reservation == 0) {
            throw framework_exception_t (framework_error_kind_t::internal_failure,
                                         "spot creation reservation sequence is exhausted");
        }
        result.reservation = _state->next_pending_spot_creation_reservation++;
        const auto inserted = _state->pending_spot_creations_by_id.emplace (
          id_value, detail::spot_node_builder_state_t::pending_spot_creation_t{
                      spot_name, result.promise->get_future ().share (), result.reservation});
        if (!inserted.second) {
            throw framework_exception_t (framework_error_kind_t::internal_failure,
                                         "spot creation reservation was not acquired");
        }
        return result;
    }).get ();

    if (admission.existing)
        return std::move (*admission.existing);
    if (admission.pending.valid ()) {
        admission.pending.get ();
        return _state->lane.run ([&] {
            const auto created = _state->spot_contexts_by_id.find (id_value);
            if (created == _state->spot_contexts_by_id.end ()) {
                throw framework_exception_t (
                  framework_error_kind_t::internal_failure,
                  "concurrent spot creation completed without a context");
            }
            return local_spot_create_result_t{
              spot_id_t (id_value), spot_create_state_t::existing, std::nullopt,
              spot_context_t (created->second._state)};
        }).get ();
    }

    try {
        auto result = create_spot_context (
          std::move (spot_name), std::move (spot_id), std::move (request), object_generation,
          std::move (mesh_name), {}, authority_owner_generation);
        const auto owned = _state->lane.run ([&] {
            const auto found = _state->pending_spot_creations_by_id.find (id_value);
            if (found == _state->pending_spot_creations_by_id.end ()
                || found->second.reservation != admission.reservation) {
                return false;
            }
            _state->pending_spot_creations_by_id.erase (found);
            return true;
        }).get ();
        if (!owned) {
            throw framework_exception_t (framework_error_kind_t::internal_failure,
                                         "spot creation reservation ownership was lost");
        }
        admission.promise->set_value ();
        return std::move (result);
    }
    catch (...) {
        const auto error = std::current_exception ();
        const auto owned = _state->lane.run ([&] {
            const auto found = _state->pending_spot_creations_by_id.find (id_value);
            if (found == _state->pending_spot_creations_by_id.end ()
                || found->second.reservation != admission.reservation) {
                return false;
            }
            _state->pending_spot_creations_by_id.erase (found);
            return true;
        }).get ();
        if (owned)
            admission.promise->set_exception (error);
        throw;
    }
}

task_t<zlink::message_t>
spot_node_runtime_t::dispatch_instance_activation (const spot_id_t &spot_id,
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
    const auto diagnostics_mode = detail::message_flow_tracer_t (_state->dispatch).mode ();
    auto flow_scope = runtime::flow_context_t::enter (std::move (flow_id), flow_origin,
                                                      diagnostics_mode, flow_origin_t::inbound);
    auto context = find_context (spot_id);
    const auto context_state = context ? context->_state : nullptr;
    const auto materialized = _state->lane.run ([&] {
        return context_state && context_state->spot_instance
               && std::find (_state->snapshot.instance_spot_names.begin (),
                             _state->snapshot.instance_spot_names.end (), context_state->spot_name)
                    != _state->snapshot.instance_spot_names.end ();
    }).get ();
    if (!materialized) {
        const framework_exception_t error (framework_error_kind_t::not_found,
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
    auto state = context_state;
    const auto message_kind =
      request ? dispatch_message_kind_t::request : dispatch_message_kind_t::send;
    report_spot_dispatch_trace (_state, message_flow_outcome_t::received,
                                dispatch_error_surface_t::spot_route, message_kind, packet_name, {},
                                std::string (spot_id), {}, correlation_id);
    /* Copies exist only for the completion-time trace/error report; skip them
     * while tracing is fully off (spec 26 §4: no allocation before the gate). */
    const bool activation_trace_enabled =
      message_flow_tracer_t (_state->dispatch).capture_enabled ();
    std::string trace_packet_name;
    std::string trace_spot_id;
    std::string trace_correlation_id;
    if (activation_trace_enabled) {
        trace_packet_name = packet_name;
        trace_spot_id = std::string (spot_id);
        trace_correlation_id = correlation_id;
    }
    auto handler_task = spot_handler_registry_t (state).invoke_erased (
      spot_handler_kind_t::packet, packet_name, {}, std::type_index (typeid (void)),
      state->spot_instance.get (), nullptr, services, serializers, zlink::message_t::from (payload),
      spot_inbound_message_t{.content_type = std::move (content_type),
                             .values = std::move (metadata),
                             .mesh_name = state->mesh_name,
                             .correlation_id = std::move (correlation_id)});
    detail::observe_task_completion (
      handler_task, [node = _state, state, request, message_kind, trace_packet_name, trace_spot_id,
                     trace_correlation_id] (const result_t<zlink::message_t> &result) {
          state->last_application_work_completed_ns.store (
            std::chrono::duration_cast<std::chrono::nanoseconds> (
              std::chrono::steady_clock::now ().time_since_epoch ())
              .count (),
            std::memory_order_relaxed);
          if (result) {
              report_spot_dispatch_trace (
                node,
                request ? message_flow_outcome_t::replied : message_flow_outcome_t::dispatched,
                dispatch_error_surface_t::spot_route,
                request ? dispatch_message_kind_t::response : message_kind, trace_packet_name, {},
                trace_spot_id, {}, trace_correlation_id);
              return;
          }
          const auto *error = result.error ();
          const framework_exception_t exception (result.error_kind (),
                                                 error != nullptr ? error->what ()
                                                                  : "Instance Spot handler failed");
          report_spot_dispatch_error (node, dispatch_error_surface_t::spot_route, message_kind,
                                      dispatch_reason_from_error (exception.kind ()),
                                      request ? dispatch_error_action_t::reply_error
                                              : dispatch_error_action_t::drop,
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
    return _state->lane.run ([&] {
        std::vector<spot_info_t> spots;
        spots.reserve (_state->spot_names_by_id.size ());
        for (const auto &[rid, name] : _state->spot_names_by_id) {
            spots.push_back (spot_info_t{spot_id_t (rid), name});
        }
        return spots;
    }).get ();
}

task_t<bool> spot_node_runtime_t::close_spot (spot_id_t spot_id)
{
    struct close_plan_t
    {
        std::optional<spot_context_t> context;
        std::shared_ptr<monitoring_runtime_state_t> monitoring;
        std::string kind;
    };
    auto plan = _state->lane.run ([&] {
        close_plan_t result;
        const auto found = _state->spot_contexts_by_id.find (std::string (spot_id));
        if (found != _state->spot_contexts_by_id.end ()) {
            result.context.emplace (spot_context_t (found->second._state));
            result.monitoring = _state->monitoring;
            result.kind = found->second._state->is_entry_spot () ? "entry" : "user";
        }
        return result;
    }).get ();
    if (!plan.context) {
        co_return result_t<bool>::success (false);
    }
    const bool closed = plan.context->close ().result ().value ();
    if (closed && plan.monitoring) {
        runtime::runtime_metrics_t metrics (plan.monitoring);
        if (metrics.enabled ()) {
            metrics.counter ("zlink.spot.closed", "{spot}", 1,
                             {{"kind", plan.kind}});
            metrics.updown ("zlink.spot.count", "{spot}", -1,
                            {{"kind", plan.kind}});
        }
    }
    co_return result_t<bool>::success (closed);
}

bool spot_node_runtime_t::close_all_user_spots ()
{
    std::vector<spot_id_t> user_spots;
    _state->lane.run ([&] {
        user_spots.reserve (_state->spot_contexts_by_id.size ());
        for (const auto &[rid, context] : _state->spot_contexts_by_id) {
            if (!context._state || context._state->closed
                || context._state->native_spot.expired ()) {
                continue;
            }
            if (context._state->is_entry_spot ()) {
                continue;
            }
            user_spots.push_back (spot_id_t (rid));
        }
    }).get ();
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
    return _state->lane.run ([&] {
        return node_rid_t::from_string (detail::effective_spot_node_rid (_state->snapshot));
    }).get ();
}

std::optional<std::string> spot_node_runtime_t::spot_name_for (spot_id_t spot_id) const
{
    return _state->lane.run ([&] { return spot_name_for_unlocked (spot_id); }).get ();
}

std::optional<std::string>
spot_node_runtime_t::spot_name_for_unlocked (const spot_id_t &spot_id) const
{
    const auto found = _state->spot_names_by_id.find (std::string (spot_id));
    if (found == _state->spot_names_by_id.end ()) {
        return std::nullopt;
    }
    return found->second;
}

std::optional<spot_route_t> spot_node_runtime_t::resolve_spot (spot_id_t spot_id) const
{
    struct resolution_plan_t
    {
        std::optional<spot_route_t> local;
        std::vector<std::function<std::optional<spot_route_t> (spot_id_t)>> resolvers;
        runtime::spot_address_resolver_t *location_resolver = nullptr;
        std::string mesh_name;
    };
    auto plan = _state->lane.run ([&] {
        resolution_plan_t result;
        const auto found = _state->spot_names_by_id.find (std::string (spot_id));
        if (found != _state->spot_names_by_id.end ()) {
            result.local = spot_route_t{
              node_rid_t::from_string (detail::effective_spot_node_rid (_state->snapshot)), spot_id,
              found->second};
            return result;
        }
        result.resolvers.reserve (_state->resolvers.size ());
        for (const auto &[_, resolver] : _state->resolvers)
            result.resolvers.push_back (resolver);
        result.location_resolver = _state->spot_location_resolver;
        result.mesh_name = _state->snapshot.name;
        return result;
    }).get ();
    if (plan.local)
        return std::move (plan.local);
    for (const auto &resolver : plan.resolvers) {
        if (auto route = resolver (spot_id)) {
            return route;
        }
    }
    if (plan.location_resolver) {
        const auto address =
          plan.location_resolver->resolve_spot_address (std::move (plan.mesh_name), spot_id)
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
    return _state->lane.run ([&] () -> std::optional<spot_id_t> {
        const auto found = _state->actor_spot_ids.find (actor_key (actor_ref));
        if (found == _state->actor_spot_ids.end ()) {
            return std::nullopt;
        }
        return found->second;
    }).get ();
}

result_t<bool> spot_node_runtime_t::destroy_actor (const actor_ref_t &actor_ref)
{
    if (::zlink::framework::detail::actor_ref_access_t::empty (actor_ref)) {
        return result_t<bool>::failure (framework_error_kind_t::invalid_operation,
                                        "ActorRef must identify an actor to destroy");
    }

    const auto local_node_rid = _state->lane.run ([&] {
        return detail::effective_spot_node_rid (_state->snapshot);
    }).get ();
    if (actor_ref.node_rid ().empty () || actor_ref.node_rid ().value () != local_node_rid) {
        return result_t<bool>::failure (framework_error_kind_t::invalid_operation,
                                        "ActorRef does not identify an actor on this node");
    }

    const auto key = actor_key (actor_ref);
    std::function<result_t<void> (const actor_ref_t &)> destroy_registry;
    const auto selected = _state->lane.run ([&] {
        if (_state->actor_transfer_coordinator.blocks_dispatch (key)) {
            return result_t<bool>::failure (framework_error_kind_t::unavailable,
                                            "Actor transfer is in progress");
        }

        const auto generation = _state->actor_generations.find (key);
        if (generation == _state->actor_generations.end ()) {
            return result_t<bool>::success (false);
        }
        if (generation->second != actor_ref.object_generation ()) {
            return result_t<bool>::failure (framework_error_kind_t::invalid_operation,
                                            "ActorRef generation does not match the current actor");
        }
        if (_state->destroying_actors.contains (key)) {
            return result_t<bool>::success (false);
        }

        _state->destroying_actors.insert (key);
        const auto queue_location = _state->actor_spot_ids.find (key);
        if (queue_location != _state->actor_spot_ids.end ()) {
            const auto context = _state->spot_contexts_by_id.find (
              std::string (queue_location->second));
            if (context != _state->spot_contexts_by_id.end () && context->second._state) {
                decrement_actor_count_unlocked (*context->second._state);
            }
        }

        const auto location = _state->actor_spot_ids.find (key);
        if (location != _state->actor_spot_ids.end ()) {
            const auto context = _state->spot_contexts_by_id.find (std::string (location->second));
            if (context != _state->spot_contexts_by_id.end () && context->second._state
                && context->second._state->spot_serial_executor) {
                context->second._state->spot_serial_executor->erase_actor_queue (key);
            }
        }
        erase_actor_route_unlocked (*_state, key);
        _state->actor_created_keys.erase (key);
        _state->destroyed_actor_keys.insert (key);
        _state->actor_instances.erase (key);
        detail::erase_actor_instance_index_unlocked (
          *_state, ::zlink::framework::detail::actor_ref_access_t::actor_type (actor_ref),
          actor_ref.actor_id ().value ());
        _state->actor_types_by_id.erase (std::string (actor_ref.actor_id ().value ()));
        _state->mesh_runtime_owned_native_actor_ids.erase (
          std::string (actor_ref.actor_id ().value ()));
        _state->core_actor_membership_epochs.erase (std::string (actor_ref.actor_id ().value ()));
        (void) _state->dispatched_request_replies.erase_if ([&] (const auto &request_key) {
            return request_key.starts_with (actor_request_dedup_prefix (key));
        });
        destroy_registry = _state->destroy_actor_registry;
        _state->destroying_actors.erase (key);
        return result_t<bool>::success (true);
    }).get ();
    if (!selected)
        return result_t<bool>::failure (
          selected.error_kind (), selected.error () != nullptr ? selected.error ()->what ()
                                                               : "Actor destroy failed");
    if (!selected.value ())
        return result_t<bool>::success (false);

    _state->actor_pending_requests_lane.run ([this, &key] {
        _state->actor_pending_requests.erase (key);
    }).get ();
    release_actor_location (_state, actor_ref);
    if (destroy_registry) {
        auto destroyed = destroy_registry (actor_ref);
        if (!destroyed) {
            return result_t<bool>::failure (destroyed.error_kind (),
                                            destroyed.error () ? destroyed.error ()->what ()
                                                               : "Actor registry cleanup failed");
        }
    }
    return result_t<bool>::success (true);
}

void spot_node_runtime_t::record_actor_spot (const actor_ref_t &actor_ref, spot_id_t spot_id)
{
    _state->lane.run ([&] {
        const auto key = actor_key (actor_ref);
        auto name = spot_name_for_unlocked (spot_id).value_or ("");
        detail::record_actor_route_unlocked (
          *_state, key,
          spot_route_t{node_rid_t::from_string (detail::effective_spot_node_rid (_state->snapshot)),
                       std::move (spot_id), std::move (name)},
          actor_ref.object_generation ());
    }).get ();
}

std::optional<spot_route_t> spot_node_runtime_t::actor_route (const actor_ref_t &actor_ref) const
{
    return _state->lane.run ([&] () -> std::optional<spot_route_t> {
        const auto found = _state->actor_routes.find (actor_key (actor_ref));
        if (found == _state->actor_routes.end ()) {
            return std::nullopt;
        }
        return found->second;
    }).get ();
}

bool spot_node_runtime_t::matches_actor_message_follow_source (
  const actor_ref_t &actor_ref, const runtime::protocol::actor_route_fence_t &source_fence) const
{
    return _state->actor_transfer_coordinator.matches_message_follow_source (actor_key (actor_ref),
                                                                             source_fence);
}

result_t<std::optional<actor_message_follow_target_t>>
spot_node_runtime_t::try_acquire_actor_message_follow (
  const actor_ref_t &actor_ref,
  std::size_t payload_bytes,
  std::size_t hop_count,
  const runtime::protocol::actor_route_fence_t &source_fence)
{
    return _state->actor_transfer_coordinator.try_acquire_message_follow (
      actor_key (actor_ref), actor_ref.object_generation (), payload_bytes, hop_count,
      source_fence);
}

void spot_node_runtime_t::release_actor_message_follow (
  const actor_ref_t &actor_ref,
  const runtime::protocol::actor_route_fence_t &source_fence,
  std::size_t payload_bytes) noexcept
{
    _state->actor_transfer_coordinator.release_message_follow (actor_key (actor_ref), source_fence,
                                                               payload_bytes);
}

bool spot_node_runtime_t::try_begin_actor_message_follow_notification (
  const actor_ref_t &actor_ref,
  const runtime::protocol::actor_route_fence_t &source_fence,
  const runtime::protocol::actor_route_fence_t &target_fence)
{
    return _state->actor_transfer_coordinator.try_begin_message_follow_notification (
      actor_key (actor_ref), source_fence, target_fence);
}

bool spot_node_runtime_t::complete_actor_message_follow_notification (
  const actor_ref_t &actor_ref,
  const runtime::protocol::actor_route_fence_t &source_fence,
  const runtime::protocol::actor_route_fence_t &target_fence,
  bool transport_accepted)
{
    return _state->actor_transfer_coordinator.complete_message_follow_notification (
      actor_key (actor_ref), source_fence, target_fence, transport_accepted);
}

void spot_node_runtime_t::record_actor_route (const actor_ref_t &actor_ref, spot_route_t route)
{
    _state->lane.run ([&] {
        const auto key = actor_key (actor_ref);
        detail::record_actor_route_unlocked (*_state, key, std::move (route),
                                             actor_ref.object_generation ());
    }).get ();
}

std::optional<std::string> spot_node_runtime_t::actor_route_transport_name () const
{
    return _state->lane.run ([&] () -> std::optional<std::string> {
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
    }).get ();
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
        _state->lane.run ([&] {
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
        }).get ();
        for (auto &context : contexts) {
            try {
                context->detach_application_instance (true, spot_close_reason_t::host_shutdown);
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
    auto [cancellation, worker, deadline] = _state->lane.run ([&] {
        return std::make_tuple (_state->worker_cancellation, _state->worker_executor,
                                _state->deadline_executor);
    }).get ();
    cancellation.request_stop ();
    if (worker)
        worker->request_stop ();
    if (deadline)
        deadline->request_stop ();
}

void spot_node_runtime_t::bind_service_provider (service_provider_t &services)
{
    _state->lane.run ([&] { _state->root_services = services; }).get ();
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
        const auto contexts = _state->lane.run ([&] {
            std::vector<std::shared_ptr<spot_context_state_t>> result;
            result.reserve (_state->spot_contexts_by_id.size ());
            for (const auto &[_, context] : _state->spot_contexts_by_id) {
                if (context._state)
                    result.push_back (context._state);
            }
            return result;
        }).get ();
        for (const auto &context : contexts)
            detail::timer_runtime_t (context).cancel_all ();
    }
    catch (...) {
    }
}

std::optional<actor_ref_t>
spot_node_runtime_t::current_actor_ref (const actor_ref_t &actor_ref) const
{
    return _state->lane.run ([&] () -> std::optional<actor_ref_t> {
        const auto key = actor_key (actor_ref);
        const auto found = _state->actor_generations.find (key);
        if (found == _state->actor_generations.end ()) {
            return std::nullopt;
        }
        // The recorded location can be this node's own local spot, or (after a
        // relocation this node followed away, e.g. a Message Follow route) a
        // route naming the remote current owner; report whichever this node
        // actually knows rather than always claiming local ownership.
        auto node_rid_value = detail::effective_spot_node_rid (_state->snapshot);
        if (const auto routed = _state->actor_routes.find (key);
            routed != _state->actor_routes.end ()) {
            node_rid_value = std::string (routed->second.node_rid.value ());
        }
        return ::zlink::framework::detail::actor_ref_access_t::make (
          node_rid_t::from_string (node_rid_value),
          std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor_ref)),
          std::string (actor_ref.actor_id ().value ()), found->second);
    }).get ();
}

void spot_node_runtime_t::attach_native_node (std::shared_ptr<service::mesh_node_t> node)
{
    struct attach_plan_t
    {
        std::vector<std::shared_ptr<spot_context_state_t>> contexts;
        std::chrono::milliseconds idle_timeout{0};
        bool create_control_spot = false;
        bool create_idle_timer = false;
    };
    const auto native = node;
    const auto plan = _state->lane.run ([&] {
        attach_plan_t result;
        _state->stopping.store (false, std::memory_order_release);
        _state->worker_cancellation = std::stop_source{};
        _state->native_node = node;
        result.create_control_spot = _state->spot_contexts_by_id.empty ();
        result.contexts.reserve (_state->spot_contexts_by_id.size ());
        for (const auto &[_, context] : _state->spot_contexts_by_id) {
            if (context._state)
                result.contexts.push_back (context._state);
        }
        result.idle_timeout = _state->instance_spot_idle_timeout;
        result.create_idle_timer =
          result.idle_timeout > std::chrono::milliseconds::zero ()
          && !_state->instance_spot_idle_timer;
        return result;
    }).get ();
    if (plan.create_control_spot && native) {
        auto control = std::make_shared<service::spot_t> (native->entry_spot ());
        _state->lane.run ([&] {
            if (_state->spot_contexts_by_id.empty () && !_state->routed_control_spot)
                _state->routed_control_spot = std::move (control);
        }).get ();
    }
    for (const auto &context : plan.contexts)
        attach_native_spot (context);
    if (plan.create_idle_timer) {
        auto weak_state = std::weak_ptr<spot_node_builder_state_t> (_state);
        auto timer = std::make_unique<zlink::timer_t> ();
        timer->on_fire ([weak_state] (std::uint64_t) {
            if (auto state = weak_state.lock ()) {
                if (!state->stopping.load (std::memory_order_acquire))
                    spot_node_runtime_t (std::move (state)).evict_idle_spots ();
            }
        });
        timer->start (plan.idle_timeout,
                      std::numeric_limits<std::uint64_t>::max ());
        _state->lane.run ([&] {
            if (!_state->instance_spot_idle_timer)
                _state->instance_spot_idle_timer = std::move (timer);
        }).get ();
    }
}

void spot_node_runtime_t::detach_native_node ()
{
    zlink::timer_t *idle_timer = nullptr;
    auto native_spots = _state->lane.run ([&] {
        std::vector<std::shared_ptr<service::spot_t>> result;
        idle_timer = _state->instance_spot_idle_timer.get ();
        result.reserve (_state->native_spots_by_id.size ());
        for (const auto &[_, native] : _state->native_spots_by_id) {
            if (native)
                result.push_back (native);
        }
        _state->native_node.reset ();
        _state->native_spots_by_id.clear ();
        _state->routed_control_spot.reset ();
        for (auto &[_, context] : _state->spot_contexts_by_id) {
            context._state->native_spot.reset ();
        }
        return result;
    }).get ();
    if (idle_timer) {
        try {
            idle_timer->stop ();
        }
        catch (...) {
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
        auto [idle_timeout, admit_eviction, initial_candidates] = _state->lane.run ([&] {
            std::vector<std::shared_ptr<spot_context_state_t>> states;
            states.reserve (_state->spot_contexts_by_id.size ());
            for (const auto &[_, context] : _state->spot_contexts_by_id) {
                if (context._state)
                    states.push_back (context._state);
            }
            return std::make_tuple (_state->instance_spot_idle_timeout,
                                    _state->admit_instance_spot_idle_eviction,
                                    std::move (states));
        }).get ();
        if (idle_timeout <= std::chrono::milliseconds::zero () || !admit_eviction
            || _state->stopping.load (std::memory_order_acquire))
            return;

        const auto now_ns = std::chrono::duration_cast<std::chrono::nanoseconds> (
                              std::chrono::steady_clock::now ().time_since_epoch ())
                              .count ();
        const auto timeout_ns =
          std::chrono::duration_cast<std::chrono::nanoseconds> (idle_timeout)
            .count ();
        std::vector<std::pair<std::shared_ptr<spot_context_state_t>, std::uint64_t>> candidates;
        for (const auto &state : initial_candidates) {
            const auto queue = state->serial_queue;
            if (!queue
                || queue->pending_count (runtime::serial_work_lane_t::application) != 0
                || queue->pending_count (runtime::serial_work_lane_t::lifecycle) != 0)
                continue;
            bool timer_busy = false;
            for (const auto &timer : state->timers) {
                if (!timer)
                    continue;
                std::lock_guard timer_lock (timer->mutex);
                if (!timer->disposed
                    && (timer->running || timer->pending_fire || timer->pending_fire_count != 0)) {
                    timer_busy = true;
                    break;
                }
            }
            if (timer_busy)
                continue;
            const auto last_ns =
              state->last_application_work_completed_ns.load (std::memory_order_relaxed);
            if (last_ns <= 0 || now_ns < last_ns || now_ns - last_ns < timeout_ns)
                continue;
            const auto sealed = state->callback_lane.run ([state] {
                    if (state->callback_admission_closed || state->callback_depth != 0
                        || state->close_requested || state->idle_eviction_in_progress)
                        return false;
                    /* Seal admission only after the candidate checks above.
                    * try_close_idle repeats the quiescence and idle-age
                     * checks after the Location Store transaction. */
                    state->idle_eviction_in_progress = true;
                    return true;
            }).get ();
            if (!sealed)
                continue;
            const auto accepted = _state->lane.run ([&] {
                const auto found = _state->spot_contexts_by_id.find (std::string (state->spot_id));
                const auto last =
                  state->last_application_work_completed_ns.load (std::memory_order_relaxed);
                return found != _state->spot_contexts_by_id.end ()
                       && found->second._state == state && !state->closed
                       && state->is_instance_spot () && state->spot_instance
                       && state->actor_count == 0 && !state->relocation_boundary_active
                       && !state->relocation_ready_deferred && state->queued_routed_packets.empty ()
                       && state->authority_owner_generation != 0
                       && !_state->pending_spot_creations_by_id.contains (
                         std::string (state->spot_id))
                       && last > 0 && now_ns >= last && now_ns - last >= timeout_ns;
            }).get ();
            if (!accepted) {
                state->callback_lane.run ([state] {
                    state->idle_eviction_in_progress = false;
                }).get ();
                continue;
            }
            const auto reservation = state->begin_idle_close_reservation ();
            if (reservation == 0) {
                state->callback_lane.run ([state] {
                    state->idle_eviction_in_progress = false;
                }).get ();
                continue;
            }
            candidates.emplace_back (state, reservation);
        }

        for (const auto &[state, reservation] : candidates) {
            bool evicted = false;
            try {
                evicted = admit_eviction (
                  state->spot_id, state->spot_name, state->object_generation,
                  state->authority_owner_generation,
                  [state, reservation] { return state->try_close_idle (reservation); });
            }
            catch (...) {
                evicted = false;
            }
            if (!evicted) {
                state->cancel_idle_close_reservation (reservation);
                const auto reset = _state->lane.run ([&] { return !state->closed; }).get ();
                if (reset) {
                    state->callback_lane.run ([state] {
                        state->idle_eviction_in_progress = false;
                    }).get ();
                }
            }
        }
    }
    catch (...) {
    }
}

void spot_node_runtime_t::record_core_actor_transfer_activation (std::string actor_id,
                                                                 std::uint64_t membership_epoch)
{
    _state->lane.run ([&] {
        _state->mesh_runtime_owned_native_actor_ids.insert (actor_id);
        _state->core_actor_membership_epochs[std::move (actor_id)] = membership_epoch;
    }).get ();
}

void spot_node_runtime_t::bind_location_lifecycle (runtime::location_lifecycle_t &lifecycle)
{
    _state->lane.run ([&] { _state->location_lifecycle = &lifecycle; }).get ();
}

bool spot_node_runtime_t::has_active_callbacks () const
{
    const auto contexts = _state->lane.run ([&] {
        std::vector<std::shared_ptr<spot_context_state_t>> result;
        result.reserve (_state->spot_contexts_by_id.size ());
        for (const auto &[_, context] : _state->spot_contexts_by_id) {
            if (context._state) {
                result.push_back (context._state);
            }
        }
        return result;
    }).get ();
    for (const auto &context : contexts) {
        if (context->has_active_callback ()) {
            return true;
        }
    }
    return false;
}

std::vector<actor_ref_t> spot_node_runtime_t::local_actor_refs () const
{
    return _state->lane.run ([&] {
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
            refs.push_back (::zlink::framework::detail::actor_ref_access_t::make (
              node_rid_t::from_string (node_rid), key.substr (0, split), key.substr (split + 1),
              generation->second));
        }
        return refs;
    }).get ();
}

std::optional<zlink::message_t>
spot_node_runtime_t::serialize_actor_snapshot (const actor_ref_t &actor_ref) const
{
    struct snapshot_plan_t
    {
        std::shared_ptr<void> actor;
        std::function<std::optional<zlink::message_t> (void *, serializer_registry_t &)> serialize;
        serializer_registry_t *serializers = nullptr;
    };
    const auto plan = _state->lane.run ([&] {
        snapshot_plan_t result;
        const auto actor = _state->actor_instances.find (actor_key (actor_ref));
        const auto factory = _state->actor_factories.find (
          std::string (::zlink::framework::detail::actor_ref_access_t::actor_type (actor_ref)));
        if (actor != _state->actor_instances.end () && actor->second
            && factory != _state->actor_factories.end () && _state->channel_runtime
            && _state->channel_runtime->serializers) {
            result.actor = actor->second;
            result.serialize = factory->second.serialize_instance;
            result.serializers = _state->channel_runtime->serializers;
        }
        return result;
    }).get ();
    if (!plan.actor || !plan.serialize || !plan.serializers) {
        return std::nullopt;
    }
    return plan.serialize (plan.actor.get (), *plan.serializers);
}

void spot_node_runtime_t::bind_drain_flag (std::shared_ptr<std::atomic_bool> flag)
{
    _state->lane.run ([&] { _state->drain_flag = std::move (flag); }).get ();
}

void spot_node_runtime_t::bind_spot_location_resolver (runtime::spot_address_resolver_t &resolver)
{
    _state->lane.run ([&] { _state->spot_location_resolver = &resolver; }).get ();
}

std::shared_ptr<service::mesh_node_t> spot_node_runtime_t::native_node () const
{
    return _state->lane.run ([&] { return _state->native_node.lock (); }).get ();
}

task_t<void>
spot_node_runtime_t::send_spot_mesh_parts (const zlink::routing_id_t &target_node_rid,
                                           const spot_id_t &target_spot_id,
                                           runtime::messaging::message_parts_t parts) const
{
    auto node = native_node ();
    if (!node) {
        throw framework_exception_t (framework_error_kind_t::not_found,
                                     "SPOT mesh route requires a running native node");
    }
    try {
        auto native_parts = parts.items ();
        if (native_parts.empty ()) {
            throw framework_exception_t (framework_error_kind_t::protocol_error,
                                         "SPOT mesh send requires at least one message part");
        }
        auto egress = node->entry_spot ();
        const auto target_generation =
          resolve_target_spot_generation (_state, target_node_rid, target_spot_id);
        if (!target_generation) {
            throw framework_exception_t (framework_error_kind_t::not_found,
                                         "SPOT mesh send target generation is unavailable");
        }
        const auto submitted = co_await egress.send_to_spot (target_node_rid, target_spot_id,
                                                             *target_generation, native_parts);
        if (submitted != zlink::submit_result_t::ok) {
            throw framework_exception_t (
              runtime::messaging::map_submit_result_error_kind (submitted),
              "SPOT mesh send was not submitted");
        }
        co_return;
    }
    catch (const framework_exception_t &error) {
        throw error;
    }
    catch (const std::exception &error) {
        throw framework_exception_t (framework_error_kind_t::internal_failure, error.what ());
    }
}

task_t<void>
spot_node_runtime_t::send_spot_mesh_parts_exact (const spot_id_t &source_spot_id,
                                                 const zlink::routing_id_t &target_node_rid,
                                                 const spot_id_t &target_spot_id,
                                                 std::uint64_t target_spot_generation,
                                                 runtime::messaging::message_parts_t parts) const
{
    const auto source = _state->lane.run ([&] {
        const auto found = _state->native_spots_by_id.find (source_spot_id);
        if (found != _state->native_spots_by_id.end ())
            return found->second;
        return std::shared_ptr<service::spot_t>{};
    }).get ();
    if (!source || target_spot_generation == 0) {
        throw framework_exception_t (
          framework_error_kind_t::not_found,
          "SPOT mesh send exact source or target generation is unavailable");
    }
    auto native_parts = parts.items ();
    if (native_parts.empty ()) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "SPOT mesh send requires at least one message part");
    }
    const auto submitted = co_await source->send_to_spot (target_node_rid, target_spot_id,
                                                          target_spot_generation, native_parts);
    if (submitted != zlink::submit_result_t::ok) {
        throw framework_exception_t (runtime::messaging::map_submit_result_error_kind (submitted),
                                     "SPOT mesh send was not submitted");
    }
}

task_t<zlink::submit_result_t>
spot_node_runtime_t::send_actor_leave_notification (const zlink::routing_id_t &target_node_rid,
                                                    runtime::messaging::message_parts_t parts) const
{
    auto sender = _state->lane.run ([&] { return _state->actor_leave_notification_sender; }).get ();
    if (!sender) {
        throw framework_exception_t (framework_error_kind_t::not_configured,
                                     "Actor OnLeave node notification transport is not configured");
    }
    auto native_parts = parts.items ();
    if (native_parts.empty ()) {
        throw framework_exception_t (
          framework_error_kind_t::protocol_error,
          "Actor OnLeave notification requires at least one message part");
    }
    co_return co_await sender (target_node_rid, std::move (native_parts));
}

std::optional<std::uint64_t>
spot_node_runtime_t::resolve_spot_generation (const zlink::routing_id_t &target_node_rid,
                                              const spot_id_t &target_spot_id) const
{
    return resolve_target_spot_generation (_state, target_node_rid, target_spot_id);
}

result_t<std::uint64_t> spot_node_runtime_t::resolve_wire_actor_join_target (
  const runtime::protocol::spot_route_fence_t &fence) const
{
    if (fence.spot_id.empty () || fence.object_generation == 0
        || fence.target_node_routing_id.empty () || fence.target_node_generation == 0
        || fence.authority_owner_generation == 0 || fence.owner_lease_generation == 0) {
        return result_t<std::uint64_t>::failure (
          framework_error_kind_t::protocol_error,
          "remote Actor Join target Spot fence is incomplete");
    }

    auto services = _state->lane.run ([&] { return _state->root_services; }).get ();
    if (!services) {
        return result_t<std::uint64_t>::failure (
          framework_error_kind_t::unavailable,
          "remote Actor Join target Spot Authority Store is unavailable");
    }
    runtime::live_location_reader_t *store = nullptr;
    try {
        store = &services->get_required<runtime::live_location_reader_t> ();
    }
    catch (const std::exception &) {
        return result_t<std::uint64_t>::failure (
          framework_error_kind_t::unavailable,
          "remote Actor Join target Spot Authority Store is unavailable");
    }

    try {
        // Admission fences must read the Authority row at this boundary.
        // store_location_resolvers_t may return a valid-but-stale cache
        // projection, which is suitable for routing but cannot authorize a
        // canonical actorJoin target.
        const auto read =
          store->read_authority (runtime::spot_authority_key (fence.spot_id)).result ();
        if (!read) {
            return result_t<std::uint64_t>::failure (
              framework_error_kind_t::unavailable,
              "remote Actor Join target Spot Authority row could not be read");
        }
        const auto *snapshot = std::get_if<authority_snapshot_t> (&read.value ());
        if (snapshot == nullptr) {
            return result_t<std::uint64_t>::failure (
              framework_error_kind_t::not_found,
              "remote Actor Join target Spot Authority row is missing");
        }
        if (snapshot->allocation.state != placement_allocation_state_t::active
            || snapshot->allocation.object_kind != placement_object_kind_t::user_spot
            || snapshot->object_generation != fence.object_generation
            || snapshot->allocation.target.node_rid.value ()
                 != zlink::routing_id_t::from (fence.target_node_routing_id).to_string ()
            || snapshot->allocation.target.node_lifecycle_generation != fence.target_node_generation
            || snapshot->authority_owner_generation != fence.authority_owner_generation
            || snapshot->owner.lease_generation <= 0
            || static_cast<std::uint64_t> (snapshot->owner.lease_generation)
                 != fence.owner_lease_generation) {
            return result_t<std::uint64_t>::failure (
              framework_error_kind_t::protocol_error,
              "remote Actor Join target Spot Authority row does not exactly match its route fence");
        }
        return result_t<std::uint64_t>::success (snapshot->object_generation);
    }
    catch (const std::exception &) {
        return result_t<std::uint64_t>::failure (
          framework_error_kind_t::unavailable,
          "remote Actor Join target Spot Authority row could not be read");
    }
    catch (...) {
        return result_t<std::uint64_t>::failure (
          framework_error_kind_t::unavailable,
          "remote Actor Join target Spot Authority row could not be read");
    }
}

void spot_node_runtime_t::set_route_client (route_client_t route_client)
{
    _state->route_client_lane
      .run ([&] { _state->route_client = std::move (route_client); })
      .get ();
}

std::vector<spot_context_t> spot_node_runtime_t::active_contexts () const
{
    return _state->lane.run ([&] {
        std::vector<spot_context_t> contexts;
        contexts.reserve (_state->spot_contexts_by_id.size ());
        for (const auto &[_, context] : _state->spot_contexts_by_id) {
            if (!context._state->closed && !context._state->native_spot.expired ()) {
                contexts.push_back (spot_context_t (context._state));
            }
        }
        return contexts;
    }).get ();
}

std::vector<spot_id_t> spot_node_runtime_t::deferred_relocation_ready_spots () const
{
    const auto contexts = _state->lane.run ([&] {
        std::vector<std::shared_ptr<spot_context_state_t>> result;
        result.reserve (_state->spot_contexts_by_id.size ());
        for (const auto &[_, context] : _state->spot_contexts_by_id) {
            const auto state = context._state;
            if (!state || state->is_entry_spot () || state->is_instance_spot ())
                continue;
            result.push_back (state);
        }
        return result;
    }).get ();
    std::vector<spot_id_t> result;
    for (const auto &state : contexts) {
        if (state->callback_lane.run ([state] {
              return state->relocation_ready_deferred;
            }).get ())
            result.push_back (state->spot_id);
    }
    return result;
}

std::vector<spot_node_runtime_t::application_relocation_unit_t>
spot_node_runtime_t::application_relocation_units () const
{
    auto plans = _state->lane.run ([&] {
        std::vector<std::pair<application_relocation_unit_t,
                              std::shared_ptr<spot_context_state_t>>>
          result;
        const auto node_rid = detail::effective_spot_node_rid (_state->snapshot);
        for (const auto &[_, context] : _state->spot_contexts_by_id) {
            const auto state = context._state;
            if (!state || state->is_entry_spot () || state->is_instance_spot ()
                || state->execution_mode != user_spot_execution_mode_t::spot_wide
                || state->relocation_coordination_mode
                     != spot_relocation_coordination_mode_t::application_signaled)
                continue;
            application_relocation_unit_t unit{state->spot_id, state->spot_name, false, {}};
            for (const auto &[key, actor_spot_id] : _state->actor_spot_ids) {
                if (actor_spot_id != state->spot_id)
                    continue;
                const auto split = key.find (':');
                if (split == std::string::npos)
                    continue;
                const auto generation = _state->actor_generations.find (key);
                if (generation == _state->actor_generations.end () || generation->second == 0) {
                    continue;
                }
                unit.actors.push_back (::zlink::framework::detail::actor_ref_access_t::make (
                  node_rid_t::from_string (node_rid), key.substr (0, split), key.substr (split + 1),
                  generation->second));
            }
            result.emplace_back (std::move (unit), state);
        }
        return result;
    }).get ();
    std::vector<application_relocation_unit_t> result;
    result.reserve (plans.size ());
    for (auto &[unit, state] : plans) {
        unit.ready = state->callback_lane.run ([state] {
            return state->relocation_ready_deferred;
        }).get ();
        result.push_back (std::move (unit));
    }
    return result;
}

void spot_node_runtime_t::begin_relocation_readiness ()
{
    const auto states = _state->lane.run ([&] {
        std::vector<std::shared_ptr<spot_context_state_t>> result;
        for (const auto &[_, context] : _state->spot_contexts_by_id) {
            const auto state = context._state;
            if (!state || state->is_entry_spot () || state->is_instance_spot ()
                || state->execution_mode != user_spot_execution_mode_t::spot_wide
                || state->relocation_coordination_mode
                     != spot_relocation_coordination_mode_t::application_signaled)
                continue;
            result.push_back (state);
        }
        return result;
    }).get ();
    for (const auto &state : states) {
        state->callback_lane.run ([state] {
            state->relocation_boundary_active = true;
        }).get ();
    }
}

void spot_node_runtime_t::end_relocation_readiness (const std::vector<spot_id_t> &relocated_spots)
{
    const auto contexts = _state->lane.run ([&] {
        std::vector<std::shared_ptr<spot_context_state_t>> result;
        for (const auto &[_, context] : _state->spot_contexts_by_id) {
            const auto state = context._state;
            if (!state)
                continue;
            result.push_back (state);
        }
        return result;
    }).get ();
    std::vector<std::shared_ptr<spot_context_state_t>> states;
    for (const auto &state : contexts) {
        const auto was_active = state->callback_lane.run ([state] {
                if (!state->relocation_boundary_active)
                    return false;
                state->relocation_boundary_active = false;
                return true;
            }).get ();
        if (!was_active)
            continue;
        states.push_back (state);
    }
    for (const auto &state : states) {
        const auto relocated =
          std::find (relocated_spots.begin (), relocated_spots.end (), state->spot_id)
          != relocated_spots.end ();
        state->complete_relocation_ready (relocated ? spot_relocation_ready_outcome_t::relocated
                                                    : spot_relocation_ready_outcome_t::continued);
    }
}

bool spot_node_runtime_t::complete_relocation_ready (const spot_id_t &spot_id,
                                                     spot_relocation_ready_outcome_t outcome)
{
    const auto state = _state->lane.run ([&] {
        const auto found = _state->spot_contexts_by_id.find (std::string (spot_id));
        if (found == _state->spot_contexts_by_id.end ())
            return std::shared_ptr<spot_context_state_t>{};
        return found->second._state;
    }).get ();
    if (!state)
        return false;
    state->complete_relocation_ready (outcome);
    return true;
}

std::size_t spot_node_runtime_t::active_user_spot_count () const
{
    return _state->lane.run ([&] {
        return static_cast<std::size_t> (std::count_if (
          _state->spot_contexts_by_id.begin (), _state->spot_contexts_by_id.end (),
          [&] (const auto &entry) {
              const auto &context = entry.second;
              return !context._state->closed && !context._state->native_spot.expired ()
                     && (!_state->snapshot.entry_spot_name
                         || context._state->spot_name != *_state->snapshot.entry_spot_name);
          }));
    }).get ();
}

bool spot_node_runtime_t::dispatch_mesh_record (const service::ready_record_t &owner,
                                                const service::receive_record_t &record,
                                                std::vector<zlink::message_t> &parts,
                                                service_provider_t &services,
                                                serializer_registry_t &serializers,
                                                std::function<void ()> deferred_terminal,
                                                bool *terminal_deferred,
                                                std::function<void ()> before_application_handler)
{
    if (terminal_deferred)
        *terminal_deferred = false;
    if (owner.owner_kind == service::owner_kind_t::spot
        && record.kind == service::record_kind_t::spot_multicast) {
        const auto context = find_context (spot_id_t (owner.spot_id));
        if (!context)
            return true;
        (void) dispatch_subscription (*context, record.topic, parts, services, serializers,
                                      std::move (before_application_handler),
                                      record.release_mailbox_reservation,
                                      record.transferred_owner_byte_cost);
        return true;
    }
    const bool spot_record = owner.owner_kind == service::owner_kind_t::spot
                             && (record.kind == service::record_kind_t::spot_send
                                 || record.kind == service::record_kind_t::spot_request);
    const bool node_record = owner.owner_kind == service::owner_kind_t::node
                             && (record.kind == service::record_kind_t::node_send
                                 || record.kind == service::record_kind_t::node_request);
    if (spot_record || node_record) {
        const auto route_client =
          _state->route_client_lane.run ([&] { return _state->route_client; }).get ();
        if (!route_client)
            return false;

        runtime::messaging::message_parts_t encoded (std::move (parts));
        runtime::messaging::envelope_codec_t codec;
        auto header = codec.decode_header (
          encoded, detail::message_flow_tracer_t (_state->dispatch).capture_enabled ());
        if (!header) {
            parts = std::move (encoded).take_items ();
            return false;
        }
        if (node_record && record.kind == service::record_kind_t::node_send
            && header.value ().message_name == actor_handoff_terminal_packet) {
            const auto terminal_route = handoff_terminal_route (header.value ().metadata);
            const auto success =
              handoff_u64 (header.value ().metadata, actor_handoff_terminal_success_key);
            if (!terminal_route || !success || *success > 1) {
                if (!terminal_route)
                    report_handoff_terminal_drop (_state, "missing_parking_node");
                return true;
            }
            const auto local_node_rid = _state->lane.run ([&] {
                return detail::effective_spot_node_rid (_state->snapshot);
            }).get ();
            const auto pending = _state->pending_handoff_requests_lane
                                   .run ([&] () -> std::optional<
                                     spot_node_builder_state_t::pending_handoff_request_t> {
                const auto found = _state->pending_handoff_requests.find (
                  handoff_pending_key (terminal_route->source_owner_node, terminal_route->operation,
                                       terminal_route->source_fence));
                if (found == _state->pending_handoff_requests.end ()
                    || found->second.reply_route_id != terminal_route->reply_route_id
                    || terminal_route->parking_node.to_string ()
                         != local_node_rid) {
                    return std::nullopt;
                }
                // The terminal must come from the node the parked request was
                // handed to. A fenced pending entry names the exact followed
                // edge; an entry recorded without a fence (the requester
                // attached no route) is admitted against the node identity of
                // any active follow target for the same Actor instead.
                bool terminal_source_admitted = false;
                if (found->second.source_fence.actor_id.empty ()) {
                    terminal_source_admitted =
                      _state->actor_transfer_coordinator.message_follow_targets_node (
                        actor_key (found->second.actor), record.source_node_rid);
                } else {
                    const auto target = _state->actor_transfer_coordinator.message_follow_target (
                      actor_key (found->second.actor), found->second.source_fence);
                    terminal_source_admitted =
                      target
                      && zlink::routing_id_t::from (std::string (target->route.node_rid.value ()))
                             .to_bytes ()
                           == record.source_node_rid.to_bytes ();
                }
                if (!terminal_source_admitted) {
                    return std::nullopt;
                }
                auto pending = std::move (found->second);
                _state->pending_handoff_requests.erase (found);
                return pending;
            })
                                   .get ();
            if (!pending) {
                return true;
            }
            detail::channel_reply_writer_t replies;
            runtime::messaging::message_parts_t terminal_parts;
            if (*success == 1) {
                auto body = codec.decode_body (encoded);
                if (!body)
                    return true;
                terminal_parts = replies.reply_raw_envelope (
                  replies.create_reply_header (runtime::messaging::message_kind_t::response,
                                               pending->request_header.channel_name,
                                               pending->request_header),
                  std::move (body.value ()));
            } else {
                const auto error_kind_value =
                  handoff_u64 (header.value ().metadata, actor_handoff_terminal_error_kind_key);
                const auto error_message = header.value ().metadata.find (
                  std::string (actor_handoff_terminal_error_message_key));
                const auto error_kind = error_kind_value
                                          ? static_cast<framework_error_kind_t> (*error_kind_value)
                                          : framework_error_kind_t::internal_failure;
                terminal_parts = replies.reply_raw_envelope (
                  replies.create_error_header (
                    pending->request_header.channel_name, pending->request_header,
                    framework_exception_t (error_kind,
                                           error_message == header.value ().metadata.end ()
                                             ? "Actor handoff request failed"
                                             : error_message->second)),
                  zlink::message_t{});
            }
            (void) service::reply (pending->reply_token, terminal_parts.items ());
            return true;
        }

        /* Native SPOT delivery bypasses the RouteMesh packet dispatcher after
         * the envelope has been decoded. Re-enter the wire flow here so the
         * remote Spot handler observes the same flow as the originating
         * STREAM/session request. */
        auto flow_scope = runtime::flow_context_t::enter (
          header.value ().flow_id, header.value ().flow_origin,
          detail::message_flow_tracer_t (_state->dispatch).mode (), flow_origin_t::inbound);

        auto &actor_gateway = services.get_required<actor_gateway_runtime_t> ();
        spot_route_internal_dispatcher_t dispatcher (*this, actor_gateway, *route_client,
                                                     serializers);
        if ((record.kind == service::record_kind_t::spot_send
             || record.kind == service::record_kind_t::node_send)
            && dispatcher.can_handle_send (header.value ().message_name)) {
            route_received_packet_t received{record.source_node_rid,
                                             record.operation_id.low == 0
                                               ? std::nullopt
                                               : std::make_optional (record.operation_id.low),
                                             std::move (encoded), std::nullopt};
            const auto dispatched = dispatcher.dispatch_send (received, services);
            if (!dispatched) {
                const framework_exception_t error (dispatched.error_kind (),
                                                   dispatched.error () != nullptr
                                                     ? dispatched.error ()->what ()
                                                     : "SPOT route send failed");
                report_spot_dispatch_error (
                  _state, dispatch_error_surface_t::spot_route, dispatch_message_kind_t::send,
                  dispatch_reason_from_error (error.kind ()), dispatch_error_action_t::drop,
                  header.value ().message_name, std::nullopt,
                  owner.spot_id.empty () ? std::nullopt : std::make_optional (owner.spot_id),
                  std::nullopt, std::make_exception_ptr (error),
                  record.operation_id.low == 0
                    ? std::nullopt
                    : std::make_optional (std::to_string (record.operation_id.low)));
            }
            return true;
        }
        if ((record.kind == service::record_kind_t::spot_request
             || record.kind == service::record_kind_t::node_request)
            && dispatcher.can_handle_request (header.value ().message_name)) {
            route_received_packet_t received{record.source_node_rid,
                                             record.operation_id.low == 0
                                               ? std::nullopt
                                               : std::make_optional (record.operation_id.low),
                                             std::move (encoded), std::nullopt};
            detail::channel_reply_writer_t replies;
            if (deferred_terminal) {
                const auto async_dispatched = dispatcher.dispatch_request_async (
                  received, header.value (), services,
                  [reply_token = record.reply_token, request_header = header.value (),
                   deferred_terminal =
                     std::move (deferred_terminal)] (result_t<zlink::message_t> response) mutable {
                      try {
                          detail::channel_reply_writer_t async_replies;
                          const auto reply_parts =
                            response ? async_replies.reply_raw_envelope (
                                         async_replies.create_reply_header (
                                           runtime::messaging::message_kind_t::response,
                                           request_header.channel_name, request_header),
                                         std::move (response.value ()))
                                     : async_replies.reply_raw_envelope (
                                         async_replies.create_error_header (
                                           request_header.channel_name, request_header,
                                           /* Internal route dispatcher failures are
                                     * framework-generated (zlink.origin marker). */
                                           detail::make_framework_origin_exception (
                                             response.error_kind (),
                                             response.error () ? response.error ()->what ()
                                                               : "SPOT route request failed")),
                                         zlink::message_t::from (""));
                          (void) service::reply (reply_token, reply_parts.items ());
                      }
                      catch (...) {
                      }
                      deferred_terminal ();
                  },
                  runtime::protocol::wire_operation_id_t{record.operation_id.high,
                                                         record.operation_id.low},
                  record.reply_route_id);
                if (async_dispatched) {
                    if (terminal_deferred)
                        *terminal_deferred = true;
                    return true;
                }
            }
            auto response = dispatcher.dispatch_request (received, header.value (), services);
            const auto reply_parts =
              response
                ? replies.reply_raw_envelope (
                    replies.create_reply_header (runtime::messaging::message_kind_t::response,
                                                 header.value ().channel_name, header.value ()),
                    std::move (response.value ()))
                : replies.reply_raw_envelope (
                    replies.create_error_header (
                      header.value ().channel_name, header.value (),
                      /* Internal route dispatcher failures are framework-
                       * generated (zlink.origin marker). */
                      detail::make_framework_origin_exception (
                        response.error_kind (), response.error () ? response.error ()->what ()
                                                                  : "SPOT route request failed")),
                    zlink::message_t::from (""));
            (void) service::reply (record.reply_token, reply_parts.items ());
            return true;
        }
        if (spot_record) {
            detail::channel_reply_writer_t replies;
            auto reply_error = [&] (const framework_exception_t &error) {
                if (record.kind != service::record_kind_t::spot_request) {
                    return;
                }
                auto reply = replies.reply_raw_envelope (
                  replies.create_error_header (header.value ().channel_name, header.value (),
                                               error),
                  zlink::message_t::from (""));
                (void) service::reply (record.reply_token, reply.items ());
            };
            struct spot_route_dispatch_state_snapshot_t
            {
                std::shared_ptr<spot_context_state_t> context_state;
                std::shared_ptr<void> spot_instance;
                runtime::location_lifecycle_t *location_lifecycle = nullptr;
                spot_id_t spot_id;
                std::uint64_t authority_owner_generation = 0;
            };
            const auto dispatch_snapshot = _state->lane.run ([&] {
                spot_route_dispatch_state_snapshot_t result;
                const auto context = find_context_core (spot_id_t (owner.spot_id));
                if (!context || !context->_state || !context->_state->spot_instance)
                    return result;
                result.context_state = context->_state;
                result.spot_instance = context->_state->spot_instance;
                result.location_lifecycle = context->_state->node
                                              ? context->_state->node->location_lifecycle
                                              : nullptr;
                result.spot_id = context->_state->spot_id;
                result.authority_owner_generation =
                  context->_state->authority_owner_generation;
                return result;
            }).get ();
            if (!dispatch_snapshot.context_state || !dispatch_snapshot.spot_instance) {
                reply_error (detail::make_framework_origin_exception (
                  framework_error_kind_t::unavailable, "Spot route owner is no longer registered"));
                return true;
            }
            if (record.spot_route) {
                const auto &target = *record.spot_route;
                std::optional<location_owner_token_t> owner_token;
                if (dispatch_snapshot.location_lifecycle) {
                    owner_token =
                      dispatch_snapshot.location_lifecycle->current_owner_token ();
                }
                if (target.spot_id != dispatch_snapshot.spot_id
                    || target.authority_owner_generation
                         != dispatch_snapshot.authority_owner_generation
                    || !owner_token
                    || owner_token->lease_generation != target.owner_lease_generation) {
                    reply_error (detail::make_framework_origin_exception (
                      framework_error_kind_t::unavailable, "Spot route fence is stale"));
                    return true;
                }
            }
            try {
                /* Complete route admission before creating the typed body. */
                auto body = codec.decode_body (encoded);
                if (!body) {
                    reply_error (detail::make_framework_origin_exception (
                      body.error_kind (), body.error () ? body.error ()->what ()
                                                        : "Spot request envelope body is invalid"));
                    return true;
                }
                report_spot_dispatch_trace (_state, message_flow_outcome_t::received,
                                            dispatch_error_surface_t::spot_route,
                                            record.kind == service::record_kind_t::spot_request
                                              ? dispatch_message_kind_t::request
                                              : dispatch_message_kind_t::send,
                                            header.value ().message_name, {}, owner.spot_id);
                auto handled =
                  spot_handler_registry_t (dispatch_snapshot.context_state)
                    .invoke_erased (
                      spot_handler_kind_t::packet, header.value ().message_name, {},
                      std::type_index (typeid (void)), dispatch_snapshot.spot_instance.get (),
                      nullptr, services, serializers, body.value (),
                      spot_inbound_message_t{.content_type = header.value ().content_type,
                                             .values = header.value ().metadata},
                      true, {}, {}, {}, spot_handler_registry_t::actor_queue_dispatch_t::acquire,
                      std::move (before_application_handler), {},
                      record.release_mailbox_reservation, record.transferred_owner_byte_cost)
                    .result ();
                if (!handled) {
                    /* Copy the failure as-is: registry/admission failures are
                     * already marked framework-origin at construction, while
                     * application handler failures stay unmarked, so the reply
                     * carries the zlink.origin marker only for the former. */
                    const auto *error = handled.error ();
                    reply_error (error != nullptr ? *error
                                                  : framework_exception_t (handled.error_kind (),
                                                                           "Spot handler failed"));
                    return true;
                }
                if (record.kind == service::record_kind_t::spot_request) {
                    auto reply = replies.reply_raw_envelope (
                      replies.create_reply_header (runtime::messaging::message_kind_t::response,
                                                   header.value ().channel_name, header.value ()),
                      std::move (handled.value ()));
                    (void) service::reply (record.reply_token, reply.items ());
                }
                report_spot_dispatch_trace (_state,
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
                reply_error (
                  framework_exception_t (framework_error_kind_t::internal_failure, error.what ()));
                return true;
            }
            catch (...) {
                reply_error (framework_exception_t (framework_error_kind_t::internal_failure,
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
        auto actor_type = _state->lane.run ([&] {
            std::string result;
            const auto found =
              _state->actor_types_by_id.find (std::string (owner.actor->actor_id ().value ())); 
            if (found != _state->actor_types_by_id.end ())
                result = found->second;
            return result;
        }).get ();
        if (actor_type.empty ()) {
            const auto actor_id = std::string (owner.actor->actor_id ().value ());
            const auto located = actor_type_from_authority (
              services.get_required<runtime::live_location_reader_t> (), actor_id);
            if (located && !located->empty ()) {
                actor_type = *located;
                _state->lane.run ([&] { _state->actor_types_by_id[actor_id] = actor_type; }).get ();
            }
        }
        runtime::messaging::message_parts_t encoded (std::move (parts));
        runtime::messaging::envelope_codec_t codec;
        auto header = codec.decode_header (
          encoded, detail::message_flow_tracer_t (_state->dispatch).capture_enabled ());
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
        const actor_ref_t actor = ::zlink::framework::detail::actor_ref_access_t::make (
          node_rid_t::from_string (std::string (owner.actor->node_rid ().value ())), actor_type,
          std::string (owner.actor->actor_id ().value ()), owner.actor->object_generation ());
        const auto actor_dispatch_kind = record.kind == service::record_kind_t::actor_send
                                           ? dispatch_message_kind_t::actor_send
                                           : dispatch_message_kind_t::actor_request;
        auto &actor_gateway = services.get_required<actor_gateway_runtime_t> ();
        const bool has_bound_session_source = record.source_session_rid.has_value ()
                                              || record.source_binding_generation != 0
                                              || record.source_session_sequence != 0;
        if (has_bound_session_source
            && (!record.source_session_rid || record.source_binding_generation == 0
                || record.source_session_sequence == 0)) {
            reply_error (framework_exception_t (framework_error_kind_t::protocol_error,
                                                "Bound Session relay source fence is incomplete"));
            return true;
        }
        std::optional<result_t<void>> session_relay_admission;
        bool targets_current_authority = false;
        bool targets_admitted_route = false;
        auto [native_node, relocation_authority, actor_route_admission,
              actor_message_follow_relay] = _state->lane.run ([&] {
            return std::make_tuple (_state->native_node.lock (), _state->relocation_authority,
                                    _state->actor_route_admission,
                                    _state->actor_message_follow_relay);
        }).get ();
        if (record.actor_route) {
            const auto &route = *record.actor_route;
            const auto local =
              native_node ? native_node->status () : runtime::host::node_status_t{};
            const bool targets_local_actor =
              native_node && route.actor_id == actor.actor_id ().value ()
              && route.target_node_routing_id == local.routing_id ().to_bytes ()
              && route.target_node_generation == local.lifecycle_generation ();
            const bool targets_exact_incarnation =
              targets_local_actor && route.object_generation == actor.object_generation ();
            const bool follows_committed_source =
              targets_exact_incarnation && matches_actor_message_follow_source (actor, route);
            const bool follows_in_flight_source =
              targets_exact_incarnation && actor_transfer_in_progress (actor);
            const bool requires_exact_incarnation =
              header.value ().message_name == actor_bound_session_bind_route_request_t::packet_name
              || header.value ().message_name == actor_bound_session_route_request_t::packet_name;
            if (has_bound_session_source && targets_exact_incarnation) {
                session_relay_admission.emplace (actor_gateway.admit_session_relay (
                  actor, record.source_node_rid, *record.source_session_rid,
                  record.source_binding_generation, record.source_session_sequence, &route));
            }
            const bool targets_current_bound_route =
              session_relay_admission && *session_relay_admission;
            if (!has_bound_session_source) {
                targets_current_authority = _state->lane.run ([&] {
                    const auto current = _state->actor_authority_fences.find (actor_key (actor));
                    return current != _state->actor_authority_fences.end ()
                           && current->second == route;
                }).get ();
                if (!targets_current_authority && relocation_authority) {
                    try {
                        const auto committed = relocation_authority->read (
                          runtime::stateful::object_kind_t::actor, route.actor_id);
                        targets_current_authority =
                          committed
                          && committed->target.kind == runtime::stateful::object_kind_t::actor
                          && committed->target.key == route.actor_id
                          && committed->target.object_generation == route.object_generation
                          && committed->target.node_id
                               == zlink::routing_id_t::from (route.target_node_routing_id)
                                    .to_string ()
                          && committed->target.authority_owner_generation
                               == route.authority_owner_generation
                          && committed->target_owner.lease_generation > 0
                          && static_cast<std::uint64_t> (committed->target_owner.lease_generation)
                               == route.owner_lease_generation;
                    }
                    catch (...) {
                        targets_current_authority = false;
                    }
                }
                if (!targets_current_authority && actor_route_admission)
                    targets_current_authority = actor_route_admission (route);
            }
            targets_admitted_route = targets_current_authority || targets_current_bound_route;
            const bool admitted = targets_local_actor
                                  && (!requires_exact_incarnation || targets_exact_incarnation)
                                  && (follows_committed_source || targets_current_authority
                                      || targets_current_bound_route
                                      || (follows_in_flight_source && !requires_exact_incarnation));
            report_actor_dispatch_stage_trace_lazy (
              _state, message_flow_outcome_t::received, actor_dispatch_kind,
              header.value ().message_name, owner.spot_id, actor.actor_id ().value (),
              "dispatch_mesh_record.route_admission", [&] {
                  return std::string ("accepted=") + (admitted ? "true" : "false")
                         + " local=" + (targets_local_actor ? "true" : "false")
                         + " incarnation=" + (targets_exact_incarnation ? "true" : "false")
                         + " committed_source=" + (follows_committed_source ? "true" : "false")
                         + " current_authority=" + (targets_current_authority ? "true" : "false")
                         + " current_bound_route="
                         + (targets_current_bound_route ? "true" : "false")
                         + " in_flight_source=" + (follows_in_flight_source ? "true" : "false");
              });
            if (!admitted) {
                const bool exact_generation_mismatch =
                  requires_exact_incarnation && targets_local_actor && !targets_exact_incarnation;
                reply_error (framework_exception_t (
                  exact_generation_mismatch ? framework_error_kind_t::invalid_operation
                                            : framework_error_kind_t::unavailable,
                  exact_generation_mismatch
                    ? "Bound session route Actor generation does not match"
                    : "Actor route owner identity is stale or not admitted"));
                return true;
            }
        } else {
            report_actor_dispatch_stage_trace (
              _state, message_flow_outcome_t::received, actor_dispatch_kind,
              header.value ().message_name, owner.spot_id, actor.actor_id ().value (),
              "dispatch_mesh_record.route_admission", "required=false");
        }
        if (has_bound_session_source) {
            if (!session_relay_admission) {
                session_relay_admission.emplace (actor_gateway.admit_session_relay (
                  actor, record.source_node_rid, *record.source_session_rid,
                  record.source_binding_generation, record.source_session_sequence));
            }
            report_actor_dispatch_stage_trace_lazy (
              _state, message_flow_outcome_t::received, actor_dispatch_kind,
              header.value ().message_name, owner.spot_id, actor.actor_id ().value (),
              "dispatch_mesh_record.session_admission", [&] {
                  return std::string ("accepted=") + (*session_relay_admission ? "true" : "false");
              });
            if (!*session_relay_admission) {
                reply_error (
                  framework_exception_t (session_relay_admission->error_kind (),
                                         session_relay_admission->error ()
                                           ? session_relay_admission->error ()->what ()
                                           : "Bound Session relay source fence was rejected"));
                return true;
            }
        } else {
            report_actor_dispatch_stage_trace (
              _state, message_flow_outcome_t::received, actor_dispatch_kind,
              header.value ().message_name, owner.spot_id, actor.actor_id ().value (),
              "dispatch_mesh_record.session_admission", "required=false");
        }
        /* Route admission is complete before creating the typed body. This
         * keeps stale authority, owner, and node generations out of the
         * application deserializer and handler path. */
        auto body = codec.decode_body (encoded);
        if (!body) {
            reply_error (framework_exception_t (framework_error_kind_t::protocol_error,
                                                "Actor request envelope body is invalid"));
            return true;
        }
        if (header.value ().message_name == actor_bound_session_bind_route_request_t::packet_name) {
            const auto route_client =
              _state->route_client_lane.run ([&] { return _state->route_client; }).get ();
            if (!route_client) {
                reply_error (framework_exception_t (
                  framework_error_kind_t::unavailable,
                  "Remote Actor session binding requires a RouteMesh client"));
                return true;
            }
            spot_route_internal_dispatcher_t dispatcher (*this, actor_gateway, *route_client,
                                                         serializers);
            route_received_packet_t received{record.source_node_rid,
                                             record.operation_id.low == 0
                                               ? std::nullopt
                                               : std::make_optional (record.operation_id.low),
                                             std::move (encoded), std::nullopt};
            auto response = dispatcher.dispatch_request (received, header.value (), services);
            const auto reply_parts =
              response
                ? replies.reply_raw_envelope (
                    replies.create_reply_header (runtime::messaging::message_kind_t::response,
                                                 header.value ().channel_name, header.value ()),
                    std::move (response.value ()))
                : replies.reply_raw_envelope (
                    replies.create_error_header (
                      header.value ().channel_name, header.value (),
                      framework_exception_t (response.error_kind (),
                                             response.error ()
                                               ? response.error ()->what ()
                                               : "Remote Actor session binding failed")),
                    zlink::message_t::from (""));
            (void) service::reply (record.reply_token, reply_parts.items ());
            return true;
        }
        auto actor_context = actor_gateway.actor_context (actor, record.source_binding_generation);
        const bool semantic_send =
          header.value ().kind == runtime::messaging::message_kind_t::command;
        const auto handoff_operation =
          runtime::protocol::wire_operation_id_t{record.operation_id.high, record.operation_id.low};
        const auto handoff_source_fence =
          record.actor_route.value_or (runtime::protocol::actor_route_fence_t{});
        const auto handoff_pending =
          handoff_pending_key (record.source_node_rid, handoff_operation, handoff_source_fence);
        bool deferred_handoff_request = false;
        /* A request that carries an operation id travels with a handoff
         * terminal route naming THIS node as the parking node (see the relay
         * metadata below), so its reply may return as a handoff terminal
         * whenever any downstream hop parks it — during the local transfer,
         * or on the follow target while its join is still completing. The
         * pending entry that restores the original reply token must exist
         * under exactly that stamping condition: gating it on the local
         * transfer state orphaned the terminal of a request that this node
         * relayed immediately but the target parked (the reply was replaced
         * by an empty one). An entry whose reply arrives inline instead is
         * erased by the relay observer below; an untouched entry expires at
         * its 30s deadline. A request without an attached route fence (e.g.
         * a local requester on the current owner) parks and replays exactly
         * like a fenced one; requiring the fence here silently orphaned its
         * terminal, leaving the requester waiting forever. */
        if (record.kind == service::record_kind_t::actor_request && !semantic_send
            && (record.operation_id.high != 0 || record.operation_id.low != 0)
            && !header.value ().metadata.contains (
              std::string (detail::actor_handoff_source_node_key))) {
            const auto now = std::chrono::steady_clock::now ();
            _state->pending_handoff_requests_lane
              .run ([&] {
                  for (auto pending = _state->pending_handoff_requests.begin ();
                       pending != _state->pending_handoff_requests.end ();) {
                      if (pending->second.deadline <= now)
                          pending = _state->pending_handoff_requests.erase (pending);
                      else
                          ++pending;
                  }
                  if (_state->pending_handoff_requests.size () < 1024) {
                      const auto [_, inserted] = _state->pending_handoff_requests.emplace (
                        handoff_pending,
                        spot_node_builder_state_t::pending_handoff_request_t{
                          actor, handoff_source_fence, record.reply_route_id, record.reply_token,
                          header.value (), now + std::chrono::seconds (30)});
                      deferred_handoff_request = inserted;
                  }
              })
              .get ();
        }
        const auto request_header = header.value ();
        auto terminal_claimed = std::make_shared<std::atomic_bool> (false);
        auto terminal_owner =
          std::make_shared<std::function<void ()>> (std::move (deferred_terminal));
        std::function<void ()> after_application_admission;
        if (record.kind == service::record_kind_t::actor_send && *terminal_owner) {
            // Spec 05 section 4.3 defines one-way acceptance at the target
            // mailbox/relay queue. Do not retain the source Session ingress
            // until application-handler completion: relocation sealing may be
            // queued behind that same handler.
            after_application_admission = [request_header, reply_token = record.reply_token,
                                           terminal_claimed, terminal_owner] {
                if (terminal_claimed->exchange (true, std::memory_order_acq_rel))
                    return;
                try {
                    detail::channel_reply_writer_t writer;
                    const auto reply = writer.reply_raw_envelope (
                      writer.create_reply_header (runtime::messaging::message_kind_t::response,
                                                  request_header.channel_name, request_header),
                      zlink::message_t{});
                    (void) service::reply (reply_token, reply.items ());
                }
                catch (...) {
                }
                (*terminal_owner) ();
            };
        }
        const auto relay_parking_node_rid = _state->lane.run ([&] {
            return detail::effective_spot_node_rid (_state->snapshot);
        }).get ();
        auto relayed = [&] {
            const bool actor_packet = record.kind == service::record_kind_t::actor_request
                                      || record.kind == service::record_kind_t::actor_send;
            /* Once this node has become the committed source of a Message
             * Follow route, that exact retained fence is newer local knowledge
             * than a positive Location-cache result for the former owner.
             * Relay it before ordinary current-authority dispatch so the
             * original operation and reply route remain attached. */
            const auto follows_committed_source =
              actor_packet && record.actor_route
              && matches_actor_message_follow_source (actor, *record.actor_route);
            const auto follows_in_flight_source =
              actor_packet && actor_transfer_in_progress (actor);
            if (follows_committed_source && !follows_in_flight_source
                && actor_message_follow_relay) {
                //  The wire record kind is the authoritative send/request
                //  semantic; the envelope header may carry a request kind
                //  for a one-way relay. Stamp the relay-kind marker so the
                //  follow target dispatches the handler registered for the
                //  actual semantic (actor_send vs actor_request).
                auto follow_header = header.value ();
                if (record.kind == service::record_kind_t::actor_send) {
                    follow_header.metadata.insert_or_assign ("__zlink.actorRelayKind", "send");
                }
                /* The follow target can still park this request while its own
                 * join is completing; the parked replay answers through the
                 * handoff terminal route, so the terminal keys must travel on
                 * this immediate relay exactly like on the relay_actor_packet
                 * branch below — the pending entry recorded above waits for
                 * them on this node. */
                if (record.kind == service::record_kind_t::actor_request
                    && (record.operation_id.high != 0 || record.operation_id.low != 0)) {
                    follow_header.metadata.insert_or_assign (
                      std::string (detail::actor_handoff_source_node_key),
                      record.source_node_rid.to_hex ());
                    follow_header.metadata.insert_or_assign (
                      std::string (detail::actor_handoff_parking_node_key),
                      zlink::routing_id_t::from (relay_parking_node_rid).to_hex ());
                    follow_header.metadata.insert_or_assign (
                      std::string (detail::actor_handoff_operation_high_key),
                      std::to_string (record.operation_id.high));
                    follow_header.metadata.insert_or_assign (
                      std::string (detail::actor_handoff_operation_low_key),
                      std::to_string (record.operation_id.low));
                    follow_header.metadata.insert_or_assign (
                      std::string (detail::actor_handoff_reply_route_key),
                      std::to_string (record.reply_route_id));
                    if (record.actor_route) {
                        const auto &route = *record.actor_route;
                        follow_header.metadata.insert_or_assign (
                          std::string (detail::actor_handoff_route_actor_id_key), route.actor_id);
                        follow_header.metadata.insert_or_assign (
                          std::string (detail::actor_handoff_route_object_generation_key),
                          std::to_string (route.object_generation));
                        follow_header.metadata.insert_or_assign (
                          std::string (detail::actor_handoff_route_target_node_key),
                          zlink::routing_id_t::from (route.target_node_routing_id).to_hex ());
                        follow_header.metadata.insert_or_assign (
                          std::string (detail::actor_handoff_route_target_node_generation_key),
                          std::to_string (route.target_node_generation));
                        follow_header.metadata.insert_or_assign (
                          std::string (detail::actor_handoff_route_authority_generation_key),
                          std::to_string (route.authority_owner_generation));
                        follow_header.metadata.insert_or_assign (
                          std::string (detail::actor_handoff_route_lease_generation_key),
                          std::to_string (route.owner_lease_generation));
                    }
                }
                return actor_message_follow_relay (
                  actor, follow_header, body.value (), std::chrono::seconds (30),
                  record.source_node_rid,
                  record.actor_route.value_or (runtime::protocol::actor_route_fence_t{}),
                  record.message_follow_hop_count,
                  runtime::protocol::wire_operation_id_t{record.operation_id.high,
                                                         record.operation_id.low},
                  record.reply_route_id);
            }
            auto relay_metadata = spot_inbound_message_t{
              .content_type = header.value ().content_type, .values = header.value ().metadata};
            if (record.kind == service::record_kind_t::actor_request
                && !header.value ().correlation_id.empty ()) {
                // The correlation is the request's stable id across a stale-route
                // retry and handoff replay. Keep it in the internal metadata so
                // the target exactly-once table sees both delivery paths as the
                // same request.
                relay_metadata.values.insert_or_assign ("__zlink.actorRequestId",
                                                        header.value ().correlation_id);
            }
            if (record.kind == service::record_kind_t::actor_request
                && (record.operation_id.high != 0 || record.operation_id.low != 0)) {
                relay_metadata.values[std::string (detail::actor_handoff_source_node_key)] =
                  record.source_node_rid.to_hex ();
                relay_metadata.values[std::string (detail::actor_handoff_parking_node_key)] =
                  zlink::routing_id_t::from (relay_parking_node_rid).to_hex ();
                relay_metadata.values[std::string (detail::actor_handoff_operation_high_key)] =
                  std::to_string (record.operation_id.high);
                relay_metadata.values[std::string (detail::actor_handoff_operation_low_key)] =
                  std::to_string (record.operation_id.low);
                relay_metadata.values[std::string (detail::actor_handoff_reply_route_key)] =
                  std::to_string (record.reply_route_id);
            }
            if (record.actor_route) {
                const auto &route = *record.actor_route;
                relay_metadata.values[std::string (detail::actor_handoff_route_actor_id_key)] =
                  route.actor_id;
                relay_metadata
                  .values[std::string (detail::actor_handoff_route_object_generation_key)] =
                  std::to_string (route.object_generation);
                relay_metadata.values[std::string (detail::actor_handoff_route_target_node_key)] =
                  zlink::routing_id_t::from (route.target_node_routing_id).to_hex ();
                relay_metadata
                  .values[std::string (detail::actor_handoff_route_target_node_generation_key)] =
                  std::to_string (route.target_node_generation);
                relay_metadata
                  .values[std::string (detail::actor_handoff_route_authority_generation_key)] =
                  std::to_string (route.authority_owner_generation);
                relay_metadata
                  .values[std::string (detail::actor_handoff_route_lease_generation_key)] =
                  std::to_string (route.owner_lease_generation);
                relay_metadata.values[std::string (detail::actor_handoff_hop_count_key)] =
                  std::to_string (record.message_follow_hop_count);
            }
            //  The wire record kind is authoritative for the send/request
            //  semantic; a bound-session one-way relay can carry a request
            //  envelope header while its record kind is actor_send.
            return relay_actor_packet (
              actor, std::move (actor_context),
              semantic_send || record.kind == service::record_kind_t::actor_send
                ? stream_message_kind_t::send
                : stream_message_kind_t::request,
              header.value ().message_name, body.value (), services, serializers,
              std::move (relay_metadata),
              targets_admitted_route && record.actor_route ? &*record.actor_route : nullptr,
              std::move (before_application_handler), std::move (after_application_admission),
              zlink::routing_id_t::from (std::uint32_t{0}), {}, 0, std::nullopt,
              record.release_mailbox_reservation,
              record.transferred_owner_byte_cost);
        }();
        if (!*terminal_owner && record.kind != service::record_kind_t::actor_request)
            return false;
        detail::observe_task_completion (
          relayed, [state = _state, deferred_handoff_request, handoff_pending, request_header,
                    reply_token = record.reply_token, terminal_claimed, terminal_owner] (
                     const result_t<std::optional<zlink::message_t>> &result) mutable {
              /* An empty relay result with a recorded pending entry means the
               * request was parked for a handoff replay: the reply token stays
               * in pending_handoff_requests and the handoff terminal from the
               * follow target restores the reply. Replying (or erasing the
               * entry) here would orphan that terminal. Any other outcome —
               * an immediate relayed reply or a relay failure — is terminal
               * right now, so the entry is settled and the requester answered
               * directly; without this, a cross-node request relayed through
               * the committed Message Follow source between the relocation
               * commit and the source cleanup lost its reply forever (the
               * requester's wire operation simply timed out). */
              const bool parked = deferred_handoff_request && result && !result.value ();
              if (!parked && deferred_handoff_request) {
                  try {
                      state->pending_handoff_requests_lane
                        .run ([&] { state->pending_handoff_requests.erase (handoff_pending); })
                        .get ();
                  }
                  catch (...) {
                  }
              }
              if (terminal_claimed->exchange (true, std::memory_order_acq_rel))
                  return;
              if (!parked) {
                  try {
                      detail::channel_reply_writer_t writer;
                      const auto reply =
                        result
                          ? writer.reply_raw_envelope (
                              writer.create_reply_header (
                                runtime::messaging::message_kind_t::response,
                                request_header.channel_name, request_header),
                              result.value () ? *result.value () : zlink::message_t{})
                          : writer.reply_raw_envelope (
                              writer.create_error_header (
                                request_header.channel_name, request_header,
                                framework_exception_t (result.error_kind (),
                                                       result.error () ? result.error ()->what ()
                                                                       : "Actor handler failed")),
                              zlink::message_t{});
                      (void) service::reply (reply_token, reply.items ());
                  }
                  catch (...) {
                  }
              }
              if (*terminal_owner)
                  (*terminal_owner) ();
          });
        if (terminal_deferred)
            *terminal_deferred = true;
        return true;
    }
    if (owner.owner_kind != service::owner_kind_t::spot
        || record.kind != service::record_kind_t::spot_control || !record.actor_control) {
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

    struct actor_join_dispatch_state_snapshot_t
    {
        std::string actor_type;
        bool targets_entry_spot = false;
        std::string local_node_rid;
    };
    auto join_dispatch_snapshot = _state->lane.run ([&] {
        actor_join_dispatch_state_snapshot_t result;
        const auto found =
          _state->actor_types_by_id.find (std::string (control.current_actor.actor_id ().value ())); 
        if (found != _state->actor_types_by_id.end ())
            result.actor_type = found->second;
        if (_state->snapshot.entry_spot_name) {
            const auto entry = _state->spot_ids_by_name.find (*_state->snapshot.entry_spot_name);
            result.targets_entry_spot =
              entry != _state->spot_ids_by_name.end () && entry->second == owner.spot_id;
        }
        result.local_node_rid = detail::effective_spot_node_rid (_state->snapshot);
        return result;
    }).get ();
    auto &actor_type = join_dispatch_snapshot.actor_type;
    if (actor_type.empty ()) {
        const auto actor_id = std::string (control.current_actor.actor_id ().value ());
        const auto located = actor_type_from_authority (
          services.get_required<runtime::live_location_reader_t> (), actor_id);
        if (located && !located->empty ()) {
            actor_type = *located;
            _state->lane.run ([&] { _state->actor_types_by_id[actor_id] = actor_type; }).get ();
        }
    }
    if (actor_type.empty ()) {
        (void) service::actor_join_reply (record.reply_token,
                                          service::actor_join_result_t::rejected, {});
        return true;
    }

    const actor_ref_t actor = ::zlink::framework::detail::actor_ref_access_t::make (
      node_rid_t::from_string (std::string (control.current_actor.node_rid ().value ())),
      actor_type, std::string (control.current_actor.actor_id ().value ()),
      control.current_actor.object_generation ());
    const zlink::message_t request = parts.empty () ? zlink::message_t{} : parts.front ();
    const auto targets_entry_spot = join_dispatch_snapshot.targets_entry_spot;
    const auto &local_node_rid = join_dispatch_snapshot.local_node_rid;
    auto joined = [&] () -> result_t<actor_join_reply_t> {
        if (parts.size () >= 10 && owner.spot_id != local_node_rid) {
            const auto target_spot = spot_id_t (owner.spot_id);
            const auto transfer_id = parts[2].to_string ();
            auto admitted = admit_remote_actor_to_spot (
              transfer_id, actor, spot_id_t (parts[3].to_string ()), target_spot, request);
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
                  framework_error_kind_t::internal_failure, "target Core MeshNode is unavailable");
            }
            service::actor_transfer_prepare_t transfer_prepare{
              .role = service::actor_transfer_role_t::target,
              .transfer_id = transfer_id,
              .actor = control.current_actor,
              .source_spot_id = parts[3].to_string (),
              .target_spot_id = owner.spot_id,
              .target_node_rid = native->status ().routing_id ()};
            service::actor_transfer_token_t transfer_token;
            service::actor_transfer_prepare_result_t transfer_result{actor, 0};
            if (!native->prepare_actor_transfer (transfer_prepare, transfer_token,
                                                 transfer_result)) {
                return result_t<actor_join_reply_t>::failure (
                  framework_error_kind_t::internal_failure,
                  "target Framework Actor relocation prepare failed");
            }
            _state->lane.run ([&] {
                // Framework target prepare installed the transferred Actor with the
                // source generation. Application materialization must reuse it.
                _state->mesh_runtime_owned_native_actor_ids.insert (
                  std::string (actor.actor_id ().value ()));
            }).get ();
            auto committed = commit_remote_actor_to_spot (
              transfer_id, actor, target_spot, parts[1],
              services.get_required<actor_gateway_runtime_t> ().actor_context (actor), {},
              &services);
            if (!committed) {
                _state->lane.run ([&] {
                    _state->mesh_runtime_owned_native_actor_ids.erase (
                      std::string (actor.actor_id ().value ()));
                }).get ();
                return committed;
            }
            const auto new_membership_epoch = transfer_result.membership_epoch + 1;
            if (!transfer_token.commit (new_membership_epoch) || !transfer_token.activate ()) {
                return result_t<actor_join_reply_t>::failure (
                  framework_error_kind_t::internal_failure,
                  "target Framework Actor relocation activation failed");
            }
            if (actor_transfer_marker_enabled ()) {
                emit_actor_transfer_marker ("location_committed", committed.value ().actor,
                                            transfer_id, target_spot);
            }
            return result_t<actor_join_reply_t>::success (actor_join_reply_t{
              committed.value ().result_code, committed.value ().actor, admission_reply});
        }
        return targets_entry_spot
                 ? [&] {
                       auto &actor_gateway = services.get_required<actor_gateway_runtime_t> ();
                       auto actor_context = actor_gateway.actor_context (actor);
                       if (actor_context.serializer_registry () == nullptr) {
                           return result_t<actor_join_reply_t>::failure (
                             framework_error_kind_t::protocol_error,
                             "Actor gateway Context has no serializer registry");
                       }
                       return join_actor_to_entry_spot_erased (
                         actor, node_rid_t::from_string (local_node_rid),
                         request, std::nullopt, std::move (actor_context));
                   }()
                 : join_actor_to_spot_erased (actor, spot_id_t (owner.spot_id), request);
    }();
    if (!joined) {
        (void) service::actor_join_reply (record.reply_token,
                                          service::actor_join_result_t::rejected, {});
        return true;
    }
    const std::vector<zlink::message_t> reply_parts =
      joined.value ().reply.to_string ().empty ()
        ? std::vector<zlink::message_t>{}
        : std::vector<zlink::message_t>{joined.value ().reply};
    (void) service::actor_join_reply (record.reply_token,
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
                                            serializer_registry_t &serializers,
                                            std::function<void ()> before_application_handler,
                                            std::function<void ()> transfer_owner_reservation,
                                            std::size_t transferred_owner_byte_cost) const
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
    /* flow-correlation §4: at Off the publisher's flow pair is neither
     * validated nor materialized from the fan-out envelope. */
    const bool capture_flow = detail::message_flow_tracer_t (_state->dispatch).capture_enabled ();
    zlink::message_t body = parts.front ();
    std::optional<std::string> flow_id;
    std::optional<flow_origin_t> flow_origin;
    std::optional<std::string> packet_name;
    std::string content_type = runtime::messaging::envelope_codec_t::default_content_type;
    bool report_decode_failure = false;
    if (parts.size () >= 2) {
        const runtime::messaging::message_parts_t envelope_parts{std::vector (parts)};
        auto header = codec.decode_header (envelope_parts, capture_flow);
        auto decoded_body = codec.decode_body (envelope_parts);
        if (header && decoded_body) {
            body = decoded_body.value ();
            flow_id = header.value ().flow_id;
            flow_origin = header.value ().flow_origin;
            content_type = header.value ().content_type;
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
                auto header = codec.decode_header (
                  zlink::message_t::from (std::vector<std::uint8_t> (
                    bytes.begin () + 8,
                    bytes.begin () + 8 + static_cast<std::ptrdiff_t> (header_size))),
                  capture_flow);
                if (header) {
                    body = zlink::message_t::from (std::vector<std::uint8_t> (
                      bytes.begin () + 8 + static_cast<std::ptrdiff_t> (header_size),
                      bytes.end ()));
                    flow_id = header.value ().flow_id;
                    flow_origin = header.value ().flow_origin;
                    content_type = header.value ().content_type;
                    if (!header.value ().message_name.empty ()) {
                        packet_name = header.value ().message_name;
                    }
                    report_decode_failure = false;
                }
            }
        }
    }
    if (report_decode_failure) {
        return result_t<void>::success ();
    }
    const auto diagnostics_mode = detail::message_flow_tracer_t (_state->dispatch).mode ();
    auto flow_scope = runtime::flow_context_t::enter (std::move (flow_id), flow_origin,
                                                      diagnostics_mode, flow_origin_t::inbound);
    const auto &message = body;
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
          _state, dispatch_error_surface_t::spot_route, dispatch_message_kind_t::send,
          dispatch_error_reason_t::handler_missing, dispatch_error_action_t::drop, packet_name,
          topic, std::string (context._state->spot_id));
        return result_t<void>::success ();
    }
    auto result =
      spot_handler_registry_t (context._state)
        .invoke_erased (spot_handler_kind_t::subscription, *packet_name, topic,
                        std::type_index (typeid (void)), context._state->spot_instance.get (),
                        nullptr, services, serializers, message,
                        spot_inbound_message_t{.content_type = std::move (content_type)}, true, {},
                        {}, {}, spot_handler_registry_t::actor_queue_dispatch_t::acquire,
                        std::move (before_application_handler), {},
                        std::move (transfer_owner_reservation), transferred_owner_byte_cost)
        .result ();
    if (!result) {
        const auto *error = result.error ();
        const framework_exception_t exception (
          result.error_kind (), error != nullptr ? error->what () : "spot subscription failed");
        return detail::result_access_t::failure<void> (exception);
    }
    return result_t<void>::success ();
}

result_t<std::size_t>
spot_node_runtime_t::dispatch_multicast (std::string topic,
                                         const std::vector<zlink::message_t> &parts,
                                         service_provider_t &services,
                                         serializer_registry_t &serializers) const
{
    std::size_t dispatched = 0;
    for (const auto &context : active_contexts ()) {
        const auto subscribed = std::any_of (
          context._state->handlers.begin (), context._state->handlers.end (),
          [&topic] (const spot_handler_descriptor_t &handler) {
              return handler.kind == spot_handler_kind_t::subscription && handler.topic == topic;
          });
        if (!subscribed)
            continue;
        auto delivered = dispatch_subscription (context, topic, parts, services, serializers);
        if (!delivered) {
            return detail::propagate_failure<std::size_t> (delivered,
                                                           "spot multicast dispatch failed");
        }
        ++dispatched;
    }
    return result_t<std::size_t>::success (dispatched);
}


} // namespace zlink::framework::detail
