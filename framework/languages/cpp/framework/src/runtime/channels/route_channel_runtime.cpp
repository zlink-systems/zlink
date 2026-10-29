/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

#include "runtime/channels/route_channel_runtime.hpp"

#include "runtime/transport/endpoint_notation.hpp"
#include "runtime/foundation/operation_registry.hpp"

#include <algorithm>
#include <utility>
#include <variant>

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
template <typename T> struct readiness_change_awaiter_t
{
    task_t<T> pending;
    bool await_ready () const noexcept { return pending.await_ready (); }
    void await_suspend (std::coroutine_handle<> continuation)
    {
        auto context = detail::capture_ambient_context ();
        context.wait_owner = {};
        const auto scope = detail::enter_ambient_context (context, false);
        pending.await_suspend (continuation);
    }
    T await_resume () { return pending.await_resume (); }
};
} // namespace

struct route_channel_runtime_t::removed_route_t
{
    std::optional<zlink::routing_id_t> target;
};

struct route_channel_runtime_t::ready_observer_t
{
    std::function<std::optional<result_t<void>> (const std::optional<removed_route_t> &)> check;
    task_completion_source_t<void> completion;
    template <typename Check>
    static std::optional<result_t<void>> evaluate (Check &check,
                                                   const std::optional<removed_route_t> &change)
    {
        try {
            return check (change);
        }
        catch (...) {
            return detail::current_exception_result<void> ();
        }
    }
    std::optional<result_t<void>> evaluate (const std::optional<removed_route_t> &change)
    {
        return evaluate (check, change);
    }
};

struct route_channel_runtime_t::state_t
{
    std::string _router_channel_id;
    mutable runtime::state_lane_t _lane;
    std::shared_ptr<runtime::foundation::operation_registry_t> readiness_operations{
      runtime::foundation::operation_registry_t::create_scheduled (_lane)};
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
    template <typename Check>
    static task_t<void> prepare (std::shared_ptr<state_t> state,
                                 Check check,
                                 std::optional<std::chrono::steady_clock::time_point> deadline)
    {
        using prepared_t = std::variant<result_t<void>, std::shared_ptr<ready_observer_t>>;
        auto prepared = state->_lane.run_sync ([&] () -> prepared_t {
            auto terminal = ready_observer_t::evaluate (check, std::nullopt);
            if (terminal)
                return prepared_t (std::in_place_index<0>, std::move (*terminal));
            auto observer = std::make_shared<ready_observer_t> ();
            observer->check = std::move (check);
            state->observers.emplace_back (observer);
            return prepared_t (std::in_place_index<1>, std::move (observer));
        });
        if (auto *terminal = std::get_if<result_t<void>> (&prepared))
            return task_t<void> (std::move (*terminal));
        return await_readiness (std::move (state), std::move (std::get<1> (prepared)), deadline);
    }

    std::vector<std::weak_ptr<ready_observer_t>> observers;
    using completion_t = std::pair<std::shared_ptr<ready_observer_t>, result_t<void>>;

    std::vector<completion_t> notify_unlocked (const std::optional<removed_route_t> &change)
    {
        std::vector<completion_t> completed;
        std::erase_if (observers, [&] (const auto &entry) {
            auto observer = entry.lock ();
            if (!observer)
                return true;
            auto terminal = observer->evaluate (change);
            if (!terminal)
                return false;
            completed.emplace_back (std::move (observer), std::move (*terminal));
            return true;
        });
        return completed;
    }

    template <typename Work>
    auto mutate (Work &&work, const std::optional<removed_route_t> &change = {})
    {
        using value_t = std::invoke_result_t<Work &>;
        auto output = _lane.run_sync ([&] {
            auto value = [&] {
                if constexpr (std::is_void_v<value_t>) {
                    work ();
                    return true;
                } else {
                    return work ();
                }
            }();
            return std::pair (std::move (value), notify_unlocked (change));
        });
        for (auto &[observer, result] : output.second)
            observer->completion.complete (std::move (result));
        if constexpr (!std::is_void_v<value_t>)
            return std::move (output.first);
    }
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

const std::string &route_channel_runtime_t::router_channel_id () const noexcept
{
    return _state->_router_channel_id;
}

void route_channel_runtime_t::routing_id (zlink::routing_id_t routing_id)
{
    _state->mutate ([&] { _state->_routing_id = std::move (routing_id); });
}

std::optional<zlink::routing_id_t> route_channel_runtime_t::routing_id () const
{
    return _state->_lane.run_sync ([&, this] { return _state->_routing_id; });
}

void route_channel_runtime_t::default_request_timeout (std::chrono::milliseconds timeout)
{
    _state->mutate ([&] { _state->_default_request_timeout = timeout; });
}

std::chrono::milliseconds route_channel_runtime_t::default_request_timeout () const
{
    return _state->_lane.run_sync ([&, this] { return _state->_default_request_timeout; });
}

void route_channel_runtime_t::start ()
{
    _state->mutate ([&] { _state->_running = true; });
}

void route_channel_runtime_t::stop ()
{
    _state->mutate ([&] {
        _state->_running = false;
        _state->_pending_requests.clear ();
        _state->_send_backend = {};
        _state->_request_backend = {};
    });
}

bool route_channel_runtime_t::running () const
{
    return _state->_lane.run_sync ([&, this] { return _state->_running; });
}

bool route_channel_runtime_t::connect (std::string endpoint)
{
    return _state->mutate ([&] { return _state->_connections.connect (std::move (endpoint)); });
}

bool route_channel_runtime_t::connect (zlink::routing_id_t peer_rid, std::string endpoint)
{
    return _state->mutate (
      [&] { return _state->_connections.connect (std::move (peer_rid), std::move (endpoint)); });
}

bool route_channel_runtime_t::disconnect (const std::string &endpoint)
{
    return disconnect_endpoint (std::nullopt, endpoint);
}

bool route_channel_runtime_t::disconnect (const zlink::routing_id_t &peer_rid,
                                          const std::string &endpoint)
{
    return disconnect_endpoint (peer_rid, endpoint);
}

bool route_channel_runtime_t::disconnect_endpoint (std::optional<zlink::routing_id_t> peer_rid,
                                                   std::string endpoint)
{
    const auto normalized = runtime::transport::normalize_endpoint (endpoint);
    std::optional<removed_route_t> change;
    return _state->mutate (
      [&] {
          const auto before = _state->_connections.targets ();
          const auto selected =
            std::find_if (before.begin (), before.end (), [&] (const auto &entry) {
                return entry.endpoint == normalized && (!peer_rid || entry.peer_rid == peer_rid);
            });
          if (selected == before.end ())
              return false;
          _state->_connections.disconnect (normalized);
          const auto remaining = _state->_connections.targets ();
          const bool has_unknown =
            std::any_of (remaining.begin (), remaining.end (), [&] (const auto &entry) {
                return !entry.peer_rid && entry.endpoint != _state->_bind_endpoint;
            });
          const bool has_same_target =
            selected->peer_rid
            && std::any_of (remaining.begin (), remaining.end (), [&] (const auto &entry) {
                   return entry.peer_rid == selected->peer_rid;
               });
          if (!has_unknown && !has_same_target
              && (selected->peer_rid || selected->endpoint != _state->_bind_endpoint)) {
              change.emplace (removed_route_t{selected->peer_rid});
          }
          if (selected->peer_rid && !has_same_target)
              _state->_ready_peer_rids.erase (selected->peer_rid->to_string ());
          return true;
      },
      change);
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
    _state->mutate ([&] { _state->_ready_peer_rids.insert (peer_rid.to_string ()); });
}

void route_channel_runtime_t::mark_peer_disconnected (const zlink::routing_id_t &peer_rid)
{
    _state->mutate ([&] { _state->_ready_peer_rids.erase (peer_rid.to_string ()); });
}

void route_channel_runtime_t::bind_endpoint (std::string endpoint)
{
    _state->mutate (
      [&] { _state->_bind_endpoint = runtime::transport::normalize_endpoint (endpoint); });
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
route_channel_runtime_t::submit_send_unlocked (const std::shared_ptr<state_t> &state,
                                               const zlink::routing_id_t &target,
                                               std::optional<std::string> spot,
                                               runtime::messaging::message_parts_t parts)
{
    append_outbound_unlocked (state, target, spot, parts, std::nullopt);
    if (auto backend = state->_send_backend)
        return backend (target, spot, parts);
    return result_t<void>::success ();
}

result_t<void>
route_channel_runtime_t::submit_send_parts (const zlink::routing_id_t &target_node_rid,
                                            runtime::messaging::message_parts_t parts)
{
    return _state->_lane.run_sync ([&] {
        if (auto ready = ensure_ready (_state, target_node_rid); !ready)
            return ready;
        return submit_send_unlocked (_state, target_node_rid, std::nullopt, std::move (parts));
    });
}

result_t<std::uint64_t>
route_channel_runtime_t::submit_request_parts (const zlink::routing_id_t &target_node_rid,
                                               runtime::messaging::message_parts_t parts)
{
    return _state->_lane.run_sync ([&, this] {
        if (auto connected = ensure_ready (_state); !connected) {
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
    if (auto ready =
          _state->_lane.run_sync ([&] { return ensure_ready (_state, target_node_rid); });
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
          if (auto connected = ensure_ready (_state); !connected) {
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

task_t<void>
route_channel_runtime_t::submit_spot_send_parts (const zlink::routing_id_t &target_node_rid,
                                                 const std::string &target_spot_id,
                                                 runtime::messaging::message_parts_t parts)
{
    auto state = _state;
    return state_t::prepare (
      state,
      [state, target = target_node_rid, spot = target_spot_id,
       parts = std::move (parts)] (const auto &change) mutable {
          auto ready = ready_unlocked (*state, target, &change);
          if (ready && *ready)
              ready = submit_send_unlocked (state, target, std::move (spot), std::move (parts));
          return ready;
      },
      std::nullopt);
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
    _state->mutate ([&] { _state->_send_backend = std::move (backend); });
}

void route_channel_runtime_t::set_request_backend (request_backend_t backend)
{
    _state->mutate ([&] { _state->_request_backend = std::move (backend); });
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

result_t<void>
route_channel_runtime_t::ensure_ready (const std::shared_ptr<state_t> &state,
                                       const std::optional<zlink::routing_id_t> &target)
{
    if (auto ready = ready_unlocked (*state, target))
        return std::move (*ready);
    return result_t<void>::failure (framework_error_kind_t::unavailable,
                                    target ? "route channel peer is not ready"
                                           : "route channel has no connected endpoint");
}

std::optional<result_t<void>>
route_channel_runtime_t::ready_unlocked (const state_t &state,
                                         const std::optional<zlink::routing_id_t> &target,
                                         const std::optional<removed_route_t> *send_change)
{
    if (!state._running)
        return result_t<void>::failure (framework_error_kind_t::shutting_down,
                                        "route channel runtime is not running");
    if (send_change && *send_change && target) {
        const auto &removed = **send_change;
        const auto target_retained = [&] {
            if (removed.target)
                return removed.target != target;
            const auto remaining = state._connections.targets ();
            return std::any_of (remaining.begin (), remaining.end (),
                                [&] (const auto &entry) { return entry.peer_rid == target; });
        }();
        if (!target_retained)
            return result_t<void>::failure (
              framework_error_kind_t::unavailable,
              "route channel target was removed during send preparation");
    }
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

task_t<void> route_channel_runtime_t::await_readiness (
  std::shared_ptr<state_t> state,
  std::shared_ptr<ready_observer_t> observer,
  std::optional<std::chrono::steady_clock::time_point> deadline)
{
    struct subscription_t
    {
        std::shared_ptr<state_t> state;
        const ready_observer_t *observer;
        ~subscription_t ()
        {
            state->_lane.run_sync ([&] {
                std::erase_if (state->observers, [&] (const auto &entry) {
                    auto current = entry.lock ();
                    return !current || current.get () == observer;
                });
            });
        }
    } subscription{state, observer.get ()};
    if (!deadline) {
        co_await readiness_change_awaiter_t<void>{observer->completion.task ()};
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
    struct registration_t
    {
        std::shared_ptr<runtime::foundation::operation_registry_t> operations;
        runtime::call_id_t id;
        ~registration_t () { operations->unregister (id); }
    } registration{state->readiness_operations,
                   {0, state->_lane.run_sync (
                         [&] { return state->_pending_requests.next_request_seq (); })}};
    auto bounded = [&] {
        auto context = detail::capture_ambient_context ();
        context.wait_owner = lifetime.cancellation.get_token ();
        detail::serial_turn_scope_t observer_turn (nullptr);
        const auto scope = detail::enter_ambient_context (context);
        auto pending = observer->completion.task ();
        if (pending.await_ready ())
            return pending;
        const bool registered = state->readiness_operations->register_operation (
          registration.id, *deadline,
          [weak = std::weak_ptr (observer)] (runtime::foundation::operation_terminal_t terminal,
                                             zlink::message_t) {
              if (auto current = weak.lock ()) {
                  if (terminal == runtime::foundation::operation_terminal_t::cancelled) {
                      current->completion.complete (detail::result_access_t::failure<void> (
                        detail::make_cancellation_exception ("request was cancelled")));
                  } else {
                      const bool expired =
                        terminal == runtime::foundation::operation_terminal_t::timed_out;
                      current->completion.complete (detail::boundary_failure<void> (
                        expired ? detail::boundary_error_t::timed_out
                                : detail::boundary_error_t::shutdown,
                        expired ? "request deadline expired" : "route channel is closed"));
                  }
              }
          },
          {}, {}, {}, lifetime.cancellation.get_token ());
        if (!registered)
            observer->completion.complete (detail::boundary_failure<void> (
              detail::boundary_error_t::shutdown, "route channel is closed"));
        return pending;
    }();
    co_await bounded;
}

task_t<void>
route_channel_runtime_t::wait_until_ready (std::shared_ptr<state_t> state,
                                           std::optional<zlink::routing_id_t> target,
                                           std::chrono::steady_clock::time_point deadline)
{
    if (remaining_request_timeout (deadline) <= std::chrono::milliseconds::zero ())
        return task_t<void> (
          detail::boundary_failure<void> (detail::boundary_error_t::timed_out,
                                          "route request deadline expired during preparation"));
    return state_t::prepare (
      state, [state, target] (const auto &) { return ready_unlocked (*state, target); }, deadline);
}

} // namespace zlink::framework::detail
