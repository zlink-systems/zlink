/* SPDX-License-Identifier: MPL-2.0 */
#ifndef ZLINK_CPP_RUNTIME_SOCKETS_SOCKET_CALLBACK_STATE_HPP_INCLUDED
#define ZLINK_CPP_RUNTIME_SOCKETS_SOCKET_CALLBACK_STATE_HPP_INCLUDED

#include <zlink/Contracts/Core/routing_id.hpp>
#include <zlink/Contracts/Messaging/message.hpp>

#include <functional>
#include <mutex>

namespace zlink
{
namespace detail
{

struct socket_callback_state_t
{
    std::function<void ()> send_ready_handler;
    std::function<void (const routing_id_t &, std::vector<message_t>)>
      completion_control_handler;
    std::mutex completion_control_handler_mutex;
    bool completion_control_handler_registered = false;
    std::function<void (const routing_id_t &, message_t &&, message_t &&)> packet_handler;
};

} // namespace detail
} // namespace zlink

#endif
