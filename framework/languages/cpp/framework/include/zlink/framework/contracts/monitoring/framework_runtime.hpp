/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/configuration/lifecycle.hpp>
#include <zlink/framework/contracts/configuration/detail/framework_options_state.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace zlink::framework
{

enum class listener_kind_t
{
    route_mesh,
    client_server,
    fanout,
    stream
};

struct listener_status_t
{
    listener_kind_t kind = listener_kind_t::route_mesh;
    std::string name;
    std::string endpoint;
    std::chrono::system_clock::time_point observed_at{};

    friend bool operator== (const listener_status_t &,
                            const listener_status_t &) = default;
};

struct observation_loss_t
{
    std::uint64_t coalesced_count = 0;
    std::uint64_t discarded_terminal_count = 0;

    friend bool operator== (const observation_loss_t &,
                            const observation_loss_t &) = default;
};

template <typename TStatus>
struct observed_status_t final
{
    TStatus status;
    observation_loss_t loss;
};

struct core_hwm_status_t
{
    std::optional<std::uint64_t> configured_memory_limit_bytes;
    std::optional<std::uint64_t> configured_budget_bytes;
    core_hwm_profile_t configured_profile =
      core_hwm_profile_t::balanced;
    std::uint64_t effective_budget_bytes = 0;
    std::uint64_t total_applied_hwm_bytes = 0;
    std::uint64_t core_queue_accounted_bytes = 0;
    std::uint64_t application_accounted_bytes = 0;
    std::uint64_t current_accounted_bytes = 0;
    std::uint64_t provisional_accounted_bytes = 0;
    std::uint64_t peak_accounted_bytes = 0;
    std::uint64_t completion_current_accounted_bytes = 0;
    std::uint64_t completion_peak_accounted_bytes = 0;
    std::uint64_t completion_pending_message_count = 0;
    std::uint64_t total_messaging_accounted_bytes = 0;
    std::uint64_t monitor_queue_applied_hwm_bytes = 0;
    std::uint64_t monitor_queue_accounted_bytes = 0;
    std::uint64_t total_instance_applied_hwm_bytes = 0;
    std::uint64_t total_instance_accounted_bytes = 0;
    std::uint64_t blocked_ratio_ppm = 0;
    std::uint64_t active_directional_queue_count = 0;
    std::uint64_t active_completion_directional_queue_count = 0;
    std::uint64_t active_send_queue_count = 0;
    std::uint64_t active_receive_queue_count = 0;
    std::uint64_t outstanding_application_lease_count = 0;
    std::uint64_t retired_queue_count = 0;
    std::uint64_t deferred_origin_credit_bytes = 0;

    friend bool operator== (const core_hwm_status_t &,
                            const core_hwm_status_t &) = default;
};

struct application_job_queue_status_t
{
    application_job_queue_profile_t configured_profile =
      application_job_queue_profile_t::balanced;
    std::optional<std::uint32_t> configured_manual_max;
    std::uint32_t effective_processor_count = 1;
    std::uint32_t effective_max_queued_application_jobs = 1;
    std::uint32_t reserved_supply_permits = 0;
    std::uint32_t queued_application_jobs = 0;
    std::uint32_t permits_in_use = 0;
    std::uint32_t peak_permits_in_use = 0;
    std::uint32_t capacity_waiters = 0;
    std::uint64_t capacity_wait_count = 0;
    std::chrono::nanoseconds capacity_wait_duration{};
    std::uint32_t configured_pause_threshold_percent = 80;
    std::uint32_t configured_resume_threshold_percent = 60;
    std::uint32_t pause_permit_count = 1;
    std::uint32_t resume_permit_count = 0;
    application_job_queue_pressure_state_t pressure_state =
      application_job_queue_pressure_state_t::running;
    std::chrono::nanoseconds current_pause_duration{};

    friend bool operator== (const application_job_queue_status_t &,
                            const application_job_queue_status_t &) = default;
};

struct host_capacity_status_t
{
    std::uint64_t measurement_epoch = 0;
    core_hwm_status_t core_hwm;
    application_job_queue_status_t application_job_queue;

    friend bool operator== (const host_capacity_status_t &,
                            const host_capacity_status_t &) = default;
};

struct framework_runtime_status_t
{
    framework_runtime_state_t state =
      framework_runtime_state_t::preparing;
    bool is_ready = false;
    bool accepting_work = false;
    std::optional<std::chrono::system_clock::time_point>
      operation_deadline;
    std::optional<relocation_result_t> relocation_result;
    std::optional<termination_result_t> termination_result;
    host_capacity_status_t capacity;
    std::uint64_t sequence = 0;
    std::chrono::system_clock::time_point observed_at;

    friend bool operator== (const framework_runtime_status_t &,
                            const framework_runtime_status_t &) = default;
};

class runtime_observation_t
{
  public:
    virtual ~runtime_observation_t () = default;
    virtual void close () noexcept = 0;
};

class framework_runtime_t
{
  public:
    virtual ~framework_runtime_t () = default;
    virtual framework_runtime_status_t status () const = 0;
    virtual void reset_capacity_metrics () = 0;
    virtual listener_status_t listener_status (
      listener_kind_t kind,
      std::string name) const = 0;
    virtual std::unique_ptr<runtime_observation_t>
    observe (
      std::size_t capacity,
      std::function<void (
        const observed_status_t<framework_runtime_status_t> &)> observer) = 0;
};

} // namespace zlink::framework
