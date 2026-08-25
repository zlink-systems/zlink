/* SPDX-License-Identifier: FSL-1.1-ALv2 */

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
raw_dealer_port_t::raw_dealer_port_t (
  zlink::dealer_socket_t &socket,
  std::mutex *shared_socket_mutex,
  zlink::poller_t *shared_poller,
  std::uintptr_t poller_slot) :
    _owned_poller (shared_poller == nullptr
                     ? std::make_unique<zlink::poller_t> ()
                     : nullptr),
    _poller (shared_poller != nullptr ? shared_poller : _owned_poller.get ()),
    _poller_slot (poller_slot == 0 ? 1 : poller_slot),
    _socket (&socket),
    _socket_mutex (shared_socket_mutex != nullptr ? shared_socket_mutex
                                                  : &_owned_socket_mutex)
{
    _poller->add (socket, zlink::poll_event_flag_t::pollin, _poller_slot);
}

task_t<bool> raw_dealer_port_t::send (const raw_message_t &parts)
{
    if (parts.empty ()) {
        throw std::invalid_argument (
          "raw dealer send requires message parts");
    }
    auto messages = materialize_binding_parts (parts);
    std::optional<zlink::async_result_t<void>> pending;
    try {
        {
            std::lock_guard lock (*_socket_mutex);
            if (!_socket) {
                co_return false;
            }
            auto operation =
              std::move (_socket->send ()).message (messages[0]);
            for (std::size_t index = 1; index < messages.size (); ++index) {
                operation = std::move (operation).message (messages[index]);
            }
            pending.emplace (std::move (operation).async ());
        }
        co_await std::move (*pending);
        co_return true;
    }
    catch (const zlink::submit_error_t &) {
        co_return false;
    }
}

task_t<zlink::submit_result_t> raw_dealer_port_t::send (
  const raw_message_t &parts,
  std::chrono::milliseconds timeout)
{
    if (parts.empty () || timeout <= std::chrono::milliseconds::zero ()) {
        throw std::invalid_argument (
          "raw dealer send requires message parts and a positive timeout");
    }
    auto messages = materialize_binding_parts (parts);
    std::optional<zlink::async_result_t<void>> pending;
    try {
        {
            std::lock_guard lock (*_socket_mutex);
            if (!_socket) {
                co_return zlink::submit_result_t::terminated;
            }
            auto operation =
              std::move (_socket->send ()).message (messages[0]);
            for (std::size_t index = 1; index < messages.size (); ++index) {
                operation = std::move (operation).message (messages[index]);
            }
            pending.emplace (
              std::move (operation).timeout (timeout).async ());
        }
        co_await std::move (*pending);
        co_return zlink::submit_result_t::ok;
    }
    catch (const zlink::submit_error_t &error) {
        co_return error.result ();
    }
}

task_t<raw_request_completion_t> raw_dealer_port_t::request (
  const raw_message_t &parts,
  std::chrono::milliseconds timeout)
{
    if (parts.empty ()
        || timeout <= std::chrono::milliseconds::zero ()) {
        throw std::invalid_argument (
          "raw dealer request requires parts and timeout");
    }
    auto messages = materialize_binding_parts (parts);
    std::optional<zlink::async_result_t<std::vector<zlink::message_t>>> pending;
    {
        std::lock_guard lock (*_socket_mutex);
        if (!_socket) {
            co_return raw_request_completion_t{
              raw_request_result_t::terminated, {}};
        }
        auto operation =
          std::move (_socket->request ()).message (messages[0]);
        for (std::size_t index = 1; index < messages.size (); ++index) {
            operation = std::move (operation).message (messages[index]);
        }
        pending.emplace (std::move (operation).timeout (timeout).async ());
    }
    try {
        auto reply = co_await std::move (*pending);
        co_return raw_request_completion_t{
          raw_request_result_t::ok, copy_binding_parts (reply)};
    }
    catch (const zlink::request_error_t &error) {
        co_return raw_request_completion_t{
          map_binding_request_result (error.result ()), {}};
    }
    catch (const zlink::submit_error_t &error) {
        const auto result =
          error.result () == zlink::submit_result_t::backpressured
            ? raw_request_result_t::timed_out
          : error.result () == zlink::submit_result_t::not_connected
            ? raw_request_result_t::not_connected
          : error.result () == zlink::submit_result_t::terminated
            ? raw_request_result_t::terminated
            : raw_request_result_t::failed;
        co_return raw_request_completion_t{result, {}};
    }
}

std::optional<raw_message_t> raw_dealer_port_t::try_receive ()
{
    std::lock_guard lock (*_socket_mutex);
    if (!_socket) {
        return std::nullopt;
    }
    zlink::poll_event_t event;
    if (_poller->wait (&event, 1, std::chrono::milliseconds::zero ()) != 1
        || event.slot != _poller_slot
        || (static_cast<short> (event.revents)
            & static_cast<short> (zlink::poll_event_flag_t::pollin))
             == 0) {
        return std::nullopt;
    }
    const auto result =
      _socket->recv (_received, zlink::recv_flags_t::dontwait);
    if (result == static_cast<int> (zlink::recv_result_t::no_data)
        || result == static_cast<int> (zlink::recv_result_t::busy)
        || (result == -1
            && (errno == EAGAIN || errno == EWOULDBLOCK))) {
        return std::nullopt;
    }
    if (result != 0) {
        throw std::runtime_error ("raw dealer receive failed");
    }
    binding_received_release_t received_release (_received);
    auto parts = copy_binding_parts (_received.parts ());
    return parts;
}

void raw_dealer_port_t::close () noexcept
{
    std::lock_guard lock (*_socket_mutex);
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
