/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"
#include "sockets/pubsub/pub.hpp"
#include "core/pipe.hpp"
#include "utils/err.hpp"
#include "core/msg.hpp"

zlink::pub_t::pub_t (class ctx_t *parent_, uint32_t tid_, int sid_) : xpub_t (parent_, tid_, sid_)
{
    options.type = ZLINK_CORE_SOCKET_PUB;
    refresh_auto_hwm_policy ();
}

zlink::pub_t::~pub_t ()
{
}

void zlink::pub_t::xattach_pipe (pipe_t *pipe_, bool subscribe_to_all_, bool locally_initiated_)
{
    zlink_assert (pipe_);

    //  Don't delay pipe termination as there is no one
    //  to receive the delimiter.
    pipe_->set_nodelay ();

    xpub_t::xattach_pipe (pipe_, subscribe_to_all_, locally_initiated_);
}

int zlink::pub_t::xrecv (class msg_t *)
{
    //  Messages cannot be received from PUB socket.
    errno = ENOTSUP;
    return -1;
}

bool zlink::pub_t::xhas_in ()
{
    return false;
}
