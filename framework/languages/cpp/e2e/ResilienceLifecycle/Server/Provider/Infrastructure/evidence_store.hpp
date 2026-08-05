/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../../Shared/resilience_lifecycle_messages.hpp"

#include <fstream>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace zlink::framework::e2e::resilience_lifecycle::provider
{

class evidence_store_t
{
  public:
    evidence_store_t (std::string provider_rid, std::string instance_id, std::string file_path) :
        _provider_rid (std::move (provider_rid)),
        _instance_id (std::move (instance_id)),
        _file_path (std::move (file_path))
    {
        if (!_file_path.empty ()) {
            std::ofstream file (_file_path, std::ios::trunc);
        }
    }

    void record (std::string marker, std::string value)
    {
        std::lock_guard lock (_mutex);
        const auto entry_marker = marker;
        const auto entry_value = value;
        _entries.push_back ({std::move (marker), _provider_rid, std::move (value)});
        if (!_file_path.empty ()) {
            std::ofstream file (_file_path, std::ios::app);
            file << evidence_line (entry_marker, entry_value) << '\n';
        }
    }

    evidence_snapshot_t snapshot () const
    {
        std::lock_guard lock (_mutex);
        return {.provider_rid = _provider_rid, .entries = _entries};
    }

    const std::string &provider_rid () const
    {
        return _provider_rid;
    }

    const std::string &instance_id () const
    {
        return _instance_id;
    }

  private:
    mutable std::mutex _mutex;
    std::string _provider_rid;
    std::string _instance_id;
    std::string _file_path;
    std::vector<evidence_entry_t> _entries;

    std::string evidence_line (const std::string &marker, const std::string &value) const
    {
        if (marker == "ProfileStart") {
            return "profile-start|rid=" + _provider_rid + "|marker=" + value;
        }
        if (marker == "ProfileReq") {
            return "profile-request|rid=" + _provider_rid + "|marker=" + value;
        }
        if (marker == "ProfileMsg") {
            return "profile-command|rid=" + _provider_rid + "|marker=" + value;
        }
        if (marker == "ProfileFault") {
            return "profile-fault|rid=" + _provider_rid + "|marker=" + value;
        }
        return marker + "|rid=" + _provider_rid + "|value=" + value;
    }
};

} // namespace zlink::framework::e2e::resilience_lifecycle::provider
