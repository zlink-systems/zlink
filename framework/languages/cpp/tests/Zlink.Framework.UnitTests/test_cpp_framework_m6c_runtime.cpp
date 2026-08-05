/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/stateful/maintenance_runtime.hpp"
#include <runtime/locations/location_repository.hpp>
#include "runtime/stateful/public_store_adapters.hpp"
#include "runtime/stateful/raw_stateful_dispatch.hpp"
#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/spots/spot_runtime.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{

using namespace zlink::framework::runtime::stateful;

static_assert (
  requires (zlink::framework::spot_context_t &context) {
      {
          context.relocation_ready ()
      } -> std::same_as<zlink::framework::spot_relocation_ready_call_t>;
  });
static_assert (
  requires (zlink::framework::spot_relocation_ready_call_t &call) {
      { call.defer () } -> std::same_as<void>;
  });
static_assert (
  !std::copy_constructible<
    zlink::framework::spot_relocation_ready_call_t>);

struct test_context_t
{
    int failures = 0;

    void require (bool condition, const char *message)
    {
        if (condition)
            return;
        ++failures;
        std::cerr << "V11-M6C-CPP: " << message << '\n';
    }
};

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
            .submit ().result ();
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
                      .submit ().result ();
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
                  factory.set_relocation_readiness (
                    zlink::framework::
                      spot_relocation_readiness_mode_t::
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

template<typename Predicate>
bool wait_until_bounded (
  Predicate &&condition,
  std::chrono::milliseconds timeout)
{
    const auto deadline =
      std::chrono::steady_clock::now () + timeout;
    while (std::chrono::steady_clock::now () < deadline) {
        if (condition ())
            return true;
        std::this_thread::sleep_for (
          std::chrono::milliseconds (1));
    }
    return condition ();
}

void test_relocation_ready_completion_runs_once_on_spot_turn (
  test_context_t &test)
{
    namespace detail = zlink::framework::detail;
    namespace runtime = zlink::framework::runtime;
    using zlink::framework::spot_relocation_readiness_mode_t;
    using zlink::framework::spot_relocation_ready_outcome_t;
    using zlink::framework::user_spot_execution_mode_t;

    auto state =
      std::make_shared<detail::spot_context_state_t> ();
    state->execution_mode =
      user_spot_execution_mode_t::spot_wide;
    state->relocation_readiness =
      spot_relocation_readiness_mode_t::application_signaled;
    state->serial_executor =
      std::make_shared<runtime::offload_executor_t> (
        2, 64, "relocation-ready-test");
    state->serial_queue =
      std::make_shared<runtime::serial_execution_queue_t> (
        *state->serial_executor, 64,
        runtime::serial_execution_queue_t::error_handler_t{},
        true);
    state->node =
      std::make_shared<detail::spot_node_builder_state_t> (
        "relocation-ready-node");
    state->channel_runtime =
      std::make_shared<detail::channel_runtime_state_t> ();
    state->spot_instance = std::make_shared<int> (1);
    std::atomic_int completions{0};
    bool completion_owned_spot_turn = false;
    std::vector<spot_relocation_ready_outcome_t> outcomes;
    state->lifecycle.on_relocation_ready_completed =
      [&] (
        void *,
        const zlink::framework::
          spot_relocation_ready_completion_t &completion) {
          outcomes.push_back (completion.outcome);
          completion_owned_spot_turn =
            state->owns_current_serial_turn ();
          completions.fetch_add (1);
      };
    auto context =
      detail::spot_context_access_t::create (state);
    auto manager_before_defer = context.manager ();
    auto worker_before_defer =
      context.run_cpu_worker ([] { return 7; });
    auto outbound_before_defer = context.outbound ();
    bool close_rejected = false;
    bool manager_rejected = false;
    bool worker_rejected = false;
    bool outbound_rejected = false;
    const auto deferred = state->run_serial_sync (
      "defer-relocation", [&] {
          context.relocation_ready ().defer ();
          try {
              const auto closed = context.close ().result ();
              close_rejected =
                !closed
                && closed.error_kind ()
                     == zlink::framework::
                       framework_error_kind_t::not_configured;
          }
          catch (
            const zlink::framework::framework_exception_t &error) {
              close_rejected =
                error.kind ()
                == zlink::framework::framework_error_kind_t::not_configured;
          }
          const auto found =
            manager_before_defer.find (
              zlink::framework::spot_id_t ("blocked-spot"))
              .result ();
          manager_rejected =
            !found
            && found.error_kind ()
                 == zlink::framework::framework_error_kind_t::not_configured;
          const auto worker = worker_before_defer.submit ().result ();
          worker_rejected =
            !worker
            && worker.error_kind ()
                 == zlink::framework::framework_error_kind_t::not_configured;
          const auto outbound =
            outbound_before_defer
              .send_to_channel (
                "blocked-channel", std::string ("blocked"))
              .submit ()
              .result ();
          outbound_rejected =
            !outbound
            && outbound.error_kind ()
                 == zlink::framework::framework_error_kind_t::not_configured;
      });
    const auto completion_deadline =
      std::chrono::steady_clock::now ()
      + std::chrono::seconds (1);
    while (completions.load () == 0
           && std::chrono::steady_clock::now ()
                < completion_deadline)
        std::this_thread::yield ();
    test.require (
      deferred && completions.load () == 1
        && completion_owned_spot_turn
        && close_rejected
        && manager_rejected
        && worker_rejected
        && outbound_rejected
        && outcomes
             == std::vector<spot_relocation_ready_outcome_t>{
               spot_relocation_ready_outcome_t::continued},
      "readiness without a prepared relocation must complete "
      "continued exactly once on the next Spot serial turn");

    {
        std::lock_guard lock (state->callback_mutex);
        state->relocation_boundary_active = true;
    }
    const auto prepared_deferred = state->run_serial_sync (
      "defer-prepared-relocation", [&] {
          context.relocation_ready ().defer ();
      });
    state->complete_relocation_ready (
      spot_relocation_ready_outcome_t::relocated);
    state->complete_relocation_ready (
      spot_relocation_ready_outcome_t::continued);
    test.require (
      prepared_deferred && completions.load () == 2
        && outcomes.back ()
             == spot_relocation_ready_outcome_t::relocated,
      "prepared relocation must consume the boundary and complete "
      "relocated exactly once");

    state->relocation_readiness =
      spot_relocation_readiness_mode_t::any_turn_boundary;
    bool rejected = false;
    try {
        (void) state->run_serial_sync (
          "reject-relocation-defer", [&] {
              context.relocation_ready ().defer ();
          });
    }
    catch (const zlink::framework::framework_exception_t &error) {
        rejected =
          error.kind ()
          == zlink::framework::framework_error_kind_t::not_configured;
    }
    test.require (
      rejected,
      "AnyTurnBoundary must reject relocation_ready().defer()");
}

void test_actor_leave_after_relocation_defer_runs_lifecycle_callbacks (
  test_context_t &test)
{
    namespace detail = zlink::framework::detail;
    namespace runtime = zlink::framework::runtime;
    using zlink::framework::actor_ref_t;
    using zlink::framework::node_rid_t;
    using zlink::framework::spot_context_t;
    using zlink::framework::spot_id_t;
    using zlink::framework::spot_relocation_readiness_mode_t;
    using zlink::framework::user_spot_execution_mode_t;

    struct test_actor_t
    {
    } actor;

    const auto node =
      std::make_shared<detail::spot_node_builder_state_t> (
        "actor-leave-after-defer-node");
    const auto node_rid = node_rid_t::from_string ("actor-leave-after-defer-node");
    const auto source_id = spot_id_t ("source-spot");
    const auto entry_id = spot_id_t ("entry-spot");
    node->snapshot.entry_spot_name = "entry";
    node->spot_ids_by_name.emplace ("entry", entry_id);

    const auto make_state = [&] (spot_id_t spot_id, std::string spot_name,
                                 bool entry_spot) {
        auto state = std::make_shared<detail::spot_context_state_t> ();
        state->node = node;
        state->node_rid = node_rid;
        state->spot_id = std::move (spot_id);
        state->spot_name = std::move (spot_name);
        state->kind = entry_spot ? detail::spot_runtime_kind_t::entry
                                 : detail::spot_runtime_kind_t::user;
        state->execution_mode = user_spot_execution_mode_t::spot_wide;
        state->relocation_readiness =
          spot_relocation_readiness_mode_t::application_signaled;
        state->serial_executor =
          std::make_shared<runtime::offload_executor_t> (
            2, 64, "actor-leave-after-defer");
        state->serial_queue =
          std::make_shared<runtime::serial_execution_queue_t> (
            *state->serial_executor, 64,
            runtime::serial_execution_queue_t::error_handler_t{}, true);
        state->spot_instance = std::make_shared<int> (1);
        return state;
    };

    const auto source = make_state (source_id, "source", false);
    const auto entry = make_state (entry_id, "entry", true);
    std::atomic_int leave_callbacks{0};
    std::atomic_int joined_callbacks{0};
    source->on_leave_actor_callbacks[std::type_index (typeid (test_actor_t))] =
      [&] (void *, void *) {
          leave_callbacks.fetch_add (1, std::memory_order_acq_rel);
          return zlink::framework::task_t<void> (
            zlink::framework::result_t<void>::success ());
      };
    entry->on_actor_joined_callbacks[std::type_index (typeid (test_actor_t))] =
      [&] (void *, void *) {
          joined_callbacks.fetch_add (1, std::memory_order_acq_rel);
          return zlink::framework::task_t<void> (
            zlink::framework::result_t<void>::success ());
      };
    node->spot_contexts_by_id.emplace (
      entry_id, detail::spot_context_access_t::create (entry));
    auto context = detail::spot_context_access_t::create (source);

    const auto actor_ref =
      ::zlink::framework::detail::actor_ref_access_t::make (
        node_rid, "test_actor", "actor-1", 1);
    const std::string key = "test_actor:actor-1";
    {
        std::lock_guard<std::recursive_mutex> lock (node->mutex);
        node->actor_spot_ids.emplace (key, source_id);
        node->actor_generations.emplace (key, actor_ref.object_generation ());
        node->actor_created_keys.emplace (key);
        node->actor_instances.emplace (
          key, std::shared_ptr<void> (std::addressof (actor), [] (void *) {}));
        node->actor_instance_index.emplace (
          std::addressof (actor), std::make_pair ("test_actor", "actor-1"));
        source->actor_count = 1;
    }

    const auto submitted = source->run_serial_sync (
      "actor-leave-with-relocation-fence", [&] {
          const auto left = context.leave_actor (actor_ref, actor).result ();
          if (!left) {
              throw std::runtime_error (
                "actor leave was not accepted from the handler turn");
          }
          context.relocation_ready ().defer ();
      });

    const auto completed = wait_until_bounded (
      [&] {
          return leave_callbacks.load (std::memory_order_acquire) == 1
                 && joined_callbacks.load (std::memory_order_acquire) == 1;
      }, std::chrono::seconds (1));
    std::string current_location;
    std::size_t source_actor_count = 0;
    std::size_t entry_actor_count = 0;
    {
        std::lock_guard<std::recursive_mutex> lock (node->mutex);
        const auto found = node->actor_spot_ids.find (key);
        if (found != node->actor_spot_ids.end ()) {
            current_location = found->second;
        }
        source_actor_count = source->actor_count;
        entry_actor_count = entry->actor_count;
    }
    test.require (
      submitted && completed && current_location == entry_id
        && source_actor_count == 0 && entry_actor_count == 1,
      "actor leave deferred by a relocation-ready handler must run source and entry lifecycle callbacks before the next relocation turn");
    {
        std::lock_guard<std::recursive_mutex> lock (node->mutex);
        node->spot_contexts_by_id.clear ();
        node->spot_ids_by_name.clear ();
        node->spot_names_by_id.clear ();
        node->actor_spot_ids.clear ();
        node->actor_generations.clear ();
        node->actor_created_keys.clear ();
        node->actor_instances.clear ();
        node->actor_instance_index.clear ();
    }
}

void test_temporary_channel_request_yield_owns_call_state (
  test_context_t &test)
{
    namespace detail = zlink::framework::detail;
    namespace runtime = zlink::framework::runtime;
    using zlink::framework::channel_request_call_t;
    using zlink::framework::encoded_payload_t;
    using zlink::framework::serializer_registry_t;
    using zlink::framework::task_t;

    struct reply_t
    {
        int value = 0;
    };

    serializer_registry_t serializers;
    serializers.add<reply_t> (
      [] (const reply_t &value) {
          return encoded_payload_t::from_string (std::to_string (value.value));
      },
      [] (const encoded_payload_t &payload) {
          return reply_t{std::stoi (payload.to_string ())};
      });

    auto state = std::make_shared<detail::spot_context_state_t> ();
    state->node = std::make_shared<detail::spot_node_builder_state_t> (
      "temporary-channel-call-node");
    state->serial_executor = std::make_shared<runtime::offload_executor_t> (
      2, 64, "temporary-channel-call");
    state->serial_queue = std::make_shared<runtime::serial_execution_queue_t> (
      *state->serial_executor, 64,
      runtime::serial_execution_queue_t::error_handler_t{}, true);

    auto reply_source =
      std::make_shared<detail::task_completion_source_t<zlink::message_t>> ();
    auto result_source =
      std::make_shared<detail::task_completion_source_t<reply_t>> ();
    auto result_task = result_source->task ();
    const auto submitted = state->run_serial_sync (
      "temporary-channel-call-yield", [&] {
          auto pending = channel_request_call_t (
            "temporary.reply", &serializers,
            [reply_source] (const std::string &, std::chrono::milliseconds,
                            const channel_request_call_t::metadata_map_t &) {
                return reply_source->task ();
            })
                           .yield<reply_t> ();
          detail::observe_task_completion (
            pending, [result_source] (const zlink::framework::result_t<reply_t> &result) {
                result_source->complete (result);
            });
      });

    reply_source->complete (
      zlink::framework::result_t<zlink::message_t>::success (
        zlink::message_t::from ("451")));
    const auto decoded = result_task.result ();
    test.require (
      submitted && decoded && decoded.value ().value == 451,
      "temporary channel request yield must retain serializers and submit state after suspension");
}

std::string authority_key (object_kind_t kind, const std::string &key)
{
    return std::to_string (static_cast<int> (kind)) + ":" + key;
}

inventory_digest_t digest_with (std::uint8_t value);

class fail_first_restore_spot_t final
    : public zlink::framework::spot_t<zlink::framework::actor_t>
{
  public:
    explicit fail_first_restore_spot_t (
      zlink::framework::spot_context_t context) :
        _context (std::move (context))
    {
    }

    zlink::framework::spot_context_t &context () noexcept override
    {
        return _context;
    }
    const zlink::framework::spot_context_t &context () const noexcept override
    {
        return _context;
    }

    void configure () override {}

    zlink::framework::task_t<zlink::framework::spot_create_response_t>
    on_create (const zlink::framework::message_t &) override
    {
        ++create_count;
        co_return zlink::framework::spot_create_response_t::accept ();
    }

    zlink::framework::task_t<void> on_initialize () override
    {
        ++initialize_count;
        co_return;
    }

    zlink::framework::task_t<zlink::framework::spot_actor_join_result_t>
    on_actor_join (std::string_view,
                   const zlink::framework::message_t &) override
    {
        co_return zlink::framework::spot_actor_join_result_t::reject ();
    }

    zlink::framework::task_t<void>
    on_actor_joined (zlink::framework::actor_t &) override
    {
        co_return;
    }

    zlink::framework::task_t<void>
    on_leave_actor (zlink::framework::actor_t &) override
    {
        co_return;
    }

    static inline int factory_count = 0;
    static inline int restore_count = 0;
    static inline int create_count = 0;
    static inline int initialize_count = 0;
    static inline std::vector<std::byte> restored_payload;

  private:
    zlink::framework::spot_context_t _context;
};

class fail_first_restore_adapter_t final
    : public zlink::framework::spot_relocation_adapter_t<
        fail_first_restore_spot_t>
{
  public:
    zlink::framework::task_t<std::vector<std::byte>>
    capture (
      fail_first_restore_spot_t &,
      std::stop_token) override
    {
        co_return std::vector<std::byte>{};
    }

    zlink::framework::task_t<void>
    restore (
      fail_first_restore_spot_t &,
      std::vector<std::byte> payload,
      std::stop_token) override
    {
        if (++fail_first_restore_spot_t::restore_count == 1) {
            throw std::runtime_error ("expected first restore failure");
        }
        fail_first_restore_spot_t::restored_payload =
          std::move (payload);
        co_return;
    }
};

class concurrent_restore_spot_t final
    : public zlink::framework::spot_t<zlink::framework::actor_t>
{
  public:
    explicit concurrent_restore_spot_t (
      zlink::framework::spot_context_t context) :
        _context (std::move (context))
    {
        ++factory_count;
    }

    zlink::framework::spot_context_t &context () noexcept override
    {
        return _context;
    }
    const zlink::framework::spot_context_t &context () const noexcept override
    {
        return _context;
    }

    void configure () override {}
    zlink::framework::task_t<zlink::framework::spot_actor_join_result_t>
    on_actor_join (std::string_view,
                   const zlink::framework::message_t &) override
    {
        co_return zlink::framework::spot_actor_join_result_t::reject ();
    }
    zlink::framework::task_t<void>
    on_actor_joined (zlink::framework::actor_t &) override
    {
        co_return;
    }
    zlink::framework::task_t<void>
    on_leave_actor (zlink::framework::actor_t &) override
    {
        co_return;
    }

    static inline std::atomic_int factory_count{0};
    static inline std::atomic_int restore_count{0};
    static inline std::mutex restore_mutex;
    static inline std::condition_variable restore_condition;
    static inline bool restore_entered = false;
    static inline bool release_restore = false;
    static inline std::vector<std::byte> restored_payload;

  private:
    zlink::framework::spot_context_t _context;
};

class concurrent_restore_adapter_t final
    : public zlink::framework::spot_relocation_adapter_t<
        concurrent_restore_spot_t>
{
  public:
    zlink::framework::task_t<std::vector<std::byte>>
    capture (
      concurrent_restore_spot_t &,
      std::stop_token) override
    {
        co_return std::vector<std::byte>{};
    }

    zlink::framework::task_t<void>
    restore (
      concurrent_restore_spot_t &,
      std::vector<std::byte> payload,
      std::stop_token) override
    {
        concurrent_restore_spot_t::restore_count.fetch_add (1);
        std::unique_lock lock (
          concurrent_restore_spot_t::restore_mutex);
        concurrent_restore_spot_t::restore_entered = true;
        concurrent_restore_spot_t::restore_condition.notify_all ();
        concurrent_restore_spot_t::restore_condition.wait (
          lock, [] {
              return concurrent_restore_spot_t::release_restore;
          });
        concurrent_restore_spot_t::restored_payload =
          std::move (payload);
        co_return;
    }
};

class public_memory_authority_store_t final :
    public zlink::framework::runtime::in_memory_location_repository_t
{
  public:
    zlink::framework::task_t<
      zlink::framework::authority_read_result_t>
    read_authority (
      zlink::framework::authority_key_t key,
      std::stop_token) override
    {
        observed_keys.push_back (std::move (key.value));
        if (!snapshot)
            return completed (
              zlink::framework::authority_read_result_t{
                zlink::framework::authority_missing_t{
                  std::chrono::system_clock::now ()}});
        return completed (
          zlink::framework::authority_read_result_t{*snapshot});
    }

    zlink::framework::task_t<
      zlink::framework::authority_compare_exchange_result_t>
    compare_exchange_authority (
      zlink::framework::authority_key_t key,
      std::string expected_store_version,
      zlink::framework::authority_mutation_t mutation,
      std::stop_token) override
    {
        observed_keys.push_back (std::move (key.value));
        const auto *put =
          std::get_if<zlink::framework::authority_put_t> (
            &mutation);
        if (!snapshot || !put
            || expected_store_version != snapshot->store_version)
            return completed (
              zlink::framework::
                authority_compare_exchange_result_t{
                  zlink::framework::authority_conflict_t{
                    snapshot
                      ? zlink::framework::authority_read_result_t{
                          *snapshot}
                      : zlink::framework::authority_read_result_t{
                          zlink::framework::authority_missing_t{
                            std::chrono::system_clock::now ()}}}});
        if (put->generation_transition
            == zlink::framework::
                 authority_generation_transition_t::new_owner) {
            if (!put->target_owner
                || !put->relocation_capacity_fence)
                return completed (
                  zlink::framework::
                    authority_compare_exchange_result_t{
                      zlink::framework::authority_conflict_t{
                        zlink::framework::
                          authority_read_result_t{
                            *snapshot}}});
            observed_target_owner = put->target_owner;
            observed_capacity_fence =
              put->relocation_capacity_fence;
            ++snapshot->authority_owner_generation;
            snapshot->owner = *put->target_owner;
        }
        snapshot->store_version =
          std::to_string (
            std::stoull (snapshot->store_version) + 1);
        snapshot->payload = put->payload;
        snapshot->store_now = std::chrono::system_clock::now ();
        return completed (
          zlink::framework::
            authority_compare_exchange_result_t{
              zlink::framework::authority_stored_t{*snapshot}});
    }

    zlink::framework::task_t<
      zlink::framework::authority_scan_result_t>
    list_authorities (
      std::string,
      std::optional<zlink::framework::authority_scan_cursor_t>,
      std::size_t,
      std::stop_token) override
    {
        return completed (
          zlink::framework::authority_scan_result_t{
            zlink::framework::authority_page_t{}});
    }

    std::optional<zlink::framework::authority_snapshot_t> snapshot;
    std::optional<zlink::framework::location_owner_token_t>
      observed_target_owner;
    std::optional<zlink::framework::relocation_capacity_fence_t>
      observed_capacity_fence;
    std::vector<std::string> observed_keys;

  private:
    template <typename T>
    static zlink::framework::task_t<T> completed (T value)
    {
        return zlink::framework::task_t<T> (
          zlink::framework::result_t<T>::success (
            std::move (value)));
    }
};

class public_memory_relocation_repository_t final :
    public zlink::framework::relocation_repository_t
{
  public:
    zlink::framework::task_t<zlink::framework::relocation_stored_t>
    put_relocation (
      std::vector<std::byte> payload,
      std::chrono::hours retention,
      std::stop_token) override
    {
        if (retention != std::chrono::hours (24))
            throw std::runtime_error ("unexpected retention");
        std::vector<std::uint8_t> checksum_input;
        checksum_input.reserve (payload.size ());
        for (const auto value : payload)
            checksum_input.push_back (
              std::to_integer<std::uint8_t> (value));
        const auto reference = "public-root";
        roots[reference] = std::move (payload);
        return completed (
          zlink::framework::relocation_stored_t{
            reference,
            maintenance_runtime_t::crc32c (checksum_input),
            std::chrono::system_clock::now () + retention,
            std::chrono::system_clock::now ()});
    }

    zlink::framework::task_t<
      zlink::framework::relocation_read_result_t>
    get_relocation (std::string reference, std::stop_token) override
    {
        const auto found = roots.find (reference);
        if (found == roots.end ())
            return completed (
              zlink::framework::relocation_read_result_t{
                zlink::framework::relocation_missing_t{}});
        return completed (
          zlink::framework::relocation_read_result_t{
            zlink::framework::relocation_found_t{found->second}});
    }

    zlink::framework::task_t<
      zlink::framework::relocation_renew_result_t>
    renew_relocation (
      std::string reference,
      std::chrono::hours retention,
      std::stop_token) override
    {
        if (!roots.contains (reference))
            return completed (
              zlink::framework::relocation_renew_result_t{
                zlink::framework::relocation_renew_missing_t{}});
        const auto now = std::chrono::system_clock::now ();
        return completed (
          zlink::framework::relocation_renew_result_t{
            zlink::framework::relocation_renewed_t{
              now + retention, now}});
    }

    zlink::framework::task_t<
      zlink::framework::relocation_delete_result_t>
    delete_relocation (std::string reference, std::stop_token) override
    {
        return completed (
          roots.erase (reference) > 0
            ? zlink::framework::relocation_delete_result_t::deleted
            : zlink::framework::relocation_delete_result_t::missing);
    }

  private:
    template <typename T>
    static zlink::framework::task_t<T> completed (T value)
    {
        return zlink::framework::task_t<T> (
          zlink::framework::result_t<T>::success (std::move (value)));
    }

    std::map<std::string, std::vector<std::byte>> roots;
};

class memory_relocation_repository_t final : public relocation_store_port_t
{
  public:
    relocation_stored_t put (
      const std::vector<std::uint8_t> &payload,
      std::chrono::hours retention) override
    {
        if (retention != std::chrono::hours (24))
            throw std::runtime_error ("unexpected retention");
        std::function<void ()> callback;
        {
            std::lock_guard lock (mutex);
            callback = on_put;
        }
        if (callback)
            callback ();
        std::lock_guard lock (mutex);
        const auto reference = "root-" + std::to_string (++next_reference);
        roots[reference] = payload;
        log.push_back ("put");
        return {reference, maintenance_runtime_t::crc32c (payload)};
    }

    std::optional<std::vector<std::uint8_t>>
    get (const std::string &reference) override
    {
        std::lock_guard lock (mutex);
        const auto found = roots.find (reference);
        return found == roots.end ()
                 ? std::optional<std::vector<std::uint8_t>>{}
                 : std::make_optional (found->second);
    }

    void remove (const std::string &reference) override
    {
        std::lock_guard lock (mutex);
        roots.erase (reference);
        removed.push_back (reference);
        log.push_back ("remove");
    }

    void erase_without_authority_change (const std::string &reference)
    {
        std::lock_guard lock (mutex);
        roots.erase (reference);
    }

    std::mutex mutex;
    std::map<std::string, std::vector<std::uint8_t>> roots;
    std::vector<std::string> removed;
    std::vector<std::string> log;
    std::function<void ()> on_put;
    std::uint64_t next_reference = 0;
};

class memory_authority_store_t final : public authority_relocation_port_t
{
  public:
    authority_publish_result_t publish (
      const object_ref_t &source,
      std::string target_node_id,
      zlink::framework::location_owner_token_t target_owner,
      zlink::framework::relocation_capacity_fence_t,
      std::string relocation_reference,
      std::uint32_t checksum_crc32c,
      inventory_digest_t inventory_digest) override
    {
        std::lock_guard lock (mutex);
        log.push_back ("publish");
        ++publish_count;
        if (force_conflict
            || (conflict_on_publish != 0
                && publish_count == conflict_on_publish))
            return {authority_publish_status_t::conflict, read_locked (
                      source.kind, source.key)};
        auto target = source;
        target.node_id = std::move (target_node_id);
        ++target.authority_owner_generation;
        authority_relocation_reference_t reference{
          .source = source,
          .target = target,
          .relocation_reference = std::move (relocation_reference),
          .checksum_crc32c = checksum_crc32c,
          .inventory_digest = inventory_digest,
          .target_owner = std::move (target_owner)};
        rows[authority_key (source.kind, source.key)] = reference;
        if (throw_after_publish)
            throw std::runtime_error ("response lost after authority commit");
        return {authority_publish_status_t::published, reference};
    }

    std::optional<authority_relocation_reference_t>
    read (object_kind_t kind, const std::string &key) override
    {
        std::lock_guard lock (mutex);
        return read_locked (kind, key);
    }

    std::optional<authority_relocation_reference_t>
    read_locked (object_kind_t kind, const std::string &key)
    {
        const auto found = rows.find (authority_key (kind, key));
        return found == rows.end ()
                 ? std::optional<authority_relocation_reference_t>{}
                 : std::make_optional (found->second);
    }

    std::mutex mutex;
    std::map<std::string, authority_relocation_reference_t> rows;
    std::vector<std::string> log;
    bool force_conflict = false;
    bool throw_after_publish = false;
    int publish_count = 0;
    int conflict_on_publish = 0;
};

class memory_aggregate_authority_t final : public aggregate_authority_port_t
{
  public:
    explicit memory_aggregate_authority_t (
      std::shared_ptr<memory_authority_store_t> authority) :
        authority (std::move (authority))
    {
    }

    aggregate_publish_result_t prepare (
      const std::vector<object_ref_t> &sources,
      std::string target_node_id,
      zlink::framework::location_owner_token_t target_owner,
      std::vector<zlink::framework::relocation_capacity_fence_t>
        relocation_capacity_fences,
      std::string relocation_reference,
      std::uint32_t checksum_crc32c,
      inventory_digest_t inventory_digest) override
    {
        std::lock_guard lock (mutex);
        ++prepare_count;
        if (sources.size () < 2 || prepared
            || relocation_capacity_fences.size ()
                 != sources.size ())
            return {aggregate_publish_status_t::conflict, {}, {}};
        pending.clear ();
        for (const auto &source : sources) {
            auto target = source;
            target.node_id = target_node_id;
            ++target.authority_owner_generation;
            pending.push_back (
              {.source = source,
               .target = target,
               .relocation_reference = relocation_reference,
               .checksum_crc32c = checksum_crc32c,
               .inventory_digest = inventory_digest,
               .target_owner = target_owner});
        }
        prepared = true;
        return {
          aggregate_publish_status_t::prepared, {++next_fence},
          pending};
    }

    aggregate_publish_result_t commit (
      aggregate_relocation_fence_t fence) override
    {
        std::lock_guard lock (mutex);
        ++commit_count;
        if (!prepared || fence.value != next_fence)
            return {aggregate_publish_status_t::conflict, fence, {}};
        {
            std::lock_guard authority_lock (authority->mutex);
            for (const auto &reference : pending) {
                authority->rows[authority_key (
                  reference.source.kind, reference.source.key)] =
                  reference;
            }
        }
        prepared = false;
        return {
          aggregate_publish_status_t::committed, fence, pending};
    }

    void abort (aggregate_relocation_fence_t fence) override
    {
        std::lock_guard lock (mutex);
        if (fence.value == next_fence)
            prepared = false;
    }

    std::shared_ptr<memory_authority_store_t> authority;
    std::mutex mutex;
    std::vector<authority_relocation_reference_t> pending;
    std::uint64_t next_fence = 0;
    int prepare_count = 0;
    int commit_count = 0;
    bool prepared = false;
};

class target_preflight_t final : public target_preflight_port_t
{
  public:
    target_preflight_result_t preflight (
      const std::vector<relocation_unit_t> &units) override
    {
        std::function<void ()> callback;
        {
            std::lock_guard lock (mutex);
            ++calls;
            observed_units = units;
            callback = on_preflight;
        }
        if (callback)
            callback ();
        if (status != target_preflight_status_t::eligible)
            return {status, {}};
        target_preflight_result_t result{
          target_preflight_status_t::eligible, {}};
        for (std::size_t index = 0; index != units.size (); ++index) {
            result.units.push_back (
              {.unit = units[index],
               .target_node_id = "node-b",
               .target_owner = {"owner-b", 1},
               .relocation_capacity_fences = [&] {
                   std::vector<zlink::framework::
                                 relocation_capacity_fence_t>
                     fences;
                   for (std::size_t participant = 0;
                        participant
                          != units[index].participants.size ();
                        ++participant)
                       fences.push_back ({
                         "capacity-"
                         + std::to_string (index + 1)
                         + "-"
                         + std::to_string (participant + 1)});
                   return fences;
               } (),
               .encoded_upper_bound = 1024 * 1024,
               .inventory_digest =
                 digest_with (
                   static_cast<std::uint8_t> (index + 10))});
        }
        return result;
    }

    std::mutex mutex;
    std::vector<relocation_unit_t> observed_units;
    std::function<void ()> on_preflight;
    target_preflight_status_t status =
      target_preflight_status_t::eligible;
    int calls = 0;
};

object_ref_t create_actor (
  stateful_object_runtime_t &runtime,
  std::string key,
  std::string node = "node-a")
{
    runtime.replace_placement_candidates (
      {{.mesh_name = "mesh",
        .node_id = std::move (node),
        .stable_types = {"actor"},
        .weight = 100,
        .active_capacity = 100,
        .active_count = 0,
        .pending_capacity = 100,
        .pending_count = 0}});
    auto created = runtime.begin_create (
      {.kind = object_kind_t::actor,
       .key = std::move (key),
       .stable_type = "actor",
       .mesh_name = std::optional<std::string>{"mesh"},
       .creation_request = {},
       .exclusive = true,
       .instance_intent = false});
    if (created.status != create_status_t::reserved
        || runtime.commit_create (created.attempt) != stateful_error_t::none)
        throw std::runtime_error ("actor creation failed");
    return created.object;
}

object_ref_t create_spot (
  stateful_object_runtime_t &runtime,
  object_kind_t kind,
  std::string key)
{
    runtime.replace_placement_candidates (
      {{.mesh_name = "mesh",
        .node_id = "node-a",
        .stable_types = {"spot", "actor"},
        .weight = 100,
        .active_capacity = 100,
        .active_count = 0,
        .pending_capacity = 100,
        .pending_count = 0}});
    auto created = runtime.begin_create (
      {.kind = kind,
       .key = std::move (key),
       .stable_type = "spot",
       .mesh_name = std::optional<std::string>{"mesh"},
       .creation_request = {},
       .exclusive = true,
       .instance_intent = kind == object_kind_t::instance_spot});
    if (created.status != create_status_t::reserved
        || runtime.commit_create (created.attempt) != stateful_error_t::none)
        throw std::runtime_error ("spot creation failed");
    return created.object;
}

inventory_digest_t digest_with (std::uint8_t value)
{
    inventory_digest_t digest{};
    digest.fill (value);
    return digest;
}

void test_generation_barrier_quiesces_yield_spot_and_timer (
  test_context_t &test)
{
    stateful_object_runtime_t objects (16, 8);
    const auto actor = create_actor (objects, "barrier-actor");
    const auto spot =
      create_spot (objects, object_kind_t::user_spot, "barrier-spot");
    test.require (
      objects.register_timer (actor, {9, 1000, 1000, 30})
        == stateful_error_t::none,
      "barrier test timer must register");
    test.require (
      objects.enqueue (
        actor, turn_domain_t::application, {10, {10}})
          == stateful_error_t::none
        && objects.enqueue (
             actor, turn_domain_t::application, {20, {20}})
             == stateful_error_t::none
        && objects.enqueue (
             spot, turn_domain_t::application, {40, {40}})
             == stateful_error_t::none,
      "barrier test turns must enqueue");

    const auto [actor_claim_error, actor_claim] =
      objects.try_claim (actor, turn_domain_t::application);
    const auto [spot_claim_error, spot_claim] =
      objects.try_claim (spot, turn_domain_t::application);
    test.require (
      actor_claim_error == stateful_error_t::none && actor_claim
        && actor_claim->sequence == 10
        && spot_claim_error == stateful_error_t::none && spot_claim
        && spot_claim->sequence == 40,
      "Actor and Spot lanes must both be active before sealing");
    test.require (
      objects.yield_claim (actor, {11, {11}})
        == stateful_error_t::none,
      "yield must retain the Actor claim until its continuation completes");

    std::atomic<bool> seal_completed = false;
    stateful_error_t seal_error = stateful_error_t::conflict;
    aggregate_relocation_seal_t seal;
    std::thread sealing ([&] {
        auto result =
          objects.try_seal_relocation_aggregate ({actor, spot});
        seal_error = result.first;
        seal = std::move (result.second);
        seal_completed.store (true, std::memory_order_release);
    });

    const bool sealed = wait_until_bounded (
      [&] {
          return objects.cancel_timer (actor, 999)
                 == stateful_error_t::moving;
      },
      std::chrono::seconds (5));
    test.require (
      sealed && !seal_completed.load (std::memory_order_acquire),
      "seal must close timer admission and wait for active lanes");
    test.require (
      objects.enqueue_timer_tick (actor, 9, {30})
        == stateful_error_t::moving,
      "timer dispatch must not mutate the sealed generation");

    const auto [continuation_error, continuation] =
      objects.try_claim (actor, turn_domain_t::application);
    test.require (
      continuation_error == stateful_error_t::none && continuation
        && continuation->sequence == 11,
      "yielded continuation must reacquire its Actor lane while sealed");
    test.require (
      objects.complete_claim (actor, turn_domain_t::application)
          == stateful_error_t::none
        && !seal_completed.load (std::memory_order_acquire),
      "Spot lane must also quiesce before capture");
    test.require (
      objects.complete_claim (spot, turn_domain_t::application)
        == stateful_error_t::none,
      "active Spot lane must complete");
    sealing.join ();

    test.require (
      seal_error == stateful_error_t::none
        && seal.participants.size () == 2
        && seal.participants[0].pending_application.size () == 1
        && seal.participants[0].pending_application[0].sequence == 20
        && seal.participants[0].timers
             == std::vector<logical_timer_t> ({{9, 1000, 1000, 30}}),
      "capture must occur after quiescence and preserve queued work and timers");

    test.require (
      objects.enqueue (
        actor, turn_domain_t::application, {21, {21}})
        == stateful_error_t::none,
      "sealed ingress must be retained for same-generation abort");
    test.require (
      objects.abort_relocation (seal.token) == stateful_error_t::none,
      "same-generation abort must reopen the seal");
    test.require (
      objects.commit_relocation_aggregate (seal.token, "node-b").first
        == stateful_error_t::not_found,
      "stale commit must not mutate an aborted generation");

    const auto [first_error, first] =
      objects.try_claim (actor, turn_domain_t::application);
    test.require (
      first_error == stateful_error_t::none && first
        && first->sequence == 20,
      "abort must restore the captured queue before held ingress");
    test.require (
      objects.complete_claim (actor, turn_domain_t::application)
        == stateful_error_t::none,
      "restored captured turn must complete");
    const auto [held_error, held] =
      objects.try_claim (actor, turn_domain_t::application);
    test.require (
      held_error == stateful_error_t::none && held
        && held->sequence == 21,
      "abort must restore held ingress in FIFO order");
    test.require (
      objects.complete_claim (actor, turn_domain_t::application)
        == stateful_error_t::none,
      "restored held turn must complete");

    const auto [second_error, second_seal] =
      objects.try_seal_relocation_aggregate ({actor, spot});
    test.require (
      second_error == stateful_error_t::none
        && objects.abort_relocation (seal.token)
             == stateful_error_t::not_found,
      "stale abort must not reopen a newer generation");
    const auto [commit_error, committed] =
      objects.commit_relocation_aggregate (second_seal.token, "node-b");
    test.require (
      commit_error == stateful_error_t::none && committed.size () == 2
        && objects.enqueue (
             actor, turn_domain_t::application, {22, {22}})
             == stateful_error_t::generation_stale,
      "post-commit ingress using the source generation must be fenced");
}

void test_close_barrier_waits_and_abort_restores_ingress (
  test_context_t &test)
{
    stateful_object_runtime_t objects (8, 4);
    const auto spot =
      create_spot (objects, object_kind_t::user_spot, "closing-spot");
    test.require (
      objects.enqueue (
        spot, turn_domain_t::application, {1, {1}})
        == stateful_error_t::none,
      "close barrier test turn must enqueue");
    const auto [claim_error, claim] =
      objects.try_claim (spot, turn_domain_t::application);
    test.require (
      claim_error == stateful_error_t::none && claim
        && claim->sequence == 1,
      "Spot lane must be active before close");

    std::atomic<bool> close_completed = false;
    stateful_error_t close_error = stateful_error_t::conflict;
    std::optional<spot_close_token_t> close_token;
    std::thread closing ([&] {
        auto result = objects.begin_close_spot (spot);
        close_error = result.first;
        close_token = std::move (result.second);
        close_completed.store (true, std::memory_order_release);
    });

    const bool sealed = wait_until_bounded (
      [&] {
          return objects.register_timer (
                   spot, {1, 1000, 1000, 1})
                 == stateful_error_t::moving;
      },
      std::chrono::seconds (5));
    test.require (
      sealed && !close_completed.load (std::memory_order_acquire),
      "close must seal timer admission and wait for the active Spot lane");
    test.require (
      objects.enqueue (
        spot, turn_domain_t::application, {2, {2}})
        == stateful_error_t::none,
      "application ingress during close must be retained");
    test.require (
      objects.complete_claim (spot, turn_domain_t::application)
        == stateful_error_t::none,
      "active Spot lane must complete before close continues");
    closing.join ();
    test.require (
      close_error == stateful_error_t::none && close_token,
      "close must return its generation token after quiescence");
    test.require (
      objects.abort_close_spot (*close_token) == stateful_error_t::none
        && objects.commit_close_spot (*close_token)
             == stateful_error_t::generation_stale,
      "only the current close generation may reopen or commit");
    const auto [held_error, held] =
      objects.try_claim (spot, turn_domain_t::application);
    test.require (
      held_error == stateful_error_t::none && held
        && held->sequence == 2,
      "close abort must restore held ingress");
}

void test_envelope_round_trip (test_context_t &test)
{
    frozen_object_state_t frozen{
      .owner =
        {.kind = object_kind_t::actor,
         .key = "actor-a",
         .object_generation = 7,
         .authority_owner_generation = 9,
         .mesh_name = "mesh",
         .node_id = "node-a"},
      .stable_type = "actor",
      .application_state = {7, 8, 9},
      .pending_application = {{1, {1, 2}}, {2, {3}}},
      .timers = {{11, 100, 50, 3}}};
    const auto digest = digest_with (0x5a);
    const auto encoded = maintenance_runtime_t::encode (frozen, digest);
    const auto decoded = maintenance_runtime_t::decode (encoded);
    test.require (decoded.has_value (), "relocation envelope must decode");
    test.require (decoded && decoded->first == frozen,
                  "queue and timer state must round-trip");
    test.require (decoded && decoded->second == digest,
                  "inventory digest must round-trip");
    test.require (maintenance_runtime_t::crc32c (encoded) != 0,
                  "CRC32C must be computed for the immutable root");

    // ZLR1 did not contain an application-state length or payload. The reader
    // keeps accepting those roots while every new root is written as ZLR2.
    auto legacy_v1 = encoded;
    legacy_v1[3] = static_cast<std::uint8_t> ('1');
    constexpr std::size_t application_state_length_offset = 59;
    constexpr std::size_t application_state_payload_offset = 63;
    legacy_v1.erase (
      legacy_v1.begin ()
        + static_cast<std::ptrdiff_t> (application_state_length_offset),
      legacy_v1.begin ()
        + static_cast<std::ptrdiff_t> (
          application_state_payload_offset
          + frozen.application_state.size ()));
    const auto decoded_v1 = maintenance_runtime_t::decode (legacy_v1);
    auto expected_v1 = frozen;
    expected_v1.application_state.clear ();
    test.require (
      decoded_v1 && decoded_v1->first == expected_v1,
      "ZLR1 roots must remain readable with empty application state");

    auto excessive_count = encoded;
    constexpr std::size_t pending_count_offset = 66;
    excessive_count[pending_count_offset] = 0;
    excessive_count[pending_count_offset + 1] = 0;
    excessive_count[pending_count_offset + 2] = 0x10;
    excessive_count[pending_count_offset + 3] = 0x01;
    test.require (
      !maintenance_runtime_t::decode (excessive_count),
      "decoder must reject pending counts above the explicit maximum");

    auto duplicate_sequence = encoded;
    constexpr std::size_t first_sequence_offset = 70;
    constexpr std::size_t second_sequence_offset = 84;
    std::copy_n (
      duplicate_sequence.begin ()
        + static_cast<std::ptrdiff_t> (first_sequence_offset),
      8,
      duplicate_sequence.begin ()
        + static_cast<std::ptrdiff_t> (second_sequence_offset));
    test.require (
      !maintenance_runtime_t::decode (duplicate_sequence),
      "decoder must reject duplicate or unordered queue sequences");

    auto unordered = frozen;
    unordered.pending_application[1].sequence = 1;
    test.require (
      maintenance_runtime_t::encode (unordered, digest).empty (),
      "encoder must reject duplicate or unordered queue sequences");

    constexpr std::size_t application_state_limit =
      64u * 1024u * 1024u;
    auto bounded = frozen;
    bounded.application_state.assign (application_state_limit, 0x5a);
    auto bounded_encoded = maintenance_runtime_t::encode (bounded, digest);
    const auto bounded_decoded =
      maintenance_runtime_t::decode (bounded_encoded);
    test.require (
      bounded_decoded
        && bounded_decoded->first.application_state.size ()
             == application_state_limit,
      "the exact 64 MiB application-state limit must round-trip");

    auto oversized_root = std::move (bounded_encoded);
    oversized_root.insert (
      oversized_root.begin ()
        + static_cast<std::ptrdiff_t> (
          application_state_payload_offset + application_state_limit),
      0x5a);
    oversized_root[application_state_length_offset] = 0x04;
    oversized_root[application_state_length_offset + 1] = 0x00;
    oversized_root[application_state_length_offset + 2] = 0x00;
    oversized_root[application_state_length_offset + 3] = 0x01;
    test.require (
      !maintenance_runtime_t::decode (oversized_root),
      "decoder must reject application state above 64 MiB");

    bounded.application_state.push_back (0x5a);
    test.require (
      maintenance_runtime_t::encode (bounded, digest).empty (),
      "encoder must reject application state above 64 MiB");
}

void test_spot_restore_stages_before_publication (
  test_context_t &test)
{
    fail_first_restore_spot_t::factory_count = 0;
    fail_first_restore_spot_t::restore_count = 0;
    fail_first_restore_spot_t::create_count = 0;
    fail_first_restore_spot_t::initialize_count = 0;
    fail_first_restore_spot_t::restored_payload.clear ();

    zlink::framework::spot_node_builder_t builder;
    builder.add_spot_factory<fail_first_restore_spot_t> (
      "restored-spot",
      [] (zlink::framework::spot_context_t context) {
          ++fail_first_restore_spot_t::factory_count;
          return std::make_shared<fail_first_restore_spot_t> (
            std::move (context));
      },
      [] (auto &factory) {
          factory.template preserve_state_with<
            fail_first_restore_adapter_t> ();
      });
    auto runtime =
      zlink::framework::detail::spot_node_runtime_t::from (builder);
    const frozen_object_state_t frozen{
      .owner =
        {.kind = object_kind_t::user_spot,
         .key = "staged-spot",
         .object_generation = 7,
         .authority_owner_generation = 9,
         .mesh_name = "mesh",
         .node_id = "source"},
      .stable_type = "restored-spot",
      .application_state = {0xca, 0xfe},
      .pending_application = {},
      .timers = {}};
    const object_ref_t target{
      .kind = object_kind_t::user_spot,
      .key = "staged-spot",
      .object_generation = 7,
      .authority_owner_generation = 10,
      .mesh_name = "mesh",
      .node_id = "target"};

    auto oversized = frozen;
    oversized.application_state.assign (
      64u * 1024u * 1024u + 1u, 0x5a);
    test.require (
      !runtime.restore_spot_relocation_state (oversized, target)
        && fail_first_restore_spot_t::factory_count == 0
        && fail_first_restore_spot_t::restore_count == 0,
      "oversized state must be rejected before Spot materialization");

    test.require (
      !runtime.restore_spot_relocation_state (frozen, target)
        && !runtime.find_spot (
          zlink::framework::spot_id_t ("staged-spot"))
        && fail_first_restore_spot_t::factory_count == 1
        && fail_first_restore_spot_t::create_count == 0
        && fail_first_restore_spot_t::initialize_count == 0,
      "failed restore must discard the private Spot before publication");

    test.require (
      runtime.restore_spot_relocation_state (frozen, target)
        && runtime.find_spot (
          zlink::framework::spot_id_t ("staged-spot"))
        && fail_first_restore_spot_t::factory_count == 2
        && fail_first_restore_spot_t::restore_count == 2
        && fail_first_restore_spot_t::create_count == 0
        && fail_first_restore_spot_t::initialize_count == 1
        && fail_first_restore_spot_t::restored_payload
             == std::vector<std::byte>{
               std::byte{0xca}, std::byte{0xfe}},
      "restore retry must use a fresh instance and publish only after success");
    runtime.request_stop ();
    runtime.cancel_pending_dispatch ();
    runtime.cancel_pending_work ();
    runtime.release_native_handles ();
}

void test_concurrent_spot_restore_owns_one_reservation (
  test_context_t &test)
{
    concurrent_restore_spot_t::factory_count.store (0);
    concurrent_restore_spot_t::restore_count.store (0);
    {
        std::lock_guard lock (
          concurrent_restore_spot_t::restore_mutex);
        concurrent_restore_spot_t::restore_entered = false;
        concurrent_restore_spot_t::release_restore = false;
        concurrent_restore_spot_t::restored_payload.clear ();
    }
    zlink::framework::spot_node_builder_t builder;
    builder.add_spot_factory<concurrent_restore_spot_t> (
      "concurrent-spot",
      [] (zlink::framework::spot_context_t context) {
          return std::make_shared<concurrent_restore_spot_t> (
            std::move (context));
      },
      [] (auto &factory) {
          factory.template preserve_state_with<
            concurrent_restore_adapter_t> ();
      });
    auto runtime =
      zlink::framework::detail::spot_node_runtime_t::from (builder);
    const frozen_object_state_t frozen{
      .owner =
        {.kind = object_kind_t::user_spot,
         .key = "concurrent-id",
         .object_generation = 1,
         .authority_owner_generation = 1,
         .mesh_name = "mesh",
         .node_id = "source"},
      .stable_type = "concurrent-spot",
      .application_state = {1},
      .pending_application = {},
      .timers = {}};
    const object_ref_t target{
      .kind = object_kind_t::user_spot,
      .key = "concurrent-id",
      .object_generation = 1,
      .authority_owner_generation = 2,
      .mesh_name = "mesh",
      .node_id = "target"};

    bool first = false;
    std::thread owner ([&] {
        first =
          runtime.restore_spot_relocation_state (frozen, target);
    });
    {
        std::unique_lock lock (
          concurrent_restore_spot_t::restore_mutex);
        concurrent_restore_spot_t::restore_condition.wait (
          lock, [] {
              return concurrent_restore_spot_t::restore_entered;
          });
    }
    std::atomic_bool contender_started{false};
    bool contender_observed_existing = false;
    std::thread contender ([&] {
        contender_started.store (true);
        const auto activated = runtime.get_or_create_spot (
          "concurrent-spot",
          zlink::framework::spot_id_t ("concurrent-id"));
        contender_observed_existing =
          activated.state
          == zlink::framework::spot_create_state_t::existing;
    });
    while (!contender_started.load ())
        std::this_thread::yield ();
    std::this_thread::sleep_for (
      std::chrono::milliseconds (10));
    test.require (
      concurrent_restore_spot_t::factory_count.load () == 1
        && concurrent_restore_spot_t::restore_count.load () == 1,
      "normal activation must wait on the relocation reservation");
    {
        std::lock_guard lock (
          concurrent_restore_spot_t::restore_mutex);
        concurrent_restore_spot_t::release_restore = true;
    }
    concurrent_restore_spot_t::restore_condition.notify_all ();
    owner.join ();
    contender.join ();

    test.require (
      first && contender_observed_existing
        && concurrent_restore_spot_t::factory_count.load () == 1
        && concurrent_restore_spot_t::restore_count.load () == 1
        && concurrent_restore_spot_t::restored_payload
             == std::vector<std::byte>{std::byte{1}}
        && runtime.find_spot (
          zlink::framework::spot_id_t ("concurrent-id")),
      "one SpotId reservation must own materialization and publication");
    runtime.request_stop ();
    runtime.cancel_pending_dispatch ();
    runtime.cancel_pending_work ();
    runtime.release_native_handles ();
}

void test_restore_validates_generation_before_spot_publication (
  test_context_t &test)
{
    stateful_object_runtime_t single;
    const auto old_spot = create_spot (
      single, object_kind_t::user_spot, "stale-single");
    test.require (
      single.close_spot (old_spot)
        == std::pair{
          stateful_error_t::none, true},
      "single stale-generation setup must close the old Spot");
    int single_restore_count = 0;
    single.configure_relocation_state (
      [] (const object_ref_t &, const std::string &,
          std::stop_token) {
          return std::vector<std::uint8_t>{};
      },
      [&] (const frozen_object_state_t &,
           const object_ref_t &,
           std::stop_token) {
          ++single_restore_count;
          return true;
      });
    auto single_frozen = frozen_object_state_t{
      .owner =
        {.kind = object_kind_t::user_spot,
         .key = "stale-single",
         .object_generation = 1,
         .authority_owner_generation = 1,
         .mesh_name = "mesh",
         .node_id = "source"},
      .stable_type = "spot",
      .application_state = {1},
      .pending_application = {},
      .timers = {}};
    auto single_target = single_frozen.owner;
    single_target.authority_owner_generation = 2;
    single_target.node_id = "target";
    const relocation_restore_identity_t single_identity{
      "single-root", 1, digest_with (1)};
    test.require (
      single.restore_relocation (
        single_frozen, single_target, single_identity)
          == stateful_error_t::generation_stale
        && single_restore_count == 0
        && single.inventory ().empty (),
      "stale single restore must not publish application Spot state");

    single_frozen.owner.object_generation = 2;
    single_target.object_generation = 2;
    const auto fresh_single = single.restore_relocation (
      single_frozen, single_target, single_identity);
    const auto single_inventory = single.inventory ();
    test.require (
      fresh_single == stateful_error_t::none
        && single_restore_count == 1
        && single_inventory.size () == 1
        && single_inventory.front ().owner == single_target,
      "fresh single retry must succeed after stale validation");

    stateful_object_runtime_t aggregate;
    const auto holder = create_spot (
      aggregate, object_kind_t::user_spot, "entry:holder");
    const auto old_actor =
      create_actor (aggregate, "stale-actor");
    const auto [join_error, join] =
      aggregate.begin_membership_move (old_actor, holder);
    const auto [commit_error, joined_actor] =
      aggregate.commit_membership_move (join);
    test.require (
      join_error == stateful_error_t::none
        && commit_error == stateful_error_t::none
        && aggregate.destroy_actor (joined_actor)
             == stateful_error_t::none
        && aggregate.close_spot (holder)
             == std::pair{
               stateful_error_t::none, true},
      "aggregate stale-generation setup must retain Actor history");
    int aggregate_restore_count = 0;
    aggregate.configure_relocation_state (
      [] (const object_ref_t &, const std::string &,
          std::stop_token) {
          return std::vector<std::uint8_t>{};
      },
      [&] (const frozen_object_state_t &,
           const object_ref_t &,
           std::stop_token) {
          ++aggregate_restore_count;
          return true;
      });
    std::vector<frozen_object_state_t> participants{
      {.owner =
         {.kind = object_kind_t::user_spot,
          .key = "aggregate-spot",
          .object_generation = 1,
          .authority_owner_generation = 1,
          .mesh_name = "mesh",
          .node_id = "source"},
       .stable_type = "spot",
       .application_state = {2},
       .pending_application = {},
       .timers = {}},
      {.owner =
         {.kind = object_kind_t::actor,
          .key = "stale-actor",
          .object_generation = 1,
          .authority_owner_generation = 1,
          .mesh_name = "mesh",
          .node_id = "source"},
       .stable_type = "actor",
       .application_state = {},
       .pending_application = {},
       .timers = {}}};
    std::vector<object_ref_t> aggregate_targets{
      participants[0].owner, participants[1].owner};
    for (auto &target : aggregate_targets) {
        target.authority_owner_generation = 2;
        target.node_id = "target";
    }
    const relocation_restore_identity_t aggregate_identity{
      "aggregate-root", 2, digest_with (2)};
    test.require (
      aggregate.restore_relocation_aggregate (
        participants, aggregate_targets, aggregate_identity)
          == stateful_error_t::generation_stale
        && aggregate_restore_count == 0
        && aggregate.inventory ().empty (),
      "one stale Actor must reject the aggregate before Spot publication");

    participants[1].owner.object_generation = 2;
    aggregate_targets[1].object_generation = 2;
    const auto fresh_aggregate =
      aggregate.restore_relocation_aggregate (
        participants, aggregate_targets, aggregate_identity);
    const auto aggregate_inventory = aggregate.inventory ();
    test.require (
      fresh_aggregate == stateful_error_t::none
        && aggregate_restore_count == 1
        && aggregate_inventory.size () == 2
        && std::any_of (
          aggregate_inventory.begin (),
          aggregate_inventory.end (),
          [&] (const object_inventory_t &entry) {
              return entry.owner == aggregate_targets[0];
          })
        && std::any_of (
          aggregate_inventory.begin (),
          aggregate_inventory.end (),
          [&] (const object_inventory_t &entry) {
              return entry.owner == aggregate_targets[1];
          }),
      "fresh aggregate retry must succeed without leaked state");
}

void test_pending_restore_holds_ingress_before_rollback (
  test_context_t &test)
{
    stateful_object_runtime_t target;
    std::mutex callback_mutex;
    std::condition_variable callback_condition;
    bool callback_entered = false;
    bool release_callback = false;
    target.configure_relocation_state (
      [] (const object_ref_t &, const std::string &,
          std::stop_token) {
          return std::vector<std::uint8_t>{};
      },
      [&] (const frozen_object_state_t &,
           const object_ref_t &,
           std::stop_token) {
          std::unique_lock lock (callback_mutex);
          callback_entered = true;
          callback_condition.notify_all ();
          callback_condition.wait (
            lock, [&] { return release_callback; });
          return false;
      });
    const frozen_object_state_t frozen{
      .owner =
        {.kind = object_kind_t::user_spot,
         .key = "rollback-spot",
         .object_generation = 1,
         .authority_owner_generation = 1,
         .mesh_name = "mesh",
         .node_id = "source"},
      .stable_type = "spot",
      .application_state = {1},
      .pending_application = {},
      .timers = {}};
    auto restored_target = frozen.owner;
    restored_target.authority_owner_generation = 2;
    restored_target.node_id = "target";
    const relocation_restore_identity_t identity{
      "rollback-root", 3, digest_with (3)};
    stateful_error_t restore_result = stateful_error_t::none;
    std::thread restoring ([&] {
        restore_result = target.restore_relocation (
          frozen, restored_target, identity);
    });
    {
        std::unique_lock lock (callback_mutex);
        callback_condition.wait (
          lock, [&] { return callback_entered; });
    }
    const auto ingress = target.enqueue (
      restored_target, turn_domain_t::application,
      {1, {9}});
    {
        std::lock_guard lock (callback_mutex);
        release_callback = true;
    }
    callback_condition.notify_all ();
    restoring.join ();
    test.require (
      ingress == stateful_error_t::none
        && restore_result == stateful_error_t::conflict
        && target.inventory ().empty (),
      "pending restore must hold ingress until rollback discards it");

    target.configure_relocation_state (
      [] (const object_ref_t &, const std::string &,
          std::stop_token) {
          return std::vector<std::uint8_t>{};
      },
      [] (const frozen_object_state_t &,
          const object_ref_t &,
          std::stop_token) { return true; });
    test.require (
      target.restore_relocation (
        frozen, restored_target, identity)
          == stateful_error_t::none
        && target.inventory ().size () == 1,
      "callback failure rollback must release the fresh retry");
}

void test_aggregate_envelope_and_crash_recovery (test_context_t &test)
{
    stateful_object_runtime_t source;
    int captured_spot_state = 0;
    bool capture_received_operation_token = false;
    source.configure_relocation_state (
      [&] (const object_ref_t &owner,
           const std::string &stable_type,
           std::stop_token cancellation) {
          ++captured_spot_state;
          capture_received_operation_token =
            cancellation.stop_possible ();
          test.require (
            owner.kind == object_kind_t::user_spot
              && stable_type == "spot",
            "Spot capture must receive the exact owner and stable type");
          return std::vector<std::uint8_t>{0xca, 0xfe};
      },
      [] (const frozen_object_state_t &,
          const object_ref_t &,
          std::stop_token) { return true; });
    const auto spot =
      create_spot (source, object_kind_t::user_spot, "spot-aggregate");
    const auto actor = create_actor (source, "actor-aggregate");
    const auto [join_error, join] =
      source.begin_membership_move (actor, spot);
    const auto [commit_error, joined_actor] =
      source.commit_membership_move (join);
    test.require (
      join_error == stateful_error_t::none
        && commit_error == stateful_error_t::none,
      "aggregate setup must join the Actor to the User Spot");
    (void) source.enqueue (
      spot, turn_domain_t::application, {1, {10}});
    (void) source.enqueue (
      joined_actor, turn_domain_t::application, {2, {20}});
    (void) source.register_timer (spot, {11, 100, 25, 3});
    (void) source.register_timer (
      joined_actor, {12, 200, 0, 4});

    auto roots = std::make_shared<memory_relocation_repository_t> ();
    auto authority = std::make_shared<memory_authority_store_t> ();
    auto aggregates =
      std::make_shared<memory_aggregate_authority_t> (authority);
    auto targets = std::make_shared<target_preflight_t> ();
    maintenance_runtime_t coordinator (
      source,
      maintenance_provider_set_t{
        authority, aggregates, roots, targets});
    const auto digest = digest_with (0x6a);
    std::stop_source operation_cancellation;
    const std::vector<object_ref_t> participants{spot, joined_actor};
    const auto moved = coordinator.relocate_aggregate (
      participants, "node-b", {"owner-b", 7},
      {{"capacity-spot"}, {"capacity-actor"}},
      1024 * 1024, digest, std::nullopt,
      operation_cancellation.get_token ());
    test.require (
      moved.terminal == relocation_terminal_t::completed
        && moved.authority.size () == 2
        && captured_spot_state == 1
        && capture_received_operation_token,
      "aggregate authority commit must publish every participant");

    const auto root =
      roots->get (moved.authority.front ().relocation_reference);
    const auto decoded =
      root ? maintenance_runtime_t::decode_aggregate (*root)
           : std::nullopt;
    test.require (
      decoded && decoded->first.size () == 2
        && decoded->second == digest,
      "aggregate envelope must decode every participant and digest");
    if (root) {
        auto excessive_participants = *root;
        excessive_participants[4] = 0;
        excessive_participants[5] = 0;
        excessive_participants[6] = 0x04;
        excessive_participants[7] = 0x01;
        test.require (
          !maintenance_runtime_t::decode_aggregate (
            excessive_participants),
          "aggregate decoder must reject participant counts above the explicit maximum");
    }

    stateful_object_runtime_t recovered;
    int restored_spot_state = 0;
    bool restore_received_operation_token = false;
    recovered.configure_relocation_state (
      [] (const object_ref_t &,
          const std::string &,
          std::stop_token) {
          return std::vector<std::uint8_t>{};
      },
      [&] (const frozen_object_state_t &frozen,
           const object_ref_t &target,
           std::stop_token cancellation) {
          ++restored_spot_state;
          restore_received_operation_token =
            cancellation.stop_possible ();
          return target.kind == object_kind_t::user_spot
                 && frozen.application_state
                      == std::vector<std::uint8_t>{0xca, 0xfe};
      });
    const auto recovery =
      coordinator.recover_aggregate (
        participants, recovered,
        operation_cancellation.get_token ());
    test.require (
      recovery.terminal == relocation_terminal_t::recovery_required
        && recovery.reason == relocation_reason_t::restore_failed
        && recovery.authority.size () == 2
        && restored_spot_state == 1
        && restore_received_operation_token,
      "materialized aggregate must remain recovery-required until lifecycle and ACK completion");

    const auto target_spot =
      authority->read (object_kind_t::user_spot, "spot-aggregate");
    const auto target_actor =
      authority->read (object_kind_t::actor, "actor-aggregate");
    test.require (
      target_spot && target_actor
        && recovered.pending (
             target_spot->target, turn_domain_t::application)
             == 1
        && recovered.pending (
             target_actor->target, turn_domain_t::application)
             == 1
        && recovered.timers (target_spot->target).size () == 1
        && recovered.timers (target_actor->target).size () == 1,
      "aggregate recovery must restore each queue and logical timer");
    test.require (
      target_actor
        && recovered.actor_membership (target_actor->target)
             == std::optional<std::string>{"spot-aggregate"},
      "aggregate recovery must restore canonical User Spot membership");
    const auto staged = recovered.inventory ();
    test.require (
      staged.size () == 2
        && std::all_of (
          staged.begin (), staged.end (),
          [] (const object_inventory_t &entry) {
              return entry.state == object_state_t::recovering;
          }),
      "recovered participants must remain admission-sealed");
    if (target_actor) {
        const auto [claim_error, claim] =
          recovered.try_claim (
            target_actor->target, turn_domain_t::application);
        test.require (
          claim_error == stateful_error_t::moving && !claim,
          "staged recovery must not expose application replay as ready");
        test.require (
          recovered.enqueue_timer_tick (
            target_actor->target, 12, {99})
            == stateful_error_t::moving,
          "staged recovery must not start logical timers before completion");
    }

    const auto repeated =
      coordinator.recover_aggregate (participants, recovered);
    test.require (
      repeated.terminal == relocation_terminal_t::recovery_required
        && recovered.inventory ().size () == 2,
      "exact staged recovery retry must remain idempotent and fail closed");

    if (decoded && target_spot && target_actor) {
        std::vector<object_ref_t> restore_targets{
          target_spot->target, target_actor->target};
        const auto wrong_root =
          recovered.restore_relocation_aggregate (
            decoded->first, restore_targets,
            {"wrong-root",
             moved.authority.front ().checksum_crc32c,
             digest});
        test.require (
          wrong_root == stateful_error_t::conflict,
          "same refs with a different root identity must not be idempotent");

        auto wrong_payload = decoded->first;
        wrong_payload.front ().stable_type = "different-type";
        const auto partial_state =
          recovered.restore_relocation_aggregate (
            std::move (wrong_payload), std::move (restore_targets),
            {moved.authority.front ().relocation_reference,
             moved.authority.front ().checksum_crc32c,
             digest});
        test.require (
          partial_state == stateful_error_t::conflict,
          "same refs with different restored state must not be idempotent");

        const auto spot_frozen = std::find_if (
          decoded->first.begin (), decoded->first.end (),
          [] (const frozen_object_state_t &participant) {
              return participant.owner.kind
                     == object_kind_t::user_spot;
          });
        stateful_object_runtime_t partial;
        const auto partial_seed =
          spot_frozen == decoded->first.end ()
            ? stateful_error_t::invalid
            : partial.restore_relocation (
                *spot_frozen, target_spot->target,
                {moved.authority.front ().relocation_reference,
                 moved.authority.front ().checksum_crc32c,
                 digest});
        const auto partial_retry =
          coordinator.recover_aggregate (participants, partial);
        test.require (
          partial_seed == stateful_error_t::none
            && partial_retry.terminal
                 == relocation_terminal_t::recovery_required
            && partial.inventory ().size () == 1,
          "partial same-ref restore must not add missing participants or report completion");
    }

    if (target_actor) {
        authority->rows[authority_key (
          object_kind_t::actor, "actor-aggregate")]
          .relocation_reference = "different-root";
    }
    stateful_object_runtime_t rejected;
    const auto inconsistent =
      coordinator.recover_aggregate (participants, rejected);
    test.require (
      inconsistent.terminal == relocation_terminal_t::data_lost
        && inconsistent.reason
             == relocation_reason_t::inventory_mismatch
        && rejected.inventory ().empty (),
      "inconsistent aggregate authority must not partially restore");
}

void test_publication_and_handoff (test_context_t &test)
{
    stateful_object_runtime_t source;
    const auto actor = create_actor (source, "actor-a");
    test.require (
      source.enqueue (
        actor, turn_domain_t::application, {1, {1}})
        == stateful_error_t::none,
      "source queue setup must succeed");
    test.require (
      source.register_timer (actor, {7, 100, 0, 2})
        == stateful_error_t::none,
      "source timer setup must succeed");

    auto roots = std::make_shared<memory_relocation_repository_t> ();
    auto authority = std::make_shared<memory_authority_store_t> ();
    authority->throw_after_publish = true;
    roots->on_put = [&] {
        test.require (
          source.enqueue (
            actor, turn_domain_t::application, {2, {2}})
            == stateful_error_t::none,
          "post-seal ingress must enter the bounded hold");
        test.require (
          source.enqueue (
            actor, turn_domain_t::infrastructure, {9, {9}})
            == stateful_error_t::none,
          "infrastructure work must remain admissible while sealed");
    };
    int terminal_observations = 0;
    maintenance_runtime_t runtime (
      source, authority, roots, {},
      [&] (const relocation_result_t &) { ++terminal_observations; });
    const auto result = runtime.relocate (
      actor, "node-b", {"owner-b", 1},
      {"capacity-durable"},
      1024 * 1024, digest_with (1));
    test.require (result.terminal == relocation_terminal_t::completed,
                  "durable relocation must complete");
    test.require (terminal_observations == 1,
                  "terminal observation must be emitted exactly once");
    test.require (
      roots->log.size () == 1 && roots->log.front () == "put"
        && authority->log.size () == 1
        && authority->log.front () == "publish",
      "immutable root must be stored before authority publication");
    test.require (result.authority
                    && source.pending (
                         result.authority->target,
                         turn_domain_t::application)
                         == 2,
                  "frozen queue must precede held ingress after commit");
    test.require (
      result.authority
        && source.pending_bytes (
             result.authority->target, turn_domain_t::application)
             == 2 * (zlink::framework::runtime::dispatch_limits::
                       fixed_work_byte_cost
                     + 1),
      "frozen and held ingress must both remain in application byte accounting");
    test.require (result.authority
                    && source.pending (
                         result.authority->target,
                         turn_domain_t::infrastructure)
                         == 1,
                  "infrastructure queue must remain available through commit");
    if (result.authority) {
        const auto [first_error, first] =
          source.try_claim (
            result.authority->target, turn_domain_t::application);
        test.require (first_error == stateful_error_t::none
                        && first && first->sequence == 1,
                      "frozen queue order must be preserved");
        (void) source.complete_claim (
          result.authority->target, turn_domain_t::application);
        const auto [second_error, second] =
          source.try_claim (
            result.authority->target, turn_domain_t::application);
        test.require (second_error == stateful_error_t::none
                        && second && second->sequence == 2,
                      "held ingress must follow the frozen queue");
        test.require (source.timers (result.authority->target).size () == 1,
                      "logical timer registration must survive commit");
    }
    test.require (runtime.gate_snapshot () == relocation_gate_snapshot_t{},
                  "all scheduler permits must be released at terminal");
}

void test_conflict_aborts_without_losing_ingress (test_context_t &test)
{
    stateful_object_runtime_t source;
    const auto actor = create_actor (source, "actor-conflict");
    (void) source.enqueue (
      actor, turn_domain_t::application, {1, {1}});
    auto roots = std::make_shared<memory_relocation_repository_t> ();
    auto authority = std::make_shared<memory_authority_store_t> ();
    authority->force_conflict = true;
    roots->on_put = [&] {
        (void) source.enqueue (
          actor, turn_domain_t::application, {2, {2}});
    };
    maintenance_runtime_t runtime (source, authority, roots);
    const auto result = runtime.relocate (
      actor, "node-b", {"owner-b", 1},
      {"capacity-conflict"},
      1024 * 1024, digest_with (2));
    test.require (result.terminal == relocation_terminal_t::conflict,
                  "authority CAS conflict must be closed");
    test.require (roots->removed.size () == 1,
                  "CAS loser root must be removed as an orphan");
    test.require (
      source.pending (actor, turn_domain_t::application) == 2,
      "precommit abort must restore frozen then held ingress");
}

void test_recovery_and_data_loss (test_context_t &test)
{
    stateful_object_runtime_t source;
    const auto actor = create_actor (source, "actor-recovery");
    (void) source.enqueue (
      actor, turn_domain_t::application, {4, {9}});
    (void) source.register_timer (actor, {3, 50, 10, 5});
    auto roots = std::make_shared<memory_relocation_repository_t> ();
    auto authority = std::make_shared<memory_authority_store_t> ();
    maintenance_runtime_t coordinator (source, authority, roots);
    const auto moved = coordinator.relocate (
      actor, "node-b", {"owner-b", 1},
      {"capacity-recovery"},
      1024 * 1024, digest_with (3));
    test.require (moved.authority.has_value (),
                  "published relocation must expose recovery authority");

    stateful_object_runtime_t recovered;
    const auto recovery = coordinator.recover (
      object_kind_t::actor, "actor-recovery", recovered);
    test.require (
      recovery.terminal == relocation_terminal_t::recovery_required
        && recovery.reason == relocation_reason_t::restore_failed,
      "published root must remain staged until lifecycle and ACK completion");
    test.require (
      moved.authority
        && recovered.pending (
             moved.authority->target, turn_domain_t::application)
             == 1
        && recovered.timers (moved.authority->target).size () == 1,
      "recovery must restore queue and logical timer state");
    if (moved.authority) {
        const auto [claim_error, claim] =
          recovered.try_claim (
            moved.authority->target, turn_domain_t::application);
        test.require (
          claim_error == stateful_error_t::moving && !claim,
          "single recovery must keep application admission sealed");
    }

    if (moved.authority)
        roots->erase_without_authority_change (
          moved.authority->relocation_reference);
    stateful_object_runtime_t missing_target;
    const auto missing = coordinator.recover (
      object_kind_t::actor, "actor-recovery", missing_target);
    test.require (
      missing.terminal == relocation_terminal_t::data_lost
        && missing.reason == relocation_reason_t::payload_missing,
      "published missing payload must be terminal data loss");
    test.require (
      authority->read (object_kind_t::actor, "actor-recovery").has_value (),
      "data loss must not roll authority back to the source");
}

void test_restart_reconstructs_relocation_replay (
  test_context_t &test)
{
    namespace mesh = zlink::framework::runtime::mesh;
    namespace protocol = zlink::framework::runtime::protocol;

    const auto bytes = [] (const std::string &value) {
        return std::vector<std::uint8_t> (
          value.begin (), value.end ());
    };
    const auto descriptor = [&] (const std::string &rid) {
        return mesh::service_node_descriptor_t{
          "mesh", bytes (rid), 1, 1, "tcp://127.0.0.1:0", {},
          mesh::service_node_state_t::preparing};
    };

    stateful_object_runtime_t source;
    const auto actor = create_actor (
      source, "actor-restart-replay", "restart-source");
    protocol::frozen_application_record_t accepted;
    accepted.kind = protocol::frozen_record_kind_t::actor_request;
    accepted.source_kind = protocol::frozen_source_kind_t::node;
    accepted.source = {
      "request-owner", 7, bytes ("request-source"), 9};
    accepted.operation = {0x101, 0x202};
    accepted.operation_kind = 4;
    accepted.reply_route_id = 11;
    accepted.body = protocol::frozen_actor_application_body_t{
      {actor.key, actor.object_generation, bytes ("restart-source"),
       1, actor.authority_owner_generation, 13},
      {"ActorPacket", "application/json", bytes ("request")}};
    test.require (
      source.enqueue (
        actor, turn_domain_t::application,
        {1,
         protocol::encode_frozen_record (
           protocol::encode_frozen_application_record (accepted))})
        == stateful_error_t::none,
      "restart recovery requires one accepted request");

    auto roots = std::make_shared<memory_relocation_repository_t> ();
    auto authority = std::make_shared<memory_authority_store_t> ();
    const protocol::relocation_id_t relocation{0x303, 0x404};
    const protocol::relocation_coordinator_fence_t coordinator{
      "coordinator-owner", 17, bytes ("coordinator-node"), 19,
      "authority-version"};

    {
        mesh::raw_mesh_node_owner_t transport (
          {descriptor ("restart-source")});
        raw_relocation_replay_coordinator_t wire (transport);
        maintenance_runtime_t runtime (source, authority, roots);
        runtime.attach_relocation_wire (wire);
        eligible_relocation_unit_t::canonical_wire_context_t context{
          .relocation = relocation,
          .target_attempt_generation = 23,
          .coordinator = coordinator,
          .target_node_routing_id = bytes ("restart-target"),
          .target_node_generation = 29,
          .participant_ids = {31},
          .prepare_target =
            [] (const std::vector<frozen_object_state_t> &,
                const std::vector<protocol::relocation_data_t> &,
                const relocation_stored_t &) { return true; },
          .acknowledged = [] (std::uint64_t, std::uint64_t) {},
          .complete_source_terminal =
            [] (std::uint64_t, std::uint64_t,
                const protocol::reply_relay_t &,
                const std::optional<protocol::application_payload_t> &) {
                return true;
            },
          .complete_target = [] { return false; },
          .abort_target = [] {}};
        const auto interrupted = runtime.relocate (
          actor, "restart-target", {"target-owner", 37},
          {"restart-capacity"}, 1024 * 1024,
          digest_with (0x41), context);
        test.require (
          interrupted.terminal
            == relocation_terminal_t::recovery_required
            && interrupted.authority,
          "post-publication interruption must retain a recoverable root");
    }

    stateful_object_runtime_t recovered;
    mesh::raw_mesh_node_owner_t restarted_transport (
      {descriptor ("restart-source")});
    raw_relocation_replay_coordinator_t restarted_wire (
      restarted_transport);
    maintenance_runtime_t restarted (source, authority, roots);
    restarted.attach_relocation_wire (restarted_wire);
    bool restored_wire_identity = false;
    eligible_relocation_unit_t::canonical_wire_context_t callbacks{
      .prepare_target =
        [&] (const std::vector<frozen_object_state_t> &participants,
             const std::vector<protocol::relocation_data_t> &records,
             const relocation_stored_t &) {
            restored_wire_identity =
              participants.size () == 1 && records.size () == 1
              && records.front ().relocation == relocation
              && records.front ().target_attempt_generation == 23
              && records.front ().coordinator == coordinator
              && records.front ().participant_id == 31
              && records.front ().frozen_record
              && records.front ().frozen_record->operation
                   == accepted.operation
              && records.front ().frozen_record->reply_route_id
                   == accepted.reply_route_id;
            return restored_wire_identity;
        },
      .acknowledged = [] (std::uint64_t, std::uint64_t) {},
      .complete_source_terminal =
        [] (std::uint64_t, std::uint64_t,
            const protocol::reply_relay_t &,
            const std::optional<protocol::application_payload_t> &) {
            return true;
        },
      .complete_target = [] { return true; },
      .abort_target = [] {}};
    const auto recovery = restarted.recover (
      object_kind_t::actor, actor.key, recovered, callbacks);
    test.require (
      recovery.terminal == relocation_terminal_t::completed
        && restored_wire_identity
        && recovery.replay_records.size () == 1,
      "restart must reconstruct command 31 replay and command 33 "
      "terminal relay identity from the stored root");
}

void test_permit_precedes_seal (test_context_t &test)
{
    stateful_object_runtime_t source;
    const auto first = create_actor (source, "actor-first");
    const auto second = create_actor (source, "actor-second");
    auto roots = std::make_shared<memory_relocation_repository_t> ();
    auto authority = std::make_shared<memory_authority_store_t> ();
    std::mutex gate;
    std::condition_variable changed;
    bool entered = false;
    bool release = false;
    roots->on_put = [&] {
        std::unique_lock lock (gate);
        entered = true;
        changed.notify_all ();
        changed.wait (lock, [&] { return release; });
    };
    relocation_limits_t limits;
    limits.outbound_units = 1;
    maintenance_runtime_t runtime (
      source, authority, roots, limits);
    relocation_result_t first_result;
    std::thread worker ([&] {
        first_result = runtime.relocate (
          first, "node-b", {"owner-b", 1},
          {"capacity-first"},
          1024, digest_with (4));
    });
    {
        std::unique_lock lock (gate);
        changed.wait (lock, [&] { return entered; });
    }
    const auto second_result = runtime.relocate (
      second, "node-b", {"owner-b", 1},
      {"capacity-second"},
      1024, digest_with (5));
    test.require (
      second_result.terminal == relocation_terminal_t::blocked
        && second_result.reason
             == relocation_reason_t::permit_unavailable,
      "unit without permits must remain unsealed");
    test.require (
      source.enqueue (
        second, turn_domain_t::application, {1, {1}})
        == stateful_error_t::none,
      "permit failure must leave normal admission open");
    {
        std::lock_guard lock (gate);
        release = true;
    }
    changed.notify_all ();
    worker.join ();
    test.require (first_result.terminal == relocation_terminal_t::completed,
                  "permitted unit must complete after store resumes");
}

void test_host_preflight_is_all_or_none (test_context_t &test)
{
    stateful_object_runtime_t objects;
    const auto actor = create_actor (objects, "preflight-actor");
    auto roots = std::make_shared<memory_relocation_repository_t> ();
    auto authority = std::make_shared<memory_authority_store_t> ();
    auto aggregates =
      std::make_shared<memory_aggregate_authority_t> (authority);
    auto targets = std::make_shared<target_preflight_t> ();
    targets->status = target_preflight_status_t::target_unavailable;
    maintenance_runtime_t relocation (
      objects,
      maintenance_provider_set_t{
        authority, aggregates, roots, targets});
    stream_session_registry_t sessions (
      [&] (const std::string &key) {
          return objects.find (object_kind_t::actor, key);
      });
    host_maintenance_runtime_t host (
      objects, sessions, relocation, targets);
    host.mark_serving ();
    const auto result = host.terminate (termination_intent_t::retire);
    test.require (
      result
        == termination_result_t{
          termination_intent_t::retire,
          termination_outcome_t::blocked,
          termination_reason_t::target_unavailable},
      "one target blocker must reject the whole host preflight");
    test.require (
      roots->roots.empty ()
        && objects.enqueue (
             actor, turn_domain_t::application, {1, {1}})
             == stateful_error_t::none,
      "failed preflight must not seal an object or write relocation data");
    const auto after_blocked = objects.begin_create (
      {.kind = object_kind_t::actor,
       .key = "after-blocked",
       .stable_type = "actor",
       .mesh_name = std::optional<std::string>{"mesh"},
       .creation_request = {},
       .exclusive = true,
       .instance_intent = false});
    test.require (
      host.state () == host_runtime_state_t::serving
        && !host.terminal_result ()
        && after_blocked.status == create_status_t::reserved,
      "blocked Retire must restore Serving without a host terminal result");
}

void test_user_spot_aggregate_and_stream_barrier (
  test_context_t &test)
{
    stateful_object_runtime_t objects;
    const auto spot =
      create_spot (objects, object_kind_t::user_spot, "user-spot");
    const auto actor = create_actor (objects, "member-actor");
    const auto [move_error, move] =
      objects.begin_membership_move (actor, spot);
    const auto [commit_error, joined] =
      objects.commit_membership_move (move);
    test.require (
      move_error == stateful_error_t::none
        && commit_error == stateful_error_t::none
        && joined == actor,
      "test actor must join the User Spot before inventory");

    auto roots = std::make_shared<memory_relocation_repository_t> ();
    auto authority = std::make_shared<memory_authority_store_t> ();
    auto aggregates =
      std::make_shared<memory_aggregate_authority_t> (authority);
    auto targets = std::make_shared<target_preflight_t> ();
    bool preflight_before_seal = false;
    bool structural_admission_sealed = false;
    targets->on_preflight = [&] {
        preflight_before_seal =
          objects.enqueue (
            actor, turn_domain_t::application, {7, {7}})
          == stateful_error_t::none;
        const auto create = objects.begin_create (
          {.kind = object_kind_t::actor,
           .key = "late-actor",
           .stable_type = "actor",
           .mesh_name = std::optional<std::string>{"mesh"},
           .creation_request = {},
           .exclusive = true,
           .instance_intent = false});
        structural_admission_sealed =
          create.status == create_status_t::failed
          && create.error == stateful_error_t::moving;
    };
    maintenance_runtime_t relocation (
      objects,
      maintenance_provider_set_t{
        authority, aggregates, roots, targets});
    stream_session_registry_t sessions (
      [&] (const std::string &key) {
          return objects.find (object_kind_t::actor, key);
      });
    const auto connection = sessions.open ("stream-a");
    const auto [bind_error, binding] =
      sessions.bind (connection, actor);
    test.require (
      bind_error == stateful_error_t::none,
      "bound STREAM session setup must succeed");

    int terminal_observations = 0;
    host_maintenance_runtime_t host (
      objects, sessions, relocation, targets,
      [&] (const termination_result_t &) {
          ++terminal_observations;
      });
    host.mark_serving ();
    const auto result = host.terminate (termination_intent_t::retire);
    test.require (
      result
        == termination_result_t{
          termination_intent_t::retire,
          termination_outcome_t::stopped,
          termination_reason_t::none},
      "eligible User Spot aggregate Retire must stop normally");
    test.require (
      preflight_before_seal && structural_admission_sealed,
      "preflight must keep existing queues open while structural inventory is sealed");
    test.require (
      aggregates->prepare_count == 1
        && aggregates->commit_count == 1
        && aggregates->pending.size () == 2,
      "User Spot and its member Actor must use one aggregate commit");
    test.require (
      targets->observed_units.size () == 1
        && targets->observed_units.front ().participants.size () == 2,
      "preflight inventory must expose one bounded User Spot aggregate");
    test.require (
      !sessions.is_current (binding),
      "owner commit must fence the old STREAM binding generation");
    const auto [stale_error, stale_dispatch] =
      sessions.admit_inbound (binding);
    test.require (
      stale_error != stateful_error_t::none && !stale_dispatch,
      "old STREAM packets must not pass after the route barrier commits");
    test.require (
      terminal_observations == 1
        && host.terminal_result ().has_value (),
      "host terminal observation and stored result must complete once");
}

void test_shutdown_wins_during_retire_preflight (
  test_context_t &test)
{
    stateful_object_runtime_t objects;
    (void) create_actor (objects, "race-actor");
    auto roots = std::make_shared<memory_relocation_repository_t> ();
    auto authority = std::make_shared<memory_authority_store_t> ();
    auto aggregates =
      std::make_shared<memory_aggregate_authority_t> (authority);
    auto targets = std::make_shared<target_preflight_t> ();
    std::mutex gate;
    std::condition_variable changed;
    bool entered = false;
    bool release = false;
    targets->on_preflight = [&] {
        std::unique_lock lock (gate);
        entered = true;
        changed.notify_all ();
        changed.wait (lock, [&] { return release; });
    };
    maintenance_runtime_t relocation (
      objects,
      maintenance_provider_set_t{
        authority, aggregates, roots, targets});
    stream_session_registry_t sessions (
      [&] (const std::string &key) {
          return objects.find (object_kind_t::actor, key);
      });
    int observations = 0;
    host_maintenance_runtime_t host (
      objects, sessions, relocation, targets,
      [&] (const termination_result_t &) { ++observations; });
    host.mark_serving ();

    termination_result_t retire_result;
    termination_result_t shutdown_result;
    std::thread retire ([&] {
        retire_result = host.terminate (termination_intent_t::retire);
    });
    {
        std::unique_lock lock (gate);
        changed.wait (lock, [&] { return entered; });
    }
    std::thread shutdown ([&] {
        shutdown_result =
          host.terminate (termination_intent_t::shutdown);
    });
    while (host.intent_snapshot ()
           != std::optional<termination_intent_t>{
             termination_intent_t::shutdown}) {
        std::this_thread::yield ();
    }
    {
        std::lock_guard lock (gate);
        release = true;
    }
    changed.notify_all ();
    retire.join ();
    shutdown.join ();
    const termination_result_t expected{
      termination_intent_t::shutdown,
      termination_outcome_t::stopped,
      termination_reason_t::none};
    test.require (
      retire_result == expected && shutdown_result == expected,
      "Shutdown seal claim during Retire preflight must win for all waiters");
    test.require (
      roots->roots.empty () && aggregates->commit_count == 0,
      "winning Shutdown must not start continuity relocation");
    test.require (
      observations == 1,
      "first-intent shared operation must emit one terminal observation");
}

void test_post_commit_failure_is_force_stopped (
  test_context_t &test)
{
    stateful_object_runtime_t objects;
    (void) create_actor (objects, "commit-first");
    (void) create_actor (objects, "conflict-second");
    auto roots = std::make_shared<memory_relocation_repository_t> ();
    auto authority = std::make_shared<memory_authority_store_t> ();
    authority->conflict_on_publish = 2;
    auto aggregates =
      std::make_shared<memory_aggregate_authority_t> (authority);
    auto targets = std::make_shared<target_preflight_t> ();
    maintenance_runtime_t relocation (
      objects,
      maintenance_provider_set_t{
        authority, aggregates, roots, targets});
    stream_session_registry_t sessions (
      [&] (const std::string &key) {
          return objects.find (object_kind_t::actor, key);
      });
    host_maintenance_runtime_t host (
      objects, sessions, relocation, targets);
    host.mark_serving ();
    const auto result = host.terminate (termination_intent_t::retire);
    test.require (
      result
        == termination_result_t{
          termination_intent_t::retire,
          termination_outcome_t::force_stopped,
          termination_reason_t::relocation_failed},
      "failure after one authority commit must not return Blocked");
    test.require (
      host.state () == host_runtime_state_t::stopped
        && host.terminal_result () == result,
      "postcommit failure must finish bounded teardown in Stopped");
}

void test_public_relocation_store_adapter (test_context_t &test)
{
    auto public_store =
      std::make_shared<public_memory_relocation_repository_t> ();
    public_relocation_store_adapter_t adapter (public_store);
    const std::vector<std::uint8_t> payload{0, 1, 127, 255};
    const auto stored = adapter.put (payload, std::chrono::hours (24));
    test.require (
      stored.reference == "public-root"
        && stored.checksum_crc32c
             == maintenance_runtime_t::crc32c (payload),
      "public relocation adapter must preserve reference and CRC32C");
    test.require (
      adapter.get (stored.reference)
        == std::optional<std::vector<std::uint8_t>>{payload},
      "public relocation adapter must preserve immutable payload bytes");
    adapter.remove (stored.reference);
    test.require (
      !adapter.get (stored.reference),
      "public relocation adapter must map delete and missing results");
}

void test_public_authority_store_adapter (test_context_t &test)
{
    public_memory_authority_store_t store;
    store.snapshot =
      zlink::framework::authority_snapshot_t{
        "1",
        {std::byte{0x01}},
        7,
        11,
        {"owner-a", 3},
        std::chrono::system_clock::now ()};
    public_authority_store_adapter_t adapter (store);
    const object_ref_t source{
      object_kind_t::actor,
      "actor-public",
      7,
      11,
      "mesh",
      "node-a"};
    const zlink::framework::location_owner_token_t target_owner{
      "owner-b", 5};
    const auto published = adapter.publish (
      source, "node-b", target_owner, {"capacity-public"},
      "root-public", 42,
      digest_with (9));
    test.require (
      published.status == authority_publish_status_t::published
        && published.current
        && published.current->target.node_id == "node-b"
        && published.current->target.authority_owner_generation == 12,
      "public authority adapter must publish exact NewOwner generation");
    test.require (
      store.observed_target_owner
        && store.observed_target_owner->owner_id == "owner-b"
        && store.observed_target_owner->lease_generation == 5
        && store.observed_capacity_fence
        && store.observed_capacity_fence->value
             == "capacity-public"
        && store.snapshot->owner.owner_id == "owner-b"
        && !store.observed_keys.empty ()
        && std::all_of (
          store.observed_keys.begin (), store.observed_keys.end (),
          [] (const auto &key) {
              return key == "1:actor-public";
          }),
      "public authority adapter must pass exact target owner and capacity fence");
    const auto read =
      adapter.read (object_kind_t::actor, "actor-public");
    test.require (
      read && read->relocation_reference == "root-public"
        && read->checksum_crc32c == 42
        && read->inventory_digest == digest_with (9)
        && read->target_owner.lease_generation == 5,
      "public authority adapter must decode only its Framework-owned payload");

    const std::vector<std::byte> application_payload{
      std::byte{0x31}, std::byte{0x32}};
    store.snapshot =
      zlink::framework::authority_snapshot_t{
        "10",
        application_payload,
        7,
        12,
        {"owner-b", 5},
        std::chrono::system_clock::now ()};
    const auto completion_published =
      adapter.publish_completion (
        object_kind_t::actor,
        "actor-public",
        "mesh",
        7,
        "completion-prepared",
        51);
    const auto completion_replaced =
      adapter.replace_completion (
        object_kind_t::actor,
        "actor-public",
        7,
        "completion-prepared",
        51,
        "completion-delivered",
        52);
    const auto completion_read =
      adapter.read (
        object_kind_t::actor,
        "actor-public");
    test.require (
      completion_published.status
          == authority_publish_status_t::published
        && completion_replaced.status
             == authority_publish_status_t::published
        && completion_read
        && completion_read->relocation_reference
             == "completion-delivered"
        && completion_read->checksum_crc32c == 52
        && completion_read->source.object_generation == 7
        && completion_read->source.authority_owner_generation
             == 12,
      "completion cursor roots must use exact preserve-generation authority CAS");
    const auto completion_released =
      adapter.release_completion (
        object_kind_t::actor,
        "actor-public",
        7,
        "completion-delivered",
        52);
    test.require (
      completion_released
        && store.snapshot
        && store.snapshot->payload == application_payload
        && store.snapshot->authority_owner_generation == 12
        && !adapter.read (
          object_kind_t::actor,
          "actor-public"),
      "Delivered release must restore authority payload before root cleanup");
}

void test_durable_join_completion_replacement_and_ordering (
  test_context_t &test)
{
    auto store = std::make_shared<memory_relocation_repository_t> ();
    durable_join_completion_store_t source (store);
    const object_ref_t actor{
      object_kind_t::actor, "actor-join", 7, 12,
      "mesh", "node-b"};
    auto root = source.prepare (
      durable_join_completion_record_t{
        0x1111, 0x2222, actor, {4, 5, 6},
        join_completion_cursor_t::prepared});
    root = source.commit (root);

    std::vector<std::string> events;
    const auto failed_root = source.deliver (
      root, actor,
      [&] (const durable_join_completion_record_t &record) {
          events.push_back ("callback-failed");
          test.require (
            record.operation_id_high == 0x1111
              && record.operation_id_low == 0x2222
              && record.raw_reply
                   == std::vector<std::uint8_t> ({4, 5, 6}),
            "replacement callback must retain operation id and raw reply");
          return false;
      });
    test.require (
      failed_root.reference == root.reference,
      "failed callback must retain the committed immutable root");

    durable_join_completion_store_t replacement (store);
    int delivered = 0;
    root = replacement.deliver (
      failed_root, actor,
      [&] (const durable_join_completion_record_t &) {
          ++delivered;
          events.push_back ("callback-delivered");
          return true;
      });
    events.push_back ("backlog");
    const auto deduplicated = replacement.deliver (
      root, actor,
      [&] (const durable_join_completion_record_t &) {
          ++delivered;
          return true;
      });
    test.require (
      delivered == 1
        && events
             == std::vector<std::string> (
               {"callback-failed", "callback-delivered", "backlog"})
        && deduplicated.reference == root.reference,
      "replacement must deliver once before opening backlog");

    auto stale = actor;
    ++stale.object_generation;
    bool fenced = false;
    try {
        (void) replacement.deliver (root, stale, {});
    }
    catch (const std::invalid_argument &) {
        fenced = true;
    }
    test.require (
      fenced,
      "replacement must reject a mismatched Actor generation");
    replacement.cleanup (root);
    test.require (
      !store->get (root.reference),
      "delivered Join completion root must be removed after cleanup");
}

} // namespace
void test_production_relocation_restore_and_replay_vertical (
  test_context_t &test)
{
    namespace mesh = zlink::framework::runtime::mesh;
    namespace protocol = zlink::framework::runtime::protocol;
    using namespace std::chrono_literals;

    const auto bytes = [] (const std::string &value) {
        return std::vector<std::uint8_t> (value.begin (), value.end ());
    };
    const auto descriptor = [&] (const std::string &rid) {
        return mesh::service_node_descriptor_t{
          "mesh", bytes (rid), 1, 1, "tcp://127.0.0.1:0", {},
          mesh::service_node_state_t::preparing};
    };

    mesh::raw_mesh_node_owner_t source_transport (
      {descriptor ("maintenance-source")});
    mesh::raw_mesh_node_owner_t target_transport (
      {descriptor ("maintenance-target")});
    source_transport.start ();
    target_transport.start ();
    const auto source_descriptor =
      source_transport.topology ().local_descriptor ();
    const auto target_descriptor =
      target_transport.topology ().local_descriptor ();
    const auto deadline = std::chrono::steady_clock::now () + 5s;
    test.require (
      source_transport.connect_peer (
        target_transport.endpoint (), target_descriptor),
      "production relocation vertical must connect source to target");
    while ((!source_transport.topology ().peer (
               target_descriptor.node_routing_id)
            || !target_transport.topology ().peer (
              source_descriptor.node_routing_id))
           && std::chrono::steady_clock::now () < deadline) {
        const auto now = mesh::service_liveness_registry_t::clock_t::now ();
        (void) source_transport.drain_monitor_events (now);
        (void) target_transport.drain_monitor_events (now);
        (void) source_transport.pump_one (now);
        (void) target_transport.pump_one (now);
        std::this_thread::yield ();
    }
    test.require (
      source_transport.topology ().peer (
        target_descriptor.node_routing_id).has_value ()
        && target_transport.topology ().peer (
          source_descriptor.node_routing_id).has_value (),
      "production relocation vertical requires two Ready owners");

    stateful_object_runtime_t source_objects;
    stateful_object_runtime_t target_objects;
    const auto actor = create_actor (
      source_objects, "production-replay-actor",
      "maintenance-source");

    protocol::frozen_application_record_t accepted;
    accepted.kind = protocol::frozen_record_kind_t::actor_request;
    accepted.source_kind = protocol::frozen_source_kind_t::node;
    accepted.source = {
      "source-owner", 17, source_descriptor.node_routing_id,
      source_descriptor.lifecycle_generation};
    accepted.operation = {
      0x1111222233334444ULL, 0x5555666677778888ULL};
    accepted.operation_kind = 4;
    accepted.reply_route_id = 77;
    accepted.body = protocol::frozen_actor_application_body_t{
      {actor.key, actor.object_generation,
       source_descriptor.node_routing_id,
       source_descriptor.lifecycle_generation,
       actor.authority_owner_generation, 19},
      {"ActorPacket", "application/json", bytes ("accepted")}};

    const auto canonical =
      protocol::encode_frozen_application_record (accepted);
    test.require (
      source_objects.enqueue (
        actor, turn_domain_t::application,
        {1, protocol::encode_frozen_record (canonical)})
        == stateful_error_t::none,
      "production relocation vertical must queue a canonical accepted request");

    raw_relocation_replay_coordinator_t source_wire (source_transport);
    raw_relocation_replay_coordinator_t target_wire (target_transport);
    auto roots = std::make_shared<memory_relocation_repository_t> ();
    auto authority = std::make_shared<memory_authority_store_t> ();
    maintenance_runtime_t maintenance (
      source_objects, authority, roots);
    maintenance.attach_relocation_wire (source_wire);

    const protocol::relocation_id_t relocation{301, 302};
    const protocol::relocation_coordinator_fence_t coordinator{
      "coordinator-owner", 23, bytes ("coordinator-rid"), 29,
      "authority-store-version"};
    object_ref_t target_actor = actor;
    target_actor.node_id = "maintenance-target";
    ++target_actor.authority_owner_generation;

    int target_restore = 0;
    int target_stage = 0;
    int target_abort = 0;
    int source_terminal_completions = 0;
    int target_terminal_acks = 0;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> acknowledged;
    eligible_relocation_unit_t::canonical_wire_context_t wire_context{
      .relocation = relocation,
      .target_attempt_generation = 31,
      .coordinator = coordinator,
      .target_node_routing_id = target_descriptor.node_routing_id,
      .target_node_generation = target_descriptor.lifecycle_generation,
      .participant_ids = {1},
      .prepare_target =
        [&] (const std::vector<frozen_object_state_t> &participants,
             const std::vector<protocol::relocation_data_t> &records,
             const relocation_stored_t &) {
            if (participants.size () != 1 || records.size () != 1
                || !records.front ().frozen_record)
                return false;
            const auto &frozen = *records.front ().frozen_record;
            if (frozen.source != accepted.source
                || frozen.operation != accepted.operation
                || frozen.reply_route_id != accepted.reply_route_id)
                return false;

            auto restored = participants.front ();
            restored.pending_application.clear ();
            const auto restore_error = target_objects.restore_relocation (
              std::move (restored), target_actor,
              {"production-restore", 1, digest_with (0x31)});
            if (restore_error != stateful_error_t::none
                && restore_error != stateful_error_t::already_exists)
                return false;
            ++target_restore;

            return target_wire.register_target ({
              relocation, 31, coordinator, 1,
              source_descriptor.node_routing_id,
              source_descriptor.lifecycle_generation,
              records.front ().object,
              [&] (const protocol::relocation_data_t &record) {
                  if (!record.frozen_record)
                      return false;
                  ++target_stage;
                  return target_objects.enqueue (
                           target_actor, turn_domain_t::application,
                           {record.sequence,
                            protocol::encode_frozen_record (
                              *record.frozen_record)})
                         == stateful_error_t::none;
              }});
        },
      .acknowledged =
        [&] (std::uint64_t participant, std::uint64_t high_water) {
            acknowledged.emplace_back (participant, high_water);
        },
      .complete_source_terminal =
        [&] (
          std::uint64_t participant,
          std::uint64_t sequence,
          const protocol::reply_relay_t &relay,
          const std::optional<protocol::application_payload_t> &reply) {
            if (participant != 1 || sequence != 1
                || relay.operation != accepted.operation
                || relay.reply_route_id != *accepted.reply_route_id
                || relay.terminal_result != 0 || !reply
                || reply->packet_name != "ActorReply"
                || reply->payload != bytes ("reply"))
                return false;
            ++source_terminal_completions;
            return true;
        },
      .complete_target = [] { return true; },
      .abort_target = [&] { ++target_abort; }};

    const auto result = maintenance.relocate (
      actor, "maintenance-target", {"target-owner", 37},
      {"capacity-fence"}, 1024 * 1024, digest_with (0x31),
      wire_context);
    test.require (
      result.terminal == relocation_terminal_t::completed
        && result.replay_records.size () == 1
        && target_restore == 1 && target_abort == 0,
      "maintenance must prepare target Restore and retain one replay record");

    raw_relocation_replay_result_t target_result =
      raw_relocation_replay_result_t::no_data;
    raw_relocation_replay_result_t source_result =
      raw_relocation_replay_result_t::no_data;
    while ((target_result == raw_relocation_replay_result_t::no_data
            || source_result == raw_relocation_replay_result_t::no_data)
           && std::chrono::steady_clock::now () < deadline) {
        const auto now = mesh::service_liveness_registry_t::clock_t::now ();
        if (target_result == raw_relocation_replay_result_t::no_data) {
            (void) target_transport.pump_one (now);
            target_result = target_wire.pump_one ();
        }
        if (source_result == raw_relocation_replay_result_t::no_data) {
            (void) source_transport.pump_one (now);
            source_result = source_wire.pump_one ();
        }
        std::this_thread::yield ();
    }
    test.require (
      target_result == raw_relocation_replay_result_t::applied
        && source_result
             == raw_relocation_replay_result_t::ack_advanced
        && target_stage == 1
        && acknowledged
             == std::vector<std::pair<std::uint64_t, std::uint64_t>>{
               {1, 1}}
        && target_objects.pending (
             target_actor, turn_domain_t::application) == 1,
      "command 31/32 must stage the exact request and persist monotonic ACK");

    const protocol::reply_relay_t terminal{
      accepted.operation,
      *accepted.reply_route_id,
      relocation,
      31,
      coordinator,
      1,
      1,
      0,
      protocol::framework_error_code::none};
    test.require (
      target_wire.register_terminal_target ({
        terminal,
        accepted.source,
        protocol::application_payload_t{
          "ActorReply", "application/json", bytes ("reply")},
        [&] (protocol::reply_relay_ack_status_t status) {
            if (status
                  != protocol::reply_relay_ack_status_t::terminal_received
                && status
                     != protocol::reply_relay_ack_status_t::already_terminal)
                return false;
            ++target_terminal_acks;
            return true;
        },
        [] { return true; }}),
      "target terminal must register after the replayed request completes");
    (void) target_wire.retry_terminal_relays (
      raw_relocation_replay_coordinator_t::clock_t::now ());
    raw_relocation_replay_result_t source_terminal =
      raw_relocation_replay_result_t::no_data;
    raw_relocation_replay_result_t target_terminal =
      raw_relocation_replay_result_t::no_data;
    while ((source_terminal
              == raw_relocation_replay_result_t::no_data
            || target_terminal
                 == raw_relocation_replay_result_t::no_data)
           && std::chrono::steady_clock::now () < deadline) {
        const auto now =
          mesh::service_liveness_registry_t::clock_t::now ();
        (void) source_transport.pump_one (now);
        (void) target_transport.pump_one (now);
        if (source_terminal
            == raw_relocation_replay_result_t::no_data)
            source_terminal = source_wire.pump_one ();
        if (target_terminal
            == raw_relocation_replay_result_t::no_data)
            target_terminal = target_wire.pump_one ();
        std::this_thread::yield ();
    }
    test.require (
      source_terminal
          == raw_relocation_replay_result_t::terminal_received
        && target_terminal
             == raw_relocation_replay_result_t::relay_acknowledged
        && source_terminal_completions == 1
        && target_terminal_acks == 1
        && target_wire.pending_terminal_relays () == 0,
      "maintenance must own the source terminal registration before replay");

    const auto idle_actor = create_actor (
      source_objects, "production-idle-actor",
      "maintenance-source");
    auto idle_target = idle_actor;
    idle_target.node_id = "maintenance-target";
    ++idle_target.authority_owner_generation;
    int idle_target_prepare = 0;
    int idle_target_abort = 0;
    eligible_relocation_unit_t::canonical_wire_context_t idle_wire_context{
      .relocation = {401, 402},
      .target_attempt_generation = 41,
      .coordinator = coordinator,
      .target_node_routing_id = target_descriptor.node_routing_id,
      .target_node_generation = target_descriptor.lifecycle_generation,
      .participant_ids = {2},
      .prepare_target =
        [&] (const std::vector<frozen_object_state_t> &participants,
             const std::vector<protocol::relocation_data_t> &records,
             const relocation_stored_t &) {
            if (participants.size () != 1 || !records.empty ())
                return false;
            ++idle_target_prepare;
            const auto restored = target_objects.restore_relocation (
              participants.front (), idle_target,
              {"production-idle-restore", 1, digest_with (0x41)});
            return restored == stateful_error_t::none
                   || restored == stateful_error_t::already_exists;
        },
      .acknowledged = [] (std::uint64_t, std::uint64_t) {},
      .complete_target = [] { return true; },
      .abort_target = [&] { ++idle_target_abort; }};
    const auto idle_result = maintenance.relocate (
      idle_actor, "maintenance-target", {"target-owner", 47},
      {"capacity-fence-idle"}, 1024 * 1024, digest_with (0x41),
      idle_wire_context);
    test.require (
      idle_result.terminal == relocation_terminal_t::completed
        && idle_result.replay_records.empty ()
        && idle_target_prepare == 1
        && idle_target_abort == 0,
      "queue-free relocation must prepare and restore the target before authority publication");

    source_transport.close ();
    target_transport.close ();
}

void test_target_replay_limits_are_relocation_scoped (test_context_t &test)
{
    namespace mesh = zlink::framework::runtime::mesh;
    namespace protocol = zlink::framework::runtime::protocol;

    const auto bytes = [] (const std::string &value) {
        return std::vector<std::uint8_t> (value.begin (), value.end ());
    };
    const auto descriptor = [&] (const std::string &rid) {
        return mesh::service_node_descriptor_t{
          "mesh", bytes (rid), 1, 1, "tcp://127.0.0.1:0", {},
          mesh::service_node_state_t::preparing};
    };
    mesh::raw_mesh_node_owner_t transport ({descriptor ("target-limit")});
    transport.start ();
    const auto target_rid = transport.topology ().local_descriptor ()
                              .node_routing_id;
    const auto source_rid = bytes ("source-limit");
    const protocol::relocation_id_t relocation{601, 602};
    const protocol::relocation_coordinator_fence_t coordinator{
      "coordinator-owner", 23, bytes ("coordinator-limit"), 29,
      "authority-store-version"};
    const protocol::request_source_fence_t source_fence{
      "source-owner", 17, source_rid, 19};
    const protocol::relocation_object_t target_object{
      protocol::relocation_object_kind_t::actor,
      "actor",
      "target-record",
      1,
      2};
    raw_relocation_replay_coordinator_t wire (transport);
    std::size_t staged_records = 0;

    const auto register_target = [&] (
      std::uint64_t attempt,
      std::uint64_t participant,
      std::function<bool (const protocol::relocation_data_t &)> stage) {
        raw_relocation_target_registration_t registration;
        registration.relocation = relocation;
        registration.target_attempt_generation = attempt;
        registration.coordinator = coordinator;
        registration.participant_id = participant;
        registration.relocation_source_node_routing_id = source_rid;
        registration.relocation_source_node_generation = 19;
        registration.object = target_object;
        registration.stage = std::move (stage);
        return wire.register_target (std::move (registration));
    };
    const auto make_record = [&] (std::uint64_t attempt,
                                  std::uint64_t participant,
                                  std::uint64_t sequence,
                                  std::size_t payload_size,
                                  std::uint64_t operation_low) {
        protocol::frozen_application_record_t application;
        application.kind = protocol::frozen_record_kind_t::actor_request;
        application.source_kind = protocol::frozen_source_kind_t::node;
        application.source = source_fence;
        application.operation = {0x1111, operation_low};
        application.operation_kind = 4;
        application.reply_route_id = sequence;
        application.body = protocol::frozen_actor_application_body_t{
          {target_object.object_id,
           target_object.object_generation,
           target_rid,
           1,
           target_object.expected_authority_owner_generation,
           1},
          {"ActorPacket", "application/json",
           std::vector<std::uint8_t> (payload_size, 0x42)}};
        const auto frozen =
          protocol::encode_frozen_application_record (application);
        protocol::relocation_data_t data;
        data.relocation = relocation;
        data.target_attempt_generation = attempt;
        data.coordinator = coordinator;
        data.sender_role = protocol::relocation_role_t::source;
        data.participant_id = participant;
        data.sequence = sequence;
        data.source = source_fence;
        data.object = target_object;
        data.phase = protocol::relocation_phase_t::prepared;
        data.frozen_record = frozen;
        mesh::service_mailbox_record_t record;
        record.owner = "target-limit";
        record.domain = mesh::service_mailbox_domain_t::infrastructure;
        record.parts.push_back (
          protocol::encode_relocation_control (data));
        record.source_routing_id = source_rid;
        record.source_node_generation = source_fence.node_generation;
        return record;
    };

    const auto counting_stage = [&] (const protocol::relocation_data_t &) {
        ++staged_records;
        return true;
    };
    test.require (
      register_target (1, 1, counting_stage),
      "target replay limit test must register its target participant");
    bool count_records_accepted = true;
    for (std::uint64_t sequence = 1; sequence <= 1024; ++sequence) {
        const auto result = wire.process (
          make_record (1, 1, sequence, 0, sequence));
        count_records_accepted =
          (result == raw_relocation_replay_result_t::applied
           || result == raw_relocation_replay_result_t::transport_failed)
          && count_records_accepted;
    }
    const auto count_overflow = wire.process (
      make_record (1, 1, 1025, 0, 1025));
    test.require (
      count_records_accepted
        && count_overflow == raw_relocation_replay_result_t::restore_failed
        && staged_records == 1024
        && wire.target_high_water (relocation, 1, 1) == 1024,
      "one target participant must reject its 1,025th temporary record");
    test.require (
      wire.unregister_target (relocation, 1, 1),
      "target replay limit test must release the count-limited target");

    staged_records = 0;
    test.require (
      register_target (2, 1, counting_stage),
      "target replay byte test must register its target participant");
    const auto byte_overflow = wire.process (
      make_record (2, 1, 1, 16u * 1024u * 1024u, 1));
    test.require (
      byte_overflow == raw_relocation_replay_result_t::restore_failed
        && staged_records == 0
        && wire.target_high_water (relocation, 2, 1) == 0,
      "one target participant must reject a temporary record over 16 MiB");
    (void) wire.unregister_target (relocation, 2, 1);

    staged_records = 0;
    test.require (
      register_target (3, 1, counting_stage)
        && register_target (3, 2, counting_stage),
      "target replay group test must register both participants");
    bool shared_records_accepted = true;
    for (std::uint64_t sequence = 1; sequence <= 512; ++sequence) {
        const auto first = wire.process (
          make_record (3, 1, sequence, 0, 1000 + sequence));
        const auto second = wire.process (
          make_record (3, 2, sequence, 0, 2000 + sequence));
        shared_records_accepted =
          (first == raw_relocation_replay_result_t::applied
           || first == raw_relocation_replay_result_t::transport_failed)
          && (second == raw_relocation_replay_result_t::applied
              || second
                   == raw_relocation_replay_result_t::transport_failed)
          && shared_records_accepted;
    }
    const auto shared_overflow = wire.process (
      make_record (3, 1, 513, 0, 1513));
    test.require (
      shared_records_accepted
        && shared_overflow == raw_relocation_replay_result_t::restore_failed
        && staged_records == 1024,
      "target replay limits must be shared across relocation participants");
    test.require (
      wire.unregister_target (relocation, 3, 2),
      "target replay group test must unregister one participant");
    const auto after_unregister = wire.process (
      make_record (3, 1, 513, 0, 1513));
    test.require (
      (after_unregister == raw_relocation_replay_result_t::applied
       || after_unregister
            == raw_relocation_replay_result_t::transport_failed),
      "target replay accounting must release one participant's retained records");
    (void) wire.unregister_target (relocation, 3, 1);

    std::mutex concurrent_mutex;
    std::condition_variable concurrent_condition;
    std::size_t concurrent_stage_calls = 0;
    std::size_t concurrent_finished = 0;
    bool release_concurrent_stage = false;
    const auto concurrent_stage = [&] (
      const protocol::relocation_data_t &) {
        std::unique_lock lock (concurrent_mutex);
        ++concurrent_stage_calls;
        concurrent_condition.notify_all ();
        concurrent_condition.wait (
          lock, [&] { return release_concurrent_stage; });
        return true;
    };
    test.require (
      register_target (4, 1, concurrent_stage)
        && register_target (4, 2, concurrent_stage),
      "target replay identity test must register both participants");
    raw_relocation_replay_result_t concurrent_first =
      raw_relocation_replay_result_t::invalid;
    raw_relocation_replay_result_t concurrent_second =
      raw_relocation_replay_result_t::invalid;
    std::thread first_replay ([&] {
        const auto result = wire.process (
          make_record (4, 1, 1, 0, 777));
        std::lock_guard lock (concurrent_mutex);
        concurrent_first = result;
        ++concurrent_finished;
        concurrent_condition.notify_all ();
    });
    std::thread second_replay ([&] {
        const auto result = wire.process (
          make_record (4, 2, 1, 0, 777));
        std::lock_guard lock (concurrent_mutex);
        concurrent_second = result;
        ++concurrent_finished;
        concurrent_condition.notify_all ();
    });
    {
        std::unique_lock lock (concurrent_mutex);
        concurrent_condition.wait_for (
          lock, std::chrono::seconds (5), [&] {
              return concurrent_stage_calls >= 2
                     || concurrent_finished != 0;
          });
        release_concurrent_stage = true;
    }
    concurrent_condition.notify_all ();
    first_replay.join ();
    second_replay.join ();
    const auto accepted_result = [] (raw_relocation_replay_result_t result) {
        return result == raw_relocation_replay_result_t::applied
               || result == raw_relocation_replay_result_t::transport_failed;
    };
    test.require (
      concurrent_stage_calls == 1
        && ((concurrent_first
               == raw_relocation_replay_result_t::conflicting_duplicate
             && accepted_result (concurrent_second))
            || (concurrent_second
                  == raw_relocation_replay_result_t::conflicting_duplicate
                && accepted_result (concurrent_first))),
      "target replay must reserve operation identity across participants before staging");
    (void) wire.unregister_target (relocation, 4, 1);
    (void) wire.unregister_target (relocation, 4, 2);

    std::mutex closing_mutex;
    std::condition_variable closing_condition;
    std::size_t closing_stage_calls = 0;
    bool release_closing_stage = false;
    const auto closing_stage = [&] (
      const protocol::relocation_data_t &) {
        std::unique_lock lock (closing_mutex);
        ++closing_stage_calls;
        closing_condition.notify_all ();
        closing_condition.wait (
          lock, [&] { return release_closing_stage; });
        return true;
    };
    test.require (
      register_target (5, 1, closing_stage),
      "target replay close test must register its target participant");
    raw_relocation_replay_result_t closing_result =
      raw_relocation_replay_result_t::invalid;
    std::thread closing_replay ([&] {
        closing_result = wire.process (
          make_record (5, 1, 1, 0, 888));
    });
    {
        std::unique_lock lock (closing_mutex);
        test.require (
          closing_condition.wait_for (
            lock, std::chrono::seconds (5), [&] {
                return closing_stage_calls == 1;
            }),
          "target replay close test must enter staging before sealing");
    }
    test.require (
      wire.seal_target (relocation, 5, 1),
      "target replay close test must seal admission before cleanup");
    {
        std::lock_guard lock (closing_mutex);
        release_closing_stage = true;
    }
    closing_condition.notify_all ();
    closing_replay.join ();
    test.require (
      closing_result == raw_relocation_replay_result_t::restore_failed
        && wire.target_high_water (relocation, 5, 1) == 0,
      "sealed target staging must not advance high-water or emit an ACK");
    test.require (
      wire.unregister_target (relocation, 5, 1),
      "target replay close test must unregister the sealed participant");
    transport.close ();
}

void test_application_relocation_uses_maintenance_and_fails_closed (
  test_context_t &test)
{
    using namespace std::chrono_literals;
    namespace detail = zlink::framework::detail;
    namespace framework = zlink::framework;

    auto state =
      std::make_shared<detail::mesh_node_builder_state_t> (
        "production-relocation-mesh");
    state->listen_endpoint = "tcp://127.0.0.1:0";
    state->routing_id =
      zlink::routing_id_t::from ("production-relocation-source");
    state->spot_state->snapshot.actor_types.push_back (
      "production.actor");

    auto roots = std::make_shared<memory_relocation_repository_t> ();
    auto authority = std::make_shared<memory_authority_store_t> ();
    detail::mesh_node_runtime_t node (state);
    node.configure_relocation_runtime (authority, roots);
    node.start ();

    const auto created = node.create_application_actor (
      "production.actor", "production-fail-closed",
      std::nullopt, 1s);
    test.require (
      static_cast<bool> (created),
      "production relocation test Actor must be created");
    if (!created) {
        node.stop ();
        return;
    }
    const auto actor = created.value ();
    const auto status = node.status ();
    framework::authority_snapshot_t snapshot{
      .store_version = "authority-v1",
      .payload = {},
      .object_generation = actor.object_generation (),
      .authority_owner_generation = 1,
      .owner = {"source-owner", 1},
      .store_now = std::chrono::system_clock::now (),
      .allocation =
        {framework::placement_allocation_state_t::active,
         framework::placement_object_kind_t::actor,
         "production.actor",
         {"production-relocation-mesh",
          framework::node_rid_t::from_string (
            status.routing_id ().to_string ()),
          status.lifecycle_generation (),
          {"source-owner", 1}},
         {1, 0, std::nullopt}}};
    framework::mesh_node_descriptor_t target;
    target.mesh_name = "production-relocation-mesh";
    target.rid =
      zlink::routing_id_t::from ("production-relocation-target");
    target.lifecycle_generation = 7;
    target.owner_id = "target-owner";
    target.lease_generation = 9;

    const auto result = node.relocate_application_actor (
      actor, target, snapshot, {"capacity-reservation"});
    const auto object = node.native_node ().resolve_actor (actor);
    test.require (
      result.terminal == relocation_terminal_t::blocked
        && result.reason == relocation_reason_t::restore_failed
        && roots->roots.empty ()
        && authority->rows.empty ()
        && object
        && node.native_node ().objects ().enqueue (
             *object, turn_domain_t::application, {1, {1}})
             == stateful_error_t::none,
      "production app relocation must enter maintenance but keep source authority and admission when target Restore is unavailable");
    node.stop ();
}

void test_application_relocation_remote_production_path (
  test_context_t &test)
{
    using namespace std::chrono_literals;
    namespace detail = zlink::framework::detail;
    namespace framework = zlink::framework;

    const auto make_state = [] (
      const std::string &rid) {
        auto state =
          std::make_shared<detail::mesh_node_builder_state_t> (
            "production-relocation-mesh");
        state->listen_endpoint = "tcp://127.0.0.1:0";
        state->routing_id = zlink::routing_id_t::from (rid);
        state->spot_state->snapshot.actor_types.push_back (
          "production.actor");
        return state;
    };
    auto roots = std::make_shared<memory_relocation_repository_t> ();
    auto authority = std::make_shared<memory_authority_store_t> ();
    detail::mesh_node_runtime_t source (
      make_state ("production-source"));
    detail::mesh_node_runtime_t target (
      make_state ("production-target"));
    source.configure_relocation_runtime (authority, roots);
    target.configure_relocation_runtime (authority, roots);
    source.configure_stateful_dispatch (
      [] (const accepted_record_authority_query_t &query)
        -> std::optional<accepted_record_authority_t> {
          return accepted_record_authority_t{
            {"target-owner", 9,
             query.source_node_routing_id,
             query.source_node_generation},
            1};
      });
    target.configure_stateful_dispatch (
      [] (const accepted_record_authority_query_t &query)
        -> std::optional<accepted_record_authority_t> {
          return accepted_record_authority_t{
            {"source-owner", 1,
             query.source_node_routing_id,
             query.source_node_generation},
            9};
      });
    source.configure_session_route_owner (
      [] {
          return std::optional<framework::location_owner_token_t>{
            {"source-owner", 1}};
      });
    target.configure_session_route_owner (
      [] {
          return std::optional<framework::location_owner_token_t>{
            {"target-owner", 9}};
      });
    source.start ();
    target.start ();
    source.connect_peer (
      target.status ().routing_id (),
      target.status ().local_endpoint ());
    const auto admission_deadline =
      std::chrono::steady_clock::now () + 5s;
    while ((!source.has_admitted_peer (
               target.status ().routing_id (),
               target.status ().lifecycle_generation ())
            || !target.has_admitted_peer (
              source.status ().routing_id (),
              source.status ().lifecycle_generation ()))
           && std::chrono::steady_clock::now ()
                < admission_deadline) {
        (void) source.dispatch_ready (
          [] (const auto &, const auto &, auto) {});
        (void) target.dispatch_ready (
          [] (const auto &, const auto &, auto) {});
        std::this_thread::sleep_for (1ms);
    }
    test.require (
      source.has_admitted_peer (
        target.status ().routing_id (),
        target.status ().lifecycle_generation ()),
      "production relocation source must admit the target");

    const auto created = source.create_application_actor (
      "production.actor", "production-remote-actor",
      std::nullopt, 1s);
    test.require (
      static_cast<bool> (created),
      "remote production relocation Actor must be created");
    if (!created) {
        source.stop ();
        target.stop ();
        return;
    }
    const auto actor = created.value ();
    framework::authority_snapshot_t snapshot{
      .store_version = "authority-remote-v1",
      .payload = {},
      .object_generation = actor.object_generation (),
      .authority_owner_generation = 1,
      .owner = {"source-owner", 1},
      .store_now = std::chrono::system_clock::now (),
      .allocation =
        {framework::placement_allocation_state_t::active,
         framework::placement_object_kind_t::actor,
         "production.actor",
         {"production-relocation-mesh",
          framework::node_rid_t::from_string (
            source.status ().routing_id ().to_string ()),
          source.status ().lifecycle_generation (),
          {"source-owner", 1}},
         {1, 0, std::nullopt}}};
    framework::mesh_node_descriptor_t target_descriptor;
    target_descriptor.mesh_name =
      "production-relocation-mesh";
    target_descriptor.rid = target.status ().routing_id ();
    target_descriptor.lifecycle_generation =
      target.status ().lifecycle_generation ();
    target_descriptor.application_version = 1;
    target_descriptor.owner_id = "target-owner";
    target_descriptor.lease_generation = 9;

    zlink::framework::runtime::host::operation_id_t replay_operation;
    test.require (
      target.request_to_actor (
        actor,
        {zlink::message_t::from ("relocation-request")},
        replay_operation, 30s, {}, 1, 1)
        == zlink::submit_result_t::ok,
      "production relocation request must be admitted by the caller");
    const auto pump_deadline =
      std::chrono::steady_clock::now () + 5s;
    while (std::chrono::steady_clock::now () < pump_deadline) {
        const auto pumped =
          source.native_node ().transport ().pump_one (
            zlink::framework::runtime::mesh::
              service_liveness_registry_t::clock_t::now ());
        if (pumped
            == zlink::framework::runtime::mesh::
              raw_mesh_pump_result_t::application)
            break;
        std::this_thread::sleep_for (1ms);
    }
    const auto source_object =
      source.native_node ().resolve_actor (actor);
    test.require (
      source_object
        && source.native_node ().ingest_stateful (*source_object)
             == stateful_error_t::none,
      "production relocation must capture an accepted request in the durable Actor journal");

    std::atomic<bool> stop_dispatch{false};
    std::atomic<int> replay_dispatches{0};
    std::atomic<int> target_callbacks{0};
    std::atomic<int> target_last_owner{-1};
    std::atomic<int> target_last_kind{-1};
    const auto dispatch = [&] (
      detail::mesh_node_runtime_t &node,
      bool reply_replayed_request) {
        while (!stop_dispatch.load (
          std::memory_order_acquire)) {
            (void) node.dispatch_ready (
              [&node, &replay_dispatches, &target_callbacks,
               &target_last_owner, &target_last_kind,
               reply_replayed_request] (
                const auto &owner,
                const auto &record,
                auto) {
                  if (reply_replayed_request) {
                      ++target_callbacks;
                      target_last_owner.store (
                        static_cast<int> (owner.owner_kind));
                      target_last_kind.store (
                        static_cast<int> (record.kind));
                  }
                  if (reply_replayed_request
                      && owner.owner_kind
                           == zlink::framework::runtime::host::
                             owner_kind_t::actor
                      && record.kind
                           == zlink::framework::runtime::host::
                             record_kind_t::actor_request)
                  {
                      ++replay_dispatches;
                      (void) node.native_node ().reply (
                        record.reply_token,
                        {zlink::message_t::from (
                          "relocation-reply")});
                  }
              });
            std::this_thread::sleep_for (1ms);
        }
      };
    relocation_result_t result;
    std::thread relocation_thread ([&] {
        result = source.relocate_application_actor (
          actor, target_descriptor, snapshot,
          {"remote-capacity-reservation"});
    });
    std::this_thread::sleep_for (10ms);
    std::thread source_dispatch (
      [&] { dispatch (source, false); });
    std::thread target_dispatch (
      [&] { dispatch (target, true); });
    relocation_thread.join ();
    const auto replay_completion =
      target.wait_for_completion (replay_operation, 5s);
    stop_dispatch.store (true, std::memory_order_release);
    source_dispatch.join ();
    target_dispatch.join ();

    object_ref_t expected_target{
      object_kind_t::actor,
      std::string (actor.actor_id ().value ()),
      actor.object_generation (),
      2,
      "production-relocation-mesh",
      target.status ().routing_id ().to_string ()};
    const auto restored_target =
      target.native_node ().objects ().find (
        object_kind_t::actor, std::string (actor.actor_id ().value ()));
    if (result.terminal != relocation_terminal_t::completed
        || !result.authority
        || result.authority->target != expected_target
        || restored_target
             != std::optional<object_ref_t>{expected_target}
        || roots->roots.size () != 1
        || !replay_completion
        || (replay_completion
            && replay_completion.value ().record.terminal_result != 0))
        std::cerr
          << "V11-M6C-CPP remote relocation diagnostic: terminal="
          << static_cast<int> (result.terminal)
          << " reason=" << static_cast<int> (result.reason)
          << " authority=" << result.authority.has_value ()
          << " restored=" << restored_target.has_value ()
          << " roots=" << roots->roots.size ()
          << " replay=" << static_cast<bool> (replay_completion)
          << " replay-terminal="
          << (replay_completion
                ? replay_completion.value ().record.terminal_result
                : -1)
          << " replay-dispatches=" << replay_dispatches.load ()
          << " callbacks=" << target_callbacks.load ()
          << " last-owner=" << target_last_owner.load ()
          << " last-kind=" << target_last_kind.load ()
          << " target-pending="
          << target.native_node ().objects ().pending (
               expected_target, turn_domain_t::application)
          << '\n';
    test.require (
      result.terminal == relocation_terminal_t::completed
        && result.authority
        && result.authority->target == expected_target
        && restored_target
             == std::optional<object_ref_t>{expected_target}
        && roots->roots.size () == 1
        && replay_completion
        && replay_completion.value ().record.terminal_result == 0,
      "production relocation must negotiate target Restore, publish authority, and activate the target");
    source.stop ();
    target.stop ();
}

void test_application_user_spot_aggregate_remote_production_path (
  test_context_t &test)
{
    using namespace std::chrono_literals;
    namespace detail = zlink::framework::detail;
    namespace framework = zlink::framework;

    const auto make_state = [] (const std::string &rid) {
        auto state =
          std::make_shared<detail::mesh_node_builder_state_t> (
            "production-aggregate-mesh");
        state->listen_endpoint = "tcp://127.0.0.1:0";
        state->routing_id = zlink::routing_id_t::from (rid);
        state->spot_state->snapshot.actor_types.push_back (
          "production.aggregate.actor");
        return state;
    };
    auto roots = std::make_shared<memory_relocation_repository_t> ();
    auto authority = std::make_shared<memory_authority_store_t> ();
    auto aggregates =
      std::make_shared<memory_aggregate_authority_t> (authority);
    detail::mesh_node_runtime_t source (
      make_state ("production-aggregate-source"));
    detail::mesh_node_runtime_t target (
      make_state ("production-aggregate-target"));
    source.configure_relocation_runtime (
      authority, roots, aggregates);
    target.configure_relocation_runtime (
      authority, roots, aggregates);
    source.configure_session_route_owner (
      [] {
          return std::optional<framework::location_owner_token_t>{
            {"source-owner", 1}};
      });
    target.configure_session_route_owner (
      [] {
          return std::optional<framework::location_owner_token_t>{
            {"target-owner", 9}};
      });
    source.start ();
    target.start ();
    source.connect_peer (
      target.status ().routing_id (),
      target.status ().local_endpoint ());
    const auto admission_deadline =
      std::chrono::steady_clock::now () + 5s;
    while ((!source.has_admitted_peer (
               target.status ().routing_id (),
               target.status ().lifecycle_generation ())
            || !target.has_admitted_peer (
              source.status ().routing_id (),
              source.status ().lifecycle_generation ()))
           && std::chrono::steady_clock::now ()
                < admission_deadline) {
        (void) source.dispatch_ready (
          [] (const auto &, const auto &, auto) {});
        (void) target.dispatch_ready (
          [] (const auto &, const auto &, auto) {});
        std::this_thread::sleep_for (1ms);
    }
    const auto source_admitted = source.has_admitted_peer (
      target.status ().routing_id (),
      target.status ().lifecycle_generation ());
    const auto target_admitted = target.has_admitted_peer (
      source.status ().routing_id (),
      source.status ().lifecycle_generation ());
    test.require (
      source_admitted && target_admitted,
      "production aggregate source and target must both admit the peer");
    if (!source_admitted || !target_admitted) {
        source.stop ();
        target.stop ();
        return;
    }

    const auto created_actor = source.create_application_actor (
      "production.aggregate.actor",
      "production-aggregate-actor", std::nullopt, 1s);
    test.require (
      static_cast<bool> (created_actor),
      "production aggregate Actor must be created");
    if (!created_actor) {
        source.stop ();
        target.stop ();
        return;
    }
    const auto actor_handle = created_actor.value ();
    auto &source_objects = source.native_node ().objects ();
    source_objects.replace_placement_candidates (
      {{.mesh_name = "production-aggregate-mesh",
        .node_id = source.status ().routing_id ().to_string (),
        .stable_types =
          {"production.aggregate.actor",
           "production.aggregate.spot"},
        .weight = 100,
        .active_capacity = 100,
        .active_count = 1,
        .pending_capacity = 100,
        .pending_count = 0}});
    auto created_spot = source_objects.begin_create (
      {.kind = object_kind_t::user_spot,
       .key = "production-aggregate-spot",
       .stable_type = "production.aggregate.spot",
       .mesh_name =
         std::optional<std::string>{
           "production-aggregate-mesh"},
       .creation_request = {},
       .exclusive = true,
       .instance_intent = false});
    test.require (
      created_spot.status == create_status_t::reserved
        && source_objects.commit_create (
             created_spot.attempt)
             == stateful_error_t::none,
      "production aggregate User Spot must be created");
    const auto actor =
      source.native_node ().resolve_actor (actor_handle);
    const auto spot = source_objects.find (
      object_kind_t::user_spot,
      "production-aggregate-spot");
    if (!actor || !spot) {
        test.require (
          false,
          "production aggregate participants must resolve");
        source.stop ();
        target.stop ();
        return;
    }
    const auto [join_error, join] =
      source_objects.begin_membership_move (*actor, *spot);
    const auto [commit_error, joined_actor] =
      source_objects.commit_membership_move (join);
    test.require (
      join_error == stateful_error_t::none
        && commit_error == stateful_error_t::none,
      "production aggregate Actor must join its User Spot");
    test.require (
      source_objects.register_timer (
        *spot, {101, 1000, 250, 7})
          == stateful_error_t::none
        && source_objects.register_timer (
             joined_actor, {102, 2000, 0, 8})
             == stateful_error_t::none,
      "production aggregate timers must be registered");

    const auto make_authority =
      [&] (const object_ref_t &object,
           framework::placement_object_kind_t kind,
           std::string stable_type,
           std::string store_version) {
          return framework::authority_snapshot_t{
            .store_version = std::move (store_version),
            .payload = {},
            .object_generation = object.object_generation,
            .authority_owner_generation =
              object.authority_owner_generation,
            .owner = {"source-owner", 1},
            .store_now = std::chrono::system_clock::now (),
            .allocation =
              {framework::placement_allocation_state_t::active,
               kind,
               std::move (stable_type),
               {"production-aggregate-mesh",
                framework::node_rid_t::from_string (
                  source.status ().routing_id ().to_string ()),
                source.status ().lifecycle_generation (),
                {"source-owner", 1}},
               {1, 0, std::nullopt}}};
      };
    framework::mesh_node_descriptor_t target_descriptor;
    target_descriptor.mesh_name = "production-aggregate-mesh";
    target_descriptor.rid = target.status ().routing_id ();
    target_descriptor.lifecycle_generation =
      target.status ().lifecycle_generation ();
    target_descriptor.application_version = 1;
    target_descriptor.owner_id = "target-owner";
    target_descriptor.lease_generation = 9;

    aggregate_relocation_result_t result;
    std::atomic<bool> stop_dispatch{false};
    const auto dispatch = [&] (detail::mesh_node_runtime_t &node) {
        while (!stop_dispatch.load (
          std::memory_order_acquire)) {
            (void) node.dispatch_ready (
              [] (const auto &, const auto &, auto) {});
            std::this_thread::sleep_for (1ms);
        }
    };
    std::thread relocation_thread ([&] {
        result = source.relocate_application_unit (
          {*spot, joined_actor},
          {"production.aggregate.spot",
           "production.aggregate.actor"},
          target_descriptor,
          {make_authority (
             *spot,
             framework::placement_object_kind_t::user_spot,
             "production.aggregate.spot",
             "aggregate-spot-v1"),
           make_authority (
             joined_actor,
             framework::placement_object_kind_t::actor,
             "production.aggregate.actor",
             "aggregate-actor-v1")},
          {{"aggregate-capacity-spot"},
           {"aggregate-capacity-actor"}});
    });
    std::thread source_dispatch (
      [&] { dispatch (source); });
    std::thread target_dispatch (
      [&] { dispatch (target); });
    relocation_thread.join ();
    stop_dispatch.store (true, std::memory_order_release);
    source_dispatch.join ();
    target_dispatch.join ();

    object_ref_t expected_spot = *spot;
    expected_spot.node_id =
      target.status ().routing_id ().to_string ();
    ++expected_spot.authority_owner_generation;
    object_ref_t expected_actor = joined_actor;
    expected_actor.node_id =
      target.status ().routing_id ().to_string ();
    ++expected_actor.authority_owner_generation;
    const auto restored_spot =
      target.native_node ().objects ().find (
        object_kind_t::user_spot,
        expected_spot.key);
    const auto restored_actor =
      target.native_node ().objects ().find (
        object_kind_t::actor,
        expected_actor.key);
    if (result.terminal != relocation_terminal_t::completed
        || result.authority.size () != 2
        || aggregates->prepare_count != 1
        || aggregates->commit_count != 1
        || restored_spot
             != std::optional<object_ref_t>{expected_spot}
        || restored_actor
             != std::optional<object_ref_t>{expected_actor}
        || target.native_node ().objects ().actor_membership (
             expected_actor)
             != std::optional<std::string>{
               expected_spot.key}
        || target.native_node ().objects ().timers (
             expected_spot).size () != 1
        || target.native_node ().objects ().timers (
             expected_actor).size () != 1)
        std::cerr
          << "V11-M6C-CPP aggregate diagnostic: terminal="
          << static_cast<int> (result.terminal)
          << " reason=" << static_cast<int> (result.reason)
          << " authority=" << result.authority.size ()
          << " prepare=" << aggregates->prepare_count
          << " commit=" << aggregates->commit_count
          << " spot=" << restored_spot.has_value ()
          << " actor=" << restored_actor.has_value ()
          << " membership="
          << target.native_node ().objects ().actor_membership (
               expected_actor)
               .value_or ("<none>")
          << " spot-timers="
          << target.native_node ().objects ().timers (
               expected_spot).size ()
          << " actor-timers="
          << target.native_node ().objects ().timers (
               expected_actor).size ()
          << '\n';
    test.require (
      result.terminal == relocation_terminal_t::completed
        && result.authority.size () == 2
        && aggregates->prepare_count == 1
        && aggregates->commit_count == 1
        && restored_spot
             == std::optional<object_ref_t>{expected_spot}
        && restored_actor
             == std::optional<object_ref_t>{expected_actor}
        && target.native_node ().objects ().actor_membership (
             expected_actor)
             == std::optional<std::string>{
               expected_spot.key}
        && target.native_node ().objects ().timers (
             expected_spot).size () == 1
        && target.native_node ().objects ().timers (
             expected_actor).size () == 1,
      "production aggregate relocation must commit and restore the User Spot, member Actor, membership, and timers");
    source.stop ();
    target.stop ();
}

void test_stateful_application_reservation_includes_active_work (
  test_context_t &test)
{
    namespace limits = zlink::framework::runtime::dispatch_limits;
    const auto fixed = limits::fixed_work_byte_cost;
    stateful_object_runtime_t count_limited (
      1, 1, 1024 * 1024, limits::control_mailbox_bytes);
    const auto count_actor =
      create_actor (count_limited, "active-count-actor");
    test.require (
      count_limited.enqueue (
        count_actor, turn_domain_t::application, {1, {1}})
        == stateful_error_t::none,
      "active reservation test must enqueue its first application turn");
    const auto [count_claim_error, count_claim] =
      count_limited.try_claim (
        count_actor, turn_domain_t::application);
    test.require (
      count_claim_error == stateful_error_t::none && count_claim,
      "active reservation test must claim the first application turn");
    test.require (
      count_limited.enqueue (
        count_actor, turn_domain_t::application, {2, {2}})
        == stateful_error_t::backpressured,
      "application count budget must include the active turn");
    if (count_claim) {
        test.require (
          count_limited.complete_claim (
            count_actor, turn_domain_t::application)
            == stateful_error_t::none,
          "active count reservation must release at handler completion");
    }
    test.require (
      count_limited.enqueue (
        count_actor, turn_domain_t::application, {2, {2}})
        == stateful_error_t::none,
      "application count budget must admit work after terminal completion");

    stateful_object_runtime_t byte_limited (
      2, 1, fixed + 4, limits::control_mailbox_bytes);
    const auto byte_actor = create_actor (
      byte_limited, "active-byte-actor");
    test.require (
      byte_limited.enqueue (
        byte_actor, turn_domain_t::application,
        {1, std::vector<std::uint8_t> (4, 0x41)})
        == stateful_error_t::none,
      "active byte reservation test must enqueue its first turn");
    const auto [byte_claim_error, byte_claim] =
      byte_limited.try_claim (
        byte_actor, turn_domain_t::application);
    test.require (
      byte_claim_error == stateful_error_t::none && byte_claim,
      "active byte reservation test must claim the first turn");
    test.require (
      byte_limited.enqueue (
        byte_actor, turn_domain_t::application, {2, {}})
        == stateful_error_t::backpressured,
      "application byte budget must include the active turn");
    if (byte_claim) {
        test.require (
          byte_limited.complete_claim (
            byte_actor, turn_domain_t::application)
            == stateful_error_t::none,
          "active byte reservation must release at handler completion");
    }
    test.require (
      byte_limited.enqueue (
        byte_actor, turn_domain_t::application, {2, {}})
        == stateful_error_t::none,
      "application byte budget must admit work after terminal completion");

    stateful_object_runtime_t restore_limited (
      2, 1, fixed + 4, limits::control_mailbox_bytes);
    const auto restore_source =
      create_actor (restore_limited, "restore-byte-actor");
    auto restore_target = restore_source;
    restore_target.node_id = "node-b";
    ++restore_target.authority_owner_generation;
    frozen_object_state_t frozen{
      .owner = restore_source,
      .stable_type = "actor",
      .application_state = {},
      .pending_application = {
        {1, std::vector<std::uint8_t> (4, 0x42)},
        {2, {}}},
      .timers = {}};
    test.require (
      restore_limited.restore_relocation (
        std::move (frozen), restore_target,
        {"restore-byte-root", 1, digest_with (0x61)})
        == stateful_error_t::backpressured,
      "relocation restore must enforce the configured application byte budget");
}

void test_aggregate_seal_failure_preserves_earlier_application_work (
  test_context_t &test)
{
    stateful_object_runtime_t objects;
    const auto first = create_spot (
      objects, object_kind_t::user_spot, "seal-failure-first");
    const auto second = create_spot (
      objects, object_kind_t::user_spot, "seal-failure-second");
    test.require (
      objects.enqueue (first, turn_domain_t::application, {1, {1, 2}})
          == stateful_error_t::none
        && objects.enqueue (first, turn_domain_t::application, {2, {3}})
             == stateful_error_t::none
        && objects.enqueue (second, turn_domain_t::application, {1, {4}})
             == stateful_error_t::none,
      "aggregate seal failure setup must retain application records");
    const auto first_bytes = objects.pending_bytes (
      first, turn_domain_t::application);
    const auto second_bytes = objects.pending_bytes (
      second, turn_domain_t::application);
    objects.configure_relocation_state (
      [&] (const object_ref_t &owner, const std::string &, std::stop_token) {
          if (owner == second)
              throw std::runtime_error ("capture failed for second participant");
          return std::vector<std::uint8_t>{0x41};
      },
      [] (const frozen_object_state_t &, const object_ref_t &, std::stop_token) {
          return true;
      });

    const auto [error, seal] = objects.try_seal_relocation_aggregate (
      {first, second});
    test.require (
      error == stateful_error_t::conflict && seal.participants.empty (),
      "aggregate capture failure must return conflict without a seal");
    test.require (
      objects.pending (first, turn_domain_t::application) == 2
        && objects.pending_bytes (first, turn_domain_t::application)
             == first_bytes,
      "aggregate capture failure must preserve the earlier participant queue");
    test.require (
      objects.pending (second, turn_domain_t::application) == 1
        && objects.pending_bytes (second, turn_domain_t::application)
             == second_bytes,
      "aggregate capture failure must preserve the failing participant queue");
}

void test_relocation_hold_restores_and_enforces_limits (
  test_context_t &test)
{
    namespace limits = zlink::framework::runtime::dispatch_limits;
    stateful_object_runtime_t failed_capture (
      2048, 8, 64u * 1024u * 1024u, limits::control_mailbox_bytes);
    const auto failed_spot = create_spot (
      failed_capture, object_kind_t::user_spot, "held-capture-failure");
    test.require (
      failed_capture.enqueue (
        failed_spot, turn_domain_t::application, {1, {1}})
          == stateful_error_t::none
        && failed_capture.enqueue (
             failed_spot, turn_domain_t::application, {2, {2}})
             == stateful_error_t::none,
      "held capture failure setup must retain the initial FIFO records");
    const auto [active_error, active] =
      failed_capture.try_claim (
        failed_spot, turn_domain_t::application);
    test.require (
      active_error == stateful_error_t::none && active
        && active->sequence == 1,
      "held capture failure setup must keep the first record active");
    failed_capture.configure_relocation_state (
      [] (const object_ref_t &, const std::string &, std::stop_token)
          -> std::vector<std::uint8_t> {
          throw std::runtime_error ("held capture failure");
      },
      [] (const frozen_object_state_t &, const object_ref_t &, std::stop_token) {
          return true;
      });

    std::atomic<bool> capture_done = false;
    stateful_error_t capture_error = stateful_error_t::none;
    aggregate_relocation_seal_t capture_seal;
    std::thread capture ([&] {
        auto result = failed_capture.try_seal_relocation_aggregate (
          {failed_spot});
        capture_error = result.first;
        capture_seal = std::move (result.second);
        capture_done.store (true, std::memory_order_release);
    });
    const bool moving = wait_until_bounded (
      [&] {
          return failed_capture.register_timer (
                   failed_spot, {91, 1000, 1000, 1})
                 == stateful_error_t::moving;
      },
      std::chrono::seconds (5));
    test.require (
      moving && !capture_done.load (std::memory_order_acquire),
      "capture failure test must enqueue while the source is moving");
    test.require (
      failed_capture.enqueue (
        failed_spot, turn_domain_t::application, {3, {3}})
        == stateful_error_t::none,
      "application ingress must enter the relocation hold before capture");
    if (active) {
        test.require (
          failed_capture.complete_claim (
            failed_spot, turn_domain_t::application)
            == stateful_error_t::none,
          "capture failure test must release the active turn");
    }
    capture.join ();
    test.require (
      capture_error == stateful_error_t::conflict
        && capture_seal.participants.empty (),
      "capture failure must return conflict without a relocation seal");
    const auto [first_error, first] =
      failed_capture.try_claim (
        failed_spot, turn_domain_t::application);
    test.require (
      first_error == stateful_error_t::none && first
        && first->sequence == 2,
      "capture failure must merge held work behind the captured queue");
    (void) failed_capture.complete_claim (
      failed_spot, turn_domain_t::application);
    const auto [second_error, second] =
      failed_capture.try_claim (
        failed_spot, turn_domain_t::application);
    test.require (
      second_error == stateful_error_t::none && second
        && second->sequence == 3,
      "capture failure must not strand or reorder held work");
    (void) failed_capture.complete_claim (
      failed_spot, turn_domain_t::application);

    stateful_object_runtime_t count_limited (
      2048, 8, 64u * 1024u * 1024u, limits::control_mailbox_bytes);
    const auto count_first = create_spot (
      count_limited, object_kind_t::user_spot, "held-count-first");
    const auto count_second = create_spot (
      count_limited, object_kind_t::user_spot, "held-count-second");
    test.require (
      count_limited.enqueue (
        count_first, turn_domain_t::application, {0, {}})
        == stateful_error_t::none,
      "aggregate hold count setup must reserve an active turn");
    const auto [count_claim_error, count_claim] =
      count_limited.try_claim (
        count_first, turn_domain_t::application);
    test.require (
      count_claim_error == stateful_error_t::none && count_claim,
      "aggregate hold count test must activate the source lane");
    std::atomic<bool> count_done = false;
    stateful_error_t count_error = stateful_error_t::conflict;
    aggregate_relocation_seal_t count_seal;
    std::thread count_sealing ([&] {
        auto result = count_limited.try_seal_relocation_aggregate (
          {count_first, count_second});
        count_error = result.first;
        count_seal = std::move (result.second);
        count_done.store (true, std::memory_order_release);
    });
    const bool count_moving = wait_until_bounded (
      [&] {
          return count_limited.register_timer (
                   count_first, {92, 1000, 1000, 1})
                 == stateful_error_t::moving;
      },
      std::chrono::seconds (5));
    test.require (
      count_moving && !count_done.load (std::memory_order_acquire),
      "aggregate hold count test must observe the moving barrier");
    const auto membership_actor = create_actor (
      count_limited, "membership-only-hold-actor");
    const object_ref_t membership_target{
      object_kind_t::user_spot,
      "membership-only-target",
      1,
      1,
      "mesh",
      "node-b"};
    const auto [membership_error, membership_move] =
      count_limited.begin_remote_membership_move (
        membership_actor, membership_target);
    test.require (
      membership_error == stateful_error_t::none
        && count_limited.enqueue (
             membership_actor, turn_domain_t::application, {1, {7}})
             == stateful_error_t::none,
      "membership-only movement must retain ingress without relocation accounting");
    bool count_records_accepted = true;
    for (std::uint64_t sequence = 1; sequence <= 512; ++sequence) {
        count_records_accepted =
          count_limited.enqueue (
            count_first, turn_domain_t::application, {sequence, {}})
          == stateful_error_t::none
          && count_records_accepted;
        count_records_accepted =
          count_limited.enqueue (
            count_second, turn_domain_t::application,
            {1000 + sequence, {}})
          == stateful_error_t::none
          && count_records_accepted;
    }
    test.require (
      count_records_accepted
        && count_limited.pending (
             count_first, turn_domain_t::application)
             + count_limited.pending (
                 count_second, turn_domain_t::application)
             == 1024,
      "relocation hold must accept up to the aggregate 1,024 record limit");
    test.require (
      count_limited.enqueue (
        count_first, turn_domain_t::application, {2000, {}})
        == stateful_error_t::backpressured,
      "relocation hold must reject the 1,025th aggregate record");
    if (membership_error == stateful_error_t::none) {
        test.require (
          count_limited.abort_membership_move (membership_move)
            == stateful_error_t::none,
          "membership-only movement must release its independent hold");
    }
    if (count_claim) {
        test.require (
          count_limited.complete_claim (
            count_first, turn_domain_t::application)
            == stateful_error_t::none,
          "aggregate hold count test must release its active turn");
    }
    count_sealing.join ();
    test.require (
      count_error == stateful_error_t::none
        && count_limited.abort_relocation (count_seal.token)
             == stateful_error_t::none,
      "aggregate hold count test must close its relocation generation");

    stateful_object_runtime_t byte_limited (
      4096, 8, 64u * 1024u * 1024u, limits::control_mailbox_bytes);
    const auto byte_first = create_spot (
      byte_limited, object_kind_t::user_spot, "held-byte-first");
    const auto byte_second = create_spot (
      byte_limited, object_kind_t::user_spot, "held-byte-second");
    test.require (
      byte_limited.enqueue (
        byte_first, turn_domain_t::application, {0, {}})
        == stateful_error_t::none,
      "aggregate hold byte setup must reserve an active turn");
    const auto [byte_claim_error, byte_claim] =
      byte_limited.try_claim (
        byte_first, turn_domain_t::application);
    test.require (
      byte_claim_error == stateful_error_t::none && byte_claim,
      "aggregate hold byte test must activate the source lane");
    std::atomic<bool> byte_done = false;
    stateful_error_t byte_error = stateful_error_t::conflict;
    aggregate_relocation_seal_t byte_seal;
    std::thread byte_sealing ([&] {
        auto result = byte_limited.try_seal_relocation_aggregate (
          {byte_first, byte_second});
        byte_error = result.first;
        byte_seal = std::move (result.second);
        byte_done.store (true, std::memory_order_release);
    });
    const bool byte_moving = wait_until_bounded (
      [&] {
          return byte_limited.register_timer (
                   byte_first, {93, 1000, 1000, 1})
                 == stateful_error_t::moving;
      },
      std::chrono::seconds (5));
    test.require (
      byte_moving && !byte_done.load (std::memory_order_acquire),
      "aggregate hold byte test must observe the moving barrier");
    const auto held_payload_bytes =
      8u * 1024u * 1024u - limits::fixed_work_byte_cost;
    test.require (
      byte_limited.enqueue (
        byte_first,
        turn_domain_t::application,
        {1, std::vector<std::uint8_t> (held_payload_bytes, 0x41)})
        == stateful_error_t::none
        && byte_limited.enqueue (
             byte_second,
             turn_domain_t::application,
             {1, std::vector<std::uint8_t> (held_payload_bytes, 0x42)})
             == stateful_error_t::none,
      "relocation hold must accept bytes up to the aggregate 16 MiB limit");
    test.require (
      byte_limited.enqueue (
        byte_first, turn_domain_t::application, {2, {}})
        == stateful_error_t::backpressured,
      "relocation hold must reject bytes beyond the aggregate 16 MiB limit");
    if (byte_claim) {
        test.require (
          byte_limited.complete_claim (
            byte_first, turn_domain_t::application)
            == stateful_error_t::none,
          "aggregate hold byte test must release its active turn");
    }
    byte_sealing.join ();
    test.require (
      byte_error == stateful_error_t::none
        && byte_limited.abort_relocation (byte_seal.token)
             == stateful_error_t::none,
      "aggregate hold byte test must close its relocation generation");
}

int main ()
{
    test_context_t test;
    test_generation_barrier_quiesces_yield_spot_and_timer (test);
    test_relocation_ready_completion_runs_once_on_spot_turn (test);
    test_actor_leave_after_relocation_defer_runs_lifecycle_callbacks (test);
    test_temporary_channel_request_yield_owns_call_state (test);
    test_close_barrier_waits_and_abort_restores_ingress (test);
    test_envelope_round_trip (test);
    test_spot_restore_stages_before_publication (test);
    test_concurrent_spot_restore_owns_one_reservation (test);
    test_restore_validates_generation_before_spot_publication (test);
    test_pending_restore_holds_ingress_before_rollback (test);
    test_aggregate_envelope_and_crash_recovery (test);
    test_publication_and_handoff (test);
    test_conflict_aborts_without_losing_ingress (test);
    test_recovery_and_data_loss (test);
    test_restart_reconstructs_relocation_replay (test);
    test_permit_precedes_seal (test);
    test_host_preflight_is_all_or_none (test);
    test_user_spot_aggregate_and_stream_barrier (test);
    test_shutdown_wins_during_retire_preflight (test);
    test_post_commit_failure_is_force_stopped (test);
    test_public_relocation_store_adapter (test);
    test_public_authority_store_adapter (test);
    test_durable_join_completion_replacement_and_ordering (test);
    test_production_relocation_restore_and_replay_vertical (test);
    test_target_replay_limits_are_relocation_scoped (test);
    test_application_relocation_uses_maintenance_and_fails_closed (test);
    test_application_relocation_remote_production_path (test);
    test_application_user_spot_aggregate_remote_production_path (
      test);
    test_aggregate_seal_failure_preserves_earlier_application_work (test);
    test_relocation_hold_restores_and_enforces_limits (test);
    test_stateful_application_reservation_includes_active_work (test);
    return test.failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
