/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include "runtime/diagnostics/mesh_request_metrics.hpp"

namespace zlink::framework::runtime
{
enum class instance_spot_activation_outcome_t
{
    ready,
    rejected,
    conflict,
    timed_out,
    shutdown,
    store_failure,
    fenced
};

class instance_spot_metrics_t
{
  public:
    explicit instance_spot_metrics_t (std::string mesh_name) : _mesh_name (std::move (mesh_name))
    {
        auto provider = opentelemetry::metrics::Provider::GetMeterProvider ();
        if (dynamic_cast<opentelemetry::metrics::NoopMeterProvider *> (provider.get ()))
            return;
        auto meter = provider->GetMeter ("zlink.framework");
        _activations =
          meter->CreateDoubleCounter ("zlink.instance_spot.activations", "", "{activation}");
        _duration =
          meter->CreateDoubleHistogram ("zlink.instance_spot.activation.duration", "", "s");
    }
    bool enabled () const noexcept { return _activations != nullptr; }
    void complete (std::chrono::steady_clock::time_point started,
                   const std::string &stable_type,
                   instance_spot_activation_outcome_t outcome) const noexcept
    {
        if (!enabled ())
            return;
        const auto mesh_name =
          opentelemetry::nostd::string_view (_mesh_name.data (), _mesh_name.size ());
        const auto type =
          opentelemetry::nostd::string_view (stable_type.data (), stable_type.size ());
        _activations->Add (1, {{mesh_request_metric_field::mesh_name, mesh_name},
                               {instance_spot_type, type},
                               {mesh_request_metric_field::outcome, name (outcome)}});
        _duration->Record (
          std::chrono::duration<double> (std::chrono::steady_clock::now () - started).count (),
          {{mesh_request_metric_field::mesh_name, mesh_name},
           {instance_spot_type, type},
           {mesh_request_metric_field::outcome, name (outcome)}},
          opentelemetry::context::Context{});
    }

  private:
    static constexpr char instance_spot_type[] = "instance_spot_type";
    static opentelemetry::nostd::string_view
    name (instance_spot_activation_outcome_t outcome) noexcept
    {
        switch (outcome) {
            case instance_spot_activation_outcome_t::ready:
                return "ready";
            case instance_spot_activation_outcome_t::rejected:
                return "rejected";
            case instance_spot_activation_outcome_t::conflict:
                return "conflict";
            case instance_spot_activation_outcome_t::timed_out:
                return "timed_out";
            case instance_spot_activation_outcome_t::shutdown:
                return "shutdown";
            case instance_spot_activation_outcome_t::store_failure:
                return "store_failure";
            case instance_spot_activation_outcome_t::fenced:
                return "fenced";
        }
        return "rejected";
    }
    const std::string _mesh_name;
    opentelemetry::nostd::unique_ptr<opentelemetry::metrics::Counter<double>> _activations;
    opentelemetry::nostd::unique_ptr<opentelemetry::metrics::Histogram<double>> _duration;
};
} // namespace zlink::framework::runtime
