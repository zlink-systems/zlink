/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_SOCKETS_STREAM_DISPATCH_INTERNAL_HPP_INCLUDED__
#define __ZLINK_SOCKETS_STREAM_DISPATCH_INTERNAL_HPP_INCLUDED__

#include <stdint.h>

namespace zlink
{
class pipe_t;
class stream_t;

class stream_dispatch_context_t
{
  public:
    stream_dispatch_context_t (stream_t *socket_, pipe_t *pipe_, uint32_t routing_id_);
    ~stream_dispatch_context_t ();

    static bool owns_socket (const stream_t *socket_);
    static pipe_t *current_pipe ();
    static uint32_t current_routing_id ();

  private:
    stream_t *_previous_socket;
    pipe_t *_previous_pipe;
    uint32_t _previous_routing_id;
};

bool stream_dispatch_owns_socket (const stream_t *socket_);
}

#endif
