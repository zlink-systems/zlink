/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/configuration/logging.hpp>
#include <zlink/framework/contracts/locations/diagnostics.hpp>
#include <zlink/framework/contracts/spots/spot.hpp>
#include <zlink/framework/contracts/timers/timer.hpp>

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace zlink::framework::detail
{

enum class socket_event_kind_t
{
    connected,
    connection_ready,
    disconnected,
    handshake_failed,
    closed
};

enum class spot_event_kind_t
{
    timer_handler_failed,
    timer_stopped_after_unhandled_exception
};

enum class stream_event_kind_t
{
    connected,
    disconnected,
    transport_error,
    handler_exception
};

enum class actor_event_kind_t
{
    bound,
    unbound,
    relay_failed,
    session_disconnected
};

enum class metric_instrument_kind_t
{
    counter,
    updown,
    observable,
    histogram
};

enum class metric_temporality_t
{
    delta,
    current,
    sample
};

enum class drain_state_t
{
    serving,
    draining,
    stopped,
    force_stopping
};

struct socket_event_payload_t
{
    std::string source_name;
    socket_event_kind_t event = socket_event_kind_t::connected;
};

struct stream_event_payload_t
{
    std::string source_name;
    stream_event_kind_t event = stream_event_kind_t::connected;
    std::string stream_name;
    std::string session_id;
    std::string message;
};

struct actor_event_payload_t
{
    std::string source_name;
    actor_event_kind_t event = actor_event_kind_t::bound;
    std::string actor_type;
    std::string actor_id;
    std::string session_id;
    std::string message;
};

struct metric_event_payload_t
{
    std::string name;
    double value = 0;
    std::string unit;
    metric_instrument_kind_t instrument_kind = metric_instrument_kind_t::counter;
    metric_temporality_t temporality = metric_temporality_t::delta;
    std::map<std::string, std::string> tags;
};

struct drain_event_t
{
    drain_state_t state = drain_state_t::serving;
};

class monitoring_runtime_state_t
{
  public:
    logger_t<> diagnostics_logger;
};

class monitoring_runtime_t
{
  public:
    explicit monitoring_runtime_t (std::shared_ptr<monitoring_runtime_state_t> state);

    const std::shared_ptr<monitoring_runtime_state_t> &state () const noexcept { return _state; }

    void publish_socket (socket_event_payload_t event) const;
    void publish_location_snapshot (std::string source_name,
                                    location_runtime_status_t status,
                                    std::vector<location_topology_entry_t> topology,
                                    std::vector<location_service_summary_t> summary) const;
    void publish_location_changes (
      std::string source_name,
      location_runtime_status_t status,
      bool status_changed,
      std::optional<std::vector<location_topology_entry_t>> topology,
      std::optional<std::vector<location_service_summary_t>> summary) const;
    void publish_stream (stream_event_payload_t event) const;
    void publish_actor (actor_event_payload_t event) const;
    void publish_timer_failure (std::string source_name,
                                spot_id_t spot_id,
                                timer_failure_event_t failure) const;
    void publish_metric (metric_event_payload_t event) const;
    void publish_drain (drain_event_t event) const;

  private:
    void log (log_level_t level,
              std::string identifier,
              std::vector<log_field_t> fields) const noexcept;

    std::shared_ptr<monitoring_runtime_state_t> _state;
};

} // namespace zlink::framework::detail
