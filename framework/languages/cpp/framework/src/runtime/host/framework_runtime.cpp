/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/host/framework_runtime.hpp"

#include <zlink/Contracts/Core/context.hpp>
#include <zlink/Contracts/Sockets/message_socket_contracts.hpp>
#include <zlink/Contracts/Sockets/routed_socket_contracts.hpp>
#include <zlink/Contracts/Sockets/stream_socket.hpp>

#include <chrono>

namespace zlink::framework::runtime
{

framework_runtime_t::framework_runtime_t () :
    _context (std::make_unique<zlink::context_t> ()), _offload (1)
{
}

framework_runtime_t::~framework_runtime_t ()
{
    drain ();
}

bool framework_runtime_t::owns_native_context () const noexcept
{
    return static_cast<bool> (_context);
}

zlink::router_socket_t &framework_runtime_t::channel_router ()
{
    if (!_router) {
        _router = std::make_unique<zlink::router_socket_t> (*_context);
    }
    return *_router;
}

zlink::dealer_socket_t &framework_runtime_t::channel_dealer ()
{
    if (!_dealer) {
        _dealer = std::make_unique<zlink::dealer_socket_t> (*_context);
    }
    return *_dealer;
}

zlink::stream_socket_t &framework_runtime_t::stream_socket ()
{
    if (!_stream) {
        _stream = std::make_unique<zlink::stream_socket_t> (*_context);
    }
    return *_stream;
}

void framework_runtime_t::drain ()
{
    if (_context) {
        try {
            _context->options ().blocky (false);
        }
        catch (...) {
        }
    }
    if (_router) {
        try {
            _router->options ().linger (std::chrono::milliseconds (0));
        }
        catch (...) {
        }
    }
    if (_dealer) {
        try {
            _dealer->options ().linger (std::chrono::milliseconds (0));
        }
        catch (...) {
        }
    }
    if (_stream) {
        try {
            _stream->options ().linger (std::chrono::milliseconds (0));
        }
        catch (...) {
        }
    }
    _offload.drain ();
    _stream.reset ();
    _dealer.reset ();
    _router.reset ();
    if (_context) {
        _context->shutdown ();
        _context->term ();
        _context.reset ();
    }
}

offload_executor_t &framework_runtime_t::offload_executor () noexcept
{
    return _offload;
}

} // namespace zlink::framework::runtime
