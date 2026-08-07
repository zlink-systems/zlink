#ifndef PERF_STREAM_PERF_CLIENT_HPP
#define PERF_STREAM_PERF_CLIENT_HPP

// Async multi-connection benchmark orchestrator (tcp/tls/ws/wss).
// Provides:
//   loopback_bind_plan_t      – source-address sharding plan for loopback
//   read_ipv4_ephemeral_port_capacity() – reads OS ephemeral port range
//   make_loopback_shard_addr()          – generates 127.x.x.x addresses
//   make_loopback_bind_plan()           – computes required shard count
//   bench_client_t             – orchestrator (implements bench_client_iface_t)
//
// bench_client_t manages the full async benchmark lifecycle:
//   1. Spin up io_context + worker threads
//   2. Create CCU client_session_t instances
//   3. Batched connect scheduling
//   4. For each size: resize → active window → completion wait → report
//   5. Shutdown and join

#include "perf_stream_client_options.hpp"
#include "perf_stream_client_session.hpp"
#include "perf_stream_common.hpp"

#include <boost/asio.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// --- Loopback port sharding ---
// When CCU exceeds the OS ephemeral port range on loopback, connections are
// distributed across multiple 127.x.x.x source addresses to avoid exhaustion.

struct loopback_bind_plan_t
{
    size_t port_capacity;
    std::vector<boost::asio::ip::address_v4> source_addrs;

    loopback_bind_plan_t () : port_capacity (0), source_addrs () {}
};

// Read usable ephemeral port count from /proc/sys/net/ipv4/ip_local_port_range.
inline size_t read_ipv4_ephemeral_port_capacity ()
{
    std::ifstream in ("/proc/sys/net/ipv4/ip_local_port_range");
    if (!in.is_open ())
        return 0;

    int low = 0;
    int high = 0;
    in >> low >> high;
    if (!in.good () && !in.eof ())
        return 0;
    if (low <= 0 || high <= 0 || high < low)
        return 0;
    return static_cast<size_t> (high - low + 1);
}

// Generate a unique loopback address: 127.0.<block>.<1..254>.
inline boost::asio::ip::address_v4 make_loopback_shard_addr (size_t idx)
{
    const size_t block = idx / 254;
    const size_t tail = (idx % 254) + 1;
    boost::asio::ip::address_v4::bytes_type bytes;
    bytes[0] = 127;
    bytes[1] = 0;
    bytes[2] = static_cast<unsigned char> (block & 0xFF);
    bytes[3] = static_cast<unsigned char> (tail);
    return boost::asio::ip::address_v4 (bytes);
}

inline size_t loopback_shard_offset_for_transport (const std::string &transport)
{
    const std::string normalized = perf_stream_common::lower_copy (transport);
    if (normalized == "tls")
        return 256;
    if (normalized == "ws")
        return 512;
    if (normalized == "wss")
        return 768;
    return 0;
}

// Compute source-address sharding plan.
// shards = ceil(ccu / usable_ephemeral_ports). Returns empty plan for non-loopback.
inline loopback_bind_plan_t make_loopback_bind_plan (const boost::asio::ip::tcp::endpoint &endpoint,
                                                     int ccu,
                                                     size_t shard_offset = 0)
{
    loopback_bind_plan_t plan;
    if (ccu <= 0)
        return plan;
    if (!endpoint.address ().is_v4 ())
        return plan;
    if (!endpoint.address ().to_v4 ().is_loopback ())
        return plan;

    plan.port_capacity = read_ipv4_ephemeral_port_capacity ();
#if defined(_WIN32)
    // Windows does not expose the dynamic TCP port range through the POSIX
    // procfs path above. Reserve a conservative per-source-address budget so
    // the 10,000-client default is spread across the 127/8 loopback range
    // instead of exhausting one local address.
    if (plan.port_capacity == 0)
        plan.port_capacity = 4096;
#endif
    if (plan.port_capacity == 0)
        return plan;

    const size_t usable_ports = plan.port_capacity > k_loopback_port_headroom
                                  ? plan.port_capacity - k_loopback_port_headroom
                                  : plan.port_capacity;
    if (usable_ports == 0)
        return plan;

    const size_t required =
      static_cast<size_t> (ccu + static_cast<int> (usable_ports) - 1) / usable_ports;
    if (required <= 1)
        return plan;

    plan.source_addrs.reserve (required);
    for (size_t i = 0; i < required; ++i)
        plan.source_addrs.push_back (make_loopback_shard_addr (shard_offset + i));
    return plan;
}

inline boost::asio::ip::tcp::endpoint
resolve_stream_target_endpoint (boost::asio::io_context &io_, const std::string &host_, int port_)
{
    boost::system::error_code ec;
    const boost::asio::ip::address parsed = boost::asio::ip::make_address (host_, ec);
    if (!ec)
        return boost::asio::ip::tcp::endpoint (parsed, static_cast<unsigned short> (port_));

    boost::asio::ip::tcp::resolver resolver (io_);
    boost::asio::ip::tcp::resolver::results_type resolved =
      resolver.resolve (host_, std::to_string (port_), ec);
    if (ec || resolved.begin () == resolved.end ())
        boost::asio::detail::throw_error (ec ? ec : boost::asio::error::host_not_found);

    return resolved.begin ()->endpoint ();
}

// --- Benchmark orchestrator ---
// Manages worker threads, sessions, phase transitions, and metrics collection.
// Implements bench_client_iface_t so sessions can report events thread-safely.

class bench_client_t : public bench_client_iface_t
{
  public:
    explicit bench_client_t (const client_options_t &opt_) :
        opt (opt_),
        io (),
        work_guard (boost::asio::make_work_guard (io)),
        next_connect_idx (0),
        connect_active (0),
        connect_success (0),
        connect_fail (0),
        connect_completed (0),
        mode (phase_ready),
        phase_end_ns (0),
        phase_size (opt.sizes.empty () ? 64 : opt.sizes[0]),
        outstanding_total (0),
        seq_gen (0),
        bytes_recv_measure (0),
        send_error_measure (0),
        recv_error_measure (0),
        timeout_error_measure (0),
        size_mismatch_measure (0),
        collect_metrics (false),
        // RTT sample storage is provisioned once during bench setup.
        rtt_samples_bits (new std::atomic<uint64_t>[k_rtt_sample_capacity]),
        sample_overwrite_idx (0),
        endpoint (resolve_stream_target_endpoint (io, opt.host, opt.port)),
        loopback_bind_plan ()
    {
        loopback_bind_plan = make_loopback_bind_plan (
          endpoint, opt.ccu, loopback_shard_offset_for_transport (opt.transport));
    }

    // Main entry: connect all sessions, run benchmarks per size, then shutdown.
    // Returns 0 on all-pass, 2 on any failure.
    int run ()
    {
        const int worker_count = std::max (1, opt.io_threads);
        for (int i = 0; i < worker_count; ++i)
            workers.push_back (std::thread ([this] () { io.run (); }));

        sessions.reserve (static_cast<size_t> (std::max (1, opt.ccu)));
        for (int i = 0; i < opt.ccu; ++i) {
            sessions.push_back (std::make_shared<client_session_t> (
              *this, io, opt.transport, source_bind_endpoint_for (static_cast<size_t> (i))));
        }

        schedule_connects ();

        const auto connect_deadline =
          std::chrono::steady_clock::now () + std::chrono::seconds (k_connect_timeout_s);
        {
            std::unique_lock<std::mutex> lk (connect_mu);
            while (connect_completed.load (std::memory_order_acquire)
                   < static_cast<long> (sessions.size ())) {
                if (connect_cv.wait_until (lk, connect_deadline) == std::cv_status::timeout)
                    break;
            }
        }

        const long completed = connect_completed.load (std::memory_order_relaxed);
        if (completed < static_cast<long> (sessions.size ())) {
            const long unresolved = static_cast<long> (sessions.size ()) - completed;
            connect_fail.fetch_add (unresolved, std::memory_order_relaxed);
            connect_completed.store (static_cast<long> (sessions.size ()),
                                     std::memory_order_release);
        }

        if (connect_success.load (std::memory_order_relaxed) <= 0) {
            if (std::getenv ("PERF_DEBUG")) {
                std::fprintf (stderr,
                              "perf_stream_client: connect_success=%ld connect_fail=%ld "
                              "connect_completed=%ld sessions=%zu\n",
                              connect_success.load (std::memory_order_relaxed),
                              connect_fail.load (std::memory_order_relaxed),
                              connect_completed.load (std::memory_order_relaxed), sessions.size ());
            }
            shutdown_all_sessions ();
            join_workers ();
            return 2;
        }

        bool all_pass = true;
        for (int run_idx = 1; run_idx <= std::max (1, opt.runs); ++run_idx) {
            for (size_t i = 0; i < opt.sizes.size (); ++i) {
                const size_t size = opt.sizes[i];
                case_metrics_t m = run_case (size);
                if (m.pass) {
                    const double throughput =
                      size > 0 ? (m.throughput_bps / static_cast<double> (size)) : 0.0;
                    const double bandwidth = (m.throughput_bps * 2.0) / 1000000.0;
                    const double latency_mean_ns = m.mean_ns > 0.0 ? m.mean_ns : m.p50_ns;
                    const double latency_p95_ns = m.p95_ns > 0.0 ? m.p95_ns : latency_mean_ns;
                    const double latency_p99_ns = m.p99_ns > 0.0 ? m.p99_ns : latency_p95_ns;
                    std::printf ("RESULT,current,%s,%s,%zu,throughput,%.3f\n", opt.pattern.c_str (),
                                 opt.transport.c_str (), size, throughput);
                    std::printf ("RESULT,current,%s,%s,%zu,bandwidth,%.3f\n", opt.pattern.c_str (),
                                 opt.transport.c_str (), size, bandwidth);
                    std::printf ("RESULT,current,%s,%s,%zu,latency,%.3f\n", opt.pattern.c_str (),
                                 opt.transport.c_str (), size, latency_mean_ns / 1000000.0);
                    std::printf ("RESULT,current,%s,%s,%zu,latency_p95,%.3f\n",
                                 opt.pattern.c_str (), opt.transport.c_str (), size,
                                 latency_p95_ns / 1000000.0);
                    std::printf ("RESULT,current,%s,%s,%zu,latency_p99,%.3f\n",
                                 opt.pattern.c_str (), opt.transport.c_str (), size,
                                 latency_p99_ns / 1000000.0);
                }
                std::fflush (stdout);
                if (!m.pass)
                    all_pass = false;
                if ((i + 1) < opt.sizes.size ())
                    run_size_transition_completion_wait ();
            }
        }

        shutdown_all_sessions ();
        join_workers ();
        return all_pass ? 0 : 2;
    }

    // --- bench_client_iface_t overrides (called from I/O threads) ---

    // Track connect success/failure and trigger next batched connect.
    void on_connect_result (bool success, const std::shared_ptr<client_session_t> &session) override
    {
        if (std::getenv ("PERF_DEBUG")) {
            std::fprintf (stderr, "perf_stream_client: on_connect_result success=%d session=%p\n",
                          success ? 1 : 0, session.get ());
        }
        if (success) {
            connect_success.fetch_add (1, std::memory_order_relaxed);
            std::lock_guard<std::mutex> lk (connected_mu);
            connected_sessions.push_back (session);
        } else {
            connect_fail.fetch_add (1, std::memory_order_relaxed);
        }

        connect_completed.fetch_add (1, std::memory_order_relaxed);
        connect_active.fetch_sub (1, std::memory_order_relaxed);

        {
            std::lock_guard<std::mutex> lk (connect_mu);
            connect_cv.notify_all ();
        }

        schedule_connects ();
    }

    bool allow_send () const override
    {
        return mode.load (std::memory_order_acquire) == phase_active
               && perf_stream_common::perf_stream_now_ns ()
                    < phase_end_ns.load (std::memory_order_relaxed);
    }

    size_t current_phase_size () const override
    {
        return phase_size.load (std::memory_order_relaxed);
    }

    uint32_t metric_run_id () const override { return 1U; }

    perf_multi_metric::phase_t metric_phase () const override
    {
        const int current_mode = mode.load (std::memory_order_acquire);
        if (current_mode == phase_active)
            return perf_multi_metric::phase_active;
        return perf_multi_metric::phase_unknown;
    }

    uint64_t next_seq () override { return seq_gen.fetch_add (1, std::memory_order_relaxed) + 1; }

    void on_send_begin (size_t) override
    {
        outstanding_total.fetch_add (1, std::memory_order_relaxed);
    }

    // Record received bytes and RTT derived from stamped payload header.
    void on_recv_done (size_t bytes, uint64_t sent_ts_ns) override
    {
        const long remaining = outstanding_total.fetch_sub (1, std::memory_order_relaxed) - 1;
        if (remaining <= 0) {
            std::lock_guard<std::mutex> lk (completion_wait_mu);
            completion_wait_cv.notify_all ();
        }
        if (!collect_metrics.load (std::memory_order_acquire))
            return;

        bytes_recv_measure.fetch_add (static_cast<long long> (bytes), std::memory_order_relaxed);

        if (sent_ts_ns > 0) {
            const uint64_t now_ns = perf_multi_metric::now_ns ();
            if (now_ns >= sent_ts_ns)
                add_rtt_sample (static_cast<double> (now_ns - sent_ts_ns));
        }
    }

    void on_send_error () override
    {
        if (!collect_metrics.load (std::memory_order_acquire))
            return;
        send_error_measure.fetch_add (1, std::memory_order_relaxed);
    }

    void on_recv_error () override
    {
        if (!collect_metrics.load (std::memory_order_acquire))
            return;
        recv_error_measure.fetch_add (1, std::memory_order_relaxed);
    }

    void on_abandon (long count) override
    {
        if (count > 0) {
            const long remaining =
              outstanding_total.fetch_sub (count, std::memory_order_relaxed) - count;
            if (remaining <= 0) {
                std::lock_guard<std::mutex> lk (completion_wait_mu);
                completion_wait_cv.notify_all ();
            }
        }
    }

    void on_size_mismatch () override
    {
        if (!collect_metrics.load (std::memory_order_acquire))
            return;
        size_mismatch_measure.fetch_add (1, std::memory_order_relaxed);
    }

  private:
    int resolve_connect_batch_limit () const
    {
        const char *raw = std::getenv ("PERF_MULTI_STREAM_CONNECT_BATCH");
        if (raw && *raw) {
            char *end = NULL;
            errno = 0;
            const long parsed = std::strtol (raw, &end, 10);
            if (errno == 0 && end != raw && parsed > 0) {
                if (parsed > INT_MAX)
                    return INT_MAX;
                return static_cast<int> (parsed);
            }
        }

        const std::string transport = perf_stream_common::lower_copy (opt.transport);
#if defined(_WIN32)
        // Keep the default connection burst bounded on Windows. A large
        // burst can overrun the listener accept queue before the server
        // thread drains it, even though the loopback port budget is enough.
        if (transport == "tcp")
            return std::min (k_connect_batch, 128);
#else
        if (transport == "tcp")
            return k_connect_batch;
#endif
        return std::min (k_connect_batch, 128);
    }

    // --- Connection management ---

    // Launch up to k_connect_batch concurrent connect() calls.
    void schedule_connects ()
    {
        std::lock_guard<std::mutex> lk (connect_sched_mu);
        const int batch_limit = resolve_connect_batch_limit ();
        while (connect_active.load (std::memory_order_relaxed) < batch_limit) {
            const size_t idx = next_connect_idx.fetch_add (1, std::memory_order_relaxed);
            if (idx >= sessions.size ())
                break;
            connect_active.fetch_add (1, std::memory_order_relaxed);
            sessions[idx]->begin_connect (endpoint);
        }
    }

    // Map session index to a loopback shard address (round-robin).
    boost::asio::ip::tcp::endpoint source_bind_endpoint_for (size_t idx) const
    {
        if (loopback_bind_plan.source_addrs.empty ())
            return boost::asio::ip::tcp::endpoint ();
        const boost::asio::ip::address_v4 &addr =
          loopback_bind_plan.source_addrs[idx % loopback_bind_plan.source_addrs.size ()];
        return boost::asio::ip::tcp::endpoint (addr, 0);
    }

    std::vector<std::shared_ptr<client_session_t>> snapshot_connected_sessions ()
    {
        std::vector<std::shared_ptr<client_session_t>> copy;
        {
            std::lock_guard<std::mutex> lk (connected_mu);
            copy.reserve (connected_sessions.size ());
            for (size_t i = 0; i < connected_sessions.size (); ++i) {
                const std::shared_ptr<client_session_t> &session = connected_sessions[i];
                if (session && session->connected ())
                    copy.push_back (session);
            }
        }
        return copy;
    }

    long count_connected_sessions ()
    {
        long count = 0;
        std::lock_guard<std::mutex> lk (connected_mu);
        for (size_t i = 0; i < connected_sessions.size (); ++i) {
            const std::shared_ptr<client_session_t> &session = connected_sessions[i];
            if (session && session->connected ())
                ++count;
        }
        return count;
    }

    // --- Phase control ---

    // Dispatch chunk-size update to all connected sessions and wait on latch.
    bool set_phase_size_for_connected (size_t size)
    {
        phase_size.store (size, std::memory_order_release);
        std::vector<std::shared_ptr<client_session_t>> copy = snapshot_connected_sessions ();
        if (copy.empty ())
            return false;

        const std::shared_ptr<resize_latch_t> latch =
          std::make_shared<resize_latch_t> (copy.size ());
        for (size_t i = 0; i < copy.size (); ++i)
            copy[i]->set_chunk_size (size, latch);

        const auto deadline =
          std::chrono::steady_clock::now () + std::chrono::seconds (k_resize_timeout_s);
        std::unique_lock<std::mutex> lk (latch->mu);
        while (latch->pending > 0) {
            if (latch->cv.wait_until (lk, deadline) == std::cv_status::timeout)
                return false;
        }
        return true;
    }

    // Start traffic on all connected sessions (begins the send loop).
    void kick_phase_for_connected ()
    {
        std::vector<std::shared_ptr<client_session_t>> copy = snapshot_connected_sessions ();
        for (size_t i = 0; i < copy.size (); ++i)
            copy[i]->start_traffic ();
    }

    void on_timeout (long count)
    {
        if (count <= 0)
            return;
        timeout_error_measure.fetch_add (count, std::memory_order_relaxed);
    }

    int effective_phase_completion_ms (size_t size) const
    {
        int completion_wait_ms = std::max (0, opt.completion_wait_ms);
        // Large frames leave a longer tail of in-flight echoes in callback
        // mode. The default short completion wait is enough for small frames
        // but not for the 64KiB policy size at high CCU.
        if (size >= 65536)
            completion_wait_ms = std::max (completion_wait_ms, 5000);
        return completion_wait_ms;
    }

    // Run the active window, then stop and wait for in-flight ops to finish.
    bool run_active_window (int duration_s)
    {
        if (duration_s <= 0)
            return true;

        reset_measurement_counters ();

        collect_metrics.store (true, std::memory_order_release);
        const uint64_t end_ns = perf_stream_common::perf_stream_now_ns ()
                                + static_cast<uint64_t> (duration_s) * 1000ULL * 1000ULL * 1000ULL;
        phase_end_ns.store (end_ns, std::memory_order_release);
        mode.store (phase_active, std::memory_order_release);

        kick_phase_for_connected ();
        std::this_thread::sleep_for (std::chrono::seconds (duration_s));

        mode.store (phase_ready, std::memory_order_release);

        const int completion_wait_ms =
          effective_phase_completion_ms (phase_size.load (std::memory_order_acquire));
        if (completion_wait_ms > 0) {
            const auto completion_wait_deadline =
              std::chrono::steady_clock::now () + std::chrono::milliseconds (completion_wait_ms);
            long remaining = 0;
            {
                std::unique_lock<std::mutex> lk (completion_wait_mu);
                while (outstanding_total.load (std::memory_order_relaxed) > 0) {
                    if (completion_wait_cv.wait_until (lk, completion_wait_deadline)
                        == std::cv_status::timeout)
                        break;
                }

                remaining = outstanding_total.load (std::memory_order_relaxed);
            }
            if (remaining > 0) {
                on_timeout (remaining);
                on_abandon (remaining);
            }
        }

        collect_metrics.store (false, std::memory_order_release);
        return true;
    }

    // Brief completion wait between size transitions.
    void run_size_transition_completion_wait ()
    {
        const int completion_wait_ms = std::max (0, opt.size_transition_completion_wait_ms);
        if (completion_wait_ms <= 0)
            return;

        const auto completion_wait_deadline =
          std::chrono::steady_clock::now () + std::chrono::milliseconds (completion_wait_ms);
        std::unique_lock<std::mutex> lk (completion_wait_mu);
        while (outstanding_total.load (std::memory_order_relaxed) > 0) {
            if (completion_wait_cv.wait_until (lk, completion_wait_deadline)
                == std::cv_status::timeout)
                break;
        }
    }

    void reset_measurement_counters ()
    {
        bytes_recv_measure.store (0, std::memory_order_relaxed);
        send_error_measure.store (0, std::memory_order_relaxed);
        recv_error_measure.store (0, std::memory_order_relaxed);
        timeout_error_measure.store (0, std::memory_order_relaxed);
        size_mismatch_measure.store (0, std::memory_order_relaxed);
        sample_overwrite_idx.store (0, std::memory_order_relaxed);
    }

    // --- RTT sample ring buffer ---
    // Stores latency samples as bit-cast double→uint64_t in atomic[] for
    // lock-free write from I/O threads and safe read from the main thread.

    static uint64_t encode_double_bits (double v)
    {
        uint64_t bits = 0;
        std::memcpy (&bits, &v, sizeof (bits));
        return bits;
    }

    static double decode_double_bits (uint64_t bits)
    {
        double v = 0.0;
        std::memcpy (&v, &bits, sizeof (v));
        return v;
    }

    void add_rtt_sample (double us)
    {
        if (!rtt_samples_bits)
            return;

        const size_t idx =
          sample_overwrite_idx.fetch_add (1, std::memory_order_relaxed) % k_rtt_sample_capacity;
        rtt_samples_bits[idx].store (encode_double_bits (us), std::memory_order_release);
    }

    // --- Per-size benchmark execution ---
    // resize → active → completion wait → collect metrics
    case_metrics_t run_case (size_t size)
    {
        const long connect_target = static_cast<long> (std::max (1, opt.ccu));
        const long required_connect = std::max<long> (1, connect_target);

        const bool resize_ok = set_phase_size_for_connected (size);
        if (!resize_ok) {
            case_metrics_t failed;
            failed.connect_ok = count_connected_sessions ();
            failed.connect_fail = std::max<long> (0, connect_target - failed.connect_ok);
            failed.timeout_error = 1;
            failed.pass = false;
            return failed;
        }

        const bool window_ok = run_active_window (std::max (1, opt.duration));

        case_metrics_t out;
        out.connect_ok = count_connected_sessions ();
        out.connect_fail = std::max<long> (0, connect_target - out.connect_ok);

        const long long bytes_recv = bytes_recv_measure.load (std::memory_order_relaxed);
        const double duration_s = static_cast<double> (std::max (1, opt.duration));
        out.throughput_bps = duration_s > 0.0 ? static_cast<double> (bytes_recv) / duration_s : 0.0;
        out.throughput_mib_s = out.throughput_bps / (1024.0 * 1024.0);

        const size_t sample_count = std::min<size_t> (
          sample_overwrite_idx.load (std::memory_order_relaxed), k_rtt_sample_capacity);
        if (sample_count > 0) {
            std::vector<double> snapshot;
            snapshot.reserve (sample_count);
            for (size_t i = 0; i < sample_count; ++i) {
                snapshot.push_back (
                  decode_double_bits (rtt_samples_bits[i].load (std::memory_order_acquire)));
            }
            double sum_ns = 0.0;
            for (size_t i = 0; i < snapshot.size (); ++i)
                sum_ns += snapshot[i];
            if (!snapshot.empty ())
                out.mean_ns = sum_ns / static_cast<double> (snapshot.size ());
            std::sort (snapshot.begin (), snapshot.end ());
            out.p50_ns = perf_stream_common::percentile_from_sorted (snapshot, 0.50);
            out.p95_ns = perf_stream_common::percentile_from_sorted (snapshot, 0.95);
            out.p99_ns = perf_stream_common::percentile_from_sorted (snapshot, 0.99);
        }

        out.send_error = send_error_measure.load (std::memory_order_relaxed);
        out.recv_error = recv_error_measure.load (std::memory_order_relaxed);
        out.timeout_error = timeout_error_measure.load (std::memory_order_relaxed);
        out.size_mismatch = size_mismatch_measure.load (std::memory_order_relaxed);

        out.pass = window_ok && out.connect_ok >= required_connect && out.send_error == 0
                   && out.recv_error == 0 && out.timeout_error == 0 && out.size_mismatch == 0
                   && out.throughput_bps > 0.0;
        if (!out.pass && std::getenv ("PERF_DEBUG")) {
            std::fprintf (stderr,
                          "perf_stream_client: case_failed size=%zu connect_ok=%ld "
                          "connect_fail=%ld send_error=%ld recv_error=%ld timeout_error=%ld "
                          "size_mismatch=%ld throughput_bps=%.3f samples=%zu window_ok=%d\n",
                          size, out.connect_ok, out.connect_fail, out.send_error, out.recv_error,
                          out.timeout_error, out.size_mismatch, out.throughput_bps, sample_count,
                          window_ok ? 1 : 0);
        }
        return out;
    }

    void shutdown_all_sessions ()
    {
        for (size_t i = 0; i < sessions.size (); ++i)
            sessions[i]->request_close ();
    }

    void join_workers ()
    {
        work_guard.reset ();
        io.stop ();
        for (size_t i = 0; i < workers.size (); ++i) {
            if (workers[i].joinable ())
                workers[i].join ();
        }
    }

    // --- Member state ---

    client_options_t opt;
    boost::asio::io_context io;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type>
      work_guard; // keeps io_context alive until shutdown

    std::vector<std::thread> workers;                                  // I/O worker threads
    std::vector<std::shared_ptr<client_session_t>> sessions;           // all sessions
    std::vector<std::shared_ptr<client_session_t>> connected_sessions; // successfully connected

    std::mutex connected_mu; // guards connected_sessions

    // --- Connect tracking (atomics for cross-thread access) ---
    std::atomic<size_t> next_connect_idx;
    std::atomic<long> connect_active;
    std::atomic<long> connect_success;
    std::atomic<long> connect_fail;
    std::atomic<long> connect_completed;

    std::mutex connect_sched_mu; // serializes schedule_connects()
    std::mutex connect_mu;       // guards connect_cv wait
    std::condition_variable connect_cv;
    std::mutex completion_wait_mu;
    std::condition_variable completion_wait_cv;

    // --- Phase state (atomics read by I/O threads) ---
    std::atomic<int> mode; // phase_mode_t
    std::atomic<uint64_t> phase_end_ns;
    std::atomic<size_t> phase_size;
    std::atomic<long> outstanding_total; // global in-flight count

    std::atomic<uint64_t> seq_gen; // monotonic sequence for latency embedding

    // --- Measurement counters (reset per-case) ---
    std::atomic<long long> bytes_recv_measure;
    std::atomic<long> send_error_measure;
    std::atomic<long> recv_error_measure;
    std::atomic<long> timeout_error_measure;
    std::atomic<long> size_mismatch_measure;

    std::atomic<bool> collect_metrics; // true only during active window

    // --- RTT ring buffer ---
    std::unique_ptr<std::atomic<uint64_t>[]> rtt_samples_bits; // bit-cast doubles
    std::atomic<size_t> sample_overwrite_idx;                  // write cursor (wraps at capacity)
    boost::asio::ip::tcp::endpoint endpoint;
    loopback_bind_plan_t loopback_bind_plan;
};

#endif
