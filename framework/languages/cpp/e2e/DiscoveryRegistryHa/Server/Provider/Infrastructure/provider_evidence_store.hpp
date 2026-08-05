/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../../Shared/store_failure_contracts.hpp"

#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace zlink::framework::e2e::store_failure::provider
{

class provider_evidence_store_t
{
  public:
    explicit provider_evidence_store_t (std::string provider_rid) :
        _provider_rid (std::move (provider_rid))
    {
    }

    void record (const std::string &marker, std::string value)
    {
        std::lock_guard lock (_mutex);
        _entries.push_back ({marker, _provider_rid, std::move (value)});
    }

    evidence_snapshot_t snapshot () const
    {
        std::lock_guard lock (_mutex);
        return {.provider_rid = _provider_rid, .entries = _entries};
    }

    bool contains (const std::string &needle) const
    {
        std::lock_guard lock (_mutex);
        for (const auto &entry : _entries) {
            if (entry.marker.find (needle) != std::string::npos
                || entry.value.find (needle) != std::string::npos) {
                return true;
            }
        }
        return false;
    }

  private:
    std::string _provider_rid;
    mutable std::mutex _mutex;
    std::vector<evidence_entry_t> _entries;
};

} // namespace zlink::framework::e2e::store_failure::provider
