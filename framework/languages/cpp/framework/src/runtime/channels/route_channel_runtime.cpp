/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/channels/route_channel_runtime.hpp"

#include <algorithm>
#include <thread>
#include <utility>

namespace zlink::framework::detail
{

route_channel_runtime_t::route_channel_runtime_t (std::string router_channel_id) :
    _router_channel_id (std::move (router_channel_id))
{
}

const std::string &route_channel_runtime_t::router_channel_id () const noexcept
{
    return _router_channel_id;
}

void route_channel_runtime_t::routing_id (zlink::routing_id_t routing_id)
{
    std::lock_guard lock (_mutex);
    _routing_id = std::move (routing_id);
}

const std::optional<zlink::routing_id_t> &route_channel_runtime_t::routing_id () const noexcept
{
    return _routing_id;
}

void route_channel_runtime_t::default_request_timeout (std::chrono::milliseconds timeout)
{
    std::lock_guard lock (_mutex);
    _default_request_timeout = timeout;
}

std::chrono::milliseconds route_channel_runtime_t::default_request_timeout () const noexcept
{
    return _default_request_timeout;
}

void route_channel_runtime_t::start () noexcept
{
    std::lock_guard lock (_mutex);
    _running = true;
}

void route_channel_runtime_t::stop () noexcept
{
    std::lock_guard lock (_mutex);
    _running = false;
    _pending_requests.clear ();
    _send_backend = {};
    _request_backend = {};
}

bool route_channel_runtime_t::running () const noexcept
{
    std::lock_guard lock (_mutex);
    return _running;
}

bool route_channel_runtime_t::connect (std::string endpoint)
{
    std::lock_guard lock (_mutex);
    return _connections.connect (std::move (endpoint));
}

bool route_channel_runtime_t::connect (zlink::routing_id_t peer_rid, std::string endpoint)
{
    std::lock_guard lock (_mutex);
    return _connections.connect (std::move (peer_rid), std::move (endpoint));
}

bool route_channel_runtime_t::disconnect (const std::string &endpoint)
{
    std::lock_guard lock (_mutex);
    const auto targets = _connections.targets ();
    const bool removed = _connections.disconnect (endpoint);
    if (removed) {
        for (const auto &target : targets) {
            if (target.endpoint == endpoint && target.peer_rid) {
                _ready_peer_rids.erase (target.peer_rid->to_string ());
            }
        }
    }
    return removed;
}

std::vector<std::string> route_channel_runtime_t::list_connections () const
{
    std::lock_guard lock (_mutex);
    return _connections.list ();
}

std::vector<route_connection_set_t::target_t>
route_channel_runtime_t::list_connection_targets () const
{
    std::lock_guard lock (_mutex);
    return _connections.targets ();
}

void route_channel_runtime_t::mark_peer_ready (const zlink::routing_id_t &peer_rid)
{
    std::lock_guard lock (_mutex);
    _ready_peer_rids.insert (peer_rid.to_string ());
}

void route_channel_runtime_t::mark_peer_disconnected (const zlink::routing_id_t &peer_rid)
{
    std::lock_guard lock (_mutex);
    _ready_peer_rids.erase (peer_rid.to_string ());
}

void route_channel_runtime_t::bind_endpoint (std::string endpoint)
{
    std::lock_guard lock (_mutex);
    _bind_endpoint = std::move (endpoint);
}

const std::string &route_channel_runtime_t::bind_endpoint () const noexcept
{
    return _bind_endpoint;
}

void route_channel_runtime_t::manual_connections (std::vector<std::string> endpoints)
{
    std::lock_guard lock (_mutex);
    _manual_connections = std::move (endpoints);
}

std::vector<std::string> route_channel_runtime_t::manual_connections () const
{
    std::lock_guard lock (_mutex);
    return _manual_connections;
}

result_t<void>
route_channel_runtime_t::submit_send_parts (const zlink::routing_id_t &target_node_rid,
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
    {
        std::lock_guard lock (_mutex);
        if (auto connected = ensure_connected (); !connected) {
            return connected;
        }
        auto &packet =
          append_outbound_unlocked (target_node_rid, std::nullopt, std::move (parts), std::nullopt);
        if (_send_backend) {
            backend = _send_backend;
            backend_target = packet.target_node_rid;
            backend_spot_target = packet.target_spot_id;
            backend_parts = packet.parts;
        }
    }
    if (backend) {
        std::lock_guard send_lock (_send_backend_mutex);
        return backend (*backend_target, backend_spot_target, backend_parts);
    }
    return result_t<void>::success ();
}

result_t<std::uint64_t>
route_channel_runtime_t::submit_request_parts (const zlink::routing_id_t &target_node_rid,
                                               runtime::messaging::message_parts_t parts)
{
    std::lock_guard lock (_mutex);
    if (auto connected = ensure_connected (); !connected) {
        return detail::propagate_failure<std::uint64_t> (connected, "route channel is not connected");
    }
    return register_request_unlocked (target_node_rid, std::nullopt, std::move (parts));
}

result_t<runtime::messaging::message_parts_t>
route_channel_runtime_t::request_reply_parts (const zlink::routing_id_t &target_node_rid,
                                              runtime::messaging::message_parts_t parts,
                                              std::chrono::milliseconds timeout)
{
    request_backend_t backend;
    std::uint64_t request_seq = 0;
    if (auto connected = wait_until_connected (timeout); !connected) {
        return detail::propagate_failure<runtime::messaging::message_parts_t> (connected, "route channel is not connected");
    }
    if (auto ready = wait_until_peer_ready (target_node_rid, timeout); !ready) {
        return detail::propagate_failure<runtime::messaging::message_parts_t> (ready, "route channel peer is not ready");
    }
    {
        std::lock_guard lock (_mutex);
        if (auto connected = ensure_connected (); !connected) {
            return detail::propagate_failure<runtime::messaging::message_parts_t> (connected, "route channel is not connected");
        }
        auto registered = register_request_unlocked (target_node_rid, std::nullopt, parts);
        request_seq = registered.value ();
        if (!_request_backend) {
            _pending_requests.remove (request_seq);
            return detail::boundary_failure<runtime::messaging::message_parts_t> (detail::boundary_error_t::timed_out,
              "route request reply was not completed by a backend");
        }
        backend = _request_backend;
    }
    auto reply = backend (target_node_rid, std::nullopt, parts, timeout);
    {
        std::lock_guard lock (_mutex);
        _pending_requests.remove (request_seq);
    }
    return reply;
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
    {
        std::lock_guard lock (_mutex);
        if (auto connected = ensure_connected (); !connected) {
            return connected;
        }
        auto &packet = append_outbound_unlocked (target_node_rid, target_spot_id,
                                                 std::move (parts), std::nullopt);
        if (_send_backend) {
            backend = _send_backend;
            backend_target = packet.target_node_rid;
            backend_spot_target = packet.target_spot_id;
            backend_parts = packet.parts;
        }
    }
    if (backend) {
        std::lock_guard send_lock (_send_backend_mutex);
        return backend (*backend_target, backend_spot_target, backend_parts);
    }
    return result_t<void>::success ();
}

result_t<std::uint64_t>
route_channel_runtime_t::request_to_spot_parts (const zlink::routing_id_t &target_node_rid,
                                                const std::string &target_spot_id,
                                                runtime::messaging::message_parts_t parts)
{
    request_backend_t backend;
    std::uint64_t request_seq = 0;
    {
        std::lock_guard lock (_mutex);
        if (auto connected = ensure_connected (); !connected) {
            return detail::propagate_failure<std::uint64_t> (connected, "route channel is not connected");
        }
        auto registered = register_request_unlocked (target_node_rid, target_spot_id, parts);
        request_seq = registered.value ();
        if (_request_backend) {
            backend = _request_backend;
        }
    }
    if (backend) {
        auto reply = backend (target_node_rid, target_spot_id, parts, default_request_timeout ());
        std::lock_guard lock (_mutex);
        _pending_requests.remove (request_seq);
        if (!reply) {
            return result_t<std::uint64_t>::failure (reply.error_kind (),
                                                     reply.error () ? reply.error ()->what ()
                                                                    : "route spot request failed");
        }
    }
    return result_t<std::uint64_t>::success (request_seq);
}

result_t<runtime::messaging::message_parts_t>
route_channel_runtime_t::request_reply_spot_parts (const zlink::routing_id_t &target_node_rid,
                                                   const std::string &target_spot_id,
                                                   runtime::messaging::message_parts_t parts,
                                                   std::chrono::milliseconds timeout)
{
    request_backend_t backend;
    std::uint64_t request_seq = 0;
    if (auto connected = wait_until_connected (timeout); !connected) {
        return detail::propagate_failure<runtime::messaging::message_parts_t> (connected, "route channel is not connected");
    }
    if (auto ready = wait_until_peer_ready (target_node_rid, timeout); !ready) {
        return detail::propagate_failure<runtime::messaging::message_parts_t> (ready, "route channel peer is not ready");
    }
    {
        std::lock_guard lock (_mutex);
        if (auto connected = ensure_connected (); !connected) {
            return detail::propagate_failure<runtime::messaging::message_parts_t> (connected, "route channel is not connected");
        }
        auto registered = register_request_unlocked (target_node_rid, target_spot_id, parts);
        request_seq = registered.value ();
        if (!_request_backend) {
            _pending_requests.remove (request_seq);
            return detail::boundary_failure<runtime::messaging::message_parts_t> (detail::boundary_error_t::timed_out,
              "route spot request reply was not completed by a backend");
        }
        backend = _request_backend;
    }
    auto reply = backend (target_node_rid, target_spot_id, parts, timeout);
    {
        std::lock_guard lock (_mutex);
        _pending_requests.remove (request_seq);
    }
    return reply;
}

result_t<void> route_channel_runtime_t::complete_request (std::uint64_t request_seq)
{
    std::lock_guard lock (_mutex);
    if (!_pending_requests.remove (request_seq)) {
        return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                        "routed reply does not match a pending request");
    }
    return result_t<void>::success ();
}

void route_channel_runtime_t::set_send_backend (send_backend_t backend)
{
    std::lock_guard lock (_mutex);
    _send_backend = std::move (backend);
}

void route_channel_runtime_t::set_request_backend (request_backend_t backend)
{
    std::lock_guard lock (_mutex);
    _request_backend = std::move (backend);
}

const std::vector<route_outbound_packet_t> &
route_channel_runtime_t::outbound_packets () const noexcept
{
    return _outbound_packets;
}

std::size_t route_channel_runtime_t::pending_request_count () const noexcept
{
    std::lock_guard lock (_mutex);
    return _pending_requests.count ();
}

route_outbound_packet_t &route_channel_runtime_t::append_outbound_unlocked (
  const zlink::routing_id_t &target_node_rid,
  std::optional<std::string> target_spot_id,
  runtime::messaging::message_parts_t parts,
  std::optional<std::uint64_t> request_seq)
{
    _outbound_packets.push_back (route_outbound_packet_t{
      target_node_rid, std::move (target_spot_id), std::move (parts), request_seq});
    return _outbound_packets.back ();
}

result_t<std::uint64_t> route_channel_runtime_t::register_request_unlocked (
  const zlink::routing_id_t &target_node_rid,
  std::optional<std::string> target_spot_id,
  runtime::messaging::message_parts_t parts)
{
    const auto request_seq = _pending_requests.next_request_seq ();
    _pending_requests.register_request (request_seq, _router_channel_id);
    append_outbound_unlocked (target_node_rid, std::move (target_spot_id), std::move (parts),
                              request_seq);
    return result_t<std::uint64_t>::success (request_seq);
}

result_t<void> route_channel_runtime_t::ensure_connected () const
{
    if (!_running) {
        return result_t<void>::failure (framework_error_kind_t::unavailable,
                                        "route channel runtime is not running");
    }
    if (!_send_backend && !_request_backend && _connections.list ().empty ()) {
        return result_t<void>::failure (framework_error_kind_t::unavailable,
                                        "route channel has no connected endpoint");
    }
    return result_t<void>::success ();
}

result_t<void> route_channel_runtime_t::wait_until_peer_ready (
  const zlink::routing_id_t &target_node_rid,
  std::chrono::milliseconds timeout) const
{
    const auto deadline =
      timeout > std::chrono::milliseconds::zero ()
        ? std::chrono::steady_clock::now () + timeout
        : std::chrono::steady_clock::time_point{};
    result_t<void> last = result_t<void>::failure (
      framework_error_kind_t::unavailable,
      "route channel peer '" + target_node_rid.to_string () + "' is not ready");
    for (;;) {
        {
            std::lock_guard lock (_mutex);
            const auto targets = _connections.targets ();
            if (!_running) {
                return last;
            }
            const bool has_ready_peer =
              _ready_peer_rids.contains (target_node_rid.to_string ());
            if (has_ready_peer) {
                return result_t<void>::success ();
            }
            const bool has_explicit_peer =
              std::any_of (targets.begin (), targets.end (),
                           [&target_node_rid] (const auto &target) {
                               return target.peer_rid && *target.peer_rid == target_node_rid;
                           });
            const bool target_is_self =
              _routing_id && *_routing_id == target_node_rid;
            const bool has_unknown_remote_endpoint =
              std::any_of (targets.begin (), targets.end (), [this] (const auto &target) {
                  return !target.peer_rid && target.endpoint != _bind_endpoint;
              });
            if (!has_explicit_peer
                && (_bind_endpoint.empty () || target_is_self || has_unknown_remote_endpoint)) {
                return result_t<void>::success ();
            }
        }
        if (timeout <= std::chrono::milliseconds::zero ()
            || std::chrono::steady_clock::now () >= deadline) {
            return last;
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (10));
    }
}

result_t<void>
route_channel_runtime_t::wait_until_connected (std::chrono::milliseconds timeout) const
{
    const auto deadline =
      timeout > std::chrono::milliseconds::zero ()
        ? std::chrono::steady_clock::now () + timeout
        : std::chrono::steady_clock::time_point{};
    result_t<void> last =
      result_t<void>::failure (framework_error_kind_t::unavailable,
                               "route channel is not connected");
    for (;;) {
        {
            std::lock_guard lock (_mutex);
            last = ensure_connected ();
            if (!_running) {
                return last;
            }
            if (last) {
                return last;
            }
        }
        if (timeout <= std::chrono::milliseconds::zero ()
            || std::chrono::steady_clock::now () >= deadline) {
            return last;
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (10));
    }
}

} // namespace zlink::framework::detail
