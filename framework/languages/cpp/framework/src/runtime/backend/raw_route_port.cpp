/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/backend/raw_route_port.hpp"

#include <zlink/Contracts/Messaging/operation_contracts.hpp>
#include <zlink/Contracts/Messaging/received.hpp>
#include <zlink/Contracts/Sockets/routed_socket_contracts.hpp>

#include <cerrno>
#include <stdexcept>
#include <utility>

namespace zlink::framework::detail::backend
{
namespace
{

raw_message_t copy_parts (const std::vector<zlink::message_t> &parts)
{
    raw_message_t result;
    result.reserve (parts.size ());
    for (const auto &part : parts) {
        result.push_back (part.to_bytes ());
    }
    return result;
}

std::vector<zlink::message_t> materialize_parts (const raw_message_t &parts)
{
    std::vector<zlink::message_t> result;
    result.reserve (parts.size ());
    for (const auto &part : parts) {
        result.push_back (zlink::message_t::from (part));
    }
    return result;
}

raw_request_result_t map_request_result (zlink::request_result_t result) noexcept
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
    if (_receive_events != zlink::poll_event_flag_t::none) {
        _poller->add (socket, _receive_events, _poller_slot);
    }
}

bool raw_route_port_t::send (const raw_bytes_t &target_routing_id,
                             const raw_message_t &parts)
{
    if (target_routing_id.empty () || parts.empty ()) {
        throw std::invalid_argument ("raw route send requires a target and message parts");
    }
    std::lock_guard lock (*_socket_mutex);
    if (_socket == nullptr) {
        return false;
    }
    auto messages = materialize_parts (parts);
    auto operation = std::move (_socket->send (zlink::routing_id_t::from (target_routing_id)))
                       .message (messages[0]);
    for (std::size_t index = 1; index < messages.size (); ++index) {
        operation = std::move (operation).message (messages[index]);
    }
    try {
        return std::move (operation)
          .flags (static_cast<int> (zlink::send_flags_t::dontwait))
          .submit ();
    }
    catch (const zlink::submit_error_t &) {
        return false;
    }
}

bool raw_route_port_t::send_completion_control (
  const raw_bytes_t &target_routing_id,
  const raw_message_t &parts)
{
    if (target_routing_id.empty () || parts.empty ()) {
        throw std::invalid_argument (
          "completion control send requires a target and message parts");
    }
    std::lock_guard lock (*_socket_mutex);
    if (_socket == nullptr) {
        return false;
    }
    const auto messages = materialize_parts (parts);
    try {
        return _socket->try_send_completion_control (
          zlink::routing_id_t::from (target_routing_id), messages);
    }
    catch (const zlink::submit_error_t &) {
        return false;
    }
}

void raw_route_port_t::set_completion_control_handler (
  completion_control_handler_t handler)
{
    if (!handler) {
        throw std::invalid_argument (
          "completion control handler is required");
    }
    std::lock_guard lock (*_socket_mutex);
    if (_socket == nullptr) {
        throw std::logic_error ("raw route port is closed");
    }
    _socket->set_completion_control_handler (
      [handler = std::move (handler)] (
        const zlink::routing_id_t &source,
        std::vector<zlink::message_t> parts) mutable {
          handler (source.to_bytes (), copy_parts (parts));
      });
}

bool raw_route_port_t::request (const raw_bytes_t &target_routing_id,
                                const raw_message_t &parts,
                                std::chrono::milliseconds timeout,
                                request_callback_t callback)
{
    if (target_routing_id.empty () || parts.empty () || !callback) {
        throw std::invalid_argument ("raw route request requires target, parts and callback");
    }
    std::lock_guard lock (*_socket_mutex);
    if (_socket == nullptr) {
        return false;
    }
    auto messages = materialize_parts (parts);
    auto operation = std::move (_socket->request (zlink::routing_id_t::from (target_routing_id)))
                       .message (messages[0]);
    for (std::size_t index = 1; index < messages.size (); ++index) {
        operation = std::move (operation).message (messages[index]);
    }
    try {
        return std::move (operation).timeout (timeout).submit (
          [callback = std::move (callback)] (zlink::request_result_t result,
                                             std::vector<zlink::message_t> reply) mutable {
              callback (map_request_result (result), copy_parts (reply));
          });
    }
    catch (const zlink::submit_error_t &) {
        return false;
    }
}

zlink::poll_event_flag_t raw_route_port_t::poll (
  std::chrono::milliseconds timeout)
{
    std::lock_guard lock (*_socket_mutex);
    if (_socket == nullptr || _receive_events == zlink::poll_event_flag_t::none)
        return zlink::poll_event_flag_t::none;
    zlink::poll_event_t event;
    if (_poller->wait (&event, 1, timeout) != 1
        || event.slot != _poller_slot)
        return zlink::poll_event_flag_t::none;
    return event.revents;
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
    zlink::received_t received;
    const int result = _socket->recv (received, zlink::recv_flags_t::dontwait);
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
    if (!received.routing_id ()) {
        throw std::runtime_error ("raw ROUTER receive omitted source routing id");
    }
    return raw_received_t{received.routing_id ()->to_bytes (), received.request_seq (),
                          copy_parts (received.parts ())};
}

std::optional<raw_received_t> raw_route_port_t::try_receive ()
{
    return receive_if_ready (poll (std::chrono::milliseconds::zero ()));
}

bool raw_route_port_t::reply (const raw_received_t &request, const raw_message_t &parts)
{
    if (request.source_routing_id.empty () || !request.request_sequence || parts.empty ()) {
        throw std::invalid_argument ("raw route reply requires request context and message parts");
    }
    std::lock_guard lock (*_socket_mutex);
    if (_socket == nullptr) {
        return false;
    }
    auto messages = materialize_parts (parts);
    auto operation = std::move (_socket->reply (
                                  zlink::routing_id_t::from (request.source_routing_id),
                                  *request.request_sequence))
                       .message (messages[0]);
    for (std::size_t index = 1; index < messages.size (); ++index) {
        operation = std::move (operation).message (messages[index]);
    }
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
    std::lock_guard lock (*_socket_mutex);
    auto *socket = _socket;
    _socket = nullptr;
    if (_receive_events != zlink::poll_event_flag_t::none) {
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
}

} // namespace zlink::framework::detail::backend
