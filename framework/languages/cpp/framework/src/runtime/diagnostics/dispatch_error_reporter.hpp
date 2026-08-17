/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/dispatch/execution.hpp>
#include <zlink/framework/contracts/errors/error.hpp>

#include "runtime/diagnostics/diagnostic_event_sink.hpp"
#include "runtime/diagnostics/dispatch_diagnostics_names.hpp"
#include "runtime/diagnostics/message_flow_tracer.hpp"

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace zlink::framework::detail
{

class dispatch_error_reporter_t
{
  public:
    explicit dispatch_error_reporter_t (dispatch_options_t options) : _options (std::move (options))
    {
    }

    void report (message_dispatch_error_event_t event) const noexcept
    {
        const auto effective_mode =
          dispatch_options_access_t::effective_message_flow (_options);
        if (effective_mode == message_flow_log_mode_t::off) {
            return;
        }
        reported_count ().fetch_add (1, std::memory_order_relaxed);
        if (event.flow_id.has_value () != event.flow_origin.has_value ()) {
            event.flow_id.reset ();
            event.flow_origin.reset ();
        }
        if (!event.flow_id) {
            if (const auto &flow = runtime::flow_context_t::current ()) {
                if (!flow->flow_id.empty ()) {
                    event.flow_id = flow->flow_id;
                    event.flow_origin = flow->origin;
                }
            }
        }
        log_default (event);
        try {
            dispatch_options_access_t::observe_dispatch_error (_options, event);
        }
        catch (...) {
            (void) observer_failures ();
        }
    }

    static std::uint64_t reported () noexcept
    {
        return reported_count ().load (std::memory_order_relaxed);
    }

    static std::uint64_t observer_failures () noexcept
    {
        return message_flow_tracer_t::observer_failures ();
    }

    static std::uint64_t observer_dropped () noexcept
    {
        return message_flow_tracer_t::observer_dropped ();
    }

    /* channel 메시징 §3.1: drop 여부와 오류 분류는 별개다. application 코드가 던진 handler
     * 예외는 one-way라도 Error로 남기고, handler 없음·decode 실패·invalid frame은 send를
     * Warning, publish를 Debug로 낮춘다. request는 error reply로 끝나므로 Error를 유지한다. */
    static log_level_t default_log_level (const message_dispatch_error_event_t &event) noexcept
    {
        if (event.reason == dispatch_error_reason_t::handler_exception) {
            return log_level_t::error;
        }
        switch (event.message_kind) {
            case dispatch_message_kind_t::publish:
                return log_level_t::debug;
            case dispatch_message_kind_t::send:
            case dispatch_message_kind_t::actor_send:
                return log_level_t::warn;
            default:
                return log_level_t::error;
        }
    }

  private:
    void log_default (const message_dispatch_error_event_t &event) const noexcept
    {
        try {
            std::vector<log_field_t> fields;
            fields.reserve (11);
            auto add = [&fields] (const char *key, std::string value) {
                diagnostic_event_sink_t::append_field (fields, key, std::move (value));
            };
            add ("event", "zlink.dispatch_error");
            add ("outcome", "failed");
            add ("surface", std::string (enum_name (event.surface)));
            add ("kind", std::string (enum_name (event.message_kind)));
            add ("reason", std::string (enum_name (event.reason)));
            add ("action", std::string (enum_name (event.action)));
            if (event.packet_name) {
                add ("packet", *event.packet_name);
            }
            if (event.channel_name) {
                add ("channel", *event.channel_name);
            }
            if (event.channel_route_kind) {
                add ("channel_route", *event.channel_route_kind);
            } else if (event.surface == dispatch_error_surface_t::route_mesh_channel) {
                add ("channel_route", "route_mesh");
            } else if (event.surface == dispatch_error_surface_t::channel) {
                add ("channel_route", "client_server");
            }
            if (event.mesh_name) {
                add ("mesh", *event.mesh_name);
            }
            if (event.topic) {
                add ("topic", *event.topic);
            }
            if (event.correlation_id) {
                add ("corr", *event.correlation_id);
            }
            if (event.flow_id) {
                add ("flow", *event.flow_id);
            }
            if (event.flow_origin) {
                add ("origin", std::string (enum_name (*event.flow_origin)));
            }
            if (event.source_rid) {
                add ("source_rid", *event.source_rid);
            }
            if (event.target_rid) {
                add ("target_rid", *event.target_rid);
            }
            if (event.server_rid) {
                add ("server_rid", *event.server_rid);
            }
            if (event.spot_id) {
                add ("spot", *event.spot_id);
            }
            if (event.actor_id) {
                add ("actor", *event.actor_id);
            }
            if (event.instance_spot_type) {
                add ("instance_type", *event.instance_spot_type);
            }
            if (event.activation_state) {
                add ("activation_state", *event.activation_state);
            }
            // Structured fields through the framework logger (collector-friendly);
            // flat clog line when no logger is wired (tests, no-app usage).
            diagnostic_event_sink_t::log_or_clog (
              dispatch_options_access_t::logger (_options), default_log_level (event), "dispatch error",
              "zlink flow:", std::move (fields));
        }
        catch (...) {
            (void) observer_failures ();
        }
    }

    static std::atomic<std::uint64_t> &reported_count () noexcept
    {
        static std::atomic<std::uint64_t> value{0};
        return value;
    }

    dispatch_options_t _options;
};

inline dispatch_error_reason_t dispatch_reason_from_error (framework_error_kind_t kind) noexcept
{
    switch (kind) {
        case framework_error_kind_t::not_found:
            return dispatch_error_reason_t::handler_missing;
        case framework_error_kind_t::protocol_error:
            return dispatch_error_reason_t::invalid_frame;
        case framework_error_kind_t::capacity_exceeded:
            return dispatch_error_reason_t::backpressure;
        case framework_error_kind_t::shutting_down:
            return dispatch_error_reason_t::shutdown;
        default:
            return dispatch_error_reason_t::handler_exception;
    }
}

inline dispatch_error_reason_t
dispatch_reason_from_error (const framework_exception_t *error) noexcept
{
    if (error != nullptr
        && detail::failure_origin (*error) == detail::failure_origin_t::payload_decode) {
        return dispatch_error_reason_t::payload_decode_failed;
    }
    if (error != nullptr) {
        switch (detail::boundary_state (*error)) {
            case detail::boundary_error_t::shutdown:
                return dispatch_error_reason_t::shutdown;
            case detail::boundary_error_t::stale_generation:
                return dispatch_error_reason_t::stale_target;
            default:
                break;
        }
    }
    return dispatch_reason_from_error (
      error != nullptr ? error->kind () : framework_error_kind_t::internal_failure);
}

} // namespace zlink::framework::detail
