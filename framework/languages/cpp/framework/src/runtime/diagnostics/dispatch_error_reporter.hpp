/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/dispatch/execution.hpp>
#include <zlink/framework/contracts/errors/error.hpp>

#include <zlink/Contracts/Sockets/results.hpp>
#include <service_wire_constants.hpp>

#include "runtime/diagnostics/diagnostic_event_sink.hpp"
#include "runtime/diagnostics/dispatch_diagnostics_names.hpp"
#include "runtime/diagnostics/message_flow_tracer.hpp"

#include <atomic>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace zlink::framework::detail
{

class dispatch_error_reporter_t
{
  public:
    explicit dispatch_error_reporter_t (const dispatch_options_t &options) : _options (&options) {}

    bool enabled () const noexcept
    {
        return dispatch_options_access_t::effective_message_flow (*_options)
                 != message_flow_log_mode_t::off
               && (dispatch_options_access_t::logger (*_options)
                   || dispatch_options_access_t::has_dispatch_error_observer (*_options));
    }

    void report (message_dispatch_error_event_t event) const noexcept
    {
        const auto effective_mode = dispatch_options_access_t::effective_message_flow (*_options);
        if (effective_mode == message_flow_log_mode_t::off) {
            return;
        }
        if (!dispatch_options_access_t::logger (*_options)
            && !dispatch_options_access_t::has_dispatch_error_observer (*_options))
            return;
        reported_count ().fetch_add (1, std::memory_order_relaxed);
        if (event.exception) {
            auto error = diagnostic_event_sink_t::exception_summary (event.exception);
            event.error_type = std::move (error.type);
            event.error_message = std::move (error.message);
        }
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
        if (dispatch_options_access_t::logger (*_options))
            log_default (event);
        try {
            dispatch_options_access_t::observe_dispatch_error (*_options, event);
        }
        catch (...) {
            (void) observer_failures ();
        }
    }

    template <typename Fn> void report_lazy (Fn &&build_event) const noexcept
    {
        if (dispatch_options_access_t::effective_message_flow (*_options)
              == message_flow_log_mode_t::off
            || (!dispatch_options_access_t::logger (*_options)
                && !dispatch_options_access_t::has_dispatch_error_observer (*_options)))
            return;
        try {
            report (std::forward<Fn> (build_event) ());
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
            // Structured fields through the configured framework logger.
            diagnostic_event_sink_t::log_if_configured (
              dispatch_options_access_t::logger (*_options), default_log_level (event),
              "dispatch error", std::move (fields));
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

    const dispatch_options_t *_options;
};

inline dispatch_error_reason_t dispatch_reason_from_error (framework_error_kind_t kind) noexcept
{
    switch (kind) {
        case framework_error_kind_t::not_found:
            return dispatch_error_reason_t::handler_missing;
        case framework_error_kind_t::protocol_error:
            return dispatch_error_reason_t::invalid_frame;
        case framework_error_kind_t::shutting_down:
            return dispatch_error_reason_t::shutdown;
        default:
            return dispatch_error_reason_t::handler_exception;
    }
}

inline dispatch_error_reason_t
dispatch_reason_from_submit_result (zlink::submit_result_t result) noexcept
{
    if (result == zlink::submit_result_t::terminated) {
        return dispatch_error_reason_t::shutdown;
    }
    if (result == zlink::submit_result_t::backpressured) {
        return dispatch_error_reason_t::backpressure;
    }
    return dispatch_error_reason_t::stale_target;
}

inline dispatch_error_reason_t
dispatch_reason_from_error (const framework_exception_t *error) noexcept
{
    if (error != nullptr
        && (detail::failure_code (*error)
              == static_cast<std::uint32_t> (
                runtime::protocol::framework_error_code::routeNotConnected)
            || detail::failure_code (*error)
                 == static_cast<std::uint32_t> (
                   runtime::protocol::framework_error_code::spotMoving))) {
        return dispatch_error_reason_t::stale_target;
    }
    if (error != nullptr
        && detail::failure_origin (*error) == detail::failure_origin_t::payload_decode) {
        return dispatch_error_reason_t::payload_decode_failed;
    }
    if (error != nullptr
        && detail::failure_origin (*error) == detail::failure_origin_t::stale_actor_slot) {
        return dispatch_error_reason_t::stale_target;
    }
    if (error != nullptr) {
        switch (detail::boundary_state (*error)) {
            case detail::boundary_error_t::shutdown:
                return dispatch_error_reason_t::shutdown;
            case detail::boundary_error_t::stale_generation:
            case detail::boundary_error_t::disconnected:
                return dispatch_error_reason_t::stale_target;
            default:
                break;
        }
    }
    return dispatch_reason_from_error (error != nullptr ? error->kind ()
                                                        : framework_error_kind_t::internal_failure);
}

inline message_flow_reason_t
message_flow_reason_from_error (const framework_exception_t *error) noexcept
{
    if (dispatch_reason_from_error (error) == dispatch_error_reason_t::shutdown)
        return message_flow_reason_t::shutdown;
    if (error && error->kind () == framework_error_kind_t::deadline_exceeded)
        return message_flow_reason_t::backpressure;
    return message_flow_reason_t::stale_target;
}

} // namespace zlink::framework::detail
