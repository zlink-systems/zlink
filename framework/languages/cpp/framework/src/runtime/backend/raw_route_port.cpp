/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/backend/raw_route_port.hpp"
#include "runtime/backend/raw_binding_adapter.hpp"

#include <zlink/Contracts/Messaging/operation_contracts.hpp>
#include <zlink/Contracts/Messaging/received.hpp>
#include <zlink/Contracts/Sockets/routed_socket_contracts.hpp>

#include <cerrno>
#include <stdexcept>
#include <string>
#include <utility>

namespace zlink::framework::detail::backend
{
namespace
{
const char *submit_result_name (zlink::submit_result_t result) noexcept
{
    switch (result) {
        case zlink::submit_result_t::ok: return "ok";
        case zlink::submit_result_t::backpressured: return "backpressured";
        case zlink::submit_result_t::not_connected: return "not_connected";
        case zlink::submit_result_t::not_found: return "not_found";
        case zlink::submit_result_t::terminated: return "terminated";
        case zlink::submit_result_t::invalid_handle: return "invalid_handle";
        case zlink::submit_result_t::invalid_argument: return "invalid_argument";
        case zlink::submit_result_t::not_supported: return "not_supported";
        case zlink::submit_result_t::invalid_state: return "invalid_state";
        case zlink::submit_result_t::thread_violation: return "thread_violation";
        case zlink::submit_result_t::out_of_memory: return "out_of_memory";
        case zlink::submit_result_t::seq_exhausted: return "seq_exhausted";
        case zlink::submit_result_t::internal_error: return "internal_error";
        case zlink::submit_result_t::not_admitted: return "not_admitted";
    }
    return "unknown";
}

struct binding_completion_observer_t
{
    struct promise_type
    {
        binding_completion_observer_t get_return_object () noexcept { return {}; }
        std::suspend_never initial_suspend () noexcept { return {}; }
        std::suspend_never final_suspend () noexcept { return {}; }
        void return_void () noexcept {}
        void unhandled_exception () noexcept { std::terminate (); }

        detail::task_scheduler_t zlink_continuation_scheduler () const
        {
            // Binding completion already arrives on its owning completion
            // resource. Raw transport classification runs there and hands the
            // terminal directly to the reserved Framework dispatcher item.
            return {};
        }
    };
};

binding_completion_observer_t observe_send_completion (
  zlink::async_result_t<void> pending,
  raw_send_stage_trace_t trace,
  std::shared_ptr<detail::task_completion_source_t<zlink::submit_result_t>> source)
{
    try {
        co_await std::move (pending);
        if (trace) {
            try {
                trace ("router_admission_submit", "admitted");
                trace ("router_admission_complete", "ok");
            }
            catch (const std::exception &error) {
                source->complete (result_t<zlink::submit_result_t>::failure (
                  framework_error_kind_t::internal_failure,
                  std::string ("raw route send completion trace failed: ") + error.what ()));
                co_return;
            }
            catch (...) {
                source->complete (result_t<zlink::submit_result_t>::failure (
                  framework_error_kind_t::internal_failure,
                  "raw route send completion trace failed"));
                co_return;
            }
        }
        source->complete (result_t<zlink::submit_result_t>::success (
          zlink::submit_result_t::ok));
    }
    catch (const zlink::submit_error_t &error) {
        if (trace) {
            try {
                trace ("router_admission_submit", submit_result_name (error.result ()));
                trace ("router_admission_complete", submit_result_name (error.result ()));
            }
            catch (const std::exception &trace_error) {
                source->complete (result_t<zlink::submit_result_t>::failure (
                  framework_error_kind_t::internal_failure,
                  std::string ("raw route send completion trace failed: ")
                    + trace_error.what ()));
                co_return;
            }
            catch (...) {
                source->complete (result_t<zlink::submit_result_t>::failure (
                  framework_error_kind_t::internal_failure,
                  "raw route send completion trace failed"));
                co_return;
            }
        }
        source->complete (result_t<zlink::submit_result_t>::success (error.result ()));
    }
    catch (const std::exception &error) {
        source->complete (result_t<zlink::submit_result_t>::failure (
          framework_error_kind_t::internal_failure, error.what ()));
    }
    catch (...) {
        source->complete (result_t<zlink::submit_result_t>::failure (
          framework_error_kind_t::internal_failure,
          "raw route send completion failed"));
    }
}

binding_completion_observer_t observe_request_completion (
  zlink::async_result_t<std::vector<zlink::message_t>> pending,
  std::shared_ptr<detail::task_completion_source_t<raw_request_completion_t>> source)
{
    try {
        auto reply = co_await std::move (pending);
        source->complete (result_t<raw_request_completion_t>::success (
          raw_request_completion_t{
            raw_request_result_t::ok, copy_binding_parts (reply)}));
    }
    catch (const zlink::request_error_t &error) {
        source->complete (result_t<raw_request_completion_t>::success (
          raw_request_completion_t{
            map_binding_request_result (error.result ()), {},
            raw_request_failure_t{
              raw_request_failure_phase_t::completion_terminal,
              std::nullopt, error.result (), error.internal_errno ()}}));
    }
    catch (const zlink::submit_error_t &error) {
        const auto result = error.result () == zlink::submit_result_t::terminated
                              ? raw_request_result_t::terminated
                              : raw_request_result_t::failed;
        source->complete (result_t<raw_request_completion_t>::success (
          raw_request_completion_t{
            result, {},
            raw_request_failure_t{
              raw_request_failure_phase_t::completion_terminal,
              error.result (), std::nullopt, error.internal_errno ()}}));
    }
    catch (const std::exception &error) {
        source->complete (result_t<raw_request_completion_t>::failure (
          framework_error_kind_t::internal_failure, error.what ()));
    }
    catch (...) {
        source->complete (result_t<raw_request_completion_t>::failure (
          framework_error_kind_t::internal_failure,
          "raw route request completion failed"));
    }
}
}

raw_route_port_t::raw_route_port_t (zlink::router_socket_t &socket,
                                    std::mutex *shared_socket_mutex,
                                    zlink::poll_event_flag_t receive_events,
                                    zlink::poller_t *shared_poller,
                                    std::uintptr_t poller_slot) :
    _owned_poller (shared_poller == nullptr
                     ? std::make_unique<zlink::poller_t> ()
                     : nullptr),
    _poller (shared_poller != nullptr ? shared_poller : _owned_poller.get ()),
    _poller_slot (poller_slot == 0 ? 1 : poller_slot),
    _socket (&socket),
    _socket_mutex (shared_socket_mutex != nullptr ? shared_socket_mutex
                                                  : &_owned_socket_mutex),
    _receive_events (receive_events)
{
    _poller->add (
      socket,
      _receive_events | zlink::poll_event_flag_t::pollout
        | zlink::poll_event_flag_t::pollcompletion,
      _poller_slot);
    _wake_timer.attach (*_poller);
}

task_t<zlink::submit_result_t> raw_route_port_t::send_result (
  const raw_bytes_t &target_routing_id,
  raw_message_t parts,
  raw_send_stage_trace_t trace)
{
    auto source =
      std::make_shared<detail::task_completion_source_t<zlink::submit_result_t>> ();
    auto result = source->task ();
    // The binding owns DONTWAIT backpressure retry and its payload snapshot.
    // Submit under the socket lock. Its terminal observer deliberately has no
    // handler-executor scheduler, so completion classification stays on the
    // binding completion resource.
    try {
        if (target_routing_id.empty () || parts.empty ())
            throw std::invalid_argument (
              "raw route send requires a target and message parts");
        auto messages = materialize_binding_parts (std::move (parts));
        if (trace) {
            try {
                trace ("router_admission_submit", "begin");
            }
            catch (const std::exception &error) {
                source->complete (result_t<zlink::submit_result_t>::failure (
                  framework_error_kind_t::internal_failure,
                  std::string ("raw route send submission trace failed: ") + error.what ()));
                return result;
            }
            catch (...) {
                source->complete (result_t<zlink::submit_result_t>::failure (
                  framework_error_kind_t::internal_failure,
                  "raw route send submission trace failed"));
                return result;
            }
        }
        std::optional<zlink::async_result_t<void>> pending;
        {
            std::lock_guard lock (*_socket_mutex);
            if (_socket == nullptr) {
                if (trace)
                    trace ("router_admission_submit", "terminated");
                source->complete (result_t<zlink::submit_result_t>::success (
                  zlink::submit_result_t::terminated));
                return result;
            }
            auto operation = std::move (_socket->send (
                                          zlink::routing_id_t::from (target_routing_id)))
                               .message (messages[0]);
            for (std::size_t index = 1; index < messages.size (); ++index) {
                operation = std::move (operation).message (messages[index]);
            }
            pending.emplace (std::move (operation).async ().admitted);
        }
        observe_send_completion (
          std::move (*pending), std::move (trace), source);
    }
    catch (const zlink::submit_error_t &error) {
        if (trace) {
            try {
                trace ("router_admission_submit", submit_result_name (error.result ()));
                trace ("router_admission_complete", submit_result_name (error.result ()));
            }
            catch (const std::exception &trace_error) {
                source->complete (result_t<zlink::submit_result_t>::failure (
                  framework_error_kind_t::internal_failure,
                  std::string ("raw route send submission trace failed: ")
                    + trace_error.what ()));
                return result;
            }
            catch (...) {
                source->complete (result_t<zlink::submit_result_t>::failure (
                  framework_error_kind_t::internal_failure,
                  "raw route send submission trace failed"));
                return result;
            }
        }
        source->complete (result_t<zlink::submit_result_t>::success (error.result ()));
    }
    catch (const std::exception &error) {
        if (trace) {
            try {
                trace ("router_admission_submit", "exception");
            }
            catch (const std::exception &trace_error) {
                source->complete (result_t<zlink::submit_result_t>::failure (
                  framework_error_kind_t::internal_failure,
                  std::string ("raw route send submission trace failed: ")
                    + trace_error.what ()));
                return result;
            }
            catch (...) {
                source->complete (result_t<zlink::submit_result_t>::failure (
                  framework_error_kind_t::internal_failure,
                  "raw route send submission trace failed"));
                return result;
            }
        }
        source->complete (result_t<zlink::submit_result_t>::failure (
          framework_error_kind_t::internal_failure, error.what ()));
    }
    catch (...) {
        source->complete (result_t<zlink::submit_result_t>::failure (
          framework_error_kind_t::internal_failure,
          "raw route send submission failed"));
    }
    return result;
}

task_t<bool> raw_route_port_t::send (const raw_bytes_t &target_routing_id,
                                     raw_message_t parts)
{
    co_return co_await send_result (target_routing_id, std::move (parts))
              == zlink::submit_result_t::ok;
}

task_t<raw_request_completion_t> raw_route_port_t::request (
  const raw_bytes_t &target_routing_id,
  raw_message_t parts,
  std::chrono::milliseconds timeout)
{
    auto source =
      std::make_shared<detail::task_completion_source_t<raw_request_completion_t>> ();
    auto result = source->task ();
    // Route selection happens synchronously inside .async(). Keep that initial
    // admission boundary separate from the pending terminal: the same errno
    // has different meaning after Core has issued a WRITABLE wait token.
    try {
        if (target_routing_id.empty () || parts.empty ()
            || timeout <= std::chrono::milliseconds::zero ()) {
            throw std::invalid_argument (
              "raw route request requires target, parts and timeout");
        }
        auto messages = materialize_binding_parts (std::move (parts));
        std::optional<zlink::async_result_t<std::vector<zlink::message_t>>> pending;
        {
            std::lock_guard lock (*_socket_mutex);
            if (_socket == nullptr) {
                source->complete (result_t<raw_request_completion_t>::success (
                  raw_request_completion_t{
                    raw_request_result_t::terminated, {}}));
                return result;
            }
            auto operation = std::move (_socket->request (
                                          zlink::routing_id_t::from (target_routing_id)))
                               .message (messages[0]);
            for (std::size_t index = 1; index < messages.size (); ++index) {
                operation = std::move (operation).message (messages[index]);
            }
            pending.emplace (std::move (operation).timeout (timeout).async ().reply);
        }
        observe_request_completion (std::move (*pending), source);
    }
    catch (const zlink::submit_error_t &error) {
        const auto phase = raw_request_failure_phase_t::initial_admission;
        const auto result =
          transient_route_failure (error.result (), error.internal_errno (), phase)
            ? raw_request_result_t::route_unavailable
          : error.result () == zlink::submit_result_t::terminated
            ? raw_request_result_t::terminated
            : raw_request_result_t::failed;
        source->complete (result_t<raw_request_completion_t>::success (
          raw_request_completion_t{
            result, {},
            raw_request_failure_t{
              phase, error.result (), std::nullopt, error.internal_errno ()}}));
    }
    catch (const std::exception &error) {
        source->complete (result_t<raw_request_completion_t>::failure (
          framework_error_kind_t::internal_failure, error.what ()));
    }
    catch (...) {
        source->complete (result_t<raw_request_completion_t>::failure (
          framework_error_kind_t::internal_failure,
          "raw route request submission failed"));
    }
    return result;
}

zlink::poll_event_flag_t raw_route_port_t::poll (
  std::chrono::milliseconds timeout, bool accept_application_receive)
{
    std::lock_guard lock (_poller_mutex);
    if (_socket == nullptr)
        return zlink::poll_event_flag_t::none;
    zlink::poll_event_t events[3];
    const auto completion_events =
      zlink::poll_event_flag_t::pollout | zlink::poll_event_flag_t::pollcompletion;
    if (!accept_application_receive)
        _poller->modify (*_socket, completion_events);
    std::size_t count;
    try {
        count = _poller->wait (events, 3, timeout);
    }
    catch (...) {
        if (!accept_application_receive)
            _poller->modify (*_socket, _receive_events | completion_events);
        throw;
    }
    if (!accept_application_receive)
        _poller->modify (*_socket, _receive_events | completion_events);
    auto readiness = zlink::poll_event_flag_t::none;
    bool wake = false;
    for (int index = 0; index < count; ++index) {
        if (_wake_timer.is_event (events[index])) {
            _wake_timer.consume ();
            wake = true;
        }
        else if (events[index].slot == _poller_slot) {
            readiness = static_cast<zlink::poll_event_flag_t> (
              static_cast<short> (readiness)
              | (static_cast<short> (events[index].revents)
                 & static_cast<short> (_receive_events)));
        }
        else {
            // Shared-poller sources (the mesh monitor) wake management;
            // their owner drains them without claiming ordinary records.
            wake = true;
        }
    }
    return readiness != zlink::poll_event_flag_t::none
             ? readiness
             : wake ? zlink::poll_event_flag_t::pollin
                    : zlink::poll_event_flag_t::none;
}

void raw_route_port_t::signal_activity () noexcept
{
    _wake_timer.signal ();
}

std::optional<raw_received_t> raw_route_port_t::receive_if_ready (
  zlink::poll_event_flag_t revents)
{
    std::lock_guard lock (*_socket_mutex);
    if (_socket == nullptr
        || (static_cast<short> (revents)
            & static_cast<short> (zlink::poll_event_flag_t::pollin))
             == 0) {
        return std::nullopt;
    }
    const int result = _socket->recv (
      _received, zlink::recv_flags_t::dontwait);
    if (result == static_cast<int> (zlink::recv_result_t::no_data)) {
        return std::nullopt;
    }
    if (result == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return std::nullopt;
    }
    if (result != 0) {
        throw std::runtime_error (
          "raw route receive failed with result "
          + std::to_string (result) + " and errno "
          + std::to_string (errno));
    }
    if (!_received.routing_id ()) {
        throw std::runtime_error ("raw ROUTER receive omitted source routing id");
    }
    auto source_routing_id = _received.routing_id ()->to_bytes ();
    auto reply_token = _received.reply_token ();
    auto parts = copy_binding_parts (_received.parts ());
    _received.close ();
    return raw_received_t{std::move (source_routing_id), std::move (reply_token),
                          std::move (parts)};
}

std::optional<raw_received_t> raw_route_port_t::try_receive ()
{
    return receive_if_ready (poll (std::chrono::milliseconds::zero ()));
}

bool raw_route_port_t::reply (
  const raw_received_t &request, raw_message_t parts)
{
    if (request.source_routing_id.empty () || !request.reply_token
        || parts.empty ()) {
        throw std::invalid_argument (
          "raw route reply requires request context and message parts");
    }
    std::lock_guard lock (*_socket_mutex);
    if (_socket == nullptr)
        return false;
    auto messages = materialize_binding_parts (std::move (parts));
    auto operation = std::move (
      _socket->reply (
        zlink::routing_id_t::from (request.source_routing_id),
        *request.reply_token))
                       .message (messages[0]);
    for (std::size_t index = 1; index < messages.size (); ++index)
        operation = std::move (operation).message (messages[index]);
    try {
        std::move (operation).submit ();
        return true;
    }
    catch (const zlink::submit_error_t &) {
        return false;
    }
}

void raw_route_port_t::close () noexcept
{
    _wake_timer.signal ();
    std::scoped_lock lock (_poller_mutex, *_socket_mutex);
    _wake_timer.detach ();
    auto *socket = _socket;
    _socket = nullptr;
    try {
        if (_owned_poller) {
            _owned_poller->close ();
        } else if (socket != nullptr) {
            _poller->remove (*socket);
        }
    }
    catch (...) {
    }
}

} // namespace zlink::framework::detail::backend
