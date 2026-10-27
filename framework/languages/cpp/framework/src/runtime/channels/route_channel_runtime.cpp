/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

#include "runtime/channels/route_channel_runtime.hpp"

#include "runtime/transport/endpoint_notation.hpp"
#include "runtime/messaging/request_deadline.hpp"

#include <algorithm>
#include <utility>

namespace zlink::framework::detail
{

namespace
{
std::chrono::milliseconds remaining_request_timeout (std::chrono::steady_clock::time_point deadline)
{
    return std::max (
      std::chrono::milliseconds::zero (),
      std::chrono::ceil<std::chrono::milliseconds> (deadline - std::chrono::steady_clock::now ()));
}
} // namespace

struct route_channel_runtime_t::state_t
{
    std::string _router_channel_id;
    runtime::offload_executor_t _lane_executor;
    mutable runtime::state_lane_t _lane{_lane_executor};
    std::optional<zlink::routing_id_t> _routing_id;
    std::chrono::milliseconds _default_request_timeout{
      runtime::configuration::default_request_timeout};
    std::string _bind_endpoint;
    std::vector<std::string> _manual_connections;
    bool _running = false;
    route_connection_set_t _connections;
    std::set<std::string> _ready_peer_rids;
    channel_pending_requests_t _pending_requests;
    std::vector<route_outbound_packet_t> _outbound_packets;
    send_backend_t _send_backend;
    request_backend_t _request_backend;
    std::mutex _send_backend_mutex;
    std::shared_ptr<task_completion_source_t<void>> changed =
      std::make_shared<task_completion_source_t<void>> ();
};

route_channel_runtime_t::route_channel_runtime_t (std::string router_channel_id) :
    _state (std::make_shared<state_t> ())
{
    _state->_router_channel_id = std::move (router_channel_id);
}

route_channel_runtime_t::~route_channel_runtime_t ()
{
    stop ();
}

void route_channel_runtime_t::notify_state_changed ()
{
    auto previous = _state->_lane.run_sync ([&] {
        return std::exchange (_state->changed, std::make_shared<task_completion_source_t<void>> ());
    });
    previous->complete (result_t<void>::success ());
}

const std::string &route_channel_runtime_t::router_channel_id () const noexcept
{
    return _state->_router_channel_id;
}

void route_channel_runtime_t::routing_id (zlink::routing_id_t routing_id)
{
    _state->_lane.run ([&, this] { _state->_routing_id = std::move (routing_id); }).get ();
    notify_state_changed ();
}

std::optional<zlink::routing_id_t> route_channel_runtime_t::routing_id () const
{
    return _state->_lane.run_sync ([&, this] { return _state->_routing_id; });
}

void route_channel_runtime_t::default_request_timeout (std::chrono::milliseconds timeout)
{
    return _state->_lane.run ([&, this] { _state->_default_request_timeout = timeout; }).get ();
}

std::chrono::milliseconds route_channel_runtime_t::default_request_timeout () const
{
    return _state->_lane.run_sync ([&, this] { return _state->_default_request_timeout; });
}

void route_channel_runtime_t::start ()
{
    _state->_lane.run ([&, this] { _state->_running = true; }).get ();
    notify_state_changed ();
}

void route_channel_runtime_t::stop ()
{
    _state->_lane
      .run ([&, this] {
          _state->_running = false;
          _state->_pending_requests.clear ();
          _state->_send_backend = {};
          _state->_request_backend = {};
      })
      .get ();
    notify_state_changed ();
}

bool route_channel_runtime_t::running () const
{
    return _state->_lane.run_sync ([&, this] { return _state->_running; });
}

bool route_channel_runtime_t::connect (std::string endpoint)
{
    auto result =
      _state->_lane.run ([&, this] { return _state->_connections.connect (std::move (endpoint)); })
        .get ();
    notify_state_changed ();
    return result;
}

bool route_channel_runtime_t::connect (zlink::routing_id_t peer_rid, std::string endpoint)
{
    auto result =
      _state->_lane
        .run ([&, this] {
            return _state->_connections.connect (std::move (peer_rid), std::move (endpoint));
        })
        .get ();
    notify_state_changed ();
    return result;
}

bool route_channel_runtime_t::disconnect (const std::string &endpoint)
{
    const auto normalized_endpoint = runtime::transport::normalize_endpoint (endpoint);
    auto result = _state->_lane
                    .run ([&, this] {
                        const auto targets = _state->_connections.targets ();
                        const bool removed = _state->_connections.disconnect (normalized_endpoint);
                        if (removed) {
                            for (const auto &target : targets) {
                                if (target.endpoint == normalized_endpoint && target.peer_rid) {
                                    _state->_ready_peer_rids.erase (target.peer_rid->to_string ());
                                }
                            }
                        }
                        return removed;
                    })
                    .get ();
    notify_state_changed ();
    return result;
}

bool route_channel_runtime_t::disconnect (const zlink::routing_id_t &peer_rid,
                                          const std::string &endpoint)
{
    const auto normalized_endpoint = runtime::transport::normalize_endpoint (endpoint);
    auto result = _state->_lane
                    .run ([&, this] {
                        const bool removed =
                          _state->_connections.disconnect (peer_rid, normalized_endpoint);
                        if (removed)
                            _state->_ready_peer_rids.erase (peer_rid.to_string ());
                        return removed;
                    })
                    .get ();
    notify_state_changed ();
    return result;
}

std::vector<std::string> route_channel_runtime_t::list_connections () const
{
    return _state->_lane.run ([&, this] { return _state->_connections.list (); }).get ();
}

std::vector<route_connection_set_t::target_t>
route_channel_runtime_t::list_connection_targets () const
{
    return _state->_lane.run ([&, this] { return _state->_connections.targets (); }).get ();
}

void route_channel_runtime_t::mark_peer_ready (const zlink::routing_id_t &peer_rid)
{
    _state->_lane.run ([&, this] { _state->_ready_peer_rids.insert (peer_rid.to_string ()); })
      .get ();
    notify_state_changed ();
}

void route_channel_runtime_t::mark_peer_disconnected (const zlink::routing_id_t &peer_rid)
{
    _state->_lane.run ([&, this] { _state->_ready_peer_rids.erase (peer_rid.to_string ()); })
      .get ();
    notify_state_changed ();
}

void route_channel_runtime_t::bind_endpoint (std::string endpoint)
{
    _state->_lane
      .run (
        [&, this] { _state->_bind_endpoint = runtime::transport::normalize_endpoint (endpoint); })
      .get ();
    notify_state_changed ();
}

std::string route_channel_runtime_t::bind_endpoint () const
{
    return _state->_lane.run ([&, this] { return _state->_bind_endpoint; }).get ();
}

void route_channel_runtime_t::manual_connections (std::vector<std::string> endpoints)
{
    return _state->_lane
      .run ([&, this] {
          _state->_manual_connections.clear ();
          _state->_manual_connections.reserve (endpoints.size ());
          for (auto &endpoint : endpoints) {
              _state->_manual_connections.push_back (
                runtime::transport::normalize_endpoint (endpoint));
          }
      })
      .get ();
}

std::vector<std::string> route_channel_runtime_t::manual_connections () const
{
    return _state->_lane.run ([&, this] { return _state->_manual_connections; }).get ();
}

result_t<void>
route_channel_runtime_t::submit_send_parts (const zlink::routing_id_t &target_node_rid,
                                            runtime::messaging::message_parts_t parts)
{
    send_backend_t backend;
    std::optional<zlink::routing_id_t> backend_target;
    std::optional<std::string> backend_spot_target;
    runtime::messaging::message_parts_t backend_parts;
    if (auto connected = wait_until_connected (std::chrono::milliseconds::zero ()); !connected) {
        return connected;
    }
    if (auto ready = wait_until_peer_ready (target_node_rid, std::chrono::milliseconds::zero ());
        !ready) {
        return ready;
    }
    auto prepared = _state->_lane
                      .run ([&, this] {
                          if (auto connected = ensure_connected (_state); !connected) {
                              return connected;
                          }
                          auto &packet = append_outbound_unlocked (
                            _state, target_node_rid, std::nullopt, std::move (parts), std::nullopt);
                          if (_state->_send_backend) {
                              backend = _state->_send_backend;
                              backend_target = packet.target_node_rid;
                              backend_spot_target = packet.target_spot_id;
                              backend_parts = packet.parts;
                          }
                          return result_t<void>::success ();
                      })
                      .get ();
    if (!prepared)
        return prepared;
    if (backend) {
        std::lock_guard send_lock (_state->_send_backend_mutex);
        return backend (*backend_target, backend_spot_target, backend_parts);
    }
    return result_t<void>::success ();
}

result_t<std::uint64_t>
route_channel_runtime_t::submit_request_parts (const zlink::routing_id_t &target_node_rid,
                                               runtime::messaging::message_parts_t parts)
{
    return _state->_lane.run_sync ([&, this] {
        if (auto connected = ensure_connected (_state); !connected) {
            return detail::propagate_failure<std::uint64_t> (connected,
                                                             "route channel is not connected");
        }
        return register_request_unlocked (target_node_rid, std::nullopt, std::move (parts));
    });
}

request_task_t<result_t<runtime::messaging::message_parts_t>>
route_channel_runtime_t::request_reply_parts (const zlink::routing_id_t &target_node_rid,
                                              runtime::messaging::message_parts_t parts,
                                              std::chrono::milliseconds timeout)
{
    using reply_t = result_t<runtime::messaging::message_parts_t>;
    if (auto ready = wait_until_peer_ready (target_node_rid, std::chrono::milliseconds::zero ());
        !ready) {
        return result_t<reply_t>::success (
          detail::propagate_failure<runtime::messaging::message_parts_t> (
            ready, "route channel peer is not ready"));
    }
    auto pending = request_backend_parts (_state, target_node_rid, std::nullopt, std::move (parts),
                                          std::chrono::steady_clock::now () + timeout);
    return detail::project_request_reply<reply_t> (
      pending, [] (const reply_t &terminal) { return result_t<reply_t>::success (terminal); });
}

request_task_t<runtime::messaging::message_parts_t>
route_channel_runtime_t::request_backend_parts (const std::shared_ptr<state_t> &_state,
                                                const zlink::routing_id_t &target_node_rid,
                                                std::optional<std::string> target_spot_id,
                                                runtime::messaging::message_parts_t parts,
                                                std::chrono::steady_clock::time_point deadline)
{
    // The selected backend may borrow its arguments through the physical terminal.
    auto packet = std::make_shared<route_outbound_packet_t> (route_outbound_packet_t{
      target_node_rid, std::move (target_spot_id), std::move (parts), std::nullopt});
    request_backend_t backend;
    auto prepared = _state->_lane.run_sync (
      [&] () -> result_t<request_task_t<runtime::messaging::message_parts_t>> {
          if (auto connected = ensure_connected (_state); !connected) {
              return detail::propagate_failure<
                request_task_t<runtime::messaging::message_parts_t>> (
                connected, "route channel is not connected");
          }
          if (!_state->_request_backend) {
              return detail::boundary_failure<request_task_t<runtime::messaging::message_parts_t>> (
                detail::boundary_error_t::timed_out,
                "route request reply was not completed by a backend");
          }
          backend = _state->_request_backend;
          // This sequence records the diagnostic packet. The backend request task
          // owns correlation and completion; no second pending entry is needed here.
          packet->request_seq = _state->_pending_requests.next_request_seq ();
          append_outbound_unlocked (_state, packet->target_node_rid, packet->target_spot_id,
                                    packet->parts, packet->request_seq);
          const auto timeout = remaining_request_timeout (deadline);
          if (timeout <= std::chrono::milliseconds::zero ()) {
              return detail::boundary_failure<request_task_t<runtime::messaging::message_parts_t>> (
                detail::boundary_error_t::timed_out,
                "route request deadline expired before binding submission");
          }
          return result_t<request_task_t<runtime::messaging::message_parts_t>>::success (
            backend (packet->target_node_rid, packet->target_spot_id, packet->parts, timeout));
      });
    if (!prepared) {
        return detail::propagate_failure<runtime::messaging::message_parts_t> (
          prepared, "route request submission failed");
    }
    auto pending = std::move (prepared.value ());
    auto physical = detail::request_task_access_t::physical_reply (pending);
    detail::observe_task_terminal (
      physical, [packet = std::move (packet), backend = std::move (backend)] (const auto &) {
          (void) packet;
          (void) backend;
      });
    return pending;
}

result_t<void>
route_channel_runtime_t::submit_spot_send_parts (const zlink::routing_id_t &target_node_rid,
                                                 const std::string &target_spot_id,
                                                 runtime::messaging::message_parts_t parts)
{
    send_backend_t backend;
    std::optional<zlink::routing_id_t> backend_target;
    std::optional<std::string> backend_spot_target;
    runtime::messaging::message_parts_t backend_parts;
    if (auto connected = wait_until_connected (default_request_timeout ()); !connected) {
        return connected;
    }
    if (auto ready = wait_until_peer_ready (target_node_rid, default_request_timeout ()); !ready) {
        return ready;
    }
    auto prepared = _state->_lane
                      .run ([&, this] {
                          if (auto connected = ensure_connected (_state); !connected) {
                              return connected;
                          }
                          auto &packet =
                            append_outbound_unlocked (_state, target_node_rid, target_spot_id,
                                                      std::move (parts), std::nullopt);
                          if (_state->_send_backend) {
                              backend = _state->_send_backend;
                              backend_target = packet.target_node_rid;
                              backend_spot_target = packet.target_spot_id;
                              backend_parts = packet.parts;
                          }
                          return result_t<void>::success ();
                      })
                      .get ();
    if (!prepared)
        return prepared;
    if (backend) {
        std::lock_guard send_lock (_state->_send_backend_mutex);
        return backend (*backend_target, backend_spot_target, backend_parts);
    }
    return result_t<void>::success ();
}

request_task_t<runtime::messaging::message_parts_t>
route_channel_runtime_t::request_reply_spot_parts (const zlink::routing_id_t &target_node_rid,
                                                   const std::string &target_spot_id,
                                                   runtime::messaging::message_parts_t parts,
                                                   std::chrono::steady_clock::time_point deadline)
{
    return request_reply_spot_parts_owned (_state, target_node_rid, target_spot_id,
                                           std::move (parts), deadline);
}

request_task_t<runtime::messaging::message_parts_t>
route_channel_runtime_t::request_reply_spot_parts_owned (
  std::shared_ptr<state_t> state,
  zlink::routing_id_t target_node_rid,
  std::string target_spot_id,
  runtime::messaging::message_parts_t parts,
  std::chrono::steady_clock::time_point deadline)
{
    co_await wait_until_ready (state, target_node_rid, deadline);
    auto pending = request_backend_parts (state, target_node_rid, std::move (target_spot_id),
                                          std::move (parts), deadline);
    co_return co_await detail::request_submission (std::move (pending));
}

result_t<void> route_channel_runtime_t::complete_request (std::uint64_t request_seq)
{
    return _state->_lane.run_sync ([&, this] {
        if (!_state->_pending_requests.remove (request_seq)) {
            return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                            "routed reply does not match a pending request");
        }
        return result_t<void>::success ();
    });
}

void route_channel_runtime_t::set_send_backend (send_backend_t backend)
{
    _state->_lane.run ([&, this] { _state->_send_backend = std::move (backend); }).get ();
    notify_state_changed ();
}

void route_channel_runtime_t::set_request_backend (request_backend_t backend)
{
    _state->_lane.run ([&, this] { _state->_request_backend = std::move (backend); }).get ();
    notify_state_changed ();
}

std::vector<route_outbound_packet_t> route_channel_runtime_t::outbound_packets () const
{
    return _state->_lane.run ([&, this] { return _state->_outbound_packets; }).get ();
}

std::size_t route_channel_runtime_t::pending_request_count () const
{
    return _state->_lane.run ([&, this] { return _state->_pending_requests.count (); }).get ();
}

route_outbound_packet_t &
route_channel_runtime_t::append_outbound_unlocked (const std::shared_ptr<state_t> &_state,
                                                   const zlink::routing_id_t &target_node_rid,
                                                   std::optional<std::string> target_spot_id,
                                                   runtime::messaging::message_parts_t parts,
                                                   std::optional<std::uint64_t> request_seq)
{
    _state->_outbound_packets.push_back (route_outbound_packet_t{
      target_node_rid, std::move (target_spot_id), std::move (parts), request_seq});
    return _state->_outbound_packets.back ();
}

result_t<std::uint64_t>
route_channel_runtime_t::register_request_unlocked (const zlink::routing_id_t &target_node_rid,
                                                    std::optional<std::string> target_spot_id,
                                                    runtime::messaging::message_parts_t parts)
{
    const auto request_seq = _state->_pending_requests.next_request_seq ();
    _state->_pending_requests.register_request (request_seq, _state->_router_channel_id);
    append_outbound_unlocked (_state, target_node_rid, std::move (target_spot_id),
                              std::move (parts), request_seq);
    return result_t<std::uint64_t>::success (request_seq);
}

result_t<void> route_channel_runtime_t::ensure_connected (const std::shared_ptr<state_t> &_state)
{
    if (auto ready = ready_unlocked (*_state, std::nullopt))
        return std::move (*ready);
    return result_t<void>::failure (framework_error_kind_t::unavailable,
                                    "route channel has no connected endpoint");
}

std::optional<result_t<void>>
route_channel_runtime_t::ready_unlocked (const state_t &state,
                                         const std::optional<zlink::routing_id_t> &target)
{
    if (!state._running)
        return result_t<void>::failure (framework_error_kind_t::unavailable,
                                        "route channel runtime is not running");
    if (!state._send_backend && !state._request_backend && state._connections.list ().empty ())
        return std::nullopt;
    if (!target)
        return result_t<void>::success ();
    if (state._ready_peer_rids.contains (target->to_string ()))
        return result_t<void>::success ();
    const auto targets = state._connections.targets ();
    const bool has_explicit_peer =
      std::any_of (targets.begin (), targets.end (), [&] (const auto &entry) {
          return entry.peer_rid && *entry.peer_rid == *target;
      });
    const bool target_is_self = state._routing_id && *state._routing_id == *target;
    const bool has_unknown_remote_endpoint =
      std::any_of (targets.begin (), targets.end (), [&] (const auto &entry) {
          return !entry.peer_rid && entry.endpoint != state._bind_endpoint;
      });
    if (!has_explicit_peer
        && (state._bind_endpoint.empty () || target_is_self || has_unknown_remote_endpoint))
        return result_t<void>::success ();
    return std::nullopt;
}

task_t<void>
route_channel_runtime_t::wait_until_ready (std::shared_ptr<state_t> state,
                                           std::optional<zlink::routing_id_t> target,
                                           std::chrono::steady_clock::time_point deadline)
{
    for (;;) {
        if (remaining_request_timeout (deadline) <= std::chrono::milliseconds::zero ())
            detail::boundary_failure<void> (detail::boundary_error_t::timed_out,
                                            "route request deadline expired during preparation")
              .value ();
        auto observation = state->_lane.run_sync (
          [&] { return std::pair (ready_unlocked (*state, target), state->changed->task ()); });
        if (observation.first) {
            observation.first->value ();
            co_return;
        }
        struct observation_lifetime_t
        {
            std::stop_source cancellation;
            ~observation_lifetime_t () { cancellation.request_stop (); }
        } lifetime;
        const auto parent = [] {
            if (auto turn = detail::capture_current_serial_turn (); turn && !turn->released ())
                return turn->wait_cancellation ();
            return detail::current_wait_owner ();
        }();
        std::stop_callback parent_cancel (parent, [&] { lifetime.cancellation.request_stop (); });
        auto bounded = [&] {
            auto context = detail::capture_ambient_context ();
            context.wait_owner = lifetime.cancellation.get_token ();
            // This observer owns only readiness; the caller's await keeps its turn.
            detail::serial_turn_scope_t observer_turn (nullptr);
            const auto observer_context = detail::enter_ambient_context (context);
            return runtime::messaging::with_request_deadline (std::move (observation.second),
                                                              deadline);
        }();
        co_await bounded;
    }
}

result_t<void>
route_channel_runtime_t::wait_until_peer_ready (const zlink::routing_id_t &target_node_rid,
                                                std::chrono::milliseconds timeout) const
{
    if (timeout > std::chrono::milliseconds::zero ())
        return wait_until_ready (_state, target_node_rid,
                                 std::chrono::steady_clock::now () + timeout)
          .result ();
    return _state->_lane.run_sync ([&] {
        if (auto ready = ready_unlocked (*_state, target_node_rid))
            return std::move (*ready);
        return result_t<void>::failure (framework_error_kind_t::unavailable,
                                        "route channel peer is not ready");
    });
}

result_t<void>
route_channel_runtime_t::wait_until_connected (std::chrono::milliseconds timeout) const
{
    if (timeout <= std::chrono::milliseconds::zero ())
        return _state->_lane.run_sync ([&] { return ensure_connected (_state); });
    return wait_until_ready (_state, std::nullopt, std::chrono::steady_clock::now () + timeout)
      .result ();
}

} // namespace zlink::framework::detail
