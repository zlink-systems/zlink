/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Shared/spot_service_contracts.hpp"

#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

class scenario_state_t
{
  public:
    explicit scenario_state_t (std::string node_rid) : node_rid (std::move (node_rid)) {}

    void record (std::string marker,
                 std::string actor_id = {},
                 std::string spot_id = {},
                 std::string value = {})
    {
        std::lock_guard lock (_mutex);
        entries.push_back ({std::move (marker), node_rid, std::move (actor_id),
                            std::move (spot_id), std::move (value)});
    }

    zlink::framework::e2e::spot_service::evidence_snapshot_t snapshot () const
    {
        std::lock_guard lock (_mutex);
        return {.node_rid = node_rid, .entries = entries};
    }

    zlink::framework::e2e::spot_service::evidence_snapshot_t wait_until (
      const std::function<bool (
        const zlink::framework::e2e::spot_service::evidence_snapshot_t &)> &matches,
      std::chrono::milliseconds timeout) const
    {
        const auto deadline = std::chrono::steady_clock::now () + timeout;
        do {
            auto current = snapshot ();
            if (matches (current)) {
                return current;
            }
            std::this_thread::sleep_for (std::chrono::milliseconds (50));
        } while (std::chrono::steady_clock::now () < deadline);
        return snapshot ();
    }

    std::string node_rid;

  private:
    mutable std::mutex _mutex;
    std::vector<zlink::framework::e2e::spot_service::evidence_entry_t> entries;
};
