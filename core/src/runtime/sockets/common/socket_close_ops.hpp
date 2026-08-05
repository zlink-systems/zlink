/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_SOCKET_CLOSE_OPS_HPP_INCLUDED__
#define __ZLINK_SOCKET_CLOSE_OPS_HPP_INCLUDED__

namespace zlink
{
class ctx_t;
class socket_base_t;

class socket_close_ops_t
{
  public:
    static int request_close (socket_base_t *&socket_);
    static int request_close (socket_base_t *&socket_, int handoff_timeout_ms_);
    static int request_close_and_wait (ctx_t *ctx_, socket_base_t *&socket_, int timeout_ms_);
    static int wait_until_closed (ctx_t *ctx_, const socket_base_t *socket_, int timeout_ms_);

  private:
    socket_close_ops_t ();
};
}

#endif
