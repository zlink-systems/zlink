/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "evidence_store.hpp"

#include <zlink/framework.hpp>

#include <string>
#include <string_view>

namespace zlink::framework::e2e::runtime_monitoring::server
{

inline std::string log_field (const zlink::framework::log_record_t &record,
                              std::string_view name)
{
    for (const auto &field : record.fields) {
        if (field.key == name) {
            return field.value;
        }
    }
    return {};
}

inline void record_runtime_log (evidence_store_t &evidence,
                                const zlink::framework::log_record_t &record)
{
    if (record.message == "zlink.runtime.transport.connection_changed") {
        evidence.add ("monitor-socket|source=" + log_field (record, "source_name")
                      + "|kind=" + log_field (record, "state"));
    } else if (record.message == "zlink.runtime.location.store_changed") {
        evidence.add ("monitor-location|source="
                      + log_field (record, "source_name")
                      + "|identifier=zlink.runtime.location.store_changed|reason="
                      + log_field (record, "state"));
    } else if (record.message == "zlink.runtime.spot.timer_failed") {
        evidence.add ("monitor-spot|source=" + log_field (record, "source_name")
                      + "|kind=TimerFailed|timer=" + log_field (record, "timer_name"));
    } else if (record.message == "message flow") {
        evidence.add ("message-flow-provider|event="
                      + log_field (record, "event_id")
                      + "|phase=" + log_field (record, "phase")
                      + "|surface=" + log_field (record, "surface")
                      + "|corr=" + log_field (record, "corr"));
    }
}

} // namespace zlink::framework::e2e::runtime_monitoring::server
