/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/host/framework_runtime.hpp"

#include <zlink/Contracts/Core/context.hpp>
#include <zlink/Contracts/Sockets/message_socket_contracts.hpp>
#include <zlink/Contracts/Sockets/routed_socket_contracts.hpp>
#include <zlink/Contracts/Sockets/stream_socket.hpp>

#include <chrono>

namespace zlink::framework::runtime
{

framework_runtime_t::framework_runtime_t (std::shared_ptr<runtime_failure_collector_t> failures) :
    _runtime_failures (std::move (failures)),
    _context (std::make_unique<zlink::context_t> ()),
    _offload (1)
{
}

framework_runtime_t::~framework_runtime_t ()
{
    if (!_runtime_failures->capture ([&] { drain (); }))
        _runtime_failures->retain ([context = std::move (_context), router = std::move (_router),
                                    dealer = std::move (_dealer),
                                    stream = std::move (_stream)] () mutable {
            close_resources (context, router, dealer, stream);
        });
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
    runtime_failure_collector_t failures;
    if (_router)
        failures.capture ([&] { _router->options ().linger (std::chrono::milliseconds (0)); });
    if (_dealer)
        failures.capture ([&] { _dealer->options ().linger (std::chrono::milliseconds (0)); });
    if (_stream)
        failures.capture ([&] { _stream->options ().linger (std::chrono::milliseconds (0)); });
    failures.capture ([&] { _offload.drain (); });
    failures.capture ([&] { close_resources (_context, _router, _dealer, _stream); });
    failures.rethrow_if_failed ();
}

void framework_runtime_t::close_resources (std::unique_ptr<zlink::context_t> &context,
                                           std::unique_ptr<zlink::router_socket_t> &router,
                                           std::unique_ptr<zlink::dealer_socket_t> &dealer,
                                           std::unique_ptr<zlink::stream_socket_t> &stream)
{
    runtime_failure_collector_t::close_resources (stream, dealer, router);
    if (context) {
        context->shutdown ();
        context->term ();
        context.reset ();
    }
}

offload_executor_t &framework_runtime_t::offload_executor () noexcept
{
    return _offload;
}

} // namespace zlink::framework::runtime
