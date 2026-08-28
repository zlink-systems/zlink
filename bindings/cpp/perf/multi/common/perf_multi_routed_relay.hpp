#pragma once

#include "perf_common.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
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
    return std::getenv ("PERF_DEBUG") != NULL;
}

inline void debug_relay_error (const char *label, const char *operation, int err = errno)
{
    if (debug_enabled ())
        std::cerr << label << " " << operation << " failed errno=" << err << std::endl;
}

struct routed_reply_state_t
{
    std::atomic<size_t> in_flight{0};
    std::atomic<bool> failed{false};
    std::atomic<int> error{0};
};

inline bool is_stale_route (const zlink::submit_error_t &error)
{
    return error.result () == zlink::submit_result_t::not_connected
           || error.result () == zlink::submit_result_t::not_found;
}

inline perf::detached_async_task_t submit_routed_reply_async (
  zlink::router_socket_t &server,
  zlink::routing_id_t routing_id,
  std::vector<zlink::message_t> parts,
  routed_reply_state_t &state)
{
    try {
        if (parts.size () == 2) {
            co_await std::move (server.send (routing_id).message (parts[0]))
              .message (parts[1])
              .async ();
        } else if (parts.size () == 1) {
            co_await std::move (server.send (routing_id))
              .message (parts[0])
              .async ();
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
    state.in_flight.fetch_sub (1, std::memory_order_acq_rel);
}

} // namespace detail

inline bool run_routed_echo_relay (zlink::router_socket_t &server,
                                   std::atomic<bool> &stop_requested,
                                   const char *debug_label)
{
    detail::routed_reply_state_t replies;
    zlink::poller_t poller;
    poller.add (server, zlink::poll_event_flag_t::pollin, 0);
    std::vector<zlink::poll_event_t> events (1);

    while (!stop_requested.load (std::memory_order_acquire)
           && !replies.failed.load (std::memory_order_acquire)) {
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
            replies.error.store (err, std::memory_order_release);
            replies.failed.store (true, std::memory_order_release);
            break;
        }
        if (ready_count == 0)
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
                || received.request_seq ().has_value ()
                || !measurement_parts_valid (received.parts ())) {
                replies.error.store (EPROTO, std::memory_order_release);
                replies.failed.store (true, std::memory_order_release);
                break;
            }

            replies.in_flight.fetch_add (1, std::memory_order_acq_rel);
            detail::submit_routed_reply_async (
              server, *received.routing_id (), std::move (received.parts ()), replies);
        }
    }

    // Async reply continuations own message parts and reference the socket.
    // Drain them before the server socket leaves scope.
    while (replies.in_flight.load (std::memory_order_acquire) != 0)
        std::this_thread::yield ();

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
