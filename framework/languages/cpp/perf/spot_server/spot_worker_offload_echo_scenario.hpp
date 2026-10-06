/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

// §10.8 spot-worker-offload-echo. Question: what a public CPU worker call adds to a local Spot echo (submission, the
// callback, and the result delivered back to the caller). Roles: HTTP Client x1, Spot (Object Server + local driver + Framework
// worker, this file) x1; no remote echo process. One operation: the local `route_client_t.request_to_spot` through caller-side
// echo identity and payload validation; the worker call interval is recorded separately in §14 worker.*. The Spot handler runs
// one `run_cpu_worker` call per request: Yield (default) hands the turn back while the worker runs, ordinary keeps it. The worker
// callback is the self-contained xorshift32-v1 task of worker-task-millis (no sleep); it checks time and cancellation every 1024
// iterations. Public worker options carry min/max threads and idle timeout; the call timeout is the workerTimeoutMs.
// worker-offload; payload 1024 bytes. Store: run Docker Redis (Spot addresses). Null: worker queue depth and Spot internals have
// no public observation; remote call, Actor, fanout do not apply.

#include <perf/server/spot_role.hpp>

namespace perf
{
// §10.8 xorshift32-v1: x=0x12345678; x^=x<<13; x^=x>>17; x^=x<<5 in 32 bits. Time and cancellation are checked every 1024
// iterations and the task ends at or after the target duration. It uses no sleep and no shared state.
inline worker_observation_t xorshift32 (int task_millis, const std::stop_token &cancellation)
{
    const auto started = now_ticks ();
    const auto target = started + static_cast<std::int64_t> (task_millis) * 1'000'000;
    std::uint32_t x = 0x12345678;
    std::uint64_t iterations = 0;
    do {
        for (int i = 0; i < 1024; ++i) {
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
        }
        iterations += 1024;
        if (cancellation.stop_requested ())
            throw fw::framework_exception_t (fw::framework_error_kind_t::shutting_down, "worker callback cancelled");
    } while (now_ticks () < target);
    return {dec (started), dec (now_ticks ()), clock_domain (), dec (iterations), x};
}

// The Spot handler: one CPU worker call, then the typed echo. The worker callback returns its own timing evidence.
class spot_worker_offload_spot_t final : public perf_spot_base_t<spot_worker_offload_spot_t>
{
  public:
    spot_worker_offload_spot_t (fw::spot_context_t context, role_t &role, fw::route_client_t &) :
        perf_spot_base_t (std::move (context)), _role (role)
    {
    }
    void configure () override { _context.handlers ().add_handler<&spot_worker_offload_spot_t::echo> (echo_request_t::packet_name); }

    fw::task_t<echo_reply_t> echo (const echo_request_t &request)
    {
        const auto received = now_ticks ();
        auto &measurement = _role.measurement;
        const auto &config = _role.config;
        const handler_scope_t scope (measurement);
        std::exception_ptr failure;
        echo_reply_t reply;
        try {
            measurement.validate_request (request);
            if (request.phase == "measured")
                _role.metrics.count ("spot.applicationHandlerEntries");
            const auto task_millis = config.worker->task_millis;
            const auto submitted = now_ticks ();
            auto call = _context
                          .run_cpu_worker ([task_millis] (std::stop_token cancellation) { return xorshift32 (task_millis, cancellation); })
                          .timeout (std::chrono::milliseconds (config.worker->worker_timeout_ms));
            worker_observation_t observation;
            if (config.terminal == "yield") {
                _role.metrics.count ("spot.applicationYieldCalls");
                observation = co_await call.yield ();
            }
            else
                observation = co_await call.async ();
            const auto resumed = now_ticks ();
            record_worker (request, observation, submitted, resumed);
            reply = payload_pattern_t::reply (request, received);
            measurement.record_reply (request);
            if (measurement.phase () == "setup" && !config.source)
                measurement.set_setup_evidence (json::array ({{{"kind", "typedProbeReply"}, {"source", "Spot request handler -> run_cpu_worker"},
                                                               {"observedValue", {{"correlationId", request.correlation_id},
                                                                                  {"iterations", observation.iterations}, {"checksum", observation.checksum}}}}}));
        }
        catch (...) {
            failure = std::current_exception ();
        }
        if (failure) {
            measurement.record_diagnostic (failure);
            std::rethrow_exception (failure);
        }
        co_return reply;
    }

  private:
    // The intervals of one worker call in the window. They are taken from one process clock; an observation from another
    // clock domain would be discarded rather than subtracted.
    void record_worker (const echo_request_t &request, const worker_observation_t &observation, std::int64_t submitted, std::int64_t resumed)
    {
        if (request.phase != "measured")
            return;
        if (observation.clock_domain_id != clock_domain () || parse_u64 (observation.iterations) == 0)
            throw validation_error_t ("SchemaMismatch", "The worker observation is not from this clock domain or is empty.");
        const auto started = parse_i64 (observation.started_ticks);
        const auto ended = parse_i64 (observation.ended_ticks);
        _role.metrics.record ("workerCallLatencyMs", submitted, resumed);
        _role.metrics.record ("workerSubmitToStartMs", submitted, started, resumed);
        _role.metrics.record ("workerTaskLatencyMs", started, ended, resumed);
        _role.metrics.record ("workerResultToContinuationMs", ended, resumed, resumed);
    }

    role_t &_role;
};

class spot_worker_offload_echo_scenario_t
{
  public:
    explicit spot_worker_offload_echo_scenario_t (role_t &role) :
        _role (role), _sequences (*role.config.workload.logical_streams)
    {
        const auto &worker = *role.config.worker;
        role.metrics.counters ({"spot.applicationHandlerEntries", "spot.applicationYieldCalls"})
          .latency ("workerCallLatencyMs", "worker.callLatency")
          .latency ("workerSubmitToStartMs", "worker.submitToStart")
          .latency ("workerTaskLatencyMs", "worker.taskLatency")
          .latency ("workerResultToContinuationMs", "worker.resultToContinuation")
          .unsupported ("PUBLIC_OBSERVATION_UNSUPPORTED", "Public worker options are settings; no queue depth snapshot exists.",
                        {"worker.pool.queueDepth.max", "worker.pool.queueDepth.mean"})
          .spot_internals_unsupported ()
          .provenance ("workerOptions",
                       {{"algorithm", worker.algorithm}, {"taskMillis", worker.task_millis},
                        {"applied", {{"minThreads", worker.min_threads}, {"maxThreads", worker.max_threads}, {"idleTimeoutMs", worker.idle_timeout_ms}}},
                       {"callTimeoutMs", worker.worker_timeout_ms}});
    }

    void prepare (const std::atomic<bool> &stopping)
    {
        auto &measurement = _role.measurement;
        const auto &config = _role.config;
        const auto objects = create_spots (_role, stopping);
        auto &route = _role.service<fw::route_client_t> ();
        json probes = json::array ();
        for (std::size_t target = 0; target < config.spot_ids.size (); ++target) {
            const auto request = measurement.request (static_cast<int> (target), _sequences.next (static_cast<int> (target)), true);
            const auto reply = route.request_to_spot (config.spot_ids[target], request)
                                 .timeout (std::chrono::milliseconds (config.workload.request_timeout_ms))
                                 .async<echo_reply_t> ()
                                 .result ()
                                 .value ();
            payload_pattern_t::validate_identity (request, reply);
            measurement.pattern ().validate (reply.payload);
            probes.push_back ({{"correlationId", request.correlation_id}, {"receivedTicks", reply.received_ticks}, {"clockDomainId", reply.clock_domain_id}});
        }
        publish_spots (_role, objects); // objectsReady only after every probe, so warmup never overlaps one
        measurement.set_setup_evidence (json::array ({{{"kind", "typedProbeEcho"}, {"source", "route_client_t.request_to_spot -> run_cpu_worker"}, {"observedValue", probes}}}));
    }

    void run (const loops_t &loops)
    {
        spawn_request_streams (loops, _role, [this] (int stream) { return loop (stream); });
    }

  private:
    fw::task_t<void> loop (int stream)
    {
        auto &measurement = _role.measurement;
        auto &route = _role.service<fw::route_client_t> ();
        const auto &config = _role.config;
        const auto &spot_id = config.spot_ids[static_cast<std::size_t> (stream) % config.spot_ids.size ()];
        auto request = measurement.request (stream, _sequences.next (stream));
        std::int64_t started = 0;
        if (!measurement.begin_operation (started))
            co_return;
        request.sent_ticks = dec (started);
        std::exception_ptr error;
        try {
            const auto reply = co_await route.request_to_spot (spot_id, request)
                                 .timeout (std::chrono::milliseconds (config.workload.request_timeout_ms))
                                 .async<echo_reply_t> ();
            payload_pattern_t::validate_identity (request, reply);
            measurement.pattern ().validate (reply.payload);
        }
        catch (...) {
            error = std::current_exception ();
        }
        measurement.complete_operation (started, error);
    }

    role_t &_role;
    stream_sequences_t _sequences;
};
} // namespace perf
