#ifndef BENCH_COMMON_ZLINK_HPP
#define BENCH_COMMON_ZLINK_HPP

#include <chrono>
#include <vector>
#include <string>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <iomanip>
#include <thread>
#include <fstream>
#include <climits>
#include <zlink.h>
#include "../../../../src/core/msg.hpp"

typedef struct zlink_peer_info_t
{
    uint64_t connected_time;
    uint64_t msgs_sent;
    uint64_t msgs_received;
    uint64_t snd_pending_msgs;
    uint64_t rcv_pending_msgs;
} zlink_peer_info_t;

inline int zlink_socket_peers (void *, zlink_peer_info_t *, size_t *)
{
    errno = ENOTSUP;
    return -1;
}

inline bool bench_msg_has_more (const zlink_msg_t &msg_)
{
    return (reinterpret_cast<const zlink::msg_t *> (&msg_)->flags () & zlink::msg_t::more) != 0;
}

#if !defined(_WIN32)
#include <dlfcn.h>
#else
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

// --- Configuration ---
static const std::vector<size_t> MSG_SIZES = {64, 256, 1024, 65536, 131072, 262144};
static const std::vector<std::string> TRANSPORTS = {"tcp", "ipc", "inproc"};
static const std::vector<std::string> STREAM_TRANSPORTS = {"tcp", "tls", "ws", "wss"};
static const size_t MAX_SOCKET_STRING = 256;
static const int SETTLE_TIME_MS = 300;

// --- Stopwatch ---
class stopwatch_t
{
  public:
    void start () { _start = std::chrono::steady_clock::now (); }
    double elapsed_ms () const
    {
        auto end = std::chrono::steady_clock::now ();
        return std::chrono::duration<double, std::milli> (end - _start).count ();
    }

  private:
    std::chrono::steady_clock::time_point _start;
};

inline int parse_positive_env (const char *name_, int default_value_)
{
    if (!name_)
        return default_value_;

    const char *env = std::getenv (name_);
    if (!env || !*env)
        return default_value_;

    errno = 0;
    char *end = NULL;
    const long parsed = std::strtol (env, &end, 10);
    if (errno != 0 || end == env || parsed <= 0)
        return default_value_;

    if (parsed > INT_MAX)
        return INT_MAX;
    return static_cast<int> (parsed);
}

inline int parse_positive_env_or_default (const char *name_, int default_value_)
{
    return parse_positive_env (name_, default_value_);
}

inline int resolve_single_duration_seconds ()
{
    return parse_positive_env_or_default ("PERF_SINGLE_DURATION_SECONDS", 2);
}

inline int resolve_single_warmup_seconds ()
{
    return parse_positive_env_or_default ("PERF_SINGLE_WARMUP_SECONDS", 2);
}

inline int resolve_single_latency_duration_seconds ()
{
    const int base = resolve_single_duration_seconds ();
    return parse_positive_env_or_default ("PERF_SINGLE_LATENCY_SECONDS", base);
}

inline size_t resolve_single_latency_sample_cap ()
{
    const int cap = parse_positive_env ("PERF_SINGLE_LATENCY_SAMPLE_CAP", 200000);
    return cap > 0 ? static_cast<size_t> (cap) : static_cast<size_t> (200000);
}

struct latency_stats_t
{
    latency_stats_t () : mean_us (0.0), p95_us (0.0), p99_us (0.0) {}
    double mean_us;
    double p95_us;
    double p99_us;
};

struct queue_stats_t
{
    queue_stats_t () :
        snd_pending_max (0.0),
        rcv_pending_max (0.0),
        rcv_pending_end (0.0),
        has_snd_pending (false),
        has_rcv_pending (false)
    {
    }

    double snd_pending_max;
    double rcv_pending_max;
    double rcv_pending_end;
    bool has_snd_pending;
    bool has_rcv_pending;
};

class latency_stats_builder_t
{
  public:
    explicit latency_stats_builder_t (size_t sample_cap_ = resolve_single_latency_sample_cap ()) :
        _sample_cap (sample_cap_ > 0 ? sample_cap_ : 1),
        _count (0),
        _sum_us (0.0),
        _rng_state (0x9e3779b97f4a7c15ULL)
    {
        _samples.reserve (_sample_cap);
    }

    void add (double latency_us_)
    {
        const double sample = latency_us_ >= 0.0 ? latency_us_ : 0.0;
        ++_count;
        _sum_us += sample;

        if (_samples.size () < _sample_cap) {
            _samples.push_back (sample);
            return;
        }

        const unsigned long long slot = next_rand_u64 () % _count;
        if (slot < static_cast<unsigned long long> (_sample_cap)) {
            _samples[static_cast<size_t> (slot)] = sample;
        }
    }

    unsigned long long count () const { return _count; }

    latency_stats_t snapshot ()
    {
        latency_stats_t out;
        if (_count == 0)
            return out;

        out.mean_us = _sum_us / static_cast<double> (_count);
        if (_samples.empty ()) {
            out.p95_us = out.mean_us;
            out.p99_us = out.mean_us;
            return out;
        }

        std::sort (_samples.begin (), _samples.end ());
        out.p95_us = percentile_from_sorted (_samples, 0.95);
        out.p99_us = percentile_from_sorted (_samples, 0.99);
        return out;
    }

  private:
    static double percentile_from_sorted (const std::vector<double> &sorted_, double q_)
    {
        if (sorted_.empty ())
            return 0.0;
        if (q_ <= 0.0)
            return sorted_.front ();
        if (q_ >= 1.0)
            return sorted_.back ();

        const double pos = (sorted_.size () - 1) * q_;
        const size_t lo = static_cast<size_t> (pos);
        const size_t hi = lo + 1 < sorted_.size () ? lo + 1 : lo;
        const double frac = pos - static_cast<double> (lo);
        return sorted_[lo] + (sorted_[hi] - sorted_[lo]) * frac;
    }

    unsigned long long next_rand_u64 ()
    {
        if (_rng_state == 0)
            _rng_state = 0x9e3779b97f4a7c15ULL;
        unsigned long long x = _rng_state;
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        _rng_state = x;
        return x;
    }

    size_t _sample_cap;
    unsigned long long _count;
    double _sum_us;
    unsigned long long _rng_state;
    std::vector<double> _samples;
};

inline int bench_io_threads ()
{
    return parse_positive_env ("PERF_IO_THREADS", 1);
}

inline int bench_max_sockets ()
{
    const int explicit_max = parse_positive_env ("PERF_MAX_SOCKETS", 0);
    if (explicit_max > 0)
        return explicit_max;

    const int clients = parse_positive_env ("PERF_MULTI_CLIENTS", 0);
    if (clients <= 0)
        return 0;

    const long required = static_cast<long> (clients) + 4096L;
    if (required > INT_MAX)
        return INT_MAX;
    return static_cast<int> (required);
}

inline void apply_ctx_options (void *ctx_)
{
    const bool debug = std::getenv ("PERF_DEBUG") != NULL;
    const int io_threads = bench_io_threads ();
    if (io_threads > 0) {
        const int rc = zlink_ctx_set (ctx_, ZLINK_IO_THREADS, io_threads);
        if (rc != 0 && debug) {
            std::cerr << "zlink_ctx_set(ZLINK_IO_THREADS) failed: "
                      << zlink_strerror (zlink_errno ()) << std::endl;
        }
    }

    const int max_sockets = bench_max_sockets ();
    if (max_sockets > 0) {
        const int rc = zlink_ctx_set (ctx_, ZLINK_MAX_SOCKETS, max_sockets);
        if (rc != 0 && debug) {
            std::cerr << "zlink_ctx_set(ZLINK_MAX_SOCKETS) failed: "
                      << zlink_strerror (zlink_errno ()) << std::endl;
        }
    }
}

class ctx_guard_t
{
  public:
    ctx_guard_t () : _ctx (zlink_ctx_new ())
    {
        if (_ctx)
            apply_ctx_options (_ctx);
    }
    ~ctx_guard_t ()
    {
        if (_ctx) {
            zlink_ctx_shutdown (_ctx);
            zlink_ctx_term (_ctx);
        }
    }

    void *get () const { return _ctx; }
    bool valid () const { return _ctx != NULL; }

  private:
    ctx_guard_t (const ctx_guard_t &);
    ctx_guard_t &operator= (const ctx_guard_t &);

    void *_ctx;
};

class socket_guard_t
{
  public:
    socket_guard_t () : _socket (NULL) {}
    socket_guard_t (void *ctx_, int type_) :
        _socket (zlink_socket (ctx_, static_cast<zlink_socket_type_t> (type_)))
    {
    }
    ~socket_guard_t ()
    {
        if (_socket)
            zlink_close (_socket);
    }

    void *get () const { return _socket; }
    bool valid () const { return _socket != NULL; }
    operator void * () const { return _socket; }

  private:
    socket_guard_t (const socket_guard_t &);
    socket_guard_t &operator= (const socket_guard_t &);

    void *_socket;
};

inline void print_result (const std::string &lib_type,
                          const std::string &pattern,
                          const std::string &transport,
                          size_t size,
                          double throughput,
                          double latency,
                          double latency_p95,
                          double latency_p99)
{
    const double bandwidth_mb_s = (throughput * static_cast<double> (size)) / 1000000.0;
    std::cout << "RESULT," << lib_type << "," << pattern << "," << transport << "," << size
              << ",throughput," << std::fixed << std::setprecision (2) << throughput << std::endl;
    std::cout << "RESULT," << lib_type << "," << pattern << "," << transport << "," << size
              << ",bandwidth," << std::fixed << std::setprecision (2) << bandwidth_mb_s
              << std::endl;
    std::cout << "RESULT," << lib_type << "," << pattern << "," << transport << "," << size
              << ",latency," << std::fixed << std::setprecision (2) << latency << std::endl;
    std::cout << "RESULT," << lib_type << "," << pattern << "," << transport << "," << size
              << ",latency_p95," << std::fixed << std::setprecision (2) << latency_p95 << std::endl;
    std::cout << "RESULT," << lib_type << "," << pattern << "," << transport << "," << size
              << ",latency_p99," << std::fixed << std::setprecision (2) << latency_p99 << std::endl;
}

inline void print_queue_metrics (const std::string &lib_type,
                                 const std::string &pattern,
                                 const std::string &transport,
                                 size_t size,
                                 const queue_stats_t &queue_stats)
{
    if (queue_stats.has_snd_pending) {
        std::cout << "RESULT," << lib_type << "," << pattern << "," << transport << "," << size
                  << ",snd_pending_max," << std::fixed << std::setprecision (2)
                  << queue_stats.snd_pending_max << std::endl;
    }
    if (queue_stats.has_rcv_pending) {
        std::cout << "RESULT," << lib_type << "," << pattern << "," << transport << "," << size
                  << ",rcv_pending_max," << std::fixed << std::setprecision (2)
                  << queue_stats.rcv_pending_max << std::endl;
        std::cout << "RESULT," << lib_type << "," << pattern << "," << transport << "," << size
                  << ",rcv_pending_end," << std::fixed << std::setprecision (2)
                  << queue_stats.rcv_pending_end << std::endl;
    }
}

inline void print_result (const std::string &lib_type,
                          const std::string &pattern,
                          const std::string &transport,
                          size_t size,
                          double throughput,
                          double latency,
                          double latency_p95,
                          double latency_p99,
                          const queue_stats_t &queue_stats)
{
    print_result (lib_type, pattern, transport, size, throughput, latency, latency_p95,
                  latency_p99);
    print_queue_metrics (lib_type, pattern, transport, size, queue_stats);
}

inline void print_result (const std::string &lib_type,
                          const std::string &pattern,
                          const std::string &transport,
                          size_t size,
                          double throughput,
                          double latency)
{
    print_result (lib_type, pattern, transport, size, throughput, latency, latency, latency);
}

inline bool bench_debug_enabled ()
{
    static const bool enabled = std::getenv ("PERF_DEBUG") != nullptr;
    return enabled;
}

inline bool set_sockopt_int (void *socket_, zlink_option_t option_, int value_, const char *name_)
{
    const int rc = zlink_set_option (socket_, option_, &value_, sizeof (value_));
    if (rc != 0 && bench_debug_enabled ()) {
        std::cerr << "setsockopt(" << name_ << ") failed: " << zlink_strerror (zlink_errno ())
                  << std::endl;
    }
    if (bench_debug_enabled ()) {
        int out = 0;
        size_t out_size = sizeof (out);
        const int grc = zlink_get_option (socket_, option_, &out, &out_size);
        if (grc == 0) {
            std::cerr << "setsockopt(" << name_ << ") = " << out << std::endl;
        }
    }
    return rc == 0;
}

inline bool set_sockopt_hwm_bytes (void *socket_,
                                   zlink_option_t option_,
                                   uint64_t value_,
                                   const char *name_)
{
    const int rc = zlink_set_option (socket_, option_, &value_, sizeof (value_));
    if (rc != 0) {
        std::cerr << "setsockopt(" << name_ << ") failed: "
                  << zlink_strerror (zlink_errno ()) << std::endl;
        return false;
    }
    if (bench_debug_enabled ())
        std::cerr << "setsockopt(" << name_ << ") = " << value_ << " bytes" << std::endl;
    return true;
}

inline bool
set_pub_opt_int (void *socket_, zlink_pub_option_t option_, int value_, const char *name_)
{
    const int rc = zlink_set_pub_option (socket_, option_, &value_, sizeof (value_));
    if (rc != 0 && bench_debug_enabled ()) {
        std::cerr << "set_pub_option(" << name_ << ") failed: " << zlink_strerror (zlink_errno ())
                  << std::endl;
    }
    return rc == 0;
}

inline int resolve_single_send_timeout_ms ()
{
    return parse_positive_env ("PERF_SINGLE_SNDTIMEO_MS", 200);
}

inline int resolve_single_recv_timeout_ms ()
{
    return parse_positive_env_or_default ("PERF_SINGLE_RCVTIMEO_MS", 200);
}

inline bool use_raw_msg_api_bench ()
{
    return false;
}

inline int bench_msg_init_copy (zlink_msg_t *msg_, const void *data_, size_t size_)
{
    if (!msg_) {
        errno = EFAULT;
        return -1;
    }

    if (::zlink_msg_init_size (msg_, size_) != 0)
        return -1;

    if (size_ > 0 && data_)
        std::memcpy (::zlink_msg_data (msg_), data_, size_);
    return 0;
}

inline int bench_send_single_part (void *socket_, zlink_msg_t *msg_, zlink_send_flags_t flags_)
{
    return ::zlink_std_compat_send (socket_, msg_, 1, flags_) == ZLINK_SUBMIT_OK ? 0 : -1;
}

inline int bench_send_single_part_routed (void *socket_,
                                          const zlink_routing_id_t *target_rid_,
                                          zlink_msg_t *msg_,
                                          zlink_send_flags_t flags_)
{
    return ::zlink_std_compat_send_rid (socket_, target_rid_, msg_, 1, flags_) == ZLINK_SUBMIT_OK
             ? 0
             : -1;
}

inline int bench_recv_single_part (void *socket_, zlink_msg_t *msg_, int flags_)
{
    if (!socket_ || !msg_) {
        errno = EFAULT;
        return -1;
    }

    zlink_msg_t *parts = NULL;
    size_t part_count = 0;
    const int rc = ::zlink_std_compat_recv (socket_, NULL, &parts, &part_count,
                                            static_cast<zlink_recv_flags_t> (flags_));
    if (rc != 0)
        return -1;

    if (!parts || part_count != 1) {
        if (parts)
            ::zlink_multipart_close (parts, part_count);
        errno = EMSGSIZE;
        return -1;
    }

    const int move_rc = ::zlink_msg_move (msg_, &parts[0]);
    const int saved_errno = errno;
    ::zlink_multipart_close (parts, part_count);
    errno = saved_errno;
    return move_rc;
}

inline int bench_recv_single_part_routed (void *socket_,
                                          zlink_msg_t *msg_,
                                          zlink_routing_id_t *source_rid_out_,
                                          int flags_)
{
    if (!socket_ || !msg_) {
        errno = EFAULT;
        return -1;
    }

    zlink_msg_t *parts = NULL;
    size_t part_count = 0;
    const int rc = ::zlink_std_compat_recv (socket_, source_rid_out_, &parts, &part_count,
                                            static_cast<zlink_recv_flags_t> (flags_));
    if (rc != 0)
        return -1;

    if (!parts || part_count != 1) {
        if (parts)
            ::zlink_multipart_close (parts, part_count);
        errno = EMSGSIZE;
        return -1;
    }

    const int move_rc = ::zlink_msg_move (msg_, &parts[0]);
    const int saved_errno = errno;
    ::zlink_multipart_close (parts, part_count);
    errno = saved_errno;
    return move_rc;
}

inline int resolve_single_pubsub_recv_timeout_ms ()
{
    return resolve_single_recv_timeout_ms ();
}

inline uint64_t resolve_single_socket_hwm_bytes (bool send_)
{
    const int base_hwm = parse_positive_env ("PERF_SINGLE_HWM", 1000);
    const int message_slots =
      send_ ? parse_positive_env ("PERF_SINGLE_SNDHWM", base_hwm)
            : parse_positive_env ("PERF_SINGLE_RCVHWM", base_hwm);
    const uint64_t unit = ZLINK_AUTO_HWM_MESSAGE_UNIT_BYTES_DFLT;
    return static_cast<uint64_t> (message_slots) > UINT64_MAX / unit
             ? UINT64_MAX
             : static_cast<uint64_t> (message_slots) * unit;
}

inline int resolve_single_queue_sample_ms ()
{
    return parse_positive_env ("PERF_SINGLE_QUEUE_SAMPLE_MS", 100);
}

class queue_probe_t
{
  public:
    queue_probe_t (void *send_socket_, void *recv_socket_) :
        _send_socket (send_socket_),
        _recv_socket (recv_socket_),
        _sample_interval_ns (resolve_sample_interval_ns ()),
        _send_last_sample_ns (0),
        _recv_last_sample_ns (0),
        _snd_pending_max (0),
        _rcv_pending_max (0),
        _rcv_pending_end (0),
        _snd_seen (false),
        _rcv_seen (false)
    {
    }

    void sample_send_if_due () { maybe_sample_send (false); }
    void sample_recv_if_due () { maybe_sample_recv (false); }
    void force_sample_send () { maybe_sample_send (true); }
    void force_sample_recv () { maybe_sample_recv (true); }

    queue_stats_t snapshot () const
    {
        queue_stats_t out;
        if (_snd_seen) {
            out.has_snd_pending = true;
            out.snd_pending_max = static_cast<double> (_snd_pending_max);
        }
        if (_rcv_seen) {
            out.has_rcv_pending = true;
            out.rcv_pending_max = static_cast<double> (_rcv_pending_max);
            out.rcv_pending_end = static_cast<double> (_rcv_pending_end);
        }
        return out;
    }

  private:
    static unsigned long long resolve_sample_interval_ns ()
    {
        const int sample_ms = resolve_single_queue_sample_ms ();
        const unsigned long long clamped_ms =
          static_cast<unsigned long long> (sample_ms > 0 ? sample_ms : 100);
        return clamped_ms * 1000000ULL;
    }

    static unsigned long long now_ns ()
    {
        return static_cast<unsigned long long> (
          std::chrono::duration_cast<std::chrono::nanoseconds> (
            std::chrono::steady_clock::now ().time_since_epoch ())
            .count ());
    }

    static unsigned long long peer_activity_score (const zlink_peer_info_t &info_)
    {
        return static_cast<unsigned long long> (info_.msgs_sent)
               + static_cast<unsigned long long> (info_.msgs_received);
    }

    static bool read_first_peer_info (void *socket_, zlink_peer_info_t *info_)
    {
        if (!socket_ || !info_)
            return false;

        size_t peer_count = 0;
        if (zlink_socket_peers (socket_, NULL, &peer_count) != 0 || peer_count == 0)
            return false;

        std::vector<zlink_peer_info_t> peers (peer_count);
        size_t to_copy = peer_count;
        if (zlink_socket_peers (socket_, &peers[0], &to_copy) != 0 || to_copy == 0)
            return false;

        size_t best = 0;
        for (size_t i = 1; i < to_copy; ++i) {
            const zlink_peer_info_t &cand = peers[i];
            const zlink_peer_info_t &cur = peers[best];
            if (cand.connected_time > cur.connected_time) {
                best = i;
                continue;
            }
            if (cand.connected_time == cur.connected_time
                && peer_activity_score (cand) > peer_activity_score (cur)) {
                best = i;
            }
        }

        *info_ = peers[best];
        return true;
    }

    void maybe_sample_send (bool force_)
    {
        if (!_send_socket)
            return;

        const unsigned long long now = now_ns ();
        if (!force_ && _send_last_sample_ns > 0
            && now - _send_last_sample_ns < _sample_interval_ns) {
            return;
        }
        _send_last_sample_ns = now;

        zlink_peer_info_t info;
        if (!read_first_peer_info (_send_socket, &info))
            return;

        const unsigned long long pending = static_cast<unsigned long long> (info.snd_pending_msgs);
        if (!_snd_seen || pending > _snd_pending_max)
            _snd_pending_max = pending;
        _snd_seen = true;
    }

    void maybe_sample_recv (bool force_)
    {
        if (!_recv_socket)
            return;

        const unsigned long long now = now_ns ();
        if (!force_ && _recv_last_sample_ns > 0
            && now - _recv_last_sample_ns < _sample_interval_ns) {
            return;
        }
        _recv_last_sample_ns = now;

        zlink_peer_info_t info;
        if (!read_first_peer_info (_recv_socket, &info))
            return;

        const unsigned long long pending = static_cast<unsigned long long> (info.rcv_pending_msgs);
        if (!_rcv_seen || pending > _rcv_pending_max)
            _rcv_pending_max = pending;
        _rcv_pending_end = pending;
        _rcv_seen = true;
    }

    void *_send_socket;
    void *_recv_socket;
    unsigned long long _sample_interval_ns;
    unsigned long long _send_last_sample_ns;
    unsigned long long _recv_last_sample_ns;
    unsigned long long _snd_pending_max;
    unsigned long long _rcv_pending_max;
    unsigned long long _rcv_pending_end;
    bool _snd_seen;
    bool _rcv_seen;

    queue_probe_t (const queue_probe_t &);
    queue_probe_t &operator= (const queue_probe_t &);
};

inline queue_stats_t sample_queue_stats (queue_probe_t *queue_probe_)
{
    if (!queue_probe_)
        return queue_stats_t ();
    queue_probe_->force_sample_send ();
    queue_probe_->force_sample_recv ();
    return queue_probe_->snapshot ();
}

inline void print_fail_result (const std::string &lib_type,
                               const std::string &pattern,
                               const std::string &transport,
                               size_t size,
                               queue_probe_t *queue_probe_ = NULL)
{
    if (!queue_probe_)
        return;
    const queue_stats_t queue_stats = sample_queue_stats (queue_probe_);
    print_queue_metrics (lib_type, pattern, transport, size, queue_stats);
}

inline bool apply_single_hwm (void *socket_)
{
    if (!socket_)
        return false;

    const uint64_t sndhwm = resolve_single_socket_hwm_bytes (true);
    const uint64_t rcvhwm = resolve_single_socket_hwm_bytes (false);
    return set_sockopt_hwm_bytes (
             socket_, ZLINK_OPT_SNDHWM, sndhwm, "ZLINK_OPT_SNDHWM")
           && set_sockopt_hwm_bytes (
             socket_, ZLINK_OPT_RCVHWM, rcvhwm, "ZLINK_OPT_RCVHWM");
}

inline void apply_single_send_timeout (void *socket_, const std::string &)
{
    if (!socket_)
        return;
    const int timeout_ms = resolve_single_send_timeout_ms ();
    set_sockopt_int (socket_, ZLINK_OPT_SNDTIMEO, timeout_ms, "ZLINK_OPT_SNDTIMEO");
}

inline void apply_debug_timeouts (void *socket_, const std::string &transport)
{
    if (!bench_debug_enabled ())
        return;
    if (transport == "tcp" || transport == "ws") {
        const int timeout_ms = 2000;
        set_sockopt_int (socket_, ZLINK_OPT_SNDTIMEO, timeout_ms, "ZLINK_OPT_SNDTIMEO");
        set_sockopt_int (socket_, ZLINK_OPT_RCVTIMEO, timeout_ms, "ZLINK_OPT_RCVTIMEO");
    }
}

inline std::string make_endpoint (const std::string &transport, const std::string &id)
{
    if (transport == "inproc")
        return "inproc://" + id;
    if (transport == "ipc")
        return "ipc://*";
    if (transport == "ws")
        return "ws://127.0.0.1:*";
    if (transport == "wss")
        return "wss://127.0.0.1:*";
    if (transport == "tls")
        return "tls://127.0.0.1:*";
    return "tcp://127.0.0.1:*";
}

inline std::string make_fixed_endpoint (const std::string &transport, int port)
{
    const std::string host = "127.0.0.1";
    const std::string port_str = std::to_string (port);
    if (transport == "ws")
        return "ws://" + host + ":" + port_str;
    if (transport == "wss")
        return "wss://" + host + ":" + port_str;
    if (transport == "tls")
        return "tls://" + host + ":" + port_str;
    return "tcp://" + host + ":" + port_str;
}

inline void *resolve_symbol (const char *name)
{
#if defined(_WIN32)
    HMODULE module = GetModuleHandleA (NULL);
    if (!module)
        return NULL;
    return reinterpret_cast<void *> (GetProcAddress (module, name));
#else
    return dlsym (RTLD_DEFAULT, name);
#endif
}

// --- Embedded Test Certificates for TLS ---
namespace test_certs
{

static const char *ca_cert_pem =
  "-----BEGIN CERTIFICATE-----\n"
  "MIIDlzCCAn+gAwIBAgIUbGLNLbwV7np9Q07zD9ZWvmA+nkAwDQYJKoZIhvcNAQEL\n"
  "BQAwWzELMAkGA1UEBhMCVVMxDTALBgNVBAgMBFRlc3QxDTALBgNVBAcMBFRlc3Qx\n"
  "FjAUBgNVBAoMDVpMaW5rIFRlc3QgQ0ExFjAUBgNVBAMMDVpMaW5rIFRlc3QgQ0Ew\n"
  "HhcNMjYwMTEyMTEyMjUzWhcNMzYwMTEwMTEyMjUzWjBbMQswCQYDVQQGEwJVUzEN\n"
  "MAsGA1UECAwEVGVzdDENMAsGA1UEBwwEVGVzdDEWMBQGA1UECgwNWkxpbmsgVGVz\n"
  "dCBDQTEWMBQGA1UEAwwNWkxpbmsgVGVzdCBDQTCCASIwDQYJKoZIhvcNAQEBBQAD\n"
  "ggEPADCCAQoCggEBAKHAdjzB5SsoFlce8T4XBvQa0LAbYP9hQ+jcLXSzoF/QDmeP\n"
  "sxGSE1WINM7ZT9BOqNa8OKl7kWWWYS45XeeqrNLVHDQbz9DvUAqUVaSsoxyAxCtV\n"
  "8Zq+F6Zy01qbLXi+Nv1jWz685X9KSc5SCKz9acoOSBU7IOtJKCQ+QM+/x9PMqQeg\n"
  "B+aRNkv+WE4RRLbpQnIGqSiZkUsNI6Z97o2otsHkGa1oVWWXmKqzUAmembVHjiCl\n"
  "Rn9Ut4/HqqopLn/k2m7/Lj62QT6sOcB8ixDe+H4TwDF6sbxgHcs/1sdobys6VsUF\n"
  "gFSJ5Dm33yYBjQmLfxXRaKMxKGukLmAofa+f28sCAwEAAaNTMFEwHQYDVR0OBBYE\n"
  "FO3BqMenuNdTJuCz5tywoNrd11KjMB8GA1UdIwQYMBaAFO3BqMenuNdTJuCz5tyw\n"
  "oNrd11KjMA8GA1UdEwEB/wQFMAMBAf8wDQYJKoZIhvcNAQELBQADggEBADF2GjWc\n"
  "BuvU/3bG2406XNFtl7pb4V70zClo269Gb/SYVrF0k6EXp2I8UQ7cPXM+ueWu8JeG\n"
  "XCbSTRADWxw702VxryCXLIYYMZ5hwF5ZtDGOagZQWSz38UFy2acCRNqY2ijyISQn\n"
  "3M8YtRdeEGOan+gtTC6/xB3IIRX1tFohT35G/wjld8hs6kJVokYhVfKhk4EZKSxH\n"
  "IiHsVaafpjUwm4EkAwCmwAWkOalKijbo5Jdq9h3UNfOn4RblN80FU/jD2cBFP+L8\n"
  "U/Juz13KFa/4NXp9flzUl/1w5o//V1UXUpfYOMsVT8BaP3dV1pa9lDwhoJERyiI1\n"
  "xj0kGsPBIt3nVwE=\n"
  "-----END CERTIFICATE-----\n";

static const char *server_cert_pem =
  "-----BEGIN CERTIFICATE-----\n"
  "MIIDrTCCApWgAwIBAgIUH3bva6lTINNSQ2BpgpJStZpT5NQwDQYJKoZIhvcNAQEL\n"
  "BQAwWzELMAkGA1UEBhMCVVMxDTALBgNVBAgMBFRlc3QxDTALBgNVBAcMBFRlc3Qx\n"
  "FjAUBgNVBAoMDVpMaW5rIFRlc3QgQ0ExFjAUBgNVBAMMDVpMaW5rIFRlc3QgQ0Ew\n"
  "HhcNMjYwMTEyMTEyMzAxWhcNMjcwMTEyMTEyMzAxWjBUMQswCQYDVQQGEwJVUzEN\n"
  "MAsGA1UECAwEVGVzdDENMAsGA1UEBwwEVGVzdDETMBEGA1UECgwKWkxpbmsgVGVz\n"
  "dDESMBAGA1UEAwwJbG9jYWxob3N0MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIB\n"
  "CgKCAQEAxZ5FpHxoY5JaTfbS3D1nSlz+BdvnrsZ5PqG+P/H1oGXJnY/2MMZGEeUZ\n"
  "SZg9pVn6ZRURyGTwAHN1X+xarpX057pKfqWtHLztj2+WSJLbBfzSzwPdYNMP/h1C\n"
  "MX9zMbui6ui8Tbys1g5IKO/ZEMRN8bVNHOJ4xkK829RzEu6f/4YCuf4Lz+Z1X4en\n"
  "VBi7DGkWRSUiACjlGvVyZ24KHkLCggbAO3HhhyjZ4FwVd9JuE+d2/jm/neUu6HTt\n"
  "J/9d/5GCovUamkuYWn+e62HA1FkpSnXNbgRrkmAkOrliJG1uCqh3btVzuF1c91Jj\n"
  "8wjm0wm23lDeGVrCWExvyFhk3LBFCwIDAQABo3AwbjAsBgNVHREEJTAjgglsb2Nh\n"
  "bGhvc3SHBH8AAAGHEAAAAAAAAAAAAAAAAAAAAAEwHQYDVR0OBBYEFFrMgnC8k4I0\n"
  "XMjURlF0zXV59HJYMB8GA1UdIwQYMBaAFO3BqMenuNdTJuCz5tywoNrd11KjMA0G\n"
  "CSqGSIb3DQEBCwUAA4IBAQCcXiKLN5y7rumetdr55PMDdx+4EV1Wl28fWCOB5nur\n"
  "kFZRy876pFphFqZppjGCHWiiHzUIsZXUej/hBmY+OhsL13ojfGiACz/44OFzqCUa\n"
  "I83V1M9ywbty09zhdqFc9DFfpiC2+ltDCn7o+eF7THUzgDg4fRZYHYM1njZElZaG\n"
  "ecFImsQzqFIpmhB/TfZIZVmBQryYN+V1fl4sUJFiYEOr49RjWnATf6RKY3J5VKHp\n"
  "TWSm7rTd4jB0CvyNlPpS+fYBdGC72m6R3zrce8Scfto+HPH4YdIU5AdoRHCCtOrA\n"
  "Mq9brLTPUzAqlzC7zDw41hI/MS1Cdcxb1dZkKHgMXu8W\n"
  "-----END CERTIFICATE-----\n";

static const char *server_key_pem =
  "-----BEGIN PRIVATE KEY-----\n"
  "MIIEvQIBADANBgkqhkiG9w0BAQEFAASCBKcwggSjAgEAAoIBAQDFnkWkfGhjklpN\n"
  "9tLcPWdKXP4F2+euxnk+ob4/8fWgZcmdj/YwxkYR5RlJmD2lWfplFRHIZPAAc3Vf\n"
  "7FqulfTnukp+pa0cvO2Pb5ZIktsF/NLPA91g0w/+HUIxf3Mxu6Lq6LxNvKzWDkgo\n"
  "79kQxE3xtU0c4njGQrzb1HMS7p//hgK5/gvP5nVfh6dUGLsMaRZFJSIAKOUa9XJn\n"
  "bgoeQsKCBsA7ceGHKNngXBV30m4T53b+Ob+d5S7odO0n/13/kYKi9RqaS5haf57r\n"
  "YcDUWSlKdc1uBGuSYCQ6uWIkbW4KqHdu1XO4XVz3UmPzCObTCbbeUN4ZWsJYTG/I\n"
  "WGTcsEULAgMBAAECggEACAoWclsKcmqN71yaf7ZbyBZBP95XW9UAn7byx25UDn5H\n"
  "3woUsgr8nehSyJuIx6CULMKPGVs3lXP4bpXbqyG4CeAss/H+XeekkL5D0nO4IsE5\n"
  "BSBkaL/Wh275kbCA8HyU9gAZkQLkZbPFCb+XCKLfOpntcHWGut2CLs/VVzCLbX1A\n"
  "hHerqJf3qEW+cU1Va5On+A2BEK7XtYFIR6IabS2LN5ecoZUfQ4EoeypdpQPRKwqM\n"
  "m1tSet4CsRfovguLdY5Z/hAhFLZCMKF5zs8zzGln9+S+G5y2fdJ4VxwbeR0OqyAh\n"
  "cB56xJo3L7rLm6hAoIb0mVXaiyRRGEuCBE/t9/pmSQKBgQD2hQgHpC20bQCyh08B\n"
  "1CyJKz1ObZJeYCWR6hE0stUKKq9QizY9Ci8Q1Hg8eEAtKCKjW74DbJ7bgGJBm6rS\n"
  "yNgpZZ3zw6NDSm4wY33y4alB5jzMR+H7izb6vxMPVcXn3DpjzoklxkN4l8JvgTbt\n"
  "KxZWxD3hS+C6NuNKE4LHipJO1wKBgQDNN89O/71ktIBpxiEZk4sKzdq3JZMErFBi\n"
  "cFJ4vATJ1LstrWdOAtOgRqQN81GhCSZ79vybrcOaq4Q4qLzsOWrAo7nb53gq684Y\n"
  "GaVAZfxzA+qECyEY3CzrKnwIbSFvJY+IfA1QL/ricce8oL7lIRIP1+MuhvGUdw55\n"
  "vXs01Wv47QKBgDo1sW60esJW1spRHvvMkPOWzTQetWgphdWNkqCB9cIf0CPRq24A\n"
  "YJq1wOpubqD7ECrIt/ZxCJXGG+1oB48cM8aaoxBzSrLR+XDdnVjjpibUadjGxHq0\n"
  "JbhRs/t0AnY8T2FP3JyZ00a/dv8DYOfhu7WjQwVW+GqgGU1djAz4EJIjAoGBAJe+\n"
  "iOBVYmowvjN4eck7vDiE9xEuC4QNFnNzssfr326Oism/yv94P5voIC7gmJ+G8JoB\n"
  "i9BhsJ2R7fcnbmsOGc3QQwJEKisyqfZQIE16HC2/240/3X1QcTaC96wTZgGVuIin\n"
  "kgCVOeJvV8423nD2/zAP5sDkr4Wkc2O5pHzwwyIRAoGAID2/HQQbczTqQlEAXltB\n"
  "K8YbNLP75FY+9w10SH3B0hUnEP+9YdeHvxkXdWtewn+TjkXnc3AYlb9A9u7GUuB+\n"
  "K2AF/TMl2YdHFOEDtMAZ8IT6womo6JHYj4+FfbxPiMmOfBmOKrdxQ/WrqfCnZwEs\n"
  "Dhpkrp6xWJWSNvXS0XcWGfM=\n"
  "-----END PRIVATE KEY-----\n";

} // namespace test_certs

// Write certificate to temp file and return path
inline std::string write_temp_cert (const char *content, const std::string &suffix)
{
    std::string path = "/tmp/bench_" + suffix + ".pem";
    std::ofstream ofs (path);
    if (ofs) {
        ofs << content;
        ofs.close ();
    }
    return path;
}

// Setup TLS options for server socket
inline bool setup_tls_server (void *socket, const std::string &transport)
{
    if (transport != "tls" && transport != "wss")
        return true;

    static std::string cert_path = write_temp_cert (test_certs::server_cert_pem, "server_cert");
    static std::string key_path = write_temp_cert (test_certs::server_key_pem, "server_key");

    if (zlink_set_tls_server (socket, cert_path.c_str (), key_path.c_str (), 0) != 0) {
        if (bench_debug_enabled ())
            std::cerr << "Failed to set TLS server options: " << zlink_strerror (zlink_errno ())
                      << std::endl;
        return false;
    }
    return true;
}

// Setup TLS options for client socket
inline bool setup_tls_client (void *socket, const std::string &transport)
{
    if (transport != "tls" && transport != "wss")
        return true;

    static std::string ca_path = write_temp_cert (test_certs::ca_cert_pem, "ca_cert");
    static const char *hostname = "localhost";

    if (!zlink_set_tls_client (socket, ca_path.c_str (), hostname, 0)) {
        if (bench_debug_enabled ())
            std::cerr << "Failed to set TLS client options: " << zlink_strerror (zlink_errno ())
                      << std::endl;
        return false;
    }
    return true;
}

inline std::string
bind_and_resolve_endpoint (void *socket_, const std::string &transport, const std::string &id)
{
    std::string endpoint = make_endpoint (transport, id);
    if (endpoint.empty ()) {
        std::cerr << "No endpoint available for transport " << transport << std::endl;
        return std::string ();
    }
    if (zlink_bind (socket_, endpoint.c_str ()) != ZLINK_BIND_OK) {
        std::cerr << "bind failed for " << endpoint << ": " << zlink_strerror (zlink_errno ())
                  << std::endl;
        return std::string ();
    }
    if (transport != "inproc") {
        char last_endpoint[MAX_SOCKET_STRING] = "";
        size_t size = sizeof (last_endpoint);
        if (!zlink_get_option (socket_, ZLINK_OPT_LAST_ENDPOINT, last_endpoint, &size)) {
            std::cerr << "getsockopt(ZLINK_SOCKOPT_LAST_ENDPOINT) failed: "
                      << zlink_strerror (zlink_errno ()) << std::endl;
            return std::string ();
        }
        endpoint.assign (last_endpoint);
        if (transport == "tcp" || transport == "ws") {
            const std::string tcp_any = "://0.0.0.0:";
            const std::string tcp_ipv6_any = "://[::]:";
            size_t pos = endpoint.find (tcp_any);
            if (pos != std::string::npos) {
                endpoint.replace (pos, tcp_any.size (), "://127.0.0.1:");
            } else {
                pos = endpoint.find (tcp_ipv6_any);
                if (pos != std::string::npos) {
                    endpoint.replace (pos, tcp_ipv6_any.size (), "://127.0.0.1:");
                }
            }
        }
        if (bench_debug_enabled ()) {
            std::cerr << "Resolved endpoint (" << transport << "): " << endpoint << std::endl;
        }
    }
    return endpoint;
}

inline bool transport_available (const std::string &transport)
{
    if (transport == "tcp" || transport == "ipc" || transport == "inproc")
        return true;
    return false;
}

inline void settle ()
{
    std::this_thread::sleep_for (std::chrono::milliseconds (SETTLE_TIME_MS));
}

inline bool connect_checked (void *socket_, const std::string &endpoint)
{
    if (zlink_connect (socket_, endpoint.c_str ()) != ZLINK_CONNECT_OK) {
        std::cerr << "connect failed for " << endpoint << ": " << zlink_strerror (zlink_errno ())
                  << std::endl;
        return false;
    }
    if (bench_debug_enabled ()) {
        std::cerr << "Connected to " << endpoint << std::endl;
    }
    return true;
}

inline bool setup_connected_pair (void *bind_socket_,
                                  void *connect_socket_,
                                  const std::string &transport_,
                                  const std::string &id_)
{
    if (!setup_tls_server (bind_socket_, transport_)
        || !setup_tls_client (connect_socket_, transport_))
        return false;

    if (!apply_single_hwm (bind_socket_) || !apply_single_hwm (connect_socket_))
        return false;

    std::string endpoint = bind_and_resolve_endpoint (bind_socket_, transport_, id_);
    if (endpoint.empty ())
        return false;
    if (!connect_checked (connect_socket_, endpoint))
        return false;

    apply_single_send_timeout (bind_socket_, transport_);
    apply_single_send_timeout (connect_socket_, transport_);

    settle ();
    return true;
}

inline int resolve_bench_count (const char *env_name, int default_value)
{
    if (const char *env = std::getenv (env_name)) {
        errno = 0;
        const long override = std::strtol (env, NULL, 10);
        if (errno == 0 && override > 0)
            return static_cast<int> (override);
    }
    return default_value;
}

template <typename RunFn> inline int run_standard_bench_main (int argc_, char **argv_, RunFn run_)
{
    if (argc_ < 4)
        return 1;
    std::string lib_name = argv_[1];
    std::string transport = argv_[2];
    size_t size = std::stoul (argv_[3]);
    run_ (transport, size, lib_name);
    return 0;
}

#endif
