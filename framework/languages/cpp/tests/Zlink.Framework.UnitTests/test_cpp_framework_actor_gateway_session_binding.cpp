/* SPDX-License-Identifier: Elastic-2.0 */

#include "runtime/stateful/stream_session_registry.hpp"
#include "../support/request_admission_fixture.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "../support/actor_gateway_cases.hpp"
#include "../support/actor_gateway_fixture.hpp"
#include "../support/actor_gateway_task_result.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/streams/stream_runtime.hpp"
#include <nlohmann/json.hpp>
#include <zlink/framework/contracts/configuration/zlink_builder.hpp>
#include <zlink/framework/contracts/dispatch/request_task.hpp>
#include <zlink/framework/contracts/dispatch/task.hpp>
#include <zlink/framework/contracts/streams/stream.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace zlink::framework::test::actor_gateway
{
int stale_session_unbind_preserves_rebind ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    auto state = std::make_shared<actor_gateway_state_t> ();
    actor_gateway_runtime_t gateway (state);
    const actor_ref_t actor = test_actor_ref ("actor-node", "player", "actor-1", 7);

    gateway.bind_session_sink (actor, [] (std::string, stream_codec_t, const zlink::message_t &) {
        return task_t<void> (result_t<void>::success ());
    });
    gateway.bind_session_stream ("actor-1", stream_t{}, stream_codec_t::message_pack, "session-old",
                                 11);
    gateway.bind_session_stream ("actor-1", stream_t{}, stream_codec_t::message_pack, "session-new",
                                 12);

    gateway.unbind_session_stream ("actor-1", "session-old", 11);
    const auto stale_unbind_preserved = state->sync ([&] {
        const auto actor_record = state->actors_by_id.find ("actor-1");
        return actor_record != state->actors_by_id.end ()
               && actor_record->second.binding_session_id == "session-new"
               && actor_record->second.binding_token == 12
               && actor_record->second.bound_session_stream_sink
               && state->bound_session_sinks.count ("actor-1") == 1;
    });
    if (!stale_unbind_preserved)
        return 1;

    gateway.unbind_session_stream ("actor-1", "session-new", 12);
    const auto current_unbind_cleared = state->sync ([&] {
        const auto actor_record = state->actors_by_id.find ("actor-1");
        return actor_record != state->actors_by_id.end ()
               && actor_record->second.binding_session_id.empty ()
               && actor_record->second.binding_token == 0
               && !actor_record->second.bound_session_stream_sink
               && state->bound_session_sinks.count ("actor-1") == 0;
    });
    if (!current_unbind_cleared)
        return 2;
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

    (void) old_session.bind (actor).async ().result ().value ();
    (void) current_session.bind (actor).async ().result ().value ();
    int disconnected = 0;
    gateway.on_disconnect (
      [&] (const actor_ref_t &, const std::optional<runtime::stateful::stream_binding_t> &) {
          ++disconnected;
          return task_t<void> (result_t<void>::success ());
      });

    if (old_session.find ("actor-replaced"))
        return 1;
    session_actor_manager_access_t::disconnect (old_session);
    if (!gateway.actor_bound ("actor-replaced") || gateway.actor_disconnected ("actor-replaced")
        || disconnected != 0) {
        return 2;
    }

    auto current = current_session.find ("actor-replaced");
    if (!current || !current->notify_disconnected ().result () || disconnected != 1
        || !gateway.actor_disconnected ("actor-replaced")) {
        return 3;
    }
    session_actor_manager_access_t::disconnect (current_session);
    return disconnected == 1 ? 0 : 4;
}

int bound_actors_are_current_ordered_snapshot ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    auto state = std::make_shared<actor_gateway_state_t> ();
    actor_gateway_runtime_t gateway (state);
    auto manager = gateway.manager ();
    if (!manager.bound ().empty ())
        return 1;

    zlink_builder_t builder = zlink::framework::test::runtime_failure_builder ();
    builder.stream ("bound-snapshot").bind ("tcp://127.0.0.1:0");
    auto runtime = stream_runtime_t::from (builder);
    auto stream = runtime.open_session ("bound-snapshot");
    const auto session_id = stream.session_id ();
    runtime.attach_transport_writer (stream,
                                     [] (const stream_header_t &, const zlink::message_t &) {
                                         return task_t<void> (result_t<void>::success ());
                                     });
    session_actor_manager_access_t::attach (manager, std::move (stream));
    session_actor_manager_access_t::bind_native (manager, [] (actor_ref_t, std::uint64_t) {
        return task_t<void> (result_t<void>::success ());
    });

    const auto first = test_actor_ref ("actor-node", "player", "z-bound-first", 1);
    const auto second = test_actor_ref ("actor-node", "player", "a-bound-second", 1);
    if (!manager.bind (first).async ().result () || manager.bound ().size () != 1
        || manager.bound ().front ().actor_id () != "z-bound-first")
        return 2;
    if (!manager.bind (second).async ().result ())
        return 3;
    const auto snapshot = manager.bound ();
    if (snapshot.size () != 2 || snapshot[0].actor_id () != "z-bound-first"
        || snapshot[1].actor_id () != "a-bound-second")
        return 4;

    const auto first_token =
      state->sync ([&] { return state->actors_by_id.at ("z-bound-first").binding_token; });
    gateway.unbind_session_stream ("z-bound-first", session_id, first_token);
    const auto after_unbind = manager.bound ();
    if (after_unbind.size () != 1 || after_unbind[0].actor_id () != "a-bound-second"
        || manager.find ("z-bound-first"))
        return 5;
    if (snapshot.size () != 2 || snapshot[0].actor_id () != "z-bound-first")
        return 6;
    return 0;
}

int bind_or_get_reuses_same_physical_session_generation ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    auto state = std::make_shared<actor_gateway_state_t> ();
    actor_gateway_runtime_t gateway (state);
    auto manager = gateway.manager ();
    zlink_builder_t old_builder = zlink::framework::test::runtime_failure_builder ();
    old_builder.stream ("bind-or-get-idempotent").bind ("tcp://127.0.0.1:0");
    auto old_runtime = stream_runtime_t::from (old_builder);
    auto old_stream = old_runtime.open_session ("bind-or-get-idempotent");
    auto old_stream_copy = old_stream;
    std::atomic_int old_stream_writes{0};
    std::atomic_int old_bound_controls{0};
    old_runtime.attach_transport_writer (
      old_stream_copy, [&old_stream_writes, &old_bound_controls] (const stream_header_t &header,
                                                                  const zlink::message_t &) {
          if (header.packet_name () == "$zlink.actor.bound")
              ++old_bound_controls;
          else
              ++old_stream_writes;
          return task_t<void> (result_t<void>::success ());
      });
    session_actor_manager_access_t::attach (manager, std::move (old_stream));
    std::atomic_int native_binds{0};
    session_actor_manager_access_t::bind_native (
      manager, [&native_binds] (actor_ref_t, std::uint64_t) {
          ++native_binds;
          return task_t<void> (result_t<void>::success ());
      });
    const auto actor = test_actor_ref ("actor-node", "player", "bind-or-get-idempotent-actor", 1);
    const auto first = manager.bind_or_get (actor).async ().result ();
    const auto second = manager.bind_or_get (actor).async ().result ();
    if (!first || !second || native_binds.load () != 1)
        return 1;

    const auto reused_token = state->sync (
      [&] { return state->actors_by_id.at ("bind-or-get-idempotent-actor").binding_token; });
    zlink_builder_t new_builder = zlink::framework::test::runtime_failure_builder ();
    new_builder.stream ("bind-or-get-idempotent").bind ("tcp://127.0.0.1:0");
    auto new_runtime = stream_runtime_t::from (new_builder);
    auto new_stream = new_runtime.open_session ("bind-or-get-idempotent");
    if (new_stream.session_id () != old_stream_copy.session_id ())
        return 2;
    auto new_stream_copy = new_stream;
    std::atomic_int new_stream_writes{0};
    std::atomic_int new_bound_controls{0};
    new_runtime.attach_transport_writer (
      new_stream_copy, [&new_stream_writes, &new_bound_controls] (const stream_header_t &header,
                                                                  const zlink::message_t &) {
          if (header.packet_name () == "$zlink.actor.bound")
              ++new_bound_controls;
          else
              ++new_stream_writes;
          return task_t<void> (result_t<void>::success ());
      });
    session_actor_manager_access_t::attach (manager, std::move (new_stream));
    const auto reconnect = manager.bind_or_get (actor).async ().result ();
    if (!reconnect || native_binds.load () != 2)
        return 3;
    if (state->sync ([&] {
            return state->actors_by_id.at ("bind-or-get-idempotent-actor").binding_token
                   <= reused_token;
        })) {
        return 4;
    }
    const auto delivered = gateway.dispatch_bound_session_send (
      actor, "reconnected-push", stream_codec_t::message_pack, zlink::message_t::from ("payload"));
    if (!delivered || old_stream_writes.load () != 0 || new_stream_writes.load () != 1
        || old_bound_controls.load () != 1 || new_bound_controls.load () != 1)
        return 5;
    return 0;
}

int bind_or_get_completion_can_reenter_same_manager ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    auto state = std::make_shared<actor_gateway_state_t> ();
    actor_gateway_runtime_t gateway (state);
    auto manager = gateway.manager ();
    zlink_builder_t builder = zlink::framework::test::runtime_failure_builder ();
    builder.stream ("bind-or-get-reentrant").bind ("tcp://127.0.0.1:0");
    auto runtime = stream_runtime_t::from (builder);
    session_actor_manager_access_t::attach (manager,
                                            runtime.open_session ("bind-or-get-reentrant"));

    auto native_completion = std::make_shared<task_completion_source_t<void>> ();
    std::atomic_int native_binds{0};
    session_actor_manager_access_t::bind_native (
      manager, [native_completion, &native_binds] (actor_ref_t, std::uint64_t) {
          ++native_binds;
          return zlink::framework::test::accepted_request_task (native_completion->task ());
      });
    const auto actor = test_actor_ref ("actor-node", "player", "bind-or-get-reentrant-actor", 1);
    auto continuation_ran = std::make_shared<std::atomic_bool> (false);
    auto reentrant = [] (session_actor_manager_t manager, actor_ref_t actor,
                         std::shared_ptr<std::atomic_bool> continuation_ran) -> task_t<void> {
        (void) co_await manager.bind_or_get (actor).async ();
        if (!manager.find ("bind-or-get-reentrant-actor")) {
            throw std::runtime_error ("bind_or_get continuation could not find its Actor");
        }
        (void) co_await manager.bind_or_get (actor).async ();
        continuation_ran->store (true, std::memory_order_release);
        co_return;
    }(manager, actor, continuation_ran);

    if (native_binds.load () != 1)
        return 1;
    native_completion->complete (result_t<void>::success ());
    const auto &terminal = reentrant.result ();
    if (!terminal || !continuation_ran->load (std::memory_order_acquire)) {
        return 2;
    }
    return native_binds.load () == 1 ? 0 : 3;
}

int bind_or_get_all_exit_paths_complete_within_deadline ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    const auto no_stream_actor = test_actor_ref ("actor-node", "player", "finite-no-stream", 1);
    actor_gateway_runtime_t no_stream_gateway;
    auto no_stream_manager = no_stream_gateway.manager ();
    const auto no_stream =
      finite_task_result (no_stream_manager.bind_or_get (no_stream_actor).async ());
    if (!no_stream || !*no_stream) {
        return 1;
    }

    actor_gateway_runtime_t reuse_gateway;
    auto reuse_manager = reuse_gateway.manager ();
    zlink_builder_t reuse_builder = zlink::framework::test::runtime_failure_builder ();
    reuse_builder.stream ("finite-reuse").bind ("tcp://127.0.0.1:0");
    auto reuse_runtime = stream_runtime_t::from (reuse_builder);
    session_actor_manager_access_t::attach (reuse_manager,
                                            reuse_runtime.open_session ("finite-reuse"));
    std::atomic_int reuse_native_binds{0};
    session_actor_manager_access_t::bind_native (
      reuse_manager, [&reuse_native_binds] (actor_ref_t, std::uint64_t) {
          ++reuse_native_binds;
          return task_t<void> (result_t<void>::success ());
      });
    const auto reuse_actor = test_actor_ref ("actor-node", "player", "finite-reuse", 1);
    const auto initial = finite_task_result (reuse_manager.bind_or_get (reuse_actor).async ());
    const auto reused = finite_task_result (reuse_manager.bind_or_get (reuse_actor).async ());
    if (!initial || !*initial || !reused || !*reused || reuse_native_binds.load () != 1) {
        return 2;
    }

    actor_gateway_runtime_t success_gateway;
    auto success_manager = success_gateway.manager ();
    zlink_builder_t success_builder = zlink::framework::test::runtime_failure_builder ();
    success_builder.stream ("finite-native-success").bind ("tcp://127.0.0.1:0");
    auto success_runtime = stream_runtime_t::from (success_builder);
    session_actor_manager_access_t::attach (success_manager,
                                            success_runtime.open_session ("finite-native-success"));
    auto success_source = std::make_shared<task_completion_source_t<void>> ();
    session_actor_manager_access_t::bind_native (
      success_manager,
      [success_source] (actor_ref_t, std::uint64_t) { return success_source->task (); });
    const auto success_actor = test_actor_ref ("actor-node", "player", "finite-native-success", 1);
    auto success_task = success_manager.bind_or_get (success_actor).async ();
    success_source->complete (result_t<void>::success ());
    const auto success = finite_task_result (std::move (success_task));
    if (!success || !*success || !success_manager.find ("finite-native-success")) {
        return 3;
    }

    actor_gateway_runtime_t failure_gateway;
    auto failure_manager = failure_gateway.manager ();
    zlink_builder_t failure_builder = zlink::framework::test::runtime_failure_builder ();
    failure_builder.stream ("finite-native-failure").bind ("tcp://127.0.0.1:0");
    auto failure_runtime = stream_runtime_t::from (failure_builder);
    session_actor_manager_access_t::attach (failure_manager,
                                            failure_runtime.open_session ("finite-native-failure"));
    auto failure_source = std::make_shared<task_completion_source_t<void>> ();
    session_actor_manager_access_t::bind_native (
      failure_manager,
      [failure_source] (actor_ref_t, std::uint64_t) { return failure_source->task (); });
    const auto failure_actor = test_actor_ref ("actor-node", "player", "finite-native-failure", 1);
    auto failure_task = failure_manager.bind_or_get (failure_actor).async ();
    failure_source->complete (
      result_t<void>::failure (framework_error_kind_t::unavailable, "native bind rejected"));
    const auto failure = finite_task_result (std::move (failure_task));
    if (!failure || *failure || failure->error_kind () != framework_error_kind_t::unavailable
        || failure_manager.find ("finite-native-failure")) {
        return 4;
    }

    actor_gateway_runtime_t exception_gateway;
    auto exception_manager = exception_gateway.manager ();
    zlink_builder_t exception_builder = zlink::framework::test::runtime_failure_builder ();
    exception_builder.stream ("finite-native-exception").bind ("tcp://127.0.0.1:0");
    auto exception_runtime = stream_runtime_t::from (exception_builder);
    session_actor_manager_access_t::attach (
      exception_manager, exception_runtime.open_session ("finite-native-exception"));
    session_actor_manager_access_t::bind_native (
      exception_manager, [] (actor_ref_t, std::uint64_t) -> task_t<void> {
          throw framework_exception_t (framework_error_kind_t::internal_failure,
                                       "native binder threw");
      });
    const auto exception_actor =
      test_actor_ref ("actor-node", "player", "finite-native-exception", 1);
    const auto exception =
      finite_task_result (exception_manager.bind_or_get (exception_actor).async ());
    if (!exception || *exception
        || exception->error_kind () != framework_error_kind_t::internal_failure
        || exception_manager.find ("finite-native-exception")) {
        return 5;
    }
    return 0;
}

int direct_rebind_publication_is_atomic_and_old_disconnect_is_fenced ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    auto state = std::make_shared<actor_gateway_state_t> ();
    actor_gateway_runtime_t gateway (state);
    auto old_session = gateway.manager ();
    auto new_session = gateway.manager ();
    zlink_builder_t builder = zlink::framework::test::runtime_failure_builder ();
    builder.stream ("direct-rebind").bind ("tcp://127.0.0.1:0");
    auto runtime = stream_runtime_t::from (builder);
    auto old_stream = runtime.open_session ("direct-rebind");
    auto new_stream = runtime.open_session ("direct-rebind");
    const auto old_session_id = old_stream.session_id ();
    const auto new_session_id = new_stream.session_id ();
    if (old_session_id == new_session_id)
        return 1;
    session_actor_manager_access_t::attach (old_session, std::move (old_stream));
    session_actor_manager_access_t::attach (new_session, std::move (new_stream));

    const auto actor = test_actor_ref ("actor-node", "player", "direct-rebind-actor", 1);
    auto old_binding = old_session.bind (actor).async ().result ();
    if (!old_binding)
        return 2;
    auto stale_handle = std::move (old_binding.value ());
    const auto old_token =
      state->sync ([&] { return state->actors_by_id.at ("direct-rebind-actor").binding_token; });
    const auto route_owner = zlink::routing_id_t::from ("session-owner");
    const auto route_session = zlink::routing_id_t::from ("same-session-rid");
    if (!gateway.record_bound_session_route (actor, route_owner, route_session, 11, 13, 17,
                                             old_token, 0, 0)) {
        return 3;
    }

    std::promise<void> binder_entered_source;
    auto binder_entered = binder_entered_source.get_future ();
    std::promise<void> release_binder_source;
    auto release_binder = release_binder_source.get_future ();
    std::atomic_uint64_t issued_generation{0};
    session_actor_manager_access_t::bind_native (
      new_session, [&] (const actor_ref_t &, std::uint64_t binding_generation) {
          issued_generation.store (binding_generation, std::memory_order_release);
          binder_entered_source.set_value ();
          release_binder.wait ();
          auto recorded = gateway.record_bound_session_route (actor, route_owner, route_session, 11,
                                                              13, 17, binding_generation, 0, 0);
          if (!recorded) {
              return task_t<void> (result_t<void>::failure (
                recorded.error_kind (),
                recorded.error () ? recorded.error ()->what () : "route publication failed"));
          }
          return task_t<void> (result_t<void>::success ());
      });
    std::optional<result_t<session_actor_t>> rebound;
    std::thread rebind_thread ([&] { rebound = new_session.bind (actor).async ().result (); });
    binder_entered.wait ();

    int disconnected = 0;
    gateway.on_disconnect (
      [&] (const actor_ref_t &, const std::optional<runtime::stateful::stream_binding_t> &) {
          ++disconnected;
          return task_t<void> (result_t<void>::success ());
      });
    // Session binding §5-2 and §6: the pending bind has not received its
    // terminal reply, so the old binding remains current until publication.
    const auto old_is_current = state->sync ([&] {
        const auto &record = state->actors_by_id.at ("direct-rebind-actor");
        return record.bound && record.binding_session_id == old_session_id
               && record.binding_token == old_token;
    });
    if (!old_is_current || !old_session.find ("direct-rebind-actor")
        || new_session.find ("direct-rebind-actor")) {
        release_binder_source.set_value ();
        rebind_thread.join ();
        return 8;
    }

    release_binder_source.set_value ();
    rebind_thread.join ();
    const auto stale_disconnect = stale_handle.notify_disconnected ().result ();
    if (stale_disconnect || stale_disconnect.error_kind () != framework_error_kind_t::not_configured
        || disconnected != 0 || old_session.find ("direct-rebind-actor")) {
        return 3;
    }
    const auto rebind_is_published = state->sync ([&] {
        const auto &record = state->actors_by_id.at ("direct-rebind-actor");
        return record.bound && !record.disconnected && record.binding_session_id == new_session_id
               && record.binding_token != 0 && record.binding_token != old_token
               && issued_generation.load (std::memory_order_acquire) == record.binding_token
               && issued_generation.load (std::memory_order_acquire) > old_token
               && record.bound_session_stream_sink;
    });
    if (!rebind_is_published) {
        return 4;
    }

    if (!rebound || !*rebound || !new_session.find ("direct-rebind-actor")
        || !gateway.actor_bound ("direct-rebind-actor")
        || gateway.actor_disconnected ("direct-rebind-actor") || disconnected != 0) {
        return 5;
    }
    const auto committed_token = state->sync ([&] {
        const auto token = state->actors_by_id.at ("direct-rebind-actor").binding_token;
        const auto &route = state->actors_by_id.at ("direct-rebind-actor").bound_session_route;
        return std::pair{token, route && route->binding_token == token};
    });
    if (!committed_token.second)
        return 6;
    std::atomic_bool repeated_binder_called{false};
    session_actor_manager_access_t::bind_native (
      new_session, [&] (const actor_ref_t &, std::uint64_t) {
          repeated_binder_called.store (true, std::memory_order_release);
          return task_t<void> (result_t<void>::failure (framework_error_kind_t::unavailable,
                                                        "deterministic native binding rejection"));
      });
    const auto repeated = new_session.bind (actor).async ().result ();
    if (!repeated || repeated_binder_called.load (std::memory_order_acquire)
        || !new_session.find ("direct-rebind-actor")) {
        return 7;
    }
    const auto rebind_remained_published = state->sync ([&] {
        const auto &record = state->actors_by_id.at ("direct-rebind-actor");
        return record.bound && !record.disconnected && record.binding_session_id == new_session_id
               && record.binding_token == committed_token.first && record.bound_session_stream_sink;
    });
    if (!rebind_remained_published)
        return 8;
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
    zlink_builder_t builder = zlink::framework::test::runtime_failure_builder ();
    builder.stream ("destroy-recreate").bind ("tcp://127.0.0.1:0");
    auto runtime = stream_runtime_t::from (builder);
    session_actor_manager_access_t::attach (old_session, runtime.open_session ("destroy-recreate"));
    session_actor_manager_access_t::attach (new_session, runtime.open_session ("destroy-recreate"));

    const auto original = test_actor_ref ("actor-node", "player", "recreated-actor", 1);
    auto original_result = old_session.bind (original).async ().result ();
    if (!original_result)
        return 1;
    auto stale_handle = std::move (original_result.value ());
    int disconnected = 0;
    gateway.on_disconnect (
      [&] (const actor_ref_t &, const std::optional<runtime::stateful::stream_binding_t> &) {
          ++disconnected;
          return task_t<void> (result_t<void>::success ());
      });
    if (!gateway.destroy_actor (original) || !stale_handle.notify_disconnected ().result ()
        || disconnected != 0) {
        return 2;
    }

    const auto recreated = test_actor_ref ("actor-node", "player", "recreated-actor", 2);
    auto recreated_result = new_session.bind (recreated).async ().result ();
    if (!recreated_result)
        return 3;
    const auto stale_after_recreate = stale_handle.notify_disconnected ().result ();
    if (stale_after_recreate
        || (stale_after_recreate.error_kind () != framework_error_kind_t::not_configured
            && stale_after_recreate.error_kind () != framework_error_kind_t::invalid_operation)
        || disconnected != 0 || !gateway.actor_bound ("recreated-actor")
        || gateway.actor_disconnected ("recreated-actor")
        || !new_session.find ("recreated-actor")) {
        return 4;
    }
    return 0;
}

int get_or_create_does_not_reuse_disconnected_record ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    auto state = std::make_shared<actor_gateway_state_t> ();
    const actor_ref_t stale = test_actor_ref ("actor-node-old", "player", "actor-reconnect", 7);
    const actor_ref_t current = test_actor_ref ("actor-node-new", "player", "actor-reconnect", 8);
    state->sync ([&] {
        state->actors_by_id.emplace (
          "actor-reconnect",
          actor_record_t{stale, false, std::make_shared<task_completion_source_t<void>> ()});
        state->bound_session_sinks.emplace (
          "actor-reconnect", std::make_shared<bound_session_sink_t> (
                               [] (std::string, stream_codec_t, const zlink::message_t &) {
                                   return task_t<void> (result_t<void>::success ());
                               }));
        state->create_dispatcher = [current] (std::string, std::string,
                                              const std::optional<zlink::message_t> &) {
            return result_t<actor_ref_t>::success (current);
        };
    });

    actor_gateway_runtime_t gateway (state);
    auto manager = gateway.manager ();
    const auto created = manager.get_or_create ("player", "actor-reconnect");
    if (!created || created.value ().ref ().node_rid ().value () != "actor-node-new"
        || created.value ().ref ().object_generation () != 8) {
        return 1;
    }
    const auto disconnected_record_replaced = state->sync ([&] {
        const auto found = state->actors_by_id.find ("actor-reconnect");
        return found != state->actors_by_id.end ()
               && found->second.ref.node_rid ().value () == "actor-node-new"
               && !found->second.disconnected
               && !state->bound_session_sinks.contains ("actor-reconnect");
    });
    if (!disconnected_record_replaced)
        return 2;
    return 0;
}

int get_or_create_refreshes_foreign_session_record ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    auto state = std::make_shared<actor_gateway_state_t> ();
    const actor_ref_t stale = test_actor_ref ("actor-node-old", "player", "actor-foreign", 7);
    const actor_ref_t current = test_actor_ref ("actor-node-current", "player", "actor-foreign", 7);
    state->sync ([&] {
        state->actors_by_id.emplace ("actor-foreign", actor_record_t{stale, true, {}});
        state->create_dispatcher = [current] (std::string, std::string,
                                              const std::optional<zlink::message_t> &) {
            return result_t<actor_ref_t>::success (current);
        };
    });

    actor_gateway_runtime_t gateway (state);
    gateway.bind_session_stream ("actor-foreign", stream_t{}, stream_codec_t::message_pack,
                                 "session-old", 11);
    auto manager = gateway.manager ();

    zlink::framework::zlink_builder_t builder = zlink::framework::test::runtime_failure_builder ();
    builder.stream ("foreign-session-test").bind ("tcp://127.0.0.1:0");
    auto runtime = stream_runtime_t::from (builder);
    auto stream = runtime.open_session ("foreign-session-test");
    session_actor_manager_access_t::attach (manager, std::move (stream));

    const auto refreshed = manager.get_or_create ("player", "actor-foreign");
    if (!refreshed || refreshed.value ().ref ().node_rid ().value () != "actor-node-current") {
        return 1;
    }
    const auto foreign_session_record_replaced = state->sync ([&] {
        const auto found = state->actors_by_id.find ("actor-foreign");
        return found != state->actors_by_id.end ()
               && found->second.ref.node_rid ().value () == "actor-node-current"
               && found->second.binding_session_id != "session-old"
               && !state->bound_session_sinks.contains ("actor-foreign");
    });
    if (!foreign_session_record_replaced)
        return 2;
    return 0;
}

int session_disconnect_is_all_settled_and_token_fenced ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    auto state = std::make_shared<actor_gateway_state_t> ();
    actor_gateway_runtime_t gateway (state);
    auto stale_manager = gateway.manager ();
    session_actor_manager_access_t::attach (stale_manager, stream_t{});
    const actor_ref_t first = test_actor_ref ("actor-node", "player", "actor-a", 1);
    const actor_ref_t second = test_actor_ref ("actor-node", "player", "actor-b", 1);

    auto stale = stale_manager.bind (first).async ().result ().value ();
    auto manager = gateway.manager ();
    session_actor_manager_access_t::attach (manager, stream_t{});
    auto current = manager.bind (first).async ().result ().value ();
    (void) manager.bind (second).async ().result ().value ();
    if (stale.notify_disconnected ().result ().error_kind ()
        != framework_error_kind_t::not_configured) {
        return 1;
    }

    std::vector<std::string> disconnected;
    gateway.on_disconnect (
      [&] (const actor_ref_t &actor, const std::optional<runtime::stateful::stream_binding_t> &) {
          disconnected.emplace_back (actor.actor_id ().value ());
          return task_t<void> (actor.actor_id ().value () == "actor-a"
                                 ? result_t<void>::failure (framework_error_kind_t::not_found,
                                                            "actor-a callback failed")
                                 : result_t<void>::success ());
      });
    session_actor_manager_access_t::disconnect (manager);
    std::sort (disconnected.begin (), disconnected.end ());
    if (disconnected != std::vector<std::string>{"actor-a", "actor-b"}
        || !gateway.actor_disconnected ("actor-a") || !gateway.actor_disconnected ("actor-b")) {
        return 2;
    }
    if (state->sync ([&] {
            return !state->actors_by_id.contains ("actor-a")
                   || !state->actors_by_id.contains ("actor-b");
        })) {
        return 3;
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
    auto first_binding = manager.bind (first).async ().result ().value ();
    auto second_binding = manager.bind (second).async ().result ().value ();
    std::vector<std::string> disconnected;
    gateway.on_disconnect (
      [&] (const actor_ref_t &actor, const std::optional<runtime::stateful::stream_binding_t> &) {
          disconnected.emplace_back (actor.actor_id ().value ());
          return task_t<void> (result_t<void>::success ());
      });

    if (!first_binding.notify_disconnected ().result ()) {
        return 1;
    }
    if (disconnected != std::vector<std::string>{"actor-a"})
        return 4;
    const auto second_binding_remains_live = state->sync ([&] {
        const auto second_record = state->actors_by_id.find ("actor-b");
        return second_record != state->actors_by_id.end ()
               && second_record->second.binding_token != 0;
    });
    if (!second_binding_remains_live)
        return 2;
    if (!second_binding.notify_disconnected ().result ()
        || disconnected != std::vector<std::string>{"actor-a", "actor-b"}) {
        return 3;
    }
    return 0;
}

int route_update_preserves_object_generation ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    serializer_registry_t relay_serializers;
    actor_gateway_runtime_t gateway;
    gateway.bind_serializers (relay_serializers);
    auto manager = gateway.manager ();
    session_actor_manager_access_t::attach (manager, stream_t{});
    const actor_ref_t original = test_actor_ref ("actor-node-a", "player", "actor-route", 7);
    auto original_binding = manager.bind (original).async ().result ().value ();
    const actor_ref_t unaffected = test_actor_ref ("actor-node-a", "player", "actor-other", 3);
    auto unaffected_binding = manager.bind (unaffected).async ().result ().value ();

    std::vector<actor_ref_t> relay_routes;
    gateway.on_relay ([&] (const actor_ref_t &actor, const actor_context_t &,
                           const stream_header_t &, const zlink::message_t &,
                           std::optional<bound_session_relay_source_t>, std::chrono::milliseconds) {
        relay_routes.push_back (actor);
        return request_task_t<std::optional<zlink::message_t>> (
          result_t<std::optional<zlink::message_t>>::success (std::nullopt));
    });

    const actor_ref_t relocated = test_actor_ref ("actor-node-b", "player", "actor-route", 7);
    if (!gateway.update_actor_ref (relocated))
        return 1;
    if (!original_binding.relay ("packet", zlink::framework::message_t{}).result ()
        || relay_routes.size () != 1
        || relay_routes.front ().node_rid ().value () != original.node_rid ().value ()) {
        return 5;
    }
    if (!original_binding.notify_disconnected ().result ()
        || !gateway.update_actor_ref (relocated)) {
        return 7;
    }
    auto relocated_binding = manager.bind (relocated).async ().result ().value ();
    if (!relocated_binding.relay ("packet", zlink::framework::message_t{}).result ()
        || relay_routes.size () != 2
        || relay_routes.back ().node_rid ().value () != relocated.node_rid ().value ()) {
        return 8;
    }
    if (!unaffected_binding.relay ("packet", zlink::framework::message_t{}).result ()
        || relay_routes.size () != 3
        || relay_routes.back ().node_rid ().value () != unaffected.node_rid ().value ()) {
        return 6;
    }
    const actor_ref_t new_incarnation = test_actor_ref ("actor-node-c", "player", "actor-route", 8);
    const auto rejected = gateway.update_actor_ref (new_incarnation);
    if (rejected || rejected.error_kind () != framework_error_kind_t::invalid_operation)
        return 2;
    const auto rejected_bind = manager.bind (new_incarnation).async ().result ();
    if (rejected_bind || rejected_bind.error_kind () != framework_error_kind_t::invalid_operation)
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
    if (!manager.bind (current).async ().result ()) {
        return 1;
    }
    const auto bind_result = manager.bind (stale).async ().result ();
    if (bind_result || bind_result.error_kind () != framework_error_kind_t::invalid_operation) {
        return 2;
    }
    const auto update_result = gateway.update_actor_ref (stale);
    if (update_result || update_result.error_kind () != framework_error_kind_t::invalid_operation) {
        return 3;
    }
    return 0;
}

int bind_registration_contract_vectors ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    std::ifstream input (std::filesystem::path (__FILE__).parent_path ()
                         / "../../../../runtime/protocol/fixtures/bind-registration-v1.json");
    nlohmann::json fixture;
    input >> fixture;
    const auto number = [] (const nlohmann::json &value, const char *key) {
        return std::stoull (value.at (key).get<std::string> ());
    };
    const auto authority = number (fixture, "authority");
    int failures = 0;
    for (const auto &vector : fixture.at ("cases")) {
        for (const bool record : {false, true}) {
            actor_gateway_runtime_t gateway;
            const auto actor =
              test_actor_ref ("actor-owner", "game.actor", "registration-actor", 1);
            const auto sink = [] (std::string, stream_codec_t, const zlink::message_t &) {
                return task_t<void> (result_t<void>::success ());
            };
            const auto route = [&] (const nlohmann::json &value) {
                return actor_bound_session_route_t{
                  zlink::routing_id_t::from (value.at ("node").get<std::string> ()),
                  zlink::routing_id_t::from (value.at ("session").get<std::string> ()),
                  1,
                  number (value, "lifecycle"),
                  authority,
                  1,
                  number (value, "generation"),
                  0,
                  0,
                  false,
                  value.at ("ownerId").get<std::string> (),
                  number (value, "lease")};
            };
            const auto original = route (fixture.at ("current"));
            if (!gateway.replace_session_route (actor, sink, original))
                return 2;
            const auto requested = route (vector);
            int terminals = 0;
            const auto result =
              record ? gateway.record_bound_session_route_transition (actor, requested,
                                                                      [&] { ++terminals; })
                     : gateway.replace_session_route (
                         actor, sink, requested, stream_codec_t::message_pack,
                         [&] (const actor_bound_session_route_t &accepted) {
                             ++terminals;
                             return accepted.binding_generation == requested.binding_generation
                                    && accepted.authority_owner_generation
                                         == requested.authority_owner_generation;
                         });
            const auto current = gateway.bound_session_route_async (actor).result ().value ();
            const bool success = vector.at ("success").get<bool> ();
            const bool same_identity =
              original.node_rid.to_hex () == requested.node_rid.to_hex ()
              && original.node_generation == requested.node_generation
              && original.session_rid == requested.session_rid
              && original.binding_generation == requested.binding_generation;
            const auto &expected = success && !same_identity ? requested : original;
            const bool current_matches =
              current && current->node_rid.to_hex () == expected.node_rid.to_hex ()
              && current->node_generation == expected.node_generation
              && current->session_rid == expected.session_rid
              && current->binding_generation == expected.binding_generation
              && current->authority_owner_generation == expected.authority_owner_generation
              && current->session_owner_id == expected.session_owner_id
              && current->session_owner_lease_generation == expected.session_owner_lease_generation;
            const bool terminal_matches =
              success
                ? result && terminals == 1 && result.value ().current
                    && result.value ().current->binding_generation == requested.binding_generation
                    && result.value ().current->authority_owner_generation == authority
                : !result && terminals == 0
                    && result.error_kind () == framework_error_kind_t::unavailable;
            if (!current_matches || !terminal_matches) {
                std::cerr << "bind registration contract failed: " << vector.at ("name")
                          << (record ? " (record)" : " (replace)") << '\n';
                ++failures;
            }
        }
    }
    return failures == 0 ? 0 : 1;
}
} // namespace zlink::framework::test::actor_gateway
