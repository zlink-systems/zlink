/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "sockets/common/socket_close_ops.hpp"

#include "core/ctx.hpp"
#include "sockets/common/socket_base.hpp"

int zlink::socket_close_ops_t::request_close (socket_base_t *&socket_)
{
    return request_close (socket_, 0);
}

int zlink::socket_close_ops_t::request_close (socket_base_t *&socket_, int handoff_timeout_ms_)
{
    if (!socket_)
        return 0;

    socket_base_t *socket = socket_;
    socket->stop ();
    socket->close (handoff_timeout_ms_);
    socket_ = NULL;
    return 0;
}

int zlink::socket_close_ops_t::request_close_and_wait (ctx_t *ctx_,
                                                       socket_base_t *&socket_,
                                                       int timeout_ms_)
{
    if (!socket_)
        return 0;

    socket_base_t *socket = socket_;
    const int rc = request_close (socket_);
    if (rc != 0 || !ctx_)
        return rc;

    return wait_until_closed (ctx_, socket, timeout_ms_);
}

int zlink::socket_close_ops_t::wait_until_closed (ctx_t *ctx_,
                                                  const socket_base_t *socket_,
                                                  int timeout_ms_)
{
    if (!ctx_ || !socket_)
        return 0;

    return ctx_->wait_for_socket_removal (socket_, timeout_ms_);
}
