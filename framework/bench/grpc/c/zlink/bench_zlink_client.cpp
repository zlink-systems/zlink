#include "../common/bench_common.hpp"

#include "raw_wire.hpp"
#include "send_metrics.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <pthread.h>

#include <unistd.h>

namespace
{
// Maximum wait before the completion poll returns to recheck completion or its deadline.
constexpr long k_completion_poll_interval_ms = 50;

constexpr const char *k_request_envelope =
  "{\"kind\":1,\"channelName\":\"bench\",\"messageName\":\"BenchPayload\",\"contentType\":"
  "\"application/"
  "x-protobuf\",\"correlationId\":null,\"deadline\":null,\"topic\":null,\"errorCode\":null,"
  "\"errorMessage\":null,\"source\":null}";

struct callback_state_t
{
    std::mutex mutex;
    std::condition_variable changed;
    uint64_t completed = 0, active_completed = 0, errors = 0, outstanding = 0, writable_pending = 0;
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max ();
    zlink_c_bench::latency_sampler_t latency;
    explicit callback_state_t (size_t sample_limit = zlink_c_bench::k_latency_sample_limit) :
        latency (sample_limit) {}
};

struct request_metrics_t
{
    uint64_t submitted = 0, blocked = 0, submit_errors = 0, max_outstanding = 0;
    double submit_wait_ms = 0;
};

struct request_context_t
{
    std::shared_ptr<callback_state_t> stream;
    uint32_t run_id;
    uint64_t seq;
    size_t size;
    zlink_c_bench::phase_t phase;
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

bool make_payload_body_msg (
  size_t size, uint32_t run_id, zlink_c_bench::phase_t phase, uint64_t seq, zlink_msg_t *msg)
{
    const size_t payload_size = std::max (size, zlink_c_bench::k_header_size);
    zlink::framework::bench::withgrpc::BenchPayload payload;
    payload.mutable_body ()->assign (payload_size, static_cast<char> (0xab));
    if (!zlink_c_bench::stamp_payload (&(*payload.mutable_body ())[0], payload_size, run_id, phase,
                                       seq))
        return false;
    return serialize_bench_payload (payload, msg);
}

bool make_request_parts (
  size_t size, uint32_t run_id, zlink_c_bench::phase_t phase, uint64_t seq, zlink_msg_t parts[2])
{
    if (!make_header_msg (&parts[0]))
        return false;
    if (!make_payload_body_msg (size, run_id, phase, seq, &parts[1])) {
        zlink_msg_close (&parts[0]);
        return false;
    }
    return true;
}

void on_reply (const zlink_completion_t &completion, const request_context_t &context)
{
    auto &state = *context.stream;
    zlink::framework::bench::withgrpc::BenchPayload payload;
    zlink_c_bench::decoded_header_t header{};
    const bool valid = completion.request_result == ZLINK_REQUEST_OK
      && completion.reply_part_count == 2
      && payload.ParseFromArray (zlink_msg_data (&completion.reply_parts[1]),
           static_cast<int> (zlink_msg_size (&completion.reply_parts[1])))
      && zlink_c_bench::decode_payload (payload.body ().data (), payload.body ().size (), &header)
      && header.run_id == context.run_id && header.phase == context.phase
      && header.seq == context.seq && header.payload_size == context.size
      && payload.body ().size () == context.size;
    if (valid) {
        ++state.completed;
        if (std::chrono::steady_clock::now () < state.deadline) {
            ++state.active_completed;
            state.latency.add_us (static_cast<double> (zlink_c_bench::now_ns () - header.sent_ns) / 1000.0);
        }
    } else
        ++state.errors;
    --state.outstanding;
}

bool poll_once (void *poller, void *socket, long timeout_ms,
                std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max ())
{
    zlink_poller_event_t event{};
    zlink_config_result_t error = ZLINK_CONFIG_OK;
    const int rc = zlink_poller_wait (poller, &event, 1, timeout_ms, &error);
    if (rc < 0) {
        if (zlink_errno () == EINTR)
            return true;
        throw std::runtime_error ("completion poll failed");
    }
    if (rc == 0)
        return true;
    for (;;) {
        if (std::chrono::steady_clock::now () >= deadline)
            return true;
        zlink_completion_t completion{};
        completion.struct_size = sizeof completion;
        const auto received = zlink_completion_recv (socket, &completion, ZLINK_RECV_FLAGS_DONTWAIT);
        if (received == ZLINK_RECV_NO_DATA)
            return true;
        if (received != ZLINK_RECV_OK)
            throw std::runtime_error ("completion receive failed");
        std::unique_ptr<request_context_t> context (
          static_cast<request_context_t *> (completion.user_context));
        if (context) {
            auto &state = *context->stream;
            std::lock_guard lock (state.mutex);
            if (completion.kind == ZLINK_COMPLETION_WRITABLE) {
                --state.writable_pending;
                if (completion.send_result != ZLINK_SEND_ADMITTED)
                    ++state.errors;
            } else if (completion.kind == ZLINK_COMPLETION_REQUEST) {
                on_reply (completion, *context);
            }
            state.changed.notify_one ();
        }
        zlink_completion_close (&completion);
    }
}

bool submit_request_once (void *socket, const zlink_routing_id_t *target,
                          size_t size, uint32_t run_id, uint64_t seq,
                          zlink_c_bench::phase_t phase,
                          const std::shared_ptr<callback_state_t> &stream,
                          request_metrics_t &metrics, bool command = false)
{
    zlink_msg_t parts[2];
    if (!make_request_parts (size, run_id, phase, seq, parts)) {
        ++metrics.submitted;
        ++metrics.submit_errors;
        return false;
    }
    auto context = std::make_unique<request_context_t> (
      request_context_t{stream, run_id, seq, size, phase});
    // Publish the reservation under this stream's lock so an immediate
    // completion cannot race the submit result's bookkeeping.
    std::lock_guard lock (stream->mutex);
    const auto begin = zlink_c_bench::now_ns ();
    const auto rc = command
      ? zlink_send_rid (socket, target, parts, 2, ZLINK_SEND_FLAGS_DONTWAIT, context.get (), nullptr)
      : zlink_request (socket, target, parts, 2, ZLINK_SEND_FLAGS_DONTWAIT, 5000, context.get (), nullptr);
    metrics.submit_wait_ms += static_cast<double> (zlink_c_bench::now_ns () - begin) / 1000000.0;
    if (rc == ZLINK_SUBMIT_OK) {
        ++metrics.submitted;
        if (command)
            ++stream->completed;
        else {
            ++stream->outstanding;
            metrics.max_outstanding = std::max (metrics.max_outstanding, stream->outstanding);
            context.release ();
        }
        return true;
    }
    if (rc == ZLINK_SUBMIT_BACKPRESSURED) {
        ++metrics.blocked;
        ++stream->writable_pending;
        context.release ();
    } else {
        ++metrics.submitted;
        ++metrics.submit_errors;
    }
    return false;
}

bool await_request_ready (void *socket, const zlink_routing_id_t *target, void *poller,
                          int timeout_ms)
{
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::milliseconds (timeout_ms);
    auto stream = std::make_shared<callback_state_t> ();
    request_metrics_t metrics;
    uint64_t seq = 0;
    while (std::chrono::steady_clock::now () < deadline) {
        bool available;
        {
            std::lock_guard lock (stream->mutex);
            if (stream->completed)
                return true;
            available = stream->outstanding == 0 && stream->writable_pending == 0;
        }
        if (available)
            submit_request_once (socket, target, 1024, 0, seq++, zlink_c_bench::phase_warmup, stream, metrics);
        poll_once (poller, socket, 10);
    }
    return false;
}

std::string send_stats_request (void *socket,
                                const zlink_routing_id_t *target,
                                void *poller,
                                const std::string &control,
                                std::chrono::steady_clock::time_point deadline)
{
    const long remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds> (
                                deadline - std::chrono::steady_clock::now ())
                                .count ();
    if (remaining_ms <= 0)
        throw std::runtime_error ("send stats exceeded drain bound");
    zlink::framework::bench::withgrpc::BenchPayload payload;
    payload.set_body (control);
    zlink_msg_t parts[2];
    if (!make_header_msg (&parts[0]))
        throw std::runtime_error ("send stats envelope allocation failed");
    if (!serialize_bench_payload (payload, &parts[1])) {
        zlink_msg_close (&parts[0]);
        throw std::runtime_error ("send stats payload allocation failed");
    }
    if (zlink_request (socket, target, parts, 2, ZLINK_SEND_FLAGS_NONE, remaining_ms, &payload,
                       nullptr)
        != ZLINK_SUBMIT_OK)
        throw std::runtime_error ("send stats request failed");
    while (std::chrono::steady_clock::now () < deadline) {
        zlink_poller_event_t event{};
        zlink_config_result_t error = ZLINK_CONFIG_OK;
        if (zlink_poller_wait (poller, &event, 1, k_completion_poll_interval_ms, &error) < 0)
            throw std::runtime_error ("send stats completion poll failed");
        zlink_completion_t completion{};
        completion.struct_size = sizeof completion;
        const auto received =
          zlink_completion_recv (socket, &completion, ZLINK_RECV_FLAGS_DONTWAIT);
        if (received == ZLINK_RECV_NO_DATA)
            continue;
        if (received != ZLINK_RECV_OK)
            throw std::runtime_error ("send stats completion receive failed");
        if (completion.kind == ZLINK_COMPLETION_WRITABLE) {
            zlink_completion_close (&completion);
            continue;
        }
        const bool success =
          completion.kind == ZLINK_COMPLETION_REQUEST && completion.user_context == &payload
          && completion.request_result == ZLINK_REQUEST_OK && completion.reply_part_count == 2;
        bool decoded = false;
        if (success) {
            const auto &body = completion.reply_parts[1];
            decoded = payload.ParseFromArray (zlink_msg_data (const_cast<zlink_msg_t *> (&body)),
                                              static_cast<int> (zlink_msg_size (&body)));
        }
        zlink_completion_close (&completion);
        if (!decoded)
            throw std::runtime_error ("invalid send stats reply");
        return payload.body ();
    }
    throw std::runtime_error ("send stats request exceeded drain bound");
}

// One completion pump owns the socket's poller. Each logical stream has one
// dedicated submit thread and one record; only the phase coordinator merges them.
zlink_c_bench::result_t run_streams (void *socket, const zlink_routing_id_t *target,
                                     void *poller, size_t size,
                                     zlink_c_bench::phase_config_t config,
                                     const char *pattern)
{
    using clock = std::chrono::steady_clock;
    const bool command = std::strcmp (pattern, "send-saturation") == 0;
    const bool serial = std::strcmp (pattern, "request-serial") == 0;
    const int count = serial ? 1 : zlink_c_bench::k_submit_concurrency;
    const uint32_t run_id = static_cast<uint32_t> (zlink_c_bench::now_ns ());
    auto resources = zlink_c_bench::resource_start ();
    const auto start = clock::now ();
    const auto deadline = start + std::chrono::seconds (config.seconds);
    if (command)
        (void) send_stats_request (socket, target, poller,
          std::string (zlink_c_bench::k_send_stats_start)
            + std::to_string (std::chrono::duration_cast<std::chrono::nanoseconds> (deadline.time_since_epoch ()).count ())
            + " " + std::to_string (config.phase),
          start + std::chrono::milliseconds (zlink_c_bench::k_drain_bound_ms));
    std::vector<std::shared_ptr<callback_state_t>> streams;
    std::vector<request_metrics_t> metrics (count);
    std::vector<std::thread> threads;
    for (int index = 0; index < count; ++index) {
        const size_t sample_limit = zlink_c_bench::k_latency_sample_limit / count
          + (static_cast<size_t> (index) < zlink_c_bench::k_latency_sample_limit % count);
        auto stream = std::make_shared<callback_state_t> (sample_limit);
        stream->deadline = deadline;
        streams.push_back (stream);
        threads.emplace_back ([&, stream, index] {
            const auto name = "bench-submit-" + std::to_string (index);
            pthread_setname_np (pthread_self (), name.c_str ());
            uint64_t seq = static_cast<uint64_t> (index) << 56;
            while (clock::now () < deadline) {
                {
                    std::unique_lock lock (stream->mutex);
                    stream->changed.wait_until (lock, deadline, [&] {
                        return stream->writable_pending == 0 && (!serial || stream->outstanding == 0);
                    });
                    if (clock::now () >= deadline)
                        break;
                }
                submit_request_once (socket, target, size, run_id, seq++, config.phase,
                                     stream, metrics[index], command);
            }
        });
    }
    while (clock::now () < deadline) {
        const auto remaining = std::chrono::ceil<std::chrono::milliseconds> (deadline - clock::now ()).count ();
        poll_once (poller, socket, std::min<long> (k_completion_poll_interval_ms, std::max<long> (1, remaining)), deadline);
    }
    for (auto &thread : threads)
        thread.join ();
    zlink_c_bench::latency_sampler_t latency (zlink_c_bench::k_latency_sample_limit);
    uint64_t completed = 0;
    for (const auto &stream : streams) {
        std::lock_guard lock (stream->mutex);
        completed += command ? stream->completed : stream->active_completed;
        latency.merge (stream->latency);
    }
    zlink_c_bench::result_t result;
    zlink_c_bench::capture_active_close (&result, start, resources, completed, command ? nullptr : &latency);
    const auto drain_start = clock::now ();
    const auto drain_deadline = drain_start + std::chrono::milliseconds (zlink_c_bench::k_drain_bound_ms);
    const auto pending = [&] {
        uint64_t total = 0;
        for (const auto &stream : streams) {
            std::lock_guard lock (stream->mutex);
            total += stream->outstanding + stream->writable_pending;
        }
        return total;
    };
    while (pending () && clock::now () < drain_deadline)
        poll_once (poller, socket, k_completion_poll_interval_ms);
    result.implementation = "zlink-c";
    result.pattern = pattern;
    result.size = size;
    result.stream_count = count;
    result.has_drain = true;
    result.abandoned = pending ();
    result.drain_bound_hit = result.abandoned != 0;
    result.terminal_completed = 0;
    for (int index = 0; index < count; ++index) {
        const auto &m = metrics[index];
        result.submitted += m.submitted;
        result.blocked += m.blocked;
        result.errors += streams[index]->errors + m.submit_errors;
        result.terminal_completed = *result.terminal_completed + streams[index]->completed;
        result.peak_in_flight += command ? 1 : m.max_outstanding;
        result.submit_wait_ms += m.submit_wait_ms;
    }
    if (command) {
        const auto snapshot = send_stats_request (socket, target, poller,
          zlink_c_bench::k_send_stats_snapshot, drain_deadline);
        zlink_c_bench::send_metrics_t::read_snapshot (snapshot, result);
    }
    result.drain_ms = std::chrono::duration<double, std::milli> (clock::now () - drain_start).count ();
    return result;
}

zlink_c_bench::result_t run_request_serial (void *socket, const zlink_routing_id_t *target,
                                            void *poller, size_t size, zlink_c_bench::phase_config_t config)
{
    return run_streams (socket, target, poller, size, config, "request-serial");
}
zlink_c_bench::result_t run_request_backpressure (void *socket, const zlink_routing_id_t *target,
                                                  void *poller, size_t size, zlink_c_bench::phase_config_t config)
{
    return run_streams (socket, target, poller, size, config, "request-backpressure");
}
zlink_c_bench::result_t run_send_saturation (void *socket, const zlink_routing_id_t *target,
                                             void *poller, size_t size, zlink_c_bench::phase_config_t config)
{
    return run_streams (socket, target, poller, size, config, "send-saturation");
}
}

int main ()
{
    const std::string request_endpoint =
      zlink_c_bench::env_string ("ZLINK_REQUEST_ENDPOINT", zlink_c_bench::k_request_endpoint);
    const std::string send_endpoint =
      zlink_c_bench::env_string ("ZLINK_SEND_ENDPOINT", zlink_c_bench::k_send_endpoint);
    const std::string patterns =
      zlink_c_bench::env_string ("PATTERNS", "request-serial,request-backpressure,send-saturation");
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
    const bool ready = await_request_ready (request_router, &request_target, poller,
                                            zlink_c_bench::k_route_ready_ms);
    std::fprintf (stderr, "[bench] route ready=%s\n", ready ? "true" : "false");
    int status = 0;
    for (const size_t size : zlink_c_bench::parse_sizes ()) {
        if (zlink_c_bench::pattern_enabled (patterns, "request-serial")) {
            if (!zlink_c_bench::run_cell (
                  [&] (zlink_c_bench::phase_config_t config) {
                      return run_request_serial (request_router, &request_target, poller, size,
                                                 config);
                  },
                  ""))
                status = 1;
        }
        if (zlink_c_bench::pattern_enabled (patterns, "request-backpressure")) {
            if (!zlink_c_bench::run_cell (
                  [&] (zlink_c_bench::phase_config_t config) {
                      return run_request_backpressure (request_router, &request_target, poller,
                                                       size, config);
                  },
                  ""))
                status = 1;
        }
        if (zlink_c_bench::pattern_enabled (patterns, "send-saturation")) {
            if (!zlink_c_bench::run_cell (
                  [&] (zlink_c_bench::phase_config_t config) {
                      return run_send_saturation (send_router, &send_target, send_poller, size,
                                                  config);
                  },
                  ""))
                status = 1;
        }
    }
    zlink_poller_destroy (&send_poller);
    zlink_poller_destroy (&poller);
    zlink_close (request_router);
    zlink_close (send_router);
    zlink_ctx_term (ctx);
    return status;
}
