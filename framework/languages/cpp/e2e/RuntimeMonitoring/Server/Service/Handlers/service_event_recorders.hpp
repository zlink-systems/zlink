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
    if (record.message != "message flow") {
        return;
    }
    evidence.add ("message-flow-provider-throw|event="
                  + server::log_field (record, "event_id")
                  + "|phase=" + server::log_field (record, "phase"));
    throw std::runtime_error ("message-flow provider failure for e2e");
}

} // namespace zlink::framework::e2e::runtime_monitoring::service
