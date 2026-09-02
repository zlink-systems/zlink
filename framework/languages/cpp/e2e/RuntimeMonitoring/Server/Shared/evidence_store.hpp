/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#pragma once

#include <nlohmann/json.hpp>

#include <zlink/framework.hpp>

#include "../../Shared/runtime_monitoring_contracts.hpp"

#include <chrono>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace zlink::framework::e2e::runtime_monitoring::server
{

class evidence_store_t
{
  public:
    evidence_store_t (std::string rid, std::string file_path = {}) :
        _rid (std::move (rid)), _file_path (std::move (file_path))
    {
        if (!_file_path.empty ()) {
            std::ofstream file (_file_path, std::ios::trunc);
        }
    }

    const std::string &rid () const noexcept { return _rid; }

    void add (std::string entry)
    {
        std::lock_guard lock (_mutex);
        _entries.push_back (entry);
        if (!_file_path.empty ()) {
            std::ofstream file (_file_path, std::ios::app);
            file << entry << '\n';
        }
    }

    std::vector<std::string> snapshot () const
    {
        std::lock_guard lock (_mutex);
        return _entries;
    }

  private:
    std::string _rid;
    std::string _file_path;
    mutable std::mutex _mutex;
    std::vector<std::string> _entries;
};

class evidence_handler_t
{
  public:

    explicit evidence_handler_t (evidence_store_t &evidence) : _evidence (evidence) {}

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &)
    {
        zlink::framework::http_response_t response;
        response.body = nlohmann::json (_evidence.snapshot ()).dump ();
        return response;
    }

  private:
    evidence_store_t &_evidence;
};

class evidence_wait_handler_t
{
  public:
    using request_type = evidence_wait_req_t;
    using reply_type = std::vector<std::string>;

    explicit evidence_wait_handler_t (evidence_store_t &evidence) : _evidence (evidence) {}

    std::vector<std::string> handle (const evidence_wait_req_t &request)
    {
        const auto timeout = std::chrono::milliseconds (
          request.timeout_milliseconds > 0 ? request.timeout_milliseconds : 1);
        const auto deadline = std::chrono::steady_clock::now () + timeout;
        do {
            auto entries = _evidence.snapshot ();
            if (matches (entries, request)) {
                return entries;
            }
            std::this_thread::sleep_for (std::chrono::milliseconds (50));
        } while (std::chrono::steady_clock::now () < deadline);

        throw std::runtime_error ("timed out waiting for evidence");
    }

  private:
    static bool contains (const std::string &value, const std::string &needle)
    {
        return value.find (needle) != std::string::npos;
    }

    static bool any_entry_contains (const std::vector<std::string> &entries,
                                    const std::string &needle)
    {
        for (const auto &entry : entries) {
            if (contains (entry, needle)) {
                return true;
            }
        }
        return false;
    }

    static bool matches (const std::vector<std::string> &entries,
                         const evidence_wait_req_t &request)
    {
        for (const auto &required : request.contains_all) {
            if (!any_entry_contains (entries, required)) {
                return false;
            }
        }
        for (const auto &group : request.contains_any_groups) {
            bool matched = group.empty ();
            for (const auto &candidate : group) {
                if (any_entry_contains (entries, candidate)) {
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

    evidence_store_t &_evidence;
};

} // namespace zlink::framework::e2e::runtime_monitoring::server
