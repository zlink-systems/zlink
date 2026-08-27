/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/channels/route_channel_runtime.hpp"

#include "runtime/transport/endpoint_notation.hpp"

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
    return _lane.run ([&, this] {
    _routing_id = std::move (routing_id);
    }).get ();
}

std::optional<zlink::routing_id_t> route_channel_runtime_t::routing_id () const
{
    return _lane.run ([&, this] {
        return _routing_id;
    }).get ();
}

void route_channel_runtime_t::default_request_timeout (std::chrono::milliseconds timeout)
{
    return _lane.run ([&, this] {
    _default_request_timeout = timeout;
    }).get ();
}

std::chrono::milliseconds route_channel_runtime_t::default_request_timeout () const
{
    return _lane.run ([&, this] {
        return _default_request_timeout;
    }).get ();
}

void route_channel_runtime_t::start ()
{
    return _lane.run ([&, this] {
    _running = true;
    }).get ();
}

void route_channel_runtime_t::stop ()
{
    return _lane.run ([&, this] {
    _running = false;
    _pending_requests.clear ();
    _send_backend = {};
    _request_backend = {};
    }).get ();
}

bool route_channel_runtime_t::running () const
{
    return _lane.run ([&, this] {
    return _running;
    }).get ();
}

bool route_channel_runtime_t::connect (std::string endpoint)
{
    return _lane.run ([&, this] {
    return _connections.connect (std::move (endpoint));
    }).get ();
}

bool route_channel_runtime_t::connect (zlink::routing_id_t peer_rid, std::string endpoint)
{
    return _lane.run ([&, this] {
    return _connections.connect (std::move (peer_rid), std::move (endpoint));
    }).get ();
}

bool route_channel_runtime_t::disconnect (const std::string &endpoint)
{
    const auto normalized_endpoint = runtime::transport::normalize_endpoint (endpoint);
    return _lane.run ([&, this] {
    const auto targets = _connections.targets ();
    const bool removed = _connections.disconnect (normalized_endpoint);
    if (removed) {
        for (const auto &target : targets) {
            if (target.endpoint == normalized_endpoint && target.peer_rid) {
                _ready_peer_rids.erase (target.peer_rid->to_string ());
            }
        }
    }
    return removed;
    }).get ();
}

bool route_channel_runtime_t::disconnect (
  const zlink::routing_id_t &peer_rid,
  const std::string &endpoint)
{
    const auto normalized_endpoint = runtime::transport::normalize_endpoint (endpoint);
    return _lane.run ([&, this] {
    const bool removed = _connections.disconnect (peer_rid, normalized_endpoint);
    if (removed)
        _ready_peer_rids.erase (peer_rid.to_string ());
    return removed;
    }).get ();
}

std::vector<std::string> route_channel_runtime_t::list_connections () const
{
    return _lane.run ([&, this] {
    return _connections.list ();
    }).get ();
}

std::vector<route_connection_set_t::target_t>
route_channel_runtime_t::list_connection_targets () const
{
    return _lane.run ([&, this] {
    return _connections.targets ();
    }).get ();
}

void route_channel_runtime_t::mark_peer_ready (const zlink::routing_id_t &peer_rid)
{
    return _lane.run ([&, this] {
    _ready_peer_rids.insert (peer_rid.to_string ());
    }).get ();
}

void route_channel_runtime_t::mark_peer_disconnected (const zlink::routing_id_t &peer_rid)
{
    return _lane.run ([&, this] {
    _ready_peer_rids.erase (peer_rid.to_string ());
    }).get ();
}

void route_channel_runtime_t::bind_endpoint (std::string endpoint)
{
    return _lane.run ([&, this] {
    _bind_endpoint = runtime::transport::normalize_endpoint (endpoint);
    }).get ();
}

std::string route_channel_runtime_t::bind_endpoint () const
{
    return _lane.run ([&, this] {
        return _bind_endpoint;
    }).get ();
}

void route_channel_runtime_t::manual_connections (std::vector<std::string> endpoints)
{
    return _lane.run ([&, this] {
    _manual_connections.clear ();
    _manual_connections.reserve (endpoints.size ());
    for (auto &endpoint : endpoints) {
        _manual_connections.push_back (runtime::transport::normalize_endpoint (endpoint));
    }
    }).get ();
}

std::vector<std::string> route_channel_runtime_t::manual_connections () const
{
    return _lane.run ([&, this] {
    return _manual_connections;
    }).get ();
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
    if (auto ready = wait_until_peer_ready (target_node_rid, default_request_timeout ());
        !ready) {
        return ready;
    }
    auto prepared = _lane
                      .run ([&, this] {
                          if (auto connected = ensure_connected (); !connected) {
                              return connected;
                          }
                          auto &packet = append_outbound_unlocked (target_node_rid, std::nullopt,
                                                                   std::move (parts), std::nullopt);
                          if (_send_backend) {
                              backend = _send_backend;
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
        std::lock_guard send_lock (_send_backend_mutex);
        return backend (*backend_target, backend_spot_target, backend_parts);
    }
    return result_t<void>::success ();
}

result_t<std::uint64_t>
route_channel_runtime_t::submit_request_parts (const zlink::routing_id_t &target_node_rid,
                                               runtime::messaging::message_parts_t parts)
{
    return _lane.run ([&, this] {
    if (auto connected = ensure_connected (); !connected) {
        return detail::propagate_failure<std::uint64_t> (connected, "route channel is not connected");
    }
    return register_request_unlocked (target_node_rid, std::nullopt, std::move (parts));
    }).get ();
}

result_t<runtime::messaging::message_parts_t>
route_channel_runtime_t::request_reply_parts (const zlink::routing_id_t &target_node_rid,
                                              runtime::messaging::message_parts_t parts,
                                              std::chrono::milliseconds timeout)
{
    request_backend_t backend;
    std::uint64_t request_seq = 0;
    if (auto connected = wait_until_connected (timeout); !connected) {
        return detail::propagate_failure<runtime::messaging::message_parts_t> (
          connected, "route channel is not connected");
    }
    if (auto ready = wait_until_peer_ready (target_node_rid, timeout); !ready) {
        return detail::propagate_failure<runtime::messaging::message_parts_t> (
          ready, "route channel peer is not ready");
    }
    auto prepared =
      _lane
        .run ([&, this] {
            if (auto connected = ensure_connected (); !connected) {
                return detail::propagate_failure<runtime::messaging::message_parts_t> (
                  connected, "route channel is not connected");
            }
            auto registered = register_request_unlocked (target_node_rid, std::nullopt, parts);
            request_seq = registered.value ();
            if (!_request_backend) {
                _pending_requests.remove (request_seq);
                return detail::boundary_failure<runtime::messaging::message_parts_t> (
                  detail::boundary_error_t::timed_out,
                  "route request reply was not completed by a backend");
            }
            backend = _request_backend;
            return result_t<runtime::messaging::message_parts_t>::success ({});
        })
        .get ();
    if (!prepared)
        return prepared;
    auto reply = backend (target_node_rid, std::nullopt, parts, timeout);
    _lane.run ([&, this] { _pending_requests.remove (request_seq); }).get ();
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
    if (auto ready = wait_until_peer_ready (target_node_rid, default_request_timeout ());
        !ready) {
        return ready;
    }
    auto prepared = _lane
                      .run ([&, this] {
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
                          return result_t<void>::success ();
                      })
                      .get ();
    if (!prepared)
        return prepared;
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
    auto registered =
      _lane
        .run ([&, this] {
            if (auto connected = ensure_connected (); !connected) {
                return detail::propagate_failure<std::uint64_t> (connected,
                                                                 "route channel is not connected");
            }
            auto result = register_request_unlocked (target_node_rid, target_spot_id, parts);
            request_seq = result.value ();
            if (_request_backend) {
                backend = _request_backend;
            }
            return result;
        })
        .get ();
    if (!registered)
        return registered;
    if (backend) {
        auto reply = backend (target_node_rid, target_spot_id, parts, default_request_timeout ());
        _lane.run ([&, this] { _pending_requests.remove (request_seq); }).get ();
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
        return detail::propagate_failure<runtime::messaging::message_parts_t> (
          connected, "route channel is not connected");
    }
    if (auto ready = wait_until_peer_ready (target_node_rid, timeout); !ready) {
        return detail::propagate_failure<runtime::messaging::message_parts_t> (
          ready, "route channel peer is not ready");
    }
    auto prepared =
      _lane
        .run ([&, this] {
            if (auto connected = ensure_connected (); !connected) {
                return detail::propagate_failure<runtime::messaging::message_parts_t> (
                  connected, "route channel is not connected");
            }
            auto registered = register_request_unlocked (target_node_rid, target_spot_id, parts);
            request_seq = registered.value ();
            if (!_request_backend) {
                _pending_requests.remove (request_seq);
                return detail::boundary_failure<runtime::messaging::message_parts_t> (
                  detail::boundary_error_t::timed_out,
                  "route spot request reply was not completed by a backend");
            }
            backend = _request_backend;
            return result_t<runtime::messaging::message_parts_t>::success ({});
        })
        .get ();
    if (!prepared)
        return prepared;
    auto reply = backend (target_node_rid, target_spot_id, parts, timeout);
    _lane.run ([&, this] { _pending_requests.remove (request_seq); }).get ();
    return reply;
}

result_t<void> route_channel_runtime_t::complete_request (std::uint64_t request_seq)
{
    return _lane.run ([&, this] {
    if (!_pending_requests.remove (request_seq)) {
        return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                        "routed reply does not match a pending request");
    }
    return result_t<void>::success ();
    }).get ();
}

void route_channel_runtime_t::set_send_backend (send_backend_t backend)
{
    return _lane.run ([&, this] {
    _send_backend = std::move (backend);
    }).get ();
}

void route_channel_runtime_t::set_request_backend (request_backend_t backend)
{
    return _lane.run ([&, this] {
    _request_backend = std::move (backend);
    }).get ();
}

std::vector<route_outbound_packet_t>
route_channel_runtime_t::outbound_packets () const
{
    return _lane.run ([&, this] {
        return _outbound_packets;
    }).get ();
}

std::size_t route_channel_runtime_t::pending_request_count () const
{
    return _lane.run ([&, this] {
    return _pending_requests.count ();
    }).get ();
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

result_t<void> route_channel_runtime_t::wait_until_peer_ready (const zlink::routing_id_t &target_node_rid,
                                                std::chrono::milliseconds timeout) const
{
    const auto deadline = timeout > std::chrono::milliseconds::zero ()
                            ? std::chrono::steady_clock::now () + timeout
                            : std::chrono::steady_clock::time_point{};
    result_t<void> last = result_t<void>::failure (
      framework_error_kind_t::unavailable,
      "route channel peer '" + target_node_rid.to_string () + "' is not ready");
    for (;;) {
        auto observed =
          _lane
            .run ([&, this] () -> std::optional<result_t<void>> {
                const auto targets = _connections.targets ();
                if (!_running) {
                    return last;
                }
                const bool has_ready_peer =
                  _ready_peer_rids.contains (target_node_rid.to_string ());
                if (has_ready_peer) {
                    return result_t<void>::success ();
                }
                const bool has_explicit_peer = std::any_of (
                  targets.begin (), targets.end (), [&target_node_rid] (const auto &target) {
                      return target.peer_rid && *target.peer_rid == target_node_rid;
                  });
                const bool target_is_self = _routing_id && *_routing_id == target_node_rid;
                const bool has_unknown_remote_endpoint =
                  std::any_of (targets.begin (), targets.end (), [this] (const auto &target) {
                      return !target.peer_rid && target.endpoint != _bind_endpoint;
                  });
                if (!has_explicit_peer
                    && (_bind_endpoint.empty () || target_is_self || has_unknown_remote_endpoint)) {
                    return result_t<void>::success ();
                }
                return std::nullopt;
            })
            .get ();
        if (observed)
            return std::move (*observed);
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
    const auto deadline = timeout > std::chrono::milliseconds::zero ()
                            ? std::chrono::steady_clock::now () + timeout
                            : std::chrono::steady_clock::time_point{};
    result_t<void> last = result_t<void>::failure (framework_error_kind_t::unavailable,
                                                   "route channel is not connected");
    for (;;) {
        auto observed = _lane
                          .run ([&, this] () -> std::optional<result_t<void>> {
                              last = ensure_connected ();
                              if (!_running) {
                                  return last;
                              }
                              if (last) {
                                  return last;
                              }
                              return std::nullopt;
                          })
                          .get ();
        if (observed)
            return std::move (*observed);
        if (timeout <= std::chrono::milliseconds::zero ()
            || std::chrono::steady_clock::now () >= deadline) {
            return last;
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (10));
    }
}

} // namespace zlink::framework::detail
