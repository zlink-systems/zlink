/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

// §10.11 Subscriber evidence: which measured sequences this process received first inside its own window or settle (§15.4).
// It counts nothing the Publisher published; the runner intersects both originals.

#include <perf/server/fanout_support.hpp>

namespace perf
{
class fanout_receipts_t
{
  public:
    fanout_receipts_t (role_t &role, std::string cell_directory) :
        _role (role),
        _sequence_file (cell_directory + "/subscriber-" + std::to_string (role.config.role_instance)
                        + "-sequences.json"),
        _round (std::make_shared<round_t> ())
    {
        auto &measurement = role.measurement;
        measurement.add_on_reset (
          [this] { std::atomic_store (&_round, std::make_shared<round_t> ()); });
        measurement.set_message_types ({{"event", "PerfPublishEvent"}});
        measurement.add_enrich_snapshot ([this] (json &snapshot) { enrich (snapshot); });
    }

    // objectsReady: this Subscriber's public fanout status shows a Ready publisher (§16.1).
    void prepare (const std::atomic<bool> &stopping)
    {
        auto &fanout = _role.service<fw::fanout_runtime_t> ();
        const auto &channel = *_role.config.channel_name;
        wait_for_public (
          _role, stopping, [&] { return fanout.snapshot (channel).ready_publisher_count > 0; },
          "a Ready publisher");
        const auto status = fanout.snapshot (channel);
        _role.objects->set (
          true, "",
          json::array ({{{"kind", "fanoutStatus"},
                         {"source", "fanout_runtime_t.snapshot"},
                         {"observedValue",
                          {{"channelName", channel},
                           {"publisherCount", status.publishers.size ()},
                           {"readyPublisherCount", status.ready_publisher_count}}}}}));
    }

    void record (const publish_event_t &message)
    {
        auto &measurement = _role.measurement;
        const auto current = std::atomic_load (&_round);
        if (message.run_id != _role.config.run_id || message.cell_id != _role.config.cell_id
            || message.topic != fanout_topic
            || (message.phase != "warmup" && message.phase != "measured")
            || message.clock_domain_id.empty ())
            throw validation_error_t ("IdentityMismatch",
                                      "Fanout event identity does not match the cell.");
        const auto sequence = parse_u64 (message.sequence);
        (void) parse_i64 (message.sent_ticks);
        measurement.pattern ().validate (message.payload);
        if (message.phase == "warmup") {
            if (parse_u64 (message.reset_seq) != 0)
                throw validation_error_t ("PhaseMismatch",
                                          "Warmup event carries a measured resetSeq.");
            ++current->warmup_events;
            if (!measurement.has_setup_evidence ())
                measurement.set_setup_evidence (
                  json::array ({{{"kind", "warmupMarker"},
                                 {"source", "typed fanout handler (PerfPublishEvent)"},
                                 {"observedValue", message.sequence}}}));
            if (measurement.reset_seq () != "0")
                ++current->ignored_warmup_in_measured;
            return;
        }
        if (message.reset_seq != measurement.reset_seq ())
            throw validation_error_t ("PhaseMismatch",
                                      "Measured event resetSeq differs from this epoch.");
        // The runner ends the settle (§4.1): receipts after the window are settle until it reads the final snapshot.
        const auto phase = measurement.phase ();
        sequence_bit_set_t *target = nullptr;
        if (!current->sealed.load ())
            target = phase == "measured"                          ? &current->window
                     : (phase == "settle" || phase == "complete") ? &current->settle
                                                                  : nullptr;
        if (!target)
            ++current->outside_window;
        else if (current->window.contains (sequence) || current->settle.contains (sequence)
                 || !target->try_set (sequence))
            ++current->duplicates;
    }

  private:
    struct round_t
    {
        sequence_bit_set_t window, settle;
        std::atomic<std::uint64_t> duplicates{0}, warmup_events{0}, ignored_warmup_in_measured{0},
          outside_window{0};
        std::atomic<bool> sealed{
          false}; // set when the runner collects the final snapshot: later events are missing deliveries
    };

    void enrich (json &snapshot)
    {
        const auto current = std::atomic_load (&_round);
        const auto &measurement = _role.measurement;
        fanout_metrics::apply_common (snapshot, true);
        fanout_metrics::value (snapshot, "fanout.duplicateEvents",
                               dec (current->duplicates.load ()));
        snapshot["runtimeMetrics"]["fanoutReceipts"] = {
          {"name", "subscriber receipts"},
          {"unit", "event"},
          {"type", "object"},
          {"value",
           {{"uniqueInWindow", dec (current->window.count ())},
            {"uniqueInSettle", dec (current->settle.count ())},
            {"warmupEvents", dec (current->warmup_events.load ())},
            {"warmupInMeasuredEpoch", dec (current->ignored_warmup_in_measured.load ())},
            {"measuredOutsideWindow", dec (current->outside_window.load ())}}}};
        const auto original =
          "subscriber-" + std::to_string (_role.config.role_instance) + "-sequences.json";
        snapshot["provenance"]["fanout"] = {
          {"channelName", _role.config.channel_name},
          {"topic", fanout_topic},
          {"subscribedTopics", json::array ()},
          {"delivery", "typed fanout subscriber handler (PerfPublishEvent)"},
          {"sequenceEvidence",
           {{"method", "one bit per received sequence, window and settle sets"},
            {"retainedBytes",
             dec (current->window.retained_bytes () + current->settle.retained_bytes ())},
            {"timingEvidence", "not collected: no shared clock domain"},
            {"original", original}}}};
        if (!measurement.final_snapshot () || snapshot["phase"] != "complete"
            || snapshot["resetSeq"] != "1")
            return;
        current->sealed = true;
        write_once (_sequence_file,
                    {{"runId", _role.config.run_id},
                     {"cellId", _role.config.cell_id},
                     {"resetSeq", snapshot["resetSeq"]},
                     {"phase", "measured"},
                     {"subscriberId", _role.config.role_instance},
                     {"windowRanges", current->window.ranges ()},
                     {"settleRanges", current->settle.ranges ()},
                     {"duplicateEvents", dec (current->duplicates.load ())},
                     {"nullReasons",
                      {{"/timingEvidence",
                        null_reason ("CLOCK_DOMAIN_UNVERIFIED",
                                     "Publisher and Subscriber use process-local monotonic clocks; "
                                     "no shared clock domain is verified (§15.2).")}}},
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
    perf_fanout_handler_t (role_t &role, fanout_receipts_t &receipts) :
        _role (role), _receipts (receipts)
    {
    }
    fw::task_t<void> handle (const publish_event_t &message)
    {
        const handler_scope_t scope (_role.measurement);
        try {
            _receipts.record (message);
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
