/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/protocol/service_wire_codec.hpp"
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

template <typename T>
std::optional<zlink::framework::result_t<T>> finite_task_result (
  zlink::framework::task_t<T> task,
  std::chrono::milliseconds timeout = std::chrono::milliseconds (500))
{
    struct wait_state_t
    {
        std::mutex mutex;
        std::condition_variable ready;
        std::optional<zlink::framework::result_t<T>> result;
    };

    auto wait_state = std::make_shared<wait_state_t> ();
    auto observed = std::make_shared<zlink::framework::task_t<T>> (
      std::move (task));
    zlink::framework::detail::observe_task_completion (
      *observed,
      [wait_state, observed] (const zlink::framework::result_t<T> &result) {
          {
              const std::lock_guard lock (wait_state->mutex);
              wait_state->result = result;
          }
          wait_state->ready.notify_one ();
      });
    std::unique_lock lock (wait_state->mutex);
    if (!wait_state->ready.wait_for (
          lock, timeout, [&] { return wait_state->result.has_value (); })) {
        return std::nullopt;
    }
    return wait_state->result;
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

int bind_or_get_reuses_same_physical_session_generation ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    auto state = std::make_shared<actor_gateway_state_t> ();
    actor_gateway_runtime_t gateway (state);
    auto manager = gateway.manager ();
    zlink_builder_t old_builder;
    old_builder.stream ("bind-or-get-idempotent").bind ("tcp://127.0.0.1:0");
    auto old_runtime = stream_runtime_t::from (old_builder);
    auto old_stream = old_runtime.open_session ("bind-or-get-idempotent");
    auto old_stream_copy = old_stream;
    std::atomic_int old_stream_writes{0};
    old_runtime.attach_transport_writer (
      old_stream_copy,
      [&old_stream_writes] (const stream_header_t &,
                            const zlink::message_t &,
                            std::optional<std::chrono::milliseconds>) {
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
    const auto actor = test_actor_ref (
      "actor-node", "player", "bind-or-get-idempotent-actor", 1);
    const auto first = manager.bind_or_get (actor).submit ().result ();
    const auto second = manager.bind_or_get (actor).submit ().result ();
    if (!first || !second || native_binds.load () != 1)
        return 1;

    std::uint64_t reused_token = 0;
    {
        const std::lock_guard lock (state->mutex);
        reused_token = state->actors_by_id.at (
          "bind-or-get-idempotent-actor").binding_token;
    }
    zlink_builder_t new_builder;
    new_builder.stream ("bind-or-get-idempotent").bind ("tcp://127.0.0.1:0");
    auto new_runtime = stream_runtime_t::from (new_builder);
    auto new_stream = new_runtime.open_session ("bind-or-get-idempotent");
    if (new_stream.session_id () != old_stream_copy.session_id ())
        return 2;
    auto new_stream_copy = new_stream;
    std::atomic_int new_stream_writes{0};
    new_runtime.attach_transport_writer (
      new_stream_copy,
      [&new_stream_writes] (const stream_header_t &,
                            const zlink::message_t &,
                            std::optional<std::chrono::milliseconds>) {
          ++new_stream_writes;
          return task_t<void> (result_t<void>::success ());
      });
    session_actor_manager_access_t::attach (manager, std::move (new_stream));
    const auto reconnect = manager.bind_or_get (actor).submit ().result ();
    if (!reconnect || native_binds.load () != 2)
        return 3;
    {
        const std::lock_guard lock (state->mutex);
        if (state->actors_by_id.at (
              "bind-or-get-idempotent-actor").binding_token
            <= reused_token) {
            return 4;
        }
    }
    const auto delivered = gateway.dispatch_bound_session_send (
      actor, "reconnected-push", stream_codec_t::message_pack,
      zlink::message_t::from ("payload"));
    if (!delivered || old_stream_writes.load () != 0
        || new_stream_writes.load () != 1)
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
    zlink_builder_t builder;
    builder.stream ("bind-or-get-reentrant").bind ("tcp://127.0.0.1:0");
    auto runtime = stream_runtime_t::from (builder);
    session_actor_manager_access_t::attach (
      manager, runtime.open_session ("bind-or-get-reentrant"));

    auto native_completion =
      std::make_shared<task_completion_source_t<void>> ();
    std::atomic_int native_binds{0};
    session_actor_manager_access_t::bind_native (
      manager, [native_completion, &native_binds] (actor_ref_t, std::uint64_t) {
          ++native_binds;
          return native_completion->task ();
      });
    const auto actor = test_actor_ref (
      "actor-node", "player", "bind-or-get-reentrant-actor", 1);
    auto continuation_ran = std::make_shared<std::atomic_bool> (false);
    auto reentrant = [] (session_actor_manager_t manager, actor_ref_t actor,
                         std::shared_ptr<std::atomic_bool> continuation_ran)
      -> task_t<void> {
        (void) co_await manager.bind_or_get (actor).submit ();
        if (!manager.find ("bind-or-get-reentrant-actor")) {
            throw std::runtime_error (
              "bind_or_get continuation could not find its Actor");
        }
        (void) co_await manager.bind_or_get (actor).submit ();
        continuation_ran->store (true, std::memory_order_release);
        co_return;
    } (manager, actor, continuation_ran);

    if (native_binds.load () != 1)
        return 1;
    native_completion->complete (result_t<void>::success ());
    const auto &terminal = reentrant.result ();
    if (!terminal
        || !continuation_ran->load (std::memory_order_acquire)) {
        return 2;
    }
    return native_binds.load () == 1 ? 0 : 3;
}

int bind_or_get_all_exit_paths_complete_within_deadline ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    const auto no_stream_actor = test_actor_ref (
      "actor-node", "player", "finite-no-stream", 1);
    actor_gateway_runtime_t no_stream_gateway;
    auto no_stream_manager = no_stream_gateway.manager ();
    const auto no_stream = finite_task_result (
      no_stream_manager.bind_or_get (no_stream_actor).submit ());
    if (!no_stream || !*no_stream) {
        return 1;
    }

    actor_gateway_runtime_t reuse_gateway;
    auto reuse_manager = reuse_gateway.manager ();
    zlink_builder_t reuse_builder;
    reuse_builder.stream ("finite-reuse").bind ("tcp://127.0.0.1:0");
    auto reuse_runtime = stream_runtime_t::from (reuse_builder);
    session_actor_manager_access_t::attach (
      reuse_manager, reuse_runtime.open_session ("finite-reuse"));
    std::atomic_int reuse_native_binds{0};
    session_actor_manager_access_t::bind_native (
      reuse_manager, [&reuse_native_binds] (actor_ref_t, std::uint64_t) {
          ++reuse_native_binds;
          return task_t<void> (result_t<void>::success ());
      });
    const auto reuse_actor = test_actor_ref (
      "actor-node", "player", "finite-reuse", 1);
    const auto initial = finite_task_result (
      reuse_manager.bind_or_get (reuse_actor).submit ());
    const auto reused = finite_task_result (
      reuse_manager.bind_or_get (reuse_actor).submit ());
    if (!initial || !*initial || !reused || !*reused
        || reuse_native_binds.load () != 1) {
        return 2;
    }

    actor_gateway_runtime_t success_gateway;
    auto success_manager = success_gateway.manager ();
    zlink_builder_t success_builder;
    success_builder.stream ("finite-native-success")
      .bind ("tcp://127.0.0.1:0");
    auto success_runtime = stream_runtime_t::from (success_builder);
    session_actor_manager_access_t::attach (
      success_manager,
      success_runtime.open_session ("finite-native-success"));
    auto success_source =
      std::make_shared<task_completion_source_t<void>> ();
    session_actor_manager_access_t::bind_native (
      success_manager, [success_source] (actor_ref_t, std::uint64_t) {
          return success_source->task ();
      });
    const auto success_actor = test_actor_ref (
      "actor-node", "player", "finite-native-success", 1);
    auto success_task = success_manager.bind_or_get (success_actor).submit ();
    success_source->complete (result_t<void>::success ());
    const auto success = finite_task_result (std::move (success_task));
    if (!success || !*success
        || !success_manager.find ("finite-native-success")) {
        return 3;
    }

    actor_gateway_runtime_t failure_gateway;
    auto failure_manager = failure_gateway.manager ();
    zlink_builder_t failure_builder;
    failure_builder.stream ("finite-native-failure")
      .bind ("tcp://127.0.0.1:0");
    auto failure_runtime = stream_runtime_t::from (failure_builder);
    session_actor_manager_access_t::attach (
      failure_manager,
      failure_runtime.open_session ("finite-native-failure"));
    auto failure_source =
      std::make_shared<task_completion_source_t<void>> ();
    session_actor_manager_access_t::bind_native (
      failure_manager, [failure_source] (actor_ref_t, std::uint64_t) {
          return failure_source->task ();
      });
    const auto failure_actor = test_actor_ref (
      "actor-node", "player", "finite-native-failure", 1);
    auto failure_task = failure_manager.bind_or_get (failure_actor).submit ();
    failure_source->complete (result_t<void>::failure (
      framework_error_kind_t::unavailable, "native bind rejected"));
    const auto failure = finite_task_result (std::move (failure_task));
    if (!failure || *failure
        || failure->error_kind () != framework_error_kind_t::unavailable
        || failure_manager.find ("finite-native-failure")) {
        return 4;
    }

    actor_gateway_runtime_t exception_gateway;
    auto exception_manager = exception_gateway.manager ();
    zlink_builder_t exception_builder;
    exception_builder.stream ("finite-native-exception")
      .bind ("tcp://127.0.0.1:0");
    auto exception_runtime = stream_runtime_t::from (exception_builder);
    session_actor_manager_access_t::attach (
      exception_manager,
      exception_runtime.open_session ("finite-native-exception"));
    session_actor_manager_access_t::bind_native (
      exception_manager,
      [] (actor_ref_t, std::uint64_t) -> task_t<void> {
          throw framework_exception_t (
            framework_error_kind_t::internal_failure,
            "native binder threw");
      });
    const auto exception_actor = test_actor_ref (
      "actor-node", "player", "finite-native-exception", 1);
    const auto exception = finite_task_result (
      exception_manager.bind_or_get (exception_actor).submit ());
    if (!exception || *exception
        || exception->error_kind ()
             != framework_error_kind_t::internal_failure
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
    const auto route_owner = zlink::routing_id_t::from ("session-owner");
    const auto route_session = zlink::routing_id_t::from ("same-session-rid");
    if (!gateway.record_bound_session_route (
          actor, route_owner, route_session, 11, 13, 17,
          old_token, 0, 0)) {
        return 3;
    }

    std::promise<void> binder_entered_source;
    auto binder_entered = binder_entered_source.get_future ();
    std::promise<void> release_binder_source;
    auto release_binder = release_binder_source.get_future ();
    std::atomic_uint64_t issued_generation{0};
    session_actor_manager_access_t::bind_native (
      new_session,
      [&] (const actor_ref_t &, std::uint64_t binding_generation) {
          issued_generation.store (
            binding_generation, std::memory_order_release);
          binder_entered_source.set_value ();
          release_binder.wait ();
          auto recorded = gateway.record_bound_session_route (
            actor, route_owner, route_session, 11, 13, 17,
            binding_generation, 0, 0);
          if (!recorded) {
              return task_t<void> (result_t<void>::failure (
                recorded.error_kind (),
                recorded.error () ? recorded.error ()->what ()
                                   : "route publication failed"));
          }
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
            || issued_generation.load (std::memory_order_acquire)
                 != record.binding_token
            || issued_generation.load (std::memory_order_acquire)
                 <= old_token
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
        const auto &route = state->actors_by_id.at (
          "direct-rebind-actor").bound_session_route;
        if (!route || route->binding_token != committed_token) {
            return 6;
        }
    }
    session_actor_manager_access_t::bind_native (
      new_session,
      [] (const actor_ref_t &, std::uint64_t) {
          return task_t<void> (result_t<void>::failure (
            framework_error_kind_t::unavailable,
            "deterministic native binding rejection"));
      });
    const auto rejected = new_session.bind (actor).submit ().result ();
    if (rejected
        || rejected.error_kind () != framework_error_kind_t::unavailable
        || !new_session.find ("direct-rebind-actor")) {
        return 7;
    }
    {
        const std::lock_guard lock (state->mutex);
        const auto &record = state->actors_by_id.at ("direct-rebind-actor");
        if (!record.bound || record.disconnected
            || record.binding_session_id != new_session_id
            || record.binding_token != committed_token
            || !record.bound_session_stream_sink) {
            return 8;
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

int actor_request_completion_keeps_dedup_state_owned_after_runtime_wrapper_unwinds ()
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
      spot_handler_kind_t::actor_request, "JoinGameMsg", "",
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
    spot_inbound_message_t request_metadata;
    request_metadata.values.emplace ("__zlink.actorRequestId", "relocating-join-request");
    // The temporary runtime wrapper has already unwound when handler_terminal
    // resumes this coroutine. The terminal path must still complete the node
    // owned exactly-once map, rather than dereferencing the dead wrapper.
    auto relayed = spot_node_runtime_t (node).relay_actor_packet (
      actor, gateway.actor_context (actor), stream_message_kind_t::request,
      "JoinGameMsg", zlink::message_t::from ("join"), provider,
      serializers, std::move (request_metadata), nullptr, {},
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
    const auto dedup = node->dispatched_request_replies.claim (
      "23:player:reconnect-playerrelocating-join-request");
    if (dedup.state != runtime::exactly_once_claim_state::completed || !dedup.value)
        return 4;
    if (disconnected.wait_for (std::chrono::seconds (1))
          != std::future_status::ready) {
        return 5;
    }
    const auto disconnected_result = disconnected.get ();
    return disconnected_result
             && disconnected_started.load (std::memory_order_acquire)
           ? 0
           : 6;
}

int old_owner_forwards_cold_probe_via_active_message_follow_route ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace runtime = zlink::framework::runtime;

    // Regression pin: a client that probes an Actor's former owner directly
    // (no Message Follow fence on the wire, hop_count 0 -- e.g. a stale
    // cached location) must be forwarded to the real current owner instead
    // of failing with "actor spot context is not registered" and, worse,
    // materializing a duplicate local Actor instance on the old owner.
    serializer_registry_t serializers;
    auto node = std::make_shared<spot_node_builder_state_t> ("actor-a");
    node->worker_executor = std::make_shared<runtime::offload_executor_t> (
      1, 16, "actor-a-worker");
    node->channel_runtime = std::make_shared<channel_runtime_state_t> ();
    node->channel_runtime->serializers = &serializers;

    std::atomic_int local_materializations{0};
    spot_node_builder_state_t::actor_factory_registration_t factory;
    factory.actor_type = std::type_index (typeid (int));
    factory.create_instance = [&local_materializations] (std::string) {
        local_materializations.fetch_add (1, std::memory_order_acq_rel);
        return std::make_shared<int> (7);
    };
    factory.configure_instance = [] (void *, const actor_ref_t &, void *) {};
    node->actor_factories.emplace ("player", std::move (factory));

    const auto actor = test_actor_ref ("actor-a", "player", "actor-remote-ok", 21);
    const auto key = std::string ("player:actor-remote-ok");

    const auto source_fence = runtime::protocol::actor_route_fence_t{
      "actor-remote-ok", 21, zlink::routing_id_t::from ("actor-a").to_bytes (), 1, 1, 1};
    const auto target_fence = runtime::protocol::actor_route_fence_t{
      "actor-remote-ok", 21, zlink::routing_id_t::from ("actor-b").to_bytes (), 1, 1, 1};
    const auto target_actor = test_actor_ref ("actor-b", "player", "actor-remote-ok", 21);
    const spot_route_t target_route{
      node_rid_t::from_string ("actor-b"), spot_id_t ("spot-remote-ok-0"), "game"};

    // Mirrors spot_node_runtime_t::complete_remote_actor_transfer: it retains
    // the Message Follow route for the old source fence and (via
    // record_actor_route_unlocked) records the remote target's location,
    // which is not a locally registered Spot on this node.
    node->actor_transfer_coordinator.activate_message_follow (
      key, source_fence, target_actor, target_route, target_fence,
      std::chrono::steady_clock::now () + std::chrono::seconds (30),
      "relocation-1");
    record_actor_route_unlocked (*node, key, target_route,
                                 target_actor.object_generation ());

    std::atomic_int relay_calls{0};
    spot_node_runtime_t spots (node);
    spots.on_actor_message_follow (
      [&] (const actor_ref_t &relayed_actor,
           const runtime::messaging::envelope_header_t &header,
           const zlink::message_t &, std::chrono::milliseconds,
           const zlink::routing_id_t &,
           const runtime::protocol::actor_route_fence_t &route,
           std::uint8_t hop_count,
           const runtime::protocol::wire_operation_id_t &,
           std::uint64_t) -> task_t<std::optional<zlink::message_t>> {
          relay_calls.fetch_add (1, std::memory_order_acq_rel);
          if (relayed_actor.actor_id ().value () != "actor-remote-ok"
              || route != source_fence || hop_count != 0
              || header.message_name != "after-transfer") {
              co_return result_t<std::optional<zlink::message_t>>::failure (
                framework_error_kind_t::internal_failure,
                "unexpected relay arguments");
          }
          co_return result_t<std::optional<zlink::message_t>>::success (
            std::make_optional (zlink::message_t::from (std::string ("relayed"))));
      });

    service_collection_t services;
    auto provider = services.build_provider ();
    actor_gateway_runtime_t gateway;
    spot_inbound_message_t metadata;

    auto relayed = spots.relay_actor_packet (
      actor, gateway.actor_context (actor), stream_message_kind_t::request,
      "after-transfer", zlink::message_t::from (std::string ("probe")), provider,
      serializers, std::move (metadata), nullptr);

    const auto outcome = finite_task_result (std::move (relayed));
    if (!outcome || !*outcome)
        return 1;
    if (relay_calls.load (std::memory_order_acquire) != 1)
        return 2;
    if (local_materializations.load (std::memory_order_acquire) != 0)
        return 3;
    return 0;
}

int source_cleanup_waits_for_leave_completion_before_erasing_actor ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace runtime = zlink::framework::runtime;

    // Regression pin (ST-B1): cleanup_expired_actor_admissions_at must not
    // erase the source Actor instance (spec 15 source membership cleanup)
    // until the OnLeave callback submit_remote_actor_leave queued has
    // actually finished (leave_completed), not merely been queued
    // (leave_submitted). submit_remote_actor_leave only enqueues OnLeave
    // onto the source Spot's serial executor and returns immediately, so on
    // some environments/timings the sweep raced ahead of that queued task
    // and erased the instance first; OnLeave's own dispatch then found it
    // gone and silently no-op'd -- relocation still completed end to end,
    // but the "leave" evidence never fired.
    auto node = std::make_shared<spot_node_builder_state_t> ("actor-a");

    const auto actor = test_actor_ref ("actor-a", "player", "leave-race-actor", 1);
    const auto key = std::string ("player:leave-race-actor");
    node->actor_instances.emplace (key, std::make_shared<int> (7));

    const auto now = std::chrono::steady_clock::now ();
    node->pending_remote_source_cleanups.push_back (
      spot_node_builder_state_t::pending_remote_source_cleanup_t{
        .source_actor = actor,
        .source_fence = runtime::protocol::actor_route_fence_t{},
        .transfer_id = "transfer-leave-race",
        .target_spot_id = spot_id_t ("target-spot"),
        .not_before = now - std::chrono::seconds (1),
        .leave_submitted = true,
        .leave_completed = false,
        .leave_deadline = now + std::chrono::seconds (30)});

    spot_node_runtime_t spots (node);
    // not_before has already passed, but OnLeave is only queued
    // (leave_submitted), not finished (leave_completed), and the last-resort
    // deadline is far off: the sweep must hold the erase.
    const auto swept_while_pending =
      spots.cleanup_expired_actor_admissions_at (now);
    if (swept_while_pending != 0)
        return 1;
    if (!node->actor_instances.contains (key))
        return 2;
    if (node->pending_remote_source_cleanups.size () != 1)
        return 3;

    // OnLeave's queued task finishes and marks completion, mirroring the
    // completion continuation submit_remote_actor_leave installs.
    node->pending_remote_source_cleanups.front ().leave_completed = true;
    const auto swept_after_completion =
      spots.cleanup_expired_actor_admissions_at (now);
    if (swept_after_completion == 0)
        return 4;
    if (node->actor_instances.contains (key))
        return 5;
    if (!node->pending_remote_source_cleanups.empty ())
        return 6;
    return 0;
}

int reconcile_deadline_restores_actor_to_local_service ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace runtime = zlink::framework::runtime;

    // Regression pin (spec 28 explicit-failure / never-unbounded-queueing):
    // a relocation attempt that ends ambiguously (fail_remote_actor_transfer
    // with reconcile=true, e.g. a FINALIZE submission whose outcome on the
    // target is unknown) has no authority-truth reconciliation today, so a
    // request arriving while the coordinator sits in the reconcile phase
    // must not park in the handoff backlog forever. This pins that
    // cleanup_expired_actor_admissions_at closes the move once
    // reconcile_deadline passes and actually replays the parked request
    // instead of leaving it stuck.
    serializer_registry_t serializers;
    auto node = std::make_shared<spot_node_builder_state_t> ("actor-a");
    node->worker_executor = std::make_shared<runtime::offload_executor_t> (
      1, 16, "reconcile-worker");
    node->channel_runtime = std::make_shared<channel_runtime_state_t> ();
    node->channel_runtime->serializers = &serializers;

    auto spot = std::make_shared<spot_context_state_t> ();
    spot->node = node;
    spot->node_rid = node_rid_t::from_string ("actor-a");
    spot->spot_id = spot_id_t ("actor-a-entry");
    spot->spot_name = "entry";
    spot->spot_instance = std::make_shared<int> (1);
    spot->channel_runtime = std::make_shared<channel_runtime_state_t> ();
    spot->channel_runtime->serializers = &serializers;
    spot->serial_executor = node->worker_executor;
    node->spot_contexts_by_id.emplace (
      spot->spot_id, spot_context_access_t::create (spot));

    spot_node_builder_state_t::actor_factory_registration_t factory;
    factory.actor_type = std::type_index (typeid (int));
    factory.create_instance = [] (std::string) {
        return std::make_shared<int> (7);
    };
    factory.configure_instance = [] (void *, const actor_ref_t &, void *) {};
    node->actor_factories.emplace ("player", std::move (factory));

    const auto actor = test_actor_ref ("actor-a", "player", "reconcile-actor", 1);
    const auto key = std::string ("player:reconcile-actor");
    node->actor_instances.emplace (key, std::make_shared<int> (7));
    node->actor_spot_ids.emplace (key, spot->spot_id);
    node->actor_generations.emplace (key, 1);

    std::atomic_bool handler_ran{false};
    spot->handlers.push_back (spot_handler_descriptor_t{
      spot_handler_kind_t::actor_send, "ReconcileProbe", "",
      std::type_index (typeid (int)), std::type_index (typeid (void)),
      std::type_index (typeid (int)), std::type_index (typeid (void))});
    spot->handler_invokers.push_back (
      [&handler_ran] (
        void *, void *, service_provider_t &, serializer_registry_t &,
        const zlink::message_t &, const spot_inbound_message_t &)
        -> task_t<zlink::message_t> {
          handler_ran.store (true, std::memory_order_release);
          co_return zlink::message_t{};
      });

    // Mirrors fail_remote_actor_transfer(actor, true): the ambiguous-outcome
    // path, bounded by a short deadline for this test.
    node->actor_transfer_coordinator.mark_reconcile (
      key, std::chrono::milliseconds (20));

    service_collection_t services;
    auto provider = services.build_provider ();
    actor_gateway_runtime_t gateway;
    spot_inbound_message_t metadata;

    spot_node_runtime_t spots (node);
    auto sent = spots.relay_actor_packet (
      actor, gateway.actor_context (actor), stream_message_kind_t::send,
      "ReconcileProbe", zlink::message_t::from (std::string ("probe")),
      provider, serializers, std::move (metadata), nullptr);

    const auto sent_result = finite_task_result (std::move (sent));
    if (!sent_result || !*sent_result)
        return 1;
    // reconcile is a "moving" phase for try_append_backlog: the packet
    // parks rather than dispatching immediately.
    if (handler_ran.load (std::memory_order_acquire))
        return 2;

    std::this_thread::sleep_for (std::chrono::milliseconds (30));
    (void) spots.cleanup_expired_actor_admissions ();
    // The deadline bounds the reconcile phase itself; assert that bound
    // directly (the move must be gone once expired), not just an
    // observable side effect of it.
    if (node->actor_transfer_coordinator.phase (key).has_value ())
        return 3;

    // Assert a bounded, observable outcome per spec 28, not just coordinator
    // state: a fresh request must be served, not parked again -- the Actor
    // is genuinely back in local service.
    spot_inbound_message_t follow_up_metadata;
    auto follow_up = spots.relay_actor_packet (
      actor, gateway.actor_context (actor), stream_message_kind_t::send,
      "ReconcileProbe", zlink::message_t::from (std::string ("probe-2")),
      provider, serializers, std::move (follow_up_metadata), nullptr);
    const auto follow_up_result = finite_task_result (std::move (follow_up));
    if (!follow_up_result || !*follow_up_result)
        return 4;

    const auto handler_deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (2);
    while (!handler_ran.load (std::memory_order_acquire)
           && std::chrono::steady_clock::now () < handler_deadline) {
        std::this_thread::yield ();
    }
    if (!handler_ran.load (std::memory_order_acquire))
        return 5;
    if (node->actor_transfer_coordinator.phase (key).has_value ())
        return 4;
    return 0;
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
    const auto old_connection = stream_sessions.open ("same-session-rid");
    const auto [old_bind_error, old_binding] =
      stream_sessions.bind_remote (
        old_connection, session_actor, 11, 17);
    const auto new_connection = stream_sessions.open ("same-session-rid");
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
      session_owner_rid, zlink::routing_id_t::from ("same-session-rid"),
      7, 11, 13, 17, old_binding.binding_generation, 0, 0};
    const actor_bound_session_route_t new_route{
      session_owner_rid, zlink::routing_id_t::from ("same-session-rid"),
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

int old_stream_disconnect_does_not_retire_reconnected_binding ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    using namespace zlink::framework::runtime::stateful;

    const object_ref_t native_actor{
      object_kind_t::actor, "disconnect-fenced-actor", 7, 13,
      "player", "actor-owner"};
    stream_session_registry_t sessions (
      [native_actor] (const std::string &actor_id)
        -> std::optional<object_ref_t> {
          return actor_id == native_actor.key
                   ? std::make_optional (native_actor)
                   : std::nullopt;
      });
    const auto old_connection = sessions.open ("old-stream-rid");
    const auto [old_error, old_binding] = sessions.bind_remote (
      old_connection, native_actor, 11, 17);
    const auto disconnect_snapshot = sessions.bindings (old_connection);
    const auto new_connection = sessions.open ("new-stream-rid");
    const auto [new_error, new_binding] = sessions.bind_remote (
      new_connection, native_actor, 11, 17);
    if (old_error != stateful_error_t::none
        || new_error != stateful_error_t::none
        || disconnect_snapshot.size () != 1
        || !sessions.bindings (old_connection).empty ()
        || sessions.bindings (new_connection).size () != 1
        || sessions.is_current_for_connection (
             old_connection, new_binding)
        || !sessions.is_current_for_connection (
             new_connection, new_binding)
        || new_binding.binding_generation
             <= old_binding.binding_generation) {
        return 1;
    }

    actor_gateway_runtime_t gateway;
    const auto actor = test_actor_ref (
      "actor-owner", "player", native_actor.key, 7);
    const auto session_owner = zlink::routing_id_t::from ("session-owner");
    const auto old_rid = zlink::routing_id_t::from ("old-stream-rid");
    const auto new_rid = zlink::routing_id_t::from ("new-stream-rid");
    std::atomic_int new_stream_received{0};
    if (!gateway.replace_session_route (
          actor,
          [] (std::string, stream_codec_t, const zlink::message_t &) {
              return task_t<void> (result_t<void>::success ());
          },
          actor_bound_session_route_t{
            session_owner, old_rid, 7, 11, 13, 17,
            old_binding.binding_generation, 0, 0})
        || !gateway.replace_session_route (
          actor,
          [&new_stream_received] (
            std::string, stream_codec_t, const zlink::message_t &) {
              ++new_stream_received;
              return task_t<void> (result_t<void>::success ());
          },
          actor_bound_session_route_t{
            session_owner, new_rid, 7, 11, 13, 17,
            new_binding.binding_generation, 0, 0})) {
        return 2;
    }

    for (const auto &retiring : disconnect_snapshot) {
        if (sessions.is_current (retiring)) {
            (void) gateway.retire_bound_session_route (
              actor, session_owner, old_rid,
              retiring.binding_generation);
        }
    }
    if (gateway.retire_bound_session_route (
          actor, session_owner, old_rid,
          old_binding.binding_generation)) {
        return 3;
    }
    const auto current = sessions.current_binding (native_actor.key);
    const auto route = gateway.bound_session_route (actor);
    const auto delivered = gateway.dispatch_bound_session_send (
      actor, "after-reconnect", stream_codec_t::message_pack,
      zlink::message_t::from ("payload"));
    return current && *current == new_binding && route
             && route->session_rid == new_rid
             && route->binding_generation
                  == new_binding.binding_generation
             && delivered && new_stream_received.load () == 1
           ? 0
           : 4;
}

int command_38_rebind_is_owned_only_by_new_connection ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    using namespace zlink::framework::runtime;

    const stateful::object_ref_t native_actor{
      stateful::object_kind_t::actor, "command-38-rebind-actor",
      7, 13, "player", "actor-owner"};
    stateful::stream_session_registry_t sessions (
      [native_actor] (const std::string &actor_id)
        -> std::optional<stateful::object_ref_t> {
          return actor_id == native_actor.key
            ? std::make_optional (native_actor)
            : std::nullopt;
      });
    actor_gateway_runtime_t gateway;
    const auto actor = test_actor_ref (
      "actor-owner", "player", native_actor.key, 7);
    const auto session_owner = zlink::routing_id_t::from (
      "session-owner");

    const auto receive_command_38 = [&] (
      const protocol::bound_session_bind_t &outbound) {
        const auto received = protocol::decode_bound_session_bind (
          protocol::encode_bound_session_bind (outbound));
        const auto session_rid = zlink::routing_id_t::from (
          received.session_routing_id);
        return gateway.record_bound_session_route (
          actor, session_owner, session_rid,
          11,
          received.actor.authority_owner_generation,
          received.actor.owner_lease_generation,
          received.binding.generation, 0, 0);
    };
    const auto bind_through_native_and_38 = [&] (
      const stateful::stream_connection_t &connection,
      const zlink::routing_id_t &session_rid,
      std::uint64_t binding_generation)
      -> std::optional<stateful::stream_binding_t> {
        auto [error, binding] = sessions.bind_remote (
          connection, native_actor, 11, 17, true,
          binding_generation);
        if (error != stateful::stateful_error_t::none)
            return std::nullopt;
        const protocol::bound_session_bind_t command_38{
          binding.binding_generation,
          protocol::actor_route_fence_t{
            native_actor.key, native_actor.object_generation,
            zlink::routing_id_t::from (native_actor.node_id).to_bytes (),
            11, native_actor.authority_owner_generation, 17},
          session_rid.to_bytes (),
          {protocol::bound_session_binding_state_t::active,
           binding.binding_generation}};
        if (!receive_command_38 (command_38))
            return std::nullopt;
        if (!sessions.complete_route_publish (binding))
            return std::nullopt;
        return binding;
    };

    const auto old_connection = sessions.open ("command-38-old");
    const auto old_binding = bind_through_native_and_38 (
      old_connection, zlink::routing_id_t::from ("session-old"), 1);
    const auto new_connection = sessions.open ("command-38-new");
    const auto new_binding = bind_through_native_and_38 (
      new_connection, zlink::routing_id_t::from ("session-new"), 2);
    if (!old_binding || !new_binding)
        return 1;
    const auto route = gateway.bound_session_route (actor);
    return sessions.bindings (old_connection).empty ()
             && sessions.bindings (new_connection).size () == 1
             && !sessions.is_current_for_connection (
                  old_connection, *new_binding)
             && sessions.is_current_for_connection (
                  new_connection, *new_binding)
             && route
             && route->session_rid
                  == zlink::routing_id_t::from ("session-new")
             && route->binding_generation == 2
           ? 0
           : 2;
}

int local_bound_session_refreshes_cached_route_before_fast_path ()
{
    using namespace zlink::framework;

    auto state = std::make_shared<detail::mesh_node_builder_state_t> (
      "bound-session-refresh-mesh");
    detail::mesh_node_runtime_t runtime (state);
    const auto actor = detail::actor_ref_access_t::make (
      node_rid_t::from_string ("current-actor-owner"), "PlayerActor",
      "reconnected-player", 7);
    const runtime::spot_address_t stale{
      "bound-session-refresh-mesh",
      zlink::routing_id_t::from ("stale-session-owner"), {}, 0, {},
      7, 11, {"stale-owner", 13}, 17};
    const runtime::spot_address_t current{
      "bound-session-refresh-mesh",
      zlink::routing_id_t::from ("current-actor-owner"), {}, 0, {},
      7, 12, {"current-owner", 14}, 18};
    bool invalidated = false;
    runtime.configure_actor_route_resolver (
      [&] (const actor_ref_t &) -> std::optional<runtime::spot_address_t> {
          return invalidated ? current : stale;
      },
      [&] (const runtime::protocol::actor_route_fence_t &route) {
          invalidated = route.actor_id == "reconnected-player"
                        && route.target_node_routing_id
                             == stale.node_rid.to_bytes ()
                        && route.target_node_generation
                             == stale.node_generation
                        && route.authority_owner_generation
                             == stale.authority_owner_generation
                        && route.owner_lease_generation
                             == static_cast<std::uint64_t> (
                               stale.owner.lease_generation);
      });
    const auto cached = runtime.resolve_application_actor_route (actor);
    if (!cached || cached->node_rid != stale.node_rid)
        return 1;
    const auto refreshed = runtime.refresh_application_actor_route (
      actor, *cached);
    return invalidated && refreshed
             && refreshed->node_rid == current.node_rid
             && refreshed->node_generation == current.node_generation
             && refreshed->authority_owner_generation
                  == current.authority_owner_generation
             && refreshed->owner.lease_generation
                  == current.owner.lease_generation
           ? 0
           : 2;
}

int late_lower_generation_bind_and_publish_are_ignored ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    using namespace zlink::framework::runtime::stateful;

    const object_ref_t native_actor{
      object_kind_t::actor, "monotonic-binding-actor", 7, 13,
      "player", "actor-owner"};
    stream_session_registry_t sessions (
      [native_actor] (const std::string &actor_id)
        -> std::optional<object_ref_t> {
          return actor_id == native_actor.key
                   ? std::make_optional (native_actor)
                   : std::nullopt;
      });
    const auto current_connection = sessions.open ("current-stream-rid");
    const auto [current_error, current_binding] = sessions.bind_remote (
      current_connection, native_actor, 11, 17, false, 22);
    const auto stale_connection = sessions.open ("stale-stream-rid");
    const auto [stale_error, stale_binding] = sessions.bind_remote (
      stale_connection, native_actor, 11, 17, false, 21);
    const auto registry_current = sessions.current_binding (native_actor.key);
    if (current_error != stateful_error_t::none
        || stale_error != stateful_error_t::conflict
        || stale_binding.binding_generation != 0
        || !registry_current || *registry_current != current_binding) {
        return 1;
    }

    actor_gateway_runtime_t gateway;
    const auto actor = test_actor_ref (
      "actor-owner", "player", native_actor.key, 7);
    const auto session_owner = zlink::routing_id_t::from ("session-owner");
    const auto current_rid = zlink::routing_id_t::from (
      "current-stream-rid");
    const auto stale_rid = zlink::routing_id_t::from (
      "stale-stream-rid");
    std::atomic_int current_stream_received{0};
    std::atomic_int stale_stream_received{0};
    if (!gateway.replace_session_route (
          actor,
          [&current_stream_received] (
            std::string, stream_codec_t, const zlink::message_t &) {
              ++current_stream_received;
              return task_t<void> (result_t<void>::success ());
          },
          actor_bound_session_route_t{
            session_owner, current_rid, 7, 11, 13, 17, 22, 0, 0})) {
        return 2;
    }
    const auto stale_publish = gateway.replace_session_route (
      actor,
      [&stale_stream_received] (
        std::string, stream_codec_t, const zlink::message_t &) {
          ++stale_stream_received;
          return task_t<void> (result_t<void>::success ());
      },
      actor_bound_session_route_t{
        session_owner, stale_rid, 7, 11, 13, 17, 21, 0, 0});
    const auto stale_record = gateway.record_bound_session_route_transition (
      actor, actor_bound_session_route_t{
               session_owner, stale_rid, 7, 11, 13, 17, 20, 0, 0});
    const auto route = gateway.bound_session_route (actor);
    const auto delivered = gateway.dispatch_bound_session_send (
      actor, "after-stale-publish", stream_codec_t::message_pack,
      zlink::message_t::from ("payload"));
    return stale_publish && !stale_publish.value ().changed
             && stale_publish.value ().current
             && stale_publish.value ().current->binding_generation == 22
             && stale_record && !stale_record.value ().changed
             && stale_record.value ().current
             && stale_record.value ().current->binding_generation == 22
             && route && route->session_rid == current_rid
             && route->binding_generation == 22 && delivered
             && current_stream_received.load () == 1
             && stale_stream_received.load () == 0
           ? 0
           : 3;
}

int same_rid_same_generation_defensively_replaces_stream_capability ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    using namespace zlink::framework::runtime::stateful;

    const object_ref_t native_actor{
      object_kind_t::actor, "defensive-rebind", 7, 13,
      "player", "actor-owner"};
    stream_session_registry_t sessions (
      [native_actor] (const std::string &actor_id)
        -> std::optional<object_ref_t> {
          return actor_id == native_actor.key
            ? std::make_optional (native_actor)
            : std::nullopt;
      });
    const auto old_connection = sessions.open ("reused-session-rid");
    const auto [old_error, old_binding] = sessions.bind_remote (
      old_connection, native_actor, 11, 17, false, 41);
    const auto new_connection = sessions.open ("reused-session-rid");
    const auto [new_error, new_binding] = sessions.bind_remote (
      new_connection, native_actor, 11, 17, false, 41);
    const auto current = sessions.current_binding (native_actor.key);
    if (old_error != stateful_error_t::none
        || new_error != stateful_error_t::none || !current
        || current->connection != new_connection
        || current->binding_generation != 41)
        return 1;

    actor_gateway_runtime_t gateway;
    const auto actor = test_actor_ref (
      "actor-owner", "player", "defensive-rebind", 7);
    const auto owner_rid = zlink::routing_id_t::from ("session-owner");
    const auto session_rid = zlink::routing_id_t::from (
      "reused-session-rid");
    std::atomic_int old_stream_received{0};
    std::atomic_int new_stream_received{0};
    const actor_bound_session_route_t old_route{
      owner_rid, session_rid, 7, 11, 13, 17, 41, 1, 0};
    const actor_bound_session_route_t new_route{
      owner_rid, session_rid, 7, 11, 13, 17, 41, 2, 0};
    if (!gateway.replace_session_route (
          actor,
          [&old_stream_received] (std::string, stream_codec_t,
                                  const zlink::message_t &) {
              ++old_stream_received;
              return task_t<void> (result_t<void>::success ());
          },
          old_route))
        return 2;
    const auto replaced = gateway.replace_session_route (
      actor,
      [&new_stream_received] (std::string, stream_codec_t,
                              const zlink::message_t &) {
          ++new_stream_received;
          return task_t<void> (result_t<void>::success ());
      },
      new_route);
    if (!replaced || !replaced.value ().changed
        || !replaced.value ().previous)
        return 3;
    const auto delivered = gateway.dispatch_bound_session_send (
      actor, "defensive-push", stream_codec_t::message_pack,
      zlink::message_t::from ("payload"));
    return delivered && old_stream_received.load () == 0
             && new_stream_received.load () == 1
           ? 0
           : 4;
}

int same_rid_registration_retains_retired_close_handler ()
{
    using namespace zlink::framework::detail;

    actor_gateway_runtime_t gateway;
    const auto session_rid = zlink::routing_id_t::from (
      "reused-handler-rid");
    std::atomic_int retired_calls{0};
    std::atomic_int current_calls{0};
    auto retired = gateway.register_bound_session_replacement_handler (
      session_rid, [&retired_calls] (const auto &) {
          ++retired_calls;
          return true;
      });
    auto current = gateway.register_bound_session_replacement_handler (
      session_rid, [&current_calls] (const auto &) {
          ++current_calls;
          return true;
      });
    zlink::framework::runtime::protocol::bound_session_replaced_t replacement;
    replacement.retired_session.session_routing_id =
      session_rid.to_bytes ();
    if (!gateway.dispatch_bound_session_replaced (replacement)
        || retired_calls.load () != 1 || current_calls.load () != 0)
        return 1;
    gateway.unregister_bound_session_replacement_handler (
      session_rid, retired);
    if (!gateway.dispatch_bound_session_replaced (replacement)
        || retired_calls.load () != 1 || current_calls.load () != 1)
        return 2;
    gateway.unregister_bound_session_replacement_handler (
      session_rid, current);
    return !gateway.dispatch_bound_session_replaced (replacement) ? 0 : 3;
}

} // namespace

/* async-execution-policy §1.3: the session Actor relay waiter is bounded —
 * when the FIFO is full a new relay completes immediately with
 * DeadlineExceeded and is never submitted later. */
int session_relay_waiter_capacity_is_bounded ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    actor_gateway_runtime_t gateway;
    auto manager = gateway.manager ();
    session_actor_manager_access_t::attach (manager, stream_t{});
    const actor_ref_t actor =
      test_actor_ref ("actor-node-a", "player", "actor-capacity", 1);
    auto binding = manager.bind (actor).submit ().result ().value ();

    std::vector<std::shared_ptr<
      task_completion_source_t<std::optional<zlink::message_t>>>> dispatched;
    gateway.on_relay (
      [&dispatched] (const actor_ref_t &, const actor_context_t &,
                     const stream_header_t &, const zlink::message_t &,
                     std::optional<bound_session_relay_source_t>) {
          auto source = std::make_shared<
            task_completion_source_t<std::optional<zlink::message_t>>> ();
          dispatched.push_back (source);
          return source->task ();
      });

    /* One relay occupies the drain turn; 1024 fill the bounded waiter. */
    std::vector<task_t<void>> accepted;
    accepted.reserve (1025);
    for (std::size_t index = 0; index != 1025; ++index)
        accepted.push_back (binding.relay ("packet", zlink::message_t{}));
    auto refused = binding.relay ("packet", zlink::message_t{});
    if (!refused.await_ready ())
        return 1;
    const auto refusal = refused.result ();
    if (refusal
        || refusal.error_kind () != framework_error_kind_t::deadline_exceeded)
        return 2;

    /* Releasing each dispatch drains the FIFO; every admitted relay
     * completes exactly once and the refused one is never resubmitted. */
    for (std::size_t index = 0; index != dispatched.size (); ++index)
        dispatched[index]->complete (
          result_t<std::optional<zlink::message_t>>::success (std::nullopt));
    if (dispatched.size () != 1025)
        return 3;
    for (auto &admitted : accepted) {
        if (!admitted.await_ready () || !admitted.result ())
            return 4;
    }
    return 0;
}

int main ()
{
    if (const auto capacity = session_relay_waiter_capacity_is_bounded ();
        capacity != 0) {
        return 290 + capacity;
    }
    if (const auto finite =
          bind_or_get_all_exit_paths_complete_within_deadline ();
        finite != 0) {
        return 285 + finite;
    }
    if (const auto reentrant =
          bind_or_get_completion_can_reenter_same_manager ();
        reentrant != 0) {
        return 280 + reentrant;
    }
    if (const auto refreshed =
          local_bound_session_refreshes_cached_route_before_fast_path ();
        refreshed != 0) {
        return 275 + refreshed;
    }
    if (const auto command_38 =
          command_38_rebind_is_owned_only_by_new_connection ();
        command_38 != 0) {
        return 270 + command_38;
    }
    if (const auto monotonic =
          late_lower_generation_bind_and_publish_are_ignored ();
        monotonic != 0) {
        return 265 + monotonic;
    }
    if (const auto disconnect_fence =
          old_stream_disconnect_does_not_retire_reconnected_binding ();
        disconnect_fence != 0) {
        return 260 + disconnect_fence;
    }
    if (const auto handlers =
          same_rid_registration_retains_retired_close_handler ();
        handlers != 0) {
        return 250 + handlers;
    }
    if (const auto defensive =
          same_rid_same_generation_defensively_replaces_stream_capability ();
        defensive != 0) {
        return 245 + defensive;
    }
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
          actor_request_completion_keeps_dedup_state_owned_after_runtime_wrapper_unwinds ();
        admission != 0) {
        return 200 + admission;
    }
    if (const auto cold_forward =
          old_owner_forwards_cold_probe_via_active_message_follow_route ();
        cold_forward != 0) {
        return 300 + cold_forward;
    }
    if (const auto leave_race =
          source_cleanup_waits_for_leave_completion_before_erasing_actor ();
        leave_race != 0) {
        return 310 + leave_race;
    }
    if (const auto reconcile_trap =
          reconcile_deadline_restores_actor_to_local_service ();
        reconcile_trap != 0) {
        return 320 + reconcile_trap;
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
    if (const auto idempotent =
          bind_or_get_reuses_same_physical_session_generation ();
        idempotent != 0) {
        return 155 + idempotent;
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
