/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "sockets/pubsub/xsub_dispatch_internal.hpp"

namespace
{
zlink::xsub_t *&xsub_dispatch_socket_tls ()
{
    static thread_local zlink::xsub_t *socket = NULL;
    return socket;
}
}

zlink::xsub_dispatch_context_t::xsub_dispatch_context_t (xsub_t *socket_) :
    _previous_socket (xsub_dispatch_socket_tls ())
{
    xsub_dispatch_socket_tls () = socket_;
}

zlink::xsub_dispatch_context_t::~xsub_dispatch_context_t ()
{
    xsub_dispatch_socket_tls () = _previous_socket;
}

bool zlink::xsub_dispatch_context_t::owns_socket (const xsub_t *socket_)
{
    return xsub_dispatch_socket_tls () == socket_;
}

bool zlink::xsub_dispatch_owns_socket (const xsub_t *socket_)
{
    return xsub_dispatch_context_t::owns_socket (socket_);
}
