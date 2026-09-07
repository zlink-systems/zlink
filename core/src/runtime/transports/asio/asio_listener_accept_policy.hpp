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

//  Same-behaviour, same-ordering termination sequence shared by the asio
//  listeners (tcp/ipc/tls/ws): store linger, release the endpoint, drain any
//  async_accept queued before release (so its callback still fires while the
//  listener is alive, as a no-op via _terminating), then hand off to
//  own_t::process_term. Mirrors prepare_asio_connecter_termination in
//  asio_timer_flag.hpp for the connecter side.
template <typename release_endpoint_fn_t, typename own_process_term_fn_t>
inline void prepare_asio_listener_termination (int linger_,
                                               int *stored_linger_,
                                               boost::asio::io_context &io_context_,
                                               const size_t *accepting_count_,
                                               release_endpoint_fn_t release_endpoint_fn_,
                                               own_process_term_fn_t own_process_term_fn_)
{
    if (stored_linger_)
        *stored_linger_ = linger_;

    release_endpoint_fn_ ();

    //  Process any pending handlers (including the cancelled async_accept)
    //  to ensure the callback fires while the object is still alive.
    //  The _terminating flag ensures the callback is a no-op.
    drain_asio_listener_pending_accepts (io_context_, accepting_count_);

    //  Now it's safe to call own_t::process_term to terminate child sessions
    own_process_term_fn_ (linger_);
}

//  Shared "ignore this accept, we're terminating" check used by the asio
//  listeners' on_accept callbacks: an async_accept queued before endpoint
//  release must never create a session, but any socket it did manage to
//  accept still needs to be closed. Returns true if the caller should bail
//  out of on_accept without proceeding.
template <typename socket_ptr_t>
inline bool cancel_asio_listener_accept_if_terminating (bool terminating_,
                                                        const socket_ptr_t &accept_socket_,
                                                        const boost::system::error_code &ec_)
{
    if (!terminating_)
        return false;

    if (!ec_ && accept_socket_ && accept_socket_->is_open ()) {
        boost::system::error_code close_ec;
        accept_socket_->close (close_ec);
    }
    return true;
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
        const std::shared_ptr<socket_t> accept_socket =
          std::make_shared<socket_t> (io_context_);
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
