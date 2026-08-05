/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../scenario_state.hpp"

#include <zlink/framework.hpp>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <stdexcept>
#include <thread>

namespace e2e = zlink::framework::e2e::spot_service;

namespace
{

class evidence_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<scenario_state_t>;

    explicit evidence_handler_t (scenario_state_t &state) : _state (state) {}

    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &)
    {
        zlink::framework::http_response_t response;
        response.body = nlohmann::json (_state.snapshot ()).dump ();
        return response;
    }

  private:
    scenario_state_t &_state;
};

class evidence_wait_handler_t
{
  public:
    using dependency_types = zlink::framework::dependency_list_t<scenario_state_t>;
    using request_type = e2e::evidence_wait_req_t;
    using reply_type = e2e::evidence_snapshot_t;

    explicit evidence_wait_handler_t (scenario_state_t &state) : _state (state) {}

    e2e::evidence_snapshot_t handle (const e2e::evidence_wait_req_t &request)
    {
        const auto timeout =
          std::chrono::milliseconds (std::clamp (request.timeout_milliseconds, 1, 30000));
        const auto deadline = std::chrono::steady_clock::now () + timeout;
        do {
            auto snapshot = _state.snapshot ();
            if (matches (snapshot, request)) {
                return snapshot;
            }
            std::this_thread::sleep_for (std::chrono::milliseconds (50));
        } while (std::chrono::steady_clock::now () < deadline);

        throw std::runtime_error ("timed out waiting for evidence");
    }

  private:
    static bool contains (const e2e::evidence_entry_t &entry, const std::string &expected)
    {
        return entry.marker.find (expected) != std::string::npos
               || entry.node_rid.find (expected) != std::string::npos
               || entry.actor_id.find (expected) != std::string::npos
               || entry.spot_id.find (expected) != std::string::npos
               || entry.value.find (expected) != std::string::npos;
    }

    static bool contains_any (const e2e::evidence_snapshot_t &snapshot,
                              const std::string &expected)
    {
        for (const auto &entry : snapshot.entries) {
            if (contains (entry, expected)) {
                return true;
            }
        }
        return false;
    }

    static bool matches (const e2e::evidence_snapshot_t &snapshot,
                         const e2e::evidence_wait_req_t &request)
    {
        for (const auto &expected : request.contains_all) {
            if (!contains_any (snapshot, expected)) {
                return false;
            }
        }
        return true;
    }

    scenario_state_t &_state;
};

class shutdown_handler_t
{
  public:
    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &)
    {
        std::thread ([] {
            std::this_thread::sleep_for (std::chrono::milliseconds (20));
            std::raise (SIGTERM);
        }).detach ();

        zlink::framework::http_response_t response;
        response.body = R"({"status":"stopping"})";
        return response;
    }
};

class crash_handler_t
{
  public:
    zlink::framework::http_response_t handle (const zlink::framework::http_request_t &)
    {
        std::thread ([] {
            std::this_thread::sleep_for (std::chrono::milliseconds (20));
            std::_Exit (137);
        }).detach ();

        zlink::framework::http_response_t response;
        response.status = 202;
        response.body = R"({"status":"accepted"})";
        return response;
    }
};

} // namespace
