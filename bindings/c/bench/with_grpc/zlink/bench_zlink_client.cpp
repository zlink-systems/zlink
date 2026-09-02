#include "../common/bench_common.hpp"

#include <zlink.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

namespace
{
constexpr const char *k_request_envelope =
  "{\"kind\":1,\"channelName\":\"bench\",\"messageName\":\"BenchPayload\",\"contentType\":\"application/x-protobuf\",\"correlationId\":null,\"deadline\":null,\"topic\":null,\"errorCode\":null,\"errorMessage\":null,\"source\":null}";

struct callback_state_t
{
    std::atomic<uint64_t> completed {0};
    std::atomic<uint64_t> errors {0};
    std::atomic<uint64_t> outstanding {0};
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

size_t varint_size (size_t value)
{
    size_t n = 1;
    while (value >= 0x80) {
        value >>= 7;
        ++n;
    }
    return n;
}

unsigned char *write_varint (unsigned char *dst, size_t value)
{
    while (value >= 0x80) {
        *dst++ = static_cast<unsigned char> ((value & 0x7fU) | 0x80U);
        value >>= 7;
    }
    *dst++ = static_cast<unsigned char> (value);
    return dst;
}

bool read_varint (const unsigned char *&p, const unsigned char *end, size_t *value)
{
    size_t result = 0;
    unsigned shift = 0;
    while (p < end && shift < sizeof (size_t) * 8) {
        const unsigned char byte = *p++;
        result |= static_cast<size_t> (byte & 0x7fU) << shift;
        if ((byte & 0x80U) == 0) {
            *value = result;
            return true;
        }
        shift += 7;
    }
    return false;
}

bool decode_bench_payload_body (const void *data, size_t size, const unsigned char **body, size_t *body_size)
{
    if (!data || !body || !body_size)
        return false;
    const unsigned char *p = static_cast<const unsigned char *> (data);
    const unsigned char *end = p + size;
    while (p < end) {
        size_t key = 0;
        if (!read_varint (p, end, &key))
            return false;
        const size_t field = key >> 3;
        const size_t wire_type = key & 0x07U;
        if (wire_type != 2)
            return false;
        size_t len = 0;
        if (!read_varint (p, end, &len) || static_cast<size_t> (end - p) < len)
            return false;
        if (field == 1) {
            *body = p;
            *body_size = len;
            return true;
        }
        p += len;
    }
    return false;
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
    const size_t encoded_size = 1 + varint_size (payload_size) + payload_size;
    if (zlink_msg_init_size (msg, encoded_size) != ZLINK_CONFIG_OK)
        return false;

    unsigned char *encoded = static_cast<unsigned char *> (zlink_msg_data (msg));
    encoded[0] = 0x0a;
    unsigned char *payload = write_varint (encoded + 1, payload_size);
    std::memset (payload, 0xab, payload_size);
    return zlink_c_bench::stamp_payload (payload, payload_size, run_id, phase, seq);
}

void on_reply (zlink_request_result_t result,
               zlink_msg_t *parts,
               size_t part_count,
               callback_state_t *state)
{
    if (state && result == ZLINK_REQUEST_OK && parts && part_count > 0) {
        zlink_c_bench::decoded_header_t header {};
        const zlink_msg_t *body_part = &parts[part_count - 1];
        const unsigned char *payload = nullptr;
        size_t payload_size = 0;
        if (decode_bench_payload_body (zlink_msg_data (const_cast<zlink_msg_t *> (body_part)),
                                       zlink_msg_size (body_part), &payload, &payload_size)
            && zlink_c_bench::decode_payload (payload, payload_size, &header)) {
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

bool make_msg (size_t size, uint32_t run_id, zlink_c_bench::phase_t phase, uint64_t seq, zlink_msg_t *msg)
{
    return make_payload_body_msg (size, run_id, phase, seq, msg);
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
        zlink_completion_close (&completion);
    }
}

bool submit_request_once (void *dealer,
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
    zlink_submit_result_t rc = zlink_request_part (
      dealer, NULL, &parts[0], flags, ZLINK_PART_MORE, 0, NULL, NULL);
    const bool header_submitted = rc == ZLINK_SUBMIT_OK;
    if (rc == ZLINK_SUBMIT_OK)
        rc = zlink_request_part (dealer, NULL, &parts[1], flags, ZLINK_PART_FINAL,
                                 5000, cb, NULL);
    const uint64_t submit_stop = zlink_c_bench::now_ns ();
    metrics->submit_wait_ms += submit_stop >= submit_start
                                 ? static_cast<double> (submit_stop - submit_start) / 1000000.0
                                 : 0.0;
    if (rc == ZLINK_SUBMIT_OK) {
        ++metrics->submitted;
        return true;
    }

    cb->outstanding.fetch_sub (1, std::memory_order_release);
    if (rc != ZLINK_SUBMIT_OK) {
        zlink_msg_close (&parts[1]);
        if (!header_submitted)
            zlink_msg_close (&parts[0]);
    }
    if (rc == ZLINK_SUBMIT_BACKPRESSURED || zlink_errno () == EAGAIN || zlink_errno () == EWOULDBLOCK)
        ++metrics->blocked;
    else
        ++metrics->submit_errors;
    return false;
}

void drain_requests (void *poller, void *dealer, callback_state_t *cb)
{
    const int drain_ms = zlink_c_bench::env_int ("DRAIN_TIMEOUT_MS", 5000);
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::milliseconds (drain_ms);
    while (cb->outstanding.load (std::memory_order_acquire) > 0
           && std::chrono::steady_clock::now () < deadline) {
        (void) poll_once (poller, dealer, cb, 50);
    }
}

zlink_c_bench::result_t finish_request_result (const char *scenario,
                                               size_t size,
                                               const std::chrono::steady_clock::time_point &start,
                                               const std::chrono::steady_clock::time_point &stop,
                                               const zlink_c_bench::resource_sample_t &resources,
                                               callback_state_t *cb,
                                               zlink_c_bench::latency_sampler_t *latency,
                                               const request_metrics_t &metrics)
{
    const uint64_t pending = cb->outstanding.load (std::memory_order_acquire);
    zlink_c_bench::result_t r;
    r.scenario = scenario;
    r.size = size;
    r.unit = "KOPS";
    r.completed = cb->completed.load (std::memory_order_relaxed);
    r.errors = cb->errors.load (std::memory_order_relaxed) + metrics.submit_errors + pending;
    r.elapsed_s = std::chrono::duration<double> (stop - start).count ();
    r.mean_us = latency->mean_us ();
    r.p95_us = latency->percentile (0.95);
    r.p99_us = latency->percentile (0.99);
    r.cpu_percent = zlink_c_bench::cpu_percent (resources, r.elapsed_s);
    r.mem_mb = zlink_c_bench::rss_mb ();
    r.server_cpu_percent = zlink_c_bench::server_cpu_percent (resources, r.elapsed_s);
    r.server_mem_mb = zlink_c_bench::server_mem_mb (resources);
    r.submitted = metrics.submitted;
    r.blocked = metrics.blocked;
    r.max_outstanding = metrics.max_outstanding;
    r.submit_wait_ms = metrics.submit_wait_ms;
    return r;
}

zlink_c_bench::result_t run_request_serial (void *dealer, void *poller, size_t size)
{
    const int duration_s = zlink_c_bench::env_int ("DURATION_SECONDS", 3);
    zlink_c_bench::latency_sampler_t latency (200000);
    callback_state_t cb;
    cb.latency = &latency;
    request_metrics_t metrics;
    const uint32_t run_id = static_cast<uint32_t> (zlink_c_bench::now_ns ());
    auto resources = zlink_c_bench::resource_start ();
    const auto start = std::chrono::steady_clock::now ();
    const auto deadline = start + std::chrono::seconds (duration_s);
    uint64_t seq = 0;
    while (std::chrono::steady_clock::now () < deadline) {
        if (submit_request_once (dealer, size, run_id, seq++, ZLINK_SEND_FLAGS_NONE, &cb, &metrics)) {
            while (cb.outstanding.load (std::memory_order_acquire) > 0)
                (void) poll_once (poller, dealer, &cb, 50);
        }
    }
    const auto stop = std::chrono::steady_clock::now ();
    return finish_request_result ("zlink-c-request-serial", size, start, stop, resources, &cb,
                                  &latency, metrics);
}

zlink_c_bench::result_t run_request_window (void *dealer,
                                            void *poller,
                                            size_t size,
                                            uint64_t window,
                                            const char *scenario)
{
    const int duration_s = zlink_c_bench::env_int ("DURATION_SECONDS", 3);
    zlink_c_bench::latency_sampler_t latency (200000);
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
        while (std::chrono::steady_clock::now () < deadline
               && cb.outstanding.load (std::memory_order_acquire) < window) {
            if (!submit_request_once (dealer, size, run_id, seq++, ZLINK_SEND_FLAGS_DONTWAIT, &cb,
                                      &metrics))
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
    drain_requests (poller, dealer, &cb);
    const auto stop = std::chrono::steady_clock::now ();
    return finish_request_result (scenario, size, start, stop, resources, &cb, &latency, metrics);
}

zlink_c_bench::result_t run_send_loop (void *dealer,
                                       size_t size,
                                       zlink_send_flags_t flags,
                                       const char *scenario)
{
    const int duration_s = zlink_c_bench::env_int ("DURATION_SECONDS", 3);
    const uint32_t run_id = static_cast<uint32_t> (zlink_c_bench::now_ns ());
    auto resources = zlink_c_bench::resource_start ();
    const auto start = std::chrono::steady_clock::now ();
    const auto deadline = start + std::chrono::seconds (duration_s);
    uint64_t seq = 0;
    uint64_t blocked = 0;
    uint64_t errors = 0;
    double submit_wait_ms = 0.0;
    while (std::chrono::steady_clock::now () < deadline) {
        zlink_msg_t parts[2];
        if (!make_request_parts (size, run_id, zlink_c_bench::phase_active, seq, parts)) {
            ++errors;
            continue;
        }
        const uint64_t submit_start = zlink_c_bench::now_ns ();
        zlink_submit_result_t rc =
          zlink_send_part (dealer, &parts[0], flags, ZLINK_PART_MORE, NULL, NULL);
        const bool header_submitted = rc == ZLINK_SUBMIT_OK;
        if (rc == ZLINK_SUBMIT_OK)
            rc = zlink_send_part (dealer, &parts[1], flags, ZLINK_PART_FINAL, NULL, NULL);
        const uint64_t submit_stop = zlink_c_bench::now_ns ();
        submit_wait_ms += submit_stop >= submit_start
                            ? static_cast<double> (submit_stop - submit_start) / 1000000.0
                            : 0.0;
        if (rc == ZLINK_SUBMIT_OK) {
            ++seq;
        } else {
            zlink_msg_close (&parts[1]);
            if (!header_submitted)
                zlink_msg_close (&parts[0]);
            if (rc == ZLINK_SUBMIT_BACKPRESSURED || zlink_errno () == EAGAIN
                || zlink_errno () == EWOULDBLOCK)
                ++blocked;
            else
                ++errors;
        }
    }
    const auto stop = std::chrono::steady_clock::now ();
    zlink_c_bench::result_t r;
    r.scenario = scenario;
    r.size = size;
    r.unit = "KMSG/s";
    r.completed = seq;
    r.errors = errors;
    r.elapsed_s = std::chrono::duration<double> (stop - start).count ();
    r.cpu_percent = zlink_c_bench::cpu_percent (resources, r.elapsed_s);
    r.mem_mb = zlink_c_bench::rss_mb ();
    r.server_cpu_percent = zlink_c_bench::server_cpu_percent (resources, r.elapsed_s);
    r.server_mem_mb = zlink_c_bench::server_mem_mb (resources);
    r.submitted = seq;
    r.blocked = blocked;
    r.submit_wait_ms = submit_wait_ms;
    return r;
}

zlink_c_bench::result_t run_send_send_serial (void *dealer, size_t size)
{
    const int duration_s = zlink_c_bench::env_int ("DURATION_SECONDS", 3);
    zlink_c_bench::latency_sampler_t latency (200000);
    const uint32_t run_id = static_cast<uint32_t> (zlink_c_bench::now_ns ());
    auto resources = zlink_c_bench::resource_start ();
    const auto start = std::chrono::steady_clock::now ();
    const auto deadline = start + std::chrono::seconds (duration_s);
    uint64_t seq = 0;
    uint64_t submitted = 0;
    uint64_t completed = 0;
    uint64_t blocked = 0;
    uint64_t errors = 0;
    double submit_wait_ms = 0.0;
    while (std::chrono::steady_clock::now () < deadline) {
        zlink_msg_t parts[2];
        if (!make_request_parts (size, run_id, zlink_c_bench::phase_active, seq, parts)) {
            ++errors;
            continue;
        }

        const uint64_t submit_start = zlink_c_bench::now_ns ();
        zlink_submit_result_t send_rc =
          zlink_send_part (dealer, &parts[0], ZLINK_SEND_FLAGS_NONE, ZLINK_PART_MORE, NULL, NULL);
        const bool header_submitted = send_rc == ZLINK_SUBMIT_OK;
        if (send_rc == ZLINK_SUBMIT_OK)
            send_rc = zlink_send_part (dealer, &parts[1], ZLINK_SEND_FLAGS_NONE,
                                       ZLINK_PART_FINAL, NULL, NULL);
        const uint64_t submit_stop = zlink_c_bench::now_ns ();
        submit_wait_ms += submit_stop >= submit_start
                            ? static_cast<double> (submit_stop - submit_start) / 1000000.0
                            : 0.0;
        if (send_rc != ZLINK_SUBMIT_OK) {
            zlink_msg_close (&parts[1]);
            if (!header_submitted)
                zlink_msg_close (&parts[0]);
            if (send_rc == ZLINK_SUBMIT_BACKPRESSURED || zlink_errno () == EAGAIN
                || zlink_errno () == EWOULDBLOCK)
                ++blocked;
            else
                ++errors;
            continue;
        }
        ++submitted;

        const zlink_routing_id_t *source_rid = nullptr;
        zlink_part_flag_t has_more = ZLINK_PART_FINAL;
        zlink_msg_t reply_parts[2];
        if (zlink_msg_init (&reply_parts[0]) != ZLINK_CONFIG_OK
            || zlink_msg_init (&reply_parts[1]) != ZLINK_CONFIG_OK) {
            ++errors;
            continue;
        }
        zlink_recv_result_t recv_rc =
          zlink_recv_part (dealer, &source_rid, &reply_parts[0], &has_more, ZLINK_RECV_FLAGS_NONE);
        if (recv_rc == ZLINK_RECV_OK && has_more == ZLINK_PART_MORE)
            recv_rc = zlink_recv_part (dealer, &source_rid, &reply_parts[1], &has_more,
                                       ZLINK_RECV_FLAGS_NONE);
        if (recv_rc != ZLINK_RECV_OK || has_more != ZLINK_PART_FINAL) {
            zlink_msg_close (&reply_parts[0]);
            zlink_msg_close (&reply_parts[1]);
            ++errors;
            continue;
        }

        zlink_c_bench::decoded_header_t header {};
        const unsigned char *payload = nullptr;
        size_t payload_size = 0;
        if (decode_bench_payload_body (zlink_msg_data (&reply_parts[1]),
                                       zlink_msg_size (&reply_parts[1]), &payload, &payload_size)
            && zlink_c_bench::decode_payload (payload, payload_size, &header)
            && header.run_id == run_id && header.seq == seq) {
            const uint64_t now = zlink_c_bench::now_ns ();
            const double us = now >= header.sent_ns
                                ? static_cast<double> (now - header.sent_ns) / 1000.0
                                : 0.0;
            latency.add_us (us);
            ++completed;
            ++seq;
        } else {
            ++errors;
        }
        zlink_msg_close (&reply_parts[0]);
        zlink_msg_close (&reply_parts[1]);
    }
    const auto stop = std::chrono::steady_clock::now ();
    zlink_c_bench::result_t r;
    r.scenario = "zlink-c-send-send-serial";
    r.size = size;
    r.unit = "KOPS";
    r.completed = completed;
    r.errors = errors;
    r.elapsed_s = std::chrono::duration<double> (stop - start).count ();
    r.mean_us = latency.mean_us ();
    r.p95_us = latency.percentile (0.95);
    r.p99_us = latency.percentile (0.99);
    r.cpu_percent = zlink_c_bench::cpu_percent (resources, r.elapsed_s);
    r.mem_mb = zlink_c_bench::rss_mb ();
    r.server_cpu_percent = zlink_c_bench::server_cpu_percent (resources, r.elapsed_s);
    r.server_mem_mb = zlink_c_bench::server_mem_mb (resources);
    r.submitted = submitted;
    r.blocked = blocked;
    r.max_outstanding = 1;
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
    const uint64_t window = static_cast<uint64_t> (zlink_c_bench::env_int ("WINDOW_SIZE", 100));
    const uint64_t saturation_window =
      static_cast<uint64_t> (zlink_c_bench::env_int ("MAX_OUTSTANDING", 4096));
    const std::string scenarios = zlink_c_bench::env_string ("ZLINK_BENCH_SCENARIOS", "all");
    void *ctx = zlink_ctx_new ();
    void *request_dealer = zlink_socket (ctx, ZLINK_SOCKET_DEALER);
    void *send_dealer = zlink_socket (ctx, ZLINK_SOCKET_DEALER);
    void *poller = zlink_poller_new ();
    if (!ctx || !request_dealer || !send_dealer || !poller)
        return 2;
    zlink_connect (request_dealer, request_endpoint.c_str ());
    zlink_connect (send_dealer, send_endpoint.c_str ());
    zlink_poller_add (poller, request_dealer, request_dealer, ZLINK_POLLCOMPLETION);
    std::this_thread::sleep_for (std::chrono::milliseconds (500));
    for (const size_t size : zlink_c_bench::parse_sizes ()) {
        if (zlink_c_bench::scenario_enabled (scenarios, "request-serial"))
            zlink_c_bench::print_result (run_request_serial (request_dealer, poller, size));
        if (zlink_c_bench::scenario_enabled (scenarios, "request-window"))
            zlink_c_bench::print_result (
              run_request_window (request_dealer, poller, size, window, "zlink-c-request-window"));
        if (zlink_c_bench::scenario_enabled (scenarios, "request-saturation"))
            zlink_c_bench::print_result (run_request_window (
              request_dealer, poller, size, saturation_window, "zlink-c-request-saturation"));
        if (zlink_c_bench::scenario_enabled (scenarios, "send-blocking"))
            zlink_c_bench::print_result (
              run_send_loop (send_dealer, size, ZLINK_SEND_FLAGS_NONE, "zlink-c-send-blocking"));
        if (zlink_c_bench::scenario_enabled (scenarios, "send-saturation"))
            zlink_c_bench::print_result (run_send_loop (
              send_dealer, size, ZLINK_SEND_FLAGS_DONTWAIT, "zlink-c-send-saturation"));
        if (scenarios != "all" && zlink_c_bench::scenario_enabled (scenarios, "send-send-serial"))
            zlink_c_bench::print_result (run_send_send_serial (send_dealer, size));
    }
    zlink_poller_destroy (&poller);
    zlink_close (request_dealer);
    zlink_close (send_dealer);
    zlink_ctx_term (ctx);
    return 0;
}
