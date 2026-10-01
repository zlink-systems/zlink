/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

// §10.11 Subscriber evidence: which measured sequences this process first received inside its own window (§15.4).
// It counts nothing the Publisher published; the runner intersects both originals.

#include <perf/server/fanout_support.hpp>

namespace perf
{
class fanout_receipts_t
{
  public:
    fanout_receipts_t (role_t &role, std::string cell_directory) :
        _role (role),
        _sequence_file (cell_directory + "/subscriber-" + std::to_string (role.config.role_instance) + "-sequences.json"),
        _round (std::make_shared<round_t> ())
    {
        auto &measurement = role.measurement;
        measurement.add_on_reset ([this] { std::atomic_store (&_round, std::make_shared<round_t> ()); });
        measurement.set_message_types ({{"event", "PerfPublishEvent"}});
        measurement.add_enrich_snapshot ([this] (json &snapshot) { enrich (snapshot); });
    }

    // objectsReady: this Subscriber's public fanout status shows a Ready publisher (§16.1).
    void prepare (const std::atomic<bool> &stopping)
    {
        auto &fanout = _role.service<fw::fanout_runtime_t> ();
        const auto &channel = *_role.config.channel_name;
        wait_for_public (_role, stopping, [&] { return fanout.snapshot (channel).ready_publisher_count > 0; }, "a Ready publisher");
        const auto status = fanout.snapshot (channel);
        _role.objects->set (true, "", json::array ({{{"kind", "fanoutStatus"}, {"source", "fanout_runtime_t.snapshot"},
                                                     {"observedValue", {{"channelName", channel}, {"publisherCount", status.publishers.size ()},
                                                                        {"readyPublisherCount", status.ready_publisher_count}}}}}));
    }

    void record (const publish_event_t &message, std::int64_t received_ticks)
    {
        auto &measurement = _role.measurement;
        const auto current = std::atomic_load (&_round);
        if (message.run_id != _role.config.run_id || message.cell_id != _role.config.cell_id || message.topic != fanout_topic
            || (message.phase != "warmup" && message.phase != "measured") || message.clock_domain_id.empty ())
            throw validation_error_t ("IdentityMismatch", "Fanout event identity does not match the cell.");
        const auto sequence = parse_u64 (message.sequence);
        (void) parse_i64 (message.sent_ticks);
        measurement.pattern ().validate (message.payload);
        if (message.phase == "warmup") {
            if (parse_u64 (message.reset_seq) != 0)
                throw validation_error_t ("PhaseMismatch", "Warmup event carries a measured resetSeq.");
            ++current->warmup_events;
            if (!measurement.has_setup_evidence ())
                measurement.set_setup_evidence (json::array ({{{"kind", "warmupMarker"}, {"source", "typed fanout handler (PerfPublishEvent)"}, {"observedValue", message.sequence}}}));
            if (measurement.reset_seq () != "0")
                ++current->ignored_warmup_in_measured;
            return;
        }
        if (message.reset_seq != measurement.reset_seq ())
            throw validation_error_t ("PhaseMismatch", "Measured event resetSeq differs from this epoch.");
        const auto start_ticks = measurement.start_ticks ();
        const auto end_ticks = measurement.end_ticks ();
        std::lock_guard lock (current->gate);
        if (!current->seen.try_set (sequence))
            ++current->duplicates;
        else if (start_ticks == 0 || received_ticks < start_ticks || received_ticks >= end_ticks)
            ++current->outside_window;
        else
            current->window.try_set (sequence);
    }

  private:
    struct round_t
    {
        std::mutex gate;
        sequence_bit_set_t window, seen;
        std::atomic<std::uint64_t> duplicates{0}, warmup_events{0}, ignored_warmup_in_measured{0}, outside_window{0};
    };

    void enrich (json &snapshot)
    {
        const auto current = std::atomic_load (&_round);
        const auto &measurement = _role.measurement;
        const bool final = measurement.final_snapshot () && snapshot["phase"] == "complete" && snapshot["resetSeq"] == "1";
        std::uint64_t unique_in_window, measured_events_seen, retained_bytes, duplicates;
        json window_ranges;
        {
            std::lock_guard lock (current->gate);
            unique_in_window = current->window.count ();
            retained_bytes = current->window.retained_bytes () + current->seen.retained_bytes ();
            measured_events_seen = current->seen.count ();
            duplicates = current->duplicates.load ();
            if (final)
                window_ranges = current->window.ranges ();
        }
        fanout_metrics::apply_common (snapshot, true);
        fanout_metrics::value (snapshot, "fanout.duplicateEvents", dec (duplicates));
        snapshot["runtimeMetrics"]["fanoutReceipts"] = {{"name", "subscriber receipts"}, {"unit", "event"}, {"type", "object"},
            {"value", {{"uniqueInWindow", dec (unique_in_window)}, {"measuredEventsSeen", dec (measured_events_seen)},
                       {"warmupEvents", dec (current->warmup_events.load ())},
                       {"warmupInMeasuredEpoch", dec (current->ignored_warmup_in_measured.load ())},
                       {"measuredOutsideWindow", dec (current->outside_window.load ())}}}};
        const auto original = "subscriber-" + std::to_string (_role.config.role_instance) + "-sequences.json";
        snapshot["provenance"]["fanout"] = {{"channelName", _role.config.channel_name}, {"topic", fanout_topic}, {"subscribedTopics", json::array ()},
            {"delivery", "typed fanout subscriber handler (PerfPublishEvent)"},
            {"sequenceEvidence", {{"method", "one bit per measured sequence seen and one bit per window receipt"},
                                  {"retainedBytes", dec (retained_bytes)},
                                  {"timingEvidence", "not collected: no shared clock domain"}, {"original", original}}}};
        if (!final)
            return;
        write_once (_sequence_file,
                    {{"runId", _role.config.run_id}, {"cellId", _role.config.cell_id}, {"resetSeq", snapshot["resetSeq"]}, {"phase", "measured"},
                     {"subscriberId", _role.config.role_instance}, {"windowRanges", window_ranges},
                     {"duplicateEvents", dec (duplicates)},
                     {"nullReasons", {{"/timingEvidence", null_reason ("CLOCK_DOMAIN_UNVERIFIED", "Publisher and Subscriber use process-local monotonic clocks; no shared clock domain is verified (§15.2).")}}},
                     {"timingEvidence", nullptr}});
    }

    role_t &_role;
    std::string _sequence_file;
    std::shared_ptr<round_t> _round;
};

// The typed fanout handler validates each event and records its unique sequence.
class perf_fanout_handler_t
{
  public:
    using event_type = publish_event_t;
    perf_fanout_handler_t (role_t &role, fanout_receipts_t &receipts) : _role (role), _receipts (receipts) {}
    fw::task_t<void> handle (const publish_event_t &message)
    {
        const auto received_ticks = now_ticks ();
        const handler_scope_t scope (_role.measurement);
        try {
            _receipts.record (message, received_ticks);
        }
        catch (...) {
            _role.measurement.record_diagnostic (std::current_exception ());
            throw;
        }
        co_return;
    }

  private:
    role_t &_role;
    fanout_receipts_t &_receipts;
};
} // namespace perf
