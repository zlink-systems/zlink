/* SPDX-License-Identifier: FSL-1.1-ALv2 */
// Server-side metrics plus the minimal HTTP endpoint the client polls.
//
// spec 5 / G3 requires `send-saturation` throughput to be the number of
// messages the SERVER received, never the number the client submitted; FB-014
// records that the C bench fails that gate because it has no such endpoint. Each
// of the three C++ bench servers therefore publishes the same three routes:
//
//   GET  /ready        readiness, used before warmup
//   POST /bench/reset  opens a measured phase
//   GET  /bench/stats  the counters at the moment of the call
//
// The client samples /bench/stats at the ACTIVE WINDOW BOUNDARY and again after
// drain (FB-013): sampling only after drain reports a filtered client submission
// rate rather than the server's consumption rate, which inflated the .NET
// framework send row 4.2x.
#ifndef ZLINK_CPP_BENCH_STATS_SERVER_HPP
#define ZLINK_CPP_BENCH_STATS_SERVER_HPP

#include "bench_common.hpp"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

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
        _latency_ns.clear ();
        _cpu_start = process_cpu_seconds_self ();
    }

    // Records one received business message. Only ACTIVE-phase messages count
    // towards the measured throughput; warmup traffic carries phase 0 and is
    // counted separately so that "the server saw nothing" and "the server saw
    // only warmup" stay distinguishable.
    void record (const void *payload, size_t size)
    {
        decoded_header_t header {};
        if (!decode_payload (payload, size, &header))
            return;
        const uint64_t now = now_ns ();
        const double latency = now >= header.sent_ns ? static_cast<double> (now - header.sent_ns) : 0.0;
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

    std::string snapshot_json ()
    {
        std::vector<double> samples;
        long long active = 0;
        long long any_phase = 0;
        long long errors = 0;
        double cpu_start = 0.0;
        {
            std::lock_guard<std::mutex> lock (_gate);
            samples = _latency_ns;
            active = _active_messages;
            any_phase = _any_phase_messages;
            errors = _errors;
            cpu_start = _cpu_start;
        }
        std::sort (samples.begin (), samples.end ());
        const double cpu = std::max (0.0, process_cpu_seconds_self () - cpu_start);
        char buffer[512];
        std::snprintf (
          buffer, sizeof (buffer),
          "{\"activeMessages\":%lld,\"anyPhaseMessages\":%lld,\"errors\":%lld,"
          "\"meanMicros\":%.6f,\"p50Micros\":%.6f,\"p95Micros\":%.6f,\"p99Micros\":%.6f,"
          "\"cpuSeconds\":%.6f,\"workingSetMb\":%.6f}",
          active, any_phase, errors, mean (samples) / 1000.0, percentile (samples, 0.50) / 1000.0,
          percentile (samples, 0.95) / 1000.0, percentile (samples, 0.99) / 1000.0, cpu,
          rss_mb ());
        return buffer;
    }

  private:
    static double mean (const std::vector<double> &sorted)
    {
        if (sorted.empty ())
            return 0.0;
        double sum = 0.0;
        for (const double v : sorted)
            sum += v;
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
    double _cpu_start = process_cpu_seconds_self ();
};

// A one-connection-at-a-time HTTP/1.1 responder. It is polled a handful of times
// per cell, so a thread pool would add moving parts without adding fidelity; it
// runs on its own thread so that stats polling never shares a thread with the
// measured receive loops.
class stats_http_server_t
{
  public:
    stats_http_server_t (server_metrics_t &metrics, int port) :
        _metrics (metrics), _port (port)
    {
    }

    ~stats_http_server_t () { stop (); }

    bool start ()
    {
        _listen_fd = ::socket (AF_INET, SOCK_STREAM, 0);
        if (_listen_fd < 0)
            return false;
        int one = 1;
        ::setsockopt (_listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof (one));
        sockaddr_in addr {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons (static_cast<uint16_t> (_port));
        ::inet_pton (AF_INET, "127.0.0.1", &addr.sin_addr);
        if (::bind (_listen_fd, reinterpret_cast<sockaddr *> (&addr), sizeof (addr)) != 0
            || ::listen (_listen_fd, 32) != 0) {
            ::close (_listen_fd);
            _listen_fd = -1;
            return false;
        }
        _thread = std::thread ([this] { serve (); });
        return true;
    }

    void stop ()
    {
        _stop.store (true);
        if (_listen_fd >= 0) {
            ::shutdown (_listen_fd, SHUT_RDWR);
            ::close (_listen_fd);
            _listen_fd = -1;
        }
        if (_thread.joinable ())
            _thread.join ();
    }

  private:
    void serve ()
    {
        while (!_stop.load ()) {
            const int fd = ::accept (_listen_fd, nullptr, nullptr);
            if (fd < 0) {
                if (_stop.load ())
                    return;
                continue;
            }
            handle (fd);
            ::close (fd);
        }
    }

    void handle (int fd)
    {
        timeval tv {};
        tv.tv_sec = 2;
        ::setsockopt (fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof (tv));
        std::string request;
        char buffer[2048];
        for (;;) {
            const ssize_t n = ::recv (fd, buffer, sizeof (buffer), 0);
            if (n <= 0)
                break;
            request.append (buffer, static_cast<size_t> (n));
            if (request.find ("\r\n\r\n") != std::string::npos)
                break;
        }
        std::string body;
        if (request.find ("/bench/stats") != std::string::npos)
            body = _metrics.snapshot_json ();
        else if (request.find ("/bench/reset") != std::string::npos) {
            _metrics.reset ();
            body = "{\"ok\":true}";
        } else
            body = "{\"ready\":true}";

        char head[256];
        const int head_len = std::snprintf (
          head, sizeof (head),
          "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n"
          "Connection: close\r\n\r\n",
          body.size ());
        std::string response (head, static_cast<size_t> (head_len));
        response += body;
        size_t sent = 0;
        while (sent < response.size ()) {
            const ssize_t n = ::send (fd, response.data () + sent, response.size () - sent, 0);
            if (n <= 0)
                break;
            sent += static_cast<size_t> (n);
        }
    }

    server_metrics_t &_metrics;
    int _port;
    int _listen_fd = -1;
    std::atomic<bool> _stop {false};
    std::thread _thread;
};

} // namespace zlink_cpp_bench

#endif
