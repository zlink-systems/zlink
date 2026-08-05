/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_SOCKET_DISPATCH_CONTEXT_HPP_INCLUDED__
#define __ZLINK_SOCKET_DISPATCH_CONTEXT_HPP_INCLUDED__

#include <zlink.h>

namespace zlink
{
class socket_base_t;
class pipe_t;

class socket_msg_dispatch_context_t
{
  public:
    socket_msg_dispatch_context_t (socket_base_t *socket_,
                                   pipe_t *pipe_,
                                   void *subject_,
                                   const zlink_routing_id_t *source_rid_);
    ~socket_msg_dispatch_context_t ();

    static socket_base_t *current_socket ();
    static pipe_t *current_pipe ();
    static void *current_subject ();
    static bool current_source_rid (zlink_routing_id_t *out_);

  private:
    socket_base_t *_previous_socket;
    pipe_t *_previous_pipe;
    void *_previous_subject;
    zlink_routing_id_t _previous_source_rid;
    bool _previous_source_rid_valid;
};

class socket_recv_source_rid_context_t
{
  public:
    socket_recv_source_rid_context_t (socket_base_t *socket_, bool enabled_);
    ~socket_recv_source_rid_context_t ();

    static socket_base_t *current_socket ();
    static bool current_enabled ();
    static void set (socket_base_t *socket_, bool enabled_);
    static bool capture_requested (const socket_base_t *socket_);

  private:
    socket_base_t *_previous_socket;
    bool _previous_enabled;
};
}

#endif
