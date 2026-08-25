/* SPDX-License-Identifier: MPL-2.0 */
#ifndef ZLINK_CPP_RUNTIME_SOCKETS_SOCKET_CALLBACK_STATE_HPP_INCLUDED
#define ZLINK_CPP_RUNTIME_SOCKETS_SOCKET_CALLBACK_STATE_HPP_INCLUDED

#include <zlink/Contracts/Core/routing_id.hpp>
#include <zlink/Contracts/Messaging/message.hpp>

#include <functional>
#include <atomic>
#include <memory>
#include <mutex>

namespace zlink
{
namespace detail
{

struct socket_callback_state_t :
    public std::enable_shared_from_this<socket_callback_state_t>
{
    std::atomic<bool> socket_closed{false};
    std::function<void (const routing_id_t &, message_t &&, message_t &&)> packet_handler;
    // A binding submit takes this gate for one complete native part sequence
    // so two threads cannot interleave parts of different messages through
    // the per-handle send-sequence gate, and so a close cannot run underneath
    // an in-flight submit.
    std::mutex outbound_record_attempt_mutex;

    // One Core send-completion handler is installed lazily per socket.
    std::mutex send_completion_mutex;
    bool send_completion_handler_registered = false;
};

} // namespace detail
} // namespace zlink

#endif
