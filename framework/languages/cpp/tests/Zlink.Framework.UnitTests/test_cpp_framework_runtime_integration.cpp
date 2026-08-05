/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/host/framework_runtime.hpp"
#include "runtime/dispatch/projection.hpp"

#include <zlink/framework.hpp>

#include <atomic>

int main ()
{
    zlink::framework::runtime::framework_runtime_t runtime;
    if (!runtime.owns_native_context ()) {
        return 1;
    }
    (void) runtime.channel_router ();
    (void) runtime.channel_dealer ();
    (void) runtime.stream_socket ();

    std::atomic<int> offloaded{0};
    runtime.offload_executor ().submit ([&] { offloaded.store (1); });
    runtime.drain ();
    if (offloaded.load () != 1 || !runtime.offload_executor ().drained ()) {
        return 2;
    }

    zlink::framework::runtime::offload_executor_t projection_offload{1};
    zlink::framework::runtime::dispatch_projection_t projection{projection_offload};
    std::atomic<int> projected{0};
    projection.register_handler (zlink::framework::runtime::runtime_event_kind_t::channel_readable,
                                 zlink::framework::handler_execution_t::inline_on_runtime,
                                 [&] (const zlink::framework::runtime::runtime_event_t &event) {
                                     if (event.sequence == 1) {
                                         projected.fetch_add (1);
                                     }
                                 });
    projection.register_handler (zlink::framework::runtime::runtime_event_kind_t::timer_readable,
                                 zlink::framework::handler_execution_t::offload,
                                 [&] (const zlink::framework::runtime::runtime_event_t &event) {
                                     if (event.sequence == 2) {
                                         projected.fetch_add (1);
                                     }
                                 });
    projection.project (zlink::framework::runtime::runtime_event_kind_t::channel_readable);
    projection.project (zlink::framework::runtime::runtime_event_kind_t::timer_readable);
    projection_offload.drain ();
    if (projected.load () != 2 || projection.projected_events ().size () != 2) {
        return 9;
    }

    const auto tcp = zlink::framework::transport_endpoint_t::parse ("tcp://127.0.0.1:7001");
    if (tcp.scheme () != zlink::framework::transport_scheme_t::tcp) {
        return 3;
    }

    const auto ipc = zlink::framework::transport_endpoint_t::parse ("ipc://x");
    if (ipc.scheme () != zlink::framework::transport_scheme_t::ipc) {
        return 4;
    }

    const auto tls = zlink::framework::transport_endpoint_t::parse ("tls://x");
    if (tls.scheme () != zlink::framework::transport_scheme_t::tls) {
        return 5;
    }

    const auto ws = zlink::framework::transport_endpoint_t::parse ("ws://x");
    if (ws.scheme () != zlink::framework::transport_scheme_t::websocket) {
        return 6;
    }

    const auto wss = zlink::framework::transport_endpoint_t::parse ("wss://x");
    if (wss.scheme () != zlink::framework::transport_scheme_t::websocket_tls) {
        return 7;
    }

    bool pgm_rejected = false;
    try {
        (void) zlink::framework::transport_endpoint_t::parse ("pgm://224.0.0.1");
    }
    catch (const zlink::framework::framework_exception_t &error) {
        pgm_rejected =
          error.kind () == zlink::framework::framework_error_kind_t::protocol_error;
    }
    if (!pgm_rejected) {
        return 8;
    }

    return 0;
}
