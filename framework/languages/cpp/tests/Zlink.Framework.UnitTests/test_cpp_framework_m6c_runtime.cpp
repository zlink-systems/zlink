/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/stateful/maintenance_runtime.hpp"
#include <runtime/locations/location_repository.hpp>
#include "runtime/stateful/public_store_adapters.hpp"
#include "runtime/stateful/raw_stateful_dispatch.hpp"
#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/locations/actor_authority_payload.hpp"
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include "runtime/spots/spot_route_packets.hpp"

#include <nlohmann/json.hpp>

#include <zlink/framework.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{

template<class T>
T await_task (zlink::framework::task_t<T> task)
{
    return std::move (task).result ().value ();
}

void await_task (zlink::framework::task_t<void> task)
{
    std::move (task).result ().value ();
}

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

std::vector<std::uint8_t> decode_hex_vector (std::string_view value)
{
    const auto nibble = [] (char ch) -> std::uint8_t {
        if (ch >= '0' && ch <= '9') return static_cast<std::uint8_t> (ch - '0');
        if (ch >= 'a' && ch <= 'f') return static_cast<std::uint8_t> (ch - 'a' + 10);
        return static_cast<std::uint8_t> (ch - 'A' + 10);
    };
    std::vector<std::uint8_t> result;
    result.reserve (value.size () / 2);
    for (std::size_t index = 0; index != value.size (); index += 2)
        result.push_back (static_cast<std::uint8_t> (
          (nibble (value[index]) << 4u) | nibble (value[index + 1])));
    return result;
}

void test_spot_lifecycle_domain_rejects_invalid_kind_combinations (
  test_context_t &test)
{
    namespace detail = zlink::framework::detail;

    static_assert (!std::default_initializable<
                   detail::spot_lifecycle_domain_t>);

    const auto entry = detail::spot_lifecycle_domain_t::entry ();
    const auto user = detail::spot_lifecycle_domain_t::user ();
    const auto instance = detail::spot_lifecycle_domain_t::instance ();
    test.require (
      entry.is_entry () && !entry.allows_relocation ()
        && !entry.allows_idle_eviction (),
      "Entry Spot domain must exclude relocation and idle eviction");
    test.require (
      !user.is_entry () && user.allows_relocation ()
        && !user.allows_idle_eviction (),
      "User Spot domain must allow relocation but exclude idle eviction");
    test.require (
      !instance.is_entry () && instance.allows_relocation ()
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
        runtime::serial_lane_policy_t::spot_wide ());
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
        state->lifecycle_domain =
          entry_spot ? detail::spot_lifecycle_domain_t::entry ()
                     : detail::spot_lifecycle_domain_t::user ();
        state->execution_mode = user_spot_execution_mode_t::spot_wide;
        state->relocation_readiness =
          spot_relocation_readiness_mode_t::application_signaled;
        state->serial_executor =
          std::make_shared<runtime::offload_executor_t> (
            2, 64, "actor-leave-after-defer");
        state->serial_queue =
          std::make_shared<runtime::serial_execution_queue_t> (
            *state->serial_executor, 64,
            runtime::serial_execution_queue_t::error_handler_t{},
            runtime::serial_lane_policy_t::spot_wide ());
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
      runtime::serial_execution_queue_t::error_handler_t{},
      runtime::serial_lane_policy_t::spot_wide ());

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

struct accepted_decode_payload_t
{
    int value = 0;
};

struct rejected_decode_payload_t
{
    int value = 0;
};

struct failed_decode_payload_t
{
    int value = 0;
};

int successful_payload_deserializations = 0;
int other_payload_deserializations = 0;
int failed_payload_deserializations = 0;

void to_json (
  nlohmann::json &json,
  const accepted_decode_payload_t &payload)
{
    json = nlohmann::json{{"value", payload.value}};
}

void to_json (
  nlohmann::json &json,
  const rejected_decode_payload_t &payload)
{
    json = nlohmann::json{{"value", payload.value}};
}

void to_json (
  nlohmann::json &json,
  const failed_decode_payload_t &payload)
{
    json = nlohmann::json{{"value", payload.value}};
}

void from_json (
  const nlohmann::json &json,
  accepted_decode_payload_t &payload)
{
    ++successful_payload_deserializations;
    payload.value = json.at ("value").get<int> ();
}

void from_json (
  const nlohmann::json &json,
  rejected_decode_payload_t &payload)
{
    ++other_payload_deserializations;
    payload.value = json.at ("value").get<int> ();
}

void from_json (
  const nlohmann::json &,
  failed_decode_payload_t &)
{
    ++failed_payload_deserializations;
    throw std::runtime_error ("expected accepted-payload decode failure");
}

class payload_decode_spot_t final
    : public zlink::framework::spot_t<zlink::framework::actor_t>
{
  public:
    explicit payload_decode_spot_t (
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

    void configure () override {}

    zlink::framework::task_t<zlink::framework::spot_create_response_t>
    on_create (const zlink::framework::message_t &request) override
    {
        if (!failure_mode) {
            const auto first = request.decode<accepted_decode_payload_t> ();
            const auto copied_request = request;
            const auto second =
              copied_request.decode<accepted_decode_payload_t> ();
            bool other_rejected = false;
            try {
                (void) request.decode<rejected_decode_payload_t> ();
            }
            catch (const zlink::framework::framework_exception_t &error) {
                other_rejected =
                  error.kind ()
                  == zlink::framework::framework_error_kind_t::protocol_error;
            }
            success_observed = first.value == 73 && second.value == 73
                               && other_rejected;
        }
        else {
            std::string first_error;
            std::string repeated_error;
            try {
                (void) request.decode<failed_decode_payload_t> ();
            }
            catch (const zlink::framework::framework_exception_t &error) {
                first_error = error.what ();
            }
            try {
                (void) request.decode<rejected_decode_payload_t> ();
            }
            catch (const zlink::framework::framework_exception_t &error) {
                repeated_error = error.what ();
            }
            failure_observed = !first_error.empty ()
                               && repeated_error == first_error;
        }
        co_return zlink::framework::spot_create_response_t::accept ();
    }

    zlink::framework::task_t<void> on_initialize () override
    {
        co_return;
    }

    zlink::framework::task_t<zlink::framework::spot_actor_join_result_t>
    on_actor_join (
      std::string_view,
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

    static inline bool failure_mode = false;
    static inline bool success_observed = false;
    static inline bool failure_observed = false;

  private:
    zlink::framework::spot_context_t _context;
};

void test_accepted_message_payload_is_deserialized_once (
  test_context_t &test)
{
    payload_decode_spot_t::failure_mode = false;
    payload_decode_spot_t::success_observed = false;
    payload_decode_spot_t::failure_observed = false;
    successful_payload_deserializations = 0;
    other_payload_deserializations = 0;
    failed_payload_deserializations = 0;

    zlink::framework::zlink_builder_t builder;
    auto mesh = builder.add_route_mesh ("decode-cache-mesh");
    mesh.add_spot_factory<payload_decode_spot_t> (
      "decode-cache",
      [] (zlink::framework::spot_context_t context) {
          return std::make_shared<payload_decode_spot_t> (
            std::move (context));
      },
      [] (auto &factory) { factory.disable_relocation (); });
    zlink::framework::serializer_registry_t serializers;
    zlink::framework::detail::channel_runtime_t::from (
      builder.message_bus ())
      .bind_serializers (serializers);
    auto found_runtime =
      zlink::framework::detail::spot_node_runtime_t::from (
        builder, "decode-cache-mesh");
    test.require (
      found_runtime.has_value (),
      "accepted-payload decode test must resolve its Spot runtime");
    if (!found_runtime)
        return;
    auto runtime = *found_runtime;

    const auto success = runtime.get_or_create_spot (
      "decode-cache",
      zlink::framework::spot_id_t ("decode-success"),
      zlink::message_t::from (R"({"value":73})"));
    test.require (
      success.state == zlink::framework::spot_create_state_t::created
        && payload_decode_spot_t::success_observed
        && successful_payload_deserializations == 1
        && other_payload_deserializations == 0,
      "one admitted payload must preserve its first successful decode across repeated and differently typed reads");

    payload_decode_spot_t::failure_mode = true;
    const auto failure = runtime.get_or_create_spot (
      "decode-cache",
      zlink::framework::spot_id_t ("decode-failure"),
      zlink::message_t::from (R"({"value":91})"));
    test.require (
      failure.state == zlink::framework::spot_create_state_t::created
        && payload_decode_spot_t::failure_observed
        && failed_payload_deserializations == 1
        && other_payload_deserializations == 0,
      "one admitted payload must preserve its first decode failure without invoking another serializer");

    runtime.request_stop ();
    runtime.cancel_pending_dispatch ();
    runtime.cancel_pending_work ();
    runtime.release_native_handles ();
}

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
          std::get_if<zlink::framework::authority_put_t> (&mutation);
        const auto *retarget =
          std::get_if<zlink::framework::authority_retarget_t> (
            &mutation);
        if (!snapshot || (!put && !retarget)
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
        if (remaining_auxiliary_conflicts != 0) {
            --remaining_auxiliary_conflicts;
            snapshot->store_version =
              std::to_string (
                std::stoull (snapshot->store_version) + 1);
            snapshot->store_now = std::chrono::system_clock::now ();
            return completed (
              zlink::framework::authority_compare_exchange_result_t{
                zlink::framework::authority_conflict_t{
                  zlink::framework::authority_read_result_t{*snapshot}}});
        }
        if (retarget) {
            observed_target_owner = retarget->target.owner;
            observed_target_placement = retarget->target;
            ++snapshot->authority_owner_generation;
            snapshot->owner = retarget->target.owner;
            snapshot->payload = retarget->payload;
        } else {
            snapshot->payload = put->payload;
        }
        snapshot->store_version =
          std::to_string (
            std::stoull (snapshot->store_version) + 1);
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
    std::optional<zlink::framework::object_creation_target_t>
      observed_target_placement;
    std::vector<std::string> observed_keys;
    int remaining_auxiliary_conflicts = 0;

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
      const object_ref_t &target,
      zlink::framework::location_owner_token_t target_owner,
      zlink::framework::object_creation_target_t,
      std::string relocation_reference,
      std::uint32_t checksum_crc32c,
      inventory_digest_t inventory_digest,
      std::vector<std::byte> target_application_payload = {}) override
    {
        std::lock_guard lock (mutex);
        log.push_back ("publish");
        ++publish_count;
        if (force_conflict
            || (conflict_on_publish != 0
                && publish_count == conflict_on_publish))
            return {authority_publish_status_t::conflict, read_locked (
                      source.kind, source.key)};
        authority_relocation_reference_t reference{
          .source = source,
          .target = target,
          .relocation_reference = std::move (relocation_reference),
          .checksum_crc32c = checksum_crc32c,
          .inventory_digest = inventory_digest,
          .target_owner = std::move (target_owner),
          .application_payload = std::move (target_application_payload)};
        rows[authority_key (source.kind, source.key)] = reference;
        if (throw_after_publish)
            throw std::runtime_error ("response lost after authority commit");
        return {authority_publish_status_t::published, reference};
    }

    std::optional<authority_relocation_reference_t>
    read (object_kind_t kind, const std::string &key) override
    {
        std::lock_guard lock (mutex);
        if (throw_on_read)
            throw std::runtime_error ("authority store unavailable");
        return read_locked (kind, key);
    }

    std::optional<std::vector<relocation_participant_identity_t>>
    list_participant_identities () override
    {
        std::lock_guard lock (mutex);
        if (participant_identities.empty ())
            return std::nullopt;
        return participant_identities;
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
    /* Store-derived participant inventory (28 §4.2): what a production
     * Location Store serves from its authority rows. */
    std::vector<relocation_participant_identity_t> participant_identities;
    std::vector<std::string> log;
    bool force_conflict = false;
    bool throw_after_publish = false;
    bool throw_on_read = false;
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
      std::string relocation_reference,
      std::uint32_t checksum_crc32c,
      inventory_digest_t inventory_digest) override
    {
        std::lock_guard lock (mutex);
        ++prepare_count;
        if (sources.size () < 2 || prepared)
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
        auto result = await_task (
          objects.try_seal_relocation_aggregate ({actor, spot}));
        seal_error = result.error;
        seal = std::move (result.seal);
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
      await_task (objects.try_seal_relocation_aggregate ({actor, spot}));
    test.require (
      second_error == stateful_error_t::none
        && objects.abort_relocation (seal.token)
             == stateful_error_t::not_found,
      "stale abort must not reopen a newer generation");
    test.require (
      objects.abort_relocation (second_seal.token)
          == stateful_error_t::none
        && objects.enqueue (
             actor, turn_domain_t::application, {22, {22}})
             == stateful_error_t::none,
      "the current pre-Cutover abort must reopen application ingress");
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
    namespace protocol = zlink::framework::runtime::protocol;
    // One canonical saved-work record, exactly as the seal canonicalizes an
    // accepted Spot request (Stage A retains the typed admission capture).
    protocol::frozen_application_record_t admitted;
    admitted.kind = protocol::frozen_record_kind_t::spot_request;
    admitted.source_kind = protocol::frozen_source_kind_t::node;
    admitted.source = {"request-source", 6, {'n'}, 1};
    admitted.operation = {0, 42};
    admitted.operation_kind = 3;
    admitted.reply_route_id = 99;
    admitted.body = protocol::frozen_spot_application_body_t{
      {"spot-a", 3, {'m'}, 2, 9, 0}, 1,
      protocol::application_payload_t{
        "ChatRequest", "application/json",
        {'{', '"', 'i', 'd', '"', ':', '1', '}'}}};
    const auto canonical =
      protocol::encode_frozen_application_record (admitted);

    frozen_object_state_t frozen{
      .owner =
        {.kind = object_kind_t::user_spot,
         .key = "spot-a",
         .object_generation = 3,
         .authority_owner_generation = 9,
         .mesh_name = "mesh",
         .node_id = "node-a"},
      .stable_type = "spot",
      .application_state = {7, 8, 9},
      .pending_application =
        {turn_record_t{
          4, canonical.canonical_bytes,
          protocol::application_payload_hwm_bytes (*canonical.application),
          std::nullopt, canonical}},
      .timers = {{11, 100, 50, 3}}};

    const protocol::relocation_id_t relocation{0, 9};
    const auto encoded =
      maintenance_runtime_t::encode_envelope ({frozen}, relocation);
    test.require (!encoded.empty (),
                  "the schema relocation envelope must encode");
    const auto envelope = maintenance_runtime_t::decode_envelope (encoded);
    test.require (envelope.has_value (),
                  "the schema relocation envelope must decode");
    test.require (
      envelope && envelope->relocation == relocation
        && envelope->object.kind
             == protocol::relocation_object_kind_t::user_spot
        && envelope->object.object_id == "spot-a"
        && envelope->object.object_generation == 3
        && envelope->object.expected_authority_owner_generation == 9
        && envelope->application_states.size () == 1
        && envelope->saved_work.size () == 1
        && envelope->saved_work.front ().order == 4
        && envelope->timer_registrations.size () == 1,
      "the stream must carry the declared schema sections");
    // Re-encoding the decoded model must be byte-exact.
    test.require (
      envelope
        && protocol::encode_relocation_envelope (*envelope) == encoded,
      "the schema relocation envelope must re-encode byte-exactly");

    const auto materialized = maintenance_runtime_t::materialize_envelope (
      *envelope,
      {relocation_participant_identity_t{frozen.owner, frozen.stable_type,
                                         std::nullopt}});
    test.require (materialized.has_value ()
                    && materialized->size () == 1
                    && materialized->front () == frozen,
      "queue and timer state must round-trip through the schema stream");

    auto unordered = frozen;
    unordered.pending_application.push_back (
      unordered.pending_application.front ());
    test.require (
      maintenance_runtime_t::encode_envelope ({unordered}, relocation)
        .empty (),
      "encoder must reject duplicate or unordered queue sequences");

    // A two-participant aggregate: the canonical inventory order is the
    // UTF-8 authority-key byte order, so the Actor precedes the Spot.
    frozen_object_state_t actor{
      .owner =
        {.kind = object_kind_t::actor,
         .key = "actor-b",
         .object_generation = 5,
         .authority_owner_generation = 2,
         .mesh_name = "mesh",
         .node_id = "node-a"},
      .stable_type = "actor",
      .application_state = {1},
      .pending_application = {},
      .timers = {}};
    const auto aggregate_encoded = maintenance_runtime_t::encode_envelope (
      {frozen, actor}, relocation);
    test.require (!aggregate_encoded.empty (),
                  "the aggregate schema envelope must encode");
    const auto aggregate_envelope =
      maintenance_runtime_t::decode_envelope (aggregate_encoded);
    test.require (
      aggregate_envelope
        && aggregate_envelope->application_states.size () == 2
        && aggregate_envelope->object.object_id == "spot-a"
        && aggregate_envelope->saved_work.size () == 1
        && aggregate_envelope->saved_work.front ().participant_id == 2,
      "participant ids must follow the sorted authority-key inventory");
    const auto aggregate_materialized =
      maintenance_runtime_t::materialize_envelope (
        *aggregate_envelope,
        {relocation_participant_identity_t{frozen.owner, frozen.stable_type,
                                           std::nullopt},
         relocation_participant_identity_t{
           actor.owner, actor.stable_type,
           std::pair{std::string ("spot-a"), std::uint64_t{3}}}});
    test.require (
      aggregate_materialized && aggregate_materialized->size () == 2
        && aggregate_materialized->front () == actor
        && aggregate_materialized->back () == frozen,
      "aggregate participants must round-trip in inventory order");

    // Tampering with the stream must be rejected by the decoder.
    auto truncated = encoded;
    truncated.pop_back ();
    test.require (!maintenance_runtime_t::decode_envelope (truncated),
                  "decoder must reject a truncated stream");
    test.require (!maintenance_runtime_t::decode_envelope ({}),
                  "decoder must reject an empty stream");
    test.require (maintenance_runtime_t::crc32c (encoded) != 0,
                  "CRC32C must be computed for the retained payload");
}

void test_actor_join_recovery_round_trip (test_context_t &test)
{
    namespace protocol = zlink::framework::runtime::protocol;
    const std::string handoff_id =
      "00112233445566778899aabbccddeeff";
    const auto relocation = protocol::actor_join_relocation_id (
      handoff_id);
    protocol::actor_join_recovery_t recovery{
      .actor_id = "actor-a",
      .actor_type = "sample.Actor",
      .handoff_id = handoff_id,
      .source_spot_id = "source-spot",
      .source_node_routing_id = {'s', 'r', 'c'},
      .actor_generation = 7,
      .actor_authority_owner_generation = 11,
      .actor_node_generation = 13,
      .expected_owner_lease_generation = 17,
      .relocation = relocation,
      .relocation_content_type =
        std::string (protocol::actor_join_snapshot_content_type),
      .request_content_type = "application/json",
      .request = {'{', '}'},
      .reservation_token = handoff_id,
      .reserved_payload_bytes =
        protocol::actor_join_reserved_payload_bytes (
          2, protocol::actor_join_snapshot_content_type),
      .target_spot_id = "target-spot",
      .target_node_routing_id = {'d', 's', 't'},
      .target_node_generation = 19,
      .target_spot_generation = 23,
      .target_authority_owner_generation = 12,
      .target_spot_authority_owner_generation = 29,
      .coordinator = {"owner-a", 17, {'s', 'r', 'c'}, 13, "store-v1"},
      .operation = {31, 37},
      .reply_content_type = "application/json",
      .reply = {'[', ']'}};
    const auto frozen =
      protocol::encode_actor_join_recovery_saved_work (recovery);
    /* Fixed Node vector from encodeCanonicalActorJoinRecoverySavedWork in
     * actor-join-recovery-codec.ts, using this test's recovery fields.  This
     * is intentionally not a C++ encode/decode round trip: changing JSON
     * property order must fail before persisted ZLJR records diverge. */
    constexpr std::string_view node_zljr_frozen_hex =
      "0101001f06373337323633000000000000000d076f776e65722d61000000000000001100000000000000000000000000000000000000000000000100"
      "000767225f5f7a6c696e6b2e6163746f722e726f757465645f6a6f696e2e7265636f76657279316170706c69636174696f6e2f782d7a6c696e6b2d61"
      "63746f722d726f757465642d6a6f696e2d7265636f766572792d76310000070e5a4c4a5201000006f900000002000000027b2252657175657374223a"
      "7b224163746f724964223a226163746f722d61222c224163746f7254797065223a2273616d706c652e4163746f72222c2248616e646f66664964223a"
      "223030313132323333343435353636373738383939616162626363646465656666222c22426f756e6453657373696f6e4e6f6465526964223a6e756c"
      "6c2c22426f756e6453657373696f6e526964223a6e756c6c2c2252656c6f636174696f6e436f6e74656e7454797065223a226170706c69636174696f"
      "6e2f766e642e7a6c696e6b2e6163746f722d72656c6f636174696f6e2e736e617073686f74222c2252656c6f636174696f6e5265666572656e636522"
      "3a2270656e64696e67222c2252656c6f636174696f6e436865636b73756d437263333263223a302c2252656c6f636174696f6e416767726567617465"
      "4964223a2230303131323233332d343435352d363637372d383839392d616162626363646465656666222c2252656c6f636174696f6e416767726567"
      "61746547656e65726174696f6e223a312c2252656c6f636174696f6e496e76656e746f7279446967657374223a224141414141414141414141414141"
      "41414141414141414141414141414141414141414141414141414141413d222c2252657175657374436f6e74656e7454797065223a226170706c6963"
      "6174696f6e2f6a736f6e222c2252657175657374223a22222c2248616e646f66664672616d6573223a5b5d2c22536f7572636553706f744964223a22"
      "736f757263652d73706f74222c22536f757263654e6f6465526964223a2263334a6a222c224163746f7247656e65726174696f6e223a372c22416374"
      "6f72417574686f726974794f776e657247656e65726174696f6e223a31312c22426f756e6453657373696f6e42696e64696e67546f6b656e223a6e75"
      "6c6c2c22426f756e6453657373696f6e42696e64696e6747656e65726174696f6e223a302c22426f756e6453657373696f6e4f626a65637447656e65"
      "726174696f6e223a302c22426f756e6453657373696f6e417574686f726974794f776e657247656e65726174696f6e223a302c22426f756e64536573"
      "73696f6e4d6573684e616d65223a6e756c6c2c22426f756e6453657373696f6e5461726765744e6f646547656e65726174696f6e223a302c22426f75"
      "6e6453657373696f6e4f776e65724c6561736547656e65726174696f6e223a302c22426f756e6453657373696f6e4f776e65724e6f646547656e6572"
      "6174696f6e223a302c22426f756e6453657373696f6e4163636570746564486967685761746572223a302c22426f756e6453657373696f6e53657373"
      "696f6e4f776e65724964223a6e756c6c2c22426f756e6453657373696f6e53657373696f6e4f776e65724c6561736547656e65726174696f6e223a30"
      "2c225265736572766174696f6e546f6b656e223a223030313132323333343435353636373738383939616162626363646465656666222c2252657365"
      "727665645061796c6f61644279746573223a38333935313631382c225461726765744e6f6465526964223a225a484e30222c225461726765744e6f64"
      "6547656e65726174696f6e223a31392c2254617267657453706f7447656e65726174696f6e223a32332c22546172676574417574686f726974794f77"
      "6e657247656e65726174696f6e223a31322c2254617267657453706f74417574686f726974794f776e657247656e65726174696f6e223a32392c2252"
      "656c6f636174696f6e436f6f7264696e61746f724f776e65724964223a226f776e65722d61222c2252656c6f636174696f6e436f6f7264696e61746f"
      "724c6561736547656e65726174696f6e223a31372c2252656c6f636174696f6e436f6f7264696e61746f724e6f6465526964223a2263334a6a222c22"
      "52656c6f636174696f6e436f6f7264696e61746f724e6f646547656e65726174696f6e223a31332c2252656c6f636174696f6e436f6f7264696e6174"
      "6f724578706563746564417574686f7269747953746f726556657273696f6e223a2273746f72652d7631222c224163746f724e6f646547656e657261"
      "74696f6e223a31332c2245787065637465644f776e65724c6561736547656e65726174696f6e223a31377d2c2254617267657453706f744964223a22"
      "7461726765742d73706f74222c225461726765744e6f6465526964223a225a484e30222c225461726765744e6f646547656e65726174696f6e223a31"
      "392c2254617267657453706f7447656e65726174696f6e223a32332c22546172676574417574686f726974794f776e657247656e65726174696f6e22"
      "3a31322c224f7065726174696f6e496448696768223a33312c224f7065726174696f6e49644c6f77223a33372c225265706c79436f6e74656e745479"
      "7065223a226170706c69636174696f6e2f6a736f6e222c225265706c79223a22227d7b7d5b5d";
    test.require (
      frozen.canonical_bytes == decode_hex_vector (node_zljr_frozen_hex),
      "ZLJR saved-work must match the fixed Node byte vector");
    const auto decoded =
      protocol::decode_actor_join_recovery_saved_work (frozen);
    test.require (
      decoded && decoded->actor_id == recovery.actor_id
        && decoded->actor_type == recovery.actor_type
        && decoded->handoff_id == handoff_id
        && decoded->relocation == relocation
        && decoded->source_spot_id == recovery.source_spot_id
        && decoded->source_node_routing_id
             == recovery.source_node_routing_id
        && decoded->target_spot_id == recovery.target_spot_id
        && decoded->target_node_routing_id
             == recovery.target_node_routing_id
        && decoded->coordinator == recovery.coordinator
        && decoded->request == recovery.request
        && decoded->reply == recovery.reply,
      "ZLJR saved-work must preserve the Node/.NET byte contract and identity fields");

    auto independent_recovery = recovery;
    independent_recovery.handoff_id = "source-issued-handoff";
    independent_recovery.reservation_token = "target-issued-reservation";
    independent_recovery.reserved_payload_bytes = 97;
    const auto independently_issued =
      protocol::decode_actor_join_recovery_saved_work (
        protocol::encode_actor_join_recovery_saved_work (independent_recovery));
    test.require (
      independently_issued
        && independently_issued->relocation == independent_recovery.relocation
        && independently_issued->handoff_id == independent_recovery.handoff_id
        && independently_issued->reservation_token
             == independent_recovery.reservation_token
        && independently_issued->reserved_payload_bytes
             == independent_recovery.reserved_payload_bytes,
      "ZLJR must preserve independently issued handoff and admission values");

    frozen_object_state_t actor{
      .owner = {object_kind_t::actor, "actor-a", 7, 11, "mesh", "src"},
      .stable_type = "sample.Actor",
      .application_state = {1, 2, 3},
      .pending_application = {
        turn_record_t{1, frozen.canonical_bytes, std::nullopt,
                      std::nullopt, frozen}},
      .timers = {}};
    const auto envelope_bytes = maintenance_runtime_t::encode_envelope (
      {actor}, relocation, 42);
    const auto envelope =
      maintenance_runtime_t::decode_envelope (envelope_bytes);
    test.require (
      envelope && envelope->application_version == 42
        && envelope->saved_work.size () == 1
        && protocol::decode_actor_join_recovery_saved_work (
             envelope->saved_work.front ().record)
             .has_value (),
      "Join relocation must carry descriptor applicationVersion and one canonical ZLJR record");
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
    const auto result = await_task (host.terminate (termination_intent_t::retire));
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
      host.state () == maintenance_admission_state_t::serving
        && !host.terminal_result ()
        && after_blocked.status == create_status_t::reserved,
      "blocked Retire must restore Serving without a host terminal result");
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
        retire_result = await_task (host.terminate (termination_intent_t::retire));
    });
    {
        std::unique_lock lock (gate);
        changed.wait (lock, [&] { return entered; });
    }
    std::thread shutdown ([&] {
        shutdown_result =
          await_task (host.terminate (termination_intent_t::shutdown));
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
    const auto source_application_payload =
      zlink::framework::runtime::encode_actor_authority_payload ({
        .state = zlink::framework::runtime::actor_authority_state_t::ready,
        .stable_type = "player",
        .actor_id = "actor-public",
        .current_spot_id = "source-spot",
        .current_spot_generation = 11,
        .current_spot_kind = zlink::framework::runtime::actor_authority_spot_kind_t::user,
        .owner_id = "owner-a",
        .owner_lease_generation = 3,
        .mesh_name = "mesh",
        .node_rid = zlink::framework::node_rid_t::from_string ("node-a"),
        .node_generation = 16});
    store.snapshot =
      zlink::framework::authority_snapshot_t{
        "1",
        source_application_payload,
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
    const zlink::framework::object_creation_target_t target_placement{
      "mesh-b",
      zlink::framework::node_rid_t::from_string ("node-b"),
      17,
      target_owner};
    auto target = source;
    target.mesh_name = "mesh-b";
    target.node_id = "node-b";
    ++target.authority_owner_generation;
    const auto relocated_application_payload =
      zlink::framework::runtime::encode_actor_authority_payload ({
        .state = zlink::framework::runtime::actor_authority_state_t::ready,
        .stable_type = "player",
        .actor_id = "actor-public",
        .current_spot_id = "target-spot",
        .current_spot_generation = 12,
        .current_spot_kind = zlink::framework::runtime::actor_authority_spot_kind_t::user,
        .owner_id = "owner-b",
        .owner_lease_generation = 5,
        .mesh_name = "mesh-b",
        .node_rid = zlink::framework::node_rid_t::from_string ("node-b"),
        .node_generation = 17});
    /* A foreign source can preserve its relocating envelope between the
     * target's read and owner-changing CAS.  That advances only storeVersion,
     * not either logical fence, so the target must refresh through the same
     * bounded retry window used by the other runtime implementations. */
    store.remaining_auxiliary_conflicts = 7;
    const auto published = adapter.publish (
      source, target, target_owner, target_placement,
      "root-public", 42,
      digest_with (9), relocated_application_payload);
    test.require (
      published.status == authority_publish_status_t::published
        && published.current
        && published.current->source.mesh_name == "mesh"
        && published.current->target.mesh_name == "mesh-b"
        && published.current->target.node_id == "node-b"
        && published.current->target.authority_owner_generation == 12
        && published.current->application_payload
             == relocated_application_payload
        && store.remaining_auxiliary_conflicts == 0,
      "public authority adapter must refresh through bounded auxiliary store-version conflicts");
    const auto stored_projection = store.snapshot
      ? zlink::framework::runtime::decode_actor_authority_payload (
          store.snapshot->payload, store.snapshot->object_generation)
      : std::nullopt;
    const auto begins_with_zlra3 = [] (const std::vector<std::byte> &bytes) {
        constexpr std::string_view magic{"ZLRA3"};
        return bytes.size () >= magic.size ()
          && std::equal (magic.begin (), magic.end (), bytes.begin (),
                         [] (char expected, std::byte actual) {
                             return static_cast<unsigned char> (expected)
                                    == std::to_integer<unsigned char> (actual);
                         });
    };
    test.require (
      store.snapshot && !begins_with_zlra3 (store.snapshot->payload)
        && stored_projection
        && stored_projection->actor.object_generation () == 7
        && stored_projection->actor.actor_id ().value () == "actor-public"
        && stored_projection->actor.node_rid ().value () == "node-b",
      "remote actor commit must store a canonical authority row resolvable by actor_directory_t::find");
    test.require (
      store.observed_target_owner
        && store.observed_target_owner->owner_id == "owner-b"
        && store.observed_target_owner->lease_generation == 5
        && store.observed_target_placement
        && store.observed_target_placement->mesh_name == "mesh-b"
        && store.observed_target_placement->node_rid.value () == "node-b"
        && store.observed_target_placement->node_lifecycle_generation == 17
        && store.snapshot->owner.owner_id == "owner-b"
        && !store.observed_keys.empty ()
        && std::all_of (
          store.observed_keys.begin (), store.observed_keys.end (),
          [] (const auto &key) {
              return key == "zla1:a:12:actor-public";
          }),
      "public authority adapter must pass the exact target placement");
    const auto read =
      adapter.read (object_kind_t::actor, "actor-public");
    test.require (
      read && read->target.key == "actor-public"
        && read->target.object_generation == 7
        && read->target.node_id == "node-b"
        && read->target_owner.lease_generation == 5,
      "public authority adapter must decode its canonical actor authority row");

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

} // namespace

void test_application_relocation_remote_production_path (
  test_context_t &test)
{
    using namespace std::chrono_literals;
    namespace detail = zlink::framework::detail;
    namespace framework = zlink::framework;
    namespace protocol = zlink::framework::runtime::protocol;

    const auto core_context = std::make_shared<zlink::context_t> ();
    const auto make_state = [core_context] (
      const std::string &rid) {
        auto state =
          std::make_shared<detail::mesh_node_builder_state_t> (
            "production-relocation-mesh");
        state->core_context = core_context;
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
    detail::mesh_node_runtime_t session_owner (
      make_state ("production-session-owner"));
    source.configure_relocation_runtime (authority, roots);
    target.configure_relocation_runtime (authority, roots);
    std::optional<detail::bound_session_relocation_route_t>
      bound_session_route;
    std::atomic<std::uint64_t> observed_session_sequence{0};
    source.configure_bound_session_relocation_resolver (
      [&bound_session_route, &observed_session_sequence] (
        const object_ref_t &candidate)
      -> std::optional<detail::bound_session_relocation_route_t> {
          if (!bound_session_route
              || candidate.key != "production-remote-actor"
              || candidate.object_generation != 1
              || candidate.authority_owner_generation != 1)
              return std::nullopt;
          auto resolved = *bound_session_route;
          resolved.observed_sequence =
            observed_session_sequence.load (
              std::memory_order_acquire);
          return resolved;
      });
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
    std::atomic<bool> route_owner_observed_target_ready{false};
    session_owner.configure_session_route_owner (
      [&target, &route_owner_observed_target_ready] {
          if (target.native_node ().objects ().find (
                object_kind_t::actor,
                "production-remote-actor")) {
              route_owner_observed_target_ready.store (
                true, std::memory_order_release);
          }
          return std::optional<framework::location_owner_token_t>{
            {"session-owner", 17}};
      });
    session_owner.configure_session_route_target_owner (
      [&target] (
        const std::string &actor_id,
        std::uint64_t object_generation,
        std::uint64_t authority_owner_generation,
        const zlink::routing_id_t &target_node,
        std::uint64_t target_node_generation)
      -> std::optional<framework::location_owner_token_t> {
          if (actor_id != "production-remote-actor"
              || object_generation != 1
              || authority_owner_generation != 2
              || target_node != target.status ().routing_id ()
              || target_node_generation
                   != target.status ().lifecycle_generation ())
              return std::nullopt;
          return framework::location_owner_token_t{
            "target-owner", 9};
      });
    source.start ();
    target.start ();
    session_owner.start ();
    source.connect_peer (
      target.status ().routing_id (),
      target.status ().local_endpoint ());
    source.connect_peer (
      session_owner.status ().routing_id (),
      session_owner.status ().local_endpoint ());
    target.connect_peer (
      session_owner.status ().routing_id (),
      session_owner.status ().local_endpoint ());
    const auto admission_deadline =
      std::chrono::steady_clock::now () + 5s;
    while ((!source.has_admitted_peer (
               target.status ().routing_id (),
               target.status ().lifecycle_generation ())
            || !target.has_admitted_peer (
              source.status ().routing_id (),
              source.status ().lifecycle_generation ())
            || !source.has_admitted_peer (
              session_owner.status ().routing_id (),
              session_owner.status ().lifecycle_generation ())
            || !session_owner.has_admitted_peer (
              source.status ().routing_id (),
              source.status ().lifecycle_generation ())
            || !target.has_admitted_peer (
              session_owner.status ().routing_id (),
              session_owner.status ().lifecycle_generation ())
            || !session_owner.has_admitted_peer (
              target.status ().routing_id (),
              target.status ().lifecycle_generation ()))
           && std::chrono::steady_clock::now ()
                < admission_deadline) {
        (void) source.dispatch_ready (
          [] (const auto &, const auto &, auto) {});
        (void) target.dispatch_ready (
          [] (const auto &, const auto &, auto) {});
        (void) session_owner.dispatch_ready (
          [] (const auto &, const auto &, auto) {});
        std::this_thread::sleep_for (1ms);
    }
    test.require (
      source.has_admitted_peer (
        target.status ().routing_id (),
        target.status ().lifecycle_generation ())
        && source.has_admitted_peer (
          session_owner.status ().routing_id (),
          session_owner.status ().lifecycle_generation ())
        && target.has_admitted_peer (
          session_owner.status ().routing_id (),
          session_owner.status ().lifecycle_generation ()),
      "production relocation source must admit the target and Session owner");

    const auto created = source.create_application_actor (
      "production.actor", "production-remote-actor",
      std::nullopt, 1s);
    test.require (
      static_cast<bool> (created),
      "remote production relocation Actor must be created");
    if (!created) {
        source.stop ();
        target.stop ();
        session_owner.stop ();
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

    const auto bound_source_object =
      source.native_node ().resolve_actor (actor);
    test.require (
      bound_source_object.has_value (),
      "production relocation must resolve the bound Actor before Session sealing");
    if (!bound_source_object) {
        source.stop ();
        target.stop ();
        session_owner.stop ();
        return;
    }
    const auto session_connection =
      session_owner.native_node ().sessions ().open (
        zlink::routing_id_t::from (
          "production-bound-session").to_hex ());
    const auto [session_bind_error, session_binding] =
      session_owner.native_node ().sessions ().bind_remote (
        session_connection, *bound_source_object,
        source.status ().lifecycle_generation (), 1);
    const auto [initial_inbound_error, initial_inbound] =
      session_owner.native_node ().sessions ().admit_inbound (
        session_binding);
    test.require (
      session_bind_error == stateful_error_t::none
        && initial_inbound_error == stateful_error_t::none
        && initial_inbound
        && initial_inbound->inbound_sequence == 1,
      "production Session checkpoint must retain pre-seal active ingress at its exact high-water");
    bound_session_route = detail::bound_session_relocation_route_t{
      session_owner.status ().routing_id (),
      session_owner.status ().lifecycle_generation (),
      {"session-owner", 17},
      zlink::routing_id_t::from ("production-bound-session"),
      session_binding.binding_generation,
      0};

    const auto initial_inbound_completed =
      initial_inbound
      && session_owner.native_node ().sessions ().complete_inbound (
           *initial_inbound)
           == stateful_error_t::none;
    test.require (
      initial_inbound_completed,
      "the active Session ingress must complete before exact sealing");
    observed_session_sequence.store (1, std::memory_order_release);

    std::atomic<bool> stop_dispatch{false};
    const auto dispatch = [&] (detail::mesh_node_runtime_t &node) {
        while (!stop_dispatch.load (
          std::memory_order_acquire)) {
            (void) node.dispatch_ready (
              [] (const auto &, const auto &, auto) {});
            std::this_thread::sleep_for (1ms);
        }
      };
    {
        /* The schema stream carries no participant stable type: the target
         * derives it from the Location Store authority row. */
        std::lock_guard lock (authority->mutex);
        authority->participant_identities = {
          relocation_participant_identity_t{
            *bound_source_object, "production.actor", std::nullopt}};
    }
    relocation_result_t result;
    std::thread relocation_thread ([&] {
        result = await_task (source.relocate_application_actor (
          actor, target_descriptor, snapshot));
    });
    std::this_thread::sleep_for (10ms);
    std::thread source_dispatch (
      [&] { dispatch (source); });
    std::thread target_dispatch (
      [&] { dispatch (target); });
    std::thread session_owner_dispatch (
      [&] { dispatch (session_owner); });
    relocation_thread.join ();
    const auto route_deadline =
      std::chrono::steady_clock::now () + 5s;
    while (std::chrono::steady_clock::now () < route_deadline) {
        const auto current = session_owner.native_node ()
                               .sessions ()
                               .current_binding (
                                 "production-remote-actor");
        if (current
            && current->actor.node_id
                 == target.status ().routing_id ().to_string ())
            break;
        std::this_thread::sleep_for (1ms);
    }

    const auto committed_binding =
      session_owner.native_node ().sessions ().current_binding (
        "production-remote-actor");
    const auto [continued_inbound_error, continued_inbound] =
      committed_binding
        ? session_owner.native_node ().sessions ().admit_inbound (
            *committed_binding)
        : std::pair{stateful_error_t::not_found,
                    std::optional<stream_dispatch_t>{}};
    const auto continued_inbound_completed =
      continued_inbound
        && session_owner.native_node ().sessions ().complete_inbound (
             *continued_inbound)
             == stateful_error_t::none;
    stop_dispatch.store (true, std::memory_order_release);
    source_dispatch.join ();
    target_dispatch.join ();
    session_owner_dispatch.join ();

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
    test.require (
      result.terminal == relocation_terminal_t::completed
        && !result.authority
        && result.target_handoff
        && result.target_handoff->sources
             == std::vector<object_ref_t>{*bound_source_object}
        && result.target_handoff->target_node_id
             == target.status ().routing_id ().to_string ()
        && restored_target
             == std::optional<object_ref_t>{expected_target}
        && roots->roots.size () == 1
        && route_owner_observed_target_ready.load (
          std::memory_order_acquire)
        && committed_binding
        && committed_binding->actor == expected_target
        && committed_binding->binding_generation
             == session_binding.binding_generation
        && committed_binding->target_node_generation
             == target.status ().lifecycle_generation ()
        && committed_binding->owner_lease_generation == 9
        && continued_inbound_error == stateful_error_t::none
        && continued_inbound_completed
        && continued_inbound->inbound_sequence == 2,
      "production relocation must commit authority only at the target, restore before the one-way Session route, and continue ingress");
    source.stop ();
    target.stop ();
    session_owner.stop ();
}

class aggregate_materialized_actor_t final
    : public zlink::framework::actor_t
{
  public:
    explicit aggregate_materialized_actor_t (
      zlink::framework::actor_context_t context) :
        _context (std::move (context))
    {
    }

    zlink::framework::actor_context_t &context () noexcept override
    {
        return _context;
    }
    const zlink::framework::actor_context_t &context () const noexcept override
    {
        return _context;
    }

    int value = 37;

  private:
    zlink::framework::actor_context_t _context;
};

class aggregate_materialized_actor_factory_t final
    : public zlink::framework::actor_factory_t<
        aggregate_materialized_actor_t>
{
  public:
    zlink::framework::task_t<
      std::shared_ptr<aggregate_materialized_actor_t>>
    create (zlink::framework::actor_context_t context,
            std::stop_token) override
    {
        if (record_target.load (std::memory_order_acquire))
            factory_order.store (++sequence, std::memory_order_release);
        co_return std::make_shared<aggregate_materialized_actor_t> (
          std::move (context));
    }

    static inline std::atomic_bool record_target{false};
    static inline std::atomic_int sequence{0};
    static inline std::atomic_int factory_order{0};
};

class aggregate_materialized_actor_adapter_t final
    : public zlink::framework::actor_relocation_adapter_t<
        aggregate_materialized_actor_t>
{
  public:
    zlink::framework::task_t<std::vector<std::byte>>
    capture (aggregate_materialized_actor_t &actor,
             std::stop_token) override
    {
        capture_count.fetch_add (1, std::memory_order_acq_rel);
        co_return std::vector<std::byte>{
          static_cast<std::byte> (actor.value)};
    }

    zlink::framework::task_t<void>
    restore (aggregate_materialized_actor_t &actor,
             std::vector<std::byte> payload,
             std::stop_token) override
    {
        actor.value = payload.empty ()
                        ? -1
                        : std::to_integer<int> (payload.front ());
        restored_value.store (actor.value, std::memory_order_release);
        restore_order.store (
          ++aggregate_materialized_actor_factory_t::sequence,
          std::memory_order_release);
        co_return;
    }

    static inline std::atomic_int capture_count{0};
    static inline std::atomic_int restore_order{0};
    static inline std::atomic_int restored_value{-1};
};

class aggregate_materialized_spot_t final
    : public zlink::framework::spot_t<
        aggregate_materialized_actor_t>
{
  public:
    explicit aggregate_materialized_spot_t (
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
    void configure () override
    {
        configure_lifecycle_probe ();
    }

    struct lifecycle_probe_message_t
    {
    };

    zlink::framework::task_t<void> lifecycle_probe (
      aggregate_materialized_actor_t &,
      zlink::framework::message_context_t &,
      const lifecycle_probe_message_t &)
    {
        co_return;
    }

    void configure_lifecycle_probe ()
    {
        _context.handlers ().add_actor_send<
          &aggregate_materialized_spot_t::lifecycle_probe> (
            "aggregate-lifecycle-probe");
    }

    zlink::framework::task_t<zlink::framework::spot_create_response_t>
    on_create (const zlink::framework::message_t &) override
    {
        co_return zlink::framework::spot_create_response_t::accept ();
    }
    zlink::framework::task_t<void> on_initialize () override
    {
        co_return;
    }
    zlink::framework::task_t<zlink::framework::spot_actor_join_result_t>
    on_actor_join (std::string_view,
                   const zlink::framework::message_t &) override
    {
        co_return zlink::framework::spot_actor_join_result_t::accept ();
    }
    zlink::framework::task_t<void>
    on_actor_joined (aggregate_materialized_actor_t &actor) override
    {
        joined_saw_membership.store (
          membership_visible && membership_visible (),
          std::memory_order_release);
        joined_saw_state.store (actor.value == 37, std::memory_order_release);
        joined_before_cas.store (
          authority_commit_count && authority_commit_count () == 0,
          std::memory_order_release);
        joined_held_ingress.store (
          target_objects && target_actor
            && target_objects->enqueue (
                 *target_actor, turn_domain_t::application,
                 {201, {0x51}})
                 == stateful_error_t::none,
          std::memory_order_release);
        joined_order.store (
          ++aggregate_materialized_actor_factory_t::sequence,
          std::memory_order_release);
        co_return;
    }
    zlink::framework::task_t<void>
    on_leave_actor (aggregate_materialized_actor_t &) override
    {
        co_return;
    }
    zlink::framework::task_t<void>
    on_relocation_ready_completed (
      const zlink::framework::spot_relocation_ready_completion_t &completion)
      override
    {
        ready_after_cas.store (
          completion.outcome
              == zlink::framework::spot_relocation_ready_outcome_t::relocated
            && authority_commit_count && authority_commit_count () == 1,
          std::memory_order_release);
        ready_before_dispatch.store (
          target_objects && target_actor
            && target_objects->try_claim (
                 *target_actor, turn_domain_t::application).first
                 == stateful_error_t::moving,
          std::memory_order_release);
        ready_order.store (
          ++aggregate_materialized_actor_factory_t::sequence,
          std::memory_order_release);
        co_return;
    }

    int value = 19;
    static inline std::function<bool ()> membership_visible;
    static inline std::function<int ()> authority_commit_count;
    static inline stateful_object_runtime_t *target_objects = nullptr;
    static inline const object_ref_t *target_actor = nullptr;
    static inline std::atomic_bool joined_saw_membership{false};
    static inline std::atomic_bool joined_saw_state{false};
    static inline std::atomic_bool joined_before_cas{false};
    static inline std::atomic_bool joined_held_ingress{false};
    static inline std::atomic_bool ready_after_cas{false};
    static inline std::atomic_bool ready_before_dispatch{false};
    static inline std::atomic_int restore_order{0};
    static inline std::atomic_int joined_order{0};
    static inline std::atomic_int ready_order{0};

  private:
    zlink::framework::spot_context_t _context;
};

class aggregate_materialized_spot_adapter_t final
    : public zlink::framework::spot_relocation_adapter_t<
        aggregate_materialized_spot_t>
{
  public:
    zlink::framework::task_t<std::vector<std::byte>>
    capture (aggregate_materialized_spot_t &spot,
             std::stop_token) override
    {
        capture_count.fetch_add (1, std::memory_order_acq_rel);
        co_return std::vector<std::byte>{
          static_cast<std::byte> (spot.value)};
    }
    zlink::framework::task_t<void>
    restore (aggregate_materialized_spot_t &spot,
             std::vector<std::byte> payload,
             std::stop_token) override
    {
        spot.value = payload.empty ()
                       ? -1
                       : std::to_integer<int> (payload.front ());
        restored_value.store (spot.value, std::memory_order_release);
        if (aggregate_materialized_actor_factory_t::record_target.load (
              std::memory_order_acquire)) {
            aggregate_materialized_spot_t::restore_order.store (
              ++aggregate_materialized_actor_factory_t::sequence,
              std::memory_order_release);
        }
        co_return;
    }

    static inline std::atomic_int capture_count{0};
    static inline std::atomic_int restored_value{-1};
};

void test_application_user_spot_aggregate_remote_production_path (
  test_context_t &test)
{
    using namespace std::chrono_literals;
    namespace detail = zlink::framework::detail;
    namespace framework = zlink::framework;

    const auto core_context = std::make_shared<zlink::context_t> ();
    const auto make_state = [core_context] (const std::string &rid) {
        auto state =
          std::make_shared<detail::mesh_node_builder_state_t> (
            "production-aggregate-mesh");
        state->core_context = core_context;
        state->listen_endpoint = "tcp://127.0.0.1:0";
        state->routing_id = zlink::routing_id_t::from (rid);
        state->spot_builder.add_spot_factory<
          aggregate_materialized_spot_t> (
            "production.aggregate.spot",
            [] (framework::spot_context_t context) {
                return std::make_shared<aggregate_materialized_spot_t> (
                  std::move (context));
            },
            [] (auto &factory) {
                factory.set_execution_mode (
                  framework::user_spot_execution_mode_t::spot_wide);
                factory.set_relocation_readiness (
                  framework::spot_relocation_readiness_mode_t::
                    application_signaled);
                factory.template preserve_state_with<
                  aggregate_materialized_spot_adapter_t> ();
            });
        state->spot_builder.add_actor_factory<
          aggregate_materialized_actor_t> (
            "production.aggregate.actor",
            std::make_shared<aggregate_materialized_actor_factory_t> (),
            [] (auto &factory) {
                factory.template preserve_state_with<
                  aggregate_materialized_actor_adapter_t> ();
            });
        return state;
    };
    aggregate_materialized_actor_factory_t::record_target.store (false);
    aggregate_materialized_actor_factory_t::sequence.store (0);
    aggregate_materialized_actor_factory_t::factory_order.store (0);
    aggregate_materialized_actor_adapter_t::capture_count.store (0);
    aggregate_materialized_actor_adapter_t::restore_order.store (0);
    aggregate_materialized_actor_adapter_t::restored_value.store (-1);
    aggregate_materialized_spot_adapter_t::capture_count.store (0);
    aggregate_materialized_spot_adapter_t::restored_value.store (-1);
    aggregate_materialized_spot_t::joined_saw_membership.store (false);
    aggregate_materialized_spot_t::joined_saw_state.store (false);
    aggregate_materialized_spot_t::joined_before_cas.store (false);
    aggregate_materialized_spot_t::joined_held_ingress.store (false);
    aggregate_materialized_spot_t::ready_after_cas.store (false);
    aggregate_materialized_spot_t::ready_before_dispatch.store (false);
    aggregate_materialized_spot_t::restore_order.store (0);
    aggregate_materialized_spot_t::joined_order.store (0);
    aggregate_materialized_spot_t::ready_order.store (0);
    auto roots = std::make_shared<memory_relocation_repository_t> ();
    auto authority = std::make_shared<memory_authority_store_t> ();
    auto aggregates =
      std::make_shared<memory_aggregate_authority_t> (authority);
    const auto source_state = make_state ("production-aggregate-source");
    const auto target_state = make_state ("production-aggregate-target");
    detail::mesh_node_runtime_t source (source_state);
    detail::mesh_node_runtime_t target (target_state);
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
    const auto configure_materialization = [authority] (
      detail::mesh_node_runtime_t &node,
      const std::shared_ptr<detail::spot_node_builder_state_t> &spot_state) {
        detail::spot_node_runtime_t (spot_state)
          .bind_relocation_authority (authority);
        auto &objects = node.native_node ().objects ();
        objects.configure_relocation_state (
          [spot_state] (const object_ref_t &object,
                        const std::string &stable_type,
                        std::stop_token cancellation) {
              return detail::spot_node_runtime_t (spot_state)
                .capture_spot_relocation_state (
                  object, stable_type, cancellation);
          },
          [spot_state] (const frozen_object_state_t &frozen,
                        const object_ref_t &object,
                        std::stop_token cancellation) {
              return detail::spot_node_runtime_t (spot_state)
                .restore_spot_relocation_state (
                  frozen, object, cancellation);
          });
        objects.configure_relocation_materialization (
          [spot_state] (const frozen_object_state_t &frozen,
                        const object_ref_t &object,
                        const std::optional<object_ref_t> &spot,
                        std::stop_token cancellation) {
              return detail::spot_node_runtime_t (spot_state)
                .materialize_relocation_state (
                  frozen, object, spot, cancellation);
          },
          [spot_state] (const std::vector<object_ref_t> &objects) {
              return detail::spot_node_runtime_t (spot_state)
                .commit_relocation_materialization (objects);
          },
          [spot_state] (const std::vector<object_ref_t> &objects) {
              detail::spot_node_runtime_t (spot_state)
                .abort_relocation_materialization (objects);
          });
    };
    configure_materialization (source, source_state->spot_state);
    configure_materialization (target, target_state->spot_state);
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

    detail::spot_node_runtime_t source_spots (source_state->spot_state);
    const auto source_native_node =
      source_state->spot_state->native_node.lock ();
    source_state->spot_state->native_node.reset ();
    const frozen_object_state_t source_application_spot{
      .owner =
        {.kind = object_kind_t::user_spot,
         .key = "production-aggregate-spot",
         .object_generation = 1,
         .authority_owner_generation = 1,
         .mesh_name = "production-aggregate-mesh",
         .node_id = source.status ().routing_id ().to_string ()},
      .stable_type = "production.aggregate.spot",
      .application_state = {19},
      .pending_application = {},
      .timers = {}};
    const auto application_spot =
      source_spots.restore_spot_relocation_state (
        source_application_spot,
        source_application_spot.owner);
    source_state->spot_state->native_node = source_native_node;
    test.require (
      application_spot,
      "production aggregate source Spot application must be materialized");
    if (!application_spot) {
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

    const auto application_actor_ref =
      ::zlink::framework::detail::actor_ref_access_t::make (
        framework::node_rid_t::from_string (joined_actor.node_id),
        "production.aggregate.actor", joined_actor.key,
        joined_actor.object_generation);
    detail::actor_gateway_runtime_t source_actor_gateway;
    auto source_actor_context =
      source_actor_gateway.actor_context (application_actor_ref);
    std::shared_ptr<void> application_actor;
    {
        std::lock_guard<std::recursive_mutex> lock (
          source_state->spot_state->mutex);
        const auto factory = source_state->spot_state->actor_factories.find (
          "production.aggregate.actor");
        if (factory != source_state->spot_state->actor_factories.end ()) {
            application_actor = factory->second.create_context_instance (
              std::move (source_actor_context));
        }
    }
    test.require (
      static_cast<bool> (application_actor),
      "production aggregate source Actor application must be materialized");
    if (!application_actor) {
        source.stop ();
        target.stop ();
        return;
    }
    {
        std::lock_guard<std::recursive_mutex> lock (
          source_state->spot_state->mutex);
        const auto key =
          std::string ("production.aggregate.actor:") + joined_actor.key;
        detail::record_actor_instance_index_unlocked (
          *source_state->spot_state, application_actor_ref,
          application_actor.get ());
        source_state->spot_state->actor_instances.emplace (
          key, application_actor);
    }
    source_spots.record_actor_spot (
      application_actor_ref,
      framework::spot_id_t ("production-aggregate-spot"));
    test.require (
      source_objects.register_timer (
        *spot, {101, 1000, 250, 7})
          == stateful_error_t::none
        && source_objects.register_timer (
             joined_actor, {102, 2000, 0, 8})
             == stateful_error_t::none,
      "production aggregate timers must be registered");
    object_ref_t expected_spot = *spot;
    expected_spot.node_id =
      target.status ().routing_id ().to_string ();
    ++expected_spot.authority_owner_generation;
    object_ref_t expected_actor = joined_actor;
    expected_actor.node_id =
      target.status ().routing_id ().to_string ();
    ++expected_actor.authority_owner_generation;
    aggregate_materialized_spot_t::authority_commit_count =
      [aggregates] {
          std::lock_guard lock (aggregates->mutex);
          return aggregates->commit_count;
      };
    aggregate_materialized_spot_t::target_objects =
      &target.native_node ().objects ();
    aggregate_materialized_spot_t::target_actor = &expected_actor;
    aggregate_materialized_spot_t::membership_visible =
      [target_state, expected_actor, expected_spot] {
          std::lock_guard<std::recursive_mutex> lock (
            target_state->spot_state->mutex);
          const auto key =
            std::string ("production.aggregate.actor:")
            + expected_actor.key;
          const auto spot_id =
            target_state->spot_state->actor_spot_ids.find (key);
          const auto generation =
            target_state->spot_state->actor_generations.find (key);
          const auto fence =
            target_state->spot_state->actor_authority_fences.find (key);
          return spot_id
                   != target_state->spot_state->actor_spot_ids.end ()
                 && std::string (spot_id->second) == expected_spot.key
                 && generation
                      != target_state->spot_state->actor_generations.end ()
                 && generation->second == expected_actor.object_generation
                 && fence
                      != target_state->spot_state->actor_authority_fences.end ()
                 && fence->second.authority_owner_generation
                      == expected_actor.authority_owner_generation;
      };
    try {
        const auto source_spot_state =
          source_spots.capture_spot_relocation_state (
            *spot, "production.aggregate.spot");
        test.require (
          source_spot_state == std::vector<std::uint8_t>{19},
          "production aggregate source Spot adapter must be capturable");
    }
    catch (const std::exception &error) {
        std::cerr << "V11-M6C-CPP source Spot capture diagnostic: "
                  << error.what () << '\n';
        test.require (
          false,
          "production aggregate source Spot adapter must be capturable");
    }
    aggregate_materialized_spot_adapter_t::capture_count.store (0);
    aggregate_materialized_actor_factory_t::record_target.store (
      true, std::memory_order_release);
    {
        /* The direct-transfer stream carries no participant identity: the
         * target reconstructs the inventory from Location Store authority
         * rows.  Serve those rows from the shared in-memory authority. */
        std::lock_guard lock (authority->mutex);
        authority->participant_identities = {
          relocation_participant_identity_t{
            *spot, "production.aggregate.spot", std::nullopt},
          relocation_participant_identity_t{
            joined_actor, "production.aggregate.actor",
            std::pair{std::string ("production-aggregate-spot"),
                      spot->object_generation}}};
    }

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
        result = await_task (source.relocate_application_unit (
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
             "aggregate-actor-v1")}));
    });
    std::thread source_dispatch (
      [&] { dispatch (source); });
    std::thread target_dispatch (
      [&] { dispatch (target); });
    relocation_thread.join ();
    const auto target_committed = wait_until_bounded (
      [&] {
          return aggregates->commit_count == 1
                 && target.native_node ().objects ().find (
                      object_kind_t::user_spot,
                      "production-aggregate-spot")
                 && target.native_node ().objects ().find (
                      object_kind_t::actor,
                      "production-aggregate-actor");
      },
      5s);
    stop_dispatch.store (true, std::memory_order_release);
    source_dispatch.join ();
    target_dispatch.join ();

    const auto restored_spot =
      target.native_node ().objects ().find (
        object_kind_t::user_spot,
        expected_spot.key);
    const auto restored_actor =
      target.native_node ().objects ().find (
        object_kind_t::actor,
        expected_actor.key);
    const auto [saved_claim_error, saved_claim] =
      target.native_node ().objects ().try_claim (
        expected_actor, turn_domain_t::application);
    const auto saved_completed =
      saved_claim
      && target.native_node ().objects ().complete_claim (
           expected_actor, turn_domain_t::application)
           == stateful_error_t::none;
    const auto lifecycle_ordered =
      aggregate_materialized_spot_t::restore_order.load (
        std::memory_order_acquire) == 1
      && aggregate_materialized_actor_factory_t::factory_order.load (
           std::memory_order_acquire) == 2
      && aggregate_materialized_actor_adapter_t::restore_order.load (
           std::memory_order_acquire) == 3
      && aggregate_materialized_spot_t::joined_order.load (
           std::memory_order_acquire) == 4
      && aggregate_materialized_spot_t::ready_order.load (
           std::memory_order_acquire) == 5;
    if (result.terminal != relocation_terminal_t::completed
        || !result.target_handoff
        || !target_committed
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
          << " lifecycle-order=" << lifecycle_ordered
          << " spot-capture="
          << aggregate_materialized_spot_adapter_t::capture_count.load ()
          << " actor-capture="
          << aggregate_materialized_actor_adapter_t::capture_count.load ()
          << " saved-claim=" << static_cast<int> (saved_claim_error)
          << '\n';
    test.require (
      result.terminal == relocation_terminal_t::completed
        && result.target_handoff
        && target_committed
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
    test.require (
      aggregate_materialized_spot_adapter_t::capture_count.load (
        std::memory_order_acquire) == 1
        && aggregate_materialized_actor_adapter_t::capture_count.load (
             std::memory_order_acquire) == 1
        && aggregate_materialized_spot_adapter_t::restored_value.load (
             std::memory_order_acquire) == 19
        && aggregate_materialized_actor_adapter_t::restored_value.load (
             std::memory_order_acquire) == 37
        && aggregate_materialized_spot_t::joined_saw_membership.load (
             std::memory_order_acquire)
        && aggregate_materialized_spot_t::joined_saw_state.load (
             std::memory_order_acquire)
        && aggregate_materialized_spot_t::joined_before_cas.load (
             std::memory_order_acquire)
        && aggregate_materialized_spot_t::joined_held_ingress.load (
             std::memory_order_acquire)
        && aggregate_materialized_spot_t::ready_after_cas.load (
             std::memory_order_acquire)
        && aggregate_materialized_spot_t::ready_before_dispatch.load (
             std::memory_order_acquire)
        && lifecycle_ordered
        && saved_claim_error == stateful_error_t::none
        && saved_claim && saved_claim->sequence == 201
        && saved_completed,
      "target materialization must restore Spot then Actor, publish membership before joined, run target-only CAS before ready completion, and open saved ingress last");
    aggregate_materialized_spot_t::membership_visible = {};
    aggregate_materialized_spot_t::authority_commit_count = {};
    aggregate_materialized_spot_t::target_objects = nullptr;
    aggregate_materialized_spot_t::target_actor = nullptr;
    source.stop ();
    target.stop ();
}

class single_capture_adapter_t final :
    public zlink::framework::spot_relocation_adapter_t<fail_first_restore_spot_t>
{
  public:
    zlink::framework::task_t<std::vector<std::byte>>
    capture (fail_first_restore_spot_t &, std::stop_token) override
    {
        co_return std::vector<std::byte>{};
    }
    zlink::framework::task_t<void>
    restore (fail_first_restore_spot_t &, std::vector<std::byte> payload,
             std::stop_token) override
    {
        ++fail_first_restore_spot_t::restore_count;
        fail_first_restore_spot_t::restored_payload = std::move (payload);
        co_return;
    }
};

/* A relocation adapter has exactly one capture()/restore() path — pin that
 * restore_spot_relocation_state runs it byte-identically end to end. */
void test_relocation_adapter_single_capture_restore_path (test_context_t &test)
{
    fail_first_restore_spot_t::factory_count = 0;
    fail_first_restore_spot_t::restore_count = 0;
    fail_first_restore_spot_t::create_count = 0;
    fail_first_restore_spot_t::initialize_count = 0;
    fail_first_restore_spot_t::restored_payload.clear ();

    zlink::framework::spot_node_builder_t builder;
    builder.add_spot_factory<fail_first_restore_spot_t> (
      "single-capture-spot",
      [] (zlink::framework::spot_context_t context) {
          ++fail_first_restore_spot_t::factory_count;
          return std::make_shared<fail_first_restore_spot_t> (
            std::move (context));
      },
      [] (auto &factory) {
          factory.template preserve_state_with<single_capture_adapter_t> ();
      });
    auto runtime = zlink::framework::detail::spot_node_runtime_t::from (builder);

    const object_ref_t source_spot{
      .kind = object_kind_t::user_spot,
      .key = "single-capture-spot-id",
      .object_generation = 1,
      .authority_owner_generation = 1,
      .mesh_name = "mesh",
      .node_id = "source"};

    const frozen_object_state_t frozen{
      .owner = source_spot,
      .stable_type = "single-capture-spot",
      .application_state = {0xca, 0xfe},
      .pending_application = {},
      .timers = {}};
    const object_ref_t target{
      .kind = object_kind_t::user_spot,
      .key = "single-capture-spot-id",
      .object_generation = 1,
      .authority_owner_generation = 2,
      .mesh_name = "mesh",
      .node_id = "target"};
    test.require (
      runtime.restore_spot_relocation_state (frozen, target)
        && fail_first_restore_spot_t::restore_count == 1
        && fail_first_restore_spot_t::restored_payload
             == std::vector<std::byte>{std::byte{0xca}, std::byte{0xfe}},
      "a relocation adapter must restore via its single capture/restore path, "
      "byte-identical to the captured application state");
    runtime.request_stop ();
    runtime.cancel_pending_dispatch ();
    runtime.cancel_pending_work ();
    runtime.release_native_handles ();
}

struct entry_relocation_test_payload_t
{
    int value = 0;
};

void to_json (
  nlohmann::json &json,
  const entry_relocation_test_payload_t &payload)
{
    json = nlohmann::json{{"value", payload.value}};
}

void from_json (
  const nlohmann::json &json,
  entry_relocation_test_payload_t &payload)
{
    payload.value = json.at ("value").get<int> ();
}

class entry_relocation_test_actor_t final : public zlink::framework::actor_t
{
  public:
    explicit entry_relocation_test_actor_t (
      zlink::framework::actor_context_t context) :
        _context (std::move (context))
    {
        ++create_count;
    }
    zlink::framework::actor_context_t &context () noexcept override
    {
        return _context;
    }
    const zlink::framework::actor_context_t &context () const noexcept override
    {
        return _context;
    }

    static inline int create_count = 0;

  private:
    zlink::framework::actor_context_t _context;
};

class entry_relocation_test_actor_factory_t final
    : public zlink::framework::actor_factory_t<
        entry_relocation_test_actor_t>
{
  public:
    zlink::framework::task_t<
      std::shared_ptr<entry_relocation_test_actor_t>>
    create (zlink::framework::actor_context_t context,
            std::stop_token) override
    {
        co_return std::make_shared<entry_relocation_test_actor_t> (
          std::move (context));
    }
};

/* An Entry Spot Actor moving as a single-Actor relocation unit never carries
 * a Spot on the wire (spec 28: the Entry Spot is already present on the
 * target node). This fixture proves the target-local Entry Spot is resolved
 * during materialize_relocation_state, the relocated Actor is joined into
 * it, and the Actor immediately serves an application message through it. */
class entry_relocation_test_entry_spot_t final
    : public zlink::framework::entry_spot_t<entry_relocation_test_actor_t>
{
  public:
    explicit entry_relocation_test_entry_spot_t (
      zlink::framework::entry_spot_context_t context) :
        _context (std::move (context))
    {
    }
    zlink::framework::entry_spot_context_t &context () noexcept override
    {
        return _context;
    }
    const zlink::framework::entry_spot_context_t &
    context () const noexcept override
    {
        return _context;
    }

    void configure () override
    {
        _context.handlers ()
          .add_actor_send<
            &entry_relocation_test_entry_spot_t::on_message> (
            "entry-relocation-test-message");
    }

    zlink::framework::task_t<void> on_message (
      entry_relocation_test_actor_t &,
      zlink::framework::message_context_t &,
      const entry_relocation_test_payload_t &payload)
    {
        ++message_served_count;
        last_message_value = payload.value;
        co_return;
    }

    zlink::framework::task_t<zlink::framework::spot_actor_join_result_t>
    on_actor_join (
      std::string_view,
      const zlink::framework::message_t &) override
    {
        co_return zlink::framework::spot_actor_join_result_t::reject ();
    }
    zlink::framework::task_t<void> on_actor_joined (
      entry_relocation_test_actor_t &actor) override
    {
        ++joined_count;
        joined_spot = this;
        joined_actor = &actor;
        co_return;
    }
    zlink::framework::task_t<void> on_leave_actor (
      entry_relocation_test_actor_t &) override
    {
        co_return;
    }

    static inline int joined_count = 0;
    static inline int message_served_count = 0;
    static inline int last_message_value = 0;
    static inline entry_relocation_test_entry_spot_t *joined_spot = nullptr;
    static inline entry_relocation_test_actor_t *joined_actor = nullptr;

  private:
    zlink::framework::entry_spot_context_t _context;
};

/* checklist B: cpp's standalone Entry Spot Actor direct-relocation restore
 * previously always failed in the real wired path because the target-side
 * Entry Spot was never resolved (restore_relocation always passed
 * target_spot=std::nullopt, and materialize_relocation_state unconditionally
 * rejected actor targets without one). This test exercises the exact
 * function mesh_node_host_service.cpp wires into production
 * (configure_relocation_materialization -> materialize_relocation_state)
 * and proves the fix: it resolves this node's own local Entry Spot. */
void test_entry_spot_actor_relocation_restore_resolves_local_entry_spot (
  test_context_t &test)
{
    entry_relocation_test_actor_t::create_count = 0;
    entry_relocation_test_entry_spot_t::joined_count = 0;
    entry_relocation_test_entry_spot_t::message_served_count = 0;
    entry_relocation_test_entry_spot_t::last_message_value = 0;
    entry_relocation_test_entry_spot_t::joined_spot = nullptr;
    entry_relocation_test_entry_spot_t::joined_actor = nullptr;

    zlink::framework::zlink_builder_t builder;
    auto mesh = builder.add_route_mesh (
      "entry-relocation-restore-mesh");
    mesh.add_entry_spot<entry_relocation_test_entry_spot_t> (
      [] (zlink::framework::entry_spot_context_t context) {
          return std::make_shared<entry_relocation_test_entry_spot_t> (
            std::move (context));
      });
    mesh.add_actor_factory<
      entry_relocation_test_actor_t,
      entry_relocation_test_actor_factory_t> (
      "entry-relocation-test-actor",
      std::make_shared<entry_relocation_test_actor_factory_t> (),
      [] (auto &factory) { factory.recreate_on_relocation (); });

    auto found_runtime =
      zlink::framework::detail::spot_node_runtime_t::from (
        builder, "entry-relocation-restore-mesh");
    test.require (
      found_runtime.has_value (),
      "entry Spot relocation restore test must resolve its Spot runtime");
    if (!found_runtime)
        return;
    auto runtime = *found_runtime;
    const auto entry_created = runtime.create_spot ("entry");
    test.require (
      entry_created.state
        == zlink::framework::spot_create_state_t::created,
      "the fixture must create its own local Entry Spot before restore");

    const object_ref_t source_actor{
      .kind = object_kind_t::actor,
      .key = "entry-relocation-test-actor-id",
      .object_generation = 1,
      .authority_owner_generation = 1,
      .mesh_name = "mesh",
      .node_id = "source"};
    const frozen_object_state_t frozen{
      .owner = source_actor,
      .stable_type = "entry-relocation-test-actor",
      .application_state = {},
      .pending_application = {},
      .timers = {}};
    const object_ref_t target_actor{
      .kind = object_kind_t::actor,
      .key = "entry-relocation-test-actor-id",
      .object_generation = 1,
      .authority_owner_generation = 2,
      .mesh_name = "mesh",
      .node_id = "target"};

    const auto materialized = runtime.materialize_relocation_state (
      frozen, target_actor, std::nullopt, {});
    test.require (
      materialized && entry_relocation_test_actor_t::create_count == 1
        && entry_relocation_test_entry_spot_t::joined_count == 1,
      "a standalone Actor relocation unit (target_spot=nullopt) must "
      "resolve the target node's own local Entry Spot and join the "
      "relocated Actor into it, per spec 28");

    test.require (
      entry_relocation_test_entry_spot_t::joined_spot != nullptr
        && entry_relocation_test_entry_spot_t::joined_actor != nullptr,
      "the relocated Actor must be reachable through the target Entry "
      "Spot instance");
    if (entry_relocation_test_entry_spot_t::joined_spot
        && entry_relocation_test_entry_spot_t::joined_actor) {
        zlink::framework::service_provider_t services;
        zlink::framework::serializer_registry_t serializers;
        const auto served =
          entry_relocation_test_entry_spot_t::joined_spot->context ()
            .handlers ()
            .invoke_actor_packet (
              "entry-relocation-test-message",
              *entry_relocation_test_entry_spot_t::joined_spot,
              *entry_relocation_test_entry_spot_t::joined_actor, services,
              serializers,
              zlink::message_t::from (R"({"value":42})"));
        test.require (
          served
            && entry_relocation_test_entry_spot_t::message_served_count
                 == 1
            && entry_relocation_test_entry_spot_t::last_message_value
                 == 42,
          "the relocated Actor must serve an application message through "
          "its target Entry Spot immediately after restore");
    }

    runtime.request_stop ();
    runtime.cancel_pending_dispatch ();
    runtime.cancel_pending_work ();
    runtime.release_native_handles ();
}

/* Negative counterpart: a genuinely spot-less arrival (no local Entry Spot
 * resolvable on the target node) must still fail explicitly, never crash. */
void test_entry_spot_actor_relocation_restore_fails_without_local_entry_spot (
  test_context_t &test)
{
    entry_relocation_test_actor_t::create_count = 0;

    zlink::framework::spot_node_builder_t builder;
    builder.add_actor_factory<
      entry_relocation_test_actor_t,
      entry_relocation_test_actor_factory_t> (
      "entry-relocation-test-actor",
      std::make_shared<entry_relocation_test_actor_factory_t> (),
      [] (auto &factory) { factory.recreate_on_relocation (); });

    auto runtime =
      zlink::framework::detail::spot_node_runtime_t::from (builder);
    /* Deliberately no add_entry_spot / create_spot ("entry"): the target
       node has no local Entry Spot to resolve. */

    const object_ref_t source_actor{
      .kind = object_kind_t::actor,
      .key = "entry-relocation-test-actor-id",
      .object_generation = 1,
      .authority_owner_generation = 1,
      .mesh_name = "mesh",
      .node_id = "source"};
    const frozen_object_state_t frozen{
      .owner = source_actor,
      .stable_type = "entry-relocation-test-actor",
      .application_state = {},
      .pending_application = {},
      .timers = {}};
    const object_ref_t target_actor{
      .kind = object_kind_t::actor,
      .key = "entry-relocation-test-actor-id",
      .object_generation = 1,
      .authority_owner_generation = 2,
      .mesh_name = "mesh",
      .node_id = "target"};

    const auto materialized = runtime.materialize_relocation_state (
      frozen, target_actor, std::nullopt, {});
    test.require (
      !materialized && entry_relocation_test_actor_t::create_count == 0,
      "a standalone Actor relocation unit must fail explicitly (not "
      "crash) when the target node has no local Entry Spot to resolve");

    runtime.request_stop ();
    runtime.cancel_pending_dispatch ();
    runtime.cancel_pending_work ();
    runtime.release_native_handles ();
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

    stateful_object_runtime_t multipart_accounting (
      2, 1, fixed + 4, limits::control_mailbox_bytes);
    const auto multipart_actor = create_actor (
      multipart_accounting, "multipart-accounting-actor");
    test.require (
      multipart_accounting.enqueue (
        multipart_actor, turn_domain_t::application,
        {1, std::vector<std::uint8_t> (128, 0x43), 4})
        == stateful_error_t::none,
      "application HWM must exclude the canonical multipart envelope");
    const auto [multipart_claim_error, multipart_claim] =
      multipart_accounting.try_claim (
        multipart_actor, turn_domain_t::application);
    test.require (
      multipart_claim_error == stateful_error_t::none && multipart_claim,
      "multipart accounting test must claim the admitted application turn");
    test.require (
      multipart_accounting.enqueue (
        multipart_actor, turn_domain_t::application, {2, {}})
        == stateful_error_t::backpressured,
      "application HWM must retain the payload-part reservation while active");
    if (multipart_claim) {
        test.require (
          multipart_accounting.complete_claim (
            multipart_actor, turn_domain_t::application)
            == stateful_error_t::none,
          "multipart payload reservation must release at completion");
    }

    stateful_object_runtime_t progressive_restore (
      1, 1, fixed + 1, limits::control_mailbox_bytes);
    bool materialized = false;
    bool lifecycle_committed = false;
    bool lifecycle_saw_closed_dispatch = false;
    bool relocation_aborted = false;
    object_ref_t restore_target{
      object_kind_t::actor,
      "progressive-restore-actor",
      1,
      2,
      "mesh",
      "node-b"};
    progressive_restore.configure_relocation_materialization (
      [&] (const frozen_object_state_t &, const object_ref_t &target,
           const std::optional<object_ref_t> &parent, std::stop_token) {
          materialized = target == restore_target && !parent;
          return materialized;
      },
      [&] (const std::vector<object_ref_t> &targets) {
          lifecycle_committed = targets == std::vector<object_ref_t>{restore_target};
          const auto [claim_error, claimed] = progressive_restore.try_claim (
            restore_target, turn_domain_t::application);
          lifecycle_saw_closed_dispatch =
            claim_error == stateful_error_t::moving && !claimed;
          return lifecycle_committed && lifecycle_saw_closed_dispatch;
      },
      [&] (const std::vector<object_ref_t> &) { relocation_aborted = true; });
    const relocation_restore_identity_t restore_identity{
      "restore-byte-root", 1, digest_with (0x61)};
    const frozen_object_state_t frozen{
      .owner =
        {.kind = object_kind_t::actor,
         .key = restore_target.key,
         .object_generation = restore_target.object_generation,
         .authority_owner_generation = 1,
         .mesh_name = restore_target.mesh_name,
         .node_id = "node-a"},
      .stable_type = "actor",
      .application_state = {},
      .pending_application = {
        {1, std::vector<std::uint8_t> (4, 0x41)},
        {2, std::vector<std::uint8_t> (4, 0x42)},
        {3, std::vector<std::uint8_t> (4, 0x43)}},
      .timers = {}};
    const auto restored = progressive_restore.restore_relocation (
      frozen, restore_target, restore_identity);
    stateful_error_t relay_ingress = stateful_error_t::conflict;
    stateful_error_t temporary_ingress = stateful_error_t::conflict;
    stateful_error_t pre_commit_claim_error = stateful_error_t::conflict;
    std::optional<turn_record_t> pre_commit_claim;
    stateful_error_t committed = stateful_error_t::conflict;
    std::size_t backlog_records_before_commit = 0;
    std::size_t backlog_bytes_before_commit = 0;
    bool progressive_fifo = false;
    std::size_t handler_turns = 0;
    if (restored == stateful_error_t::none) {
        relay_ingress = progressive_restore.enqueue (
          restore_target, turn_domain_t::application, {4, {0x44}});
        temporary_ingress = progressive_restore.enqueue (
          restore_target, turn_domain_t::application, {5, {0x45}});
        backlog_records_before_commit = progressive_restore.pending (
          restore_target, turn_domain_t::application);
        backlog_bytes_before_commit = progressive_restore.pending_bytes (
          restore_target, turn_domain_t::application);
        std::tie (pre_commit_claim_error, pre_commit_claim) =
          progressive_restore.try_claim (
            restore_target, turn_domain_t::application);
        committed = progressive_restore.commit_relocation_restore (
          restore_target, restore_identity);
        progressive_fifo = committed == stateful_error_t::none;
        for (std::uint64_t sequence = 1;
             progressive_fifo && sequence <= 5; ++sequence) {
            const auto [claim_error, claimed] =
              progressive_restore.try_claim (
                restore_target, turn_domain_t::application);
            const auto [concurrent_error, concurrent] =
              progressive_restore.try_claim (
                restore_target, turn_domain_t::application);
            progressive_fifo =
              claim_error == stateful_error_t::none && claimed
              && claimed->sequence == sequence
              && concurrent_error == stateful_error_t::none && !concurrent;
            if (claimed) {
                ++handler_turns;
                progressive_fifo =
                  progressive_restore.complete_claim (
                    restore_target, turn_domain_t::application)
                    == stateful_error_t::none
                  && progressive_fifo;
            }
        }
    }
    test.require (
      restored == stateful_error_t::none
        && materialized
        && relay_ingress == stateful_error_t::none
        && temporary_ingress == stateful_error_t::none
        && backlog_records_before_commit == 5
        && backlog_bytes_before_commit > fixed + 1
        && pre_commit_claim_error == stateful_error_t::moving
        && !pre_commit_claim
        && progressive_restore.pending_bytes (
             restore_target, turn_domain_t::application)
             == 0
        && lifecycle_committed
        && lifecycle_saw_closed_dispatch
        && !relocation_aborted
        && progressive_fifo
        && handler_turns == 5,
      "relocation must retain a saved/relay/temporary durable backlog beyond "
      "live count and byte limits, keep handlers closed through lifecycle, "
      "then admit one FIFO turn at a time");

    stateful_object_runtime_t aggregate_restore (
      1, 1, fixed + 1, limits::control_mailbox_bytes);
    std::size_t aggregate_materialized = 0;
    std::size_t aggregate_commit_count = 0;
    aggregate_restore.configure_relocation_materialization (
      [&] (const frozen_object_state_t &, const object_ref_t &,
           const std::optional<object_ref_t> &, std::stop_token) {
          ++aggregate_materialized;
          return true;
      },
      [&] (const std::vector<object_ref_t> &) {
          ++aggregate_commit_count;
          return true;
      },
      [] (const std::vector<object_ref_t> &) {});
    std::vector<frozen_object_state_t> aggregate_frozen{
      {.owner =
         {.kind = object_kind_t::user_spot,
          .key = "progressive-aggregate-spot",
          .object_generation = 1,
          .authority_owner_generation = 1,
          .mesh_name = "mesh",
          .node_id = "node-a"},
       .stable_type = "spot",
       .application_state = {},
       .pending_application = {{1, {0x51}}, {2, {0x52}}, {3, {0x53}}},
       .timers = {}},
      {.owner =
         {.kind = object_kind_t::actor,
          .key = "progressive-aggregate-actor",
          .object_generation = 1,
          .authority_owner_generation = 1,
          .mesh_name = "mesh",
          .node_id = "node-a"},
       .stable_type = "actor",
       .application_state = {},
       .pending_application = {{1, {0x61}}, {2, {0x62}}, {3, {0x63}}},
       .timers = {}}};
    std::vector<object_ref_t> aggregate_targets{
      aggregate_frozen[0].owner, aggregate_frozen[1].owner};
    for (auto &target : aggregate_targets) {
        target.authority_owner_generation = 2;
        target.node_id = "node-b";
    }
    const relocation_restore_identity_t aggregate_identity{
      "aggregate-progressive-root", 2, digest_with (0x62)};
    const auto aggregate_restored =
      aggregate_restore.restore_relocation_aggregate (
        aggregate_frozen, aggregate_targets, aggregate_identity);
    const auto aggregate_committed =
      aggregate_restored == stateful_error_t::none
        ? aggregate_restore.commit_relocation_restore_aggregate (
            aggregate_targets, aggregate_identity)
        : stateful_error_t::conflict;
    bool aggregate_fifo =
      aggregate_committed == stateful_error_t::none;
    for (const auto &target : aggregate_targets) {
        for (std::uint64_t sequence = 1;
             aggregate_fifo && sequence <= 3; ++sequence) {
            const auto [claim_error, claimed] = aggregate_restore.try_claim (
              target, turn_domain_t::application);
            aggregate_fifo =
              claim_error == stateful_error_t::none && claimed
              && claimed->sequence == sequence;
            if (claimed) {
                aggregate_fifo =
                  aggregate_restore.complete_claim (
                    target, turn_domain_t::application)
                    == stateful_error_t::none
                  && aggregate_fifo;
            }
        }
    }
    test.require (
      aggregate_restored == stateful_error_t::none
        && aggregate_materialized == 2
        && aggregate_commit_count == 1
        && aggregate_fifo,
      "aggregate relocation must restore and progressively drain every "
      "participant backlog beyond the live count and byte limits");
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

    const auto [error, seal] = await_task (objects.try_seal_relocation_aggregate (
      {first, second}));
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

void test_relocation_hold_restores_without_dedicated_limits (
  test_context_t &test)
{
    namespace limits = zlink::framework::runtime::dispatch_limits;
    namespace protocol = zlink::framework::runtime::protocol;

    // A bound-session request that arrives while the object is sealed for
    // relocation enters the ingress hold. When the hold merges back, the
    // record must travel with its canonical bytes unmodified so the session
    // fence survives the post-seal relay.
    const auto bound_session_bytes = [] {
        const std::vector<std::uint8_t> session_rid{'h', 'e', 'l', 'd'};
        const std::vector<std::uint8_t> node_rid{'s', 'r', 'c'};
        const auto multipart = [] (const std::vector<std::uint8_t> &part) {
            std::vector<std::uint8_t> encoded;
            const auto append_u32 = [&] (std::uint32_t value) {
                encoded.push_back ((value >> 24u) & 0xffu);
                encoded.push_back ((value >> 16u) & 0xffu);
                encoded.push_back ((value >> 8u) & 0xffu);
                encoded.push_back (value & 0xffu);
            };
            append_u32 (1);
            append_u32 (static_cast<std::uint32_t> (part.size ()));
            encoded.insert (encoded.end (), part.begin (), part.end ());
            return encoded;
        };
        protocol::frozen_application_record_t held;
        held.kind = protocol::frozen_record_kind_t::actor_request;
        held.source_kind = protocol::frozen_source_kind_t::bound_session;
        held.source = {"request-owner", 7, node_rid, 1};
        held.source_actor = std::make_pair (std::string ("held-actor"), 5u);
        held.source_session_routing_id = session_rid;
        held.source_binding_generation = 5;
        held.source_session_sequence = 9;
        held.operation = {0x991, 0x992};
        held.operation_kind = 4;
        held.reply_route_id = 61;
        held.body = protocol::frozen_actor_application_body_t{
          {"held-actor", 5, node_rid, 1, 1, 19},
          {protocol::framework_multipart_packet_name,
           protocol::framework_multipart_content_type,
           multipart ({'h', 'e', 'l', 'd'})}};
        return protocol::encode_frozen_record (
          protocol::encode_frozen_application_record (held));
    } ();
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
        auto result = await_task (failed_capture.try_seal_relocation_aggregate (
          {failed_spot}));
        capture_error = result.error;
        capture_seal = std::move (result.seal);
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
        failed_spot, turn_domain_t::application,
        {3, bound_session_bytes})
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
    {
        bool tail_intact = false;
        if (second && second->payload == bound_session_bytes) {
            const auto decoded =
              protocol::decode_frozen_record (second->payload);
            tail_intact =
              decoded.source_kind
                == protocol::frozen_source_kind_t::bound_session
              && decoded.source_session_routing_id
              && *decoded.source_session_routing_id
                   == std::vector<std::uint8_t>{'h', 'e', 'l', 'd'}
              && decoded.source_binding_generation == 5
              && decoded.source_session_sequence == 9
              && decoded.source_actor
              && decoded.source_actor->first == "held-actor"
              && decoded.source_actor->second == 5;
        }
        test.require (
          tail_intact,
          "held bound-session records must merge with the session tail "
          "intact");
    }
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
        auto result = await_task (count_limited.try_seal_relocation_aggregate (
          {count_first, count_second}));
        count_error = result.error;
        count_seal = std::move (result.seal);
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
    for (std::uint64_t sequence = 1; sequence <= 600; ++sequence) {
        count_records_accepted =
          count_limited.enqueue (
            count_first, turn_domain_t::application, {sequence, {}})
          == stateful_error_t::none
          && count_records_accepted;
        count_records_accepted =
          count_limited.enqueue (
            count_second, turn_domain_t::application,
            {3000 + sequence, {}})
          == stateful_error_t::none
          && count_records_accepted;
    }
    test.require (
      count_records_accepted
        && count_limited.pending (
             count_first, turn_domain_t::application)
             + count_limited.pending (
                 count_second, turn_domain_t::application)
             == 1200,
      "relocation hold must accept records beyond the former aggregate "
      "1,024-record limit");
    for (std::uint64_t sequence = 601; sequence <= 2049; ++sequence) {
        count_records_accepted =
          count_limited.enqueue (
            count_first, turn_domain_t::application, {sequence, {}})
          == stateful_error_t::none
          && count_records_accepted;
    }
    test.require (
      count_records_accepted
        && count_limited.pending (
             count_first, turn_domain_t::application) == 2049,
      "source relocation ingress must remain held beyond the configured "
      "application-lane record capacity");
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
    bool source_abort_fifo = count_error == stateful_error_t::none;
    for (std::uint64_t sequence = 1; sequence <= 2049; ++sequence) {
        const auto [claim_error, claimed] = count_limited.try_claim (
          count_first, turn_domain_t::application);
        source_abort_fifo = source_abort_fifo
          && claim_error == stateful_error_t::none && claimed
          && claimed->sequence == sequence;
        if (claimed) {
            source_abort_fifo =
              count_limited.complete_claim (
                count_first, turn_domain_t::application)
                == stateful_error_t::none
              && source_abort_fifo;
        }
    }
    test.require (
      source_abort_fifo,
      "source relocation abort must restore an over-capacity hold in FIFO order");

    stateful_object_runtime_t byte_limited (
      4096, 8, 20u * 1024u * 1024u, limits::control_mailbox_bytes);
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
        auto result = await_task (byte_limited.try_seal_relocation_aggregate (
          {byte_first, byte_second}));
        byte_error = result.error;
        byte_seal = std::move (result.seal);
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
      "relocation hold byte setup must retain 16 MiB across participants");
    const auto former_bound_excess_payload_bytes =
      1u * 1024u * 1024u - limits::fixed_work_byte_cost;
    test.require (
      byte_limited.enqueue (
        byte_first, turn_domain_t::application,
        {2, std::vector<std::uint8_t> (
              former_bound_excess_payload_bytes, 0x43)})
        == stateful_error_t::none,
      "relocation hold must accept bytes beyond the former aggregate "
      "16 MiB limit");
    const auto application_capacity_fill_payload_bytes =
      11u * 1024u * 1024u
      - 2u * limits::fixed_work_byte_cost;
    test.require (
      byte_limited.enqueue (
        byte_first, turn_domain_t::application,
        {3, std::vector<std::uint8_t> (
              application_capacity_fill_payload_bytes, 0x44)})
          == stateful_error_t::none
        && byte_limited.enqueue (
             byte_first, turn_domain_t::application, {4, {}})
             == stateful_error_t::none
        && byte_limited.enqueue (
             byte_first, turn_domain_t::application, {5, {}})
             == stateful_error_t::none
        && byte_limited.pending_bytes (
             byte_first, turn_domain_t::application)
             > 20u * 1024u * 1024u,
      "source relocation ingress must remain held beyond the configured "
      "application-lane byte capacity");
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

    stateful_object_runtime_t target_hold (
      2, 8, limits::fixed_work_byte_cost + 1,
      limits::control_mailbox_bytes);
    std::mutex target_mutex;
    std::condition_variable target_condition;
    bool target_restore_entered = false;
    bool release_target_restore = false;
    target_hold.configure_relocation_state (
      [] (const object_ref_t &, const std::string &, std::stop_token) {
          return std::vector<std::uint8_t>{};
      },
      [&] (const frozen_object_state_t &, const object_ref_t &,
           std::stop_token) {
          std::unique_lock lock (target_mutex);
          target_restore_entered = true;
          target_condition.notify_all ();
          target_condition.wait (
            lock, [&] { return release_target_restore; });
          return true;
      });
    const frozen_object_state_t target_frozen{
      .owner =
        {.kind = object_kind_t::user_spot,
         .key = "target-unbounded-hold",
         .object_generation = 1,
         .authority_owner_generation = 1,
         .mesh_name = "mesh",
         .node_id = "source"},
      .stable_type = "spot",
      .application_state = {},
      .pending_application = {},
      .timers = {}};
    auto target_ref = target_frozen.owner;
    target_ref.authority_owner_generation = 2;
    target_ref.node_id = "target";
    const relocation_restore_identity_t target_identity{
      "target-unbounded-root", 7, digest_with (0x71)};
    stateful_error_t target_restore_result = stateful_error_t::conflict;
    std::thread target_restoring ([&] {
        target_restore_result = target_hold.restore_relocation (
          target_frozen, target_ref, target_identity);
    });
    {
        std::unique_lock lock (target_mutex);
        test.require (
          target_condition.wait_for (
            lock, std::chrono::seconds (5), [&] {
                return target_restore_entered;
            }),
          "target temporary hold test must enter restore before ingress");
    }
    bool target_records_held =
      target_hold.enqueue (
        target_ref, turn_domain_t::application,
        {1, std::vector<std::uint8_t> (
              17u * 1024u * 1024u, 0x51)})
      == stateful_error_t::none;
    for (std::uint64_t sequence = 2; sequence <= 1025; ++sequence) {
        target_records_held =
          target_hold.enqueue (
            target_ref, turn_domain_t::application,
            {sequence, {0x52}})
            == stateful_error_t::none
          && target_records_held;
    }
    {
        std::lock_guard lock (target_mutex);
        release_target_restore = true;
    }
    target_condition.notify_all ();
    target_restoring.join ();
    test.require (
      target_records_held
        && target_restore_result == stateful_error_t::none
        && target_hold.pending (target_ref, turn_domain_t::application)
             == 1025
        && target_hold.pending_bytes (
             target_ref, turn_domain_t::application)
             > 16u * 1024u * 1024u
        && target_hold.commit_relocation_restore (
             target_ref, target_identity) == stateful_error_t::none,
      "target relocation ingress must remain held beyond the configured lane "
      "and former 1,024-record/16 MiB temporary limits");
    for (std::uint64_t sequence = 1; sequence <= 1025; ++sequence) {
        const auto [claim_error, claimed] = target_hold.try_claim (
          target_ref, turn_domain_t::application);
        test.require (
          claim_error == stateful_error_t::none && claimed
            && claimed->sequence == sequence,
          "target relocation hold must resume in FIFO order after commit");
        if (claimed) {
            (void) target_hold.complete_claim (
              target_ref, turn_domain_t::application);
        }
    }

    stateful_object_runtime_t target_abort (
      1, 8, limits::fixed_work_byte_cost + 1,
      limits::control_mailbox_bytes);
    std::mutex abort_mutex;
    std::condition_variable abort_condition;
    bool abort_restore_entered = false;
    bool release_abort_restore = false;
    target_abort.configure_relocation_state (
      [] (const object_ref_t &, const std::string &, std::stop_token) {
          return std::vector<std::uint8_t>{};
      },
      [&] (const frozen_object_state_t &, const object_ref_t &,
           std::stop_token) {
          std::unique_lock lock (abort_mutex);
          abort_restore_entered = true;
          abort_condition.notify_all ();
          abort_condition.wait (
            lock, [&] { return release_abort_restore; });
          return true;
      });
    auto abort_frozen = target_frozen;
    abort_frozen.owner.key = "target-abort-hold";
    auto abort_target = abort_frozen.owner;
    abort_target.authority_owner_generation = 2;
    abort_target.node_id = "target";
    const relocation_restore_identity_t abort_identity{
      "target-abort-root", 8, digest_with (0x72)};
    stateful_error_t abort_restore_result = stateful_error_t::conflict;
    std::thread abort_restoring ([&] {
        abort_restore_result = target_abort.restore_relocation (
          abort_frozen, abort_target, abort_identity);
    });
    {
        std::unique_lock lock (abort_mutex);
        test.require (
          abort_condition.wait_for (
            lock, std::chrono::seconds (5), [&] {
                return abort_restore_entered;
            }),
          "target abort test must enter restore before ingress");
    }
    test.require (
      target_abort.enqueue (
        abort_target, turn_domain_t::application, {1, {0x71}})
          == stateful_error_t::none
        && target_abort.enqueue (
             abort_target, turn_domain_t::application, {2, {0x72}})
             == stateful_error_t::none,
      "target abort test must hold records beyond its normal lane");
    {
        std::lock_guard lock (abort_mutex);
        release_abort_restore = true;
    }
    abort_condition.notify_all ();
    abort_restoring.join ();
    test.require (
      abort_restore_result == stateful_error_t::none
        && target_abort.abort_relocation_restore (
             abort_target, abort_identity) == stateful_error_t::none
        && !target_abort.find (
             abort_target.kind, abort_target.key),
      "target relocation abort must discard its temporary hold and object reservation");
    target_abort.configure_relocation_state (
      [] (const object_ref_t &, const std::string &, std::stop_token) {
          return std::vector<std::uint8_t>{};
      },
      [] (const frozen_object_state_t &, const object_ref_t &,
          std::stop_token) { return true; });
    test.require (
      target_abort.restore_relocation (
        abort_frozen, abort_target, abort_identity)
          == stateful_error_t::none
        && target_abort.commit_relocation_restore (
             abort_target, abort_identity) == stateful_error_t::none
        && target_abort.pending (
             abort_target, turn_domain_t::application) == 0,
      "target relocation abort must leave no held records in a retried restore");

    stateful_object_runtime_t normal_lane_caps (
      2, 8, 2u * limits::fixed_work_byte_cost + 4,
      limits::control_mailbox_bytes);
    const auto membership_capped = create_actor (
      normal_lane_caps, "membership-normal-cap");
    const object_ref_t remote_membership_target{
      object_kind_t::user_spot, "membership-normal-target", 1, 1,
      "mesh", "node-b"};
    const auto [normal_move_error, normal_move] =
      normal_lane_caps.begin_remote_membership_move (
        membership_capped, remote_membership_target);
    test.require (
      normal_move_error == stateful_error_t::none
        && normal_lane_caps.enqueue (
             membership_capped, turn_domain_t::application,
             {99, std::vector<std::uint8_t> (
                    2u * limits::fixed_work_byte_cost + 5, 0x61)})
             == stateful_error_t::backpressured
        && normal_lane_caps.enqueue (
             membership_capped, turn_domain_t::application, {1, {}})
             == stateful_error_t::none
        && normal_lane_caps.enqueue (
             membership_capped, turn_domain_t::application, {2, {}})
             == stateful_error_t::none
        && normal_lane_caps.enqueue (
             membership_capped, turn_domain_t::application, {3, {}})
             == stateful_error_t::backpressured,
      "membership-only movement must retain the normal application count and byte caps");
    if (normal_move_error == stateful_error_t::none)
        (void) normal_lane_caps.abort_membership_move (normal_move);

    const auto closing_capped = create_spot (
      normal_lane_caps, object_kind_t::user_spot, "closing-normal-cap");
    const auto [close_error, close_token] =
      normal_lane_caps.begin_close_spot (closing_capped);
    test.require (
      close_error == stateful_error_t::none && close_token
        && normal_lane_caps.enqueue (
             closing_capped, turn_domain_t::application,
             {99, std::vector<std::uint8_t> (
                    2u * limits::fixed_work_byte_cost + 5, 0x62)})
             == stateful_error_t::backpressured
        && normal_lane_caps.enqueue (
             closing_capped, turn_domain_t::application, {1, {}})
             == stateful_error_t::none
        && normal_lane_caps.enqueue (
             closing_capped, turn_domain_t::application, {2, {}})
             == stateful_error_t::none
        && normal_lane_caps.enqueue (
             closing_capped, turn_domain_t::application, {3, {}})
             == stateful_error_t::backpressured,
      "closing objects must retain the normal application count and byte caps");
    if (close_token)
        (void) normal_lane_caps.abort_close_spot (*close_token);
}

void test_advertised_receive_chunk_limit_wiring (test_context_t &test)
{
    namespace spots = zlink::framework::detail;

    /* Target advertises a fixed 32768-byte inbound cap on every actor
     * join reply. */
    test.require (
      spots::spot_actor_join_advertised_receive_chunk_limit_bytes == 32768,
      "join reply must advertise a 32768-byte receive chunk limit");

    /* Round-trip: to_json/from_json preserve receiveChunkLimitBytes. */
    {
        spots::spot_actor_join_route_reply_t reply;
        reply.result_code = 0;
        reply.actor_node_rid = "node-a";
        reply.actor_type = "demo.actor";
        reply.actor_id = "actor-1";
        reply.actor_generation = 7;
        reply.payload = {1, 2, 3};
        reply.receive_chunk_limit_bytes =
          spots::spot_actor_join_advertised_receive_chunk_limit_bytes;

        nlohmann::json encoded = reply;
        const auto decoded = encoded.get<spots::spot_actor_join_route_reply_t> ();
        test.require (
          decoded.receive_chunk_limit_bytes == 32768,
          "join reply round-trip must preserve receiveChunkLimitBytes");
        test.require (
          decoded.actor_id == "actor-1" && decoded.actor_generation == 7,
          "join reply round-trip must preserve unrelated fields");
    }

    /* Tolerant decode: a peer on an older schema omits the field
     * entirely; absence must decode to 0, not throw. */
    {
        nlohmann::json legacy{{"resultCode", 0},
                              {"actorNodeRid", "node-a"},
                              {"actorType", "demo.actor"},
                              {"actorId", "actor-1"},
                              {"actorGeneration", 7},
                              {"payload", ""}};
        const auto decoded =
          legacy.get<spots::spot_actor_join_route_reply_t> ();
        test.require (
          decoded.receive_chunk_limit_bytes == 0,
          "legacy join reply missing receiveChunkLimitBytes must decode as 0 (not advertised)");
    }

    /* Source-side consumption: min(configured, advertised>0?advertised:local)
     * applied to the direct-transfer chunk-plan cap. */
    test.require (
      maintenance_runtime_t::apply_advertised_receive_chunk_limit (
        262144, 0)
        == 262144,
      "advertised=0 (not advertised) must leave the local chunk cap unchanged");
    test.require (
      maintenance_runtime_t::apply_advertised_receive_chunk_limit (
        262144, 32768)
        == 32768,
      "a smaller advertised cap must win over a larger local budget");
    test.require (
      maintenance_runtime_t::apply_advertised_receive_chunk_limit (
        16384, 32768)
        == 16384,
      "a smaller local budget must still win over a larger advertised cap");

    /* Chunk-split proof: the same payload plans into more chunks once
     * capped by a smaller advertised limit. */
    const std::vector<std::uint8_t> payload (20000, 0x5a);
    const auto uncapped_limit =
      maintenance_runtime_t::apply_advertised_receive_chunk_limit (
        262144, 0);
    const auto capped_limit =
      maintenance_runtime_t::apply_advertised_receive_chunk_limit (
        262144, 8192);
    const auto chunk_count = [] (std::size_t total, std::uint64_t limit) {
        return limit == 0 ? std::size_t{0}
                          : (total + limit - 1) / limit;
    };
    test.require (
      chunk_count (payload.size (), uncapped_limit) == 1
        && chunk_count (payload.size (), capped_limit) == 3,
      "an advertised cap smaller than the local budget must split the same payload into more chunks");
}

// C-5 increment 2b (originate opt-in reverted b/49b6c-follow-up): the
// actorJoin(28) originate fence-gate (mesh_node_runtime_t::
// observe_spot_authority / admit_remote_application_actor_join_via_wire's
// caller in join_application_actor_to_spot) is a dormant mechanism. C++ does
// NOT originate a live cross-node actorJoin(28) per spec 51 §9 ("C++ and .NET
// ... do not originate a cross-node actorJoin operation"); the production
// join_application_actor_to_spot remote branch does NOT call
// observe_spot_authority, so observed_spot_authority always returns nullopt
// there and the branch falls through to the JSON admission path. Activating
// the opt-in (1b3b21b2e3, reverted) took an incomplete canonical receiver and
// broke ST-B1; completing that receiver is H-12/H-15/H-4a. This test still
// proves the gate MECHANISM is real (not dead code) "for whichever future
// caller opts a peer in": it directly calls observe_spot_authority and reads
// the observation back. It does not exercise
// join_application_actor_to_spot directly (the full remote-join call chain
// fails deep inside completion delivery for a synthetic, never-locally-
// created Actor regardless of which path is taken, and so cannot
// discriminate between them); it proves the gate mechanism itself directly
// -- a freshly started node's cache has no entry for a Spot address
// nothing has observed, so the exact lookup join_application_actor_to_spot
// performs returns nullopt, and after an explicit observe_spot_authority
// call with a fully valid, nonzero fence (mirroring what the production
// call site now does with a resolved spot_address_t), that same lookup
// returns exactly what was recorded -- this is the one piece of state
// the gate actually branches on.
void test_actor_join_wire_gate_records_target_authority (
  test_context_t &test)
{
    namespace detail = zlink::framework::detail;

    const auto core_context = std::make_shared<zlink::context_t> ();
    auto state = std::make_shared<detail::mesh_node_builder_state_t> (
      "actor-join-gate-mesh");
    state->core_context = core_context;
    state->listen_endpoint = "tcp://127.0.0.1:0";
    state->routing_id = zlink::routing_id_t::from ("gate-node");
    detail::mesh_node_runtime_t node (state);
    node.start ();

    const auto target_rid = zlink::routing_id_t::from ("gate-target");
    test.require (
      !node.observed_spot_authority (target_rid, "gate-spot", 5).has_value (),
      "a freshly started node's cache has no entry for a Spot address "
      "nothing has observed or joined yet");

    node.observe_spot_authority (target_rid, "gate-spot", 5, 9, 7, 8);
    const auto observed = node.observed_spot_authority (target_rid, "gate-spot", 5);
    test.require (
      observed && observed->target_node_generation == 9
        && observed->authority_owner_generation == 7
        && observed->owner_lease_generation == 8,
      "an explicit observe_spot_authority call must be readable back "
      "exactly -- proving the gate mechanism itself is real, not dead code, "
      "for whichever future caller opts a peer in");

    // A stale/mismatched key (wrong object generation) must not match --
    // the cache key is the full (node, Spot, generation) identity.
    test.require (
      !node.observed_spot_authority (target_rid, "gate-spot", 6).has_value (),
      "a different object generation must not read back an unrelated "
      "observation");

    node.stop ();
}

int main ()
{
    test_context_t test;
    test_spot_lifecycle_domain_rejects_invalid_kind_combinations (test);
    test_generation_barrier_quiesces_yield_spot_and_timer (test);
    test_relocation_ready_completion_runs_once_on_spot_turn (test);
    test_actor_leave_after_relocation_defer_runs_lifecycle_callbacks (test);
    test_temporary_channel_request_yield_owns_call_state (test);
    test_accepted_message_payload_is_deserialized_once (test);
    test_close_barrier_waits_and_abort_restores_ingress (test);
    test_envelope_round_trip (test);
    test_actor_join_recovery_round_trip (test);
    test_spot_restore_stages_before_publication (test);
    test_concurrent_spot_restore_owns_one_reservation (test);
    test_restore_validates_generation_before_spot_publication (test);
    test_pending_restore_holds_ingress_before_rollback (test);
    test_host_preflight_is_all_or_none (test);
    test_shutdown_wins_during_retire_preflight (test);
    test_public_relocation_store_adapter (test);
    test_public_authority_store_adapter (test);
    test_application_relocation_remote_production_path (test);
    test_application_user_spot_aggregate_remote_production_path (
      test);
    test_aggregate_seal_failure_preserves_earlier_application_work (test);
    test_relocation_adapter_single_capture_restore_path (test);
    test_entry_spot_actor_relocation_restore_resolves_local_entry_spot (
      test);
    test_entry_spot_actor_relocation_restore_fails_without_local_entry_spot (
      test);
    test_relocation_hold_restores_without_dedicated_limits (test);
    test_stateful_application_reservation_includes_active_work (test);
    test_advertised_receive_chunk_limit_wiring (test);
    test_actor_join_wire_gate_records_target_authority (test);
    return test.failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
