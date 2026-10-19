/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once
#include "runtime/diagnostics/mesh_request_metrics.hpp"
#include "runtime/stateful/object_ref.hpp"
#include <zlink/framework/contracts/spots/spot.hpp>
#include <map>

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
    explicit relocation_metrics_t (std::string mesh_name) : _mesh_name (std::move (mesh_name))
    {
        auto provider = opentelemetry::metrics::Provider::GetMeterProvider ();
        if (dynamic_cast<opentelemetry::metrics::NoopMeterProvider *> (provider.get ()))
            return;
        auto meter = provider->GetMeter ("zlink.framework");
        if (!meter)
            return;
        _started = meter->CreateDoubleCounter ("zlink.relocation.started", "", "{relocation}");
        _completed = meter->CreateDoubleCounter ("zlink.relocation.completed", "", "{relocation}");
        _duration = meter->CreateDoubleHistogram ("zlink.relocation.duration", "", "s");
        _bytes = meter->CreateDoubleHistogram ("zlink.relocation.bytes", "", "By");
        _interruption = meter->CreateDoubleHistogram ("zlink.relocation.interruption", "", "s");
        _resume = meter->CreateDoubleHistogram ("zlink.relocation.target_resume", "", "s");
        _convergence = meter->CreateDoubleHistogram ("zlink.relocation.route_convergence", "", "s");
        _timeout = meter->CreateDoubleCounter ("zlink.relocation.cutover_timeout", "", "{warning}");
    }
    bool enabled () const noexcept { return _started && _completed && _duration && _bytes; }
    using tags_t = std::map<std::string, std::string>;
    tags_t tags (stateful::object_kind_t kind, detail::factory_relocation_kind_t policy) const
    {
        return {{"mesh_name", _mesh_name},
                {"object_kind", metric_object_kind (kind)},
                {"policy", policy == detail::factory_relocation_kind_t::preserve_state
                             ? "snapshot"
                             : "recreate"}};
    }
    void start (const tags_t &tags) const { _started->Add (1, tags); }
    void complete (tags_t tags, const char *outcome, double seconds) const
    {
        tags.emplace ("outcome", outcome);
        _completed->Add (1, tags);
        _duration->Record (seconds, tags, opentelemetry::context::Context{});
    }
    void bytes (const tags_t &tags, double value) const
    {
        _bytes->Record (value, tags, opentelemetry::context::Context{});
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
    std::string _mesh_name;
    opentelemetry::nostd::unique_ptr<opentelemetry::metrics::Counter<double>> _started, _completed,
      _timeout;
    opentelemetry::nostd::unique_ptr<opentelemetry::metrics::Histogram<double>> _duration, _bytes,
      _interruption, _resume, _convergence;
};
} // namespace zlink::framework::runtime
