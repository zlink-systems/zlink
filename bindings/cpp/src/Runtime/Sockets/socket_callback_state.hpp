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

class routed_admission_state_t;
class publish_admission_state_t;

struct socket_callback_state_t :
    public std::enable_shared_from_this<socket_callback_state_t>
{
    std::atomic<bool> socket_closed{false};
    std::mutex send_ready_mutex;
    std::function<void ()> send_ready_handler;
    std::once_flag native_send_ready_handler_once;
    std::function<void (const routing_id_t &, message_t &&, message_t &&)> packet_handler;
    // Every binding-owned outbound record on this native socket takes this
    // gate only for one complete native submit attempt.  It is deliberately
    // released before routed readiness is awaited or user code is resumed.
    std::mutex outbound_record_attempt_mutex;
    std::mutex routed_admission_mutex;
    std::shared_ptr<routed_admission_state_t> routed_admission;
    std::mutex publish_admission_mutex;
    std::shared_ptr<publish_admission_state_t> publish_admission;
};

void ensure_native_send_ready_handler (
  void *socket_,
  socket_callback_state_t &state_);

} // namespace detail
} // namespace zlink

#endif
