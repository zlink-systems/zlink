/* SPDX-License-Identifier: MPL-2.0 */
#ifndef ZLINK_CPP_RUNTIME_SOCKETS_SOCKET_CALLBACK_STATE_HPP_INCLUDED
#define ZLINK_CPP_RUNTIME_SOCKETS_SOCKET_CALLBACK_STATE_HPP_INCLUDED

#include <zlink/Contracts/Core/routing_id.hpp>
#include <zlink/Contracts/Messaging/message.hpp>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>

namespace zlink
{
namespace detail
{

struct socket_callback_state_t :
    public std::enable_shared_from_this<socket_callback_state_t>
{
    std::function<void (const routing_id_t &, message_t &&, message_t &&)> packet_handler;

    // One Core send-completion handler is installed lazily per socket. The
    // atomic flag keeps the mutex off every submit after that first install.
    std::mutex send_completion_mutex;
    std::atomic<bool> send_completion_handler_registered{false};
};

} // namespace detail
} // namespace zlink

#endif
