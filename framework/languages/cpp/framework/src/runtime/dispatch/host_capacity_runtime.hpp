/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/diagnostics/monitoring_runtime.hpp"
#include "runtime/dispatch/application_job_queue.hpp"

#include <zlink/Contracts/Core/context.hpp>

#include <opentelemetry/metrics/async_instruments.h>

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
        _configured_core_profile (configured_core_profile)
    {
        if (!_core_context || !_application_jobs)
            throw std::invalid_argument (
              "Host Capacity requires Core and Application Job Queue owners");
        if (!monitoring || !monitoring->metric_meter)
            return;
        for (std::size_t index = 0; index < _metrics.size (); ++index) {
            auto &registration = _metrics[index];
            const auto &descriptor = host_capacity_metric_catalog[index];
            const opentelemetry::nostd::string_view name (descriptor.name.data (),
                                                          descriptor.name.size ());
            const opentelemetry::nostd::string_view unit (descriptor.unit.data (),
                                                          descriptor.unit.size ());
            registration.owner = this;
            registration.index = index;
            registration.instrument =
              descriptor.kind == ::zlink::framework::detail::metric_instrument_kind_t::counter
                ? monitoring->metric_meter->CreateDoubleObservableCounter (name, "", unit)
                : monitoring->metric_meter->CreateDoubleObservableGauge (name, "", unit);
            registration.instrument->AddCallback (&emit_metrics, &registration);
        }
    }

    ~host_capacity_runtime_t ()
    {
        for (auto &registration : _metrics) {
            if (registration.instrument)
                registration.instrument->RemoveCallback (&emit_metrics, &registration);
        }
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

    struct metric_registration_t
    {
        host_capacity_runtime_t *owner = nullptr;
        std::size_t index = 0;
        opentelemetry::nostd::shared_ptr<opentelemetry::metrics::ObservableInstrument> instrument;
    };

    static void emit_metrics (opentelemetry::metrics::ObserverResult result, void *state)
    {
        const auto &registration = *static_cast<metric_registration_t *> (state);
        const auto &owner = *registration.owner;
        auto observer = opentelemetry::nostd::get<
          opentelemetry::nostd::shared_ptr<opentelemetry::metrics::ObserverResultT<double>>> (
          result);
        // Each callback reads the existing bounded aggregates under the same
        // lock as Reset. Collection never resets or re-accumulates an epoch.
        std::lock_guard lock (owner._mutex);
        const auto core = owner.project_core (owner._core_context->core_hwm_budget_snapshot ());
        const auto application_jobs = owner._application_jobs->observation_snapshot ();
        const auto &jobs = application_jobs.status;
        const auto &pressure = application_jobs.pressure;
        const auto pressure_state =
          jobs.pressure_state == application_job_queue_pressure_state_t::paused ? "paused"
                                                                                : "running";
        switch (registration.index) {
            case 0: // zlink.host.core_hwm.effective_budget
                observer->Observe (static_cast<double> (core.effective_budget_bytes));
                break;
            case 1: // zlink.host.core_hwm.applied
                observer->Observe (static_cast<double> (core.total_applied_hwm_bytes));
                break;
            case 2: // zlink.host.core_hwm.accounted
                observer->Observe (static_cast<double> (core.current_accounted_bytes),
                                   {{"state", "current"}});
                observer->Observe (static_cast<double> (core.peak_accounted_bytes),
                                   {{"state", "peak"}});
                break;
            case 3: // zlink.host.core_hwm.completion_accounted
                observer->Observe (static_cast<double> (core.completion_current_accounted_bytes),
                                   {{"state", "current"}});
                observer->Observe (static_cast<double> (core.completion_peak_accounted_bytes),
                                   {{"state", "peak"}});
                break;
            case 4: // zlink.host.core_hwm.blocked_ratio
                observer->Observe (static_cast<double> (core.blocked_ratio_ppm));
                break;
            case 5: // zlink.host.application_job_queue.limit
                observer->Observe (jobs.effective_max_queued_application_jobs);
                break;
            case 6: // zlink.host.application_job_queue.jobs
                observer->Observe (jobs.reserved_supply_permits, {{"state", "reserved"}});
                observer->Observe (jobs.queued_application_jobs, {{"state", "queued"}});
                observer->Observe (jobs.permits_in_use, {{"state", "in_use"}});
                observer->Observe (jobs.peak_permits_in_use, {{"state", "peak"}});
                break;
            case 7: // zlink.host.application_job_queue.capacity_waiters
                observer->Observe (jobs.capacity_waiters);
                break;
            case 8: // zlink.host.application_job_queue.capacity_waits
                observer->Observe (static_cast<double> (jobs.capacity_wait_count));
                break;
            case 9: // zlink.host.application_job_queue.capacity_wait_duration
                observer->Observe (
                  std::chrono::duration<double> (jobs.capacity_wait_duration).count ());
                break;
            case 10: // zlink.host.application_job_queue.pressure_state
                observer->Observe (1.0, {{"state", pressure_state}});
                break;
            case 11: // zlink.host.application_job_queue.pressure_transitions
                observer->Observe (static_cast<double> (pressure.running_transition_count),
                                   {{"state", "running"}});
                observer->Observe (static_cast<double> (pressure.paused_transition_count),
                                   {{"state", "paused"}});
                break;
            case 12: // zlink.host.application_job_queue.pause_duration
                observer->Observe (
                  std::chrono::duration<double> (jobs.current_pause_duration).count (),
                  {{"state", "current"}});
                observer->Observe (
                  std::chrono::duration<double> (pressure.cumulative_pause_duration).count (),
                  {{"state", "cumulative"}});
                break;
            case 13: // zlink.host.application_job_queue.flow_state_config_failures
                observer->Observe (static_cast<double> (pressure.flow_state_config_failure_count));
                break;
        }
    }

    std::shared_ptr<zlink::context_t> _core_context;
    std::shared_ptr<application_job_queue_t> _application_jobs;
    std::optional<std::uint64_t> _configured_core_memory_limit;
    std::optional<std::uint64_t> _configured_core_budget;
    core_hwm_profile_t _configured_core_profile;
    std::array<metric_registration_t, host_capacity_metric_catalog.size ()> _metrics;
    mutable std::mutex _mutex;
    std::uint64_t _measurement_epoch = 0;
};

} // namespace zlink::framework::runtime
