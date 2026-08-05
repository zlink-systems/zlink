/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/backend/raw_route_port.hpp"
#include "runtime/messaging/envelope_codec.hpp"

#include <zlink/Contracts/Core/routing_id.hpp>

#include <chrono>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace zlink
{
class router_socket_t;
} // namespace zlink

namespace zlink::framework::detail::backend
{

class native_route_backend_t
{
  public:
    explicit native_route_backend_t (zlink::router_socket_t &router);
    native_route_backend_t (zlink::router_socket_t &router,
                            std::atomic_bool &stop,
                            dispatch_options_t dispatch);

    result_t<void> submit_send (const zlink::routing_id_t &target_node_rid,
                                const std::optional<std::string> &target_spot_id,
                                const runtime::messaging::message_parts_t &parts);

    result_t<runtime::messaging::message_parts_t>
    submit_request (const zlink::routing_id_t &target_node_rid,
                    const std::optional<std::string> &target_spot_id,
                    const runtime::messaging::message_parts_t &parts,
                    std::chrono::milliseconds timeout);

    bool handle_router_received (const zlink::routing_id_t &source_node_rid,
                                 std::vector<zlink::message_t> &parts,
                                 std::optional<std::uint64_t> request_seq);

    void close () noexcept;
    std::mutex &router_mutex () noexcept;

  private:
    bool stopping () const noexcept;

    zlink::router_socket_t *_router;
    std::mutex _router_mutex;
    std::shared_ptr<raw_route_port_t> _raw_port;
    std::atomic_bool *_stop = nullptr;
    dispatch_options_t _dispatch;
};

} // namespace zlink::framework::detail::backend
