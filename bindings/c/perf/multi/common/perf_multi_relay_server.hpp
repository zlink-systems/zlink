#ifndef PERF_MULTI_RELAY_SERVER_HPP
#define PERF_MULTI_RELAY_SERVER_HPP

#include "perf_common.hpp"
#include "perf_common_multi.hpp"
#include "perf_multi_client_helpers.hpp"
#include "../../common/perf_tls_setup.hpp"
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace perf_multi_relay_server
{

using ::setup_tls_server;

static std::atomic<int> g_debug_relay_logs (0);

inline std::string format_rid_debug (const zlink_routing_id_t *rid)
{
    if (!rid || rid->size == 0)
        return "<empty>";

    std::ostringstream os;
    for (size_t i = 0; i < rid->size; ++i) {
        const unsigned char c = rid->data[i];
        if (i != 0)
            os << ' ';
        if (c >= 32 && c <= 126)
            os << static_cast<char> (c);
        else
            os << '.';
        os << std::hex << std::uppercase << std::setw (2) << std::setfill ('0')
           << static_cast<unsigned> (c) << std::dec;
    }
    return os.str ();
}

struct relay_server_config_t
{
    relay_server_config_t () :
        pattern_name (NULL),
        token (NULL),
        socket_type (ZLINK_SOCKET_ROUTER),
        has_server_routing_id (false),
        server_routing_id (NULL),
        msg_size (0)
    {
    }

    const char *pattern_name;
    const char *token;
    zlink_socket_type_t socket_type;
    bool has_server_routing_id;
    const char *server_routing_id;
    size_t msg_size;
};

// Owned snapshot of one routed reply that has not been sent yet because the
// socket reported EAGAIN. Holds an immutable copy of the routing id (the
// pointer returned by zlink_router_recv is to zlink internal storage and may
// be invalidated by subsequent recv calls) and the moved-out message parts.
struct pending_reply_t
{
    zlink_routing_id_t rid;
    std::vector<zlink_msg_t> parts;

    pending_reply_t () : rid (), parts () {}

    pending_reply_t (pending_reply_t &&other) noexcept :
        rid (other.rid), parts (std::move (other.parts))
    {
        std::memset (&other.rid, 0, sizeof (other.rid));
    }

    pending_reply_t &operator= (pending_reply_t &&other) noexcept
    {
        if (this != &other) {
            release_parts ();
            rid = other.rid;
            parts = std::move (other.parts);
            std::memset (&other.rid, 0, sizeof (other.rid));
        }
        return *this;
    }

    pending_reply_t (const pending_reply_t &) = delete;
    pending_reply_t &operator= (const pending_reply_t &) = delete;

    ~pending_reply_t () { release_parts (); }

    void release_parts ()
    {
        if (!parts.empty ()) {
            zlink_multipart_close (parts.data (), parts.size ());
            parts.clear ();
        }
    }
};

inline void close_received_reply_parts (zlink_msg_t *parts, size_t part_count)
{
    if (!parts)
        return;
    zlink_multipart_close (parts, part_count);
    std::free (parts);
}

inline bool capture_pending_reply (const zlink_routing_id_t *source_rid,
                                   zlink_msg_t *parts,
                                   size_t part_count,
                                   pending_reply_t *out)
{
    if (!source_rid || !parts || part_count == 0 || !out)
        return false;

    out->release_parts ();
    out->rid = *source_rid;
    out->parts.resize (part_count);
    for (size_t i = 0; i < part_count; ++i) {
        if (zlink_msg_init (&out->parts[i]) != 0) {
            for (size_t j = 0; j < i; ++j)
                zlink_msg_close (&out->parts[j]);
            out->parts.clear ();
            close_received_reply_parts (parts, part_count);
            return false;
        }
    }
    for (size_t i = 0; i < part_count; ++i) {
        if (zlink_msg_move (&out->parts[i], &parts[i]) != 0) {
            for (size_t j = 0; j < part_count; ++j)
                zlink_msg_close (&out->parts[j]);
            out->parts.clear ();
            close_received_reply_parts (parts, part_count);
            return false;
        }
    }
    // move leaves every source as an empty initialized message. Close those
    // handles and release the malloc/realloc-owned receive array.
    close_received_reply_parts (parts, part_count);
    return true;
}

enum reply_send_status_t
{
    reply_send_ok = 0,
    reply_send_backpressured = 1,
    reply_send_stale_route = 2,
    reply_send_failed = 3
};

// Keep at most one immutable application-owned reply while its target-specific
// wait token is live. Further requests remain behind the socket receive HWM
// instead of moving into an unbounded relay queue.
struct reply_wait_state_t
{
    reply_wait_state_t () :
        socket (NULL), wait_token (0), target_rid (),
        drop_retained_reply (false), pollout_suppressed (false)
    {
    }

    void *socket;
    zlink_completion_id_t wait_token;
    zlink_routing_id_t target_rid;
    bool drop_retained_reply;
    bool pollout_suppressed;
};

inline reply_send_status_t classify_reply_send_result (zlink_submit_result_t result,
                                                       int err)
{
    if (result == ZLINK_SUBMIT_NOT_CONNECTED || result == ZLINK_SUBMIT_NOT_FOUND)
        return reply_send_stale_route;
    if (result == ZLINK_SUBMIT_BACKPRESSURED
        && (err == EAGAIN || err == EWOULDBLOCK))
        return reply_send_backpressured;
    return reply_send_failed;
}

inline reply_send_status_t try_send_reply_now (void *server,
                                               const zlink_routing_id_t *source_rid,
                                               zlink_msg_t *parts,
                                               size_t part_count,
                                               reply_wait_state_t *wait_state)
{
    if (!wait_state || wait_state->socket != server) {
        errno = EINVAL;
        return reply_send_failed;
    }
    if (!source_rid || !parts || part_count == 0) {
        errno = EINVAL;
        return reply_send_failed;
    }
    if (wait_state->wait_token != 0)
        return reply_send_backpressured;

    // Every part submit consumes its input even on failure and discards an
    // already staged prefix. Send from a fresh shared-storage copy so the
    // pending record remains an immutable retry snapshot.
    std::vector<zlink_msg_t> attempt (part_count);
    size_t initialized_count = 0;
    for (size_t i = 0; i < part_count; ++i) {
        if (zlink_msg_init (&attempt[i]) != 0) {
            const int init_errno = zlink_errno ();
            zlink_multipart_close (attempt.data (), initialized_count);
            if (init_errno != 0)
                errno = init_errno;
            return reply_send_failed;
        }
        ++initialized_count;
        if (zlink_msg_copy (&attempt[i], &parts[i]) != ZLINK_CONFIG_OK) {
            const int copy_errno = zlink_errno ();
            zlink_multipart_close (attempt.data (), initialized_count);
            if (copy_errno != 0)
                errno = copy_errno;
            return reply_send_failed;
        }
    }

    zlink_submit_result_t send_rc = ZLINK_SUBMIT_INTERNAL_ERROR;
    int err = 0;
    zlink_completion_id_t wait_token = 0;
    bool final_attempted = false;
    for (size_t i = 0; i < part_count; ++i) {
        const bool is_final = i + 1 == part_count;
        final_attempted = is_final;
        send_rc = zlink_send_part_rid (
          server, source_rid, &attempt[i],
          static_cast<zlink_send_flags_t> (ZLINK_SEND_FLAGS_DONTWAIT),
          is_final ? ZLINK_PART_FINAL : ZLINK_PART_MORE,
          is_final ? server : NULL,
          is_final ? &wait_token : NULL);
        if (send_rc != ZLINK_SUBMIT_OK)
            err = zlink_errno ();

        if (send_rc != ZLINK_SUBMIT_OK)
            break;
    }

    reply_send_status_t status = send_rc == ZLINK_SUBMIT_OK
                                   ? reply_send_ok
                                   : classify_reply_send_result (send_rc, err);
    if (status == reply_send_ok && wait_token != 0) {
        status = reply_send_failed;
        err = EPROTO;
    } else if (status == reply_send_backpressured) {
        if (!final_attempted || wait_token == 0
            || (err != EAGAIN && err != EWOULDBLOCK)) {
            status = reply_send_failed;
            err = EPROTO;
        } else {
            wait_state->wait_token = wait_token;
            wait_state->target_rid = *source_rid;
            wait_state->pollout_suppressed = false;
        }
    }
    // Submitted entries and any untouched suffix are all initialized handles.
    zlink_multipart_close (attempt.data (), attempt.size ());
    if (err != 0)
        errno = err;
    if (status == reply_send_ok)
        return status;
    if (status != reply_send_failed)
        return status;

    if (bench_debug_enabled ()) {
        std::cerr << "[perf-multi-relay] reply send failed err=" << err << std::endl;
    }
    return reply_send_failed;
}

inline bool drain_reply_writable (void *server,
                                  reply_wait_state_t *state,
                                  bool suppress_pollout_if_empty = false)
{
    if (!server || !state || state->socket != server) {
        errno = EINVAL;
        return false;
    }

    for (;;) {
        zlink_completion_t completion;
        std::memset (&completion, 0, sizeof (completion));
        completion.struct_size = sizeof (completion);
        const zlink_recv_result_t rc = zlink_completion_recv (
          server, &completion, ZLINK_RECV_FLAGS_DONTWAIT);
        if (rc == ZLINK_RECV_NO_DATA) {
            if (suppress_pollout_if_empty && state->wait_token != 0)
                state->pollout_suppressed = true;
            return true;
        }
        if (rc != ZLINK_RECV_OK)
            return false;

        const bool valid = completion.kind == ZLINK_COMPLETION_WRITABLE
                           && completion.completion_id != 0
                           && completion.completion_id == state->wait_token
                           && completion.user_context == server
                           && perf_multi_client::routing_ids_equal (
                             completion.peer_rid, state->target_rid);
        const zlink_send_complete_result_t result = completion.send_result;
        const int terminal_errno = completion.send_terminal_errno;
        zlink_completion_close (&completion);
        if (!valid) {
            errno = EPROTO;
            return false;
        }

        state->wait_token = 0;
        state->pollout_suppressed = false;
        std::memset (&state->target_rid, 0, sizeof (state->target_rid));
        if (result == ZLINK_SEND_ADMITTED && terminal_errno == 0)
            continue;
        if (result == ZLINK_SEND_TERMINAL && terminal_errno == ENOENT) {
            // A route can disappear while its wait token is live. Drop the
            // retained reply just like an immediate stale-route result.
            state->drop_retained_reply = true;
            continue;
        }
        errno = terminal_errno != 0 ? terminal_errno : EIO;
        return false;
    }
}

inline bool flush_pending_replies (void *server,
                                   std::deque<pending_reply_t> *pending,
                                   reply_wait_state_t *wait_state)
{
    if (!pending || !wait_state)
        return true;
    if (wait_state->drop_retained_reply) {
        if (pending->empty ()) {
            errno = EPROTO;
            return false;
        }
        pending->pop_front ();
        wait_state->drop_retained_reply = false;
    }
    while (!pending->empty ()) {
        pending_reply_t &front = pending->front ();
        const reply_send_status_t status = try_send_reply_now (
          server, &front.rid, front.parts.data (), front.parts.size (),
          wait_state);
        if (status == reply_send_ok) {
            // The attempt consumed only a copy. Popping closes the immutable
            // snapshot exactly once.
            pending->pop_front ();
            continue;
        }
        if (status == reply_send_backpressured)
            return true;
        if (status == reply_send_stale_route) {
            // The source route no longer exists. Drop this reply and continue
            // so one disconnected peer cannot pin the global FIFO head.
            pending->pop_front ();
            continue;
        }
        return false;
    }
    return true;
}

inline bool drain_recv_and_relay (void *server,
                                  std::deque<pending_reply_t> *pending,
                                  reply_wait_state_t *wait_state,
                                  bool *recv_drained)
{
    if (recv_drained)
        *recv_drained = false;
    if (!pending || !wait_state || wait_state->socket != server) {
        errno = EINVAL;
        return false;
    }

    while (true) {
        if (perf_stop_requested ().load (std::memory_order_acquire))
            return true;
        if (!pending->empty () || wait_state->wait_token != 0)
            return true;

        zlink_routing_id_t source_rid;
        bool has_source_rid = false;
        zlink_reply_token_t reply_token = 0;
        zlink_msg_t *parts = NULL;
        size_t part_count = 0;
        const zlink_recv_result_t rc = ::perf_zlink_router_recv_parts (
          server, &source_rid, &has_source_rid, &reply_token, &parts, &part_count,
          static_cast<zlink_recv_flags_t> (ZLINK_RECV_FLAGS_DONTWAIT));
        if (rc != ZLINK_RECV_OK) {
            const int err = zlink_errno ();
            if (err == EAGAIN || err == EWOULDBLOCK || err == EINTR) {
                if (recv_drained)
                    *recv_drained = true;
                return true;
            }
            if (bench_debug_enabled ()) {
                std::cerr << "[perf-multi-relay] recv failed err=" << err << std::endl;
            }
            return false;
        }

        if (!has_source_rid || source_rid.size == 0 || reply_token != 0
            || part_count == 0 || !parts) {
            close_received_reply_parts (parts, part_count);
            errno = EPROTO;
            return false;
        }
        if (bench_debug_enabled ()
            && g_debug_relay_logs.fetch_add (1, std::memory_order_acq_rel) < 12) {
            std::cerr << "[perf-multi-relay] echo request size="
                      << (part_count > 0 && parts ? zlink_msg_size (&parts[0]) : 0)
                      << " rid_size=" << static_cast<int> (source_rid.size)
                      << " rid=" << format_rid_debug (&source_rid) << " part_count=" << part_count
                      << std::endl;
        }

        // Take ownership before the first submit. A failed multipart attempt
        // consumes its copies, while this one-record application snapshot can
        // be retried intact.
        pending->emplace_back ();
        if (!capture_pending_reply (&source_rid, parts, part_count, &pending->back ())) {
            pending->pop_back ();
            return false;
        }
        if (!flush_pending_replies (server, pending, wait_state))
            return false;
    }
}

inline bool run_server_loop (void *server)
{
    if (!server)
        return false;

    std::deque<pending_reply_t> pending;
    reply_wait_state_t wait_state;
    wait_state.socket = server;

    void *poller = zlink_poller_new ();
    short registered_events = static_cast<short> (ZLINK_POLLIN
                                                   | ZLINK_POLLCOMPLETION);
    if (!poller
        || zlink_poller_add (poller, server, &wait_state,
                             registered_events)
             != ZLINK_CONFIG_OK) {
        if (poller)
            zlink_poller_destroy (&poller);
        return false;
    }

    bool loop_ok = true;
    while (!perf_stop_requested ().load (std::memory_order_acquire)) {
        short desired_events = ZLINK_POLLCOMPLETION;
        if (pending.empty () && wait_state.wait_token == 0)
            desired_events = static_cast<short> (desired_events | ZLINK_POLLIN);
        // ROUTER POLLOUT is socket-wide. Try it once for a new exact-target
        // token, then suppress it after a NO_DATA pull so another writable RID
        // cannot spin this loop; target WRITABLE still wakes POLLCOMPLETION.
        if (!pending.empty ()
            && (wait_state.wait_token == 0
                || !wait_state.pollout_suppressed))
            desired_events = static_cast<short> (desired_events | ZLINK_POLLOUT);
        if (desired_events != registered_events) {
            if (zlink_poller_modify (poller, server, desired_events)
                != ZLINK_CONFIG_OK) {
                loop_ok = false;
                break;
            }
            registered_events = desired_events;
        }

        // The stdin watcher sets perf_stop_requested(), but it cannot wake a
        // socket poll that waits forever. Use the common auxiliary wait so a
        // STOP command is observed promptly after the last client message.
        zlink_poller_event_t event;
        std::memset (&event, 0, sizeof (event));
        const int poll_rc = zlink_poller_wait (
          poller, &event, 1, perf_aux_poll_wait_ms (), NULL);
        if (poll_rc < 0) {
            if (zlink_errno () == EINTR)
                continue;
            if (bench_debug_enabled ()) {
                std::cerr << "[perf-multi-relay] poll failed err=" << zlink_errno () << std::endl;
            }
            loop_ok = false;
            break;
        }
        if (perf_stop_requested ().load (std::memory_order_acquire))
            break;
        if (poll_rc > 0 && event.user_data != &wait_state) {
            errno = EPROTO;
            loop_ok = false;
            break;
        }
        if (poll_rc > 0
            && (event.events & (ZLINK_POLLOUT | ZLINK_POLLCOMPLETION)) != 0
            && wait_state.wait_token != 0) {
            if (!drain_reply_writable (server, &wait_state, true)) {
                loop_ok = false;
                break;
            }
        }
        if (poll_rc > 0
            && (event.events & (ZLINK_POLLOUT | ZLINK_POLLCOMPLETION)) != 0
            && wait_state.wait_token == 0 && !pending.empty ()) {
            if (!flush_pending_replies (server, &pending,
                                        &wait_state)) {
                loop_ok = false;
                break;
            }
        }
        if (poll_rc > 0 && (event.events & ZLINK_POLLIN) != 0
            && pending.empty () && wait_state.wait_token == 0) {
            bool recv_drained = false;
            if (!drain_recv_and_relay (server, &pending, &wait_state,
                                       &recv_drained)) {
                loop_ok = false;
                break;
            }
        }
    }

    // CLIENT_DONE can reach the runner while the relay's final reply still has
    // a live wait token. Admit the one immutable retry before normal socket
    // teardown, but keep the shutdown path bounded if Core or a route stalls.
    const std::chrono::steady_clock::time_point drain_deadline =
      std::chrono::steady_clock::now ()
      + std::chrono::milliseconds (
        perf_multi_client::send_retry_drain_timeout_ms ());
    while (loop_ok
           && (!pending.empty () || wait_state.wait_token != 0)
           && std::chrono::steady_clock::now () < drain_deadline) {
        if (wait_state.wait_token != 0
            && !drain_reply_writable (server, &wait_state)) {
            loop_ok = false;
            break;
        }
        if (wait_state.wait_token == 0 && !pending.empty ()
            && !flush_pending_replies (server, &pending, &wait_state)) {
            loop_ok = false;
            break;
        }
        if (pending.empty () && wait_state.wait_token == 0)
            break;

        const short drain_events = static_cast<short> (
          ZLINK_POLLCOMPLETION
          | (!pending.empty ()
                 && (wait_state.wait_token == 0
                     || !wait_state.pollout_suppressed)
               ? ZLINK_POLLOUT
               : 0));
        if (drain_events != registered_events) {
            if (zlink_poller_modify (poller, server, drain_events)
                != ZLINK_CONFIG_OK) {
                loop_ok = false;
                break;
            }
            registered_events = drain_events;
        }

        const int wait_ms = static_cast<int> (std::max<long long> (
          1, std::min<long long> (
               50, std::chrono::duration_cast<std::chrono::milliseconds> (
                     drain_deadline - std::chrono::steady_clock::now ())
                     .count ())));
        zlink_poller_event_t event;
        std::memset (&event, 0, sizeof (event));
        const int poll_rc = zlink_poller_wait (poller, &event, 1, wait_ms, NULL);
        if (poll_rc < 0) {
            if (zlink_errno () == EINTR || zlink_errno () == EAGAIN)
                continue;
            loop_ok = false;
            break;
        }
        if (poll_rc == 0)
            continue;
        if (event.socket != server || event.user_data != &wait_state) {
            errno = EPROTO;
            loop_ok = false;
            break;
        }
        if ((event.events & (ZLINK_POLLOUT | ZLINK_POLLCOMPLETION)) != 0
            && wait_state.wait_token != 0
            && !drain_reply_writable (server, &wait_state, true)) {
            loop_ok = false;
            break;
        }
        if ((event.events & (ZLINK_POLLOUT | ZLINK_POLLCOMPLETION)) != 0
            && wait_state.wait_token == 0 && !pending.empty ()
            && !flush_pending_replies (server, &pending, &wait_state)) {
            loop_ok = false;
            break;
        }
    }
    if (loop_ok && (!pending.empty () || wait_state.wait_token != 0)) {
        errno = ETIMEDOUT;
        loop_ok = false;
    }

    if (zlink_poller_destroy (&poller) != ZLINK_CLOSE_OK)
        loop_ok = false;
    return loop_ok;
}

inline int run_server_benchmark (const relay_server_config_t &config,
                                 const std::string &lib_name,
                                 const std::string &transport)
{
    set_perf_multi_pattern_env (config.pattern_name);

    if (!perf_multi_client::is_supported_transport (transport)) {
        std::cout << "UNSUPPORTED," << lib_name << "," << config.pattern_name << "," << transport
                  << std::endl;
        return 0;
    }

    if (!transport_available (transport)) {
        std::cerr << "transport unavailable: " << transport << std::endl;
        return 1;
    }

    ctx_guard_t ctx;
    if (!ctx.valid ())
        return 1;

    void *server = zlink_socket (ctx.get (), config.socket_type);
    if (!server)
        return 1;

    const multi_bench_settings_t settings = resolve_multi_bench_settings ();
    const std::vector<size_t> sizes = resolve_bench_msg_sizes (64);
    const size_t msg_size = config.msg_size > 0 ? config.msg_size : sizes.front ();
    if (msg_size == 0) {
        zlink_close (server);
        return 1;
    }
    const int linger_ms = 0;
    set_sockopt_int (server, ZLINK_OPT_LINGER, linger_ms, "ZLINK_OPT_LINGER");
    apply_benchmark_hwm (server, settings.hwm);
    if (config.has_server_routing_id && config.server_routing_id) {
        zlink_set_routing_id (server, config.server_routing_id,
                              std::strlen (config.server_routing_id));
    }

    if (!setup_tls_server (server, transport)) {
        zlink_close (server);
        return 1;
    }

    const std::string endpoint = bind_server_endpoint (
      server, transport, lib_name + std::string ("_") + config.token + "_server");
    if (endpoint.empty ()) {
        zlink_close (server);
        return 1;
    }
    if (zlink_ctx_auto_hwm_recalculate (ctx.get ()) != ZLINK_CONFIG_OK) {
        zlink_close (server);
        return 1;
    }
    perf_print_auto_hwm_snapshot (server, false, "server", transport, true, msg_size,
                                  config.socket_type);
    perf_stop_requested ().store (false, std::memory_order_release);
    install_perf_signal_handlers ();

    std::thread stdin_watcher ([] () {
        std::string line;
        while (std::getline (std::cin, line)) {
            if (line == "STOP" || line == "QUIT") {
                perf_stop_requested ().store (true, std::memory_order_release);
                return;
            }
        }
        perf_stop_requested ().store (true, std::memory_order_release);
    });
    stdin_watcher.detach ();

    std::cout << "READY," << endpoint << std::endl;

    const bool loop_ok =
      run_server_loop (server);

    zlink_close (server);
    return loop_ok ? 0 : 1;
}

} // namespace perf_multi_relay_server

#endif
