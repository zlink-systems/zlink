/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "sockets/common/socket_dispatch_context.hpp"

#include "sockets/common/socket_base.hpp"

namespace
{
struct socket_msg_dispatch_tls_t
{
    socket_msg_dispatch_tls_t () :
        socket (NULL), pipe (NULL), subject (NULL), source_rid_valid (false)
    {
        memset (&source_rid, 0, sizeof (source_rid));
    }

    zlink::socket_base_t *socket;
    zlink::pipe_t *pipe;
    void *subject;
    zlink_routing_id_t source_rid;
    bool source_rid_valid;
};

struct socket_recv_source_rid_tls_t
{
    socket_recv_source_rid_tls_t () : socket (NULL), enabled (false) {}

    zlink::socket_base_t *socket;
    bool enabled;
};

socket_msg_dispatch_tls_t &socket_msg_dispatch_tls ()
{
    static thread_local socket_msg_dispatch_tls_t context;
    return context;
}

socket_recv_source_rid_tls_t &socket_recv_source_rid_tls ()
{
    static thread_local socket_recv_source_rid_tls_t context;
    return context;
}

void set_dispatch_source_rid (socket_msg_dispatch_tls_t &context_,
                              const zlink_routing_id_t *source_rid_)
{
    if (source_rid_) {
        context_.source_rid = *source_rid_;
        context_.source_rid_valid = true;
        return;
    }

    memset (&context_.source_rid, 0, sizeof (context_.source_rid));
    context_.source_rid_valid = false;
}
}

zlink::socket_msg_dispatch_context_t::socket_msg_dispatch_context_t (
  socket_base_t *socket_, pipe_t *pipe_, void *subject_,
  const zlink_routing_id_t *source_rid_) :
    _previous_socket (socket_msg_dispatch_tls ().socket),
    _previous_pipe (socket_msg_dispatch_tls ().pipe),
    _previous_subject (socket_msg_dispatch_tls ().subject),
    _previous_source_rid_valid (socket_msg_dispatch_tls ().source_rid_valid)
{
    socket_msg_dispatch_tls_t &context = socket_msg_dispatch_tls ();
    if (_previous_source_rid_valid)
        _previous_source_rid = context.source_rid;
    context.socket = socket_;
    context.pipe = pipe_;
    context.subject = subject_;
    set_dispatch_source_rid (context, source_rid_);
}

zlink::socket_msg_dispatch_context_t::~socket_msg_dispatch_context_t ()
{
    socket_msg_dispatch_tls_t &context = socket_msg_dispatch_tls ();
    context.socket = _previous_socket;
    context.pipe = _previous_pipe;
    context.subject = _previous_subject;
    if (_previous_source_rid_valid) {
        set_dispatch_source_rid (context, &_previous_source_rid);
    } else {
        set_dispatch_source_rid (context, NULL);
    }
}

zlink::socket_base_t *zlink::socket_msg_dispatch_context_t::current_socket ()
{
    return socket_msg_dispatch_tls ().socket;
}

zlink::pipe_t *zlink::socket_msg_dispatch_context_t::current_pipe ()
{
    return socket_msg_dispatch_tls ().pipe;
}

void *zlink::socket_msg_dispatch_context_t::current_subject ()
{
    return socket_msg_dispatch_tls ().subject;
}

bool zlink::socket_msg_dispatch_context_t::current_source_rid (zlink_routing_id_t *out_)
{
    const socket_msg_dispatch_tls_t &context = socket_msg_dispatch_tls ();
    if (!out_ || !context.source_rid_valid)
        return false;
    *out_ = context.source_rid;
    return true;
}

zlink::socket_recv_source_rid_context_t::socket_recv_source_rid_context_t (socket_base_t *socket_,
                                                                           bool enabled_) :
    _previous_socket (socket_recv_source_rid_tls ().socket),
    _previous_enabled (socket_recv_source_rid_tls ().enabled)
{
    socket_recv_source_rid_tls_t &context = socket_recv_source_rid_tls ();
    context.socket = socket_;
    context.enabled = enabled_;
}

zlink::socket_recv_source_rid_context_t::~socket_recv_source_rid_context_t ()
{
    socket_recv_source_rid_tls_t &context = socket_recv_source_rid_tls ();
    context.socket = _previous_socket;
    context.enabled = _previous_enabled;
}

zlink::socket_base_t *zlink::socket_recv_source_rid_context_t::current_socket ()
{
    return socket_recv_source_rid_tls ().socket;
}

bool zlink::socket_recv_source_rid_context_t::current_enabled ()
{
    return socket_recv_source_rid_tls ().enabled;
}

void zlink::socket_recv_source_rid_context_t::set (socket_base_t *socket_, bool enabled_)
{
    socket_recv_source_rid_tls_t &context = socket_recv_source_rid_tls ();
    context.socket = socket_;
    context.enabled = enabled_;
}

bool zlink::socket_recv_source_rid_context_t::capture_requested (const socket_base_t *socket_)
{
    const socket_recv_source_rid_tls_t &context = socket_recv_source_rid_tls ();
    return context.enabled && context.socket == socket_;
}
