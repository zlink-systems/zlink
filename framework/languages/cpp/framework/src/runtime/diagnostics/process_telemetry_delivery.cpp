/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "runtime/diagnostics/process_telemetry_delivery.hpp"
#include "runtime/diagnostics/dispatch_error_reporter.hpp"
#include "runtime/dispatch/offload_executor.hpp"

#include <array>
#include <cstdio>
#include <list>
#include <mutex>
#include <unordered_set>

namespace zlink::framework::detail
{
bool process_telemetry_has_provider (const dispatch_options_t &options,
                                     telemetry_event_kind_t kind) noexcept
{
    return dispatch_options_access_t::logger (options).has_value ()
           || (kind != telemetry_event_kind_t::dispatch_error
               && dispatch_options_access_t::has_observer (options))
           || (kind != telemetry_event_kind_t::message_flow
               && dispatch_options_access_t::has_dispatch_error_observer (options));
}

namespace
{
std::atomic<std::uint64_t> dropped_count{0};
enum class provider_role_t
{
    logger,
    trace,
    fallback,
    count
};
struct overflow_instrument_t
{
    runtime::metric_provider_t provider;
    opentelemetry::nostd::shared_ptr<opentelemetry::metrics::Meter> meter;
    opentelemetry::nostd::unique_ptr<opentelemetry::metrics::Counter<double>> counter;
};
class process_telemetry_delivery_t;
process_telemetry_delivery_t &delivery ();
void logger_failure () noexcept;
static const char *terminal_outcome_name (message_flow_outcome_t outcome) noexcept
{
    switch (outcome) {
        case message_flow_outcome_t::dropped:
            return "dropped";
        case message_flow_outcome_t::backpressured:
            return "backpressured";
        default:
            return "succeeded";
    }
}

void log_flow (const telemetry_record_t &record)
{
    const auto &event = std::get<message_flow_event_t> (record.event);
    const auto effective_mode = record.mode;
    {
        // Preserve the existing structured field projection on the delivery worker.
        std::vector<log_field_t> fields;
        fields.reserve (16);
        auto add = [&fields] (const char *key, std::string value) {
            diagnostic_event_sink_t::append_field (fields, key, std::move (value));
        };
        add (dispatch_event_field::event_id, "zlink.message_flow");
        add (dispatch_event_field::phase, std::string (enum_name (event.outcome)));
        add (dispatch_event_field::outcome, event.result ? std::string (enum_name (*event.result))
                                                         : terminal_outcome_name (event.outcome));
        add (dispatch_event_field::surface, std::string (enum_name (event.surface)));
        add (dispatch_event_field::kind, std::string (enum_name (event.message_kind)));
        if (event.packet_name) {
            add (dispatch_event_field::packet, *event.packet_name);
        }
        if (event.channel_name) {
            add (dispatch_event_field::channel, *event.channel_name);
        }
        if (event.channel_route_kind) {
            add (dispatch_event_field::channel_route, *event.channel_route_kind);
        } else if (const auto route_name = default_channel_route_name (event.surface)) {
            add (dispatch_event_field::channel_route, std::string (*route_name));
        }
        if (event.mesh_name) {
            add (dispatch_event_field::mesh, *event.mesh_name);
        }
        if (event.topic) {
            add (dispatch_event_field::topic, *event.topic);
        }
        if (event.correlation_id) {
            add (dispatch_event_field::corr, *event.correlation_id);
        }
        if (event.flow_id) {
            add (dispatch_event_field::flow, *event.flow_id);
        }
        if (event.flow_origin) {
            add (dispatch_event_field::origin, std::string (enum_name (*event.flow_origin)));
        }
        if (event.source_rid) {
            add (dispatch_event_field::source_rid, *event.source_rid);
        }
        if (event.target_rid) {
            add (dispatch_event_field::target_rid, *event.target_rid);
        }
        if (event.server_rid) {
            add (dispatch_event_field::server_rid, *event.server_rid);
        }
        if (event.spot_id) {
            add (dispatch_event_field::spot, *event.spot_id);
        }
        if (event.actor_id) {
            add (dispatch_event_field::actor, *event.actor_id);
        }
        if (event.stream_session_id) {
            add (dispatch_event_field::session, *event.stream_session_id);
        }
        if (event.instance_spot_type) {
            add (dispatch_event_field::instance_type, *event.instance_spot_type);
        }
        if (event.activation_state) {
            add (dispatch_event_field::activation_state, *event.activation_state);
        }
        if (event.reason) {
            add (dispatch_event_field::reason, std::string (enum_name (*event.reason)));
        } else if (event.error_reason) {
            add (dispatch_event_field::reason, std::string (enum_name (*event.error_reason)));
        }
        if (event.message_size && effective_mode == message_flow_log_mode_t::detailed
            && record.include_sizes) {
            add (dispatch_event_field::size, std::to_string (*event.message_size));
        }
        if (effective_mode == message_flow_log_mode_t::detailed) {
            if (event.detail_stage)
                add (dispatch_event_field::stage, *event.detail_stage);
            if (event.detail_result)
                add (dispatch_event_field::result, *event.detail_result);
        }
        if (record.error) {
            auto error = *record.error;
            add (dispatch_event_field::error_type, std::move (error.type));
            add (dispatch_event_field::error_message, std::move (error.message));
        }
        // Emit structured fields only through an explicitly configured
        // framework logger; observer-only and no-sink paths stay silent.
        diagnostic_event_sink_t::log_if_configured (
          record.logger, log_level_t::info, "message flow", std::move (fields), logger_failure);
    }
}
void log_error (const telemetry_record_t &record)
{
    const auto &event = std::get<message_dispatch_error_event_t> (record.event);
    {
        std::vector<log_field_t> fields;
        fields.reserve (11);
        auto add = [&fields] (const char *key, std::string value) {
            diagnostic_event_sink_t::append_field (fields, key, std::move (value));
        };
        add (dispatch_event_field::event_id, "zlink.dispatch_error");
        add (dispatch_event_field::outcome, "failed");
        add (dispatch_event_field::surface, std::string (enum_name (event.surface)));
        add (dispatch_event_field::kind, std::string (enum_name (event.message_kind)));
        add (dispatch_event_field::reason, std::string (enum_name (event.reason)));
        add (dispatch_event_field::action, std::string (enum_name (event.action)));
        if (event.packet_name) {
            add (dispatch_event_field::packet, *event.packet_name);
        }
        if (event.channel_name) {
            add (dispatch_event_field::channel, *event.channel_name);
        }
        if (event.channel_route_kind) {
            add (dispatch_event_field::channel_route, *event.channel_route_kind);
        } else if (const auto route_name = default_channel_route_name (event.surface)) {
            add (dispatch_event_field::channel_route, std::string (*route_name));
        }
        if (event.mesh_name) {
            add (dispatch_event_field::mesh, *event.mesh_name);
        }
        if (event.topic) {
            add (dispatch_event_field::topic, *event.topic);
        }
        if (event.correlation_id) {
            add (dispatch_event_field::corr, *event.correlation_id);
        }
        if (event.flow_id) {
            add (dispatch_event_field::flow, *event.flow_id);
        }
        if (event.flow_origin) {
            add (dispatch_event_field::origin, std::string (enum_name (*event.flow_origin)));
        }
        if (event.source_rid) {
            add (dispatch_event_field::source_rid, *event.source_rid);
        }
        if (event.target_rid) {
            add (dispatch_event_field::target_rid, *event.target_rid);
        }
        if (event.server_rid) {
            add (dispatch_event_field::server_rid, *event.server_rid);
        }
        if (event.spot_id) {
            add (dispatch_event_field::spot, *event.spot_id);
        }
        if (event.actor_id) {
            add (dispatch_event_field::actor, *event.actor_id);
        }
        if (event.instance_spot_type) {
            add (dispatch_event_field::instance_type, *event.instance_spot_type);
        }
        if (event.activation_state) {
            add (dispatch_event_field::activation_state, *event.activation_state);
        }
        if (event.error_type) {
            add (dispatch_event_field::error_type, *event.error_type);
        }
        if (event.error_message) {
            add (dispatch_event_field::error_message, *event.error_message);
        }
        diagnostic_event_sink_t::log_if_configured (
          record.logger, dispatch_error_reporter_t::default_log_level (event), "dispatch error",
          std::move (fields), logger_failure);
    }
}

class process_telemetry_delivery_t
{
  public:
    std::shared_ptr<telemetry_registration_t> register_host ()
    {
        std::shared_ptr<telemetry_registration_t> registration;
        {
            std::lock_guard lock (_mutex);
            registration = std::make_shared<telemetry_registration_t> (++_next_registration);
            _registrations.insert (registration->id);
        }
        return registration;
    }

    void bind_metrics (std::uint64_t registration,
                       runtime::metric_provider_t provider,
                       opentelemetry::nostd::shared_ptr<opentelemetry::metrics::Meter> meter)
    {
        if (!meter)
            return;
        auto instrument = std::make_shared<overflow_instrument_t> ();
        instrument->provider = std::move (provider);
        instrument->meter = std::move (meter);
        {
            std::lock_guard lock (_mutex);
            if (_registrations.contains (registration))
                _overflow.swap (instrument);
        }
    }

    void retire (std::uint64_t registration) noexcept
    {
        std::list<telemetry_record_t> retired;
        {
            std::lock_guard lock (_mutex);
            _registrations.erase (registration);
            for (auto at = _pending.begin (); at != _pending.end ();) {
                auto current = at++;
                if (current->registration == registration)
                    retired.splice (retired.end (), _pending, current);
            }
        }
        // Provider closures may run application destructors; release outside the lock.
    }

    void submit (telemetry_record_t record) noexcept
    {
        std::list<telemetry_record_t> unscheduled;
        try {
            std::lock_guard lock (_mutex);
            if (!_registrations.contains (record.registration))
                return;
            if (_pending.size () == pending_capacity) {
                dropped_count.fetch_add (1, std::memory_order_relaxed);
                return;
            }
            _pending.push_back (std::move (record));
            if (!_scheduled) {
                _scheduled = true;
                try {
                    if (_executor.try_submit_internal ([this] { drain (); }))
                        return;
                }
                catch (...) {
                    // Scheduling failure is counted below; never invoke a provider here.
                }
                _scheduled = false;
                unscheduled.splice (unscheduled.end (), _pending);
                telemetry_failure_count ().fetch_add (1, std::memory_order_relaxed);
            }
        }
        catch (...) {
            telemetry_failure_count ().fetch_add (1, std::memory_order_relaxed);
        }
    }

    void failure (provider_role_t role) noexcept
    {
        telemetry_failure_count ().fetch_add (1, std::memory_order_relaxed);
        const auto index = static_cast<std::size_t> (role);
        if (_failed[index])
            return;
        _failed[index] = true;
        if (role == provider_role_t::fallback
            || _failed[static_cast<std::size_t> (provider_role_t::fallback)])
            return;
        const auto message = role == provider_role_t::logger
                               ? "zlink telemetry logger provider failed\n"
                               : "zlink telemetry trace provider failed\n";
        if (std::fputs (message, stderr) == EOF || std::fflush (stderr) == EOF)
            failure (provider_role_t::fallback);
    }

  private:
    void flush_overflow () noexcept
    {
        std::shared_ptr<overflow_instrument_t> instrument;
        {
            std::lock_guard lock (_mutex);
            instrument = _overflow;
        }
        if (!instrument)
            return;
        const auto total = dropped_count.load (std::memory_order_relaxed);
        const auto delta = total - _exported_overflow;
        if (delta == 0)
            return;
        _exported_overflow = total;
        try {
            if (!instrument->counter)
                instrument->counter = instrument->meter->CreateDoubleCounter (
                  "zlink.observability.events.overflow", "", "{event}");
            instrument->counter->Add (static_cast<double> (delta), {{"source", "message_flow"}});
        }
        catch (...) {
            failure (provider_role_t::trace);
        }
    }

    void drain () noexcept
    {
        for (;;) {
            std::list<telemetry_record_t> current;
            {
                std::lock_guard lock (_mutex);
                if (_pending.empty ())
                    _scheduled = false;
                else
                    current.splice (current.end (), _pending, _pending.begin ());
            }
            if (current.empty ()) {
                // Retirement can remove every waiting record before the worker takes one.
                flush_overflow ();
                return;
            }
            const auto &record = current.front ();
            if (record.logger) {
                try {
                    if (std::holds_alternative<message_flow_event_t> (record.event))
                        log_flow (record);
                    else
                        log_error (record);
                }
                catch (...) {
                    failure (provider_role_t::logger);
                }
            }
            if (record.observer) {
                try {
                    std::visit ([&] (const auto &event) { record.observer (&event); },
                                record.event);
                }
                catch (...) {
                    failure (provider_role_t::trace);
                }
            }
            flush_overflow ();
        }
    }

    // Spec 26 §6 counts waiting records, excluding the one taken by drain().
    static constexpr std::size_t pending_capacity = 8192;
    std::mutex _mutex;
    std::unordered_set<std::uint64_t> _registrations;
    std::uint64_t _next_registration = 0;
    std::list<telemetry_record_t> _pending;
    bool _scheduled = false;
    std::shared_ptr<overflow_instrument_t> _overflow;
    std::uint64_t _exported_overflow = 0;
    std::array<bool, static_cast<std::size_t> (provider_role_t::count)> _failed{};
    // The process-lifetime owner keeps executor state alive until process exit.
    runtime::offload_executor_t _executor{1, "zlink-telemetry"};
};

process_telemetry_delivery_t &delivery ()
{
    // Never destroy the owner: static hosts may retire late, and providers may remain blocked.
    static process_telemetry_delivery_t *owner = new process_telemetry_delivery_t;
    return *owner;
}
void logger_failure () noexcept
{
    delivery ().failure (provider_role_t::logger);
}
} // namespace

std::shared_ptr<telemetry_registration_t> register_process_telemetry ()
{
    return delivery ().register_host ();
}
void bind_process_telemetry_metrics (
  std::uint64_t registration,
  runtime::metric_provider_t provider,
  opentelemetry::nostd::shared_ptr<opentelemetry::metrics::Meter> meter)
{
    delivery ().bind_metrics (registration, std::move (provider), std::move (meter));
}
telemetry_registration_t::~telemetry_registration_t ()
{
    retire_process_telemetry (id);
}
void retire_process_telemetry (std::uint64_t registration) noexcept
{
    delivery ().retire (registration);
}
void submit_process_telemetry (telemetry_record_t record) noexcept
{
    delivery ().submit (std::move (record));
}
std::atomic<std::uint64_t> &telemetry_traced_count () noexcept
{
    static std::atomic<std::uint64_t> value{0};
    return value;
}
std::atomic<std::uint64_t> &telemetry_reported_count () noexcept
{
    static std::atomic<std::uint64_t> value{0};
    return value;
}
std::atomic<std::uint64_t> &telemetry_failure_count () noexcept
{
    static std::atomic<std::uint64_t> value{0};
    return value;
}
std::uint64_t telemetry_dropped () noexcept
{
    return dropped_count.load (std::memory_order_relaxed);
}
} // namespace zlink::framework::detail
