/* SPDX-License-Identifier: FSL-1.1-ALv2 */

// Verifies the message-flow tracing feature: dispatch_diagnostics_options_t::
// message_flow now actually gates a structured, correlation-id-keyed log stream
// (success transitions via message_flow_tracer_t) and silences the error log in
// the off mode (dispatch_error_reporter_t). The tracer's default sink is
// std::clog, so the tests capture that buffer and assert on the emitted lines.

#include "runtime/diagnostics/dispatch_error_reporter.hpp"
#include "runtime/diagnostics/dispatch_diagnostics_names.hpp"
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/diagnostics/message_flow_tracer.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

namespace
{

using namespace zlink::framework;
using zlink::framework::detail::dispatch_error_reporter_t;
using zlink::framework::detail::message_flow_tracer_t;

// Redirects std::clog for the duration of a block and returns what was written.
template <typename Fn> std::string capture_clog (Fn &&fn)
{
    std::ostringstream captured;
    auto *previous = std::clog.rdbuf (captured.rdbuf ());
    std::forward<Fn> (fn) ();
    std::clog.rdbuf (previous);
    return captured.str ();
}

dispatch_options_t options_with_mode (message_flow_log_mode_t mode)
{
    dispatch_options_t options;
    options.message_flow (mode);
    return options;
}

message_flow_event_t flow_event (message_flow_outcome_t outcome)
{
    return message_flow_event_t{outcome,
                                dispatch_error_surface_t::channel,
                                dispatch_message_kind_t::request,
                                std::string ("PlaceOrder"),
                                std::string ("orders"),
                                std::nullopt,
                                std::string ("corr-123"),
                                std::nullopt,
                                std::nullopt,
                                std::nullopt,
                                std::optional<std::size_t> (42)};
}

message_dispatch_error_event_t error_event ()
{
    return message_dispatch_error_event_t{dispatch_error_surface_t::channel,
                                          dispatch_message_kind_t::request,
                                          dispatch_error_reason_t::handler_missing,
                                          dispatch_error_action_t::reply_error,
                                          std::string ("PlaceOrder"),
                                          std::string ("orders"),
                                          std::nullopt,
                                          std::nullopt,
                                          std::nullopt,
                                          std::nullopt,
                                          std::string ("corr-123"),
                                          nullptr};
}

bool contains (const std::string &haystack, const std::string &needle)
{
    return haystack.find (needle) != std::string::npos;
}

std::size_t occurrences (const std::string &haystack, const std::string &needle)
{
    std::size_t count = 0;
    for (std::size_t at = 0; (at = haystack.find (needle, at)) != std::string::npos;
         at += needle.size ()) {
        ++count;
    }
    return count;
}

bool wait_until (const std::function<bool ()> &predicate)
{
    for (int attempt = 0; attempt < 50; ++attempt) {
        if (predicate ()) {
            return true;
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (10));
    }
    return predicate ();
}

} // namespace

int main ()
{
    // off silences every success transition, including drops.
    {
        const auto out = capture_clog ([] {
            const auto opts = options_with_mode (message_flow_log_mode_t::off);
            message_flow_tracer_t tracer (opts);
            tracer.trace (flow_event (message_flow_outcome_t::received));
            tracer.trace (flow_event (message_flow_outcome_t::dropped));
        });
        if (!out.empty ()) {
            return 1;
        }
    }

    // errors emits the drop decision but not healthy transitions.
    {
        const auto out = capture_clog ([] {
            const auto opts = options_with_mode (message_flow_log_mode_t::errors);
            message_flow_tracer_t tracer (opts);
            tracer.trace (flow_event (message_flow_outcome_t::received));
            tracer.trace (flow_event (message_flow_outcome_t::dropped));
        });
        if (contains (out, "phase=received")) {
            return 2;
        }
        if (!contains (out, "phase=dropped")) {
            return 3;
        }
    }

    // normal emits the lifecycle, keyed by correlation id; no sizes yet.
    {
        const auto out = capture_clog ([] {
            const auto opts = options_with_mode (message_flow_log_mode_t::normal);
            message_flow_tracer_t tracer (opts);
            tracer.trace (flow_event (message_flow_outcome_t::received));
            tracer.trace (flow_event (message_flow_outcome_t::replied));
        });
        if (!contains (out, "zlink flow:")
            || !contains (out, "event=zlink.message_flow")) {
            return 4;
        }
        if (!contains (out, "phase=received")
            || !contains (out, "phase=replied")
            || !contains (out, "outcome=succeeded")) {
            return 5;
        }
        if (!contains (out, "corr=corr-123")) {
            return 6;
        }
        if (!contains (out, "packet=PlaceOrder")) {
            return 7;
        }
        if (contains (out, "size=42")) {
            return 8;
        }
    }

    // detailed appends the message size...
    {
        const auto out = capture_clog ([] {
            auto options = options_with_mode (message_flow_log_mode_t::detailed);
            options.include_message_sizes (true);
            message_flow_tracer_t (options).trace (
              flow_event (message_flow_outcome_t::received));
        });
        if (!contains (out, "size=42")) {
            return 9;
        }
    }

    // ...unless include_message_sizes opts out.
    {
        auto options = options_with_mode (message_flow_log_mode_t::detailed);
        options.include_message_sizes (false);
        const auto out = capture_clog ([&] {
            message_flow_tracer_t (options).trace (flow_event (message_flow_outcome_t::received));
        });
        if (contains (out, "size=")) {
            return 10;
        }
    }

    // Internal bound-session pipeline stages are Detailed-only even though
    // they use the admitted phase shared by normal message-flow events.
    {
        zlink::framework::detail::actor_gateway_runtime_t gateway;
        gateway.set_dispatch (
          options_with_mode (message_flow_log_mode_t::normal));
        const auto normal = capture_clog ([&] {
            gateway.trace_bound_session_send_stage (
              "player-1", "router_admission_wait", "pending");
        });
        if (!normal.empty ())
            return 33;

        gateway.set_dispatch (
          options_with_mode (message_flow_log_mode_t::detailed));
        const auto detailed = capture_clog ([&] {
            gateway.trace_bound_session_send_stage (
              "player-1", "router_admission_wait", "pending");
        });
        if (!contains (detailed, "phase=admitted")
            || !contains (detailed, "actor=player-1")
            || !contains (detailed, "stage=router_admission_wait")
            || !contains (detailed, "result=pending")) {
            return 34;
        }
    }

    // off also silences the dispatch error log...
    {
        const auto reported_before = dispatch_error_reporter_t::reported ();
        const auto out = capture_clog ([] {
            dispatch_error_reporter_t (options_with_mode (message_flow_log_mode_t::off))
              .report (error_event ());
        });
        if (!out.empty ()
            || dispatch_error_reporter_t::reported () != reported_before) {
            return 11;
        }
    }

    // ...while errors (the default) keeps reporting errors.
    {
        const auto out = capture_clog ([] {
            dispatch_error_reporter_t (options_with_mode (message_flow_log_mode_t::errors))
              .report (error_event ());
        });
        if (!contains (out, "zlink flow:")) {
            return 12;
        }
        if (!contains (out, "event=zlink.dispatch_error")
            || !contains (out, "outcome=failed")) {
            return 13;
        }
        if (!contains (out, "reason=no_handler")) {
            return 14;
        }
        // failure lines now carry the correlation id, so a single grep corr=<id>
        // follows a message whether it succeeded or failed.
        if (!contains (out, "corr=corr-123")) {
            return 15;
        }
    }

    // Every processing point reads the live shared mode once; ambient entry
    // snapshots never override a later runtime change.
    {
        auto options = options_with_mode (message_flow_log_mode_t::off);
        auto live = std::make_shared<std::atomic<message_flow_log_mode_t>> (
          message_flow_log_mode_t::off);
        zlink::framework::detail::dispatch_options_access_t::set_live_mode (
          options, live);

        // Static says off, live says off -> nothing.
        auto out = capture_clog ([&] {
            message_flow_tracer_t (options).trace (flow_event (message_flow_outcome_t::received));
        });
        if (!out.empty ()) {
            return 15;
        }

        // Flip live to normal at runtime -> now it traces, despite static off.
        live->store (message_flow_log_mode_t::normal);
        out = capture_clog ([&] {
            message_flow_tracer_t (options).trace (flow_event (message_flow_outcome_t::received));
        });
        if (!contains (out, "phase=received")) {
            return 16;
        }

        // Enter while normal, then turn diagnostics off before the processing
        // point: the transition is silent.
        out = capture_clog ([&] {
            auto message_scope =
              runtime::flow_context_t::enter_current_or_create (
                flow_origin_t::application,
                message_flow_tracer_t (options).mode ());
            live->store (message_flow_log_mode_t::off);
            message_flow_tracer_t (options).trace (
              flow_event (message_flow_outcome_t::received));
        });
        if (!out.empty ()) {
            return 29;
        }

        // The next message sees off and is silent.
        live->store (message_flow_log_mode_t::off);
        out = capture_clog ([&] {
            auto message_scope =
              runtime::flow_context_t::enter_current_or_create (
                flow_origin_t::application,
                message_flow_tracer_t (options).mode ());
            message_flow_tracer_t (options).trace (
              flow_event (message_flow_outcome_t::received));
        });
        if (!out.empty ()) {
            return 30;
        }

        // Enter while off, then turn diagnostics on before the processing
        // point: that transition uses the new live level.
        out = capture_clog ([&] {
            auto message_scope =
              runtime::flow_context_t::enter_current_or_create (
                flow_origin_t::application,
                message_flow_tracer_t (options).mode ());
            live->store (message_flow_log_mode_t::normal);
            message_flow_tracer_t (options).trace (
              flow_event (message_flow_outcome_t::received));
        });
        if (!contains (out, "phase=received")) {
            return 31;
        }
        out = capture_clog ([&] {
            auto message_scope =
              runtime::flow_context_t::enter_current_or_create (
                flow_origin_t::application,
                message_flow_tracer_t (options).mode ());
            message_flow_tracer_t (options).trace (
              flow_event (message_flow_outcome_t::received));
        });
        if (!contains (out, "phase=received")) {
            return 32;
        }
    }

    // sample_rate gates healthy transitions but never backpressure or drops.
    {
        auto options = options_with_mode (message_flow_log_mode_t::normal);
        options.trace_sample_rate (0.0);
        const auto out = capture_clog ([&] {
            message_flow_tracer_t tracer (options);
            tracer.trace (flow_event (message_flow_outcome_t::received));
            tracer.trace (flow_event (message_flow_outcome_t::backpressured));
        });
        if (contains (out, "phase=received")) {
            return 18;
        }
        if (!contains (out, "phase=backpressured")
            || contains (out, "phase=error")) {
            return 19;
        }
    }

    // Failed/cancelled/shutdown request terminals are also never sampled,
    // even though their phase is reply_received.
    {
        auto options = options_with_mode (message_flow_log_mode_t::errors);
        options.trace_sample_rate (0.0);
        const auto out = capture_clog ([&] {
            auto event = flow_event (message_flow_outcome_t::reply_received);
            event.result = message_flow_result_t::cancelled;
            message_flow_tracer_t (options).trace (
              message_flow_outcome_t::reply_received,
              message_flow_result_t::cancelled,
              [&] { return event; });
        });
        if (!contains (out, "phase=reply_received")
            || !contains (out, "outcome=cancelled")) {
            return 35;
        }
    }

    // Structured logging uses the spec projection rather than legacy keys.
    {
        const auto out = capture_clog ([] {
            auto event = flow_event (message_flow_outcome_t::sent);
            event.surface = dispatch_error_surface_t::route_mesh_channel;
            event.message_kind = dispatch_message_kind_t::control;
            event.mesh_name = "game-mesh";
            event.channel_route_kind = "route_mesh";
            event.source_rid = "source-1";
            event.target_rid = "target-2";
            event.server_rid = "server-3";
            message_flow_tracer_t (options_with_mode (message_flow_log_mode_t::normal))
              .trace (std::move (event));
        });
        if (!contains (out, "event=zlink.message_flow")
            || !contains (out, "kind=control")
            || !contains (out, "mesh=game-mesh")
            || !contains (out, "channel_route=route_mesh")
            || !contains (out, "source_rid=source-1")
            || !contains (out, "target_rid=target-2")
            || !contains (out, "server_rid=server-3")
            || contains (out, "event_id=")
            || contains (out, " src=")) {
            return 36;
        }
    }

    // The internal test observer sees the same event fields as the logger adapter.
    {
        auto options = options_with_mode (message_flow_log_mode_t::normal);
        std::atomic_int observed{0};
        std::atomic_bool saw_packet{false};
        zlink::framework::detail::dispatch_options_access_t::set_observer_for_tests (
          options, [&] (const message_flow_event_t &event) {
            if (event.packet_name && *event.packet_name == "PlaceOrder") {
                saw_packet.store (true, std::memory_order_release);
            }
            observed.fetch_add (1, std::memory_order_acq_rel);
          });
        (void) capture_clog ([&] {
            message_flow_tracer_t (options).trace (flow_event (message_flow_outcome_t::replied));
        });
        if (!wait_until ([&] { return observed.load (std::memory_order_acquire) == 1; })) {
            return 20;
        }
        if (!saw_packet.load (std::memory_order_acquire)) {
            return 21;
        }
    }

    // Classic fanout dispatch has no normal flow record. A missing subscriber
    // handler emits exactly one closed-vocabulary dispatch-error record.
    {
        auto options = options_with_mode (message_flow_log_mode_t::normal);
        const framework_exception_t missing_handler (
          framework_error_kind_t::not_found, "handler is not registered");
        const auto out = capture_clog ([&] {
            dispatch_error_reporter_t (options).report (message_dispatch_error_event_t{
              .surface = dispatch_error_surface_t::classic_fanout,
              .message_kind = dispatch_message_kind_t::send,
              .reason = detail::dispatch_reason_from_error (&missing_handler),
              .action = dispatch_error_action_t::drop,
              .packet_name = std::string ("MissingEventMsg"),
              .channel_name = std::string ("pubsub.events"),
              .topic = std::string ("fanout"),
              .exception = std::make_exception_ptr (missing_handler)});
        });
        if (!contains (out, "event=zlink.dispatch_error")
            || !contains (out, "surface=classic_fanout")
            || !contains (out, "kind=send")
            || !contains (out, "reason=no_handler")
            || contains (out, "channel_route=")
            || occurrences (out, "event=zlink.dispatch_error") != 1
            || contains (out, "event=zlink.message_flow")) {
            return 28;
        }
    }

    // enum names are stable grep tokens for logs and metric sinks.
    {
        using zlink::framework::detail::enum_name;
        if (enum_name (dispatch_error_surface_t::channel) != "channel"
            || enum_name (dispatch_error_surface_t::route_mesh_channel)
                 != "channel"
            || enum_name (dispatch_error_surface_t::spot_route) != "spot"
            || enum_name (dispatch_error_surface_t::spot_subscription) != "spot"
            || enum_name (dispatch_error_surface_t::spot_actor) != "actor"
            || enum_name (dispatch_error_surface_t::stream_session) != "stream"
            || enum_name (dispatch_error_surface_t::node) != "node"
            || enum_name (dispatch_error_surface_t::instance_spot) != "instance_spot"
            || enum_name (dispatch_error_surface_t::actor_relocation) != "actor_relocation"
            || enum_name (dispatch_error_surface_t::classic_fanout) != "classic_fanout") {
            return 22;
        }
        if (enum_name (dispatch_message_kind_t::send) != "send"
            || enum_name (dispatch_message_kind_t::request) != "request"
            || enum_name (dispatch_message_kind_t::publish) != "send"
            || enum_name (dispatch_message_kind_t::response) != "response"
            || enum_name (dispatch_message_kind_t::error) != "error"
            || enum_name (dispatch_message_kind_t::actor_send) != "send"
            || enum_name (dispatch_message_kind_t::actor_request) != "request"
            || enum_name (dispatch_message_kind_t::control) != "control") {
            return 23;
        }
        if (enum_name (dispatch_error_reason_t::handler_missing) != "no_handler"
            || enum_name (dispatch_error_reason_t::payload_decode_failed)
                 != "decode_error"
            || enum_name (dispatch_error_reason_t::handler_exception) != "handler_exception"
            || enum_name (dispatch_error_reason_t::invalid_frame) != "invalid_frame"
            || enum_name (dispatch_error_reason_t::reply_path_missing) != "reply_path_missing"
            || enum_name (dispatch_error_reason_t::unexpected_reply) != "unexpected_reply"
            || enum_name (dispatch_error_reason_t::backpressure) != "backpressure"
            || enum_name (dispatch_error_reason_t::stale_target) != "stale_target"
            || enum_name (dispatch_error_reason_t::shutdown) != "shutdown") {
            return 24;
        }
        /* framework API §2.4.3: reply frame이 없는 local 경로의 실패는 fail_caller로 관측한다. */
        if (enum_name (dispatch_error_action_t::drop) != "drop"
            || enum_name (dispatch_error_action_t::reply_error) != "reply_error"
            || enum_name (dispatch_error_action_t::fail_caller) != "fail_caller") {
            return 25;
        }

        /* channel 메시징 §3.1: handler 예외는 one-way라도 Error, handler 없음·decode 실패·
         * invalid frame은 send=Warning / publish=Debug로 낮춘다(request는 error reply). */
        auto log_level_for = [] (dispatch_error_reason_t reason, dispatch_message_kind_t kind) {
            auto event = error_event ();
            event.reason = reason;
            event.message_kind = kind;
            return detail::dispatch_error_reporter_t::default_log_level (event);
        };
        if (log_level_for (dispatch_error_reason_t::handler_exception,
                           dispatch_message_kind_t::publish)
              != log_level_t::error
            || log_level_for (dispatch_error_reason_t::handler_exception,
                              dispatch_message_kind_t::send)
                 != log_level_t::error) {
            return 26;
        }
        if (log_level_for (dispatch_error_reason_t::handler_missing,
                           dispatch_message_kind_t::publish)
              != log_level_t::debug
            || log_level_for (dispatch_error_reason_t::payload_decode_failed,
                              dispatch_message_kind_t::send)
                 != log_level_t::warn
            || log_level_for (dispatch_error_reason_t::invalid_frame,
                              dispatch_message_kind_t::actor_send)
                 != log_level_t::warn
            || log_level_for (dispatch_error_reason_t::handler_missing,
                              dispatch_message_kind_t::request)
                 != log_level_t::error) {
            return 27;
        }
        if (enum_name (message_flow_outcome_t::received) != "received"
            || enum_name (message_flow_outcome_t::admitted) != "admitted"
            || enum_name (message_flow_outcome_t::dispatched) != "dispatched"
            || enum_name (message_flow_outcome_t::completed) != "completed"
            || enum_name (message_flow_outcome_t::replied) != "replied"
            || enum_name (message_flow_outcome_t::dropped) != "dropped"
            || enum_name (message_flow_outcome_t::sent) != "sent"
            || enum_name (message_flow_outcome_t::reply_received) != "reply_received"
            || enum_name (message_flow_outcome_t::backpressured) != "backpressured") {
            return 26;
        }
        if (enum_name (message_flow_result_t::succeeded) != "succeeded"
            || enum_name (message_flow_result_t::failed) != "failed"
            || enum_name (message_flow_result_t::backpressured) != "backpressured"
            || enum_name (message_flow_result_t::dropped) != "dropped"
            || enum_name (message_flow_result_t::cancelled) != "cancelled"
            || enum_name (message_flow_result_t::shutdown) != "shutdown") {
            return 37;
        }
        if (enum_name (message_flow_reason_t::backpressure) != "backpressure"
            || enum_name (message_flow_reason_t::stale_target) != "stale_target"
            || enum_name (message_flow_reason_t::target_closed) != "target_closed"
            || enum_name (message_flow_reason_t::shutdown) != "shutdown"
            || enum_name (message_flow_reason_t::location_unavailable)
                 != "location_unavailable"
            || enum_name (message_flow_reason_t::activation_rejected)
                 != "activation_rejected"
            || enum_name (message_flow_reason_t::activation_timeout)
                 != "activation_timeout") {
            return 38;
        }
    }

    return 0;
}
