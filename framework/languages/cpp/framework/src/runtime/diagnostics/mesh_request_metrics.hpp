/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <opentelemetry/metrics/provider.h>
#include <opentelemetry/metrics/noop.h>

#include <chrono>
#include <memory>
#include <string>
#include <utility>

namespace zlink::framework::runtime
{

enum class mesh_request_surface_t { none, node, channel, spot, instance_spot, actor };

// Instruments and label storage belong to the MeshNode, not to each request.
// The operation registry supplies its existing single terminal decision.
class mesh_request_metrics_t
{
  public:
    explicit mesh_request_metrics_t (std::string mesh_name) : _mesh_name (std::move (mesh_name))
    {
        auto provider = opentelemetry::metrics::Provider::GetMeterProvider ();
        if (dynamic_cast<opentelemetry::metrics::NoopMeterProvider *> (provider.get ()))
            return;
        auto meter = provider->GetMeter ("zlink.framework");
        _inflight = meter->CreateDoubleUpDownCounter (
          "zlink.mesh_node.requests.inflight", "", "{request}");
        _duration = meter->CreateDoubleHistogram ("zlink.mesh_node.request.duration", "", "s");
        _timeouts = meter->CreateDoubleCounter ("zlink.mesh_node.request.timeouts", "", "{request}");
    }

    bool enabled () const noexcept { return _inflight != nullptr; }

    void start (mesh_request_surface_t surface) const noexcept
    {
        _inflight->Add (1, {{"mesh_name", view (_mesh_name)}, {"surface", name (surface)}});
    }

    void complete (mesh_request_surface_t surface, std::chrono::steady_clock::time_point started,
                   opentelemetry::nostd::string_view outcome) const noexcept
    {
        _inflight->Add (-1, {{"mesh_name", view (_mesh_name)}, {"surface", name (surface)}});
        _duration->Record (
          std::chrono::duration<double> (std::chrono::steady_clock::now () - started).count (),
          {{"mesh_name", view (_mesh_name)}, {"surface", name (surface)}, {"outcome", outcome}},
          opentelemetry::context::Context{});
        if (outcome == "timed_out")
            _timeouts->Add (1, {{"mesh_name", view (_mesh_name)}, {"surface", name (surface)}});
    }

  private:
    static opentelemetry::nostd::string_view view (const std::string &value) noexcept
    {
        return {value.data (), value.size ()};
    }

    static opentelemetry::nostd::string_view name (mesh_request_surface_t surface) noexcept
    {
        switch (surface) {
            case mesh_request_surface_t::node: return "node";
            case mesh_request_surface_t::channel: return "channel";
            case mesh_request_surface_t::spot: return "spot";
            case mesh_request_surface_t::instance_spot: return "instance_spot";
            case mesh_request_surface_t::actor: return "actor";
            case mesh_request_surface_t::none: return "";
        }
        return "";
    }

    const std::string _mesh_name;
    opentelemetry::nostd::unique_ptr<opentelemetry::metrics::UpDownCounter<double>> _inflight;
    opentelemetry::nostd::unique_ptr<opentelemetry::metrics::Histogram<double>> _duration;
    opentelemetry::nostd::unique_ptr<opentelemetry::metrics::Counter<double>> _timeouts;
};

// Arm only after admission to the existing completion registry. A failed
// selection never waits for a reply and belongs solely to selection_failures.
class mesh_request_metric_t
{
  public:
    mesh_request_metric_t () = default;
    // Take by reference and copy only when armed. Taking by value would cost a
    // refcount pair on every request even with metrics collection disabled.
    mesh_request_metric_t (const std::shared_ptr<mesh_request_metrics_t> &metrics,
                           mesh_request_surface_t surface) : _surface (surface)
    {
        if (metrics && metrics->enabled () && surface != mesh_request_surface_t::none)
            _metrics = metrics;
    }
    mesh_request_metric_t (mesh_request_metric_t &&other) noexcept = default;
    mesh_request_metric_t &operator= (mesh_request_metric_t &&other) noexcept
    {
        complete ("failed");
        _metrics = std::move (other._metrics);
        _surface = other._surface;
        _started = other._started;
        return *this;
    }
    ~mesh_request_metric_t () { complete ("failed"); }

    void start () noexcept
    {
        if (_metrics) {
            _started = std::chrono::steady_clock::now ();
            _metrics->start (_surface);
        }
    }

    void complete (opentelemetry::nostd::string_view outcome) noexcept
    {
        if (_metrics && _started != std::chrono::steady_clock::time_point{})
            _metrics->complete (_surface, _started, outcome);
        _metrics.reset ();
    }

  private:
    std::shared_ptr<mesh_request_metrics_t> _metrics;
    mesh_request_surface_t _surface = mesh_request_surface_t::none;
    std::chrono::steady_clock::time_point _started;
};

} // namespace zlink::framework::runtime
