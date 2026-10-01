/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

// Perf spec §4, §13, §14, §15.4: the cohort accounting of one owner process. Application counters only: no socket
// state, retry, transport polling or completion pump lives here.

#include <perf/config.hpp>
#include <perf/core.hpp>

#include <sys/resource.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace perf
{
// A connector failure at the STREAM connector boundary (client only): the wrapper keeps the public error code.
class connector_error_t : public std::runtime_error
{
  public:
    connector_error_t (std::string code, bool timeout, const std::string &message) :
        std::runtime_error (message), _code (std::move (code)), _timeout (timeout)
    {
    }
    const std::string &code () const noexcept { return _code; }
    bool timeout () const noexcept { return _timeout; }

  private:
    std::string _code;
    bool _timeout;
};

inline const std::vector<std::string> &latency_suffixes ()
{
    static const std::vector<std::string> suffixes{"meanMs", "p50Ms", "p95Ms", "p99Ms", "maxMs"};
    return suffixes;
}

// §14 keys that no scenario fills by default: null with NOT_APPLICABLE until a scenario names them.
inline void baseline_nulls (json &metrics, json &histograms, json &reasons)
{
    static const char *const inapplicable[] = {
      "messages.admitted", "messages.expired", "messages.duplicateReply", "messages.lateReply",
      "messages.unknownCorrelation", "spot.applicationYieldCalls", "spot.applicationHandlerEntries", "driver.issued",
      "driver.notStarted", "driver.failed", "messages.publishedInWindow",
      "fanout.subscriberCount", "fanout.deliveredInWindow",
      "fanout.duplicateEvents", "fanout.outOfCohortEvents", "fanout.deliveryRatio",
      "fanout.publishOpsPerSec", "fanout.deliveryOpsPerSec", "spot.mailboxDepth.max", "spot.mailboxDepth.mean",
      "spot.suspendedTurns", "spot.resumedTurns", "spot.resumeLatency.p95Ms", "spot.resumeLatency.p99Ms",
      "worker.pool.queueDepth.max", "worker.pool.queueDepth.mean"};
    static const char *const prefixes[] = {"actor.sourceAdmission.latency", "spot.remoteCallLatency", "driver.latency",
                                           "worker.callLatency", "worker.submitToStart", "worker.taskLatency",
                                           "worker.resultToContinuation", "fanout.deliveryLatency"};
    static const char *const histogram_keys[] = {"sourceAdmissionMs", "driverLatencyMs", "workerCallLatencyMs",
                                                 "workerSubmitToStartMs", "workerTaskLatencyMs",
                                                 "workerResultToContinuationMs", "fanoutDeliveryLatencyMs"};
    const auto null_metric = [&] (const std::string &key, const char *code, const char *why) {
        metrics[key] = nullptr;
        reasons["/metrics/" + key] = null_reason (code, why);
    };
    for (const char *key : inapplicable)
        null_metric (key, "NOT_APPLICABLE", "The request baseline has no corresponding operation.");
    for (const char *prefix : prefixes)
        for (const auto &suffix : latency_suffixes ())
            null_metric (std::string (prefix) + "." + suffix, "NOT_APPLICABLE",
                         "The request baseline has no corresponding operation.");
    for (const char *key : histogram_keys) {
        histograms[key] = nullptr;
        reasons[std::string ("/histograms/") + key] =
          null_reason ("NOT_APPLICABLE", "The request baseline does not measure this interval.");
    }
    for (const char *suffix : {"p50Ms", "p95Ms", "p99Ms"})
        null_metric (std::string ("host.queueWaitLatency.") + suffix, "PUBLIC_OBSERVATION_UNSUPPORTED",
                     "Public status provides no exact pre-receive to handler queue-wait hook.");
}

// §14 process.*: CPU time, RSS sampling and the window. C++ has no allocation counter and no GC generations
// (§14: those stay null with a reason); the RSS is /proc/self/statm resident pages.
class process_sampler_t
{
  public:
    void start ()
    {
        _started = _last = now_ticks ();
        _cpu_start = cpu_seconds ();
        _rss_max = rss_bytes ();
        _intervals.clear ();
    }
    void sample ()
    {
        const auto now = now_ticks ();
        _intervals.push_back (now - _last);
        _last = now;
        _rss_max = std::max (_rss_max, rss_bytes ());
    }
    void end ()
    {
        sample ();
        _cpu_end = cpu_seconds ();
    }
    void export_to (json &metrics, json &runtime, json &reasons) const
    {
        const double seconds = static_cast<double> (_last - _started) / 1e9;
        metrics["process.cpuPercent"] = seconds > 0 ? (_cpu_end - _cpu_start) / seconds * 100 : 0.0;
        metrics["process.rssMb"] = static_cast<double> (_rss_max) / 1048576.0;
        for (const char *key : {"process.allocatedMb", "gc.gen0", "gc.gen1", "gc.gen2"}) {
            metrics[key] = nullptr;
            reasons[std::string ("/metrics/") + key] =
              null_reason ("RUNTIME_METRIC_UNSUPPORTED",
                           "C++ has no garbage collector and its runtime exposes no cumulative allocation counter.");
        }
        json intervals = json::array ();
        for (const auto interval : _intervals)
            intervals.push_back (dec (interval));
        runtime["rssSampling"] = {{"name", "/proc/self/statm resident pages"},
                                  {"unit", "ns"},
                                  {"type", "sampling"},
                                  {"value", {{"requestedIntervalNs", "100000000"},
                                             {"actualIntervalsNs", intervals},
                                             {"startedTicks", dec (_started)},
                                             {"endedTicks", dec (_last)}}}};
        runtime["cpuObservationSeconds"] = {{"name", "getrusage(RUSAGE_SELF) user+system over the window"},
                                            {"unit", "s"},
                                            {"type", "number"},
                                            {"value", seconds}};
    }

  private:
    static double cpu_seconds ()
    {
        rusage usage{};
        getrusage (RUSAGE_SELF, &usage);
        return static_cast<double> (usage.ru_utime.tv_sec + usage.ru_stime.tv_sec)
               + static_cast<double> (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1e6;
    }
    static std::uint64_t rss_bytes ()
    {
        std::ifstream statm ("/proc/self/statm");
        std::uint64_t size = 0, resident = 0;
        statm >> size >> resident;
        return resident * static_cast<std::uint64_t> (sysconf (_SC_PAGESIZE));
    }
    std::int64_t _started = 0, _last = 0;
    double _cpu_start = 0, _cpu_end = 0;
    std::uint64_t _rss_max = 0;
    std::vector<std::int64_t> _intervals;
};

// The workload coroutines of one phase. Warmup drains before reset; measured work stops at endTicks.
class loop_group_t : public std::enable_shared_from_this<loop_group_t>
{
  public:
    // A workload loop that is not a coroutine (a callback chain) brackets itself with enter/leave.
    void enter ()
    {
        std::lock_guard lock (_mutex);
        ++_pending;
    }
    void leave ()
    {
        std::lock_guard lock (_mutex);
        --_pending;
        _idle.notify_all ();
    }
    void spawn (zlink::framework::task_t<void> task, std::function<void (std::exception_ptr)> on_error = {})
    {
        enter ();
        (void) observe (shared_from_this (), std::move (task), std::move (on_error));
    }
    void wait_idle ()
    {
        std::unique_lock lock (_mutex);
        _idle.wait (lock, [this] { return _pending == 0; });
    }

  private:
    // Every loop leaves the group once, whatever ended it; a failure goes to on_error.
    static zlink::framework::task_t<void> observe (std::shared_ptr<loop_group_t> self, zlink::framework::task_t<void> task,
                                                   std::function<void (std::exception_ptr)> on_error)
    {
        try {
            co_await task;
        }
        catch (...) {
            if (on_error)
                on_error (std::current_exception ());
        }
        self->leave ();
    }

    std::mutex _mutex;
    std::condition_variable _idle;
    std::size_t _pending = 0;
};

// The group is shared so warmup can wait for all callback chains before reset.
using loops_t = std::shared_ptr<loop_group_t>;
using workload_fn_t = std::function<void (const loops_t &)>;

class measurement_t
{
  public:
    measurement_t (role_config_t config, bool primary) :
        _config (std::move (config)), _primary (primary), _pattern (_config.workload.payload_size)
    {
    }
    ~measurement_t ()
    {
        if (_phase_thread.joinable ())
            _phase_thread.join ();
    }
    measurement_t (const measurement_t &) = delete;
    measurement_t &operator= (const measurement_t &) = delete;

    const role_config_t &config () const noexcept { return _config; }
    const payload_pattern_t &pattern () const noexcept { return _pattern; }
    std::string phase () const { std::lock_guard lock (_gate); return _phase; }
    std::string reset_seq () const { std::lock_guard lock (_gate); return _reset_seq; }
    std::int64_t end_ticks () const { std::lock_guard lock (_gate); return _end; }
    bool can_issue () const
    {
        std::lock_guard lock (_gate);
        return !_sealed && _start != 0 && now_ticks () < _end;
    }
    bool has_errors () const
    {
        std::lock_guard lock (_gate);
        return !_by_kind.empty () || !_harness.empty () || !_language.empty ();
    }
    json error_evidence () const { std::lock_guard lock (_gate); return json (_errors); }
    void set_setup_evidence (json evidence) { std::lock_guard lock (_gate); _setup_evidence = std::move (evidence); }
    json setup_evidence () const { std::lock_guard lock (_gate); return _setup_evidence; }
    bool has_setup_evidence () const { std::lock_guard lock (_gate); return _setup_evidence.is_array () && !_setup_evidence.empty (); }
    void set_connection_result (std::uint64_t connected, std::uint64_t failed)
    {
        std::lock_guard lock (_gate);
        _connected = connected;
        _connection_failures = failed;
    }
    void set_sample_public_state (std::function<json ()> sample) { _sample_public_state = std::move (sample); }
    // A scenario's own counters: cleared with the window at reset, and added to every snapshot (§14 family keys).
    void add_on_reset (std::function<void ()> on_reset) { _on_reset.push_back (std::move (on_reset)); }
    void add_enrich_snapshot (std::function<void (json &)> enrich) { _enrich.push_back (std::move (enrich)); }
    void set_final_snapshot (bool value) { _final = value; }
    bool final_snapshot () const { return _final; }
    // The typed messages this scenario's measured path carries, one serializedMessageBytes row each (§15.2).
    void set_message_types (std::vector<std::pair<std::string, std::string>> types) { _message_types = std::move (types); }

    echo_request_t request (int stream, std::uint64_t sequence, bool probe = false) const
    {
        const auto current_phase = phase ();
        const bool warmup = probe || current_phase == "warmup";
        echo_request_t out;
        out.run_id = _config.run_id;
        out.cell_id = _config.cell_id;
        out.reset_seq = probe ? "0" : reset_seq ();
        out.phase = warmup ? "warmup" : "measured";
        out.client_id = stream;
        out.sequence = dec (sequence);
        out.correlation_id = _config.cell_id + "/" + out.phase + "/" + std::to_string (stream) + "/" + out.sequence;
        out.sent_ticks = dec (now_ticks ());
        out.clock_domain_id = clock_domain ();
        out.payload = _pattern.base64 ();
        return out;
    }

    // A send/send request names its return address (§10.4, §10.6); an echo request names none.
    void validate_request (const echo_request_t &request, const std::optional<std::string> &return_channel = std::nullopt,
                           const std::optional<std::string> &return_spot_id = std::nullopt) const
    {
        if (request.run_id != _config.run_id || request.cell_id != _config.cell_id || request.client_id < 0
            || (request.phase != "warmup" && request.phase != "measured") || request.return_spot_id != return_spot_id
            || request.return_channel != return_channel
            || request.correlation_id != request.cell_id + "/" + request.phase + "/" + std::to_string (request.client_id) + "/" + request.sequence
            || request.clock_domain_id.empty ())
            throw validation_error_t ("IdentityMismatch", "Request identity does not match the cell.");
        (void) parse_u64 (request.sequence);
        (void) parse_i64 (request.sent_ticks);
        const auto seq = parse_u64 (request.reset_seq);
        if (request.phase == "warmup" ? seq != 0 : (seq == 0 || request.reset_seq != reset_seq ()))
            throw validation_error_t ("PhaseMismatch", "Request reset sequence does not match the phase.");
        _pattern.validate (request.payload);
    }

    trigger_reply_t start (const trigger_request_t &trigger, workload_fn_t workload)
    {
        std::lock_guard lock (_gate);
        if (trigger.run_id != _config.run_id || trigger.cell_id != _config.cell_id
            || (trigger.phase != "warmup" && trigger.phase != "measured") || trigger.reset_seq != _reset_seq
            || (trigger.phase == "warmup" ? _reset_seq != "0" : _reset_seq == "0"))
            return ack (trigger, false, "rejected", "Identity, resetSeq or phase is invalid.");
        const auto key = trigger.phase + "/" + trigger.reset_seq;
        if (const auto previous = _starts.find (key); previous != _starts.end ()) {
            auto again = previous->second;
            again.state = "alreadyStarted";
            return again;
        }
        if (!_phase_complete || _inflight != 0 || _active_handlers != 0
            || (trigger.phase == "warmup" ? _phase != "setup" : _phase != "reset"))
            return ack (trigger, false, "rejected", "Previous phase has not drained and reset.");
        _phase = trigger.phase;
        _sealed = false;
        _start = now_ticks ();
        const double seconds = _phase == "warmup" ? _config.workload.warmup_seconds : _config.workload.duration_seconds;
        _end = _start + static_cast<std::int64_t> (seconds * 1e9);
        _start_unix = unix_ms ();
        _sampler.start ();
        const auto reply = ack (trigger, true, "started", std::nullopt);
        _starts.emplace (key, reply);
        _phase_complete = false;
        if (_phase_thread.joinable ())
            _phase_thread.join ();
        // The phase runs on its own thread so the HTTP/control acknowledgement leaves before load consumes the runtime.
        _phase_thread = std::thread ([this, workload = std::move (workload)] () mutable { run_phase (std::move (workload)); });
        return reply;
    }

    // Blocks until the phase window has ended (the control pipe's `wait`).
    void wait_phase () const
    {
        std::unique_lock lock (_gate);
        _phase_done.wait (lock, [this] { return _phase_complete; });
    }

    // {reply body, HTTP status}; a rejected reset changes nothing.
    std::pair<json, int> reset (const reset_request_t &request, const std::function<std::optional<std::uint64_t> ()> &reset_capacity)
    {
        const auto requested = parse_u64 (request.reset_seq);
        std::lock_guard lock (_gate);
        const bool drained = _phase_complete && (_start == 0 || now_ticks () >= _end) && _inflight == 0 && _active_handlers == 0;
        if (request.run_id == _config.run_id && request.cell_id == _config.cell_id && _reset_ack
            && (*_reset_ack)["resetSeq"] == request.reset_seq && drained)
            return {*_reset_ack, 200};
        std::optional<std::string> reason;
        if (request.run_id != _config.run_id || request.cell_id != _config.cell_id)
            reason = "Different run or cell.";
        else if (requested == 0 || requested <= parse_u64 (_reset_seq))
            reason = "resetSeq must advance.";
        else if (!drained || _phase == "setup")
            reason = "Warmup or measured operations have not drained.";
        else if (!_by_kind.empty () || !_harness.empty () || !_language.empty ())
            reason = "Previous phase failed.";
        if (reason)
            return {{{"ok", false}, {"runId", request.run_id}, {"cellId", request.cell_id}, {"role", _config.role},
                     {"roleInstance", _config.role_instance}, {"resetSeq", request.reset_seq},
                     {"applicationResetAtUnixMs", unix_ms ()}, {"capacityEpoch", nullptr}, {"reason", *reason},
                     {"nullReasons", json::object ()}},
                    409};
        _counts.clear (); _by_kind.clear (); _harness.clear (); _language.clear (); _errors.clear (); _directional.clear ();
        _public_state_samples = json::array ();
        _latency = histogram_t (); _max_inflight = 0;
        _start = _end = 0;
        _start_unix.reset (); _end_unix.reset (); _sealed = false;
        for (const auto &hook : _on_reset)
            hook ();
        _reset_seq = request.reset_seq;
        _phase = "reset";
        const auto reset_at = unix_ms ();
        const auto epoch = reset_capacity ? reset_capacity () : std::nullopt;
        json reasons = json::object ();
        if (!epoch)
            reasons["/capacityEpoch"] = null_reason ("NOT_APPLICABLE", "The client owns no Framework host.");
        _reset_ack = json{{"ok", true}, {"runId", _config.run_id}, {"cellId", _config.cell_id}, {"role", _config.role},
                          {"roleInstance", _config.role_instance}, {"resetSeq", _reset_seq},
                          {"applicationResetAtUnixMs", reset_at}, {"capacityEpoch", epoch ? json (dec (*epoch)) : json (nullptr)},
                          {"reason", nullptr}, {"nullReasons", reasons}};
        return {*_reset_ack, 200};
    }

    bool begin_operation (std::int64_t &started, const std::string &direction = "request")
    {
        std::lock_guard lock (_gate);
        started = now_ticks ();
        if (_sealed || _start == 0 || started >= _end)
            return false;
        ++_counts["sent"];
        ++_inflight;
        _max_inflight = std::max (_max_inflight, _inflight);
        ++_directional[direction];
        return true;
    }

    std::int64_t start_ticks () const { std::lock_guard lock (_gate); return _start; }

    bool complete_operation (std::int64_t started, std::exception_ptr error = nullptr,
                             std::optional<std::int64_t> completed_ticks = std::nullopt)
    {
        const auto completed = completed_ticks.value_or (now_ticks ());
        std::lock_guard lock (_gate);
        if (_sealed)
            return false;
        --_inflight;
        if (completed >= _end)
            return false;
        if (!error) {
            ++_counts["completed"];
            _latency.record (completed - started);
            return true;
        }
        record_error (error, true);
        return false;
    }

    void handler_enter () { std::lock_guard lock (_gate); ++_active_handlers; }
    void handler_exit () { std::lock_guard lock (_gate); --_active_handlers; }
    int active_handlers () const { std::lock_guard lock (_gate); return _active_handlers; }
    void record_reply (const echo_request_t &request) { record_application_call (request, "reply"); }
    // A public call this process starts (send) or a typed reply it returns, counted once inside its own window.
    void record_application_call (const echo_request_t &request, const std::string &direction)
    {
        std::lock_guard lock (_gate);
        if (request.reset_seq == _reset_seq && _start != 0 && now_ticks () < _end
            && request.phase == (_reset_seq == "0" ? "warmup" : "measured"))
            ++_directional[direction];
    }
    void record_diagnostic (std::exception_ptr error) { std::lock_guard lock (_gate); record_error (error, false); }

    json snapshot (const json &public_status)
    {
        std::lock_guard lock (_gate);
        json metrics = json::object (), histograms = json::object (), runtime = json::object (), reasons = json::object ();
        baseline_nulls (metrics, histograms, reasons);
        static const char *const outcomes[] = {"sent", "completed", "failed", "timeout", "cancelled", "inflightAtEnd"};
        const auto null_metric = [&] (const std::string &key, const char *code, const std::string &why) {
            metrics[key] = nullptr;
            reasons["/metrics/" + key] = null_reason (code, why);
        };
        for (const std::string key : outcomes) {
            if (_primary)
                metrics["messages." + key] = dec (key == "inflightAtEnd" ? count ("sent") - count ("completed") - count ("failed") - count ("timeout") - count ("cancelled") : count (key));
            else
                null_metric ("messages." + key, "NOT_APPLICABLE", "Echo outcomes belong to the source process.");
        }
        if (_primary) {
            _latency.export_to ("latencyMs", "latency", metrics, histograms, reasons);
        }
        else {
            for (const auto &suffix : latency_suffixes ())
                null_metric (std::string ("latency.") + suffix, "NOT_APPLICABLE", "RTT belongs to the source process.");
            for (const char *key : {"latencyMs"}) {
                histograms[key] = nullptr;
                reasons[std::string ("/histograms/") + key] = null_reason ("NOT_APPLICABLE", "RTT belongs to the source process.");
            }
        }
        const bool cs_client = _config.role == "client" && _config.workload.connections;
        for (const char *key : {"requested", "connected", "failed"}) {
            const std::string name = std::string ("connections.") + key;
            if (cs_client) {
                const auto total = static_cast<std::uint64_t> (*_config.workload.connections);
                const auto clients = static_cast<std::uint64_t> (_config.workload.client_count);
                const auto mine = total / clients + (static_cast<std::uint64_t> (_config.role_instance) < total % clients ? 1 : 0);
                metrics[name] = dec (std::string (key) == "requested" ? mine : std::string (key) == "connected" ? _connected : _connection_failures);
            }
            else
                null_metric (name, "NOT_APPLICABLE", "This process owns no physical connector pool.");
        }
        for (const char *key : {"logicalStreams", "inflightPerStream", "inflight.max"}) {
            const std::string name = std::string ("load.") + key;
            if (_primary && (std::string (key) != "logicalStreams" || !cs_client))
                metrics[name] = dec (std::string (key) == "logicalStreams" ? static_cast<std::uint64_t> (*_config.workload.logical_streams)
                                     : std::string (key) == "inflightPerStream" ? static_cast<std::uint64_t> (_config.workload.inflight)
                                                                                : _max_inflight);
            else
                null_metric (name, "NOT_APPLICABLE", "No server logical streams are owned here; CS slots are connector based.");
        }
        std::uint64_t application_count = 0;
        for (const char *direction : {"request", "send", "reply", "event"}) {
            const auto found = _directional.find (direction);
            const std::uint64_t value = found == _directional.end () ? 0 : found->second;
            application_count += value;
            metrics[std::string ("applicationMessages.") + direction] = dec (value);
            metrics[std::string ("applicationPayloadBytes.") + direction] = dec (value * static_cast<std::uint64_t> (_config.workload.payload_size));
        }
        const std::optional<double> seconds = _start == 0 ? std::nullopt : std::optional (static_cast<double> (_end - _start) / 1e9);
        metrics["throughput.kops"] = _primary && seconds && *seconds > 0 ? json (static_cast<double> (count ("completed")) / *seconds / 1000) : json (nullptr);
        metrics["throughput.messagesPerSec"] = seconds && *seconds > 0 ? json (static_cast<double> (application_count) / *seconds) : json (nullptr);
        metrics["throughput.megabytesPerSec"] = seconds && *seconds > 0
                                                  ? json (static_cast<double> (application_count) * _config.workload.payload_size / *seconds / 1048576)
                                                  : json (nullptr);
        metrics["errors.byKind"] = counts_json (_by_kind);
        metrics["errors.harness"] = counts_json (_harness);
        metrics["errors.language"] = counts_json (_language);
        for (const char *key : {"throughput.kops", "throughput.messagesPerSec", "throughput.megabytesPerSec"})
            if (metrics[key].is_null ())
                reasons[std::string ("/metrics/") + key] = null_reason (_start == 0 ? "PHASE_NOT_STARTED" : "NOT_APPLICABLE", "No applicable completed measurement window.");
        if (_phase == "complete")
            _sampler.export_to (metrics, runtime, reasons);
        else
            for (const char *key : {"process.cpuPercent", "process.rssMb", "process.allocatedMb", "gc.gen0", "gc.gen1", "gc.gen2"}) {
                metrics[key] = nullptr;
                reasons[std::string ("/metrics/") + key] = null_reason ("PHASE_NOT_STARTED", "Process window sampling has not completed.");
            }
        runtime["setupEvidence"] = {{"name", "setupEvidence"}, {"unit", "observation"}, {"type", "array"}, {"value", _setup_evidence.is_array () ? _setup_evidence : json::array ()}};
        runtime["publicReadinessSamples"] = {{"name", "public host readiness and pressure samples"}, {"unit", "observation"}, {"type", "array"}, {"value", _public_state_samples}};
        runtime["errors"] = {{"name", "firstErrors"}, {"unit", "observation"}, {"type", "array"}, {"value", json (_errors)}};
        runtime["activeHandlers"] = {{"name", "application active handlers"}, {"unit", "count"}, {"type", "integer"}, {"value", dec (static_cast<std::uint64_t> (_active_handlers))}};

        json window = {{"startedAtUnixMs", _start_unix ? json (*_start_unix) : json (nullptr)},
                       {"endedAtUnixMs", _end_unix ? json (*_end_unix) : json (nullptr)},
                       {"startTicks", _start == 0 ? json (nullptr) : json (dec (_start))},
                       {"endTicks", _end == 0 ? json (nullptr) : json (dec (_end))},
                       {"measuredSeconds", seconds ? json (*seconds) : json (nullptr)}};
        for (const auto &[key, value] : window.items ())
            if (value.is_null ())
                reasons["/window/" + key] = null_reason ("PHASE_NOT_STARTED", "The measured window has not started.");
        for (const char *key : {"alignmentMethod", "maxErrorNs", "validFromTicks", "validThroughTicks"})
            reasons[std::string ("/clock/") + key] = null_reason ("NOT_APPLICABLE", "RTT uses the caller process clock only.");
        if (public_status.is_null ())
            reasons["/publicStatus"] = null_reason ("NOT_APPLICABLE", "The client has no Framework host runtime.");
        json serialized = json::array ();
        for (std::size_t i = 0; i < _message_types.size (); ++i) {
            serialized.push_back ({{"direction", _message_types[i].first}, {"packetName", _message_types[i].second},
                                   {"logicalPayloadBytes", dec (static_cast<std::uint64_t> (_config.workload.payload_size))},
                                   {"observedSerializedBytes", nullptr}});
            reasons["/serializedMessageBytes/" + std::to_string (i) + "/observedSerializedBytes"] =
              null_reason ("PUBLIC_OBSERVATION_UNSUPPORTED", "No public per-DTO serialized byte observation; the measured message is serialized once by the Framework.");
        }
        json provenance = _config.provenance;
        provenance["pid"] = getpid ();
        char host[256] = {};
        gethostname (host, sizeof host - 1);
        provenance["host"] = host;
        provenance["messageCountScope"] = "application-call-boundaries";
        provenance["configHash"] = _config.config_hash;
        provenance["resetAcknowledgement"] = _reset_ack ? *_reset_ack : json (nullptr);
        provenance["primaryEchoOwner"] = _primary;
        provenance["runtimeVersion"] = std::string ("gcc ") + __VERSION__;
        provenance["effectiveProcessorCount"] = std::thread::hardware_concurrency ();
        provenance["executor"] = {{"name", "Framework host coroutine executor"},
                                  {"threads", "not publicly observable; see effectiveProcessorCount"}};
        json snapshot = {{"schemaVersion", 2}, {"runId", _config.run_id}, {"cellId", _config.cell_id}, {"resetSeq", _reset_seq},
                         {"language", _config.language}, {"role", _config.role}, {"roleInstance", _config.role_instance},
                         {"configHash", _config.config_hash}, {"phase", _phase}, {"window", window}, {"clock", clock_metadata ()},
                         {"serializedMessageBytes", serialized}, {"metrics", metrics}, {"histograms", histograms},
                         {"nullReasons", reasons}, {"publicStatus", public_status}, {"publicMetrics", json::array ()},
                         {"runtimeMetrics", runtime}, {"provenance", provenance}};
        for (const auto &hook : _enrich)
            hook (snapshot);
        return snapshot;
    }

  private:
    trigger_reply_t ack (const trigger_request_t &trigger, bool accepted, const std::string &state, std::optional<std::string> reason) const
    {
        trigger_reply_t out;
        out.run_id = trigger.run_id; out.cell_id = trigger.cell_id; out.reset_seq = trigger.reset_seq; out.phase = trigger.phase;
        out.accepted = accepted; out.state = state; out.reason = std::move (reason); out.config_hash = _config.config_hash;
        return out;
    }

    void run_phase (workload_fn_t workload)
    {
        const auto loops = std::make_shared<loop_group_t> ();
        try {
            if (workload)
                workload (loops);
        }
        catch (...) {
            record_diagnostic (std::current_exception ());
        }
        if (_sample_public_state)
            add_public_state (_sample_public_state ());
        while (true) {
            const auto remaining = _end - now_ticks ();
            if (remaining <= 0)
                break;
            std::this_thread::sleep_for (std::chrono::nanoseconds (std::min<std::int64_t> (100'000'000, remaining)));
            // Public runtime status may marshal to a runtime lane; never call it under the counter lock.
            json public_state = _sample_public_state ? _sample_public_state () : json (nullptr);
            std::lock_guard lock (_gate);
            _sampler.sample ();
            if (!public_state.is_null ())
                _public_state_samples.push_back (public_state);
        }
        const bool warmup = _phase == "warmup";
        {
            std::lock_guard lock (_gate);
            _sampler.end ();
            _end_unix = unix_ms ();
            if (!warmup) {
                _sealed = true;
                _phase = "complete";
                _phase_complete = true;
                _phase_done.notify_all ();
                return;
            }
        }
        // Setup requires every warmup callback to reach a terminal result before reset clears its counters.
        loops->wait_idle ();
        std::lock_guard lock (_gate);
        _sealed = true;
        _phase = "complete";
        _phase_complete = true;
        _phase_done.notify_all ();
    }

    void add_public_state (json state)
    {
        std::lock_guard lock (_gate);
        if (!state.is_null ())
            _public_state_samples.push_back (std::move (state));
    }

    std::uint64_t count (const std::string &key) const
    {
        const auto found = _counts.find (key);
        return found == _counts.end () ? 0 : found->second;
    }
    static json counts_json (const std::unordered_map<std::string, std::uint64_t> &values)
    {
        json out = json::object ();
        for (const auto &[key, value] : values)
            out[key] = dec (value);
        return out;
    }

    void record_error (std::exception_ptr error, bool outcome)
    {
        std::string category = "failed";
        json evidence = {{"type", nullptr}, {"message", nullptr}, {"publicKind", nullptr}, {"harnessKind", nullptr}, {"connectorCode", nullptr}};
        try {
            std::rethrow_exception (error);
        }
        catch (const zlink::framework::framework_exception_t &framework) {
            const std::string kind = kind_name (framework.kind ());
            ++_by_kind[kind];
            if (framework.kind () == zlink::framework::framework_error_kind_t::deadline_exceeded)
                category = "timeout";
            evidence = {{"type", type_name (typeid (framework))}, {"message", framework.what ()}, {"publicKind", kind}, {"harnessKind", nullptr}, {"connectorCode", nullptr}};
        }
        catch (const validation_error_t &validation) {
            ++_harness[validation.kind ()];
            if (validation.kind () == "CorrelationExpired")
                category = "timeout"; // §13: an expired correlation is a timeout
            evidence = {{"type", type_name (typeid (validation))}, {"message", validation.what ()}, {"publicKind", nullptr}, {"harnessKind", validation.kind ()}, {"connectorCode", nullptr}};
        }
        catch (const connector_error_t &connector) {
            ++_language["zlink::stream_connector::error_t"];
            if (connector.timeout ())
                category = "timeout";
            evidence = {{"type", "zlink::stream_connector::error_t"}, {"message", connector.what ()}, {"publicKind", nullptr}, {"harnessKind", nullptr}, {"connectorCode", connector.code ()}};
        }
        catch (const std::system_error &system) {
            ++_language[type_name (typeid (system))];
            if (system.code () == std::make_error_code (std::errc::timed_out))
                category = "timeout";
            else if (system.code () == std::make_error_code (std::errc::operation_canceled))
                category = "cancelled";
            evidence = {{"type", type_name (typeid (system))}, {"message", system.what ()}, {"publicKind", nullptr},
                        {"harnessKind", nullptr}, {"connectorCode", system.code ().message ()}};
        }
        catch (const std::exception &other) {
            ++_language[type_name (typeid (other))];
            evidence = {{"type", type_name (typeid (other))}, {"message", other.what ()}, {"publicKind", nullptr}, {"harnessKind", nullptr}, {"connectorCode", nullptr}};
        }
        catch (...) {
            ++_language["unknown exception"];
        }
        if (outcome)
            ++_counts[category];
        if (_errors.size () < 32)
            _errors.push_back (evidence);
    }

    role_config_t _config;
    bool _primary;
    payload_pattern_t _pattern;
    mutable std::mutex _gate;
    mutable std::condition_variable _phase_done;
    process_sampler_t _sampler;
    histogram_t _latency;
    std::unordered_map<std::string, std::uint64_t> _counts, _by_kind, _harness, _language, _directional;
    std::vector<json> _errors;
    json _public_state_samples = json::array ();
    std::uint64_t _inflight = 0, _max_inflight = 0, _connected = 0, _connection_failures = 0;
    int _active_handlers = 0;
    std::int64_t _start = 0, _end = 0;
    std::optional<std::string> _start_unix, _end_unix;
    std::string _phase = "setup", _reset_seq = "0";
    bool _sealed = false, _phase_complete = true;
    std::atomic<bool> _final{false};
    std::optional<json> _reset_ack;
    std::unordered_map<std::string, trigger_reply_t> _starts;
    std::thread _phase_thread;
    json _setup_evidence = json::array ();
    std::function<json ()> _sample_public_state;
    std::vector<std::function<void ()>> _on_reset;
    std::vector<std::function<void (json &)>> _enrich;
    std::vector<std::pair<std::string, std::string>> _message_types{{"request", "PerfEchoRequest"}, {"reply", "PerfEchoReply"}};
};
// The application handler turn of §13: the role is drained only while no handler scope is alive.
class handler_scope_t
{
  public:
    explicit handler_scope_t (measurement_t &measurement) : _measurement (measurement) { _measurement.handler_enter (); }
    ~handler_scope_t () { _measurement.handler_exit (); }
    handler_scope_t (const handler_scope_t &) = delete;
    handler_scope_t &operator= (const handler_scope_t &) = delete;

  private:
    measurement_t &_measurement;
};
} // namespace perf
