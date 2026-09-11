/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "monitoring_runtime.hpp"

#include <opentelemetry/metrics/provider.h>
#include <opentelemetry/metrics/sync_instruments.h>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace
{

bool blank_monitoring_source (const std::string &value)
{
    return value.empty ()
           || std::all_of (
             value.begin (), value.end (), [] (char ch) {
                 return ch == ' ' || ch == '\t'
                        || ch == '\r' || ch == '\n';
             });
}

} // namespace

namespace zlink::framework
{

monitoring_builder_t::monitoring_builder_t () :
    _state (std::make_shared<detail::monitoring_runtime_state_t> ())
{
}

monitoring_builder_t::monitoring_builder_t (
  std::shared_ptr<detail::monitoring_runtime_state_t> state) :
    _state (std::move (state))
{
}

monitoring_builder_t::~monitoring_builder_t () = default;
monitoring_builder_t::monitoring_builder_t (
  monitoring_builder_t &&) noexcept = default;
monitoring_builder_t &monitoring_builder_t::operator= (
  monitoring_builder_t &&) noexcept = default;

monitoring_builder_t &monitoring_builder_t::add_spot_events (
  std::string source_name)
{
    if (blank_monitoring_source (source_name))
        throw std::invalid_argument (
          "Spot monitoring source name must not be empty");
    _state->lane.run ([&] {
    if (std::find (_state->spot_sources.begin (),
                   _state->spot_sources.end (), source_name)
        != _state->spot_sources.end ())
        throw std::invalid_argument (
          "Spot monitoring source is already registered");
    _state->spot_sources.push_back (std::move (source_name));
    }).get ();
    return *this;
}

monitoring_builder_t &monitoring_builder_t::on_spot_event (
  spot_event_handler_t handler)
{
    if (!handler)
        throw std::invalid_argument (
          "Spot monitoring handler is required");
    _state->lane.run ([&] {
    _state->spot_handlers.push_back (std::move (handler));
    }).get ();
    return *this;
}

} // namespace zlink::framework

namespace zlink::framework::detail
{
namespace
{

const char *socket_event_name (socket_event_kind_t event) noexcept
{
    switch (event) {
        case socket_event_kind_t::connected:
            return "connected";
        case socket_event_kind_t::connection_ready:
            return "ready";
        case socket_event_kind_t::disconnected:
            return "disconnected";
        case socket_event_kind_t::handshake_failed:
            return "handshake_failed";
        case socket_event_kind_t::closed:
            return "closed";
    }
    return "unknown";
}

const char *stream_event_name (stream_event_kind_t event) noexcept
{
    switch (event) {
        case stream_event_kind_t::connected:
            return "connected";
        case stream_event_kind_t::disconnected:
            return "disconnected";
        case stream_event_kind_t::transport_error:
            return "transport_error";
        case stream_event_kind_t::handler_exception:
            return "handler_exception";
    }
    return "unknown";
}

const char *actor_event_name (actor_event_kind_t event) noexcept
{
    switch (event) {
        case actor_event_kind_t::bound:
            return "bound";
        case actor_event_kind_t::unbound:
            return "unbound";
        case actor_event_kind_t::relay_failed:
            return "relay_failed";
        case actor_event_kind_t::session_disconnected:
            return "session_disconnected";
    }
    return "unknown";
}

const char *drain_state_name (drain_state_t state) noexcept
{
    switch (state) {
        case drain_state_t::serving:
            return "serving";
        case drain_state_t::draining:
            return "draining";
        case drain_state_t::stopped:
            return "stopped";
        case drain_state_t::force_stopping:
            return "force_stopping";
    }
    return "unknown";
}

} // namespace

// A no-op provider has no collection path. Real providers own reader/view
// selection; the Framework does not infer it from log sinks or log levels.
monitoring_runtime_state_t::monitoring_runtime_state_t ()
{
    const auto provider = opentelemetry::metrics::Provider::GetMeterProvider ();
    if (dynamic_cast<opentelemetry::metrics::NoopMeterProvider *> (provider.get ()) == nullptr)
        metric_meter = provider->GetMeter ("zlink.framework");
}

class metric_instrument_t
{
  public:
    metric_instrument_t (opentelemetry::metrics::Meter &meter,
                         const metric_event_payload_t &event) :
        _kind (event.instrument_kind)
    {
        switch (_kind) {
            case metric_instrument_kind_t::counter:
                _counter = meter.CreateDoubleCounter (event.name, "", event.unit);
                break;
            case metric_instrument_kind_t::updown:
                _updown = meter.CreateDoubleUpDownCounter (event.name, "", event.unit);
                break;
            case metric_instrument_kind_t::histogram:
                _histogram = meter.CreateDoubleHistogram (event.name, "", event.unit);
                break;
            case metric_instrument_kind_t::observable:
                _observable = meter.CreateDoubleObservableGauge (event.name, "", event.unit);
                _observable->AddCallback (&observe, this);
                break;
        }
    }

    ~metric_instrument_t ()
    {
        if (_observable)
            _observable->RemoveCallback (&observe, this);
    }

    void record (const metric_event_payload_t &event)
    {
        switch (_kind) {
            case metric_instrument_kind_t::counter:
                _counter->Add (event.value, event.tags);
                break;
            case metric_instrument_kind_t::updown:
                _updown->Add (event.value, event.tags);
                break;
            case metric_instrument_kind_t::histogram:
                _histogram->Record (event.value, event.tags, opentelemetry::context::Context{});
                break;
            case metric_instrument_kind_t::observable: {
                // ABI 1 exposes asynchronous gauges. Retain only the latest
                // value for each existing bounded label set, never samples.
                std::lock_guard lock (_mutex);
                _current[event.tags] = event.value;
                break;
            }
        }
    }

  private:
    static void observe (opentelemetry::metrics::ObserverResult result, void *state)
    {
        auto &instrument = *static_cast<metric_instrument_t *> (state);
        auto observer = opentelemetry::nostd::get<
          opentelemetry::nostd::shared_ptr<opentelemetry::metrics::ObserverResultT<double>>> (
          result);
        std::lock_guard lock (instrument._mutex);
        for (const auto &[tags, value] : instrument._current)
            observer->Observe (value, tags);
    }

    metric_instrument_kind_t _kind;
    opentelemetry::nostd::unique_ptr<opentelemetry::metrics::Counter<double>> _counter;
    opentelemetry::nostd::unique_ptr<opentelemetry::metrics::UpDownCounter<double>> _updown;
    opentelemetry::nostd::unique_ptr<opentelemetry::metrics::Histogram<double>> _histogram;
    opentelemetry::nostd::shared_ptr<opentelemetry::metrics::ObservableInstrument> _observable;
    std::mutex _mutex;
    std::map<std::map<std::string, std::string>, double> _current;
};

monitoring_runtime_t::monitoring_runtime_t (
  std::shared_ptr<monitoring_runtime_state_t> state) :
    _state (std::move (state))
{
}

monitoring_runtime_t monitoring_runtime_t::from (
  const monitoring_builder_t &builder)
{
    return monitoring_runtime_t (builder._state);
}

void monitoring_runtime_t::log (log_level_t level,
                                std::string identifier,
                                std::vector<log_field_t> fields) const noexcept
{
    if (!_state || !_state->diagnostics_logger.is_enabled (level)) {
        return;
    }
    try {
        _state->diagnostics_logger.log_with_fields (
          level, std::move (identifier), std::move (fields));
    }
    catch (...) {
        // Diagnostics must never change runtime behavior.
    }
}

void monitoring_runtime_t::publish_socket (socket_event_payload_t event) const
{
    log (log_level_t::debug,
         "zlink.runtime.transport.connection_changed",
         {{"source_name", std::move (event.source_name)},
          {"state", socket_event_name (event.event)}});
}

void monitoring_runtime_t::publish_location_snapshot (
  std::string source_name,
  location_runtime_status_t status,
  std::vector<location_topology_entry_t> topology,
  std::vector<location_service_summary_t> summary) const
{
    publish_location_changes (std::move (source_name), std::move (status), true,
                              std::move (topology), std::move (summary));
}

void monitoring_runtime_t::publish_location_changes (
  std::string source_name,
  location_runtime_status_t status,
  bool status_changed,
  std::optional<std::vector<location_topology_entry_t>> topology,
  std::optional<std::vector<location_service_summary_t>> summary) const
{
    if (status_changed) {
        log (status.store_healthy ? log_level_t::info : log_level_t::warn,
             "zlink.runtime.location.store_changed",
             {{"source_name", source_name},
              {"state", status.store_healthy ? "ready" : "degraded"}});
    }
    if (topology) {
        log (log_level_t::debug,
             "zlink.runtime.mesh_node.peer_changed",
             {{"source_name", source_name},
              {"entry_count", std::to_string (topology->size ())}});
    }
    if (summary) {
        log (log_level_t::debug,
             "zlink.runtime.mesh_node.state_changed",
             {{"source_name", std::move (source_name)},
              {"summary_count", std::to_string (summary->size ())}});
    }
}

void monitoring_runtime_t::publish_stream (stream_event_payload_t event) const
{
    log (event.event == stream_event_kind_t::transport_error
             || event.event == stream_event_kind_t::handler_exception
           ? log_level_t::warn
           : log_level_t::debug,
         "zlink.runtime.stream.state_changed",
         {{"source_name", std::move (event.source_name)},
          {"stream_name", std::move (event.stream_name)},
          {"session_id", std::move (event.session_id)},
          {"state", stream_event_name (event.event)},
          {"message", std::move (event.message)}});
}

void monitoring_runtime_t::publish_actor (actor_event_payload_t event) const
{
    log (event.event == actor_event_kind_t::relay_failed
           ? log_level_t::warn
           : log_level_t::debug,
         "zlink.runtime.actor.session_changed",
         {{"source_name", std::move (event.source_name)},
          {"actor_type", std::move (event.actor_type)},
          {"actor_id", std::move (event.actor_id)},
          {"session_id", std::move (event.session_id)},
          {"state", actor_event_name (event.event)},
          {"message", std::move (event.message)}});
}

void monitoring_runtime_t::publish_application_job_queue_failure () const
{
    log (log_level_t::error,
         "zlink.runtime.host.application_job_queue.receive_flow_config_failed",
         {{"category", "receive_flow_state_configuration"},
          {"message",
           "Failed to apply the absolute Application Job Queue receive-flow state"}});
}

void monitoring_runtime_t::publish_timer_failure (
  std::string source_name,
  spot_id_t spot_id,
  timer_failure_event_t failure) const
{
    std::vector<spot_event_handler_t> handlers;
    if (_state) {
        _state->lane.run ([&] {
        if (std::find (_state->spot_sources.begin (),
                       _state->spot_sources.end (), source_name)
            != _state->spot_sources.end ())
            handlers = _state->spot_handlers;
        }).get ();
    }
    if (!handlers.empty ()) {
        const spot_event_t event{
          source_name,
          std::chrono::system_clock::now (),
          failure.stopped
            ? spot_event_kind_t::
                timer_stopped_after_unhandled_exception
            : spot_event_kind_t::timer_handler_failed,
          {spot_id, failure.timer_name,
           failure.handler_type.name (),
           failure.delivery_index, failure.message}};
        for (const auto &handler : handlers) {
            try {
                handler (event);
            }
            catch (...) {
                // Monitoring callbacks cannot affect timer delivery.
            }
        }
    }
    log (log_level_t::error,
         "zlink.runtime.spot.timer_failed",
         {{"source_name", std::move (source_name)},
          {"spot_id", std::string (spot_id)},
          {"timer_name", std::move (failure.timer_name)},
          {"handler_type", failure.handler_type.name ()},
          {"delivery_index", std::to_string (failure.delivery_index)},
          {"stopped", failure.stopped ? "true" : "false"},
          {"message", std::move (failure.message)}});
}

void monitoring_runtime_t::publish_metric (metric_event_payload_t event) const
{
    if (!_state || !_state->metric_meter)
        return;
    std::shared_ptr<metric_instrument_t> instrument;
    {
        std::lock_guard lock (_state->metric_mutex);
        auto &registered = _state->metric_instruments[event.name];
        if (!registered)
            registered = std::make_shared<metric_instrument_t> (*_state->metric_meter, event);
        instrument = registered;
    }
    instrument->record (event);
}

void monitoring_runtime_t::publish_drain (drain_event_t event) const
{
    log (log_level_t::info,
         "zlink.runtime.host.termination_changed",
         {{"state", drain_state_name (event.state)}});
}

} // namespace zlink::framework::detail
