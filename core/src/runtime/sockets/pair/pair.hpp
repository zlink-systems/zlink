/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_PAIR_HPP_INCLUDED__
#define __ZLINK_PAIR_HPP_INCLUDED__

#include "utils/blob.hpp"
#include "sockets/common/socket_base.hpp"
#include "core/session_base.hpp"

namespace zlink
{
class ctx_t;
class msg_t;
class pipe_t;
class io_thread_t;

class pair_t ZLINK_FINAL : public socket_base_t
{
  public:
    pair_t (zlink::ctx_t *parent_, uint32_t tid_, int sid_);
    ~pair_t ();

    //  Overrides of functions from socket_base_t.
    void xattach_pipe (zlink::pipe_t *pipe_, bool subscribe_to_all_, bool locally_initiated_);
    int xsend (zlink::msg_t *msg_,
               pipe_message_admission_t *admission_out_ = NULL);
    int xrollback () ZLINK_OVERRIDE;
    int xrecv (zlink::msg_t *msg_);
    int xrecv_pipe (zlink::msg_t *msg_,
                    zlink::pipe_t **pipe_out_) ZLINK_OVERRIDE;
    bool xhas_in ();
    bool xhas_out ();
    void xread_activated (zlink::pipe_t *pipe_);
    void xwrite_activated (zlink::pipe_t *pipe_);
    void xpipe_terminated (zlink::pipe_t *pipe_);

  private:
    zlink::pipe_t *_pipe;
    size_t _recv_part_index;

    ZLINK_NON_COPYABLE_NOR_MOVABLE (pair_t)
};

#ifdef ZLINK_BUILD_TESTS
typedef void (*pair_xsend_gate_hook_fn) (void *userdata_);
void test_set_pair_xsend_gate_hook (pair_xsend_gate_hook_fn hook_,
                                    void *userdata_);
#endif
}

#endif
