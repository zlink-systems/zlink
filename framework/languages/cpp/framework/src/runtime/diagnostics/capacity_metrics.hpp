/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once
#include "runtime/diagnostics/mesh_request_metrics.hpp"
#include "runtime/locations/location_records.hpp"
#include "runtime/diagnostics/monitoring_runtime.hpp"
#include <map>
#include <optional>

namespace zlink::framework::runtime
{
// One last confirmed capacity record per registered local MeshNode. Reads of
// observables never perform Store I/O or enumerate the object population.
class capacity_metrics_t
{
    struct local_t
    {
        std::string mesh_name;
        placement_capacity_t limits;
        std::optional<placement_capacity_t> confirmed;
    };
    using snapshot_t = std::map<std::string, local_t>;
    struct observer_t
    {
        capacity_metrics_t *owner;
        unsigned field;
        bool typed;
        opentelemetry::nostd::shared_ptr<opentelemetry::metrics::ObservableInstrument> instrument;
    };

  public:
    explicit capacity_metrics_t (
      std::shared_ptr<framework::detail::monitoring_runtime_state_t> monitoring) :
        _monitoring (std::move (monitoring))
    {
        auto provider = opentelemetry::metrics::Provider::GetMeterProvider ();
        if (dynamic_cast<opentelemetry::metrics::NoopMeterProvider *> (provider.get ()))
            return;
        auto meter = provider->GetMeter ("zlink.framework");
        if (!meter)
            return;
        constexpr const char *names[] = {
          "zlink.object.capacity.active",      "zlink.object.capacity.reserved",
          "zlink.object.capacity.limit",       "zlink.spot.type.capacity.active",
          "zlink.spot.type.capacity.reserved", "zlink.spot.type.capacity.limit"};
        for (unsigned i = 0; i != 6; ++i) {
            auto &observer = _observers[i];
            observer.owner = this;
            observer.field = i % 3;
            observer.typed = i >= 3;
            observer.instrument = meter->CreateDoubleObservableGauge (
              names[i], "", observer.typed ? "{spot}" : "{object}");
            if (observer.instrument)
                observer.instrument->AddCallback (&observe, &observer);
        }
    }
    ~capacity_metrics_t ()
    {
        for (auto &observer : _observers)
            if (observer.instrument)
                observer.instrument->RemoveCallback (&observe, &observer);
    }
    void register_local (std::string key, const mesh_node_descriptor_t &descriptor)
    {
        mutate ([&] (snapshot_t &snapshot) {
            snapshot[key].mesh_name = descriptor.mesh_name;
            snapshot[key].limits = descriptor.capacity;
        });
    }
    template <typename Update> void confirm (const std::string &key, Update update)
    {
        mutate ([&] (snapshot_t &snapshot) {
            const auto found = snapshot.find (key);
            if (found == snapshot.end ())
                return;
            auto value = found->second.limits;
            update (value);
            found->second.confirmed = std::move (value);
        });
    }
    bool contains (const std::string &key) const { return snapshot ()->contains (key); }

  private:
    std::shared_ptr<const snapshot_t> snapshot () const
    {
        std::lock_guard lock (_monitoring->metric_mutex);
        return _snapshot;
    }
    template <typename Update> void mutate (Update update)
    {
        // Reuse the existing telemetry mutex; no runtime execution gate is
        // acquired, and no lock is added to application message processing.
        std::lock_guard lock (_monitoring->metric_mutex);
        auto next = std::make_shared<snapshot_t> (*_snapshot);
        update (*next);
        _snapshot = std::move (next);
    }
    static void observe (opentelemetry::metrics::ObserverResult result, void *context)
    {
        const auto &observation = *static_cast<observer_t *> (context);
        auto observer = opentelemetry::nostd::get<
          opentelemetry::nostd::shared_ptr<opentelemetry::metrics::ObserverResultT<double>>> (
          result);
        const auto snapshot = observation.owner->snapshot ();
        const auto value = [&] (const auto &usage) -> double {
            return observation.field == 0   ? usage.active
                   : observation.field == 1 ? usage.reserved
                                            : usage.limit;
        };
        for (const auto &[key, local] : *snapshot) {
            if (!local.confirmed)
                continue;
            const auto &capacity = *local.confirmed;
            const auto &mesh = local.mesh_name;
            if (observation.typed) {
                for (const auto &type : capacity.spot_types)
                    observer->Observe (
                      value (type.usage),
                      {{"mesh_name", mesh},
                       {"stable_type", type.stable_type},
                       {"spot_kind", type.object_kind == placement_object_kind_t::user_spot
                                       ? "user"
                                       : "instance"}});
            } else {
                observer->Observe (value (capacity.actors),
                                   {{"mesh_name", mesh}, {"capacity_scope", "actor"}});
                observer->Observe (value (capacity.spots),
                                   {{"mesh_name", mesh}, {"capacity_scope", "spot"}});
            }
        }
    }
    std::shared_ptr<framework::detail::monitoring_runtime_state_t> _monitoring;
    std::shared_ptr<const snapshot_t> _snapshot{std::make_shared<snapshot_t> ()};
    observer_t _observers[6]{};
};
} // namespace zlink::framework::runtime
