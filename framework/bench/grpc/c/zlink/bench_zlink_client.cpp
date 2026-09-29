#include "../common/bench_common.hpp"

#include "raw_wire.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>

#include <unistd.h>

namespace
{
constexpr const char *k_request_envelope =
  "{\"kind\":1,\"channelName\":\"bench\",\"messageName\":\"BenchPayload\",\"contentType\":\"application/x-protobuf\",\"correlationId\":null,\"deadline\":null,\"topic\":null,\"errorCode\":null,\"errorMessage\":null,\"source\":null}";

struct callback_state_t
{
    std::atomic<uint64_t> completed {0};
    std::atomic<uint64_t> errors {0};
    std::atomic<uint64_t> outstanding {0};
    std::atomic<uint64_t> writable {0};
    zlink_c_bench::latency_sampler_t *latency = nullptr;
};

struct request_metrics_t
{
    uint64_t submitted = 0;
    uint64_t blocked = 0;
    uint64_t submit_errors = 0;
    uint64_t max_outstanding = 0;
    double submit_wait_ms = 0.0;
};

zlink_routing_id_t make_routing_id (const std::string &value)
{
    zlink_routing_id_t rid;
    rid.size = static_cast<uint8_t> (std::min (value.size (), sizeof (rid.data)));
    if (rid.size > 0)
        std::memcpy (rid.data, value.data (), rid.size);
    return rid;
}

bool make_header_msg (zlink_msg_t *msg)
{
    const size_t size = std::strlen (k_request_envelope);
    if (zlink_msg_init_size (msg, size) != ZLINK_CONFIG_OK)
        return false;
    std::memcpy (zlink_msg_data (msg), k_request_envelope, size);
    return true;
}

bool make_payload_body_msg (size_t size,
                            uint32_t run_id,
                            zlink_c_bench::phase_t phase,
                            uint64_t seq,
                            zlink_msg_t *msg)
{
    const size_t payload_size = std::max (size, zlink_c_bench::k_header_size);
    zlink::framework::bench::withgrpc::BenchPayload payload;
    payload.mutable_body ()->assign (payload_size, static_cast<char> (0xab));
    if (!zlink_c_bench::stamp_payload (&(*payload.mutable_body ())[0], payload_size,
                                      run_id, phase, seq))
        return false;
    return serialize_bench_payload (payload, msg);
}

void on_reply (zlink_request_result_t result,
               zlink_msg_t *parts,
               size_t part_count,
               callback_state_t *state)
{
    if (state && result == ZLINK_REQUEST_OK && parts && part_count > 0) {
        zlink_c_bench::decoded_header_t header {};
        const zlink_msg_t *body_part = &parts[part_count - 1];
        zlink::framework::bench::withgrpc::BenchPayload payload;
        if (payload.ParseFromArray (zlink_msg_data (const_cast<zlink_msg_t *> (body_part)),
                                    static_cast<int> (zlink_msg_size (body_part)))
            && zlink_c_bench::decode_payload (payload.body ().data (), payload.body ().size (),
                                              &header)) {
            const uint64_t now = zlink_c_bench::now_ns ();
            const double us = now >= header.sent_ns ? static_cast<double> (now - header.sent_ns) / 1000.0 : 0.0;
            state->latency->add_us (us);
            state->completed.fetch_add (1, std::memory_order_relaxed);
        } else {
            state->errors.fetch_add (1, std::memory_order_relaxed);
        }
    } else if (state) {
        state->errors.fetch_add (1, std::memory_order_relaxed);
    }
    if (state)
        state->outstanding.fetch_sub (1, std::memory_order_release);
}

bool make_request_parts (size_t size,
                         uint32_t run_id,
                         zlink_c_bench::phase_t phase,
                         uint64_t seq,
                         zlink_msg_t parts[2])
{
    if (!make_header_msg (&parts[0]))
        return false;
    if (!make_payload_body_msg (size, run_id, phase, seq, &parts[1])) {
        zlink_msg_close (&parts[0]);
        return false;
    }
    return true;
}

bool poll_once (void *poller, void *dealer, callback_state_t *state, long timeout_ms)
{
    zlink_poller_event_t event {};
    zlink_config_result_t error = ZLINK_CONFIG_OK;
    const int rc = zlink_poller_wait (poller, &event, 1, timeout_ms, &error);
    if (rc < 0)
        return zlink_errno () == EINTR;
    if (rc == 0 || (event.events & ZLINK_POLLCOMPLETION) == 0)
        return true;

    for (;;) {
        zlink_completion_t completion {};
        completion.struct_size = sizeof (completion);
        const zlink_recv_result_t recv_result = zlink_completion_recv (
          dealer, &completion, ZLINK_RECV_FLAGS_DONTWAIT);
        if (recv_result == ZLINK_RECV_NO_DATA)
            return true;
        if (recv_result != ZLINK_RECV_OK) {
            if (state)
                state->errors.fetch_add (1, std::memory_order_relaxed);
            return false;
        }
        if (completion.kind == ZLINK_COMPLETION_REQUEST && completion.user_context == state)
            on_reply (completion.request_result, completion.reply_parts,
                      completion.reply_part_count, state);
        else if (completion.kind == ZLINK_COMPLETION_WRITABLE && state)
            state->writable.fetch_add (1, std::memory_order_relaxed);
        zlink_completion_close (&completion);
    }
}

bool submit_request_once (void *dealer,
                          const zlink_routing_id_t *target_rid,
                          size_t size,
                          uint32_t run_id,
                          uint64_t seq,
                          zlink_send_flags_t flags,
                          callback_state_t *cb,
                          request_metrics_t *metrics)
{
    zlink_msg_t parts[2];
    if (!make_request_parts (size, run_id, zlink_c_bench::phase_active, seq, parts)) {
        ++metrics->submit_errors;
        return false;
    }

    cb->outstanding.fetch_add (1, std::memory_order_release);
    metrics->max_outstanding = std::max<uint64_t> (
      metrics->max_outstanding, cb->outstanding.load (std::memory_order_acquire));
    const uint64_t submit_start = zlink_c_bench::now_ns ();
    const zlink_submit_result_t rc = zlink_request (
      dealer, target_rid, parts, 2, flags, 5000, cb, NULL);
    const uint64_t submit_stop = zlink_c_bench::now_ns ();
    metrics->submit_wait_ms += submit_stop >= submit_start
                                 ? static_cast<double> (submit_stop - submit_start) / 1000000.0
                                 : 0.0;
    if (rc == ZLINK_SUBMIT_OK) {
        ++metrics->submitted;
        return true;
    }

    cb->outstanding.fetch_sub (1, std::memory_order_release);
    if (rc == ZLINK_SUBMIT_BACKPRESSURED || zlink_errno () == EAGAIN || zlink_errno () == EWOULDBLOCK)
        ++metrics->blocked;
    else
        ++metrics->submit_errors;
    return false;
}

// Bounded drain (README §3): returns how long it waited. The caller reads the remaining
// outstanding count as `abandoned`.
double drain_requests (void *poller, void *dealer, callback_state_t *cb)
{
    const auto begin = std::chrono::steady_clock::now ();
    const auto deadline = begin + std::chrono::milliseconds (zlink_c_bench::k_drain_bound_ms);
    while (cb->outstanding.load (std::memory_order_acquire) > 0
           && std::chrono::steady_clock::now () < deadline) {
        (void) poll_once (poller, dealer, cb, 50);
    }
    return std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now () - begin)
      .count ();
}

// Fills what is known after the active window closed. Throughput inputs (completed, elapsed,
// CPU, latency) come from capture_active_close, which the caller ran before any drain.
void fill_request_result (zlink_c_bench::result_t *r,
                          const char *pattern,
                          size_t size,
                          callback_state_t *cb,
                          const request_metrics_t &metrics)
{
    const uint64_t pending = cb->outstanding.load (std::memory_order_acquire);
    r->implementation = "zlink-c";
    r->pattern = pattern;
    r->size = size;
    r->errors = cb->errors.load (std::memory_order_relaxed) + metrics.submit_errors + pending;
    r->submitted = metrics.submitted;
    r->blocked = metrics.blocked;
    r->peak_in_flight = metrics.max_outstanding;
    r->submit_wait_ms = metrics.submit_wait_ms;
}

zlink_c_bench::result_t run_request_serial (void *dealer,
                                            const zlink_routing_id_t *target_rid,
                                            void *poller,
                                            size_t size)
{
    const int duration_s = zlink_c_bench::duration_seconds ();
    zlink_c_bench::latency_sampler_t latency (zlink_c_bench::k_latency_sample_limit);
    callback_state_t cb;
    cb.latency = &latency;
    request_metrics_t metrics;
    const uint32_t run_id = static_cast<uint32_t> (zlink_c_bench::now_ns ());
    auto resources = zlink_c_bench::resource_start ();
    const auto start = std::chrono::steady_clock::now ();
    const auto deadline = start + std::chrono::seconds (duration_s);
    uint64_t seq = 0;
    while (std::chrono::steady_clock::now () < deadline) {
        if (submit_request_once (dealer, target_rid, size, run_id, seq++, ZLINK_SEND_FLAGS_NONE,
                                 &cb, &metrics)) {
            while (cb.outstanding.load (std::memory_order_acquire) > 0)
                (void) poll_once (poller, dealer, &cb, 50);
        }
    }
    zlink_c_bench::result_t r;
    zlink_c_bench::capture_active_close (&r, start, resources,
                                         cb.completed.load (std::memory_order_relaxed), &latency);
    fill_request_result (&r, "request-serial", size, &cb, metrics);
    return r;
}

// Bounded readiness before any measured phase. A ROUTER's first request races
// the connection handshake, and a 500 ms sleep is not a proof that the route
// exists. The race is invisible while an application window caps the submit
// loop at a small depth, because the first hundred requests are slow enough
// for the route to come up. With no window the loop submits thousands into a
// route that is not established yet, every one of them expires at its reply
// timeout, and the cell reports zero completions.
//
// The probe drives ONE request at a time through the same submit and poll path
// the measured phases use, so readiness proves the path the cells will use. It
// runs outside every measured phase.
bool await_request_ready (void *dealer,
                          const zlink_routing_id_t *target_rid,
                          void *poller,
                          int timeout_ms)
{
    const auto deadline =
      std::chrono::steady_clock::now () + std::chrono::milliseconds (timeout_ms);
    uint64_t seq = 0;
    while (std::chrono::steady_clock::now () < deadline) {
        zlink_c_bench::latency_sampler_t latency (16);
        callback_state_t cb;
        cb.latency = &latency;
        request_metrics_t metrics;
        if (submit_request_once (dealer, target_rid, 1024, 0, seq++,
                                 ZLINK_SEND_FLAGS_DONTWAIT, &cb, &metrics)) {
            const auto attempt_deadline =
              std::min (deadline,
                        std::chrono::steady_clock::now () + std::chrono::milliseconds (250));
            while (cb.outstanding.load (std::memory_order_acquire) > 0
                   && std::chrono::steady_clock::now () < attempt_deadline)
                (void) poll_once (poller, dealer, &cb, 10);
            if (cb.completed.load (std::memory_order_relaxed) > 0)
                return true;
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (10));
    }
    return false;
}

// request-backpressure (README §2): no application ceiling on outstanding requests, so the
// only thing that stops submission is the request terminal refusing DONTWAIT admission
// with ZLINK_SUBMIT_BACKPRESSURED.
zlink_c_bench::result_t run_request_backpressure (void *dealer,
                                                  const zlink_routing_id_t *target_rid,
                                                  void *poller,
                                                  size_t size)
{
    const int duration_s = zlink_c_bench::duration_seconds ();
    zlink_c_bench::latency_sampler_t latency (zlink_c_bench::k_latency_sample_limit);
    callback_state_t cb;
    cb.latency = &latency;
    request_metrics_t metrics;
    const uint32_t run_id = static_cast<uint32_t> (zlink_c_bench::now_ns ());
    auto resources = zlink_c_bench::resource_start ();
    const auto start = std::chrono::steady_clock::now ();
    const auto deadline = start + std::chrono::seconds (duration_s);
    uint64_t seq = 0;
    uint64_t submitted_since_poll = 0;
    while (std::chrono::steady_clock::now () < deadline) {
        bool submitted_any = false;
        while (std::chrono::steady_clock::now () < deadline) {
            if (!submit_request_once (dealer, target_rid, size, run_id, seq++,
                                      ZLINK_SEND_FLAGS_DONTWAIT, &cb, &metrics))
                break;
            submitted_any = true;
            if (++submitted_since_poll >= 64) {
                submitted_since_poll = 0;
                (void) poll_once (poller, dealer, &cb, 0);
            }
        }
        if (!submitted_any && cb.outstanding.load (std::memory_order_acquire) == 0) {
            std::this_thread::sleep_for (std::chrono::milliseconds (1));
            continue;
        }
        (void) poll_once (poller, dealer, &cb, 1);
    }
    zlink_c_bench::result_t r;
    zlink_c_bench::capture_active_close (&r, start, resources,
                                         cb.completed.load (std::memory_order_relaxed), &latency);
    const double drain_ms = drain_requests (poller, dealer, &cb);
    fill_request_result (&r, "request-backpressure", size, &cb, metrics);
    r.has_drain = true;
    r.abandoned = cb.outstanding.load (std::memory_order_acquire);
    r.drain_ms = drain_ms;
    r.drain_bound_hit = r.abandoned > 0;
    return r;
}

// send-saturation (README §2): one-way DONTWAIT sends; the server counts what it received.
// A BACKPRESSURED submit holds one completion reservation until its WRITABLE record is taken
// (core socket spec), so the loop takes completions and waits for WRITABLE before resubmitting.
zlink_c_bench::result_t run_send_saturation (void *dealer,
                                             const zlink_routing_id_t *target_rid,
                                             void *poller,
                                             size_t size)
{
    const int duration_s = zlink_c_bench::duration_seconds ();
    const uint32_t run_id = static_cast<uint32_t> (zlink_c_bench::now_ns ());
    auto resources = zlink_c_bench::resource_start ();
    const auto start = std::chrono::steady_clock::now ();
    const auto deadline = start + std::chrono::seconds (duration_s);
    uint64_t seq = 0;
    uint64_t blocked = 0;
    uint64_t errors = 0;
    uint64_t submitted_since_poll = 0;
    double submit_wait_ms = 0.0;
    callback_state_t cb;
    while (std::chrono::steady_clock::now () < deadline) {
        zlink_msg_t parts[2];
        if (!make_request_parts (size, run_id, zlink_c_bench::phase_active, seq, parts)) {
            ++errors;
            continue;
        }
        const uint64_t submit_start = zlink_c_bench::now_ns ();
        const zlink_submit_result_t rc =
          zlink_send_rid (dealer, target_rid, parts, 2, ZLINK_SEND_FLAGS_DONTWAIT, NULL, NULL);
        const uint64_t submit_stop = zlink_c_bench::now_ns ();
        submit_wait_ms += submit_stop >= submit_start
                            ? static_cast<double> (submit_stop - submit_start) / 1000000.0
                            : 0.0;
        if (rc == ZLINK_SUBMIT_OK) {
            ++seq;
            if (++submitted_since_poll >= 64) {
                submitted_since_poll = 0;
                (void) poll_once (poller, dealer, &cb, 0);
            }
        } else if (rc == ZLINK_SUBMIT_BACKPRESSURED) {
            ++blocked;
            while (cb.writable.load (std::memory_order_relaxed) < blocked
                   && std::chrono::steady_clock::now () < deadline)
                if (!poll_once (poller, dealer, &cb, 50))
                    break;
        } else {
            ++errors;
        }
    }
    // send-saturation counts what was submitted OK inside the window (a one-way send has no
    // reply); the WRITABLE wait below is the drain and is not part of the measured window.
    zlink_c_bench::result_t r;
    zlink_c_bench::capture_active_close (&r, start, resources, seq, nullptr);
    const auto drain_deadline = std::chrono::steady_clock::now ()
                                + std::chrono::milliseconds (zlink_c_bench::k_drain_bound_ms);
    while (cb.writable.load (std::memory_order_relaxed) < blocked
           && std::chrono::steady_clock::now () < drain_deadline)
        if (!poll_once (poller, dealer, &cb, 50))
            break;
    r.implementation = "zlink-c";
    r.pattern = "send-saturation";
    r.size = size;
    r.errors = errors + cb.errors.load (std::memory_order_relaxed);
    r.submitted = seq;
    r.blocked = blocked;
    r.submit_wait_ms = submit_wait_ms;
    return r;
}
}

int main ()
{
    const std::string request_endpoint =
      zlink_c_bench::env_string ("ZLINK_REQUEST_ENDPOINT", "tcp://127.0.0.1:6075");
    const std::string send_endpoint =
      zlink_c_bench::env_string ("ZLINK_SEND_ENDPOINT", "tcp://127.0.0.1:6077");
    const std::string patterns = zlink_c_bench::env_string (
      "PATTERNS", "request-serial,request-backpressure,send-saturation");
    void *ctx = zlink_ctx_new ();
    void *request_router = zlink_socket (ctx, ZLINK_SOCKET_ROUTER);
    void *send_router = zlink_socket (ctx, ZLINK_SOCKET_ROUTER);
    void *poller = zlink_poller_new ();
    void *send_poller = zlink_poller_new ();
    if (!ctx || !request_router || !send_router || !poller || !send_poller)
        return 2;

    // The raw ZLink row is ROUTER<->ROUTER (README §3), so the client addresses each server
    // socket by its routing id (07-router.ko.md sec.6/sec.7 require a non-NULL target RID).
    const zlink_routing_id_t request_target = make_routing_id (
      zlink_c_bench::env_string ("ZLINK_REQUEST_ROUTING_ID", "zlink-c-bench-request-server"));
    const zlink_routing_id_t send_target = make_routing_id (
      zlink_c_bench::env_string ("ZLINK_SEND_ROUTING_ID", "zlink-c-bench-send-server"));

    char client_rid[64];
    std::snprintf (client_rid, sizeof (client_rid), "zlink-c-bench-request-client-%d",
                   static_cast<int> (::getpid ()));
    (void) zlink_set_routing_id (request_router, client_rid, std::strlen (client_rid));
    std::snprintf (client_rid, sizeof (client_rid), "zlink-c-bench-send-client-%d",
                   static_cast<int> (::getpid ()));
    (void) zlink_set_routing_id (send_router, client_rid, std::strlen (client_rid));

    zlink_connect (request_router, request_endpoint.c_str ());
    zlink_connect (send_router, send_endpoint.c_str ());
    zlink_poller_add (poller, request_router, request_router, ZLINK_POLLCOMPLETION);
    zlink_poller_add (send_poller, send_router, send_router, ZLINK_POLLCOMPLETION);
    std::this_thread::sleep_for (std::chrono::milliseconds (500));
    const bool ready = await_request_ready (request_router, &request_target, poller,
                                            zlink_c_bench::k_route_ready_ms);
    std::fprintf (stderr, "[bench] route ready=%s\n", ready ? "true" : "false");
    int status = 0;
    for (const size_t size : zlink_c_bench::parse_sizes ()) {
        zlink_c_bench::result_t results[3];
        int count = 0;
        if (zlink_c_bench::pattern_enabled (patterns, "request-serial"))
            results[count++] = run_request_serial (request_router, &request_target, poller, size);
        if (zlink_c_bench::pattern_enabled (patterns, "request-backpressure"))
            results[count++] =
              run_request_backpressure (request_router, &request_target, poller, size);
        if (zlink_c_bench::pattern_enabled (patterns, "send-saturation"))
            results[count++] = run_send_saturation (send_router, &send_target, send_poller, size);
        for (int i = 0; i < count; ++i)
            if (!zlink_c_bench::write_cell_json (results[i], ""))
                status = 1;
    }
    zlink_poller_destroy (&send_poller);
    zlink_poller_destroy (&poller);
    zlink_close (request_router);
    zlink_close (send_router);
    zlink_ctx_term (ctx);
    return status;
}
