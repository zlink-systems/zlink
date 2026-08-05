/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include <zlink/Contracts/Eventing/poller.hpp>

namespace zlink
{
class router_socket_t;
}

namespace zlink::framework::detail::backend
{

using raw_bytes_t = std::vector<std::uint8_t>;
using raw_message_t = std::vector<raw_bytes_t>;

struct raw_received_t
{
    raw_bytes_t source_routing_id;
    std::optional<std::uint64_t> request_sequence;
    raw_message_t parts;
};

enum class raw_request_result_t
{
    ok,
    timed_out,
    not_connected,
    terminated,
    failed
};

class raw_route_port_t
{
  public:
    using request_callback_t =
      std::function<void (raw_request_result_t, raw_message_t)>;
    using completion_control_handler_t =
      std::function<void (raw_bytes_t, raw_message_t)>;

    explicit raw_route_port_t (
      zlink::router_socket_t &socket,
      std::mutex *shared_socket_mutex = nullptr,
      zlink::poll_event_flag_t receive_events =
        zlink::poll_event_flag_t::pollin,
      zlink::poller_t *shared_poller = nullptr,
      std::uintptr_t poller_slot = 1);

    bool send (const raw_bytes_t &target_routing_id, const raw_message_t &parts);
    bool send_completion_control (
      const raw_bytes_t &target_routing_id,
      const raw_message_t &parts);
    void set_completion_control_handler (
      completion_control_handler_t handler);
    bool request (const raw_bytes_t &target_routing_id,
                  const raw_message_t &parts,
                  std::chrono::milliseconds timeout,
                  request_callback_t callback);
    zlink::poll_event_flag_t poll (std::chrono::milliseconds timeout);
    std::optional<raw_received_t> receive_if_ready (
      zlink::poll_event_flag_t revents);
    std::optional<raw_received_t> try_receive ();
    bool reply (const raw_received_t &request, const raw_message_t &parts);
    void close () noexcept;

  private:
    std::unique_ptr<zlink::poller_t> _owned_poller;
    zlink::poller_t *_poller;
    std::uintptr_t _poller_slot;
    zlink::router_socket_t *_socket;
    std::mutex _owned_socket_mutex;
    std::mutex *_socket_mutex;
    zlink::poll_event_flag_t _receive_events;
};

} // namespace zlink::framework::detail::backend
