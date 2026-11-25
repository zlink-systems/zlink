/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/request_admission_fixture.hpp"
#include "runtime/actors/actor_client.hpp"
#include "../support/m6b_cases.hpp"
#include "../support/m6b_stateful_fixture.hpp"
#include "runtime/execution/actor_execution_context.hpp"
#include "runtime/stateful/stateful_object_runtime.hpp"
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mesh = zlink::framework::runtime::mesh;
namespace stateful = zlink::framework::runtime::stateful;
using namespace std::chrono_literals;

namespace zlink::framework::test::m6b
{
class execution_mode_spot_t final : public zlink::framework::spot_t<zlink::framework::actor_t>
{
  public:
    explicit execution_mode_spot_t (zlink::framework::spot_context_t context) :
        _context (std::move (context))
    {
    }

    zlink::framework::spot_context_t &context () noexcept override { return _context; }
    const zlink::framework::spot_context_t &context () const noexcept override { return _context; }
    void configure () override {}
    zlink::framework::task_t<zlink::framework::spot_actor_join_result_t>
    on_actor_join (std::string_view, const zlink::framework::message_t &) override
    {
        co_return zlink::framework::spot_actor_join_result_t::reject ();
    }
    zlink::framework::task_t<void> on_actor_joined (zlink::framework::actor_t &) override
    {
        co_return;
    }
    zlink::framework::task_t<void> on_leave_actor (zlink::framework::actor_t &) override
    {
        co_return;
    }

  private:
    zlink::framework::spot_context_t _context;
};

class recording_actor_client_t final : public zlink::framework::actor_client_t
{
  public:
    std::atomic_int request_submissions{0};
    std::atomic_bool delay_next_request{false};
    zlink::framework::task_completion_source_t<zlink::framework::message_t> delayed_request;

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
        request_submissions.fetch_add (1);
        if (delay_next_request.exchange (false))
            return zlink::framework::test::accepted_request_task (delayed_request.task ());
        return zlink::framework::request_task_t<zlink::framework::message_t> (
          zlink::framework::result_t<zlink::framework::message_t>::success (
            zlink::framework::message_t{}));
    }

    zlink::framework::serializer_registry_t &actor_client_serializers () override
    {
        return serializers;
    }

  private:
    zlink::framework::serializer_registry_t serializers;
};

// A handler awaits its Yield: the Spot gate is released where it suspends.
zlink::framework::task_t<zlink::framework::message_t>
await_other_actor_yield (recording_actor_client_t &client)
{
    using namespace zlink::framework;
    actor_request_call_t request (client, actor_id_t ("actor-2"), "OtherRequest", message_t{});
    co_return co_await request.yield_message ();
}

zlink::framework::task_t<zlink::framework::message_t>
request_after_actor_await (recording_actor_client_t &client)
{
    using namespace zlink::framework;
    actor_request_call_t other (client, actor_id_t ("other"), "OtherRequest", message_t{});
    try {
        (void) co_await other.async_message ();
    }
    catch (const framework_exception_t &error) {
        co_return result_t<message_t>::failure (error.kind (), error.what ());
    }
    actor_request_call_t self (client, actor_id_t ("actor-1"), "SelfRequest", message_t{});
    try {
        co_return co_await self.async_message ();
    }
    catch (const framework_exception_t &error) {
        co_return result_t<message_t>::failure (error.kind (), error.what ());
    }
}
void verify_user_spot_execution_mode_registration ()
{
    using namespace zlink::framework;

    spot_node_builder_t builder;
    const auto factory = [] (spot_context_t context) {
        return std::make_shared<execution_mode_spot_t> (std::move (context));
    };
    builder.add_spot_factory<execution_mode_spot_t> (
      "wide", factory, [] (auto &configuration) { configuration.disable_relocation (); });
    builder.add_spot_factory<execution_mode_spot_t> ("actors", factory, [] (auto &configuration) {
        configuration.set_execution_mode (user_spot_execution_mode_t::per_actor);
        configuration.recreate_on_relocation ();
    });

    const auto snapshot = builder.snapshot ();
    assert (snapshot.spot_execution_modes.at ("wide") == user_spot_execution_mode_t::spot_wide);
    assert (snapshot.spot_execution_modes.at ("actors") == user_spot_execution_mode_t::per_actor);
}

void verify_self_actor_request_rejected_before_submission ()
{
    using namespace zlink::framework;

    recording_actor_client_t actor_client;
    const actor_ref_t actor =
      detail::actor_ref_access_t::make (node_rid_t::from_string ("node"), "player", "actor-1", 1);
    runtime::actor_execution_scope_t scope ("player:actor-1", "spot-1");
    actor_request_call_t async_request (actor_client, actor.actor_id (), "SelfRequest",
                                        message_t{});

    const auto async_result = async_request.async_message ().result ();
    assert (!async_result);
    assert (async_result.error_kind () == framework_error_kind_t::invalid_operation);
    actor_request_call_t yield_request (actor_client, actor.actor_id (), "SelfRequest",
                                        message_t{});
    const auto yield_result = yield_request.yield_message ().result ();
    assert (!yield_result);
    assert (yield_result.error_kind () == framework_error_kind_t::invalid_operation);
    assert (actor_client.request_submissions.load () == 0);

    runtime::offload_executor_t executor (2);
    runtime::serial_execution_queue_t queue (executor, runtime::serial_execution_queue_options_t{},
                                             {}, runtime::serial_lane_policy_t::spot_wide ());
    std::mutex gate;
    std::condition_variable changed;
    bool yield_checked = false;
    bool release_probe = false;
    bool sibling_ran = false;
    assert (queue.try_post ("self-actor-yield", [&] {
        runtime::actor_execution_scope_t turn_scope ("player:actor-1", "spot-1");
        assert (runtime::actor_request_requires_current_spot_gate ("spot-1", false));
        assert (!runtime::actor_request_requires_current_spot_gate ("spot-1", true));
        assert (!runtime::actor_request_requires_current_spot_gate ("spot-2", false));
        actor_request_call_t request (actor_client, actor.actor_id (), "SelfRequest", message_t{});
        const auto result = request.yield_message ().result ();
        assert (!result);
        assert (result.error_kind () == framework_error_kind_t::invalid_operation);
        std::unique_lock lock (gate);
        yield_checked = true;
        changed.notify_all ();
        changed.wait (lock, [&] { return release_probe; });
    }));
    assert (queue.try_post ("self-actor-sibling", [&] {
        std::lock_guard lock (gate);
        sibling_ran = true;
        changed.notify_all ();
    }));
    {
        std::unique_lock lock (gate);
        assert (changed.wait_for (lock, 1s, [&] { return yield_checked; }));
        assert (!sibling_ran);
        release_probe = true;
    }
    changed.notify_all ();
    queue.drain ();
    assert (sibling_ran);
    assert (actor_client.request_submissions.load () == 0);
}

void verify_actor_context_survives_coroutine_await ()
{
    using namespace zlink::framework;

    runtime::offload_executor_t executor (1);
    runtime::serial_execution_queue_t queue (executor);
    recording_actor_client_t actor_client;
    actor_client.delay_next_request.store (true);
    std::optional<task_t<message_t>> operation;
    queue.run ("actor-context-await", [&] {
        runtime::actor_execution_scope_t scope ("player:actor-1", "spot-1");
        operation.emplace (request_after_actor_await (actor_client));
    });
    assert (operation.has_value ());
    actor_client.delayed_request.complete (result_t<message_t>::success (message_t{}));
    const auto &result = operation->result ();
    assert (!result);
    assert (result.error_kind () == framework_error_kind_t::invalid_operation);
    assert (actor_client.request_submissions.load () == 1);
    queue.close ();
}

void verify_actor_yield_releases_spot_gate_before_reply ()
{
    using namespace zlink::framework;

    runtime::offload_executor_t executor (2);
    runtime::serial_execution_queue_t queue (executor, runtime::serial_execution_queue_options_t{},
                                             {}, runtime::serial_lane_policy_t::spot_wide ());
    recording_actor_client_t actor_client;
    actor_client.delay_next_request.store (true);
    std::mutex gate;
    std::condition_variable changed;
    bool sibling_ran = false;
    bool request_completed = false;
    std::shared_ptr<task_t<message_t>> request_task;

    assert (queue.try_post_async ("different-member-actor-yield", [&] (auto complete) {
        runtime::actor_execution_scope_t actor_scope ("player:actor-1", "spot-1");
        request_task = std::make_shared<task_t<message_t>> (await_other_actor_yield (actor_client));
        detail::observe_task_completion (
          *request_task,
          [&, request_task, complete = std::move (complete)] (const auto &result) mutable {
              assert (result);
              {
                  std::lock_guard lock (gate);
                  request_completed = true;
              }
              changed.notify_all ();
              complete ([] {});
          });
    }));
    assert (queue.try_post ("same-spot-sibling", [&] {
        std::lock_guard lock (gate);
        sibling_ran = true;
        changed.notify_all ();
    }));
    {
        std::unique_lock lock (gate);
        assert (changed.wait_for (lock, 1s, [&] { return sibling_ran; }));
        assert (!request_completed);
    }
    assert (actor_client.request_submissions.load () == 1);
    actor_client.delayed_request.complete (result_t<message_t>::success (message_t{}));
    {
        std::unique_lock lock (gate);
        assert (changed.wait_for (lock, 1s, [&] { return request_completed; }));
    }
    queue.drain ();
}

void verify_same_gate_request_rejected_before_submission ()
{
    using namespace zlink::framework;

    std::atomic_int submissions = 0;
    request_call_t<int> request (
      "SameGate",
      [&submissions] (const auto &, auto, const auto &) {
          submissions.fetch_add (1);
          return request_task_t<int> (result_t<int>::success (1));
      },
      [] (bool) {
          return result_t<void>::failure (
            framework_error_kind_t::invalid_operation,
            "awaited request requires the current Spot execution gate");
      });

    const auto result = request.async ().result ();
    assert (!result);
    assert (result.error_kind () == framework_error_kind_t::invalid_operation);
    assert (submissions.load () == 0);
}
void verify_membership_turns_and_independent_infrastructure ()
{
    stateful::stateful_object_runtime_t runtime;
    runtime.replace_placement_candidates (
      {{"mesh-a", "node-a", {"player", "room"}, 100, 16, 0, 8, 0},
       {"mesh-b", "node-b", {"player", "room"}, 100, 16, 0, 8, 0}});
    const auto actor = create_ready (runtime, {stateful::object_kind_t::actor,
                                               "actor-turn",
                                               "player",
                                               std::string ("mesh-a"),
                                               {},
                                               false,
                                               false});
    const auto spot = create_ready (runtime, {stateful::object_kind_t::user_spot,
                                              "spot-global",
                                              "room",
                                              std::string ("mesh-b"),
                                              {},
                                              false,
                                              false});

    assert (runtime.enqueue (actor, stateful::turn_domain_t::application, {50, {50}})
            == stateful::stateful_error_t::none);
    assert (runtime.register_timer (actor, {7, 1000, 1000, 60})
            == stateful::stateful_error_t::none);
    const auto [prepare_error, token] =
      runtime.begin_membership_move_async (actor, spot).result ().value ();
    assert (prepare_error == stateful::stateful_error_t::none);
    assert (runtime.enqueue (actor, stateful::turn_domain_t::application, {51, {51}})
            == stateful::stateful_error_t::none);
    assert (runtime.enqueue_timer_tick (actor, 7, {60}) == stateful::stateful_error_t::none);
    const auto [commit_error, moved_actor] =
      runtime.commit_membership_move_async (token).result ().value ();
    assert (commit_error == stateful::stateful_error_t::none);
    assert (moved_actor.object_generation == actor.object_generation);
    assert (moved_actor.authority_owner_generation == actor.authority_owner_generation + 1);
    assert (runtime.actor_membership (moved_actor) == std::optional<std::string> ("spot-global"));
    assert (runtime.timers (moved_actor)
            == std::vector<stateful::logical_timer_t> ({{7, 1000, 1000, 61}}));
    assert (runtime.close_spot (spot) == std::pair (stateful::stateful_error_t::none, false));

    for (const auto expected : {50u, 51u, 60u}) {
        const auto [queued_error, queued] =
          runtime.try_claim (moved_actor, stateful::turn_domain_t::application);
        assert (queued_error == stateful::stateful_error_t::none);
        assert (queued && queued->sequence == expected);
        assert (runtime.complete_claim (moved_actor, stateful::turn_domain_t::application)
                == stateful::stateful_error_t::none);
    }

    assert (runtime.enqueue (moved_actor, stateful::turn_domain_t::application, {1, {1}})
            == stateful::stateful_error_t::none);
    assert (runtime.enqueue (moved_actor, stateful::turn_domain_t::application, {2, {2}})
            == stateful::stateful_error_t::none);
    assert (runtime.enqueue (moved_actor, stateful::turn_domain_t::infrastructure, {9, {9}})
            == stateful::stateful_error_t::none);

    const auto [first_error, first] =
      runtime.try_claim (moved_actor, stateful::turn_domain_t::application);
    assert (first_error == stateful::stateful_error_t::none);
    assert (first && first->sequence == 1);

    const auto [infra_error, infrastructure] =
      runtime.try_claim (moved_actor, stateful::turn_domain_t::infrastructure);
    assert (infra_error == stateful::stateful_error_t::none);
    assert (infrastructure && infrastructure->sequence == 9);
    assert (runtime.complete_claim (moved_actor, stateful::turn_domain_t::infrastructure)
            == stateful::stateful_error_t::none);

    assert (runtime.yield_claim (moved_actor, {3, {3}}) == stateful::stateful_error_t::none);
    const auto [second_error, second] =
      runtime.try_claim (moved_actor, stateful::turn_domain_t::application);
    assert (second_error == stateful::stateful_error_t::none);
    assert (second && second->sequence == 3);
    assert (runtime.complete_claim (moved_actor, stateful::turn_domain_t::application)
            == stateful::stateful_error_t::none);
    const auto [continuation_error, continuation] =
      runtime.try_claim (moved_actor, stateful::turn_domain_t::application);
    assert (continuation_error == stateful::stateful_error_t::none);
    assert (continuation && continuation->sequence == 2);
    assert (runtime.complete_claim (moved_actor, stateful::turn_domain_t::application)
            == stateful::stateful_error_t::none);
}
}
