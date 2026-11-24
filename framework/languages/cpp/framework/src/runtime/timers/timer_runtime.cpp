/* SPDX-License-Identifier: Elastic-2.0 */

#include "timer_runtime.hpp"

#include "runtime/diagnostics/flow_context.hpp"
#include "runtime/diagnostics/message_flow_tracer.hpp"
#include "runtime/diagnostics/monitoring_runtime.hpp"
#include "runtime/spots/spot_runtime.hpp"

#include <algorithm>
#include <exception>
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
    return !_state || _state->disposed.load (std::memory_order_acquire);
}

namespace
{

// A queued fire owns its callback claim until dispatch completes or timer
// cancellation releases the claim without starting application work.
struct timer_callback_admission_t
{
    explicit timer_callback_admission_t (std::shared_ptr<detail::spot_context_state_t> value = {}) :
        context (std::move (value))
    {
    }
    timer_callback_admission_t (const timer_callback_admission_t &) = delete;
    ~timer_callback_admission_t ()
    {
        if (context)
            context->leave_callback ();
    }
    std::shared_ptr<detail::spot_context_state_t> context;
};

void complete_cancel_if_ready (const std::shared_ptr<detail::timer_state_t> &state)
{
    std::shared_ptr<task_completion_source_t<void>> completion;
    std::optional<result_t<void>> result;
    {
        std::lock_guard lock (state->mutex);
        if (!state->cancel_completed && state->cancel_completion && state->cleanup_result
            && !state->running) {
            state->cancel_completed = true;
            completion = state->cancel_completion;
            result = state->cleanup_result;
        }
    }
    if (completion) {
        if (const auto context = state->spot_context.lock ())
            context->timers.erase (state);
        completion->complete (std::move (*result));
    }
}

} // namespace

task_t<void> timer_t::cancel ()
{
    if (!_state)
        return task_t<void> (result_t<void>::success ());

    const auto state = _state;
    std::shared_ptr<task_completion_source_t<void>> completion;
    timer_callback_admission_t pending_admission;
    std::unique_ptr<detail::timer_resource_t> resource;
    bool owns_cleanup = false;
    {
        std::lock_guard lock (state->mutex);
        if (!state->cancel_completion) {
            state->disposed.store (true, std::memory_order_release);
            if (state->pending_fire) {
                auto pending = std::static_pointer_cast<timer_callback_admission_t> (
                  state->pending_fire->lock ());
                if (pending)
                    pending_admission.context = std::move (pending->context);
                state->pending_fire.reset ();
            }
            state->pending_fire_count = 0;
            state->cancel_completion = std::make_shared<task_completion_source_t<void>> ();
            resource = std::move (state->native_timer);
            owns_cleanup = true;
        }
        completion = state->cancel_completion;
    }

    auto task = completion->task ();
    if (owns_cleanup) {
        auto cleanup_result = resource ? resource->cancel () : result_t<void>::success ();
        {
            std::lock_guard lock (state->mutex);
            state->cleanup_result = std::move (cleanup_result);
        }
        complete_cancel_if_ready (state);
    }
    return task;
}

timer_t spot_context_t::add_timer_erased (
  std::string name,
  std::chrono::milliseconds period,
  timer_options_t options,
  std::type_index handler_type,
  std::function<std::shared_ptr<void> (service_provider_t *)> handler_factory,
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
        && (options.max_catch_up_ticks == 0
            || options.max_catch_up_ticks
                 > static_cast<std::uint64_t> (std::numeric_limits<int>::max ()))) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "SPOT timer max catch-up ticks must be between 1 and INT_MAX");
    }

    auto state = std::make_shared<detail::timer_state_t> ();
    state->name = std::move (name);
    state->period = period;
    state->options = options;
    state->handler_type = handler_type;
    state->spot_context = _state;
    const auto existing_handler = _state->timer_handler_instances.find (handler_type);
    if (existing_handler != _state->timer_handler_instances.end ()) {
        state->handler_instance = existing_handler->second;
    } else {
        auto *activation_services =
          _state->activation_scope ? &_state->activation_scope->provider () : nullptr;
        auto handler_instance = handler_factory (activation_services);
        if (!handler_instance) {
            throw framework_exception_t (framework_error_kind_t::not_configured,
                                         "SPOT timer handler factory returned null");
        }
        _state->timer_handler_instances.emplace (handler_type, handler_instance);
        state->handler_instance = handler_instance;
    }
    state->handler_invoker = std::move (handler_invoker);
    if (_state->execution_mode == user_spot_execution_mode_t::per_actor) {
        if (const auto coordinator = _state->ensure_spot_serial_executor ())
            state->serial_queue = coordinator->timer_queue (state->name);
    }
    auto native_timer =
      std::make_unique<detail::core_timer_resource_t> (detail::runtime_failures_for (
        _state->state_lane_owner () ? _state->state_lane_owner ()->monitoring : nullptr));
    auto *native_timer_loop = &native_timer->loop ();
    state->native_timer = std::move (native_timer);
    auto context = _state;
    const auto timer_queue = state->serial_queue;
    native_timer_loop->start (period, std::numeric_limits<std::uint64_t>::max (),
                              [context, state, timer_queue] (std::uint64_t fire_count) {
                                  detail::timer_runtime_t::post_fire_count (
                                    context, state, timer_queue, fire_count);
                              });
    if (auto previous = _state->timers.push_back (state)) {
        // Retire the previous generation only after the new timer started. Its cancellation
        // may complete only after the callback performing this replacement returns.
        (void) timer_t (std::move (previous)).cancel ();
    }
    return timer_t (state);
}

} // namespace zlink::framework

namespace zlink::framework::detail
{

std::shared_ptr<timer_state_t> spot_timer_registry_t::push_back (const timer_ptr_t &timer)
{
    std::lock_guard lock (_mutex);
    const auto previous =
      std::find_if (_timers.rbegin (), _timers.rend (), [&timer] (const auto &candidate) {
          return candidate->name == timer->name && !candidate->disposed;
      });
    auto replaced = previous == _timers.rend () ? nullptr : *previous;
    _timers.push_back (timer);
    return replaced;
}

void spot_timer_registry_t::erase (const timer_ptr_t &timer)
{
    std::lock_guard lock (_mutex);
    std::erase (_timers, timer);
}

spot_timer_registry_t::snapshot_t spot_timer_registry_t::snapshot () const
{
    std::lock_guard lock (_mutex);
    return _timers;
}

result_t<void> core_timer_resource_t::cancel () noexcept
{
    std::optional<framework_exception_t> failure;
    if (_loop.valid ()) {
        try {
            _loop.stop ();
        }
        catch (const framework_exception_t &error) {
            failure = error;
        }
        catch (const std::exception &error) {
            failure.emplace (framework_error_kind_t::internal_failure, error.what ());
        }
        catch (...) {
            failure.emplace (framework_error_kind_t::internal_failure,
                             "unknown SPOT timer stop failure");
        }
    }
    try {
        _loop.close ();
    }
    catch (const framework_exception_t &error) {
        if (!failure)
            failure = error;
    }
    catch (const std::exception &error) {
        if (!failure) {
            failure.emplace (framework_error_kind_t::internal_failure, error.what ());
        }
    }
    catch (...) {
        if (!failure) {
            failure.emplace (framework_error_kind_t::internal_failure,
                             "unknown SPOT timer close failure");
        }
    }
    return failure ? result_access_t::failure<void> (std::move (*failure))
                   : result_t<void>::success ();
}

timer_runtime_t::timer_runtime_t (std::shared_ptr<spot_context_state_t> context) :
    _context (std::move (context))
{
}

timer_runtime_t timer_runtime_t::from (spot_context_t &context)
{
    return timer_runtime_t (context._state);
}

void timer_runtime_t::post_fire_count (
  const std::shared_ptr<spot_context_state_t> &context,
  const std::shared_ptr<timer_state_t> &state,
  const std::shared_ptr<runtime::serial_execution_queue_t> &queue,
  std::uint64_t fire_count)
{
    if (!context || !state)
        return;
    std::shared_ptr<timer_callback_admission_t> admission;
    {
        std::lock_guard lock (state->mutex);
        if (state->disposed)
            return;
        const auto available =
          std::numeric_limits<std::uint64_t>::max () - state->pending_fire_count;
        state->pending_fire_count += std::min (fire_count, available);
        if (state->pending_fire || state->running) {
            if (!state->pending_fire)
                state->pending_fire.emplace ();
            return;
        }
        admission = std::make_shared<timer_callback_admission_t> ();
        state->pending_fire.emplace (admission);
    }
    const auto rollback = [&] (void *) noexcept {
        std::lock_guard lock (state->mutex);
        if (state->pending_fire && state->pending_fire->lock ().get () == admission.get ())
            state->pending_fire.reset ();
    };
    std::unique_ptr<void, decltype (rollback)> rollback_pending (state.get (), rollback);
    // Queued work owns the claim. The timer can cancel that claim without
    // running the application lane; active callbacks keep their claim.
    const bool admitted = context->admit_timer_fire (queue);
    timer_callback_admission_t claimed (admitted ? context : nullptr);
    {
        std::lock_guard lock (state->mutex);
        const auto pending = state->pending_fire ? state->pending_fire->lock () : nullptr;
        if (state->disposed || pending.get () != admission.get ())
            return;
        admission->context = std::move (claimed.context);
    }
    auto work = [context, state, admission] (auto complete) mutable {
        framework::timer_t timer (state);
        auto runtime = timer_runtime_t (context);
        auto task = runtime.dispatch_fire_count_async (
          timer, {}, std::optional<const void *>{admission.get ()});
        detail::observe_task_completion (task, [complete, admission = std::move (admission)] (
                                                 const result_t<timer_tick_t> &) mutable {
            admission.reset ();
            complete ([] {});
        });
    };
    const auto queue_name = "spot-timer:" + state->name;
    const bool posted = admitted
                        && (queue ? queue->try_post_async (queue_name, std::move (work))
                                  : context->try_post_serial_async (queue_name, std::move (work)));
    if (posted)
        (void) rollback_pending.release ();
}

namespace
{

template <typename T> void append_observation (std::deque<T> &history, T value)
{
    if (history.size () == timer_state_t::observation_history_limit)
        history.pop_front ();
    history.push_back (std::move (value));
}

std::vector<timer_tick_t> make_ticks (const timer_state_t &state, std::uint64_t fire_count)
{
    const auto previous_index = state.last_scheduled_index;
    const auto available = std::max<std::uint64_t> (1, fire_count);
    const auto due_index = previous_index + available;
    std::uint64_t first_index = due_index;
    std::uint64_t delivery_count = 1;
    if (state.options.overrun_policy == timer_overrun_policy_t::delay_next_tick) {
        first_index = previous_index + 1;
    } else if (state.options.overrun_policy == timer_overrun_policy_t::catch_up_bounded) {
        delivery_count = std::min (available, state.options.max_catch_up_ticks);
        first_index = due_index - delivery_count + 1;
    }

    std::vector<timer_tick_t> ticks;
    ticks.reserve (static_cast<std::size_t> (delivery_count));
    const auto started_elapsed =
      state.options.overrun_policy == timer_overrun_policy_t::delay_next_tick
        ? state.period * (state.delivery_index + 1)
        : state.period * due_index;
    for (std::uint64_t offset = 0; offset < delivery_count; ++offset) {
        const auto scheduled_index = first_index + offset;
        const auto skipped_ticks = offset == 0 ? scheduled_index - previous_index - 1 : 0;
        timer_tick_t tick{state.name,
                          state.delivery_index + offset + 1,
                          scheduled_index,
                          state.period,
                          state.period * scheduled_index,
                          started_elapsed,
                          started_elapsed - state.period * scheduled_index,
                          skipped_ticks};
        ticks.push_back (std::move (tick));
    }
    return ticks;
}

bool try_start_tick (timer_state_t &state, const timer_tick_t &tick)
{
    if (state.disposed.load (std::memory_order_acquire))
        return false;
    state.delivery_index = tick.delivery_index;
    state.last_scheduled_index = tick.scheduled_index;
    append_observation (state.delivered_ticks, tick);
    return true;
}

void record_timer_failure (const std::shared_ptr<spot_context_state_t> &context,
                           const std::shared_ptr<timer_state_t> &state,
                           bool stopped,
                           std::string message)
{
    timer_failure_event_t failure{state->name, state->handler_type, state->delivery_index, stopped,
                                  std::move (message)};
    append_observation (state->failure_events, failure);
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
    const auto state = timer._state;
    if (!state) {
        return detail::boundary_failure<timer_tick_t> (detail::boundary_error_t::closed,
                                                       "SPOT timer is disposed");
    }
    {
        std::lock_guard lock (state->mutex);
        if (state->disposed.load (std::memory_order_acquire)) {
            return detail::boundary_failure<timer_tick_t> (detail::boundary_error_t::closed,
                                                           "SPOT timer is disposed");
        }
        if (state->running) {
            return result_t<timer_tick_t>::failure (framework_error_kind_t::rejected,
                                                    "SPOT timer callback is already running");
        }
        state->running = true;
    }
    if (!_context->admit (true)) {
        {
            std::lock_guard lock (state->mutex);
            state->running = false;
        }
        complete_cancel_if_ready (state);
        return detail::result_access_t::failure<timer_tick_t> (_context->admission_error ());
    }
    auto reset_running = [&state, this] {
        _context->leave_callback ();
        {
            std::lock_guard lock (state->mutex);
            state->running = false;
        }
        complete_cancel_if_ready (state);
    };

    try {
        auto ticks = make_ticks (*state, fire_count);
        timer_tick_t *last_started = nullptr;
        for (auto &tick : ticks) {
            if (!try_start_tick (*state, tick))
                break;
            last_started = &tick;
            if (handler)
                handler (tick);
        }
        reset_running ();
        if (last_started)
            return result_t<timer_tick_t>::success (std::move (*last_started));
        return detail::boundary_failure<timer_tick_t> (detail::boundary_error_t::closed,
                                                       "SPOT timer is disposed");
    }
    catch (const std::exception &error) {
        const auto stopped = state->options.stop_on_unhandled_exception;
        record_timer_failure (_context, state, stopped, error.what ());
        if (stopped) {
            state->disposed = true;
        }
        reset_running ();
        return result_t<timer_tick_t>::failure (framework_error_kind_t::internal_failure,
                                                error.what ());
    }
    catch (...) {
        const auto stopped = state->options.stop_on_unhandled_exception;
        record_timer_failure (_context, state, stopped, "unknown timer handler failure");
        if (stopped) {
            state->disposed = true;
        }
        reset_running ();
        return result_t<timer_tick_t>::failure (framework_error_kind_t::internal_failure,
                                                "unknown timer handler failure");
    }
}

task_t<timer_tick_t> timer_runtime_t::dispatch_fire_count_async (timer_t &timer,
                                                                 std::uint64_t fire_count) const
{
    // A direct dispatch is its own admission (Spot address messaging §7 step 2).
    const auto context = _context;
    if (!context) {
        return task_t<timer_tick_t> (result_t<timer_tick_t>::failure (
          framework_error_kind_t::protocol_error, "SPOT timer context is not configured"));
    }
    if (!context->admit_timer_fire ()) {
        return task_t<timer_tick_t> (
          detail::result_access_t::failure<timer_tick_t> (context->admission_error ()));
    }
    auto task = dispatch_fire_count_async (timer, fire_count, std::nullopt);
    detail::observe_task_completion (
      task, [context] (const result_t<timer_tick_t> &) { context->leave_callback (); });
    return task;
}

task_t<timer_tick_t> timer_runtime_t::dispatch_fire_count_async (
  timer_t &timer, std::uint64_t fire_count, std::optional<const void *> pending_admission) const
{
    auto state = timer._state;
    auto context = _context;
    if (!state) {
        co_return detail::boundary_failure<timer_tick_t> (detail::boundary_error_t::closed,
                                                          "SPOT timer is disposed");
    }
    {
        std::lock_guard lock (state->mutex);
        if (state->disposed.load (std::memory_order_acquire)
            || (pending_admission
                && (!*pending_admission || !state->pending_fire
                    || state->pending_fire->lock ().get () != *pending_admission))) {
            co_return detail::boundary_failure<timer_tick_t> (detail::boundary_error_t::closed,
                                                              "SPOT timer is disposed");
        }
        if (pending_admission) {
            fire_count = std::exchange (state->pending_fire_count, 0);
            state->pending_fire.reset ();
        }
        if (state->running) {
            if (!state->pending_fire)
                state->pending_fire.emplace ();
            const auto available =
              std::numeric_limits<std::uint64_t>::max () - state->pending_fire_count;
            state->pending_fire_count += std::min (fire_count, available);
            co_return result_t<timer_tick_t>::failure (framework_error_kind_t::rejected,
                                                       "SPOT timer callback is already running");
        }
        state->running = true;
    }
    if (!state->handler_invoker) {
        {
            std::lock_guard lock (state->mutex);
            state->running = false;
        }
        complete_cancel_if_ready (state);
        co_return result_t<timer_tick_t>::failure (framework_error_kind_t::protocol_error,
                                                   "SPOT timer handler is not configured");
    }
    if (!context) {
        {
            std::lock_guard lock (state->mutex);
            state->running = false;
        }
        complete_cancel_if_ready (state);
        co_return result_t<timer_tick_t>::failure (framework_error_kind_t::protocol_error,
                                                   "SPOT timer context is not configured");
    }
    auto reset_running = [context, state] {
        bool post_pending = false;
        std::uint64_t pending_fire_count = 0;
        {
            std::lock_guard lock (state->mutex);
            state->running = false;
            post_pending = state->pending_fire && !state->disposed;
            pending_fire_count = state->pending_fire_count;
            state->pending_fire.reset ();
            state->pending_fire_count = 0;
        }
        complete_cancel_if_ready (state);
        if (post_pending) {
            timer_runtime_t::post_fire_count (context, state, state->serial_queue,
                                              pending_fire_count);
        }
    };

    const auto spot_instance = context->spot_instance;
    const auto channel_runtime = context->channel_runtime;
    try {
        auto ticks = make_ticks (*state, fire_count);
        /* Timer callbacks have no inbound message: they start a new flow with
         * origin=timer when tracing capture is enabled (flow-correlation §4.2). */
        auto timer_flow = framework::runtime::flow_context_t::enter_current_or_create (
          flow_origin_t::timer, message_flow_tracer_t (channel_runtime->dispatch).mode ());
        auto handler_instance = state->handler_instance.lock ();
        if (!handler_instance) {
            throw framework_exception_t (framework_error_kind_t::protocol_error,
                                         "SPOT timer handler activation is no longer available");
        }
        timer_tick_t *last_started = nullptr;
        for (auto &tick : ticks) {
            if (!try_start_tick (*state, tick))
                break;
            last_started = &tick;
            auto handler_task = state->handler_invoker (
              spot_instance.get (), handler_instance.get (), *channel_runtime->serializers, tick);
            (void) co_await handler_task;
        }
        reset_running ();
        if (last_started)
            co_return result_t<timer_tick_t>::success (std::move (*last_started));
        co_return detail::boundary_failure<timer_tick_t> (detail::boundary_error_t::closed,
                                                          "SPOT timer is disposed");
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
    for (const auto &timer : context.timers.snapshot ()) {
        framework::timer_t handle (timer);
        (void) handle.cancel ();
    }
    // A retired timer's named queue can still hold work that has to be discarded.
    if (const auto coordinator = context.spot_serial_executor)
        coordinator->cancel_timers ();
}

void timer_test_access_t::finish_callback (const std::shared_ptr<timer_state_t> &state)
{
    {
        std::lock_guard lock (state->mutex);
        state->running = false;
    }
    complete_cancel_if_ready (state);
}

std::vector<timer_failure_event_t> timer_runtime_t::failure_events (const timer_t &timer) const
{
    if (!timer._state) {
        return {};
    }
    return {timer._state->failure_events.begin (), timer._state->failure_events.end ()};
}

std::vector<timer_tick_t> timer_runtime_t::delivered_ticks (const timer_t &timer) const
{
    if (!timer._state) {
        return {};
    }
    return {timer._state->delivered_ticks.begin (), timer._state->delivered_ticks.end ()};
}

} // namespace zlink::framework::detail
