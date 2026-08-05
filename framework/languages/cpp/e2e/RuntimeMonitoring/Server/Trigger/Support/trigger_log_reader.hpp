/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#pragma once

#include "../../../Shared/runtime_monitoring_contracts.hpp"

#include <chrono>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace zlink::framework::e2e::runtime_monitoring::trigger
{

inline std::vector<std::string> read_log_lines (const std::string &path)
{
    std::ifstream file (path);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline (file, line)) {
        lines.push_back (line);
    }
    return lines;
}

inline bool contains (const std::string &value, const std::string &needle)
{
    return value.find (needle) != std::string::npos;
}

inline bool any_line_contains (const std::vector<std::string> &lines, const std::string &needle)
{
    for (const auto &line : lines) {
        if (contains (line, needle)) {
            return true;
        }
    }
    return false;
}

inline bool matches_wait_request (const std::vector<std::string> &lines,
                                  const evidence_wait_req_t &request)
{
    for (const auto &required : request.contains_all) {
        if (!any_line_contains (lines, required)) {
            return false;
        }
    }
    for (const auto &group : request.contains_any_groups) {
        bool matched = group.empty ();
        for (const auto &candidate : group) {
            if (any_line_contains (lines, candidate)) {
                matched = true;
                break;
            }
        }
        if (!matched) {
            return false;
        }
    }
    return true;
}

inline std::vector<std::string> wait_for_log_lines (const std::string &path,
                                                    const evidence_wait_req_t &request)
{
    const auto timeout = std::chrono::milliseconds (
      request.timeout_milliseconds > 0 ? request.timeout_milliseconds : 1);
    const auto deadline = std::chrono::steady_clock::now () + timeout;
    do {
        auto lines = read_log_lines (path);
        if (matches_wait_request (lines, request)) {
            return lines;
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (100));
    } while (std::chrono::steady_clock::now () < deadline);
    throw std::runtime_error ("timed out waiting for log lines");
}

} // namespace zlink::framework::e2e::runtime_monitoring::trigger
