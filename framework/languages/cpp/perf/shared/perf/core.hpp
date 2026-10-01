/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

// Perf spec §12, §15.2, §15.3: the application DTOs, the decimal-string number forms, the monotonic clock, the payload
// pattern and the histogram. These are measuring devices only; none of them changes which public call a scenario makes.

#include <zlink/framework.hpp>

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdlib>
#include <cxxabi.h>
#include <fstream>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <time.h>
#include <unistd.h>
#include <vector>

// nlohmann::json has no std::optional support of its own: an empty optional is JSON null (§15.2 nullable fields).
namespace nlohmann
{
template <typename T> struct adl_serializer<std::optional<T>>
{
    static void to_json (json &out, const std::optional<T> &value)
    {
        if (value)
            out = *value;
        else
            out = nullptr;
    }
    static void from_json (const json &in, std::optional<T> &value)
    {
        if (in.is_null ())
            value.reset ();
        else
            value = in.get<T> ();
    }
};
} // namespace nlohmann

namespace perf
{
using json = nlohmann::json;

// ---- errors of the harness itself (§14.2 harness namespace) ----
class validation_error_t : public std::runtime_error
{
  public:
    validation_error_t (std::string kind, const std::string &message) :
        std::runtime_error (message), _kind (std::move (kind))
    {
    }
    const std::string &kind () const noexcept { return _kind; }

  private:
    std::string _kind;
};

// ---- decimal strings (§15.2 U64 / I64: canonical, no sign on U64, no leading zero) ----
inline std::string dec (std::uint64_t value) { return std::to_string (value); }
inline std::string dec (std::int64_t value) { return std::to_string (value); }
inline std::string dec (unsigned __int128 value)
{
    if (value == 0)
        return "0";
    std::string text;
    while (value != 0) {
        text.insert (text.begin (), static_cast<char> ('0' + static_cast<int> (value % 10)));
        value /= 10;
    }
    return text;
}
inline std::uint64_t parse_u64 (const std::string &text)
{
    if (text.empty () || text.size () > 20 || (text.size () > 1 && text[0] == '0'))
        throw validation_error_t ("SchemaMismatch", "Noncanonical U64 decimal string.");
    unsigned __int128 value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9')
            throw validation_error_t ("SchemaMismatch", "Noncanonical U64 decimal string.");
        value = value * 10 + static_cast<unsigned> (c - '0');
    }
    if (value > UINT64_MAX)
        throw validation_error_t ("SchemaMismatch", "U64 overflow.");
    return static_cast<std::uint64_t> (value);
}
inline std::int64_t parse_i64 (const std::string &text)
{
    const bool negative = !text.empty () && text[0] == '-';
    const auto digits = negative ? text.substr (1) : text;
    const auto magnitude = parse_u64 (digits);
    if (negative ? magnitude > static_cast<std::uint64_t> (INT64_MAX) + 1 || magnitude == 0
                 : magnitude > static_cast<std::uint64_t> (INT64_MAX))
        throw validation_error_t ("SchemaMismatch", "Noncanonical I64 decimal string.");
    return negative ? static_cast<std::int64_t> (0 - magnitude) : static_cast<std::int64_t> (magnitude);
}

// ---- clock (§15.2: nanosecond ticks from the monotonic clock; Unix time is for display only) ----
inline std::int64_t now_ticks ()
{
    timespec spec{};
    clock_gettime (CLOCK_MONOTONIC, &spec);
    return static_cast<std::int64_t> (spec.tv_sec) * 1'000'000'000 + spec.tv_nsec;
}
inline std::string unix_ms ()
{
    timespec spec{};
    clock_gettime (CLOCK_REALTIME, &spec);
    return dec (static_cast<std::int64_t> (spec.tv_sec) * 1000 + spec.tv_nsec / 1'000'000);
}
inline const std::string &clock_domain ()
{
    static const std::string domain = [] {
        std::random_device device;
        std::ostringstream stream;
        stream << "process-" << getpid () << "-" << std::hex << device () << device ();
        return stream.str ();
    }();
    return domain;
}
inline json clock_metadata ()
{
    return {{"source", "clock_gettime(CLOCK_MONOTONIC)"},
            {"nativeFrequencyHz", "1000000000"},
            {"ticksUnit", "ns"},
            {"clockDomainId", clock_domain ()},
            {"scope", "process"},
            {"alignmentMethod", nullptr},
            {"maxErrorNs", nullptr},
            {"validFromTicks", nullptr},
            {"validThroughTicks", nullptr},
            {"evidence", json::array ({"RTT uses only this process clock; remote receivedTicks is diagnostic."})}};
}

inline std::string type_name (const std::type_info &info)
{
    int status = 0;
    char *demangled = abi::__cxa_demangle (info.name (), nullptr, nullptr, &status);
    std::string name = status == 0 && demangled ? demangled : info.name ();
    std::free (demangled);
    return name;
}

// The public Framework error kinds by name (§14.2). C++ declares kinds 0..11; the name table is the mapping.
inline const char *kind_name (zlink::framework::framework_error_kind_t kind)
{
    using k = zlink::framework::framework_error_kind_t;
    switch (kind) {
        case k::not_found: return "NotFound";
        case k::already_exists: return "AlreadyExists";
        case k::type_mismatch: return "TypeMismatch";
        case k::not_configured: return "NotConfigured";
        case k::rejected: return "Rejected";
        case k::unavailable: return "Unavailable";
        case k::deadline_exceeded: return "DeadlineExceeded";
        case k::shutting_down: return "ShuttingDown";
        case k::protocol_error: return "ProtocolError";
        case k::invalid_operation: return "InvalidOperation";
        case k::data_lost: return "DataLost";
        case k::internal_failure: return "InternalFailure";
    }
    return "InternalFailure";
}

// ---- §12 / §15.2 application DTOs. Field names on the wire are the lowerCamelCase of §15.2. ----
struct identity_t
{
    std::string run_id, cell_id, reset_seq, phase;
};
inline void identity_to_json (json &out, const identity_t &v)
{
    out["runId"] = v.run_id;
    out["cellId"] = v.cell_id;
    out["resetSeq"] = v.reset_seq;
    out["phase"] = v.phase;
}
inline void identity_from_json (const json &in, identity_t &v)
{
    v.run_id = in.at ("runId").get<std::string> ();
    v.cell_id = in.at ("cellId").get<std::string> ();
    v.reset_seq = in.at ("resetSeq").get<std::string> ();
    v.phase = in.at ("phase").get<std::string> ();
}

struct echo_request_t : identity_t
{
    static constexpr const char *packet_name = "PerfEchoRequest";
    int client_id = 0;
    std::string sequence, correlation_id, sent_ticks, clock_domain_id;
    std::optional<std::string> return_spot_id, return_channel;
    std::string payload;
};
inline void to_json (json &out, const echo_request_t &v)
{
    identity_to_json (out, v);
    out["clientId"] = v.client_id;
    out["sequence"] = v.sequence;
    out["correlationId"] = v.correlation_id;
    out["sentTicks"] = v.sent_ticks;
    out["clockDomainId"] = v.clock_domain_id;
    out["returnSpotId"] = v.return_spot_id;
    out["returnChannel"] = v.return_channel;
    out["payload"] = v.payload;
}
inline void from_json (const json &in, echo_request_t &v)
{
    identity_from_json (in, v);
    v.client_id = in.at ("clientId").get<int> ();
    v.sequence = in.at ("sequence").get<std::string> ();
    v.correlation_id = in.at ("correlationId").get<std::string> ();
    v.sent_ticks = in.at ("sentTicks").get<std::string> ();
    v.clock_domain_id = in.at ("clockDomainId").get<std::string> ();
    v.return_spot_id = in.at ("returnSpotId").get<std::optional<std::string>> ();
    v.return_channel = in.at ("returnChannel").get<std::optional<std::string>> ();
    v.payload = in.at ("payload").get<std::string> ();
}

struct echo_reply_t : identity_t
{
    static constexpr const char *packet_name = "PerfEchoReply";
    int client_id = 0;
    std::string sequence, correlation_id, received_ticks, clock_domain_id, payload;
};
inline void to_json (json &out, const echo_reply_t &v)
{
    identity_to_json (out, v);
    out["clientId"] = v.client_id;
    out["sequence"] = v.sequence;
    out["correlationId"] = v.correlation_id;
    out["receivedTicks"] = v.received_ticks;
    out["clockDomainId"] = v.clock_domain_id;
    out["payload"] = v.payload;
}
inline void from_json (const json &in, echo_reply_t &v)
{
    identity_from_json (in, v);
    v.client_id = in.at ("clientId").get<int> ();
    v.sequence = in.at ("sequence").get<std::string> ();
    v.correlation_id = in.at ("correlationId").get<std::string> ();
    v.received_ticks = in.at ("receivedTicks").get<std::string> ();
    v.clock_domain_id = in.at ("clockDomainId").get<std::string> ();
    v.payload = in.at ("payload").get<std::string> ();
}

struct drive_request_t
{
    static constexpr const char *packet_name = "PerfDriveRequest";
    echo_request_t echo;
};
inline void to_json (json &out, const drive_request_t &v) { out = {{"echo", v.echo}}; }
inline void from_json (const json &in, drive_request_t &v) { v.echo = in.at ("echo").get<echo_request_t> (); }

struct drive_reply_t
{
    static constexpr const char *packet_name = "PerfDriveReply";
    bool started = false;
    std::optional<echo_reply_t> echo;
};
inline void to_json (json &out, const drive_reply_t &v)
{
    out = {{"started", v.started}, {"echo", v.echo}};
}
inline void from_json (const json &in, drive_reply_t &v)
{
    v.started = in.at ("started").get<bool> ();
    v.echo = in.at ("echo").get<std::optional<echo_reply_t>> ();
}

struct trigger_request_t : identity_t
{
};
inline void from_json (const json &in, trigger_request_t &v) { identity_from_json (in, v); }
inline void to_json (json &out, const trigger_request_t &v) { identity_to_json (out, v); }

struct trigger_reply_t : identity_t
{
    bool accepted = false;
    std::string state, config_hash;
    std::optional<std::string> reason;
};
inline void to_json (json &out, const trigger_reply_t &v)
{
    identity_to_json (out, v);
    out["accepted"] = v.accepted;
    out["state"] = v.state;
    out["configHash"] = v.config_hash;
    out["reason"] = v.reason;
}
inline void from_json (const json &in, trigger_reply_t &v)
{
    identity_from_json (in, v);
    v.accepted = in.at ("accepted").get<bool> ();
    v.state = in.at ("state").get<std::string> ();
    v.config_hash = in.at ("configHash").get<std::string> ();
    v.reason = in.at ("reason").get<std::optional<std::string>> ();
}

struct publish_event_t : identity_t
{
    static constexpr const char *packet_name = "PerfPublishEvent";
    std::string sequence, topic, sent_ticks, clock_domain_id, payload;
};
inline void to_json (json &out, const publish_event_t &v)
{
    identity_to_json (out, v);
    out["sequence"] = v.sequence;
    out["topic"] = v.topic;
    out["sentTicks"] = v.sent_ticks;
    out["clockDomainId"] = v.clock_domain_id;
    out["payload"] = v.payload;
}
inline void from_json (const json &in, publish_event_t &v)
{
    identity_from_json (in, v);
    v.sequence = in.at ("sequence").get<std::string> ();
    v.topic = in.at ("topic").get<std::string> ();
    v.sent_ticks = in.at ("sentTicks").get<std::string> ();
    v.clock_domain_id = in.at ("clockDomainId").get<std::string> ();
    v.payload = in.at ("payload").get<std::string> ();
}

struct worker_observation_t
{
    static constexpr const char *packet_name = "WorkerObservation";
    std::string started_ticks, ended_ticks, clock_domain_id, iterations;
    std::uint32_t checksum = 0;
};
inline void to_json (json &out, const worker_observation_t &v)
{
    out = {{"startedTicks", v.started_ticks},
           {"endedTicks", v.ended_ticks},
           {"clockDomainId", v.clock_domain_id},
           {"iterations", v.iterations},
           {"checksum", v.checksum}};
}
inline void from_json (const json &in, worker_observation_t &v)
{
    v.started_ticks = in.at ("startedTicks").get<std::string> ();
    v.ended_ticks = in.at ("endedTicks").get<std::string> ();
    v.clock_domain_id = in.at ("clockDomainId").get<std::string> ();
    v.iterations = in.at ("iterations").get<std::string> ();
    v.checksum = in.at ("checksum").get<std::uint32_t> ();
}

struct reset_request_t
{
    std::string run_id, cell_id, reset_seq;
};
inline void from_json (const json &in, reset_request_t &v)
{
    v.run_id = in.at ("runId").get<std::string> ();
    v.cell_id = in.at ("cellId").get<std::string> ();
    v.reset_seq = in.at ("resetSeq").get<std::string> ();
}
inline void to_json (json &out, const reset_request_t &v)
{
    out = {{"runId", v.run_id}, {"cellId", v.cell_id}, {"resetSeq", v.reset_seq}};
}

inline json null_reason (const std::string &code, const std::string &reason,
                         const std::string &owner = "perf/README.ko.md", json lower_bound_ms = nullptr)
{
    return {{"code", code}, {"reason", reason}, {"owner", owner}, {"lowerBoundMs", std::move (lower_bound_ms)}};
}

// ---- §15.2 payload pattern: b[i] = (31*i + 17*floor(i/251) + 29) mod 256, carried as canonical padded Base64 ----
class payload_pattern_t
{
  public:
    explicit payload_pattern_t (int size) : _base64 (generate (size)) {}
    const std::string &base64 () const noexcept { return _base64; }
    // The canonical Base64 string is the whole contract: equality with it proves length and every logical byte.
    void validate (const std::string &payload) const
    {
        if (payload != _base64)
            throw validation_error_t ("PayloadMismatch", "Payload is not the canonical Base64 pattern.");
    }
    static void validate_identity (const echo_request_t &request, const echo_reply_t &reply)
    {
        if (request.run_id != reply.run_id || request.cell_id != reply.cell_id || request.reset_seq != reply.reset_seq
            || request.phase != reply.phase || request.client_id != reply.client_id
            || request.sequence != reply.sequence || request.correlation_id != reply.correlation_id
            || reply.clock_domain_id.empty ())
            throw validation_error_t ("IdentityMismatch", "Echo identity differs from the submitted operation.");
        (void) parse_i64 (reply.received_ticks);
    }
    static echo_reply_t reply (const echo_request_t &request, std::int64_t received_ticks)
    {
        echo_reply_t out;
        out.run_id = request.run_id;
        out.cell_id = request.cell_id;
        out.reset_seq = request.reset_seq;
        out.phase = request.phase;
        out.client_id = request.client_id;
        out.sequence = request.sequence;
        out.correlation_id = request.correlation_id;
        out.received_ticks = dec (received_ticks);
        out.clock_domain_id = clock_domain ();
        out.payload = request.payload;
        return out;
    }

  private:
    static std::string generate (int length)
    {
        static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::vector<unsigned char> bytes (static_cast<std::size_t> (length));
        for (int i = 0; i < length; ++i)
            bytes[static_cast<std::size_t> (i)] = static_cast<unsigned char> ((31 * i + 17 * (i / 251) + 29) % 256);
        std::string out;
        for (std::size_t i = 0; i < bytes.size (); i += 3) {
            const std::size_t left = bytes.size () - i;
            const unsigned value = (bytes[i] << 16) | (left > 1 ? bytes[i + 1] << 8 : 0) | (left > 2 ? bytes[i + 2] : 0);
            out += alphabet[(value >> 18) & 63];
            out += alphabet[(value >> 12) & 63];
            out += left > 1 ? alphabet[(value >> 6) & 63] : '=';
            out += left > 2 ? alphabet[value & 63] : '=';
        }
        return out;
    }
    std::string _base64;
};

// ---- §15.3 histogram: bounds come from the one shared schema file the runner also reads ----
inline const std::vector<double> &histogram_bounds ()
{
    static const std::vector<double> bounds = [] {
        std::ifstream file (PERF_HISTOGRAM_BOUNDS_FILE);
        if (!file)
            throw std::runtime_error (std::string ("Cannot read ") + PERF_HISTOGRAM_BOUNDS_FILE);
        return json::parse (file).get<std::vector<double>> ();
    }();
    return bounds;
}

class histogram_t
{
  public:
    histogram_t () : _counts (histogram_bounds ().size (), 0)
    {
        for (const double bound : histogram_bounds ())
            _bounds_ns.push_back (std::llround (bound * 1'000'000));
    }
    void record (std::int64_t elapsed_ns)
    {
        if (elapsed_ns < 0)
            throw std::out_of_range ("elapsed ticks are negative");
        ++_count;
        std::size_t bucket = 0;
        while (bucket < _bounds_ns.size () && elapsed_ns > _bounds_ns[bucket])
            ++bucket;
        if (bucket == _bounds_ns.size ())
            ++_overflow;
        else
            ++_counts[bucket];
        _sum += static_cast<unsigned __int128> (elapsed_ns);
        _max = std::max (_max, static_cast<std::uint64_t> (elapsed_ns));
    }
    std::uint64_t count () const noexcept { return _count; }
    json snapshot () const
    {
        json counts = json::array ();
        for (const auto value : _counts)
            counts.push_back (dec (value));
        return {{"unit", "ms"},
                {"ticksUnit", "ns"},
                {"bounds", histogram_bounds ()},
                {"counts", counts},
                {"overflow", dec (_overflow)},
                {"count", dec (_count)},
                {"sumNs", dec (_sum)},
                {"maxNs", _count == 0 ? json (nullptr) : json (dec (_max))},
                {"percentileMethod", "nearest-rank-bucket-upper-bound-capped-by-max"}};
    }
    // Writes the histogram and its dotted latency keys; a null carries its reason (§15.5).
    void export_to (const std::string &histogram_key, const std::string &prefix, json &metrics, json &histograms,
                    json &reasons) const
    {
        histograms[histogram_key] = snapshot ();
        for (const char *name : {"meanMs", "p50Ms", "p95Ms", "p99Ms", "maxMs"}) {
            const std::string key = prefix + "." + name;
            const std::string suffix = name;
            json value = nullptr;
            if (_count != 0) {
                if (suffix == "meanMs")
                    value = static_cast<double> (_sum) / static_cast<double> (_count) / 1'000'000;
                else if (suffix == "maxMs")
                    value = static_cast<double> (_max) / 1'000'000.0;
                else
                    value = percentile (std::stoi (suffix.substr (1, 2)));
            }
            metrics[key] = value;
            if (value.is_null ())
                reasons["/metrics/" + key] =
                  _count == 0 ? null_reason ("NO_SAMPLES", "No successful samples in this cohort and window.")
                              : null_reason ("HISTOGRAM_OVERFLOW", "Nearest rank lies above the final bucket.",
                                             "perf/README.ko.md §15.3", histogram_bounds ().back ());
        }
        if (_count == 0)
            reasons["/histograms/" + histogram_key + "/maxNs"] =
              null_reason ("NO_SAMPLES", "No successful samples in this cohort and window.");
    }

  private:
    json percentile (int percent) const
    {
        const unsigned __int128 rank = (static_cast<unsigned __int128> (percent) * _count + 99) / 100;
        unsigned __int128 cumulative = 0;
        for (std::size_t i = 0; i < _counts.size (); ++i) {
            cumulative += _counts[i];
            if (cumulative >= rank)
                return std::min (histogram_bounds ()[i], static_cast<double> (_max) / 1'000'000.0);
        }
        return nullptr;
    }
    std::vector<std::uint64_t> _counts;
    std::vector<std::int64_t> _bounds_ns;
    std::uint64_t _count = 0, _overflow = 0, _max = 0;
    unsigned __int128 _sum = 0;
};

} // namespace perf
