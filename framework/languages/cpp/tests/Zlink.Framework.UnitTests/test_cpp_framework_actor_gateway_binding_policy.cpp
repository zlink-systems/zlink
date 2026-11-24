/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/runtime_failure_fixture.hpp"
#include "runtime/execution/actor_execution_context.hpp"
#include "../support/actor_gateway_cases.hpp"
#include "../support/actor_gateway_fixture.hpp"
#include "../support/actor_gateway_task_result.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/diagnostics/diagnostic_event_sink.hpp"
#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/host/bound_session_bind.hpp"
#include "runtime/protocol/service_wire_codec.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include "runtime/streams/stream_runtime.hpp"
#include <zlink/framework/contracts/configuration/zlink_builder.hpp>
#include <zlink/framework/contracts/dispatch/request_task.hpp>
#include <zlink/framework/contracts/dispatch/task.hpp>
#include <zlink/framework/contracts/streams/stream.hpp>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <tuple>
#include <utility>

namespace zlink::framework::test::actor_gateway
{
using zlink::framework::actor_context_t;
using zlink::framework::actor_session_binding_identity_t;
using zlink::framework::actor_t;
using zlink::framework::task_t;

struct duplicate_bind_test_observation_t
{
    std::atomic_int callback_calls{0};
    std::atomic_bool callback_in_actor_turn{false};
    const zlink::framework::runtime::serial_execution_queue_t *expected_serial_queue = nullptr;
    std::optional<actor_session_binding_identity_t> current;
    std::optional<actor_session_binding_identity_t> incoming;
};

class duplicate_bind_test_actor_t : public actor_t
{
  public:
    duplicate_bind_test_actor_t (actor_context_t context,
                                 std::shared_ptr<duplicate_bind_test_observation_t> observation) :
        _context (std::move (context)), _observation (std::move (observation))
    {
    }

    actor_context_t &context () noexcept override { return _context; }
    const actor_context_t &context () const noexcept override { return _context; }

  protected:
    std::shared_ptr<duplicate_bind_test_observation_t> _observation;
    actor_context_t _context;
};

class callback_duplicate_bind_test_actor_t final : public duplicate_bind_test_actor_t
{
  public:
    using callback_t =
      std::function<task_t<std::optional<std::string>> (const actor_session_binding_identity_t &,
                                                        const actor_session_binding_identity_t &,
                                                        std::stop_token)>;

    callback_duplicate_bind_test_actor_t (
      actor_context_t context,
      std::shared_ptr<duplicate_bind_test_observation_t> observation,
      callback_t callback) :
        duplicate_bind_test_actor_t (std::move (context), std::move (observation)),
        _callback (std::move (callback))
    {
    }

    task_t<std::optional<std::string>>
    on_duplicate_bind (const actor_session_binding_identity_t &current,
                       const actor_session_binding_identity_t &incoming,
                       std::stop_token cancellation) override
    {
        _observation->callback_calls.fetch_add (1, std::memory_order_acq_rel);
        _observation->current = current;
        _observation->incoming = incoming;
        const auto serial_turn = zlink::framework::detail::capture_current_serial_turn ();
        _observation->callback_in_actor_turn.store (
          zlink::framework::runtime::current_actor_execution.actor_key
              == "player:duplicate-bind-actor"
            && zlink::framework::runtime::current_actor_execution.spot_id
                 == std::optional<std::string> ("duplicate-bind-spot")
            && serial_turn && serial_turn->belongs_to (_observation->expected_serial_queue),
          std::memory_order_release);
        return _callback (current, incoming, cancellation);
    }

  private:
    callback_t _callback;
};

template <typename TActor>
class duplicate_bind_test_factory_t final : public zlink::framework::actor_factory_t<TActor>
{
  public:
    explicit duplicate_bind_test_factory_t (std::shared_ptr<TActor> instance) :
        _instance (std::move (instance))
    {
    }

    task_t<std::shared_ptr<TActor>> create (actor_context_t, std::stop_token) override
    {
        co_return _instance;
    }

  private:
    std::shared_ptr<TActor> _instance;
};

task_t<std::optional<std::string>>
wait_for_duplicate_bind_decision (task_t<std::optional<std::string>> decision,
                                  std::stop_token cancellation,
                                  std::shared_ptr<std::promise<void>> cancelled,
                                  std::shared_ptr<std::promise<void>> completed)
{
    std::stop_callback stop (cancellation, [cancelled] { cancelled->set_value (); });
    auto result = co_await decision;
    completed->set_value ();
    co_return result;
}

class session_bound_test_actor_t final : public duplicate_bind_test_actor_t
{
  public:
    using callback_t =
      std::function<task_t<void> (const actor_session_binding_identity_t &, std::stop_token)>;
    session_bound_test_actor_t (actor_context_t context,
                                std::shared_ptr<duplicate_bind_test_observation_t> observation,
                                callback_t callback) :
        duplicate_bind_test_actor_t (std::move (context), std::move (observation)),
        callback (std::move (callback))
    {
    }
    task_t<void> on_session_bound (const actor_session_binding_identity_t &incoming,
                                   std::stop_token stop) override
    {
        ++_observation->callback_calls;
        _observation->incoming = incoming;
        const auto turn = zlink::framework::detail::capture_current_serial_turn ();
        _observation->callback_in_actor_turn =
          turn && turn->belongs_to (_observation->expected_serial_queue)
          && zlink::framework::runtime::current_actor_execution.actor_key
               == "player:duplicate-bind-actor";
        return callback (incoming, stop);
    }
    task_t<std::optional<std::string>>
    on_duplicate_bind (const actor_session_binding_identity_t &current,
                       const actor_session_binding_identity_t &,
                       std::stop_token) override
    {
        _observation->current = current;
        co_return reject ? std::make_optional (std::string ("bound-test-reject")) : std::nullopt;
    }
    bool reject = false;
    callback_t callback;
};

int session_bound_callback_policy ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace runtime = zlink::framework::runtime;
    namespace protocol = zlink::framework::runtime::protocol;
    completion_owner_t completion_owner;
    enum class case_t
    {
        first,
        same,
        replacement,
        failure,
        timeout,
        shutdown,
        runtime_shutdown,
        suspend,
        reject,
        retiring,
        actor_shutdown,
        application_cancel,
        queued
    };
    const auto run_case = [] (case_t test_case, user_spot_execution_mode_t execution_mode) -> int {
        auto gateway_state = std::make_shared<actor_gateway_state_t> ();
        actor_gateway_runtime_t gateway (gateway_state);
        const auto actor = test_actor_ref ("actor-owner", "player", "duplicate-bind-actor", 7);
        const auto wire_actor = actor_ref_access_t::with_actor_type (actor, std::string{});
        auto observation = std::make_shared<duplicate_bind_test_observation_t> ();
        auto entered = std::make_shared<task_completion_source_t<void>> ();
        auto release = std::make_shared<task_completion_source_t<void>> ();
        std::shared_ptr<spot_node_builder_state_t> callback_owner;
        std::atomic_bool cancelled{false};
        std::atomic_bool committed_in_callback{false};
        auto actor_instance = std::make_shared<session_bound_test_actor_t> (
          gateway.actor_context (actor), observation,
          [&, entered, release] (const auto &identity,
                                 std::stop_token cancellation) -> task_t<void> {
              auto route = co_await gateway.bound_session_route_async (actor);
              committed_in_callback =
                route && route->binding_generation == identity.binding_generation;
              entered->complete (result_t<void>::success ());
              std::stop_callback on_stop (cancellation, [&] { cancelled = true; });
              if (test_case == case_t::actor_shutdown) {
                  co_await callback_owner->lane.run_task ([callback_owner] {
                      callback_owner->retiring_actor_keys.insert ("player:duplicate-bind-actor");
                  });
                  std::rethrow_exception (detail::make_cancellation_exception ("Actor ended"));
              }
              if (test_case == case_t::application_cancel)
                  std::rethrow_exception (
                    detail::make_cancellation_exception ("Application cancelled"));
              if (test_case == case_t::failure)
                  throw std::runtime_error ("session-bound-test-failure");
              if (test_case == case_t::timeout || test_case == case_t::shutdown
                  || test_case == case_t::runtime_shutdown || test_case == case_t::suspend)
                  co_await release->task ();
              co_return;
          });
        actor_instance->reject = test_case == case_t::reject;
        const auto spot_name = spot_id_t ("duplicate-bind-spot");
        auto spot_context = std::make_shared<spot_context_state_t> ();
        spot_context->spot_id = spot_name;
        spot_context->spot_name = std::string (spot_name);
        spot_context->spot_instance = std::make_shared<int> (1);
        spot_context->execution_mode = execution_mode;
        const auto policy = execution_mode == user_spot_execution_mode_t::spot_wide
                              ? runtime::serial_lane_policy_t::spot_wide ()
                              : runtime::serial_lane_policy_t::per_actor_spot ();
        spot_context->serial_executor =
          std::make_shared<runtime::offload_executor_t> (1, "duplicate-bind-actor");
        spot_context->serial_queue = std::make_shared<runtime::serial_execution_queue_t> (
          *spot_context->serial_executor, runtime::serial_execution_queue_options_t{},
          runtime::serial_execution_queue_t::error_handler_t{}, policy);
        spot_node_builder_t actor_builder;
        actor_builder.add_actor_factory<session_bound_test_actor_t,
                                        duplicate_bind_test_factory_t<session_bound_test_actor_t>> (
          "player",
          std::make_shared<duplicate_bind_test_factory_t<session_bound_test_actor_t>> (
            actor_instance),
          [] (auto &factory) { factory.recreate_on_relocation (); });
        auto spot_runtime = spot_node_runtime_t::from (actor_builder);
        auto spot_state = spot_runtime.weak_state ().lock ();
        callback_owner = spot_state;
        spot_state->monitoring = test::runtime_failure_monitoring ();
        const std::string actor_key = "player:duplicate-bind-actor";
        spot_context->spot_serial_executor = std::make_shared<spot_serial_executor_t> (
          spot_context->serial_executor, spot_state->lane, policy, spot_context->serial_queue);
        observation->expected_serial_queue =
          execution_mode == user_spot_execution_mode_t::spot_wide
            ? spot_context->serial_queue.get ()
            : spot_context->spot_serial_executor->actor_executor (actor_key)->queue ().get ();
        auto &factory = spot_state->actor_factories.at ("player");
        spot_state->actor_instances.emplace (
          actor_key, factory.create_context_instance (gateway.actor_context (actor)));
        spot_state->actor_types_by_id.emplace ("duplicate-bind-actor", "player");
        spot_state->actor_spot_ids.emplace (actor_key, spot_name);
        spot_state->spot_contexts_by_id.emplace (std::string (spot_name),
                                                 spot_context_access_t::create (spot_context));

        const auto previous_owner = zlink::routing_id_t::from ("previous-session-owner");
        const auto previous_session = zlink::routing_id_t::from ("previous-session");
        const actor_bound_session_route_t previous_route{previous_owner,
                                                         previous_session,
                                                         actor.object_generation (),
                                                         5,
                                                         13,
                                                         17,
                                                         19,
                                                         0,
                                                         0,
                                                         false,
                                                         "previous-owner",
                                                         23};
        const auto no_op_sink = [] (std::string, stream_codec_t, const zlink::message_t &) {
            return task_t<void> (result_t<void>::success ());
        };
        if (test_case != case_t::first
            && !gateway.replace_session_route (actor, no_op_sink, previous_route))
            return 1;

        zlink_builder_t builder = test::runtime_failure_builder ();
        builder.stream ("duplicate-bind").bind ("tcp://127.0.0.1:0");
        auto stream_runtime = stream_runtime_t::from (builder);
        std::mutex delay_mutex;
        delay_owner_t delays (delay_mutex);
        const auto incoming_owner = test_case == case_t::same
                                      ? previous_owner
                                      : zlink::routing_id_t::from ("incoming-session-owner");
        const auto incoming_session = test_case == case_t::same
                                        ? previous_session
                                        : zlink::routing_id_t::from ("incoming-session");
        const std::uint64_t incoming_generation = test_case == case_t::same ? 19 : 20;
        const location_owner_token_t incoming_owner_token =
          test_case == case_t::same ? location_owner_token_t{"previous-owner", 23}
                                    : location_owner_token_t{"incoming-owner", 29};
        const protocol::bound_session_bind_t bind{
          1,
          protocol::actor_route_fence_t{
            std::string (actor.actor_id ().value ()), actor.object_generation (),
            zlink::routing_id_t::from (std::string (actor.node_rid ().value ())).to_bytes (), 11,
            13, 17},
          incoming_session.to_bytes (),
          {protocol::bound_session_binding_state_t::active, incoming_generation}};
        bool terminal_submitted = false;
        std::optional<std::tuple<std::uint32_t, std::uint32_t,
                                 std::optional<protocol::stream_bind_reply_tail_t>>>
          terminal;
        auto submit_terminal_reply = [&] (std::uint32_t result, std::uint32_t failure_code,
                                          std::optional<protocol::stream_bind_reply_tail_t> tail) {
            terminal_submitted = true;
            terminal.emplace (result, failure_code, std::move (tail));
            return true;
        };

        std::atomic_int reports{0};
        std::mutex report_gate;
        std::string last_report;
        spot_state->runtime_failures ()->bind_runtime_reporter ([&] (std::exception_ptr error) {
            if (error) {
                const auto summary = diagnostic_event_sink_t::exception_summary (error);
                const std::lock_guard lock (report_gate);
                last_report = summary.type + ": " + summary.message;
                ++reports;
            }
        });
        auto gate_entered = std::make_shared<task_completion_source_t<void>> ();
        auto gate_release = std::make_shared<task_completion_source_t<void>> ();
        auto blocker = spot_runtime.run_actor_turn (
          actor,
          [gate_entered, gate_release, spot_state, actor_key,
           test_case] (actor_t &, std::stop_token) -> task_t<result_t<bool>> {
              if (test_case == case_t::retiring) {
                  co_await spot_state->lane.run_task ([spot_state, actor_key] {
                      spot_state->retiring_actor_keys.insert (actor_key);
                  });
              }
              gate_entered->complete (result_t<void>::success ());
              co_await gate_release->task ();
              co_return result_t<bool>::success (true);
          });
        if (!finite_task_result (gate_entered->task ()))
            return 8;
        auto pending = apply_bound_session_bind_async (
          gateway.weak_state (), {}, spot_state, stream_runtime, wire_actor, bind, incoming_owner,
          test_case == case_t::same ? 5 : 9, incoming_owner_token,
          std::move (submit_terminal_reply), delays,
          std::chrono::milliseconds (test_case == case_t::timeout ? 25 : 2000));
        std::optional<decltype (pending)> queued;
        if (test_case == case_t::queued) {
            auto next = bind;
            next.session_routing_id = zlink::routing_id_t::from ("queued-session").to_bytes ();
            next.binding.generation = 21;
            queued.emplace (apply_bound_session_bind_async (
              gateway.weak_state (), {}, spot_state, stream_runtime, wire_actor, next,
              incoming_owner, 9, incoming_owner_token, [] (auto, auto, auto) { return true; },
              delays, std::chrono::milliseconds (2000)));
        }
        const bool waited_for_gate = !pending.result_for (std::chrono::milliseconds (10));
        gate_release->complete (result_t<void>::success ());
        (void) finite_task_result (std::move (blocker));
        const auto result = finite_task_result (std::move (pending));
        if (queued) {
            const auto next = finite_task_result (std::move (*queued));
            auto drained = spot_runtime.run_actor_turn (
              actor, [] (actor_t &, std::stop_token) -> task_t<result_t<bool>> {
                  co_return result_t<bool>::success (true);
              });
            (void) finite_task_result (std::move (drained));
            const auto route = gateway.bound_session_route_async (actor).result ().value ();
            return result && *result && next && *next && observation->callback_calls == 2
                       && observation->current && observation->current->binding_generation == 20
                       && observation->incoming && observation->incoming->binding_generation == 21
                       && route && route->binding_generation == 21 && reports == 0
                     ? 0
                     : 12;
        }
        if (test_case == case_t::retiring) {
            return result && !*result && result->error_kind () == framework_error_kind_t::not_found
                       && observation->callback_calls == 0 && !terminal_submitted
                     ? 0
                     : 11;
        }
        if (test_case == case_t::reject) {
            const auto route = gateway.bound_session_route_async (actor).result ().value ();
            return result && *result
                       && result->value ().terminal_result
                            == static_cast<std::uint32_t> (
                              protocol::request_terminal_result::rejected)
                       && observation->callback_calls == 0 && reports == 0 && route
                       && route->binding_generation == 19
                     ? 0
                     : 9;
        }
        if (!result || !*result
            || result->value ().error != runtime::stateful::stateful_error_t::none)
            return 1;
        const auto callback_started = entered->task ().result_for (std::chrono::milliseconds (500));
        if (!callback_started) {
            std::cerr << "FAIL: bind succeeded without on_session_bound callback\n";
            return 2;
        }
        if (!waited_for_gate || !terminal_submitted || !committed_in_callback
            || observation->callback_calls != 1 || !observation->callback_in_actor_turn
            || !observation->incoming
            || *observation->incoming
                 != actor_session_binding_identity_t{
                   node_rid_t::from_string (incoming_owner.to_string ()),
                   test_case == case_t::same ? 5u : 9u, incoming_generation})
            return 3;
        if (test_case == case_t::shutdown)
            delays.request_stop ();
        if (test_case == case_t::runtime_shutdown)
            spot_state->worker_cancellation.request_stop ();
        if (test_case == case_t::suspend) {
            auto following = spot_runtime.run_actor_turn (
              actor, [] (actor_t &, std::stop_token) -> task_t<result_t<bool>> {
                  co_return result_t<bool>::success (true);
              });
            const bool still_serial = !following.result_for (std::chrono::milliseconds (10));
            release->complete (result_t<void>::success ());
            (void) finite_task_result (std::move (following));
            if (!still_serial || cancelled)
                return 10;
        }
        if (test_case == case_t::timeout || test_case == case_t::shutdown
            || test_case == case_t::runtime_shutdown) {
            const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (2);
            while (!cancelled && std::chrono::steady_clock::now () < deadline)
                std::this_thread::yield ();
            if (!cancelled)
                return 4;
            release->complete (result_t<void>::success ());
        }
        auto drained = std::make_shared<task_completion_source_t<void>> ();
        const bool posted = spot_context->spot_serial_executor->execute_actor (
          actor_key, "session-bound-drain", [drained] (auto complete) {
              complete ([drained] { drained->complete (result_t<void>::success ()); });
          });
        if (!posted || !finite_task_result (drained->task ()))
            return 5;
        const auto expected_reports = test_case == case_t::failure || test_case == case_t::timeout
                                          || test_case == case_t::application_cancel
                                        ? 1
                                        : 0;
        if (reports != expected_reports) {
            const std::lock_guard lock (report_gate);
            std::cerr << "runtime reports=" << reports << " expected=" << expected_reports
                      << " detail=" << last_report << '\n';
            return 6;
        }
        const auto route = gateway.bound_session_route_async (actor).result ().value ();
        if (!route || route->binding_generation != incoming_generation)
            return 7;
        return 0;
    };
    int failures = 0;
    for (const auto mode :
         {user_spot_execution_mode_t::spot_wide, user_spot_execution_mode_t::per_actor})
        for (const auto test_case :
             {case_t::first, case_t::same, case_t::replacement, case_t::failure, case_t::timeout,
              case_t::shutdown, case_t::runtime_shutdown, case_t::suspend, case_t::reject,
              case_t::retiring, case_t::actor_shutdown, case_t::application_cancel,
              case_t::queued}) {
            const auto result = run_case (test_case, mode);
            if (result) {
                std::cerr << "session bound case=" << static_cast<int> (test_case)
                          << " mode=" << static_cast<int> (mode) << " failure=" << result << '\n';
                ++failures;
            }
        }
    if (!failures)
        std::cout << "session bound callback: 26 cases passed\n";
    return failures ? 1 : 0;
}

int duplicate_bind_callback_policy ()
{
    zlink::framework::completion_owner_t completion_owner;
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace runtime = zlink::framework::runtime;
    namespace protocol = zlink::framework::runtime::protocol;
    enum class case_t
    {
        accept,
        accept_empty,
        reject,
        timeout,
        no_callback,
        factory_subtype,
        first_binding,
        same_session
    };

    const auto run_case = [] (case_t test_case, user_spot_execution_mode_t execution_mode) -> int {
        auto gateway_state = std::make_shared<actor_gateway_state_t> ();
        actor_gateway_runtime_t gateway (gateway_state);
        const auto actor = test_actor_ref ("actor-owner", "player", "duplicate-bind-actor", 7);
        const auto wire_actor = actor_ref_access_t::with_actor_type (actor, std::string{});
        auto observation = std::make_shared<duplicate_bind_test_observation_t> ();
        std::shared_ptr<duplicate_bind_test_actor_t> actor_instance;
        auto actor_context = gateway.actor_context (actor);
        auto release_decision =
          std::make_shared<task_completion_source_t<std::optional<std::string>>> ();
        auto cancelled = std::make_shared<std::promise<void>> ();
        auto cancelled_result = cancelled->get_future ();
        auto callback_completed = std::make_shared<std::promise<void>> ();
        auto callback_completed_result = callback_completed->get_future ();
        if (test_case == case_t::no_callback) {
            actor_instance = std::make_shared<duplicate_bind_test_actor_t> (
              std::move (actor_context), observation);
        } else {
            callback_duplicate_bind_test_actor_t::callback_t callback;
            if (test_case == case_t::accept || test_case == case_t::accept_empty
                || test_case == case_t::first_binding || test_case == case_t::same_session) {
                callback = [test_case] (const auto &, const auto &, std::stop_token) {
                    return task_t<std::optional<std::string>> (
                      result_t<std::optional<std::string>>::success (
                        test_case == case_t::accept_empty
                          ? std::optional<std::string> (std::string{})
                          : std::nullopt));
                };
            } else if (test_case == case_t::reject || test_case == case_t::factory_subtype) {
                callback = [] (const auto &, const auto &, std::stop_token) {
                    return task_t<std::optional<std::string>> (
                      result_t<std::optional<std::string>>::success (
                        std::make_optional (std::string ("already signed in"))));
                };
            } else {
                callback = [release_decision, cancelled, callback_completed] (
                             const auto &, const auto &, std::stop_token cancellation_token) {
                    return wait_for_duplicate_bind_decision (
                      release_decision->task (), cancellation_token, cancelled, callback_completed);
                };
            }
            actor_instance = std::make_shared<callback_duplicate_bind_test_actor_t> (
              std::move (actor_context), observation, std::move (callback));
        }

        const auto spot_name = spot_id_t ("duplicate-bind-spot");
        auto spot_context = std::make_shared<spot_context_state_t> ();
        spot_context->spot_id = spot_name;
        spot_context->spot_name = std::string (spot_name);
        spot_context->spot_instance = std::make_shared<int> (1);
        spot_context->execution_mode = execution_mode;
        const auto policy = execution_mode == user_spot_execution_mode_t::spot_wide
                              ? runtime::serial_lane_policy_t::spot_wide ()
                              : runtime::serial_lane_policy_t::per_actor_spot ();
        spot_context->serial_executor =
          std::make_shared<runtime::offload_executor_t> (1, "duplicate-bind-actor");
        spot_context->serial_queue = std::make_shared<runtime::serial_execution_queue_t> (
          *spot_context->serial_executor, runtime::serial_execution_queue_options_t{},
          runtime::serial_execution_queue_t::error_handler_t{}, policy);
        spot_node_builder_t actor_builder;
        if (test_case == case_t::no_callback || test_case == case_t::factory_subtype) {
            actor_builder
              .add_actor_factory<duplicate_bind_test_actor_t,
                                 duplicate_bind_test_factory_t<duplicate_bind_test_actor_t>> (
                "player",
                std::make_shared<duplicate_bind_test_factory_t<duplicate_bind_test_actor_t>> (
                  actor_instance),
                [] (auto &factory) { factory.recreate_on_relocation (); });
        } else {
            actor_builder.add_actor_factory<
              callback_duplicate_bind_test_actor_t,
              duplicate_bind_test_factory_t<callback_duplicate_bind_test_actor_t>> (
              "player",
              std::make_shared<
                duplicate_bind_test_factory_t<callback_duplicate_bind_test_actor_t>> (
                std::static_pointer_cast<callback_duplicate_bind_test_actor_t> (actor_instance)),
              [] (auto &factory) { factory.recreate_on_relocation (); });
        }
        auto spot_runtime = spot_node_runtime_t::from (actor_builder);
        auto spot_state = spot_runtime.weak_state ().lock ();
        spot_state->monitoring = test::runtime_failure_monitoring ();
        const std::string actor_key = "player:duplicate-bind-actor";
        spot_context->spot_serial_executor = std::make_shared<spot_serial_executor_t> (
          spot_context->serial_executor, spot_state->lane, policy, spot_context->serial_queue);
        observation->expected_serial_queue =
          execution_mode == user_spot_execution_mode_t::spot_wide
            ? spot_context->serial_queue.get ()
            : spot_context->spot_serial_executor->actor_executor (actor_key)->queue ().get ();
        auto &factory = spot_state->actor_factories.at ("player");
        spot_state->actor_instances.emplace (
          actor_key, factory.create_context_instance (gateway.actor_context (actor)));
        spot_state->actor_types_by_id.emplace ("duplicate-bind-actor", "player");
        spot_state->actor_spot_ids.emplace (actor_key, spot_name);
        spot_state->spot_contexts_by_id.emplace (std::string (spot_name),
                                                 spot_context_access_t::create (spot_context));

        const auto previous_owner = zlink::routing_id_t::from ("previous-session-owner");
        const auto previous_session = zlink::routing_id_t::from ("previous-session");
        const actor_bound_session_route_t previous_route{previous_owner,
                                                         previous_session,
                                                         actor.object_generation (),
                                                         5,
                                                         13,
                                                         17,
                                                         19,
                                                         0,
                                                         0,
                                                         false,
                                                         "previous-owner",
                                                         23};
        const auto no_op_sink = [] (std::string, stream_codec_t, const zlink::message_t &) {
            return task_t<void> (result_t<void>::success ());
        };
        if (test_case != case_t::first_binding
            && !gateway.replace_session_route (actor, no_op_sink, previous_route))
            return 1;

        zlink_builder_t builder = test::runtime_failure_builder ();
        builder.stream ("duplicate-bind").bind ("tcp://127.0.0.1:0");
        auto stream_runtime = stream_runtime_t::from (builder);
        std::mutex delay_mutex;
        delay_owner_t delays (delay_mutex);
        const auto incoming_owner = test_case == case_t::same_session
                                      ? previous_owner
                                      : zlink::routing_id_t::from ("incoming-session-owner");
        const auto incoming_session = test_case == case_t::same_session
                                        ? previous_session
                                        : zlink::routing_id_t::from ("incoming-session");
        const std::uint64_t incoming_generation = test_case == case_t::same_session ? 19 : 20;
        const location_owner_token_t incoming_owner_token =
          test_case == case_t::same_session ? location_owner_token_t{"previous-owner", 23}
                                            : location_owner_token_t{"incoming-owner", 29};
        const protocol::bound_session_bind_t bind{
          1,
          protocol::actor_route_fence_t{
            std::string (actor.actor_id ().value ()), actor.object_generation (),
            zlink::routing_id_t::from (std::string (actor.node_rid ().value ())).to_bytes (), 11,
            13, 17},
          incoming_session.to_bytes (),
          {protocol::bound_session_binding_state_t::active, incoming_generation}};
        bool terminal_submitted = false;
        std::optional<std::tuple<std::uint32_t, std::uint32_t,
                                 std::optional<protocol::stream_bind_reply_tail_t>>>
          terminal;
        auto submit_terminal_reply = [&] (std::uint32_t result, std::uint32_t failure_code,
                                          std::optional<protocol::stream_bind_reply_tail_t> tail) {
            terminal_submitted = true;
            terminal.emplace (result, failure_code, std::move (tail));
            return true;
        };
        auto disconnect_entered = std::make_shared<task_completion_source_t<void>> ();
        auto release_disconnect = std::make_shared<task_completion_source_t<void>> ();
        std::optional<task_t<result_t<bool>>> disconnect;
        const bool gate_free = test_case == case_t::no_callback
                               || test_case == case_t::first_binding
                               || test_case == case_t::same_session;
        const bool hold_disconnect = gate_free || test_case == case_t::reject
                                     || test_case == case_t::factory_subtype
                                     || test_case == case_t::accept;
        if (hold_disconnect) {
            // The previous Session disconnect owns the Actor turn until explicitly released.
            disconnect.emplace (spot_runtime.run_actor_turn (
              actor,
              [disconnect_entered, release_disconnect] (actor_t &,
                                                        std::stop_token) -> task_t<result_t<bool>> {
                  disconnect_entered->complete (result_t<void>::success ());
                  co_await release_disconnect->task ();
                  co_return result_t<bool>::success (true);
              }));
            if (!finite_task_result (disconnect_entered->task ())) {
                release_disconnect->complete (result_t<void>::success ());
                (void) finite_task_result (std::move (*disconnect));
                return 8;
            }
        }
        auto pending = apply_bound_session_bind_async (
          gateway.weak_state (), {}, spot_state, std::move (stream_runtime), wire_actor, bind,
          incoming_owner, test_case == case_t::same_session ? 5 : 9, incoming_owner_token,
          std::move (submit_terminal_reply), delays, std::chrono::milliseconds (25));
        if (hold_disconnect && !gate_free) {
            const bool waited_for_gate =
              !pending.await_ready () && observation->callback_calls.load () == 0;
            release_disconnect->complete (result_t<void>::success ());
            (void) finite_task_result (std::move (*disconnect));
            if (!waited_for_gate)
                return 9;
        }
        if (test_case == case_t::timeout
            && cancelled_result.wait_for (std::chrono::milliseconds (500))
                 != std::future_status::ready)
            return 2;
        auto bind_finished = std::make_shared<std::promise<void>> ();
        auto bind_finished_result = bind_finished->get_future ();
        observe_task_completion (pending,
                                 [bind_finished] (const auto &) { bind_finished->set_value (); });
        auto result = finite_task_result (std::move (pending), std::chrono::milliseconds (500));
        if (gate_free) {
            const bool disconnect_still_held = !disconnect->await_ready ();
            release_disconnect->complete (result_t<void>::success ());
            const auto disconnected = finite_task_result (std::move (*disconnect));
            // Drain the bind on RED too, before destroying its reply captures and timer owner.
            if (!result)
                (void) bind_finished_result.wait_for (std::chrono::milliseconds (500));
            if (!disconnect_still_held || !disconnected || !*disconnected) {
                std::cerr << "Actor lifecycle held=" << disconnect_still_held
                          << " terminal=" << static_cast<bool> (disconnected)
                          << " success=" << (disconnected && static_cast<bool> (*disconnected))
                          << '\n';
                return 10;
            }
        }
        if (!result || !*result)
            return 3;
        const auto &bind_result = result->value ();
        const auto current_route = gateway.bound_session_route_async (actor).result ().value ();
        const auto route_is_incoming = current_route && current_route->session_rid
                                       && *current_route->session_rid == incoming_session
                                       && current_route->binding_generation == incoming_generation;
        const bool route_is_previous = current_route && current_route->session_rid
                                       && *current_route->session_rid == previous_session
                                       && current_route->binding_generation == 19;
        if (test_case == case_t::reject || test_case == case_t::factory_subtype) {
            return bind_result.terminal_result
                         == static_cast<std::uint32_t> (protocol::request_terminal_result::rejected)
                       && bind_result.reply_tail && bind_result.reply_tail->rejection_message
                       && *bind_result.reply_tail->rejection_message == "already signed in"
                       && bind_result.error == runtime::stateful::stateful_error_t::conflict
                       && !terminal_submitted && route_is_previous
                       && observation->callback_calls.load (std::memory_order_acquire) == 1
                       && observation->callback_in_actor_turn.load (std::memory_order_acquire)
                     ? 0
                     : 4;
        }
        if (test_case == case_t::timeout) {
            auto actor_started = std::make_shared<std::promise<void>> ();
            auto actor_started_result = actor_started->get_future ();
            auto spot_started = std::make_shared<std::promise<void>> ();
            auto spot_started_result = spot_started->get_future ();
            const auto actor_posted = spot_context->spot_serial_executor->execute_actor (
              actor_key, "duplicate-bind-next-actor", [actor_started] (auto complete) {
                  complete ([actor_started] { actor_started->set_value (); });
              });
            const auto spot_posted = spot_context->spot_serial_executor->execute_spot (
              "duplicate-bind-other-spot", [spot_started] { spot_started->set_value (); });
            const auto actor_waited = actor_started_result.wait_for (std::chrono::milliseconds (10))
                                      == std::future_status::timeout;
            if (execution_mode == user_spot_execution_mode_t::per_actor)
                spot_started_result.wait ();
            const auto spot_status = spot_started_result.wait_for (std::chrono::milliseconds (10));
            const auto spot_scope_matches = execution_mode == user_spot_execution_mode_t::spot_wide
                                              ? spot_status == std::future_status::timeout
                                              : spot_status == std::future_status::ready;
            release_decision->complete (
              result_t<std::optional<std::string>>::success (std::nullopt));
            if (callback_completed_result.wait_for (std::chrono::milliseconds (500))
                != std::future_status::ready)
                return 5;
            if (!actor_posted || !spot_posted || !actor_waited || !spot_scope_matches
                || actor_started_result.wait_for (std::chrono::milliseconds (500))
                     != std::future_status::ready
                || spot_started_result.wait_for (std::chrono::milliseconds (500))
                     != std::future_status::ready)
                return 11;
            const auto after_late_accept =
              gateway.bound_session_route_async (actor).result ().value ();
            return bind_result.terminal_result
                         == static_cast<std::uint32_t> (protocol::request_terminal_result::timedOut)
                       && bind_result.error == runtime::stateful::stateful_error_t::unavailable
                       && !terminal_submitted && route_is_previous && after_late_accept
                       && after_late_accept->session_rid
                       && *after_late_accept->session_rid == previous_session
                       && observation->callback_calls.load (std::memory_order_acquire) == 1
                       && observation->callback_in_actor_turn.load (std::memory_order_acquire)
                     ? 0
                     : 6;
        }
        const auto accepted_by_callback =
          test_case == case_t::accept || test_case == case_t::accept_empty;
        const auto callback_state_matches =
          accepted_by_callback
            ? observation->callback_calls.load (std::memory_order_acquire) == 1
                && observation->current
                && *observation->current
                     == actor_session_binding_identity_t{node_rid_t::from_string (
                                                           previous_owner.to_string ()),
                                                         5, 19}
                && observation->incoming
                && *observation->incoming
                     == actor_session_binding_identity_t{node_rid_t::from_string (
                                                           incoming_owner.to_string ()),
                                                         9, 20}
                && observation->callback_in_actor_turn.load (std::memory_order_acquire)
            : observation->callback_calls.load (std::memory_order_acquire) == 0;
        return bind_result.error == runtime::stateful::stateful_error_t::none && route_is_incoming
                   && terminal_submitted && terminal
                   && std::get<0> (*terminal)
                        == static_cast<std::uint32_t> (protocol::request_terminal_result::ok)
                   && std::get<1> (*terminal)
                        == static_cast<std::uint32_t> (protocol::framework_error_code::none)
                   && std::get<2> (*terminal)
                   && std::get<2> (*terminal)->binding_generation == incoming_generation
                   && std::get<2> (*terminal)->authority_owner_generation == 13
                   && ((test_case == case_t::same_session || test_case == case_t::first_binding)
                         ? !bind_result.replacement
                         : bind_result.replacement
                             && bind_result.replacement->retired_session.retired_binding_generation
                                  == 19)
                   && callback_state_matches
                 ? 0
                 : 7;
    };

    for (const auto execution_mode :
         {user_spot_execution_mode_t::spot_wide, user_spot_execution_mode_t::per_actor}) {
        for (const auto test_case : {case_t::accept, case_t::accept_empty, case_t::reject,
                                     case_t::timeout, case_t::no_callback, case_t::factory_subtype,
                                     case_t::first_binding, case_t::same_session}) {
            if (const auto result = run_case (test_case, execution_mode); result != 0) {
                std::cerr << "duplicate bind case=" << static_cast<int> (test_case)
                          << " mode=" << static_cast<int> (execution_mode) << " failure=" << result
                          << '\n';
                return result;
            }
        }
    }
    return 0;
}
} // namespace zlink::framework::test::actor_gateway
