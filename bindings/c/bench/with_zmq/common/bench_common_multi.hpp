#ifndef BENCH_COMMON_MULTI_HPP
#define BENCH_COMMON_MULTI_HPP

#include <chrono>
#include <vector>
#include <string>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <thread>
#include <atomic>

struct multi_bench_settings_t
{
    size_t clients;
    int inflight;
    int connect_concurrency;
    int warmup_seconds;
    int measure_seconds;
    int drain_ms;
};

inline int resolve_multi_int_env (const char *env_name, int default_value, int min_value)
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

inline multi_bench_settings_t resolve_multi_bench_settings ()
{
    multi_bench_settings_t settings;
    settings.clients = static_cast<size_t> (resolve_multi_int_env ("BENCH_MULTI_CLIENTS", 100, 1));
    settings.inflight = resolve_multi_int_env ("BENCH_MULTI_INFLIGHT", 30, 1);
    settings.connect_concurrency =
      resolve_multi_int_env ("BENCH_MULTI_CONNECT_CONCURRENCY", 128, 1);
    settings.warmup_seconds = resolve_multi_int_env ("BENCH_MULTI_WARMUP_SECONDS", 3, 0);
    settings.measure_seconds =
      resolve_multi_int_env ("BENCH_MULTI_DURATION_SECONDS",
                             resolve_multi_int_env ("BENCH_MULTI_MEASURE_SECONDS", 10, 1), 1);
    settings.drain_ms = resolve_multi_int_env ("BENCH_MULTI_DRAIN_MS", 300, 0);
    return settings;
}

template <typename ConnectFn>
inline bool connect_clients_concurrently (const std::vector<void *> &sockets,
                                          const std::string &endpoint,
                                          ConnectFn connect_fn,
                                          int max_concurrency)
{
    if (sockets.empty ())
        return true;

    std::atomic<bool> ok (true);
    const size_t total = sockets.size ();
    size_t start = 0;
    const size_t chunk = max_concurrency > 0 ? static_cast<size_t> (max_concurrency) : 1;

    while (start < total) {
        const size_t end = start + chunk < total ? start + chunk : total;
        std::vector<std::thread> workers;
        for (size_t i = start; i < end; ++i) {
            workers.emplace_back ([&, i] () {
                if (!connect_fn (sockets[i], endpoint))
                    ok.store (false, std::memory_order_relaxed);
            });
        }
        for (size_t i = 0; i < workers.size (); ++i)
            workers[i].join ();

        if (!ok.load (std::memory_order_relaxed))
            return false;

        start = end;
    }

    return true;
}
#endif
