/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Shared/monitoring_event_recorders.hpp"

#include <zlink/framework.hpp>

#include <stdexcept>
#include <string>

namespace zlink::framework::e2e::runtime_monitoring::service
{

inline void record_throwing_runtime_log (server::evidence_store_t &evidence,
                                         const zlink::framework::log_record_t &record)
{
    if (record.message != "zlink.runtime.transport.connection_changed") {
        return;
    }
    evidence.add ("monitor-throw|source=" + server::log_field (record, "source_name")
                  + "|kind=" + server::log_field (record, "state"));
    throw std::runtime_error ("monitoring dispatch failure for e2e");
}

} // namespace zlink::framework::e2e::runtime_monitoring::service
