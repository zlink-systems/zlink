/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/streams/stream_runtime.hpp"

#include <zlink/framework/contracts/configuration/zlink_builder.hpp>

#include <algorithm>
#include <atomic>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{

zlink::framework::actor_ref_t test_actor_ref (std::string node,
                                              std::string actor_type,
                                              std::string actor_id,
                                              std::uint64_t generation)
{
    return zlink::framework::detail::actor_ref_access_t::make (
      zlink::framework::node_rid_t::from_string (std::move (node)),
      std::move (actor_type), std::move (actor_id), generation);
}

int relay_dispatch_scope_restores_nested_and_exception_state ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    const stream_header_t outer (
      stream_message_kind_t::send, stream_codec_t::json,
      stream_header_flags_t::none, std::nullopt, "outer");
    const stream_header_t inner (
      stream_message_kind_t::request, stream_codec_t::json,
      stream_header_flags_t::none, std::nullopt, "inner");
    {
        const stream_relay_dispatch_scope_t outer_scope (outer);
        try {
            {
                const stream_relay_dispatch_scope_t inner_scope (inner);
                const auto current = current_stream_relay_dispatch ();
                if (!current || current->packet_name () != "inner") {
                    return 1;
                }
                throw std::runtime_error ("relay failure");
            }
        }
        catch (const std::runtime_error &) {
        }
        const auto restored = current_stream_relay_dispatch ();
        if (!restored || restored->packet_name () != "outer") {
            return 2;
        }
    }
    return current_stream_relay_dispatch () ? 3 : 0;
}

int actor_identity_validation_is_bounded_and_utf8_exact ()
{
    using namespace zlink::framework;

    const actor_id_t unicode_id ("사용자");
    if (unicode_id.value () != "사용자")
        return 1;

    const auto rejects = [] (std::string value) {
        try {
            actor_id_t invalid (std::move (value));
            static_cast<void> (invalid);
        }
        catch (const std::invalid_argument &) {
            return true;
        }
        return false;
    };
    if (!rejects ({})
        || !rejects (std::string ("\xc3\x28"))
        || !rejects (std::string ("\xc0\x80"))
        || !rejects (std::string ("\xed\xa0\x80"))
        || !rejects (std::string ("\xf4\x90\x80\x80"))
        || !rejects (std::string (256, 'a'))) {
        return 2;
    }

    const auto node = node_rid_t::from_string ("actor-node");
    const auto max_generation = static_cast<std::uint64_t> (
      std::numeric_limits<std::int64_t>::max ());
    const actor_ref_t maximum (
      actor_id_t ("actor"), max_generation, "game", node);
    if (maximum.object_generation () != max_generation)
        return 3;

    const auto rejects_generation = [&] (std::uint64_t generation) {
        try {
            actor_ref_t invalid (
              actor_id_t ("actor"), generation, "game", node);
            static_cast<void> (invalid);
        }
        catch (const std::invalid_argument &) {
            return true;
        }
        return false;
    };
    if (!rejects_generation (0)
        || !rejects_generation (max_generation + 1)) {
        return 4;
    }
    return 0;
}

class recording_actor_client_t final : public zlink::framework::actor_client_t
{
  public:
    std::atomic_int attempts{0};

  protected:
    zlink::framework::task_t<void> send_erased (
      zlink::framework::actor_id_t,
      std::string,
      zlink::framework::message_t,
      const zlink::framework::actor_send_call_t::metadata_map_t &) override
    {
        ++attempts;
        return zlink::framework::task_t<void> (
          zlink::framework::result_t<void>::success ());
    }

    zlink::framework::task_t<zlink::framework::message_t> request_erased (
      zlink::framework::actor_id_t,
      std::string,
      zlink::framework::message_t,
      std::optional<std::chrono::milliseconds>,
      const zlink::framework::actor_request_call_t::metadata_map_t &) override
    {
        return zlink::framework::task_t<zlink::framework::message_t> (
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

int stale_session_unbind_preserves_rebind ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    auto state = std::make_shared<actor_gateway_state_t> ();
    actor_gateway_runtime_t gateway (state);
    const actor_ref_t actor = test_actor_ref ("actor-node", "player", "actor-1", 7);

    gateway.bind_session_sink (
      actor,
      [] (std::string, stream_codec_t, const zlink::message_t &) {
          return task_t<void> (result_t<void>::success ());
      });
    gateway.bind_session_stream (
      "actor-1", stream_t{}, stream_codec_t::message_pack, "session-old", 11);
    gateway.bind_session_stream (
      "actor-1", stream_t{}, stream_codec_t::message_pack, "session-new", 12);

    gateway.unbind_session_stream ("actor-1", "session-old", 11);
    {
        const std::lock_guard lock (state->mutex);
        const auto actor_record = state->actors_by_id.find ("actor-1");
        if (actor_record == state->actors_by_id.end ()
            || actor_record->second.binding_session_id != "session-new"
            || actor_record->second.binding_token != 12
            || !actor_record->second.bound_session_stream_sink
            || state->bound_session_sinks.count ("actor-1") != 1) {
            return 1;
        }
    }

    gateway.unbind_session_stream ("actor-1", "session-new", 12);
    {
        const std::lock_guard lock (state->mutex);
        const auto actor_record = state->actors_by_id.find ("actor-1");
        if (actor_record == state->actors_by_id.end ()
            || !actor_record->second.binding_session_id.empty ()
            || actor_record->second.binding_token != 0
            || actor_record->second.bound_session_stream_sink
            || state->bound_session_sinks.count ("actor-1") != 0) {
            return 2;
        }
    }
    return 0;
}

int actor_send_is_one_shot ()
{
    using namespace zlink::framework;
    recording_actor_client_t client;
    actor_send_call_t call (
      client,
      actor_id_t ("actor-2"),
      "message", message_t{});
    auto copied = call;
    call.submit ().result ().value ();
    bool rejected = false;
    try {
        (void) copied.submit ().result ().value ();
    }
    catch (const framework_exception_t &error) {
        rejected = error.kind () == framework_error_kind_t::protocol_error;
    }
    return rejected && client.attempts.load () == 1 ? 0 : 2;
}

int get_or_create_does_not_reuse_disconnected_record ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    auto state = std::make_shared<actor_gateway_state_t> ();
    const actor_ref_t stale =
      test_actor_ref ("actor-node-old", "player", "actor-reconnect", 7);
    const actor_ref_t current =
      test_actor_ref ("actor-node-new", "player", "actor-reconnect", 8);
    {
        const std::lock_guard lock (state->mutex);
        state->actors_by_id.emplace (
          "actor-reconnect", actor_record_t{stale, false, true});
        state->bound_session_sinks.emplace (
          "actor-reconnect",
          std::make_shared<bound_session_sink_t> (
            [] (std::string, stream_codec_t, const zlink::message_t &) {
                return task_t<void> (result_t<void>::success ());
            }));
        state->create_dispatcher =
          [current] (std::string, std::string,
                     const std::optional<zlink::message_t> &) {
              return result_t<actor_ref_t>::success (current);
          };
    }

    actor_gateway_runtime_t gateway (state);
    auto manager = gateway.manager ();
    const auto created = manager.get_or_create ("player", "actor-reconnect");
    if (!created || created.value ().ref ().node_rid ().value () != "actor-node-new"
        || created.value ().ref ().object_generation () != 8) {
        return 1;
    }
    {
        const std::lock_guard lock (state->mutex);
        const auto found = state->actors_by_id.find ("actor-reconnect");
        if (found == state->actors_by_id.end ()
            || found->second.ref.node_rid ().value () != "actor-node-new"
            || found->second.disconnected
            || state->bound_session_sinks.contains ("actor-reconnect")) {
            return 2;
        }
    }
    return 0;
}

int get_or_create_refreshes_foreign_session_record ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    auto state = std::make_shared<actor_gateway_state_t> ();
    const actor_ref_t stale =
      test_actor_ref ("actor-node-old", "player", "actor-foreign", 7);
    const actor_ref_t current =
      test_actor_ref ("actor-node-current", "player", "actor-foreign", 7);
    {
        const std::lock_guard lock (state->mutex);
        state->actors_by_id.emplace (
          "actor-foreign", actor_record_t{stale, true, false});
        state->create_dispatcher =
          [current] (std::string, std::string,
                     const std::optional<zlink::message_t> &) {
              return result_t<actor_ref_t>::success (current);
          };
    }

    actor_gateway_runtime_t gateway (state);
    gateway.bind_session_stream (
      "actor-foreign", stream_t{}, stream_codec_t::message_pack, "session-old", 11);
    auto manager = gateway.manager ();

    zlink::framework::zlink_builder_t builder;
    builder.stream ("foreign-session-test").bind ("tcp://127.0.0.1:0");
    auto runtime = stream_runtime_t::from (builder);
    auto stream = runtime.open_session ("foreign-session-test");
    session_actor_manager_access_t::attach (manager, std::move (stream));

    const auto refreshed = manager.get_or_create ("player", "actor-foreign");
    if (!refreshed || refreshed.value ().ref ().node_rid ().value () != "actor-node-current") {
        return 1;
    }
    {
        const std::lock_guard lock (state->mutex);
        const auto found = state->actors_by_id.find ("actor-foreign");
        if (found == state->actors_by_id.end ()
            || found->second.ref.node_rid ().value () != "actor-node-current"
            || found->second.binding_session_id == "session-old"
            || state->bound_session_sinks.contains ("actor-foreign")) {
            return 2;
        }
    }
    return 0;
}

int session_disconnect_is_all_settled_and_token_fenced ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    auto state = std::make_shared<actor_gateway_state_t> ();
    actor_gateway_runtime_t gateway (state);
    auto manager = gateway.manager ();
    session_actor_manager_access_t::attach (manager, stream_t{});
    const actor_ref_t first = test_actor_ref ("actor-node", "player", "actor-a", 1);
    const actor_ref_t second = test_actor_ref ("actor-node", "player", "actor-b", 1);

    auto stale = manager.bind (first).submit ().result ().value ();
    auto current = manager.bind (first).submit ().result ().value ();
    (void) manager.bind (second).submit ().result ().value ();
    if (stale.notify_disconnected ().result ().error_kind ()
        != framework_error_kind_t::not_configured) {
        return 1;
    }

    std::vector<std::string> disconnected;
    gateway.on_disconnect (
      [&] (const actor_ref_t &actor) {
          disconnected.emplace_back (actor.actor_id ().value ());
          return actor.actor_id ().value () == "actor-a"
                   ? result_t<void>::failure (
                       framework_error_kind_t::not_found,
                       "actor-a callback failed")
                   : result_t<void>::success ();
      });
    session_actor_manager_access_t::disconnect (manager);
    std::sort (disconnected.begin (), disconnected.end ());
    if (disconnected != std::vector<std::string>{"actor-a", "actor-b"}
        || !gateway.actor_disconnected ("actor-a")
        || !gateway.actor_disconnected ("actor-b")) {
        return 2;
    }
    {
        const std::lock_guard lock (state->mutex);
        if (!state->actors_by_id.contains ("actor-a")
            || !state->actors_by_id.contains ("actor-b")) {
            return 3;
        }
    }
    if (current.notify_disconnected ().result ())
        return 4;
    return 0;
}

int logical_disconnect_is_selected_and_keeps_session_live ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    auto state = std::make_shared<actor_gateway_state_t> ();
    actor_gateway_runtime_t gateway (state);
    auto manager = gateway.manager ();
    session_actor_manager_access_t::attach (manager, stream_t{});
    const actor_ref_t first = test_actor_ref ("actor-node", "player", "actor-a", 1);
    const actor_ref_t second = test_actor_ref ("actor-node", "player", "actor-b", 1);
    auto first_binding = manager.bind (first).submit ().result ().value ();
    auto second_binding = manager.bind (second).submit ().result ().value ();
    std::vector<std::string> disconnected;
    gateway.on_disconnect (
      [&] (const actor_ref_t &actor) {
          disconnected.emplace_back (actor.actor_id ().value ());
          return result_t<void>::success ();
      });

    if (!first_binding.notify_disconnected ().result ()) {
        return 1;
    }
    if (disconnected != std::vector<std::string>{"actor-a"})
        return 4;
    {
        const std::lock_guard lock (state->mutex);
        const auto second_record = state->actors_by_id.find ("actor-b");
        if (second_record == state->actors_by_id.end ()
            || second_record->second.binding_token == 0) {
            return 2;
        }
    }
    if (!second_binding.notify_disconnected ().result ()
        || disconnected
             != std::vector<std::string>{"actor-a", "actor-b"}) {
        return 3;
    }
    return 0;
}

int route_update_preserves_object_generation ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    actor_gateway_runtime_t gateway;
    auto manager = gateway.manager ();
    session_actor_manager_access_t::attach (manager, stream_t{});
    const actor_ref_t original =
      test_actor_ref ("actor-node-a", "player", "actor-route", 7);
    auto original_binding =
      manager.bind (original).submit ().result ().value ();
    const actor_ref_t unaffected =
      test_actor_ref ("actor-node-a", "player", "actor-other", 3);
    auto unaffected_binding =
      manager.bind (unaffected).submit ().result ().value ();

    std::vector<actor_ref_t> relay_routes;
    gateway.on_relay (
      [&] (const actor_ref_t &actor,
           const actor_context_t &,
           const stream_header_t &,
           const zlink::message_t &) {
          relay_routes.push_back (actor);
          return result_t<std::optional<zlink::message_t>>::success (
            std::nullopt);
      });

    const actor_ref_t relocated =
      test_actor_ref ("actor-node-b", "player", "actor-route", 7);
    if (!gateway.update_actor_ref (relocated))
        return 1;
    if (!original_binding.relay ("packet", zlink::message_t{}).result ()
        || relay_routes.size () != 1
        || relay_routes.front ().node_rid ().value ()
             != relocated.node_rid ().value ()) {
        return 5;
    }
    if (!unaffected_binding.relay ("packet", zlink::message_t{}).result ()
        || relay_routes.size () != 2
        || relay_routes.back ().node_rid ().value ()
             != unaffected.node_rid ().value ()) {
        return 6;
    }
    const actor_ref_t new_incarnation =
      test_actor_ref ("actor-node-c", "player", "actor-route", 8);
    const auto rejected = gateway.update_actor_ref (new_incarnation);
    if (rejected || rejected.error_kind () != framework_error_kind_t::invalid_operation)
        return 2;
    const auto rejected_bind = manager.bind (new_incarnation).submit ().result ();
    if (rejected_bind || rejected_bind.error_kind ()
                           != framework_error_kind_t::invalid_operation)
        return 3;
    const auto disconnected = original_binding.notify_disconnected ().result ();
    if (!disconnected || gateway.actor_bound ("actor-route")) {
        return 4;
    }
    return 0;
}

int exact_session_generation_mismatch_is_invalid_operation ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    actor_gateway_runtime_t gateway;
    auto manager = gateway.manager ();
    session_actor_manager_access_t::attach (manager, stream_t{});
    const actor_ref_t current = test_actor_ref ("actor-node", "player", "exact-actor", 3);
    const actor_ref_t stale = test_actor_ref ("actor-node", "player", "exact-actor", 2);
    if (!manager.bind (current).submit ().result ()) {
        return 1;
    }
    const auto bind_result = manager.bind (stale).submit ().result ();
    if (bind_result || bind_result.error_kind ()
                         != framework_error_kind_t::invalid_operation) {
        return 2;
    }
    const auto update_result = gateway.update_actor_ref (stale);
    if (update_result || update_result.error_kind ()
                              != framework_error_kind_t::invalid_operation) {
        return 3;
    }
    return 0;
}

int actor_context_identity_and_source_fence_are_exact ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    actor_gateway_runtime_t gateway;
    const actor_ref_t source = test_actor_ref ("actor-node-a", "player", "actor-context", 7);
    const actor_ref_t same = test_actor_ref ("actor-node-a", "player", "actor-context", 7);
    const actor_ref_t successor =
      test_actor_ref ("actor-node-b", "player", "actor-context", 7);
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
        || gateway.same_context_source_fence (
          source_context, new_incarnation_context)) {
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
    gateway.bind_session_sink (
      actor,
      [] (std::string, stream_codec_t, const zlink::message_t &) {
          return task_t<void> (result_t<void>::success ());
      });
    gateway.record_bound_session_route (
      actor, zlink::routing_id_t::from (std::string ("session-node")), std::nullopt,
      11, 13, 17, 19, 23, 29);

    const auto route = gateway.bound_session_route (actor);
    if (!route || route->object_generation != 7
        || route->node_generation != 11
        || route->authority_owner_generation != 13
        || route->owner_lease_generation != 17
        || route->binding_generation != 19
        || route->binding_token != 23
        || route->session_sequence != 29) {
        return 1;
    }
    if (!gateway.dispatch_bound_session_send (
          actor, "push", stream_codec_t::json, zlink::message_t{})) {
        return 2;
    }
    const auto advanced = gateway.bound_session_route (actor);
    return advanced && advanced->session_sequence == 30 ? 0 : 3;
}

int bound_session_ref_normalization_preserves_type_and_rejects_conflicts ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    auto state = std::make_shared<actor_gateway_state_t> ();
    actor_gateway_runtime_t gateway (state);
    const auto node = node_rid_t::from_string ("actor-node");
    const actor_ref_t typed = test_actor_ref (
      "actor-node", "support-user", "actor-public-ref", 7);
    const actor_ref_t public_ref (
      actor_id_t ("actor-public-ref"), 7, "mesh", node);
    auto sends = std::make_shared<std::atomic_int> (0);
    const auto sink = [sends] (std::string,
                               stream_codec_t,
                               const zlink::message_t &) {
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
    {
        const std::lock_guard lock (state->mutex);
        const auto found = state->actors_by_id.find ("actor-public-ref");
        if (found == state->actors_by_id.end ()
            || actor_ref_access_t::actor_type (found->second.ref) != "support-user"
            || found->second.ref.mesh_name () != "mesh"
            || found->second.ref.node_rid ().value () != "actor-node") {
            return 3;
        }
    }

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

    const actor_ref_t wrong_type = test_actor_ref (
      "actor-node", "other-actor", "actor-public-ref", 7);
    const auto wrong_type_bind = gateway.bind_session_sink (wrong_type, sink);
    if (wrong_type_bind
        || wrong_type_bind.error_kind () != framework_error_kind_t::type_mismatch) {
        return 6;
    }

    const actor_ref_t stale = test_actor_ref (
      "actor-node", "support-user", "actor-public-ref", 8);
    const auto stale_bind = gateway.bind_session_sink (stale, sink);
    if (stale_bind
        || stale_bind.error_kind () != framework_error_kind_t::invalid_operation) {
        return 7;
    }
    const auto stale_route = gateway.record_bound_session_route (
      stale, zlink::routing_id_t::from (std::string ("other-session")));
    if (stale_route
        || stale_route.error_kind () != framework_error_kind_t::invalid_operation) {
        return 8;
    }
    const auto route_after_rejections = gateway.bound_session_route (public_ref);
    if (!route_after_rejections
        || route_after_rejections->node_rid.to_string () != "session-node"
        || route_after_rejections->session_sequence != 1) {
        return 9;
    }

    const actor_ref_t public_first (
      actor_id_t ("actor-type-enrichment"), 3, "mesh", node);
    const actor_ref_t typed_second = test_actor_ref (
      "actor-node", "support-user", "actor-type-enrichment", 3);
    if (!gateway.bind_session_sink (public_first, sink)
        || !gateway.bind_session_sink (typed_second, sink)) {
        return 10;
    }
    {
        const std::lock_guard lock (state->mutex);
        const auto found = state->actors_by_id.find ("actor-type-enrichment");
        if (found == state->actors_by_id.end ()
            || actor_ref_access_t::actor_type (found->second.ref) != "support-user") {
            return 11;
        }
    }
    return 0;
}

int bound_session_route_installs_sink_and_fence_together ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    auto state = std::make_shared<actor_gateway_state_t> ();
    actor_gateway_runtime_t gateway (state);
    const auto actor = test_actor_ref (
      "route-node", "support-user", "route-actor", 5);
    const auto bound = gateway.bind_session_route (
      actor, route_client_t{}, "actor-route",
      zlink::routing_id_t::from (std::string ("session-node")),
      stream_codec_t::message_pack, true,
      std::make_optional (zlink::routing_id_t::from (std::string ("session-rid"))));
    if (!bound) {
        return 1;
    }
    {
        const std::lock_guard lock (state->mutex);
        const auto found = state->actors_by_id.find ("route-actor");
        if (found == state->actors_by_id.end ()
            || !found->second.bound
            || !found->second.bound_session_route
            || state->bound_session_sinks.count ("route-actor") != 1) {
            return 2;
        }
    }
    const auto route = gateway.bound_session_route (actor);
    if (!route || route->node_rid.to_string () != "session-node"
        || !route->session_rid
        || route->session_rid->to_string () != "session-rid") {
        return 3;
    }

    const auto non_replacing = gateway.bind_session_route (
      actor, route_client_t{}, "replacement-route",
      zlink::routing_id_t::from (std::string ("replacement-node")),
      stream_codec_t::json, false,
      std::make_optional (zlink::routing_id_t::from (std::string ("replacement-rid"))));
    if (!non_replacing) {
        return 4;
    }
    const auto retained = gateway.bound_session_route (actor);
    if (!retained || retained->node_rid.to_string () != "session-node"
        || !retained->session_rid
        || retained->session_rid->to_string () != "session-rid") {
        return 5;
    }

    const auto wrong_type = test_actor_ref (
      "route-node", "other-actor", "route-actor", 5);
    const auto rejected = gateway.bind_session_route (
      wrong_type, route_client_t{}, "actor-route",
      zlink::routing_id_t::from (std::string ("other-node")));
    if (rejected
        || rejected.error_kind () != framework_error_kind_t::type_mismatch) {
        return 6;
    }
    const auto preserved = gateway.bound_session_route (actor);
    return preserved && preserved->node_rid.to_string () == "session-node"
             && preserved->session_rid
             && preserved->session_rid->to_string () == "session-rid"
           ? 0
           : 7;
}

} // namespace

int main ()
{
    if (const auto identity = actor_identity_validation_is_bounded_and_utf8_exact ();
        identity != 0) {
        return 120 + identity;
    }
    if (const auto relay_scope =
          relay_dispatch_scope_restores_nested_and_exception_state ();
        relay_scope != 0) {
        return 110 + relay_scope;
    }
    if (const auto route_fence =
          bound_session_route_preserves_private_fences ();
        route_fence != 0) {
        return 100 + route_fence;
    }
    if (const auto normalized =
          bound_session_ref_normalization_preserves_type_and_rejects_conflicts ();
        normalized != 0) {
        return 80 + normalized;
    }
    if (const auto atomic_route =
          bound_session_route_installs_sink_and_fence_together ();
        atomic_route != 0) {
        return 70 + atomic_route;
    }
    if (const auto context_fence = actor_context_identity_and_source_fence_are_exact ();
        context_fence != 0) {
        return 90 + context_fence;
    }
    if (const auto stale = stale_session_unbind_preserves_rebind (); stale != 0) {
        return stale;
    }
    if (const auto exact = exact_session_generation_mismatch_is_invalid_operation (); exact != 0) {
        return exact;
    }
    const auto one_shot = actor_send_is_one_shot ();
    if (one_shot != 0)
        return 10 + one_shot;
    const auto reconnect = get_or_create_does_not_reuse_disconnected_record ();
    if (reconnect != 0)
        return 15 + reconnect;
    const auto foreign = get_or_create_refreshes_foreign_session_record ();
    if (foreign != 0)
        return 17 + foreign;
    const auto disconnected =
      session_disconnect_is_all_settled_and_token_fenced ();
    if (disconnected != 0)
        return 20 + disconnected;
    const auto logical =
      logical_disconnect_is_selected_and_keeps_session_live ();
    if (logical != 0)
        return 30 + logical;
    const auto route = route_update_preserves_object_generation ();
    return route == 0 ? 0 : 40 + route;
}
