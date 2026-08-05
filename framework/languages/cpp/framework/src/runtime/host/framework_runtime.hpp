/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/dispatch/offload_executor.hpp"

#include <memory>

namespace zlink
{
class context_t;
class router_socket_t;
class dealer_socket_t;
class stream_socket_t;
} // namespace zlink

namespace zlink::framework::runtime
{

class framework_runtime_t
{
  public:
    framework_runtime_t ();
    ~framework_runtime_t ();

    framework_runtime_t (const framework_runtime_t &) = delete;
    framework_runtime_t &operator= (const framework_runtime_t &) = delete;

    bool owns_native_context () const noexcept;
    zlink::router_socket_t &channel_router ();
    zlink::dealer_socket_t &channel_dealer ();
    zlink::stream_socket_t &stream_socket ();
    void drain ();
    offload_executor_t &offload_executor () noexcept;

  private:
    std::unique_ptr<zlink::context_t> _context;
    std::unique_ptr<zlink::router_socket_t> _router;
    std::unique_ptr<zlink::dealer_socket_t> _dealer;
    std::unique_ptr<zlink::stream_socket_t> _stream;
    offload_executor_t _offload;
};

} // namespace zlink::framework::runtime
