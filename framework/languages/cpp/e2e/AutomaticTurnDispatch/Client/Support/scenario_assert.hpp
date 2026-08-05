/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace zlink::framework::e2e::automatic_turn_dispatch::client
{

inline void ensure (bool condition, const std::string &message)
{
    if (!condition) {
        throw std::runtime_error (message);
    }
}

template <typename TResult>
void ensure_result (const TResult &result, const std::string &message)
{
    if (static_cast<bool> (result)) {
        return;
    }
    if (const auto &error = result.error ()) {
        std::cerr << message << ": " << error->message << "\n";
    }
    throw std::runtime_error (message);
}

inline bool contains_in_order (const std::vector<std::string> &evidence,
                               const std::string &request_id,
                               const std::vector<std::string> &markers)
{
    std::size_t cursor = 0;
    for (const auto &marker : markers) {
        bool found = false;
        for (; cursor < evidence.size (); ++cursor) {
            const auto &line = evidence[cursor];
            if (line.find ("request=" + request_id) != std::string::npos
                && line.find (marker) != std::string::npos) {
                ++cursor;
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

inline void ensure_contains_in_order (const std::vector<std::string> &evidence,
                                      const std::string &request_id,
                                      const std::vector<std::string> &markers,
                                      const std::string &message)
{
    if (contains_in_order (evidence, request_id, markers)) {
        return;
    }
    std::cerr << message << "\n";
    for (const auto &line : evidence) {
        if (line.find ("request=" + request_id) != std::string::npos) {
            std::cerr << line << "\n";
        }
    }
    throw std::runtime_error (message);
}

inline bool every_request_line_has (const std::vector<std::string> &evidence,
                                    const std::string &request_id,
                                    const std::vector<std::string> &fragments)
{
    bool saw_request = false;
    for (const auto &line : evidence) {
        if (line.find ("request=" + request_id) == std::string::npos) {
            continue;
        }
        saw_request = true;
        for (const auto &fragment : fragments) {
            if (line.find (fragment) == std::string::npos) {
                return false;
            }
        }
    }
    return saw_request;
}

inline bool contains_request_marker (const std::vector<std::string> &evidence,
                                     const std::string &request_id,
                                     const std::string &marker)
{
    for (const auto &line : evidence) {
        if (line.find ("request=" + request_id) != std::string::npos
            && line.find (marker) != std::string::npos) {
            return true;
        }
    }
    return false;
}

inline std::string unique_id (const std::string &prefix)
{
    return prefix + "-" + std::to_string (
                        std::chrono::steady_clock::now ().time_since_epoch ().count ());
}

} // namespace zlink::framework::e2e::automatic_turn_dispatch::client
