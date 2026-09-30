/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

// Perf spec §15.4: sequence originals of the PS cells (`publisher-sequences.json`, `subscriber-<id>-sequences.json`) and the
// metric keys a PS role owns or hands to the runner's sequence intersection (§14). The Publisher and Subscriber roles include
// this file, so the original format has one definition.

#include <perf/server/server_application.hpp>

#include <filesystem>
#include <map>

namespace perf
{
inline constexpr const char *fanout_topic = "perf.echo";

// A set of U64 sequences kept as a chunked bit set: one bit per sequence, safe for concurrent writers.
class sequence_bit_set_t
{
  public:
    // False when the sequence was already in the set.
    bool try_set (std::uint64_t sequence)
    {
        auto *chunk = chunk_of (sequence / chunk_bits, true);
        const auto bit = static_cast<std::size_t> (sequence % chunk_bits);
        const std::uint64_t mask = 1ULL << (bit & 63);
        if ((chunk[bit >> 6].fetch_or (mask) & mask) != 0)
            return false;
        _count.fetch_add (1);
        return true;
    }
    bool contains (std::uint64_t sequence) const
    {
        const auto *chunk = const_cast<sequence_bit_set_t *> (this)->chunk_of (sequence / chunk_bits, false);
        if (!chunk)
            return false;
        const auto bit = static_cast<std::size_t> (sequence % chunk_bits);
        return (chunk[bit >> 6].load () & (1ULL << (bit & 63))) != 0;
    }
    std::uint64_t count () const noexcept { return _count.load (); }
    std::uint64_t retained_bytes () const
    {
        std::lock_guard lock (_gate);
        return _chunks.size () * chunk_words * sizeof (std::uint64_t);
    }
    // Maximal contiguous intervals, ascending, both ends inclusive (§15.4): [{first,last}] with U64 decimal strings.
    json ranges () const
    {
        std::vector<std::pair<std::uint64_t, std::uint64_t>> found;
        std::optional<std::uint64_t> open;
        std::uint64_t next = 0; // the sequence expected next while an interval is open
        const auto close = [&] {
            if (open)
                found.emplace_back (*open, next - 1);
            open.reset ();
        };
        std::lock_guard lock (_gate);
        for (const auto &[index, chunk] : _chunks)
            for (std::size_t word = 0; word < chunk_words; ++word) {
                std::uint64_t bits = chunk[word].load ();
                const std::uint64_t position = index * chunk_bits + word * 64;
                if (bits == 0) {
                    close ();
                    continue;
                }
                while (bits != 0) {
                    const int low = __builtin_ctzll (bits);
                    const auto shifted = ~(bits >> low);
                    const int run = shifted == 0 ? 64 - low : __builtin_ctzll (shifted);
                    const std::uint64_t first = position + static_cast<std::uint64_t> (low);
                    if (!open)
                        open = first;
                    else if (first != next) {
                        close ();
                        open = first;
                    }
                    next = first + static_cast<std::uint64_t> (run);
                    bits = run + low >= 64 ? 0 : bits & ~(((1ULL << run) - 1) << low);
                }
                // An interval that stops before the end of this word is closed by the next gap or the end of input.
                if ((chunk[word].load () >> 63) == 0)
                    close ();
            }
        close ();
        json out = json::array ();
        for (const auto &[first, last] : found)
            out.push_back ({{"first", dec (first)}, {"last", dec (last)}});
        return out;
    }

  private:
    static constexpr std::uint64_t chunk_bits = 1ULL << 18;
    static constexpr std::size_t chunk_words = chunk_bits / 64;
    using chunk_t = std::unique_ptr<std::atomic<std::uint64_t>[]>;

    std::atomic<std::uint64_t> *chunk_of (std::uint64_t index, bool create)
    {
        std::lock_guard lock (_gate);
        auto found = _chunks.find (index);
        if (found == _chunks.end ()) {
            if (!create)
                return nullptr;
            chunk_t chunk (new std::atomic<std::uint64_t>[chunk_words]);
            for (std::size_t word = 0; word < chunk_words; ++word)
                chunk[word].store (0);
            found = _chunks.emplace (index, std::move (chunk)).first;
        }
        return found->second.get ();
    }

    mutable std::mutex _gate;
    std::map<std::uint64_t, chunk_t> _chunks;
    std::atomic<std::uint64_t> _count{0};
};

// Metric keys a PS role owns or hands to the runner's sequence intersection (§14, §15.4).
namespace fanout_metrics
{
inline void value (json &snapshot, const std::string &key, json value)
{
    snapshot["metrics"][key] = std::move (value);
    snapshot["nullReasons"].erase ("/metrics/" + key);
}
inline void null_key (json &snapshot, const std::string &key, const char *code, const std::string &why)
{
    snapshot["metrics"][key] = nullptr;
    snapshot["nullReasons"]["/metrics/" + key] = null_reason (code, why);
}
// Every PS role: the echo outcomes and echo latency do not apply (§10.11); delivery is intersected by the runner.
inline void apply_common (json &snapshot, bool has_delivery_owner)
{
    for (const char *key : {"fanout.subscriberCount", "fanout.uniqueDelivered", "fanout.deliveredInWindow", "fanout.settleDelivered",
                            "fanout.outOfCohortEvents", "fanout.deliveryRatio", "fanout.deliveryOpsPerSec"})
        null_key (snapshot, key, "NOT_APPLICABLE", "Delivery counts come from the runner's intersection of the publisher and subscriber sequence originals (§15.4).");
    for (const char *prefix : {"latency", "settle.latency"})
        for (const auto &suffix : latency_suffixes ())
            null_key (snapshot, std::string (prefix) + "." + suffix, "NOT_APPLICABLE", "A fanout cell has no echo round trip (§10.11).");
    for (const char *key : {"latencyMs", "settleLatencyMs"}) {
        snapshot["histograms"][key] = nullptr;
        snapshot["nullReasons"][std::string ("/histograms/") + key] = null_reason ("NOT_APPLICABLE", "A fanout cell has no echo round trip (§10.11).");
        snapshot["nullReasons"].erase (std::string ("/histograms/") + key + "/maxNs");
    }
    for (const char *key : {"messages.completed", "messages.settleCompleted", "throughput.kops"})
        null_key (snapshot, key, "NOT_APPLICABLE", "A fanout cell records publish admission, not echo completion (§10.11).");
    const char *code = has_delivery_owner ? "CLOCK_DOMAIN_UNVERIFIED" : "NOT_APPLICABLE";
    const std::string why = has_delivery_owner ? "Publisher and Subscriber use process-local monotonic clocks; no shared clock domain is verified (§15.2)."
                                               : "Delivery latency is observed by Subscriber processes.";
    for (const char *prefix : {"fanout.deliveryLatency", "fanout.settleDeliveryLatency"})
        for (const auto &suffix : latency_suffixes ())
            null_key (snapshot, std::string (prefix) + "." + suffix, code, why);
    for (const char *key : {"fanoutDeliveryLatencyMs", "fanoutSettleDeliveryLatencyMs"}) {
        snapshot["histograms"][key] = nullptr;
        snapshot["nullReasons"][std::string ("/histograms/") + key] = null_reason (code, has_delivery_owner ? "No verified shared clock domain between Publisher and Subscriber processes (§15.2)." : why);
    }
}
} // namespace fanout_metrics

// The original of this cell is written once and never replaced.
inline void write_once (const std::string &path, const json &original)
{
    if (std::filesystem::exists (path))
        return;
    std::ofstream file (path, std::ios::out | std::ios::trunc | std::ios::binary);
    file << original.dump () << '\n';
}

// role-configs/<role>.json sits one folder below the cell directory (perf §15.1), where the sequence original is written.
inline std::string cell_directory_of (const char *config_path)
{
    return std::filesystem::absolute (config_path).parent_path ().parent_path ().string ();
}
} // namespace perf
