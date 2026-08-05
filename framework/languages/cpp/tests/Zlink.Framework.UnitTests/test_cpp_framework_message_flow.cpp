/* SPDX-License-Identifier: FSL-1.1-ALv2 */

// Verifies the message-flow tracing feature: dispatch_diagnostics_options_t::
// message_flow now actually gates a structured, correlation-id-keyed log stream
// (success transitions via message_flow_tracer_t) and silences the error log in
// the off mode (dispatch_error_reporter_t). The tracer's default sink is
// std::clog, so the tests capture that buffer and assert on the emitted lines.

#include "runtime/diagnostics/dispatch_error_reporter.hpp"
#include "runtime/diagnostics/dispatch_diagnostics_names.hpp"
#include "runtime/diagnostics/message_flow_tracer.hpp"

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

    // errors_only emits the drop decision but not healthy transitions.
    {
        const auto out = capture_clog ([] {
            const auto opts = options_with_mode (message_flow_log_mode_t::errors_only);
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

    // key_transitions emits the lifecycle, keyed by correlation id; no sizes yet.
    {
        const auto out = capture_clog ([] {
            const auto opts = options_with_mode (message_flow_log_mode_t::key_transitions);
            message_flow_tracer_t tracer (opts);
            tracer.trace (flow_event (message_flow_outcome_t::received));
            tracer.trace (flow_event (message_flow_outcome_t::replied));
        });
        if (!contains (out, "event_id=zlink.message_flow")) {
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

    // verbose appends the message size...
    {
        const auto out = capture_clog ([] {
            message_flow_tracer_t (options_with_mode (message_flow_log_mode_t::verbose))
              .trace (flow_event (message_flow_outcome_t::received));
        });
        if (!contains (out, "size=42")) {
            return 9;
        }
    }

    // ...unless include_message_sizes opts out.
    {
        auto options = options_with_mode (message_flow_log_mode_t::verbose);
        options.include_message_sizes (false);
        const auto out = capture_clog ([&] {
            message_flow_tracer_t (options).trace (flow_event (message_flow_outcome_t::received));
        });
        if (contains (out, "size=")) {
            return 10;
        }
    }

    // off also silences the dispatch error log...
    {
        const auto out = capture_clog ([] {
            dispatch_error_reporter_t (options_with_mode (message_flow_log_mode_t::off))
              .report (error_event ());
        });
        if (!out.empty ()) {
            return 11;
        }
    }

    // ...while errors_only (the default) keeps reporting errors.
    {
        const auto out = capture_clog ([] {
            dispatch_error_reporter_t (options_with_mode (message_flow_log_mode_t::errors_only))
              .report (error_event ());
        });
        if (!contains (out, "dispatch error")) {
            return 12;
        }
        if (!contains (out, "event_id=zlink.dispatch_error")
            || !contains (out, "outcome=failed")) {
            return 13;
        }
        if (!contains (out, "reason=handler_missing")) {
            return 14;
        }
        // failure lines now carry the correlation id, so a single grep corr=<id>
        // follows a message whether it succeeded or failed.
        if (!contains (out, "corr=corr-123")) {
            return 15;
        }
    }

    // live_mode (runtime toggle) overrides the static mode and is read live, so an
    // operator can turn tracing on/off without restart.
    {
        auto options = options_with_mode (message_flow_log_mode_t::off);
        auto live = std::make_shared<std::atomic<message_flow_log_mode_t>> (
          message_flow_log_mode_t::off);
        options.message_flow_live (live);

        // Static says off, live says off -> nothing.
        auto out = capture_clog ([&] {
            message_flow_tracer_t (options).trace (flow_event (message_flow_outcome_t::received));
        });
        if (!out.empty ()) {
            return 15;
        }

        // Flip live to key_transitions at runtime -> now it traces, despite static off.
        live->store (message_flow_log_mode_t::key_transitions);
        out = capture_clog ([&] {
            message_flow_tracer_t (options).trace (flow_event (message_flow_outcome_t::received));
        });
        if (!contains (out, "phase=received")) {
            return 16;
        }

        // Flip back to off -> silent again.
        live->store (message_flow_log_mode_t::off);
        out = capture_clog ([&] {
            message_flow_tracer_t (options).trace (flow_event (message_flow_outcome_t::received));
        });
        if (!out.empty ()) {
            return 17;
        }
    }

    // sample_rate gates healthy transitions but never drops error transitions.
    {
        auto options = options_with_mode (message_flow_log_mode_t::key_transitions);
        options.trace_sample_rate (0.0);
        const auto out = capture_clog ([&] {
            message_flow_tracer_t tracer (options);
            tracer.trace (flow_event (message_flow_outcome_t::received));
            tracer.trace (flow_event (message_flow_outcome_t::error));
        });
        if (contains (out, "phase=received")) {
            return 18;
        }
        if (!contains (out, "phase=error")) {
            return 19;
        }
    }

    // observer callbacks are delivered asynchronously and see the same event fields.
    {
        auto options = options_with_mode (message_flow_log_mode_t::key_transitions);
        std::atomic_int observed{0};
        std::atomic_bool saw_packet{false};
        options.set_message_flow_observer ([&] (const message_flow_event_t &event) {
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

    // Classic fanout dispatch has no reply frame.  A missing subscriber handler
    // must still be observable as publish/handler_missing/drop so the runtime
    // owner can preserve the public negative-dispatch contract.
    {
        auto options = options_with_mode (message_flow_log_mode_t::key_transitions);
        std::atomic_bool saw_fanout_failure{false};
        options.set_message_flow_observer ([&] (const message_flow_event_t &event) {
            if (event.outcome == message_flow_outcome_t::error
                && event.surface == dispatch_error_surface_t::channel
                && event.message_kind == dispatch_message_kind_t::publish
                && event.packet_name && *event.packet_name == "MissingEventMsg"
                && event.channel_name && *event.channel_name == "pubsub.events"
                && event.topic && *event.topic == "fanout"
                && event.error_reason
                   == std::optional<dispatch_error_reason_t> (
                     dispatch_error_reason_t::handler_missing)
                && event.error_action
                   == std::optional<dispatch_error_action_t> (
                     dispatch_error_action_t::drop)) {
                saw_fanout_failure.store (true, std::memory_order_release);
            }
        });
        const framework_exception_t missing_handler (
          framework_error_kind_t::not_found, "handler is not registered");
        dispatch_error_reporter_t (options).report (message_dispatch_error_event_t{
          .surface = dispatch_error_surface_t::channel,
          .message_kind = dispatch_message_kind_t::publish,
          .reason = detail::dispatch_reason_from_error (&missing_handler),
          .action = dispatch_error_action_t::drop,
          .packet_name = std::string ("MissingEventMsg"),
          .channel_name = std::string ("pubsub.events"),
          .topic = std::string ("fanout"),
          .exception = std::make_exception_ptr (missing_handler)});
        if (!wait_until ([&] {
                return saw_fanout_failure.load (std::memory_order_acquire);
            })) {
            return 28;
        }
    }

    // enum names are stable grep tokens for logs and metric sinks.
    {
        using zlink::framework::detail::enum_name;
        if (enum_name (dispatch_error_surface_t::channel) != "channel"
            || enum_name (dispatch_error_surface_t::route_mesh_channel)
                 != "route_mesh_channel"
            || enum_name (dispatch_error_surface_t::spot_route) != "spot_route"
            || enum_name (dispatch_error_surface_t::spot_subscription) != "spot_subscription"
            || enum_name (dispatch_error_surface_t::spot_actor) != "spot_actor"
            || enum_name (dispatch_error_surface_t::stream_session) != "stream_session") {
            return 22;
        }
        if (enum_name (dispatch_message_kind_t::send) != "send"
            || enum_name (dispatch_message_kind_t::request) != "request"
            || enum_name (dispatch_message_kind_t::publish) != "publish"
            || enum_name (dispatch_message_kind_t::response) != "response"
            || enum_name (dispatch_message_kind_t::error) != "error"
            || enum_name (dispatch_message_kind_t::actor_send) != "actor_send"
            || enum_name (dispatch_message_kind_t::actor_request) != "actor_request") {
            return 23;
        }
        if (enum_name (dispatch_error_reason_t::handler_missing) != "handler_missing"
            || enum_name (dispatch_error_reason_t::payload_decode_failed)
                 != "payload_decode_failed"
            || enum_name (dispatch_error_reason_t::handler_exception) != "handler_exception"
            || enum_name (dispatch_error_reason_t::invalid_frame) != "invalid_frame"
            || enum_name (dispatch_error_reason_t::reply_path_missing) != "reply_path_missing"
            || enum_name (dispatch_error_reason_t::unexpected_reply) != "unexpected_reply") {
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
            || enum_name (message_flow_outcome_t::dispatched) != "dispatched"
            || enum_name (message_flow_outcome_t::replied) != "replied"
            || enum_name (message_flow_outcome_t::dropped) != "dropped"
            || enum_name (message_flow_outcome_t::sent) != "sent"
            || enum_name (message_flow_outcome_t::reply_received) != "reply_received"
            || enum_name (message_flow_outcome_t::error) != "error") {
            return 26;
        }
    }

    return 0;
}
