/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/runtime_failure_fixture.hpp"
#include "../support/execution_cases.hpp"
#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/execution/serial_execution_queue.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include "../support/scope_exit.hpp"
#include <zlink/framework/contracts/spots/spot.hpp>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
using zlink::framework::test::scope_exit_t;

namespace zlink::framework::test::execution
{
// Spot address messaging §9: a draining host ends new Spot creation and Actor
// admission with ShuttingDown before any Spot is consulted.
bool verify_draining_host_refuses_new_admission_with_shutting_down ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    auto node = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::spot_node_builder_state_t> ("draining-host-node");
    spot_node_runtime_t runtime (node);
    runtime.bind_drain_flag (std::make_shared<std::atomic_bool> (true));
    const auto draining_actor = actor_ref_access_t::make (
      node_rid_t::from_string ("draining-host-node"), "player", "draining-actor", 1);
    const auto shutting_down = [] (const auto &result) {
        return !result && result.error_kind () == framework_error_kind_t::shutting_down;
    };

    const auto remote_join = runtime
                               .join_remote_actor_to_spot_erased (
                                 draining_actor, spot_id_t ("draining-room"), zlink::message_t{},
                                 spot_node_runtime_t::default_actor_context ())
                               .result ();
    const auto remote_admission =
      runtime
        .admit_remote_actor_to_spot ("draining-transfer", draining_actor,
                                     spot_id_t ("draining-source"), spot_id_t ("draining-room"),
                                     zlink::message_t{})
        .result ();
    const auto entry_join =
      runtime
        .join_actor_to_entry_spot_erased (
          draining_actor, node_rid_t::from_string ("draining-host-node"), zlink::message_t{},
          std::nullopt, spot_node_runtime_t::default_actor_context ())
        .result ();
    bool create_refused = false;
    try {
        (void) runtime.create_spot ("draining-room");
    }
    catch (const framework_exception_t &error) {
        create_refused = error.kind () == framework_error_kind_t::shutting_down;
    }
    if (!shutting_down (remote_join) || !shutting_down (remote_admission)
        || !shutting_down (entry_join) || !create_refused) {
        std::cerr << "draining host admission kinds: remote_join="
                  << static_cast<int> (remote_join.error_kind ())
                  << " remote_admission=" << static_cast<int> (remote_admission.error_kind ())
                  << " entry_join=" << static_cast<int> (entry_join.error_kind ())
                  << " create_refused=" << create_refused << '\n';
        return false;
    }
    return true;
}

bool verify_idle_instance_spot_eviction_closes_local_context ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    auto node = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::spot_node_builder_state_t> ("idle-node");
    node->instance_spot_idle_timeout = std::chrono::seconds (1);

    auto executor = std::make_shared<runtime::offload_executor_t> (1);
    auto context = std::make_shared<spot_context_state_t> ();
    context->node = node;
    context->spot_id = "instance-1";
    context->spot_name = "instance-player";
    context->lifecycle_domain = detail::spot_lifecycle_domain_t::instance ();
    context->object_generation = 7;
    context->authority_owner_generation = 11;
    context->spot_instance = std::make_shared<int> (1);
    context->serial_executor = executor;
    context->serial_queue = std::make_shared<runtime::serial_execution_queue_t> (
      *executor, runtime::serial_execution_queue_options_t{});
    const auto set_last_application_work = [&context] (auto age) {
        context->last_application_work_completed_ns.store (
          std::chrono::duration_cast<std::chrono::nanoseconds> (
            std::chrono::steady_clock::now ().time_since_epoch ())
              .count ()
            - std::chrono::duration_cast<std::chrono::nanoseconds> (age).count (),
          std::memory_order_relaxed);
    };
    context->last_application_work_completed_ns.store (
      std::chrono::duration_cast<std::chrono::nanoseconds> (
        std::chrono::steady_clock::now ().time_since_epoch ())
          .count ()
        - std::chrono::duration_cast<std::chrono::nanoseconds> (std::chrono::milliseconds (100))
            .count (),
      std::memory_order_relaxed);

    bool closing_called = false;
    bool location_visible_while_closing = false;
    spot_close_reason_t closing_reason = spot_close_reason_t::explicit_close;
    context->lifecycle.on_closing = [&] (void *, const spot_closing_context_t &closing,
                                         std::stop_token) {
        closing_called = true;
        location_visible_while_closing = node->spot_contexts_by_id.contains (context->spot_id)
                                         && node->spot_names_by_id.contains (context->spot_id)
                                         && context->node == node;
        closing_reason = closing.reason;
        return task_t<void> (result_t<void>::success ());
    };

    node->spot_ids_by_name.emplace (context->spot_name, context->spot_id);
    node->spot_names_by_id.emplace (context->spot_id, context->spot_name);
    node->spot_contexts_by_id.emplace (context->spot_id, spot_context_access_t::create (context));

    bool admission_called = false;
    bool late_application_post_rejected = false;
    node->admit_instance_spot_idle_eviction =
      [&] (const spot_id_t &spot_id, std::string_view spot_name, std::uint64_t object_generation,
           std::uint64_t authority_owner_generation, std::function<void ()> close_local) {
          admission_called = true;
          if (spot_id != "instance-1" || spot_name != "instance-player" || object_generation != 7
              || authority_owner_generation != 11)
              return false;
          late_application_post_rejected =
            !context->try_post_serial ("late-idle-eviction-application", [] {});
          close_local ();
          return true;
      };
    // Drains before the locals above are destroyed; the close turn references them.
    scope_exit_t drain_on_exit ([&] { executor->drain (); });

    spot_node_runtime_t runtime (node);
    set_last_application_work (std::chrono::milliseconds (100));
    runtime.evict_idle_spots ();
    if (admission_called || context->admission_sealed) {
        return false;
    }
    set_last_application_work (std::chrono::seconds (2));
    runtime.evict_idle_spots ();
    executor->drain ();

    const bool result = admission_called && late_application_post_rejected && closing_called
                        && location_visible_while_closing
                        && closing_reason == spot_close_reason_t::idle_evicted && context->closed
                        && !context->node && !context->spot_instance
                        && node->spot_contexts_by_id.empty () && node->spot_ids_by_name.empty ()
                        && node->spot_names_by_id.empty ();
    return result;
}
bool verify_explicit_instance_spot_close_releases_authority_after_callback ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace service = zlink::framework::runtime::host;

    auto node = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::spot_node_builder_state_t> ("instance-explicit-close-node");
    auto context = std::make_shared<spot_context_state_t> ();
    context->node = node;
    auto executor = std::make_shared<runtime::offload_executor_t> (1);
    context->serial_executor = executor;
    context->serial_queue = std::make_shared<runtime::serial_execution_queue_t> (
      *executor, runtime::serial_execution_queue_options_t{});
    context->spot_id = "instance-explicit-close";
    context->spot_name = "instance-player";
    context->lifecycle_domain = spot_lifecycle_domain_t::instance ();
    context->object_generation = 13;
    context->authority_owner_generation = 17;
    context->spot_instance = std::make_shared<int> (1);

    std::vector<std::string> order;
    int begin_calls = 0;
    int completion_calls = 0;
    context->lifecycle.on_closing = [&] (void *, const spot_closing_context_t &closing,
                                         std::stop_token) {
        if (closing.reason != spot_close_reason_t::explicit_close
            || !node->spot_contexts_by_id.contains (context->spot_id)) {
            order.push_back ("invalid-local-cleanup");
            return task_t<void> (result_t<void>::success ());
        }
        order.push_back ("local-cleanup");
        return task_t<void> (result_t<void>::success ());
    };
    node->begin_instance_spot_close =
      [&] (const spot_id_t &spot_id, std::string_view stable_type, std::uint64_t object_generation,
           std::uint64_t authority_owner_generation) -> task_t<service::spot_close_commit_t> {
        using commit_t = service::spot_close_commit_t;
        ++begin_calls;
        if (spot_id != context->spot_id || stable_type != context->spot_name
            || object_generation != context->object_generation
            || authority_owner_generation != context->authority_owner_generation) {
            return task_t<commit_t> (result_t<commit_t>::success (commit_t{}));
        }
        order.push_back ("authority-closing");
        return task_t<commit_t> (result_t<commit_t>::success (
          commit_t{result_t<bool>::success (true), [&] {
                       ++completion_calls;
                       order.push_back ("authority-released");
                       return task_t<bool> (result_t<bool>::success (true));
                   }}));
    };

    node->spot_ids_by_name.emplace (context->spot_name, context->spot_id);
    node->spot_names_by_id.emplace (context->spot_id, context->spot_name);
    node->spot_contexts_by_id.emplace (context->spot_id, spot_context_access_t::create (context));

    if (!context->admit (true)) {
        return false;
    }
    auto public_context = spot_context_access_t::create (context);
    auto close = public_context.close ();

    context->leave_callback ();
    context->drain_serial ();

    const auto result = close.result ();
    return result && result.value () && begin_calls == 1 && completion_calls == 1 && context->closed
           && !context->node && !context->spot_instance && node->spot_contexts_by_id.empty ()
           && node->spot_ids_by_name.empty () && node->spot_names_by_id.empty ()
           && order
                == std::vector<std::string>{"authority-closing", "local-cleanup",
                                            "authority-released"};
}

// §7 pending intent는 Release 뒤 재배치하지 않고 원 terminal에서 끝난다.
bool verify_close_terminal_for_operational_boundary ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace host = zlink::framework::runtime::host;
    for (int boundary = 0; boundary != 3; ++boundary) {
        auto node = zlink::framework::test::runtime_failure_fixture<
          zlink::framework::detail::spot_node_builder_state_t> ("close-terminal-node");
        node->drain_flag = std::make_shared<std::atomic_bool> (false);
        auto state = std::make_shared<spot_context_state_t> ();
        state->node = node;
        auto executor = std::make_shared<runtime::offload_executor_t> (1);
        state->serial_executor = executor;
        state->serial_queue = std::make_shared<runtime::serial_execution_queue_t> (
          *executor, runtime::serial_execution_queue_options_t{});
        state->spot_id = "close-terminal-spot";
        state->spot_name = "close-terminal-type";
        state->lifecycle_domain = spot_lifecycle_domain_t::instance ();
        state->object_generation = 1;
        state->authority_owner_generation = 1;
        state->spot_instance = std::make_shared<int> (1);
        int releases = 0;
        int reincarnations = 0;
        state->lifecycle.on_closing = [&, state] (void *, const spot_closing_context_t &,
                                                  std::stop_token) {
            node->host_lifecycle = [boundary] {
                return host_lifecycle_snapshot_t{boundary == 0 ? framework_runtime_state_t::draining
                                                 : boundary == 1
                                                   ? framework_runtime_state_t::relocating
                                                   : framework_runtime_state_t::relocated};
            };
            node->drain_flag->store (boundary == 0);
            state->relocation_boundary_active = boundary != 0;
            state->admission_sealed = boundary == 2;
            return task_t<void> (result_t<void>::success ());
        };
        node->begin_instance_spot_close = [&] (const spot_id_t &, std::string_view, std::uint64_t,
                                               std::uint64_t) {
            return task_t<host::spot_close_commit_t> (
              result_t<host::spot_close_commit_t>::success (host::spot_close_commit_t{
                result_t<bool>::success (true),
                [&] {
                    ++releases;
                    return task_t<bool> (result_t<bool>::success (true));
                },
                [&] {
                    ++reincarnations;
                    return task_t<authority_snapshot_t> (result_t<authority_snapshot_t>::failure (
                      framework_error_kind_t::internal_failure, "unexpected Reincarnate"));
                }}));
        };
        node->spot_contexts_by_id.emplace (state->spot_id, spot_context_access_t::create (state));
        using terminal_t = runtime::serial_execution_queue_t::async_completion_t;
        std::promise<terminal_t> entered;
        auto active = entered.get_future ();
        if (!state->serial_queue->try_post_async ("active-before-close", [&] (terminal_t terminal) {
                entered.set_value (std::move (terminal));
            }))
            return false;
        auto finish_active = active.get ();
        // Finishes the active turn and drains before the locals above are destroyed.
        scope_exit_t drain_on_exit ([&] {
            finish_active ([] {});
            state->drain_serial ();
        });
        auto close = spot_context_access_t::create (state).close ();
        auto record = std::make_shared<instance_spot_retained_message_t> ();
        record->completion = std::make_shared<task_completion_source_t<zlink::message_t>> ();
        runtime::serial_work_options_t options;
        options.retained_message = record;
        bool terminal_observed = false;
        const auto expected = boundary == 0 ? framework_error_kind_t::shutting_down
                                            : framework_error_kind_t::unavailable;
        if (!state->serial_queue->try_post_async (
              "pending-intent",
              [&, record] (terminal_t terminal) {
                  auto result = record->completion->task ();
                  terminal_observed = result.await_ready () && !result.result ()
                                      && result.result ().error_kind () == expected;
                  terminal ([] {});
              },
              std::move (options)))
            return false;
        finish_active ([] {});
        state->drain_serial ();
        const auto result = close.result ();
        if (!result || !result.value () || !terminal_observed || releases != 1
            || reincarnations != 0 || !node->spot_contexts_by_id.empty ())
            return false;
    }
    return true;
}

// Store 대기 동안 turn을 반환하며, 미실행 application은 Close의 lifecycle
// operation 뒤에서 실행된다(Spot address messaging §7·§9).
bool verify_spot_close_returns_turn_while_store_step_is_pending ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    using namespace std::chrono_literals;
    namespace service = zlink::framework::runtime::host;
    using commit_t = service::spot_close_commit_t;

    auto node = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::spot_node_builder_state_t> ("close-store-step-node");
    auto context = std::make_shared<spot_context_state_t> ();
    context->node = node;
    auto executor = std::make_shared<runtime::offload_executor_t> (2);
    context->serial_executor = executor;
    context->serial_queue = std::make_shared<runtime::serial_execution_queue_t> (
      *executor, runtime::serial_execution_queue_options_t{});
    context->spot_id = "close-store-step";
    context->spot_name = "instance-player";
    context->lifecycle_domain = spot_lifecycle_domain_t::instance ();
    context->object_generation = 5;
    context->authority_owner_generation = 9;
    context->spot_instance = std::make_shared<int> (1);
    std::atomic_int closing_calls{0};
    std::atomic_bool application_ran{false};
    std::vector<std::pair<std::string, bool>> order;
    context->lifecycle.on_closing = [&] (void *, const spot_closing_context_t &, std::stop_token) {
        ++closing_calls;
        order.emplace_back ("onClosing", application_ran.load ());
        return task_t<void> (result_t<void>::success ());
    };
    auto step = std::make_shared<task_completion_source_t<commit_t>> ();
    std::promise<void> step_started;
    auto started = step_started.get_future ();
    node->begin_instance_spot_close = [&] (const spot_id_t &, std::string_view, std::uint64_t,
                                           std::uint64_t) {
        step_started.set_value ();
        return step->task ();
    };
    node->spot_ids_by_name.emplace (context->spot_name, context->spot_id);
    node->spot_names_by_id.emplace (context->spot_id, context->spot_name);
    node->spot_contexts_by_id.emplace (context->spot_id, spot_context_access_t::create (context));

    std::promise<result_t<bool>> closed;
    auto close_result = closed.get_future ();
    context->request_close ({}, [&] (result_t<bool> result) { closed.set_value (result); });
    started.get ();
    // Settles the store step and drains before the locals above are destroyed.
    scope_exit_t drain_on_exit ([&] {
        (void) step->complete (result_t<commit_t>::success (
          commit_t{result_t<bool>::success (true),
                   [] { return task_t<bool> (result_t<bool>::success (true)); }}));
        context->drain_serial ();
    });

    // 이 raw application 항목은 이미 실행 중인 handler의 resume capability가 아니다.
    if (!context->serial_queue->try_post ("application-while-close-waits", [&] {
            application_ran = true;
            order.emplace_back ("application", true);
        }))
        return false;
    const bool application_held =
      !application_ran.load ()
      && context->serial_queue->pending_count (runtime::serial_work_lane_t::application) == 1;
    const bool close_still_pending =
      close_result.wait_for (0ms) == std::future_status::timeout && closing_calls == 0;

    step->complete (result_t<commit_t>::success (
      commit_t{result_t<bool>::success (true), [&] {
                   order.emplace_back ("authorityReleased", application_ran.load ());
                   return task_t<bool> (result_t<bool>::success (true));
               }}));
    if (close_result.wait_for (5s) != std::future_status::ready) {
        return false;
    }
    const auto result = close_result.get ();
    context->drain_serial ();
    return application_held && close_still_pending && result && result.value ()
           && closing_calls == 1 && context->closed && node->spot_contexts_by_id.empty ()
           && order
                == (std::vector<std::pair<std::string, bool>>{
                  {"onClosing", false}, {"authorityReleased", false}, {"application", true}});
}
}
