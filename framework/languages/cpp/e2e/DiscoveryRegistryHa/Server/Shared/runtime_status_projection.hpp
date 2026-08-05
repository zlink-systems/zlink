/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Shared/store_failure_contracts.hpp"

#include <zlink/framework/contracts/locations/diagnostics.hpp>

#include <chrono>
#include <cstdint>

namespace zlink::framework::e2e::store_failure::server
{

inline std::int64_t unix_milliseconds (
  const std::optional<std::chrono::system_clock::time_point> &value)
{
    return value ? std::chrono::duration_cast<std::chrono::milliseconds> (
                     value->time_since_epoch ())
                     .count ()
                 : 0;
}

inline runtime_status_res_t project_runtime_status (const location_runtime_status_t &status)
{
    return {.store_healthy = status.store_healthy,
            .watch_enabled = status.watch_enabled,
            .owner_lease_healthy = status.owner_lease_healthy,
            .owner_lease_renewed_at_unix_ms = unix_milliseconds (status.owner_lease_renewed_at),
            .last_refresh_at_unix_ms = unix_milliseconds (status.last_refresh_at),
            .last_error = status.last_error.value_or (std::string{})};
}

} // namespace zlink::framework::e2e::store_failure::server
