/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/m6c_runtime_cases.hpp"
#include "../support/m6c_spot_turn.hpp"
#include "../support/m6c_task.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include "runtime/timers/timer_runtime.hpp"
#include <zlink/framework/contracts/configuration/app.hpp>

#include <atomic>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <exception>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace m6c_test
{

using namespace zlink::framework::runtime::stateful;

static_assert (requires (zlink::framework::spot_context_t &context) {
    { context.relocation_ready () } -> std::same_as<zlink::framework::spot_relocation_ready_call_t>;
});
static_assert (requires (zlink::framework::spot_relocation_ready_call_t &call) {
    { call.defer () } -> std::same_as<void>;
});
static_assert (!std::copy_constructible<zlink::framework::spot_relocation_ready_call_t>);

void test_spot_lifecycle_domain_rejects_invalid_kind_combinations (test_context_t &test)
{
    namespace detail = zlink::framework::detail;

    static_assert (!std::default_initializable<detail::spot_lifecycle_domain_t>);

    const auto entry = detail::spot_lifecycle_domain_t::entry ();
    const auto user = detail::spot_lifecycle_domain_t::user ();
    const auto instance = detail::spot_lifecycle_domain_t::instance ();
    test.require (entry.is_entry () && !entry.allows_relocation ()
                    && !entry.allows_idle_eviction (),
                  "Entry Spot domain must exclude relocation and idle eviction");
    test.require (!user.is_entry () && user.allows_relocation () && !user.allows_idle_eviction (),
                  "User Spot domain must allow relocation but exclude idle eviction");
    test.require (!instance.is_entry () && instance.allows_relocation ()
                    && instance.allows_idle_eviction (),
                  "Instance Spot domain must own relocation and idle eviction rules");
}

#if 0
struct host_relocation_ready_message_t
{
    int value = 0;
};

class host_relocation_spot_t final
    : public zlink::framework::spot_t<
        zlink::framework::actor_t>
{
  public:
    explicit host_relocation_spot_t (
      zlink::framework::spot_context_t context) :
        _context (std::move (context))
    {
    }

    zlink::framework::spot_context_t &context () noexcept override
    {
        return _context;
    }

    const zlink::framework::spot_context_t &
    context () const noexcept override
    {
        return _context;
    }

    void configure () override
    {
        _context.handlers ().add_handler<
          &host_relocation_spot_t::on_ready> (
            "host-relocation-ready");
    }

    zlink::framework::task_t<void> on_ready (
      const host_relocation_ready_message_t &)
    {
        _context.relocation_ready ().defer ();
        co_return;
    }

    zlink::framework::task_t<
      zlink::framework::spot_create_response_t>
    on_create (
      const zlink::framework::message_t &) override
    {
        co_return zlink::framework::spot_create_response_t::
          accept ();
    }

    zlink::framework::task_t<void>
    on_initialize () override
    {
        co_return;
    }

    zlink::framework::task_t<
      zlink::framework::spot_actor_join_result_t>
    on_actor_join (
      std::string_view,
      const zlink::framework::message_t &) override
    {
        co_return zlink::framework::
          spot_actor_join_result_t::reject ();
    }

    zlink::framework::task_t<void>
    on_actor_joined (
      zlink::framework::actor_t &) override
    {
        co_return;
    }

    zlink::framework::task_t<void>
    on_leave_actor (
      zlink::framework::actor_t &) override
    {
        co_return;
    }

    zlink::framework::task_t<void>
    on_relocation_ready_completed (
      const zlink::framework::
        spot_relocation_ready_completion_t &completion) override
    {
        last_outcome.store (
          static_cast<int> (completion.outcome),
          std::memory_order_release);
        completion_count.fetch_add (
          1, std::memory_order_acq_rel);
        co_return;
    }

    static inline std::atomic_int completion_count{0};
    static inline std::atomic_int last_outcome{-1};

  private:
    zlink::framework::spot_context_t _context;
};

class host_relocation_spot_adapter_t final
    : public zlink::framework::spot_relocation_adapter_t<
        host_relocation_spot_t>
{
  public:
    zlink::framework::task_t<std::vector<std::byte>>
    capture (
      host_relocation_spot_t &,
      std::stop_token) override
    {
        co_return std::vector<std::byte>{
          std::byte{0x51}, std::byte{0x52}};
    }

    zlink::framework::task_t<void>
    restore (
      host_relocation_spot_t &,
      std::vector<std::byte> payload,
      std::stop_token) override
    {
        restored =
          payload
          == std::vector<std::byte>{
            std::byte{0x51}, std::byte{0x52}};
        co_return;
    }

    static inline std::atomic_bool restored{false};
};

class host_relocation_source_service_t final
    : public zlink::framework::hosted_service_t
{
  public:
    host_relocation_source_service_t (
      zlink::framework::app_t &app,
      std::shared_ptr<std::atomic_bool> send_ready) :
        _app (&app), _send_ready (std::move (send_ready))
    {
    }

    void start (
      zlink::framework::service_provider_t &services) override
    {
        auto &manager = services.get_required<
          zlink::framework::spot_manager_t> ();
        const auto created =
          manager.get_or_create (
            zlink::framework::spot_id_t (
              "host-relocation-spot"),
            "host-relocation-spot")
            .timeout (std::chrono::seconds (5))
            .async ().result ();
        if (!created) {
            error = created.error ()
              ? created.error ()->what ()
              : "host relocation Spot create failed";
            return;
        }
        created_spot.store (true, std::memory_order_release);
        auto client =
          _app->advanced ().zlink ().route_client (
            services.get_required<
              zlink::framework::serializer_registry_t> ());
        _sender = std::thread (
          [this, client = std::move (client)] () mutable {
              while (!_stop.load (
                       std::memory_order_acquire)
                     && !_send_ready->load (
                       std::memory_order_acquire))
                  std::this_thread::sleep_for (
                    std::chrono::milliseconds (1));
              if (_stop.load (std::memory_order_acquire))
                  return;
              const auto deadline =
                std::chrono::steady_clock::now ()
                + std::chrono::seconds (5);
              while (std::chrono::steady_clock::now ()
                       < deadline) {
                  const auto sent =
                    client.send_to_spot (
                      zlink::framework::spot_id_t (
                        "host-relocation-spot"),
                      host_relocation_ready_message_t{1})
                      .async ().result ();
                  if (sent) {
                      ready_sent.store (
                        true, std::memory_order_release);
                      return;
                  }
                  std::this_thread::sleep_for (
                    std::chrono::milliseconds (10));
              }
              error =
                "host relocation readiness message was not submitted";
          });
    }

    void stop () noexcept override
    {
        _stop.store (true, std::memory_order_release);
        _send_ready->store (true, std::memory_order_release);
        if (_sender.joinable ())
            _sender.join ();
    }

    std::atomic_bool created_spot{false};
    std::atomic_bool ready_sent{false};
    std::string error;

  private:
    zlink::framework::app_t *_app;
    std::shared_ptr<std::atomic_bool> _send_ready;
    std::atomic_bool _stop{false};
    std::thread _sender;
};

bool wait_until (
  const std::function<bool ()> &condition,
  std::chrono::milliseconds timeout)
{
    const auto deadline =
      std::chrono::steady_clock::now () + timeout;
    while (!condition ()
           && std::chrono::steady_clock::now () < deadline)
        std::this_thread::sleep_for (
          std::chrono::milliseconds (1));
    return condition ();
}

void configure_host_relocation_app (
  zlink::framework::app_t &app,
  const std::shared_ptr<
    zlink::framework::runtime::in_memory_location_store_t>
    &location_store,
  const std::shared_ptr<
    zlink::framework::runtime::in_memory_relocation_store_t>
    &relocation_store,
  std::string routing_id)
{
    app.add_zlink_framework (
      [location_store, relocation_store,
       routing_id = std::move (routing_id)] (
        zlink::framework::zlink_framework_options_t &options) {
          options.configure_locations ().owner_lease_renew_interval = std::chrono::milliseconds (10);
          options.configure_locations ().owner_lease_fencing_margin = std::chrono::milliseconds (1);
          options.add_location_store (location_store);
          options.add_relocation_store (relocation_store);
          options.configure_locations ().polling_interval =
            std::chrono::milliseconds (10);
          auto mesh =
            options.add_route_mesh ("host-relocation-mesh");
          mesh
            .listen ("tcp://127.0.0.1:0")
            .set_routing_id (
              zlink::routing_id_t::from (routing_id))
            .add_spot_factory<host_relocation_spot_t> (
              "host-relocation-spot",
              [] (zlink::framework::spot_context_t context) {
                  return std::make_shared<
                    host_relocation_spot_t> (
                    std::move (context));
              },
              [] (auto &factory) {
                  factory.set_execution_mode (
                    zlink::framework::
                      user_spot_execution_mode_t::spot_wide);
                  factory.set_relocation_coordination_mode (
                    zlink::framework::
                      spot_relocation_coordination_mode_t::
                        application_signaled);
                  factory.template preserve_state_with<
                    host_relocation_spot_adapter_t> ();
              });
      });
}

void test_app_relocate_waits_for_application_boundary (
  test_context_t &test)
{
    host_relocation_spot_t::completion_count.store (
      0, std::memory_order_release);
    host_relocation_spot_t::last_outcome.store (
      -1, std::memory_order_release);
    host_relocation_spot_adapter_t::restored.store (
      false, std::memory_order_release);

    auto location_store = std::make_shared<
      zlink::framework::runtime::in_memory_location_store_t> ();
    auto relocation_store = std::make_shared<
      zlink::framework::runtime::in_memory_relocation_store_t> ();
    auto send_ready = std::make_shared<std::atomic_bool> (false);

    auto source = zlink::framework::app_t::create ();
    configure_host_relocation_app (
      source, location_store, relocation_store,
      "host-relocation-source");
    auto source_service = std::make_unique<
      host_relocation_source_service_t> (
      source, send_ready);
    auto *source_service_view = source_service.get ();
    source.add_hosted_service (std::move (source_service));

    char source_program[] = "host-relocation-source";
    char *source_arguments[] = {source_program, nullptr};
    int source_exit_code = -1;
    std::thread source_thread ([&] {
        source_exit_code =
          source.run (1, source_arguments);
    });

    const bool source_started =
      wait_until (
        [&] {
            return source.is_ready ()
                   && source_service_view
                        ->created_spot.load (
                          std::memory_order_acquire);
        },
        std::chrono::seconds (5));
    test.require (
      source_started,
      "source app must serve and create the application-signaled Spot");
    if (!source_started) {
        source.request_stop ();
        source_thread.join ();
        return;
    }

    auto target = zlink::framework::app_t::create ();
    configure_host_relocation_app (
      target, location_store, relocation_store,
      "host-relocation-target");
    char target_program[] = "host-relocation-target";
    char *target_arguments[] = {target_program, nullptr};
    int target_exit_code = -1;
    std::thread target_thread ([&] {
        target_exit_code =
          target.run (1, target_arguments);
    });
    const bool target_started =
      wait_until (
        [&] { return target.is_ready (); },
        std::chrono::seconds (5));
    test.require (
      target_started,
      "target app must reach Serving before relocation");
    if (!target_started) {
        target.request_stop ();
        source.request_stop ();
        target_thread.join ();
        source_thread.join ();
        return;
    }

    auto relocation = source.relocate (
      {.mode =
         zlink::framework::relocation_mode_t::
           planned_maintenance,
       .deadline = std::chrono::seconds (10)});
    send_ready->store (true, std::memory_order_release);
    const auto result = relocation.result ().value ();

    test.require (
      source_service_view->ready_sent.load (
        std::memory_order_acquire),
      "public Spot messaging must submit the readiness turn");
    test.require (
      result.outcome
        == zlink::framework::relocation_outcome_t::relocated,
      "public app relocate must complete after the application boundary");
    test.require (
      host_relocation_spot_adapter_t::restored.load (
        std::memory_order_acquire),
      "target app must restore the Spot state");
    test.require (
      host_relocation_spot_t::completion_count.load (
        std::memory_order_acquire)
        == 1,
      "relocation readiness completion must run exactly once");
    test.require (
      host_relocation_spot_t::last_outcome.load (
        std::memory_order_acquire)
        == static_cast<int> (
          zlink::framework::
            spot_relocation_ready_outcome_t::relocated),
      "relocation readiness completion must report relocated");

    const auto target_shutdown =
      target.shutdown (std::chrono::seconds (5))
        .result ().value ();
    const auto source_shutdown =
      source.shutdown (std::chrono::seconds (5))
        .result ().value ();
    target_thread.join ();
    source_thread.join ();
    test.require (
      target_shutdown.outcome
          == zlink::framework::termination_outcome_t::stopped
        && source_shutdown.outcome
             == zlink::framework::termination_outcome_t::stopped
        && target_exit_code == 0
        && source_exit_code == 0,
      "host relocation integration apps must stop cleanly");
    test.require (
      source_service_view->error.empty (),
      "host relocation source service must not report an error");
}

#endif

void test_relocation_ready_completion_runs_once_on_spot_turn (test_context_t &test)
{
    namespace detail = zlink::framework::detail;
    namespace runtime = zlink::framework::runtime;
    using zlink::framework::spot_relocation_coordination_mode_t;
    using zlink::framework::spot_relocation_ready_outcome_t;
    using zlink::framework::user_spot_execution_mode_t;

    auto state = std::make_shared<detail::spot_context_state_t> ();
    state->execution_mode = user_spot_execution_mode_t::spot_wide;
    state->relocation_coordination_mode = spot_relocation_coordination_mode_t::application_signaled;
    state->serial_executor =
      std::make_shared<runtime::offload_executor_t> (2, "relocation-ready-test");
    state->serial_queue = std::make_shared<runtime::serial_execution_queue_t> (
      *state->serial_executor, runtime::serial_execution_queue_options_t{},
      runtime::serial_execution_queue_t::error_handler_t{},
      runtime::serial_lane_policy_t::spot_wide ());
    state->node = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::spot_node_builder_state_t> ("relocation-ready-node");
    state->channel_runtime = std::make_shared<detail::channel_runtime_state_t> ();
    state->spot_instance = std::make_shared<int> (1);
    std::atomic_int completions{0};
    bool completion_owned_spot_turn = false;
    std::vector<spot_relocation_ready_outcome_t> outcomes;
    state->lifecycle.on_relocation_ready_completed =
      [&] (void *, const zlink::framework::spot_relocation_ready_completion_t &completion) {
          outcomes.push_back (completion.outcome);
          completion_owned_spot_turn = state->owns_current_serial_turn ();
          completions.fetch_add (1);
      };
    auto context = detail::spot_context_access_t::create (state);
    auto manager_before_defer = context.manager ();
    auto worker_before_defer = context.run_cpu_worker ([] { return 7; });
    auto outbound_before_defer = context.outbound ();
    bool close_rejected = false;
    bool manager_rejected = false;
    bool worker_rejected = false;
    bool outbound_rejected = false;
    const auto deferred = state->run_serial_sync ("defer-relocation", [&] {
        context.relocation_ready ().defer ();
        try {
            context.close ();
        }
        catch (const zlink::framework::framework_exception_t &error) {
            close_rejected =
              error.kind () == zlink::framework::framework_error_kind_t::invalid_operation;
        }
        const auto found =
          manager_before_defer.find (zlink::framework::spot_id_t ("blocked-spot")).result ();
        manager_rejected =
          !found
          && found.error_kind () == zlink::framework::framework_error_kind_t::invalid_operation;
        const auto worker = worker_before_defer.async ().result ();
        worker_rejected =
          !worker
          && worker.error_kind () == zlink::framework::framework_error_kind_t::invalid_operation;
        const auto outbound =
          outbound_before_defer.send_to_channel ("blocked-channel", std::string ("blocked"))
            .async ()
            .result ();
        outbound_rejected =
          !outbound
          && outbound.error_kind () == zlink::framework::framework_error_kind_t::invalid_operation;
    });
    const auto completion_deadline = std::chrono::steady_clock::now () + std::chrono::seconds (1);
    while (completions.load () == 0 && std::chrono::steady_clock::now () < completion_deadline)
        std::this_thread::yield ();
    test.require (
      deferred && completions.load () == 1 && completion_owned_spot_turn && close_rejected
        && manager_rejected && worker_rejected && outbound_rejected
        && outcomes
             == std::vector<
               spot_relocation_ready_outcome_t>{spot_relocation_ready_outcome_t::continued},
      "readiness without a prepared relocation must complete "
      "continued exactly once on the next Spot serial turn");

    state->node->lane.run ([state] { state->relocation_boundary_active = true; }).get ();
    const auto prepared_deferred = run_serial_turn (state, "defer-prepared-relocation",
                                                    [&] { context.relocation_ready ().defer (); });
    state->complete_relocation_ready (spot_relocation_ready_outcome_t::relocated);
    state->complete_relocation_ready (spot_relocation_ready_outcome_t::continued);
    test.require (prepared_deferred && completions.load () == 2
                    && outcomes.back () == spot_relocation_ready_outcome_t::relocated,
                  "prepared relocation must consume the boundary and complete "
                  "relocated exactly once");

    state->relocation_coordination_mode = spot_relocation_coordination_mode_t::framework_managed;
    bool rejected = false;
    try {
        (void) state->run_serial_sync ("reject-relocation-defer",
                                       [&] { context.relocation_ready ().defer (); });
    }
    catch (const zlink::framework::framework_exception_t &error) {
        rejected = error.kind () == zlink::framework::framework_error_kind_t::not_configured;
    }
    test.require (rejected, "FrameworkManaged must reject relocation_ready().defer()");
}

/* Spot model §5.1: only the turn that registered RelocationReady().Defer()
 * rejects later Framework operations. A lifecycle callback queued before the
 * boundary, such as OnLeaveActor submitted by a timer cleanup, runs in its own
 * turn and may start operations. */
void test_relocation_ready_defer_rejects_only_its_own_turn (test_context_t &test)
{
    namespace detail = zlink::framework::detail;
    namespace runtime = zlink::framework::runtime;
    using zlink::framework::framework_error_kind_t;
    using zlink::framework::framework_exception_t;
    using zlink::framework::spot_relocation_coordination_mode_t;
    using zlink::framework::spot_relocation_ready_outcome_t;
    using zlink::framework::user_spot_execution_mode_t;

    auto state = std::make_shared<detail::spot_context_state_t> ();
    state->execution_mode = user_spot_execution_mode_t::spot_wide;
    state->relocation_coordination_mode = spot_relocation_coordination_mode_t::application_signaled;
    // A prepared boundary keeps the deferral pending while the queued turn runs.
    state->relocation_boundary_active = true;
    state->serial_executor =
      std::make_shared<runtime::offload_executor_t> (2, "relocation-ready-own-turn");
    state->serial_queue = std::make_shared<runtime::serial_execution_queue_t> (
      *state->serial_executor, runtime::serial_execution_queue_options_t{},
      runtime::serial_execution_queue_t::error_handler_t{},
      runtime::serial_lane_policy_t::spot_wide ());
    state->node = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::spot_node_builder_state_t> ("relocation-own-turn-node");
    state->channel_runtime = std::make_shared<detail::channel_runtime_state_t> ();
    state->spot_instance = std::make_shared<int> (1);
    auto context = detail::spot_context_access_t::create (state);

    const auto operation_allowed = [&state] {
        try {
            state->ensure_relocation_turn_open ();
            return true;
        }
        catch (const framework_exception_t &error) {
            if (error.kind () != framework_error_kind_t::invalid_operation)
                throw;
            return false;
        }
    };
    std::promise<bool> queued_turn_allowed;
    auto queued_result = queued_turn_allowed.get_future ();
    bool deferring_turn_rejected = false;
    const auto deferred = run_serial_turn (state, "defer-in-timer-turn", [&] {
        state->serial_queue->post (
          "queued-leave-callback", [&] { queued_turn_allowed.set_value (operation_allowed ()); },
          runtime::serial_work_options_t{runtime::serial_work_lane_t::lifecycle});
        context.relocation_ready ().defer ();
        deferring_turn_rejected = !operation_allowed ();
    });
    const bool queued_ran =
      queued_result.wait_for (std::chrono::seconds (1)) == std::future_status::ready;
    const bool queued_allowed = queued_ran && queued_result.get ();
    state->complete_relocation_ready (spot_relocation_ready_outcome_t::continued);
    state->serial_queue->drain ();
    if (!queued_allowed)
        std::cerr << "relocation defer rejected another turn: queued_ran=" << queued_ran << '\n';
    test.require (deferred && deferring_turn_rejected && queued_allowed,
                  "relocation readiness defer must reject operations only in its own turn");
}

void test_relocation_ready_defer_holds_queued_timer_turn (test_context_t &test)
{
    namespace detail = zlink::framework::detail;
    namespace runtime = zlink::framework::runtime;
    using zlink::framework::spot_relocation_coordination_mode_t;
    using zlink::framework::spot_relocation_ready_outcome_t;
    using zlink::framework::timer_tick_t;
    using zlink::framework::user_spot_execution_mode_t;

    auto state = std::make_shared<detail::spot_context_state_t> ();
    state->execution_mode = user_spot_execution_mode_t::spot_wide;
    state->relocation_coordination_mode = spot_relocation_coordination_mode_t::application_signaled;
    state->relocation_boundary_active = true;
    state->serial_executor =
      std::make_shared<runtime::offload_executor_t> (2, "relocation-ready-timer-test");
    state->serial_queue = std::make_shared<runtime::serial_execution_queue_t> (
      *state->serial_executor, runtime::serial_execution_queue_options_t{},
      runtime::serial_execution_queue_t::error_handler_t{},
      runtime::serial_lane_policy_t::spot_wide ());
    state->node = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::spot_node_builder_state_t> ("relocation-ready-timer-node");
    state->channel_runtime = std::make_shared<detail::channel_runtime_state_t> ();
    zlink::framework::serializer_registry_t serializers;
    state->channel_runtime->serializers = &serializers;
    state->spot_instance = std::make_shared<int> (1);

    std::atomic_int completions{0};
    std::atomic_int timer_calls{0};
    std::atomic_bool timer_ran_after_completion{false};
    state->lifecycle.on_relocation_ready_completed =
      [&] (void *, const zlink::framework::spot_relocation_ready_completion_t &) {
          completions.fetch_add (1, std::memory_order_acq_rel);
      };

    auto timer_state = std::make_shared<detail::timer_state_t> ();
    timer_state->name = "queued-before-relocation-ready";
    auto timer_handler = std::make_shared<int> (1);
    timer_state->handler_instance = timer_handler;
    timer_state->handler_invoker =
      [&] (void *, void *, zlink::framework::serializer_registry_t &,
           const timer_tick_t &) -> zlink::framework::task_t<zlink::message_t> {
        timer_ran_after_completion.store (completions.load (std::memory_order_acquire) == 1,
                                          std::memory_order_release);
        timer_calls.fetch_add (1, std::memory_order_acq_rel);
        co_return zlink::message_t{};
    };
    state->timers.push_back (timer_state);

    auto context = detail::spot_context_access_t::create (state);
    const auto deferred = run_serial_turn (state, "defer-with-queued-timer", [&] {
        detail::timer_runtime_t::post_fire_count (state, timer_state, {}, 1);
        context.relocation_ready ().defer ();
    });
    const auto timer_ran_while_deferred =
      wait_until_bounded ([&] { return timer_calls.load (std::memory_order_acquire) != 0; },
                          std::chrono::milliseconds (100));

    test.require (deferred && !timer_ran_while_deferred,
                  "a timer turn queued before relocation readiness defer must remain held");

    state->complete_relocation_ready (spot_relocation_ready_outcome_t::continued);
    const auto timer_resumed = wait_until_bounded (
      [&] { return timer_calls.load (std::memory_order_acquire) == 1; }, std::chrono::seconds (1));
    test.require (timer_resumed && completions.load (std::memory_order_acquire) == 1
                    && timer_ran_after_completion.load (std::memory_order_acquire),
                  "the held timer turn must resume after relocation readiness completion");
}

} // namespace m6c_test
