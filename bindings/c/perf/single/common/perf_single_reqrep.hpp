#ifndef PERF_SINGLE_REQREP_HPP
#define PERF_SINGLE_REQREP_HPP

#include "bench_common.hpp"
#include "perf_single_latency.hpp"
#include "perf_single_metric_header.hpp"
#include "perf_single_monitor.hpp"
#include "perf_single_phase.hpp"
#include "../../common/perf_zlink_part_helpers.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
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
        active_deadline (std::chrono::steady_clock::time_point::min ()),
        completed (0),
        in_flight (0),
        next_seq (1),
        fatal (false),
        capture_latency (false),
        latency ()
    {
    }

    uint32_t run_id;
    size_t msg_size;
    size_t latency_sample_cap;
    std::chrono::steady_clock::time_point active_deadline;
    std::atomic<unsigned long long> completed;
    std::atomic<int> in_flight;
    std::atomic<unsigned long long> next_seq;
    std::atomic<bool> fatal;
    bool capture_latency;
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
    const size_t fallback = 1000000;
    const char *value = std::getenv ("PERF_SINGLE_LATENCY_SAMPLE_CAP");
    if (!value || !*value || *value == '-')
        return fallback;

    char *end = NULL;
    errno = 0;
    const unsigned long long parsed = std::strtoull (value, &end, 10);
    if (errno != 0 || end == value || !end || *end != '\0'
        || parsed > static_cast<unsigned long long> (std::numeric_limits<size_t>::max ()))
        return fallback;
    return static_cast<size_t> (parsed);
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
              << ",latency," << std::fixed << std::setprecision (6)
              << (latency.mean_ns / 1000000.0) << std::endl;
    std::cout << "RESULT," << lib_type << "," << pattern << "," << transport << "," << size
              << ",latency_p95," << std::fixed << std::setprecision (6)
              << (latency.p95_ns / 1000000.0) << std::endl;
    std::cout << "RESULT," << lib_type << "," << pattern << "," << transport << "," << size
              << ",latency_p99," << std::fixed << std::setprecision (6)
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

inline void record_request_completion (request_state_t *state_,
                                       zlink_request_result_t result_,
                                       zlink_msg_t *parts_,
                                       size_t part_count_)
{
    if (!state_)
        return;

    const std::chrono::steady_clock::time_point completed_at =
      std::chrono::steady_clock::now ();

    if (result_ == ZLINK_REQUEST_OK && parts_
        && part_count_ == perf_measurement_part_count ()) {
        if (part_count_ == 2 && zlink_msg_size (&parts_[1]) != 0) {
            state_->fatal.store (true, std::memory_order_release);
            state_->in_flight.fetch_sub (1, std::memory_order_release);
            return;
        }
        perf_single_metric::header_t header;
        const zlink_msg_t &part = parts_[0];
        if (perf_single_metric::decode_payload_header (zlink_msg_data (const_cast<zlink_msg_t *> (&part)),
                                                       zlink_msg_size (const_cast<zlink_msg_t *> (&part)),
                                                       &header)
            && perf_single_metric::is_expected (header, state_->run_id,
                                                perf_single_metric::phase_active,
                                                state_->msg_size)
            && completed_at < state_->active_deadline) {
            const uint64_t now_ns = perf_single_metric::now_ns ();
            if (now_ns >= static_cast<uint64_t> (header.sent_ts_ns)) {
                if (state_->capture_latency) {
                    std::lock_guard<std::mutex> guard (state_->latency_mutex);
                    state_->latency.add (
                      static_cast<double> (now_ns - static_cast<uint64_t> (header.sent_ts_ns)));
                }
                state_->completed.fetch_add (1, std::memory_order_relaxed);
            }
        }
    } else if (result_ != ZLINK_REQUEST_TIMED_OUT) {
        if (bench_debug_enabled ())
            std::cerr << "[perf-single-reqrep] request callback failed result=" << result_
                      << std::endl;
        state_->fatal.store (true, std::memory_order_release);
    }
    state_->in_flight.fetch_sub (1, std::memory_order_release);
}

#if defined(PERF_ZLINK_LEGACY_REQUEST_CALLBACK_API)
inline void on_request_reply (zlink_request_result_t result_,
                              zlink_msg_t *parts_,
                              size_t part_count_,
                              void *userdata_)
{
    record_request_completion (static_cast<request_state_t *> (userdata_), result_, parts_,
                               part_count_);
}
#endif

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

#if defined(PERF_ZLINK_LEGACY_REQUEST_CALLBACK_API)
    state_->in_flight.fetch_add (1, std::memory_order_release);
    const zlink_submit_result_t rc =
      submit_fn_ (&part, timeout_ms_, on_request_reply, state_);
    if (rc == ZLINK_SUBMIT_OK)
        return submit_step_submitted;

    const int err = zlink_errno ();
    zlink_msg_close (&part);
    state_->in_flight.fetch_sub (1, std::memory_order_release);
#else
    zlink_completion_id_t completion_id = 0;
    const zlink_submit_result_t rc =
      submit_fn_ (&part, timeout_ms_, state_, &completion_id);
    if (rc == ZLINK_SUBMIT_OK && completion_id != 0) {
        state_->in_flight.fetch_add (1, std::memory_order_release);
        return submit_step_submitted;
    }

    const int err = zlink_errno ();
    if (rc == ZLINK_SUBMIT_OK) {
        if (bench_debug_enabled ())
            std::cerr << "[perf-single-reqrep] successful request returned zero completion id"
                      << std::endl;
        return submit_step_fatal;
    }
#endif
    if (rc == ZLINK_SUBMIT_BACKPRESSURED || err == EAGAIN || err == EWOULDBLOCK || err == EINTR
        || err == ETIMEDOUT || err == EHOSTUNREACH || err == ENOTCONN)
        return submit_step_blocked;
    if (bench_debug_enabled ())
        std::cerr << "[perf-single-reqrep] request submit failed rc=" << rc << " errno=" << err
                  << std::endl;
    return submit_step_fatal;
}

#if !defined(PERF_ZLINK_LEGACY_REQUEST_CALLBACK_API)
inline bool drain_request_completions (void *requester_, request_state_t *state_)
{
    if (!requester_ || !state_)
        return false;
    for (;;) {
        zlink_completion_t completion;
        std::memset (&completion, 0, sizeof (completion));
        completion.struct_size = sizeof (completion);
        const zlink_recv_result_t rc = zlink_completion_recv (
          requester_, &completion, ZLINK_RECV_FLAGS_DONTWAIT);
        if (rc == ZLINK_RECV_NO_DATA)
            return true;
        if (rc != ZLINK_RECV_OK)
            return false;
        // A DONTWAIT handshake send that met backpressure left a wait token
        // whose WRITABLE record shares this queue. Nothing waits on it here.
        if (completion.kind == ZLINK_COMPLETION_WRITABLE) {
            zlink_completion_close (&completion);
            continue;
        }
        const bool valid = completion.kind == ZLINK_COMPLETION_REQUEST
                           && completion.completion_id != 0
                           && completion.user_context == state_;
        if (valid) {
            record_request_completion (state_, completion.request_result,
                                       completion.reply_parts,
                                       completion.reply_part_count);
        }
        zlink_completion_close (&completion);
        if (!valid)
            return false;
    }
}
#endif

inline bool poll_completion_once (void *poller_,
                                  void *requester_,
                                  request_state_t *state_,
                                  long timeout_ms_)
{
    zlink_poller_event_t event;
    std::memset (&event, 0, sizeof (event));
    zlink_config_result_t error = ZLINK_CONFIG_OK;
    const int rc = zlink_poller_wait (poller_, &event, 1, timeout_ms_, &error);
    if (rc < 0)
        return zlink_errno () == EINTR;
#if defined(PERF_ZLINK_LEGACY_REQUEST_CALLBACK_API)
    (void) requester_;
    (void) state_;
    return true;
#else
    return drain_request_completions (requester_, state_);
#endif
}

template <typename SubmitFn>
inline bool run_request_phase (void *requester_,
                               request_state_t *state_,
                               std::vector<char> *payload_,
                               int duration_s_,
                               unsigned long long max_in_flight_,
                               bool capture_latency_,
                               SubmitFn submit_fn_,
                               void *poller_,
                               unsigned long long *completed_out_,
                               latency_stats_t *latency_out_)
{
    if (!requester_ || !state_ || !payload_ || !poller_ || !completed_out_ || !latency_out_)
        return false;

    state_->completed.store (0, std::memory_order_release);
    state_->in_flight.store (0, std::memory_order_release);
    state_->next_seq.store (1, std::memory_order_release);
    state_->fatal.store (false, std::memory_order_release);
    state_->capture_latency = capture_latency_;
    {
        std::lock_guard<std::mutex> guard (state_->latency_mutex);
        state_->latency = latency_stats_builder_t (state_->latency_sample_cap);
    }

    const uint32_t timeout_ms = resolve_request_timeout_ms ();
    const int drain_timeout_ms = resolve_completion_drain_timeout_ms ();
    const auto deadline = std::chrono::steady_clock::now ()
                          + std::chrono::seconds (std::max (1, duration_s_));
    state_->active_deadline = deadline;
    while (std::chrono::steady_clock::now () < deadline
           && !state_->fatal.load (std::memory_order_acquire)) {
        bool submitted_any = false;
        unsigned int submitted_since_progress = 0;
        // Submit continuously until the public request API reports
        // backpressure.  The socket HWM, rather than a runner-side window,
        // owns the outstanding request depth.
        while (std::chrono::steady_clock::now () < deadline) {
            if (max_in_flight_ > 0
                && static_cast<unsigned long long> (
                     state_->in_flight.load (std::memory_order_acquire))
                     >= max_in_flight_)
                break;
            const submit_step_t step = submit_request (state_, payload_, submit_fn_, timeout_ms);
            if (step == submit_step_submitted) {
                submitted_any = true;
                ++submitted_since_progress;
                if (submitted_since_progress >= 64) {
                    submitted_since_progress = 0;
                    if (!poll_completion_once (poller_, requester_, state_, 0)) {
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
        if (!poll_completion_once (poller_, requester_, state_, 50)) {
            state_->fatal.store (true, std::memory_order_release);
            break;
        }
    }

    const auto drain_deadline = std::chrono::steady_clock::now ()
                                + std::chrono::milliseconds (drain_timeout_ms);
    while (state_->in_flight.load (std::memory_order_acquire) > 0
           && std::chrono::steady_clock::now () < drain_deadline) {
        if (!poll_completion_once (poller_, requester_, state_, 50)) {
            state_->fatal.store (true, std::memory_order_release);
            break;
        }
    }

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
    if (capture_latency_) {
        std::lock_guard<std::mutex> guard (state_->latency_mutex);
        if (state_->latency.count () == 0)
            return false;
        *latency_out_ = state_->latency.snapshot ();
    } else {
        *latency_out_ = latency_stats_t ();
    }
    return true;
}

inline int latency_phase_duration_seconds ()
{
    return 1;
}

inline unsigned long long latency_phase_max_in_flight ()
{
    return 1;
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

    // The caller owns the replier thread, so it closes this poller after the
    // replier has stopped. Destroying a completion poller while the peer can
    // still publish replies can stall the measured shutdown path.
    *completion_poller_out_ = poller;

    latency_stats_t ignored_latency;
    if (!run_request_phase (requester_, state_, payload_, duration_s_, 0, false, submit_fn_,
                            poller, completed_out_, &ignored_latency)) {
        return false;
    }

    // Phase 1 has drained every saturated request completion. Phase 2 keeps
    // exactly one request in flight and timestamps the next request only after
    // the preceding REQUEST completion has been received.
    unsigned long long latency_completed = 0;
    return run_request_phase (
      requester_, state_, payload_, latency_phase_duration_seconds (),
      latency_phase_max_in_flight (), true, submit_fn_, poller, &latency_completed,
      latency_out_);
}

inline int recv_router_request (void *router_,
                                zlink_routing_id_t *source_rid_out_,
                                uint64_t *reply_token_out_,
                                zlink_msg_t *payload_out_)
{
    if (!router_ || !source_rid_out_ || !reply_token_out_ || !payload_out_)
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
      zlink_router_recv_part (router_, &source_rid, reply_token_out_, payload_out_,
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
    const bool is_stop = is_stop_token (zlink_msg_data (payload_out_), zlink_msg_size (payload_out_));
    if (!source_rid || source_rid->size == 0
        || (is_stop ? has_more != ZLINK_PART_FINAL
                    : !perf_zlink_recv_measurement_tail (
                        router_, has_more, ZLINK_RECV_FLAGS_NONE, perf_zlink_recv_next_router))) {
        if (bench_debug_enabled ())
            std::cerr << "[perf-single-reqrep] invalid router recv metadata source_rid="
                      << (source_rid ? static_cast<unsigned int> (source_rid->size) : 0)
                      << " has_more=" << has_more
                      << " reply_token=" << *reply_token_out_ << std::endl;
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
        uint64_t reply_token = 0;
        zlink_msg_t request;
        const int recv_rc = recv_router_request (router_, &source_rid, &reply_token, &request);
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
        if (reply_token == 0) {
            zlink_msg_close (&request);
            continue;
        }

        if (perf_measurement_part_count () == 2u) {
#if defined(PERF_ZLINK_LEGACY_REQUEST_CALLBACK_API)
            zlink_submit_result_t payload_rc = zlink_router_reply_part (
#else
            zlink_submit_result_t payload_rc = zlink_reply_part (
#endif
              router_, &source_rid, reply_token, &request, ZLINK_PART_MORE);
            if (payload_rc != ZLINK_SUBMIT_OK) {
                state_->fatal.store (true, std::memory_order_release);
                return;
            }
            zlink_submit_result_t final_rc = ZLINK_SUBMIT_BACKPRESSURED;
            const auto retry_deadline = std::chrono::steady_clock::now ()
                                      + std::chrono::milliseconds (
                                        resolve_completion_drain_timeout_ms ());
            while (final_rc == ZLINK_SUBMIT_BACKPRESSURED
                   && std::chrono::steady_clock::now () < retry_deadline
                   && !state_->stop.load (std::memory_order_acquire)) {
                zlink_msg_t empty_part;
                if (zlink_msg_init (&empty_part) != 0) {
                    state_->fatal.store (true, std::memory_order_release);
                    return;
                }
#if defined(PERF_ZLINK_LEGACY_REQUEST_CALLBACK_API)
                final_rc = zlink_router_reply_part (
#else
                final_rc = zlink_reply_part (
#endif
                  router_, &source_rid, reply_token, &empty_part, ZLINK_PART_FINAL);
                if (final_rc == ZLINK_SUBMIT_BACKPRESSURED)
                    std::this_thread::yield ();
            }
            if (final_rc != ZLINK_SUBMIT_OK) {
                state_->fatal.store (true, std::memory_order_release);
                return;
            }
            continue;
        }

        // The reply submit consumes its part even when it reports
        // backpressure. Keep a shared-storage copy so a retry preserves the
        // request metric payload without copying the bytes on the hot path.
        zlink_msg_t retry_template;
        const bool retry_template_initialized = zlink_msg_init (&retry_template) == 0;
        if (!retry_template_initialized
            || zlink_msg_copy (&retry_template, &request) != ZLINK_CONFIG_OK) {
            if (retry_template_initialized)
                zlink_msg_close (&retry_template);
            zlink_msg_close (&request);
            state_->fatal.store (true, std::memory_order_release);
            return;
        }

#if defined(PERF_ZLINK_LEGACY_REQUEST_CALLBACK_API)
        zlink_submit_result_t reply_rc = zlink_router_reply_part (
#else
        zlink_submit_result_t reply_rc = zlink_reply_part (
#endif
          router_, &source_rid, reply_token, &request, ZLINK_PART_FINAL);
        if (reply_rc == ZLINK_SUBMIT_BACKPRESSURED) {
            const auto retry_deadline =
              std::chrono::steady_clock::now ()
              + std::chrono::milliseconds (resolve_completion_drain_timeout_ms ());
            while (reply_rc == ZLINK_SUBMIT_BACKPRESSURED
                   && std::chrono::steady_clock::now () < retry_deadline
                   && !state_->stop.load (std::memory_order_acquire)) {
                zlink_msg_t retry;
                const bool retry_initialized = zlink_msg_init (&retry) == 0;
                if (!retry_initialized
                    || zlink_msg_copy (&retry, &retry_template) != ZLINK_CONFIG_OK) {
                    if (retry_initialized)
                        zlink_msg_close (&retry);
                    zlink_msg_close (&retry_template);
                    state_->fatal.store (true, std::memory_order_release);
                    return;
                }
                std::this_thread::yield ();
#if defined(PERF_ZLINK_LEGACY_REQUEST_CALLBACK_API)
                reply_rc = zlink_router_reply_part (
#else
                reply_rc = zlink_reply_part (
#endif
                  router_, &source_rid, reply_token, &retry, ZLINK_PART_FINAL);
            }
        }
        zlink_msg_close (&retry_template);
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
