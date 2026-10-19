/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once
#include "runtime/diagnostics/mesh_request_metrics.hpp"
#include <atomic>
#include <array>
#include <map>

namespace zlink::framework::runtime
{
class instance_spot_metrics_t
{
  public:
    enum class outcome_t
    {
        ready,
        rejected,
        conflict,
        timed_out,
        shutdown,
        store_failure,
        fenced
    };
    enum class conflict_reason_t
    {
        authority,
        spot_kind,
        spot_type,
        closing
    };
    instance_spot_metrics_t (std::string mesh, std::string type) :
        _tags{{"mesh_name", std::move (mesh)}, {"instance_spot_type", std::move (type)}}
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
        _conflicts =
          meter->CreateDoubleCounter ("zlink.instance_spot.claim.conflicts", "", "{claim}");
        _messages = meter->CreateDoubleObservableGauge ("zlink.instance_spot.pending.messages", "",
                                                        "{message}");
        _bytes = meter->CreateDoubleObservableGauge ("zlink.instance_spot.pending.bytes", "", "By");
        constexpr const char *outcomes[] = {"ready",    "rejected",      "conflict", "timed_out",
                                            "shutdown", "store_failure", "fenced"};
        for (unsigned i = 0; i != _outcomes.size (); ++i) {
            _outcomes[i] = _tags;
            _outcomes[i].emplace ("outcome", outcomes[i]);
        }
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
    ~instance_spot_metrics_t ()
    {
        if (_messages)
            _messages->RemoveCallback (&observe_messages, this);
        if (_bytes)
            _bytes->RemoveCallback (&observe_bytes, this);
    }
    bool enabled () const noexcept { return _activations && _duration; }
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
    void terminal (std::chrono::steady_clock::time_point started, outcome_t outcome) const
    {
        if (!enabled ())
            return;
        const auto &tags = _outcomes[static_cast<unsigned> (outcome)];
        _activations->Add (1, tags);
        _duration->Record (
          std::chrono::duration<double> (std::chrono::steady_clock::now () - started).count (),
          tags, opentelemetry::context::Context{});
    }
    void conflict (conflict_reason_t reason) const
    {
        if (_conflicts)
            _conflicts->Add (1, _reasons[static_cast<unsigned> (reason)]);
    }
    static outcome_t outcome (framework_error_kind_t kind) noexcept
    {
        switch (kind) {
            case framework_error_kind_t::deadline_exceeded:
                return outcome_t::timed_out;
            case framework_error_kind_t::shutting_down:
                return outcome_t::shutdown;
            case framework_error_kind_t::already_exists:
            case framework_error_kind_t::type_mismatch:
                return outcome_t::conflict;
            case framework_error_kind_t::invalid_operation:
                return outcome_t::fenced;
            default:
                return outcome_t::rejected;
        }
    }

  private:
    static void observe_value (opentelemetry::metrics::ObserverResult result,
                               const instance_spot_metrics_t &metrics,
                               double value)
    {
        auto observer = opentelemetry::nostd::get<
          opentelemetry::nostd::shared_ptr<opentelemetry::metrics::ObserverResultT<double>>> (
          result);
        observer->Observe (value, metrics._tags);
    }
    static void observe_messages (opentelemetry::metrics::ObserverResult result, void *context)
    {
        const auto &metrics = *static_cast<instance_spot_metrics_t *> (context);
        observe_value (result, metrics, metrics._pending_messages.load (std::memory_order_relaxed));
    }
    static void observe_bytes (opentelemetry::metrics::ObserverResult result, void *context)
    {
        const auto &metrics = *static_cast<instance_spot_metrics_t *> (context);
        observe_value (result, metrics, metrics._pending_bytes.load (std::memory_order_relaxed));
    }
    using tags_t = std::map<std::string, std::string>;
    tags_t _tags;
    std::array<tags_t, 7> _outcomes;
    std::array<tags_t, 4> _reasons;
    std::atomic<std::uint64_t> _pending_messages{0}, _pending_bytes{0};
    opentelemetry::nostd::unique_ptr<opentelemetry::metrics::Counter<double>> _activations,
      _conflicts;
    opentelemetry::nostd::unique_ptr<opentelemetry::metrics::Histogram<double>> _duration;
    opentelemetry::nostd::shared_ptr<opentelemetry::metrics::ObservableInstrument> _messages,
      _bytes;
};
} // namespace zlink::framework::runtime
