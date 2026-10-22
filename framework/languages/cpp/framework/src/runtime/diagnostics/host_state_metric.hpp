/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once
#include "runtime/diagnostics/mesh_request_metrics.hpp"
#include <zlink/framework/contracts/configuration/lifecycle.hpp>
#include <atomic>

namespace zlink::framework::runtime
{
// The observable reads the lifecycle authority itself. There is no second
// state snapshot and no retained series for states the host has left.
class host_state_metric_t
{
  public:
    host_state_metric_t ()
    {
        auto provider = opentelemetry::metrics::Provider::GetMeterProvider ();
        if (dynamic_cast<opentelemetry::metrics::NoopMeterProvider *> (provider.get ()))
            return;
        auto meter = provider->GetMeter ("zlink.framework");
        if (!meter)
            return;
        _instrument = meter->CreateDoubleObservableGauge ("zlink.host.state", "", "{runtime}");
        if (_instrument)
            _instrument->AddCallback (&observe, this);
    }
    ~host_state_metric_t ()
    {
        if (_instrument)
            _instrument->RemoveCallback (&observe, this);
    }
    framework_runtime_state_t load (std::memory_order order) const noexcept
    {
        return _state.load (order);
    }
    void store (framework_runtime_state_t state, std::memory_order order) noexcept
    {
        _state.store (state, order);
    }
    bool compare_exchange_strong (framework_runtime_state_t &expected,
                                  framework_runtime_state_t desired,
                                  std::memory_order order) noexcept
    {
        return _state.compare_exchange_strong (expected, desired, order);
    }

  private:
    static void observe (opentelemetry::metrics::ObserverResult result, void *context)
    {
        const auto state =
          static_cast<host_state_metric_t *> (context)->load (std::memory_order_acquire);
        constexpr const char *names[] = {"preparing", "serving", "relocating", "relocated",
                                         "draining",  "stopped", "error"};
        auto observer = opentelemetry::nostd::get<
          opentelemetry::nostd::shared_ptr<opentelemetry::metrics::ObserverResultT<double>>> (
          result);
        // Cumulative readers may retain an attribute set from an earlier
        // collection. Publish its current zero as well, so a departed state
        // never remains at one in the provider's retained series.
        for (unsigned i = 0; i != std::size (names); ++i)
            observer->Observe (i == static_cast<unsigned> (state) ? 1.0 : 0.0,
                               {{"state", names[i]}});
    }
    std::atomic<framework_runtime_state_t> _state{framework_runtime_state_t::preparing};
    opentelemetry::nostd::shared_ptr<opentelemetry::metrics::ObservableInstrument> _instrument;
};
} // namespace zlink::framework::runtime
