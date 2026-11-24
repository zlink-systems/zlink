/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/actor_gateway_cases.hpp"
#include "../support/actor_gateway_fixture.hpp"
#include "../support/telemetry_observation.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include <zlink/framework/contracts/dispatch/task.hpp>
#include <zlink/framework/contracts/streams/stream.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace zlink::framework::test::actor_gateway
{
int join_completion_waits_for_bound_session_delivery_terminal ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    auto state = std::make_shared<actor_gateway_state_t> ();
    serializer_registry_t serializers;
    state->serializers = &serializers;
    actor_gateway_runtime_t gateway (state);
    const auto actor = test_actor_ref ("join-completion-node", "game.actor", "join-player", 7);
    auto delivery = std::make_shared<task_completion_source_t<void>> ();
    // The sink and settlement callbacks may run after an early return; they own this state.
    struct delivery_state_t
    {
        std::atomic_int delivery_calls{0};
        std::atomic_bool delivery_started{false};
        std::atomic_bool next_delivery_started{false};
        std::mutex mutex;
        std::condition_variable changed;
        std::optional<result_t<void>> terminal;
        std::atomic<int> terminal_calls{0};
        std::atomic<int> next_terminal_calls{0};
        std::atomic_bool next_succeeded{false};
    };
    const auto shared = std::make_shared<delivery_state_t> ();
    auto &delivery_started = shared->delivery_started;
    auto &next_delivery_started = shared->next_delivery_started;
    if (!gateway.bind_session_sink (
          actor,
          [delivery, shared] (std::string, stream_codec_t,
                              const zlink::message_t &) -> task_t<void> {
              if (shared->delivery_calls.fetch_add (1, std::memory_order_acq_rel) == 0) {
                  shared->delivery_started.store (true, std::memory_order_release);
                  co_await delivery->task ();
              } else {
                  shared->next_delivery_started.store (true, std::memory_order_release);
              }
              co_return;
          })) {
        return 1;
    }

    const auto fence = gateway.begin_join_completion_delivery_fence (actor);
    auto submitted =
      gateway.actor_context (actor).bound_session ().send (std::string ("join-response")).async ();
    if (!submitted.result ())
        return 2;

    auto &mutex = shared->mutex;
    auto &changed = shared->changed;
    auto &terminal = shared->terminal;
    auto &terminal_calls = shared->terminal_calls;
    gateway.settle_join_completion_delivery_fence (
      actor, fence, result_t<void>::success (), [shared] (result_t<void> result) {
          shared->terminal_calls.fetch_add (1, std::memory_order_relaxed);
          {
              std::lock_guard lock (shared->mutex);
              shared->terminal.emplace (std::move (result));
          }
          shared->changed.notify_all ();
          throw std::runtime_error ("delivery settlement observer failed");
      });
    const auto delivery_deadline = std::chrono::steady_clock::now () + std::chrono::seconds (1);
    while (!delivery_started.load (std::memory_order_acquire)
           && std::chrono::steady_clock::now () < delivery_deadline) {
        std::this_thread::yield ();
    }
    if (!delivery_started.load (std::memory_order_acquire))
        return 4;
    {
        std::unique_lock lock (mutex);
        if (changed.wait_for (lock, std::chrono::milliseconds (25),
                              [&] { return terminal.has_value (); })) {
            return 3;
        }
    }
    auto next_submitted = gateway.actor_context (actor)
                            .bound_session ()
                            .send (std::string ("after-settlement-observer"))
                            .async ();
    if (!next_submitted.result () || !next_delivery_started.load (std::memory_order_acquire)) {
        return 5;
    }
    delivery->complete (
      result_t<void>::failure (framework_error_kind_t::unavailable, "join callback push failed"));
    {
        std::unique_lock lock (mutex);
        if (!changed.wait_for (lock, std::chrono::seconds (1),
                               [&] { return terminal.has_value (); })) {
            return 6;
        }
    }
    const auto next_delivery_deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (1);
    while (!next_delivery_started.load (std::memory_order_acquire)
           && std::chrono::steady_clock::now () < next_delivery_deadline) {
        std::this_thread::yield ();
    }
    if (!next_delivery_started.load (std::memory_order_acquire))
        return 7;
    if (*terminal || terminal->error_kind () != framework_error_kind_t::unavailable
        || terminal_calls.load (std::memory_order_relaxed) != 1) {
        return 8;
    }

    const auto next_fence = gateway.begin_join_completion_delivery_fence (actor);
    if (next_fence == fence)
        return 9;
    auto &next_terminal_calls = shared->next_terminal_calls;
    auto &next_succeeded = shared->next_succeeded;
    gateway.settle_join_completion_delivery_fence (
      actor, next_fence, result_t<void>::success (), [shared] (result_t<void> result) {
          shared->next_succeeded = static_cast<bool> (result);
          shared->next_terminal_calls.fetch_add (1, std::memory_order_relaxed);
          throw std::runtime_error ("immediate settlement observer failed");
      });
    return next_succeeded && next_terminal_calls.load (std::memory_order_relaxed) == 1 ? 0 : 10;
}

int terminal_reply_precedes_session_route_publication ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    auto state = std::make_shared<actor_gateway_state_t> ();
    actor_gateway_runtime_t gateway (state);
    const auto actor = test_actor_ref ("actor-owner", "player", "reply-before-publish", 7);
    const auto sink = [] (std::string, stream_codec_t, const zlink::message_t &) {
        return task_t<void> (result_t<void>::success ());
    };
    const actor_bound_session_route_t old_route{zlink::routing_id_t::from ("owner"),
                                                zlink::routing_id_t::from ("old"),
                                                7,
                                                11,
                                                13,
                                                17,
                                                1,
                                                1,
                                                0};
    const actor_bound_session_route_t next_route{zlink::routing_id_t::from ("owner"),
                                                 zlink::routing_id_t::from ("next"),
                                                 7,
                                                 11,
                                                 13,
                                                 17,
                                                 2,
                                                 2,
                                                 0};
    if (!gateway.replace_session_route (actor, sink, old_route))
        return 1;
    bool terminal_submitted = false;
    const auto replaced = gateway.replace_session_route (
      actor, sink, next_route, stream_codec_t::message_pack,
      [&] (const actor_bound_session_route_t &) {
          terminal_submitted = true;
          const auto &published = state->actors_by_id.at ("reply-before-publish");
          return published.bound_session_route
                 && published.bound_session_route->binding_generation == 1;
      });
    const auto published = gateway.resolve_bound_session_push_route (actor, next_route);
    if (!replaced || !terminal_submitted || !published
        || published->binding_generation != next_route.binding_generation)
        return 2;
    return 0;
}

int join_completion_callback_failure_is_reported ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    auto state = std::make_shared<actor_gateway_state_t> ();
    auto observations =
      std::make_shared<test::telemetry_observations_t<message_dispatch_error_event_t>> ();
    state->dispatch.message_flow (message_flow_log_mode_t::normal);
    dispatch_options_access_t::set_dispatch_error_observer_for_tests (
      state->dispatch, [observations] (const auto &event) { observations->record (event); });
    actor_gateway_runtime_t gateway (state);
    auto actor = test_actor_ref ("node", "player", "callback-failure", 7);
    auto fence = gateway.begin_join_completion_delivery_fence (actor);
    gateway.settle_join_completion_delivery_fence (
      actor, fence, result_t<void>::success (),
      [] (result_t<void>) { throw std::runtime_error ("completion failure"); });
    test::acknowledge_delivery ();
    const auto errors = observations->snapshot ();
    return errors.size () == 1
               && errors.front ().reason == dispatch_error_reason_t::handler_exception
               && errors.front ().error_message == "completion failure"
             ? 0
             : 1;
}
} // namespace zlink::framework::test::actor_gateway
