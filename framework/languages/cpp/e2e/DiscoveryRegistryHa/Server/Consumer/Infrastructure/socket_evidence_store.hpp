/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../../Shared/store_failure_contracts.hpp"

#include <zlink/framework.hpp>

#include <mutex>
#include <string>
#include <vector>

namespace zlink::framework::e2e::store_failure::consumer
{

class socket_evidence_store_t
{
  public:
    void record (const zlink::framework::log_record_t &record)
    {
        if (record.message != "zlink.runtime.transport.connection_changed") {
            return;
        }
        std::string kind;
        for (const auto &field : record.fields) {
            if (field.key != "state") {
                continue;
            }
            if (field.value == "connected")
                kind = "Connected";
            else if (field.value == "ready")
                kind = "ConnectionReady";
            else if (field.value == "disconnected")
                kind = "Disconnected";
        }
        if (kind.empty ())
            return;
        std::lock_guard lock (_gate);
        _entries.push_back ({std::move (kind), {}});
    }

    std::vector<socket_evidence_entry_t> snapshot () const
    {
        std::lock_guard lock (_gate);
        return _entries;
    }

  private:
    mutable std::mutex _gate;
    std::vector<socket_evidence_entry_t> _entries;
};

} // namespace zlink::framework::e2e::store_failure::consumer
