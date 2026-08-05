#ifndef PERF_SETTINGS_COMMON_HPP
#define PERF_SETTINGS_COMMON_HPP

#include <algorithm>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <cctype>
#include <cstdint>
#include <iostream>
#include <string>

struct bench_settings_t
{
    size_t clients;
    uint64_t hwm;
    int duration_seconds;
    int connect_ready_timeout_ms;
};

inline int resolve_int_env (const char *env_name, int default_value, int min_value)
{
    if (!env_name)
        return default_value;

    const char *value = std::getenv (env_name);
    if (!value || !*value)
        return default_value;

    char *end = NULL;
    errno = 0;
    const long parsed = std::strtol (value, &end, 10);
    if (errno != 0 || end == value)
        return default_value;

    if (parsed < min_value)
        return min_value;
    return static_cast<int> (parsed);
}

inline const char *resolve_multi_env_value (const char *env_name, const char *legacy_name = NULL)
{
    if (env_name && *env_name) {
        const char *value = std::getenv (env_name);
        if (value && *value)
            return value;
    }

    if (legacy_name && *legacy_name) {
        const char *value = std::getenv (legacy_name);
        if (value && *value)
            return value;
    }

    return NULL;
}

inline int resolve_multi_int_env_with_fallback (const char *env_name,
                                                const char *legacy_name,
                                                int default_value,
                                                int min_value)
{
    const char *value = resolve_multi_env_value (env_name, legacy_name);
    if (!value)
        return default_value;

    char *end = NULL;
    errno = 0;
    const long parsed = std::strtol (value, &end, 10);
    if (errno != 0 || end == value)
        return default_value;

    if (parsed < min_value)
        return min_value;
    return static_cast<int> (parsed);
}

inline uint64_t resolve_multi_uint64_env_with_fallback (const char *env_name,
                                                        const char *legacy_name,
                                                        uint64_t default_value)
{
    const char *value = resolve_multi_env_value (env_name, legacy_name);
    if (!value)
        return default_value;
    if (std::strspn (value, "0123456789") != std::strlen (value))
        return default_value;

    char *end = NULL;
    errno = 0;
    const unsigned long long parsed = std::strtoull (value, &end, 10);
    if (errno != 0 || end == value || *end != '\0')
        return default_value;
    return static_cast<uint64_t> (parsed);
}

inline std::string normalize_multi_pattern_name (const char *pattern)
{
    if (!pattern || !*pattern)
        return std::string ();

    std::string normalized (pattern);
    std::transform (normalized.begin (), normalized.end (), normalized.begin (), ::toupper);
    if (normalized.compare (0, 6, "MULTI_") == 0)
        normalized.erase (0, 6);
    return normalized;
}

inline uint64_t resolve_default_hwm (const char *pattern, int clients)
{
    (void) clients;
    (void) pattern;
    return 0;
}

inline int resolve_default_clients (const char *pattern)
{
    const std::string normalized = normalize_multi_pattern_name (pattern);
    if (normalized == "STREAM")
        return 10000;

    return 100;
}

inline size_t resolve_service_clients (size_t requested_clients)
{
    const int override_clients = resolve_multi_int_env_with_fallback ("PERF_MULTI_SERVICE_CLIENTS",
                                                                      "PERF_SERVICE_CLIENTS", 0, 1);
    if (override_clients <= 0)
        return requested_clients;

    return std::min (requested_clients, static_cast<size_t> (override_clients));
}

inline bench_settings_t resolve_bench_settings ()
{
    bench_settings_t settings;
    const char *pattern = resolve_multi_env_value ("PERF_MULTI_PATTERN", "PERF_PATTERN");
    const int resolved_clients = resolve_multi_int_env_with_fallback (
      "PERF_MULTI_CLIENTS", "PERF_CLIENTS", resolve_default_clients (pattern), 1);
    settings.clients = static_cast<size_t> (resolved_clients);
    settings.hwm = resolve_multi_uint64_env_with_fallback (
      "PERF_MULTI_HWM", "PERF_HWM", resolve_default_hwm (pattern, resolved_clients));
    settings.duration_seconds = resolve_multi_int_env_with_fallback ("PERF_MULTI_DURATION_SECONDS",
                                                                     "PERF_DURATION_SECONDS", 5, 1);
    settings.connect_ready_timeout_ms = resolve_multi_int_env_with_fallback (
      "PERF_MULTI_CONNECT_READY_TIMEOUT_MS", "PERF_CONNECT_READY_TIMEOUT_MS", 1000, 0);
    return settings;
}

inline void set_perf_multi_pattern_env (const char *pattern)
{
    if (!pattern || !*pattern)
        return;
#if defined(_WIN32)
    _putenv_s ("PERF_MULTI_PATTERN", pattern);
#else
    setenv ("PERF_MULTI_PATTERN", pattern, 1);
#endif
}

typedef bench_settings_t multi_bench_settings_t;

inline bench_settings_t resolve_multi_bench_settings ()
{
    return resolve_bench_settings ();
}

inline int resolve_multi_int_env (const char *env_name, int default_value, int min_value)
{
    return resolve_multi_int_env_with_fallback (env_name, NULL, default_value, min_value);
}

inline std::string resolve_multi_perf_recv_mode ()
{
    const char *env = resolve_multi_env_value ("PERF_RECV_MODE", NULL);
    if (!env || !*env)
        return "recv";

    std::string mode (env);
    std::transform (mode.begin (), mode.end (), mode.begin (), ::tolower);
    if (mode != "recv") {
        std::cerr << "policy violation: multi perf supports recv only" << std::endl;
        std::exit (1);
    }
    return "recv";
}

inline bool multi_perf_validate_recv_mode_for_pattern (const char *pattern)
{
    if (!pattern || !*pattern)
        return false;
    (void) resolve_multi_perf_recv_mode ();
    return true;
}

inline size_t resolve_multi_service_clients (size_t requested_clients)
{
    return resolve_service_clients (requested_clients);
}

#endif
