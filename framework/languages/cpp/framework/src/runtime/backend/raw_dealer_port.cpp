/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/backend/raw_dealer_port.hpp"

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
namespace
{

raw_message_t copy_parts (
  const std::vector<zlink::message_t> &parts)
{
    raw_message_t result;
    result.reserve (parts.size ());
    for (const auto &part : parts) {
        result.push_back (part.to_bytes ());
    }
    return result;
}

std::vector<zlink::message_t> materialize_parts (
  const raw_message_t &parts)
{
    std::vector<zlink::message_t> result;
    result.reserve (parts.size ());
    for (const auto &part : parts) {
        result.push_back (zlink::message_t::from (part));
    }
    return result;
}

raw_request_result_t map_request_result (
  zlink::request_result_t result) noexcept
{
    switch (result) {
        case zlink::request_result_t::ok:
            return raw_request_result_t::ok;
        case zlink::request_result_t::timed_out:
            return raw_request_result_t::timed_out;
        case zlink::request_result_t::not_connected:
            return raw_request_result_t::not_connected;
        case zlink::request_result_t::terminated:
            return raw_request_result_t::terminated;
        default:
            return raw_request_result_t::failed;
    }
}

} // namespace

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
    // The same poller owns both raw receive readiness and asynchronous
    // request-reply completion callbacks for this DEALER socket.
    _poller->add (
      socket,
      zlink::poll_event_flag_t::pollin
        | zlink::poll_event_flag_t::pollcompletion,
      _poller_slot);
}

bool raw_dealer_port_t::send (const raw_message_t &parts)
{
    if (parts.empty ()) {
        throw std::invalid_argument (
          "raw dealer send requires message parts");
    }
    std::lock_guard lock (*_socket_mutex);
    if (!_socket) {
        return false;
    }
    auto messages = materialize_parts (parts);
    auto operation = std::move (_socket->send ()).message (messages[0]);
    for (std::size_t index = 1; index < messages.size (); ++index) {
        operation = std::move (operation).message (messages[index]);
    }
    try {
        return std::move (operation).submit ();
    }
    catch (const zlink::submit_error_t &) {
        return false;
    }
}

bool raw_dealer_port_t::request (
  const raw_message_t &parts,
  std::chrono::milliseconds timeout,
  raw_route_port_t::request_callback_t callback)
{
    if (parts.empty () || !callback) {
        throw std::invalid_argument (
          "raw dealer request requires parts and callback");
    }
    std::lock_guard lock (*_socket_mutex);
    if (!_socket) {
        return false;
    }
    auto messages = materialize_parts (parts);
    auto operation =
      std::move (_socket->request ()).message (messages[0]);
    for (std::size_t index = 1; index < messages.size (); ++index) {
        operation = std::move (operation).message (messages[index]);
    }
    try {
        return std::move (operation).timeout (timeout).submit (
          [callback = std::move (callback)] (
            zlink::request_result_t result,
            std::vector<zlink::message_t> reply) mutable {
              callback (map_request_result (result), copy_parts (reply));
          });
    }
    catch (const zlink::submit_error_t &) {
        return false;
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
    zlink::received_t received;
    const auto result =
      _socket->recv (received, zlink::recv_flags_t::dontwait);
    if (result == static_cast<int> (zlink::recv_result_t::no_data)
        || result == static_cast<int> (zlink::recv_result_t::busy)
        || (result == -1
            && (errno == EAGAIN || errno == EWOULDBLOCK))) {
        return std::nullopt;
    }
    if (result != 0) {
        throw std::runtime_error ("raw dealer receive failed");
    }
    return copy_parts (received.parts ());
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
