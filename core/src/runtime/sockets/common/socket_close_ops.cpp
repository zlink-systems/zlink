/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "sockets/common/socket_close_ops.hpp"

#include "sockets/common/socket_base.hpp"

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
