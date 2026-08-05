#ifndef PERF_MULTI_COMMON_HPP
#define PERF_MULTI_COMMON_HPP

#include "perf_common_multi.hpp"
#include "perf_handshake.hpp"
#include "perf_metric_header.hpp"
#include "perf_tls.hpp"
#include "perf_socket_option_helpers.hpp"
#include "../../common/perf_latency_sampler.hpp"
#include "../../common/perf_monitor_wait.hpp"
#include "../../common/perf_socket_adapter.hpp"

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace perf
{
namespace multi
{

typedef ::perf::socket_t perf_socket_t;

static const char *k_stop_token = "__zlink_perf_stop__";

inline bool is_supported_transport (const std::string &transport)
{
    return transport == "tcp" || transport == "tls" || transport == "ws" || transport == "wss";
}

inline void noop_free (void *, void *)
{
}

inline zlink::message_t message_from_external_buffer (std::vector<char> &buffer, size_t size)
{
    return zlink::message_t::from (std::as_bytes (std::span<const char> (buffer.data (), size)));
}

inline std::vector<size_t> resolve_case_msg_sizes (size_t fallback_size)
{
    std::vector<size_t> sizes;
    const char *env = std::getenv ("PERF_MSG_SIZES");
    if (env && *env) {
        const char *cursor = env;
        while (*cursor) {
            char *end = NULL;
            errno = 0;
            const unsigned long parsed = std::strtoul (cursor, &end, 10);
            if (errno == 0 && end != cursor && parsed > 0)
                sizes.push_back (static_cast<size_t> (parsed));
            if (!end || *end == '\0')
                break;
            cursor = *end == ',' ? end + 1 : end;
        }
    }

    if (sizes.empty ())
        sizes.push_back (fallback_size > 0 ? fallback_size : 64);
    return sizes;
}

inline size_t max_case_msg_size (const std::vector<size_t> &sizes, size_t fallback_size)
{
    size_t max_size = fallback_size > 0 ? fallback_size : 64;
    for (size_t size : sizes) {
        if (size > max_size)
            max_size = size;
    }
    return max_size;
}

inline int bench_io_threads ()
{
    return parse_positive_env ("PERF_IO_THREADS", 0);
}

inline int bench_ctx_blocky ()
{
    const char *value = std::getenv ("PERF_CTX_BLOCKY");
    if (!value || !*value)
        return 0;

    errno = 0;
    char *end = NULL;
    const long parsed = std::strtol (value, &end, 10);
    if (errno != 0 || end == value)
        return 0;
    return parsed != 0 ? 1 : 0;
}

inline zlink::auto_hwm_profile bench_ctx_auto_hwm_profile ()
{
    const char *value = std::getenv ("PERF_CTX_AUTO_HWM_PROFILE");
    if (!value || !*value)
        value = std::getenv ("PERF_AUTO_HWM_PROFILE");
    if (!value || !*value)
        return zlink::auto_hwm_profile::balanced;

    if (std::strcmp (value, "compact") == 0)
        return zlink::auto_hwm_profile::compact;
    if (std::strcmp (value, "low_latency") == 0 || std::strcmp (value, "low-latency") == 0)
        return zlink::auto_hwm_profile::low_latency;
    if (std::strcmp (value, "throughput") == 0)
        return zlink::auto_hwm_profile::throughput;
    return zlink::auto_hwm_profile::balanced;
}

inline int bench_max_sockets ()
{
    const int explicit_max = parse_positive_env ("PERF_MAX_SOCKETS", 0);
    if (explicit_max > 0)
        return explicit_max;

    const multi_bench_settings_t settings = resolve_multi_bench_settings ();
    if (settings.clients == 0)
        return 0;

    const char *pattern_env = std::getenv ("PERF_PATTERN");
    const std::string pattern = pattern_env ? pattern_env : "";

    long required = 0;
    if (pattern == "SPOT" || pattern == "MULTI_SPOT") {
        required = static_cast<long> (settings.clients) * 16L + 64L;
    } else {
        required = static_cast<long> (settings.clients) * 3L + 4096L;
    }

    if (required > INT_MAX)
        return INT_MAX;
    return static_cast<int> (required);
}

class ctx_guard_t
{
  public:
    ctx_guard_t () : _ctx ()
    {
        zlink::context_options_t options = _ctx.options ();
        const int io_threads = bench_io_threads ();
        if (io_threads > 0)
            (void) options.io_threads (zlink::io_thread_count_t::value (io_threads));

        const int max_sockets = bench_max_sockets ();
        if (max_sockets > 0)
            (void) options.max_sockets (zlink::socket_count_t::value (max_sockets));

        (void) options.blocky (bench_ctx_blocky () != 0);
        (void) options.auto_hwm_enabled (true);
        (void) options.auto_hwm_profile (bench_ctx_auto_hwm_profile ());
    }

    ~ctx_guard_t ()
    {
        if (_ctx.valid ())
            (void) _ctx.shutdown ();
    }

    zlink::context_t &ctx () { return _ctx; }
    operator zlink::context_t & () { return _ctx; }

  private:
    zlink::context_t _ctx;
};

using ::perf::socket_guard_t;

struct connect_monitor_t
{
    connect_monitor_t () : monitor () {}

    std::unique_ptr<zlink::socket_monitor_t> monitor;
};

// Migrated to unified perf::latency_sampler_stats_t / perf::latency_sampler_t.
// These typedefs preserve the old multi-bench names at call sites.
typedef ::perf::latency_sampler_stats_t bench_latency_stats_t;
typedef ::perf::latency_sampler_t bench_latency_sampler_t;

template <typename SocketLike>
inline void apply_benchmark_hwm (SocketLike &socket, uint64_t sndhwm, uint64_t rcvhwm)
{
    if (!manual_socket_overrides_enabled ())
        return;
    const uint64_t snd_value = sndhwm > 0 ? sndhwm : 1;
    const uint64_t rcv_value = rcvhwm > 0 ? rcvhwm : 1;
    (void) set_common_socket_option (socket, perf::options::socket_options::sndhwm, snd_value);
    (void) set_common_socket_option (socket, perf::options::socket_options::rcvhwm, rcv_value);
}

template <typename SocketLike>
inline void apply_debug_timeouts (SocketLike &socket, const std::string &)
{
    const multi_bench_settings_t settings = resolve_multi_bench_settings ();
    (void) set_common_socket_option (socket, perf::options::socket_options::sndtimeo,
                                     settings.sndtimeo_ms);
    (void) set_common_socket_option (socket, perf::options::socket_options::rcvtimeo,
                                     settings.rcvtimeo_ms);
    (void) set_common_socket_option (socket, perf::options::socket_options::linger, 0);
}

inline bool apply_benchmark_auto_hwm_msg_unit (ctx_guard_t &ctx, size_t msg_size)
{
    if (msg_size == 0)
        return true;
    try {
        zlink::context_options_t options = ctx.ctx ().options ();
        options.auto_hwm_msg_unit_bytes (
          zlink::byte_count_t::bytes (static_cast<uint64_t> (msg_size)));
        return true;
    }
    catch (const zlink::config_error_t &err) {
        errno = err.internal_errno ();
        return false;
    }
}

inline bool recalculate_auto_hwm (ctx_guard_t &ctx)
{
    try {
        ctx.ctx ().recalculate_auto_hwm ();
        return true;
    }
    catch (const zlink::config_error_t &err) {
        errno = err.internal_errno ();
        return false;
    }
}

inline bool auto_hwm_detail_enabled ()
{
    const char *value = std::getenv ("PERF_MULTI_PRINT_AUTO_HWM_DETAIL");
    if (!value || !*value)
        value = std::getenv ("PERF_PRINT_AUTO_HWM_DETAIL");
    if (!value || !*value)
        return true;
    return std::strcmp (value, "0") != 0;
}

inline const char *auto_hwm_role_name (uint32_t role)
{
    switch (role) {
        case 1:
            return "control";
        case 2:
            return "routed";
        case 3:
            return "fanout";
        case 4:
            return "recv_ingress";
        case 5:
            return "spot_data";
        case 6:
            return "peer_queue";
        case 7:
            return "stream";
        default:
            return "none";
    }
}

inline const char *auto_hwm_profile_name (uint32_t profile)
{
    switch (profile) {
        case static_cast<uint32_t> (zlink::auto_hwm_profile::compact):
            return "compact";
        case static_cast<uint32_t> (zlink::auto_hwm_profile::low_latency):
            return "low_latency";
        case static_cast<uint32_t> (zlink::auto_hwm_profile::balanced):
            return "balanced";
        case static_cast<uint32_t> (zlink::auto_hwm_profile::throughput):
            return "throughput";
        default:
            return "unknown";
    }
}

inline const char *auto_hwm_policy_class_name (uint32_t policy_class)
{
    switch (policy_class) {
        case 1:
            return "fanout";
        case 2:
            return "spot_data";
        case 3:
            return "recv_ingress";
        case 4:
            return "routed";
        case 5:
            return "peer_queue";
        case 6:
            return "stream";
        case 7:
            return "control";
        default:
            return "none";
    }
}

inline const char *auto_hwm_recalc_reason_name (uint32_t reason)
{
    switch (reason) {
        case 1:
            return "initial";
        case 2:
            return "role_change";
        case 3:
            return "policy_toggle";
        case 4:
            return "refresh";
        case 5:
            return "deferred_shrink";
        default:
            return "unknown";
    }
}

inline std::string auto_hwm_env_or_default (const char *name, const char *default_value)
{
    const char *value = std::getenv (name);
    if (value && *value)
        return value;
    return default_value ? default_value : "";
}

template <typename SocketLike>
inline void emit_auto_hwm_detail (SocketLike &socket,
                                  const std::string &component,
                                  const std::string &label,
                                  const std::string &transport,
                                  size_t msg_size,
                                  const char *socket_type)
{
    if (!auto_hwm_detail_enabled ())
        return;

    zlink::monitor_status_t snapshot;
    try {
        zlink::socket_monitor_t monitor = socket.monitor_open (zlink::monitor_event::all);
        if (!monitor.valid ())
            return;
        snapshot = monitor.status ();
    }
    catch (const zlink::binding_error_t &) {
        return;
    }

    const std::string pattern = auto_hwm_env_or_default ("PERF_MULTI_PATTERN", "unknown");
    const std::string transport_value =
      transport.empty () ? auto_hwm_env_or_default ("PERF_MULTI_TRANSPORT", "unknown") : transport;
    const std::string socket_type_value = socket_type && *socket_type ? socket_type : "unknown";

    // Match the C multi reference (perf_multi_runtime.hpp
    // perf_auto_hwm_sndbuf_display / perf_auto_hwm_rcvbuf_display, also
    // applied in the spotnode path below): a SUB/XSUB carrying
    // recv_ingress(4)|control(1) never sends and a PUB/XPUB carrying
    // spot_data(5)|control(1) never receives, so the C benchmark reports
    // effective_sndbuf=0 / effective_rcvbuf=0 for the inactive direction
    // (sndhwm/rcvhwm stay raw).
    int32_t effective_sndbuf = snapshot.auto_hwm_effective_sndbuf;
    int32_t effective_rcvbuf = snapshot.auto_hwm_effective_rcvbuf;
    {
        const uint32_t role = snapshot.auto_hwm_role;
        if ((socket_type_value == "pub" || socket_type_value == "xpub") && (role == 1 || role == 5))
            effective_rcvbuf = 0;
        if ((socket_type_value == "sub" || socket_type_value == "xsub") && (role == 1 || role == 4))
            effective_sndbuf = 0;
    }

    const std::string key = pattern + "|" + transport_value + "|" + component + "|" + label + "|"
                            + std::to_string (msg_size) + "|" + socket_type_value + "|"
                            + std::to_string (snapshot.auto_hwm_role) + "|"
                            + std::to_string (snapshot.auto_hwm_applied_sndhwm_bytes) + "|"
                            + std::to_string (snapshot.auto_hwm_applied_rcvhwm_bytes) + "|"
                            + std::to_string (snapshot.auto_hwm_unit_budget_bytes) + "|"
                            + std::to_string (snapshot.auto_hwm_effective_message_bytes) + "|"
                            + std::to_string (effective_sndbuf) + "|"
                            + std::to_string (effective_rcvbuf);

    static std::mutex mutex;
    static std::set<std::string> emitted;
    {
        std::lock_guard<std::mutex> lock (mutex);
        if (emitted.find (key) != emitted.end ())
            return;
        emitted.insert (key);
    }

    std::cout << "AUTO_HWM_DETAIL" << ",pattern=" << pattern << ",transport=" << transport_value
              << ",component=" << component << ",label=" << label
              << ",socket_type=" << socket_type_value << ",msg_size=" << msg_size
              << ",source=monitor_snapshot" << ",enabled=" << (snapshot.auto_hwm_enabled ? 1 : 0)
              << ",role=" << auto_hwm_role_name (snapshot.auto_hwm_role)
              << ",role_id=" << snapshot.auto_hwm_role
              << ",profile=" << auto_hwm_profile_name (snapshot.auto_hwm_profile)
              << ",profile_id=" << snapshot.auto_hwm_profile
              << ",policy_class=" << auto_hwm_policy_class_name (snapshot.auto_hwm_policy_class)
              << ",policy_class_id=" << snapshot.auto_hwm_policy_class
              << ",unit_budget_bytes=" << snapshot.auto_hwm_unit_budget_bytes
              << ",size_cap=" << snapshot.auto_hwm_size_cap
              << ",sndhwm=" << snapshot.auto_hwm_applied_sndhwm_bytes
              << ",rcvhwm=" << snapshot.auto_hwm_applied_rcvhwm_bytes
              << ",socket_message_slots=" << snapshot.auto_hwm_socket_message_slots
              << ",effective_message_bytes=" << snapshot.auto_hwm_effective_message_bytes
              << ",effective_sndbuf=" << effective_sndbuf
              << ",effective_rcvbuf=" << effective_rcvbuf
              << ",last_recalc_ms=" << snapshot.auto_hwm_last_recalc_ms << ",last_recalc_reason="
              << auto_hwm_recalc_reason_name (snapshot.auto_hwm_last_recalc_reason)
              << ",send_blocked_ratio_ppm=" << snapshot.auto_hwm_send_blocked_ratio_ppm
              << ",deferred_sndhwm="
              << (snapshot.auto_hwm_deferred_sndhwm_valid
                    ? std::to_string (snapshot.auto_hwm_deferred_sndhwm_bytes)
                    : "-")
              << ",deferred_rcvhwm="
              << (snapshot.auto_hwm_deferred_rcvhwm_valid
                    ? std::to_string (snapshot.auto_hwm_deferred_rcvhwm_bytes)
                    : "-")
              << std::endl;
}

template <typename SocketLike>
inline void apply_benchmark_socket_options (SocketLike &socket,
                                            const multi_bench_settings_t &settings,
                                            const std::string &transport)
{
    // Manual HWM is a debug-only override; the default benchmark surface uses
    // context auto-HWM.
    apply_benchmark_hwm (socket, settings.sndhwm, settings.rcvhwm);
    apply_debug_timeouts (socket, transport);
}

template <typename SocketLike>
inline bool
open_socket_monitor (SocketLike &socket, zlink::monitor_event events, connect_monitor_t &out)
{
    zlink::socket_monitor_t monitor (socket.monitor_open (events));
    if (!monitor.valid ())
        return false;

    out.monitor.reset (new zlink::socket_monitor_t (std::move (monitor)));
    return true;
}

template <typename SocketLike>
inline bool open_connect_monitor (SocketLike &socket, connect_monitor_t &out)
{
    return open_socket_monitor (socket, zlink::monitor_event::connection_ready, out);
}

// Migrated to unified perf::wait_socket_monitor_event in
// common/perf_monitor_wait.hpp (socket_monitor_t overload).
using ::perf::wait_socket_monitor_event;

inline int poll_connect_ready_count (connect_monitor_t &mon)
{
    if (!mon.monitor.get ())
        return 0;

    int ready = 0;
    for (;;) {
        const std::optional<zlink::monitor_event_t> ev =
          mon.monitor->recv (static_cast<int> (zlink::send_flags_t::dontwait));
        if (!ev)
            break;
        if (static_cast<uint64_t> (ev->event)
            == static_cast<uint64_t> (zlink::monitor_event::connection_ready)) {
            ++ready;
        }
    }

    return ready;
}

inline bool wait_connect_ready_count (connect_monitor_t &mon, size_t expected_ready, int timeout_ms)
{
    if (expected_ready == 0)
        return true;
    if (!mon.monitor.get ())
        return false;

    size_t ready = static_cast<size_t> (poll_connect_ready_count (mon));
    if (ready >= expected_ready)
        return true;
    if (timeout_ms <= 0)
        return false;

    zlink::poller_t poller;
    zlink::poll_event_t event;
    try {
        poller.add (*mon.monitor, zlink::poll_event_flag_t::pollin, 0);
    }
    catch (const zlink::binding_error_t &) {
        return false;
    }

    const auto deadline =
      std::chrono::steady_clock::now () + std::chrono::milliseconds (timeout_ms);
    while (ready < expected_ready) {
        const auto now = std::chrono::steady_clock::now ();
        if (now >= deadline)
            break;

        long wait_ms =
          std::chrono::duration_cast<std::chrono::milliseconds> (deadline - now).count ();
        if (wait_ms < 1)
            wait_ms = 1;

        const size_t rc = poller.wait (&event, 1, std::chrono::milliseconds (wait_ms));
        if (rc == 0)
            continue;

        ready += static_cast<size_t> (poll_connect_ready_count (mon));
    }

    return ready >= expected_ready;
}

inline bool wait_connect_ready (connect_monitor_t &mon, int timeout_ms)
{
    return wait_connect_ready_count (mon, 1, timeout_ms);
}

inline bool wait_connect_ready_all (std::vector<connect_monitor_t> &monitors, int timeout_ms)
{
    if (monitors.empty ())
        return true;

    std::vector<char> ready (monitors.size (), 0);
    std::vector<size_t> active_indices;
    active_indices.reserve (monitors.size ());

    size_t ready_count = 0;
    for (size_t i = 0; i < monitors.size (); ++i) {
        if (!monitors[i].monitor.get ()) {
            ready[i] = 1;
            ++ready_count;
            continue;
        }
        active_indices.push_back (i);
    }

    if (ready_count >= monitors.size ())
        return true;
    if (timeout_ms <= 0)
        return false;

    zlink::poller_t poller;
    std::vector<zlink::poll_event_t> events (active_indices.size ());
    try {
        for (size_t i = 0; i < active_indices.size (); ++i) {
            poller.add (*monitors[active_indices[i]].monitor, zlink::poll_event_flag_t::pollin, i);
        }
    }
    catch (const zlink::binding_error_t &) {
        return false;
    }

    const auto deadline =
      std::chrono::steady_clock::now () + std::chrono::milliseconds (timeout_ms);
    while (ready_count < monitors.size ()) {
        const auto now = std::chrono::steady_clock::now ();
        if (now >= deadline)
            break;

        long wait_ms =
          std::chrono::duration_cast<std::chrono::milliseconds> (deadline - now).count ();
        if (wait_ms < 1)
            wait_ms = 1;

        const size_t rc =
          poller.wait (events.data (), events.size (), std::chrono::milliseconds (wait_ms));
        if (rc == 0)
            continue;

        for (size_t i = 0; i < rc; ++i) {
            const size_t active_index = events[i].slot;
            if (active_index >= active_indices.size ())
                continue;
            const size_t monitor_index = active_indices[active_index];
            if (ready[monitor_index])
                continue;
            const int count = poll_connect_ready_count (monitors[monitor_index]);
            if (count <= 0)
                continue;
            ready[monitor_index] = 1;
            ++ready_count;
        }
    }

    return ready_count >= monitors.size ();
}

inline void close_connect_monitor (connect_monitor_t &mon)
{
    if (mon.monitor.get ())
        (void) mon.monitor->close ();
    mon.monitor.reset ();
}

inline std::string
make_endpoint (const std::string &transport, const std::string &id, int fixed_port)
{
    if (transport == "inproc")
        return std::string ("inproc://") + id;
    if (transport == "ipc")
        return "ipc://*";

    const std::string host = "127.0.0.1";
    if (fixed_port > 0) {
        if (transport == "ws")
            return "ws://" + host + ":" + std::to_string (fixed_port);
        if (transport == "wss")
            return "wss://" + host + ":" + std::to_string (fixed_port);
        if (transport == "tls")
            return "tls://" + host + ":" + std::to_string (fixed_port);
        return "tcp://" + host + ":" + std::to_string (fixed_port);
    }

    if (transport == "ws")
        return "ws://" + host + ":*";
    if (transport == "wss")
        return "wss://" + host + ":*";
    if (transport == "tls")
        return "tls://" + host + ":*";
    return "tcp://" + host + ":*";
}

inline std::string normalize_endpoint_host (const std::string &endpoint)
{
    std::string out = endpoint;
    const std::string any_v4 = "://0.0.0.0:";
    const std::string any_v6 = "://[::]:";
    size_t pos = out.find (any_v4);
    if (pos != std::string::npos)
        out.replace (pos, any_v4.size (), "://127.0.0.1:");
    pos = out.find (any_v6);
    if (pos != std::string::npos)
        out.replace (pos, any_v6.size (), "://127.0.0.1:");
    return out;
}

template <typename SocketLike>
inline std::string bind_and_resolve_endpoint (SocketLike &socket,
                                              const std::string &transport,
                                              const std::string &id,
                                              int fixed_port)
{
    // For wildcard binds, normalize to loopback so clients always connect to 127.0.0.1.
    const std::string endpoint = make_endpoint (transport, id, fixed_port);
    try {
        socket.bind (endpoint);
    }
    catch (const zlink::binding_error_t &) {
        return std::string ();
    }

    if (transport == "inproc")
        return endpoint;

    std::string last;
    if (get_common_socket_option (socket, perf::options::socket_options::last_endpoint, last) != 0)
        return std::string ();

    return normalize_endpoint_host (last);
}

inline bool debug_header_trace_enabled ()
{
    return std::getenv ("PERF_DEBUG_HEADER_TRACE") != NULL;
}

inline int debug_header_trace_limit ()
{
    const char *env = std::getenv ("PERF_DEBUG_HEADER_TRACE_LIMIT");
    if (!env || !*env)
        return 8;

    char *end = NULL;
    const long parsed = std::strtol (env, &end, 10);
    if (end == env || *end != '\0' || parsed <= 0)
        return 8;
    if (parsed > INT_MAX)
        return INT_MAX;
    return static_cast<int> (parsed);
}

inline void debug_header_trace (const char *role,
                                const std::string &pattern,
                                const std::string &transport,
                                size_t size,
                                const char *stage,
                                uint32_t phase,
                                uint32_t run_id,
                                uint64_t seq,
                                uint64_t sent_ts_ns,
                                uint64_t observed_ts_ns,
                                double latency_ns)
{
    if (!debug_header_trace_enabled ())
        return;

    static int remaining = debug_header_trace_limit ();
    if (remaining <= 0)
        return;
    --remaining;

    std::cerr << "HEADER_TRACE" << ",role=" << (role ? role : "") << ",pattern=" << pattern
              << ",transport=" << transport << ",size=" << size << ",stage=" << (stage ? stage : "")
              << ",phase=" << phase << ",run_id=" << run_id << ",seq=" << seq
              << ",sent_ts_ns=" << sent_ts_ns << ",observed_ts_ns=" << observed_ts_ns;
    if (latency_ns >= 0.0) {
        std::cerr << ",latency_ns=" << std::fixed << std::setprecision (3) << latency_ns;
    }
    std::cerr << std::endl;
}

inline bool is_stop_token (const void *data, size_t size)
{
    const size_t token_size = std::strlen (k_stop_token);
    if (size == token_size && std::memcmp (data, k_stop_token, token_size) == 0) {
        return true;
    }

    return false;
}

inline bool is_stop_token_message (const zlink::message_t &msg)
{
    return is_stop_token (msg.data (), msg.size ());
}

inline void print_result (const std::string &lib,
                          const std::string &pattern,
                          const std::string &transport,
                          size_t size,
                          double throughput,
                          double bandwidth,
                          double latency_ns,
                          double p95_ns,
                          double p99_ns)
{
    const double latency_ms = latency_ns / 1000000.0;
    const double p95_ms = p95_ns / 1000000.0;
    const double p99_ms = p99_ns / 1000000.0;

    std::cout << "RESULT," << lib << "," << pattern << "," << transport << "," << size
              << ",throughput," << std::fixed << std::setprecision (3) << throughput << std::endl;
    std::cout << "RESULT," << lib << "," << pattern << "," << transport << "," << size
              << ",bandwidth," << std::fixed << std::setprecision (3) << bandwidth << std::endl;
    std::cout << "RESULT," << lib << "," << pattern << "," << transport << "," << size
              << ",latency," << std::fixed << std::setprecision (3) << latency_ms << std::endl;
    std::cout << "RESULT," << lib << "," << pattern << "," << transport << "," << size
              << ",latency_p95," << std::fixed << std::setprecision (3) << p95_ms << std::endl;
    std::cout << "RESULT," << lib << "," << pattern << "," << transport << "," << size
              << ",latency_p99," << std::fixed << std::setprecision (3) << p99_ms << std::endl;
}

inline void print_client_result_lines (const std::string &lib,
                                       const std::string &pattern,
                                       const std::string &transport,
                                       size_t size,
                                       unsigned long long active_count,
                                       int active_seconds,
                                       double bandwidth_multiplier,
                                       const bench_latency_stats_t &latency)
{
    const double throughput =
      static_cast<double> (active_count) / static_cast<double> (std::max (1, active_seconds));
    const double bandwidth =
      throughput * static_cast<double> (size) * bandwidth_multiplier / 1000000.0;

    print_result (lib, pattern, transport, size, throughput, bandwidth, latency.mean_ns,
                  latency.p95_ns, latency.p99_ns);
}

inline void print_ready (const std::string &endpoint)
{
    std::cout << "READY," << endpoint << std::endl;
    std::cout.flush ();
}

} // namespace multi
} // namespace perf

#endif
