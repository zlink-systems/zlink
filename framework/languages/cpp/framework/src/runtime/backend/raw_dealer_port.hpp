/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once
#include <zlink/framework/contracts/dispatch/request_task.hpp>

#include "runtime/backend/raw_route_port.hpp"
#include "runtime/foundation/operation_registry.hpp"
#include "runtime/execution/state_lane.hpp"

#include <zlink/Contracts/Eventing/poller.hpp>
#include <zlink/Contracts/Messaging/received.hpp>

#include <memory>
#include <atomic>
#include <functional>

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
                                std::uintptr_t poller_slot = 1,
                                runtime::state_lane_t *owner = nullptr,
                                std::function<void ()> deadline_changed = {});
    ~raw_dealer_port_t () noexcept;

    task_t<bool> send (const raw_message_t &parts);
    task_t<zlink::submit_result_t> send_result (const raw_message_t &parts);
    task_t<raw_request_completion_t>
    request (const raw_message_t &parts,
             std::chrono::milliseconds timeout,
             runtime::mesh::service_liveness_registry_t::connection_t admission = {},
             std::shared_ptr<task_completion_source_t<void>> admitted = {},
             std::optional<std::chrono::steady_clock::time_point> deadline = {});
    task_t<zlink::submit_result_t> send_result (std::vector<zlink::message_t> messages);
    task_t<raw_request_completion_t>
    request (std::vector<zlink::message_t> messages,
             std::chrono::milliseconds timeout,
             runtime::mesh::service_liveness_registry_t::connection_t admission = {},
             std::shared_ptr<task_completion_source_t<void>> admitted = {},
             std::optional<std::chrono::steady_clock::time_point> deadline = {});
    std::optional<raw_message_t> try_receive ();
    void close ();
    std::size_t expire_requests (std::chrono::steady_clock::time_point now);
    std::optional<std::chrono::steady_clock::time_point> next_request_deadline () const;

  private:
    std::shared_ptr<runtime::foundation::operation_registry_t> _requests;
    std::atomic_uint64_t _next_request{1};
    std::unique_ptr<zlink::poller_t> _owned_poller;
    zlink::poller_t *_poller;
    std::uintptr_t _poller_slot;
    zlink::dealer_socket_t *_socket;
    std::shared_ptr<std::mutex> _socket_mutex;
    // Reused for every receive so the binding owns one stable receive buffer.
    zlink::received_t _received;
};

} // namespace zlink::framework::detail::backend
