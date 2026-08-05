#ifndef BENCH_COMMON_HPP
#define BENCH_COMMON_HPP

#include <chrono>
#include <algorithm>
#include <vector>
#include <set>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <iostream>
#include <iomanip>
#include <thread>
#include <atomic>
#include <cstring>
#include <climits>
#include <zmq.h>

// --- Configuration ---
static const std::vector<size_t> MSG_SIZES = {64, 256, 1024, 65536, 131072, 262144};
static const std::vector<std::string> TRANSPORTS = {"tcp", "inproc", "ipc"};
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

inline void print_result (const std::string &lib_type,
                          const std::string &pattern,
                          const std::string &transport,
                          size_t size,
                          double throughput,
                          double latency)
{
    const bool is_echo_pattern = pattern == "MULTI_DEALER_DEALER"
                                 || pattern == "MULTI_DEALER_ROUTER"
                                 || pattern == "MULTI_ROUTER_ROUTER";
    const double direction_factor = is_echo_pattern ? 2.0 : 1.0;
    const double bandwidth_mb_s =
      (throughput * static_cast<double> (size) * direction_factor) / 1000000.0;
    std::cout << "RESULT," << lib_type << "," << pattern << "," << transport << "," << size
              << ",throughput," << std::fixed << std::setprecision (2) << throughput << std::endl;
    std::cout << "RESULT," << lib_type << "," << pattern << "," << transport << "," << size
              << ",bandwidth," << std::fixed << std::setprecision (2) << bandwidth_mb_s
              << std::endl;
    std::cout << "RESULT," << lib_type << "," << pattern << "," << transport << "," << size
              << ",latency," << std::fixed << std::setprecision (2) << latency << std::endl;
}

inline bool bench_debug_enabled ()
{
    static const bool enabled = std::getenv ("BENCH_DEBUG") != NULL;
    return enabled;
}

inline bool bench_blocking_send_enabled ()
{
    static int enabled = -1;
    if (enabled >= 0)
        return enabled != 0;

    const char *explicit_mode = std::getenv ("BENCH_MULTI_BLOCKING_SEND");
    if (explicit_mode && std::strcmp (explicit_mode, "0") != 0) {
        enabled = 1;
        return true;
    }

    const char *profile = std::getenv ("BENCH_PROFILE");
    if (profile && std::strcmp (profile, "realistic") == 0) {
        enabled = 1;
        return true;
    }

    enabled = 0;
    return false;
}

inline int bench_send_flags (int base_flags = 0)
{
    return bench_blocking_send_enabled () ? base_flags : (base_flags | ZMQ_DONTWAIT);
}

inline int bench_io_threads ()
{
    static int threads = -1;
    if (threads >= 0)
        return threads;

    const char *env = std::getenv ("BENCH_IO_THREADS");
    if (!env || !*env) {
        threads = 0;
        return threads;
    }

    const int val = std::atoi (env);
    threads = val > 0 ? val : 0;
    return threads;
}

inline void apply_io_threads (void *ctx_)
{
    const int threads = bench_io_threads ();
    if (threads <= 0)
        return;

    const int rc = zmq_ctx_set (ctx_, ZMQ_IO_THREADS, threads);
    if (rc != 0 && bench_debug_enabled ()) {
        std::cerr << "zmq_ctx_set(ZMQ_IO_THREADS) failed: " << zmq_strerror (zmq_errno ())
                  << std::endl;
    }
}

class ctx_guard_t
{
  public:
    ctx_guard_t () : _ctx (zmq_ctx_new ())
    {
        if (_ctx)
            apply_io_threads (_ctx);
    }

    ~ctx_guard_t ()
    {
        if (_ctx)
            zmq_ctx_term (_ctx);
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

    socket_guard_t (void *ctx_, int type_) : _socket (zmq_socket (ctx_, type_)) {}

    ~socket_guard_t ()
    {
        if (_socket)
            zmq_close (_socket);
    }

    void *get () const { return _socket; }
    bool valid () const { return _socket != NULL; }
    operator void * () const { return _socket; }

  private:
    socket_guard_t (const socket_guard_t &);
    socket_guard_t &operator= (const socket_guard_t &);

    void *_socket;
};

struct connect_monitor_t
{
    void *owner;
    void *monitor;
    connect_monitor_t () : owner (NULL), monitor (NULL) {}
};

inline int bench_monitor_hwm ()
{
    static int monitor_hwm = -1;
    if (monitor_hwm >= 0)
        return monitor_hwm;

    const char *env = std::getenv ("BENCH_MULTI_MONITOR_HWM");
    if (!env || !*env) {
        monitor_hwm = 200000;
        return monitor_hwm;
    }

    errno = 0;
    char *end = NULL;
    const long parsed = std::strtol (env, &end, 10);
    if (errno != 0 || end == env || parsed <= 0) {
        monitor_hwm = 0;
        return monitor_hwm;
    }

    monitor_hwm = static_cast<int> (parsed);
    return monitor_hwm;
}

inline bool monitor_event_is_ready (uint16_t event_)
{
#ifdef ZMQ_EVENT_HANDSHAKE_SUCCEEDED
    if (event_ == ZMQ_EVENT_HANDSHAKE_SUCCEEDED)
        return true;
#endif
#ifdef ZMQ_EVENT_ACCEPTED
    if (event_ == ZMQ_EVENT_ACCEPTED)
        return true;
#endif
    return event_ == ZMQ_EVENT_CONNECTED;
}

inline bool open_connect_monitor (
  void *ctx_, void *socket_, const std::string &tag_, size_t idx_, connect_monitor_t &out_)
{
    static std::atomic<unsigned long> seq (0);
    char monitor_ep[160];
    const unsigned long id = seq.fetch_add (1, std::memory_order_relaxed);
    std::snprintf (monitor_ep, sizeof (monitor_ep), "inproc://multi-mon-%s-%zu-%lu", tag_.c_str (),
                   idx_, id);

    int events = ZMQ_EVENT_CONNECTED;
#ifdef ZMQ_EVENT_HANDSHAKE_SUCCEEDED
    events |= ZMQ_EVENT_HANDSHAKE_SUCCEEDED;
#endif
#ifdef ZMQ_EVENT_ACCEPTED
    events |= ZMQ_EVENT_ACCEPTED;
#endif

    if (zmq_socket_monitor (socket_, monitor_ep, events) != 0)
        return false;

    void *monitor = zmq_socket (ctx_, ZMQ_PAIR);
    if (!monitor) {
        zmq_socket_monitor (socket_, NULL, 0);
        return false;
    }

    const int linger_ms = 0;
    zmq_setsockopt (monitor, ZMQ_LINGER, &linger_ms, sizeof (linger_ms));
    const int monitor_hwm = bench_monitor_hwm ();
    if (monitor_hwm > 0) {
        zmq_setsockopt (monitor, ZMQ_RCVHWM, &monitor_hwm, sizeof (monitor_hwm));
        zmq_setsockopt (monitor, ZMQ_SNDHWM, &monitor_hwm, sizeof (monitor_hwm));
    }
    if (zmq_connect (monitor, monitor_ep) != 0) {
        zmq_close (monitor);
        zmq_socket_monitor (socket_, NULL, 0);
        return false;
    }

    out_.owner = socket_;
    out_.monitor = monitor;
    return true;
}

inline int poll_connect_ready_count (connect_monitor_t &monitor_);

inline bool wait_connect_ready (connect_monitor_t &monitor_, int timeout_ms_)
{
    if (!monitor_.monitor)
        return false;

    const auto deadline =
      std::chrono::steady_clock::now () + std::chrono::milliseconds (std::max (0, timeout_ms_));
    while (true) {
        const auto now = std::chrono::steady_clock::now ();
        if (now >= deadline)
            return false;

        const long remain_ms =
          std::chrono::duration_cast<std::chrono::milliseconds> (deadline - now).count ();
        zmq_pollitem_t item[] = {{monitor_.monitor, 0, ZMQ_POLLIN, 0}};
        const int rc = zmq_poll (item, 1, remain_ms > 0 ? remain_ms : 0);
        if (rc < 0) {
            if (zmq_errno () == EINTR)
                continue;
            return false;
        }
        if (rc == 0 || (item[0].revents & ZMQ_POLLIN) == 0)
            continue;

        if (poll_connect_ready_count (monitor_) > 0)
            return true;
    }
}

inline int poll_connect_ready_count (connect_monitor_t &monitor_)
{
    if (!monitor_.monitor)
        return 0;

    int ready = 0;
    for (;;) {
        zmq_msg_t part1;
        zmq_msg_t part2;
        zmq_msg_init (&part1);
        zmq_msg_init (&part2);

        const int n = zmq_msg_recv (&part1, monitor_.monitor, ZMQ_DONTWAIT);
        if (n < 0) {
            zmq_msg_close (&part1);
            zmq_msg_close (&part2);
            break;
        }

        (void) zmq_msg_recv (&part2, monitor_.monitor, ZMQ_DONTWAIT);

        uint16_t event = 0;
        if (zmq_msg_size (&part1) >= sizeof (uint16_t)) {
            std::memcpy (&event, zmq_msg_data (&part1), sizeof (uint16_t));
        }

        zmq_msg_close (&part1);
        zmq_msg_close (&part2);

        if (monitor_event_is_ready (event))
            ++ready;
    }
    return ready;
}

inline bool
wait_connect_ready_count (connect_monitor_t &monitor_, size_t expected_ready_, int timeout_ms_)
{
    if (expected_ready_ == 0)
        return true;

    size_t ready = 0;
    const auto deadline =
      std::chrono::steady_clock::now () + std::chrono::milliseconds (std::max (0, timeout_ms_));
    while (std::chrono::steady_clock::now () < deadline && ready < expected_ready_) {
        ready += static_cast<size_t> (poll_connect_ready_count (monitor_));
        if (ready >= expected_ready_)
            return true;
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    return ready >= expected_ready_;
}

inline bool
wait_connect_ready_unique (connect_monitor_t &monitor_, size_t expected_ready_, int timeout_ms_)
{
    if (!monitor_.monitor || expected_ready_ == 0)
        return true;

    std::set<std::string> peers;
    const auto deadline =
      std::chrono::steady_clock::now () + std::chrono::milliseconds (std::max (0, timeout_ms_));
    while (std::chrono::steady_clock::now () < deadline && peers.size () < expected_ready_) {
        bool progress = false;
        for (;;) {
            zmq_msg_t part1;
            zmq_msg_t part2;
            zmq_msg_init (&part1);
            zmq_msg_init (&part2);

            const int n = zmq_msg_recv (&part1, monitor_.monitor, ZMQ_DONTWAIT);
            if (n < 0) {
                zmq_msg_close (&part1);
                zmq_msg_close (&part2);
                break;
            }

            (void) zmq_msg_recv (&part2, monitor_.monitor, ZMQ_DONTWAIT);

            uint16_t event = 0;
            int32_t value = 0;
            if (zmq_msg_size (&part1) >= sizeof (uint16_t)) {
                std::memcpy (&event, zmq_msg_data (&part1), sizeof (uint16_t));
            }
            if (zmq_msg_size (&part1) >= sizeof (uint16_t) + sizeof (int32_t)) {
                std::memcpy (&value,
                             static_cast<const char *> (zmq_msg_data (&part1)) + sizeof (uint16_t),
                             sizeof (int32_t));
            }
            if (monitor_event_is_ready (event)) {
                const char *data = static_cast<const char *> (zmq_msg_data (&part2));
                const size_t size = zmq_msg_size (&part2);
                if (value != 0) {
                    char fallback[64];
                    std::snprintf (fallback, sizeof (fallback), "fd:%d", value);
                    peers.insert (std::string (fallback));
                } else if (data && size > 0) {
                    peers.insert (std::string (data, size));
                } else {
                    char fallback[32];
                    std::snprintf (fallback, sizeof (fallback), "ev:%u",
                                   static_cast<unsigned> (event));
                    peers.insert (std::string (fallback));
                }
            }

            zmq_msg_close (&part1);
            zmq_msg_close (&part2);
            progress = true;
        }
        if (peers.size () >= expected_ready_)
            return true;
        if (!progress)
            std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    const bool ok = peers.size () >= expected_ready_;
    if (!ok && bench_debug_enabled ()) {
        std::cerr << "wait_connect_ready_unique timeout: peers=" << peers.size ()
                  << " expected=" << expected_ready_ << std::endl;
    }
    return ok;
}

inline bool wait_connect_ready_all (std::vector<connect_monitor_t> &monitors, int timeout_ms_)
{
    if (monitors.empty ())
        return true;

    std::vector<char> ready (monitors.size (), 0);
    size_t ready_count = 0;
    const auto deadline =
      std::chrono::steady_clock::now () + std::chrono::milliseconds (std::max (0, timeout_ms_));
    while (std::chrono::steady_clock::now () < deadline && ready_count < monitors.size ()) {
        for (size_t i = 0; i < monitors.size (); ++i) {
            if (ready[i])
                continue;
            if (poll_connect_ready_count (monitors[i]) > 0) {
                ready[i] = 1;
                ++ready_count;
            }
        }
        if (ready_count >= monitors.size ())
            return true;
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    return ready_count >= monitors.size ();
}

inline void close_connect_monitor (connect_monitor_t &monitor_)
{
    if (monitor_.owner)
        zmq_socket_monitor (monitor_.owner, NULL, 0);
    if (monitor_.monitor)
        zmq_close (monitor_.monitor);
    monitor_.owner = NULL;
    monitor_.monitor = NULL;
}

inline bool set_sockopt_int (void *socket_, int option_, int value_, const char *name_)
{
    const int rc = zmq_setsockopt (socket_, option_, &value_, sizeof (value_));
    if (rc != 0 && bench_debug_enabled ()) {
        std::cerr << "setsockopt(" << name_ << ") failed: " << zmq_strerror (zmq_errno ())
                  << std::endl;
    }

    if (bench_debug_enabled ()) {
        int out = 0;
        size_t out_size = sizeof (out);
        const int grc = zmq_getsockopt (socket_, option_, &out, &out_size);
        if (grc == 0) {
            std::cerr << "setsockopt(" << name_ << ") = " << out << std::endl;
        }
    }

    return rc == 0;
}

inline void apply_benchmark_hwm (void *socket_, int inflight_)
{
    if (inflight_ <= 0)
        return;

    set_sockopt_int (socket_, ZMQ_SNDHWM, inflight_, "ZMQ_SNDHWM");
    set_sockopt_int (socket_, ZMQ_RCVHWM, inflight_, "ZMQ_RCVHWM");
}

inline int bench_timeout_ms_from_env (const char *name_, int default_ms_)
{
    if (!name_ || !*name_)
        return default_ms_;

    const char *value = std::getenv (name_);
    if (!value || !*value)
        return default_ms_;

    errno = 0;
    char *end = NULL;
    const long parsed = std::strtol (value, &end, 10);
    if (errno != 0 || end == value || parsed <= 0)
        return default_ms_;
    if (parsed > INT_MAX)
        return INT_MAX;
    return static_cast<int> (parsed);
}

inline void apply_debug_timeouts (void *socket_, const std::string &transport)
{
    if (transport == "inproc")
        return;

    const int sndtimeo_ms = bench_timeout_ms_from_env ("BENCH_MULTI_SNDTIMEO_MS", 5000);
    const int rcvtimeo_ms = bench_timeout_ms_from_env ("BENCH_MULTI_RCVTIMEO_MS", 5000);
    set_sockopt_int (socket_, ZMQ_SNDTIMEO, sndtimeo_ms, "ZMQ_SNDTIMEO");
    set_sockopt_int (socket_, ZMQ_RCVTIMEO, rcvtimeo_ms, "ZMQ_RCVTIMEO");
}

inline std::string transport_from_endpoint (const std::string &endpoint)
{
    const std::string::size_type pos = endpoint.find ("://");
    if (pos == std::string::npos)
        return std::string ();
    return endpoint.substr (0, pos);
}

inline std::string make_endpoint (const std::string &transport, const std::string &id)
{
    if (transport == "inproc")
        return "inproc://" + id;
    if (transport == "ipc")
        return "ipc://*";
    if (transport == "ws")
        return "ws://127.0.0.1:*";
    return "tcp://127.0.0.1:*";
}

inline std::string
bind_and_resolve_endpoint (void *socket_, const std::string &transport, const std::string &id)
{
    std::string endpoint = make_endpoint (transport, id);
    if (zmq_bind (socket_, endpoint.c_str ()) != 0) {
        std::cerr << "bind failed for " << endpoint << ": " << zmq_strerror (zmq_errno ())
                  << std::endl;
        return std::string ();
    }

    if (transport != "inproc") {
        char last_endpoint[MAX_SOCKET_STRING] = "";
        size_t size = sizeof (last_endpoint);
        if (zmq_getsockopt (socket_, ZMQ_LAST_ENDPOINT, last_endpoint, &size) != 0) {
            std::cerr << "getsockopt(ZMQ_LAST_ENDPOINT) failed: " << zmq_strerror (zmq_errno ())
                      << std::endl;
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
                if (pos != std::string::npos)
                    endpoint.replace (pos, tcp_ipv6_any.size (), "://127.0.0.1:");
            }
        }

        if (bench_debug_enabled ()) {
            std::cerr << "Resolved endpoint (" << transport << "): " << endpoint << std::endl;
        }
    }
    apply_debug_timeouts (socket_, transport);

    return endpoint;
}

inline bool transport_available (const std::string &transport)
{
    if (transport == "ipc")
        return zmq_has ("ipc") != 0;
    return true;
}

inline void settle ()
{
    std::this_thread::sleep_for (std::chrono::milliseconds (SETTLE_TIME_MS));
}

inline bool connect_checked (void *socket_, const std::string &endpoint)
{
    if (zmq_connect (socket_, endpoint.c_str ()) != 0) {
        std::cerr << "connect failed for " << endpoint << ": " << zmq_strerror (zmq_errno ())
                  << std::endl;
        return false;
    }
    apply_debug_timeouts (socket_, transport_from_endpoint (endpoint));

    if (bench_debug_enabled ())
        std::cerr << "Connected to " << endpoint << std::endl;
    return true;
}

inline bool setup_connected_pair (void *bind_socket_,
                                  void *connect_socket_,
                                  const std::string &transport_,
                                  const std::string &id_)
{
    std::string endpoint = bind_and_resolve_endpoint (bind_socket_, transport_, id_);
    if (endpoint.empty ())
        return false;
    if (!connect_checked (connect_socket_, endpoint))
        return false;

    apply_debug_timeouts (bind_socket_, transport_);
    settle ();
    return true;
}

template <typename StepFn> inline void repeat_n (int count_, StepFn step_)
{
    for (int i = 0; i < count_; ++i)
        step_ ();
}

template <typename RoundTripFn>
inline double measure_roundtrip_latency_us (int roundtrip_count_, RoundTripFn roundtrip_)
{
    stopwatch_t sw;
    sw.start ();
    repeat_n (roundtrip_count_, roundtrip_);
    return (sw.elapsed_ms () * 1000.0) / (roundtrip_count_ * 2);
}

template <typename SendOneFn, typename RecvOneFn>
inline double
measure_throughput_msgs_per_sec (int msg_count_, SendOneFn send_one_, RecvOneFn recv_one_)
{
    std::thread receiver ([&] () { repeat_n (msg_count_, recv_one_); });
    stopwatch_t sw;
    sw.start ();
    repeat_n (msg_count_, send_one_);
    receiver.join ();

    const double elapsed_ms = sw.elapsed_ms ();
    return elapsed_ms > 0 ? (double) msg_count_ / (elapsed_ms / 1000.0) : 0.0;
}

inline int resolve_msg_count (size_t size)
{
    int count = (size <= 1024) ? 200000 : 20000;
    if (const char *env = std::getenv ("BENCH_MSG_COUNT")) {
        errno = 0;
        const long override = std::strtol (env, NULL, 10);
        if (errno == 0 && override > 0)
            count = static_cast<int> (override);
    }
    return count;
}

inline std::vector<size_t> resolve_bench_msg_sizes (size_t fallback_size)
{
    std::vector<size_t> sizes;
    if (const char *env = std::getenv ("BENCH_MSG_SIZES")) {
        const char *cur = env;
        while (*cur) {
            while (*cur == ',' || *cur == ' ' || *cur == '\t')
                ++cur;
            if (!*cur)
                break;

            errno = 0;
            char *end = NULL;
            const unsigned long parsed = std::strtoul (cur, &end, 10);
            if (errno == 0 && end != cur && parsed > 0)
                sizes.push_back (static_cast<size_t> (parsed));

            if (!end || end == cur)
                break;
            cur = end;
            while (*cur && *cur != ',')
                ++cur;
            if (*cur == ',')
                ++cur;
        }
    }

    if (sizes.empty ())
        sizes.push_back (fallback_size);
    return sizes;
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
    int count = resolve_msg_count (size);
    run_ (transport, size, count, lib_name);
    return 0;
}

#endif
