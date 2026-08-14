/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_SUB_HPP_INCLUDED__
#define __ZLINK_SUB_HPP_INCLUDED__

#include "sockets/pubsub/xsub.hpp"

namespace zlink
{
class ctx_t;
class msg_t;
class io_thread_t;
class socket_base_t;

class sub_t ZLINK_FINAL : public xsub_t
{
  public:
    sub_t (zlink::ctx_t *parent_, uint32_t tid_, int sid_);
    ~sub_t ();

  protected:
    int xsetsockopt (int option_, const void *optval_, size_t optvallen_);
    int xsend (zlink::msg_t *msg_,
               pipe_message_admission_t *admission_out_ = NULL);
    bool xhas_out ();

    ZLINK_NON_COPYABLE_NOR_MOVABLE (sub_t)
};
}

#endif
