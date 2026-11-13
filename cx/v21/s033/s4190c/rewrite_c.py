from pathlib import Path
p=Path('framework/bench/grpc/c/zlink/bench_zlink_client.cpp');s=p.read_text()
s=s.replace('#include <thread>','#include <thread>\n#include <mutex>\n#include <condition_variable>\n#include <memory>\n#include <pthread.h>')
a=s.index('struct callback_state_t');b=s.index('zlink_routing_id_t make_routing_id',a)
s=s[:a]+r'''struct callback_state_t
{
    std::mutex mutex;
    std::condition_variable changed;
    uint64_t completed = 0, errors = 0, outstanding = 0, writable_pending = 0;
    zlink_c_bench::latency_sampler_t latency{zlink_c_bench::k_latency_sample_limit};
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

'''+s[b:]
a=s.index('void on_reply (');b=s.index('bool make_request_parts (',a)
s=s[:a]+s[b:]
a=s.index('bool poll_once (');b=s.index('std::string send_stats_request (',a)
s=s[:a]+r'''bool poll_once (void *poller, void *socket, long timeout_ms)
{
    zlink_poller_event_t event{};
    zlink_config_result_t error = ZLINK_CONFIG_OK;
    const int rc = zlink_poller_wait (poller, &event, 1, timeout_ms, &error);
    if (rc < 0)
        throw std::runtime_error ("completion poll failed");
    if (rc == 0)
        return true;
    for (;;) {
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
                zlink::framework::bench::withgrpc::BenchPayload payload;
                zlink_c_bench::decoded_header_t header{};
                const bool valid = completion.request_result == ZLINK_REQUEST_OK
                  && completion.reply_part_count == 2
                  && payload.ParseFromArray (zlink_msg_data (&completion.reply_parts[1]),
                       static_cast<int> (zlink_msg_size (&completion.reply_parts[1])))
                  && zlink_c_bench::decode_payload (payload.body ().data (), payload.body ().size (), &header)
                  && header.run_id == context->run_id && header.phase == context->phase
                  && header.seq == context->seq && header.payload_size == context->size
                  && payload.body ().size () == context->size;
                if (valid) {
                    ++state.completed;
                    state.latency.add_us (static_cast<double> (zlink_c_bench::now_ns () - header.sent_ns) / 1000.0);
                } else
                    ++state.errors;
                --state.outstanding;
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
    } else
        ++metrics.submit_errors;
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

'''+s[b:]
a=s.index('// send-saturation (README');b=s.index('\n}\n\nint main ()',a)
s=s[:a]+r'''// One completion pump owns the socket's poller. Each logical stream has one
// dedicated submit thread and one record; only the phase coordinator merges them.
zlink_c_bench::result_t run_streams (void *socket, const zlink_routing_id_t *target,
                                     void *poller, size_t size,
                                     zlink_c_bench::phase_config_t config,
                                     const char *pattern)
{
    using clock = std::chrono::steady_clock;
    const bool command = std::strcmp (pattern, "send-saturation") == 0;
    const bool serial = std::strcmp (pattern, "request-serial") == 0;
    const int count = serial ? 1 : 8;
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
        auto stream = std::make_shared<callback_state_t> ();
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
    while (clock::now () < deadline)
        poll_once (poller, socket, k_completion_poll_interval_ms);
    for (auto &thread : threads)
        thread.join ();
    zlink_c_bench::latency_sampler_t latency (zlink_c_bench::k_latency_sample_limit);
    uint64_t completed = 0;
    for (const auto &stream : streams) {
        std::lock_guard lock (stream->mutex);
        completed += stream->completed;
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
}'''+s[b:]
p.write_text(s)
p=Path('framework/bench/grpc/c/common/bench_common.hpp');s=p.read_text().replace('    double mean_us () const', '''    void merge (const latency_sampler_t &other)
    {
        _sum += other._sum;
        _count += other._count;
        const auto count = std::min (_cap - _samples.size (), other._samples.size ());
        _samples.insert (_samples.end (), other._samples.begin (), other._samples.begin () + count);
    }

    double mean_us () const''')
s=s.replace('    uint64_t completed = 0;','    uint64_t completed = 0;\n    std::optional<uint64_t> terminal_completed;\n    int stream_count = 0;')
s=s.replace('    if (!grpc_version.empty ())\n        out', '''    if (r.stream_count)
        out << ",\\n    \\"submitConcurrency\\": 8";
    if (!grpc_version.empty ())
        out'''.replace('\\"','\\"'))
# Add stream declaration only to the scoped C raw row, leaving grpc-c unchanged.
s=s.replace('    if (r.has_drain)\n        out', '''    if (r.stream_count)
        out << "      \\"streams\\": {\\"count\\": " << r.stream_count
            << ", \\"inFlightPerStream\\": " << (r.pattern == "request-backpressure" ? "null" : "1") << "},\\n";
    if (r.has_drain)
        out''')
s=s.replace('<< ", \\"completed\\": " << r.completed', '<< ", \\"completed\\": " << r.terminal_completed.value_or (r.completed)')
p.write_text(s)
p=Path('framework/bench/grpc/cpp/client/bench_cpp_client.cpp');s=p.read_text().replace('result.error ().get ()','result.error ()');p.write_text(s)
