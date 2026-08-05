/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_PUB_HPP_INCLUDED__
#define __ZLINK_PUB_HPP_INCLUDED__

#include "sockets/pubsub/xpub.hpp"

namespace zlink
{
class ctx_t;
class io_thread_t;
class socket_base_t;
class msg_t;

class pub_t ZLINK_FINAL : public xpub_t
{
  public:
    pub_t (zlink::ctx_t *parent_, uint32_t tid_, int sid_);
    ~pub_t ();

    //  Implementations of virtual functions from socket_base_t.
    void xattach_pipe (zlink::pipe_t *pipe_,
                       bool subscribe_to_all_ = false,
                       bool locally_initiated_ = false);
    int xrecv (zlink::msg_t *msg_);
    bool xhas_in ();

    ZLINK_NON_COPYABLE_NOR_MOVABLE (pub_t)
};
}

#endif
