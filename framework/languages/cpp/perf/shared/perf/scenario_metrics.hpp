/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

// The §14 family keys a scenario fills beside the shared echo counters (driver.*, spot.*, worker.*, messages.admitted...).
// Counters and histograms clear with the window at reset and are written into every snapshot; a key the scenario
// declares as unsupported keeps its null with the public-observation reason. Keys it does not name stay as
// measurement_t wrote them (null, NOT_APPLICABLE).

#include <perf/measurement.hpp>

#include <map>

namespace perf
{
class scenario_metrics_t
{
  public:
    explicit scenario_metrics_t (measurement_t &measurement) : _measurement (measurement)
    {
        measurement.add_on_reset ([this] { reset (); });
        measurement.add_enrich_snapshot ([this] (json &snapshot) { enrich (snapshot); });
    }
    scenario_metrics_t (const scenario_metrics_t &) = delete;
    scenario_metrics_t &operator= (const scenario_metrics_t &) = delete;

    // Counter keys the scenario measures; they read "0" until observed instead of staying null.
    scenario_metrics_t &counters (std::initializer_list<const char *> keys)
    {
        std::lock_guard lock (_gate);
        for (const char *key : keys)
            _counts.try_emplace (key, 0);
        return *this;
    }
    // A latency histogram (§15.3): the histogram key and the dotted metric prefix it exports.
    scenario_metrics_t &latency (const std::string &histogram_key, const std::string &prefix)
    {
        std::lock_guard lock (_gate);
        _histograms.insert_or_assign (histogram_key, entry_t{prefix, histogram_t ()});
        return *this;
    }
    scenario_metrics_t &unsupported (const std::string &code, const std::string &reason, std::initializer_list<const char *> keys)
    {
        std::lock_guard lock (_gate);
        for (const char *key : keys)
            _unsupported[key] = {code, reason};
        return *this;
    }
    // §14.1: no public observation of a Spot's mailbox or turn internals; a Spot workload keeps these null with that reason.
    scenario_metrics_t &spot_internals_unsupported ()
    {
        return unsupported ("PUBLIC_OBSERVATION_UNSUPPORTED",
                            "Public status is a host aggregate; no per-Spot mailbox, turn or resume observation exists.",
                            {"spot.mailboxDepth.max", "spot.mailboxDepth.mean", "spot.suspendedTurns", "spot.resumedTurns",
                             "spot.resumeLatency.p95Ms", "spot.resumeLatency.p99Ms"});
    }
    // A metric family that is the same interval as another (§15.3: spot.remoteCallLatency.* is latency.*).
    scenario_metrics_t &alias_latency (const std::string &from_prefix, const std::string &to_prefix)
    {
        std::lock_guard lock (_gate);
        _aliases.emplace_back (from_prefix, to_prefix);
        return *this;
    }
    scenario_metrics_t &provenance (const std::string &key, json value)
    {
        std::lock_guard lock (_gate);
        _provenance[key] = std::move (value);
        return *this;
    }
    // State a scenario keeps beside the counters (a correlation table) clears with the same reset.
    void on_reset (std::function<void ()> reset)
    {
        std::lock_guard lock (_gate);
        _resets.push_back (std::move (reset));
    }

    void count (const std::string &key, std::uint64_t amount = 1)
    {
        std::lock_guard lock (_gate);
        _counts[key] += amount;
    }
    // Only a sample whose operation finished inside the measured window belongs to the window histogram.
    void record (const std::string &histogram_key, std::int64_t started, std::int64_t completed)
    {
        record (histogram_key, started, completed, completed);
    }
    // window_ticks: when the operation this interval belongs to finished (a worker interval ends before its operation does).
    void record (const std::string &histogram_key, std::int64_t started, std::int64_t ended, std::int64_t window_ticks)
    {
        if (window_ticks < _measurement.start_ticks () || window_ticks >= _measurement.end_ticks ())
            return;
        std::lock_guard lock (_gate);
        _histograms.at (histogram_key).histogram.record (ended - started);
    }

  private:
    struct entry_t
    {
        std::string prefix;
        histogram_t histogram;
    };

    void reset ()
    {
        std::lock_guard lock (_gate);
        for (auto &[key, value] : _counts)
            value = 0;
        for (auto &[key, value] : _histograms)
            value.histogram = histogram_t ();
        for (auto &reset : _resets)
            reset ();
    }

    void enrich (json &snapshot)
    {
        std::lock_guard lock (_gate);
        auto &metrics = snapshot["metrics"];
        auto &reasons = snapshot["nullReasons"];
        for (const auto &[key, value] : _counts) {
            metrics[key] = dec (value);
            reasons.erase ("/metrics/" + key);
        }
        for (const auto &[key, value] : _histograms) {
            for (const auto &suffix : latency_suffixes ())
                reasons.erase ("/metrics/" + value.prefix + "." + suffix);
            reasons.erase ("/histograms/" + key);
            value.histogram.export_to (key, value.prefix, metrics, snapshot["histograms"], reasons);
        }
        for (const auto &[from, to] : _aliases)
            for (const auto &suffix : latency_suffixes ()) {
                metrics[to + "." + suffix] = metrics[from + "." + suffix];
                if (reasons.contains ("/metrics/" + from + "." + suffix))
                    reasons["/metrics/" + to + "." + suffix] = reasons["/metrics/" + from + "." + suffix];
                else
                    reasons.erase ("/metrics/" + to + "." + suffix);
            }
        for (const auto &[key, why] : _unsupported) {
            metrics[key] = nullptr;
            reasons["/metrics/" + key] = null_reason (why.first, why.second, "spec/server/06-observability/01-runtime-monitoring");
        }
        for (const auto &[key, value] : _provenance)
            snapshot["provenance"][key] = value;
    }

    measurement_t &_measurement;
    std::mutex _gate;
    std::map<std::string, std::uint64_t> _counts;
    std::map<std::string, entry_t> _histograms;
    std::vector<std::function<void ()>> _resets;
    std::vector<std::pair<std::string, std::string>> _aliases;
    std::map<std::string, std::pair<std::string, std::string>> _unsupported;
    std::map<std::string, json> _provenance;
};
} // namespace perf
