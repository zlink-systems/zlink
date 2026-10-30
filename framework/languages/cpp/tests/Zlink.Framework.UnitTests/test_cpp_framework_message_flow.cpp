/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

// Verifies the message-flow tracing feature: dispatch_diagnostics_options_t::
// message_flow now actually gates a structured, correlation-id-keyed log stream
// (success transitions via message_flow_tracer_t) and silences the error log in
// the off mode (dispatch_error_reporter_t). Tests install an application logger;
// a runtime without a configured logger must not write to a process console.

#include "../support/telemetry_observation.hpp"
#include "../support/allocation_counter.hpp"

#include "runtime/diagnostics/dispatch_error_reporter.hpp"
#include "metric_test_reader.hpp"
#include <set>
#include "runtime/diagnostics/dispatch_diagnostics_names.hpp"
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/diagnostics/message_flow_tracer.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/protocol/service_wire_codec.hpp"
#include <zlink/framework/contracts/configuration/app.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

namespace
{

using namespace zlink::framework;
using zlink::framework::detail::dispatch_error_reporter_t;
using zlink::framework::detail::message_flow_tracer_t;
constexpr auto error_message_max_length =
  zlink::framework::detail::diagnostic_event_sink_t::error_message_max_length;

struct log_capture_t
{
    std::mutex mutex;
    std::condition_variable changed;
    std::string text;
    std::size_t records = 0;
};
std::atomic<std::shared_ptr<log_capture_t>> captured_logs;
// Some legacy cases construct temporary standalone options. Keep their registration
// until the acknowledged record below has bounded every preceding observation.
std::vector<dispatch_options_t> retained_test_options;
std::uint64_t captured_trace_attempts = 0;

using zlink::framework::test::acknowledge_delivery;

template <typename Fn> std::string capture_logs (Fn &&fn)
{
    auto capture = std::make_shared<log_capture_t> ();
    auto previous = captured_logs.exchange (capture);
    const auto before = message_flow_tracer_t::traced () + dispatch_error_reporter_t::reported ();
    std::forward<Fn> (fn) ();
    captured_trace_attempts = message_flow_tracer_t::traced ();
    const auto expected = captured_trace_attempts + dispatch_error_reporter_t::reported () - before;
    {
        std::unique_lock lock (capture->mutex);
        if (!capture->changed.wait_for (lock, std::chrono::milliseconds (500),
                                        [&] { return capture->records >= expected; }))
            throw std::runtime_error ("log record acknowledgment timed out");
    }
    acknowledge_delivery ();
    captured_logs.store (previous);
    retained_test_options.clear ();
    std::lock_guard lock (capture->mutex);
    return capture->text;
}

dispatch_options_t options_with_mode (message_flow_log_mode_t mode)
{
    dispatch_options_t options;
    options.message_flow (mode);
    logging_builder_t logging;
    logging.use_provider ("message-flow-test", [] (const log_record_t &record) {
        auto capture = captured_logs.load ();
        if (!capture)
            return;
        std::lock_guard lock (capture->mutex);
        capture->text += "zlink flow:";
        for (const auto &field : record.fields)
            capture->text += " " + field.key + "=" + field.value;
        capture->text.push_back ('\n');
        ++capture->records;
        capture->changed.notify_all ();
    });
    zlink::framework::detail::dispatch_options_access_t::set_logger (
      options, logging.create_logger ("message-flow-test"));
    retained_test_options.push_back (options);
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

bool file_provider_failure_does_not_change_message_result ()
{
    namespace fs = std::filesystem;
    const auto failed_log_path =
      fs::temp_directory_path ()
      / ("zlink-message-flow-provider-failure-"
         + std::to_string (std::chrono::steady_clock::now ().time_since_epoch ().count ()));
    std::error_code create_error;
    if (!fs::create_directory (failed_log_path, create_error) || create_error) {
        return false;
    }

    app_t app;
    app.logging ().use_file (failed_log_path.string ());
    dispatch_options_t options;
    options.message_flow (message_flow_log_mode_t::normal);
    zlink::framework::detail::dispatch_options_access_t::set_logger (
      options, app.logging ().factory ().create ("message-flow-test"));
    auto observed_success = std::make_shared<std::atomic_bool> (false);
    zlink::framework::detail::dispatch_options_access_t::set_observer_for_tests (
      options, [observed_success] (const message_flow_event_t &event) {
          if (event.outcome == message_flow_outcome_t::completed
              && event.result == message_flow_result_t::succeeded) {
              observed_success->store (true, std::memory_order_release);
          }
      });

    const auto failures_before = message_flow_tracer_t::observer_failures ();
    const auto message_result = [&] {
        auto event = flow_event (message_flow_outcome_t::completed);
        event.result = message_flow_result_t::succeeded;
        message_flow_tracer_t (options).trace (std::move (event));
        return message_flow_result_t::succeeded;
    }();
    const bool observer_received_success =
      wait_until ([&] { return observed_success->load (std::memory_order_acquire); });
    const auto failures_after = message_flow_tracer_t::observer_failures ();
    std::error_code cleanup_error;
    fs::remove_all (failed_log_path, cleanup_error);

    return message_result == message_flow_result_t::succeeded && observer_received_success
           && failures_after == failures_before + 1 && !cleanup_error;
}

} // namespace

int dispatch_error_observer_failures_preserve_operation ()
{
    bool passed = true;
    for (const bool factory_failure : {false, true}) {
        auto options = options_with_mode (message_flow_log_mode_t::normal);
        auto observation = std::make_shared<unsigned> (0);
        auto &observer_calls = *observation;
        unsigned factory_calls = 0;
        zlink::framework::detail::dispatch_options_access_t::set_dispatch_error_observer_for_tests (
          options, [observation, factory_failure] (const auto &) {
              ++*observation;
              if (!factory_failure)
                  throw std::runtime_error ("dispatch observer failed");
          });
        const auto before = dispatch_error_reporter_t::observer_failures ();
        const auto operation = [&] {
            dispatch_error_reporter_t reporter (options);
            if (factory_failure)
                reporter.report_lazy ([&] () -> message_dispatch_error_event_t {
                    ++factory_calls;
                    throw std::runtime_error ("dispatch event factory failed");
                });
            else
                reporter.report (error_event ());
            return factory_failure ? result_t<void>::failure (framework_error_kind_t::rejected,
                                                              "operation rejected")
                                   : result_t<void>::success ();
        }();
        acknowledge_delivery ();
        const auto delta = dispatch_error_reporter_t::observer_failures () - before;
        const bool unchanged =
          factory_failure
            ? !operation && operation.error_kind () == framework_error_kind_t::rejected
                && std::string (operation.error ()->what ()) == "operation rejected"
            : static_cast<bool> (operation);
        std::cout << "dispatch_failure=" << (factory_failure ? "factory" : "observer")
                  << " observer_calls=" << observer_calls << " factory_calls=" << factory_calls
                  << " failure_delta=" << delta << " operation_unchanged=" << unchanged
                  << std::endl;
        passed = passed && delta == 1 && unchanged && observer_calls == (factory_failure ? 0u : 1u)
                 && factory_calls == (factory_failure ? 1u : 0u);
    }
    return passed ? 0 : 1;
}

int dispatch_error_logger_failures_preserve_operation ()
{
    namespace fs = std::filesystem;
    bool passed = true;
    for (const bool file_failure : {false, true}) {
        fs::path failed_log_path;
        if (file_failure) {
            failed_log_path =
              fs::temp_directory_path ()
              / ("zlink-dispatch-log-provider-failure-"
                 + std::to_string (std::chrono::steady_clock::now ().time_since_epoch ().count ()));
            if (!fs::create_directory (failed_log_path))
                return 2;
        }
        app_t app;
        logging_builder_t logging;
        struct counts_t { unsigned provider = 0, observer = 0; };
        auto counts = std::make_shared<counts_t> ();
        auto &provider_calls = counts->provider;
        auto &observer_calls = counts->observer;
        auto options = options_with_mode (message_flow_log_mode_t::normal);
        if (file_failure) {
            app.logging ().use_file (failed_log_path.string ());
            zlink::framework::detail::dispatch_options_access_t::set_logger (
              options, app.logging ().factory ().create ("dispatch-file-failure"));
        } else {
            logging.use_provider ("throwing-dispatch-provider", [counts] (const log_record_t &) {
                ++counts->provider;
                throw std::runtime_error ("dispatch logging provider failed");
            });
            zlink::framework::detail::dispatch_options_access_t::set_logger (
              options, logging.create_logger ("dispatch-provider-failure"));
        }
        zlink::framework::detail::dispatch_options_access_t::set_dispatch_error_observer_for_tests (
          options, [counts] (const auto &) { ++counts->observer; });
        const auto before = dispatch_error_reporter_t::observer_failures ();
        const auto operation_result = [&] {
            dispatch_error_reporter_t (options).report (error_event ());
            return message_flow_result_t::succeeded;
        }();
        acknowledge_delivery ();
        const auto delta = dispatch_error_reporter_t::observer_failures () - before;
        const bool unchanged = operation_result == message_flow_result_t::succeeded;
        std::error_code cleanup_error;
        if (file_failure)
            fs::remove_all (failed_log_path, cleanup_error);
        std::cout << "logger_failure=" << (file_failure ? "file_path" : "throw_provider")
                  << " provider_calls=" << provider_calls << " observer_calls=" << observer_calls
                  << " failure_delta=" << delta << " operation_unchanged=" << unchanged
                  << " cleanup_error=" << cleanup_error.value () << std::endl;
        passed = passed && delta == 1 && unchanged && observer_calls == 1 && !cleanup_error
                 && (file_failure || provider_calls == 1);
    }
    return passed ? 0 : 1;
}

int errors_flow_owner ()
{
    using zlink::framework::detail::enum_name;
    using access = zlink::framework::detail::dispatch_options_access_t;
    const std::string ambient_id = "019a1234-5678-7000-8000-000000000001";
    const std::string explicit_id = "019a1234-5678-7000-8000-000000000002";
    bool passed = true;
    for (const auto mode : {message_flow_log_mode_t::errors, message_flow_log_mode_t::normal,
                            message_flow_log_mode_t::detailed, message_flow_log_mode_t::off}) {
        for (const bool live_change : {false, true}) {
            for (const bool explicit_pair : {false, true}) {
                auto scope = runtime::flow_context_t::enter (
                  std::optional<std::string>{ambient_id}, flow_origin_t::application,
                  message_flow_log_mode_t::normal, flow_origin_t::inbound,
                  std::string ("foreign-session"));
                auto options =
                  options_with_mode (live_change ? message_flow_log_mode_t::normal : mode);
                auto live = std::make_shared<std::atomic<message_flow_log_mode_t>> (
                  message_flow_log_mode_t::normal);
                if (live_change) {
                    access::set_live_mode (options, live);
                    live->store (mode);
                }
                options.trace_sample_rate (0.0);
                const bool emits = mode != message_flow_log_mode_t::off;
                const bool traces = mode == message_flow_log_mode_t::normal
                                    || mode == message_flow_log_mode_t::detailed;
                const auto expected_id = explicit_pair ? explicit_id : ambient_id;
                const auto expected_origin =
                  explicit_pair ? flow_origin_t::timer : flow_origin_t::application;
                struct observation_t { unsigned calls = 0; bool fields = true; };
                auto error_observation = std::make_shared<observation_t> ();
                auto &error_calls = error_observation->calls;
                auto &error_fields = error_observation->fields;
                access::set_dispatch_error_observer_for_tests (
                  options, [error_observation, traces, expected_id, expected_origin] (const message_dispatch_error_event_t &event) {
                      auto &error_calls = error_observation->calls;
                      auto &error_fields = error_observation->fields;
                      ++error_calls;
                      error_fields = event.correlation_id == "corr-123"
                                     && (traces ? event.flow_id == expected_id
                                                    && event.flow_origin == expected_origin
                                                : !event.flow_id && !event.flow_origin);
                  });
                unsigned error_builds = 0;
                const auto reported_before = dispatch_error_reporter_t::reported ();
                const auto error_log = capture_logs ([&] {
                    dispatch_error_reporter_t (options).report_lazy ([&] {
                        ++error_builds;
                        auto event = error_event ();
                        if (explicit_pair) {
                            event.flow_id = explicit_id;
                            event.flow_origin = flow_origin_t::timer;
                        }
                        return event;
                    });
                });
                const bool error_ok =
                  error_calls == unsigned (emits) && error_builds == unsigned (emits)
                  && dispatch_error_reporter_t::reported () - reported_before == unsigned (emits)
                  && error_fields
                  && occurrences (error_log, "event_id=zlink.dispatch_error") == unsigned (emits)
                  && occurrences (error_log, "corr=corr-123") == unsigned (emits)
                  && occurrences (error_log, " flow=") == unsigned (traces)
                  && occurrences (error_log, " origin=") == unsigned (traces)
                  && (!traces
                      || (contains (error_log, " flow=" + expected_id)
                          && contains (error_log,
                                       " origin=" + std::string (enum_name (expected_origin)))));
                std::cout << "errors_flow_reporter mode=" << int (mode) << " live=" << live_change
                          << " explicit_pair=" << explicit_pair << " passed=" << error_ok << '\n';
                passed = error_ok && passed;
                for (const bool explicit_session : {false, true}) {
                    // Exercise each stamp_flow caller without sampling errors or drops.
                    for (int entry = 0; entry != 4; ++entry) {
                        auto observation = std::make_shared<observation_t> ();
                        auto &calls = observation->calls;
                        auto &fields = observation->fields;
                        const auto expected_session =
                          explicit_session ? std::optional<std::string>{"own-session"}
                          : traces         ? std::optional<std::string>{"foreign-session"}
                                           : std::nullopt;
                        access::set_observer_for_tests (
                          options, [observation, expected_session, traces, expected_id, expected_origin] (const message_flow_event_t &event) {
                              auto &calls = observation->calls;
                              auto &fields = observation->fields;
                              ++calls;
                              fields = event.outcome == message_flow_outcome_t::dropped
                                       && event.correlation_id == "corr-123"
                                       && event.stream_session_id == expected_session
                                       && (traces ? event.flow_id == expected_id
                                                      && event.flow_origin == expected_origin
                                                  : !event.flow_id && !event.flow_origin);
                          });
                        unsigned builds = 0;
                        const auto build = [&] {
                            ++builds;
                            auto event = flow_event (message_flow_outcome_t::dropped);
                            if (explicit_pair) {
                                event.flow_id = explicit_id;
                                event.flow_origin = flow_origin_t::timer;
                            }
                            if (explicit_session)
                                event.stream_session_id = "own-session";
                            return event;
                        };
                        const auto before = message_flow_tracer_t::traced ();
                        const auto log = capture_logs ([&] {
                            message_flow_tracer_t tracer (options);
                            if (entry == 0)
                                tracer.trace (build ());
                            else if (entry == 1)
                                tracer.trace (message_flow_outcome_t::dropped, build);
                            else if (entry == 2)
                                tracer.trace (message_flow_outcome_t::dropped, explicit_id, build);
                            else
                                tracer.trace (message_flow_outcome_t::dropped,
                                              message_flow_result_t::failed, build);
                        });
                        const bool ok =
                          calls == unsigned (emits) && fields
                          && builds == unsigned (entry == 0 || emits)
                          && captured_trace_attempts - before == unsigned (emits)
                          && occurrences (log, "event_id=zlink.message_flow") == unsigned (emits)
                          && occurrences (log, "phase=dropped") == unsigned (emits)
                          && occurrences (log, "corr=corr-123") == unsigned (emits)
                          && occurrences (log, " flow=") == unsigned (traces)
                          && occurrences (log, " origin=") == unsigned (traces)
                          && occurrences (log, " session=")
                               == unsigned (emits && expected_session.has_value ())
                          && (!emits || !expected_session
                              || contains (log, " session=" + *expected_session))
                          && (!traces
                              || (contains (log, " flow=" + expected_id)
                                  && contains (
                                    log, " origin=" + std::string (enum_name (expected_origin)))));
                        std::cout << "errors_flow_drop mode=" << int (mode)
                                  << " live=" << live_change << " explicit_pair=" << explicit_pair
                                  << " explicit_session=" << explicit_session << " entry=" << entry
                                  << " passed=" << ok << '\n';
                        passed = ok && passed;
                    }
                }
            }
        }
    }
    return passed ? 0 : 1;
}

template <typename T> struct release_provider_t
{
    std::shared_ptr<T> state;
    ~release_provider_t ()
    {
        std::lock_guard lock (state->mutex);
        state->release = true;
        state->changed.notify_all ();
    }
};

int blocked_public_provider (bool reporter = false)
{
    struct state_t {
        std::mutex mutex;
        std::condition_variable changed;
        bool entered = false;
        bool release = false;
        bool completed = false;
    };
    auto state = std::make_shared<state_t> ();
    logging_builder_t logging;
    logging.use_provider ("blocked-public-provider", [state] (const log_record_t &) {
        std::unique_lock lock (state->mutex);
        state->entered = true;
        state->changed.notify_all ();
        state->changed.wait (lock, [&] { return state->release; });
    });
    dispatch_options_t options;
    options.message_flow (message_flow_log_mode_t::normal);
    detail::dispatch_options_access_t::set_logger (options, logging.create_logger ("blocked"));
    std::thread producer ([&] {
        if (reporter)
            dispatch_error_reporter_t (options).report (error_event ());
        else
            message_flow_tracer_t (options).trace (flow_event (message_flow_outcome_t::received));
        std::lock_guard lock (state->mutex);
        state->completed = true;
        state->changed.notify_all ();
    });
    struct join_t {
        std::thread &producer;
        ~join_t () { if (producer.joinable ()) producer.join (); }
    } join{producer};
    release_provider_t release{state};
    bool entered, progressed;
    {
        std::unique_lock lock (state->mutex);
        const auto budget = std::chrono::milliseconds (500); // Existing wait_until fixture budget.
        entered = state->changed.wait_for (lock, budget, [&] { return state->entered; });
        progressed = state->changed.wait_for (lock, budget, [&] { return state->completed; });
        state->release = true;
        state->changed.notify_all ();
    }
    producer.join ();
    std::cout << "blocked_public_provider reporter=" << reporter << " entered=" << entered
              << " producer_completed_before_release=" << progressed << std::endl;
    return entered && progressed ? 0 : 81;
}

int bounded_process_delivery (bool retire_first, int waiting = 8192)
{
    using access = detail::dispatch_options_access_t;
    struct state_t {
        std::mutex mutex;
        std::condition_variable changed;
        bool entered = false, release = false, produced = false;
        std::vector<std::string> ids;
    };
    auto state = std::make_shared<state_t> ();
    release_provider_t release{state};
    metric_test::provider_t metrics;
    auto provider = metrics.api ();
    auto meter = provider->GetMeter ("zlink.framework");
    auto first = std::make_unique<dispatch_options_t> ();
    auto second = std::make_unique<dispatch_options_t> ();
    logging_builder_t logging;
    logging.disable_record_capture ().use_provider ("bounded-process-delivery", [state] (const log_record_t &record) {
        std::unique_lock lock (state->mutex);
        if (!state->entered) {
            state->entered = true;
            state->changed.notify_all ();
            state->changed.wait (lock, [&] { return state->release; });
        }
        for (const auto &field : record.fields)
            if (field.key == "corr")
                state->ids.push_back (field.value);
        state->changed.notify_all ();
    });
    for (auto *options : {first.get (), second.get ()}) {
        options->message_flow (message_flow_log_mode_t::normal);
        access::set_logger (*options, logging.create_logger ("bounded"));
        access::bind_telemetry (*options, provider, meter);
    }
    auto submit = [] (dispatch_options_t &options, int index, std::string id) {
        if (index % 3 == 1) {
            auto event = error_event ();
            event.correlation_id = std::move (id);
            dispatch_error_reporter_t (options).report (std::move (event));
        } else {
            auto event = flow_event (index % 3 == 0 ? message_flow_outcome_t::received
                                                  : message_flow_outcome_t::dropped);
            event.correlation_id = std::move (id);
            message_flow_tracer_t (options).trace (std::move (event));
        }
    };
    const auto before = message_flow_tracer_t::observer_dropped ();
    submit (*first, 0, "inflight-A");
    bool entered;
    {
        std::unique_lock lock (state->mutex);
        entered = state->changed.wait_for (lock, std::chrono::milliseconds (500), [&] { return state->entered; });
    }
    // Both registrations share the spec's process limit, excluding the executing record.
    std::thread producer ([&] {
        for (int index = 0; index != waiting; ++index)
            submit (index % 2 == 0 ? *first : *second, index, std::to_string (index));
        for (int index = 0; index != 3; ++index)
            submit (index % 2 == 0 ? *first : *second, index, "overflow-" + std::to_string (index));
        std::lock_guard lock (state->mutex);
        state->produced = true;
        state->changed.notify_all ();
    });
    struct join_t {
        std::thread &producer;
        std::shared_ptr<state_t> state;
        ~join_t () {
            if (!producer.joinable ()) return;
            { std::lock_guard lock (state->mutex); state->release = true; state->changed.notify_all (); }
            producer.join ();
        }
    } join{producer, state};
    bool producer_completed;
    {
        std::unique_lock lock (state->mutex);
        producer_completed = state->changed.wait_for (lock, std::chrono::milliseconds (500),
                                                     [&] { return state->produced; });
        if (!producer_completed) {
            state->release = true;
            state->changed.notify_all ();
        }
    }
    producer.join ();
    const auto overflow = message_flow_tracer_t::observer_dropped () - before;
    // Host metric attachment must preserve the registration and its already queued records.
    access::bind_telemetry (*first, provider, meter);
    if (retire_first) {
        access::retire_telemetry (*first);
        access::retire_telemetry (*first); // Normal close and destruction may both retire.
        submit (*first, 0, "closed-A");
        submit (*second, 0, "surviving-B");
        first.reset ();
        second->message_flow (message_flow_log_mode_t::off); // Metrics remain enabled.
    }
    {
        std::lock_guard lock (state->mutex);
        state->release = true;
        state->changed.notify_all ();
    }
    const auto expected = retire_first ? waiting / 2 + 2 : waiting + 1;
    bool delivered;
    {
        std::unique_lock lock (state->mutex);
        delivered = state->changed.wait_for (lock, std::chrono::milliseconds (500),
          [&] { return state->ids.size () >= std::size_t (expected); });
    }
    acknowledge_delivery ();
    bool ids_ok;
    {
        std::lock_guard lock (state->mutex);
        const std::set<std::string> ids (state->ids.begin (), state->ids.end ());
        ids_ok = state->ids.size () == std::size_t (expected) && ids.size () == state->ids.size ()
                 && ids.contains ("inflight-A") && !ids.contains ("closed-A");
        for (int index = 0; index != waiting; ++index)
            ids_ok = ids_ok && ids.contains (std::to_string (index)) == (!retire_first || index % 2 == 1);
        for (int index = 0; index != 3; ++index)
            ids_ok = ids_ok && !ids.contains ("overflow-" + std::to_string (index));
        if (retire_first)
            ids_ok = ids_ok && ids.contains ("surviving-B");
    }
    double exported = 0;
    for (const auto &fields : metrics.collect_fields ())
        if (fields.at ("name") == "zlink.observability.events.overflow"
            && fields.at ("unit") == "{event}" && fields.at ("source") == "message_flow")
            exported += std::stod (fields.at ("value"));
    const bool passed = entered && producer_completed && delivered && overflow == 3 && ids_ok && exported == 3
                        && message_flow_tracer_t::observer_dropped () - before == 3;
    std::cout << "bounded_process_delivery retire=" << retire_first << " entered=" << entered
              << " producer_completed_before_release=" << producer_completed << " overflow=" << overflow << " metric=" << exported
              << " delivered=" << state->ids.size () << " expected=" << expected
              << " exact_ids=" << ids_ok << " passed=" << passed << std::endl;
    return passed ? 0 : 82;
}

int frozen_process_capture (bool wrong_capture = false)
{
    using access = detail::dispatch_options_access_t;
    struct state_t {
        std::mutex mutex;
        std::condition_variable changed;
        bool entered = false, release = false;
        std::vector<log_record_t> records;
    };
    auto state = std::make_shared<state_t> ();
    release_provider_t release{state};
    logging_builder_t logging;
    logging.use_provider ("frozen-process-capture", [state] (const log_record_t &record) {
        std::unique_lock lock (state->mutex);
        if (!state->entered) {
            state->entered = true;
            state->changed.notify_all ();
            state->changed.wait (lock, [&] { return state->release; });
        }
        state->records.push_back (record);
    });
    dispatch_options_t options;
    options.message_flow (message_flow_log_mode_t::detailed).include_message_sizes (true);
    auto live = std::make_shared<std::atomic<message_flow_log_mode_t>> (message_flow_log_mode_t::detailed);
    access::set_live_mode (options, live);
    access::set_logger (options, logging.create_logger ("frozen"));
    message_flow_tracer_t (options).trace (flow_event (message_flow_outcome_t::received));
    bool entered;
    {
        std::unique_lock lock (state->mutex);
        entered = state->changed.wait_for (lock, std::chrono::milliseconds (500), [&] { return state->entered; });
    }
    struct owned_exception_t : std::runtime_error {
        std::shared_ptr<int> owner;
        explicit owned_exception_t (std::shared_ptr<int> owner) :
            std::runtime_error ("token=secret\nprivate"), owner (std::move (owner)) {}
    };
    std::weak_ptr<int> exception_owner;
    {
        auto scope = runtime::flow_context_t::enter (
          std::optional<std::string>{wrong_capture ? "019a1234-5678-7000-8000-000000000099" : "019a1234-5678-7000-8000-000000000001"}, flow_origin_t::application,
          message_flow_log_mode_t::normal, flow_origin_t::inbound, std::string ("frozen-session"));
        auto event = flow_event (message_flow_outcome_t::completed);
        event.result = message_flow_result_t::failed;
        auto owner = std::make_shared<int> (2758);
        exception_owner = owner;
        event.exception = std::make_exception_ptr (owned_exception_t (owner));
        message_flow_tracer_t (options).trace (std::move (event));
        live->store (message_flow_log_mode_t::errors);
        auto error = error_event ();
        error.flow_id = "019a1234-5678-7000-8000-000000000002";
        error.flow_origin = flow_origin_t::timer;
        dispatch_error_reporter_t (options).report (std::move (error));
    }
    const bool exception_released = exception_owner.expired ();
    options.include_message_sizes (false);
    live->store (message_flow_log_mode_t::off);
    {
        std::lock_guard lock (state->mutex);
        state->release = true;
        state->changed.notify_all ();
    }
    acknowledge_delivery ();
    std::string flow, error;
    for (const auto &record : state->records) {
        auto &out = record.message == "dispatch error" ? error : flow;
        for (const auto &field : record.fields)
            out += " " + field.key + "=" + field.value;
    }
    const bool passed = entered && exception_released && state->records.size () == 3
                        && contains (flow, " flow=019a1234-5678-7000-8000-000000000001")
                        && contains (flow, " origin=application") && contains (flow, " session=frozen-session")
                        && contains (flow, " size=42") && contains (flow, " error_message=token=<redacted>")
                        && !contains (flow, "secret") && !contains (flow, "private")
                        && contains (error, " corr=corr-123") && !contains (error, " flow=")
                        && !contains (error, " origin=");
    std::cout << "frozen_process_capture records=" << state->records.size ()
              << " exception_released=" << exception_released << " passed=" << passed << std::endl;
    return passed ? 0 : 83;
}

int process_failure_roles (bool fallback_fails)
{
    struct state_t { std::atomic_uint logger{0}, trace{0}; };
    auto state = std::make_shared<state_t> ();
    const auto before = message_flow_tracer_t::observer_failures ();
    constexpr unsigned records_per_host = 3;
    for (unsigned host = 0; host != 2; ++host) {
        logging_builder_t logging;
        logging.use_provider ("failing-provider-role", [state] (const log_record_t &) {
            ++state->logger;
            throw std::runtime_error ("logger failure");
        });
        dispatch_options_t options;
        options.message_flow (message_flow_log_mode_t::normal);
        detail::dispatch_options_access_t::set_logger (options, logging.create_logger ("failure-role"));
        detail::dispatch_options_access_t::set_observer_for_tests (options, [state] (const auto &) {
            ++state->trace;
            throw std::runtime_error ("trace failure");
        });
        for (unsigned index = 0; index != records_per_host; ++index)
            message_flow_tracer_t (options).trace (flow_event (message_flow_outcome_t::received));
        acknowledge_delivery ();
    }
    const auto failures = message_flow_tracer_t::observer_failures () - before;
    const bool passed = state->logger == 2 * records_per_host && state->trace == 2 * records_per_host
                        && failures == 4 * records_per_host + unsigned (fallback_fails);
    std::cout << "process_failure_roles logger=" << state->logger << " trace=" << state->trace
              << " failures=" << failures << " expected=" << 4 * records_per_host + unsigned (fallback_fails)
              << " passed=" << passed << std::endl;
    return passed ? 0 : 84;
}

// Provider absence must gate capture and both lazy builders before allocation.
int no_provider_capture ()
{
    dispatch_options_t options;
    options.message_flow (message_flow_log_mode_t::detailed);
    unsigned built = 0;
    bool capture = false;
    bool reporter = false;
    const auto allocations = zlink::framework::tests::count_allocations ([&] {
        detail::dispatch_options_access_t::set_logger (options, std::nullopt);
        detail::dispatch_options_access_t::set_observer_for_tests (options, {});
        detail::dispatch_options_access_t::set_dispatch_error_observer_for_tests (options, {});
        message_flow_tracer_t tracer (options);
        capture = tracer.capture_enabled ();
        reporter = dispatch_error_reporter_t (options).enabled ();
        if (capture) {
            ++built;
            const std::string snapshot (1024, 's');
            if (snapshot.empty ())
                ++built;
        }
        tracer.trace (message_flow_outcome_t::received, [&] {
            ++built;
            return flow_event (message_flow_outcome_t::received);
        });
        dispatch_error_reporter_t (options).report_lazy ([&] {
            ++built;
            return error_event ();
        });
    });
    std::cout << "no_provider allocations=" << allocations << " built=" << built
              << " capture=" << capture << " reporter=" << reporter << '\n';
    if (allocations != 0 || built != 0 || capture || reporter) {
        std::cerr << "no-provider capture must not build or allocate telemetry\n";
        return 91;
    }
    return 0;
}

int main (int argc, char **argv)
{
    if (const auto result = no_provider_capture (); result != 0)
        return result;
    if (argc == 2 && std::string (argv[1]) == "--no-provider")
        return 0;
    if (argc == 2 && std::string (argv[1]) == "--negative-overflow-control")
        return bounded_process_delivery (false, 8191);
    if (argc == 2 && std::string (argv[1]) == "--negative-capture-control")
        return frozen_process_capture (true);
    if (argc == 2 && std::string (argv[1]) == "--failure-roles")
        return process_failure_roles (false);
    if (argc == 2 && std::string (argv[1]) == "--failed-fallback")
        return process_failure_roles (true);
    if (argc == 2 && std::string (argv[1]) == "--bounded-process")
        return bounded_process_delivery (false);
    if (argc == 2 && std::string (argv[1]) == "--retire-process")
        return bounded_process_delivery (true);
    if (argc == 2 && std::string (argv[1]) == "--frozen-capture")
        return frozen_process_capture ();
    if (argc == 2 && std::string (argv[1]) == "--blocked-reporter")
        return blocked_public_provider (true);
    if (argc == 2 && std::string (argv[1]) == "--blocked-provider")
        return blocked_public_provider ();
    if (argc == 1) {
        if (const auto result = blocked_public_provider (); result != 0) return result;
        if (const auto result = blocked_public_provider (true); result != 0) return result;
    }
    if (argc == 2 && std::string (argv[1]) == "--errors-flow-owner")
        return errors_flow_owner ();
    if (argc == 2 && std::string (argv[1]) == "--logger-failures")
        return dispatch_error_logger_failures_preserve_operation ();
    if (argc == 2 && std::string (argv[1]) == "--observer-failures")
        return dispatch_error_observer_failures_preserve_operation ();
    if (const auto result = errors_flow_owner (); result != 0)
        return result;
    if (const auto result = dispatch_error_observer_failures_preserve_operation (); result != 0)
        return result;
    if (const auto result = dispatch_error_logger_failures_preserve_operation (); result != 0)
        return result;
    if (const auto result = bounded_process_delivery (false); result != 0)
        return result;
    if (const auto result = bounded_process_delivery (true); result != 0)
        return result;
    if (const auto result = frozen_process_capture (); result != 0)
        return result;
    // off silences every success transition, including drops.
    {
        const auto out = capture_logs ([] {
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
        const auto out = capture_logs ([] {
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
        const auto out = capture_logs ([] {
            const auto opts = options_with_mode (message_flow_log_mode_t::normal);
            message_flow_tracer_t tracer (opts);
            tracer.trace (flow_event (message_flow_outcome_t::received));
            tracer.trace (flow_event (message_flow_outcome_t::replied));
        });
        if (!contains (out, "zlink flow:") || !contains (out, "event_id=zlink.message_flow")) {
            return 4;
        }
        if (!contains (out, "phase=received") || !contains (out, "phase=replied")
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

    // A STREAM flow's session identity is stamped on every record in its scope.
    {
        const auto absent = capture_logs ([] {
            const auto opts = options_with_mode (message_flow_log_mode_t::normal);
            auto event = flow_event (message_flow_outcome_t::received);
            event.surface = dispatch_error_surface_t::stream_session;
            message_flow_tracer_t (opts).trace (std::move (event));
        });
        if (contains (absent, "session="))
            return 60;
        const auto out = capture_logs ([] {
            const auto opts = options_with_mode (message_flow_log_mode_t::normal);
            auto scope = zlink::framework::runtime::flow_context_t::enter (
              std::optional<std::string>{}, std::nullopt, message_flow_log_mode_t::normal,
              flow_origin_t::inbound, std::string ("session-42"));
            auto event = flow_event (message_flow_outcome_t::received);
            event.surface = dispatch_error_surface_t::stream_session;
            message_flow_tracer_t (opts).trace (std::move (event));
        });
        if (!contains (out, "session=session-42"))
            return 61;
    }

    // detailed appends the message size...
    {
        const auto out = capture_logs ([] {
            auto options = options_with_mode (message_flow_log_mode_t::detailed);
            options.include_message_sizes (true);
            message_flow_tracer_t (options).trace (flow_event (message_flow_outcome_t::received));
        });
        if (!contains (out, "size=42")) {
            return 9;
        }
    }

    // ...unless include_message_sizes opts out.
    {
        auto options = options_with_mode (message_flow_log_mode_t::detailed);
        options.include_message_sizes (false);
        const auto out = capture_logs ([&] {
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
        int detail_builds = 0;
        const auto build_result = [&] {
            ++detail_builds;
            return std::string ("pending");
        };

        gateway.set_dispatch (options_with_mode (message_flow_log_mode_t::off));
        const auto off = capture_logs ([&] {
            gateway.trace_bound_session_send_stage ("player-1", "router_admission_wait",
                                                    build_result, nullptr);
        });
        if (!off.empty () || detail_builds != 0)
            return 44;

        gateway.set_dispatch (options_with_mode (message_flow_log_mode_t::normal));
        const auto normal = capture_logs ([&] {
            gateway.trace_bound_session_send_stage ("player-1", "router_admission_wait",
                                                    build_result, nullptr);
        });
        if (!normal.empty () || detail_builds != 0)
            return 33;

        gateway.set_dispatch (options_with_mode (message_flow_log_mode_t::detailed));
        const auto detailed = capture_logs ([&] {
            gateway.trace_bound_session_send_stage ("player-1", "router_admission_wait",
                                                    build_result, nullptr);
        });
        if (!contains (detailed, "phase=admitted") || !contains (detailed, "actor=player-1")
            || !contains (detailed, "stage=router_admission_wait")
            || !contains (detailed, "result=pending") || detail_builds != 1) {
            return 34;
        }
    }

    // off also silences the dispatch error log...
    {
        const auto reported_before = dispatch_error_reporter_t::reported ();
        const auto out = capture_logs ([] {
            dispatch_error_reporter_t (options_with_mode (message_flow_log_mode_t::off))
              .report (error_event ());
        });
        if (!out.empty () || dispatch_error_reporter_t::reported () != reported_before) {
            return 11;
        }
    }

    // ...while errors (the default) keeps reporting errors.
    {
        const auto out = capture_logs ([] {
            dispatch_error_reporter_t (options_with_mode (message_flow_log_mode_t::errors))
              .report (error_event ());
        });
        if (!contains (out, "zlink flow:")) {
            return 12;
        }
        if (!contains (out, "event_id=zlink.dispatch_error") || !contains (out, "outcome=failed")) {
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

    // Handler exceptions project one sanitized pair to both the trace observer and log.
    {
        const auto message =
          std::string ("Authorization: Bearer auth Bearer standalone password=p token=t ")
          + std::string (error_message_max_length + 1, 'x') + "\n at Secret.Handler";
        const auto expected =
          (std::string (
             "Authorization: <redacted> Bearer <redacted> password=<redacted> token=<redacted> ")
           + std::string (error_message_max_length + 1, 'x'))
            .substr (0, error_message_max_length);
        auto options = options_with_mode (message_flow_log_mode_t::errors);
        auto observation = std::make_shared<std::optional<message_dispatch_error_event_t>> ();
        auto &observed = *observation;
        zlink::framework::detail::dispatch_options_access_t::set_dispatch_error_observer_for_tests (
          options, [observation] (const message_dispatch_error_event_t &event) { *observation = event; });
        const auto out = capture_logs ([&] {
            dispatch_error_reporter_t (options).report (message_dispatch_error_event_t{
              .surface = dispatch_error_surface_t::channel,
              .message_kind = dispatch_message_kind_t::send,
              .reason = dispatch_error_reason_t::handler_exception,
              .action = dispatch_error_action_t::drop,
              .exception = std::make_exception_ptr (std::runtime_error (message))});
        });
        const auto type = std::string (typeid (std::runtime_error).name ());
        if (!observed || !observed->exception || observed->error_type != type
            || observed->error_message != expected)
            return 40;
        try {
            std::rethrow_exception (observed->exception);
        }
        catch (const std::runtime_error &error) {
            if (std::string (error.what ()) != message)
                return 40;
        }
        if (!contains (out, "error_type=" + type) || !contains (out, "error_message=" + expected)
            || contains (out, "Bearer auth") || contains (out, "Bearer standalone")
            || contains (out, "password=p") || contains (out, "token=t")
            || contains (out, "Secret.Handler")) {
            return 41;
        }
    }

    // Empty exception messages remain present in both observer and log projections.
    {
        auto options = options_with_mode (message_flow_log_mode_t::errors);
        auto observation = std::make_shared<std::optional<message_dispatch_error_event_t>> ();
        auto &observed = *observation;
        zlink::framework::detail::dispatch_options_access_t::set_dispatch_error_observer_for_tests (
          options, [observation] (const message_dispatch_error_event_t &event) { *observation = event; });
        const auto out = capture_logs ([&] {
            dispatch_error_reporter_t (options).report (message_dispatch_error_event_t{
              .surface = dispatch_error_surface_t::channel,
              .message_kind = dispatch_message_kind_t::send,
              .reason = dispatch_error_reason_t::handler_exception,
              .action = dispatch_error_action_t::drop,
              .exception = std::make_exception_ptr (std::runtime_error (""))});
        });
        const auto type = std::string (typeid (std::runtime_error).name ());
        if (!observed || observed->error_type != type || !observed->error_message.has_value ()
            || !observed->error_message->empty ())
            return 42;
        if (!contains (out, "error_type=" + type) || !contains (out, "error_message="))
            return 43;
    }

    // 완료 단계의 예외도 정제한 두 필드를 structured log에 기록한다.
    {
        const auto message =
          std::string ("Authorization: Bearer auth Bearer standalone password=p token=t ")
          + std::string (error_message_max_length + 1, 'x') + "\n at Secret.Handler";
        const auto expected =
          (std::string (
             "Authorization: <redacted> Bearer <redacted> password=<redacted> token=<redacted> ")
           + std::string (error_message_max_length + 1, 'x'))
            .substr (0, error_message_max_length);
        for (const auto &[text, summary] :
             {std::pair{message, expected}, std::pair{std::string{}, std::string{}}}) {
            auto options = options_with_mode (message_flow_log_mode_t::errors);
            const auto exception = std::make_exception_ptr (std::runtime_error (text));
            auto event = flow_event (message_flow_outcome_t::completed);
            event.result = message_flow_result_t::failed;
            event.exception = exception;
            const auto out = capture_logs ([&] { message_flow_tracer_t (options).trace (event); });
            const auto type = std::string (typeid (std::runtime_error).name ());
            if (!contains (out, "phase=completed outcome=failed")
                || !contains (out, "error_type=" + type)
                || !contains (out, "error_message=" + summary + "\n")
                || occurrences (out, "error_type=") != 1 || occurrences (out, "error_message=") != 1
                || contains (out, "Bearer auth") || contains (out, "Bearer standalone")
                || contains (out, "password=p") || contains (out, "token=t")
                || contains (out, "Secret.Handler")) {
                std::cerr << "message-flow exception fields must preserve the sanitized pair\n";
                return 45;
            }
        }
        auto options = options_with_mode (message_flow_log_mode_t::errors);
        auto event = flow_event (message_flow_outcome_t::completed);
        event.result = message_flow_result_t::failed;
        const auto out = capture_logs ([&] { message_flow_tracer_t (options).trace (event); });
        if (contains (out, "error_type=") || contains (out, "error_message="))
            return 47;
    }

    // Every processing point reads the live shared mode once; ambient entry
    // snapshots never override a later runtime change.
    {
        auto options = options_with_mode (message_flow_log_mode_t::off);
        auto live =
          std::make_shared<std::atomic<message_flow_log_mode_t>> (message_flow_log_mode_t::off);
        zlink::framework::detail::dispatch_options_access_t::set_live_mode (options, live);

        // Static says off, live says off -> nothing.
        auto out = capture_logs ([&] {
            message_flow_tracer_t (options).trace (flow_event (message_flow_outcome_t::received));
        });
        if (!out.empty ()) {
            return 15;
        }

        // Flip live to normal at runtime -> now it traces, despite static off.
        live->store (message_flow_log_mode_t::normal);
        out = capture_logs ([&] {
            message_flow_tracer_t (options).trace (flow_event (message_flow_outcome_t::received));
        });
        if (!contains (out, "phase=received")) {
            return 16;
        }

        // Enter while normal, then turn diagnostics off before the processing
        // point: the transition is silent.
        out = capture_logs ([&] {
            auto message_scope = runtime::flow_context_t::enter_current_or_create (
              flow_origin_t::application, message_flow_tracer_t (options).mode ());
            live->store (message_flow_log_mode_t::off);
            message_flow_tracer_t (options).trace (flow_event (message_flow_outcome_t::received));
        });
        if (!out.empty ()) {
            return 29;
        }

        // The next message sees off and is silent.
        live->store (message_flow_log_mode_t::off);
        out = capture_logs ([&] {
            auto message_scope = runtime::flow_context_t::enter_current_or_create (
              flow_origin_t::application, message_flow_tracer_t (options).mode ());
            message_flow_tracer_t (options).trace (flow_event (message_flow_outcome_t::received));
        });
        if (!out.empty ()) {
            return 30;
        }

        // Enter while off, then turn diagnostics on before the processing
        // point: that transition uses the new live level.
        out = capture_logs ([&] {
            auto message_scope = runtime::flow_context_t::enter_current_or_create (
              flow_origin_t::application, message_flow_tracer_t (options).mode ());
            live->store (message_flow_log_mode_t::normal);
            message_flow_tracer_t (options).trace (flow_event (message_flow_outcome_t::received));
        });
        if (!contains (out, "phase=received")) {
            return 31;
        }
        out = capture_logs ([&] {
            auto message_scope = runtime::flow_context_t::enter_current_or_create (
              flow_origin_t::application, message_flow_tracer_t (options).mode ());
            message_flow_tracer_t (options).trace (flow_event (message_flow_outcome_t::received));
        });
        if (!contains (out, "phase=received")) {
            return 32;
        }
    }

    // sample_rate gates healthy transitions but never backpressure or drops.
    {
        auto options = options_with_mode (message_flow_log_mode_t::normal);
        options.trace_sample_rate (0.0);
        std::atomic_int built{0};
        const auto out = capture_logs ([&] {
            message_flow_tracer_t tracer (options);
            tracer.trace (message_flow_outcome_t::received, [&] {
                built.fetch_add (1, std::memory_order_relaxed);
                return flow_event (message_flow_outcome_t::received);
            });
            tracer.trace (flow_event (message_flow_outcome_t::backpressured));
        });
        if (contains (out, "phase=received") || built.load (std::memory_order_relaxed) != 0) {
            return 18;
        }
        if (!contains (out, "phase=backpressured") || contains (out, "phase=error")) {
            return 19;
        }
    }

    // Off exits before either event kind builds an event or string field.
    {
        auto options = options_with_mode (message_flow_log_mode_t::off);
        std::atomic_int built{0};
        message_flow_tracer_t (options).trace (message_flow_outcome_t::received, [&] {
            built.fetch_add (1, std::memory_order_relaxed);
            return flow_event (message_flow_outcome_t::received);
        });
        dispatch_error_reporter_t (options).report_lazy ([&] {
            built.fetch_add (1, std::memory_order_relaxed);
            return error_event ();
        });
        if (built.load (std::memory_order_relaxed) != 0)
            return 39;
    }

    // Failed/cancelled/shutdown request terminals are also never sampled,
    // even though their phase is reply_received.
    {
        auto options = options_with_mode (message_flow_log_mode_t::errors);
        options.trace_sample_rate (0.0);
        const auto out = capture_logs ([&] {
            auto event = flow_event (message_flow_outcome_t::reply_received);
            event.result = message_flow_result_t::cancelled;
            message_flow_tracer_t (options).trace (message_flow_outcome_t::reply_received,
                                                   message_flow_result_t::cancelled,
                                                   [&] { return event; });
        });
        if (!contains (out, "phase=reply_received") || !contains (out, "outcome=cancelled")) {
            return 35;
        }
    }

    // Structured logging uses the spec projection rather than legacy keys.
    {
        const auto out = capture_logs ([] {
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
        if (!contains (out, "event_id=zlink.message_flow") || !contains (out, "kind=control")
            || !contains (out, "mesh=game-mesh") || !contains (out, "channel_route=route_mesh")
            || !contains (out, "source_rid=source-1") || !contains (out, "target_rid=target-2")
            || !contains (out, "server_rid=server-3") || contains (out, " event=")
            || contains (out, " src=")) {
            return 36;
        }
    }

    // The internal test observer sees the same event fields as the logger adapter.
    {
        auto options = options_with_mode (message_flow_log_mode_t::normal);
        struct observation_t { std::atomic_int calls{0}; std::atomic_bool packet{false}; };
        auto observation = std::make_shared<observation_t> ();
        auto &observed = observation->calls;
        auto &saw_packet = observation->packet;
        zlink::framework::detail::dispatch_options_access_t::set_observer_for_tests (
          options, [observation] (const message_flow_event_t &event) {
              auto &observed = observation->calls;
              auto &saw_packet = observation->packet;
              if (event.packet_name && *event.packet_name == "PlaceOrder") {
                  saw_packet.store (true, std::memory_order_release);
              }
              observed.fetch_add (1, std::memory_order_acq_rel);
          });
        (void) capture_logs ([&] {
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
        const framework_exception_t missing_handler (framework_error_kind_t::not_found,
                                                     "handler is not registered");
        const auto out = capture_logs ([&] {
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
        if (!contains (out, "event_id=zlink.dispatch_error")
            || !contains (out, "surface=classic_fanout") || !contains (out, "kind=send")
            || !contains (out, "reason=no_handler") || !contains (out, "packet=MissingEventMsg")
            || !contains (out, "error_type=" + std::string (typeid (framework_exception_t).name ()))
            || !contains (out, "error_message=handler is not registered")
            || contains (out, "channel_route=")
            || occurrences (out, "event_id=zlink.dispatch_error") != 1
            || contains (out, "event_id=zlink.message_flow")) {
            return 28;
        }
    }

    // enum names are stable grep tokens for logs and metric sinks.
    {
        using zlink::framework::detail::enum_name;
        if (enum_name (dispatch_error_surface_t::channel) != "channel"
            || enum_name (dispatch_error_surface_t::route_mesh_channel) != "channel"
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
            || enum_name (dispatch_error_reason_t::payload_decode_failed) != "decode_error"
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
        // Packet names do not choose severity. Malformed notifications use
        // the existing one-way classification at their decode boundary.
        for (const auto kind :
             {dispatch_message_kind_t::actor_send, dispatch_message_kind_t::actor_request}) {
            for (const auto packet :
                 {runtime::protocol::session_disconnected_packet_name, "other.actor.packet"}) {
                auto event = error_event ();
                event.message_kind = kind;
                event.reason = dispatch_error_reason_t::invalid_frame;
                event.action = dispatch_error_action_t::drop;
                event.packet_name = packet;
                const auto expected = kind == dispatch_message_kind_t::actor_send
                                        ? log_level_t::warn
                                        : log_level_t::error;
                if (detail::dispatch_error_reporter_t::default_log_level (event) != expected)
                    return 71;
                event.action = dispatch_error_action_t::reply_error;
                if (detail::dispatch_error_reporter_t::default_log_level (event) != expected)
                    return 73;
                event.reason = dispatch_error_reason_t::handler_exception;
                if (detail::dispatch_error_reporter_t::default_log_level (event)
                    != log_level_t::error)
                    return 74;
            }
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
            || enum_name (message_flow_reason_t::location_unavailable) != "location_unavailable"
            || enum_name (message_flow_reason_t::activation_rejected) != "activation_rejected"
            || enum_name (message_flow_reason_t::activation_timeout) != "activation_timeout") {
            return 38;
        }
        if (zlink::framework::detail::diagnostic_absent_value != "<none>"
            || zlink::framework::detail::diagnostic_decoded_value != "<decoded>") {
            return 39;
        }
    }

    if (!file_provider_failure_does_not_change_message_result ()) {
        std::cerr << "message-flow file provider failure changed message observation\n";
        return 70;
    }

    return 0;
}
