/* SPDX-License-Identifier: MPL-2.0 */

#ifndef ZLINK_ASIO_LISTENER_ACCEPT_POLICY_HPP_INCLUDED
#define ZLINK_ASIO_LISTENER_ACCEPT_POLICY_HPP_INCLUDED

#include "core/options.hpp"
#include "utils/err.hpp"
#include "utils/env.hpp"

#include <boost/asio/io_context.hpp>
#include <memory>
#include <new>

namespace zlink
{
inline size_t asio_stream_accept_target (const options_t &options_)
{
    if (options_.type != ZLINK_CORE_SOCKET_STREAM)
        return 1;
    return env::asio_stream_accept_concurrency ();
}

inline void drain_asio_listener_pending_accepts (boost::asio::io_context &io_context_,
                                                 const size_t *accepting_count_)
{
    for (size_t i = 0; i < 4096 && accepting_count_ && *accepting_count_ > 0; ++i)
        io_context_.poll_one ();
}

template <typename socket_t, typename acceptor_t, typename trace_fn_t, typename accept_fn_t>
inline void start_asio_listener_accepts (boost::asio::io_context &io_context_,
                                         acceptor_t &acceptor_,
                                         size_t *accepting_count_,
                                         const options_t &options_,
                                         trace_fn_t trace_fn_,
                                         accept_fn_t accept_fn_)
{
    if (!acceptor_.is_open () || !accepting_count_)
        return;

    const size_t target_accepts = asio_stream_accept_target (options_);
    while (*accepting_count_ < target_accepts && acceptor_.is_open ()) {
        const std::shared_ptr<socket_t> accept_socket (new (std::nothrow) socket_t (io_context_));
        alloc_assert (accept_socket.get ());

        ++*accepting_count_;
        trace_fn_ (*accepting_count_, target_accepts);
        acceptor_.async_accept (*accept_socket,
                                [accept_fn_, accept_socket] (
                                  const boost::system::error_code &ec) {
                                    accept_fn_ (accept_socket, ec);
                                });
    }
}
}

#endif
