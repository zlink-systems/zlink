/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_TCP_HPP_INCLUDED__
#define __ZLINK_TCP_HPP_INCLUDED__

#include "utils/fd.hpp"

namespace zlink
{
//  Tunes the supplied TCP socket.
//  tcp_nodelay_:
//    1  -> enable TCP_NODELAY
//    0  -> disable TCP_NODELAY
//   -1  -> do not touch TCP_NODELAY
int tune_tcp_socket (fd_t s_, int tcp_nodelay_ = 1);

//  Sets the socket send buffer size.
int set_tcp_send_buffer (fd_t sockfd_, int bufsize_);

//  Sets the socket receive buffer size.
int set_tcp_receive_buffer (fd_t sockfd_, int bufsize_);

//  Tunes TCP keep-alives
int tune_tcp_keepalives (
  fd_t s_, int keepalive_, int keepalive_cnt_, int keepalive_idle_, int keepalive_intvl_);

//  Tunes TCP max retransmit timeout
int tune_tcp_maxrt (fd_t sockfd_, int timeout_);

void tcp_tune_loopback_fast_path (fd_t socket_);

void tune_tcp_busy_poll (fd_t socket_, int busy_poll_);
}

#endif
