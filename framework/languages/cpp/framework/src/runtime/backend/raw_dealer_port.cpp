/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

#include "runtime/backend/raw_dealer_port.hpp"
#include "runtime/backend/raw_binding_adapter.hpp"

#include <zlink/Contracts/Eventing/poll_event.hpp>
#include <zlink/Contracts/Eventing/poller.hpp>
#include <zlink/Contracts/Errors/errors.hpp>
#include <zlink/Contracts/Messaging/message.hpp>
#include <zlink/Contracts/Messaging/operation_contracts.hpp>
#include <zlink/Contracts/Messaging/received.hpp>
#include <zlink/Contracts/Messaging/request_result.hpp>
#include <zlink/Contracts/Sockets/message_socket_contracts.hpp>
#include <zlink/Contracts/Sockets/results.hpp>

#include <cerrno>
#include <stdexcept>
#include <utility>

namespace zlink::framework::detail::backend
{
raw_dealer_port_t::raw_dealer_port_t (zlink::dealer_socket_t &socket,
                                      std::shared_ptr<std::mutex> shared_socket_mutex,
                                      zlink::poller_t *shared_poller,
                                      std::uintptr_t poller_slot,
                                      runtime::state_lane_t *owner,
                                      std::function<void ()> deadline_changed) :
    _requests (owner ? std::make_shared<runtime::foundation::operation_registry_t> (
                         *owner, &std::chrono::steady_clock::now, std::move (deadline_changed))
                     : std::make_shared<runtime::foundation::operation_registry_t> (
                         &std::chrono::steady_clock::now, std::move (deadline_changed))),
    _owned_poller (shared_poller == nullptr ? std::make_unique<zlink::poller_t> () : nullptr),
    _poller (shared_poller != nullptr ? shared_poller : _owned_poller.get ()),
    _poller_slot (poller_slot == 0 ? 1 : poller_slot),
    _socket (&socket),
    _socket_mutex (shared_socket_mutex ? std::move (shared_socket_mutex)
                                       : std::make_shared<std::mutex> ())
{
    _poller->add (socket,
                  zlink::poll_event_flag_t::pollin | zlink::poll_event_flag_t::pollout
                    | zlink::poll_event_flag_t::pollcompletion,
                  _poller_slot);
}

raw_dealer_port_t::~raw_dealer_port_t () noexcept
{
    _requests->shutdown ();
}

task_t<bool> raw_dealer_port_t::send (const raw_message_t &parts)
{
    co_return (co_await send_result (parts)) == zlink::submit_result_t::ok;
}

task_t<zlink::submit_result_t> raw_dealer_port_t::send_result (const raw_message_t &parts)
{
    return send_result (materialize_binding_parts (parts));
}

task_t<zlink::submit_result_t>
raw_dealer_port_t::send_result (std::vector<zlink::message_t> messages)
{
    if (messages.empty ())
        throw std::invalid_argument ("raw dealer send requires message parts");
    std::optional<zlink::async_result_t<void>> pending;
    try {
        {
            std::lock_guard lock (*_socket_mutex);
            if (!_socket) {
                co_return zlink::submit_result_t::terminated;
            }
            auto operation = std::move (_socket->send ()).message (messages[0]);
            for (std::size_t index = 1; index < messages.size (); ++index) {
                operation = std::move (operation).message (messages[index]);
            }
            auto submission = std::move (operation).async ();
            pending = take_submission_admission (submission);
        }
        if (pending)
            co_await std::move (*pending);
        co_return zlink::submit_result_t::ok;
    }
    catch (const zlink::submit_error_t &error) {
        co_return pending ? runtime::messaging::map_submit_completion_result (error.result ())
                          : error.result ();
    }
}

task_t<raw_request_completion_t>
raw_dealer_port_t::request (const raw_message_t &parts,
                            std::chrono::milliseconds timeout,
                            runtime::mesh::service_liveness_registry_t::connection_t admission,
                            std::shared_ptr<task_completion_source_t<void>> admitted,
                            std::optional<std::chrono::steady_clock::time_point> deadline)
{
    return request (materialize_binding_parts (parts), timeout, std::move (admission),
                    std::move (admitted), deadline);
}

task_t<raw_request_completion_t>
raw_dealer_port_t::request (std::vector<zlink::message_t> messages,
                            std::chrono::milliseconds timeout,
                            runtime::mesh::service_liveness_registry_t::connection_t admission,
                            std::shared_ptr<task_completion_source_t<void>> admitted,
                            std::optional<std::chrono::steady_clock::time_point> deadline)
try {
    if (messages.empty () || timeout <= std::chrono::milliseconds::zero ())
        throw std::invalid_argument ("raw dealer request requires parts and timeout");
    const auto wait_owner = detail::current_wait_owner ();
    const auto expires_at = deadline ? *deadline : std::chrono::steady_clock::now () + timeout;
    auto source = std::make_shared<task_completion_source_t<raw_request_completion_t>> ();
    auto observation = source->task ();
    observe_raw_request_admission_failure (observation, admitted);
    const runtime::call_id_t id{0, _next_request.fetch_add (1, std::memory_order_relaxed)};
    const auto registered = _requests->register_operation (
      id, expires_at,
      [source] (runtime::foundation::operation_terminal_t terminal,
                ::zlink::framework::result_t<zlink::message_t>) {
          if (terminal == runtime::foundation::operation_terminal_t::cancelled) {
              source->complete (detail::result_access_t::failure<raw_request_completion_t> (
                detail::make_cancellation_exception ("request was cancelled")));
              return;
          }
          const auto error = terminal == runtime::foundation::operation_terminal_t::timed_out
                               ? detail::boundary_error_t::timed_out
                               : detail::boundary_error_t::shutdown;
          source->complete (detail::boundary_failure<raw_request_completion_t> (
            error, terminal == runtime::foundation::operation_terminal_t::timed_out
                     ? "request deadline expired"
                     : "raw dealer is closed"));
      },
      {}, {}, {}, wait_owner);
    if (!registered) {
        source->complete (detail::boundary_failure<raw_request_completion_t> (
          detail::boundary_error_t::shutdown, "raw dealer is closed"));
        return source->task ();
    }
    if (wait_owner.stop_requested ())
        return source->task ();
    const auto finish = [requests = _requests, id,
                         source] (result_t<raw_request_completion_t> result) {
        if (requests->unregister (id))
            source->complete (std::move (result));
    };
    std::optional<request_submission_stages_t> stages;
    bool closed = false;
    try {
        {
            std::lock_guard lock (*_socket_mutex);
            closed = !_socket;
            if (!closed) {
                const auto remaining = std::chrono::ceil<std::chrono::milliseconds> (
                  expires_at - std::chrono::steady_clock::now ());
                if (remaining > std::chrono::milliseconds::zero ()) {
                    auto operation = std::move (_socket->request ()).message (messages[0]);
                    for (std::size_t index = 1; index < messages.size (); ++index) {
                        operation = std::move (operation).message (messages[index]);
                    }
                    stages.emplace (submit_request_once (
                      [&] { return std::move (operation).timeout (remaining).async (); }));
                }
            }
        }
    }
    catch (const zlink::submit_error_t &error) {
        finish (raw_request_failure_result (error, raw_request_failure_phase_t::initial_admission));
        return source->task ();
    }
    catch (...) {
        finish (
          detail::result_access_t::failure<raw_request_completion_t> (std::current_exception ()));
        return source->task ();
    }
    if (closed) {
        finish (result_t<raw_request_completion_t>::success (
          raw_request_completion_t{zlink::request_result_t::terminated, {}}));
        return source->task ();
    }
    if (!stages) {
        return source->task ();
    }
    observe_request_completion (std::move (*stages), source, std::move (admission), admitted,
                                _requests, id);
    return source->task ();
}
catch (...) {
    return task_t<raw_request_completion_t> (
      detail::current_exception_result<raw_request_completion_t> ());
}

std::optional<raw_message_t> raw_dealer_port_t::try_receive ()
{
    std::lock_guard lock (*_socket_mutex);
    if (!_socket) {
        return std::nullopt;
    }
    // Only a poller's owner waits on it. A shared transport poller belongs to the runtime
    // worker, so this port reads its socket directly and the non-blocking receive below
    // reports an empty socket.
    if (_owned_poller) {
        zlink::poll_event_t event;
        if (_poller->wait (&event, 1, std::chrono::milliseconds::zero ()) != 1
            || event.slot != _poller_slot
            || (static_cast<short> (event.revents)
                & static_cast<short> (zlink::poll_event_flag_t::pollin))
                 == 0) {
            return std::nullopt;
        }
    }
    const auto result = _socket->recv (_received, zlink::recv_flags_t::dontwait);
    if (result == static_cast<int> (zlink::recv_result_t::no_data)
        || result == static_cast<int> (zlink::recv_result_t::busy)
        || (result == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))) {
        return std::nullopt;
    }
    if (result != 0) {
        throw std::runtime_error ("raw dealer receive failed");
    }
    binding_received_release_t received_release (_received);
    auto parts = copy_binding_parts (_received.parts ());
    return parts;
}

task_t<std::size_t> raw_dealer_port_t::expire_requests (std::chrono::steady_clock::time_point now)
{
    return _requests->expire (now);
}

std::optional<std::chrono::steady_clock::time_point>
raw_dealer_port_t::next_request_deadline () const
{
    return _requests->next_deadline ();
}

void raw_dealer_port_t::close ()
{
    _requests->shutdown ();
    std::lock_guard lock (*_socket_mutex);
    auto *socket = _socket;
    if (_owned_poller) {
        _owned_poller->close ();
    } else if (socket != nullptr) {
        _poller->remove (*socket);
    }
    _socket = nullptr;
}

} // namespace zlink::framework::detail::backend
