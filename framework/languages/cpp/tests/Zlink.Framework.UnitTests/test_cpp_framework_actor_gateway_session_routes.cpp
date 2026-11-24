/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/runtime_failure_fixture.hpp"
#include "../support/actor_gateway_cases.hpp"
#include "../support/actor_gateway_fixture.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/streams/stream_runtime.hpp"
#include <zlink/framework/contracts/configuration/zlink_builder.hpp>
#include <zlink/framework/contracts/dispatch/task.hpp>
#include <zlink/framework/contracts/streams/stream.hpp>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace zlink::framework::test::actor_gateway
{
int actor_context_identity_and_source_fence_are_exact ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    actor_gateway_runtime_t gateway;
    const actor_ref_t source = test_actor_ref ("actor-node-a", "player", "actor-context", 7);
    const actor_ref_t same = test_actor_ref ("actor-node-a", "player", "actor-context", 7);
    const actor_ref_t successor = test_actor_ref ("actor-node-b", "player", "actor-context", 7);
    const actor_ref_t new_incarnation =
      test_actor_ref ("actor-node-a", "player", "actor-context", 8);

    const auto source_context = gateway.actor_context (source);
    const auto same_context = gateway.actor_context (same);
    const auto successor_context = gateway.actor_context (successor);
    const auto new_incarnation_context = gateway.actor_context (new_incarnation);

    if (source_context.actor_id ().value () != "actor-context"
        || source_context.object_generation () != 7
        || source_context.actor_ref ().node_rid ().value () != "actor-node-a") {
        return 1;
    }
    if (!gateway.same_context_source_fence (source_context, same_context)) {
        return 2;
    }
    if (gateway.same_context_source_fence (source_context, successor_context)
        || gateway.same_context_source_fence (source_context, new_incarnation_context)) {
        return 3;
    }
    return 0;
}

int bound_session_route_preserves_private_fences ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    actor_gateway_runtime_t gateway;
    const actor_ref_t actor = test_actor_ref ("actor-node", "player", "actor-fenced", 7);
    gateway.bind_session_sink (actor, [] (std::string, stream_codec_t, const zlink::message_t &) {
        return task_t<void> (result_t<void>::success ());
    });
    gateway.record_bound_session_route (actor,
                                        zlink::routing_id_t::from (std::string ("session-node")),
                                        std::nullopt, 11, 13, 17, 19, 23, 29);

    const auto route = gateway.bound_session_route_async (actor).result ().value ();
    if (!route || route->object_generation != 7 || route->node_generation != 11
        || route->authority_owner_generation != 13 || route->owner_lease_generation != 17
        || route->binding_generation != 19 || route->binding_token != 23
        || route->session_sequence != 29) {
        return 1;
    }
    if (!gateway.dispatch_bound_session_send (actor, "push", stream_codec_t::json,
                                              zlink::message_t{})) {
        return 2;
    }
    const auto advanced = gateway.bound_session_route_async (actor).result ().value ();
    return advanced && advanced->session_sequence == 29 ? 0 : 3;
}

zlink::framework::task_t<std::optional<zlink::framework::detail::actor_bound_session_route_t>>
read_bound_session_route_for_actor_join (
  std::shared_ptr<zlink::framework::detail::actor_gateway_state_t> state,
  zlink::framework::actor_ref_t actor)
{
    zlink::framework::detail::actor_gateway_runtime_t gateway (std::move (state));
    co_return co_await gateway.bound_session_route_async (std::move (actor));
}

int bound_session_route_suspends_until_owner_lane_runs ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    auto state = std::make_shared<actor_gateway_state_t> ();
    const auto actor = test_actor_ref ("actor-node", "player", "pending-route", 7);
    {
        actor_gateway_runtime_t gateway (state);
        gateway.bind_session_sink (actor,
                                   [] (std::string, stream_codec_t, const zlink::message_t &) {
                                       return task_t<void> (result_t<void>::success ());
                                   });
        gateway.record_bound_session_route (actor, zlink::routing_id_t::from ("session-node"),
                                            std::nullopt, 11, 13, 17, 19, 23, 29);
    }

    std::promise<void> entered;
    std::promise<void> release;
    auto released = release.get_future ().share ();
    if (!state->lane.try_post ([&entered, released] {
            entered.set_value ();
            released.wait ();
        }))
        return 1;
    entered.get_future ().wait ();

    using route_task_t = task_t<std::optional<actor_bound_session_route_t>>;
    std::promise<std::pair<route_task_t, bool>> submitted;
    auto submission = submitted.get_future ();
    std::promise<void> progressed;
    auto progress = progressed.get_future ();
    runtime::offload_executor_t infrastructure (1);
    if (!infrastructure.try_submit_internal ([state, actor, &submitted] {
            auto query = read_bound_session_route_for_actor_join (state, actor);
            const bool pending = !query.await_ready ();
            submitted.set_value (std::make_pair (std::move (query), pending));
        })) {
        release.set_value ();
        return 2;
    }
    if (!infrastructure.try_submit_internal ([&progressed] { progressed.set_value (); })) {
        release.set_value ();
        return 3;
    }
    const bool progressed_while_lane_held =
      progress.wait_for (std::chrono::seconds (2)) == std::future_status::ready;
    release.set_value ();
    auto [query, pending] = submission.get ();
    progress.wait ();
    if (!progressed_while_lane_held || !pending)
        return 4;
    const auto route = query.result ().value ();
    return route && route->binding_generation == 19 && route->session_sequence == 29 ? 0 : 5;
}

int bound_session_send_does_not_publish_caller_location ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    auto state = std::make_shared<actor_gateway_state_t> ();
    serializer_registry_t serializers;
    state->serializers = &serializers;
    actor_gateway_runtime_t gateway (state);
    const auto source = test_actor_ref ("source-node", "player", "send-route-owner", 7);
    const auto target = test_actor_ref ("target-node", "player", "send-route-owner", 7);
    // The detached delivery can run after this scope ends, so the sink owns its counter.
    auto sends = std::make_shared<std::atomic_int> (0);
    if (!gateway.bind_session_sink (
          source, [sends] (std::string, stream_codec_t, const zlink::message_t &) {
              ++*sends;
              return task_t<void> (result_t<void>::success ());
          })) {
        return 1;
    }

    auto sent = gateway.actor_context (target)
                  .bound_session ()
                  .send (std::string ("push"))
                  .async ()
                  .result ();
    const auto delivery_deadline = std::chrono::steady_clock::now () + std::chrono::seconds (1);
    while (sends->load (std::memory_order_acquire) != 1
           && std::chrono::steady_clock::now () < delivery_deadline) {
        std::this_thread::yield ();
    }
    if (!sent || sends->load () != 1) {
        return 2;
    }
    if (state->sync ([&] {
            const auto found = state->actors_by_id.find ("send-route-owner");
            return found == state->actors_by_id.end ()
                   || found->second.ref.node_rid ().value () != "source-node";
        })) {
        return 3;
    }
    return 0;
}

int bound_session_ref_normalization_preserves_type_and_rejects_conflicts ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    auto state = std::make_shared<actor_gateway_state_t> ();
    actor_gateway_runtime_t gateway (state);
    const auto node = node_rid_t::from_string ("actor-node");
    const actor_ref_t typed = test_actor_ref ("actor-node", "support-user", "actor-public-ref", 7);
    const actor_ref_t public_ref (actor_id_t ("actor-public-ref"), 7, "mesh", node);
    auto sends = std::make_shared<std::atomic_int> (0);
    const auto sink = [sends] (std::string, stream_codec_t, const zlink::message_t &) {
        ++*sends;
        return task_t<void> (result_t<void>::success ());
    };

    const auto first_bind = gateway.bind_session_sink (typed, sink);
    if (!first_bind) {
        return 1;
    }
    const auto public_bind = gateway.bind_session_sink (public_ref, sink);
    if (!public_bind) {
        return 2;
    }
    const auto public_ref_is_enriched = state->sync ([&] {
        const auto found = state->actors_by_id.find ("actor-public-ref");
        return found != state->actors_by_id.end ()
               && actor_ref_access_t::actor_type (found->second.ref) == "support-user"
               && found->second.ref.mesh_name () == "mesh"
               && found->second.ref.node_rid ().value () == "actor-node";
    });
    if (!public_ref_is_enriched)
        return 3;

    const auto route_recorded = gateway.record_bound_session_route (
      public_ref, zlink::routing_id_t::from (std::string ("session-node")));
    if (!route_recorded) {
        return 4;
    }
    const auto dispatched = gateway.dispatch_bound_session_send (
      public_ref, "push", stream_codec_t::json, zlink::message_t{});
    if (!dispatched || sends->load () != 1) {
        return 5;
    }

    const actor_ref_t wrong_type =
      test_actor_ref ("actor-node", "other-actor", "actor-public-ref", 7);
    const auto wrong_type_bind = gateway.bind_session_sink (wrong_type, sink);
    if (wrong_type_bind || wrong_type_bind.error_kind () != framework_error_kind_t::type_mismatch) {
        return 6;
    }

    const actor_ref_t stale = test_actor_ref ("actor-node", "support-user", "actor-public-ref", 8);
    const auto stale_bind = gateway.bind_session_sink (stale, sink);
    if (stale_bind || stale_bind.error_kind () != framework_error_kind_t::invalid_operation) {
        return 7;
    }
    const auto stale_route = gateway.record_bound_session_route (
      stale, zlink::routing_id_t::from (std::string ("other-session")));
    if (stale_route || stale_route.error_kind () != framework_error_kind_t::invalid_operation) {
        return 8;
    }
    const auto route_after_rejections =
      gateway.bound_session_route_async (public_ref).result ().value ();
    if (!route_after_rejections || route_after_rejections->node_rid.to_string () != "session-node"
        || route_after_rejections->session_sequence != 0) {
        return 9;
    }

    const actor_ref_t public_first (actor_id_t ("actor-type-enrichment"), 3, "mesh", node);
    const actor_ref_t typed_second =
      test_actor_ref ("actor-node", "support-user", "actor-type-enrichment", 3);
    if (!gateway.bind_session_sink (public_first, sink)
        || !gateway.bind_session_sink (typed_second, sink)) {
        return 10;
    }
    const auto actor_type_is_enriched = state->sync ([&] {
        const auto found = state->actors_by_id.find ("actor-type-enrichment");
        return found != state->actors_by_id.end ()
               && actor_ref_access_t::actor_type (found->second.ref) == "support-user";
    });
    if (!actor_type_is_enriched)
        return 11;
    return 0;
}

int bound_session_route_installs_sink_and_fence_together ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    serializer_registry_t serializers;
    auto state = std::make_shared<actor_gateway_state_t> ();
    actor_gateway_runtime_t gateway (state);
    const auto actor = test_actor_ref ("route-node", "support-user", "route-actor", 5);
    const auto bound = gateway.bind_session_route (
      actor, nullptr, serializers, "actor-route",
      zlink::routing_id_t::from (std::string ("session-node")), stream_codec_t::message_pack, true,
      std::make_optional (zlink::routing_id_t::from (std::string ("session-rid"))));
    if (!bound) {
        return 1;
    }
    const auto route_is_installed = state->sync ([&] {
        const auto found = state->actors_by_id.find ("route-actor");
        return found != state->actors_by_id.end () && found->second.bound
               && found->second.bound_session_route
               && state->bound_session_sinks.count ("route-actor") == 1;
    });
    if (!route_is_installed)
        return 2;
    const auto route = gateway.bound_session_route_async (actor).result ().value ();
    if (!route || route->node_rid.to_string () != "session-node" || !route->session_rid
        || route->session_rid->to_string () != "session-rid") {
        return 3;
    }

    const auto non_replacing = gateway.bind_session_route (
      actor, nullptr, serializers, "replacement-route",
      zlink::routing_id_t::from (std::string ("replacement-node")), stream_codec_t::json, false,
      std::make_optional (zlink::routing_id_t::from (std::string ("replacement-rid"))));
    if (!non_replacing) {
        return 4;
    }
    const auto retained = gateway.bound_session_route_async (actor).result ().value ();
    if (!retained || retained->node_rid.to_string () != "session-node" || !retained->session_rid
        || retained->session_rid->to_string () != "session-rid") {
        return 5;
    }

    const auto wrong_type = test_actor_ref ("route-node", "other-actor", "route-actor", 5);
    const auto rejected =
      gateway.bind_session_route (wrong_type, nullptr, serializers, "actor-route",
                                  zlink::routing_id_t::from (std::string ("other-node")));
    if (rejected || rejected.error_kind () != framework_error_kind_t::type_mismatch) {
        return 6;
    }
    const auto preserved = gateway.bound_session_route_async (actor).result ().value ();
    return preserved && preserved->node_rid.to_string () == "session-node" && preserved->session_rid
               && preserved->session_rid->to_string () == "session-rid"
             ? 0
             : 7;
}

int bound_session_transition_is_atomic_and_idempotent ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    actor_gateway_runtime_t gateway;
    const auto actor = test_actor_ref ("actor-owner", "game.actor", "actor-a", 1);
    const auto sink = [] (std::string, stream_codec_t, const zlink::message_t &) {
        return task_t<void> (result_t<void>::success ());
    };
    if (!gateway.bind_session_sink (actor, sink))
        return 1;

    const actor_bound_session_route_t first{
      zlink::routing_id_t::from (std::string ("session-owner-a")),
      zlink::routing_id_t::from (std::string ("session-a")),
      1,
      2,
      3,
      4,
      5,
      7,
      9};
    const auto installed = gateway.record_bound_session_route_transition (actor, first);
    if (!installed || !installed.value ().changed || installed.value ().previous)
        return 2;

    auto same_identity = first;
    same_identity.session_sequence = 0;
    const auto idempotent = gateway.record_bound_session_route_transition (actor, same_identity);
    if (!idempotent || idempotent.value ().changed || idempotent.value ().previous
        || !idempotent.value ().current || idempotent.value ().current->binding_token != 7
        || idempotent.value ().current->session_sequence != 9)
        return 3;

    auto authority_update = first;
    authority_update.authority_owner_generation = 11;
    authority_update.owner_lease_generation = 13;
    authority_update.binding_token = 0;
    authority_update.session_sequence = 0;
    const auto retained = gateway.record_bound_session_route_transition (
      test_actor_ref ("actor-target", "game.actor", "actor-a", 1), authority_update);
    const auto retained_route = gateway.bound_session_route_async (actor).result ().value ();
    if (!retained || retained.value ().changed || retained.value ().previous || !retained_route
        || retained_route->authority_owner_generation != 11
        || retained_route->owner_lease_generation != 13 || retained_route->binding_token != 7
        || retained_route->session_sequence != 9)
        return 4;

    auto second = authority_update;
    second.session_rid = zlink::routing_id_t::from (std::string ("session-b"));
    second.binding_generation = 6;
    const auto replaced = gateway.record_bound_session_route_transition (actor, second);
    if (!replaced || !replaced.value ().changed || !replaced.value ().previous
        || replaced.value ().previous->session_rid != first.session_rid
        || replaced.value ().previous->binding_generation != 5
        || replaced.value ().previous->authority_owner_generation != 11
        || replaced.value ().previous->session_sequence != 9)
        return 5;

    return 0;
}

int authority_only_route_update_keeps_physical_session_current ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    auto state = std::make_shared<actor_gateway_state_t> ();
    actor_gateway_runtime_t gateway (state);
    const auto source = test_actor_ref ("actor-source", "game.actor", "actor-physical", 7);
    const auto target = test_actor_ref ("actor-target", "game.actor", "actor-physical", 7);
    const auto session_owner = zlink::routing_id_t::from (std::string ("session-owner"));
    const auto session_rid = zlink::routing_id_t::from (std::string ("session-rid"));
    std::atomic_int source_sink_calls{0};
    std::atomic_int target_sink_calls{0};
    const auto source_sink = [&source_sink_calls] (std::string, stream_codec_t,
                                                   const zlink::message_t &) {
        ++source_sink_calls;
        return task_t<void> (result_t<void>::success ());
    };
    const auto target_sink = [&target_sink_calls] (std::string, stream_codec_t,
                                                   const zlink::message_t &) {
        ++target_sink_calls;
        return task_t<void> (result_t<void>::success ());
    };

    const actor_bound_session_route_t initial{
      session_owner, session_rid, 7, 17, 19, 23, 29, 31, 37};
    const auto installed =
      gateway.replace_session_route (source, source_sink, initial, stream_codec_t::message_pack);
    if (!installed || !installed.value ().changed || installed.value ().previous)
        return 1;

    auto relocated = initial;
    relocated.authority_owner_generation = 41;
    relocated.owner_lease_generation = 43;
    relocated.binding_token = 0;
    relocated.session_sequence = 0;
    const auto updated =
      gateway.replace_session_route (target, target_sink, relocated, stream_codec_t::message_pack);
    const auto current = gateway.bound_session_route_async (target).result ().value ();
    if (!updated || updated.value ().changed || updated.value ().previous || !current
        || current->authority_owner_generation != 41 || current->owner_lease_generation != 43
        || current->binding_token != 31 || current->session_sequence != 37
        || !gateway.actor_bound ("actor-physical") || gateway.actor_disconnected ("actor-physical"))
        return 2;
    const auto dispatched = gateway.dispatch_bound_session_send (
      target, "AuthorityChangedNotify", stream_codec_t::message_pack, zlink::message_t{});
    if (!dispatched || source_sink_calls.load () != 0 || target_sink_calls.load () != 1)
        return 3;

    auto replacement = relocated;
    replacement.session_rid = zlink::routing_id_t::from (std::string ("replacement-session"));
    replacement.binding_generation = 47;
    const auto replaced = gateway.replace_session_route (target, target_sink, replacement,
                                                         stream_codec_t::message_pack);
    if (!replaced || !replaced.value ().changed || !replaced.value ().previous
        || replaced.value ().previous->session_rid != session_rid
        || replaced.value ().previous->binding_generation != 29)
        return 4;

    auto local_state = std::make_shared<actor_gateway_state_t> ();
    actor_gateway_runtime_t local_gateway (local_state);
    zlink_builder_t builder = zlink::framework::test::runtime_failure_builder ();
    builder.stream ("authority-stream-sink").bind ("tcp://127.0.0.1:0");
    auto runtime = stream_runtime_t::from (builder);
    auto stream = runtime.open_session ("authority-stream-sink");
    std::atomic_int local_stream_calls{0};
    std::atomic_int replacement_sink_calls{0};
    runtime.attach_transport_writer (
      stream, [&local_stream_calls] (const stream_header_t &, const zlink::message_t &) {
          ++local_stream_calls;
          return task_t<void> (result_t<void>::success ());
      });
    local_gateway.bind_session_stream (
      "actor-local-stream", stream, stream_codec_t::message_pack, "physical-session", 53,
      test_actor_ref ("actor-source", "game.actor", "actor-local-stream", 59));
    const actor_bound_session_route_t local_initial{
      session_owner, session_rid, 59, 61, 67, 71, 73, 53, 79};
    const auto local_installed = local_gateway.record_bound_session_route (
      test_actor_ref ("actor-source", "game.actor", "actor-local-stream", 59), session_owner,
      session_rid, 61, 67, 71, 73, 53, 79);
    if (!local_installed)
        return 5;
    auto local_relocated = local_initial;
    local_relocated.authority_owner_generation = 83;
    local_relocated.owner_lease_generation = 89;
    local_relocated.binding_token = 0;
    local_relocated.session_sequence = 0;
    const auto local_updated = local_gateway.replace_session_route (
      test_actor_ref ("actor-target", "game.actor", "actor-local-stream", 59),
      [&replacement_sink_calls] (std::string, stream_codec_t, const zlink::message_t &) {
          ++replacement_sink_calls;
          return task_t<void> (result_t<void>::success ());
      },
      local_relocated, stream_codec_t::message_pack);
    if (!local_updated || local_updated.value ().changed)
        return 6;
    const auto local_dispatched = local_gateway.dispatch_bound_session_send (
      test_actor_ref ("actor-target", "game.actor", "actor-local-stream", 59),
      "AuthorityChangedNotify", stream_codec_t::message_pack, zlink::message_t{});
    if (!local_dispatched || local_stream_calls.load () != 1 || replacement_sink_calls.load () != 0)
        return 7;
    auto local_rebound = local_relocated;
    local_rebound.session_rid = zlink::routing_id_t::from (std::string ("replacement-session"));
    local_rebound.binding_generation = 97;
    const auto rebound = local_gateway.replace_session_route (
      test_actor_ref ("actor-target", "game.actor", "actor-local-stream", 59),
      [&replacement_sink_calls] (std::string, stream_codec_t, const zlink::message_t &) {
          ++replacement_sink_calls;
          return task_t<void> (result_t<void>::success ());
      },
      local_rebound, stream_codec_t::message_pack);
    if (!rebound || !rebound.value ().changed || !rebound.value ().previous
        || rebound.value ().previous->session_rid != session_rid
        || rebound.value ().previous->binding_generation != 73)
        return 8;
    const auto rebound_dispatch = local_gateway.dispatch_bound_session_send (
      test_actor_ref ("actor-target", "game.actor", "actor-local-stream", 59), "ReconnectedNotify",
      stream_codec_t::message_pack, zlink::message_t{});
    if (!rebound_dispatch || local_stream_calls.load () != 1 || replacement_sink_calls.load () != 1)
        return 9;
    return 0;
}
} // namespace zlink::framework::test::actor_gateway
