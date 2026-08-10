#ifndef PERF_CPP_SINGLE_REQREP_HPP
#define PERF_CPP_SINGLE_REQREP_HPP

#include "perf_single_common.hpp"

#include <atomic>
#include <mutex>

namespace perf
{
namespace single
{

struct reqrep_config_t
{
    const char *pattern;
    bool routed_request;
};

struct reqrep_state_t
{
    reqrep_state_t (uint32_t run_id_, size_t msg_size_) :
        run_id (run_id_),
        msg_size (msg_size_),
        completed (0),
        in_flight (0),
        fatal (false),
        latency (resolve_single_latency_sample_cap ())
    {
    }

    uint32_t run_id;
    size_t msg_size;
    std::atomic<unsigned long long> completed;
    std::atomic<unsigned long long> in_flight;
    std::atomic<bool> fatal;
    std::mutex latency_mutex;
    latency_stats_builder_t latency;
};

inline bool reqrep_transient_errno (int err_)
{
    return err_ == EAGAIN || err_ == EWOULDBLOCK || err_ == EINTR || err_ == ETIMEDOUT
           || err_ == EHOSTUNREACH || err_ == ENOTCONN;
}

inline void emit_reqrep_result (const std::string &lib_name_,
                                const char *pattern_,
                                const std::string &transport_,
                                size_t msg_size_,
                                double throughput_,
                                const latency_stats_t &latency_)
{
    const double bandwidth = throughput_ * static_cast<double> (msg_size_) * 2.0 / 1000000.0;
    std::cout << "RESULT," << lib_name_ << "," << pattern_ << "," << transport_ << ","
              << msg_size_ << ",throughput," << std::fixed << std::setprecision (3)
              << throughput_ << std::endl;
    std::cout << "RESULT," << lib_name_ << "," << pattern_ << "," << transport_ << ","
              << msg_size_ << ",bandwidth," << bandwidth << std::endl;
    std::cout << "RESULT," << lib_name_ << "," << pattern_ << "," << transport_ << ","
              << msg_size_ << ",latency," << latency_.mean_ns / 1000000.0 << std::endl;
    std::cout << "RESULT," << lib_name_ << "," << pattern_ << "," << transport_ << ","
              << msg_size_ << ",latency_p95," << latency_.p95_ns / 1000000.0 << std::endl;
    std::cout << "RESULT," << lib_name_ << "," << pattern_ << "," << transport_ << ","
              << msg_size_ << ",latency_p99," << latency_.p99_ns / 1000000.0 << std::endl;
}

inline bool run_reqrep_pattern (const reqrep_config_t &config_,
                                const std::string &transport_,
                                size_t msg_size_,
                                const std::string &lib_name_)
{
    if (!transport_available (transport_)) {
        std::cout << "UNSUPPORTED," << lib_name_ << "," << config_.pattern << "," << transport_
                  << std::endl;
        return true;
    }

    ctx_guard_t ctx;
    socket_guard_t server (ctx, zlink::socket_type::router);
    socket_guard_t client (ctx, config_.routed_request ? zlink::socket_type::router
                                                       : zlink::socket_type::dealer);
    if (!ctx.valid () || !server.valid () || !client.valid ())
        return false;

    const zlink::routing_id_t server_rid = zlink::routing_id_t::from (std::string ("SERVER"));
    (void) server.sock ().set_routing_id ("SERVER");
    if (config_.routed_request) {
        (void) client.sock ().set_routing_id ("CLIENT");
        (void) client.sock ().set (perf::options::router_options::mandatory, 1);
        (void) server.sock ().set (perf::options::router_options::mandatory, 1);
    }
    if (!apply_single_auto_hwm_msg_unit (ctx, msg_size_) || !recalculate_single_auto_hwm (ctx)
        || !setup_connected_pair (server.sock (), client.sock (), transport_,
                                  lib_name_ + "_" + config_.pattern))
        return false;
    if (config_.routed_request
        && !complete_router_router_handshake (server.sock (), client.sock (), server_rid))
        return false;

    const size_t payload_size = std::max<size_t> (msg_size_, perf_single_metric::header_size ());
    std::vector<char> payload (payload_size, 'r');
    const int duration_s = std::max (1, resolve_single_duration_seconds ());
    const int request_timeout_ms = parse_positive_env ("PERF_SINGLE_REQREP_TIMEOUT_MS", 200);
    const int drain_timeout_ms = parse_positive_env ("PERF_SINGLE_REQREP_DRAIN_TIMEOUT_MS", 10000);
    reqrep_state_t state (1U, msg_size_);
    std::atomic<bool> server_ok (true);

    std::thread server_thread ([&] () {
        while (true) {
            zlink::received_t received;
            const int rc = server.sock ().receive (received, 0);
            if (rc != 0) {
                if (errno == EAGAIN || errno == EINTR)
                    continue;
                server_ok.store (false, std::memory_order_release);
                return;
            }
            if (received.parts ().empty ())
                continue;
            if (is_stop_token_message (received.parts ().front ()))
                return;
            if (!received.request_seq ().has_value ())
                continue;
            try {
                zlink::message_t &part = received.parts ().front ();
                bool replied = false;
                const auto retry_deadline =
                  std::chrono::steady_clock::now ()
                  + std::chrono::milliseconds (drain_timeout_ms);
                while (!replied && std::chrono::steady_clock::now () < retry_deadline) {
                    try {
                        std::move (received.reply ().message (part)).submit ();
                        replied = true;
                    }
                    catch (const zlink::binding_error_t &err) {
                        if (!reqrep_transient_errno (err.internal_errno ())) {
                            server_ok.store (false, std::memory_order_release);
                            return;
                        }
                        std::this_thread::yield ();
                    }
                }
                if (!replied) {
                    server_ok.store (false, std::memory_order_release);
                    return;
                }
            }
            catch (const zlink::binding_error_t &err) {
                if (reqrep_transient_errno (err.internal_errno ()))
                    continue;
                server_ok.store (false, std::memory_order_release);
                return;
            }
        }
    });

    zlink::poller_t poller;
    client.sock ().poller_add (poller, zlink::poll_event_flag_t::pollcompletion);
    constexpr size_t pipeline_budget_bytes = 768u * 1024u;
    const size_t message_bytes = std::max<size_t> (1, msg_size_);
    const unsigned long long max_in_flight = std::max<size_t> (
      1, std::min<size_t> (64, pipeline_budget_bytes / message_bytes));
    uint64_t seq = 1;
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (duration_s);
    while (std::chrono::steady_clock::now () < deadline
           && !state.fatal.load (std::memory_order_acquire)) {
        // This is the measured request hot path. Match the C reference's
        // count-and-byte bound so expired requests cannot accumulate in queues.
        for (unsigned int burst = 0;
             burst < max_in_flight
             && state.in_flight.load (std::memory_order_acquire) < max_in_flight
             && std::chrono::steady_clock::now () < deadline;
             ++burst) {
            if (!perf_single_metric::stamp_payload (payload.data (), payload.size (), state.run_id,
                                                    perf_single_metric::phase_active,
                                                    state.msg_size, seq,
                                                    perf_single_metric::now_ns ())) {
                state.fatal.store (true, std::memory_order_release);
                break;
            }
            zlink::message_t part = message_from_payload (payload.data (), payload.size ());
            if (!part.valid ()) {
                state.fatal.store (true, std::memory_order_release);
                break;
            }
            state.in_flight.fetch_add (1, std::memory_order_release);
            try {
                const auto callback = [&state] (zlink::request_result_t result,
                                                std::vector<zlink::message_t> parts) {
                    if (result == zlink::request_result_t::ok && !parts.empty ()) {
                        perf_single_metric::header_t header;
                        if (perf_single_metric::decode_payload_header (
                              parts.back ().data (), parts.back ().size (), &header)
                            && perf_single_metric::is_expected (
                              header, state.run_id, perf_single_metric::phase_active,
                              state.msg_size)) {
                            const uint64_t now = perf_single_metric::now_ns ();
                            std::lock_guard<std::mutex> lock (state.latency_mutex);
                            state.latency.add (
                              perf_single_metric::elapsed_latency_ns (now, header.sent_ts_ns));
                            state.completed.fetch_add (1, std::memory_order_relaxed);
                        }
                    } else if (result != zlink::request_result_t::timed_out) {
                        state.fatal.store (true, std::memory_order_release);
                    }
                    state.in_flight.fetch_sub (1, std::memory_order_release);
                };
                const bool ok = config_.routed_request
                                  ? client.sock ().request (
                                      server_rid, part, std::chrono::milliseconds (request_timeout_ms),
                                      static_cast<int> (zlink::send_flags_t::none), callback)
                                  : client.sock ().request (
                                      part, std::chrono::milliseconds (request_timeout_ms),
                                      static_cast<int> (zlink::send_flags_t::none), callback);
                if (!ok) {
                    state.in_flight.fetch_sub (1, std::memory_order_release);
                    break;
                }
                ++seq;
            }
            catch (const zlink::submit_error_t &err) {
                state.in_flight.fetch_sub (1, std::memory_order_release);
                if (err.result () == zlink::submit_result_t::backpressured
                    || reqrep_transient_errno (err.internal_errno ()))
                    break;
                state.fatal.store (true, std::memory_order_release);
                break;
            }
        }
        zlink::poll_event_t event;
        (void) poller.wait (&event, 1, std::chrono::milliseconds (50));
    }

    const auto drain_deadline =
      std::chrono::steady_clock::now () + std::chrono::milliseconds (drain_timeout_ms);
    while (state.in_flight.load (std::memory_order_acquire) > 0
           && std::chrono::steady_clock::now () < drain_deadline) {
        zlink::poll_event_t event;
        (void) poller.wait (&event, 1, std::chrono::milliseconds (50));
    }
    const bool stop_ok = config_.routed_request
                           ? send_stop_token_blocking (client.sock (), server_rid)
                           : send_stop_token_blocking (client.sock ());
    server_thread.join ();

    const unsigned long long completed = state.completed.load (std::memory_order_acquire);
    if (!stop_ok || !server_ok.load (std::memory_order_acquire)
        || state.fatal.load (std::memory_order_acquire)
        || state.in_flight.load (std::memory_order_acquire) != 0 || completed == 0
        || state.latency.count () == 0)
        return false;

    const latency_stats_t latency = state.latency.snapshot ();
    emit_reqrep_result (lib_name_, config_.pattern, transport_, msg_size_,
                        static_cast<double> (completed) / duration_s, latency);
    return true;
}

} // namespace single
} // namespace perf

#endif
