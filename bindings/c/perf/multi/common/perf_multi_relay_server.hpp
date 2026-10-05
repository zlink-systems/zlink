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
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
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
    zlink_completion_id_t wait_token;

    pending_reply_t () : rid (), parts (), wait_token (0) {}

    pending_reply_t (pending_reply_t &&other) noexcept :
        rid (other.rid), parts (std::move (other.parts)), wait_token (other.wait_token)
    {
        std::memset (&other.rid, 0, sizeof (other.rid));
        other.wait_token = 0;
    }

    pending_reply_t &operator= (pending_reply_t &&other) noexcept
    {
        if (this != &other) {
            release_parts ();
            rid = other.rid;
            parts = std::move (other.parts);
            wait_token = other.wait_token;
            std::memset (&other.rid, 0, sizeof (other.rid));
            other.wait_token = 0;
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

typedef std::unordered_map<zlink_completion_id_t, pending_reply_t> pending_replies_t;

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
    reply_send_failed = 3,
    reply_send_reservation_full = 4
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

inline reply_send_status_t try_send_reply_now (void *server, pending_reply_t *reply)
{
    if (!server || !reply || reply->parts.empty ()) {
        errno = EINVAL;
        return reply_send_failed;
    }
    if (reply->wait_token != 0)
        return reply_send_backpressured;

    // Whole-record submit consumes every input even on failure. Send from a
    // fresh shared-storage copy so the
    // pending record remains an immutable retry snapshot.
    std::vector<zlink_msg_t> attempt (reply->parts.size ());
    size_t initialized_count = 0;
    for (size_t i = 0; i < reply->parts.size (); ++i) {
        if (zlink_msg_init (&attempt[i]) != 0) {
            const int init_errno = zlink_errno ();
            zlink_multipart_close (attempt.data (), initialized_count);
            if (init_errno != 0)
                errno = init_errno;
            return reply_send_failed;
        }
        ++initialized_count;
        if (zlink_msg_copy (&attempt[i], &reply->parts[i]) != ZLINK_CONFIG_OK) {
            const int copy_errno = zlink_errno ();
            zlink_multipart_close (attempt.data (), initialized_count);
            if (copy_errno != 0)
                errno = copy_errno;
            return reply_send_failed;
        }
    }

    zlink_submit_result_t send_rc = ZLINK_SUBMIT_INTERNAL_ERROR;
    zlink_completion_id_t wait_token = 0;
    send_rc = zlink_send_rid (server, &reply->rid, attempt.data (), attempt.size (),
                              static_cast<zlink_send_flags_t> (ZLINK_SEND_FLAGS_DONTWAIT), server,
                              &wait_token);
    int err = send_rc == ZLINK_SUBMIT_OK ? 0 : zlink_errno ();

    reply_send_status_t status = send_rc == ZLINK_SUBMIT_OK
                                   ? reply_send_ok
                                   : classify_reply_send_result (send_rc, err);
    if (status == reply_send_ok && wait_token != 0) {
        status = reply_send_failed;
        err = EPROTO;
    } else if (status == reply_send_backpressured) {
        if (err != EAGAIN && err != EWOULDBLOCK) {
            status = reply_send_failed;
            err = EPROTO;
        } else if (wait_token == 0) {
            status = reply_send_reservation_full;
        } else {
            reply->wait_token = wait_token;
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

inline bool
drain_reply_writable (void *server, pending_replies_t *pending, std::vector<pending_reply_t> *ready)
{
    if (!server || !pending || !ready) {
        errno = EINVAL;
        return false;
    }
    for (;;) {
        zlink_completion_t completion;
        std::memset (&completion, 0, sizeof (completion));
        completion.struct_size = sizeof (completion);
        const zlink_recv_result_t rc = zlink_completion_recv (
          server, &completion, ZLINK_RECV_FLAGS_DONTWAIT);
        if (rc == ZLINK_RECV_NO_DATA)
            return true;
        if (rc != ZLINK_RECV_OK)
            return false;
        pending_replies_t::iterator found = pending->find (completion.completion_id);
        const bool valid =
          found != pending->end () && completion.kind == ZLINK_COMPLETION_WRITABLE
          && completion.completion_id != 0 && completion.user_context == server
          && completion.completion_id == found->second.wait_token
          && perf_multi_client::routing_ids_equal (completion.peer_rid, found->second.rid);
        const zlink_send_complete_result_t result = completion.send_result;
        const int terminal_errno = completion.send_terminal_errno;
        zlink_completion_close (&completion);
        if (!valid) {
            errno = EPROTO;
            return false;
        }

        found->second.wait_token = 0;
        const perf_multi_client::writable_outcome_t outcome =
          perf_multi_client::classify_writable_outcome (result, terminal_errno);
        if (outcome == perf_multi_client::writable_retry) {
            ready->push_back (std::move (found->second));
            pending->erase (found);
            continue;
        }
        if (outcome == perf_multi_client::writable_not_found) {
            pending->erase (found);
            continue;
        }
        errno = terminal_errno != 0 ? terminal_errno : EIO;
        return false;
    }
}

inline bool retain_or_send_reply (pending_replies_t *pending,
                                  std::unique_ptr<pending_reply_t> *reservation_full,
                                  pending_reply_t &&reply,
                                  reply_send_status_t status)
{
    if (status == reply_send_ok || status == reply_send_stale_route)
        return true;
    if (status == reply_send_backpressured) {
        const zlink_completion_id_t token = reply.wait_token;
        if (!pending->emplace (token, std::move (reply)).second) {
            errno = EPROTO;
            return false;
        }
        return true;
    }
    if (status == reply_send_reservation_full) {
        if (*reservation_full) {
            errno = EPROTO;
            return false;
        }
        reservation_full->reset (new pending_reply_t (std::move (reply)));
        return true;
    }
    return false;
}

inline bool retain_or_send_reply (void *server,
                                  pending_replies_t *pending,
                                  std::unique_ptr<pending_reply_t> *reservation_full,
                                  pending_reply_t &&reply)
{
    const reply_send_status_t status = try_send_reply_now (server, &reply);
    return retain_or_send_reply (pending, reservation_full, std::move (reply), status);
}

inline bool flush_pending_replies (void *server,
                                   pending_replies_t *pending,
                                   std::unique_ptr<pending_reply_t> *reservation_full,
                                   std::vector<pending_reply_t> *ready)
{
    // N completions release N reservations. Retrying the one tokenless record first,
    // then N ready records, can leave at most one tokenless record; a second is EPROTO.
    if (*reservation_full) {
        pending_reply_t reply = std::move (**reservation_full);
        reservation_full->reset ();
        if (!retain_or_send_reply (server, pending, reservation_full, std::move (reply)))
            return false;
    }
    for (size_t i = 0; i < ready->size (); ++i) {
        if (!retain_or_send_reply (server, pending, reservation_full, std::move ((*ready)[i])))
            return false;
    }
    ready->clear ();
    return true;
}

inline bool drain_recv_and_relay (void *server,
                                  pending_replies_t *pending,
                                  std::unique_ptr<pending_reply_t> *reservation_full,
                                  bool *recv_drained)
{
    if (recv_drained)
        *recv_drained = false;
    if (!pending || !reservation_full || !server) {
        errno = EINVAL;
        return false;
    }

    while (true) {
        if (perf_stop_requested ().load (std::memory_order_acquire))
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
        pending_reply_t reply;
        if (!capture_pending_reply (&source_rid, parts, part_count, &reply))
            return false;
        if (!retain_or_send_reply (server, pending, reservation_full, std::move (reply)))
            return false;
        if (*reservation_full)
            return true;
    }
}

inline bool run_server_loop (void *server)
{
    if (!server)
        return false;

    pending_replies_t pending;
    std::unique_ptr<pending_reply_t> reservation_full;
    std::vector<pending_reply_t> ready;

    void *poller = zlink_poller_new ();
    short registered_events = static_cast<short> (ZLINK_POLLIN
                                                   | ZLINK_POLLCOMPLETION);
    if (!poller
        || zlink_poller_add (poller, server, &pending, registered_events) != ZLINK_CONFIG_OK) {
        if (poller)
            zlink_poller_destroy (&poller);
        return false;
    }

    bool loop_ok = true;
    while (!perf_stop_requested ().load (std::memory_order_acquire)) {
        short desired_events = ZLINK_POLLCOMPLETION;
        if (!reservation_full)
            desired_events = static_cast<short> (desired_events | ZLINK_POLLIN);
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
        if (poll_rc > 0 && event.user_data != &pending) {
            errno = EPROTO;
            loop_ok = false;
            break;
        }
        if (poll_rc > 0 && (event.events & ZLINK_POLLCOMPLETION) != 0) {
            if (!drain_reply_writable (server, &pending, &ready)) {
                loop_ok = false;
                break;
            }
            if (!ready.empty () || reservation_full) {
                if (!flush_pending_replies (server, &pending, &reservation_full, &ready)) {
                    loop_ok = false;
                    break;
                }
            }
        }
        if (poll_rc > 0 && !reservation_full && (event.events & ZLINK_POLLIN) != 0) {
            bool recv_drained = false;
            if (!drain_recv_and_relay (server, &pending, &reservation_full, &recv_drained)) {
                loop_ok = false;
                break;
            }
        }
    }

    // CLIENT_DONE can reach the runner while replies still have wait tokens.
    const std::chrono::steady_clock::time_point drain_deadline =
      std::chrono::steady_clock::now ()
      + std::chrono::milliseconds (
        perf_multi_client::send_retry_drain_timeout_ms ());
    if (registered_events != ZLINK_POLLCOMPLETION
        && zlink_poller_modify (poller, server, ZLINK_POLLCOMPLETION) != ZLINK_CONFIG_OK)
        loop_ok = false;
    while (loop_ok && (!pending.empty () || reservation_full)
           && std::chrono::steady_clock::now () < drain_deadline) {
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
        if (event.socket != server || event.user_data != &pending) {
            errno = EPROTO;
            loop_ok = false;
            break;
        }
        if ((event.events & ZLINK_POLLCOMPLETION) != 0
            && !drain_reply_writable (server, &pending, &ready)) {
            loop_ok = false;
            break;
        }
        if ((!ready.empty () || reservation_full)
            && !flush_pending_replies (server, &pending, &reservation_full, &ready)) {
            loop_ok = false;
            break;
        }
    }
    if (loop_ok && (!pending.empty () || reservation_full)) {
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
