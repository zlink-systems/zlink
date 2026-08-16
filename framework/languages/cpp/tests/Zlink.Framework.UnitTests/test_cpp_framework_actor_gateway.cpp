/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include "runtime/stateful/stream_session_registry.hpp"
#include "runtime/streams/stream_runtime.hpp"

#include <zlink/framework/contracts/configuration/zlink_builder.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
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

int replaced_session_find_is_exact_and_disconnects_once ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    auto state = std::make_shared<actor_gateway_state_t> ();
    actor_gateway_runtime_t gateway (state);
    auto old_session = gateway.manager ();
    auto current_session = gateway.manager ();
    session_actor_manager_access_t::attach (old_session, stream_t{});
    session_actor_manager_access_t::attach (current_session, stream_t{});
    const auto actor = test_actor_ref ("actor-node", "player", "actor-replaced", 1);

    (void) old_session.bind (actor).submit ().result ().value ();
    (void) current_session.bind (actor).submit ().result ().value ();
    int disconnected = 0;
    gateway.on_disconnect ([&] (const actor_ref_t &) {
        ++disconnected;
        return task_t<void> (result_t<void>::success ());
    });

    if (old_session.find ("actor-replaced"))
        return 1;
    session_actor_manager_access_t::disconnect (old_session);
    if (!gateway.actor_bound ("actor-replaced")
        || gateway.actor_disconnected ("actor-replaced")
        || disconnected != 0) {
        return 2;
    }

    auto current = current_session.find ("actor-replaced");
    if (!current || !current->notify_disconnected ().result ()
        || disconnected != 1 || !gateway.actor_disconnected ("actor-replaced")) {
        return 3;
    }
    session_actor_manager_access_t::disconnect (current_session);
    return disconnected == 1 ? 0 : 4;
}

int direct_rebind_publication_is_atomic_and_old_disconnect_is_fenced ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    auto state = std::make_shared<actor_gateway_state_t> ();
    actor_gateway_runtime_t gateway (state);
    auto old_session = gateway.manager ();
    auto new_session = gateway.manager ();
    zlink_builder_t builder;
    builder.stream ("direct-rebind").bind ("tcp://127.0.0.1:0");
    auto runtime = stream_runtime_t::from (builder);
    auto old_stream = runtime.open_session ("direct-rebind");
    auto new_stream = runtime.open_session ("direct-rebind");
    const auto old_session_id = old_stream.session_id ();
    const auto new_session_id = new_stream.session_id ();
    if (old_session_id == new_session_id)
        return 1;
    session_actor_manager_access_t::attach (
      old_session, std::move (old_stream));
    session_actor_manager_access_t::attach (
      new_session, std::move (new_stream));

    const auto actor =
      test_actor_ref ("actor-node", "player", "direct-rebind-actor", 1);
    auto old_binding = old_session.bind (actor).submit ().result ();
    if (!old_binding)
        return 2;
    auto stale_handle = std::move (old_binding.value ());
    std::uint64_t old_token = 0;
    {
        const std::lock_guard lock (state->mutex);
        old_token = state->actors_by_id.at ("direct-rebind-actor").binding_token;
    }

    std::promise<void> binder_entered_source;
    auto binder_entered = binder_entered_source.get_future ();
    std::promise<void> release_binder_source;
    auto release_binder = release_binder_source.get_future ();
    session_actor_manager_access_t::bind_native (
      new_session,
      [&] (const actor_ref_t &) {
          binder_entered_source.set_value ();
          release_binder.wait ();
          return task_t<void> (result_t<void>::success ());
      });
    std::optional<result_t<session_actor_t>> rebound;
    std::thread rebind_thread ([&] {
        rebound = new_session.bind (actor).submit ().result ();
    });
    binder_entered.wait ();

    int disconnected = 0;
    gateway.on_disconnect ([&] (const actor_ref_t &) {
        ++disconnected;
        return task_t<void> (result_t<void>::success ());
    });
    const auto stale_disconnect = stale_handle.notify_disconnected ().result ();
    if (stale_disconnect
        || stale_disconnect.error_kind ()
             != framework_error_kind_t::not_configured
        || disconnected != 0 || old_session.find ("direct-rebind-actor")) {
        release_binder_source.set_value ();
        rebind_thread.join ();
        return 3;
    }
    {
        const std::lock_guard lock (state->mutex);
        const auto &record = state->actors_by_id.at ("direct-rebind-actor");
        if (!record.bound || record.disconnected
            || record.binding_session_id != new_session_id
            || record.binding_token == 0 || record.binding_token == old_token
            || !record.bound_session_stream_sink) {
            release_binder_source.set_value ();
            rebind_thread.join ();
            return 4;
        }
    }

    release_binder_source.set_value ();
    rebind_thread.join ();
    if (!rebound || !*rebound || !new_session.find ("direct-rebind-actor")
        || !gateway.actor_bound ("direct-rebind-actor")
        || gateway.actor_disconnected ("direct-rebind-actor")
        || disconnected != 0) {
        return 5;
    }
    std::uint64_t committed_token = 0;
    {
        const std::lock_guard lock (state->mutex);
        committed_token =
          state->actors_by_id.at ("direct-rebind-actor").binding_token;
    }
    session_actor_manager_access_t::bind_native (
      new_session,
      [] (const actor_ref_t &) {
          return task_t<void> (result_t<void>::failure (
            framework_error_kind_t::unavailable,
            "deterministic native binding rejection"));
      });
    const auto rejected = new_session.bind (actor).submit ().result ();
    if (rejected
        || rejected.error_kind () != framework_error_kind_t::unavailable
        || !new_session.find ("direct-rebind-actor")) {
        return 6;
    }
    {
        const std::lock_guard lock (state->mutex);
        const auto &record = state->actors_by_id.at ("direct-rebind-actor");
        if (!record.bound || record.disconnected
            || record.binding_session_id != new_session_id
            || record.binding_token != committed_token
            || !record.bound_session_stream_sink) {
            return 7;
        }
    }
    return 0;
}

int destroyed_or_recreated_actor_ignores_stale_disconnect_handle ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    auto state = std::make_shared<actor_gateway_state_t> ();
    actor_gateway_runtime_t gateway (state);
    auto old_session = gateway.manager ();
    auto new_session = gateway.manager ();
    zlink_builder_t builder;
    builder.stream ("destroy-recreate").bind ("tcp://127.0.0.1:0");
    auto runtime = stream_runtime_t::from (builder);
    session_actor_manager_access_t::attach (
      old_session, runtime.open_session ("destroy-recreate"));
    session_actor_manager_access_t::attach (
      new_session, runtime.open_session ("destroy-recreate"));

    const auto original =
      test_actor_ref ("actor-node", "player", "recreated-actor", 1);
    auto original_result = old_session.bind (original).submit ().result ();
    if (!original_result)
        return 1;
    auto stale_handle = std::move (original_result.value ());
    int disconnected = 0;
    gateway.on_disconnect ([&] (const actor_ref_t &) {
        ++disconnected;
        return task_t<void> (result_t<void>::success ());
    });
    if (!gateway.destroy_actor (original)
        || !stale_handle.notify_disconnected ().result ()
        || disconnected != 0) {
        return 2;
    }

    const auto recreated =
      test_actor_ref ("actor-node", "player", "recreated-actor", 2);
    auto recreated_result = new_session.bind (recreated).submit ().result ();
    if (!recreated_result)
        return 3;
    const auto stale_after_recreate =
      stale_handle.notify_disconnected ().result ();
    if (stale_after_recreate
        || (stale_after_recreate.error_kind ()
              != framework_error_kind_t::not_configured
            && stale_after_recreate.error_kind ()
                 != framework_error_kind_t::invalid_operation)
        || disconnected != 0 || !gateway.actor_bound ("recreated-actor")
        || gateway.actor_disconnected ("recreated-actor")
        || !new_session.find ("recreated-actor")) {
        return 4;
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
          return task_t<void> (
            actor.actor_id ().value () == "actor-a"
              ? result_t<void>::failure (
                  framework_error_kind_t::not_found,
                  "actor-a callback failed")
              : result_t<void>::success ());
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
          return task_t<void> (result_t<void>::success ());
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
           const zlink::message_t &,
          std::optional<bound_session_relay_source_t>) {
          relay_routes.push_back (actor);
          return task_t<std::optional<zlink::message_t>> (
            result_t<std::optional<zlink::message_t>>::success (std::nullopt));
      });

    const actor_ref_t relocated =
      test_actor_ref ("actor-node-b", "player", "actor-route", 7);
    if (!gateway.update_actor_ref (relocated))
        return 1;
    if (!original_binding.relay ("packet", zlink::message_t{}).result ()
        || relay_routes.size () != 1
        || relay_routes.front ().node_rid ().value ()
             != original.node_rid ().value ()) {
        return 5;
    }
    if (!original_binding.notify_disconnected ().result ()
        || !gateway.update_actor_ref (relocated)) {
        return 7;
    }
    auto relocated_binding =
      manager.bind (relocated).submit ().result ().value ();
    if (!relocated_binding.relay ("packet", zlink::message_t{}).result ()
        || relay_routes.size () != 2
        || relay_routes.back ().node_rid ().value ()
             != relocated.node_rid ().value ()) {
        return 8;
    }
    if (!unaffected_binding.relay ("packet", zlink::message_t{}).result ()
        || relay_routes.size () != 3
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
    const auto disconnected = relocated_binding.notify_disconnected ().result ();
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
    return advanced && advanced->session_sequence == 29 ? 0 : 3;
}

int bound_session_send_does_not_publish_caller_location ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    auto state = std::make_shared<actor_gateway_state_t> ();
    serializer_registry_t serializers;
    state->serializers = &serializers;
    actor_gateway_runtime_t gateway (state);
    const auto source = test_actor_ref (
      "source-node", "player", "send-route-owner", 7);
    const auto target = test_actor_ref (
      "target-node", "player", "send-route-owner", 7);
    std::atomic_int sends{0};
    if (!gateway.bind_session_sink (
          source,
          [&sends] (std::string, stream_codec_t, const zlink::message_t &) {
              ++sends;
              return task_t<void> (result_t<void>::success ());
          })) {
        return 1;
    }

    auto sent = gateway.actor_context (target)
                  .bound_session ()
                  .send (std::string ("push"))
                  .submit ()
                  .result ();
    const auto delivery_deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (1);
    while (sends.load (std::memory_order_acquire) != 1
           && std::chrono::steady_clock::now () < delivery_deadline) {
        std::this_thread::yield ();
    }
    if (!sent || sends.load () != 1) {
        return 2;
    }
    const std::lock_guard lock (state->mutex);
    const auto found = state->actors_by_id.find ("send-route-owner");
    if (found == state->actors_by_id.end ()
        || found->second.ref.node_rid ().value () != "source-node") {
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
        || route_after_rejections->session_sequence != 0) {
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

int bound_session_push_detaches_before_direct_sink_entry ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    auto state = std::make_shared<actor_gateway_state_t> ();
    serializer_registry_t serializers;
    state->serializers = &serializers;
    actor_gateway_runtime_t gateway (state);
    const auto actor = test_actor_ref (
      "actor-owner", "player", "detached-bound-session", 7);

    std::mutex gate_mutex;
    std::condition_variable gate_changed;
    int sender_started = 0;
    bool release_sender = false;
    auto direct_stream_sink =
      [&] (std::string, stream_codec_t,
           const zlink::message_t &) -> task_t<void> {
          const auto ordinal = [&] {
              std::unique_lock lock (gate_mutex);
              const auto value = ++sender_started;
              gate_changed.notify_all ();
              if (value == 1)
                  gate_changed.wait (lock, [&] { return release_sender; });
              return value;
          } ();
          if (ordinal < 1)
              throw framework_exception_t (
                framework_error_kind_t::internal_failure,
                "invalid detached sink order");
          co_return;
      };
    const auto installed = gateway.replace_session_route (
      actor, std::move (direct_stream_sink),
      actor_bound_session_route_t{
        zlink::routing_id_t::from ("session-owner"),
        zlink::routing_id_t::from ("session-rid"),
        7, 11, 13, 17, 19, 23, 0});
    if (!installed) {
        return 1;
    }

    auto submitted = std::async (
      std::launch::async,
      [&] {
          return gateway.actor_context (actor)
            .bound_session ()
            .send (std::string ("joined"))
            .submit ()
            .result ();
      });
    const auto immediate = submitted.wait_for (
      std::chrono::milliseconds (100));
    auto second = gateway.actor_context (actor)
                    .bound_session ()
                    .send (std::string ("joined-again"))
                    .submit ();
    {
        std::unique_lock lock (gate_mutex);
        if (!gate_changed.wait_for (
              lock, std::chrono::seconds (1),
              [&] { return sender_started == 1; })) {
            release_sender = true;
            gate_changed.notify_all ();
            return 2;
        }
        if (sender_started != 1 || !second.await_ready ()) {
            release_sender = true;
            gate_changed.notify_all ();
            return 3;
        }
        release_sender = true;
    }
    gate_changed.notify_all ();
    if (immediate != std::future_status::ready)
        return 4;
    if (!submitted.get () || !second.result ())
        return 5;
    {
        std::unique_lock lock (gate_mutex);
        if (!gate_changed.wait_for (
              lock, std::chrono::seconds (1),
              [&] { return sender_started == 2; })) {
            return 6;
        }
    }
    return 0;
}

int bound_session_transition_is_atomic_and_idempotent ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    actor_gateway_runtime_t gateway;
    const auto actor = test_actor_ref (
      "actor-owner", "game.actor", "actor-a", 1);
    const auto sink = [] (std::string, stream_codec_t,
                          const zlink::message_t &) {
        return task_t<void> (result_t<void>::success ());
    };
    if (!gateway.bind_session_sink (actor, sink))
        return 1;

    const actor_bound_session_route_t first{
      zlink::routing_id_t::from (std::string ("session-owner-a")),
      zlink::routing_id_t::from (std::string ("session-a")),
      1, 2, 3, 4, 5, 7, 9};
    const auto installed =
      gateway.record_bound_session_route_transition (actor, first);
    if (!installed || !installed.value ().changed
        || installed.value ().previous)
        return 2;

    auto same_identity = first;
    same_identity.session_sequence = 0;
    const auto idempotent =
      gateway.record_bound_session_route_transition (
        actor, same_identity);
    if (!idempotent || idempotent.value ().changed
        || idempotent.value ().previous
        || !idempotent.value ().current
        || idempotent.value ().current->binding_token != 7
        || idempotent.value ().current->session_sequence != 9)
        return 3;

    auto authority_update = first;
    authority_update.authority_owner_generation = 11;
    authority_update.owner_lease_generation = 13;
    authority_update.binding_token = 0;
    authority_update.session_sequence = 0;
    const auto retained =
      gateway.record_bound_session_route_transition (
        test_actor_ref ("actor-target", "game.actor", "actor-a", 1),
        authority_update);
    const auto retained_route = gateway.bound_session_route (actor);
    if (!retained || retained.value ().changed
        || retained.value ().previous || !retained_route
        || retained_route->authority_owner_generation != 11
        || retained_route->owner_lease_generation != 13
        || retained_route->binding_token != 7
        || retained_route->session_sequence != 9)
        return 4;

    auto second = authority_update;
    second.session_rid = zlink::routing_id_t::from (
      std::string ("session-b"));
    second.binding_generation = 6;
    const auto replaced =
      gateway.record_bound_session_route_transition (actor, second);
    if (!replaced || !replaced.value ().changed
        || !replaced.value ().previous
        || replaced.value ().previous->session_rid
             != first.session_rid
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
    const auto source = test_actor_ref (
      "actor-source", "game.actor", "actor-physical", 7);
    const auto target = test_actor_ref (
      "actor-target", "game.actor", "actor-physical", 7);
    const auto session_owner = zlink::routing_id_t::from (
      std::string ("session-owner"));
    const auto session_rid = zlink::routing_id_t::from (
      std::string ("session-rid"));
    std::atomic_int source_sink_calls{0};
    std::atomic_int target_sink_calls{0};
    const auto source_sink = [&source_sink_calls] (
                               std::string, stream_codec_t,
                               const zlink::message_t &) {
        ++source_sink_calls;
        return task_t<void> (result_t<void>::success ());
    };
    const auto target_sink = [&target_sink_calls] (
                               std::string, stream_codec_t,
                               const zlink::message_t &) {
        ++target_sink_calls;
        return task_t<void> (result_t<void>::success ());
    };

    const actor_bound_session_route_t initial{
      session_owner, session_rid, 7, 17, 19, 23, 29, 31, 37};
    const auto installed = gateway.replace_session_route (
      source, source_sink, initial, stream_codec_t::message_pack);
    if (!installed || !installed.value ().changed
        || installed.value ().previous)
        return 1;

    auto relocated = initial;
    relocated.authority_owner_generation = 41;
    relocated.owner_lease_generation = 43;
    relocated.binding_token = 0;
    relocated.session_sequence = 0;
    const auto updated = gateway.replace_session_route (
      target, target_sink, relocated, stream_codec_t::message_pack);
    const auto current = gateway.bound_session_route (target);
    if (!updated || updated.value ().changed
        || updated.value ().previous || !current
        || current->authority_owner_generation != 41
        || current->owner_lease_generation != 43
        || current->binding_token != 31
        || current->session_sequence != 37
        || !gateway.actor_bound ("actor-physical")
        || gateway.actor_disconnected ("actor-physical"))
        return 2;
    const auto dispatched = gateway.dispatch_bound_session_send (
      target, "AuthorityChangedNotify", stream_codec_t::message_pack,
      zlink::message_t{});
    if (!dispatched || source_sink_calls.load () != 0
        || target_sink_calls.load () != 1)
        return 3;

    auto replacement = relocated;
    replacement.session_rid = zlink::routing_id_t::from (
      std::string ("replacement-session"));
    replacement.binding_generation = 47;
    const auto replaced = gateway.replace_session_route (
      target, target_sink, replacement, stream_codec_t::message_pack);
    if (!replaced || !replaced.value ().changed
        || !replaced.value ().previous
        || replaced.value ().previous->session_rid != session_rid
        || replaced.value ().previous->binding_generation != 29)
        return 4;

    auto local_state = std::make_shared<actor_gateway_state_t> ();
    actor_gateway_runtime_t local_gateway (local_state);
    zlink_builder_t builder;
    builder.stream ("authority-stream-sink").bind ("tcp://127.0.0.1:0");
    auto runtime = stream_runtime_t::from (builder);
    auto stream = runtime.open_session ("authority-stream-sink");
    std::atomic_int local_stream_calls{0};
    std::atomic_int replacement_sink_calls{0};
    runtime.attach_transport_writer (
      stream,
      [&local_stream_calls] (const stream_header_t &,
                             const zlink::message_t &,
                             std::optional<std::chrono::milliseconds>) {
          ++local_stream_calls;
          return task_t<void> (result_t<void>::success ());
      });
    local_gateway.bind_session_stream (
      "actor-local-stream", stream, stream_codec_t::message_pack,
      "physical-session", 53,
      test_actor_ref ("actor-source", "game.actor",
                      "actor-local-stream", 59));
    const actor_bound_session_route_t local_initial{
      session_owner, session_rid, 59, 61, 67, 71, 73, 53, 79};
    const auto local_installed = local_gateway.record_bound_session_route (
      test_actor_ref ("actor-source", "game.actor",
                      "actor-local-stream", 59),
      session_owner, session_rid, 61, 67, 71, 73, 53, 79);
    if (!local_installed)
        return 5;
    auto local_relocated = local_initial;
    local_relocated.authority_owner_generation = 83;
    local_relocated.owner_lease_generation = 89;
    local_relocated.binding_token = 0;
    local_relocated.session_sequence = 0;
    const auto local_updated = local_gateway.replace_session_route (
      test_actor_ref ("actor-target", "game.actor",
                      "actor-local-stream", 59),
      [&replacement_sink_calls] (std::string, stream_codec_t,
                                 const zlink::message_t &) {
          ++replacement_sink_calls;
          return task_t<void> (result_t<void>::success ());
      },
      local_relocated, stream_codec_t::message_pack);
    if (!local_updated || local_updated.value ().changed)
        return 6;
    const auto local_dispatched = local_gateway.dispatch_bound_session_send (
      test_actor_ref ("actor-target", "game.actor",
                      "actor-local-stream", 59),
      "AuthorityChangedNotify", stream_codec_t::message_pack,
      zlink::message_t{});
    if (!local_dispatched || local_stream_calls.load () != 1
        || replacement_sink_calls.load () != 0)
        return 7;
    auto local_rebound = local_relocated;
    local_rebound.session_rid = zlink::routing_id_t::from (
      std::string ("replacement-session"));
    local_rebound.binding_generation = 97;
    const auto rebound = local_gateway.replace_session_route (
      test_actor_ref ("actor-target", "game.actor",
                      "actor-local-stream", 59),
      [&replacement_sink_calls] (std::string, stream_codec_t,
                                 const zlink::message_t &) {
          ++replacement_sink_calls;
          return task_t<void> (result_t<void>::success ());
      },
      local_rebound, stream_codec_t::message_pack);
    if (!rebound || !rebound.value ().changed
        || !rebound.value ().previous
        || rebound.value ().previous->session_rid != session_rid
        || rebound.value ().previous->binding_generation != 73)
        return 8;
    const auto rebound_dispatch = local_gateway.dispatch_bound_session_send (
      test_actor_ref ("actor-target", "game.actor",
                      "actor-local-stream", 59),
      "ReconnectedNotify", stream_codec_t::message_pack,
      zlink::message_t{});
    if (!rebound_dispatch || local_stream_calls.load () != 1
        || replacement_sink_calls.load () != 1)
        return 9;
    return 0;
}

int bound_session_relay_admission_is_exact_and_monotonic ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    auto state = std::make_shared<actor_gateway_state_t> ();
    actor_gateway_runtime_t gateway (state);
    const auto actor = test_actor_ref ("actor-owner", "game.actor", "relay-actor", 3);
    if (!gateway.bind_session_sink (
          actor, [] (std::string, stream_codec_t, const zlink::message_t &) {
              return task_t<void> (result_t<void>::success ());
          })) {
        return 1;
    }
    const auto session_owner = zlink::routing_id_t::from (std::string ("session-owner"));
    const auto session_rid = zlink::routing_id_t::from (std::string ("session-rid"));
    if (!gateway.record_bound_session_route (actor, session_owner, session_rid,
                                             7, 11, 13, 17, 0, 0)) {
        return 2;
    }
    if (!gateway.begin_session_relay_completion (
          actor, session_owner, session_rid, 17, 1))
        return 3;
    if (!gateway.admit_session_relay (actor, session_owner, session_rid, 17, 1))
        return 4;
    if (!gateway.complete_session_relay (
          actor, session_owner, session_rid, 17, 1)) {
        return 5;
    }
    if (gateway.complete_session_relay (
          actor, session_owner, session_rid, 17, 1))
        return 6;
    if (gateway.admit_session_relay (actor, session_owner, session_rid, 17, 1)
        || gateway.admit_session_relay (actor, session_owner, session_rid, 17, 3)
        || gateway.admit_session_relay (
          actor, zlink::routing_id_t::from (std::string ("other-owner")),
          session_rid, 17, 2)) {
        return 7;
    }
    if (!gateway.begin_session_relay_completion (
          actor, session_owner, session_rid, 17, 2))
        return 8;
    if (!gateway.admit_session_relay (actor, session_owner, session_rid, 17, 2))
        return 9;
    if (!gateway.complete_session_relay (
          actor, session_owner, session_rid, 17, 2)
        || gateway.complete_session_relay (
          actor, session_owner, session_rid, 17, 4)) {
        return 10;
    }
    const auto route = gateway.bound_session_route (actor);
    if (!route || route->session_sequence != 2)
        return 11;

    if (!gateway.begin_session_relay_completion (
          actor, session_owner, session_rid, 17, 3))
        return 12;
    if (!gateway.destroy_actor (actor))
        return 13;
    if (gateway.complete_session_relay (
          actor, zlink::routing_id_t::from (std::string ("other-owner")),
          session_rid, 17, 3))
        return 14;
    if (!gateway.complete_session_relay (
          actor, session_owner, session_rid, 17, 3))
        return 15;
    {
        const std::lock_guard lock (state->mutex);
        if (!state->active_session_relay_completions.empty ())
            return 16;
    }
    if (gateway.complete_session_relay (
          actor, session_owner, session_rid, 17, 3)
        || gateway.begin_session_relay_completion (
          actor, session_owner, session_rid, 17, 4))
        return 17;

    if (!gateway.bind_session_sink (
          actor, [] (std::string, stream_codec_t, const zlink::message_t &) {
              return task_t<void> (result_t<void>::success ());
          })
        || !gateway.record_bound_session_route (
          actor, session_owner, session_rid, 7, 11, 13, 17, 0, 0)
        || !gateway.begin_session_relay_completion (
          actor, session_owner, session_rid, 17, 1))
        return 18;
    {
        const std::lock_guard lock (state->mutex);
        state->actors_by_id.at ("relay-actor")
          .bound_session_route->session_sequence = 3;
    }
    if (gateway.complete_session_relay (
          actor, session_owner, session_rid, 17, 1))
        return 19;
    {
        const std::lock_guard lock (state->mutex);
        if (!state->active_session_relay_completions.empty ())
            return 20;
    }
    return 0;
}

int session_relay_queue_is_ordered_without_blocking_other_actors ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    actor_gateway_runtime_t gateway;
    auto manager = gateway.manager ();
    session_actor_manager_access_t::attach (manager, stream_t{});
    const auto actor_a = test_actor_ref ("actor-owner", "game.actor", "relay-a", 1);
    const auto actor_b = test_actor_ref ("actor-owner", "game.actor", "relay-b", 1);
    auto bound_a = manager.bind (actor_a).submit ().result ().value ();
    auto bound_b = manager.bind (actor_b).submit ().result ().value ();
    const auto session_rid = zlink::routing_id_t::from (std::string ("relay-session"));
    if (!gateway.record_session_relay_source (actor_a, session_rid, 31)
        || !gateway.record_session_relay_source (actor_b, session_rid, 37)) {
        return 1;
    }

    std::mutex mutex;
    std::condition_variable changed;
    auto first_completion = std::make_shared<
      task_completion_source_t<std::optional<zlink::message_t>>> ();
    std::vector<std::string> started;
    std::vector<std::pair<std::string, std::uint64_t>> sources;
    gateway.on_relay (
      [&] (const actor_ref_t &actor, actor_context_t, const stream_header_t &header,
           const zlink::message_t &, std::optional<bound_session_relay_source_t> source) {
          const auto marker = std::string (header.packet_name ());
          {
              const std::lock_guard lock (mutex);
              started.push_back (marker);
              if (source)
                  sources.emplace_back (marker, source->session_sequence);
          }
          changed.notify_all ();
          if (actor.actor_id ().value () == "relay-a" && marker == "A1") {
              return first_completion->task ();
          }
          return task_t<std::optional<zlink::message_t>> (
            result_t<std::optional<zlink::message_t>>::success (std::nullopt));
      });

    auto first = bound_a.relay ("A1", zlink::message_t{});
    auto second = bound_a.relay ("A2", zlink::message_t{});
    auto independent = bound_b.relay ("B1", zlink::message_t{});
    {
        std::unique_lock lock (mutex);
        if (!changed.wait_for (lock, std::chrono::seconds (2), [&] {
                return std::find (started.begin (), started.end (), "A1") != started.end ()
                       && std::find (started.begin (), started.end (), "B1") != started.end ();
            })) {
            return 2;
        }
        if (std::find (started.begin (), started.end (), "A2") != started.end ())
            return 3;
    }
    first_completion->complete (
      result_t<std::optional<zlink::message_t>>::success (std::nullopt));
    if (!first.result () || !second.result () || !independent.result ())
        return 4;
    const auto a1 = std::find (started.begin (), started.end (), "A1");
    const auto a2 = std::find (started.begin (), started.end (), "A2");
    if (a1 == started.end () || a2 == started.end () || a1 > a2)
        return 5;
    const std::vector<std::pair<std::string, std::uint64_t>> expected_sources{
      {"A1", 1}, {"B1", 1}, {"A2", 2}};
    for (const auto &expected : expected_sources) {
        if (std::find (sources.begin (), sources.end (), expected) == sources.end ())
            return 6;
    }
    return 0;
}

zlink::framework::task_t<std::optional<zlink::message_t>>
inspect_pending_relay_arguments (
  const zlink::framework::actor_ref_t &actor,
  const zlink::framework::detail::stream_header_t &header,
  const zlink::message_t &payload,
  const std::shared_ptr<
    zlink::framework::detail::task_completion_source_t<void>> &pending,
  const std::shared_ptr<std::atomic_bool> &started,
  std::string expected_actor_id,
  std::string expected_packet_name,
  std::string expected_payload)
{
    started->store (true, std::memory_order_release);
    co_await pending->task ();
    if (actor.actor_id ().value () != expected_actor_id
        || header.packet_name () != expected_packet_name
        || payload.to_string () != expected_payload) {
        throw std::runtime_error (
          "pending actor relay did not retain its request arguments");
    }
    co_return zlink::message_t::from ("delayed-reply");
}

zlink::framework::task_t<void>
inspect_pending_disconnect_argument (
  const zlink::framework::actor_ref_t &actor,
  const std::shared_ptr<
    zlink::framework::detail::task_completion_source_t<void>> &pending,
  const std::shared_ptr<std::atomic_bool> &started,
  std::string expected_actor_id)
{
    started->store (true, std::memory_order_release);
    co_await pending->task ();
    if (actor.actor_id ().value () != expected_actor_id) {
        throw std::runtime_error (
          "pending actor disconnect did not retain its actor argument");
    }
    co_return;
}

int disconnect_notification_survives_pending_dispatcher_completion ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    auto state = std::make_shared<actor_gateway_state_t> ();
    actor_gateway_runtime_t gateway (state);
    auto manager = gateway.manager ();
    session_actor_manager_access_t::attach (manager, stream_t{});
    const std::string actor_id (96, 'd');
    const auto actor =
      test_actor_ref ("actor-owner", "game.actor", actor_id, 1);
    auto bound = manager.bind (actor).submit ().result ().value ();
    auto pending = std::make_shared<task_completion_source_t<void>> ();
    auto disconnect_started = std::make_shared<std::atomic_bool> (false);
    gateway.on_disconnect (
      [pending, disconnect_started, actor_id] (const actor_ref_t &disconnected_actor) {
          return inspect_pending_disconnect_argument (
            disconnected_actor, pending, disconnect_started, actor_id);
      });

    auto notification = bound.notify_disconnected ();
    if (!disconnect_started->load (std::memory_order_acquire))
        return 1;

    std::vector<std::string> reclaimed_storage;
    reclaimed_storage.reserve (8192);
    for (std::size_t index = 0; index < 8192; ++index) {
        reclaimed_storage.emplace_back (
          96, static_cast<char> ('A' + (index % 26)));
    }
    std::thread completion ([pending] {
        pending->complete (result_t<void>::success ());
    });
    completion.join ();
    if (!notification.result ())
        return 2;

    const std::lock_guard lock (state->mutex);
    const auto found = state->actors_by_id.find (actor_id);
    return found != state->actors_by_id.end ()
               && found->second.binding_token == 0
               && found->second.binding_session_id.empty ()
             ? 0
             : 3;
}

int relay_request_survives_pending_dispatcher_completion ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    actor_gateway_runtime_t gateway;
    auto manager = gateway.manager ();
    session_actor_manager_access_t::attach (manager, stream_t{});
    const std::string actor_id (96, 'a');
    const std::string packet_name (96, 'p');
    const std::string payload_text (4096, 'v');
    const auto actor =
      test_actor_ref ("actor-owner", "game.actor", actor_id, 1);
    auto bound = manager.bind (actor).submit ().result ().value ();
    auto pending = std::make_shared<task_completion_source_t<void>> ();
    auto relay_started = std::make_shared<std::atomic_bool> (false);
    gateway.on_relay (
      [pending, relay_started, actor_id, packet_name, payload_text] (
           const actor_ref_t &relayed_actor, actor_context_t,
           const stream_header_t &header, const zlink::message_t &payload,
           std::optional<bound_session_relay_source_t>) {
          return inspect_pending_relay_arguments (
            relayed_actor, header, payload, pending, relay_started,
            actor_id, packet_name, payload_text);
      });

    auto request = bound.relay_request (
      packet_name, zlink::message_t::from (payload_text)).submit ();
    if (!relay_started->load (std::memory_order_acquire))
        return 1;

    std::vector<std::string> reclaimed_storage;
    reclaimed_storage.reserve (8192);
    for (std::size_t index = 0; index < 8192; ++index) {
        reclaimed_storage.emplace_back (4096, static_cast<char> ('A' + (index % 26)));
    }
    std::thread completion ([pending] {
        pending->complete (result_t<void>::success ());
    });
    completion.join ();
    const auto completed = request.result ();
    return completed && completed.value ().to_string () == "delayed-reply" ? 0 : 2;
}

int relay_send_survives_pending_dispatcher_completion ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    actor_gateway_runtime_t gateway;
    auto manager = gateway.manager ();
    session_actor_manager_access_t::attach (manager, stream_t{});
    const std::string actor_id (96, 's');
    const std::string packet_name (96, 'n');
    const std::string payload_text (4096, 'b');
    const auto actor =
      test_actor_ref ("actor-owner", "game.actor", actor_id, 1);
    auto bound = manager.bind (actor).submit ().result ().value ();
    auto pending = std::make_shared<task_completion_source_t<void>> ();
    auto relay_started = std::make_shared<std::atomic_bool> (false);
    gateway.on_relay (
      [pending, relay_started, actor_id, packet_name, payload_text] (
           const actor_ref_t &relayed_actor, actor_context_t,
           const stream_header_t &header, const zlink::message_t &payload,
           std::optional<bound_session_relay_source_t>) {
          return inspect_pending_relay_arguments (
            relayed_actor, header, payload, pending, relay_started,
            actor_id, packet_name, payload_text);
      });

    auto relayed = bound.relay (
      packet_name, zlink::message_t::from (payload_text));
    if (!relay_started->load (std::memory_order_acquire))
        return 1;

    std::vector<std::string> reclaimed_storage;
    reclaimed_storage.reserve (8192);
    for (std::size_t index = 0; index < 8192; ++index) {
        reclaimed_storage.emplace_back (4096, static_cast<char> ('A' + (index % 26)));
    }
    std::thread completion ([pending] {
        pending->complete (result_t<void>::success ());
    });
    completion.join ();
    return relayed.result () ? 0 : 2;
}

int actor_send_reports_fifo_admission_before_handler_terminal ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace runtime = zlink::framework::runtime;

    serializer_registry_t serializers;
    auto node = std::make_shared<spot_node_builder_state_t> (
      "actor-send-admission-node");
    node->worker_executor = std::make_shared<runtime::offload_executor_t> (
      1, 16, "actor-send-admission");
    node->channel_runtime = std::make_shared<channel_runtime_state_t> ();
    node->channel_runtime->serializers = &serializers;

    auto spot = std::make_shared<spot_context_state_t> ();
    spot->node = node;
    spot->node_rid = node_rid_t::from_string (
      "actor-send-admission-node");
    spot->spot_id = spot_id_t ("actor-send-admission-spot");
    spot->spot_name = "actor-send-admission";
    spot->spot_instance = std::make_shared<int> (1);
    spot->channel_runtime = std::make_shared<channel_runtime_state_t> ();
    spot->channel_runtime->serializers = &serializers;
    spot->serial_executor = node->worker_executor;
    node->spot_contexts_by_id.emplace (
      spot->spot_id, spot_context_access_t::create (spot));

    std::atomic_bool disconnected_started{false};
    spot_actor_admission_callbacks_t admission_callbacks;
    admission_callbacks.on_disconnect_actor =
      [&disconnected_started] (void *, void *) -> task_t<void> {
          disconnected_started.store (true, std::memory_order_release);
          co_return;
      };
    spot->actor_admissions.emplace (
      std::type_index (typeid (int)), std::move (admission_callbacks));

    spot_node_builder_state_t::actor_factory_registration_t factory;
    factory.actor_type = std::type_index (typeid (int));
    factory.create_instance = [] (std::string) {
        return std::make_shared<int> (7);
    };
    factory.configure_instance = [] (void *, const actor_ref_t &, void *) {};
    node->actor_factories.emplace ("player", std::move (factory));

    const auto actor = test_actor_ref (
      "actor-send-admission-node", "player", "reconnect-player", 1);
    const auto actor_key = std::string ("player:reconnect-player");
    node->actor_instances.emplace (actor_key, std::make_shared<int> (7));
    node->actor_spot_ids.emplace (actor_key, spot->spot_id);
    node->actor_generations.emplace (actor_key, 1);

    auto handler_terminal =
      std::make_shared<task_completion_source_t<void>> ();
    std::atomic_bool handler_started{false};
    spot->handlers.push_back (spot_handler_descriptor_t{
      spot_handler_kind_t::actor_send, "JoinGameMsg", "",
      std::type_index (typeid (int)), std::type_index (typeid (void)),
      std::type_index (typeid (int)), std::type_index (typeid (void))});
    spot->handler_invokers.push_back (
      [handler_terminal, &handler_started] (
        void *, void *, service_provider_t &, serializer_registry_t &,
        const zlink::message_t &, const spot_inbound_message_t &)
        -> task_t<zlink::message_t> {
          handler_started.store (true, std::memory_order_release);
          co_await handler_terminal->task ();
          co_return zlink::message_t{};
      });

    service_collection_t services;
    auto provider = services.build_provider ();
    actor_gateway_runtime_t gateway;
    std::atomic_int admitted{0};
    auto relayed = spot_node_runtime_t (node).relay_actor_packet (
      actor, gateway.actor_context (actor), stream_message_kind_t::send,
      "JoinGameMsg", zlink::message_t::from ("join"), provider,
      serializers, {}, nullptr, {},
      [&admitted] {
          admitted.fetch_add (1, std::memory_order_acq_rel);
      });

    const auto handler_deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (1);
    while (!handler_started.load (std::memory_order_acquire)
           && std::chrono::steady_clock::now () < handler_deadline) {
        std::this_thread::yield ();
    }
    if (admitted.load (std::memory_order_acquire) != 1
        || relayed.await_ready ()) {
        return 1;
    }
    auto disconnected = std::async (
      std::launch::async,
      [node, actor] {
          return spot_node_runtime_t (node)
            .notify_actor_disconnected_erased (actor);
      });
    if (disconnected.wait_for (std::chrono::milliseconds (50))
          != std::future_status::timeout
        || disconnected_started.load (std::memory_order_acquire)) {
        handler_terminal->complete (result_t<void>::success ());
        (void) disconnected.get ();
        return 2;
    }
    handler_terminal->complete (result_t<void>::success ());
    if (!relayed.result ())
        return 3;
    if (disconnected.wait_for (std::chrono::seconds (1))
          != std::future_status::ready) {
        return 4;
    }
    const auto disconnected_result = disconnected.get ();
    return disconnected_result
             && disconnected_started.load (std::memory_order_acquire)
           ? 0
           : 5;
}

int rebound_session_keeps_prior_ingress_exact_fence ()
{
    using namespace zlink::framework::runtime::stateful;

    const object_ref_t actor{
      object_kind_t::actor, "reconnect-player", 1, 7, "player", "node-a"};
    stream_session_registry_t sessions (
      [actor] (const std::string &actor_id) -> std::optional<object_ref_t> {
          return actor_id == actor.key ? std::make_optional (actor)
                                       : std::nullopt;
      });
    const auto connection = sessions.open ("reconnect-session");
    const auto [first_error, first] = sessions.bind (
      connection, actor, 11, 13);
    if (first_error != stateful_error_t::none)
        return 1;
    const auto [admit_error, admitted] = sessions.admit_inbound (first);
    if (admit_error != stateful_error_t::none || !admitted)
        return 2;

    const auto [rebind_error, rebound] = sessions.bind (
      connection, actor, 11, 13);
    if (rebind_error != stateful_error_t::none
        || rebound.binding_generation == first.binding_generation) {
        return 3;
    }
    const auto seal = sessions.seal_remote_route (
      connection.connection_id, rebound.binding_generation,
      actor, 11, 13);
    if (seal.error != stateful_error_t::backpressured
        || sessions.remote_route_seal_ready (seal.barrier)) {
        return 4;
    }
    if (sessions.complete_inbound (*admitted)
          != stateful_error_t::none
        || !sessions.remote_route_seal_ready (seal.barrier)) {
        return 5;
    }
    return sessions.abort_barrier (seal.barrier)
               == stateful_error_t::none
             ? 0
             : 6;
}

int reconnect_binding_publish_holds_new_route_push ()
{
    using namespace zlink::framework::runtime::stateful;

    const object_ref_t actor{
      object_kind_t::actor, "reconnect-route-actor", 7, 11,
      "player", "actor-owner"};
    stream_session_registry_t sessions (
      [actor] (const std::string &actor_id)
        -> std::optional<object_ref_t> {
          return actor_id == actor.key ? std::make_optional (actor)
                                       : std::nullopt;
      });
    const auto old_connection = sessions.open ("old-session-rid");
    const auto [old_error, old_binding] = sessions.bind_remote (
      old_connection, actor, 13, 17);
    if (old_error != stateful_error_t::none)
        return 1;

    const auto new_connection = sessions.open ("new-session-rid");
    const auto [new_error, new_binding] = sessions.bind_remote (
      new_connection, actor, 13, 17, true);
    if (new_error != stateful_error_t::none
        || new_binding.binding_generation
             <= old_binding.binding_generation)
        return 2;

    std::atomic_int settled{0};
    std::atomic_bool delivered{false};
    const stream_remote_tenure_t new_tenure{
      actor.key, actor.object_generation,
      actor.authority_owner_generation, actor.node_id, 13, 17,
      new_binding.binding_generation};
    const auto held = sessions.admit_outbound (
      new_tenure, std::nullopt,
      [&] (bool accepted) {
          delivered.store (accepted, std::memory_order_release);
          settled.fetch_add (1, std::memory_order_acq_rel);
      });
    if (held.error != stateful_error_t::none
        || held.kind != stream_outbound_admission_kind_t::retained
        || settled.load (std::memory_order_acquire) != 0)
        return 3;

    auto retained = sessions.complete_route_publish (new_binding);
    if (!retained)
        return 4;
    for (auto &settle : *retained)
        settle (true);
    if (settled.load (std::memory_order_acquire) != 1
        || !delivered.load (std::memory_order_acquire))
        return 5;

    const stream_remote_tenure_t stale_tenure{
      actor.key, actor.object_generation,
      actor.authority_owner_generation, actor.node_id, 13, 17,
      old_binding.binding_generation};
    return sessions.admit_outbound (
             stale_tenure, std::nullopt, [] (bool) {})
               .error == stateful_error_t::conflict
             ? 0
             : 6;
}

int reconnect_push_reaches_new_session_across_two_hosts ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    auto actor_owner_state = std::make_shared<actor_gateway_state_t> ();
    auto session_owner_state = std::make_shared<actor_gateway_state_t> ();
    serializer_registry_t serializers;
    actor_owner_state->serializers = &serializers;
    session_owner_state->serializers = &serializers;
    actor_gateway_runtime_t actor_owner (actor_owner_state);
    actor_gateway_runtime_t session_owner (session_owner_state);
    const auto actor = test_actor_ref (
      "actor-owner", "player", "two-host-reconnect", 7);
    const auto session_owner_rid = zlink::routing_id_t::from (
      "session-owner");
    const zlink::framework::runtime::stateful::object_ref_t session_actor{
      zlink::framework::runtime::stateful::object_kind_t::actor,
      "two-host-reconnect", 7, 13, "player", "actor-owner"};
    zlink::framework::runtime::stateful::stream_session_registry_t
      stream_sessions (
        [session_actor] (const std::string &actor_id)
          -> std::optional<zlink::framework::runtime::stateful::object_ref_t> {
            return actor_id == session_actor.key
                     ? std::make_optional (session_actor)
                     : std::nullopt;
        });
    const auto old_connection = stream_sessions.open ("old-session");
    const auto [old_bind_error, old_binding] =
      stream_sessions.bind_remote (
        old_connection, session_actor, 11, 17);
    const auto new_connection = stream_sessions.open ("new-session");
    const auto [new_bind_error, new_binding] =
      stream_sessions.bind_remote (
        new_connection, session_actor, 11, 17, true);
    if (old_bind_error
          != zlink::framework::runtime::stateful::stateful_error_t::none
        || new_bind_error
          != zlink::framework::runtime::stateful::stateful_error_t::none
        || new_binding.binding_generation
             <= old_binding.binding_generation) {
        return 1;
    }
    const actor_bound_session_route_t old_route{
      session_owner_rid, zlink::routing_id_t::from ("old-session"),
      7, 11, 13, 17, old_binding.binding_generation, 0, 0};
    const actor_bound_session_route_t new_route{
      session_owner_rid, zlink::routing_id_t::from ("new-session"),
      7, 11, 13, 17, new_binding.binding_generation, 0, 0};

    std::atomic_int old_client_received{0};
    std::atomic_int new_client_received{0};
    if (!session_owner.replace_session_route (
          actor,
          [&old_client_received] (std::string, stream_codec_t,
                                  const zlink::message_t &) {
              old_client_received.fetch_add (1, std::memory_order_acq_rel);
              return task_t<void> (result_t<void>::success ());
          },
          old_route)) {
        return 2;
    }

    std::atomic_bool saw_correlated_split{false};
    dispatch_options_t trace_options;
    trace_options.message_flow (message_flow_log_mode_t::detailed);
    dispatch_options_access_t::set_observer_for_tests (
      trace_options,
      [&] (const message_flow_event_t &event) {
          if (event.outcome == message_flow_outcome_t::admitted
              && event.surface == dispatch_error_surface_t::stream_session
              && event.message_kind == dispatch_message_kind_t::send
              && event.detail_stage
                == std::optional<std::string> (
                  "actor_owner_push_target")
              && event.detail_result
              && event.detail_result->find (
                   "current=session_rid="
                   + new_route.session_rid->to_hex () + "/bg="
                   + std::to_string (new_binding.binding_generation))
                   != std::string::npos
              && event.detail_result->find (
                   "staged=session_rid="
                   + old_route.session_rid->to_hex () + "/bg="
                   + std::to_string (old_binding.binding_generation))
                   != std::string::npos) {
              saw_correlated_split.store (true, std::memory_order_release);
          }
      });
    actor_owner.set_dispatch (std::move (trace_options));

    const auto make_remote_sink =
      [&actor_owner, &session_owner, actor] (
        actor_bound_session_route_t staged_route) {
          return [&actor_owner, &session_owner, actor,
                  staged_route = std::move (staged_route)] (
                   std::string packet_name, stream_codec_t codec,
                   const zlink::message_t &payload) mutable -> task_t<void> {
              const auto current = actor_owner.resolve_bound_session_push_route (
                actor, staged_route);
              if (!current) {
                  throw framework_exception_t (
                    framework_error_kind_t::not_configured,
                    "current reconnect route is unavailable");
              }
              auto delivery = session_owner.admit_bound_session_delivery (
                actor, current->binding_generation);
              if (!delivery) {
                  throw framework_exception_t (
                    framework_error_kind_t::not_configured,
                    "new Session host rejected the push");
              }
              const auto delivered = (*delivery) (
                std::move (packet_name), codec, payload);
              if (!delivered) {
                  throw delivered.error ()
                          ? *delivered.error ()
                          : framework_exception_t (
                              framework_error_kind_t::internal_failure,
                              "new Session client delivery failed");
              }
              co_return;
          };
      };

    if (!actor_owner.replace_session_route (
          actor, make_remote_sink (old_route), old_route)) {
        return 3;
    }
    std::shared_ptr<bound_session_sink_t> staged_old_sink;
    {
        const std::lock_guard lock (actor_owner_state->mutex);
        staged_old_sink = actor_owner_state->bound_session_sinks.at (
          "two-host-reconnect");
    }

    if (!session_owner.replace_session_route (
          actor,
          [&new_client_received] (std::string, stream_codec_t,
                                  const zlink::message_t &) {
              new_client_received.fetch_add (1, std::memory_order_acq_rel);
              return task_t<void> (result_t<void>::success ());
          },
          new_route)
        || !actor_owner.replace_session_route (
          actor, make_remote_sink (new_route), new_route)) {
        return 4;
    }
    if (!stream_sessions.complete_route_publish (new_binding))
        return 5;

    const auto stale_capability_push = (*staged_old_sink) (
      "reconnected-push", stream_codec_t::message_pack,
      zlink::message_t::from ("payload")).result ();
    if (!stale_capability_push)
        return 6;

    const auto public_push = actor_owner.actor_context (actor)
                               .bound_session ()
                               .send (std::string ("public-push"))
                               .submit ()
                               .result ();
    if (!public_push)
        return 7;
    const auto deadline = std::chrono::steady_clock::now ()
                          + std::chrono::seconds (1);
    while ((new_client_received.load (std::memory_order_acquire) != 2
            || !saw_correlated_split.load (std::memory_order_acquire))
           && std::chrono::steady_clock::now () < deadline) {
        std::this_thread::yield ();
    }
    return old_client_received.load (std::memory_order_acquire) == 0
             && new_client_received.load (std::memory_order_acquire) == 2
             && saw_correlated_split.load (std::memory_order_acquire)
           ? 0
           : 8;
}

} // namespace

int main ()
{
    if (const auto reconnect =
          reconnect_push_reaches_new_session_across_two_hosts ();
        reconnect != 0) {
        return 240 + reconnect;
    }
    if (const auto reconnect =
          reconnect_binding_publish_holds_new_route_push ();
        reconnect != 0) {
        return 230 + reconnect;
    }
    if (const auto detached =
          bound_session_push_detaches_before_direct_sink_entry ();
        detached != 0) {
        return 220 + detached;
    }
    if (const auto fence =
          rebound_session_keeps_prior_ingress_exact_fence ();
        fence != 0) {
        return 210 + fence;
    }
    if (const auto admission =
          actor_send_reports_fifo_admission_before_handler_terminal ();
        admission != 0) {
        return 200 + admission;
    }
    if (const auto pending =
          disconnect_notification_survives_pending_dispatcher_completion ();
        pending != 0) {
        return 190 + pending;
    }
    if (const auto pending =
          relay_request_survives_pending_dispatcher_completion ();
        pending != 0) {
        return 160 + pending;
    }
    if (const auto pending =
          relay_send_survives_pending_dispatcher_completion ();
        pending != 0) {
        return 170 + pending;
    }
    if (const auto queue = session_relay_queue_is_ordered_without_blocking_other_actors ();
        queue != 0) {
        return 150 + queue;
    }
    if (const auto admission = bound_session_relay_admission_is_exact_and_monotonic ();
        admission != 0) {
        return 140 + admission;
    }
    if (const auto transition =
          bound_session_transition_is_atomic_and_idempotent ();
        transition != 0) {
        return 130 + transition;
    }
    if (const auto route_update =
          authority_only_route_update_keeps_physical_session_current ();
        route_update != 0) {
        return 180 + route_update;
    }
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
    if (const auto bound_send =
          bound_session_send_does_not_publish_caller_location ();
        bound_send != 0) {
        return 95 + bound_send;
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
    if (const auto replaced = replaced_session_find_is_exact_and_disconnects_once ();
        replaced != 0) {
        return 10 + replaced;
    }
    if (const auto atomic_rebind =
          direct_rebind_publication_is_atomic_and_old_disconnect_is_fenced ();
        atomic_rebind != 0) {
        return 160 + atomic_rebind;
    }
    if (const auto stale_destroy =
          destroyed_or_recreated_actor_ignores_stale_disconnect_handle ();
        stale_destroy != 0) {
        return 170 + stale_destroy;
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
