#ifndef BENCH_COMMON_ZMQ_HPP
#define BENCH_COMMON_ZMQ_HPP

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
#include <stdint.h>
#include <zmq.h>

// Reuse the single-bench source structure against libzmq types.
typedef zmq_msg_t zlink_msg_t;
typedef zmq_pollitem_t zlink_pollitem_t;
typedef struct zlink_routing_id_t
{
    uint8_t size;
    unsigned char data[256];
} zlink_routing_id_t;

#ifndef ZLINK_IO_THREADS
#define ZLINK_IO_THREADS ZMQ_IO_THREADS
#endif
#ifndef ZLINK_MAX_SOCKETS
#define ZLINK_MAX_SOCKETS ZMQ_MAX_SOCKETS
#endif
#ifndef ZLINK_SNDHWM
#define ZLINK_SNDHWM ZMQ_SNDHWM
#endif
#ifndef ZLINK_RCVHWM
#define ZLINK_RCVHWM ZMQ_RCVHWM
#endif
#ifndef ZLINK_SNDTIMEO
#define ZLINK_SNDTIMEO ZMQ_SNDTIMEO
#endif
#ifndef ZLINK_RCVTIMEO
#define ZLINK_RCVTIMEO ZMQ_RCVTIMEO
#endif
#ifndef ZLINK_DONTWAIT
#define ZLINK_DONTWAIT ZMQ_DONTWAIT
#endif
#ifndef ZLINK_RCVMORE
#define ZLINK_RCVMORE ZMQ_RCVMORE
#endif
#ifndef ZLINK_SOCKOPT_LAST_ENDPOINT
#define ZLINK_SOCKOPT_LAST_ENDPOINT ZMQ_LAST_ENDPOINT
#endif
#ifndef ZLINK_SNDMORE
#define ZLINK_SNDMORE ZMQ_SNDMORE
#endif
#ifndef ZLINK_POLLIN
#define ZLINK_POLLIN ZMQ_POLLIN
#endif
#ifndef ZLINK_SOCKET_PAIR
#define ZLINK_SOCKET_PAIR ZMQ_PAIR
#endif
#ifndef ZLINK_SOCKET_PUB
#define ZLINK_SOCKET_PUB ZMQ_PUB
#endif
#ifndef ZLINK_SOCKET_SUB
#define ZLINK_SOCKET_SUB ZMQ_SUB
#endif
#ifndef ZLINK_SOCKET_DEALER
#define ZLINK_SOCKET_DEALER ZMQ_DEALER
#endif
#ifndef ZLINK_SOCKET_ROUTER
#define ZLINK_SOCKET_ROUTER ZMQ_ROUTER
#endif
#ifndef ZLINK_ROUTING_ID
#define ZLINK_ROUTING_ID ZMQ_ROUTING_ID
#endif
#ifndef ZLINK_ROUTER_MANDATORY
#define ZLINK_ROUTER_MANDATORY ZMQ_ROUTER_MANDATORY
#endif
#ifndef ZLINK_ROUTER_OPT_MANDATORY
#define ZLINK_ROUTER_OPT_MANDATORY ZLINK_ROUTER_MANDATORY
#endif
#ifndef ZLINK_SUBSCRIBE
#define ZLINK_SUBSCRIBE ZMQ_SUBSCRIBE
#endif
#ifndef ZLINK_XPUB_NODROP
#define ZLINK_XPUB_NODROP ZMQ_XPUB_NODROP
#endif
#ifndef ZLINK_TLS_TRUST_SYSTEM
#define ZLINK_TLS_TRUST_SYSTEM 0
#endif

#define zlink_ctx_new zmq_ctx_new
#define zlink_ctx_set zmq_ctx_set
#define zlink_ctx_shutdown zmq_ctx_shutdown
#define zlink_ctx_term zmq_ctx_term
#define zlink_socket zmq_socket
#define zlink_close zmq_close
#define zlink_send zmq_send
#define zlink_recv zmq_recv
#define zlink_msg_init zmq_msg_init
#define zlink_msg_init_size zmq_msg_init_size
#define zlink_msg_close zmq_msg_close
#define zlink_msg_move zmq_msg_move
#define zlink_msg_size zmq_msg_size
#define zlink_msg_data zmq_msg_data
#define zlink_msg_more zmq_msg_more
#define zlink_setsockopt zmq_setsockopt
#define zlink_getsockopt zmq_getsockopt
#define zlink_set_subscription(socket_, filter_)                                                   \
    zmq_setsockopt ((socket_), ZMQ_SUBSCRIBE, (filter_), std::strlen (filter_))
#define zlink_set_routing_id(socket_, data_, size_)                                                \
    zmq_setsockopt ((socket_), ZMQ_ROUTING_ID, (data_), (size_))
#define zlink_set_router_option(socket_, option_, optval_, optvallen_)                             \
    zmq_setsockopt ((socket_), (option_), (optval_), (optvallen_))
#define zlink_poll zmq_poll
#define zlink_bind zmq_bind
#define zlink_connect zmq_connect
#define zlink_errno zmq_errno
#define zlink_strerror zmq_strerror
#define zlink_has zmq_has
#ifndef ZLINK_BIND_OK
#define ZLINK_BIND_OK 0
#endif
#ifndef ZLINK_CONNECT_OK
#define ZLINK_CONNECT_OK 0
#endif

struct zlink_peer_info_t
{
    uint64_t connected_time;
    uint64_t msgs_sent;
    uint64_t msgs_received;
    uint64_t snd_pending_msgs;
    uint64_t rcv_pending_msgs;
};

inline int
zlink_socket_peers (void * /*socket_*/, zlink_peer_info_t * /*peers_*/, size_t * /*count_*/)
{
    errno = ENOTSUP;
    return -1;
}

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <dlfcn.h>
#else
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#endif

// --- TLS Socket Options ---
#ifndef ZLINK_TLS_CERT
#define ZLINK_TLS_CERT 95
#endif
#ifndef ZLINK_TLS_KEY
#define ZLINK_TLS_KEY 96
#endif
#ifndef ZLINK_TLS_CA
#define ZLINK_TLS_CA 97
#endif
#ifndef ZLINK_TLS_HOSTNAME
#define ZLINK_TLS_HOSTNAME 100
#endif
#ifndef ZLINK_OPT_TLS_CERT
#define ZLINK_OPT_TLS_CERT ZLINK_TLS_CERT
#endif
#ifndef ZLINK_OPT_TLS_KEY
#define ZLINK_OPT_TLS_KEY ZLINK_TLS_KEY
#endif
#ifndef ZLINK_OPT_TLS_CA
#define ZLINK_OPT_TLS_CA ZLINK_TLS_CA
#endif
#ifndef ZLINK_OPT_TLS_HOSTNAME
#define ZLINK_OPT_TLS_HOSTNAME ZLINK_TLS_HOSTNAME
#endif
#ifndef ZLINK_OPT_TLS_TRUST_SYSTEM
#define ZLINK_OPT_TLS_TRUST_SYSTEM ZLINK_TLS_TRUST_SYSTEM
#endif
#ifndef ZLINK_OPT_LAST_ENDPOINT
#define ZLINK_OPT_LAST_ENDPOINT ZLINK_SOCKOPT_LAST_ENDPOINT
#endif

#define zlink_set_option zlink_setsockopt
#define zlink_get_option zlink_getsockopt

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
    socket_guard_t (void *ctx_, int type_) : _socket (zlink_socket (ctx_, type_)) {}
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

inline bool set_sockopt_int (void *socket_, int option_, int value_, const char *name_)
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

inline bool bench_msg_has_more (const zlink_msg_t &msg_)
{
    return zmq_msg_more (const_cast<zlink_msg_t *> (&msg_)) != 0;
}

inline int bench_msg_init_copy (zlink_msg_t *msg_, const void *data_, size_t size_)
{
    if (!msg_) {
        errno = EFAULT;
        return -1;
    }

    if (zlink_msg_init_size (msg_, size_) != 0)
        return -1;

    if (size_ > 0 && data_)
        std::memcpy (zlink_msg_data (msg_), data_, size_);
    return 0;
}

inline int bench_send_single_part (void *socket_, zlink_msg_t *msg_, int flags_)
{
    if (!socket_ || !msg_) {
        errno = EFAULT;
        return -1;
    }
    return zmq_msg_send (msg_, socket_, flags_);
}

inline int bench_recv_single_part (void *socket_, zlink_msg_t *msg_, int flags_)
{
    if (!socket_ || !msg_) {
        errno = EFAULT;
        return -1;
    }
    return zmq_msg_recv (msg_, socket_, flags_);
}

inline int bench_send_single_part_routed (void *socket_,
                                          const zlink_routing_id_t *target_rid_,
                                          zlink_msg_t *msg_,
                                          int flags_)
{
    if (!socket_ || !target_rid_ || !msg_) {
        errno = EFAULT;
        return -1;
    }

    zlink_msg_t rid_msg;
    if (bench_msg_init_copy (&rid_msg, target_rid_->data, target_rid_->size) != 0) {
        return -1;
    }

    const int rid_rc = zmq_msg_send (&rid_msg, socket_, flags_ | ZLINK_SNDMORE);
    if (rid_rc < 0) {
        zlink_msg_close (&rid_msg);
        return -1;
    }
    return zmq_msg_send (msg_, socket_, flags_);
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

    zlink_msg_t rid_msg;
    if (zlink_msg_init (&rid_msg) != 0)
        return -1;

    const int rid_rc = zmq_msg_recv (&rid_msg, socket_, flags_);
    if (rid_rc < 0) {
        zlink_msg_close (&rid_msg);
        return -1;
    }

    if (!bench_msg_has_more (rid_msg)) {
        zlink_msg_close (&rid_msg);
        errno = EMSGSIZE;
        return -1;
    }

    if (source_rid_out_) {
        const size_t rid_size = std::min (static_cast<size_t> (255), zlink_msg_size (&rid_msg));
        source_rid_out_->size = static_cast<uint8_t> (rid_size);
        if (rid_size > 0) {
            std::memcpy (source_rid_out_->data, zlink_msg_data (&rid_msg), rid_size);
        }
    }

    zlink_msg_close (&rid_msg);
    return zmq_msg_recv (msg_, socket_, 0);
}

inline int
bench_send_pubsub_single_part (void *socket_, const char *topic_, zlink_msg_t *msg_, int flags_)
{
    if (!socket_ || !msg_) {
        errno = EFAULT;
        return -1;
    }

    const size_t topic_size = topic_ ? std::strlen (topic_) : 0;
    zlink_msg_t topic_msg;
    if (bench_msg_init_copy (&topic_msg, topic_, topic_size) != 0)
        return -1;

    const int topic_rc = zmq_msg_send (&topic_msg, socket_, flags_ | ZLINK_SNDMORE);
    if (topic_rc < 0) {
        zlink_msg_close (&topic_msg);
        return -1;
    }
    return zmq_msg_send (msg_, socket_, flags_);
}

inline int bench_recv_pubsub_single_part (
  void *socket_, zlink_msg_t *msg_, char *topic_out_, size_t *topic_len_out_, int flags_)
{
    if (!socket_ || !msg_) {
        errno = EFAULT;
        return -1;
    }

    zlink_msg_t topic_msg;
    if (zlink_msg_init (&topic_msg) != 0)
        return -1;

    const int topic_rc = zmq_msg_recv (&topic_msg, socket_, flags_);
    if (topic_rc < 0) {
        zlink_msg_close (&topic_msg);
        return -1;
    }

    const size_t topic_size = zlink_msg_size (&topic_msg);
    if (topic_len_out_)
        *topic_len_out_ = topic_size;
    if (topic_out_ && topic_size > 0)
        std::memcpy (topic_out_, zlink_msg_data (&topic_msg), topic_size);

    if (!bench_msg_has_more (topic_msg)) {
        zlink_msg_close (&topic_msg);
        errno = EMSGSIZE;
        return -1;
    }

    zlink_msg_close (&topic_msg);
    return zmq_msg_recv (msg_, socket_, 0);
}

inline int resolve_single_send_timeout_ms ()
{
    return parse_positive_env ("PERF_SINGLE_SNDTIMEO_MS", 200);
}

inline int resolve_single_recv_timeout_ms ()
{
    return parse_positive_env_or_default ("PERF_SINGLE_RCVTIMEO_MS", 200);
}

inline int resolve_single_pubsub_recv_timeout_ms ()
{
    return resolve_single_recv_timeout_ms ();
}

inline int resolve_single_socket_hwm (bool send_)
{
    const int base_hwm = parse_positive_env ("PERF_SINGLE_HWM", 1000);
    return send_ ? parse_positive_env ("PERF_SINGLE_SNDHWM", base_hwm)
                 : parse_positive_env ("PERF_SINGLE_RCVHWM", base_hwm);
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

inline void apply_single_hwm (void *socket_)
{
    if (!socket_)
        return;

    const int sndhwm = resolve_single_socket_hwm (true);
    const int rcvhwm = resolve_single_socket_hwm (false);
    set_sockopt_int (socket_, ZLINK_SNDHWM, sndhwm, "ZLINK_SNDHWM");
    set_sockopt_int (socket_, ZLINK_RCVHWM, rcvhwm, "ZLINK_RCVHWM");
}

inline void apply_single_send_timeout (void *socket_, const std::string &transport_)
{
    if (!socket_)
        return;
    if (transport_ == "pgm" || transport_ == "epgm")
        return;

    const int timeout_ms = resolve_single_send_timeout_ms ();
    set_sockopt_int (socket_, ZLINK_SNDTIMEO, timeout_ms, "ZLINK_SNDTIMEO");
}

inline void apply_debug_timeouts (void *socket_, const std::string &transport)
{
    if (!bench_debug_enabled ())
        return;
    if (transport == "tcp" || transport == "ws") {
        const int timeout_ms = 2000;
        set_sockopt_int (socket_, ZLINK_SNDTIMEO, timeout_ms, "ZLINK_SNDTIMEO");
        set_sockopt_int (socket_, ZLINK_RCVTIMEO, timeout_ms, "ZLINK_RCVTIMEO");
    }
}

inline std::string make_endpoint (const std::string &transport, const std::string &id)
{
    if (transport == "pgm" || transport == "epgm") {
        if (transport == "pgm") {
            if (const char *env = std::getenv ("PERF_PGM_ENDPOINT")) {
                if (*env)
                    return std::string (env);
            }
        } else {
            if (const char *env = std::getenv ("PERF_EPGM_ENDPOINT")) {
                if (*env)
                    return std::string (env);
            }
        }
#if !defined(_WIN32)
        struct ifaddrs *ifaddr = nullptr;
        if (getifaddrs (&ifaddr) == 0) {
            for (struct ifaddrs *ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
                if (!ifa->ifa_addr)
                    continue;
                if (!(ifa->ifa_flags & IFF_UP))
                    continue;
                if (!(ifa->ifa_flags & IFF_MULTICAST))
                    continue;
                if (ifa->ifa_flags & IFF_LOOPBACK)
                    continue;
                if (ifa->ifa_addr->sa_family != AF_INET)
                    continue;
                char addr[INET_ADDRSTRLEN];
                const struct sockaddr_in *sa =
                  reinterpret_cast<const struct sockaddr_in *> (ifa->ifa_addr);
                if (inet_ntop (AF_INET, &sa->sin_addr, addr, sizeof (addr))) {
                    std::string endpoint = transport + "://" + addr + ";239.192.1.1:5555";
                    freeifaddrs (ifaddr);
                    return endpoint;
                }
            }
            freeifaddrs (ifaddr);
        }
#endif
        return std::string ();
    }
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
  "MIIDlzCCAn+gAwIBAgIUR/zXRG8ehuv+DU7HSxpg8ch7AxMwDQYJKoZIhvcNAQEL\n"
  "BQAwWzELMAkGA1UEBhMCVVMxDTALBgNVBAgMBFRlc3QxDTALBgNVBAcMBFRlc3Qx\n"
  "FjAUBgNVBAoMDVpMaW5rIFRlc3QgQ0ExFjAUBgNVBAMMDVpMaW5rIFRlc3QgQ0Ew\n"
  "HhcNMjYwOTE1MDIzODI4WhcNNDYwOTE1MDIzODI4WjBbMQswCQYDVQQGEwJVUzEN\n"
  "MAsGA1UECAwEVGVzdDENMAsGA1UEBwwEVGVzdDEWMBQGA1UECgwNWkxpbmsgVGVz\n"
  "dCBDQTEWMBQGA1UEAwwNWkxpbmsgVGVzdCBDQTCCASIwDQYJKoZIhvcNAQEBBQAD\n"
  "ggEPADCCAQoCggEBAMLcka2BjyM0pIaWzJKnOO5yeAeWhwOMbOsLYJTmQxB9eIC8\n"
  "eI0bQVf7PObBdOSHOuKVqzo46n2WOr3lFkjIq9wI/ww+QE0O9a2kYmd7eKXQBLeF\n"
  "VmEuVNQ4WqN4CkcTpSgRxeJHeFrxWAm9HRrkIunI/8KMITx0sXI9BQM2U8JeZkld\n"
  "VP3ai2reH6qx/zBHxPQ4+kodkPbHN8t3SzrV09YhmnAUbp0Ic9Ckpala85HSBfGp\n"
  "/PMbmwWoAj4ok2sPX82+t7UrOOuF9gH8p04wbIaIQkifa35PwmfUMOkSKB819xBl\n"
  "SOwF9G0o9caIpyRIpwCjP9WDS8j0BRX5kad2EjsCAwEAAaNTMFEwHQYDVR0OBBYE\n"
  "FCyGURPMFQ+Jw3orkJAlt2kK7V7iMB8GA1UdIwQYMBaAFCyGURPMFQ+Jw3orkJAl\n"
  "t2kK7V7iMA8GA1UdEwEB/wQFMAMBAf8wDQYJKoZIhvcNAQELBQADggEBAIAH4SmC\n"
  "yUZ6K5TkGCcMOOy9W/sOuN/Sz2TvNiXy0smWmzsHGbgmSqnqt/KKV/nv8YyPEleX\n"
  "JwN7Dt8ALlzKfB3/CquFac0elMqcTj3Y/27kVvx+E1st/Gym32D5AAnItLsG6Yc0\n"
  "EEEtqAFC0ls/kv+bomGsAQ3u7OKjVa4Q5VvDxb1Ni1c68kKB/OSNGTVkU0VI7yUj\n"
  "rJobxkQYptNs+LS+Z/rx0ob1PvSYiv1/AHgTuBQWJ7RKf+aDN/iOmWzrQa6llpve\n"
  "S7GykhR+8xE686qgN8xwOf/An/csQnt/9F7t7AXjkdsywLRBiFYSSbwIoRyXTIw8\n"
  "lx+/+brpb7UJedA=\n"
  "-----END CERTIFICATE-----\n";

static const char *server_cert_pem =
  "-----BEGIN CERTIFICATE-----\n"
  "MIIDrTCCApWgAwIBAgIUb7A7AMPrTUvHCvP9KrIgUzGBurQwDQYJKoZIhvcNAQEL\n"
  "BQAwWzELMAkGA1UEBhMCVVMxDTALBgNVBAgMBFRlc3QxDTALBgNVBAcMBFRlc3Qx\n"
  "FjAUBgNVBAoMDVpMaW5rIFRlc3QgQ0ExFjAUBgNVBAMMDVpMaW5rIFRlc3QgQ0Ew\n"
  "HhcNMjYwOTE1MDIzODI4WhcNNDYwOTEwMDIzODI4WjBUMQswCQYDVQQGEwJVUzEN\n"
  "MAsGA1UECAwEVGVzdDENMAsGA1UEBwwEVGVzdDETMBEGA1UECgwKWkxpbmsgVGVz\n"
  "dDESMBAGA1UEAwwJbG9jYWxob3N0MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIB\n"
  "CgKCAQEAwDUlAkS3oP5N8u9FACGHHhsivQZIjxKKz9Ji+Avg9lFuZ0iKUjpROHT/\n"
  "MgEtzGAXhSuHKkqyYwL7h5tC3f4BfKyxjbDk01nWz/8rjP95HoaMXo+i9a/bJ6lb\n"
  "febOUGmmktA8LzzxRiI6he7xH72zIxdw2R0ZKcutuiErfW/+AbqyDYDLUytMZgSt\n"
  "/ee8jwHQLNeRJpIR49hKFHkQNKtWilzQ7aTt4Cpln4J03EqujmGIXSASrf8cFV8x\n"
  "uwV6yExKtk0HmheoMIYlC6ei9LmABnOw5QSiwHwi+YCU04f2k24r/9a+95qgCg3y\n"
  "j33vgYEAzWjItCUzjBmLfBDzWAEqdwIDAQABo3AwbjAsBgNVHREEJTAjgglsb2Nh\n"
  "bGhvc3SHBH8AAAGHEAAAAAAAAAAAAAAAAAAAAAEwHQYDVR0OBBYEFBH5HRIxKpue\n"
  "/bFS8DDwjYnsQLS5MB8GA1UdIwQYMBaAFCyGURPMFQ+Jw3orkJAlt2kK7V7iMA0G\n"
  "CSqGSIb3DQEBCwUAA4IBAQCMoKcdWymrRxTMtbsm37MZp6JyglUbJ/OCzpKWiIY/\n"
  "niEf+S5U4JLBMvAkeu2j4KsT2CFXGj0l9O1KboEFHb/D9xblYR8nKVOsBQMvGEAM\n"
  "uWbcbjznLEO8di9ZJRbm3aG8w/0q0nndPQLBn+WGiZ/7vpGRRtI2o9b5fqfuUvM6\n"
  "bVjHCN3kZn3FqstBHpNlBGujeC/dUxgLEFXz7bznEe+c888RJFbJod32SKInFSJ6\n"
  "qxGOK+0h0566FVj5Rqa1KXKhHnu7JAYFn/9TRxCFieGyDogp2++oTkbNHe26IQv6\n"
  "LCHni8pSVfBqepP6LQVx/x++Z0th8uVVKKDm5wjWGjY0\n"
  "-----END CERTIFICATE-----\n";

static const char *server_key_pem =
  "-----BEGIN PRIVATE KEY-----\n"
  "MIIEvAIBADANBgkqhkiG9w0BAQEFAASCBKYwggSiAgEAAoIBAQDANSUCRLeg/k3y\n"
  "70UAIYceGyK9BkiPEorP0mL4C+D2UW5nSIpSOlE4dP8yAS3MYBeFK4cqSrJjAvuH\n"
  "m0Ld/gF8rLGNsOTTWdbP/yuM/3kehoxej6L1r9snqVt95s5QaaaS0DwvPPFGIjqF\n"
  "7vEfvbMjF3DZHRkpy626ISt9b/4BurINgMtTK0xmBK3957yPAdAs15EmkhHj2EoU\n"
  "eRA0q1aKXNDtpO3gKmWfgnTcSq6OYYhdIBKt/xwVXzG7BXrITEq2TQeaF6gwhiUL\n"
  "p6L0uYAGc7DlBKLAfCL5gJTTh/aTbiv/1r73mqAKDfKPfe+BgQDNaMi0JTOMGYt8\n"
  "EPNYASp3AgMBAAECggEAR59GFCtRFd/NYhpA5wSXWeOYtUEzJoUtTrXCBVY/1OmR\n"
  "L1F7oZpzi4slURfZXg/sk8YdjufYw0ZoPibf6uLs4O1lGDxzeEJA5q7aJqdIFdTj\n"
  "V5VEjzKhgoz8N9UayiIkXQ7VbnDSI2U7046vMTm6F/hzJ6RNLSLlsLcNgqeJylCG\n"
  "EtMqDwYznt1k4U0U94eO49w9rDX8MXCCWDnPtywdRZWUD0nTqyLbO0+I7AsWhnJv\n"
  "Hi4mhzi8DP85Ec64QWxDoGYBueoDSEsisSgUhMD6hHgklO3FLtPMto+Qxcde8WTe\n"
  "jU+tPzV3YZOB1twjlrrFh6Zl/IQt8kJy5WcI0zCQGQKBgQD38yVoI8UwWs6uRI+f\n"
  "15dHxw3JRbKRNl065BtD9u7VtmCD/3ppUXjYjTpTYhk0hdrkvzDL5U3dogeCc+gK\n"
  "bFRfFZHCTO5O7EIE3NDcK9ORPB0HdjLqwnx/Ec4D1usfP/oagJK0EIwDx5SW7btw\n"
  "4Le66/KoVXtf/v9Fnz3qwOd8YwKBgQDGcrFhALSxM9zt7WxMmGxtAHB2PoehubTt\n"
  "MjBS+fznoBjsF4A5uKExrkdKKliHkkqDs26aeX4AUmnJqaJm3u9S8x6arzsNU4Dy\n"
  "bC0Vq1+ag72UzVFbG8UWl+j//D6Hy6wBSfKuDBsxq5i30vdVaUFRrg372/D9NzPc\n"
  "F8e9wFXj3QKBgH7SZhKzIRwPhmGKfe/jBOTYwottU92EcgE6RVvpBNZY91rspL8T\n"
  "xfz1l5yos32y7XhM9neD7OTtCGxIPqp+KFWOIcTBNq81lrsH+uhynj9OAQcdBQQg\n"
  "wC76e2ZpWk/cmF9P3jmtsQAJ6E2egV5GApPgNXi2aGl8czM4NSJK0txDAoGAc2kM\n"
  "Y5+ndk71M6Iak8kpdZMF1J60/ockA7ZmiDs+q+5d0CAywF7x0BTM/QL3jZC0qTdX\n"
  "IZt6ffFv+IohGraYdKNTrx4tt6hSm6nx5mJOLWxkev+VSukxi9w483bdXthCZlV9\n"
  "P19nCVIEdRPKJ/AYvsn88/aLhpfuHxftYBtVWDkCgYBTxKJrLl6dIciPyPEqr65b\n"
  "LNqlNSN5pHm3WwKP8Zl4doP6eZV9yvP2eqqU/j/yu9xCPt2SEehI3EvLG3pO3NRB\n"
  "s8PjxWqGiemjYMr3d3Jr7QgaJ8lZ6mmd7NScwxcYexqvU4ZW1YS5OAFfkcWpxnYV\n"
  "1TQ99aQXlcSaltTPhzuqug==\n"
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

    if (zlink_set_option (socket, ZLINK_OPT_TLS_CERT, cert_path.c_str (), cert_path.size ()) != 0) {
        if (bench_debug_enabled ())
            std::cerr << "Failed to set ZLINK_OPT_TLS_CERT: " << zlink_strerror (zlink_errno ())
                      << std::endl;
        return false;
    }
    if (zlink_set_option (socket, ZLINK_OPT_TLS_KEY, key_path.c_str (), key_path.size ()) != 0) {
        if (bench_debug_enabled ())
            std::cerr << "Failed to set ZLINK_OPT_TLS_KEY: " << zlink_strerror (zlink_errno ())
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

    if (zlink_set_option (socket, ZLINK_OPT_TLS_CA, ca_path.c_str (), ca_path.size ()) != 0) {
        if (bench_debug_enabled ())
            std::cerr << "Failed to set ZLINK_OPT_TLS_CA: " << zlink_strerror (zlink_errno ())
                      << std::endl;
        return false;
    }
    if (zlink_set_option (socket, ZLINK_OPT_TLS_HOSTNAME, hostname, std::strlen (hostname)) != 0) {
        if (bench_debug_enabled ())
            std::cerr << "Failed to set ZLINK_OPT_TLS_HOSTNAME: " << zlink_strerror (zlink_errno ())
                      << std::endl;
        return false;
    }
    int trust_system = 0;
    if (zlink_set_option (socket, ZLINK_OPT_TLS_TRUST_SYSTEM, &trust_system, sizeof (trust_system))
        != 0) {
        if (bench_debug_enabled ())
            std::cerr << "Failed to set ZLINK_OPT_TLS_TRUST_SYSTEM: "
                      << zlink_strerror (zlink_errno ()) << std::endl;
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
            std::cerr << "get_option(ZLINK_OPT_LAST_ENDPOINT) failed: "
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

    apply_single_hwm (bind_socket_);
    apply_single_hwm (connect_socket_);

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
