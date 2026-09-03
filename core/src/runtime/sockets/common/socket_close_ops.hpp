/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_SOCKET_CLOSE_OPS_HPP_INCLUDED__
#define __ZLINK_SOCKET_CLOSE_OPS_HPP_INCLUDED__

namespace zlink
{
class socket_base_t;

class socket_close_ops_t
{
  public:
    static int request_close (socket_base_t *&socket_, int handoff_timeout_ms_);

  private:
    socket_close_ops_t ();
};
}

#endif
