/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>
#include <string_view>

#include <zlink/Contracts/Eventing/poller.hpp>
#include <zlink/Contracts/Messaging/request_result.hpp>
#include <zlink/Contracts/Messaging/received.hpp>
#include <zlink/Contracts/Sockets/results.hpp>
#include <zlink/framework/contracts/dispatch/task.hpp>

#include "runtime/eventing/runtime_wake_timer.hpp"

namespace zlink
{
class router_socket_t;
}

namespace zlink::framework::detail::backend
{

using raw_bytes_t = std::vector<std::uint8_t>;
using raw_message_t = std::vector<raw_bytes_t>;
using raw_send_stage_trace_t =
  std::function<void (std::string_view, std::string_view)>;

struct raw_received_t
{
    raw_bytes_t source_routing_id;
    std::optional<zlink::reply_token_t> reply_token;
    raw_message_t parts;
};

enum class raw_request_result_t
{
    ok,
    timed_out,
    not_connected,
    route_unavailable,
    terminated,
    failed
};

enum class raw_request_failure_phase_t
{
    initial_admission,
    completion_terminal
};

struct raw_request_failure_t
{
    raw_request_failure_phase_t phase;
    std::optional<zlink::submit_result_t> submit_result;
    std::optional<zlink::request_result_t> request_result;
    int internal_errno = 0;
};

struct raw_request_completion_t
{
    raw_request_result_t result = raw_request_result_t::failed;
    raw_message_t parts;
    std::optional<raw_request_failure_t> failure;
};

class raw_route_port_t
{
  public:
    explicit raw_route_port_t (
      zlink::router_socket_t &socket,
      std::mutex *shared_socket_mutex = nullptr,
      zlink::poll_event_flag_t receive_events =
        zlink::poll_event_flag_t::pollin,
      zlink::poller_t *shared_poller = nullptr,
      std::uintptr_t poller_slot = 1);

    task_t<zlink::submit_result_t> send_result (
      const raw_bytes_t &target_routing_id,
      const raw_message_t &parts,
      raw_send_stage_trace_t trace = {});
    task_t<bool> send (const raw_bytes_t &target_routing_id,
                       const raw_message_t &parts);
    task_t<raw_request_completion_t> request (
      const raw_bytes_t &target_routing_id,
      const raw_message_t &parts,
      std::chrono::milliseconds timeout);
    zlink::poll_event_flag_t poll (std::chrono::milliseconds timeout);
    void signal_activity () noexcept;
    std::optional<raw_received_t> receive_if_ready (
      zlink::poll_event_flag_t revents);
    std::optional<raw_received_t> try_receive ();
    bool reply (const raw_received_t &request,
                const raw_message_t &parts);
    void close () noexcept;

  private:
    std::unique_ptr<zlink::poller_t> _owned_poller;
    zlink::poller_t *_poller;
    std::uintptr_t _poller_slot;
    zlink::router_socket_t *_socket;
    std::mutex _owned_socket_mutex;
    std::mutex *_socket_mutex;
    zlink::poll_event_flag_t _receive_events;
    runtime::eventing::runtime_wake_timer_t _wake_timer;
    // Keep the binding receive envelope across polls so its message storage
    // capacity is reused by the binding public receive API.
    zlink::received_t _received;
};

} // namespace zlink::framework::detail::backend
