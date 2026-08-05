#ifndef PERF_SINGLE_COMMON_HPP
#define PERF_SINGLE_COMMON_HPP

#include "perf_single_metric_header.hpp"
#include "../../common/perf_latency_sampler.hpp"
#include "../../common/perf_monitor_wait.hpp"
#include "../../common/perf_socket_adapter.hpp"
#include "../../common/perf_tls.hpp"

#include <chrono>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace perf
{
namespace single
{

typedef ::perf::socket_t perf_socket_t;

typedef ::perf::latency_sampler_stats_t latency_stats_t;

typedef ::perf::latency_sampler_t latency_stats_builder_t;

inline zlink::message_t message_from_payload (const void *data_, size_t size_)
{
    return zlink::message_t::from (
      std::as_bytes (std::span<const char> (static_cast<const char *> (data_), size_)));
}

class ctx_guard_t
{
  public:
    ctx_guard_t ();
    ~ctx_guard_t ();

    zlink::context_t &ctx () { return _ctx; }
    operator zlink::context_t & () { return _ctx; }
    bool valid () const { return _ctx.valid (); }

  private:
    zlink::context_t _ctx;
};

using ::perf::socket_guard_t;

// Reads positive integer env var; returns default when missing/invalid/non-positive.
int parse_positive_env (const char *name_, int default_value_);
uint64_t parse_positive_uint64_env (const char *name_, uint64_t default_value_);
int resolve_single_duration_seconds ();
size_t resolve_single_latency_sample_cap ();
int resolve_single_send_timeout_ms ();
int resolve_single_recv_timeout_ms ();
int resolve_single_pubsub_recv_timeout_ms ();
int resolve_single_pubsub_ready_settle_ms ();
int resolve_single_connect_ready_timeout_ms ();
uint64_t resolve_single_socket_hwm (bool send_);
zlink::auto_hwm_profile resolve_single_ctx_auto_hwm_profile ();
bool single_manual_socket_overrides_enabled ();

bool bench_debug_enabled ();

// Applies shared benchmark context options (io_threads/max_sockets).
void apply_ctx_options (zlink::context_t &ctx_);
bool set_sockopt_int (perf_socket_t &socket_,
                      perf::options::socket_option_key_t<int> option_,
                      int value_,
                      const char *name_);
bool set_sockopt_hwm (perf_socket_t &socket_,
                      perf::options::socket_option_key_t<uint64_t> option_,
                      uint64_t value_,
                      const char *name_);
template <typename SocketLike>
bool set_sockopt_int (SocketLike &socket_,
                      perf::options::socket_option_key_t<int> option_,
                      int value_,
                      const char *name_)
{
    try {
        zlink::common_socket_options_t options = socket_.options ();
        switch (option_.option) {
            case perf::options::socket_option::linger:
                options.linger (std::chrono::milliseconds (value_));
                return true;
            case perf::options::socket_option::sndhwm:
                options.send_hwm (zlink::byte_count_t::bytes (static_cast<uint64_t> (value_)));
                return true;
            case perf::options::socket_option::rcvhwm:
                options.recv_hwm (zlink::byte_count_t::bytes (static_cast<uint64_t> (value_)));
                return true;
            case perf::options::socket_option::sndtimeo:
                options.send_timeout (std::chrono::milliseconds (value_));
                return true;
            case perf::options::socket_option::rcvtimeo:
                options.recv_timeout (std::chrono::milliseconds (value_));
                return true;
            case perf::options::socket_option::tcp_nodelay:
                options.tcp_no_delay (value_ != 0);
                return true;
            default:
                errno = EOPNOTSUPP;
                if (bench_debug_enabled ()) {
                    std::cerr << "setsockopt(" << (name_ ? name_ : "?")
                              << ") failed: unsupported public option" << std::endl;
                }
                return false;
        }
    }
    catch (const zlink::binding_error_t &err) {
        errno = err.internal_errno ();
        if (bench_debug_enabled ()) {
            std::cerr << "setsockopt(" << (name_ ? name_ : "?") << ") failed: " << err.what ()
                      << std::endl;
        }
        return false;
    }
}
template <typename SocketLike>
bool set_sockopt_hwm (SocketLike &socket_,
                      perf::options::socket_option_key_t<uint64_t> option_,
                      uint64_t value_,
                      const char *name_)
{
    try {
        zlink::common_socket_options_t options = socket_.options ();
        switch (option_.option) {
            case perf::options::socket_option::sndhwm:
                options.send_hwm (zlink::byte_count_t::bytes (value_));
                return true;
            case perf::options::socket_option::rcvhwm:
                options.recv_hwm (zlink::byte_count_t::bytes (value_));
                return true;
            default:
                errno = EOPNOTSUPP;
                if (bench_debug_enabled ()) {
                    std::cerr << "setsockopt(" << (name_ ? name_ : "?")
                              << ") failed: unsupported byte option" << std::endl;
                }
                return false;
        }
    }
    catch (const zlink::config_error_t &err) {
        errno = err.internal_errno ();
        if (bench_debug_enabled ()) {
            std::cerr << "setsockopt(" << (name_ ? name_ : "?") << ") failed: " << err.what ()
                      << std::endl;
        }
        return false;
    }
}
void apply_single_hwm (perf_socket_t &socket_);
bool apply_single_auto_hwm_msg_unit (ctx_guard_t &ctx_, size_t msg_size_);
bool recalculate_single_auto_hwm (ctx_guard_t &ctx_);

template <typename SocketLike> void apply_single_hwm (SocketLike &socket_)
{
    if (!single_manual_socket_overrides_enabled ())
        return;
    const uint64_t sndhwm = resolve_single_socket_hwm (true);
    const uint64_t rcvhwm = resolve_single_socket_hwm (false);
    (void) set_sockopt_hwm (socket_, perf::options::socket_options::sndhwm, sndhwm, "sndhwm");
    (void) set_sockopt_hwm (socket_, perf::options::socket_options::rcvhwm, rcvhwm, "rcvhwm");
}
// Applies linger/send/recv timeout defaults for benchmark sockets.
void apply_single_benchmark_socket_options (perf_socket_t &socket_, const std::string &transport_);
template <typename SocketLike>
void apply_single_benchmark_socket_options (SocketLike &socket_, const std::string &)
{
    const int linger_ms = 0;
    const int sndtimeo_ms = resolve_single_send_timeout_ms ();
    const int rcvtimeo_ms = resolve_single_recv_timeout_ms ();
    (void) set_sockopt_int (socket_, perf::options::socket_options::linger, linger_ms, "linger");
    (void) set_sockopt_int (socket_, perf::options::socket_options::sndtimeo, sndtimeo_ms,
                            "sndtimeo");
    (void) set_sockopt_int (socket_, perf::options::socket_options::rcvtimeo, rcvtimeo_ms,
                            "rcvtimeo");
}

// Creates wildcard endpoint string for a transport/id pair.
std::string make_endpoint (const std::string &transport, const std::string &id);
std::string make_fixed_endpoint (const std::string &transport, int port);
// Binds socket and returns normalized concrete endpoint (127.0.0.1 host form).
std::string bind_and_resolve_endpoint (perf_socket_t &socket_,
                                       const std::string &transport,
                                       const std::string &id);
template <typename SocketLike>
std::string
bind_and_resolve_endpoint (SocketLike &socket_, const std::string &transport, const std::string &id)
{
    std::string endpoint = make_endpoint (transport, id);
    if (endpoint.empty ())
        return std::string ();
    try {
        socket_.bind (endpoint);
    }
    catch (const zlink::binding_error_t &) {
        return std::string ();
    }

    if (transport != "inproc") {
        endpoint = socket_.options ().last_endpoint ();

        const std::string any_v4 = "://0.0.0.0:";
        const std::string any_v6 = "://[::]:";
        size_t pos = endpoint.find (any_v4);
        if (pos != std::string::npos) {
            endpoint.replace (pos, any_v4.size (), "://127.0.0.1:");
        } else {
            pos = endpoint.find (any_v6);
            if (pos != std::string::npos)
                endpoint.replace (pos, any_v6.size (), "://127.0.0.1:");
        }
    }

    return endpoint;
}

bool transport_available (const std::string &transport);
// Binds first socket and connects second socket to resolved endpoint.
bool setup_connected_pair (perf_socket_t &bind_socket_,
                           perf_socket_t &connect_socket_,
                           const std::string &transport_,
                           const std::string &id_);
template <typename BindSocketLike, typename ConnectSocketLike>
bool setup_connected_pair (BindSocketLike &bind_socket_,
                           ConnectSocketLike &connect_socket_,
                           const std::string &transport_,
                           const std::string &id_)
{
    if (!setup_tls_server (bind_socket_, transport_)
        || !setup_tls_client (connect_socket_, transport_)) {
        return false;
    }

    apply_single_hwm (bind_socket_);
    apply_single_hwm (connect_socket_);

    zlink::socket_monitor_t bind_monitor =
      zlink::socket_monitor_t::open (bind_socket_, zlink::monitor_event::connection_ready);
    zlink::socket_monitor_t connect_monitor =
      zlink::socket_monitor_t::open (connect_socket_, zlink::monitor_event::connection_ready);
    if (!bind_monitor.valid () || !connect_monitor.valid ())
        return false;

    const std::string endpoint = bind_and_resolve_endpoint (bind_socket_, transport_, id_);
    if (endpoint.empty ())
        return false;
    try {
        connect_socket_.connect (endpoint);
    }
    catch (const zlink::binding_error_t &) {
        return false;
    }

    apply_single_benchmark_socket_options (bind_socket_, transport_);
    apply_single_benchmark_socket_options (connect_socket_, transport_);
    const int connect_ready_timeout_ms = resolve_single_connect_ready_timeout_ms ();
    if (!wait_socket_monitor_event (bind_monitor,
                                    static_cast<uint64_t> (zlink::monitor_event::connection_ready),
                                    connect_ready_timeout_ms)
        || !wait_socket_monitor_event (
          connect_monitor, static_cast<uint64_t> (zlink::monitor_event::connection_ready),
          connect_ready_timeout_ms)) {
        return false;
    }
    return true;
}
// Migrated to unified perf::wait_socket_monitor_event in
// common/perf_monitor_wait.hpp.
using ::perf::wait_socket_monitor_event;

void print_result (const std::string &lib_type,
                   const std::string &pattern,
                   const std::string &transport,
                   size_t size,
                   double throughput,
                   double latency,
                   double latency_p95,
                   double latency_p99);

typedef bool (*phase_send_fn_t) (void *userdata_, const void *data_, size_t size_);

// Wire-level stop token used by sender threads to signal phase end to a
// receiver waiting on a poller. PERF_SINGLE_TEST_POLICY § 1.4 mandates
// this pattern instead of `std::atomic<bool> sender_done` + short polling.
static const char *const k_stop_token = "__zlink_perf_stop__";

inline bool is_stop_token (const void *data_, size_t size_)
{
    const size_t token_size = std::strlen (k_stop_token);
    return data_ != NULL && size_ == token_size
           && std::memcmp (data_, k_stop_token, token_size) == 0;
}

inline bool is_stop_token_message (const zlink::message_t &msg_)
{
    return is_stop_token (msg_.data (), msg_.size ());
}

// C-faithful AUTO_HWM_DETAIL emitter. Mirrors
// bindings/c/perf/single/common/bench_common_runtime.hpp
// emit_single_socket_hwm_detail byte-for-byte so the runner's
// "## Auto-HWM Detail" block is identical to the C reference.
inline const char *single_auto_hwm_role_name (uint32_t role_)
{
    switch (role_) {
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

inline bool single_auto_hwm_monitor_status_visible (const zlink::monitor_status_t &monitor_status_)
{
    return monitor_status_.auto_hwm_applied_sndhwm_bytes > 0
           || monitor_status_.auto_hwm_applied_rcvhwm_bytes > 0
           || monitor_status_.auto_hwm_effective_message_bytes > 0
           || monitor_status_.auto_hwm_socket_message_slots > 0;
}

template <typename SocketLike>
inline void emit_single_socket_hwm_detail (const SocketLike &socket_,
                                           const char *pattern_,
                                           const std::string &transport_,
                                           const char *component_,
                                           const char *socket_type_,
                                           size_t msg_size_)
{
    if (!pattern_ || !component_ || !socket_type_)
        return;

    zlink::monitor_status_t snapshot;
    try {
        zlink::socket_monitor_t monitor =
          socket_.monitor_open (zlink::monitor_event::connection_ready);
        if (!monitor.valid ())
            return;
        snapshot = monitor.status ();
    }
    catch (const zlink::binding_error_t &) {
        return;
    }
    if (!single_auto_hwm_monitor_status_visible (snapshot))
        return;

    std::cout << "AUTO_HWM_DETAIL" << ",pattern=" << pattern_ << ",transport=" << transport_
              << ",component=" << component_ << ",msg_size=" << msg_size_ << ",owner=socket"
              << ",owner_id=0" << ",socket=" << component_ << ",socket_type=" << socket_type_
              << ",role=" << single_auto_hwm_role_name (snapshot.auto_hwm_role)
              << ",sndhwm=" << snapshot.auto_hwm_applied_sndhwm_bytes
              << ",rcvhwm=" << snapshot.auto_hwm_applied_rcvhwm_bytes
              << ",effective_message_bytes=" << snapshot.auto_hwm_effective_message_bytes
              << ",effective_sndbuf=" << snapshot.auto_hwm_effective_sndbuf
              << ",effective_rcvbuf=" << snapshot.auto_hwm_effective_rcvbuf
              << ",socket_message_slots=" << snapshot.auto_hwm_socket_message_slots << std::endl;
}

inline bool send_payload_blocking (perf_socket_t &socket_, const void *data_, size_t size_)
{
    zlink::message_t msg = message_from_payload (data_, size_);
    if (!msg.valid ())
        return false;
    try {
        return ::perf::send_socket (socket_, msg, 0) == 0;
    }
    catch (const zlink::binding_error_t &err) {
        errno = err.internal_errno ();
        return false;
    }
}

inline bool send_payload_blocking (perf_socket_t &socket_,
                                   const zlink::routing_id_t &routing_id_,
                                   const void *data_,
                                   size_t size_)
{
    zlink::message_t msg = message_from_payload (data_, size_);
    if (!msg.valid ())
        return false;
    try {
        return ::perf::send_socket (socket_, routing_id_, msg, 0) == 0;
    }
    catch (const zlink::binding_error_t &err) {
        errno = err.internal_errno ();
        return false;
    }
}

inline bool is_transient_send_errno (int err_)
{
    return err_ == EINTR || err_ == EAGAIN || err_ == EWOULDBLOCK || err_ == ETIMEDOUT;
}

inline bool is_transient_routed_send_errno (int err_)
{
    return is_transient_send_errno (err_) || err_ == ENOTCONN || err_ == EHOSTUNREACH
           || err_ == ENETUNREACH;
}

inline bool send_payload_blocking_retry (perf_socket_t &socket_,
                                         const void *data_,
                                         size_t size_,
                                         int max_retries_ = 100)
{
    for (int retry = 0; retry < max_retries_; ++retry) {
        if (send_payload_blocking (socket_, data_, size_))
            return true;
        if (!is_transient_send_errno (errno))
            return false;
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    errno = EAGAIN;
    return false;
}

inline bool send_payload_blocking_retry (perf_socket_t &socket_,
                                         const zlink::routing_id_t &routing_id_,
                                         const void *data_,
                                         size_t size_,
                                         int max_retries_ = 100)
{
    for (int retry = 0; retry < max_retries_; ++retry) {
        if (send_payload_blocking (socket_, routing_id_, data_, size_))
            return true;
        if (!is_transient_send_errno (errno))
            return false;
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    errno = EAGAIN;
    return false;
}

inline bool send_payload_blocking (zlink::pair_socket_t &socket_, const void *data_, size_t size_)
{
    zlink::message_t msg = message_from_payload (data_, size_);
    if (!msg.valid ())
        return false;
    try {
        return socket_.send ().message (msg).submit ();
    }
    catch (const zlink::binding_error_t &err) {
        errno = err.internal_errno ();
        return false;
    }
}

inline bool publish_payload_blocking (zlink::pub_socket_t &publisher_,
                                      const std::string &topic_,
                                      const void *data_,
                                      size_t size_)
{
    zlink::message_t msg = message_from_payload (data_, size_);
    if (!msg.valid ())
        return false;
    try {
        return publisher_.publish (topic_).message (msg).submit ();
    }
    catch (const zlink::binding_error_t &err) {
        errno = err.internal_errno ();
        return false;
    }
}

inline bool publish_payload_blocking_retry (zlink::pub_socket_t &publisher_,
                                            const std::string &topic_,
                                            const void *data_,
                                            size_t size_,
                                            int max_retries_ = 100)
{
    for (int retry = 0; retry < max_retries_; ++retry) {
        if (publish_payload_blocking (publisher_, topic_, data_, size_))
            return true;
        if (!is_transient_send_errno (errno))
            return false;
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    errno = EAGAIN;
    return false;
}

// C-faithful DONTWAIT send (mirrors bindings/c/perf single
// perf_single_one_way.hpp send_socket_active_message with
// retry_on_eagain=true). Returns:
//   1  -> sent
//   0  -> transient backpressure (EAGAIN/EWOULDBLOCK/ETIMEDOUT/EINTR);
//         caller must re-stamp a fresh timestamp and retry, exactly like
//         C's send_active_samples loop, so the delivered message never
//         carries a stale timestamp.
//  -1  -> fatal error
inline int send_payload_dontwait (zlink::pair_socket_t &socket_, const void *data_, size_t size_)
{
    zlink::message_t msg = message_from_payload (data_, size_);
    if (!msg.valid ())
        return -1;
    try {
        const bool sent = std::move (socket_.send ().message (msg).flags (
                                       static_cast<int> (zlink::send_flags_t::dontwait)))
                            .submit ();
        return sent ? 1 : 0;
    }
    catch (const zlink::binding_error_t &err) {
        errno = err.internal_errno ();
        if (is_transient_send_errno (errno))
            return 0;
        return -1;
    }
}

inline int send_payload_dontwait (perf_socket_t &socket_, const void *data_, size_t size_)
{
    zlink::message_t msg = message_from_payload (data_, size_);
    if (!msg.valid ())
        return -1;
    try {
        // send_socket() returns 0 on success and -1 with errno already
        // set (EAGAIN on backpressure, internal_errno on error).
        if (::perf::send_socket (socket_, msg, static_cast<int> (zlink::send_flags_t::dontwait))
            == 0)
            return 1;
        return is_transient_send_errno (errno) ? 0 : -1;
    }
    catch (const zlink::binding_error_t &err) {
        errno = err.internal_errno ();
        if (is_transient_send_errno (errno))
            return 0;
        return -1;
    }
}

inline bool send_stop_token_blocking (perf_socket_t &socket_)
{
    return send_payload_blocking_retry (socket_, k_stop_token, std::strlen (k_stop_token));
}

inline bool send_stop_token_blocking (perf_socket_t &socket_,
                                      const zlink::routing_id_t &routing_id_)
{
    return send_payload_blocking_retry (socket_, routing_id_, k_stop_token,
                                        std::strlen (k_stop_token));
}

inline bool send_stop_token_blocking (zlink::pair_socket_t &socket_)
{
    return send_payload_blocking (socket_, k_stop_token, std::strlen (k_stop_token));
}

inline bool publish_stop_token_blocking (zlink::pub_socket_t &publisher_, const std::string &topic_)
{
    return publish_payload_blocking_retry (publisher_, topic_, k_stop_token,
                                           std::strlen (k_stop_token));
}

#include "perf_single_report.hpp"

} // namespace single
} // namespace perf

#endif
