/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "sockets/stream/stream_dispatch_internal.hpp"

namespace
{
struct stream_dispatch_tls_t
{
    stream_dispatch_tls_t () :
        socket (NULL), pipe (NULL), routing_id (0), packet_callback (false)
    {
    }

    zlink::stream_t *socket;
    zlink::pipe_t *pipe;
    uint32_t routing_id;
    bool packet_callback;
};

stream_dispatch_tls_t &stream_dispatch_tls ()
{
    static thread_local stream_dispatch_tls_t context;
    return context;
}
}

zlink::stream_dispatch_context_t::stream_dispatch_context_t (stream_t *socket_,
                                                             pipe_t *pipe_,
                                                             uint32_t routing_id_,
                                                             bool packet_callback_) :
    _previous_socket (stream_dispatch_tls ().socket),
    _previous_pipe (stream_dispatch_tls ().pipe),
    _previous_routing_id (stream_dispatch_tls ().routing_id),
    _previous_packet_callback (stream_dispatch_tls ().packet_callback)
{
    stream_dispatch_tls_t &context = stream_dispatch_tls ();
    context.socket = socket_;
    context.pipe = pipe_;
    context.routing_id = routing_id_;
    context.packet_callback = packet_callback_;
}

zlink::stream_dispatch_context_t::~stream_dispatch_context_t ()
{
    stream_dispatch_tls_t &context = stream_dispatch_tls ();
    context.socket = _previous_socket;
    context.pipe = _previous_pipe;
    context.routing_id = _previous_routing_id;
    context.packet_callback = _previous_packet_callback;
}

bool zlink::stream_dispatch_context_t::owns_socket (const stream_t *socket_)
{
    return stream_dispatch_tls ().socket == socket_;
}

zlink::pipe_t *zlink::stream_dispatch_context_t::current_pipe ()
{
    return stream_dispatch_tls ().pipe;
}

uint32_t zlink::stream_dispatch_context_t::current_routing_id ()
{
    return stream_dispatch_tls ().routing_id;
}

bool zlink::stream_dispatch_context_t::in_packet_callback ()
{
    return stream_dispatch_tls ().packet_callback;
}

bool zlink::stream_dispatch_owns_socket (const stream_t *socket_)
{
    return stream_dispatch_context_t::owns_socket (socket_);
}
