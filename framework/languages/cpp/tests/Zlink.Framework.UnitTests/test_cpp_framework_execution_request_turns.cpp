/* SPDX-License-Identifier: Elastic-2.0 */

#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "../support/request_admission_fixture.hpp"
#include "runtime/mesh/mesh_node_host_service.hpp"
#include "../support/execution_cases.hpp"
#include "../support/execution_wait.hpp"
#include "../support/execution_signal.hpp"
#include "../support/execution_serial_fixture.hpp"
#include <zlink/json.hpp>
#include "../support/runtime_failure_fixture.hpp"
#include "../support/manual_internal_scheduler.hpp"
#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/execution/serial_execution_queue.hpp"
#include "runtime/execution/serial_handler_participant.hpp"
#include "runtime/execution/actor_execution_context.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include "runtime/stateful/public_host_runtime.hpp"
#include "runtime/streams/session_serial_executor.hpp"
#include "../support/scope_exit.hpp"
#include <zlink/framework/contracts/spots/spot.hpp>
#include <zlink/framework/contracts/actors/actor.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

using zlink::framework::test::scope_exit_t;

namespace zlink::framework::test::execution
{

int verify_logical_multicast_observation ()
{
    {
        auto state = zlink::framework::test::runtime_failure_fixture<
          zlink::framework::detail::spot_node_builder_state_t> ("logical-multicast-observation");
        auto observed = std::make_shared<std::atomic_bool> (false);
        zlink::framework::detail::dispatch_options_access_t::set_dispatch_error_observer_for_tests (
          state->dispatch,
          [observed] (const zlink::framework::message_dispatch_error_event_t &event) {
              if (event.surface == zlink::framework::dispatch_error_surface_t::spot_route
                  && event.message_kind == zlink::framework::dispatch_message_kind_t::send
                  && event.reason == zlink::framework::dispatch_error_reason_t::stale_target
                  && event.action == zlink::framework::dispatch_error_action_t::drop
                  && !event.packet_name && event.channel_name && *event.channel_name == "world"
                  && event.mesh_name && *event.mesh_name == "game" && event.target_rid
                  && *event.target_rid == "gone-player" && event.topic
                  && *event.topic == "players") {
                  observed->store (true, std::memory_order_release);
              }
          });
        zlink::framework::detail::report_logical_multicast_failure (
          state, "world", "game", "players", "gone-player",
          zlink::framework::dispatch_error_reason_t::stale_target);
        if (!wait_until ([&] { return observed->load (std::memory_order_acquire); })) {
            return 90;
        }
    }
    return 0;
}

enum class origin_terminal_case_t
{
    success,
    failure,
    cancel,
    reply_encoding
};

struct origin_reply_t
{
};
}

template <> struct zlink::json::dto_traits<zlink::framework::test::execution::origin_reply_t>
{
    static constexpr auto fields = std::tuple{};
};

namespace zlink::framework::test::execution
{
class reentry_probe_actor_client_t final : public zlink::framework::actor_client_t
{
  public:
    std::atomic_int request_calls{0};

  protected:
    zlink::framework::task_t<void>
    send_erased (zlink::framework::actor_id_t,
                 std::string,
                 zlink::framework::message_t,
                 const zlink::framework::actor_send_call_t::metadata_map_t &) override
    {
        return zlink::framework::task_t<void> (zlink::framework::result_t<void>::success ());
    }

    zlink::framework::request_task_t<zlink::framework::message_t>
    request_erased (zlink::framework::actor_id_t,
                    std::string,
                    zlink::framework::message_t,
                    std::optional<std::chrono::milliseconds>,
                    const zlink::framework::actor_request_call_t::metadata_map_t &) override
    {
        ++request_calls;
        return zlink::framework::request_task_t<zlink::framework::message_t> (
          zlink::framework::result_t<zlink::framework::message_t>::failure (
            zlink::framework::framework_error_kind_t::internal_failure,
            "same-Actor request reached transport"));
    }

    zlink::framework::serializer_registry_t &actor_client_serializers () override
    {
        return serializers;
    }

  private:
    zlink::framework::serializer_registry_t serializers;
};

zlink::framework::task_t<void>
run_request_turn_probe (std::shared_ptr<zlink::framework::task_completion_source_t<int>> reply,
                        std::shared_ptr<std::vector<int>> order,
                        std::shared_ptr<std::mutex> order_gate,
                        bool release_turn)
{
    {
        std::lock_guard lock (*order_gate);
        order->push_back (1);
    }
    zlink::framework::request_call_t<int> call (
      "TurnProbe", [reply] (const auto &, auto, const auto &) {
          return zlink::framework::test::accepted_request_task (reply->task ());
      });
    int value = 0;
    if (release_turn)
        value = co_await call.yield ();
    else
        value = co_await call.async ();
    if (value != 7) {
        throw std::runtime_error ("turn probe reply mismatch");
    }
    {
        std::lock_guard lock (*order_gate);
        order->push_back (3);
    }
    co_return;
}

bool verify_request_turn_mode (bool release_turn, const std::vector<int> &expected)
{
    zlink::framework::runtime::offload_executor_t executor (2);
    zlink::framework::runtime::serial_execution_queue_t queue (
      executor, {}, zlink::framework::runtime::serial_execution_queue_t::error_handler_t{},
      zlink::framework::runtime::serial_lane_policy_t::spot_wide ());
    auto reply = std::make_shared<zlink::framework::task_completion_source_t<int>> ();
    auto order = std::make_shared<std::vector<int>> ();
    auto order_gate = std::make_shared<std::mutex> ();
    // Completes the reply the request waits for and drains, also when a check fails early.
    scope_exit_t drain_on_exit ([&] {
        (void) reply->complete (zlink::framework::result_t<int>::success (7));
        queue.drain ();
    });

    queue.post_async ("request", [reply, order, order_gate, release_turn] (auto complete) {
        auto task = std::make_shared<zlink::framework::task_t<void>> (
          run_request_turn_probe (reply, order, order_gate, release_turn));
        zlink::framework::detail::observe_task_completion (
          *task, [task, complete = std::move (complete)] (const auto &result) mutable {
              complete ([task, result] {
                  if (!result) {
                      throw std::runtime_error ("turn probe request failed");
                  }
              });
          });
    });
    queue.post ("sibling", [order, order_gate] {
        std::lock_guard lock (*order_gate);
        order->push_back (2);
    });

    if (!wait_until ([&] {
            std::lock_guard lock (*order_gate);
            return release_turn ? order->size () >= 2 : order->size () == 1;
        })) {
        return false;
    }
    if (!release_turn) {
        std::lock_guard lock (*order_gate);
        if (*order != std::vector<int>{1}) {
            return false;
        }
    }
    reply->complete (zlink::framework::result_t<int>::success (7));
    if (!wait_until ([&] {
            std::lock_guard lock (*order_gate);
            return order->size () == 3;
        })) {
        return false;
    }
    queue.drain ();
    std::lock_guard lock (*order_gate);
    if (*order != expected) {
        std::cerr << "turn mode=" << (release_turn ? "yield" : "async") << " order=";
        for (const auto item : *order) {
            std::cerr << item;
        }
        std::cerr << '\n';
    }
    return *order == expected;
}
struct spot_wide_yield_probe_state_t
{
    void record (std::string event)
    {
        std::lock_guard lock (mutex);
        events.push_back (std::move (event));
    }

    std::mutex mutex;
    std::vector<std::string> events;
    serial_test_signal_t first_started;
    serial_test_signal_t first_finished;
    serial_test_signal_t same_actor_next;
    serial_test_signal_t other_actor;
    serial_test_signal_t spot;
    serial_test_signal_t timer;
    std::atomic_bool failed{false};
};

zlink::framework::task_t<void> run_spot_wide_actor_yield_probe (
  std::shared_ptr<zlink::framework::task_completion_source_t<int>> reply,
  std::shared_ptr<spot_wide_yield_probe_state_t> state)
{
    state->record ("actor-a:start");
    state->first_started.set ();
    zlink::framework::request_call_t<int> call (
      "YieldProbe", [reply] (const auto &, auto, const auto &) {
          return zlink::framework::test::accepted_request_task (reply->task ());
      });
    const auto value = co_await call.yield ();
    if (value != 7)
        throw std::runtime_error ("yield probe reply mismatch");
    state->record ("actor-a:resume");
    co_return;
}

bool verify_spot_wide_yield_retains_actor_claim ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::runtime;

    auto fixture =
      std::make_unique<serial_executor_test_fixture_t> (serial_lane_policy_t::spot_wide ());
    auto reply = std::make_shared<task_completion_source_t<int>> ();
    auto state = std::make_shared<spot_wide_yield_probe_state_t> ();
    // Completes the reply the yielded handler waits for, so the fixture can drain on early returns.
    scope_exit_t release_reply ([&] { (void) reply->complete (result_t<int>::success (7)); });

    const auto first_accepted = fixture->serial.execute_actor (
      "actor-a", "yield-first", [reply, state] (auto complete) mutable {
          auto task =
            std::make_shared<task_t<void>> (run_spot_wide_actor_yield_probe (reply, state));
          detail::observe_task_completion (
            *task, [task, state, complete = std::move (complete)] (const auto &result) mutable {
                if (!result)
                    state->failed.store (true, std::memory_order_release);
                state->first_finished.set ();
                complete ([result] { result.value (); });
            });
      });
    if (!first_accepted || !state->first_started.wait_for ())
        return false;

    const auto same_actor_accepted =
      fixture->serial.execute_actor ("actor-a", "yield-same-actor-next", [state] (auto complete) {
          state->record ("actor-a:next");
          state->same_actor_next.set ();
          complete ([] {});
      });
    const auto other_actor_accepted =
      fixture->serial.execute_actor ("actor-b", "yield-other-actor", [state] (auto complete) {
          state->record ("actor-b");
          state->other_actor.set ();
          complete ([] {});
      });
    const auto spot_accepted = fixture->serial.execute_spot ("yield-spot", [state] {
        state->record ("spot");
        state->spot.set ();
    });
    const auto timer_accepted =
      fixture->serial.execute_timer ("tick", "yield-timer", [state] (auto complete) {
          state->record ("timer");
          state->timer.set ();
          complete ([] {});
      });
    const auto others_progressed =
      state->other_actor.wait_for () && state->spot.wait_for () && state->timer.wait_for ();
    const auto same_actor_ran_while_yielded = state->same_actor_next.is_set ();

    reply->complete (result_t<int>::success (7));
    const auto first_finished = state->first_finished.wait_for ();
    const auto same_actor_resumed =
      state->same_actor_next.wait_for (std::chrono::milliseconds (250));
    if (!first_finished || !same_actor_resumed) {
        // A failed claim-release assertion leaves the tested Actor queue
        // intentionally active. Let process teardown reclaim this failed-test
        // fixture instead of blocking the test at its destructor.
        (void) fixture.release ();
        return false;
    }

    fixture->spot_queue->drain ();
    fixture->serial.actor_executor ("actor-a")->queue ()->drain ();
    fixture->serial.actor_executor ("actor-b")->queue ()->drain ();
    fixture->serial.timer_queue ("tick")->drain ();
    std::lock_guard lock (state->mutex);
    const auto resume = std::find (state->events.begin (), state->events.end (), "actor-a:resume");
    const auto next = std::find (state->events.begin (), state->events.end (), "actor-a:next");
    return same_actor_accepted && other_actor_accepted && spot_accepted && timer_accepted
           && others_progressed && !same_actor_ran_while_yielded
           && !state->failed.load (std::memory_order_acquire) && resume != state->events.end ()
           && next != state->events.end () && resume < next;
}

struct spot_wide_yield_idle_eviction_probe_t
{
    std::shared_ptr<zlink::framework::task_completion_source_t<int>> reply =
      std::make_shared<zlink::framework::task_completion_source_t<int>> ();
    serial_test_signal_t handler_entered;
    serial_test_signal_t handler_resumed;
    serial_test_signal_t follower_ran;
};

zlink::framework::task_t<zlink::message_t> run_spot_wide_yield_idle_eviction_probe (
  const std::shared_ptr<spot_wide_yield_idle_eviction_probe_t> &probe)
{
    probe->handler_entered.set ();
    zlink::framework::request_call_t<int> call (
      "YieldIdleEvictionProbe", [reply = probe->reply] (const auto &, auto, const auto &) {
          return zlink::framework::test::accepted_request_task (reply->task ());
      });
    const auto value = co_await call.yield ();
    if (value != 7)
        throw std::runtime_error ("yield idle-eviction probe reply mismatch");
    probe->handler_resumed.set ();
    co_return zlink::message_t{};
}

bool verify_spot_wide_yield_blocks_idle_eviction_until_handler_terminal ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace runtime = zlink::framework::runtime;
    namespace host = zlink::framework::runtime::host;
    namespace messaging = zlink::framework::runtime::messaging;
    namespace protocol = zlink::framework::runtime::protocol;

    auto node = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::spot_node_builder_state_t> ("yield-idle-eviction-node");
    node->instance_spot_idle_timeout = std::chrono::seconds (1);

    auto executor = std::make_shared<runtime::offload_executor_t> (1);
    node->worker_executor = executor;
    serializer_registry_t serializers;
    node->channel_runtime =
      zlink::framework::test::runtime_failure_fixture<channel_runtime_state_t> ();
    node->channel_runtime->serializers = &serializers;
    const auto native = std::make_shared<host::public_host_runtime_t> (host::host_options_t{
      .mesh = {.descriptor = {.mesh_name = "yield-idle-eviction",
                              .node_routing_id =
                                zlink::routing_id_t::from (std::string ("yield-idle-eviction-node"))
                                  .to_bytes (),
                              .lifecycle_generation = 3,
                              .descriptor_revision = 1,
                              .advertised_endpoint = "tcp://127.0.0.1:0"},
               .runtime_failures =
                 std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()}});
    node->native_node = native;

    auto context = std::make_shared<spot_context_state_t> ();
    context->node = node;
    context->lane_owner = node;
    context->spot_id = "yield-idle-eviction-spot";
    context->spot_name = "yield-idle-eviction-player";
    context->lifecycle_domain = spot_lifecycle_domain_t::instance ();
    context->execution_mode = user_spot_execution_mode_t::spot_wide;
    context->object_generation = 17;
    context->authority_owner_generation = 19;
    auto spot_instance = std::make_shared<int> (1);
    context->spot_instance = spot_instance;
    context->serial_executor = executor;
    context->serial_queue = std::make_shared<runtime::serial_execution_queue_t> (
      *executor, runtime::serial_execution_queue_options_t{},
      runtime::serial_execution_queue_t::error_handler_t{},
      runtime::serial_lane_policy_t::spot_wide ());

    auto probe = std::make_shared<spot_wide_yield_idle_eviction_probe_t> ();
    context->handlers.push_back (
      spot_handler_descriptor_t{spot_handler_kind_t::actor_send, "YieldIdleEvictionProbe", "",
                                std::type_index (typeid (int)), std::type_index (typeid (void)),
                                std::type_index (typeid (int)), std::type_index (typeid (void))});
    context->handler_invokers.push_back (
      [probe] (void *, void *, service_provider_t &, serializer_registry_t &,
               const zlink::message_t &,
               const spot_inbound_message_t &) -> task_t<zlink::message_t> {
          return run_spot_wide_yield_idle_eviction_probe (probe);
      });

    node->spot_ids_by_name.emplace (context->spot_name, context->spot_id);
    node->spot_names_by_id.emplace (context->spot_id, context->spot_name);
    node->spot_contexts_by_id.emplace (context->spot_id, spot_context_access_t::create (context));

    spot_node_builder_state_t::actor_factory_registration_t factory;
    factory.actor_type = std::type_index (typeid (int));
    factory.create_instance = [] (std::string) { return std::make_shared<int> (7); };
    factory.configure_instance = [] (void *, const actor_ref_t &, void *) {};
    node->actor_factories.emplace ("player", std::move (factory));
    const auto actor = actor_ref_access_t::make (
      node_rid_t::from_string ("yield-idle-eviction-node"), "player", "yield-actor", 5);
    const auto actor_key = std::string ("player:yield-actor");
    node->actor_types_by_id.emplace ("yield-actor", "player");
    node->actor_instances.emplace (actor_key, std::make_shared<int> (7));
    node->actor_spot_ids.emplace (actor_key, context->spot_id);
    node->actor_generations.emplace (actor_key, 5);
    const protocol::actor_route_fence_t exact_route{"yield-actor",
                                                    5,
                                                    native->status ().routing_id ().to_bytes (),
                                                    native->status ().lifecycle_generation (),
                                                    11,
                                                    13};
    node->actor_authority_fences.emplace (actor_key, exact_route);

    int eviction_admission_calls = 0;
    bool eviction_identity_valid = true;
    bool eviction_after_terminal = false;
    bool closing_called = false;
    bool closing_after_resume = false;
    context->lifecycle.on_closing = [&] (void *, const spot_closing_context_t &, std::stop_token) {
        closing_called = true;
        closing_after_resume = probe->handler_resumed.is_set ();
        return task_t<void> (result_t<void>::success ());
    };
    node->admit_instance_spot_idle_eviction =
      [&] (const spot_id_t &spot_id, std::string_view spot_name, std::uint64_t object_generation,
           std::uint64_t authority_owner_generation, std::function<void ()> close_local) {
          ++eviction_admission_calls;
          if (spot_id != context->spot_id || spot_name != context->spot_name
              || object_generation != context->object_generation
              || authority_owner_generation != context->authority_owner_generation) {
              eviction_identity_valid = false;
              return false;
          }
          eviction_after_terminal =
            probe->handler_resumed.is_set () && !context->has_active_callback ();
          close_local ();
          return true;
      };

    const auto set_idle_age = [&context] {
        context->last_application_work_completed_ns.store (
          std::chrono::duration_cast<std::chrono::nanoseconds> (
            std::chrono::steady_clock::now ().time_since_epoch ())
              .count ()
            - std::chrono::duration_cast<std::chrono::nanoseconds> (std::chrono::seconds (2))
                .count (),
          std::memory_order_relaxed);
    };

    service_collection_t services;
    services.add_singleton<actor_gateway_runtime_t> ();
    auto provider = services.build_provider ();
    const host::ready_record_t owner{.owner_kind = host::owner_kind_t::actor,
                                     .domain = host::ready_domain_t::application,
                                     .spot_id = std::string (context->spot_id),
                                     .actor = actor};
    host::receive_record_t record{.kind = host::record_kind_t::actor_send,
                                  .domain = host::ready_domain_t::application};
    record.actor_route = exact_route;
    --record.actor_route->object_generation;
    messaging::envelope_codec_t codec;
    auto encoded = zlink::framework::detail::encode_actor_packet (
      messaging::envelope_header_t{.kind = messaging::message_kind_t::command,
                                   .channel_name = "actor",
                                   .message_name = "YieldIdleEvictionProbe"},
      zlink::message_t::from (std::string ("yield")));
    auto parts = std::move (encoded).take_items ();
    std::atomic_int ingress_terminals{0};
    bool terminal_deferred = false;
    const auto dispatched = spot_node_runtime_t (node).dispatch_mesh_record (
      owner, record, parts,
      zlink::framework::runtime::decode_mesh_record_header (record, parts, false).value (),
      provider, serializers,
      [&ingress_terminals] { ingress_terminals.fetch_add (1, std::memory_order_acq_rel); },
      &terminal_deferred);

    const auto handler_entered =
      dispatched && terminal_deferred && probe->handler_entered.wait_for ();
    const auto follower_posted =
      handler_entered && context->try_post_serial ("yield-idle-eviction-follower", [probe] {
          probe->follower_ran.set ();
      });
    const auto follower_ran = follower_posted && probe->follower_ran.wait_for ();
    if (follower_ran)
        context->serial_queue->drain ();
    const auto single_lifecycle_claim =
      follower_ran && node->lane.run ([&] { return context->callback_depth == 1; }).get ();

    bool eviction_rejected_while_yielded = false;
    if (follower_ran) {
        set_idle_age ();
        spot_node_runtime_t (node).evict_idle_spots ();
        eviction_rejected_while_yielded = eviction_admission_calls == 0 && !closing_called
                                          && !probe->handler_resumed.is_set ()
                                          && context->has_active_callback ()
                                          && node->spot_contexts_by_id.contains (context->spot_id);
    }

    const auto coordinator = context->ensure_spot_serial_executor ();
    const auto actor_executor = coordinator->find_actor_executor (actor_key);
    auto terminal_fence = std::make_shared<serial_test_signal_t> ();
    probe->reply->complete (result_t<int>::success (7));
    const auto handler_resumed = probe->handler_resumed.wait_for ();
    const auto terminal_fence_posted =
      actor_executor && handler_resumed
      && coordinator->execute_actor (actor_key, "yield-idle-eviction-terminal-fence",
                                     [terminal_fence] (auto complete) {
                                         complete ([terminal_fence] { terminal_fence->set (); });
                                     });
    const auto terminal_observed = terminal_fence_posted && terminal_fence->wait_for ();
    // Yield releases the Spot gate, while this Actor FIFO retains the handler claim.
    if (actor_executor)
        actor_executor->queue ()->drain ();
    context->serial_queue->drain ();
    executor->drain ();

    const auto handler_completed =
      terminal_observed && probe->handler_resumed.is_set () && !context->has_active_callback ();
    if (!handler_entered || !follower_ran || !single_lifecycle_claim
        || !eviction_rejected_while_yielded || !handler_completed) {
        return false;
    }

    set_idle_age ();
    spot_node_runtime_t (node).evict_idle_spots ();

    return ingress_terminals.load (std::memory_order_acquire) == 1 && eviction_admission_calls == 1
           && eviction_identity_valid && eviction_after_terminal && closing_called
           && closing_after_resume && node->spot_contexts_by_id.empty ()
           && node->spot_ids_by_name.empty () && node->spot_names_by_id.empty ();
}

bool verify_same_actor_synchronous_reentry_is_immediate ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::runtime;

    serial_executor_test_fixture_t fixture (serial_lane_policy_t::per_actor_spot ());
    reentry_probe_actor_client_t client;
    serial_test_signal_t completed;
    std::atomic_bool immediate_invalid_operation{false};
    const auto accepted =
      fixture.serial.execute_actor ("actor-a", "same-actor-reentry", [&] (auto complete) {
          actor_execution_scope_t actor_scope ("probe:actor-a", "spot-a");
          actor_request_call_t request (client, actor_id_t ("actor-a"), "ReentryProbe",
                                        message_t{});
          const auto started = std::chrono::steady_clock::now ();
          const auto result = request.async_message ().result ();
          const auto elapsed = std::chrono::steady_clock::now () - started;
          try {
              result.value ();
          }
          catch (const framework_exception_t &error) {
              immediate_invalid_operation.store (error.kind ()
                                                     == framework_error_kind_t::invalid_operation
                                                   && elapsed < std::chrono::milliseconds (250),
                                                 std::memory_order_release);
          }
          completed.set ();
          complete ([] {});
      });
    const auto observed = completed.wait_for ();
    fixture.serial.actor_executor ("actor-a")->queue ()->drain ();
    return accepted && observed && immediate_invalid_operation.load (std::memory_order_acquire)
           && client.request_calls.load (std::memory_order_acquire) == 0;
}
bool verify_released_spot_turn_does_not_inline_lifecycle_task ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace runtime = zlink::framework::runtime;

    auto executor = std::make_shared<runtime::offload_executor_t> (2, "released-spot-turn");
    auto owner = std::make_shared<spot_context_state_t> ();
    owner->serial_executor = executor;
    owner->serial_queue = std::make_shared<runtime::serial_execution_queue_t> (
      *executor, runtime::serial_execution_queue_options_t{},
      runtime::serial_execution_queue_t::error_handler_t{},
      runtime::serial_lane_policy_t::spot_wide ());
    auto queue = owner->serial_queue;

    std::atomic_bool release_succeeded{false};
    std::atomic_bool released_turn_kept_queue_affiliation{false};
    std::atomic_bool released_stack_had_callback_context{true};
    std::atomic_bool lifecycle_had_fresh_turn{false};
    std::atomic_bool lifecycle_allowed_yield{false};
    std::atomic_bool lifecycle_succeeded{false};
    std::atomic_bool completed{false};
    // Drains before the locals above are destroyed; the queued handler references them.
    scope_exit_t drain_on_exit ([&] { queue->drain (); });
    if (!queue->try_post_async ("release-spot-handler-turn", [&] (auto complete) {
            const auto released_turn = capture_current_serial_turn ();
            const auto released = released_turn && released_turn->release ();
            release_succeeded.store (released, std::memory_order_release);
            if (!released) {
                complete ([] {});
                completed.store (true, std::memory_order_release);
                return;
            }

            released_turn_kept_queue_affiliation.store (owner->owns_current_serial_turn (),
                                                        std::memory_order_release);
            released_stack_had_callback_context.store (owner->is_current_callback_thread (),
                                                       std::memory_order_release);
            const auto result =
              owner
                ->run_serial_task (
                  "lifecycle-after-released-handler-turn",
                  [&, released_turn] () -> task_t<void> {
                      const auto lifecycle_turn = capture_current_serial_turn ();
                      lifecycle_had_fresh_turn.store (
                        lifecycle_turn && lifecycle_turn != released_turn
                          && !lifecycle_turn->released () && owner->owns_current_serial_turn (),
                        std::memory_order_release);
                      lifecycle_allowed_yield.store (current_serial_turn_allows_yield (),
                                                     std::memory_order_release);
                      co_return;
                  })
                .result ();
            lifecycle_succeeded.store (static_cast<bool> (result), std::memory_order_release);
            complete ([] {});
            completed.store (true, std::memory_order_release);
        })) {
        return false;
    }

    if (!wait_until ([&] { return completed.load (std::memory_order_acquire); })) {
        queue->cancel_pending ();
        return false;
    }
    queue->drain ();
    return release_succeeded.load (std::memory_order_acquire)
           && released_turn_kept_queue_affiliation.load (std::memory_order_acquire)
           && !released_stack_had_callback_context.load (std::memory_order_acquire)
           && lifecycle_had_fresh_turn.load (std::memory_order_acquire)
           && lifecycle_allowed_yield.load (std::memory_order_acquire)
           && lifecycle_succeeded.load (std::memory_order_acquire);
}
zlink::framework::task_t<void> origin_handler_task (
  std::vector<zlink::framework::task_completion_source_t<void>> &sources, int &stage, bool fail)
{
    namespace fw = zlink::framework;
    stage = 1;
    co_await fw::detail::yield_call<void> ([&] { return sources[0].task (); });
    stage = 2;
    co_await sources[1].task ();
    stage = 3;
    co_await fw::detail::yield_call<void> ([&] { return sources[2].task (); });
    stage = 4;
    co_await sources[3].task ();
    stage = 5;
    if (fail)
        throw std::runtime_error ("origin handler failed");
}

// Real coroutines cover two Yield/ordinary-await pairs. An ordinary await
// after reacquiring the gate keeps the next application record queued.
bool verify_handler_origin_terminals (origin_terminal_case_t outcome, bool borrowed)
{
    namespace fw = zlink::framework;
    namespace rt = fw::runtime;
    fw::test::manual_internal_scheduler_t worker;
    int errors = 0;
    auto queue = std::make_shared<rt::serial_execution_queue_t> (
      worker, rt::serial_execution_queue_options_t{},
      [&] (const auto &, const auto &) { ++errors; }, rt::serial_lane_policy_t::spot_wide ());
    std::shared_ptr<fw::detail::serial_turn_t> parent;
    int parent_terminal = 0;
    if (borrowed) {
        queue->post_async (
          "finished-parent",
          [&] (auto complete) {
              parent = fw::detail::capture_current_serial_turn ();
              complete ([&] { ++parent_terminal; });
          },
          rt::serial_work_options_t{.lane = rt::serial_work_lane_t::lifecycle});
        worker.run_all ();
    }
    fw::serializer_registry_t serializers;
    serializers.add<origin_reply_t> (
      [] (const origin_reply_t &) -> fw::encoded_payload_t {
          throw std::runtime_error ("origin reply encoding failed");
      },
      [] (const fw::encoded_payload_t &) { return origin_reply_t{}; },
      "application/x-origin-reply");
    std::vector<fw::task_completion_source_t<void>> first (4), second (4);
    int first_stage = 0, second_stage = 0, activated = 0, cancelled = 0;
    int terminals = 0, suffix = 0;
    std::vector<std::shared_ptr<fw::task_t<void>>> tasks;
    const auto submit = [&] (std::string name, auto &sources, int &stage) {
        rt::serial_execution_queue_t::async_work_t work = [&sources, &stage, &tasks, &activated,
                                                           &cancelled, &terminals, &serializers,
                                                           outcome] (auto complete) {
            auto turn = fw::detail::capture_current_serial_turn ();
            if (!turn->defer (
                  [&activated] {
                      ++activated;
                      return fw::task_t<void> (fw::result_t<void>::success ());
                  },
                  [&cancelled] { ++cancelled; }))
                throw std::runtime_error ("origin Defer was rejected");
            auto task = std::make_shared<fw::task_t<void>> (
              origin_handler_task (sources, stage, outcome == origin_terminal_case_t::failure));
            tasks.push_back (task);
            fw::detail::observe_task_terminal (*task, [task, complete, &terminals, &serializers,
                                                       outcome] (const fw::result_t<void> &result) {
                complete ([result, &terminals, &serializers, outcome] {
                    ++terminals;
                    result.value ();
                    if (outcome == origin_terminal_case_t::reply_encoding)
                        (void) serializers.get<origin_reply_t> ().serialize ({});
                });
            });
        };
        if (borrowed)
            return queue->try_post_continuation (std::move (name), std::move (work), parent, true);
        return queue->try_post_async (std::move (name), std::move (work));
    };
    bool passed = submit ("origin-first", first, first_stage);
    if (!borrowed)
        passed = submit ("origin-second", second, second_stage) && passed;
    worker.run_all ();
    auto observed = queue->observe_handler_terminal ();
    worker.run_all ();
    auto terminal = observed.value ()->run ();
    const auto handlers = borrowed ? 1 : 2;
    passed = passed && first_stage == 1 && (borrowed || second_stage == 1)
             && !terminal.await_ready () && parent_terminal == (borrowed ? 1 : 0);
    first[0].complete (fw::result_t<void>::success ());
    worker.run_all ();
    queue->post ("origin-suffix", [&] { ++suffix; });
    worker.run_all ();
    passed = passed && first_stage == 2 && suffix == 0 && !terminal.await_ready ();
    first[1].complete (fw::result_t<void>::success ());
    worker.run_all ();
    passed = passed && first_stage == 3 && suffix == 1 && !terminal.await_ready ();
    first[2].complete (fw::result_t<void>::success ());
    worker.run_all ();
    passed = passed && first_stage == 4 && !terminal.await_ready () && activated == 0;
    first[3].complete (outcome == origin_terminal_case_t::cancel
                         ? fw::result_t<void>::failure (fw::framework_error_kind_t::shutting_down,
                                                        "origin wait cancelled")
                         : fw::result_t<void>::success ());
    worker.run_all ();
    if (!borrowed) {
        passed = passed && !terminal.await_ready () && terminals == 1;
        for (auto &source : second) {
            source.complete (
              outcome == origin_terminal_case_t::cancel && &source == &second.back ()
                ? fw::result_t<void>::failure (fw::framework_error_kind_t::shutting_down,
                                               "origin wait cancelled")
                : fw::result_t<void>::success ());
            worker.run_all ();
        }
    }
    worker.run_all ();
    worker.run_until (terminal);
    worker.run_all ();
    passed = passed && terminal.await_ready () && terminals == handlers
             && activated == (outcome == origin_terminal_case_t::success ? handlers : 0)
             && cancelled == (outcome == origin_terminal_case_t::success ? 0 : handlers)
             && errors == (outcome == origin_terminal_case_t::success ? 0 : handlers);
    auto late = queue->observe_handler_terminal ();
    worker.run_all ();
    auto late_terminal = late.value ()->run ();
    passed = passed && late_terminal.await_ready ();
    observed.value ()->cancel ();
    late.value ()->cancel ();
    tasks.clear ();
    parent.reset ();
    queue->close ();
    worker.run_all ();
    queue->drain ();
    worker.drain ();
    std::cout << "origin-terminals outcome=" << static_cast<int> (outcome)
              << " borrowed=" << borrowed << " terminals=" << terminals
              << " activated=" << activated << " cancelled=" << cancelled << " errors=" << errors
              << " passed=" << passed << '\n';
    return passed;
}

bool verify_all_handler_origin_terminals ()
{
    bool passed = verify_intermediate_resume_keeps_handler_terminal ();
    for (const auto outcome :
         {origin_terminal_case_t::success, origin_terminal_case_t::failure,
          origin_terminal_case_t::cancel, origin_terminal_case_t::reply_encoding})
        for (const bool borrowed : {false, true})
            passed = verify_handler_origin_terminals (outcome, borrowed) && passed;
    return passed;
}
bool verify_serial_resume_waits_behind_queued_work ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::runtime;

    offload_executor_t executor (2);
    serial_execution_queue_options_t options;
    serial_execution_queue_t queue (executor, options, {}, serial_lane_policy_t::spot_wide ());

    auto reply = std::make_shared<task_completion_source_t<int>> ();
    auto task_finished = std::make_shared<std::atomic_bool> (false);
    auto observed_kind = std::make_shared<std::atomic_int> (-1);

    if (!queue.try_post_async (
          "resume-capacity",
          [reply, task_finished, observed_kind] (auto complete) {
              auto task = std::make_shared<task_t<void>> (
                run_request_turn_probe (reply, std::make_shared<std::vector<int>> (),
                                        std::make_shared<std::mutex> (), true));
              detail::observe_task_completion (
                *task, [task, task_finished, observed_kind,
                        complete = std::move (complete)] (const auto &result) mutable {
                    if (!result) {
                        observed_kind->store (static_cast<int> (result.error_kind ()),
                                              std::memory_order_release);
                    }
                    task_finished->store (true, std::memory_order_release);
                    complete ([] {});
                });
          },
          serial_work_options_t{serial_work_lane_t::application,
                                serial_execution_queue_t::fixed_work_byte_cost})) {
        return false;
    }

    // Resumption is queued behind the active filler instead of being rejected.
    std::mutex filler_gate;
    std::condition_variable filler_changed;
    bool filler_entered = false;
    bool release_filler = false;
    bool filler_posted = false;
    // Releases the filler and drains before the locals above are destroyed.
    scope_exit_t drain_on_exit ([&] {
        {
            std::lock_guard lock (filler_gate);
            release_filler = true;
        }
        filler_changed.notify_all ();
        queue.drain ();
    });
    for (int attempt = 0; attempt < 100 && !filler_posted; ++attempt) {
        filler_posted = queue.try_post (
          "resume-filler",
          [&] {
              std::unique_lock lock (filler_gate);
              filler_entered = true;
              filler_changed.notify_all ();
              filler_changed.wait (lock, [&] { return release_filler; });
          },
          serial_work_options_t{serial_work_lane_t::application,
                                serial_execution_queue_t::fixed_work_byte_cost});
        if (!filler_posted)
            std::this_thread::sleep_for (std::chrono::milliseconds (2));
    }
    if (!filler_posted) {
        queue.cancel_pending ();
        return false;
    }
    {
        std::unique_lock lock (filler_gate);
        if (!filler_changed.wait_for (lock, std::chrono::seconds (1),
                                      [&] { return filler_entered; })) {
            queue.cancel_pending ();
            return false;
        }
    }

    reply->complete (result_t<int>::success (7));
    if (task_finished->load (std::memory_order_acquire))
        return false;

    {
        std::lock_guard lock (filler_gate);
        release_filler = true;
    }
    filler_changed.notify_all ();
    if (!wait_until ([&] { return task_finished->load (std::memory_order_acquire); }))
        return false;
    queue.drain ();
    return observed_kind->load (std::memory_order_acquire) == -1;
}
}
