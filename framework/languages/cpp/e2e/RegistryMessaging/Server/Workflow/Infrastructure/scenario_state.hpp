/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../../Shared/registry_messaging_contracts.hpp"

#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace zlink::framework::e2e::registry_messaging::workflow
{

class scenario_state_t
{
  public:
    scenario_state_t (std::string provider_rid, std::string instance_id) :
        provider_rid (std::move (provider_rid)), instance_id (std::move (instance_id))
    {
    }

    void record (std::string marker, std::string value)
    {
        std::lock_guard lock (_mutex);
        entries.push_back ({std::move (marker), provider_rid, std::move (value)});
    }

    evidence_snapshot_t snapshot () const
    {
        std::lock_guard lock (_mutex);
        return {.provider_rid = provider_rid, .entries = entries};
    }

    std::string provider_rid;
    std::string instance_id;

  private:
    mutable std::mutex _mutex;
    std::vector<evidence_entry_t> entries;
};

} // namespace zlink::framework::e2e::registry_messaging::workflow
