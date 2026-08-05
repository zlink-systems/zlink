/* SPDX-License-Identifier: MPL-2.0 */

#ifndef ZLINK_ASIO_TIMER_FLAG_HPP_INCLUDED
#define ZLINK_ASIO_TIMER_FLAG_HPP_INCLUDED

namespace zlink
{
template <typename cancel_fn_t>
inline void cancel_asio_timer_if_started (bool *started_, cancel_fn_t cancel_fn_)
{
    if (!started_ || !*started_)
        return;
    cancel_fn_ ();
    *started_ = false;
}

template <typename start_fn_t>
inline bool start_asio_timer_if_positive (int interval_, bool *started_, start_fn_t start_fn_)
{
    if (interval_ <= 0 || !started_)
        return false;
    start_fn_ (interval_);
    *started_ = true;
    return true;
}

template <typename reconnect_fn_t, typename timeout_fn_t>
inline bool handle_asio_connecter_timer_event (int id_,
                                               int reconnect_timer_id_,
                                               int connect_timer_id_,
                                               bool *reconnect_started_,
                                               bool *connect_started_,
                                               reconnect_fn_t reconnect_fn_,
                                               timeout_fn_t timeout_fn_)
{
    if (id_ == reconnect_timer_id_) {
        if (reconnect_started_)
            *reconnect_started_ = false;
        reconnect_fn_ ();
        return true;
    }

    if (id_ == connect_timer_id_) {
        if (connect_started_)
            *connect_started_ = false;
        timeout_fn_ ();
        return true;
    }

    return false;
}

template <typename cancel_reconnect_fn_t,
          typename cancel_connect_fn_t,
          typename close_fn_t,
          typename poll_fn_t>
inline void prepare_asio_connecter_termination (int linger_,
                                                bool *terminating_,
                                                int *stored_linger_,
                                                bool *reconnect_started_,
                                                bool *connect_started_,
                                                const bool *connecting_,
                                                cancel_reconnect_fn_t cancel_reconnect_fn_,
                                                cancel_connect_fn_t cancel_connect_fn_,
                                                close_fn_t close_fn_,
                                                poll_fn_t poll_fn_)
{
    if (terminating_)
        *terminating_ = true;
    if (stored_linger_)
        *stored_linger_ = linger_;

    cancel_asio_timer_if_started (reconnect_started_, cancel_reconnect_fn_);
    cancel_asio_timer_if_started (connect_started_, cancel_connect_fn_);

    close_fn_ ();

    if (connecting_ && *connecting_)
        poll_fn_ ();
}
}

#endif
