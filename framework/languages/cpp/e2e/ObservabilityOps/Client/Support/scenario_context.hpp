/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace zlink::framework::e2e::observability_ops::client
{

struct verification_input_t
{
    std::string scenario_id;
    std::map<std::string, std::string> files;
};

inline void require (bool condition, const std::string &message)
{
    if (!condition) {
        throw std::runtime_error (message);
    }
}

inline const std::string &required_file (const verification_input_t &input, const std::string &name)
{
    const auto found = input.files.find (name);
    require (found != input.files.end () && !found->second.empty (),
             input.scenario_id + " requires evidence file " + name);
    return found->second;
}

inline nlohmann::json read_json (const verification_input_t &input, const std::string &name)
{
    std::ifstream stream (required_file (input, name));
    require (static_cast<bool> (stream), input.scenario_id + " cannot open evidence file " + name);
    return nlohmann::json::parse (stream);
}

inline std::vector<std::string> read_lines (const verification_input_t &input,
                                            const std::string &name)
{
    std::ifstream stream (required_file (input, name));
    require (static_cast<bool> (stream), input.scenario_id + " cannot open log file " + name);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline (stream, line)) {
        lines.push_back (std::move (line));
    }
    return lines;
}

inline std::vector<std::string> read_optional_lines (const verification_input_t &input,
                                                     const std::string &name)
{
    std::ifstream stream (required_file (input, name));
    std::vector<std::string> lines;
    std::string line;
    while (std::getline (stream, line)) {
        lines.push_back (std::move (line));
    }
    return lines;
}

inline std::set<std::string> flow_ids (const std::vector<std::string> &lines,
                                       const std::string &required = {})
{
    static const std::regex pattern (R"(flow=([0-9a-f-]{36}))");
    std::set<std::string> ids;
    for (const auto &line : lines) {
        if (!required.empty () && line.find (required) == std::string::npos) {
            continue;
        }
        std::smatch match;
        if (std::regex_search (line, match, pattern)) {
            ids.insert (match[1].str ());
        }
    }
    return ids;
}

inline std::string require_shared_flow (const std::vector<std::string> &lines,
                                        const std::vector<std::string> &markers,
                                        const std::string &message)
{
    require (!markers.empty (), message);
    auto candidates = flow_ids (lines, markers.front ());
    for (auto marker = std::next (markers.begin ()); marker != markers.end (); ++marker) {
        const auto matching = flow_ids (lines, *marker);
        std::set<std::string> shared;
        std::set_intersection (candidates.begin (), candidates.end (), matching.begin (),
                               matching.end (), std::inserter (shared, shared.end ()));
        candidates = std::move (shared);
    }
    require (!candidates.empty (), message);
    return *candidates.begin ();
}

inline std::string require_flow_sequence (
  const std::vector<std::vector<std::string>> &ordered_logs,
  const std::vector<std::string> &markers,
  const std::string &message)
{
    require (ordered_logs.size () == markers.size (), message);
    std::optional<std::string> expected;
    std::optional<std::size_t> previous_position;
    for (std::size_t index = 0; index < markers.size (); ++index) {
        const auto ids = flow_ids (ordered_logs[index], markers[index]);
        require (!ids.empty (), message);
        if (!expected) {
            expected = *ids.begin ();
        } else {
            require (ids.contains (*expected), message);
        }
        const auto matching = std::find_if (
          ordered_logs[index].begin (), ordered_logs[index].end (),
          [&] (const auto &line) {
              return line.find (markers[index]) != std::string::npos
                     && line.find ("flow=" + *expected) != std::string::npos;
          });
        require (matching != ordered_logs[index].end (), message);
        const auto position = static_cast<std::size_t> (
          std::distance (ordered_logs[index].begin (), matching));
        if (index > 0 && ordered_logs[index] == ordered_logs[index - 1]) {
            require (previous_position && position > *previous_position, message);
        }
        previous_position = position;
    }
    return *expected;
}

inline std::string require_same_flow (const std::vector<std::string> &upstream,
                                      const std::string &upstream_marker,
                                      const std::vector<std::string> &downstream,
                                      const std::string &downstream_marker,
                                      const std::string &message)
{
    return require_flow_sequence ({upstream, downstream},
                                  {upstream_marker, downstream_marker}, message);
}

inline std::vector<std::vector<std::string>> read_line_groups (
  const verification_input_t &input,
  const std::string &name)
{
    std::vector<std::vector<std::string>> groups;
    std::stringstream paths (required_file (input, name));
    std::string path;
    while (std::getline (paths, path, ';')) {
        std::ifstream stream (path);
        require (static_cast<bool> (stream), input.scenario_id + " cannot open log file " + path);
        std::vector<std::string> lines;
        std::string line;
        while (std::getline (stream, line)) {
            lines.push_back (std::move (line));
        }
        groups.push_back (std::move (lines));
    }
    require (!groups.empty (), input.scenario_id + " requires subscriber logs");
    return groups;
}

inline std::string require_fanout_flow (
  const std::vector<std::string> &publisher,
  const std::vector<std::vector<std::string>> &subscribers,
  const std::string &message)
{
    const auto published = flow_ids (publisher, "phase=sent surface=spot_subscription");
    require (!published.empty (), message);
    for (const auto &candidate : published) {
        const auto all_received = std::all_of (
          subscribers.begin (), subscribers.end (), [&candidate] (const auto &lines) {
              return flow_ids (lines, "phase=received surface=spot_subscription")
                .contains (candidate);
          });
        if (all_received) {
            return candidate;
        }
    }
    throw std::runtime_error (message);
}

inline bool has_line (const std::vector<std::string> &lines, const std::string &value)
{
    return std::any_of (lines.begin (), lines.end (),
                        [&] (const auto &line) { return line.find (value) != std::string::npos; });
}

inline std::vector<nlohmann::json> metrics_named (const nlohmann::json &body,
                                                  const std::string &name)
{
    std::vector<nlohmann::json> result;
    for (const auto &metric : body.at ("metrics")) {
        if (metric.at ("name").get<std::string> () == name) {
            result.push_back (metric);
        }
    }
    return result;
}

inline double metric_total (const nlohmann::json &body, const std::string &name)
{
    double total = 0;
    for (const auto &metric : metrics_named (body, name)) {
        total += metric.at ("value").get<double> ();
    }
    return total;
}

inline const std::set<std::string> &forbidden_metric_labels ()
{
    static const std::set<std::string> labels{
      "correlation_id", "flow_id", "actor_id", "spot_id"};
    return labels;
}

inline void require_bounded_metric_labels (const nlohmann::json &body,
                                           const std::string &message)
{
    for (const auto &metric : body.at ("metrics")) {
        for (const auto &label : forbidden_metric_labels ()) {
            require (!metric.at ("tags").contains (label), message);
        }
    }
}

inline bool has_drain_state (const nlohmann::json &body, const std::string &state)
{
    return std::any_of (body.at ("drainEvents").begin (), body.at ("drainEvents").end (),
                        [&] (const nlohmann::json &event) {
                            return event.at ("state").get<std::string> () == state;
                        });
}

int run_scenario_verification (const verification_input_t &input);

} // namespace zlink::framework::e2e::observability_ops::client
