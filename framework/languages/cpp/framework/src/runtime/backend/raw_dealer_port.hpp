/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/backend/raw_route_port.hpp"

#include <zlink/Contracts/Eventing/poller.hpp>
#include <zlink/Contracts/Messaging/received.hpp>

#include <memory>

namespace zlink
{
class dealer_socket_t;
}

namespace zlink::framework::detail::backend
{

class raw_dealer_port_t
{
  public:
    explicit raw_dealer_port_t (zlink::dealer_socket_t &socket,
                                std::shared_ptr<std::mutex> shared_socket_mutex = {},
                                zlink::poller_t *shared_poller = nullptr,
                                std::uintptr_t poller_slot = 1);

    task_t<bool> send (const raw_message_t &parts);
    task_t<zlink::submit_result_t> send_result (const raw_message_t &parts);
    task_t<raw_request_completion_t> request (const raw_message_t &parts,
                                              std::chrono::milliseconds timeout);
    std::optional<raw_message_t> try_receive ();
    void close ();

  private:
    std::unique_ptr<zlink::poller_t> _owned_poller;
    zlink::poller_t *_poller;
    std::uintptr_t _poller_slot;
    zlink::dealer_socket_t *_socket;
    std::shared_ptr<std::mutex> _socket_mutex;
    // Reused for every receive so the binding owns one stable receive buffer.
    zlink::received_t _received;
};

} // namespace zlink::framework::detail::backend
