/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "monitoring_runtime.hpp"

#include <utility>

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

const char *metric_kind_name (metric_instrument_kind_t kind) noexcept
{
    switch (kind) {
        case metric_instrument_kind_t::counter:
            return "counter";
        case metric_instrument_kind_t::updown:
            return "updown";
        case metric_instrument_kind_t::observable:
            return "observable";
        case metric_instrument_kind_t::histogram:
            return "histogram";
    }
    return "unknown";
}

const char *metric_temporality_name (metric_temporality_t temporality) noexcept
{
    switch (temporality) {
        case metric_temporality_t::delta:
            return "delta";
        case metric_temporality_t::current:
            return "current";
        case metric_temporality_t::sample:
            return "sample";
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

monitoring_runtime_t::monitoring_runtime_t (
  std::shared_ptr<monitoring_runtime_state_t> state) :
    _state (std::move (state))
{
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

void monitoring_runtime_t::publish_timer_failure (
  std::string source_name,
  spot_id_t spot_id,
  timer_failure_event_t failure) const
{
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
    std::vector<log_field_t> fields{
      {"name", std::move (event.name)},
      {"value", std::to_string (event.value)},
      {"unit", std::move (event.unit)},
      {"instrument_kind", metric_kind_name (event.instrument_kind)},
      {"temporality", metric_temporality_name (event.temporality)}};
    fields.reserve (fields.size () + event.tags.size ());
    for (auto &[key, value] : event.tags) {
        fields.push_back ({std::move (key), std::move (value)});
    }
    log (log_level_t::debug, "zlink.runtime.metric.recorded", std::move (fields));
}

void monitoring_runtime_t::publish_drain (drain_event_t event) const
{
    log (log_level_t::info,
         "zlink.runtime.host.termination_changed",
         {{"state", drain_state_name (event.state)}});
}

} // namespace zlink::framework::detail
