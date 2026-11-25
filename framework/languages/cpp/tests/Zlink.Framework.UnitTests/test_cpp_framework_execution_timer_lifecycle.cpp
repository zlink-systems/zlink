/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/execution_cases.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/execution/serial_execution_queue.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include "runtime/timers/core_timer_drain_loop.hpp"
#include "runtime/timers/timer_runtime.hpp"
#include <zlink/framework/contracts/spots/spot.hpp>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <string_view>
#include <utility>
#include <vector>

namespace zlink::framework::test::execution
{
struct timer_activation_dependency_t
{
    timer_activation_dependency_t () { ++created; }
    ~timer_activation_dependency_t () { ++destroyed; }

    static inline std::atomic_int created{0};
    static inline std::atomic_int destroyed{0};
};

struct timer_activation_spot_t
{
};

struct timer_activation_handler_t
{
    explicit timer_activation_handler_t (timer_activation_dependency_t &dependency) :
        dependency (&dependency)
    {
        ++created;
    }

    ~timer_activation_handler_t () { ++destroyed; }

    zlink::framework::task_t<void> handle (timer_activation_spot_t &,
                                           const zlink::framework::timer_tick_t &)
    {
        auto *expected = static_cast<timer_activation_dependency_t *> (nullptr);
        if (!observed_dependency.compare_exchange_strong (expected, dependency)
            && expected != dependency) {
            dependency_mismatch = true;
        }
        ++calls;
        co_return;
    }

    timer_activation_dependency_t *dependency;
    static inline std::atomic_int created{0};
    static inline std::atomic_int destroyed{0};
    static inline std::atomic_int calls{0};
    static inline std::atomic<timer_activation_dependency_t *> observed_dependency{nullptr};
    static inline std::atomic_bool dependency_mismatch{false};
};

bool verify_timer_handler_activation_lifetime ()
{
    timer_activation_dependency_t::created = 0;
    timer_activation_dependency_t::destroyed = 0;
    timer_activation_handler_t::created = 0;
    timer_activation_handler_t::destroyed = 0;
    timer_activation_handler_t::calls = 0;
    timer_activation_handler_t::observed_dependency = nullptr;
    timer_activation_handler_t::dependency_mismatch = false;

    zlink::framework::service_collection_t services;
    services.add_scoped<timer_activation_dependency_t> ();
    auto root = services.build_provider ();
    zlink::framework::serializer_registry_t serializers;

    auto run_activation = [&] {
        const auto handlers_before = timer_activation_handler_t::created.load ();
        const auto dependencies_before = timer_activation_dependency_t::created.load ();
        const auto calls_before = timer_activation_handler_t::calls.load ();
        timer_activation_handler_t::observed_dependency = nullptr;
        timer_activation_handler_t::dependency_mismatch = false;
        auto state = std::make_shared<zlink::framework::detail::spot_context_state_t> ();
        state->activation_scope = std::make_shared<zlink::framework::detail::service_scope_t> (
          zlink::framework::detail::service_scope_t::create (
            root, zlink::framework::detail::service_scope_kind_t::spot_activation));
        state->channel_runtime = zlink::framework::test::runtime_failure_fixture<
          zlink::framework::detail::channel_runtime_state_t> ();
        state->node = std::make_shared<zlink::framework::detail::spot_node_builder_state_t> (
          "timer-activation");
        state->node->monitoring = state->channel_runtime->monitoring;
        state->channel_runtime->serializers = &serializers;
        state->spot_instance = std::make_shared<timer_activation_spot_t> ();

        auto context = zlink::framework::detail::spot_context_access_t::create (state);
        auto first =
          context.add_timer<timer_activation_handler_t> ("first", std::chrono::hours (24));
        auto second =
          context.add_timer<timer_activation_handler_t> ("second", std::chrono::hours (24));

        auto timer_runtime = zlink::framework::detail::timer_runtime_t::from (context);
        const auto first_result = timer_runtime.dispatch_fire_count_async (first, 1).result ();
        const auto second_result = timer_runtime.dispatch_fire_count_async (second, 1).result ();
        if (!first_result || !second_result) {
            std::cerr << "timer activation dispatch failed: "
                      << static_cast<int> (first_result.error_kind ()) << ", "
                      << static_cast<int> (second_result.error_kind ()) << '\n';
            return false;
        }
        zlink::framework::timer_options_t catch_up_options;
        catch_up_options.overrun_policy =
          zlink::framework::timer_overrun_policy_t::catch_up_bounded;
        catch_up_options.max_catch_up_ticks = 3;
        auto catch_up = context.add_timer<timer_activation_handler_t> (
          "catch-up", std::chrono::hours (24), catch_up_options);
        std::vector<zlink::framework::timer_tick_t> caught_up;
        const auto catch_up_result = timer_runtime.dispatch_fire_count (
          catch_up, 5,
          [&] (const zlink::framework::timer_tick_t &tick) { caught_up.push_back (tick); });
        if (!catch_up_result || caught_up.size () != 3 || caught_up[0].scheduled_index != 3
            || caught_up[0].skipped_ticks != 2 || caught_up[1].scheduled_index != 4
            || caught_up[2].scheduled_index != 5) {
            std::cerr << "bounded timer catch-up mismatch\n";
            return false;
        }
        for (int index = 0; index < 300; ++index) {
            if (!timer_runtime.dispatch_fire_count (catch_up, 1)) {
                return false;
            }
        }
        const auto tick_history = timer_runtime.delivered_ticks (catch_up);
        if (tick_history.size ()
              != zlink::framework::detail::timer_state_t::observation_history_limit
            || tick_history.back ().scheduled_index != 305) {
            std::cerr << "timer observation history must stay bounded\n";
            return false;
        }

        bool oversized_catch_up_rejected = false;
        try {
            auto invalid_options = catch_up_options;
            invalid_options.max_catch_up_ticks =
              static_cast<std::uint64_t> (std::numeric_limits<int>::max ()) + 1;
            (void) context.add_timer<timer_activation_handler_t> (
              "invalid-catch-up", std::chrono::hours (24), invalid_options);
        }
        catch (const zlink::framework::framework_exception_t &error) {
            oversized_catch_up_rejected =
              error.kind () == zlink::framework::framework_error_kind_t::protocol_error;
        }
        if (!oversized_catch_up_rejected) {
            std::cerr << "oversized timer catch-up count must be rejected\n";
            return false;
        }
        const auto reused =
          timer_activation_handler_t::created.load () == handlers_before + 1
          && timer_activation_dependency_t::created.load () == dependencies_before + 1
          && timer_activation_handler_t::calls.load () >= calls_before + 2
          && timer_activation_handler_t::observed_dependency.load () != nullptr
          && !timer_activation_handler_t::dependency_mismatch.load ();
        if (!reused) {
            std::cerr << "timer activation reuse mismatch: handlers="
                      << timer_activation_handler_t::created.load ()
                      << " deps=" << timer_activation_dependency_t::created.load ()
                      << " calls=" << timer_activation_handler_t::calls.load () << '\n';
            return false;
        }

        state->detach_application_instance (false);
        const auto released = timer_activation_handler_t::destroyed.load ()
                                == timer_activation_handler_t::created.load ()
                              && timer_activation_dependency_t::destroyed.load ()
                                   == timer_activation_dependency_t::created.load ();
        if (!released) {
            std::cerr << "timer activation release mismatch: handlers="
                      << timer_activation_handler_t::created.load () << "/"
                      << timer_activation_handler_t::destroyed.load ()
                      << " deps=" << timer_activation_dependency_t::created.load () << "/"
                      << timer_activation_dependency_t::destroyed.load () << '\n';
        }
        return released;
    };

    if (!run_activation () || timer_activation_handler_t::created.load () != 1) {
        return false;
    }
    if (!run_activation () || timer_activation_handler_t::created.load () != 2) {
        return false;
    }
    return timer_activation_handler_t::destroyed.load () == 2
           && timer_activation_dependency_t::destroyed.load () == 2;
}

bool verify_throwing_timer_callback_preserves_default_policy ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    serializer_registry_t serializers;
    auto state = std::make_shared<spot_context_state_t> ();
    state->spot_instance = std::make_shared<int> (1);
    state->channel_runtime =
      zlink::framework::test::runtime_failure_fixture<channel_runtime_state_t> ();
    state->channel_runtime->serializers = &serializers;

    auto timer_state = std::make_shared<timer_state_t> ();
    timer_state->name = "throwing-callback";
    timer_state->handler_type = std::type_index (typeid (int));
    auto handler_instance = std::make_shared<int> (1);
    timer_state->handler_instance = handler_instance;
    timer_state->handler_invoker = [] (void *, void *, serializer_registry_t &,
                                       const timer_tick_t &) -> task_t<zlink::message_t> {
        throw std::runtime_error ("timer callback failed");
        co_return zlink::message_t{};
    };

    auto timer = timer_test_access_t::create (timer_state);
    timer_runtime_t runtime (state);
    const auto first = runtime.dispatch_fire_count_async (timer, 1).result ();
    const auto first_failures = runtime.failure_events (timer);
    if (first || first.error_kind () != framework_error_kind_t::internal_failure
        || timer.is_disposed () || first_failures.size () != 1 || first_failures[0].stopped
        || first_failures[0].message != "timer callback failed") {
        return false;
    }

    const auto second = runtime.dispatch_fire_count_async (timer, 1).result ();
    const auto second_failures = runtime.failure_events (timer);
    return !second && second.error_kind () == framework_error_kind_t::internal_failure
           && !timer.is_disposed () && second_failures.size () == 2
           && second_failures[1].delivery_index == first_failures[0].delivery_index + 1;
}

struct timer_cleanup_probe_t
{
    std::atomic_int stop_calls{0};
    std::atomic_int close_calls{0};
    bool fail_close = false;
};

class controlled_timer_resource_t final : public zlink::framework::detail::timer_resource_t
{
  public:
    explicit controlled_timer_resource_t (std::shared_ptr<timer_cleanup_probe_t> probe) :
        _probe (std::move (probe))
    {
    }

    zlink::framework::result_t<void> cancel () noexcept override
    {
        ++_probe->stop_calls;
        ++_probe->close_calls;
        if (_probe->fail_close) {
            return zlink::framework::result_t<void>::failure (
              zlink::framework::framework_error_kind_t::internal_failure,
              "controlled timer cleanup failure");
        }
        return zlink::framework::result_t<void>::success ();
    }

  private:
    std::shared_ptr<timer_cleanup_probe_t> _probe;
};

std::vector<zlink::framework::task_t<void>> cancel_concurrently (zlink::framework::timer_t timer,
                                                                 int caller_count)
{
    std::barrier start (caller_count + 1);
    std::vector<std::future<zlink::framework::task_t<void>>> futures;
    futures.reserve (static_cast<std::size_t> (caller_count));
    for (int index = 0; index < caller_count; ++index) {
        futures.push_back (std::async (std::launch::async, [timer, &start] () mutable {
            start.arrive_and_wait ();
            return timer.cancel ();
        }));
    }
    start.arrive_and_wait ();

    std::vector<zlink::framework::task_t<void>> tasks;
    tasks.reserve (futures.size ());
    for (auto &future : futures)
        tasks.push_back (future.get ());
    return tasks;
}

bool verify_timer_cancel_generation_contract ()
{
    using zlink::framework::detail::timer_state_t;
    using zlink::framework::detail::timer_test_access_t;

    {
        auto probe = std::make_shared<timer_cleanup_probe_t> ();
        auto state = std::make_shared<timer_state_t> ();
        state->native_timer = std::make_unique<controlled_timer_resource_t> (probe);
        auto tasks = cancel_concurrently (timer_test_access_t::create (state), 8);
        if (probe->stop_calls.load () != 1 || probe->close_calls.load () != 1) {
            return false;
        }
        const auto *terminal = &tasks.front ().result ();
        for (const auto &task : tasks) {
            if (!task.await_ready () || !task.result () || &task.result () != terminal) {
                return false;
            }
        }
    }

    {
        auto probe = std::make_shared<timer_cleanup_probe_t> ();
        auto state = std::make_shared<timer_state_t> ();
        state->running = true;
        state->native_timer = std::make_unique<controlled_timer_resource_t> (probe);
        auto timer = timer_test_access_t::create (state);
        auto cancellation = timer.cancel ();
        if (probe->stop_calls.load () != 1 || probe->close_calls.load () != 1
            || cancellation.await_ready ()) {
            return false;
        }
        timer_test_access_t::finish_callback (state);
        if (!cancellation.await_ready () || !cancellation.result ()) {
            return false;
        }
    }

    {
        auto probe = std::make_shared<timer_cleanup_probe_t> ();
        probe->fail_close = true;
        auto state = std::make_shared<timer_state_t> ();
        state->running = true;
        state->native_timer = std::make_unique<controlled_timer_resource_t> (probe);
        auto tasks = cancel_concurrently (timer_test_access_t::create (state), 8);
        if (probe->stop_calls.load () != 1 || probe->close_calls.load () != 1) {
            return false;
        }
        for (const auto &task : tasks) {
            if (task.await_ready ()) {
                return false;
            }
        }
        timer_test_access_t::finish_callback (state);
        const auto *terminal = &tasks.front ().result ();
        for (const auto &task : tasks) {
            const auto &result = task.result ();
            if (!task.await_ready () || result || &result != terminal
                || result.error_kind ()
                     != zlink::framework::framework_error_kind_t::internal_failure
                || !result.error ()
                || std::string_view (result.error ()->what ())
                     != "controlled timer cleanup failure") {
                return false;
            }
        }
    }
    return true;
}

bool verify_close_waits_for_timer_callback_barrier ()
{
    const auto timer_calls_before = timer_activation_handler_t::calls.load ();
    const auto handlers_created_before = timer_activation_handler_t::created.load ();
    const auto handlers_destroyed_before = timer_activation_handler_t::destroyed.load ();
    const auto dependencies_created_before = timer_activation_dependency_t::created.load ();
    const auto dependencies_destroyed_before = timer_activation_dependency_t::destroyed.load ();

    zlink::framework::service_collection_t services;
    services.add_scoped<timer_activation_dependency_t> ();
    auto root = services.build_provider ();
    auto state = std::make_shared<zlink::framework::detail::spot_context_state_t> ();
    auto node_released = std::make_shared<std::promise<void>> ();
    auto node_release = node_released->get_future ();
    auto node = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::spot_node_builder_state_t> ("timer-close-race");
    auto *node_pointer = node.get ();
    state->node = std::shared_ptr<zlink::framework::detail::spot_node_builder_state_t> (
      node_pointer, [node = std::move (node), node_released] (auto *) mutable {
          node.reset ();
          node_released->set_value ();
      });
    const auto node_lifetime = std::weak_ptr (state->node);
    auto executor = std::make_shared<zlink::framework::runtime::offload_executor_t> (1);
    state->serial_executor = executor;
    state->serial_queue = std::make_shared<zlink::framework::runtime::serial_execution_queue_t> (
      *executor, zlink::framework::runtime::serial_execution_queue_options_t{});
    state->spot_id = "timer-close-race-spot";
    state->spot_instance = std::make_shared<timer_activation_spot_t> ();
    state->activation_scope = std::make_shared<zlink::framework::detail::service_scope_t> (
      zlink::framework::detail::service_scope_t::create (
        root, zlink::framework::detail::service_scope_kind_t::spot_activation));
    auto handler = std::make_shared<timer_activation_handler_t> (
      state->activation_scope->provider ().get_required<timer_activation_dependency_t> ());
    state->timer_handler_instances.emplace (std::type_index (typeid (timer_activation_handler_t)),
                                            handler);
    handler.reset ();

    auto context = zlink::framework::detail::spot_context_access_t::create (state);
    zlink::framework::serializer_registry_t serializers;
    state->channel_runtime = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::channel_runtime_state_t> ();
    state->channel_runtime->serializers = &serializers;
    auto pending_timer = std::make_shared<zlink::framework::detail::timer_state_t> ();
    pending_timer->name = "queued-before-close";
    pending_timer->serial_queue = state->serial_queue;
    pending_timer->handler_instance =
      state->timer_handler_instances.at (std::type_index (typeid (timer_activation_handler_t)));
    auto cleanup = std::make_shared<timer_cleanup_probe_t> ();
    pending_timer->native_timer = std::make_unique<controlled_timer_resource_t> (cleanup);
    pending_timer->handler_invoker =
      [] (
        void *spot, void *handler, zlink::framework::serializer_registry_t &,
        const zlink::framework::timer_tick_t &tick) -> zlink::framework::task_t<zlink::message_t> {
        co_await static_cast<timer_activation_handler_t *> (handler)->handle (
          *static_cast<timer_activation_spot_t *> (spot), tick);
        co_return zlink::message_t{};
    };
    state->timers.push_back (pending_timer);
    auto timer = zlink::framework::detail::timer_test_access_t::create (pending_timer);
    std::atomic_int closing_calls{0};
    bool tick_queued = false;
    const auto fail = [&] (const char *condition, bool node_expired) {
        std::cerr << "timer-close-barrier condition=" << condition
                  << " calls=" << timer_activation_handler_t::calls.load () - timer_calls_before
                  << " handlers="
                  << timer_activation_handler_t::created.load () - handlers_created_before << "/"
                  << timer_activation_handler_t::destroyed.load () - handlers_destroyed_before
                  << " dependencies="
                  << timer_activation_dependency_t::created.load () - dependencies_created_before
                  << "/"
                  << timer_activation_dependency_t::destroyed.load ()
                       - dependencies_destroyed_before
                  << " closed=" << state->closed.has_value ()
                  << " closing_calls=" << closing_calls.load ()
                  << " pending=" << state->serial_queue->pending_count () << " application_pending="
                  << state->serial_queue->pending_count (
                       zlink::framework::runtime::serial_work_lane_t::application)
                  << " tick_queued=" << tick_queued << " node_expired=" << node_expired
                  << " disposed=" << timer.is_disposed () << " stop=" << cleanup->stop_calls.load ()
                  << " close=" << cleanup->close_calls.load () << '\n';
        return false;
    };
    // 동일한 admission 경로의 정상 tick이 실제 handler를 호출하는지 먼저 확인한다.
    zlink::framework::detail::timer_runtime_t::post_fire_count (state, pending_timer,
                                                                state->serial_queue, 1);
    state->drain_serial ();
    if (timer_activation_handler_t::calls.load () != timer_calls_before + 1 || timer.is_disposed ())
        return fail ("normal-tick", node_lifetime.expired ());
    state->lifecycle.on_closing = [&] (void *, const zlink::framework::spot_closing_context_t &,
                                       std::stop_token) {
        ++closing_calls;
        return zlink::framework::task_t<void> (zlink::framework::result_t<void>::success ());
    };
    using completion_t = zlink::framework::runtime::serial_execution_queue_t::async_completion_t;
    std::promise<completion_t> entered;
    auto terminal = entered.get_future ();
    std::atomic_bool admitted{false};
    if (!state->serial_queue->try_post_async ("active-timer-handler-turn",
                                              [state, &entered, &admitted] (completion_t complete) {
                                                  admitted = state->admit (true);
                                                  entered.set_value (std::move (complete));
                                              })) {
        return fail ("submit-active-turn", node_lifetime.expired ());
    }
    auto complete = terminal.get ();
    if (!admitted) {
        complete ([] {});
        state->drain_serial ();
        return fail ("admit-active-turn", node_lifetime.expired ());
    }

    context.close ();
    context.close ();
    const auto pending_before = state->serial_queue->pending_count (
      zlink::framework::runtime::serial_work_lane_t::application);
    zlink::framework::detail::timer_runtime_t::post_fire_count (state, pending_timer,
                                                                state->serial_queue, 1);
    tick_queued = state->serial_queue->pending_count (
                    zlink::framework::runtime::serial_work_lane_t::application)
                  == pending_before + 1;
    if (state->closed || closing_calls != 0
        || timer_activation_handler_t::destroyed.load () != handlers_destroyed_before
        || timer_activation_dependency_t::destroyed.load () != dependencies_destroyed_before) {
        const bool failed = fail ("close-before-turn-terminal", node_lifetime.expired ());
        complete ([state] { state->leave_callback (); });
        state->drain_serial ();
        return failed;
    }

    complete ([state] { state->leave_callback (); });
    state->drain_serial ();
    if (!state->closed || closing_calls != 1 || state->activation_scope
        || !state->timer_handler_instances.empty ()
        || timer_activation_handler_t::created.load () != handlers_created_before + 1
        || timer_activation_handler_t::destroyed.load () != handlers_destroyed_before + 1
        || timer_activation_dependency_t::created.load () != dependencies_created_before + 1
        || timer_activation_dependency_t::destroyed.load () != dependencies_destroyed_before + 1) {
        return fail ("close-after-drain", node_lifetime.expired ());
    }

    // Queue drain observes turn retirement, not destruction of completion captures.
    // Wait for the final node release before checking its weak pointer.
    if (node_release.wait_for (std::chrono::seconds (30)) != std::future_status::ready)
        return fail ("node-not-released", node_lifetime.expired ());
    const bool node_expired = node_lifetime.expired ();
    const bool released =
      node_expired && tick_queued && timer.is_disposed () && cleanup->stop_calls == 1
      && cleanup->close_calls == 1 && state->serial_queue->pending_count () == 0
      && timer_activation_handler_t::calls.load () == timer_calls_before + 1
      && timer_activation_handler_t::destroyed.load () == handlers_destroyed_before + 1
      && timer_activation_dependency_t::destroyed.load () == dependencies_destroyed_before + 1;
    return released || fail ("released-resources", node_expired);
}
bool verify_timer_terminal_precedes_lifecycle_close_turn ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    serializer_registry_t serializers;
    // Declared before the state so the queued close turn never outlives them.
    std::atomic_bool close_requested{false};
    std::atomic_bool closing_called{false};
    std::atomic_bool cancel_completed_in_closing{false};
    auto state = std::make_shared<spot_context_state_t> ();
    state->node = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::spot_node_builder_state_t> ("timer-terminal-before-close");
    auto executor = std::make_shared<runtime::offload_executor_t> (1);
    state->serial_executor = executor;
    state->serial_queue = std::make_shared<runtime::serial_execution_queue_t> (
      *executor, runtime::serial_execution_queue_options_t{});
    state->spot_id = "timer-terminal-before-close-spot";
    state->spot_name = "timer-terminal-before-close-player";
    state->lifecycle_domain = spot_lifecycle_domain_t::instance ();
    state->object_generation = 1;
    state->authority_owner_generation = 1;
    state->spot_instance = std::make_shared<int> (1);
    state->channel_runtime =
      zlink::framework::test::runtime_failure_fixture<channel_runtime_state_t> ();
    state->channel_runtime->serializers = &serializers;
    auto timer_state = std::make_shared<timer_state_t> ();
    timer_state->name = "closing";
    auto handler_instance = std::make_shared<int> (1);
    timer_state->handler_instance = handler_instance;
    state->timers.push_back (timer_state);
    auto timer = timer_test_access_t::create (timer_state);

    state->lifecycle.on_closing = [timer_state, &closing_called, &cancel_completed_in_closing] (
                                    void *, const spot_closing_context_t &, std::stop_token) {
        closing_called.store (true, std::memory_order_release);
        auto cancellation = timer_test_access_t::create (timer_state).cancel ();
        const auto result = cancellation.result ();
        cancel_completed_in_closing.store (bool (result), std::memory_order_release);
        return task_t<void> (result_t<void>::success ());
    };
    timer_state->handler_invoker =
      [weak_state = std::weak_ptr<spot_context_state_t> (state), &close_requested] (
        void *, void *, serializer_registry_t &, const timer_tick_t &) -> task_t<zlink::message_t> {
        const auto owner = weak_state.lock ();
        if (!owner)
            throw std::logic_error ("timer fixture lost its Spot context");
        auto context = spot_context_access_t::create (owner);
        context.close ();
        close_requested.store (true, std::memory_order_release);
        co_return zlink::message_t{};
    };

    auto dispatch = std::async (std::launch::async, [state, timer = std::move (timer)] () mutable {
        auto runtime = timer_runtime_t (state);
        return runtime.dispatch_fire_count_async (timer, 1).result ();
    });
    if (dispatch.wait_for (std::chrono::seconds (1)) != std::future_status::ready) {
        // Release the intentionally reproduced old-order deadlock so the test
        // can fail within its finite wait instead of leaving its worker blocked.
        timer_test_access_t::finish_callback (timer_state);
        if (dispatch.wait_for (std::chrono::seconds (1)) != std::future_status::ready)
            return false;
        (void) dispatch.get ();
        return false;
    }

    const auto result = dispatch.get ();
    state->drain_serial ();
    return result && close_requested.load (std::memory_order_acquire)
           && closing_called.load (std::memory_order_acquire)
           && cancel_completed_in_closing.load (std::memory_order_acquire);
}

bool verify_timer_self_cancel_completes_after_callback ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    serializer_registry_t serializers;
    auto state = std::make_shared<spot_context_state_t> ();
    state->spot_instance = std::make_shared<int> (1);
    state->channel_runtime =
      zlink::framework::test::runtime_failure_fixture<channel_runtime_state_t> ();
    state->channel_runtime->serializers = &serializers;
    auto timer_state = std::make_shared<timer_state_t> ();
    timer_state->name = "self-cancel";
    auto handler_instance = std::make_shared<int> (1);
    timer_state->handler_instance = handler_instance;
    auto timer = timer_test_access_t::create (timer_state);

    std::atomic_bool callback_completed{false};
    std::atomic_bool cancel_was_pending_in_callback{false};
    timer_state->handler_invoker =
      [weak_timer = std::weak_ptr<timer_state_t> (timer_state), &callback_completed,
       &cancel_was_pending_in_callback] (void *, void *, serializer_registry_t &,
                                         const timer_tick_t &) -> task_t<zlink::message_t> {
        const auto owner = weak_timer.lock ();
        if (!owner)
            throw std::logic_error ("timer fixture lost its timer state");
        auto cancellation = timer_test_access_t::create (owner).cancel ();
        cancel_was_pending_in_callback.store (!cancellation.await_ready (),
                                              std::memory_order_release);
        callback_completed.store (true, std::memory_order_release);
        co_return zlink::message_t{};
    };

    auto dispatch = std::async (std::launch::async, [state, timer = std::move (timer)] () mutable {
        auto runtime = timer_runtime_t (state);
        return runtime.dispatch_fire_count_async (timer, 1).result ();
    });
    if (dispatch.wait_for (std::chrono::seconds (1)) != std::future_status::ready) {
        timer_test_access_t::finish_callback (timer_state);
        if (dispatch.wait_for (std::chrono::seconds (1)) != std::future_status::ready)
            return false;
        (void) dispatch.get ();
        return false;
    }

    const auto dispatch_result = dispatch.get ();
    auto cancellation = timer_test_access_t::create (timer_state).cancel ();
    return dispatch_result && callback_completed.load (std::memory_order_acquire)
           && cancel_was_pending_in_callback.load (std::memory_order_acquire)
           && cancellation.await_ready () && cancellation.result ();
}
bool verify_idle_timer_closes_before_executor_releases_last_node_reference ()
{
    using namespace zlink::framework::detail;
    auto state = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::spot_node_builder_state_t> ("idle-timer-lifetime-node");
    auto timer_worker = std::make_shared<zlink::framework::runtime::offload_executor_t> ();
    std::weak_ptr<zlink::framework::runtime::offload_executor_t> weak_timer_worker = timer_worker;
    state->worker_executor = timer_worker;
    state->instance_spot_idle_timer = std::make_unique<core_timer_drain_loop_t> (
      std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ());
    state->instance_spot_idle_timer->start (std::chrono::hours (1), 1,
                                            [timer_worker] (std::uint64_t) {});
    timer_worker.reset ();
    spot_node_runtime_t (state).cancel_pending_work ();
    spot_node_runtime_t (state).detach_native_node ();
    if (state->instance_spot_idle_timer || !weak_timer_worker.expired ())
        return false;

    zlink::framework::runtime::offload_executor_t executor;
    std::weak_ptr<spot_node_builder_state_t> weak_state = state;
    auto finished = std::make_shared<std::promise<void>> ();
    auto completion = finished->get_future ();
    auto last_owner = std::move (state);
    if (!executor.try_submit_internal ([last_owner = std::move (last_owner), finished] () mutable {
            last_owner.reset ();
            finished->set_value ();
        }))
        return false;
    completion.wait ();
    return weak_state.expired ();
}
}
