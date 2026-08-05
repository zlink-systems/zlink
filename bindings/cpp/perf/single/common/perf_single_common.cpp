#include "perf_single_common.hpp"

#include <cerrno>

namespace perf
{
namespace single
{

ctx_guard_t::ctx_guard_t () : _ctx ()
{
    if (_ctx.valid ())
        apply_ctx_options (_ctx);
}

ctx_guard_t::~ctx_guard_t ()
{
    if (_ctx.valid ())
        (void) _ctx.shutdown ();
}

int parse_positive_env (const char *name_, int default_value_)
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

uint64_t parse_positive_uint64_env (const char *name_, uint64_t default_value_)
{
    if (!name_)
        return default_value_;

    const char *env = std::getenv (name_);
    if (!env || !*env || *env == '-')
        return default_value_;

    errno = 0;
    char *end = NULL;
    const unsigned long long parsed = std::strtoull (env, &end, 10);
    if (errno != 0 || end == env || *end != '\0' || parsed == 0)
        return default_value_;
    return static_cast<uint64_t> (parsed);
}

int parse_nonnegative_env (const char *name_, int default_value_)
{
    if (!name_)
        return default_value_;

    const char *env = std::getenv (name_);
    if (!env || !*env)
        return default_value_;

    errno = 0;
    char *end = NULL;
    const long parsed = std::strtol (env, &end, 10);
    if (errno != 0 || end == env || *end != '\0')
        return default_value_;

    if (parsed < 0)
        return 0;
    if (parsed > INT_MAX)
        return INT_MAX;
    return static_cast<int> (parsed);
}

int resolve_single_duration_seconds ()
{
    return parse_positive_env ("PERF_SINGLE_DURATION_SECONDS", 5);
}

size_t resolve_single_latency_sample_cap ()
{
    const int cap = parse_positive_env ("PERF_SINGLE_LATENCY_SAMPLE_CAP", 200000);
    return cap > 0 ? static_cast<size_t> (cap) : static_cast<size_t> (200000);
}

int resolve_single_send_timeout_ms ()
{
    return parse_positive_env ("PERF_SINGLE_SNDTIMEO_MS", 200);
}

int resolve_single_recv_timeout_ms ()
{
    return parse_positive_env ("PERF_SINGLE_RCVTIMEO_MS", 200);
}

int resolve_single_pubsub_recv_timeout_ms ()
{
    return parse_positive_env ("PERF_SINGLE_PUBSUB_RCVTIMEO_MS", resolve_single_recv_timeout_ms ());
}

int resolve_single_pubsub_ready_settle_ms ()
{
    return parse_positive_env ("PERF_SINGLE_PUBSUB_READY_SETTLE_MS", 1000);
}

int resolve_single_connect_ready_timeout_ms ()
{
    return parse_positive_env ("PERF_CONNECT_READY_TIMEOUT_MS", 3000);
}

uint64_t resolve_single_socket_hwm (bool send_)
{
    const uint64_t base_hwm = parse_positive_uint64_env ("PERF_SINGLE_HWM", 1000);
    return send_ ? parse_positive_uint64_env ("PERF_SINGLE_SNDHWM", base_hwm)
                 : parse_positive_uint64_env ("PERF_SINGLE_RCVHWM", base_hwm);
}

zlink::auto_hwm_profile resolve_single_ctx_auto_hwm_profile ()
{
    const char *value = std::getenv ("PERF_CTX_AUTO_HWM_PROFILE");
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

bool single_manual_socket_overrides_enabled ()
{
    return parse_positive_env ("PERF_SINGLE_ALLOW_MANUAL_SOCKET_OVERRIDES", 0) > 0
           || parse_positive_env ("PERF_ALLOW_MANUAL_SOCKET_OVERRIDES", 0) > 0;
}

bool bench_debug_enabled ()
{
    static const bool enabled = std::getenv ("PERF_DEBUG") != NULL;
    return enabled;
}

void apply_ctx_options (zlink::context_t &ctx_)
{
    zlink::context_options_t options = ctx_.options ();
    const int io_threads = parse_positive_env ("PERF_IO_THREADS", 1);
    if (io_threads > 0)
        (void) options.io_threads (zlink::io_thread_count_t::value (io_threads));

    const int max_sockets = parse_positive_env ("PERF_MAX_SOCKETS", 0);
    if (max_sockets > 0)
        (void) options.max_sockets (zlink::socket_count_t::value (max_sockets));

    (void) options.blocky (parse_nonnegative_env ("PERF_CTX_BLOCKY", 0) != 0);
    (void) options.auto_hwm_enabled (parse_nonnegative_env ("PERF_CTX_AUTO_HWM_ENABLE", 1) != 0);
    (void) options.auto_hwm_profile (resolve_single_ctx_auto_hwm_profile ());
}

bool set_sockopt_int (perf_socket_t &socket_,
                      perf::options::socket_option_key_t<int> option_,
                      int value_,
                      const char *name_)
{
    const int rc = socket_.set (option_, value_);
    if (rc != 0 && bench_debug_enabled ()) {
        std::cerr << "setsockopt(" << (name_ ? name_ : "?")
                  << ") failed: " << zlink::last_error ().what () << std::endl;
    }
    return rc == 0;
}

bool set_sockopt_hwm (perf_socket_t &socket_,
                      perf::options::socket_option_key_t<uint64_t> option_,
                      uint64_t value_,
                      const char *name_)
{
    const int rc = socket_.set (option_, value_);
    if (rc != 0 && bench_debug_enabled ()) {
        std::cerr << "setsockopt(" << (name_ ? name_ : "?")
                  << ") failed: " << zlink::last_error ().what () << std::endl;
    }
    return rc == 0;
}

void apply_single_hwm (perf_socket_t &socket_)
{
    if (!single_manual_socket_overrides_enabled ())
        return;
    const uint64_t sndhwm = resolve_single_socket_hwm (true);
    const uint64_t rcvhwm = resolve_single_socket_hwm (false);
    (void) set_sockopt_hwm (socket_, perf::options::socket_options::sndhwm, sndhwm, "sndhwm");
    (void) set_sockopt_hwm (socket_, perf::options::socket_options::rcvhwm, rcvhwm, "rcvhwm");
}

bool apply_single_auto_hwm_msg_unit (ctx_guard_t &ctx_, size_t msg_size_)
{
    if (msg_size_ == 0)
        return true;
    try {
        zlink::context_options_t options = ctx_.ctx ().options ();
        options.auto_hwm_msg_unit_bytes (
          zlink::byte_count_t::bytes (static_cast<uint64_t> (msg_size_)));
        return true;
    }
    catch (const zlink::config_error_t &err) {
        errno = err.internal_errno ();
        return false;
    }
}

bool recalculate_single_auto_hwm (ctx_guard_t &ctx_)
{
    try {
        ctx_.ctx ().recalculate_auto_hwm ();
        return true;
    }
    catch (const zlink::config_error_t &err) {
        errno = err.internal_errno ();
        return false;
    }
}

void apply_single_benchmark_socket_options (perf_socket_t &socket_, const std::string &)
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

std::string make_endpoint (const std::string &transport, const std::string &id)
{
    if (transport == "inproc")
        return std::string ("inproc://") + id;
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

std::string make_fixed_endpoint (const std::string &transport, int port)
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

std::string bind_and_resolve_endpoint (perf_socket_t &socket_,
                                       const std::string &transport,
                                       const std::string &id)
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
        std::string last_endpoint;
        if (socket_.get (perf::options::socket_options::last_endpoint, last_endpoint) != 0)
            return std::string ();
        endpoint = last_endpoint;

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

bool transport_available (const std::string &transport)
{
    if (transport == "tcp" || transport == "inproc")
        return true;
    if (transport == "ipc")
        return zlink::has ("ipc");
    if (transport == "tls")
        return zlink::has ("tls");
    if (transport == "ws")
        return zlink::has ("ws");
    if (transport == "wss")
        return zlink::has ("wss");
    return false;
}

bool setup_connected_pair (perf_socket_t &bind_socket_,
                           perf_socket_t &connect_socket_,
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
      bind_socket_.monitor_open (zlink::monitor_event::connection_ready);
    zlink::socket_monitor_t connect_monitor =
      connect_socket_.monitor_open (zlink::monitor_event::connection_ready);
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

} // namespace single
} // namespace perf
