#ifndef PERF_INFRA_HPP
#define PERF_INFRA_HPP

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <climits>
#include <fstream>
#include <iostream>
#include <string>
#include "perf_zlink_part_helpers.hpp"

#if !defined(_WIN32)
#include <dlfcn.h>
#else
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

// --- Configuration (shared constants) ---
static const size_t MAX_SOCKET_STRING = 256;

// ---------------------------------------------------------------------------
// parse_positive_env - parse a positive integer from an environment variable
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// stopwatch_t - simple steady-clock stopwatch
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// bench_debug_enabled - check PERF_DEBUG environment variable
// ---------------------------------------------------------------------------
inline bool bench_debug_enabled ()
{
    static const bool enabled = std::getenv ("PERF_DEBUG") != nullptr;
    return enabled;
}

// ---------------------------------------------------------------------------
// set_sockopt_int - set an integer socket option with debug logging
// ---------------------------------------------------------------------------
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

inline bool
set_sockopt_u64 (void *socket_, zlink_option_t option_, uint64_t value_, const char *name_)
{
    const int rc = zlink_set_option (socket_, option_, &value_, sizeof (value_));
    if (rc != 0 && bench_debug_enabled ()) {
        std::cerr << "setsockopt(" << name_ << ") failed: " << zlink_strerror (zlink_errno ())
                  << std::endl;
    }
    if (bench_debug_enabled ()) {
        uint64_t out = 0;
        size_t out_size = sizeof (out);
        const int grc = zlink_get_option (socket_, option_, &out, &out_size);
        if (grc == 0)
            std::cerr << "setsockopt(" << name_ << ") = " << out << std::endl;
    }
    return rc == 0;
}

inline bool set_ctx_opt_int (void *ctx_, zlink_ctx_option_t option_, int value_, const char *name_)
{
    const int rc = zlink_ctx_set (ctx_, option_, value_);
    if (rc != 0 && bench_debug_enabled ()) {
        std::cerr << "zlink_ctx_set(" << name_ << ") failed: " << zlink_strerror (zlink_errno ())
                  << std::endl;
    }
    return rc == 0;
}

inline bool
set_ctx_opt_u64 (void *ctx_, zlink_ctx_option_t option_, uint64_t value_, const char *name_)
{
    const int rc = zlink_ctx_set_data (ctx_, option_, &value_, sizeof (value_));
    if (rc != 0 && bench_debug_enabled ()) {
        std::cerr << "zlink_ctx_set_data(" << name_
                  << ") failed: " << zlink_strerror (zlink_errno ()) << std::endl;
    }
    return rc == 0;
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

// ---------------------------------------------------------------------------
// make_endpoint / make_fixed_endpoint - construct transport endpoints
// ---------------------------------------------------------------------------
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
    const std::string host = (transport == "tls" || transport == "wss") ? "localhost" : "127.0.0.1";
    const std::string port_str = std::to_string (port);
    if (transport == "ws")
        return "ws://" + host + ":" + port_str;
    if (transport == "wss")
        return "wss://" + host + ":" + port_str;
    if (transport == "tls")
        return "tls://" + host + ":" + port_str;
    return "tcp://" + host + ":" + port_str;
}

inline bool perf_supports_service_transport (const std::string &transport)
{
    return transport == "tcp" || transport == "tls" || transport == "ws" || transport == "wss"
           || transport == "ipc" || transport == "inproc";
}

inline bool perf_is_tls_transport (const std::string &transport)
{
    return transport == "tls" || transport == "wss";
}

inline const char *perf_loopback_host_for_transport (const std::string &transport)
{
    return perf_is_tls_transport (transport) ? "localhost" : "127.0.0.1";
}

inline std::string perf_normalize_bind_endpoint_host (const std::string &endpoint,
                                                      const std::string &transport)
{
    std::string normalized = endpoint;
    const std::string any_v4 = "://0.0.0.0:";
    const std::string any_v6 = "://[::]:";
    const std::string loopback_v4 = "://127.0.0.1:";
    const std::string host =
      std::string ("://") + perf_loopback_host_for_transport (transport) + ":";
    size_t pos = normalized.find (any_v4);
    if (pos != std::string::npos)
        normalized.replace (pos, any_v4.size (), host);
    pos = normalized.find (any_v6);
    if (pos != std::string::npos)
        normalized.replace (pos, any_v6.size (), host);
    if (perf_is_tls_transport (transport)) {
        pos = normalized.find (loopback_v4);
        if (pos != std::string::npos)
            normalized.replace (pos, loopback_v4.size (), host);
    }
    if ((transport == "ws" || transport == "wss") && normalized.size () > 1
        && normalized[normalized.size () - 1] == '/') {
        normalized.erase (normalized.size () - 1);
    }
    return normalized;
}

inline void perf_close_multipart (zlink_msg_t *parts_, size_t part_count_)
{
    if (!parts_)
        return;
    for (size_t i = 0; i < part_count_; ++i)
        zlink_msg_close (&parts_[i]);
}

typedef int (*perf_bind_endpoint_fn_t) (void *, const char *);

inline int perf_bind_socket_endpoint (void *socket_, const char *endpoint_)
{
    return zlink_bind (socket_, endpoint_);
}

inline std::string perf_bind_endpoint_once (void *target_,
                                            const std::string &endpoint_,
                                            const std::string &transport_,
                                            perf_bind_endpoint_fn_t bind_fn_,
                                            bool resolve_last_endpoint_)
{
    if (!target_ || !bind_fn_ || endpoint_.empty ())
        return std::string ();

    if (bind_fn_ (target_, endpoint_.c_str ()) != 0) {
        std::cerr << "bind failed for " << endpoint_ << ": " << zlink_strerror (zlink_errno ())
                  << std::endl;
        return std::string ();
    }

    std::string resolved = endpoint_;
    if (resolve_last_endpoint_) {
        char last_endpoint[MAX_SOCKET_STRING] = "";
        size_t size = sizeof (last_endpoint);
        if (zlink_get_option (target_, ZLINK_OPT_LAST_ENDPOINT, last_endpoint, &size) == 0) {
            resolved.assign (last_endpoint);
        }
    }

    return perf_normalize_bind_endpoint_host (resolved, transport_);
}

inline std::string perf_bind_fixed_endpoint_range (void *target_,
                                                   const std::string &transport_,
                                                   int base_port_,
                                                   int attempts_,
                                                   perf_bind_endpoint_fn_t bind_fn_,
                                                   bool resolve_last_endpoint_ = false)
{
    if (!target_ || !bind_fn_ || attempts_ <= 0)
        return std::string ();

    for (int attempt = 0; attempt < attempts_; ++attempt) {
        const std::string endpoint = make_fixed_endpoint (transport_, base_port_ + attempt);
        if (endpoint.empty ())
            continue;
        if (bind_fn_ (target_, endpoint.c_str ()) != 0)
            continue;

        std::string resolved = endpoint;
        if (resolve_last_endpoint_) {
            char last_endpoint[MAX_SOCKET_STRING] = "";
            size_t size = sizeof (last_endpoint);
            if (zlink_get_option (target_, ZLINK_OPT_LAST_ENDPOINT, last_endpoint, &size) == 0) {
                resolved.assign (last_endpoint);
            }
        }

        return perf_normalize_bind_endpoint_host (resolved, transport_);
    }

    return std::string ();
}

// ---------------------------------------------------------------------------
// resolve_symbol - dlsym / GetProcAddress wrapper
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// resolve_bench_count - read bench count from environment
// ---------------------------------------------------------------------------
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

#endif
