/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

// §10.11 Publisher: one process issues every sequence of the run. Each logical stream awaits the public publish admission
// (`publisher_t.publish(channel,topic,event).async()`); nothing waits for a subscriber. Delivery is not observed here: the
// Subscribers' own originals are intersected with this process's window-success set by the runner (§15.4).
// Publisher x1 (source, this file), Subscriber x subscriberCount (each its own process); publish, ordinary; payload 1024 bytes.
// Store: run Docker Redis (automatic discovery). Null: echo completed/KOPS/echo latency; Spot, worker, Actor.

#include <perf/server/fanout_support.hpp>
#include <perf/server/stream_loops.hpp>

namespace perf
{
class pubsub_fanout_echo_scenario_t
{
  public:
    pubsub_fanout_echo_scenario_t (role_t &role, std::string cell_directory) :
        _role (role), _sequence_file (cell_directory + "/publisher-sequences.json"), _sets (std::make_shared<published_sets_t> ())
    {
        auto &measurement = role.measurement;
        measurement.add_on_reset ([this] {
            std::atomic_store (&_sets, std::make_shared<published_sets_t> ());
            _measured_base = _issued.load ();
        });
        measurement.set_message_types ({{"event", "PerfPublishEvent"}});
        measurement.add_enrich_snapshot ([this] (json &snapshot) { enrich (snapshot); });
    }

    // The Publisher has no subscriber-facing status: its host Ready plus the Subscribers' public Ready (fanout runtime status,
    // one per Subscriber process) is the prepared state.
    void prepare (const std::atomic<bool> &stopping)
    {
        wait_for_public (_role, stopping, [&] { return _role.runtime.load ()->status ().is_ready; }, "the Publisher host");
        const auto status = _role.runtime.load ()->status ();
        _role.objects->set (true, "", json::array ({{{"kind", "publisherHostReady"}, {"source", "framework_runtime_t.status"},
                                                     {"observedValue", {{"state", static_cast<int> (status.state)}, {"isReady", status.is_ready}, {"acceptingWork", status.accepting_work}}}}}));
    }

    void run (const loops_t &loops) { spawn_stream_loops (loops, _role, [this] (int stream) { return loop (stream); }); }

  private:
    struct published_sets_t
    {
        std::mutex gate;
        sequence_bit_set_t window;
        void record_window_success (std::uint64_t sequence)
        {
            std::lock_guard lock (gate);
            window.try_set (sequence);
        }
    };

    fw::task_t<void> loop (int)
    {
        auto &measurement = _role.measurement;
        auto &fanout = _role.service<fw::publisher_t> ();
        const auto &config = _role.config;
        const auto reset_seq = measurement.reset_seq ();
        const bool warmup = reset_seq == "0";
        const auto sets = std::atomic_load (&_sets);
        publish_event_t message;
        message.run_id = config.run_id;
        message.cell_id = config.cell_id;
        message.reset_seq = reset_seq;
        message.phase = warmup ? "warmup" : "measured";
        message.topic = fanout_topic;
        message.clock_domain_id = clock_domain ();
        message.payload = measurement.pattern ().base64 ();
        std::int64_t started = 0;
        if (!measurement.begin_operation (started, "event"))
            co_return;
        const auto sequence = _issued.fetch_add (1) + 1;
        message.sequence = dec (sequence);
        message.sent_ticks = dec (started);
        std::exception_ptr error;
        try {
            co_await fanout.publish (*config.channel_name, fanout_topic, message).async ();
        }
        catch (...) {
            error = std::current_exception ();
        }
        if (error) {
            measurement.complete_operation (started, error);
            co_return;
        }
        const auto completed = now_ticks ();
        if (measurement.complete_operation (started, nullptr, completed))
            sets->record_window_success (sequence);
        if (warmup) {
            if (!measurement.has_setup_evidence ())
                measurement.set_setup_evidence (json::array ({{{"kind", "warmupMarkerPublished"}, {"source", "publisher_t.publish.async"}, {"observedValue", message.sequence}}}));
        }
    }

    void enrich (json &snapshot)
    {
        const auto current = std::atomic_load (&_sets);
        const auto &measurement = _role.measurement;
        const bool final = measurement.final_snapshot () && snapshot["phase"] == "complete" && snapshot["resetSeq"] == "1";
        std::uint64_t published_in_window;
        json window_success;
        {
            std::lock_guard lock (current->gate);
            published_in_window = current->window.count ();
            if (final)
                window_success = current->window.ranges ();
        }
        fanout_metrics::apply_common (snapshot, false);
        fanout_metrics::value (snapshot, "messages.publishedInWindow", dec (published_in_window));
        const auto &seconds = snapshot["window"]["measuredSeconds"];
        if (seconds.is_number () && seconds.get<double> () > 0)
            fanout_metrics::value (snapshot, "fanout.publishOpsPerSec", static_cast<double> (published_in_window) / seconds.get<double> ());
        else
            fanout_metrics::null_key (snapshot, "fanout.publishOpsPerSec", "PHASE_NOT_STARTED", "No measured window has run.");
        snapshot["provenance"]["fanout"] = {{"channelName", _role.config.channel_name}, {"topic", fanout_topic}, {"noDrop", true},
                                            {"socketSendTimeoutMs", _role.config.workload.socket_send_timeout_ms},
                                            {"publisherSequenceScope", "one counter per run; warmup and measured ranges are disjoint"},
                                            {"sequenceOriginal", "publisher-sequences.json"}};
        if (!final)
            return;
        json attempted = json::array ();
        const auto last = _issued.load ();
        if (last > _measured_base)
            attempted.push_back ({{"first", dec (_measured_base + 1)}, {"last", dec (last)}});
        json original = {{"runId", _role.config.run_id}, {"cellId", _role.config.cell_id}, {"resetSeq", snapshot["resetSeq"]}, {"phase", "measured"},
                         {"attemptedRanges", attempted}, {"windowSuccessRanges", window_success}};
        write_once (_sequence_file, original);
    }

    role_t &_role;
    std::string _sequence_file;
    std::atomic<std::uint64_t> _issued{0};  // run-wide: warmup and measured ranges never overlap
    std::uint64_t _measured_base = 0;
    std::shared_ptr<published_sets_t> _sets;
};
} // namespace perf
