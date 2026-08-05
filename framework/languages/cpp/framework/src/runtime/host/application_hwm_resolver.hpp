/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/configuration/detail/framework_options_state.hpp>

#include <cstdint>
#include <optional>

namespace zlink::framework::runtime::host::detail
{

// These candidates are kept separate so the C++ runtime can select the
// smallest finite OS limit while retaining an explicit process limit as the
// authoritative value. The structure is also the internal test seam for the
// host-memory fallback rules.
struct application_hwm_memory_limits_t
{
    std::optional<std::uint64_t> explicit_process;
    std::optional<std::uint64_t> cgroup;
    std::optional<std::uint64_t> windows_job;
    std::optional<std::uint64_t> address_space;
    std::optional<std::uint64_t> physical_total;
};

inline std::optional<std::uint64_t> effective_application_memory_limit (
  const application_hwm_memory_limits_t &limits) noexcept
{
    auto selected = limits.explicit_process;
    const auto take_smaller = [&selected] (std::optional<std::uint64_t> candidate) {
        if (candidate && (!selected || *candidate < *selected)) {
            selected = candidate;
        }
    };
    if (!selected) {
        take_smaller (limits.cgroup);
        take_smaller (limits.windows_job);
        take_smaller (limits.address_space);
    }
    if (!selected) {
        selected = limits.physical_total;
    }
    return selected;
}

inline std::optional<std::uint64_t> calculate_application_hwm (
  std::optional<std::uint64_t> configured,
  zlink::framework::application_hwm_profile_t profile,
  const application_hwm_memory_limits_t &limits) noexcept
{
    if (configured) {
        return configured;
    }

    const auto effective = effective_application_memory_limit (limits);
    if (!effective) {
        return std::nullopt;
    }

    std::uint64_t percent = 0;
    switch (profile) {
        case zlink::framework::application_hwm_profile_t::compact:
            percent = 2;
            break;
        case zlink::framework::application_hwm_profile_t::low_latency:
            percent = 5;
            break;
        case zlink::framework::application_hwm_profile_t::balanced:
            percent = 10;
            break;
        case zlink::framework::application_hwm_profile_t::throughput:
            percent = 20;
            break;
    }

    const auto result =
      (*effective / 100) * percent
      + ((*effective % 100) * percent) / 100;
    return result == 0 ? std::nullopt : std::optional<std::uint64_t> (result);
}

} // namespace zlink::framework::runtime::host::detail
