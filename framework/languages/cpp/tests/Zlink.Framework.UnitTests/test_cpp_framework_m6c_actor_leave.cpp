/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/m6c_runtime_cases.hpp"
#include "../support/m6c_spot_turn.hpp"
#include "../support/m6c_task.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "../support/telemetry_observation.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include "runtime/timers/timer_runtime.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <typeindex>
#include <typeinfo>
#include <utility>

namespace m6c_test
{

using namespace zlink::framework::runtime::stateful;

void test_remote_actor_leave_commits_before_source_callback (test_context_t &test)
{
    namespace detail = zlink::framework::detail;
    namespace runtime = zlink::framework::runtime;
    const auto owner = zlink::framework::node_rid_t::from_string ("remote-leave-owner");
    const auto actor_ref = detail::actor_ref_access_t::make (owner, "remote_actor", "actor-1", 1);
    const auto committed_actor_ref = detail::actor_ref_access_t::make (
      zlink::framework::node_rid_t::from_string ("remote-leave-committed-owner"), "remote_actor",
      "actor-1", 1);
    detail::actor_gateway_runtime_t gateway;
    struct test_actor_t : zlink::framework::actor_t
    {
        explicit test_actor_t (zlink::framework::actor_context_t context) :
            state (std::move (context))
        {
        }
        zlink::framework::actor_context_t &context () noexcept override { return state; }
        const zlink::framework::actor_context_t &context () const noexcept override
        {
            return state;
        }
        void set_actor_ref (const zlink::framework::actor_ref_t &ref) { committed_ref = ref; }
        zlink::framework::actor_context_t state;
        std::optional<zlink::framework::actor_ref_t> committed_ref;
    } actor (gateway.actor_context (actor_ref));
    auto node = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::spot_node_builder_state_t> ("remote-leave-source");
    node->dispatch.message_flow (zlink::framework::message_flow_log_mode_t::normal);
    zlink::framework::logging_builder_t logging;
    logging.use_provider ("remote-leave-test", [] (const zlink::framework::log_record_t &record) {
        std::clog << "remote-leave-flow ";
        for (const auto &field : record.fields)
            std::clog << field.key << '=' << field.value << ' ';
        std::clog << '\n';
    });
    detail::dispatch_options_access_t::set_logger (node->dispatch,
                                                   logging.create_logger ("remote-leave-test"));
    auto source = std::make_shared<detail::spot_context_state_t> ();
    source->node = node;
    source->node_rid = zlink::framework::node_rid_t::from_string ("remote-leave-source");
    source->spot_id = "source";
    source->spot_instance = std::make_shared<int> (1);
    source->serial_executor = std::make_shared<runtime::offload_executor_t> (2, "remote-leave");
    source->serial_queue = std::make_shared<runtime::serial_execution_queue_t> (
      *source->serial_executor, runtime::serial_execution_queue_options_t{},
      runtime::serial_execution_queue_t::error_handler_t{},
      runtime::serial_lane_policy_t::spot_wide ());
    std::atomic_bool committed{false};
    std::atomic_bool callback_after_commit{false};
    std::atomic_int callbacks{0};
    auto join_completion =
      std::make_shared<zlink::framework::task_completion_source_t<detail::actor_join_reply_t>> ();
    source->actor_admissions[std::type_index (typeid (test_actor_t))].on_leave_actor =
      [&] (void *, void *) {
          callback_after_commit.store (committed.load ());
          callbacks.fetch_add (1);
          return zlink::framework::task_t<void> (zlink::framework::result_t<void>::failure (
            zlink::framework::framework_error_kind_t::internal_failure,
            "remote source leave callback failed after membership commit"));
      };
    node->lane
      .run_checked ([&] {
          node->actor_spot_ids.emplace ("remote_actor:actor-1", source->spot_id);
          node->actor_generations.emplace ("remote_actor:actor-1", 1);
          source->actor_count = 1;
          node->actor_instances.emplace ("remote_actor:actor-1",
                                         std::shared_ptr<void> (&actor, [] (void *) {}));
      })
      .get ();
    gateway.on_join_entry_spot (
      [&] (const auto &, const auto &, auto) { return join_completion->task (); });
    auto context = detail::spot_context_access_t::create (source);
    auto leaving = context.leave_actor (actor);
    const auto source_retained = node->lane
                                   .run ([&] {
                                       return node->actor_spot_ids.contains ("remote_actor:actor-1")
                                              && source->actor_count == 1;
                                   })
                                   .get ();
    test.require (!leaving.await_ready () && source_retained && callbacks.load () == 0
                    && !actor.committed_ref,
                  "remote leave must retain source membership and callback until entry commit");
    committed.store (true);
    join_completion->complete (zlink::framework::result_t<detail::actor_join_reply_t>::success (
      detail::actor_join_reply_t{.actor = committed_actor_ref}));
    const auto left = leaving.result ();
    const auto completed =
      wait_until_bounded ([&] { return callbacks.load () == 1; }, std::chrono::seconds (1));
    const auto source_removed = node->lane
                                  .run ([&] {
                                      return !node->actor_spot_ids.contains ("remote_actor:actor-1")
                                             && source->actor_count == 0;
                                  })
                                  .get ();
    test.require (left && completed && callback_after_commit.load () && source_removed
                    && actor.context ().actor_ref ().node_rid ().value ()
                         == committed_actor_ref.node_rid ().value ()
                    && actor.committed_ref
                    && actor.committed_ref->node_rid ().value ()
                         == committed_actor_ref.node_rid ().value (),
                  "remote source leave callback must run after entry membership commit");

    callbacks.store (0);
    join_completion =
      std::make_shared<zlink::framework::task_completion_source_t<detail::actor_join_reply_t>> ();
    node->lane
      .run_checked ([&] {
          node->actor_spot_ids.emplace ("remote_actor:actor-1", source->spot_id);
          node->actor_generations.emplace ("remote_actor:actor-1", 1);
          source->actor_count = 1;
      })
      .get ();
    auto failed_leave = context.leave_actor (actor);
    join_completion->complete (zlink::framework::result_t<detail::actor_join_reply_t>::failure (
      zlink::framework::framework_error_kind_t::internal_failure, "entry commit failed"));
    const auto failed = failed_leave.result ();
    const auto failure_retained_source =
      node->lane
        .run ([&] {
            return node->actor_spot_ids.contains ("remote_actor:actor-1")
                   && source->actor_count == 1;
        })
        .get ();
    test.require (
      !failed && failed.error_kind () == zlink::framework::framework_error_kind_t::internal_failure
        && failure_retained_source && callbacks.load () == 0,
      "failed remote entry commit must retain source membership and skip leave callback");
    source->serial_queue->close ();
    source->serial_queue->drain ();
    source->serial_executor->drain ();
}

void test_actor_leave_after_relocation_defer_runs_lifecycle_callbacks (test_context_t &test)
{
    namespace detail = zlink::framework::detail;
    namespace runtime = zlink::framework::runtime;
    using zlink::framework::actor_ref_t;
    using zlink::framework::node_rid_t;
    using zlink::framework::spot_context_t;
    using zlink::framework::spot_id_t;
    using zlink::framework::spot_relocation_coordination_mode_t;
    using zlink::framework::user_spot_execution_mode_t;

    struct test_actor_t
    {
        std::optional<zlink::framework::actor_context_t> state;
        zlink::framework::actor_context_t &context () noexcept { return *state; }
    } actor;

    const auto node = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::spot_node_builder_state_t> ("actor-leave-after-defer-node");
    const auto node_rid = node_rid_t::from_string ("actor-leave-after-defer-node");
    const auto source_id = spot_id_t ("source-spot");
    const auto entry_id = spot_id_t ("entry-spot");
    node->lane
      .run_checked ([&] {
          node->snapshot.entry_spot_name = "entry";
          node->spot_ids_by_name.emplace ("entry", entry_id);
      })
      .get ();

    const auto make_state = [&] (spot_id_t spot_id, std::string spot_name, bool entry_spot) {
        auto state = std::make_shared<detail::spot_context_state_t> ();
        state->node = node;
        state->node_rid = node_rid;
        state->spot_id = std::move (spot_id);
        state->spot_name = std::move (spot_name);
        state->lifecycle_domain = entry_spot ? detail::spot_lifecycle_domain_t::entry ()
                                             : detail::spot_lifecycle_domain_t::user ();
        state->execution_mode = user_spot_execution_mode_t::spot_wide;
        state->relocation_coordination_mode =
          spot_relocation_coordination_mode_t::application_signaled;
        state->serial_executor =
          std::make_shared<runtime::offload_executor_t> (2, "actor-leave-after-defer");
        state->serial_queue = std::make_shared<runtime::serial_execution_queue_t> (
          *state->serial_executor, runtime::serial_execution_queue_options_t{},
          runtime::serial_execution_queue_t::error_handler_t{},
          runtime::serial_lane_policy_t::spot_wide ());
        state->spot_instance = std::make_shared<int> (1);
        return state;
    };

    const auto source = make_state (source_id, "source", false);
    const auto entry = make_state (entry_id, "entry", true);
    std::atomic_int leave_callbacks{0};
    std::atomic_int joined_callbacks{0};
    std::atomic_bool leave_saw_committed_entry{false};
    source->on_leave_actor_callbacks[std::type_index (typeid (test_actor_t))] = [&] (void *,
                                                                                     void *) {
        leave_saw_committed_entry.store (
          node->lane
            .run ([&] {
                const auto found = node->actor_spot_ids.find ("test_actor:actor-1");
                return found != node->actor_spot_ids.end () && found->second == entry_id;
            })
            .get (),
          std::memory_order_release);
        leave_callbacks.fetch_add (1, std::memory_order_acq_rel);
        return zlink::framework::task_t<void> (zlink::framework::result_t<void>::failure (
          zlink::framework::framework_error_kind_t::internal_failure,
          "source leave callback failed after membership commit"));
    };
    entry->on_actor_joined_callbacks[std::type_index (typeid (test_actor_t))] = [&] (void *,
                                                                                     void *) {
        joined_callbacks.fetch_add (1, std::memory_order_acq_rel);
        return zlink::framework::task_t<void> (zlink::framework::result_t<void>::success ());
    };
    auto context = detail::spot_context_access_t::create (source);

    const auto actor_ref =
      ::zlink::framework::detail::actor_ref_access_t::make (node_rid, "test_actor", "actor-1", 1);
    detail::actor_gateway_runtime_t gateway;
    actor.state.emplace (gateway.actor_context (actor_ref));
    const std::string key = "test_actor:actor-1";
    node->lane
      .run_checked ([&] {
          node->spot_contexts_by_id.emplace (entry_id,
                                             detail::spot_context_access_t::create (entry));
          node->actor_spot_ids.emplace (key, source_id);
          node->actor_generations.emplace (key, actor_ref.object_generation ());
          node->actor_created_keys.emplace (key);
          node->actor_instances.emplace (
            key, std::shared_ptr<void> (std::addressof (actor), [] (void *) {}));
          node->actor_instance_index.emplace (std::addressof (actor),
                                              std::make_pair ("test_actor", "actor-1"));
          source->actor_count = 1;
      })
      .get ();

    const auto submitted = source->run_serial_sync ("actor-leave-with-relocation-fence", [&] {
        const auto left = context.leave_actor (actor).result ();
        if (!left) {
            throw std::runtime_error ("actor leave was not accepted from the handler turn");
        }
        context.relocation_ready ().defer ();
    });

    const auto completed = wait_until_bounded (
      [&] {
          return leave_callbacks.load (std::memory_order_acquire) == 1
                 && joined_callbacks.load (std::memory_order_acquire) == 1;
      },
      std::chrono::seconds (1));
    std::string current_location;
    std::size_t source_actor_count = 0;
    std::size_t entry_actor_count = 0;
    node->lane
      .run_checked ([&] {
          const auto found = node->actor_spot_ids.find (key);
          if (found != node->actor_spot_ids.end ()) {
              current_location = found->second;
          }
          source_actor_count = source->actor_count;
          entry_actor_count = entry->actor_count;
      })
      .get ();
    test.require (submitted && completed && current_location == entry_id && source_actor_count == 0
                    && entry_actor_count == 1
                    && leave_saw_committed_entry.load (std::memory_order_acquire),
                  "source leave callback failure must not prevent the committed entry membership "
                  "or its joined callback");
    node->lane
      .run_checked ([&] {
          node->spot_contexts_by_id.clear ();
          node->spot_ids_by_name.clear ();
          node->spot_names_by_id.clear ();
          node->actor_spot_ids.clear ();
          node->actor_generations.clear ();
          node->actor_created_keys.clear ();
          node->actor_instances.clear ();
          node->actor_instance_index.clear ();
      })
      .get ();
}

void test_actor_return_to_entry_spot_skips_admission_and_runs_lifecycle_callbacks (
  test_context_t &test, const std::shared_ptr<authority_relocation_port_t> &authority)
{
    namespace detail = zlink::framework::detail;
    namespace runtime = zlink::framework::runtime;
    using zlink::framework::node_rid_t;
    using zlink::framework::serializer_registry_t;
    using zlink::framework::spot_id_t;
    using zlink::framework::user_spot_execution_mode_t;

    struct returning_actor_t
    {
    };

    const auto run_case = [&] (bool remote, bool decline_source = false, bool cancel_source = false,
                               bool cleanup_before_cancel = false) {
        // The observer can outlive this scope, so it owns its state.
        struct leave_diagnostics_t
        {
            std::atomic_int failed{0};
            std::atomic_int wrong_correlation{0};
            std::atomic_int borrowed_context{0};
        };
        const auto diagnostics = std::make_shared<leave_diagnostics_t> ();
        const std::string case_name = remote ? "remote" : "same-node";
        const auto node = zlink::framework::test::runtime_failure_fixture<
          zlink::framework::detail::spot_node_builder_state_t> ("entry-return-" + case_name);
        const auto local_node_name = "entry-return-target-" + case_name;
        const auto local_node_rid = node_rid_t::from_string (local_node_name);
        const auto source_id = spot_id_t ("source-spot-" + case_name);
        const auto entry_id = spot_id_t ("entry-spot-" + case_name);
        node->snapshot.routing_id = zlink::routing_id_t::from (local_node_name);
        node->snapshot.entry_spot_name = "entry";
        node->spot_ids_by_name.emplace ("entry", entry_id);
        node->spot_names_by_id.emplace (source_id, "source");
        node->spot_names_by_id.emplace (entry_id, "entry");

        serializer_registry_t serializers;
        auto channel_runtime = std::make_shared<detail::channel_runtime_state_t> ();
        channel_runtime->serializers = &serializers;
        const auto make_state = [&] (spot_id_t spot_id, std::string spot_name, bool entry_spot) {
            auto state = std::make_shared<detail::spot_context_state_t> ();
            state->node = node;
            state->node_rid = local_node_rid;
            state->spot_id = std::move (spot_id);
            state->spot_name = std::move (spot_name);
            state->lifecycle_domain = entry_spot ? detail::spot_lifecycle_domain_t::entry ()
                                                 : detail::spot_lifecycle_domain_t::user ();
            state->execution_mode = user_spot_execution_mode_t::spot_wide;
            state->serial_executor =
              std::make_shared<runtime::offload_executor_t> (2, "entry-return-" + case_name);
            state->serial_queue = std::make_shared<runtime::serial_execution_queue_t> (
              *state->serial_executor, runtime::serial_execution_queue_options_t{},
              runtime::serial_execution_queue_t::error_handler_t{},
              runtime::serial_lane_policy_t::spot_wide ());
            state->spot_instance = std::make_shared<int> (1);
            state->channel_runtime = channel_runtime;
            return state;
        };

        const auto source = make_state (source_id, "source", false);
        const auto entry = make_state (entry_id, "entry", true);
        std::atomic_int leave_callbacks{0};
        std::atomic_int joined_callbacks{0};
        detail::spot_actor_admission_callbacks_t source_callbacks;
        source_callbacks.on_leave_actor = [&] (void *, void *) {
            leave_callbacks.fetch_add (1, std::memory_order_acq_rel);
            return zlink::framework::task_t<void> (zlink::framework::result_t<void>::success ());
        };
        detail::spot_actor_admission_callbacks_t entry_callbacks;
        entry_callbacks.on_actor_joined = [&] (void *, void *) {
            joined_callbacks.fetch_add (1, std::memory_order_acq_rel);
            return zlink::framework::task_t<void> (zlink::framework::result_t<void>::success ());
        };
        const auto actor_type = std::type_index (typeid (returning_actor_t));
        source->actor_admissions.emplace (actor_type, std::move (source_callbacks));
        entry->actor_admissions.emplace (actor_type, std::move (entry_callbacks));
        node->spot_contexts_by_id.emplace (source_id,
                                           detail::spot_context_access_t::create (source));
        node->spot_contexts_by_id.emplace (entry_id, detail::spot_context_access_t::create (entry));

        detail::spot_node_builder_state_t::actor_factory_registration_t registration;
        registration.actor_type = actor_type;
        registration.create_instance = [] (std::string) {
            return std::make_shared<returning_actor_t> ();
        };
        registration.configure_instance = [] (void *, const auto &, void *) {};
        node->actor_factories.emplace ("returning-actor", std::move (registration));

        const auto source_node_rid =
          remote ? node_rid_t::from_string ("entry-return-source-remote") : local_node_rid;
        const auto actor_ref =
          detail::actor_ref_access_t::make (source_node_rid, "returning-actor", "actor-1", 1);
        const std::string key = "returning-actor:actor-1";
        auto actor = std::make_shared<returning_actor_t> ();
        node->actor_instances.emplace (key, actor);
        node->actor_instance_index.emplace (actor.get (),
                                            std::make_pair ("returning-actor", "actor-1"));
        node->actor_spot_ids.emplace (key, source_id);
        node->actor_generations.emplace (key, 1);
        node->actor_created_keys.emplace (key);
        source->actor_count = 1;

        detail::spot_node_runtime_t spot_runtime (node);
        if (decline_source) {
            constexpr auto transfer_id = "entry-return-declined-leave";
            auto leave_logs = std::make_shared<
              zlink::framework::test::telemetry_observations_t<zlink::framework::log_record_t>> ();
            zlink::framework::logging_builder_t logging;
            logging.use_provider (
              "declined-leave", [leave_logs] (const auto &record) { leave_logs->record (record); });
            detail::dispatch_options_access_t::set_logger (
              node->dispatch, logging.create_logger ("declined-leave"));
            node->dispatch.message_flow (zlink::framework::message_flow_log_mode_t::errors);
            detail::dispatch_options_access_t::set_observer_for_tests (
              node->dispatch, [diagnostics, cleanup_before_cancel,
                               transfer_id] (const zlink::framework::message_flow_event_t &event) {
                  if (event.packet_name == "spot_actor_leave"
                      && event.result == zlink::framework::message_flow_result_t::failed
                      && event.outcome == zlink::framework::message_flow_outcome_t::completed
                      && event.surface == zlink::framework::dispatch_error_surface_t::spot_actor
                      && event.message_kind == zlink::framework::dispatch_message_kind_t::control
                      && !event.reason && event.exception) {
                      ++diagnostics->failed;
                      if (!(event.correlation_id == transfer_id))
                          ++diagnostics->wrong_correlation;
                      if (cleanup_before_cancel && (event.spot_id || event.actor_id))
                          ++diagnostics->borrowed_context;
                  }
              });
            const auto native_source = std::make_shared<detail::service::spot_t> (
              nullptr, object_ref_t{object_kind_t::user_spot,
                                    std::string (source_id),
                                    source->object_generation,
                                    0,
                                    {},
                                    local_node_name});
            source->native_spot = native_source;
            const object_ref_t source_ref{object_kind_t::actor, "actor-1", 1, 1, {},
                                          local_node_name};
            auto target_ref = source_ref;
            target_ref.node_id = "entry-return-next-owner";
            ++target_ref.authority_owner_generation;
            const auto target_id = spot_id_t ("entry-return-next-spot");
            const auto target_fence = runtime::protocol::actor_route_fence_t{
              source_ref.key,
              source_ref.object_generation,
              zlink::routing_id_t::from (target_ref.node_id).to_bytes (),
              source->object_generation,
              target_ref.authority_owner_generation,
              1};
            authority->publish (source_ref, target_ref, {"entry-return-next-owner", 1}, {},
                                transfer_id, 0, {});
            node->relocation_authority = authority;
            const auto cleanup_boundary = std::chrono::steady_clock::now ();
            node->pending_remote_source_cleanups.push_back (
              detail::spot_node_builder_state_t::pending_remote_source_cleanup_t{
                .source_actor = actor_ref,
                .transfer_id = transfer_id,
                .source_spot_id = source_id,
                .source_spot_generation = source->object_generation,
                .target_spot_id = target_id,
                .target_identity = {target_fence.target_node_routing_id,
                                    target_fence.target_node_generation,
                                    {},
                                    target_fence.owner_lease_generation},
                .not_before = cleanup_boundary,
                .leave_deadline = cleanup_boundary + node->message_follow_duration});
            std::optional<runtime::serial_execution_queue_t::async_completion_t> blocker;
            if (cancel_source) {
                std::promise<runtime::serial_execution_queue_t::async_completion_t> entered;
                auto started = entered.get_future ();
                const bool accepted = source->serial_queue->try_post_async (
                  "source-leave-cancellation-boundary",
                  [&] (auto complete) { entered.set_value (std::move (complete)); });
                test.require (accepted, "source cancellation fixture must hold an actual turn");
                if (!accepted)
                    return;
                blocker = started.get ();
            } else {
                source->admission_sealed = true;
            }
            const auto submitted = spot_runtime
                                     .submit_remote_actor_leave_async (
                                       transfer_id, actor_ref, source_id, source->object_generation,
                                       target_id, target_fence)
                                     .result ()
                                     .value ();
            if (cancel_source) {
                if (cleanup_before_cancel) {
                    spot_runtime.cleanup_expired_actor_admissions_at (
                      node->pending_remote_source_cleanups.front ().leave_deadline);
                    test.require (!node->actor_instances.contains (key)
                                    && node->pending_remote_source_cleanups.empty (),
                                  "fixture must remove the source record at its existing deadline");
                }
                source->serial_queue->cancel_pending ();
                (*blocker) ([] {});
                source->serial_queue->drain ();
            }
            test.require (submitted
                            && (cleanup_before_cancel
                                  ? node->pending_remote_source_cleanups.empty ()
                                  : node->pending_remote_source_cleanups.front ().leave_completed),
                          cancel_source
                            ? "source leave canceled before activation must finish retirement"
                            : "source leave declined before activation must finish retirement");
            spot_runtime.cleanup_expired_actor_admissions_at (cleanup_boundary);
            zlink::framework::test::acknowledge_delivery ();
            const auto leave_records = leave_logs->snapshot ();
            test.require (
              std::count_if (
                leave_records.begin (), leave_records.end (),
                [transfer_id] (const auto &record) {
                    const auto has = [&] (std::string_view key, std::string_view value) {
                        return std::any_of (record.fields.begin (), record.fields.end (),
                                            [&] (const auto &field) {
                                                return field.key == key && field.value == value;
                                            });
                    };
                    return has (detail::dispatch_event_field::packet, "spot_actor_leave")
                           && has (detail::dispatch_event_field::corr, transfer_id)
                           && has (detail::dispatch_event_field::error_type,
                                   typeid (zlink::framework::framework_exception_t).name ())
                           && std::any_of (
                             record.fields.begin (), record.fields.end (), [] (const auto &field) {
                                 return field.key == detail::dispatch_event_field::error_message
                                        && !field.value.empty ();
                             });
                })
                == 1,
              "declined Leave must report its owned error summary once");
            test.require (diagnostics->wrong_correlation == 0,
                          "source leave failure must retain its transfer correlation");
            test.require (diagnostics->borrowed_context == 0,
                          "removed source record must diagnose without borrowed context");
            test.require (!node->actor_instances.contains (key)
                            && node->pending_remote_source_cleanups.empty ()
                            && leave_callbacks.load (std::memory_order_acquire) == 0
                            && diagnostics->failed == 1,
                          "terminal source leave failure must remove registry and diagnose once");
            spot_runtime
              .submit_remote_actor_leave_async (transfer_id, actor_ref, source_id,
                                                source->object_generation, target_id, target_fence)
              .result ()
              .value ();
            spot_runtime.cleanup_expired_actor_admissions_at (cleanup_boundary);
            zlink::framework::test::acknowledge_delivery ();
            test.require (leave_callbacks.load (std::memory_order_acquire) == 0
                            && diagnostics->failed == 1,
                          "terminal source leave failure must not rerun callback or diagnostics");
            spot_runtime.request_stop ();
            spot_runtime.cancel_pending_dispatch ();
            spot_runtime.cancel_pending_work ();
            spot_runtime.release_native_handles ();
            return;
        }
        const auto joined =
          (remote
             ? spot_runtime.join_remote_actor_to_spot_erased (actor_ref, entry_id,
                                                              zlink::message_t{})
             : spot_runtime.join_actor_to_spot_erased (actor_ref, entry_id, zlink::message_t{}))
            .result ();
        const auto source_drained =
          run_serial_turn (source, "entry-return-source-leave-observed", [] {});
        const auto current = node->actor_spot_ids.find (key);
        test.require (joined && joined.value ().result_code == 0
                        && current != node->actor_spot_ids.end () && current->second == entry_id
                        && source->actor_count == 0 && entry->actor_count == 1
                        && joined_callbacks.load (std::memory_order_acquire) == 1 && source_drained
                        && leave_callbacks.load (std::memory_order_acquire) == 1,
                      remote ? "remote User Spot to Entry Spot return must commit without "
                               "admission and run target joined/source leave exactly once"
                             : "same-node User Spot to Entry Spot return must commit without "
                               "admission and run target joined/source leave exactly once");

        spot_runtime.request_stop ();
        spot_runtime.cancel_pending_dispatch ();
        spot_runtime.cancel_pending_work ();
        spot_runtime.release_native_handles ();
    };

    run_case (false);
    run_case (true);
    run_case (false, true);
    run_case (false, true, true);
    run_case (false, true, true, true);
}

} // namespace m6c_test
