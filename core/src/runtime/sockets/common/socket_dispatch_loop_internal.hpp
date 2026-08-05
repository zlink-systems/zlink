/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_SOCKET_DISPATCH_LOOP_INTERNAL_HPP_INCLUDED__
#define __ZLINK_SOCKET_DISPATCH_LOOP_INTERNAL_HPP_INCLUDED__

#include "core/msg.hpp"
#include "core/pipe.hpp"
#include "utils/err.hpp"

namespace zlink
{
template <typename RecvFn, typename DispatchFn>
inline void drain_socket_dispatch_loop (RecvFn recv_fn_, DispatchFn dispatch_fn_)
{
    msg_t msg;
    const int init_rc = msg.init ();
    errno_assert (init_rc == 0);

    pipe_t *dispatch_pipe = NULL;
    while (recv_fn_ (&msg, &dispatch_pipe) == 0) {
        const int dispatch_rc = dispatch_fn_ (&msg, dispatch_pipe);
        if (dispatch_rc <= 0)
            break;
    }

    const int close_rc = msg.close ();
    errno_assert (close_rc == 0);
}
}

#endif
