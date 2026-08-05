#ifndef PERF_SINGLE_REQREP_HPP
#define PERF_SINGLE_REQREP_HPP

#include "bench_common.hpp"
#include "perf_single_latency.hpp"
#include "perf_single_metric_header.hpp"
#include "perf_single_monitor.hpp"
#include "perf_single_phase.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

namespace perf_single_reqrep
{

struct request_state_t
{
    request_state_t () :
        run_id (0),
        msg_size (0),
        latency_sample_cap (0),
        completed (0),
        in_flight (0),
        next_seq (1),
        fatal (false),
        latency ()
    {
    }

    uint32_t run_id;
    size_t msg_size;
    size_t latency_sample_cap;
    std::atomic<unsigned long long> completed;
    std::atomic<int> in_flight;
    std::atomic<unsigned long long> next_seq;
    std::atomic<bool> fatal;
    std::mutex latency_mutex;
    latency_stats_builder_t latency;
};

struct reply_state_t
{
    reply_state_t () : received (0), replied (0), stop (false), fatal (false) {}

    std::atomic<unsigned long long> received;
    std::atomic<unsigned long long> replied;
    std::atomic<bool> stop;
    std::atomic<bool> fatal;
};

enum submit_step_t
{
    submit_step_submitted = 0,
    submit_step_blocked = 1,
    submit_step_fatal = 2
};

inline uint32_t resolve_request_timeout_ms ()
{
    return static_cast<uint32_t> (parse_positive_env ("PERF_SINGLE_REQREP_TIMEOUT_MS", 200));
}

inline int resolve_completion_drain_timeout_ms ()
{
    return parse_positive_env ("PERF_SINGLE_REQREP_DRAIN_TIMEOUT_MS", 10000);
}

inline size_t resolve_latency_sample_cap ()
{
    return static_cast<size_t> (parse_positive_env ("PERF_SINGLE_LATENCY_SAMPLE_CAP", 4000000));
}

inline void print_reqrep_result (const std::string &lib_type,
                                 const std::string &pattern,
                                 const std::string &transport,
                                 size_t size,
                                 double throughput,
                                 const latency_stats_t &latency)
{
    const double bandwidth_mb_s = (throughput * static_cast<double> (size) * 2.0) / 1000000.0;
    std::cout << "RESULT," << lib_type << "," << pattern << "," << transport << "," << size
              << ",throughput," << std::fixed << std::setprecision (3) << throughput << std::endl;
    std::cout << "RESULT," << lib_type << "," << pattern << "," << transport << "," << size
              << ",bandwidth," << std::fixed << std::setprecision (3) << bandwidth_mb_s
              << std::endl;
    std::cout << "RESULT," << lib_type << "," << pattern << "," << transport << "," << size
              << ",latency," << std::fixed << std::setprecision (3)
              << (latency.mean_ns / 1000000.0) << std::endl;
    std::cout << "RESULT," << lib_type << "," << pattern << "," << transport << "," << size
              << ",latency_p95," << std::fixed << std::setprecision (3)
              << (latency.p95_ns / 1000000.0) << std::endl;
    std::cout << "RESULT," << lib_type << "," << pattern << "," << transport << "," << size
              << ",latency_p99," << std::fixed << std::setprecision (3)
              << (latency.p99_ns / 1000000.0) << std::endl;
}

inline bool init_routing_id_text (const char *text_, zlink_routing_id_t *rid_out_)
{
    if (!text_ || !rid_out_)
        return false;
    const size_t len = std::strlen (text_);
    if (len > sizeof (rid_out_->data))
        return false;
    std::memset (rid_out_, 0, sizeof (*rid_out_));
    rid_out_->size = static_cast<uint8_t> (len);
    if (len > 0)
        std::memcpy (rid_out_->data, text_, len);
    return true;
}

inline void on_request_reply (zlink_request_result_t result_,
                              zlink_msg_t *parts_,
                              size_t part_count_,
                              void *userdata_)
{
    request_state_t *state = static_cast<request_state_t *> (userdata_);
    if (!state)
        return;

    if (result_ == ZLINK_REQUEST_OK && parts_ && part_count_ > 0) {
        perf_single_metric::header_t header;
        const zlink_msg_t &part = parts_[part_count_ - 1];
        if (perf_single_metric::decode_payload_header (zlink_msg_data (const_cast<zlink_msg_t *> (&part)),
                                                       zlink_msg_size (const_cast<zlink_msg_t *> (&part)),
                                                       &header)
            && perf_single_metric::is_expected (header, state->run_id,
                                                perf_single_metric::phase_active,
                                                state->msg_size)) {
            const uint64_t now_ns = perf_single_metric::now_ns ();
            if (now_ns >= static_cast<uint64_t> (header.sent_ts_ns)) {
                {
                    std::lock_guard<std::mutex> guard (state->latency_mutex);
                    state->latency.add (
                      static_cast<double> (now_ns - static_cast<uint64_t> (header.sent_ts_ns)));
                }
                state->completed.fetch_add (1, std::memory_order_relaxed);
            }
        }
    } else if (result_ != ZLINK_REQUEST_TIMED_OUT) {
        if (bench_debug_enabled ())
            std::cerr << "[perf-single-reqrep] request callback failed result=" << result_
                      << std::endl;
        state->fatal.store (true, std::memory_order_release);
    }
    state->in_flight.fetch_sub (1, std::memory_order_release);
}

template <typename SubmitFn>
inline submit_step_t submit_request (request_state_t *state_,
                                     std::vector<char> *payload_,
                                     SubmitFn submit_fn_,
                                     uint32_t timeout_ms_)
{
    if (!state_ || !payload_)
        return submit_step_fatal;
    const unsigned long long seq = state_->next_seq.fetch_add (1, std::memory_order_relaxed);
    if (!perf_single_metric::stamp_payload (payload_->data (), payload_->size (), state_->run_id,
                                            perf_single_metric::phase_active, state_->msg_size, seq,
                                            perf_single_metric::now_ns ())) {
        return submit_step_fatal;
    }

    zlink_msg_t part;
    if (zlink_msg_init_size (&part, payload_->size ()) != 0)
        return submit_step_fatal;
    if (!payload_->empty ())
        std::memcpy (zlink_msg_data (&part), payload_->data (), payload_->size ());

    state_->in_flight.fetch_add (1, std::memory_order_release);
    const zlink_submit_result_t rc = submit_fn_ (&part, timeout_ms_, on_request_reply, state_);
    if (rc == ZLINK_SUBMIT_OK)
        return submit_step_submitted;

    const int err = zlink_errno ();
    zlink_msg_close (&part);
    state_->in_flight.fetch_sub (1, std::memory_order_release);
    if (rc == ZLINK_SUBMIT_BACKPRESSURED || err == EAGAIN || err == EWOULDBLOCK || err == EINTR
        || err == ETIMEDOUT || err == EHOSTUNREACH || err == ENOTCONN)
        return submit_step_blocked;
    if (bench_debug_enabled ())
        std::cerr << "[perf-single-reqrep] request submit failed rc=" << rc << " errno=" << err
                  << std::endl;
    return submit_step_fatal;
}

inline bool poll_completion_once (void *poller_, long timeout_ms_)
{
    zlink_poller_event_t event;
    std::memset (&event, 0, sizeof (event));
    zlink_config_result_t error = ZLINK_CONFIG_OK;
    const int rc = zlink_poller_wait (poller_, &event, 1, timeout_ms_, &error);
    if (rc < 0)
        return zlink_errno () == EINTR;
    return true;
}

template <typename SubmitFn>
inline bool run_requester (void *requester_,
                           request_state_t *state_,
                           std::vector<char> *payload_,
                           int duration_s_,
                           SubmitFn submit_fn_,
                           void **completion_poller_out_,
                           unsigned long long *completed_out_,
                           latency_stats_t *latency_out_)
{
    if (!requester_ || !state_ || !payload_ || !completion_poller_out_ || !completed_out_
        || !latency_out_)
        return false;
    *completion_poller_out_ = NULL;

    void *poller = zlink_poller_new ();
    if (!poller)
        return false;
    if (zlink_poller_add (poller, requester_, NULL, ZLINK_POLLCOMPLETION) != ZLINK_CONFIG_OK) {
        (void) zlink_poller_destroy (&poller);
        return false;
    }

    state_->completed.store (0, std::memory_order_release);
    state_->in_flight.store (0, std::memory_order_release);
    state_->next_seq.store (1, std::memory_order_release);
    state_->fatal.store (false, std::memory_order_release);
    state_->latency = latency_stats_builder_t (state_->latency_sample_cap);

    const uint32_t timeout_ms = resolve_request_timeout_ms ();
    const int drain_timeout_ms = resolve_completion_drain_timeout_ms ();
    constexpr size_t pipeline_budget_bytes = 768u * 1024u;
    const size_t message_bytes = std::max<size_t> (1, state_->msg_size);
    const int max_in_flight = static_cast<int> (std::max<size_t> (
      1, std::min<size_t> (64, pipeline_budget_bytes / message_bytes)));
    const auto deadline = std::chrono::steady_clock::now ()
                          + std::chrono::seconds (std::max (1, duration_s_));
    while (std::chrono::steady_clock::now () < deadline
           && !state_->fatal.load (std::memory_order_acquire)) {
        bool submitted_any = false;
        unsigned int submitted_since_progress = 0;
        // This is the measured request hot path. Bound both request count and
        // payload bytes below the balanced HWM budget.
        while (state_->in_flight.load (std::memory_order_acquire) < max_in_flight
               && std::chrono::steady_clock::now () < deadline) {
            const submit_step_t step = submit_request (state_, payload_, submit_fn_, timeout_ms);
            if (step == submit_step_submitted) {
                submitted_any = true;
                ++submitted_since_progress;
                if (submitted_since_progress >= 64) {
                    submitted_since_progress = 0;
                    if (!poll_completion_once (poller, 0)) {
                        state_->fatal.store (true, std::memory_order_release);
                        break;
                    }
                }
                continue;
            }
            if (step == submit_step_blocked)
                break;
            state_->fatal.store (true, std::memory_order_release);
            break;
        }
        if (state_->fatal.load (std::memory_order_acquire))
            break;
        if (!submitted_any && state_->in_flight.load (std::memory_order_acquire) == 0) {
            (void) perf_socket_poll (NULL, 0, 1);
            continue;
        }
        if (!poll_completion_once (poller, 50)) {
            state_->fatal.store (true, std::memory_order_release);
            break;
        }
    }

    const auto drain_deadline = std::chrono::steady_clock::now ()
                                + std::chrono::milliseconds (drain_timeout_ms);
    while (state_->in_flight.load (std::memory_order_acquire) > 0
           && std::chrono::steady_clock::now () < drain_deadline) {
        if (!poll_completion_once (poller, 50)) {
            state_->fatal.store (true, std::memory_order_release);
            break;
        }
    }

    // The caller owns the replier thread, so it closes this poller after the
    // replier has stopped. Destroying a completion poller while the peer can
    // still publish replies can stall the measured shutdown path.
    *completion_poller_out_ = poller;
    if (state_->fatal.load (std::memory_order_acquire)) {
        if (bench_debug_enabled ())
            std::cerr << "[perf-single-reqrep] requester fatal after completion poll"
                      << std::endl;
        return false;
    }
    if (state_->in_flight.load (std::memory_order_acquire) != 0) {
        if (bench_debug_enabled ())
            std::cerr << "[perf-single-reqrep] completion drain timed out in_flight="
                      << state_->in_flight.load (std::memory_order_acquire)
                      << " completed=" << state_->completed.load (std::memory_order_acquire)
                      << std::endl;
        return false;
    }

    *completed_out_ = state_->completed.load (std::memory_order_relaxed);
    if (*completed_out_ == 0) {
        if (bench_debug_enabled ())
            std::cerr << "[perf-single-reqrep] no completed request" << std::endl;
        return false;
    }
    {
        std::lock_guard<std::mutex> guard (state_->latency_mutex);
        if (state_->latency.count () == 0)
            return false;
        *latency_out_ = state_->latency.snapshot ();
    }
    return true;
}

inline int recv_router_request (void *router_,
                                zlink_routing_id_t *source_rid_out_,
                                uint64_t *request_seq_out_,
                                zlink_msg_t *payload_out_)
{
    if (!router_ || !source_rid_out_ || !request_seq_out_ || !payload_out_)
        return -1;

    const zlink_routing_id_t *source_rid = NULL;
    zlink_part_flag_t has_more = ZLINK_PART_FINAL;
    if (zlink_msg_init (payload_out_) != 0) {
        if (bench_debug_enabled ())
            std::cerr << "[perf-single-reqrep] payload init failed errno="
                      << zlink_errno () << std::endl;
        return -1;
    }
    const zlink_recv_result_t rc =
      zlink_router_recv_part (router_, &source_rid, request_seq_out_, payload_out_,
                              &has_more, ZLINK_RECV_FLAGS_NONE);
    if (rc != ZLINK_RECV_OK) {
        zlink_msg_close (payload_out_);
        if (rc == ZLINK_RECV_NO_DATA)
            return 0;
        if (bench_debug_enabled ())
            std::cerr << "[perf-single-reqrep] zlink_router_recv_part rc=" << rc
                      << " errno=" << zlink_errno () << std::endl;
        return -1;
    }
    if (!source_rid || source_rid->size == 0 || has_more != ZLINK_PART_FINAL) {
        if (bench_debug_enabled ())
            std::cerr << "[perf-single-reqrep] invalid router recv metadata source_rid="
                      << (source_rid ? static_cast<unsigned int> (source_rid->size) : 0)
                      << " has_more=" << has_more
                      << " request_seq=" << *request_seq_out_ << std::endl;
        zlink_msg_close (payload_out_);
        return -1;
    }
    std::memset (source_rid_out_, 0, sizeof (*source_rid_out_));
    source_rid_out_->size = source_rid->size;
    std::memcpy (source_rid_out_->data, source_rid->data, source_rid->size);
    return 1;
}

inline void run_router_replier (void *router_, reply_state_t *state_)
{
    if (!router_ || !state_)
        return;
    while (!state_->stop.load (std::memory_order_acquire)) {
        zlink_routing_id_t source_rid;
        uint64_t request_seq = 0;
        zlink_msg_t request;
        const int recv_rc = recv_router_request (router_, &source_rid, &request_seq, &request);
        if (recv_rc == 0)
            continue;
        if (recv_rc < 0) {
            if (bench_debug_enabled ())
                std::cerr << "[perf-single-reqrep] router recv failed rc=" << recv_rc
                          << " errno=" << zlink_errno () << std::endl;
            state_->fatal.store (true, std::memory_order_release);
            return;
        }
        state_->received.fetch_add (1, std::memory_order_relaxed);
        if (is_stop_token (zlink_msg_data (&request), zlink_msg_size (&request))) {
            zlink_msg_close (&request);
            return;
        }
        if (request_seq == 0) {
            zlink_msg_close (&request);
            continue;
        }

        // Zero-copy on the first attempt: the received payload is handed
        // straight back as the reply. The submit entry consumes the message
        // either way, so a backpressured retry rebuilds a same-size payload
        // instead of keeping a copy of every reply on the hot path.
        const size_t reply_size = zlink_msg_size (&request);
        zlink_submit_result_t reply_rc = zlink_router_reply_part (
          router_, &source_rid, request_seq, &request, ZLINK_PART_FINAL);
        if (reply_rc == ZLINK_SUBMIT_BACKPRESSURED) {
            //  Only a backpressured reply pays for the clock read and the
            //  rebuilt payload, so the measured path keeps one submit call.
            const auto retry_deadline =
              std::chrono::steady_clock::now ()
              + std::chrono::milliseconds (resolve_completion_drain_timeout_ms ());
            while (reply_rc == ZLINK_SUBMIT_BACKPRESSURED
                   && std::chrono::steady_clock::now () < retry_deadline
                   && !state_->stop.load (std::memory_order_acquire)) {
                zlink_msg_t retry;
                if (zlink_msg_init_size (&retry, reply_size) != 0) {
                    state_->fatal.store (true, std::memory_order_release);
                    return;
                }
                std::this_thread::yield ();
                reply_rc = zlink_router_reply_part (
                  router_, &source_rid, request_seq, &retry, ZLINK_PART_FINAL);
            }
        }
        if (reply_rc != ZLINK_SUBMIT_OK) {
            if (bench_debug_enabled ())
                std::cerr << "[perf-single-reqrep] router reply failed rc=" << reply_rc
                          << " errno=" << zlink_errno () << std::endl;
            state_->fatal.store (true, std::memory_order_release);
            return;
        }

        state_->replied.fetch_add (1, std::memory_order_relaxed);
    }
}

inline bool send_stop_to_router (void *dealer_)
{
    for (int i = 0; i < 100; ++i) {
        zlink_msg_t part;
        if (zlink_msg_init_size (&part, stop_token_size ()) != 0)
            return false;
        std::memcpy (zlink_msg_data (&part), k_stop_token, stop_token_size ());
        if (perf_zlink_send_parts (dealer_, &part, 1, ZLINK_SEND_FLAGS_NONE) == 0)
            return true;
        const int err = zlink_errno ();
        zlink_msg_close (&part);
        if (err != EAGAIN && err != EWOULDBLOCK && err != EINTR && err != ETIMEDOUT)
            return false;
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    return false;
}

inline bool send_routed_stop_to_router (void *router_, const zlink_routing_id_t *target_rid_)
{
    if (!router_ || !target_rid_)
        return false;
    for (int i = 0; i < 100; ++i) {
        zlink_msg_t part;
        if (zlink_msg_init_size (&part, stop_token_size ()) != 0)
            return false;
        std::memcpy (zlink_msg_data (&part), k_stop_token, stop_token_size ());
        if (perf_zlink_send_rid_parts (router_, target_rid_, &part, 1, ZLINK_SEND_FLAGS_NONE) == 0)
            return true;
        const int err = zlink_errno ();
        zlink_msg_close (&part);
        if (err != EAGAIN && err != EWOULDBLOCK && err != EINTR && err != ETIMEDOUT
            && err != EHOSTUNREACH && err != ENOTCONN)
            return false;
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    return false;
}

} // namespace perf_single_reqrep

#endif
