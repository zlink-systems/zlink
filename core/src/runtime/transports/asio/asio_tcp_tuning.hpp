/* SPDX-License-Identifier: MPL-2.0 */

#ifndef ZLINK_ASIO_TCP_TUNING_HPP_INCLUDED
#define ZLINK_ASIO_TCP_TUNING_HPP_INCLUDED

#include "core/options.hpp"
#include "transports/tcp/tcp.hpp"
#include "utils/fd.hpp"

namespace zlink
{
inline int tune_asio_tcp_socket (fd_t fd_,
                                 const options_t &options_,
                                 bool apply_buffer_options_)
{
    int rc = tune_tcp_socket (fd_, options_.tcp_nodelay);
#ifdef ZLINK_HAVE_WINDOWS
    // The multi-socket benchmark and local applications commonly use TCP
    // loopback. Enable the Windows loopback fast path when the OS supports it.
    tcp_tune_loopback_fast_path (fd_);
#endif
    if (apply_buffer_options_ && options_.sndbuf >= 0)
        rc = rc | set_tcp_send_buffer (fd_, options_.sndbuf);
    if (apply_buffer_options_ && options_.rcvbuf >= 0)
        rc = rc | set_tcp_receive_buffer (fd_, options_.rcvbuf);
    rc = rc
         | tune_tcp_keepalives (fd_, options_.tcp_keepalive, options_.tcp_keepalive_cnt,
                                options_.tcp_keepalive_idle, options_.tcp_keepalive_intvl);
    rc = rc | tune_tcp_maxrt (fd_, options_.tcp_maxrt);
    return rc;
}
}

#endif
