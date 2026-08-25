/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/dispatch/application_job_queue.hpp"

#include <zlink/framework/contracts/errors/error.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <thread>

#if defined(__linux__)
#include <sched.h>
#endif

namespace zlink::framework::runtime
{

struct application_job_queue_processor_limits_t
{
    std::optional<std::uint32_t> logical_processors;
    std::optional<std::uint32_t> affinity_processors;
    std::optional<std::uint32_t> cpuset_processors;
    std::optional<std::uint32_t> quota_processors;
    std::optional<std::uint32_t> executor_max_concurrency;
};

inline std::uint32_t effective_application_job_processors (
  const application_job_queue_processor_limits_t &limits) noexcept
{
    std::optional<std::uint32_t> result;
    const auto take = [&result] (std::optional<std::uint32_t> value) {
        if (!value || *value == 0)
            return;
        if (!result || *value < *result)
            result = *value;
    };
    take (limits.logical_processors);
    take (limits.affinity_processors);
    take (limits.cpuset_processors);
    take (limits.quota_processors);
    take (limits.executor_max_concurrency);
    return result.value_or (1);
}

inline std::uint32_t application_job_queue_profile_multiplier (
  application_job_queue_profile_t profile)
{
    switch (profile) {
        case application_job_queue_profile_t::compact:
            return 32;
        case application_job_queue_profile_t::low_latency:
            return 64;
        case application_job_queue_profile_t::balanced:
            return 128;
        case application_job_queue_profile_t::throughput:
            return 256;
    }
    throw framework_exception_t (
      framework_error_kind_t::protocol_error,
      "Application Job Queue profile is invalid");
}

inline std::uint32_t calculate_application_job_queue_limit (
  application_job_queue_profile_t profile,
  std::uint32_t effective_processors)
{
    const auto multiplier =
      application_job_queue_profile_multiplier (profile);
    const auto calculated =
      static_cast<std::uint64_t> (multiplier)
      * std::max<std::uint32_t> (1, effective_processors);
    if (calculated
        > static_cast<std::uint64_t> (
          std::numeric_limits<std::int32_t>::max ())) {
        throw framework_exception_t (
          framework_error_kind_t::protocol_error,
          "Application Job Queue effective capacity overflows signed 32-bit range");
    }
    return static_cast<std::uint32_t> (calculated);
}

namespace application_job_queue_capacity_detail
{

inline std::optional<std::uint64_t> read_unsigned_file (
  const char *path) noexcept
{
    try {
        std::ifstream input (path);
        std::uint64_t value = 0;
        if (input >> value && value != 0)
            return value;
    }
    catch (...) {
    }
    return std::nullopt;
}

inline std::optional<std::uint32_t> parse_cpuset_count (
  std::string value) noexcept
{
    try {
        std::uint64_t count = 0;
        std::stringstream ranges (std::move (value));
        std::string range;
        while (std::getline (ranges, range, ',')) {
            const auto dash = range.find ('-');
            const auto first = static_cast<std::uint64_t> (
              std::stoull (range.substr (0, dash)));
            const auto last = dash == std::string::npos
                                ? first
                                : static_cast<std::uint64_t> (
                                    std::stoull (range.substr (dash + 1)));
            if (last < first)
                return std::nullopt;
            count += last - first + 1;
        }
        if (count == 0)
            return std::nullopt;
        return static_cast<std::uint32_t> (
          std::min<std::uint64_t> (
            count, std::numeric_limits<std::uint32_t>::max ()));
    }
    catch (...) {
        return std::nullopt;
    }
}

inline std::optional<std::uint32_t> read_cpuset_count () noexcept
{
#if defined(__linux__)
    for (const char *path : {
           "/sys/fs/cgroup/cpuset.cpus.effective",
           "/sys/fs/cgroup/cpuset/cpuset.cpus"}) {
        try {
            std::ifstream input (path);
            std::string value;
            if (input >> value) {
                if (auto count = parse_cpuset_count (std::move (value)))
                    return count;
            }
        }
        catch (...) {
        }
    }
#endif
    return std::nullopt;
}

inline std::optional<std::uint32_t> read_quota_processors () noexcept
{
#if defined(__linux__)
    try {
        std::ifstream input ("/sys/fs/cgroup/cpu.max");
        std::string quota;
        std::uint64_t period = 0;
        if (input >> quota >> period && quota != "max" && period != 0) {
            const auto quota_value = std::stoull (quota);
            return static_cast<std::uint32_t> (
              std::max<std::uint64_t> (1, quota_value / period));
        }
    }
    catch (...) {
    }
    const auto quota = read_unsigned_file (
      "/sys/fs/cgroup/cpu/cpu.cfs_quota_us");
    const auto period = read_unsigned_file (
      "/sys/fs/cgroup/cpu/cpu.cfs_period_us");
    if (quota && period && *period != 0) {
        return static_cast<std::uint32_t> (
          std::max<std::uint64_t> (1, *quota / *period));
    }
#endif
    return std::nullopt;
}

inline std::optional<std::uint32_t> affinity_processors () noexcept
{
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO (&set);
    if (sched_getaffinity (0, sizeof (set), &set) == 0) {
        const auto count = CPU_COUNT (&set);
        if (count > 0)
            return static_cast<std::uint32_t> (count);
    }
#endif
    return std::nullopt;
}

} // namespace application_job_queue_capacity_detail

inline application_job_queue_processor_limits_t
detect_application_job_queue_processor_limits (
  std::optional<std::uint32_t> executor_max_concurrency = std::nullopt)
{
    const auto logical = std::thread::hardware_concurrency ();
    return {
      logical == 0 ? std::nullopt
                   : std::optional<std::uint32_t> (logical),
      application_job_queue_capacity_detail::affinity_processors (),
      application_job_queue_capacity_detail::read_cpuset_count (),
      application_job_queue_capacity_detail::read_quota_processors (),
      executor_max_concurrency};
}

inline application_job_queue_configuration_t
resolve_application_job_queue_configuration (
  application_job_queue_profile_t profile,
  std::optional<std::uint32_t> configured_manual_max,
  const application_job_queue_processor_limits_t &limits,
  std::uint32_t pause_threshold_percent = 80,
  std::uint32_t resume_threshold_percent = 60)
{
    if (configured_manual_max
        && (*configured_manual_max == 0
            || *configured_manual_max
                 > static_cast<std::uint32_t> (
                   std::numeric_limits<std::int32_t>::max ()))) {
        throw framework_exception_t (
          framework_error_kind_t::protocol_error,
          "maximum queued application jobs must be between 1 and 2147483647");
    }
    const auto processors =
      effective_application_job_processors (limits);
    return {
      profile,
      configured_manual_max,
      processors,
      configured_manual_max.value_or (
        calculate_application_job_queue_limit (profile, processors)),
      pause_threshold_percent,
      resume_threshold_percent};
}

} // namespace zlink::framework::runtime
