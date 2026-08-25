/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/diagnostics/monitoring_runtime.hpp"
#include "runtime/dispatch/application_job_queue.hpp"

#include <zlink/Contracts/Core/context.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace zlink::framework::runtime
{

struct host_capacity_metric_descriptor_t
{
    std::string_view name;
    ::zlink::framework::detail::metric_instrument_kind_t kind;
    std::string_view unit;
    bool state_label = false;
};

inline constexpr std::array<host_capacity_metric_descriptor_t, 14>
  host_capacity_metric_catalog{{
    {"zlink.host.core_hwm.effective_budget",
     ::zlink::framework::detail::metric_instrument_kind_t::observable, "By", false},
    {"zlink.host.core_hwm.applied",
     ::zlink::framework::detail::metric_instrument_kind_t::observable, "By", false},
    {"zlink.host.core_hwm.accounted",
     ::zlink::framework::detail::metric_instrument_kind_t::observable, "By", true},
    {"zlink.host.core_hwm.completion_accounted",
     ::zlink::framework::detail::metric_instrument_kind_t::observable, "By", true},
    {"zlink.host.core_hwm.blocked_ratio",
     ::zlink::framework::detail::metric_instrument_kind_t::observable, "{ppm}", false},
    {"zlink.host.application_job_queue.limit",
     ::zlink::framework::detail::metric_instrument_kind_t::observable, "{job}", false},
    {"zlink.host.application_job_queue.jobs",
     ::zlink::framework::detail::metric_instrument_kind_t::observable, "{job}", true},
    {"zlink.host.application_job_queue.capacity_waiters",
     ::zlink::framework::detail::metric_instrument_kind_t::observable, "{waiter}", false},
    {"zlink.host.application_job_queue.capacity_waits",
     ::zlink::framework::detail::metric_instrument_kind_t::counter, "{wait}", false},
    {"zlink.host.application_job_queue.capacity_wait_duration",
     ::zlink::framework::detail::metric_instrument_kind_t::counter, "s", false},
    {"zlink.host.application_job_queue.pressure_state",
     ::zlink::framework::detail::metric_instrument_kind_t::observable, "{state}", true},
    {"zlink.host.application_job_queue.pressure_transitions",
     ::zlink::framework::detail::metric_instrument_kind_t::counter, "{transition}", true},
    {"zlink.host.application_job_queue.pause_duration",
     ::zlink::framework::detail::metric_instrument_kind_t::observable, "s", true},
    {"zlink.host.application_job_queue.flow_state_config_failures",
     ::zlink::framework::detail::metric_instrument_kind_t::counter, "{failure}", false},
  }};

class host_capacity_runtime_t
{
  public:
    host_capacity_runtime_t (
      std::shared_ptr<zlink::context_t> core_context,
      std::shared_ptr<application_job_queue_t> application_jobs,
      std::optional<std::uint64_t> configured_core_memory_limit,
      std::optional<std::uint64_t> configured_core_budget,
      core_hwm_profile_t configured_core_profile,
      std::shared_ptr<::zlink::framework::detail::monitoring_runtime_state_t> monitoring) :
        _core_context (std::move (core_context)),
        _application_jobs (std::move (application_jobs)),
        _configured_core_memory_limit (configured_core_memory_limit),
        _configured_core_budget (configured_core_budget),
        _configured_core_profile (configured_core_profile),
        _monitoring (std::move (monitoring))
    {
        if (!_core_context || !_application_jobs)
            throw std::invalid_argument (
              "Host Capacity requires Core and Application Job Queue owners");
    }

    std::shared_ptr<application_job_queue_t> application_jobs () const noexcept
    {
        return _application_jobs;
    }

    host_capacity_status_t snapshot () const
    {
        std::lock_guard lock (_mutex);
        const auto core = _core_context->core_hwm_budget_snapshot ();
        const auto application_jobs =
          _application_jobs->observation_snapshot ();
        host_capacity_status_t status;
        status.measurement_epoch = _measurement_epoch;
        status.core_hwm = project_core (core);
        status.application_job_queue = application_jobs.status;
        emit_metrics (status, application_jobs.pressure);
        return status;
    }

    void reset_metrics ()
    {
        std::lock_guard lock (_mutex);
        _core_context->reset_core_hwm_budget_metrics ();
        _application_jobs->reset_metrics ();
        ++_measurement_epoch;
    }

  private:
    core_hwm_status_t project_core (
      const zlink::core_hwm_budget_snapshot_t &core) const noexcept
    {
        return {
          _configured_core_memory_limit,
          _configured_core_budget,
          _configured_core_profile,
          core.effective_core_budget_bytes (),
          core.total_applied_hwm_bytes (),
          core.core_queue_accounted_bytes (),
          core.application_accounted_bytes (),
          core.current_accounted_bytes (),
          core.provisional_accounted_bytes (),
          core.peak_accounted_bytes (),
          core.completion_current_accounted_bytes (),
          core.completion_peak_accounted_bytes (),
          core.completion_pending_message_count (),
          core.total_messaging_accounted_bytes (),
          core.monitor_queue_applied_hwm_bytes (),
          core.monitor_queue_accounted_bytes (),
          core.total_instance_applied_hwm_bytes (),
          core.total_instance_accounted_bytes (),
          core.blocked_ratio_ppm (),
          core.active_directional_queue_count (),
          core.active_completion_directional_queue_count (),
          core.active_send_queue_count (),
          core.active_receive_queue_count (),
          core.outstanding_application_lease_count (),
          core.retired_queue_count (),
          core.deferred_origin_credit_bytes ()};
    }

    void emit_metrics (
      const host_capacity_status_t &status,
      const application_job_queue_pressure_metrics_t &pressure) const noexcept
    {
        if (!_monitoring
            || !_monitoring->diagnostics_logger.is_enabled (
              log_level_t::debug)) {
            return;
        }
        const auto publish = [this] (
          std::string name,
          double value,
          std::string unit,
          ::zlink::framework::detail::metric_instrument_kind_t kind,
          std::map<std::string, std::string> tags = {}) {
            ::zlink::framework::detail::monitoring_runtime_t (_monitoring).publish_metric ({
              std::move (name), value, std::move (unit), kind,
              ::zlink::framework::detail::metric_temporality_t::current, std::move (tags)});
        };
        const auto &core = status.core_hwm;
        publish ("zlink.host.core_hwm.effective_budget",
                 static_cast<double> (core.effective_budget_bytes), "By",
                 ::zlink::framework::detail::metric_instrument_kind_t::observable);
        publish ("zlink.host.core_hwm.applied",
                 static_cast<double> (core.total_applied_hwm_bytes), "By",
                 ::zlink::framework::detail::metric_instrument_kind_t::observable);
        publish ("zlink.host.core_hwm.accounted",
                 static_cast<double> (core.current_accounted_bytes), "By",
                 ::zlink::framework::detail::metric_instrument_kind_t::observable,
                 {{"state", "current"}});
        publish ("zlink.host.core_hwm.accounted",
                 static_cast<double> (core.peak_accounted_bytes), "By",
                 ::zlink::framework::detail::metric_instrument_kind_t::observable,
                 {{"state", "peak"}});
        publish ("zlink.host.core_hwm.completion_accounted",
                 static_cast<double> (
                   core.completion_current_accounted_bytes),
                 "By", ::zlink::framework::detail::metric_instrument_kind_t::observable,
                 {{"state", "current"}});
        publish ("zlink.host.core_hwm.completion_accounted",
                 static_cast<double> (
                   core.completion_peak_accounted_bytes),
                 "By", ::zlink::framework::detail::metric_instrument_kind_t::observable,
                 {{"state", "peak"}});
        publish ("zlink.host.core_hwm.blocked_ratio",
                 static_cast<double> (core.blocked_ratio_ppm), "{ppm}",
                 ::zlink::framework::detail::metric_instrument_kind_t::observable);

        const auto &jobs = status.application_job_queue;
        publish ("zlink.host.application_job_queue.limit",
                 jobs.effective_max_queued_application_jobs, "{job}",
                 ::zlink::framework::detail::metric_instrument_kind_t::observable);
        publish ("zlink.host.application_job_queue.jobs",
                 jobs.reserved_supply_permits, "{job}",
                 ::zlink::framework::detail::metric_instrument_kind_t::observable,
                 {{"state", "reserved"}});
        publish ("zlink.host.application_job_queue.jobs",
                 jobs.queued_application_jobs, "{job}",
                 ::zlink::framework::detail::metric_instrument_kind_t::observable,
                 {{"state", "queued"}});
        publish ("zlink.host.application_job_queue.jobs",
                 jobs.permits_in_use, "{job}",
                 ::zlink::framework::detail::metric_instrument_kind_t::observable,
                 {{"state", "in_use"}});
        publish ("zlink.host.application_job_queue.jobs",
                 jobs.peak_permits_in_use, "{job}",
                 ::zlink::framework::detail::metric_instrument_kind_t::observable,
                 {{"state", "peak"}});
        publish ("zlink.host.application_job_queue.capacity_waiters",
                 jobs.capacity_waiters, "{waiter}",
                 ::zlink::framework::detail::metric_instrument_kind_t::observable);
        publish ("zlink.host.application_job_queue.capacity_waits",
                 static_cast<double> (jobs.capacity_wait_count), "{wait}",
                 ::zlink::framework::detail::metric_instrument_kind_t::counter);
        publish ("zlink.host.application_job_queue.capacity_wait_duration",
                 std::chrono::duration<double> (
                   jobs.capacity_wait_duration).count (),
                 "s", ::zlink::framework::detail::metric_instrument_kind_t::counter);
        const auto pressure_state =
          jobs.pressure_state
              == application_job_queue_pressure_state_t::paused
            ? "paused"
            : "running";
        publish ("zlink.host.application_job_queue.pressure_state",
                 1.0, "{state}",
                 ::zlink::framework::detail::metric_instrument_kind_t::observable,
                 {{"state", pressure_state}});
        publish ("zlink.host.application_job_queue.pressure_transitions",
                 static_cast<double> (pressure.running_transition_count),
                 "{transition}",
                 ::zlink::framework::detail::metric_instrument_kind_t::counter,
                 {{"state", "running"}});
        publish ("zlink.host.application_job_queue.pressure_transitions",
                 static_cast<double> (pressure.paused_transition_count),
                 "{transition}",
                 ::zlink::framework::detail::metric_instrument_kind_t::counter,
                 {{"state", "paused"}});
        publish ("zlink.host.application_job_queue.pause_duration",
                 std::chrono::duration<double> (
                   jobs.current_pause_duration).count (),
                 "s",
                 ::zlink::framework::detail::metric_instrument_kind_t::observable,
                 {{"state", "current"}});
        publish ("zlink.host.application_job_queue.pause_duration",
                 std::chrono::duration<double> (
                   pressure.cumulative_pause_duration).count (),
                 "s",
                 ::zlink::framework::detail::metric_instrument_kind_t::observable,
                 {{"state", "cumulative"}});
        publish ("zlink.host.application_job_queue.flow_state_config_failures",
                 static_cast<double> (
                   pressure.flow_state_config_failure_count),
                 "{failure}",
                 ::zlink::framework::detail::metric_instrument_kind_t::counter);
    }

    std::shared_ptr<zlink::context_t> _core_context;
    std::shared_ptr<application_job_queue_t> _application_jobs;
    std::optional<std::uint64_t> _configured_core_memory_limit;
    std::optional<std::uint64_t> _configured_core_budget;
    core_hwm_profile_t _configured_core_profile;
    std::shared_ptr<::zlink::framework::detail::monitoring_runtime_state_t> _monitoring;
    mutable std::mutex _mutex;
    std::uint64_t _measurement_epoch = 0;
};

} // namespace zlink::framework::runtime
