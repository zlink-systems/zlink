/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#ifndef ZLINK_CPP_BENCH_STATS_SERVER_HPP
#define ZLINK_CPP_BENCH_STATS_SERVER_HPP

#include "bench_common.hpp"

#include <zlink/framework.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace zlink_cpp_bench
{

class server_metrics_t
{
  public:
    void reset ()
    {
        std::lock_guard<std::mutex> lock (_gate);
        _active_messages = 0;
        _any_phase_messages = 0;
        _errors = 0;
        _rejected_baseline = _rejected_total;
        _latency_ns.clear ();
        _cpu_start = process_cpu_seconds_self ();
    }

    void record (const void *payload, size_t size)
    {
        decoded_header_t header {};
        if (!decode_payload (payload, size, &header)
            || header.payload_size != size
            || (header.phase != phase_warmup && header.phase != phase_active)) {
            record_error ();
            return;
        }
        const uint64_t now = now_ns ();
        const double latency =
          now >= header.sent_ns ? static_cast<double> (now - header.sent_ns) : 0.0;
        std::lock_guard<std::mutex> lock (_gate);
        ++_any_phase_messages;
        if (header.phase != phase_active)
            return;
        ++_active_messages;
        if (_latency_ns.size () < 2000000)
            _latency_ns.push_back (latency);
    }

    void record_error ()
    {
        std::lock_guard<std::mutex> lock (_gate);
        ++_errors;
    }

    void observe_rejected_total (long long total)
    {
        std::lock_guard<std::mutex> lock (_gate);
        _rejected_total = std::max (_rejected_total, total);
    }

    std::string snapshot_json ()
    {
        std::vector<double> samples;
        long long active = 0;
        long long any_phase = 0;
        long long errors = 0;
        long long rejected = 0;
        double cpu_start = 0.0;
        {
            std::lock_guard<std::mutex> lock (_gate);
            samples = _latency_ns;
            active = _active_messages;
            any_phase = _any_phase_messages;
            errors = _errors;
            rejected = _rejected_total - _rejected_baseline;
            cpu_start = _cpu_start;
        }
        std::sort (samples.begin (), samples.end ());
        const double cpu = std::max (0.0, process_cpu_seconds_self () - cpu_start);
        char buffer[768];
        std::snprintf (
          buffer, sizeof (buffer),
          "{\"role\":\"target\",\"ready\":true,\"phase\":\"idle\","
          "\"submitted\":0,\"completed\":%lld,\"errors\":%lld,\"received\":%lld,"
          "\"inFlight\":0,\"currentInFlight\":0,\"peakInFlight\":0,"
          "\"activeMessages\":%lld,\"anyPhaseMessages\":%lld,"
          "\"rejected\":%lld,\"meanMicros\":%.6f,\"p50Micros\":%.6f,\"p95Micros\":%.6f,"
          "\"p99Micros\":%.6f,\"cpuSeconds\":%.6f,\"workingSetMb\":%.6f}",
          active, errors, active, active, any_phase, rejected, mean (samples) / 1000.0,
          percentile (samples, 0.50) / 1000.0, percentile (samples, 0.95) / 1000.0,
          percentile (samples, 0.99) / 1000.0, cpu, rss_mb ());
        return buffer;
    }

  private:
    static double mean (const std::vector<double> &sorted)
    {
        if (sorted.empty ())
            return 0.0;
        double sum = 0.0;
        for (const double value : sorted)
            sum += value;
        return sum / static_cast<double> (sorted.size ());
    }

    static double percentile (const std::vector<double> &sorted, double q)
    {
        if (sorted.empty ())
            return 0.0;
        const size_t index = static_cast<size_t> (std::min<double> (
          static_cast<double> (sorted.size () - 1), q * static_cast<double> (sorted.size () - 1)));
        return sorted[index];
    }

    std::mutex _gate;
    std::vector<double> _latency_ns;
    long long _active_messages = 0;
    long long _any_phase_messages = 0;
    long long _errors = 0;
    long long _rejected_total = 0;
    long long _rejected_baseline = 0;
    double _cpu_start = process_cpu_seconds_self ();
};

struct bench_http_reply_t
{
    int status = 200;
    std::string json = "{}";
};

using bench_http_callback_t = std::function<bench_http_reply_t (
  const std::string &method, const std::string &path, const std::string &body)>;

namespace detail
{

inline std::string json_quote (const std::string &value)
{
    std::string result;
    result.reserve (value.size () + 2);
    result.push_back ('\"');
    for (const char ch : value) {
        switch (ch) {
            case '\\': result += "\\\\"; break;
            case '\"': result += "\\\""; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default: result.push_back (ch); break;
        }
    }
    result.push_back ('\"');
    return result;
}

inline const char *method_name (zlink::framework::http_method_t method)
{
    switch (method) {
        case zlink::framework::http_method_t::get: return "GET";
        case zlink::framework::http_method_t::post: return "POST";
        case zlink::framework::http_method_t::put: return "PUT";
        case zlink::framework::http_method_t::delete_: return "DELETE";
    }
    return "UNKNOWN";
}

class bench_http_dispatch_t
{
  public:
    explicit bench_http_dispatch_t (bench_http_callback_t callback) :
        _callback (std::move (callback))
    {
    }

    zlink::framework::http_response_t dispatch (
      const zlink::framework::http_request_t &request) const
    {
        try {
            const auto reply = _callback (method_name (request.method), request.path, request.body);
            return {.status = reply.status, .body = reply.json, .content_type = "application/json"};
        }
        catch (const std::exception &error) {
            return {.status = 500,
                    .body = "{\"reason\":" + json_quote (error.what ()) + "}",
                    .content_type = "application/json"};
        }
        catch (...) {
            return {.status = 500,
                    .body = "{\"reason\":\"unknown HTTP callback failure\"}",
                    .content_type = "application/json"};
        }
    }

  private:
    bench_http_callback_t _callback;
};

template <int Route> class bench_http_handler_t
{
  public:
    explicit bench_http_handler_t (bench_http_dispatch_t &dispatch) : _dispatch (dispatch) {}

    zlink::framework::http_response_t handle (
      const zlink::framework::http_request_t &request) const
    {
        return _dispatch.dispatch (request);
    }

  private:
    bench_http_dispatch_t &_dispatch;
};

using ready_http_handler_t = bench_http_handler_t<0>;
using stats_http_handler_t = bench_http_handler_t<1>;
using reset_http_handler_t = bench_http_handler_t<2>;
using start_http_handler_t = bench_http_handler_t<3>;

} // namespace detail

// Owns the one Framework app host used by both benchmark HTTP control and, for
// the framework implementation, its channel topology. Multiple listen ports
// intentionally share the same routes and lifecycle. Empty ports create the
// channel-only source host, started after its HTTP control host has bound.
class stats_http_server_t
{
  public:
    stats_http_server_t (server_metrics_t &metrics, int port,
                         std::function<void ()> refresh_metrics = {}) :
        stats_http_server_t (
          std::vector<int>{port},
          [&metrics, refresh_metrics = std::move (refresh_metrics)] (const std::string &method, const std::string &path,
                      const std::string &) -> bench_http_reply_t {
              if (method == "GET" && path == "/ready")
                  return {200, "{\"ready\":true}"};
              if (method == "GET" && path == "/bench/stats") {
                  if (refresh_metrics) refresh_metrics ();
                  return {200, metrics.snapshot_json ()};
              }
              if (method == "POST" && path == "/bench/reset") {
                  if (refresh_metrics) refresh_metrics ();
                  metrics.reset ();
                  return {200, "{\"ok\":true}"};
              }
              return {404, "{}"};
          })
    {
    }

    stats_http_server_t (int port, bench_http_callback_t callback) :
        stats_http_server_t (std::vector<int>{port}, std::move (callback))
    {
    }

    stats_http_server_t (std::vector<int> ports, bench_http_callback_t callback) :
        _ports (std::move (ports)), _callback (std::move (callback)),
        _app (zlink::framework::app_t::create ())
    {
    }

    ~stats_http_server_t () { stop (); }

    stats_http_server_t (const stats_http_server_t &) = delete;
    stats_http_server_t &operator= (const stats_http_server_t &) = delete;

    zlink::framework::app_t &app () noexcept { return _app; }

    bool start ()
    {
        if (_thread.joinable () || (!_ports.empty () && !_callback))
            return false;

        try {
            auto &framework = _app.add_zlink_framework ();
            if (!_ports.empty ()) {
                framework.services ().add_singleton<detail::bench_http_dispatch_t> (
                  std::make_unique<detail::bench_http_dispatch_t> (std::move (_callback)));
                auto &http = framework.http ();
                for (const int port : _ports) {
                    if (port <= 0 || port > 65535)
                        return false;
                    http.listen ("http://127.0.0.1:" + std::to_string (port));
                }
                http.map_get<detail::ready_http_handler_t> ("/ready")
                  .map_get<detail::stats_http_handler_t> ("/bench/stats")
                  .map_post<detail::reset_http_handler_t> ("/bench/reset")
                  .map_post<detail::start_http_handler_t> ("/bench/start");
            }
        }
        catch (const std::exception &error) {
            std::fprintf (stderr, "C++ bench HTTP host configuration failed: %s\n",
                          error.what ());
            return false;
        }
        catch (...) {
            std::fprintf (stderr, "C++ bench HTTP host configuration failed: unknown error\n");
            return false;
        }

        _run_done.store (false, std::memory_order_release);
        _thread = std::thread ([this] {
            try {
                const int rc = _app.run (0, nullptr);
                if (rc != 0)
                    std::fprintf (stderr, "C++ bench HTTP host exited with rc=%d\n", rc);
            }
            catch (const std::exception &error) {
                std::fprintf (stderr, "C++ bench HTTP host failed: %s\n", error.what ());
            }
            catch (...) {
                std::fprintf (stderr, "C++ bench HTTP host failed: unknown error\n");
            }
            _run_done.store (true, std::memory_order_release);
        });

        const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (30);
        while (!_app.is_ready () && !_run_done.load (std::memory_order_acquire)
               && std::chrono::steady_clock::now () < deadline) {
            std::this_thread::sleep_for (std::chrono::milliseconds (1));
        }
        if (_app.is_ready ())
            return true;
        stop ();
        return false;
    }

    void stop ()
    {
        if (_thread.joinable ()) {
            _app.request_stop ();
            _thread.join ();
        }
    }

  private:
    std::vector<int> _ports;
    bench_http_callback_t _callback;
    zlink::framework::app_t _app;
    std::atomic<bool> _run_done {false};
    std::thread _thread;
};

} // namespace zlink_cpp_bench

#endif
