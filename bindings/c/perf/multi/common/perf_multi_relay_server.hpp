#ifndef PERF_MULTI_RELAY_SERVER_HPP
#define PERF_MULTI_RELAY_SERVER_HPP

#include "perf_common.hpp"
#include "perf_common_multi.hpp"
#include "perf_multi_client_helpers.hpp"
#include "../../common/perf_tls_setup.hpp"
#include <atomic>
#include <cerrno>
#include <cstring>
#include <deque>
#include <iomanip>
#include <iostream>
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
        server_routing_id (NULL)
    {
    }

    const char *pattern_name;
    const char *token;
    zlink_socket_type_t socket_type;
    bool has_server_routing_id;
    const char *server_routing_id;
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

inline bool capture_pending_reply (const zlink_routing_id_t *source_rid,
                                   zlink_msg_t *parts,
                                   size_t part_count,
                                   pending_reply_t *out)
{
    if (!source_rid || !parts || part_count == 0 || !out)
        return false;

    out->rid = *source_rid;
    out->parts.resize (part_count);
    for (size_t i = 0; i < part_count; ++i) {
        if (zlink_msg_init (&out->parts[i]) != 0) {
            // Roll back any successfully initialized slots and the still-owned
            // recv parts that we have not yet moved into our queue.
            for (size_t j = 0; j < i; ++j)
                zlink_msg_close (&out->parts[j]);
            zlink_multipart_close (parts, part_count);
            out->parts.clear ();
            return false;
        }
    }
    for (size_t i = 0; i < part_count; ++i) {
        if (zlink_msg_move (&out->parts[i], &parts[i]) != 0) {
            // Close any messages we already moved into out; close untouched
            // recv parts as well.
            for (size_t j = 0; j < part_count; ++j)
                zlink_msg_close (&out->parts[j]);
            for (size_t j = i; j < part_count; ++j)
                zlink_msg_close (&parts[j]);
            out->parts.clear ();
            return false;
        }
    }
    return true;
}

inline bool try_send_reply_now (void *server,
                                const zlink_routing_id_t *source_rid,
                                zlink_msg_t *parts,
                                size_t part_count,
                                bool *would_block)
{
    if (would_block)
        *would_block = false;

    const zlink_submit_result_t send_rc =
      ::perf_zlink_send_rid_parts (server, source_rid, parts, part_count,
                                   static_cast<zlink_send_flags_t> (ZLINK_SEND_FLAGS_DONTWAIT));
    if (send_rc == ZLINK_SUBMIT_OK)
        return true;

    const int err = zlink_errno ();
    if (err == EAGAIN || err == EINTR || err == EHOSTUNREACH || err == ENOTCONN) {
        if (would_block)
            *would_block = true;
        return false;
    }

    if (bench_debug_enabled ()) {
        std::cerr << "[perf-multi-relay] reply send failed err=" << err << std::endl;
    }
    return false;
}

inline bool flush_pending_replies (void *server, std::deque<pending_reply_t> *pending)
{
    if (!pending)
        return true;
    while (!pending->empty ()) {
        pending_reply_t &front = pending->front ();
        bool would_block = false;
        const bool ok = try_send_reply_now (server, &front.rid, front.parts.data (),
                                            front.parts.size (), &would_block);
        if (ok) {
            // zlink_send_rid consumed the parts on success; clear the vector
            // without closing them again so the destructor is a no-op.
            front.parts.clear ();
            pending->pop_front ();
            continue;
        }
        if (would_block)
            return true;
        return false;
    }
    return true;
}

inline bool drain_recv_and_relay (void *server,
                                  void *ctx,
                                  zlink_socket_type_t socket_type,
                                  uint64_t hwm_value,
                                  const std::string &transport,
                                  size_t *active_msg_size,
                                  std::deque<pending_reply_t> *pending,
                                  bool *recv_drained)
{
    if (recv_drained)
        *recv_drained = false;

    while (true) {
        if (perf_stop_requested ().load (std::memory_order_acquire))
            return true;

        const zlink_routing_id_t *source_rid = NULL;
        uint64_t request_seq = 0;
        zlink_msg_t *parts = NULL;
        size_t part_count = 0;
        const zlink_recv_result_t rc = ::perf_zlink_router_recv_parts (
          server, &source_rid, &request_seq, &parts, &part_count,
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

        if (!source_rid || source_rid->size == 0 || request_seq != 0 || part_count == 0 || !parts) {
            zlink_multipart_close (parts, part_count);
            errno = EPROTO;
            return false;
        }
        if (bench_debug_enabled ()
            && g_debug_relay_logs.fetch_add (1, std::memory_order_acq_rel) < 12) {
            std::cerr << "[perf-multi-relay] echo request size="
                      << (part_count > 0 && parts ? zlink_msg_size (&parts[0]) : 0)
                      << " rid_size=" << static_cast<int> (source_rid->size)
                      << " rid=" << format_rid_debug (source_rid) << " part_count=" << part_count
                      << std::endl;
        }

        const size_t msg_size = part_count > 0 && parts ? zlink_msg_size (&parts[0]) : 0;
        if (active_msg_size && msg_size > 0 && *active_msg_size != msg_size) {
            if (!apply_benchmark_context_auto_hwm_msg_unit (ctx, msg_size)) {
                zlink_multipart_close (parts, part_count);
                return false;
            }
            apply_benchmark_hwm (server, hwm_value);
            *active_msg_size = msg_size;
            perf_print_auto_hwm_snapshot (server, false, "server", transport, true, msg_size,
                                          socket_type);
        }

        // While we still have backlog, push everything onto the queue to keep
        // ordering. Otherwise try to send immediately.
        bool would_block = false;
        if (pending && pending->empty ()
            && try_send_reply_now (server, source_rid, parts, part_count, &would_block)) {
            continue;
        }
        if (!would_block && pending && !pending->empty ()) {
            // Already pending; do not even attempt and keep ordering.
        } else if (!would_block) {
            // try_send_reply_now returned false but did not block: that means
            // a real error path (logged inside) — propagate.
            zlink_multipart_close (parts, part_count);
            return false;
        }

        if (!pending) {
            zlink_multipart_close (parts, part_count);
            errno = ENOSPC;
            return false;
        }
        pending->emplace_back ();
        if (!capture_pending_reply (source_rid, parts, part_count, &pending->back ())) {
            pending->pop_back ();
            return false;
        }
    }
}

inline bool run_server_loop (const relay_server_config_t &config,
                             void *server,
                             void *ctx,
                             uint64_t hwm_value,
                             const std::string &lib_name,
                             const std::string &transport)
{
    (void) lib_name;
    if (!server)
        return false;

    size_t active_msg_size = 0;
    std::deque<pending_reply_t> pending;

    while (!perf_stop_requested ().load (std::memory_order_acquire)) {
        zlink_pollitem_t item;
        item.socket = server;
        item.fd = 0;
        item.events = ZLINK_POLLIN;
        if (!pending.empty ())
            item.events |= ZLINK_POLLOUT;
        item.revents = 0;
        const int poll_rc = perf_socket_poll (&item, 1, -1);
        if (poll_rc < 0) {
            if (zlink_errno () == EINTR)
                continue;
            if (bench_debug_enabled ()) {
                std::cerr << "[perf-multi-relay] poll failed err=" << zlink_errno () << std::endl;
            }
            return false;
        }
        if (perf_stop_requested ().load (std::memory_order_acquire))
            break;
        if ((item.revents & ZLINK_POLLOUT) != 0) {
            if (!flush_pending_replies (server, &pending))
                return false;
        }
        if ((item.revents & ZLINK_POLLIN) != 0) {
            bool recv_drained = false;
            if (!drain_recv_and_relay (server, ctx, config.socket_type, hwm_value, transport,
                                       &active_msg_size, &pending, &recv_drained))
                return false;
        }
    }

    return true;
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
      run_server_loop (config, server, ctx.get (), settings.hwm, lib_name, transport);

    zlink_close (server);
    return loop_ok ? 0 : 1;
}

} // namespace perf_multi_relay_server

#endif
