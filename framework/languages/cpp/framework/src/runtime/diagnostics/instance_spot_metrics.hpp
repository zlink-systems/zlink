/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include "runtime/diagnostics/mesh_request_metrics.hpp"
#include <array>
#include <atomic>
#include <map>

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
        if (!meter)
            return;
        _activations =
          meter->CreateDoubleCounter ("zlink.instance_spot.activations", "", "{activation}");
        _duration =
          meter->CreateDoubleHistogram ("zlink.instance_spot.activation.duration", "", "s");
    }
    bool enabled () const noexcept { return _activations && _duration; }
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
class instance_spot_admission_metrics_t
{
  public:
    enum class conflict_reason_t
    {
        authority,
        spot_kind,
        spot_type,
        closing
    };
    instance_spot_admission_metrics_t (std::string mesh, std::string type) :
        _tags{{"mesh_name", std::move (mesh)}, {"instance_spot_type", std::move (type)}}
    {
        auto provider = opentelemetry::metrics::Provider::GetMeterProvider ();
        if (dynamic_cast<opentelemetry::metrics::NoopMeterProvider *> (provider.get ()))
            return;
        auto meter = provider->GetMeter ("zlink.framework");
        if (!meter)
            return;
        _conflicts =
          meter->CreateDoubleCounter ("zlink.instance_spot.claim.conflicts", "", "{claim}");
        _messages = meter->CreateDoubleObservableGauge ("zlink.instance_spot.pending.messages", "",
                                                        "{message}");
        _bytes = meter->CreateDoubleObservableGauge ("zlink.instance_spot.pending.bytes", "", "By");
        constexpr const char *reasons[] = {"authority", "spot_kind", "spot_type", "closing"};
        for (unsigned i = 0; i != _reasons.size (); ++i) {
            _reasons[i] = _tags;
            _reasons[i].emplace ("reason", reasons[i]);
        }
        if (_messages)
            _messages->AddCallback (&observe_messages, this);
        if (_bytes)
            _bytes->AddCallback (&observe_bytes, this);
    }
    ~instance_spot_admission_metrics_t ()
    {
        if (_messages)
            _messages->RemoveCallback (&observe_messages, this);
        if (_bytes)
            _bytes->RemoveCallback (&observe_bytes, this);
    }
    bool enabled () const noexcept { return _messages || _bytes || _conflicts; }
    void retain (std::size_t bytes) noexcept
    {
        _pending_messages.fetch_add (1, std::memory_order_relaxed);
        _pending_bytes.fetch_add (bytes, std::memory_order_relaxed);
    }
    void release (std::size_t bytes) noexcept
    {
        _pending_messages.fetch_sub (1, std::memory_order_relaxed);
        _pending_bytes.fetch_sub (bytes, std::memory_order_relaxed);
    }
    void conflict (conflict_reason_t reason) const
    {
        if (_conflicts)
            _conflicts->Add (1, _reasons[static_cast<unsigned> (reason)]);
    }

  private:
    static void observe_value (opentelemetry::metrics::ObserverResult result,
                               const instance_spot_admission_metrics_t &metrics,
                               double value)
    {
        auto observer = opentelemetry::nostd::get<
          opentelemetry::nostd::shared_ptr<opentelemetry::metrics::ObserverResultT<double>>> (
          result);
        observer->Observe (value, metrics._tags);
    }
    static void observe_messages (opentelemetry::metrics::ObserverResult result, void *context)
    {
        const auto &metrics = *static_cast<instance_spot_admission_metrics_t *> (context);
        observe_value (result, metrics, metrics._pending_messages.load (std::memory_order_relaxed));
    }
    static void observe_bytes (opentelemetry::metrics::ObserverResult result, void *context)
    {
        const auto &metrics = *static_cast<instance_spot_admission_metrics_t *> (context);
        observe_value (result, metrics, metrics._pending_bytes.load (std::memory_order_relaxed));
    }
    using tags_t = std::map<std::string, std::string>;
    tags_t _tags;
    std::array<tags_t, 4> _reasons;
    std::atomic<std::uint64_t> _pending_messages{0}, _pending_bytes{0};
    opentelemetry::nostd::unique_ptr<opentelemetry::metrics::Counter<double>> _conflicts;
    opentelemetry::nostd::shared_ptr<opentelemetry::metrics::ObservableInstrument> _messages,
      _bytes;
};
} // namespace zlink::framework::runtime
