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

//  Endpoint release can precede the ownership termination command. Keep both
//  lifecycle stages in the existing listener state, so release alone cannot
//  authorize destruction from an accept callback.
enum class asio_listener_state_t
{
    accepting,
    endpoint_released,
    terminating
};

//  This is the only decision to advance ownership termination. The caller must
//  return without accessing the listener after this callback: it may delete it.
template <typename own_process_term_fn_t>
inline void finish_asio_listener_termination (
  asio_listener_state_t state_, size_t accepting_count_, int linger_,
  own_process_term_fn_t own_process_term_fn_)
{
    if (state_ == asio_listener_state_t::terminating && accepting_count_ == 0)
        own_process_term_fn_ (linger_);
}

template <typename release_endpoint_fn_t, typename own_process_term_fn_t>
inline void prepare_asio_listener_termination (
  int linger_, int *stored_linger_, asio_listener_state_t *state_,
  const size_t *accepting_count_, release_endpoint_fn_t release_endpoint_fn_,
  own_process_term_fn_t own_process_term_fn_)
{
    *stored_linger_ = linger_;
    release_endpoint_fn_ ();
    *state_ = asio_listener_state_t::terminating;
    finish_asio_listener_termination (*state_, *accepting_count_, linger_,
                                      own_process_term_fn_);
}

//  Account for every completion before deciding whether to create a session.
//  During shutdown the final completion continues ownership termination, after
//  closing any socket accepted before endpoint release.
template <typename socket_ptr_t, typename own_process_term_fn_t>
inline bool complete_asio_listener_accept (
  asio_listener_state_t state_, size_t *accepting_count_, int linger_,
  const socket_ptr_t &accept_socket_,
  const boost::system::error_code &ec_,
  own_process_term_fn_t own_process_term_fn_)
{
    zlink_assert (*accepting_count_ > 0);
    --*accepting_count_;
    if (state_ == asio_listener_state_t::accepting)
        return false;

    if (!ec_ && accept_socket_ && accept_socket_->is_open ()) {
        boost::system::error_code close_ec;
        accept_socket_->close (close_ec);
    }
    finish_asio_listener_termination (state_, *accepting_count_, linger_,
                                      own_process_term_fn_);
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
