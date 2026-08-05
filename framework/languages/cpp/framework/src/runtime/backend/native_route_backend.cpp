/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/backend/native_route_backend.hpp"

#include <zlink/Contracts/Errors/errors.hpp>
#include <zlink/Contracts/Sockets/routed_socket_contracts.hpp>

#include <cerrno>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <mutex>
#include <utility>
#include <vector>

namespace zlink::framework::detail::backend
{

namespace
{

std::vector<zlink::message_t> copy_parts (const runtime::messaging::message_parts_t &parts)
{
    std::vector<zlink::message_t> copied;
    copied.reserve (parts.size ());
    for (const auto &part : parts.items ()) {
        copied.push_back (part);
    }
    return copied;
}

template <typename TOperation>
auto append_remaining_parts (TOperation operation, std::vector<zlink::message_t> &parts)
{
    for (std::size_t index = 1; index < parts.size (); ++index) {
        operation = std::move (operation).message (parts[index]);
    }
    return operation;
}

result_t<void> submit_failure (const char *message)
{
    return result_t<void>::failure (framework_error_kind_t::internal_failure, message);
}

bool is_route_unreachable_errno (int value)
{
    return value == EHOSTUNREACH || value == ENETUNREACH || value == ECONNREFUSED
           || value == ENOTCONN;
}

bool route_channel_trace_enabled ()
{
    const char *value = std::getenv ("ZLINK_CPP_CHANNEL_TRACE");
    return value != nullptr && *value != '\0';
}

void trace_native_route_backend (const std::string &message)
{
    if (route_channel_trace_enabled ()) {
        std::cerr << "zlink route-backend " << message << '\n';
    }
}

framework_exception_t map_native_route_exception (const std::exception &error)
{
    if (const auto *request_error = dynamic_cast<const zlink::request_error_t *> (&error);
        request_error != nullptr) {
        if (request_error->result () == zlink::request_result_t::timed_out) {
            return detail::make_boundary_exception (
              detail::boundary_error_t::timed_out,
              "native route request timed out");
        }
        if (request_error->result () == zlink::request_result_t::not_found) {
            return framework_exception_t (framework_error_kind_t::not_found,
                                          request_error->what ());
        }
        if (request_error->result () == zlink::request_result_t::not_connected
            || is_route_unreachable_errno (request_error->internal_errno ())) {
            return framework_exception_t (framework_error_kind_t::unavailable,
                                          request_error->what ());
        }
        return framework_exception_t (framework_error_kind_t::internal_failure,
                                      request_error->what ());
    }
    if (const auto *submit_error = dynamic_cast<const zlink::submit_error_t *> (&error);
        submit_error != nullptr) {
        if (submit_error->result () == zlink::submit_result_t::not_connected
            || is_route_unreachable_errno (submit_error->internal_errno ())) {
            return framework_exception_t (framework_error_kind_t::unavailable,
                                          submit_error->what ());
        }
    }
    return framework_exception_t (framework_error_kind_t::internal_failure, error.what ());
}

struct route_request_callback_state_t
{
    std::mutex mutex;
    std::condition_variable changed;
    bool completed = false;
    zlink::request_result_t result = zlink::request_result_t::internal_error;
    std::vector<zlink::message_t> reply;
};

result_t<void> wait_for_route_request_callback (
  const std::shared_ptr<route_request_callback_state_t> &callback_state,
  std::chrono::milliseconds timeout,
  const std::function<bool ()> &stopping,
  const std::function<void ()> &progress = {})
{
    const auto has_deadline = timeout > std::chrono::milliseconds::zero ();
    const auto deadline = has_deadline ? std::chrono::steady_clock::now () + timeout
                                       : std::chrono::steady_clock::time_point::max ();
    while (true) {
        {
            std::lock_guard lock (callback_state->mutex);
            if (callback_state->completed) {
                return result_t<void>::success ();
            }
        }
        if (stopping && stopping ()) {
            return detail::boundary_failure<void> (detail::boundary_error_t::shutdown,
                                            "native route backend is shutting down");
        }
        if (progress) {
            progress ();
            std::lock_guard lock (callback_state->mutex);
            if (callback_state->completed) {
                return result_t<void>::success ();
            }
        }
        if (has_deadline && std::chrono::steady_clock::now () >= deadline) {
            return detail::boundary_failure<void> (detail::boundary_error_t::disconnected,
                                            "native route request disconnected before reply");
        }
        auto wait_time = std::chrono::milliseconds (50);
        if (has_deadline) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds> (
              deadline - std::chrono::steady_clock::now ());
            if (remaining < wait_time) {
                wait_time = remaining > std::chrono::milliseconds::zero ()
                              ? remaining
                              : std::chrono::milliseconds (1);
            }
        }
        std::unique_lock lock (callback_state->mutex);
        callback_state->changed.wait_for (lock, wait_time);
    }
}

} // namespace

native_route_backend_t::native_route_backend_t (zlink::router_socket_t &router) : _router (&router)
{
    _raw_port = std::make_shared<raw_route_port_t> (
      router, &_router_mutex, zlink::poll_event_flag_t::none);
}

native_route_backend_t::native_route_backend_t (zlink::router_socket_t &router,
                                                std::atomic_bool &stop,
                                                dispatch_options_t dispatch) :
    _router (&router), _stop (&stop), _dispatch (std::move (dispatch))
{
    _raw_port = std::make_shared<raw_route_port_t> (
      router, &_router_mutex, zlink::poll_event_flag_t::none);
}

result_t<void>
native_route_backend_t::submit_send (const zlink::routing_id_t &target_node_rid,
                                     const std::optional<std::string> &target_spot_id,
                                     const runtime::messaging::message_parts_t &parts)
{
    if (_router == nullptr) {
        return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                        "native route backend has no router socket");
    }
    auto copied = copy_parts (parts);
    if (copied.empty ()) {
        return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                        "native route send requires at least one message part");
    }
    try {
        if (target_spot_id) {
            return result_t<void>::failure (
              framework_error_kind_t::not_found,
              "legacy RouteChannel Spot bridge is unavailable; "
              "the stateful runtime must submit an exact Spot route fence");
        }
        std::shared_ptr<raw_route_port_t> raw_port;
        {
            std::lock_guard route_lock (_router_mutex);
            raw_port = _raw_port;
        }
        if (raw_port == nullptr) {
            return result_t<void>::failure (framework_error_kind_t::protocol_error,
                                            "native route backend has no router socket");
        }
        trace_native_route_backend (
          "router-send target=" + target_node_rid.to_string ()
          + " parts=" + std::to_string (copied.size ()));
        raw_message_t raw_parts;
        raw_parts.reserve (copied.size ());
        for (const auto &part : copied) {
            raw_parts.push_back (part.to_bytes ());
        }
        if (!raw_port->send (target_node_rid.to_bytes (), raw_parts)) {
            trace_native_route_backend ("router-send-submit-failed");
            return submit_failure ("native route send was not accepted");
        }
        trace_native_route_backend ("router-send-submitted");
        return result_t<void>::success ();
    }
    catch (const std::exception &ex) {
        const auto error = map_native_route_exception (ex);
        return detail::result_access_t::failure<void> (error);
    }
}

result_t<runtime::messaging::message_parts_t>
native_route_backend_t::submit_request (const zlink::routing_id_t &target_node_rid,
                                        const std::optional<std::string> &target_spot_id,
                                        const runtime::messaging::message_parts_t &parts,
                                        std::chrono::milliseconds timeout)
{
    if (_router == nullptr) {
        return result_t<runtime::messaging::message_parts_t>::failure (
          framework_error_kind_t::protocol_error,
          "native route backend has no router socket");
    }
    auto copied = copy_parts (parts);
    if (copied.empty ()) {
        return result_t<runtime::messaging::message_parts_t>::failure (
          framework_error_kind_t::protocol_error,
          "native route request requires at least one message part");
    }
    try {
        if (target_spot_id) {
            return result_t<runtime::messaging::message_parts_t>::failure (
              framework_error_kind_t::not_found,
              "legacy RouteChannel Spot bridge is unavailable; "
              "the stateful runtime must submit an exact Spot route fence");
        }
        auto callback_state = std::make_shared<route_request_callback_state_t> ();
        bool submitted = false;
        std::shared_ptr<raw_route_port_t> raw_port;
        {
            std::lock_guard route_lock (_router_mutex);
            raw_port = _raw_port;
        }
        if (raw_port != nullptr) {
            raw_message_t raw_parts;
            raw_parts.reserve (copied.size ());
            for (const auto &part : copied) {
                raw_parts.push_back (part.to_bytes ());
            }
            trace_native_route_backend (
              "router-request target=" + target_node_rid.to_string ()
              + " parts=" + std::to_string (copied.size ()));
            submitted = raw_port->request (
              target_node_rid.to_bytes (), raw_parts, timeout,
              [callback_state] (raw_request_result_t result,
                                raw_message_t reply) {
                  std::vector<zlink::message_t> materialized;
                  materialized.reserve (reply.size ());
                  for (const auto &part : reply) {
                      materialized.push_back (zlink::message_t::from (part));
                  }
                  zlink::request_result_t mapped =
                    zlink::request_result_t::internal_error;
                  switch (result) {
                      case raw_request_result_t::ok:
                          mapped = zlink::request_result_t::ok;
                          break;
                      case raw_request_result_t::timed_out:
                          mapped = zlink::request_result_t::timed_out;
                          break;
                      case raw_request_result_t::not_connected:
                          mapped = zlink::request_result_t::not_connected;
                          break;
                      case raw_request_result_t::terminated:
                          mapped = zlink::request_result_t::terminated;
                          break;
                      default:
                          break;
                  }
                  trace_native_route_backend (
                    "router-request-callback result="
                    + std::to_string (static_cast<int> (mapped))
                    + " parts=" + std::to_string (materialized.size ()));
                  {
                      std::lock_guard lock (callback_state->mutex);
                      callback_state->result = mapped;
                      callback_state->reply = std::move (materialized);
                      callback_state->completed = true;
                  }
                  callback_state->changed.notify_all ();
              });
        }
        if (!submitted) {
            trace_native_route_backend ("router-request-submit-failed");
            return result_t<runtime::messaging::message_parts_t>::failure (
              framework_error_kind_t::internal_failure, "native route request was not submitted");
        }
        trace_native_route_backend ("router-request-submitted");
        if (auto waited = wait_for_route_request_callback (callback_state, timeout,
                                                           [this] { return stopping (); },
                                                           [] {});
            !waited) {
            trace_native_route_backend (
              "router-request-wait-failed error="
              + std::string (waited.error () ? waited.error ()->what ()
                                             : "native route request failed"));
            return detail::propagate_failure<runtime::messaging::message_parts_t> (waited, "native route request failed");
        }
        if (callback_state->result != zlink::request_result_t::ok) {
            throw zlink::request_error_t (callback_state->result);
        }
        auto reply = std::move (callback_state->reply);
        return result_t<runtime::messaging::message_parts_t>::success (
          runtime::messaging::message_parts_t (std::move (reply)));
    }
    catch (const std::exception &ex) {
        const auto error = map_native_route_exception (ex);
        return detail::result_access_t::failure<runtime::messaging::message_parts_t> (error);
    }
}

bool native_route_backend_t::handle_router_received (const zlink::routing_id_t &source_node_rid,
                                                     std::vector<zlink::message_t> &parts,
                                                     std::optional<std::uint64_t> request_seq)
{
    static_cast<void> (source_node_rid);
    static_cast<void> (parts);
    static_cast<void> (request_seq);
    return false;
}

void native_route_backend_t::close () noexcept
{
    std::shared_ptr<raw_route_port_t> raw_port;
    {
        std::lock_guard route_lock (_router_mutex);
        raw_port = std::move (_raw_port);
        _router = nullptr;
    }
    if (raw_port) {
        raw_port->close ();
    }
}

std::mutex &native_route_backend_t::router_mutex () noexcept
{
    return _router_mutex;
}

bool native_route_backend_t::stopping () const noexcept
{
    return _stop != nullptr && _stop->load (std::memory_order_acquire);
}

} // namespace zlink::framework::detail::backend
