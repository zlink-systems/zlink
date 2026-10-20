/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once
#include "runtime/diagnostics/mesh_request_metrics.hpp"
#include "runtime/stateful/object_ref.hpp"

namespace zlink::framework::runtime
{
inline const char *metric_object_kind (stateful::object_kind_t kind) noexcept
{
    switch (kind) {
        case stateful::object_kind_t::actor:
            return "actor";
        case stateful::object_kind_t::user_spot:
            return "user_spot";
        case stateful::object_kind_t::instance_spot:
            return "instance_spot";
    }
    std::terminate ();
}

class relocation_metrics_t
{
  public:
    relocation_metrics_t ()
    {
        auto provider = opentelemetry::metrics::Provider::GetMeterProvider ();
        if (dynamic_cast<opentelemetry::metrics::NoopMeterProvider *> (provider.get ()))
            return;
        auto meter = provider->GetMeter ("zlink.framework");
        if (!meter)
            return;
        _interruption = meter->CreateDoubleHistogram ("zlink.relocation.interruption", "", "s");
        _resume = meter->CreateDoubleHistogram ("zlink.relocation.target_resume", "", "s");
        _convergence = meter->CreateDoubleHistogram ("zlink.relocation.route_convergence", "", "s");
        _timeout = meter->CreateDoubleCounter ("zlink.relocation.cutover_timeout", "", "{warning}");
    }
    void interruption (stateful::object_kind_t kind, double seconds) const
    {
        if (_interruption)
            _interruption->Record (seconds, {{"unit_kind", metric_object_kind (kind)}},
                                   opentelemetry::context::Context{});
    }
    void resume (stateful::object_kind_t kind, double seconds) const
    {
        if (_resume)
            _resume->Record (seconds, {{"unit_kind", metric_object_kind (kind)}},
                             opentelemetry::context::Context{});
    }
    void convergence (stateful::object_kind_t kind, double seconds) const
    {
        if (_convergence)
            _convergence->Record (seconds, {{"unit_kind", metric_object_kind (kind)}},
                                  opentelemetry::context::Context{});
    }
    void timeout (stateful::object_kind_t kind) const
    {
        if (_timeout)
            _timeout->Add (1, {{"unit_kind", metric_object_kind (kind)}});
    }

  private:
    opentelemetry::nostd::unique_ptr<opentelemetry::metrics::Counter<double>> _timeout;
    opentelemetry::nostd::unique_ptr<opentelemetry::metrics::Histogram<double>> _interruption,
      _resume, _convergence;
};
} // namespace zlink::framework::runtime
