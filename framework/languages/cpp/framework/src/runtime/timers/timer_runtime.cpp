/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "timer_runtime.hpp"

#include "runtime/diagnostics/flow_context.hpp"
#include "runtime/diagnostics/message_flow_tracer.hpp"
#include "runtime/diagnostics/monitoring_runtime.hpp"
#include "runtime/spots/spot_runtime.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace zlink::framework
{

timer_t::timer_t () : _state (std::make_shared<detail::timer_state_t> ())
{
}

timer_t::timer_t (std::shared_ptr<detail::timer_state_t> state) : _state (std::move (state))
{
}

timer_t::~timer_t () = default;
timer_t::timer_t (timer_t &&) noexcept = default;
timer_t &timer_t::operator= (timer_t &&) noexcept = default;

bool timer_t::is_disposed () const noexcept
{
    return !_state || _state->disposed;
}

void timer_t::cancel () noexcept
{
    if (_state) {
        _state->disposed = true;
        if (_state->native_timer && _state->native_timer->valid ()) {
            try {
                _state->native_timer->stop ();
            }
            catch (...) {
            }
        }
        // The native callback captures the shared timer state. Destroy the
        // native timer after stopping it so that callback storage releases
        // that capture and does not keep the state in a cycle.
        _state->native_timer.reset ();
    }
}

timer_t spot_context_t::add_timer_erased (std::string name,
                                          std::chrono::milliseconds period,
                                          timer_options_t options,
                                          std::type_index handler_type,
                                          std::function<std::shared_ptr<void> (
                                            service_provider_t *)> handler_factory,
                                          detail::timer_state_t::handler_invoker_t handler_invoker)
{
    ensure_submission_open ();
    if (name.empty ()) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "SPOT timer name must not be empty");
    }
    if (period <= std::chrono::milliseconds::zero ()) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "SPOT timer period must be greater than zero");
    }
    if (options.overrun_policy == timer_overrun_policy_t::catch_up_bounded
        && options.max_catch_up_ticks == 0) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "SPOT timer max catch-up ticks must be greater than zero");
    }

    const auto duplicate = std::any_of (_state->timers.begin (), _state->timers.end (),
                                        [&] (const std::shared_ptr<detail::timer_state_t> &timer) {
                                            return timer->name == name && !timer->disposed;
                                        });
    if (duplicate) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "duplicate SPOT timer registration");
    }

    auto state = std::make_shared<detail::timer_state_t> ();
    state->name = std::move (name);
    state->period = period;
    state->options = options;
    state->handler_type = handler_type;
    const auto existing_handler =
      _state->timer_handler_instances.find (handler_type);
    if (existing_handler != _state->timer_handler_instances.end ()) {
        state->handler_instance = existing_handler->second;
    } else {
        auto *activation_services =
          _state->activation_scope
            ? &_state->activation_scope->provider ()
            : nullptr;
        auto handler_instance = handler_factory (activation_services);
        if (!handler_instance) {
            throw framework_exception_t (
              framework_error_kind_t::not_configured,
              "SPOT timer handler factory returned null");
        }
        _state->timer_handler_instances.emplace (
          handler_type, handler_instance);
        state->handler_instance = handler_instance;
    }
    state->handler_invoker = std::move (handler_invoker);
    if (_state->execution_mode == user_spot_execution_mode_t::per_actor
        && _state->serial_executor) {
        state->lane = std::make_shared<runtime::serial_execution_queue_t> (
          *_state->serial_executor,
          runtime::serial_execution_queue_options_t{});
    }
    state->native_timer = std::make_unique<zlink::timer_t> ();
    auto context = _state;
    state->native_timer->on_fire ([context, state] (std::uint64_t fire_count) {
        detail::timer_runtime_t::post_fire_count (context, state, fire_count);
    });
    state->native_timer->start (period, std::numeric_limits<std::uint64_t>::max ());
    _state->timers.push_back (state);
    return timer_t (state);
}

} // namespace zlink::framework

namespace zlink::framework::detail
{

timer_runtime_t::timer_runtime_t (std::shared_ptr<spot_context_state_t> context) :
    _context (std::move (context))
{
}

timer_runtime_t timer_runtime_t::from (spot_context_t &context)
{
    return timer_runtime_t (context._state);
}

void timer_runtime_t::post_fire_count (const std::shared_ptr<spot_context_state_t> &context,
                                       const std::shared_ptr<timer_state_t> &state,
                                       std::uint64_t fire_count)
{
    if (!context || !state || state->disposed) {
        return;
    }
    auto work = [context, state, fire_count] (auto complete) mutable {
          framework::timer_t timer (state);
          auto runtime = timer_runtime_t (context);
          auto task = runtime.dispatch_fire_count_async (timer, fire_count);
          detail::observe_task_completion (
            task, [complete] (const result_t<timer_tick_t> &) mutable { complete ([] {}); });
      };
    const bool posted =
      state->lane
        ? state->lane->try_post_async (
            "spot-timer:" + state->name, std::move (work))
        : context->try_post_serial_async (
            "spot-timer:" + state->name, std::move (work));
    if (!posted) {
        std::lock_guard lock (state->mutex);
        if (!state->disposed) {
            state->pending_fire = true;
            state->pending_fire_count = fire_count;
        }
    }
}

namespace
{

std::uint64_t select_scheduled_index (const timer_state_t &state, std::uint64_t fire_count)
{
    if (state.options.overrun_policy == timer_overrun_policy_t::delay_next_tick) {
        return state.last_scheduled_index + 1;
    }

    const auto due = state.last_scheduled_index + std::max<std::uint64_t> (1, fire_count);
    if (state.options.overrun_policy == timer_overrun_policy_t::skip_late_ticks) {
        return due;
    }

    const auto available = due - state.last_scheduled_index;
    if (available > state.options.max_catch_up_ticks) {
        return due - state.options.max_catch_up_ticks + 1;
    }
    return state.last_scheduled_index + 1;
}

timer_tick_t make_tick (timer_state_t &state, std::uint64_t fire_count)
{
    const auto scheduled_index = select_scheduled_index (state, fire_count);
    const auto skipped_ticks = scheduled_index - state.last_scheduled_index - 1;
    state.delivery_index++;

    const auto scheduled_elapsed = state.period * scheduled_index;
    const auto started_elapsed =
      state.options.overrun_policy == timer_overrun_policy_t::delay_next_tick
        ? state.period * state.delivery_index
        : state.period * (state.last_scheduled_index + std::max<std::uint64_t> (1, fire_count));

    timer_tick_t tick{state.name,
                      state.delivery_index,
                      scheduled_index,
                      state.period,
                      scheduled_elapsed,
                      started_elapsed,
                      started_elapsed - scheduled_elapsed,
                      skipped_ticks};
    state.last_scheduled_index = scheduled_index;
    state.delivered_ticks.push_back (tick);
    return tick;
}

void record_timer_failure (const std::shared_ptr<spot_context_state_t> &context,
                           const std::shared_ptr<timer_state_t> &state,
                           bool stopped,
                           std::string message)
{
    timer_failure_event_t failure{state->name, state->handler_type, state->delivery_index, stopped,
                                  std::move (message)};
    state->failure_events.push_back (failure);
    if (context && context->node && context->node->monitoring) {
        monitoring_runtime_t (context->node->monitoring)
          .publish_timer_failure (context->node->snapshot.name, context->spot_id,
                                  std::move (failure));
    }
}

} // namespace

result_t<timer_tick_t>
timer_runtime_t::dispatch_fire_count (timer_t &timer,
                                      std::uint64_t fire_count,
                                      std::function<void (const timer_tick_t &)> handler) const
{
    if (timer.is_disposed ()) {
        return detail::boundary_failure<timer_tick_t> (detail::boundary_error_t::closed,
                                                "SPOT timer is disposed");
    }
    if (timer._state->running) {
        return result_t<timer_tick_t>::failure (framework_error_kind_t::rejected,
                                                "SPOT timer callback is already running");
    }

    timer._state->running = true;
    if (!_context->enter_callback ()) {
        timer._state->running = false;
        return detail::boundary_failure<timer_tick_t> (
          detail::boundary_error_t::closed,
          "SPOT timer activation is closed");
    }
    auto reset_running = [&timer, this] {
        timer._state->running = false;
        _context->leave_callback ();
    };

    try {
        auto tick = make_tick (*timer._state, fire_count);
        if (handler) {
            handler (tick);
        }
        reset_running ();
        return result_t<timer_tick_t>::success (std::move (tick));
    }
    catch (const std::exception &error) {
        const auto stopped = timer._state->options.stop_on_unhandled_exception;
        record_timer_failure (_context, timer._state, stopped, error.what ());
        if (stopped) {
            timer._state->disposed = true;
        }
        reset_running ();
        return result_t<timer_tick_t>::failure (framework_error_kind_t::internal_failure,
                                                error.what ());
    }
    catch (...) {
        const auto stopped = timer._state->options.stop_on_unhandled_exception;
        record_timer_failure (_context, timer._state, stopped, "unknown timer handler failure");
        if (stopped) {
            timer._state->disposed = true;
        }
        reset_running ();
        return result_t<timer_tick_t>::failure (framework_error_kind_t::internal_failure,
                                                "unknown timer handler failure");
    }
}

task_t<timer_tick_t> timer_runtime_t::dispatch_fire_count_async (timer_t &timer,
                                                                 std::uint64_t fire_count) const
{
    auto state = timer._state;
    auto context = _context;
    if (!state || state->disposed) {
        co_return detail::boundary_failure<timer_tick_t> (detail::boundary_error_t::closed,
                                                   "SPOT timer is disposed");
    }
    {
        std::lock_guard lock (state->mutex);
        if (state->running) {
            state->pending_fire = true;
            state->pending_fire_count = fire_count;
            co_return result_t<timer_tick_t>::failure (framework_error_kind_t::rejected,
                                                       "SPOT timer callback is already running");
        }
        state->running = true;
    }
    if (!state->handler_invoker) {
        co_return result_t<timer_tick_t>::failure (framework_error_kind_t::protocol_error,
                                                   "SPOT timer handler is not configured");
    }
    if (!context || !context->spot_instance || !context->channel_runtime
        || !context->channel_runtime->serializers) {
        co_return result_t<timer_tick_t>::failure (framework_error_kind_t::protocol_error,
                                                   "SPOT timer context is not configured");
    }

    if (!context->enter_callback ()) {
        std::lock_guard lock (state->mutex);
        state->running = false;
        co_return detail::boundary_failure<timer_tick_t> (
          detail::boundary_error_t::closed,
          "SPOT timer activation is closed");
    }
    auto reset_running = [context, state] {
        bool post_pending = false;
        std::uint64_t pending_fire_count = 0;
        {
            std::lock_guard lock (state->mutex);
            state->running = false;
            post_pending = state->pending_fire && !state->disposed;
            pending_fire_count = state->pending_fire_count;
            state->pending_fire = false;
            state->pending_fire_count = 0;
        }
        context->leave_callback ();
        if (post_pending) {
            timer_runtime_t::post_fire_count (context, state, pending_fire_count);
        }
    };

    try {
        auto tick = make_tick (*state, fire_count);
        auto spot_keep_alive = context->spot_instance;
        /* Timer callbacks have no inbound message: they start a new flow with
         * origin=timer when tracing capture is enabled (flow-correlation §4.2). */
        auto timer_flow = framework::runtime::flow_context_t::enter_current_or_create (
          flow_origin_t::timer,
          message_flow_tracer_t (context->channel_runtime->dispatch).capture_enabled ());
        auto handler_instance = state->handler_instance.lock ();
        if (!handler_instance) {
            throw framework_exception_t (
              framework_error_kind_t::protocol_error,
              "SPOT timer handler activation is no longer available");
        }
        auto handler_task = state->handler_invoker (spot_keep_alive.get (),
                                                    handler_instance.get (),
                                                    *context->channel_runtime->serializers, tick);
        (void) co_await handler_task;
        reset_running ();
        co_return result_t<timer_tick_t>::success (std::move (tick));
    }
    catch (const framework_exception_t &error) {
        const auto stopped = state->options.stop_on_unhandled_exception;
        record_timer_failure (context, state, stopped, error.what ());
        if (stopped) {
            state->disposed = true;
        }
        reset_running ();
        co_return detail::result_access_t::failure<timer_tick_t> (error);
    }
    catch (const std::exception &error) {
        const auto stopped = state->options.stop_on_unhandled_exception;
        record_timer_failure (context, state, stopped, error.what ());
        if (stopped) {
            state->disposed = true;
        }
        reset_running ();
        co_return result_t<timer_tick_t>::failure (framework_error_kind_t::internal_failure,
                                                   error.what ());
    }
    catch (...) {
        const auto stopped = state->options.stop_on_unhandled_exception;
        record_timer_failure (context, state, stopped, "unknown timer handler failure");
        if (stopped) {
            state->disposed = true;
        }
        reset_running ();
        co_return result_t<timer_tick_t>::failure (framework_error_kind_t::internal_failure,
                                                   "unknown timer handler failure");
    }
}

void timer_runtime_t::cancel_all () const noexcept
{
    cancel_all (*_context);
}

void timer_runtime_t::cancel_all (spot_context_state_t &context) noexcept
{
    for (const auto &timer : context.timers) {
        timer->disposed = true;
        if (timer->native_timer && timer->native_timer->valid ()) {
            try {
                timer->native_timer->stop ();
            }
            catch (...) {
            }
        }
        timer->running = false;
        // Stop alone leaves the binding callback object attached to the
        // native timer. Destroy it to break the callback-to-state cycle.
        timer->native_timer.reset ();
        if (timer->lane) {
            timer->lane->cancel_pending ();
        }
    }
}

std::vector<timer_failure_event_t> timer_runtime_t::failure_events (const timer_t &timer) const
{
    if (!timer._state) {
        return {};
    }
    return timer._state->failure_events;
}

std::vector<timer_tick_t> timer_runtime_t::delivered_ticks (const timer_t &timer) const
{
    if (!timer._state) {
        return {};
    }
    return timer._state->delivered_ticks;
}

} // namespace zlink::framework::detail
