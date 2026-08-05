#ifndef PERF_SINGLE_BENCH_COMMON_RUNTIME_HPP
#define PERF_SINGLE_BENCH_COMMON_RUNTIME_HPP

#include "bench_common.hpp"
#include "perf_single_monitor.hpp"
#include "perf_single_phase.hpp"

// --- Single runtime helpers ---

inline int resolve_single_ctx_nonnegative_env (const char *name_, int default_value_)
{
    if (!name_ || !*name_)
        return default_value_;

    const char *value = std::getenv (name_);
    if (!value || !*value)
        return default_value_;

    errno = 0;
    char *end = NULL;
    const long parsed = std::strtol (value, &end, 10);
    if (errno != 0 || end == value || *end != '\0')
        return default_value_;
    if (parsed < 0)
        return 0;
    if (parsed > INT_MAX)
        return INT_MAX;
    return static_cast<int> (parsed);
}

inline int resolve_single_ctx_auto_hwm_profile ()
{
    const char *value = std::getenv ("PERF_CTX_AUTO_HWM_PROFILE");
    if (!value || !*value)
        return ZLINK_CTX_AUTO_HWM_PROFILE_DFLT;

    if (std::strcmp (value, "compact") == 0)
        return ZLINK_AUTO_HWM_PROFILE_COMPACT;
    if (std::strcmp (value, "low_latency") == 0 || std::strcmp (value, "low-latency") == 0)
        return ZLINK_AUTO_HWM_PROFILE_LOW_LATENCY;
    if (std::strcmp (value, "balanced") == 0)
        return ZLINK_AUTO_HWM_PROFILE_BALANCED;
    if (std::strcmp (value, "throughput") == 0)
        return ZLINK_AUTO_HWM_PROFILE_THROUGHPUT;

    return ZLINK_CTX_AUTO_HWM_PROFILE_DFLT;
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

    const int blocky = resolve_single_ctx_nonnegative_env ("PERF_CTX_BLOCKY", 0) != 0 ? 1 : 0;
    set_ctx_opt_int (ctx_, ZLINK_CTX_OPT_BLOCKY, blocky, "ZLINK_CTX_OPT_BLOCKY");

    const int auto_hwm_enable = resolve_single_ctx_nonnegative_env ("PERF_CTX_AUTO_HWM_ENABLE",
                                                                    ZLINK_CTX_AUTO_HWM_ENABLE_DFLT)
                                    != 0
                                  ? 1
                                  : 0;
    set_ctx_opt_int (ctx_, ZLINK_CTX_OPT_AUTO_HWM_ENABLE, auto_hwm_enable,
                     "ZLINK_CTX_OPT_AUTO_HWM_ENABLE");

    set_ctx_opt_int (ctx_, ZLINK_CTX_OPT_AUTO_HWM_PROFILE, resolve_single_ctx_auto_hwm_profile (),
                     "ZLINK_CTX_OPT_AUTO_HWM_PROFILE");
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
    socket_guard_t (void *ctx_,
                    int type_,
                    zlink_socket_msg_handler_fn handler_,
                    void *userdata_ = NULL) :
        _socket (zlink_socket (ctx_, static_cast<zlink_socket_type_t> (type_)))
    {
        if (_socket && handler_
            && zlink_recv_handler (_socket, handler_, userdata_) != ZLINK_HANDLER_OK) {
            zlink_close (_socket);
            _socket = NULL;
        }
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

class poller_guard_t
{
  public:
    poller_guard_t () : _poller (zlink_poller_new ()) {}
    ~poller_guard_t ()
    {
        if (_poller)
            (void) zlink_poller_destroy (&_poller);
    }

    bool valid () const { return _poller != NULL; }
    void *get () const { return _poller; }

    bool add (void *socket_, void *user_data_, short events_)
    {
        return _poller && zlink_poller_add (_poller, socket_, user_data_, events_) == 0;
    }

    int wait (zlink_poller_event_t *event_, int timeout_ms_)
    {
        if (!_poller)
            return -1;
        return zlink_poller_wait (_poller, event_, 1, timeout_ms_, NULL);
    }

  private:
    poller_guard_t (const poller_guard_t &);
    poller_guard_t &operator= (const poller_guard_t &);

    void *_poller;
};

inline int resolve_single_send_timeout_ms ()
{
    return parse_positive_env ("PERF_SINGLE_SNDTIMEO_MS", 200);
}

inline int resolve_single_recv_timeout_ms ()
{
    return parse_positive_env ("PERF_SINGLE_RCVTIMEO_MS", 200);
}

inline int resolve_single_pubsub_recv_timeout_ms ()
{
    return parse_positive_env ("PERF_SINGLE_PUBSUB_RCVTIMEO_MS", resolve_single_recv_timeout_ms ());
}

inline int resolve_single_nonnegative_env (const char *env_name_, int default_value_)
{
    const char *value = std::getenv (env_name_);
    if (!value || !*value)
        return default_value_;

    char *end = NULL;
    const long parsed = std::strtol (value, &end, 10);
    if (end == value || *end != '\0')
        return default_value_;
    if (parsed < 0)
        return 0;
    if (parsed > INT_MAX)
        return INT_MAX;
    return static_cast<int> (parsed);
}

inline int resolve_single_pubsub_ready_settle_ms ()
{
    return resolve_single_nonnegative_env ("PERF_SINGLE_PUBSUB_READY_SETTLE_MS", 1000);
}

inline uint32_t next_single_metric_run_id ()
{
    static std::atomic<uint32_t> next_id (1);
    uint32_t run_id = next_id.fetch_add (1, std::memory_order_relaxed);
    if (run_id == 0)
        run_id = next_id.fetch_add (1, std::memory_order_relaxed);
    return run_id;
}

inline bool bench_single_manual_socket_overrides_allowed ()
{
    const char *value = std::getenv ("PERF_SINGLE_ALLOW_MANUAL_SOCKET_OVERRIDES");
    if (!value || !*value)
        value = std::getenv ("PERF_ALLOW_MANUAL_SOCKET_OVERRIDES");
    return value && std::strcmp (value, "1") == 0;
}

inline uint64_t parse_single_positive_u64_env (const char *name_, uint64_t default_value_)
{
    const char *value = std::getenv (name_);
    if (!value || !*value)
        return default_value_;
    if (std::strspn (value, "0123456789") != std::strlen (value))
        return default_value_;

    errno = 0;
    char *end = NULL;
    const unsigned long long parsed = std::strtoull (value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed == 0)
        return default_value_;
    return static_cast<uint64_t> (parsed);
}

inline uint64_t resolve_single_socket_hwm (bool send_)
{
    const uint64_t base_hwm = parse_single_positive_u64_env ("PERF_SINGLE_HWM", 0);
    return send_ ? parse_single_positive_u64_env ("PERF_SINGLE_SNDHWM", base_hwm)
                 : parse_single_positive_u64_env ("PERF_SINGLE_RCVHWM", base_hwm);
}

inline int parse_single_byte_size_token (const char *value_, int default_value_)
{
    if (!value_ || !*value_)
        return default_value_;

    errno = 0;
    char *end = NULL;
    const unsigned long long parsed = std::strtoull (value_, &end, 10);
    if (errno != 0 || end == value_)
        return default_value_;

    unsigned long long multiplier = 1;
    if (end && *end) {
        char suffix[3] = {0, 0, 0};
        size_t suffix_len = 0;
        while (end[suffix_len] != '\0' && suffix_len < 2) {
            suffix[suffix_len] =
              static_cast<char> (std::tolower (static_cast<unsigned char> (end[suffix_len])));
            ++suffix_len;
        }
        if (end[suffix_len] != '\0')
            return default_value_;

        if (suffix[0] == 'b' && suffix[1] == '\0')
            multiplier = 1;
        else if (suffix[0] == 'k' && (suffix[1] == '\0' || suffix[1] == 'b'))
            multiplier = 1024ULL;
        else if (suffix[0] == 'm' && (suffix[1] == '\0' || suffix[1] == 'b'))
            multiplier = 1024ULL * 1024ULL;
        else if (suffix[0] == 'g' && (suffix[1] == '\0' || suffix[1] == 'b'))
            multiplier = 1024ULL * 1024ULL * 1024ULL;
        else
            return default_value_;
    }

    const unsigned long long bytes = parsed * multiplier;
    if (bytes == 0)
        return default_value_;
    if (bytes > static_cast<unsigned long long> (INT_MAX))
        return INT_MAX;
    return static_cast<int> (bytes);
}

inline bool
wait_socket_event (void *socket_, short events_, long timeout_ms_, short *revents_out_ = NULL)
{
    if (!socket_)
        return false;

    zlink_pollitem_t item;
    item.socket = socket_;
    item.fd = 0;
    item.events = events_;
    item.revents = 0;
    const int rc = perf_socket_poll (&item, 1, timeout_ms_);
    if (rc < 0)
        return false;
    if (revents_out_)
        *revents_out_ = item.revents;
    return rc > 0 && (item.revents & events_) != 0;
}

inline int
poll_socket_event (void *socket_, short events_, long timeout_ms_, short *revents_out_ = NULL)
{
    if (!socket_)
        return -1;

    zlink_pollitem_t item;
    item.socket = socket_;
    item.fd = 0;
    item.events = events_;
    item.revents = 0;
    const int rc = perf_socket_poll (&item, 1, timeout_ms_);
    if (revents_out_)
        *revents_out_ = item.revents;
    return rc;
}

inline long remaining_timeout_ms (const std::chrono::steady_clock::time_point &deadline_,
                                  long minimum_ms_ = 1)
{
    const long long remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds> (
                                     deadline_ - std::chrono::steady_clock::now ())
                                     .count ();
    return remaining_ms > 0 ? static_cast<long> (remaining_ms)
                            : (minimum_ms_ > 0 ? minimum_ms_ : 0);
}

inline bool wait_socket_event_until (void *socket_,
                                     short events_,
                                     const std::chrono::steady_clock::time_point &deadline_,
                                     short *revents_out_ = NULL)
{
    if (!socket_)
        return false;
    if (std::chrono::steady_clock::now () >= deadline_)
        return false;
    return wait_socket_event (socket_, events_, remaining_timeout_ms (deadline_, 1), revents_out_);
}

inline void apply_single_hwm (void *socket_)
{
    if (!socket_ || !bench_single_manual_socket_overrides_allowed ())
        return;

    const uint64_t sndhwm = resolve_single_socket_hwm (true);
    const uint64_t rcvhwm = resolve_single_socket_hwm (false);
    if (sndhwm > 0)
        set_sockopt_u64 (socket_, ZLINK_OPT_SNDHWM, sndhwm, "ZLINK_OPT_SNDHWM");
    if (rcvhwm > 0)
        set_sockopt_u64 (socket_, ZLINK_OPT_RCVHWM, rcvhwm, "ZLINK_OPT_RCVHWM");
}

inline bool apply_single_auto_hwm_msg_unit (void *ctx_, size_t msg_size_)
{
    if (!ctx_ || msg_size_ == 0)
        return true;

    const uint64_t msg_unit =
      msg_size_ > static_cast<size_t> (UINT64_MAX) ? UINT64_MAX
                                                  : static_cast<uint64_t> (msg_size_);
    if (!set_ctx_opt_u64 (ctx_, ZLINK_CTX_OPT_AUTO_HWM_MSG_UNIT_BYTES, msg_unit,
                          "ZLINK_CTX_OPT_AUTO_HWM_MSG_UNIT_BYTES"))
        return false;
    return zlink_ctx_auto_hwm_recalculate (ctx_) == ZLINK_CONFIG_OK;
}

inline const char *single_socket_type_name (zlink_socket_type_t type_)
{
    switch (type_) {
        case ZLINK_SOCKET_PAIR:
            return "pair";
        case ZLINK_SOCKET_PUB:
            return "pub";
        case ZLINK_SOCKET_SUB:
            return "sub";
        case ZLINK_SOCKET_DEALER:
            return "dealer";
        case ZLINK_SOCKET_ROUTER:
            return "router";
        case ZLINK_SOCKET_XPUB:
            return "xpub";
        case ZLINK_SOCKET_XSUB:
            return "xsub";
        case ZLINK_SOCKET_STREAM:
            return "stream";
        default:
            return "unknown";
    }
}

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
        case 6:
            return "peer_queue";
        case 7:
            return "stream";
        default:
            return "none";
    }
}

inline bool single_auto_hwm_snapshot_visible (const zlink_monitor_status_t &snapshot_)
{
    return snapshot_.auto_hwm_applied_sndhwm_bytes > 0
           || snapshot_.auto_hwm_applied_rcvhwm_bytes > 0
           || snapshot_.auto_hwm_effective_message_bytes > 0
           || snapshot_.auto_hwm_socket_message_slots > 0;
}

inline void emit_single_socket_hwm_detail (void *socket_,
                                           const char *pattern_,
                                           const std::string &transport_,
                                           const char *component_,
                                           zlink_socket_type_t socket_type_,
                                           size_t msg_size_)
{
    if (!socket_ || !pattern_ || !component_)
        return;

    zlink_socket_monitor_open_options_t options;
    std::memset (&options, 0, sizeof (options));
    options.events = ZLINK_EVENT_CONNECTION_READY;
    void *monitor = zlink_socket_monitor_open (socket_, &options);
    if (!monitor)
        return;

    zlink_monitor_status_t snapshot;
    std::memset (&snapshot, 0, sizeof (snapshot));
    const zlink_config_result_t snapshot_rc = zlink_monitor_status (monitor, &snapshot);
    zlink_monitor_close (&monitor);
    if (snapshot_rc != ZLINK_CONFIG_OK || !single_auto_hwm_snapshot_visible (snapshot)) {
        return;
    }

    const char *socket_name = component_;
    const char *socket_type = single_socket_type_name (socket_type_);
    std::cout << "AUTO_HWM_DETAIL" << ",pattern=" << pattern_ << ",transport=" << transport_
              << ",component=" << component_ << ",msg_size=" << msg_size_ << ",owner=socket"
              << ",owner_id=0" << ",socket=" << socket_name << ",socket_type=" << socket_type
              << ",role=" << single_auto_hwm_role_name (snapshot.auto_hwm_role)
              << ",sndhwm=" << snapshot.auto_hwm_applied_sndhwm_bytes
              << ",rcvhwm=" << snapshot.auto_hwm_applied_rcvhwm_bytes
              << ",effective_message_bytes=" << snapshot.auto_hwm_effective_message_bytes
              << ",effective_sndbuf=" << snapshot.auto_hwm_effective_sndbuf
              << ",effective_rcvbuf=" << snapshot.auto_hwm_effective_rcvbuf
              << ",socket_message_slots=" << snapshot.auto_hwm_socket_message_slots << std::endl;
}

inline void apply_single_benchmark_socket_options (void *socket_, const std::string &)
{
    if (!socket_)
        return;
    const int linger_ms = 0;
    const int sndtimeo_ms = resolve_single_send_timeout_ms ();
    const int rcvtimeo_ms = resolve_single_recv_timeout_ms ();
    const int sndbuf = bench_single_manual_socket_overrides_allowed ()
                         ? parse_single_byte_size_token (std::getenv ("PERF_SINGLE_SNDBUF"), -1)
                         : -1;
    const int rcvbuf = bench_single_manual_socket_overrides_allowed ()
                         ? parse_single_byte_size_token (std::getenv ("PERF_SINGLE_RCVBUF"), -1)
                         : -1;
    set_sockopt_int (socket_, ZLINK_OPT_LINGER, linger_ms, "ZLINK_OPT_LINGER");
    if (sndbuf > 0)
        set_sockopt_int (socket_, ZLINK_OPT_SNDBUF, sndbuf, "ZLINK_OPT_SNDBUF");
    if (rcvbuf > 0)
        set_sockopt_int (socket_, ZLINK_OPT_RCVBUF, rcvbuf, "ZLINK_OPT_RCVBUF");
    set_sockopt_int (socket_, ZLINK_OPT_SNDTIMEO, sndtimeo_ms, "ZLINK_OPT_SNDTIMEO");
    set_sockopt_int (socket_, ZLINK_OPT_RCVTIMEO, rcvtimeo_ms, "ZLINK_OPT_RCVTIMEO");
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

inline std::string
bind_and_resolve_endpoint (void *socket_, const std::string &transport, const std::string &id)
{
    std::string endpoint = make_endpoint (transport, id);
    if (endpoint.empty ()) {
        std::cerr << "No endpoint available for transport " << transport << std::endl;
        return std::string ();
    }
    endpoint =
      perf_bind_endpoint_once (socket_, endpoint, transport, &perf_bind_socket_endpoint, true);
    if (endpoint.empty ())
        return std::string ();
    if (transport == "inproc")
        return endpoint;
    if (bench_debug_enabled ()) {
        std::cerr << "Resolved endpoint (" << transport << "): " << endpoint << std::endl;
    }
    return endpoint;
}

inline bool transport_available (const std::string &transport)
{
    if (transport == "tcp" || transport == "inproc")
        return true;
    if (transport == "ipc")
        return zlink_has ("ipc") != 0;
    if (transport == "tls")
        return zlink_has ("tls") != 0;
    if (transport == "ws")
        return zlink_has ("ws") != 0;
    if (transport == "wss")
        return zlink_has ("wss") != 0;
    return false;
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

    void *bind_monitor =
      open_configured_socket_monitor (bind_socket_, ZLINK_EVENT_CONNECTION_READY);
    if (!bind_monitor)
        return false;
    void *connect_monitor =
      open_configured_socket_monitor (connect_socket_, ZLINK_EVENT_CONNECTION_READY);
    if (!connect_monitor) {
        zlink_monitor_close (&bind_monitor);
        return false;
    }

    if (!connect_checked (connect_socket_, endpoint)) {
        zlink_monitor_close (&connect_monitor);
        zlink_monitor_close (&bind_monitor);
        return false;
    }

    apply_single_benchmark_socket_options (bind_socket_, transport_);
    apply_single_benchmark_socket_options (connect_socket_, transport_);

    const int timeout_ms = parse_positive_env ("PERF_CONNECT_READY_TIMEOUT_MS", 1000);
    const bool bind_ready =
      wait_for_socket_monitor_event (bind_monitor, ZLINK_EVENT_CONNECTION_READY, timeout_ms);
    const bool connect_ready =
      wait_for_socket_monitor_event (connect_monitor, ZLINK_EVENT_CONNECTION_READY, timeout_ms);
    zlink_monitor_close (&connect_monitor);
    zlink_monitor_close (&bind_monitor);
    if (bench_debug_enabled () && !(bind_ready && connect_ready)) {
        std::cerr << "[perf-single] setup_connected_pair failed:" << " bind_ready="
                  << (bind_ready ? 1 : 0) << " connect_ready=" << (connect_ready ? 1 : 0)
                  << std::endl;
    }
    return bind_ready && connect_ready;
}

template <typename RunFn>
inline int run_standard_bench_main (int argc_, char **argv_, const char *pattern_, RunFn run_)
{
    if (argc_ < 4)
        return 1;
    if (!single_perf_validate_recv_mode_for_pattern (pattern_))
        return 1;
    std::string lib_name = argv_[1];
    std::string transport = argv_[2];
    size_t size = std::stoul (argv_[3]);
    run_ (transport, size, lib_name);
    return 0;
}

#endif
