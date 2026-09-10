#pragma once

#include "perf_common.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace perf
{
namespace multi
{

namespace detail
{

inline bool debug_enabled ()
{
    // Read once per process: PERF_DEBUG is a launch-time knob and this guard
    // is evaluated on the per-message path, so a per-call getenv would put
    // harness instrumentation inside the measured path. Same shape as the C
    // reference bench_debug_enabled().
    static const bool enabled = std::getenv ("PERF_DEBUG") != NULL;
    return enabled;
}

inline void debug_relay_error (const char *label, const char *operation, int err = errno)
{
    if (debug_enabled ())
        std::cerr << label << " " << operation << " failed errno=" << err << std::endl;
}

struct routed_reply_state_t
{
    // Public POLLCOMPLETION keeps this queue and its sender on the poll thread.
    std::deque<zlink::received_t> pending;
    bool sending = false;
    std::atomic<bool> failed{false};
    std::atomic<int> error{0};
};

inline bool is_stale_route (const zlink::submit_error_t &error)
{
    return error.result () == zlink::submit_result_t::not_connected
           || error.result () == zlink::submit_result_t::not_found;
}

inline perf::detached_async_task_t send_routed_replies_async (
  zlink::router_socket_t &server,
  routed_reply_state_t &state)
{
    state.sending = true;
    while (!state.pending.empty ()
           && !state.failed.load (std::memory_order_acquire)) {
        zlink::received_t &received = state.pending.front ();
        std::vector<zlink::message_t> &parts = received.parts ();
        try {
            if (parts.size () == 2) {
                zlink::send_submission_t submission =
                  server.send (*received.routing_id ()).message (parts[0])
                  .message (parts[1])
                  .async ();
                if (submission.result == ZLINK_SUBMIT_BACKPRESSURED)
                    co_await std::move (submission.admitted);
                else if (submission.result != ZLINK_SUBMIT_OK)
                    throw std::logic_error ("unexpected send submit result");
            } else if (parts.size () == 1) {
                zlink::send_submission_t submission =
                  server.send (*received.routing_id ())
                  .message (parts[0])
                  .async ();
                if (submission.result == ZLINK_SUBMIT_BACKPRESSURED)
                    co_await std::move (submission.admitted);
                else if (submission.result != ZLINK_SUBMIT_OK)
                    throw std::logic_error ("unexpected send submit result");
            } else {
                state.error.store (EPROTO, std::memory_order_release);
                state.failed.store (true, std::memory_order_release);
            }
        }
        catch (const zlink::submit_error_t &error) {
            if (!is_stale_route (error)) {
                state.error.store (error.internal_errno (), std::memory_order_release);
                state.failed.store (true, std::memory_order_release);
            }
        }
        catch (const zlink::binding_error_t &error) {
            state.error.store (error.internal_errno (), std::memory_order_release);
            state.failed.store (true, std::memory_order_release);
        }
        catch (...) {
            state.error.store (EIO, std::memory_order_release);
            state.failed.store (true, std::memory_order_release);
        }
        state.pending.pop_front ();
    }
    state.sending = false;
}

} // namespace detail

inline bool run_routed_echo_relay (zlink::router_socket_t &server,
                                   std::atomic<bool> &stop_requested,
                                   const char *debug_label,
                                   std::chrono::milliseconds drain_timeout)
{
    detail::routed_reply_state_t replies;
    zlink::poller_t poller;
    poller.add (server,
                zlink::poll_event_flag_t::pollin
                  | zlink::poll_event_flag_t::pollcompletion,
                0);
    std::vector<zlink::poll_event_t> events (1);
    bool draining = false;
    std::chrono::steady_clock::time_point drain_deadline;

    for (;;) {
        const bool accepting =
          !stop_requested.load (std::memory_order_acquire)
          && !replies.failed.load (std::memory_order_acquire);
        if (!accepting) {
            if (!draining) {
                draining = true;
                drain_deadline = std::chrono::steady_clock::now () + drain_timeout;
            }
            if (!replies.sending)
                break;
            if (std::chrono::steady_clock::now () >= drain_deadline) {
                try {
                    server.close ();
                }
                catch (const zlink::binding_error_t &) {
                }
                replies.error.store (ETIMEDOUT, std::memory_order_release);
                replies.failed.store (true, std::memory_order_release);
                break;
            }
        }

        size_t ready_count = 0;
        try {
            // The stdin watcher cannot wake this socket poll. Keep the wait
            // bounded solely so runner STOP/EOF is observed.
            ready_count = poller.wait (
              events.data (), events.size (), std::chrono::milliseconds (200));
        }
        catch (const zlink::binding_error_t &error) {
            const int err = error.internal_errno ();
            if (err == EINTR)
                continue;
            try {
                server.close ();
            }
            catch (const zlink::binding_error_t &) {
            }
            replies.error.store (err, std::memory_order_release);
            replies.failed.store (true, std::memory_order_release);
            break;
        }
        if (ready_count == 0)
            continue;

        // poller.wait() has already drained WRITABLE and resumed any matching
        // send. During shutdown, keep doing that without accepting new input.
        if (!accepting)
            continue;

        const short revents = static_cast<short> (events[0].revents);
        if ((revents & static_cast<short> (zlink::poll_event_flag_t::pollin)) == 0)
            continue;

        while (!stop_requested.load (std::memory_order_acquire)
               && !replies.failed.load (std::memory_order_acquire)) {
            zlink::received_t received;
            const int recv_rc = server.recv (received, zlink::recv_flags_t::dontwait);
            if (recv_rc != 0) {
                const int err = errno;
                if (recv_rc == static_cast<int> (zlink::recv_result_t::no_data)
                    || err == EAGAIN || err == EWOULDBLOCK || err == EINTR) {
                    break;
                }
                replies.error.store (err, std::memory_order_release);
                replies.failed.store (true, std::memory_order_release);
                break;
            }
            if (!received.routing_id ().has_value ()
                || received.routing_id ()->size () == 0
                || received.reply_token ().has_value ()
                || !measurement_parts_valid (received.parts ())) {
                replies.error.store (EPROTO, std::memory_order_release);
                replies.failed.store (true, std::memory_order_release);
                break;
            }

            // Match the C relay FIFO: keep receiving under backpressure, but
            // await admission before submitting the next reply on this socket.
            // The binding owns the single suspended send and its WRITABLE retry.
            replies.pending.emplace_back (std::move (received));
            if (!replies.sending)
                detail::send_routed_replies_async (server, replies);
        }
    }

    if (replies.failed.load (std::memory_order_acquire)) {
        const int err = replies.error.load (std::memory_order_acquire);
        errno = err != 0 ? err : EIO;
        detail::debug_relay_error (debug_label, "async send", errno);
        return false;
    }
    return true;
}

} // namespace multi
} // namespace perf
