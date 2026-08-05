/* SPDX-License-Identifier: MPL-2.0 */
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace zlink::framework
{

enum class framework_runtime_state_t : std::uint8_t
{
    preparing = 0,
    serving = 1,
    relocating = 2,
    relocated = 3,
    draining = 4,
    stopped = 5,
    error = 6
};

enum class relocation_mode_t : std::uint8_t
{
    planned_maintenance = 0,
    rolling_update = 1
};

enum class relocation_outcome_t : std::uint8_t
{
    relocated = 0,
    blocked = 1
};

enum class relocation_reason_t : std::uint8_t
{
    none = 0,
    target_unavailable = 1,
    store_unavailable = 2,
    relocation_disabled = 3,
    state_incompatible = 4,
    deadline_exceeded = 5,
    relocation_failed = 6,
    runtime_not_ready = 7,
    manual_topology_unsupported = 8,
    shutdown_requested = 9,
    operation_in_progress = 10
};

struct relocation_options_t
{
    relocation_mode_t mode = relocation_mode_t::planned_maintenance;
    std::optional<std::int64_t> target_application_version;
    std::optional<std::chrono::milliseconds> deadline;

    friend bool operator== (const relocation_options_t &,
                            const relocation_options_t &) = default;
};

struct relocation_result_t
{
    relocation_mode_t mode = relocation_mode_t::planned_maintenance;
    std::int64_t effective_target_application_version = 0;
    relocation_outcome_t outcome = relocation_outcome_t::blocked;
    relocation_reason_t reason = relocation_reason_t::runtime_not_ready;

    friend bool operator== (const relocation_result_t &,
                            const relocation_result_t &) = default;
};

enum class termination_outcome_t : std::uint8_t
{
    stopped = 0,
    force_stopped = 1
};

enum class termination_reason_t : std::uint8_t
{
    none = 0,
    deadline_exceeded = 1,
    teardown_failed = 2
};

struct termination_result_t
{
    termination_outcome_t outcome = termination_outcome_t::stopped;
    termination_reason_t reason = termination_reason_t::none;

    friend bool operator== (const termination_result_t &,
                            const termination_result_t &) = default;
};

} // namespace zlink::framework
