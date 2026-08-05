/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/backend/raw_route_port.hpp"

#include <zlink/Contracts/Eventing/poller.hpp>

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
    explicit raw_dealer_port_t (
      zlink::dealer_socket_t &socket,
      std::mutex *shared_socket_mutex = nullptr,
      zlink::poller_t *shared_poller = nullptr,
      std::uintptr_t poller_slot = 1);

    bool send (const raw_message_t &parts);
    bool request (const raw_message_t &parts,
                  std::chrono::milliseconds timeout,
                  raw_route_port_t::request_callback_t callback);
    std::optional<raw_message_t> try_receive ();
    void close () noexcept;

  private:
    std::unique_ptr<zlink::poller_t> _owned_poller;
    zlink::poller_t *_poller;
    std::uintptr_t _poller_slot;
    zlink::dealer_socket_t *_socket;
    std::mutex _owned_socket_mutex;
    std::mutex *_socket_mutex;
};

} // namespace zlink::framework::detail::backend
