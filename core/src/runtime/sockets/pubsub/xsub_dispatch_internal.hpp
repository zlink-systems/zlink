/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_SOCKETS_XSUB_DISPATCH_INTERNAL_HPP_INCLUDED__
#define __ZLINK_SOCKETS_XSUB_DISPATCH_INTERNAL_HPP_INCLUDED__

namespace zlink
{
class xsub_t;

class xsub_dispatch_context_t
{
  public:
    explicit xsub_dispatch_context_t (xsub_t *socket_);
    ~xsub_dispatch_context_t ();

    static bool owns_socket (const xsub_t *socket_);

  private:
    xsub_t *_previous_socket;
};

bool xsub_dispatch_owns_socket (const xsub_t *socket_);
}

#endif
