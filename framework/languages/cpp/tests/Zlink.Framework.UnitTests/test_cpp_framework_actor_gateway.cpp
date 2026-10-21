/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "../support/session_disconnect_probe_spot.hpp"
#include "runtime/mesh/mesh_node_host_service.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "../support/manual_internal_scheduler.hpp"
#include "../support/request_admission_fixture.hpp"

#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/channels/channel_reply_writer.hpp"
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/diagnostics/flow_context.hpp"
#include "runtime/diagnostics/dispatch_error_reporter.hpp"
#include "runtime/dispatch/coroutine_executor.hpp"
#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/execution/actor_execution_context.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/locations/actor_authority_payload.hpp"
#include "runtime/locations/in_memory_location_store.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/live_location_reader.hpp"
#include "runtime/locations/provider_location_repository.hpp"
#include "runtime/protocol/service_wire_codec.hpp"
#include "runtime/spots/spot_route_internal_dispatcher.hpp"
#include "runtime/spots/spot_route_packets.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include "runtime/stateful/stream_session_registry.hpp"
#include "runtime/stateful/accepted_actor_record.hpp"
#include "runtime/streams/stream_runtime.hpp"

#include <zlink/codecs/protobuf.hpp>
#include <zlink/framework.hpp>
#include <zlink/framework/contracts/configuration/zlink_builder.hpp>

#include <google/protobuf/wrappers.pb.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <future>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace
{

zlink::framework::actor_ref_t test_actor_ref (std::string node,
                                              std::string actor_type,
                                              std::string actor_id,
                                              std::uint64_t generation)
{
    return zlink::framework::detail::actor_ref_access_t::make (
      zlink::framework::node_rid_t::from_string (std::move (node)), std::move (actor_type),
      std::move (actor_id), generation);
}

class actor_join_authority_store_t final
    : public zlink::framework::runtime::in_memory_location_repository_t
{
  public:
    std::optional<zlink::framework::authority_snapshot_t> snapshot;
    bool unreadable = false;

    zlink::framework::task_t<zlink::framework::authority_read_result_t>
    read_authority (zlink::framework::authority_key_t, std::stop_token) override
    {
        if (unreadable) {
            return zlink::framework::task_t<zlink::framework::authority_read_result_t> (
              zlink::framework::result_t<zlink::framework::authority_read_result_t>::failure (
                zlink::framework::framework_error_kind_t::unavailable,
                "Location Store is unavailable"));
        }
        if (snapshot) {
            return zlink::framework::task_t<zlink::framework::authority_read_result_t> (
              zlink::framework::result_t<zlink::framework::authority_read_result_t>::success (
                zlink::framework::authority_read_result_t{*snapshot}));
        }
        return zlink::framework::task_t<zlink::framework::authority_read_result_t> (
          zlink::framework::result_t<zlink::framework::authority_read_result_t>::success (
            zlink::framework::authority_read_result_t{
              zlink::framework::authority_missing_t{std::chrono::system_clock::now ()}}));
    }
};

template <typename T>
std::optional<zlink::framework::result_t<T>>
finite_task_result (zlink::framework::task_t<T> task,
                    std::chrono::milliseconds timeout = std::chrono::milliseconds (500))
{
    struct wait_state_t
    {
        std::mutex mutex;
        std::condition_variable ready;
        std::optional<zlink::framework::result_t<T>> result;
    };

    auto wait_state = std::make_shared<wait_state_t> ();
    auto observed = std::make_shared<zlink::framework::task_t<T>> (std::move (task));
    zlink::framework::detail::observe_task_completion (
      *observed, [wait_state, observed] (const zlink::framework::result_t<T> &result) {
          {
              const std::lock_guard lock (wait_state->mutex);
              wait_state->result = result;
          }
          wait_state->ready.notify_one ();
      });
    std::unique_lock lock (wait_state->mutex);
    if (!wait_state->ready.wait_for (lock, timeout,
                                     [&] { return wait_state->result.has_value (); })) {
        return std::nullopt;
    }
    return wait_state->result;
}

int relay_dispatch_scope_restores_nested_and_exception_state ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    const stream_header_t outer (stream_message_kind_t::send, stream_codec_t::json,
                                 stream_header_flags_t::none, std::nullopt, "outer");
    const stream_header_t inner (stream_message_kind_t::request, stream_codec_t::json,
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

// A session handler that relays after an await which resumed outside the
// dispatch scope still sees the header of the frame it is handling.
int relay_dispatch_header_follows_handler_await ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    runtime::install_host_context_hooks ();
    auto pending = std::make_shared<task_completion_source_t<void>> ();
    auto observed = std::make_shared<std::optional<std::string>> ();
    auto handler = [] (std::shared_ptr<task_completion_source_t<void>> pending,
                       std::shared_ptr<std::optional<std::string>> observed) -> task_t<void> {
        co_await pending->task ();
        if (const auto header = current_stream_relay_dispatch ())
            *observed = std::string (header->packet_name ());
        co_return;
    };
    task_t<void> running = [&] {
        const stream_relay_dispatch_scope_t scope (
          stream_header_t (stream_message_kind_t::request, stream_codec_t::json,
                           stream_header_flags_t::none, std::nullopt, "dispatched"));
        return handler (pending, observed);
    }();
    if (current_stream_relay_dispatch ())
        return 1;
    bool resumer_leaked = true;
    std::thread resumer ([&] {
        pending->complete (result_t<void>::success ());
        resumer_leaked = current_stream_relay_dispatch ().has_value ();
    });
    resumer.join ();
    if (!running.result ())
        return 2;
    if (*observed != std::optional<std::string> ("dispatched"))
        return 3;
    return resumer_leaked ? 4 : 0;
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
    if (!rejects ({}) || !rejects (std::string ("\xc3\x28")) || !rejects (std::string ("\xc0\x80"))
        || !rejects (std::string ("\xed\xa0\x80")) || !rejects (std::string ("\xf4\x90\x80\x80"))
        || !rejects (std::string (256, 'a'))) {
        return 2;
    }

    const auto node = node_rid_t::from_string ("actor-node");
    const auto max_generation =
      static_cast<std::uint64_t> (std::numeric_limits<std::int64_t>::max ());
    const actor_ref_t maximum (actor_id_t ("actor"), max_generation, "game", node);
    if (maximum.object_generation () != max_generation)
        return 3;

    const auto rejects_generation = [&] (std::uint64_t generation) {
        try {
            actor_ref_t invalid (actor_id_t ("actor"), generation, "game", node);
            static_cast<void> (invalid);
        }
        catch (const std::invalid_argument &) {
            return true;
        }
        return false;
    };
    if (!rejects_generation (0) || !rejects_generation (max_generation + 1)) {
        return 4;
    }
    return 0;
}

class recording_actor_client_t final : public zlink::framework::actor_client_t
{
  public:
    std::atomic_int attempts{0};

  protected:
    zlink::framework::task_t<void>
    send_erased (zlink::framework::actor_id_t,
                 std::string,
                 zlink::framework::message_t,
                 const zlink::framework::actor_send_call_t::metadata_map_t &) override
    {
        ++attempts;
        return zlink::framework::task_t<void> (zlink::framework::result_t<void>::success ());
    }

    zlink::framework::request_task_t<zlink::framework::message_t>
    request_erased (zlink::framework::actor_id_t,
                    std::string,
                    zlink::framework::message_t,
                    std::optional<std::chrono::milliseconds>,
                    const zlink::framework::actor_request_call_t::metadata_map_t &) override
    {
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
    old_runtime.attach_transport_writer (
      old_stream_copy, [&old_stream_writes] (const stream_header_t &, const zlink::message_t &) {
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
    new_runtime.attach_transport_writer (
      new_stream_copy, [&new_stream_writes] (const stream_header_t &, const zlink::message_t &) {
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
    if (!delivered || old_stream_writes.load () != 0 || new_stream_writes.load () != 1)
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
    const auto stale_disconnect = stale_handle.notify_disconnected ().result ();
    if (stale_disconnect || stale_disconnect.error_kind () != framework_error_kind_t::not_configured
        || disconnected != 0 || old_session.find ("direct-rebind-actor")) {
        release_binder_source.set_value ();
        rebind_thread.join ();
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
        release_binder_source.set_value ();
        rebind_thread.join ();
        return 4;
    }

    release_binder_source.set_value ();
    rebind_thread.join ();
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

int actor_send_is_one_shot ()
{
    using namespace zlink::framework;
    recording_actor_client_t client;
    actor_send_call_t call (client, actor_id_t ("actor-2"), "message", message_t{});
    auto copied = call;
    call.async ().result ().value ();
    bool rejected = false;
    try {
        (void) copied.async ().result ().value ();
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

    actor_gateway_runtime_t gateway;
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
    if (!original_binding.relay ("packet", zlink::message_t{}).result ()
        || relay_routes.size () != 1
        || relay_routes.front ().node_rid ().value () != original.node_rid ().value ()) {
        return 5;
    }
    if (!original_binding.notify_disconnected ().result ()
        || !gateway.update_actor_ref (relocated)) {
        return 7;
    }
    auto relocated_binding = manager.bind (relocated).async ().result ().value ();
    if (!relocated_binding.relay ("packet", zlink::message_t{}).result ()
        || relay_routes.size () != 2
        || relay_routes.back ().node_rid ().value () != relocated.node_rid ().value ()) {
        return 8;
    }
    if (!unaffected_binding.relay ("packet", zlink::message_t{}).result ()
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

zlink::framework::task_t<zlink::framework::result_t<void>>
invoke_app_bound_session_sender_with_temporary_arguments (
  const zlink::framework::detail::actor_gateway_state_t::bound_session_sender_t &sender,
  const zlink::framework::node_rid_t &node_rid,
  const std::string &actor_id)
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    auto actor =
      std::make_unique<actor_ref_t> (actor_ref_access_t::make (node_rid, "player", actor_id, 7));
    auto header = std::make_unique<stream_header_t> (
      stream_message_kind_t::send, stream_codec_t::json, stream_header_flags_t::none, std::nullopt,
      std::string (96, 'h'));
    auto payload =
      std::make_unique<zlink::message_t> (zlink::message_t::from (std::string (256, 'p')));
    auto local_sender = sender;
    return local_sender (*actor, 19, *header, *payload);
}

int app_bound_session_sender_keeps_arguments_alive_after_route_lookup_suspends ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    auto app = app_t::create ();
    const auto node_id = "bound-session-lifetime-node";
    const auto framework_node_rid = node_rid_t::from_string (node_id);
    const auto node_rid = zlink::routing_id_t::from (node_id);
    app.add_zlink_framework ([&] (zlink_framework_options_t &options) {
        options.add_route_mesh ("bound-session-lifetime-mesh")
          .set_object_role (object_role_t::none)
          .set_routing_id (node_rid)
          .listen (0);
    });

    auto provider = app.advanced ().services ().build_provider ();
    auto &gateway = provider.get_required<actor_gateway_runtime_t> ();
    const auto state = gateway.weak_state ().lock ();
    if (!state || !state->bound_session_sender) {
        provider.close ();
        return 1;
    }

    const std::string actor_id = "bound-session-lifetime-actor-" + std::string (48, 'a');
    const auto actor = actor_ref_access_t::make (framework_node_rid, "player", actor_id, 7);
    if (!gateway.record_bound_session_route (
          actor, node_rid, zlink::routing_id_t::from ("bound-session-lifetime-session"), 11, 13, 17,
          19)) {
        provider.close ();
        return 2;
    }

    const char *const raw_arguments[] = {"actor-gateway-coroutine-lifetime"};
    auto **const arguments = const_cast<char **> (raw_arguments);
    int app_exit_code = -1;
    std::exception_ptr app_failure;
    std::thread app_thread ([&] {
        try {
            app_exit_code = app.run (1, arguments);
        }
        catch (const std::exception &) {
            app_failure = std::current_exception ();
        }
    });
    const auto app_ready_deadline = std::chrono::steady_clock::now () + std::chrono::seconds (5);
    while (!app.is_ready () && app.runtime_state () != framework_runtime_state_t::error
           && std::chrono::steady_clock::now () < app_ready_deadline) {
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    if (!app.is_ready ()) {
        app.stop ();
        app_thread.join ();
        provider.close ();
        return 3;
    }

    std::promise<void> lane_entered;
    auto lane_has_entered = lane_entered.get_future ();
    std::promise<void> lane_release;
    auto release_lane = lane_release.get_future ().share ();
    std::exception_ptr lane_failure;
    std::thread lane_thread ([&] {
        try {
            state->lane
              .run ([&] {
                  lane_entered.set_value ();
                  release_lane.wait ();
              })
              .get ();
        }
        catch (const std::exception &) {
            lane_failure = std::current_exception ();
        }
    });
    if (lane_has_entered.wait_for (std::chrono::seconds (1)) != std::future_status::ready) {
        lane_release.set_value ();
        lane_thread.join ();
        app.stop ();
        app_thread.join ();
        provider.close ();
        return 4;
    }

    const auto sender = state->bound_session_sender;
    auto pending = invoke_app_bound_session_sender_with_temporary_arguments (
      sender, framework_node_rid, actor_id);
    const bool suspended_on_route_lookup = !pending.result_for (std::chrono::milliseconds (0));
    lane_release.set_value ();
    lane_thread.join ();
    const auto sent = pending.result ();

    app.stop ();
    app_thread.join ();
    provider.close ();
    if (app_failure || app_exit_code != 0 || lane_failure) {
        return 5;
    }
    // The native Session may not exist in this unit scenario. The send outcome
    // is immaterial here: the registered app callback must resume and consume
    // its actor, header, and payload after route lookup has yielded.
    (void) sent;
    return suspended_on_route_lookup ? 0 : 6;
}

int generated_protobuf_bound_session_uses_typed_serializer_codec ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    auto state = std::make_shared<actor_gateway_state_t> ();
    serializer_registry_t serializers;
    codec_registration_context_t registration (serializers);
    zlink::framework_codecs::protobuf ().register_framework_codecs (registration);
    state->serializers = &serializers;
    actor_gateway_runtime_t gateway (state);
    const auto actor = test_actor_ref ("actor-node", "player", "protobuf-bound-session", 7);
    struct observation_t
    {
        std::atomic_bool delivered{false};
        std::string packet_name;
        stream_codec_t codec = stream_codec_t::raw;
        std::string payload;
    };
    const auto observation = std::make_shared<observation_t> ();
    if (!gateway.bind_session_sink (actor, [observation] (std::string packet_name,
                                                          stream_codec_t codec,
                                                          const zlink::message_t &payload) {
            observation->packet_name = std::move (packet_name);
            observation->codec = codec;
            observation->payload = payload.to_string ();
            observation->delivered.store (true, std::memory_order_release);
            return task_t<void> (result_t<void>::success ());
        })) {
        return 1;
    }

    google::protobuf::StringValue message;
    message.set_value ("bound-protobuf");
    const auto expected_payload = message.SerializeAsString ();
    const auto sent =
      gateway.actor_context (actor).bound_session ().send (message).async ().result ();
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (1);
    while (!observation->delivered.load (std::memory_order_acquire)
           && std::chrono::steady_clock::now () < deadline) {
        std::this_thread::yield ();
    }
    if (!sent || !observation->delivered.load (std::memory_order_acquire)) {
        return 2;
    }
    if (observation->packet_name != "StringValue" || observation->codec != stream_codec_t::protobuf
        || observation->payload != expected_payload) {
        return 3;
    }
    return 0;
}

int relocation_target_prewarm_publishes_store_confirmed_actor_and_session_fence_atomically ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace protocol = zlink::framework::runtime::protocol;

    auto state = std::make_shared<actor_gateway_state_t> ();
    serializer_registry_t serializers;
    state->serializers = &serializers;
    actor_gateway_runtime_t gateway (state);
    const auto source = test_actor_ref ("source-node", "player", "relocation-prewarm", 7);
    const auto target = test_actor_ref ("target-node", "player", "relocation-prewarm", 7);
    const auto session_owner = zlink::routing_id_t::from (std::string ("session-owner"));
    const auto session_rid = zlink::routing_id_t::from (std::string ("session-rid"));
    // The gateway state outlives this scope, so the callbacks own what they write.
    auto sends = std::make_shared<std::atomic_int> (0);
    auto sent_node = std::make_shared<std::string> ();
    // The gateway state outlives this scope, so the observer owns its flag.
    auto saw_stale_target = std::make_shared<std::atomic_bool> (false);
    dispatch_options_t trace_options;
    trace_options.message_flow (message_flow_log_mode_t::detailed);
    dispatch_options_access_t::set_observer_for_tests (
      trace_options, [saw_stale_target] (const message_flow_event_t &event) {
          if (event.outcome == message_flow_outcome_t::dropped
              && event.reason == message_flow_reason_t::stale_target
              && event.detail_stage == std::optional<std::string> ("route_fence")) {
              saw_stale_target->store (true, std::memory_order_release);
          }
      });
    gateway.set_dispatch (std::move (trace_options));
    gateway.on_bound_session_send (
      [sends, sent_node] (const actor_ref_t &actor, std::uint64_t binding_generation,
                          const stream_header_t &,
                          const zlink::message_t &) -> task_t<result_t<void>> {
          if (binding_generation == 23)
              *sent_node = std::string (actor.node_rid ().value ());
          ++*sends;
          co_return result_t<void>::success ();
      });
    // The target can retain a lower cache generation when it did not own an
    // intervening relocation. Post-CAS prewarm must advance that cache to the
    // Store-confirmed route rather than require it to equal the previous fence.
    if (!gateway.record_bound_session_route (source, session_owner, session_rid, 11, 15, 19, 23, 0,
                                             1)) {
        return 1;
    }
    const auto staged_source_sink =
      state->sync ([&] { return state->bound_session_sinks.at ("relocation-prewarm"); });

    const protocol::session_relocation_route_t route{
      .sender_role = protocol::relocation_role_t::target,
      .actor = {"relocation-prewarm", 7},
      .session_owner_node_routing_id = session_owner.to_bytes (),
      .session_owner_node_generation = 11,
      .session_owner_id = "session-owner-id",
      .session_owner_lease_generation = 13,
      .session_routing_id = session_rid.to_bytes (),
      .binding_generation = 23,
      .route = {protocol::session_relocation_route_action_t::commit, 17, 29,
                zlink::routing_id_t::from (std::string ("target-node")).to_bytes (), 31, 0, 37}};
    if (!gateway.prepare_session_relocation_target_route (route, 37)) {
        return 2;
    }
    const auto prewarmed_route_matches = state->sync ([&] {
        const auto &record = state->actors_by_id.at ("relocation-prewarm");
        return record.ref.node_rid ().value () == target.node_rid ().value ()
               && record.bound_session_route
               && record.bound_session_route->authority_owner_generation == 29
               && record.bound_session_route->owner_lease_generation == 37
               && record.bound_session_route->session_sequence == 0
               && record.bound_session_route->session_sequence_baseline_unknown;
    });
    if (!prewarmed_route_matches)
        return 3;

    const auto staged_push =
      (*staged_source_sink) ("RelocationStagedPush", stream_codec_t::message_pack,
                             zlink::message_t::from ("staged"))
        .result ();
    if (!staged_push || sends->load (std::memory_order_acquire) != 1
        || *sent_node != "target-node") {
        return 4;
    }
    sends->store (0, std::memory_order_release);
    sent_node->clear ();

    const protocol::actor_route_fence_t target_fence{"relocation-prewarm",
                                                     7,
                                                     route.route.target_node_routing_id,
                                                     route.route.target_node_generation,
                                                     route.route.target_authority_owner_generation,
                                                     37};
    if (!gateway.admit_session_relay (target, session_owner, session_rid, 23, 25, &target_fence)
        || !gateway.prepare_session_relocation_target_route (route, 37)) {
        return 5;
    }
    const auto prewarmed_sequence_is_admitted = state->sync ([&] {
        const auto &prepared = *state->actors_by_id.at ("relocation-prewarm").bound_session_route;
        return prepared.session_sequence == 25 && !prepared.session_sequence_baseline_unknown;
    });
    if (!prewarmed_sequence_is_admitted)
        return 6;

    const auto submitted = gateway.actor_context (target)
                             .bound_session ()
                             .send (std::string ("RelocationReady"))
                             .async ()
                             .result ();
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (1);
    while (sends->load (std::memory_order_acquire) != 1
           && std::chrono::steady_clock::now () < deadline) {
        std::this_thread::yield ();
    }
    if (!submitted || sends->load () != 1 || *sent_node != "target-node")
        return 7;

    auto stale = route;
    stale.route.previous_authority_owner_generation = 16;
    stale.route.target_authority_owner_generation = 30;
    if (gateway.prepare_session_relocation_target_route (stale, 41))
        return 8;

    auto replacement = *gateway.bound_session_route_async (target).result ().value ();
    ++replacement.binding_generation;
    if (!gateway.record_bound_session_route_transition (target, replacement))
        return 9;
    const auto stale_binding_push =
      (*staged_source_sink) ("StaleBindingPush", stream_codec_t::message_pack,
                             zlink::message_t::from ("stale"))
        .result ();
    if (stale_binding_push || !saw_stale_target->load (std::memory_order_acquire))
        return 10;

    auto fresh_state = std::make_shared<actor_gateway_state_t> ();
    fresh_state->serializers = &serializers;
    actor_gateway_runtime_t fresh_gateway (fresh_state);
    fresh_gateway.on_bound_session_send ([] (const actor_ref_t &, std::uint64_t,
                                             const stream_header_t &,
                                             const zlink::message_t &) -> task_t<result_t<void>> {
        co_return result_t<void>::success ();
    });
    if (!fresh_gateway.prepare_session_relocation_target_route (route, 37))
        return 11;
    const auto fresh_route_is_prewarmed = fresh_state->sync ([&] {
        const auto found = fresh_state->actors_by_id.find ("relocation-prewarm");
        return found != fresh_state->actors_by_id.end () && found->second.bound
               && !found->second.disconnected
               && found->second.ref.node_rid ().value () == target.node_rid ().value ()
               && found->second.bound_session_route
               && found->second.bound_session_route->node_rid.to_bytes ()
                    == session_owner.to_bytes ()
               && found->second.bound_session_route->session_rid == session_rid
               && found->second.bound_session_route->authority_owner_generation == 29
               && found->second.bound_session_route->owner_lease_generation == 37
               && found->second.bound_session_route->session_sequence_baseline_unknown
               && fresh_state->bound_session_sinks.contains ("relocation-prewarm");
    });
    if (!fresh_route_is_prewarmed)
        return 12;
    return 0;
}

int stale_binding_registration_and_relocation_prepare_preserve_current_tenure ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace protocol = zlink::framework::runtime::protocol;

    auto state = std::make_shared<actor_gateway_state_t> ();
    actor_gateway_runtime_t gateway (state);
    const auto actor = test_actor_ref ("source-node", "player", "stale-binding", 7);
    const auto session_owner = zlink::routing_id_t::from (std::string ("session-owner"));
    const auto session_rid = zlink::routing_id_t::from (std::string ("session-rid"));
    if (!gateway.record_bound_session_route (actor, session_owner, session_rid, 11, 17, 19, 24, 0,
                                             1, false, "session-owner-id", 13))
        return 1;

    auto stale = *gateway.bound_session_route_async (actor).result ().value ();
    stale.binding_generation = 23;
    stale.authority_owner_generation = 29;
    stale.owner_lease_generation = 37;
    const auto registered = gateway.record_bound_session_route_transition (actor, stale);
    const protocol::session_relocation_route_t route{
      .sender_role = protocol::relocation_role_t::target,
      .actor = {"stale-binding", 7},
      .session_owner_node_routing_id = session_owner.to_bytes (),
      .session_owner_node_generation = 11,
      .session_owner_id = "session-owner-id",
      .session_owner_lease_generation = 13,
      .session_routing_id = session_rid.to_bytes (),
      .binding_generation = 23,
      .route = {protocol::session_relocation_route_action_t::commit, 17, 29,
                zlink::routing_id_t::from (std::string ("target-node")).to_bytes (), 31, 0, 37}};
    const bool prepared = gateway.prepare_session_relocation_target_route (route, 37);
    const bool unchanged = state->sync ([&] {
        const auto &record = state->actors_by_id.at ("stale-binding");
        const auto &current = *record.bound_session_route;
        return record.ref.node_rid ().value () == "source-node" && current.binding_generation == 24
               && current.authority_owner_generation == 17 && current.owner_lease_generation == 19
               && current.session_sequence == 1
               && current.node_rid.to_bytes () == session_owner.to_bytes ()
               && current.session_rid == session_rid;
    });
    if (registered || !registered.error ()
        || registered.error_kind () != framework_error_kind_t::unavailable || prepared
        || !unchanged) {
        std::cerr << "stale binding: registered=" << static_cast<bool> (registered)
                  << " prepared=" << prepared << " current_unchanged=" << unchanged << '\n';
        return 2;
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

int bound_session_push_detaches_before_direct_sink_entry ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    auto state = std::make_shared<actor_gateway_state_t> ();
    serializer_registry_t serializers;
    state->serializers = &serializers;
    actor_gateway_runtime_t gateway (state);
    const auto actor = test_actor_ref ("actor-owner", "player", "detached-bound-session", 7);
    // The second detached delivery can trace its completion after this test
    // returns, so the observer owns the flags it writes.
    auto saw_fifo_session = std::make_shared<std::atomic_bool> (false);
    auto saw_completion_session = std::make_shared<std::atomic_bool> (false);
    dispatch_options_t trace_options;
    trace_options.message_flow (message_flow_log_mode_t::detailed);
    dispatch_options_access_t::set_observer_for_tests (
      trace_options,
      [saw_fifo_session, saw_completion_session] (const message_flow_event_t &event) {
          if (event.surface != dispatch_error_surface_t::stream_session
              || event.stream_session_id
                   != std::optional<std::string> (
                     zlink::routing_id_t::from ("session-rid").to_hex ()))
              return;
          if (event.detail_stage == std::optional<std::string> ("fifo_accepted"))
              saw_fifo_session->store (true, std::memory_order_release);
          if (event.detail_stage == std::optional<std::string> ("detached_delivery_complete"))
              saw_completion_session->store (true, std::memory_order_release);
      });
    gateway.set_dispatch (std::move (trace_options));

    // The detached delivery can run after this scope ends, so the sink owns its gate.
    struct sender_gate_t
    {
        std::mutex mutex;
        std::condition_variable changed;
        int started = 0;
        bool release = false;
    };
    auto gate = std::make_shared<sender_gate_t> ();
    auto direct_stream_sink = [gate] (std::string, stream_codec_t,
                                      const zlink::message_t &) -> task_t<void> {
        const auto ordinal = [&] {
            std::unique_lock lock (gate->mutex);
            const auto value = ++gate->started;
            gate->changed.notify_all ();
            if (value == 1)
                gate->changed.wait (lock, [&] { return gate->release; });
            return value;
        }();
        if (ordinal < 1)
            throw framework_exception_t (framework_error_kind_t::internal_failure,
                                         "invalid detached sink order");
        co_return;
    };
    const auto installed = gateway.replace_session_route (
      actor, std::move (direct_stream_sink),
      actor_bound_session_route_t{zlink::routing_id_t::from ("session-owner"),
                                  zlink::routing_id_t::from ("session-rid"), 7, 11, 13, 17, 19, 23,
                                  0});
    if (!installed) {
        return 1;
    }

    auto submitted = std::async (std::launch::async, [&] {
        return gateway.actor_context (actor)
          .bound_session ()
          .send (std::string ("joined"))
          .async ()
          .result ();
    });
    const auto immediate = submitted.wait_for (std::chrono::milliseconds (100));
    auto second =
      gateway.actor_context (actor).bound_session ().send (std::string ("joined-again")).async ();
    {
        std::unique_lock lock (gate->mutex);
        if (!gate->changed.wait_for (lock, std::chrono::seconds (1),
                                     [&] { return gate->started == 1; })) {
            gate->release = true;
            gate->changed.notify_all ();
            return 2;
        }
        if (gate->started != 1 || !second.await_ready ()) {
            gate->release = true;
            gate->changed.notify_all ();
            return 3;
        }
        gate->release = true;
    }
    gate->changed.notify_all ();
    if (immediate != std::future_status::ready)
        return 4;
    if (!submitted.get () || !second.result ())
        return 5;
    {
        std::unique_lock lock (gate->mutex);
        if (!gate->changed.wait_for (lock, std::chrono::seconds (1),
                                     [&] { return gate->started == 2; })) {
            return 6;
        }
    }
    if (!saw_fifo_session->load (std::memory_order_acquire)
        || !saw_completion_session->load (std::memory_order_acquire))
        return 8;
    return 0;
}

int admitted_bound_session_delivery_keeps_original_session_after_rebind ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    actor_gateway_runtime_t gateway;
    const auto actor = test_actor_ref ("actor-owner", "player", "admitted-rebind", 7);
    const auto owner = zlink::routing_id_t::from ("session-owner");
    const auto original_rid = zlink::routing_id_t::from ("original-session");
    const auto replacement_rid = zlink::routing_id_t::from ("replacement-session");
    std::vector<message_flow_event_t> events;
    auto live_mode =
      std::make_shared<std::atomic<message_flow_log_mode_t>> (message_flow_log_mode_t::detailed);
    dispatch_options_t trace_options;
    trace_options.message_flow (message_flow_log_mode_t::detailed);
    dispatch_options_access_t::set_live_mode (trace_options, live_mode);
    dispatch_options_access_t::set_observer_for_tests (
      trace_options, [&] (const message_flow_event_t &event) { events.push_back (event); });
    gateway.set_dispatch (std::move (trace_options));

    const auto original_route =
      actor_bound_session_route_t{owner, original_rid, 7, 11, 13, 17, 19, 0, 0};
    auto replacement_route =
      actor_bound_session_route_t{owner, replacement_rid, 7, 11, 13, 17, 23, 0, 0};
    if (!gateway.replace_session_route (
          actor,
          [] (std::string, stream_codec_t, const zlink::message_t &) {
              return task_t<void> (result_t<void>::success ());
          },
          original_route))
        return 1;
    auto admitted = gateway.admit_bound_session_delivery (actor, 19);
    if (!admitted)
        return 2;
    if (!gateway.replace_session_route (
          actor,
          [] (std::string, stream_codec_t, const zlink::message_t &) {
              return task_t<void> (result_t<void>::success ());
          },
          replacement_route))
        return 3;
    if (!(*admitted) ("push", stream_codec_t::message_pack, zlink::message_t::from ("body")))
        return 4;
    const auto original_hex = original_rid.to_hex ();
    const auto completion = std::find_if (events.begin (), events.end (), [&] (const auto &event) {
        return event.detail_stage
                 == std::optional<std::string> ("session_node_stream_write_terminal")
               && event.detail_result == std::optional<std::string> ("ok");
    });
    if (completion == events.end () || completion->stream_session_id != original_hex)
        return 5;

    replacement_route.session_rid = zlink::routing_id_t::from ("failed-session");
    replacement_route.binding_generation = 29;
    if (!gateway.replace_session_route (
          actor,
          [] (std::string, stream_codec_t, const zlink::message_t &) {
              return task_t<void> (
                result_t<void>::failure (framework_error_kind_t::unavailable, "sink failure"));
          },
          replacement_route))
        return 6;
    admitted = gateway.admit_bound_session_delivery (actor, 29);
    if (!admitted)
        return 7;
    replacement_route.session_rid = zlink::routing_id_t::from ("later-session");
    replacement_route.binding_generation = 31;
    if (!gateway.replace_session_route (
          actor,
          [] (std::string, stream_codec_t, const zlink::message_t &) {
              return task_t<void> (result_t<void>::success ());
          },
          replacement_route))
        return 8;
    if ((*admitted) ("push", stream_codec_t::message_pack, zlink::message_t::from ("body")))
        return 9;
    const auto failed_hex = zlink::routing_id_t::from ("failed-session").to_hex ();
    const auto failure = std::find_if (events.begin (), events.end (), [&] (const auto &event) {
        return event.detail_stage == std::optional<std::string> ("detached_delivery")
               && event.detail_result
               && event.detail_result->starts_with ("session_node_stream_write_terminal failed")
               && event.outcome == message_flow_outcome_t::dropped;
    });
    if (failure == events.end () || failure->stream_session_id != failed_hex)
        return 10;
    const auto failure_stage =
      std::find_if (events.begin (), events.end (), [&] (const auto &event) {
          return event.detail_stage
                   == std::optional<std::string> ("session_node_stream_write_terminal")
                 && event.outcome == message_flow_outcome_t::admitted && event.detail_result
                 && event.detail_result->starts_with ("failed");
      });
    if (failure_stage == events.end () || failure_stage->stream_session_id != failed_hex)
        return 11;

    live_mode->store (message_flow_log_mode_t::off, std::memory_order_relaxed);
    const auto event_count_before_off = events.size ();
    admitted = gateway.admit_bound_session_delivery (actor, 31);
    if (!admitted || events.size () != event_count_before_off)
        return 12;
    replacement_route.session_rid = zlink::routing_id_t::from ("newest-session");
    replacement_route.binding_generation = 37;
    if (!gateway.replace_session_route (
          actor,
          [] (std::string, stream_codec_t, const zlink::message_t &) {
              return task_t<void> (result_t<void>::success ());
          },
          replacement_route))
        return 13;
    live_mode->store (message_flow_log_mode_t::detailed, std::memory_order_relaxed);
    if (!(*admitted) ("push", stream_codec_t::message_pack, zlink::message_t::from ("body")))
        return 14;
    const auto switched_completion = std::find_if (
      events.begin () + static_cast<std::ptrdiff_t> (event_count_before_off), events.end (),
      [&] (const auto &event) {
          return event.detail_stage
                   == std::optional<std::string> ("session_node_stream_write_terminal")
                 && event.detail_result == std::optional<std::string> ("ok");
      });
    return switched_completion != events.end ()
               && switched_completion->stream_session_id
                    == zlink::routing_id_t::from ("later-session").to_hex ()
             ? 0
             : 15;
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

int bound_session_relay_admission_is_exact_and_monotonic ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    auto state = std::make_shared<actor_gateway_state_t> ();
    actor_gateway_runtime_t gateway (state);
    const auto actor = test_actor_ref ("actor-owner", "game.actor", "relay-actor", 3);
    if (!gateway.bind_session_sink (actor,
                                    [] (std::string, stream_codec_t, const zlink::message_t &) {
                                        return task_t<void> (result_t<void>::success ());
                                    })) {
        return 1;
    }
    const auto session_owner = zlink::routing_id_t::from (std::string ("session-owner"));
    const auto session_rid = zlink::routing_id_t::from (std::string ("session-rid"));
    if (!gateway.record_bound_session_route (actor, session_owner, session_rid, 7, 11, 13, 17, 0,
                                             0)) {
        return 2;
    }
    if (!gateway.admit_session_relay (actor, session_owner, session_rid, 17, 1))
        return 4;
    if (gateway.admit_session_relay (actor, session_owner, session_rid, 17, 1)
        || gateway.admit_session_relay (
          actor, zlink::routing_id_t::from (std::string ("other-owner")), session_rid, 17, 2)) {
        return 7;
    }
    if (!gateway.admit_session_relay (actor, session_owner, session_rid, 17, 2))
        return 9;
    const auto route = gateway.bound_session_route_async (actor).result ().value ();
    if (!route || route->session_sequence != 2)
        return 11;
    if (!gateway.admit_session_relay (actor, session_owner, session_rid, 17, 4)
        || gateway.admit_session_relay (actor, session_owner, session_rid, 17, 3)
        || gateway.admit_session_relay (actor, session_owner, session_rid, 17, 4))
        return 22;
    const auto after_gap = gateway.bound_session_route_async (actor).result ().value ();
    if (!after_gap || after_gap->session_sequence != 4)
        return 23;

    namespace stateful = runtime::stateful;
    const stateful::object_ref_t owner{
      stateful::object_kind_t::actor, "relay-actor", 3, 11, "relay-mesh", "actor-owner"};
    auto other_owner = owner;
    other_owner.key = "other-actor";
    stateful::stream_session_registry_t sessions (
      [&] (const std::string &key) -> std::optional<stateful::object_ref_t> {
          if (key == owner.key)
              return owner;
          if (key == other_owner.key)
              return other_owner;
          return std::nullopt;
      });
    const auto connection = sessions.open (session_rid.to_hex (), {}, session_owner, 7);
    const auto [binding_error, binding] = sessions.bind (connection, owner, 7, 13);
    if (binding_error != stateful::stateful_error_t::none)
        return 12;
    const auto [first_error, first] = sessions.admit_inbound (binding);
    if (first_error != stateful::stateful_error_t::none || !first || first->inbound_sequence != 1)
        return 13;
    if (sessions.complete_inbound (*first) != stateful::stateful_error_t::none
        || sessions.complete_inbound (*first) == stateful::stateful_error_t::none)
        return 14;
    const auto [second_error, second] = sessions.admit_inbound (binding);
    if (second_error != stateful::stateful_error_t::none || !second
        || second->inbound_sequence != 2)
        return 15;
    auto wrong_binding = *second;
    ++wrong_binding.binding.binding_generation;
    auto wrong_sequence = *second;
    wrong_sequence.inbound_sequence += 2;
    if (sessions.complete_inbound (wrong_binding) == stateful::stateful_error_t::none
        || sessions.complete_inbound (wrong_sequence) == stateful::stateful_error_t::none
        || sessions.complete_inbound (*second) != stateful::stateful_error_t::none)
        return 16;

    const auto [third_error, third] = sessions.admit_inbound (binding);
    const auto other_connection = sessions.open ("other-session", {}, session_owner, 7);
    const auto [other_binding_error, other_binding] =
      sessions.bind (other_connection, other_owner, 7, 13);
    const auto [other_error, other] = sessions.admit_inbound (other_binding);
    if (third_error != stateful::stateful_error_t::none || !third || third->inbound_sequence != 3
        || other_binding_error != stateful::stateful_error_t::none
        || other_error != stateful::stateful_error_t::none || !other)
        return 17;
    auto unaccepted_other_token = *third;
    unaccepted_other_token.drain = other->drain;
    if (sessions.complete_inbound (unaccepted_other_token) == stateful::stateful_error_t::none
        || !gateway.destroy_actor (actor)
        || sessions.try_seal_actor (owner).first != stateful::stateful_error_t::backpressured
        || sessions.try_seal_actor (other_owner).first != stateful::stateful_error_t::backpressured)
        return 18;
    if (sessions.complete_inbound (*third) != stateful::stateful_error_t::none
        || sessions.complete_inbound (*third) == stateful::stateful_error_t::none
        || gateway.admit_session_relay (actor, session_owner, session_rid, 17, 3)
        || sessions.complete_inbound (*other) != stateful::stateful_error_t::none)
        return 19;
    const auto [seal_error, seal] = sessions.try_seal_actor (owner);
    if (seal_error != stateful::stateful_error_t::none
        || sessions.abort_barrier (seal) != stateful::stateful_error_t::none)
        return 20;

    // A later accepted ingress does not invalidate an earlier completion token.
    const auto [fourth_error, fourth] = sessions.admit_inbound (binding);
    const auto [fifth_error, fifth] = sessions.admit_inbound (binding);
    if (fourth_error != stateful::stateful_error_t::none || !fourth
        || fifth_error != stateful::stateful_error_t::none || !fifth
        || fourth->inbound_sequence != 4 || fifth->inbound_sequence != 5
        || sessions.complete_inbound (*fourth) != stateful::stateful_error_t::none
        || sessions.complete_inbound (*fifth) != stateful::stateful_error_t::none
        || sessions.complete_inbound (*fourth) == stateful::stateful_error_t::none)
        return 21;
    return 0;
}

int bound_session_actor_dispatch_uses_current_binding_without_location_reread ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace host = zlink::framework::runtime::host;
    namespace messaging = zlink::framework::runtime::messaging;
    namespace protocol = zlink::framework::runtime::protocol;

    serializer_registry_t serializers;
    auto node = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::spot_node_builder_state_t> ("bound-session-target");
    node->worker_executor =
      std::make_shared<runtime::offload_executor_t> (1, "bound-session-target");
    node->channel_runtime =
      zlink::framework::test::runtime_failure_fixture<channel_runtime_state_t> ();
    node->channel_runtime->serializers = &serializers;

    const auto native = std::make_shared<host::public_host_runtime_t> (host::host_options_t{
      .mesh = {.descriptor =
                 {.mesh_name = "bound-session-dispatch",
                  .node_routing_id =
                    zlink::routing_id_t::from (std::string ("bound-session-target")).to_bytes (),
                  .lifecycle_generation = 7,
                  .descriptor_revision = 1,
                  .advertised_endpoint = "tcp://127.0.0.1:0"},
               .runtime_failures =
                 std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()}});
    node->native_node = native;

    auto spot = std::make_shared<spot_context_state_t> ();
    spot->node = node;
    spot->node_rid = node_rid_t::from_string ("bound-session-target");
    spot->spot_id = spot_id_t ("bound-session-spot");
    spot->spot_name = "bound-session-spot";
    spot->spot_instance = std::make_shared<int> (1);
    spot->channel_runtime =
      zlink::framework::test::runtime_failure_fixture<channel_runtime_state_t> ();
    spot->channel_runtime->serializers = &serializers;
    spot->serial_executor = node->worker_executor;
    node->spot_contexts_by_id.emplace (spot->spot_id, spot_context_access_t::create (spot));

    spot_node_builder_state_t::actor_factory_registration_t factory;
    factory.actor_type = std::type_index (typeid (int));
    factory.create_instance = [] (std::string) { return std::make_shared<int> (7); };
    factory.configure_instance = [] (void *, const actor_ref_t &, void *) {};
    node->actor_factories.emplace ("player", std::move (factory));

    const auto actor = test_actor_ref ("bound-session-target", "player", "bound-player", 3);
    const auto actor_key = std::string ("player:bound-player");
    node->actor_instances.emplace (actor_key, std::make_shared<int> (7));
    node->actor_spot_ids.emplace (actor_key, spot->spot_id);
    node->actor_generations.emplace (actor_key, 3);
    node->actor_types_by_id.emplace ("bound-player", "player");

    std::atomic_int handler_calls{0};
    spot->handlers.push_back (
      spot_handler_descriptor_t{spot_handler_kind_t::actor_send, "MoveMsg", "",
                                std::type_index (typeid (int)), std::type_index (typeid (void)),
                                std::type_index (typeid (int)), std::type_index (typeid (void))});
    spot->handler_invokers.push_back (
      [&handler_calls] (void *, void *, service_provider_t &, serializer_registry_t &,
                        const zlink::message_t &,
                        const spot_inbound_message_t &) -> task_t<zlink::message_t> {
          handler_calls.fetch_add (1, std::memory_order_acq_rel);
          co_return zlink::message_t{};
      });

    service_collection_t services;
    services.add_singleton<actor_gateway_runtime_t> ();
    auto provider = services.build_provider ();
    auto &gateway = provider.get_required<actor_gateway_runtime_t> ();
    const auto session_owner = zlink::routing_id_t::from (std::string ("session-owner"));
    const auto session_rid = zlink::routing_id_t::from (std::string ("session-rid"));
    if (!gateway.record_bound_session_route (actor, session_owner, session_rid,
                                             /*session owner generation=*/11,
                                             /*authority owner generation=*/13,
                                             /*target owner lease generation=*/17,
                                             /*binding generation=*/19, 0, 0)) {
        return 1;
    }

    std::atomic_int location_reads{0};
    node->actor_route_admission = [&location_reads] (const protocol::actor_route_fence_t &) {
        location_reads.fetch_add (1, std::memory_order_acq_rel);
        return false;
    };

    const auto exact_target =
      protocol::actor_route_fence_t{"bound-player",
                                    3,
                                    native->status ().routing_id ().to_bytes (),
                                    native->status ().lifecycle_generation (),
                                    13,
                                    17};
    const host::ready_record_t owner{.owner_kind = host::owner_kind_t::actor,
                                     .domain = host::ready_domain_t::application,
                                     .spot_id = std::string (spot->spot_id),
                                     .actor = actor};
    spot_node_runtime_t spots (node);

    const auto dispatch = [&] (protocol::actor_route_fence_t target, std::uint64_t sequence) {
        host::receive_record_t record{.kind = host::record_kind_t::actor_send,
                                      .domain = host::ready_domain_t::application};
        record.source_node_rid = session_owner;
        record.source_session_rid = session_rid;
        record.source_binding_generation = 19;
        record.source_session_sequence = sequence;
        record.actor_route = std::move (target);
        auto encoded = zlink::framework::detail::encode_actor_packet (
          messaging::envelope_header_t{.kind = messaging::message_kind_t::command,
                                       .channel_name = "actor",
                                       .message_name = "MoveMsg"},
          zlink::message_t::from (std::string ("move")));
        auto parts = std::move (encoded).take_items ();
        return spots.dispatch_mesh_record (
          owner, record, parts,
          zlink::framework::runtime::decode_mesh_record_header (record, parts, false).value (),
          provider, serializers);
    };
    const auto wait_for_calls = [&] (int expected) {
        const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (1);
        while (handler_calls.load (std::memory_order_acquire) != expected
               && std::chrono::steady_clock::now () < deadline) {
            std::this_thread::yield ();
        }
        return handler_calls.load (std::memory_order_acquire) == expected;
    };

    (void) dispatch (exact_target, 1);
    if (!wait_for_calls (1)) {
        node->worker_executor->drain ();
        node->worker_executor.reset ();
        return 5;
    }
    if (location_reads.load (std::memory_order_acquire) != 0) {
        node->worker_executor->drain ();
        node->worker_executor.reset ();
        return 6;
    }

    auto stale_target = exact_target;
    ++stale_target.authority_owner_generation;
    (void) dispatch (std::move (stale_target), 2);
    if (!wait_for_calls (2) || location_reads.load (std::memory_order_acquire) != 0) {
        node->worker_executor->drain ();
        node->worker_executor.reset ();
        return 4;
    }
    (void) dispatch (exact_target, 3);
    if (!wait_for_calls (3)) {
        node->worker_executor->drain ();
        node->worker_executor.reset ();
        return 7;
    }
    node->worker_executor->drain ();
    node->worker_executor.reset ();
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
    auto bound_a = manager.bind (actor_a).async ().result ().value ();
    auto bound_b = manager.bind (actor_b).async ().result ().value ();
    const auto session_rid = zlink::routing_id_t::from (std::string ("relay-session"));
    if (!gateway.record_session_relay_source (actor_a, session_rid, 31)
        || !gateway.record_session_relay_source (actor_b, session_rid, 37)) {
        return 1;
    }

    std::mutex mutex;
    std::condition_variable changed;
    auto first_completion =
      std::make_shared<task_completion_source_t<std::optional<zlink::message_t>>> ();
    std::vector<std::string> started;
    std::vector<std::pair<std::string, std::uint64_t>> sources;
    gateway.on_relay ([&] (const actor_ref_t &actor, actor_context_t, const stream_header_t &header,
                           const zlink::message_t &,
                           std::optional<bound_session_relay_source_t> source,
                           std::chrono::milliseconds) {
        const auto marker = std::string (header.packet_name ());
        {
            const std::lock_guard lock (mutex);
            started.push_back (marker);
            if (source)
                sources.emplace_back (marker, source->session_sequence);
        }
        changed.notify_all ();
        if (actor.actor_id ().value () == "relay-a" && marker == "A1") {
            return test::accepted_request_task (first_completion->task ());
        }
        return request_task_t<std::optional<zlink::message_t>> (
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
    first_completion->complete (result_t<std::optional<zlink::message_t>>::success (std::nullopt));
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

int session_relay_does_not_start_actor_dispatch_on_session_thread ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    actor_gateway_runtime_t gateway;
    gateway.offload_session_relay ();
    auto manager = gateway.manager ();
    session_actor_manager_access_t::attach (manager, stream_t{});
    const auto actor = test_actor_ref ("actor-owner", "game.actor", "relay-execution", 1);
    auto binding = manager.bind (actor).async ().result ().value ();
    const auto session_thread = std::this_thread::get_id ();
    std::thread::id actor_dispatch_thread;
    gateway.on_relay ([&] (const actor_ref_t &, actor_context_t, const stream_header_t &,
                           const zlink::message_t &, std::optional<bound_session_relay_source_t>,
                           std::chrono::milliseconds) {
        actor_dispatch_thread = std::this_thread::get_id ();
        return request_task_t<std::optional<zlink::message_t>> (
          result_t<std::optional<zlink::message_t>>::success (std::nullopt));
    });

    const auto delivered = binding.relay ("execution", zlink::message_t{}).result ();
    return !delivered || actor_dispatch_thread == std::thread::id{}
               || actor_dispatch_thread == session_thread
             ? 1
             : 0;
}

zlink::framework::task_t<std::optional<zlink::message_t>> inspect_pending_relay_arguments (
  const zlink::framework::actor_ref_t &actor,
  const zlink::framework::detail::stream_header_t &header,
  const zlink::message_t &payload,
  const std::shared_ptr<zlink::framework::task_completion_source_t<void>> &pending,
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
        throw std::runtime_error ("pending actor relay did not retain its request arguments");
    }
    co_return zlink::message_t::from ("delayed-reply");
}

zlink::framework::task_t<void> inspect_pending_disconnect_argument (
  const zlink::framework::actor_ref_t &actor,
  const std::shared_ptr<zlink::framework::task_completion_source_t<void>> &pending,
  const std::shared_ptr<std::atomic_bool> &started,
  std::string expected_actor_id)
{
    started->store (true, std::memory_order_release);
    co_await pending->task ();
    if (actor.actor_id ().value () != expected_actor_id) {
        throw std::runtime_error ("pending actor disconnect did not retain its actor argument");
    }
    co_return;
}

zlink::framework::task_t<void> inspect_pending_manager_disconnect_argument (
  const zlink::framework::actor_ref_t &actor,
  const std::shared_ptr<zlink::framework::task_completion_source_t<void>> &pending,
  const std::shared_ptr<std::atomic_bool> &started,
  const std::shared_ptr<std::promise<bool>> &observed,
  std::string expected_actor_id)
{
    started->store (true, std::memory_order_release);
    co_await pending->task ();
    bool retained = false;
    try {
        retained = actor.actor_id ().value () == expected_actor_id;
    }
    catch (const std::exception &) {
    }
    observed->set_value (retained);
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
    const auto actor = test_actor_ref ("actor-owner", "game.actor", actor_id, 1);
    auto bound = manager.bind (actor).async ().result ().value ();
    auto pending = std::make_shared<task_completion_source_t<void>> ();
    auto disconnect_started = std::make_shared<std::atomic_bool> (false);
    gateway.on_disconnect ([pending, disconnect_started,
                            actor_id] (const actor_ref_t &disconnected_actor,
                                       const std::optional<runtime::stateful::stream_binding_t> &) {
        return inspect_pending_disconnect_argument (disconnected_actor, pending, disconnect_started,
                                                    actor_id);
    });

    auto notification = bound.notify_disconnected ();
    if (!disconnect_started->load (std::memory_order_acquire))
        return 1;

    std::vector<std::string> reclaimed_storage;
    reclaimed_storage.reserve (8192);
    for (std::size_t index = 0; index < 8192; ++index) {
        reclaimed_storage.emplace_back (96, static_cast<char> ('A' + (index % 26)));
    }
    std::thread completion ([pending] { pending->complete (result_t<void>::success ()); });
    completion.join ();
    if (!notification.result ())
        return 2;

    return state->sync ([&] {
        const auto found = state->actors_by_id.find (actor_id);
        return found != state->actors_by_id.end () && found->second.binding_token == 0
                   && found->second.binding_session_id.empty ()
                 ? 0
                 : 3;
    });
}

int session_manager_disconnect_survives_pending_dispatcher_completion ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    actor_gateway_runtime_t gateway;
    auto manager = gateway.manager ();
    session_actor_manager_access_t::attach (manager, stream_t{});
    const std::string actor_id (96, 'm');
    const auto actor = test_actor_ref ("actor-owner", "game.actor", actor_id, 1);
    (void) manager.bind (actor).async ().result ().value ();
    auto pending = std::make_shared<task_completion_source_t<void>> ();
    auto disconnect_started = std::make_shared<std::atomic_bool> (false);
    auto observed = std::make_shared<std::promise<bool>> ();
    auto actor_retained = observed->get_future ();
    gateway.on_disconnect ([pending, disconnect_started, observed,
                            actor_id] (const actor_ref_t &disconnected_actor,
                                       const std::optional<runtime::stateful::stream_binding_t> &) {
        return inspect_pending_manager_disconnect_argument (disconnected_actor, pending,
                                                            disconnect_started, observed, actor_id);
    });

    session_actor_manager_access_t::disconnect (manager);
    if (!disconnect_started->load (std::memory_order_acquire))
        return 1;

    std::vector<std::string> reclaimed_storage;
    reclaimed_storage.reserve (8192);
    for (std::size_t index = 0; index < 8192; ++index) {
        reclaimed_storage.emplace_back (96, static_cast<char> ('A' + (index % 26)));
    }
    std::thread completion ([pending] { pending->complete (result_t<void>::success ()); });
    completion.join ();
    actor_retained.wait ();
    return actor_retained.get () ? 0 : 2;
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
    const auto actor = test_actor_ref ("actor-owner", "game.actor", actor_id, 1);
    auto bound = manager.bind (actor).async ().result ().value ();
    auto pending = std::make_shared<task_completion_source_t<void>> ();
    auto relay_started = std::make_shared<std::atomic_bool> (false);
    gateway.on_relay ([pending, relay_started, actor_id, packet_name, payload_text] (
                        const actor_ref_t &relayed_actor, actor_context_t,
                        const stream_header_t &header, const zlink::message_t &payload,
                        std::optional<bound_session_relay_source_t>, std::chrono::milliseconds) {
        return test::accepted_request_task (
          inspect_pending_relay_arguments (relayed_actor, header, payload, pending, relay_started,
                                           actor_id, packet_name, payload_text));
    });

    auto request =
      bound.relay_request (packet_name, zlink::message_t::from (payload_text)).async ();
    auto admitted = request.admitted ();
    if (!admitted.await_ready () || !admitted.result () || request.await_ready ()
        || !relay_started->load (std::memory_order_acquire))
        return 1;

    std::vector<std::string> reclaimed_storage;
    reclaimed_storage.reserve (8192);
    for (std::size_t index = 0; index < 8192; ++index) {
        reclaimed_storage.emplace_back (4096, static_cast<char> ('A' + (index % 26)));
    }
    std::thread completion ([pending] { pending->complete (result_t<void>::success ()); });
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
    const auto actor = test_actor_ref ("actor-owner", "game.actor", actor_id, 1);
    auto bound = manager.bind (actor).async ().result ().value ();
    auto pending = std::make_shared<task_completion_source_t<void>> ();
    auto relay_started = std::make_shared<std::atomic_bool> (false);
    gateway.on_relay ([pending, relay_started, actor_id, packet_name, payload_text] (
                        const actor_ref_t &relayed_actor, actor_context_t,
                        const stream_header_t &header, const zlink::message_t &payload,
                        std::optional<bound_session_relay_source_t>, std::chrono::milliseconds) {
        return test::accepted_request_task (
          inspect_pending_relay_arguments (relayed_actor, header, payload, pending, relay_started,
                                           actor_id, packet_name, payload_text));
    });

    auto relayed = bound.relay (packet_name, zlink::message_t::from (payload_text));
    if (!relay_started->load (std::memory_order_acquire))
        return 1;

    std::vector<std::string> reclaimed_storage;
    reclaimed_storage.reserve (8192);
    for (std::size_t index = 0; index < 8192; ++index) {
        reclaimed_storage.emplace_back (4096, static_cast<char> ('A' + (index % 26)));
    }
    std::thread completion ([pending] { pending->complete (result_t<void>::success ()); });
    completion.join ();
    return relayed.result () ? 0 : 2;
}

int session_disconnect_callback_owns_spot_lifecycle_gate ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace rt = zlink::framework::runtime;
    for (const bool entry : {false, true}) {
        serializer_registry_t serializers;
        auto node =
          test::runtime_failure_fixture<spot_node_builder_state_t> ("disconnect-gate-node");
        node->worker_executor = std::make_shared<rt::offload_executor_t> (1, "disconnect-gate");
        auto spot = std::make_shared<spot_context_state_t> ();
        spot->node = node;
        spot->node_rid = node_rid_t::from_string ("disconnect-gate-node");
        spot->spot_id = spot_id_t ("disconnect-gate-spot");
        spot->spot_name = "disconnect-gate";
        spot->serial_executor = node->worker_executor;
        spot->execution_mode =
          entry ? user_spot_execution_mode_t::spot_wide : user_spot_execution_mode_t::per_actor;
        spot->lifecycle_domain =
          entry ? spot_lifecycle_domain_t::entry () : spot_lifecycle_domain_t::user ();
        spot->serial_queue = std::make_shared<rt::serial_execution_queue_t> (
          *node->worker_executor, rt::serial_execution_queue_options_t{},
          rt::serial_execution_queue_t::error_handler_t{},
          entry ? rt::serial_lane_policy_t::entry_spot ()
                : rt::serial_lane_policy_t::per_actor_spot ());
        node->spot_contexts_by_id.emplace (spot->spot_id, spot_context_access_t::create (spot));
        node->spot_ids_by_name.emplace (spot->spot_name, spot->spot_id);
        if (entry)
            node->snapshot.entry_spot_name = spot->spot_name;
        std::atomic_int callbacks{0};
        std::atomic_bool owned_gate{false};
        std::atomic_bool owned_actor_context{false};
        spot_actor_admission_callbacks_t admission;
        admission.on_disconnect_actor = [&] (void *, void *) -> task_t<void> {
            const auto turn = capture_current_serial_turn ();
            owned_gate.store (turn && turn->belongs_to (spot->serial_queue.get ()),
                              std::memory_order_release);
            owned_actor_context.store (rt::current_actor_execution.actor_key == "player:gate-player"
                                         && rt::current_actor_execution.spot_id
                                              == std::string (spot->spot_id),
                                       std::memory_order_release);
            callbacks.fetch_add (1, std::memory_order_acq_rel);
            co_return;
        };
        test::register_session_disconnect_probe (spot, std::move (admission.on_disconnect_actor));
        spot_node_builder_state_t::actor_factory_registration_t factory;
        factory.actor_type = std::type_index (typeid (int));
        factory.create_instance = [] (std::string) { return std::make_shared<int> (7); };
        factory.configure_instance = [] (void *, const actor_ref_t &, void *) {};
        node->actor_factories.emplace ("player", std::move (factory));
        const auto actor = test_actor_ref ("disconnect-gate-node", "player", "gate-player", 1);
        node->actor_instances.emplace ("player:gate-player", std::make_shared<int> (7));
        node->actor_spot_ids.emplace ("player:gate-player", spot->spot_id);
        node->actor_generations.emplace ("player:gate-player", 1);
        record_actor_instance_index_unlocked (
          *node, actor, node->actor_instances.at ("player:gate-player").get ());
        service_collection_t services;
        services.add_singleton<actor_gateway_runtime_t> ();
        auto provider = services.build_provider ();
        auto &gateway = provider.get_required<actor_gateway_runtime_t> ();
        auto release = std::make_shared<task_completion_source_t<void>> ();
        const auto coordinator = spot->ensure_spot_serial_executor ();
        if (!coordinator || coordinator->uses_spot_execution_gate ())
            return 2;
        std::promise<bool> holding;
        auto hold_signalled = std::make_shared<std::atomic_bool> (false);
        auto held_started = holding.get_future ();
        auto held =
          spot->run_serial_task ("hold-disconnect-spot-lifecycle", [&] () -> task_t<void> {
              if (!hold_signalled->exchange (true))
                  holding.set_value (true);
              co_await release->task ();
          });
        // The observer may run after held.result () returns and this scope
        // ends, so it owns the flag and touches the promise only before then.
        observe_task_completion (held, [&holding, hold_signalled] (const auto &) {
            if (!hold_signalled->exchange (true))
                holding.set_value (false);
        });
        if (!held_started.get ()) {
            release->complete (result_t<void>::success ());
            (void) held.result ();
            return 2;
        }
        std::promise<bool> admitted;
        auto accepted = admitted.get_future ();
        auto signalled = std::make_shared<std::atomic_bool> (false);
        auto notification = spot_node_runtime_t (node).relay_actor_packet (
          actor, gateway.actor_context (actor), stream_message_kind_t::request,
          rt::protocol::session_disconnected_packet_name,
          zlink::message_t::from (rt::protocol::encode_session_disconnected (
            {zlink::routing_id_t::from ("session-owner").to_bytes (), 2, 7})),
          provider, serializers, {}, nullptr, {}, [&] {
              if (!signalled->exchange (true))
                  admitted.set_value (true);
          });
        observe_task_completion (notification, [&admitted, signalled] (const auto &) {
            if (!signalled->exchange (true))
                admitted.set_value (false);
        });
        const bool entered_actor = accepted.get ();
        const bool ran_while_held =
          callbacks.load (std::memory_order_acquire) != 0 || notification.await_ready ();
        release->complete (result_t<void>::success ());
        const auto held_result = held.result ();
        const auto notified = notification.result ();
        const bool owns_gate = owned_gate.load (std::memory_order_acquire);
        const bool owns_actor_context = owned_actor_context.load (std::memory_order_acquire);
        std::cerr << "Session disconnect lifecycle gate entry=" << entry
                  << " admitted=" << entered_actor << " ran_while_held=" << ran_while_held
                  << " owns_spot_turn=" << owns_gate << " owns_actor_context=" << owns_actor_context
                  << std::endl;
        spot->serial_queue->drain ();
        node->worker_executor->drain ();
        if (!held_result || !notified || !notified.value () || !notified.value ()->is_empty ()
            || !entered_actor || ran_while_held || !owns_gate || !owns_actor_context
            || callbacks != 1)
            return 1;
    }
    return 0;
}

int session_disconnect_resolves_context_without_ingress_metadata (bool replay)
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace rt = zlink::framework::runtime;
    serializer_registry_t serializers;
    service_collection_t services;
    services.add_singleton<actor_gateway_runtime_t> ();
    auto provider = services.build_provider ();
    auto &gateway = provider.get_required<actor_gateway_runtime_t> ();
    auto node =
      test::runtime_failure_fixture<spot_node_builder_state_t> ("disconnect-context-node");
    node->worker_executor = std::make_shared<rt::offload_executor_t> (1, "disconnect-context");
    auto spot = std::make_shared<spot_context_state_t> ();
    spot->node = node;
    spot->node_rid = node_rid_t::from_string ("disconnect-context-node");
    spot->spot_id = spot_id_t ("disconnect-context-spot");
    spot->spot_name = "disconnect-context";
    spot->serial_executor = node->worker_executor;
    spot->channel_runtime = test::runtime_failure_fixture<channel_runtime_state_t> ();
    spot->channel_runtime->serializers = &serializers;
    auto release = std::make_shared<task_completion_source_t<void>> ();
    auto entered = std::make_shared<std::promise<bool>> ();
    auto callback_started = entered->get_future ();
    auto entered_once = std::make_shared<std::atomic_bool> (false);
    auto callbacks = std::make_shared<std::atomic_int> (0);
    test::register_session_disconnect_probe (
      spot, [callbacks, entered_once, entered, release] (void *, void *) -> task_t<void> {
          callbacks->fetch_add (1, std::memory_order_acq_rel);
          if (!entered_once->exchange (true))
              entered->set_value (true);
          co_await release->task ();
      });
    node->spot_contexts_by_id.emplace (spot->spot_id, spot_context_access_t::create (spot));
    spot_node_builder_state_t::actor_factory_registration_t factory;
    factory.actor_type = std::type_index (typeid (int));
    factory.create_instance = [] (std::string) { return std::make_shared<int> (7); };
    factory.configure_instance = [] (void *, const actor_ref_t &, void *) {};
    node->actor_factories.emplace ("player", std::move (factory));
    const auto actor = test_actor_ref ("disconnect-context-node", "player", "context-player", 1);
    const auto instance = std::make_shared<int> (7);
    node->actor_instances.emplace ("player:context-player", instance);
    node->actor_spot_ids.emplace ("player:context-player", spot->spot_id);
    node->actor_generations.emplace ("player:context-player", 1);
    node->actor_types_by_id.emplace ("context-player", "player");
    record_actor_instance_index_unlocked (*node, actor, instance.get ());
    (void) gateway.actor_context (actor);
    if (!gateway.record_bound_session_route (actor, zlink::routing_id_t::from ("session-owner"),
                                             zlink::routing_id_t::from ("session"), 2, 1, 1, 7))
        return 1;
    const auto body = zlink::message_t::from (rt::protocol::encode_session_disconnected (
      {zlink::routing_id_t::from ("session-owner").to_bytes (), 2, 7}));
    spot_node_runtime_t spots (node);
    spots.bind_service_provider (provider);
    if (!replay) {
        release->complete (result_t<void>::success ());
        auto typed_spot =
          std::static_pointer_cast<test::session_disconnect_probe_spot_t> (spot->spot_instance);
        std::cerr << "Session disconnect public invocation without ingress metadata starts"
                  << std::endl;
        const auto delivered =
          spot_context_access_t::create (spot).handlers ().invoke_actor_packet (
            rt::protocol::session_disconnected_packet_name, *typed_spot, *instance, provider,
            serializers, body);
        const auto mismatched =
          spot_context_access_t::create (spot).handlers ().invoke_actor_packet (
            rt::protocol::session_disconnected_packet_name, *typed_spot, *instance, provider,
            serializers,
            zlink::message_t::from (rt::protocol::encode_session_disconnected (
              {zlink::routing_id_t::from ("session-owner").to_bytes (), 2, 8})));
        int unregistered = 9;
        const auto missing = spot_context_access_t::create (spot).handlers ().invoke_actor_packet (
          rt::protocol::session_disconnected_packet_name, *typed_spot, unregistered, provider,
          serializers, body);
        node->worker_executor->drain ();
        return delivered && delivered.value ().is_empty () && mismatched
                   && mismatched.value ().is_empty () && callbacks->load () == 1 && !missing
                   && missing.error_kind () == framework_error_kind_t::not_found
                 ? 0
                 : 2;
    }

    auto terminal_source = std::make_shared<task_completion_source_t<zlink::message_t>> ();
    auto terminal = terminal_source->task ();
    auto terminals = std::make_shared<std::atomic_int> (0);
    auto preserved_route = std::make_shared<std::atomic_bool> (false);
    node->actor_handoff_terminal_sender =
      [terminals, preserved_route, entered_once, entered,
       terminal_source] (const zlink::routing_id_t &parking, const zlink::routing_id_t &owner,
                         const rt::protocol::wire_operation_id_t &operation, std::uint64_t reply,
                         const rt::protocol::actor_route_fence_t &,
                         const result_t<zlink::message_t> &completed) -> task_t<bool> {
        preserved_route->store (parking.to_string () == "parking-node"
                                  && owner.to_string () == "request-owner" && operation.high == 83
                                  && operation.low == 91 && reply == 17,
                                std::memory_order_release);
        terminals->fetch_add (1, std::memory_order_acq_rel);
        if (!entered_once->exchange (true))
            entered->set_value (false);
        terminal_source->complete (completed);
        co_return true;
    };
    const auto reserved = spots.reserve_actor_join_barrier (actor);
    if (!reserved) {
        release->complete (result_t<void>::success ());
        return 3;
    }
    const auto barrier = reserved.value ();
    // Reservation retains the Actor FIFO; only the actual source seal moves
    // later ingress into the handoff hold.
    const auto sealed =
      spots
        .seal_actor_application_suffix (
          actor,
          [&] {
              if (!node->actor_transfer_coordinator->try_begin_local ("player:context-player"))
                  throw std::runtime_error ("disconnect replay source did not seal");
          })
        .result ();
    if (!sealed) {
        release->complete (result_t<void>::success ());
        barrier->cancel ();
        node->actor_transfer_coordinator->cancel_move ("player:context-player");
        return 8;
    }
    spot_inbound_message_t metadata;
    metadata.values.emplace (std::string (actor_handoff_source_node_key),
                             zlink::routing_id_t::from ("request-owner").to_hex ());
    metadata.values.emplace (std::string (actor_handoff_parking_node_key),
                             zlink::routing_id_t::from ("parking-node").to_hex ());
    metadata.values.emplace (std::string (actor_handoff_operation_high_key), "83");
    metadata.values.emplace (std::string (actor_handoff_operation_low_key), "91");
    metadata.values.emplace (std::string (actor_handoff_reply_route_key), "17");
    auto captured = spots.relay_actor_packet (actor, gateway.actor_context (actor),
                                              stream_message_kind_t::request,
                                              rt::protocol::session_disconnected_packet_name, body,
                                              provider, serializers, std::move (metadata));
    if (!captured.await_ready ()) {
        release->complete (result_t<void>::success ());
        barrier->cancel ();
        (void) captured.result ();
        node->worker_executor->drain ();
        return 4;
    }
    const auto parked = captured.result ();
    if (!parked || parked.value () || callbacks->load () != 0) {
        release->complete (result_t<void>::success ());
        barrier->cancel ();
        node->worker_executor->drain ();
        return 5;
    }
    std::cerr << "Session disconnect actual handoff replay without ingress metadata starts"
              << std::endl;
    const auto activated = barrier->activate_async (
      [spots, actor] (deferred_barrier_t::async_completion_t complete) mutable {
          auto opened = spots.open_local_actor_handoff (actor);
          observe_task_completion (opened, [complete = std::move (complete)] (
                                             const result_t<result_t<void>> &outcome) mutable {
              complete (outcome ? outcome.value ()
                                : result_t<void>::failure (outcome.error_kind (),
                                                           outcome.error ()->what ()));
          });
      });
    if (!activated) {
        release->complete (result_t<void>::success ());
        barrier->cancel ();
        node->worker_executor->drain ();
        return 6;
    }
    const bool began = callback_started.get ();
    const bool acknowledged_early = terminal.await_ready ();
    release->complete (result_t<void>::success ());
    const auto completed = terminal.result ();
    if (spot->serial_queue)
        spot->serial_queue->drain ();
    node->worker_executor->drain ();
    return began && !acknowledged_early && completed && completed.value ().is_empty ()
               && callbacks->load () == 1 && terminals->load () == 1
               && preserved_route->load (std::memory_order_acquire)
               && node->actor_transfer_coordinator->pending_count () == 0
             ? 0
             : 7;
}

int physical_disconnect_submits_all_before_waiting_for_terminal ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    for (const bool non_std : {false, true}) {
        actor_gateway_runtime_t gateway;
        auto manager = gateway.manager ();
        session_actor_manager_access_t::attach (manager, stream_t{});
        const auto first = test_actor_ref ("owner", "player", "first", 1);
        const auto second = test_actor_ref ("owner", "player", "second", 1);
        auto first_bound = manager.bind (first).async ().result ().value ();
        auto second_bound = manager.bind (second).async ().result ().value ();
        auto release = std::make_shared<task_completion_source_t<void>> ();
        std::vector<std::string> started;
        gateway.on_disconnect (
          [&] (const actor_ref_t &actor,
               const std::optional<runtime::stateful::stream_binding_t> &) -> task_t<void> {
              started.emplace_back (actor.actor_id ().value ());
              if (actor.actor_id ().value () == "first") {
                  co_await release->task ();
                  if (non_std)
                      throw 7;
                  throw framework_exception_t (framework_error_kind_t::invalid_operation,
                                               "first callback failed");
              }
              co_return;
          });
        auto pending = session_actor_manager_access_t::disconnect_async (manager);
        const bool all_started =
          started == std::vector<std::string>{"first", "second"} && !pending.await_ready ();
        release->complete (result_t<void>::success ());
        const auto completed = pending.result ();
        const auto first_again = first_bound.notify_disconnected ().result ();
        const auto second_again = second_bound.notify_disconnected ().result ();
        const bool cleaned = !first_again && !second_again;
        std::cerr << "Session disconnect terminal non_std=" << non_std
                  << " all_started=" << all_started << " cleaned=" << cleaned
                  << " completed=" << static_cast<bool> (completed) << std::endl;
        if (!all_started || !completed || !cleaned || started.size () != 2)
            return 1;
    }
    return 0;
}
int session_disconnect_dispatcher_preserves_identity_snapshot ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace sf = zlink::framework::runtime::stateful;
    for (const int mode : {0, 1, 2, 3, 4, 5, 6, 7}) {
        const bool physical = mode != 0 && (mode < 5 || mode == 7);
        const bool racing = mode >= 2 && mode < 5;
        const bool failing = mode == 3;
        actor_gateway_runtime_t gateway;
        auto manager = gateway.manager ();
        session_actor_manager_access_t::attach (manager, stream_t{});
        const auto actor = test_actor_ref ("actor-owner", "player", "snapshot-player", 1);
        const sf::object_ref_t target{
          sf::object_kind_t::actor, "snapshot-player", 1, 9, "mesh", "actor-owner"};
        sf::stream_session_registry_t registry (
          [&] (const auto &) { return std::make_optional (target); });
        const auto connection = registry.open (
          "connection", {}, zlink::routing_id_t::from ("original-owner"), UINT64_MAX);
        session_actor_manager_access_t::bind_native (
          manager, [&] (actor_ref_t, std::uint64_t token) -> task_t<void> {
              const auto result = registry.bind_remote (connection, target, 11, 13, {}, token);
              if (result.first != sf::stateful_error_t::none)
                  throw std::runtime_error ("canonical bind failed");
              co_return;
          });
        std::optional<sf::stream_binding_t> retired;
        session_actor_manager_access_t::bind_disconnect (
          manager,
          [&] (const actor_ref_t &, std::uint64_t) {
              return registry.current_binding (target.key);
          },
          [&] (sf::stream_binding_t binding) -> task_t<void> {
              const auto removed = registry.unbind (binding);
              retired = removed.second;
              co_return;
          });
        auto bound = manager.bind (actor).async ().result ().value ();
        const auto original = *registry.current_binding (target.key);
        int callbacks = 0;
        bool exact = false;
        auto release = std::make_shared<task_completion_source_t<void>> ();
        gateway.on_disconnect (
          [&] (const actor_ref_t &,
               const std::optional<sf::stream_binding_t> &snapshot) -> task_t<void> {
              ++callbacks;
              exact = snapshot && *snapshot == original;
              co_await release->task ();
              // The coroutine's snapshot argument remains alive until terminal.
              exact = exact && snapshot && *snapshot == original;
              if (failing)
                  throw framework_exception_t (framework_error_kind_t::invalid_operation,
                                               "callback failure");
          });
        std::optional<task_t<void>> logical;
        if (racing && mode != 4)
            logical.emplace (bound.notify_disconnected ());
        std::optional<task_t<void>> pending;
        bool closed_exactly = true;
        if (physical) {
            pending.emplace (session_actor_manager_access_t::disconnect_async (manager, [&] {
                auto closed = registry.close (connection);
                closed_exactly =
                  closed && closed.bindings == std::vector<sf::stream_binding_t>{original}
                  && !registry.current_binding (target.key) && !registry.close (connection);
                // Force logical capture into the exact gap after canonical
                // removal and before physical snapshot dispatch.
                if (mode == 4)
                    logical.emplace (bound.notify_disconnected ());
                return std::move (closed.bindings);
            }));
        } else
            pending.emplace (bound.notify_disconnected ());
        bool route_changed = true;
        if (mode == 5) {
            const auto sealed = registry.try_seal_actor (target);
            auto moved = target;
            moved.node_id = "moved-actor-owner";
            moved.authority_owner_generation = 17;
            const auto committed =
              registry.commit_remote_route (connection.connection_id, original.binding_generation,
                                            target.key, target.object_generation, moved, 19, 23);
            route_changed = sealed.first == sf::stateful_error_t::none
                            && committed.error == sf::stateful_error_t::none;
        } else if (mode == 6) {
            const auto rebound = registry.bind_remote (connection, target, 11, 13, {},
                                                       original.binding_generation + 1);
            route_changed = rebound.first == sf::stateful_error_t::none;
        }
        const bool held = callbacks == 1 && !pending->await_ready ();
        if (mode == 7)
            manager = session_actor_manager_t ();
        release->complete (result_t<void>::success ());
        const auto completed = pending->result ();
        const auto logical_result = logical ? logical->result () : result_t<void>::success ();
        const auto remaining = registry.current_binding (target.key);
        const bool expected_remaining =
          mode == 6 ? remaining && remaining->binding_generation == original.binding_generation + 1
                    : !remaining;
        const bool cleanup_route = mode == 5
                                     ? retired && retired->actor.node_id == "moved-actor-owner"
                                         && retired->target_node_generation == 19
                                         && retired->owner_lease_generation == 23
                                   : mode == 6 ? !retired
                                               : true;
        if (!completed || !closed_exactly || !route_changed || !cleanup_route
            || (static_cast<bool> (logical_result) == failing) || !held || !exact || callbacks != 1
            || !expected_remaining)
            return 2;
        if (remaining)
            (void) registry.unbind (*remaining);
        const auto repeated = bound.notify_disconnected ().result ();
        if (callbacks != 1 || (mode == 7 && repeated))
            return 3;
    }
    return 0;
}

int common_session_disconnect_uses_actor_packet_route ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace runtime = zlink::framework::runtime;
    serializer_registry_t serializers;
    auto node = test::runtime_failure_fixture<spot_node_builder_state_t> ("disconnect-node");
    node->worker_executor = std::make_shared<runtime::offload_executor_t> (1, "disconnect-test");
    auto spot = std::make_shared<spot_context_state_t> ();
    spot->node = node;
    spot->node_rid = node_rid_t::from_string ("disconnect-node");
    spot->spot_id = spot_id_t ("disconnect-spot");
    spot->spot_name = "disconnect";
    spot->serial_executor = node->worker_executor;
    node->spot_contexts_by_id.emplace (spot->spot_id, spot_context_access_t::create (spot));
    // The callbacks live in node state and can run after this scope ends, so they own their state.
    auto callbacks_counter = std::make_shared<std::atomic_int> (0);
    const auto &callbacks = *callbacks_counter;
    spot_actor_admission_callbacks_t admission;
    admission.on_disconnect_actor = [callbacks_counter] (void *, void *) -> task_t<void> {
        ++*callbacks_counter;
        co_return;
    };
    test::register_session_disconnect_probe (spot, std::move (admission.on_disconnect_actor));
    spot_node_builder_state_t::actor_factory_registration_t factory;
    factory.actor_type = std::type_index (typeid (int));
    factory.create_instance = [] (std::string) { return std::make_shared<int> (7); };
    factory.configure_instance = [] (void *, const actor_ref_t &, void *) {};
    node->actor_factories.emplace ("player", std::move (factory));
    const auto actor = test_actor_ref ("disconnect-node", "player", "player-one", 1);
    node->actor_instances.emplace ("player:player-one", std::make_shared<int> (7));
    node->actor_spot_ids.emplace ("player:player-one", spot->spot_id);
    node->actor_generations.emplace ("player:player-one", 1);
    node->actor_types_by_id.emplace ("player-one", "player");
    record_actor_instance_index_unlocked (*node, actor,
                                          node->actor_instances.at ("player:player-one").get ());
    service_collection_t services;
    services.add_singleton<actor_gateway_runtime_t> ();
    auto provider = services.build_provider ();
    auto &gateway = provider.get_required<actor_gateway_runtime_t> ();
    // Canonical shortRid vector shared by .NET and Node. The runtime has no binding.
    const std::vector<std::uint8_t> bytes{6, 'n', 'o', 'd', 'e', '-', 'a', 0, 0, 0, 0, 0,
                                          0, 0,   2,   0,   0,   0,   0,   0, 0, 0, 7};
    auto delivered =
      spot_node_runtime_t (node)
        .relay_actor_packet (actor, gateway.actor_context (actor), stream_message_kind_t::request,
                             runtime::protocol::session_disconnected_packet_name,
                             zlink::message_t::from (bytes), provider, serializers)
        .result ();
    std::cerr << "common Session disconnect: delivered=" << bool (delivered)
              << " callbacks=" << callbacks << std::endl;
    if (!delivered && delivered.error ())
        std::cerr << "common Session disconnect failure: " << delivered.error ()->what ()
                  << std::endl;
    if (!delivered || !delivered.value () || !delivered.value ()->is_empty () || callbacks != 1)
        return 1;
    const auto deliver = [&] (const std::vector<std::uint8_t> &body) {
        // A Message Follow hop has a different transport source from the body owner.
        return spot_node_runtime_t (node)
          .relay_actor_packet (actor, gateway.actor_context (actor), stream_message_kind_t::request,
                               runtime::protocol::session_disconnected_packet_name,
                               zlink::message_t::from (body), provider, serializers, {}, nullptr,
                               {}, {}, zlink::routing_id_t::from ("forwarding-node"))
          .result ();
    };
    const auto empty_success = [] (const auto &result) {
        return result && result.value () && result.value ()->is_empty ();
    };
    if (!gateway.record_bound_session_route (actor, zlink::routing_id_t::from ("node-a"),
                                             zlink::routing_id_t::from ("session-a"), 2, 1, 1, 7)
        || !empty_success (deliver (bytes)) || callbacks != 2)
        return 2;
    for (const auto offset : {1u, 14u, 22u}) {
        auto stale = bytes;
        ++stale[offset];
        if (!empty_success (deliver (stale)) || callbacks != 2)
            return 3;
    }
    auto errors_owner = std::make_shared<std::vector<message_dispatch_error_event_t>> ();
    const auto &errors = *errors_owner;
    node->dispatch.message_flow (message_flow_log_mode_t::normal);
    dispatch_options_access_t::set_dispatch_error_observer_for_tests (
      node->dispatch, [errors_owner] (const auto &event) { errors_owner->push_back (event); });
    auto trailing = bytes;
    trailing.push_back (0);
    auto truncated = bytes;
    truncated.pop_back ();
    auto zero_owner = bytes;
    zero_owner[14] = 0;
    auto zero_binding = bytes;
    zero_binding[22] = 0;
    for (const auto &malformed :
         {std::vector<std::uint8_t>{}, trailing, truncated, zero_owner, zero_binding}) {
        if (!empty_success (deliver (malformed)) || callbacks != 2)
            return 4;
    }
    if (errors.size () != 5)
        return 5;
    for (const auto &error : errors) {
        if (error.message_kind != dispatch_message_kind_t::actor_send) {
            std::cerr << "Session disconnect malformed diagnostic kind mismatch: expected "
                         "actor_send, actual="
                      << static_cast<int> (error.message_kind) << std::endl;
            return 6;
        }
        if (error.reason != dispatch_error_reason_t::invalid_frame
            || error.action != dispatch_error_action_t::drop
            || dispatch_error_reporter_t::default_log_level (error) != log_level_t::warn)
            return 6;
        try {
            std::rethrow_exception (error.exception);
        }
        catch (const framework_exception_t &failure) {
            if (failure.kind () != framework_error_kind_t::protocol_error)
                return 7;
        }
    }
    // Block the Actor with an explicitly completed handler; admit the notification,
    // replace the binding, and release the handler. The comparison belongs to the
    // notification's Actor turn, after replacement, rather than its queue admission.
    auto release = std::make_shared<task_completion_source_t<void>> ();
    auto entered = std::make_shared<std::promise<bool>> ();
    auto entered_once = std::make_shared<std::atomic_bool> (false);
    auto started = entered->get_future ();
    spot->handlers.push_back (
      spot_handler_descriptor_t{spot_handler_kind_t::actor_send, "DisconnectHold", "",
                                std::type_index (typeid (int)), std::type_index (typeid (void)),
                                std::type_index (typeid (int)), std::type_index (typeid (void))});
    spot->handler_invokers.push_back (
      [release, entered, entered_once] (
        void *, void *, service_provider_t &, serializer_registry_t &, const zlink::message_t &,
        const spot_inbound_message_t &) -> task_t<zlink::message_t> {
          if (!entered_once->exchange (true))
              entered->set_value (true);
          co_await release->task ();
          co_return zlink::message_t{};
      });
    auto held = spot_node_runtime_t (node).relay_actor_packet (
      actor, gateway.actor_context (actor), stream_message_kind_t::send, "DisconnectHold",
      zlink::message_t{}, provider, serializers);
    detail::observe_task_completion (held, [entered, entered_once] (const auto &) {
        if (!entered_once->exchange (true))
            entered->set_value (false);
    });
    if (!started.get ()) {
        release->complete (result_t<void>::success ());
        (void) held.result ();
        return 8;
    }
    auto enqueued = std::make_shared<std::promise<bool>> ();
    auto enqueued_once = std::make_shared<std::atomic_bool> (false);
    auto accepted = enqueued->get_future ();
    auto queued = spot_node_runtime_t (node).relay_actor_packet (
      actor, gateway.actor_context (actor), stream_message_kind_t::send,
      runtime::protocol::session_disconnected_packet_name, zlink::message_t::from (bytes), provider,
      serializers, {}, nullptr, {}, [enqueued, enqueued_once] {
          if (!enqueued_once->exchange (true))
              enqueued->set_value (true);
      });
    detail::observe_task_completion (queued, [enqueued, enqueued_once] (const auto &) {
        if (!enqueued_once->exchange (true))
            enqueued->set_value (false);
    });
    if (!accepted.get ()) {
        release->complete (result_t<void>::success ());
        (void) held.result ();
        (void) queued.result ();
        return 8;
    }
    const bool premature = queued.await_ready () || callbacks != 2;
    const auto replaced =
      gateway.record_bound_session_route (actor, zlink::routing_id_t::from ("node-a"),
                                          zlink::routing_id_t::from ("session-b"), 2, 1, 1, 8);
    release->complete (result_t<void>::success ());
    const auto held_result = held.result ();
    const auto queued_result = queued.result ();
    if (!held_result || !queued_result || premature || !replaced || callbacks != 2)
        return 8;
    auto current_body = bytes;
    current_body[22] = 8;
    if (!empty_success (deliver (current_body)) || callbacks != 3)
        return 9;
    // Seal strictly after transfer admission and before Actor FIFO submission.
    // The final admission recheck must preserve the untouched body for replay.
    spot_node_runtime_t spots (node);
    std::shared_ptr<deferred_barrier_t> barrier;
    bool reserved = false;
    node->actor_factories.at ("player").configure_instance = [&] (void *,
                                                                  const actor_ref_t &current_actor,
                                                                  void *) {
        if (reserved)
            return;
        reserved = true;
        auto result = spots.reserve_actor_join_barrier (current_actor);
        if (result) {
            barrier = std::move (result.value ());
            const auto sealed =
              spots
                .seal_actor_application_suffix (
                  current_actor,
                  [&] {
                      if (!node->actor_transfer_coordinator->try_begin_local ("player:player-one"))
                          throw std::runtime_error ("disconnect wire source did not seal");
                  })
                .result ();
            if (!sealed)
                throw framework_exception_t (sealed.error_kind (), sealed.error ()->what ());
        }
    };
    auto capture =
      spots.relay_actor_packet (actor, gateway.actor_context (actor), stream_message_kind_t::send,
                                runtime::protocol::session_disconnected_packet_name,
                                zlink::message_t::from (current_body), provider, serializers);
    const bool terminal_before_release = capture.await_ready ();
    if (!terminal_before_release) {
        if (barrier)
            barrier->cancel ();
        node->actor_transfer_coordinator->cancel_move ("player:player-one");
        (void) capture.result ();
        return 10;
    }
    const auto preserved = capture.result ();
    auto backlog = node->actor_transfer_coordinator->take_backlog ("player:player-one");
    const bool captured =
      preserved && barrier && callbacks == 3 && backlog.size () == 1
      && backlog.front ().packet_name == runtime::protocol::session_disconnected_packet_name
      && backlog.front ().payload == current_body && !backlog.front ().is_request;
    if (barrier)
        barrier->cancel ();
    node->actor_transfer_coordinator->cancel_move ("player:player-one");
    if (!captured)
        return 10;
    if (!empty_success (deliver (backlog.front ().payload)) || callbacks != 4)
        return 11;
    auto callback_terminal = std::make_shared<task_completion_source_t<void>> ();
    auto callback_entered = std::make_shared<std::promise<bool>> ();
    auto callback_started = callback_entered->get_future ();
    auto callback_entered_once = std::make_shared<std::atomic_bool> (false);
    spot->actor_admissions.at (std::type_index (typeid (int))).on_disconnect_actor =
      [callback_terminal, callback_entered, callback_entered_once] (void *,
                                                                    void *) -> task_t<void> {
        if (!callback_entered_once->exchange (true))
            callback_entered->set_value (true);
        co_await callback_terminal->task ();
    };
    auto awaiting_callback = spots.relay_actor_packet (
      actor, gateway.actor_context (actor), stream_message_kind_t::request,
      runtime::protocol::session_disconnected_packet_name, zlink::message_t::from (current_body),
      provider, serializers);
    detail::observe_task_completion (awaiting_callback,
                                     [callback_entered, callback_entered_once] (const auto &) {
                                         if (!callback_entered_once->exchange (true))
                                             callback_entered->set_value (false);
                                     });
    const bool began = callback_started.get ();
    const bool acknowledged_before_callback = awaiting_callback.await_ready ();
    callback_terminal->complete (result_t<void>::success ());
    const auto callback_reply = awaiting_callback.result ();
    if (!began || acknowledged_before_callback || !callback_reply || !callback_reply.value ()
        || !callback_reply.value ()->is_empty ())
        return 12;
    spot->actor_admissions.at (std::type_index (typeid (int))).on_disconnect_actor =
      [] (void *, void *) -> task_t<void> {
        return task_t<void> (result_t<void>::failure (framework_error_kind_t::invalid_operation,
                                                      "disconnect callback failed"));
    };
    const auto failed_callback = deliver (current_body);
    if (failed_callback
        || failed_callback.error_kind () != framework_error_kind_t::invalid_operation)
        return 13;
    node->worker_executor->drain ();
    return 0;
}

int actor_request_completion_keeps_dedup_state_owned_after_runtime_wrapper_unwinds ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace runtime = zlink::framework::runtime;

    serializer_registry_t serializers;
    auto node = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::spot_node_builder_state_t> ("actor-send-admission-node");
    node->worker_executor =
      std::make_shared<runtime::offload_executor_t> (1, "actor-send-admission");
    node->channel_runtime =
      zlink::framework::test::runtime_failure_fixture<channel_runtime_state_t> ();
    node->channel_runtime->serializers = &serializers;

    auto spot = std::make_shared<spot_context_state_t> ();
    spot->node = node;
    spot->node_rid = node_rid_t::from_string ("actor-send-admission-node");
    spot->spot_id = spot_id_t ("actor-send-admission-spot");
    spot->spot_name = "actor-send-admission";
    spot->channel_runtime =
      zlink::framework::test::runtime_failure_fixture<channel_runtime_state_t> ();
    spot->channel_runtime->serializers = &serializers;
    spot->serial_executor = node->worker_executor;
    node->spot_contexts_by_id.emplace (spot->spot_id, spot_context_access_t::create (spot));

    std::atomic_bool disconnected_started{false};
    spot_actor_admission_callbacks_t admission_callbacks;
    admission_callbacks.on_disconnect_actor = [&disconnected_started] (void *,
                                                                       void *) -> task_t<void> {
        disconnected_started.store (true, std::memory_order_release);
        co_return;
    };
    test::register_session_disconnect_probe (spot,
                                             std::move (admission_callbacks.on_disconnect_actor));

    spot_node_builder_state_t::actor_factory_registration_t factory;
    factory.actor_type = std::type_index (typeid (int));
    factory.create_instance = [] (std::string) { return std::make_shared<int> (7); };
    factory.configure_instance = [] (void *, const actor_ref_t &, void *) {};
    node->actor_factories.emplace ("player", std::move (factory));

    const auto actor =
      test_actor_ref ("actor-send-admission-node", "player", "reconnect-player", 1);
    const auto actor_key = std::string ("player:reconnect-player");
    node->actor_instances.emplace (actor_key, std::make_shared<int> (7));
    node->actor_spot_ids.emplace (actor_key, spot->spot_id);
    node->actor_generations.emplace (actor_key, 1);
    record_actor_instance_index_unlocked (
      *node, actor, node->actor_instances.at ("player:reconnect-player").get ());

    auto handler_terminal = std::make_shared<task_completion_source_t<void>> ();
    std::atomic_bool handler_started{false};
    std::promise<bool> handler_entered;
    auto entered = handler_entered.get_future ();
    auto handler_signalled = std::make_shared<std::atomic_bool> (false);
    spot->handlers.push_back (
      spot_handler_descriptor_t{spot_handler_kind_t::actor_request, "JoinGameMsg", "",
                                std::type_index (typeid (int)), std::type_index (typeid (void)),
                                std::type_index (typeid (int)), std::type_index (typeid (void))});
    spot->handler_invokers.push_back (
      [handler_terminal, &handler_started, &handler_entered, handler_signalled] (
        void *, void *, service_provider_t &, serializer_registry_t &, const zlink::message_t &,
        const spot_inbound_message_t &) -> task_t<zlink::message_t> {
          handler_started.store (true, std::memory_order_release);
          if (!handler_signalled->exchange (true))
              handler_entered.set_value (true);
          co_await handler_terminal->task ();
          co_return zlink::message_t{};
      });

    service_collection_t services;
    services.add_singleton<actor_gateway_runtime_t> ();
    auto provider = services.build_provider ();
    auto &gateway = provider.get_required<actor_gateway_runtime_t> ();
    std::atomic_int admitted{0};
    spot_inbound_message_t request_metadata;
    request_metadata.values.emplace ("__zlink.actorRequestId", "relocating-join-request");
    const auto operation_owner = zlink::routing_id_t::from ("actor-send-admission-node").to_hex ();
    request_metadata.values.emplace (std::string (actor_handoff_source_node_key), operation_owner);
    request_metadata.values.emplace (std::string (actor_handoff_parking_node_key), operation_owner);
    request_metadata.values.emplace (std::string (actor_handoff_operation_high_key), "83");
    request_metadata.values.emplace (std::string (actor_handoff_operation_low_key), "91");
    request_metadata.values.emplace (std::string (actor_handoff_reply_route_key), "91");
    // The temporary runtime wrapper has already unwound when handler_terminal
    // resumes this coroutine. The terminal path must still complete the node
    // owned exactly-once map, rather than dereferencing the dead wrapper.
    auto relayed = spot_node_runtime_t (node).relay_actor_packet (
      actor, gateway.actor_context (actor), stream_message_kind_t::request, "JoinGameMsg",
      zlink::message_t::from ("join"), provider, serializers, std::move (request_metadata), nullptr,
      {}, [&admitted] { admitted.fetch_add (1, std::memory_order_acq_rel); });

    observe_task_completion (relayed, [&handler_entered, handler_signalled] (const auto &) {
        if (!handler_signalled->exchange (true))
            handler_entered.set_value (false);
    });
    const bool began = entered.get ();
    if (!began || !handler_started.load (std::memory_order_acquire)
        || admitted.load (std::memory_order_acquire) != 1 || relayed.await_ready ()) {
        handler_terminal->complete (result_t<void>::success ());
        (void) relayed.result ();
        return 1;
    }
    std::promise<bool> disconnect_admitted;
    auto disconnect_accepted = disconnect_admitted.get_future ();
    auto disconnect_signalled = std::make_shared<std::atomic_bool> (false);
    const auto body = runtime::protocol::encode_session_disconnected (
      {zlink::routing_id_t::from ("session-owner").to_bytes (), 2, 7});
    auto disconnected = spot_node_runtime_t (node).relay_actor_packet (
      actor, gateway.actor_context (actor), stream_message_kind_t::send,
      runtime::protocol::session_disconnected_packet_name, zlink::message_t::from (body), provider,
      serializers, {}, nullptr, {}, [&disconnect_admitted, disconnect_signalled] {
          if (!disconnect_signalled->exchange (true))
              disconnect_admitted.set_value (true);
      });
    observe_task_completion (disconnected,
                             [&disconnect_admitted, disconnect_signalled] (const auto &) {
                                 if (!disconnect_signalled->exchange (true))
                                     disconnect_admitted.set_value (false);
                             });
    const bool accepted = disconnect_accepted.get ();
    const bool premature =
      disconnected.await_ready () || disconnected_started.load (std::memory_order_acquire);
    handler_terminal->complete (result_t<void>::success ());
    const auto relayed_result = relayed.result ();
    const auto disconnected_result = disconnected.result ();
    if (!accepted || premature)
        return 2;
    if (!relayed_result)
        return 3;
    const auto dedup = node->dispatched_request_replies.claim ("23:player:reconnect-player83:91");
    if (dedup.state != runtime::exactly_once_claim_state::completed || !dedup.value)
        return 4;
    return disconnected_result && disconnected_started.load (std::memory_order_acquire) ? 0 : 6;
}

int old_owner_forwards_cold_probe_via_active_message_follow_route (bool disconnect = false)
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
    auto node = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::spot_node_builder_state_t> ("actor-a");
    node->worker_executor = std::make_shared<runtime::offload_executor_t> (1, "actor-a-worker");
    node->channel_runtime =
      zlink::framework::test::runtime_failure_fixture<channel_runtime_state_t> ();
    node->channel_runtime->serializers = &serializers;

    // The factory and the relay handler can run after this scope ends (the 500 ms bounded wait
    // may return first), so they own what they write and capture read-only values by value.
    auto local_materializations = std::make_shared<std::atomic_int> (0);
    spot_node_builder_state_t::actor_factory_registration_t factory;
    factory.actor_type = std::type_index (typeid (int));
    factory.create_instance = [local_materializations] (std::string) {
        local_materializations->fetch_add (1, std::memory_order_acq_rel);
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
    const spot_route_t target_route{node_rid_t::from_string ("actor-b"),
                                    spot_id_t ("spot-remote-ok-0"), "game"};

    // Mirrors spot_node_runtime_t::complete_remote_actor_transfer: it retains
    // the Message Follow route for the old source fence and (via
    // record_actor_route_unlocked) records the remote target's location,
    // which is not a locally registered Spot on this node.
    node->actor_transfer_coordinator->activate_message_follow (
      key, source_fence, target_actor, target_route, target_fence,
      std::chrono::steady_clock::now () + std::chrono::seconds (30), "relocation-1");
    record_actor_route_unlocked (*node, key, target_route, target_actor.object_generation ());

    // The caller's own identity (spec 28.en:584-591): a synthesized-fence
    // relay must still forward these to the Message Follow relay, not the
    // zero/empty placeholders that would silently skip the stale-cache
    // notice back to whoever actually sent this probe.
    const auto inbound_source_node_rid = zlink::routing_id_t::from ("probing-client");
    const auto inbound_operation = runtime::protocol::wire_operation_id_t{111, 222};
    constexpr std::uint64_t inbound_reply_route_id = 333;
    const std::string inbound_deadline = "2030-01-02T03:04:05Z";

    const auto packet_name =
      disconnect ? runtime::protocol::session_disconnected_packet_name : "after-transfer";
    const auto payload = disconnect
                           ? runtime::protocol::encode_session_disconnected (
                               {zlink::routing_id_t::from ("session-owner").to_bytes (), 2, 7})
                           : zlink::message_t::from (std::string ("probe")).to_bytes ();
    auto follow_terminal =
      std::make_shared<task_completion_source_t<std::optional<zlink::message_t>>> ();
    std::promise<bool> follow_entered;
    auto follow_started = follow_entered.get_future ();
    auto follow_entered_once = std::make_shared<std::atomic_bool> (false);
    auto relay_calls = std::make_shared<std::atomic_int> (0);
    spot_node_runtime_t spots (node);
    spots.on_actor_message_follow (
      [relay_calls, follow_terminal, follow_entered_once, &follow_entered, disconnect, source_fence,
       packet_name, payload, inbound_deadline, inbound_source_node_rid, inbound_operation] (
        const actor_ref_t &relayed_actor, const runtime::messaging::envelope_header_t &header,
        const zlink::message_t &body, std::chrono::milliseconds relay_timeout,
        const zlink::routing_id_t &source_node, const runtime::protocol::actor_route_fence_t &route,
        std::uint8_t hop_count, const runtime::protocol::wire_operation_id_t &operation,
        std::uint64_t reply_route_id) -> task_t<std::optional<zlink::message_t>> {
          relay_calls->fetch_add (1, std::memory_order_acq_rel);
          if (disconnect && !follow_entered_once->exchange (true))
              follow_entered.set_value (true);
          if (relayed_actor.actor_id ().value () != "actor-remote-ok" || route != source_fence
              || hop_count != 0 || header.message_name != packet_name || body.to_bytes () != payload
              || header.deadline != inbound_deadline || relay_timeout != std::chrono::seconds (30)
              || source_node.to_bytes () != inbound_source_node_rid.to_bytes ()
              || operation.high != inbound_operation.high || operation.low != inbound_operation.low
              || reply_route_id != inbound_reply_route_id) {
              co_return result_t<std::optional<zlink::message_t>>::failure (
                framework_error_kind_t::internal_failure, "unexpected relay arguments");
          }
          if (disconnect)
              co_return co_await follow_terminal->task ();
          co_return result_t<std::optional<zlink::message_t>>::success (
            std::make_optional (zlink::message_t::from (std::string ("relayed"))));
      });

    service_collection_t services;
    auto provider = services.build_provider ();
    actor_gateway_runtime_t gateway;
    spot_inbound_message_t metadata;

    auto relayed = spots.relay_actor_packet (
      actor, gateway.actor_context (actor), stream_message_kind_t::request, packet_name,
      zlink::message_t::from (payload), provider, serializers, std::move (metadata), nullptr,
      std::function<void ()>{}, std::function<void ()>{}, inbound_source_node_rid,
      inbound_operation, inbound_reply_route_id, inbound_deadline);

    bool premature = false;
    if (disconnect) {
        detail::observe_task_completion (relayed,
                                         [&follow_entered, follow_entered_once] (const auto &) {
                                             if (!follow_entered_once->exchange (true))
                                                 follow_entered.set_value (false);
                                         });
        const bool began = follow_started.get ();
        premature = !began || relayed.await_ready ();
        follow_terminal->complete (result_t<std::optional<zlink::message_t>>::success (
          std::make_optional (zlink::message_t{})));
    }
    const auto outcome = disconnect ? std::make_optional (relayed.result ())
                                    : finite_task_result (std::move (relayed));
    if (!outcome || !*outcome)
        return 1;
    if (relay_calls->load (std::memory_order_acquire) != 1)
        return 2;
    if (disconnect && (premature || !outcome->value () || !outcome->value ()->is_empty ()))
        return 4;
    if (local_materializations->load (std::memory_order_acquire) != 0)
        return 3;
    return 0;
}

int returned_owner_serves_locally_past_retained_message_follow_route ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace runtime = zlink::framework::runtime;

    // A->B->A keeps one Actor incarnation. When A becomes the Store-confirmed
    // owner again, its newer authority fence must win over the old A->B
    // Message Follow route that remains for the retransmission window. A
    // locally originated packet carries no follow fence, so relay_actor_packet
    // synthesizes the retained source fence; that must not reopen the old edge.
    serializer_registry_t serializers;
    auto node = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::spot_node_builder_state_t> ("actor-a");
    node->worker_executor =
      std::make_shared<runtime::offload_executor_t> (1, "returned-owner-worker");
    node->channel_runtime =
      zlink::framework::test::runtime_failure_fixture<channel_runtime_state_t> ();
    node->channel_runtime->serializers = &serializers;

    auto spot = std::make_shared<spot_context_state_t> ();
    spot->node = node;
    spot->node_rid = node_rid_t::from_string ("actor-a");
    spot->spot_id = spot_id_t ("actor-a-returned");
    spot->spot_name = "returned";
    spot->spot_instance = std::make_shared<int> (1);
    spot->channel_runtime =
      zlink::framework::test::runtime_failure_fixture<channel_runtime_state_t> ();
    spot->channel_runtime->serializers = &serializers;
    spot->serial_executor = node->worker_executor;
    node->spot_contexts_by_id.emplace (spot->spot_id, spot_context_access_t::create (spot));

    spot_node_builder_state_t::actor_factory_registration_t factory;
    factory.actor_type = std::type_index (typeid (int));
    factory.create_instance = [] (std::string) { return std::make_shared<int> (7); };
    factory.configure_instance = [] (void *, const actor_ref_t &, void *) {};
    node->actor_factories.emplace ("player", std::move (factory));

    const auto actor = test_actor_ref ("actor-a", "player", "returned-actor", 21);
    const auto key = std::string ("player:returned-actor");
    node->actor_instances.emplace (key, std::make_shared<int> (7));
    node->actor_spot_ids.emplace (key, spot->spot_id);
    node->actor_routes.emplace (
      key, spot_route_t{node_rid_t::from_string ("actor-a"), spot->spot_id, "returned"});
    node->actor_generations.emplace (key, actor.object_generation ());

    const auto old_source_fence = runtime::protocol::actor_route_fence_t{
      "returned-actor", 21, zlink::routing_id_t::from ("actor-a").to_bytes (), 1, 3, 5};
    const auto old_target_fence = runtime::protocol::actor_route_fence_t{
      "returned-actor", 21, zlink::routing_id_t::from ("actor-b").to_bytes (), 1, 4, 7};
    node->actor_transfer_coordinator->activate_message_follow (
      key, old_source_fence, test_actor_ref ("actor-b", "player", "returned-actor", 21),
      spot_route_t{node_rid_t::from_string ("actor-b"), spot_id_t ("actor-b-away"), "away"},
      old_target_fence, std::chrono::steady_clock::now () + std::chrono::seconds (30),
      "relocation-away");
    node->actor_authority_fences.emplace (
      key, runtime::protocol::actor_route_fence_t{
             "returned-actor", 21, zlink::routing_id_t::from ("actor-a").to_bytes (), 1, 5, 11});

    std::atomic_int local_calls{0};
    spot->handlers.push_back (
      spot_handler_descriptor_t{spot_handler_kind_t::actor_send, "ReturnedProbe", "",
                                std::type_index (typeid (int)), std::type_index (typeid (void)),
                                std::type_index (typeid (int)), std::type_index (typeid (void))});
    spot->handler_invokers.push_back (
      [&local_calls] (void *, void *, service_provider_t &, serializer_registry_t &,
                      const zlink::message_t &,
                      const spot_inbound_message_t &) -> task_t<zlink::message_t> {
          local_calls.fetch_add (1, std::memory_order_acq_rel);
          co_return zlink::message_t{};
      });

    std::atomic_int relay_calls{0};
    spot_node_runtime_t spots (node);
    spots.on_actor_message_follow (
      [&relay_calls] (const actor_ref_t &, const runtime::messaging::envelope_header_t &,
                      const zlink::message_t &, std::chrono::milliseconds,
                      const zlink::routing_id_t &, const runtime::protocol::actor_route_fence_t &,
                      std::uint8_t, const runtime::protocol::wire_operation_id_t &,
                      std::uint64_t) -> task_t<std::optional<zlink::message_t>> {
          relay_calls.fetch_add (1, std::memory_order_acq_rel);
          co_return result_t<std::optional<zlink::message_t>>::success (std::nullopt);
      });

    service_collection_t services;
    auto provider = services.build_provider ();
    actor_gateway_runtime_t gateway;
    spot_inbound_message_t metadata;
    auto sent =
      spots.relay_actor_packet (actor, gateway.actor_context (actor), stream_message_kind_t::send,
                                "ReturnedProbe", zlink::message_t::from (std::string ("probe")),
                                provider, serializers, std::move (metadata), nullptr);
    const auto outcome = finite_task_result (std::move (sent));
    node->worker_executor->drain ();
    node->worker_executor.reset ();
    if (!outcome || !*outcome)
        return 1;
    if (local_calls.load (std::memory_order_acquire) != 1)
        return 2;
    return relay_calls.load (std::memory_order_acquire) == 0 ? 0 : 3;
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
    auto node = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::spot_node_builder_state_t> ("actor-a");

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
    if (spots.next_management_activity_async ().result ().value ()
        != now + std::chrono::seconds (30))
        return 12;
    // not_before has already passed, but OnLeave is only queued
    // (leave_submitted), not finished (leave_completed), and the last-resort
    // deadline is far off: the sweep must hold the erase.
    const auto swept_while_pending = spots.cleanup_expired_actor_admissions_at (now);
    if (swept_while_pending != 0)
        return 1;
    if (!node->actor_instances.contains (key))
        return 2;
    if (node->pending_remote_source_cleanups.size () != 1)
        return 3;

    // OnLeave's queued task finishes and marks completion, mirroring the
    // completion continuation submit_remote_actor_leave installs.
    node->pending_remote_source_cleanups.front ().leave_completed = true;
    const auto swept_after_completion = spots.cleanup_expired_actor_admissions_at (now);
    if (swept_after_completion == 0)
        return 4;
    if (node->actor_instances.contains (key))
        return 5;
    if (!node->pending_remote_source_cleanups.empty ())
        return 6;

    // A return admission can begin after OnLeave completed but before this
    // sweep. The retained cleanup fence is the proof materialization needs to
    // distinguish the old A->B source instance from a live conflict. Preserve
    // both while dispatch is blocked by that in-flight move, then clean them
    // normally once the move closes.
    node->actor_instances.emplace (key, std::make_shared<int> (8));
    node->pending_remote_source_cleanups.push_back (
      spot_node_builder_state_t::pending_remote_source_cleanup_t{
        .source_actor = actor,
        .source_fence = runtime::protocol::actor_route_fence_t{},
        .transfer_id = "transfer-return-race",
        .target_spot_id = spot_id_t ("target-spot"),
        .not_before = now - std::chrono::seconds (1),
        .leave_submitted = true,
        .leave_completed = true,
        .leave_deadline = now - std::chrono::seconds (1)});
    if (!node->actor_transfer_coordinator->try_reserve_source (key, "return-admission"))
        return 7;
    if (spots.cleanup_expired_actor_admissions_at (now) != 0)
        return 8;
    if (!node->actor_instances.contains (key) || node->pending_remote_source_cleanups.size () != 1)
        return 9;
    node->actor_transfer_coordinator->cancel_move (key);
    if (spots.cleanup_expired_actor_admissions_at (now) == 0)
        return 10;
    if (node->actor_instances.contains (key) || !node->pending_remote_source_cleanups.empty ())
        return 11;
    return 0;
}

int sweep_preserves_pending_leave_added_after_snapshot ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace runtime = zlink::framework::runtime;

    auto node = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::spot_node_builder_state_t> ("actor-a");
    const auto actor = test_actor_ref ("actor-a", "player", "sweep-race-actor", 7);
    const auto pending = [&] (std::string transfer_id) {
        return spot_node_builder_state_t::pending_remote_actor_leave_t{
          .transfer_id = std::move (transfer_id),
          .source_actor = actor,
          .source_spot_id = spot_id_t ("source-spot"),
          .target_spot_id = spot_id_t ("target-spot"),
          .target_fence = runtime::protocol::actor_route_fence_t{}};
    };
    node->pending_remote_actor_leaves.push_back (pending ("old-transfer"));
    spot_node_runtime_t spots (node);
    const auto removed =
      spots.cleanup_expired_actor_admissions_at (std::chrono::steady_clock::now (), [&] {
          if (!node->lane.try_post ([node, &pending] {
                  node->pending_remote_actor_leaves.push_back (pending ("new-transfer"));
              }))
              throw std::runtime_error ("node lane closed before pending leave insertion");
      });
    const auto remaining = node->lane
                             .run ([&] {
                                 std::vector<std::string> transfers;
                                 for (const auto &leave : node->pending_remote_actor_leaves)
                                     transfers.push_back (leave.transfer_id);
                                 return transfers;
                             })
                             .get ();
    return removed == 1 && remaining == std::vector<std::string>{"new-transfer"} ? 0 : 1;
}

// Shared stub: the committed relocation record cleanup_expired_actor_admissions_at
// reads at reconcile-deadline expiry to reconcile against the Location Store's
// authority (spec 28 relay-ready irreversibility) instead of blindly replaying
// locally. Its record is nulled or filled per test to select the branch under
// test.
class fixed_reconcile_authority_store_t final
    : public zlink::framework::runtime::stateful::authority_relocation_port_t
{
  public:
    zlink::framework::runtime::stateful::source_relocation_settlement_t settle_source_relocation (
      const zlink::framework::runtime::stateful::relocation_authority_fence_t &fence,
      const std::vector<zlink::framework::runtime::stateful::object_ref_t> &participants,
      bool preserve) override
    {
        namespace sf = zlink::framework::runtime::stateful;
        (void) preserve;
        sf::source_relocation_settlement_t settled{observe_relocation (fence), {}};
        if (settled.outcome == sf::relocation_authority_t::target_committed) {
            for (const auto &participant : participants) {
                if (const auto current = read (participant.kind, participant.key))
                    settled.follow_generations.emplace (
                      std::pair{participant.kind, participant.key},
                      current->target.authority_owner_generation);
            }
        }
        return settled;
    }

    std::optional<zlink::framework::runtime::stateful::authority_relocation_reference_t> record;

    zlink::framework::runtime::stateful::authority_publish_result_t
    publish (const zlink::framework::runtime::stateful::object_ref_t &,
             const zlink::framework::runtime::stateful::object_ref_t &,
             zlink::framework::location_owner_token_t,
             zlink::framework::object_creation_target_t,
             std::string,
             std::uint32_t,
             zlink::framework::runtime::stateful::inventory_digest_t,
             std::vector<std::byte> = {},
             std::string = {},
             zlink::framework::runtime::protocol::relocation_id_t = {},
             zlink::framework::location_owner_token_t = {}) override
    {
        return {};
    }
    std::optional<zlink::framework::runtime::stateful::authority_relocation_reference_t>
    read (zlink::framework::runtime::stateful::object_kind_t, const std::string &) override
    {
        return record;
    }
};

int reconcile_deadline_adopts_target_when_store_shows_committed ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace runtime = zlink::framework::runtime;
    namespace stateful = zlink::framework::runtime::stateful;

    // 04-relocation-flow.ko.md §9, paragraph after the failure table:
    // after accepted relay-ready and an unknown cutover result, a visible
    // target commit adopts the target route and forward-drains the backlog.
    // Source dispatch must never reopen.
    auto node = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::spot_node_builder_state_t> ("actor-a");
    node->worker_executor =
      std::make_shared<runtime::offload_executor_t> (1, "reconcile-adopt-worker");
    service_collection_t services;
    node->root_services = services.build_provider ();

    const auto key = std::string ("player:reconcile-actor");
    node->actor_generations.emplace (key, 1);

    const auto source_fence =
      runtime::protocol::actor_route_fence_t{"reconcile-actor", 1, {}, 0, 5, 1};
    const auto target_fence =
      runtime::protocol::actor_route_fence_t{"reconcile-actor", 1, {}, 0, 6, 2};
    const auto target_actor = test_actor_ref ("actor-b", "player", "reconcile-actor", 1);
    const auto target_route =
      spot_route_t{node_rid_t::from_string ("actor-b"), spot_id_t ("target-spot"), ""};

    auto authority = std::make_shared<fixed_reconcile_authority_store_t> ();
    authority->record = stateful::authority_relocation_reference_t{
      .source = stateful::object_ref_t{stateful::object_kind_t::actor, "reconcile-actor", 1, 5,
                                       "mesh", "actor-a"},
      .target = stateful::object_ref_t{stateful::object_kind_t::actor, "reconcile-actor", 1, 6,
                                       "mesh", "actor-b"}};
    node->relocation_authority = authority;

    const auto before_reconcile = std::chrono::steady_clock::now ();
    node->actor_transfer_coordinator->mark_reconcile (
      key, std::chrono::milliseconds (20),
      reconcile_target_context_t{target_route, target_actor, source_fence, target_fence,
                                 "transfer-reconcile"});
    const auto after_reconcile = std::chrono::steady_clock::now ();

    // A packet parked while reconcile was pending must actually be drained
    // by forwarding to the adopted target -- not merely have its route
    // adopted with the backlog left stranded.
    // The adoption task can run after an early return, so the handler owns its counter.
    auto relay_calls = std::make_shared<std::atomic_int> (0);
    spot_node_runtime_t spots (node);
    spots.on_actor_message_follow (
      [relay_calls] (const actor_ref_t &, const runtime::messaging::envelope_header_t &,
                     const zlink::message_t &, std::chrono::milliseconds,
                     const zlink::routing_id_t &, const runtime::protocol::actor_route_fence_t &,
                     std::uint8_t, const runtime::protocol::wire_operation_id_t &,
                     std::uint64_t) -> task_t<std::optional<zlink::message_t>> {
          relay_calls->fetch_add (1, std::memory_order_acq_rel);
          co_return result_t<std::optional<zlink::message_t>>::success (std::nullopt);
      });
    (void) node->actor_transfer_coordinator->try_append_backlog (
      key, handoff_packet_t{"AdoptedProbe",
                            std::vector<std::uint8_t> ({'p', 'r', 'o', 'b', 'e'}),
                            "text/plain",
                            {},
                            false});

    if (spots.cleanup_expired_actor_admissions_at (before_reconcile
                                                   + std::chrono::milliseconds (19))
          != 0
        || relay_calls->load (std::memory_order_acquire) != 0
        || !node->actor_transfer_coordinator->blocks_dispatch (key))
        return 6;
    if (spots.cleanup_expired_actor_admissions_at (after_reconcile + std::chrono::milliseconds (20))
        != 1)
        return 7;

    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (2);
    while (node->actor_transfer_coordinator->phase (key).has_value ()
           && std::chrono::steady_clock::now () < deadline) {
        std::this_thread::yield ();
    }
    if (node->actor_transfer_coordinator->phase (key).has_value ())
        return 1;
    const auto route = node->actor_routes.find (key);
    const auto found_target_route = route != node->actor_routes.end ();
    const auto spot_matches =
      found_target_route && route->second.spot_id == std::string ("target-spot");
    const auto node_matches =
      found_target_route && route->second.node_rid.value () == std::string_view ("actor-b");
    // The async adoption task (worker_executor's own thread) is the sole
    // other owner of `node` at this point; wait for it to fully settle and
    // detach the executor here, on the main thread, before `node` goes out
    // of scope -- otherwise the worker thread could end up dropping the
    // last reference to its own executor and self-joining.
    node->worker_executor->drain ();
    node->worker_executor.reset ();
    if (relay_calls->load (std::memory_order_acquire) != 1)
        return 5;
    if (!found_target_route)
        return 2;
    if (!spot_matches)
        return 3;
    if (!node_matches)
        return 4;
    return 0;
}

std::shared_ptr<zlink::framework::runtime::serial_application_record_t> accepted_actor_fifo_record (
  const zlink::framework::runtime::protocol::actor_route_fence_t &source_fence,
  std::uint64_t sequence,
  const zlink::framework::runtime::messaging::envelope_header_t &envelope,
  std::string payload)
{
    namespace runtime = zlink::framework::runtime;
    const bool request = envelope.kind == runtime::messaging::message_kind_t::request;
    auto parts =
      zlink::framework::detail::encode_actor_packet (envelope, zlink::message_t::from (payload));
    runtime::protocol::actor_message_header_t header;
    header.operation = {1, sequence};
    header.target = source_fence;
    if (request)
        header.correlation = sequence;
    auto record = std::make_shared<runtime::serial_application_record_t> ();
    record->payload ().emplace ();
    record->payload ()->sequence = sequence;
    record->payload ()->application_payload_bytes = payload.size ();
    record->payload ()->application_record = runtime::stateful::accepted_actor_record (
      header,
      {"fence-source-owner", 1, source_fence.target_node_routing_id,
       source_fence.target_node_generation},
      runtime::protocol::application_payload_t::from_parts (std::move (parts).take_items ()),
      request);
    return record;
}

// Regression pin for the relocation admission TOCTOU (B-2):
// spot-actor membership :903 ("Defer() 뒤 source seal 전 message는 barrier 뒤
// Actor queue에 두고 capture 대상") and relocation-flow :108/:155.
//
// Configure injects Deferred Join registration between coordinator admission
// and the Actor FIFO post. The native accepted record must remain visible in
// the queue suffix when seal starts; a committed source instead follows it.
// A manual scheduler fixes these boundaries without a timing race.
int relocation_barrier_fences_late_actor_fifo_admission (bool commit_before_post = false)
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace runtime = zlink::framework::runtime;

    serializer_registry_t serializers;
    auto node = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::spot_node_builder_state_t> ("actor-a");
    auto scheduler = std::make_shared<zlink::framework::test::manual_internal_scheduler_t> ();
    scheduler->publication_fence (
      zlink::framework::test::state_lane_publication_fence (node->lane, node->lane_executor));
    node->worker_executor = scheduler;
    node->channel_runtime =
      zlink::framework::test::runtime_failure_fixture<channel_runtime_state_t> ();
    node->channel_runtime->serializers = &serializers;

    auto spot = std::make_shared<spot_context_state_t> ();
    spot->node = node;
    spot->node_rid = node_rid_t::from_string ("actor-a");
    spot->spot_id = spot_id_t ("actor-a-entry");
    spot->spot_name = "entry";
    spot->spot_instance = std::make_shared<int> (1);
    spot->channel_runtime =
      zlink::framework::test::runtime_failure_fixture<channel_runtime_state_t> ();
    spot->channel_runtime->serializers = &serializers;
    spot->serial_executor = node->worker_executor;
    node->spot_contexts_by_id.emplace (spot->spot_id, spot_context_access_t::create (spot));

    const auto actor = test_actor_ref ("actor-a", "player", "fence-actor", 1);
    const auto key = std::string ("player:fence-actor");
    spot_node_runtime_t spots (node);

    const runtime::protocol::actor_route_fence_t source_fence{
      "fence-actor", 1, zlink::routing_id_t::from ("actor-a").to_bytes (), 1, 1, 1};
    auto target_fence = source_fence;
    target_fence.target_node_routing_id = zlink::routing_id_t::from ("actor-b").to_bytes ();
    target_fence.authority_owner_generation = 2;
    std::atomic_int follow_calls{0};
    spots.on_actor_message_follow (
      [&] (const actor_ref_t &, const runtime::messaging::envelope_header_t &header,
           const zlink::message_t &payload, std::chrono::milliseconds, const zlink::routing_id_t &,
           const runtime::protocol::actor_route_fence_t &fence, std::uint8_t,
           runtime::protocol::wire_operation_id_t,
           std::uint64_t) -> task_t<std::optional<zlink::message_t>> {
          ++follow_calls;
          if (fence != source_fence || header.message_name != "FencedProbe"
              || header.metadata.at ("sequence") != "LATE" || payload.to_string () != "late")
              co_return result_t<std::optional<zlink::message_t>>::failure (
                framework_error_kind_t::internal_failure, "late packet follow arguments changed");
          co_return result_t<std::optional<zlink::message_t>>::success (zlink::message_t{});
      });
    std::atomic_int configure_calls{0};
    std::shared_ptr<deferred_barrier_t> barrier;
    std::atomic_bool barrier_reserved{false};
    spot_node_builder_state_t::actor_factory_registration_t factory;
    factory.actor_type = std::type_index (typeid (int));
    factory.create_instance = [] (std::string) { return std::make_shared<int> (7); };
    factory.configure_instance = [&] (void *, const actor_ref_t &current, void *) {
        if (configure_calls.fetch_add (1, std::memory_order_acq_rel) != 0)
            return;
        // Registration retains this Actor mailbox. Until seal, accepted
        // application records still belong to the Actor FIFO.
        auto reserved = spots.reserve_actor_join_barrier (current);
        if (!reserved)
            return;
        barrier = std::move (reserved.value ());
        barrier_reserved.store (true, std::memory_order_release);
        if (commit_before_post) {
            node->actor_transfer_coordinator->activate_message_follow (
              key, source_fence, test_actor_ref ("actor-b", "player", "fence-actor", 1),
              spot_route_t{node_rid_t::from_string ("actor-b"), spot_id_t ("target"), "game"},
              target_fence, std::chrono::steady_clock::now () + std::chrono::seconds (30),
              "commit-before-post");
            node->actor_transfer_coordinator->cancel_move (key);
        }
    };
    node->actor_factories.emplace ("player", std::move (factory));

    node->actor_instances.emplace (key, std::make_shared<int> (7));
    node->actor_spot_ids.emplace (key, spot->spot_id);
    node->actor_generations.emplace (key, 1);
    node->actor_types_by_id.emplace ("fence-actor", "player");

    std::atomic_int source_handler_runs{0};
    std::atomic_int admission_notifications{0};
    std::atomic_int owner_transfers{0};
    spot->handlers.push_back (
      spot_handler_descriptor_t{spot_handler_kind_t::actor_send, "FencedProbe", "",
                                std::type_index (typeid (int)), std::type_index (typeid (void)),
                                std::type_index (typeid (int)), std::type_index (typeid (void))});
    spot->handler_invokers.push_back (
      [&source_handler_runs] (void *, void *, service_provider_t &, serializer_registry_t &,
                              const zlink::message_t &,
                              const spot_inbound_message_t &) -> task_t<zlink::message_t> {
          source_handler_runs.fetch_add (1, std::memory_order_acq_rel);
          co_return zlink::message_t{};
      });

    service_collection_t services;
    auto provider = services.build_provider ();
    actor_gateway_runtime_t gateway;

    runtime::messaging::envelope_header_t envelope;
    envelope.kind = runtime::messaging::message_kind_t::command;
    envelope.channel_name = "actor";
    envelope.message_name = "FencedProbe";
    envelope.content_type = "application/json";
    envelope.metadata.emplace ("sequence", "LATE");
    auto late_record =
      commit_before_post ? nullptr : accepted_actor_fifo_record (source_fence, 1, envelope, "late");
    envelope.metadata.clear ();
    auto resumed_record = accepted_actor_fifo_record (source_fence, 2, envelope, "resumed");
    std::vector<const runtime::serial_application_record_t *> transferred_records;
    spot_inbound_message_t metadata;
    metadata.values.emplace ("sequence", "LATE");
    auto sent = spots.relay_actor_packet (
      actor, gateway.actor_context (actor), stream_message_kind_t::send, "FencedProbe",
      zlink::message_t::from (std::string ("late")), provider, serializers, std::move (metadata),
      nullptr, {}, [&] { ++admission_notifications; }, zlink::routing_id_t::from (std::uint32_t{0}),
      {}, 0, std::nullopt,
      [&] {
          ++owner_transfers;
          transferred_records.push_back (late_record.get ());
      },
      0, {}, late_record);

    const auto cleanup = [&] {
        if (barrier)
            barrier->cancel ();
        node->actor_transfer_coordinator->cancel_move (key);
        scheduler->run_all ();
        node->worker_executor->drain ();
        node->worker_executor.reset ();
    };

    if (!commit_before_post && barrier_reserved.load (std::memory_order_acquire)) {
        auto sealed = spots.seal_actor_application_suffix (actor, [&] {
            if (!node->actor_transfer_coordinator->try_begin_source_remote (key, "fifo-fence"))
                throw std::runtime_error ("reserved Actor did not enter source handoff");
        });
        scheduler->run_all ();
        const auto sealed_result = finite_task_result (std::move (sealed));
        if (!sealed_result || !*sealed_result) {
            cleanup ();
            return 10;
        }
    }
    scheduler->run_all ();
    const auto sent_result = finite_task_result (std::move (sent));

    if (!barrier_reserved.load (std::memory_order_acquire)) {
        cleanup ();
        return 1;
    }
    // A one-way send preserved for the move is indistinguishable from
    // immediate dispatch on the wire (spot-actor §10.2-1).
    if (!sent_result || !*sent_result) {
        cleanup ();
        return 2;
    }
    // The contract, not just the symptom: the old owner must never run it.
    if (source_handler_runs.load (std::memory_order_acquire) != 0
        || admission_notifications != 1
        // Queue acceptance transfers this exact record once before seal;
        // a committed-source relay does not submit to the old Actor queue.
        || owner_transfers != (commit_before_post ? 0 : 1)) {
        cleanup ();
        return 3;
    }
    if (commit_before_post) {
        const bool forwarded =
          follow_calls == 1 && node->actor_transfer_coordinator->take_backlog (key).empty ();
        cleanup ();
        return forwarded ? 0 : 9;
    }
    if (!node->actor_transfer_coordinator->blocks_dispatch (key)) {
        cleanup ();
        return 4;
    }
    // It must be in the capture set that travels with the commit, exactly once.
    auto backlog = node->actor_transfer_coordinator->take_backlog (key);
    if (backlog.size () != 1 || backlog.front ().packet_name != "FencedProbe"
        || backlog.front ().is_request || backlog.front ().application_record != late_record
        || transferred_records
             != std::vector<const runtime::serial_application_record_t *>{late_record.get ()}
        || !late_record->saved_work ()) {
        cleanup ();
        return 5;
    }
    const auto sequence = backlog.front ().metadata.find ("sequence");
    if (sequence == backlog.front ().metadata.end () || sequence->second != "LATE") {
        cleanup ();
        return 6;
    }

    // The barrier is released and the move closed: ordinary ingress resumes on
    // the same queue rather than staying fenced forever.
    barrier->cancel ();
    barrier.reset ();
    node->actor_transfer_coordinator->cancel_move (key);
    spot_inbound_message_t resumed_metadata;
    auto resumed = spots.relay_actor_packet (
      actor, gateway.actor_context (actor), stream_message_kind_t::send, "FencedProbe",
      zlink::message_t::from (std::string ("resumed")), provider, serializers,
      std::move (resumed_metadata), nullptr, {}, [&] { ++admission_notifications; },
      zlink::routing_id_t::from (std::uint32_t{0}), {}, 0, std::nullopt,
      [&] {
          ++owner_transfers;
          transferred_records.push_back (resumed_record.get ());
      },
      0, {}, resumed_record);
    scheduler->run_all ();
    const auto resumed_result = finite_task_result (std::move (resumed));
    const auto resumed_deadline = std::chrono::steady_clock::now () + std::chrono::seconds (2);
    while (source_handler_runs.load (std::memory_order_acquire) == 0
           && std::chrono::steady_clock::now () < resumed_deadline) {
        std::this_thread::yield ();
    }
    const auto resumed_runs = source_handler_runs.load (std::memory_order_acquire);
    cleanup ();
    if (!resumed_result || !*resumed_result)
        return 7;
    // Resumed ingress is a second record with its own single queue transfer.
    if (resumed_runs != 1 || admission_notifications != 2 || owner_transfers != 2
        || transferred_records
             != std::vector<const runtime::serial_application_record_t *>{late_record.get (),
                                                                          resumed_record.get ()})
        return 8;
    return 0;
}

// A request caught in the same window must park too, and must not deadlock
// against the barrier that is waiting for the relocation to finish.
int relocation_barrier_fences_late_actor_request_without_deadlock ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace runtime = zlink::framework::runtime;

    serializer_registry_t serializers;
    auto node = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::spot_node_builder_state_t> ("actor-a");
    auto scheduler = std::make_shared<zlink::framework::test::manual_internal_scheduler_t> ();
    scheduler->publication_fence (
      zlink::framework::test::state_lane_publication_fence (node->lane, node->lane_executor));
    node->worker_executor = scheduler;
    node->channel_runtime =
      zlink::framework::test::runtime_failure_fixture<channel_runtime_state_t> ();
    node->channel_runtime->serializers = &serializers;

    auto spot = std::make_shared<spot_context_state_t> ();
    spot->node = node;
    spot->node_rid = node_rid_t::from_string ("actor-a");
    spot->spot_id = spot_id_t ("actor-a-entry");
    spot->spot_name = "entry";
    spot->spot_instance = std::make_shared<int> (1);
    spot->channel_runtime =
      zlink::framework::test::runtime_failure_fixture<channel_runtime_state_t> ();
    spot->channel_runtime->serializers = &serializers;
    spot->serial_executor = node->worker_executor;
    node->spot_contexts_by_id.emplace (spot->spot_id, spot_context_access_t::create (spot));

    const auto actor = test_actor_ref ("actor-a", "player", "fence-request-actor", 1);
    const auto key = std::string ("player:fence-request-actor");
    spot_node_runtime_t spots (node);

    std::atomic_int configure_calls{0};
    std::shared_ptr<deferred_barrier_t> barrier;
    std::atomic_bool barrier_reserved{false};
    spot_node_builder_state_t::actor_factory_registration_t factory;
    factory.actor_type = std::type_index (typeid (int));
    factory.create_instance = [] (std::string) { return std::make_shared<int> (7); };
    factory.configure_instance = [&] (void *, const actor_ref_t &current, void *) {
        if (configure_calls.fetch_add (1, std::memory_order_acq_rel) != 0)
            return;
        auto reserved = spots.reserve_actor_join_barrier (current);
        if (!reserved)
            return;
        barrier = std::move (reserved.value ());
        barrier_reserved.store (true, std::memory_order_release);
    };
    node->actor_factories.emplace ("player", std::move (factory));

    node->actor_instances.emplace (key, std::make_shared<int> (7));
    node->actor_spot_ids.emplace (key, spot->spot_id);
    node->actor_generations.emplace (key, 1);
    node->actor_types_by_id.emplace ("fence-request-actor", "player");

    std::atomic_int source_handler_runs{0};
    std::atomic_int admission_notifications{0};
    std::atomic_int owner_transfers{0};
    spot->handlers.push_back (
      spot_handler_descriptor_t{spot_handler_kind_t::actor_request, "FencedAsk", "",
                                std::type_index (typeid (int)), std::type_index (typeid (void)),
                                std::type_index (typeid (int)), std::type_index (typeid (void))});
    spot->handler_invokers.push_back (
      [&source_handler_runs] (void *, void *, service_provider_t &, serializer_registry_t &,
                              const zlink::message_t &,
                              const spot_inbound_message_t &) -> task_t<zlink::message_t> {
          source_handler_runs.fetch_add (1, std::memory_order_acq_rel);
          co_return zlink::message_t{};
      });

    service_collection_t services;
    auto provider = services.build_provider ();
    actor_gateway_runtime_t gateway;

    const runtime::protocol::actor_route_fence_t source_fence{
      "fence-request-actor", 1, zlink::routing_id_t::from ("actor-a").to_bytes (), 1, 1, 1};
    runtime::messaging::envelope_header_t envelope;
    envelope.kind = runtime::messaging::message_kind_t::request;
    envelope.channel_name = "actor";
    envelope.message_name = "FencedAsk";
    envelope.content_type = "application/json";
    envelope.metadata.emplace ("__zlink.actorRequestId", "fence-request-1");
    auto request_record = accepted_actor_fifo_record (source_fence, 1, envelope, "ask");
    const runtime::serial_application_record_t *transferred_record = nullptr;
    spot_inbound_message_t metadata;
    metadata.values = envelope.metadata;
    auto asked = spots.relay_actor_packet (
      actor, gateway.actor_context (actor), stream_message_kind_t::request, "FencedAsk",
      zlink::message_t::from (std::string ("ask")), provider, serializers, std::move (metadata),
      nullptr, {}, [&] { ++admission_notifications; }, zlink::routing_id_t::from (std::uint32_t{0}),
      {}, 0, std::nullopt,
      [&] {
          ++owner_transfers;
          transferred_record = request_record.get ();
      },
      0, {}, request_record);

    const auto cleanup = [&] {
        if (barrier)
            barrier->cancel ();
        node->actor_transfer_coordinator->cancel_move (key);
        scheduler->run_all ();
        node->worker_executor->drain ();
        node->worker_executor.reset ();
    };

    if (barrier_reserved.load (std::memory_order_acquire)) {
        auto sealed = spots.seal_actor_application_suffix (actor, [&] {
            if (!node->actor_transfer_coordinator->try_begin_source_remote (key, "request-fence"))
                throw std::runtime_error ("reserved request did not enter source handoff");
        });
        scheduler->run_all ();
        const auto sealed_result = finite_task_result (std::move (sealed));
        if (!sealed_result || !*sealed_result) {
            cleanup ();
            return 5;
        }
    }
    scheduler->run_all ();
    // Export ends the old invocation, while the accepted request retains its
    // original operation and reply capability for replay.
    const auto asked_result = finite_task_result (std::move (asked));

    if (!barrier_reserved.load (std::memory_order_acquire)) {
        cleanup ();
        return 1;
    }
    // Preserved request: success with no reply yet (the target replies after
    // replay), never a silent local dispatch.
    if (!asked_result || !*asked_result || (*asked_result).value ().has_value ()) {
        cleanup ();
        return 2;
    }
    if (source_handler_runs.load (std::memory_order_acquire) != 0 || admission_notifications != 1
        || owner_transfers != 1 || transferred_record != request_record.get ()) {
        cleanup ();
        return 3;
    }
    auto backlog = node->actor_transfer_coordinator->take_backlog (key);
    if (backlog.size () != 1 || !backlog.front ().is_request
        || backlog.front ().packet_name != "FencedAsk"
        || backlog.front ().metadata.at ("__zlink.actorRequestId") != "fence-request-1"
        || backlog.front ().application_record != request_record
        || !request_record->saved_work ()) {
        cleanup ();
        return 4;
    }
    bool payload_preserved = false;
    runtime::stateful::with_accepted_application (
      *request_record->payload (), [&] (auto kind, const auto &application) {
          const runtime::messaging::message_parts_t parts (
            runtime::protocol::decode_application_parts (application));
          const auto body = runtime::messaging::envelope_codec_t{}.decode_body (parts);
          payload_preserved = kind == runtime::protocol::frozen_record_kind_t::actor_request && body
                              && body.value ().to_string () == "ask";
      });
    cleanup ();
    return payload_preserved ? 0 : 6;
}

int reconcile_deadline_fast_fails_without_target_commit (
  std::shared_ptr<fixed_reconcile_authority_store_t> authority)
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace runtime = zlink::framework::runtime;

    // 04-relocation-flow.ko.md §9, paragraph after the failure table:
    // after relay-ready is accepted, an unknown cutover cannot reopen source
    // dispatch even when the Store still shows the source. The target may
    // continue its CAS within Restore validity. At the reconciliation deadline,
    // held requests must end Unavailable and the source must stay sealed.
    serializer_registry_t serializers;
    auto node = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::spot_node_builder_state_t> ("actor-a");
    node->worker_executor =
      std::make_shared<runtime::offload_executor_t> (1, "reconcile-indeterminate-worker");
    node->channel_runtime =
      zlink::framework::test::runtime_failure_fixture<channel_runtime_state_t> ();
    node->channel_runtime->serializers = &serializers;

    auto spot = std::make_shared<spot_context_state_t> ();
    spot->node = node;
    spot->node_rid = node_rid_t::from_string ("actor-a");
    spot->spot_id = spot_id_t ("actor-a-entry");
    spot->spot_name = "entry";
    spot->spot_instance = std::make_shared<int> (1);
    spot->channel_runtime =
      zlink::framework::test::runtime_failure_fixture<channel_runtime_state_t> ();
    spot->channel_runtime->serializers = &serializers;
    spot->serial_executor = node->worker_executor;
    node->spot_contexts_by_id.emplace (spot->spot_id, spot_context_access_t::create (spot));

    spot_node_builder_state_t::actor_factory_registration_t factory;
    factory.actor_type = std::type_index (typeid (int));
    factory.create_instance = [] (std::string) { return std::make_shared<int> (7); };
    factory.configure_instance = [] (void *, const actor_ref_t &, void *) {};
    node->actor_factories.emplace ("player", std::move (factory));

    const auto actor = test_actor_ref ("actor-a", "player", "reconcile-actor", 1);
    const auto key = std::string ("player:reconcile-actor");
    node->actor_instances.emplace (key, std::make_shared<int> (7));
    node->actor_spot_ids.emplace (key, spot->spot_id);
    node->actor_generations.emplace (key, 1);

    // The handlers and sinks below can run after an early return skips the drain, so they own
    // their flags and counters.
    auto handler_ran = std::make_shared<std::atomic_bool> (false);
    spot->handlers.push_back (
      spot_handler_descriptor_t{spot_handler_kind_t::actor_send, "ReconcileProbe", "",
                                std::type_index (typeid (int)), std::type_index (typeid (void)),
                                std::type_index (typeid (int)), std::type_index (typeid (void))});
    spot->handler_invokers.push_back (
      [handler_ran] (void *, void *, service_provider_t &, serializer_registry_t &,
                     const zlink::message_t &,
                     const spot_inbound_message_t &) -> task_t<zlink::message_t> {
          handler_ran->store (true, std::memory_order_release);
          co_return zlink::message_t{};
      });

    const auto source_fence =
      runtime::protocol::actor_route_fence_t{"reconcile-actor", 1, {}, 0, 5, 1};
    const auto target_fence =
      runtime::protocol::actor_route_fence_t{"reconcile-actor", 1, {}, 0, 6, 2};
    const auto target_actor = test_actor_ref ("actor-b", "player", "reconcile-actor", 1);
    const auto target_route =
      spot_route_t{node_rid_t::from_string ("actor-b"), spot_id_t ("target-spot"), ""};
    node->relocation_authority = std::move (authority);
    const auto before_reconcile = std::chrono::steady_clock::now ();
    node->actor_transfer_coordinator->mark_reconcile (
      key, std::chrono::milliseconds (20),
      reconcile_target_context_t{target_route, target_actor, source_fence, target_fence,
                                 "transfer-reconcile"});

    const auto after_reconcile = std::chrono::steady_clock::now ();

    // A parked *request* (not just a send) must actually receive an
    // explicit failure reply -- "bounded wait then requestFailed", not
    // just silently dropped or left to time out on its own.
    auto terminal_calls = std::make_shared<std::atomic_int> (0);
    auto terminal_was_failure = std::make_shared<std::atomic_bool> (false);
    node->actor_handoff_terminal_sender =
      [terminal_calls,
       terminal_was_failure] (const zlink::routing_id_t &, const zlink::routing_id_t &,
                              const runtime::protocol::wire_operation_id_t &, std::uint64_t,
                              const runtime::protocol::actor_route_fence_t &,
                              const result_t<zlink::message_t> &completed) -> task_t<bool> {
        terminal_calls->fetch_add (1, std::memory_order_acq_rel);
        terminal_was_failure->store (!completed, std::memory_order_release);
        co_return true;
    };
    std::map<std::string, std::string> terminal_metadata;
    terminal_metadata[std::string (actor_handoff_source_node_key)] =
      zlink::routing_id_t::from ("remote-caller-node").to_hex ();
    terminal_metadata[std::string (actor_handoff_parking_node_key)] =
      zlink::routing_id_t::from ("actor-a").to_hex ();
    terminal_metadata[std::string (actor_handoff_operation_high_key)] = "111";
    terminal_metadata[std::string (actor_handoff_operation_low_key)] = "222";
    terminal_metadata[std::string (actor_handoff_reply_route_key)] = "333";
    // This backlog belongs to actor-a, so its terminal is delivered through
    // actor-a's parked reply entry rather than re-sent to the remote caller.
    // Keep a real local reply token here: after terminal identity gained the
    // parking-node and initiating-fence dimensions, a hand-written backlog
    // with no matching pending entry only exercises the intentional drop.
    auto local_reply_calls = std::make_shared<std::atomic_int> (0);
    auto local_reply_was_unavailable = std::make_shared<std::atomic_bool> (false);
    auto reply_host = std::make_shared<host::public_host_runtime_t> (host::host_options_t{
      .mesh = {.descriptor = {.mesh_name = "reconcile-indeterminate",
                              .node_routing_id =
                                zlink::routing_id_t::from (std::string ("actor-a")).to_bytes (),
                              .lifecycle_generation = 1,
                              .descriptor_revision = 1,
                              .advertised_endpoint = "tcp://127.0.0.1:0"},
               .runtime_failures =
                 std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()}});
    const auto make_reply_token = [reply_host] (std::shared_ptr<std::atomic_int> calls,
                                                std::shared_ptr<std::atomic_bool> was_unavailable) {
        host::reply_token_t token;
        token.host = reply_host;
        token.local_reply = [calls, was_unavailable] (const std::vector<zlink::message_t> &parts) {
            const auto reply =
              detail::decode_actor_reply (runtime::messaging::message_parts_t (parts));
            was_unavailable->store (!reply
                                      && reply.error_kind () == framework_error_kind_t::unavailable,
                                    std::memory_order_release);
            calls->fetch_add (1, std::memory_order_acq_rel);
            return true;
        };
        return token;
    };
    auto local_reply = make_reply_token (local_reply_calls, local_reply_was_unavailable);
    node->pending_handoff_requests.emplace (
      spot_node_builder_state_t::pending_handoff_request_key_t{
        zlink::routing_id_t::from ("remote-caller-node").to_hex (), 111, 222, {}},
      spot_node_builder_state_t::pending_handoff_request_t{actor,
                                                           {},
                                                           333,
                                                           std::move (local_reply),
                                                           {},
                                                           std::chrono::steady_clock::now ()
                                                             + std::chrono::seconds (30)});
    auto second_reply_calls = std::make_shared<std::atomic_int> (0);
    auto second_reply_was_unavailable = std::make_shared<std::atomic_bool> (false);
    auto second_metadata = terminal_metadata;
    second_metadata[std::string (actor_handoff_operation_high_key)] = "112";
    second_metadata[std::string (actor_handoff_operation_low_key)] = "223";
    second_metadata[std::string (actor_handoff_reply_route_key)] = "334";
    node->pending_handoff_requests.emplace (
      spot_node_builder_state_t::pending_handoff_request_key_t{
        zlink::routing_id_t::from ("remote-caller-node").to_hex (), 112, 223, {}},
      spot_node_builder_state_t::pending_handoff_request_t{
        actor,
        {},
        334,
        make_reply_token (second_reply_calls, second_reply_was_unavailable),
        {},
        std::chrono::steady_clock::now () + std::chrono::seconds (30)});
    (void) node->actor_transfer_coordinator->try_append_backlog (
      key,
      handoff_packet_t{"FastFailProbe", {}, "application/octet-stream", terminal_metadata, true});
    (void) node->actor_transfer_coordinator->try_append_backlog (
      key,
      handoff_packet_t{"FastFailProbe", {}, "application/octet-stream", second_metadata, true});

    service_collection_t services;
    auto provider = services.build_provider ();
    actor_gateway_runtime_t gateway;
    spot_inbound_message_t metadata;

    spot_node_runtime_t spots (node);
    auto sent =
      spots.relay_actor_packet (actor, gateway.actor_context (actor), stream_message_kind_t::send,
                                "ReconcileProbe", zlink::message_t::from (std::string ("probe")),
                                provider, serializers, std::move (metadata), nullptr);
    const auto sent_result = finite_task_result (std::move (sent));
    if (!sent_result || !*sent_result)
        return 1;
    if (handler_ran->load (std::memory_order_acquire))
        return 2;

    // Drive the existing monotonic sweep clock across the deadline without
    // sleeping. Before expiry the reply must remain parked.
    if (spots.cleanup_expired_actor_admissions_at (before_reconcile
                                                   + std::chrono::milliseconds (19))
          != 0
        || local_reply_calls->load (std::memory_order_acquire) != 0
        || second_reply_calls->load (std::memory_order_acquire) != 0)
        return 9;
    const auto expired = after_reconcile + std::chrono::milliseconds (20);
    if (spots.cleanup_expired_actor_admissions_at (expired) != 1)
        return 10;
    // No target commit is visible: keep the move sealed, regardless of
    // whether the Store returned source evidence, no record, or no answer.
    if (!node->actor_transfer_coordinator->phase (key).has_value ())
        return 3;
    if (*node->actor_transfer_coordinator->phase (key) != actor_move_phase_t::reconcile)
        return 4;
    if (terminal_calls->load (std::memory_order_acquire) != 0
        || local_reply_calls->load (std::memory_order_acquire) != 1
        || second_reply_calls->load (std::memory_order_acquire) != 1)
        return 7;
    if (terminal_was_failure->load (std::memory_order_acquire)
        || !local_reply_was_unavailable->load (std::memory_order_acquire)
        || !second_reply_was_unavailable->load (std::memory_order_acquire))
        return 8;
    if (!node->pending_handoff_requests.empty ()
        || !node->actor_transfer_coordinator->blocks_dispatch (key)
        || node->actor_routes.contains (key))
        return 11;

    // A fresh request still cannot reach the handler -- it parks again
    // rather than being served, because the Actor genuinely stays
    // unavailable.
    spot_inbound_message_t follow_up_metadata;
    auto follow_up =
      spots.relay_actor_packet (actor, gateway.actor_context (actor), stream_message_kind_t::send,
                                "ReconcileProbe", zlink::message_t::from (std::string ("probe-2")),
                                provider, serializers, std::move (follow_up_metadata), nullptr);
    const auto follow_up_result = finite_task_result (std::move (follow_up));
    if (!follow_up_result || !*follow_up_result)
        return 5;
    (void) spots.cleanup_expired_actor_admissions_at (expired);
    node->worker_executor->drain ();
    if (handler_ran->load (std::memory_order_acquire))
        return 6;
    if (local_reply_calls->load (std::memory_order_acquire) != 1
        || second_reply_calls->load (std::memory_order_acquire) != 1
        || !node->actor_transfer_coordinator->blocks_dispatch (key)
        || node->actor_routes.contains (key))
        return 12;
    return 0;
}

int reconcile_deadline_fast_fails_when_store_shows_source ()
{
    namespace stateful = zlink::framework::runtime::stateful;
    auto authority = std::make_shared<fixed_reconcile_authority_store_t> ();
    // This record still names actor-a, not the expected actor-b commit. Its
    // matching source fence used to authorize the unsafe source_owns replay.
    const auto source = stateful::object_ref_t{
      stateful::object_kind_t::actor, "reconcile-actor", 1, 5, "mesh", "actor-a"};
    authority->record =
      stateful::authority_relocation_reference_t{.source = source, .target = source};
    return reconcile_deadline_fast_fails_without_target_commit (std::move (authority));
}

int reconcile_deadline_fast_fails_when_store_has_no_commit ()
{
    // A missing relocation record also used to authorize source_owns replay.
    return reconcile_deadline_fast_fails_without_target_commit (
      std::make_shared<fixed_reconcile_authority_store_t> ());
}

int reconcile_deadline_fast_fails_when_store_is_indeterminate ()
{
    return reconcile_deadline_fast_fails_without_target_commit (nullptr);
}

int leave_notification_travels_node_level_and_reaches_source_entry_spot_once ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace runtime = zlink::framework::runtime;
    namespace stateful = zlink::framework::runtime::stateful;

    // Regression pin: the OnLeave notification travels node-level (not
    // spot-addressed -- the source's Entry Spot is fixed to node lifecycle
    // and is never published into mesh spot routing, so a spot-addressed
    // send can never resolve it; that was ST-B1's "leave" marker never
    // firing). This drives the actual production sender
    // (spot_node_runtime_t::send_actor_leave_notification, reached via the
    // same on_actor_leave_notification hook app.cpp wires to
    // application_mesh->send_to_node) and the actual production receiver
    // (spot_route_internal_dispatcher_t::dispatch_send ->
    // submit_remote_actor_leave), connected by a stand-in for the wire hop.
    // Asserts OnLeave reaches the source Entry Spot exactly once and that
    // the just-landed completion-ordering gate (leave_completed) becomes
    // reachable and true afterward.
    serializer_registry_t serializers;

    // SOURCE node ("actor-a"): the Entry Spot the Actor left from.
    auto source_node = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::spot_node_builder_state_t> ("actor-a");
    source_node->worker_executor =
      std::make_shared<runtime::offload_executor_t> (1, "leave-notify-source");
    source_node->channel_runtime =
      zlink::framework::test::runtime_failure_fixture<channel_runtime_state_t> ();
    source_node->channel_runtime->serializers = &serializers;

    auto entry_spot = std::make_shared<spot_context_state_t> ();
    entry_spot->node = source_node;
    entry_spot->node_rid = node_rid_t::from_string ("actor-a");
    entry_spot->spot_id = spot_id_t ("actor-a-entry");
    entry_spot->spot_name = "entry";
    entry_spot->spot_instance = std::make_shared<int> (1);
    entry_spot->channel_runtime =
      zlink::framework::test::runtime_failure_fixture<channel_runtime_state_t> ();
    entry_spot->channel_runtime->serializers = &serializers;
    entry_spot->serial_executor = source_node->worker_executor;
    // spot_handle_t::status() only reads the object_ref_t it was
    // constructed with, never the host, so a null host is a faithful stand
    // -in here for the lifecycle_generation() check
    // submit_remote_actor_leave makes.
    const auto native_entry_spot = std::make_shared<service::spot_t> (
      nullptr, stateful::object_ref_t{stateful::object_kind_t::user_spot, "actor-a-entry", 7, 0,
                                      "mesh", "actor-a"});
    entry_spot->native_spot = native_entry_spot;
    source_node->spot_contexts_by_id.emplace (entry_spot->spot_id,
                                              spot_context_access_t::create (entry_spot));

    std::atomic_int leave_calls{0};
    spot_actor_admission_callbacks_t admission_callbacks;
    admission_callbacks.on_leave_actor = [&leave_calls] (void *, void *) -> task_t<void> {
        leave_calls.fetch_add (1, std::memory_order_release);
        co_return;
    };
    entry_spot->actor_admissions.emplace (std::type_index (typeid (int)),
                                          std::move (admission_callbacks));

    spot_node_builder_state_t::actor_factory_registration_t factory;
    factory.actor_type = std::type_index (typeid (int));
    factory.create_instance = [] (std::string) { return std::make_shared<int> (1); };
    factory.configure_instance = [] (void *, const actor_ref_t &, void *) {};
    source_node->actor_factories.emplace ("player", std::move (factory));

    const auto actor = test_actor_ref ("actor-a", "player", "leave-notify-actor", 7);
    const auto key = std::string ("player:leave-notify-actor");
    source_node->actor_instances.emplace (key, std::make_shared<int> (1));
    source_node->actor_spot_ids.emplace (key, entry_spot->spot_id);
    source_node->actor_generations.emplace (key, 7);

    // Authority store: the committed relocation record
    // submit_remote_actor_leave validates the OnLeave command against.
    class fixed_authority_store_t final : public stateful::authority_relocation_port_t
    {
      public:
        zlink::framework::runtime::stateful::source_relocation_settlement_t
        settle_source_relocation (
          const zlink::framework::runtime::stateful::relocation_authority_fence_t &fence,
          const std::vector<zlink::framework::runtime::stateful::object_ref_t> &participants,
          bool preserve) override
        {
            namespace sf = zlink::framework::runtime::stateful;
            (void) preserve;
            sf::source_relocation_settlement_t settled{observe_relocation (fence), {}};
            if (settled.outcome == sf::relocation_authority_t::target_committed) {
                for (const auto &participant : participants) {
                    if (const auto current = read (participant.kind, participant.key))
                        settled.follow_generations.emplace (
                          std::pair{participant.kind, participant.key},
                          current->target.authority_owner_generation);
                }
            }
            return settled;
        }

        std::optional<stateful::authority_relocation_reference_t> record;

        stateful::authority_publish_result_t
        publish (const stateful::object_ref_t &,
                 const stateful::object_ref_t &,
                 location_owner_token_t,
                 object_creation_target_t,
                 std::string,
                 std::uint32_t,
                 stateful::inventory_digest_t,
                 std::vector<std::byte> = {},
                 std::string = {},
                 zlink::framework::runtime::protocol::relocation_id_t = {},
                 location_owner_token_t = {}) override
        {
            return {};
        }
        std::optional<stateful::authority_relocation_reference_t>
        read (stateful::object_kind_t, const std::string &) override
        {
            return record;
        }
    };
    auto authority = std::make_shared<fixed_authority_store_t> ();
    authority->record = stateful::authority_relocation_reference_t{
      .source = stateful::object_ref_t{stateful::object_kind_t::actor, "leave-notify-actor", 7, 0,
                                       "", "actor-a"},
      .target = stateful::object_ref_t{stateful::object_kind_t::actor, "leave-notify-actor", 7, 5,
                                       "", "actor-b"},
      .relocation_reference = "transfer-leave-notify",
      .checksum_crc32c = 0,
      .inventory_digest = {},
      .target_owner = location_owner_token_t{"owner-b", 9},
      .application_payload = {}};
    source_node->relocation_authority = authority;

    // A pending source cleanup for this transfer, exactly as
    // complete_remote_actor_transfer would have pushed at commit -- gated
    // on leave_completed (this session's completion-ordering fix).
    const auto source_fence = runtime::protocol::actor_route_fence_t{
      "leave-notify-actor", 7, zlink::routing_id_t::from ("actor-a").to_bytes (), 1, 1, 1};
    const auto target_fence = runtime::protocol::actor_route_fence_t{
      "leave-notify-actor", 7, zlink::routing_id_t::from ("actor-b").to_bytes (), 1, 5, 9};
    source_node->pending_remote_source_cleanups.push_back (
      spot_node_builder_state_t::pending_remote_source_cleanup_t{
        .source_actor = actor,
        .source_fence = source_fence,
        .transfer_id = "transfer-leave-notify",
        .source_spot_id = spot_id_t ("actor-a-entry"),
        .source_spot_generation = 7,
        .target_spot_id = spot_id_t ("spot-b-target"),
        .target_identity = {target_fence.target_node_routing_id,
                            target_fence.target_node_generation,
                            {},
                            target_fence.owner_lease_generation},
        .not_before = std::chrono::steady_clock::now (),
        .leave_submitted = false,
        .leave_completed = false,
        .leave_deadline = std::chrono::steady_clock::now () + std::chrono::seconds (30)});
    source_node->actor_transfer_coordinator->activate_message_follow (
      key, source_fence, test_actor_ref ("actor-b", "player", "leave-notify-actor", 7),
      spot_route_t{node_rid_t::from_string ("actor-b"), spot_id_t ("spot-b-target"), "game"},
      target_fence, std::chrono::steady_clock::now () + std::chrono::seconds (30),
      "transfer-leave-notify");

    // TARGET node ("actor-b"): only needs enough to hold the
    // on_actor_leave_notification sender under test.
    auto target_node = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::spot_node_builder_state_t> ("actor-b");

    spot_node_runtime_t source_spots (source_node);
    spot_node_runtime_t target_spots (target_node);

    service_collection_t services;
    auto provider = services.build_provider ();
    actor_gateway_runtime_t source_gateway;

    std::atomic_int notification_sends{0};
    target_spots.on_actor_leave_notification (
      [&] (const zlink::routing_id_t &target_node_rid,
           std::vector<zlink::message_t> parts) -> task_t<zlink::submit_result_t> {
          notification_sends.fetch_add (1, std::memory_order_release);
          if (target_node_rid.to_string () != "actor-a")
              co_return zlink::submit_result_t::not_found;
          // Stand-in for the wire hop: RouteMesh delivers a node-level send
          // to the receiving node's own dispatch_send exactly like this.
          spot_route_internal_dispatcher_t dispatcher (source_spots, source_gateway,
                                                       route_client_t{}, serializers);
          runtime::messaging::message_parts_t encoded (std::move (parts));
          route_received_packet_t received{zlink::routing_id_t::from ("actor-b"), std::nullopt,
                                           std::move (encoded), std::nullopt};
          auto header =
            runtime::messaging::envelope_codec_t{}.decode_header (received.parts, false);
          if (!header)
              co_return zlink::submit_result_t::internal_error;
          const auto dispatched = dispatcher.dispatch_send (received, header.value (), provider);
          co_return dispatched ? zlink::submit_result_t::ok
                               : zlink::submit_result_t::internal_error;
      });

    // Build the leave command exactly as dispatch_actor_commit_request's
    // finalize branch does, and submit it through the production sender.
    const auto leave_command =
      spot_actor_leave_route_command_t{.transfer_id = "transfer-leave-notify",
                                       .actor_node_rid = "actor-a",
                                       .actor_type = "player",
                                       .actor_id = "leave-notify-actor",
                                       .actor_generation = 7,
                                       .source_spot_id = "actor-a-entry",
                                       .source_spot_generation = 7,
                                       .target_spot_id = "spot-b-target",
                                       .target_node_rid = "actor-b",
                                       .target_node_generation = 1,
                                       .target_authority_owner_generation = 5,
                                       .target_owner_lease_generation = 9};
    runtime::messaging::envelope_header_t leave_header;
    leave_header.kind = runtime::messaging::message_kind_t::command;
    leave_header.channel_name = "node";
    leave_header.message_name = spot_actor_leave_route_command_t::packet_name;
    auto leave_parts = runtime::messaging::envelope_codec_t{}.encode_parts (
      leave_header, leave_command, serializers);

    auto sent = target_spots.send_actor_leave_notification (zlink::routing_id_t::from ("actor-a"),
                                                            std::move (leave_parts));
    const auto sent_result = finite_task_result (std::move (sent));
    if (!sent_result || !*sent_result || sent_result->value () != zlink::submit_result_t::ok)
        return 1;
    if (notification_sends.load (std::memory_order_acquire) != 1)
        return 2;

    const auto leave_deadline = std::chrono::steady_clock::now () + std::chrono::seconds (2);
    while (leave_calls.load (std::memory_order_acquire) == 0
           && std::chrono::steady_clock::now () < leave_deadline) {
        std::this_thread::yield ();
    }
    if (leave_calls.load (std::memory_order_acquire) != 1)
        return 3;

    // leave_completed is set from the queued callback's own completion
    // continuation, asynchronously; poll for it the same way.
    const auto completion_deadline = std::chrono::steady_clock::now () + std::chrono::seconds (2);
    bool completed = false;
    while (std::chrono::steady_clock::now () < completion_deadline) {
        const auto found = std::find_if (
          source_node->pending_remote_source_cleanups.begin (),
          source_node->pending_remote_source_cleanups.end (),
          [] (const auto &candidate) { return candidate.transfer_id == "transfer-leave-notify"; });
        if (found != source_node->pending_remote_source_cleanups.end () && found->leave_completed) {
            completed = true;
            break;
        }
        std::this_thread::yield ();
    }
    if (!completed)
        return 4;

    // A duplicate delivery of the same command must not re-invoke OnLeave.
    auto duplicate_parts = runtime::messaging::envelope_codec_t{}.encode_parts (
      leave_header, leave_command, serializers);
    auto duplicate_sent = target_spots.send_actor_leave_notification (
      zlink::routing_id_t::from ("actor-a"), std::move (duplicate_parts));
    const auto duplicate_result = finite_task_result (std::move (duplicate_sent));
    if (!duplicate_result || !*duplicate_result)
        return 5;
    std::this_thread::sleep_for (std::chrono::milliseconds (50));
    if (leave_calls.load (std::memory_order_acquire) != 1)
        return 6;
    return 0;
}

int known_generation_source_leave_claims_transfer ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace stateful = zlink::framework::runtime::stateful;

    auto node = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::spot_node_builder_state_t> ("actor-a");
    auto entry_spot = std::make_shared<spot_context_state_t> ();
    entry_spot->node = node;
    entry_spot->node_rid = node_rid_t::from_string ("actor-a");
    entry_spot->spot_id = spot_id_t ("actor-a-entry");
    entry_spot->spot_name = "entry";
    entry_spot->spot_instance = std::make_shared<int> (1);
    entry_spot->actor_count = 1;
    auto native_spot = std::make_shared<service::spot_t> (
      nullptr, stateful::object_ref_t{stateful::object_kind_t::user_spot, "actor-a-entry", 7, 0,
                                      "mesh", "actor-a"});
    entry_spot->native_spot = native_spot;
    node->spot_contexts_by_id.emplace (entry_spot->spot_id,
                                       spot_context_access_t::create (entry_spot));

    const auto actor = test_actor_ref ("actor-a", "player", "known-leave-actor", 7);
    const auto key = std::string ("player:known-leave-actor");
    constexpr auto transfer_id = "transfer-known-leave";
    node->actor_instances.emplace (key, std::make_shared<int> (1));
    node->actor_spot_ids.emplace (key, entry_spot->spot_id);
    spot_node_builder_state_t::actor_factory_registration_t factory;
    factory.actor_type = std::type_index (typeid (int));
    node->actor_factories.emplace ("player", std::move (factory));
    if (!node->actor_transfer_coordinator->try_reserve_source (key, transfer_id)
        || !node->actor_transfer_coordinator->try_begin_source_remote (key, transfer_id)) {
        return 1;
    }

    auto authority = std::make_shared<fixed_reconcile_authority_store_t> ();
    authority->record = stateful::authority_relocation_reference_t{
      .source = stateful::object_ref_t{stateful::object_kind_t::actor, "known-leave-actor", 7, 0,
                                       "", "actor-a"},
      .target = stateful::object_ref_t{stateful::object_kind_t::actor, "known-leave-actor", 7, 28,
                                       "", "actor-b"},
      .relocation_reference = transfer_id,
      .target_owner = location_owner_token_t{"owner-b", 9}};
    node->relocation_authority = authority;

    spot_node_runtime_t spots (node);
    const auto fence = runtime::protocol::actor_route_fence_t{
      "known-leave-actor", 7, zlink::routing_id_t::from ("actor-b").to_bytes (), 1, 5, 9};
    const auto result = spots.submit_remote_actor_leave (transfer_id, actor, entry_spot->spot_id, 7,
                                                         spot_id_t ("spot-b-target"), fence);
    if (!result || node->pending_remote_actor_leaves.size () != 1
        || !node->actor_spot_ids.contains (key))
        return 2;
    const auto source_fence = runtime::protocol::actor_route_fence_t{
      "known-leave-actor", 7, zlink::routing_id_t::from ("actor-a").to_bytes (), 1, 1, 8};
    // The original B notification carries generation 5 even when B has
    // returned to current authority generation 28 before source settlement.
    const auto completed = finite_task_result (spots.complete_remote_actor_transfer (
      actor, test_actor_ref ("actor-b", "player", "known-leave-actor", 7),
      {node_rid_t::from_string ("actor-b"), spot_id_t ("spot-b-target"), {}}, source_fence,
      {fence.target_node_routing_id, fence.target_node_generation, "owner-b",
       fence.owner_lease_generation},
      28, transfer_id));
    if (!completed || !*completed || node->pending_remote_source_cleanups.size () != 1
        || !node->pending_remote_source_cleanups.front ().leave_submitted)
        return 3;
    const auto cleanup_at = node->pending_remote_source_cleanups.front ().not_before;
    for (auto duplicate = 0; duplicate < 2; ++duplicate) {
        if (!node->lane
               .run ([&] {
                   const auto &cleanup = node->pending_remote_source_cleanups.front ();
                   return cleanup.leave_submitted && cleanup.leave_completed
                          && node->pending_remote_actor_leaves.empty ()
                          && entry_spot->actor_count == 0;
               })
               .get ())
            return 4;
        if (!node->lane
               .run ([&] {
                   return node->actor_spot_ids.contains (key)
                          && node->actor_spot_ids.at (key) == spot_id_t ("spot-b-target")
                          && node->actor_routes.contains (key)
                          && node->actor_routes.at (key).node_rid.value () == "actor-b"
                          && node->actor_routes.at (key).spot_id == spot_id_t ("spot-b-target");
               })
               .get ())
            return 5;
        const auto follow =
          node->actor_transfer_coordinator->message_follow_target (key, source_fence);
        if (!follow || follow->route.node_rid.value () != "actor-b"
            || follow->route.spot_id != spot_id_t ("spot-b-target")
            || follow->target_fence.authority_owner_generation != 28)
            return 6;
        if (duplicate == 0
            && !spots.submit_remote_actor_leave (transfer_id, actor, entry_spot->spot_id, 7,
                                                 spot_id_t ("spot-b-target"), fence))
            return 7;
    }

    // Advance the existing cleanup boundary without waiting for wall-clock time.
    const auto cleaned =
      finite_task_result (spots.cleanup_expired_actor_admissions_at_async (cleanup_at));
    if (!cleaned || !*cleaned || cleaned->value () != 1)
        return 8;
    if (!node->lane
           .run ([&] {
               return !node->actor_instances.contains (key)
                      && node->pending_remote_source_cleanups.empty ()
                      && node->pending_remote_actor_leaves.empty () && entry_spot->actor_count == 0
                      && node->actor_spot_ids.contains (key)
                      && node->actor_spot_ids.at (key) == spot_id_t ("spot-b-target")
                      && node->actor_routes.contains (key)
                      && node->actor_routes.at (key).node_rid.value () == "actor-b"
                      && node->actor_routes.at (key).spot_id == spot_id_t ("spot-b-target");
           })
           .get ())
        return 9;
    const auto follow = node->actor_transfer_coordinator->message_follow_target (key, source_fence);
    return follow && follow->route.node_rid.value () == "actor-b"
               && follow->route.spot_id == spot_id_t ("spot-b-target")
               && follow->target_fence.authority_owner_generation == 28
             ? 0
             : 10;
}

int early_zero_generation_leave_waits_for_source_transfer_completion (bool no_follow = false)
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace runtime = zlink::framework::runtime;
    namespace stateful = zlink::framework::runtime::stateful;

    // The target-side materialization path cannot project the source Spot
    // lifecycle generation, so its node-level OnLeave command legitimately
    // carries zero. Pin the ordering in which that one-way command reaches
    // the source before complete_remote_actor_transfer has installed the
    // source-cleanup record that resolves the generation. The command must
    // be retained and applied exactly once after the completion registration.
    serializer_registry_t serializers;
    auto node = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::spot_node_builder_state_t> ("actor-a");
    // Message Follow may be disabled independently of membership lifecycle.
    // The early OnLeave must still survive until the source transfer completes.
    node->message_follow_duration = std::chrono::milliseconds (0);
    node->worker_executor = std::make_shared<runtime::offload_executor_t> (1, "early-actor-leave");
    std::mutex blocker_mutex;
    std::condition_variable blocker_changed;
    std::optional<runtime::serial_execution_queue_t::async_completion_t> complete_blocker;
    bool blocker_entered = false;
    const auto release_blocker = [&] {
        std::optional<runtime::serial_execution_queue_t::async_completion_t> completion;
        {
            std::lock_guard lock (blocker_mutex);
            if (complete_blocker) {
                completion.emplace (std::move (*complete_blocker));
                complete_blocker.reset ();
            }
        }
        if (completion)
            (*completion) ([] {});
    };
    const auto finish = [&] (int code) {
        release_blocker ();
        if (node->worker_executor) {
            node->worker_executor->drain ();
            node->worker_executor.reset ();
        }
        return code;
    };
    node->channel_runtime =
      zlink::framework::test::runtime_failure_fixture<channel_runtime_state_t> ();
    node->channel_runtime->serializers = &serializers;

    auto entry_spot = std::make_shared<spot_context_state_t> ();
    entry_spot->node = node;
    entry_spot->node_rid = node_rid_t::from_string ("actor-a");
    entry_spot->spot_id = spot_id_t ("actor-a-entry");
    entry_spot->spot_name = "entry";
    entry_spot->spot_instance = std::make_shared<int> (1);
    entry_spot->channel_runtime =
      zlink::framework::test::runtime_failure_fixture<channel_runtime_state_t> ();
    entry_spot->channel_runtime->serializers = &serializers;
    entry_spot->serial_executor = node->worker_executor;
    runtime::serial_execution_queue_options_t serial_options;
    entry_spot->serial_queue = std::make_shared<runtime::serial_execution_queue_t> (
      *entry_spot->serial_executor, serial_options);
    const auto native_entry_spot = std::make_shared<service::spot_t> (
      nullptr, stateful::object_ref_t{stateful::object_kind_t::user_spot, "actor-a-entry", 7, 0,
                                      "mesh", "actor-a"});
    entry_spot->native_spot = native_entry_spot;
    node->spot_contexts_by_id.emplace (entry_spot->spot_id,
                                       spot_context_access_t::create (entry_spot));

    std::atomic_int leave_calls{0};
    spot_actor_admission_callbacks_t callbacks;
    callbacks.on_leave_actor = [&leave_calls] (void *, void *) -> task_t<void> {
        leave_calls.fetch_add (1, std::memory_order_acq_rel);
        co_return;
    };
    entry_spot->actor_admissions.emplace (std::type_index (typeid (int)), std::move (callbacks));

    spot_node_builder_state_t::actor_factory_registration_t factory;
    factory.actor_type = std::type_index (typeid (int));
    factory.create_instance = [] (std::string) { return std::make_shared<int> (1); };
    factory.configure_instance = [] (void *, const actor_ref_t &, void *) {};
    node->actor_factories.emplace ("player", std::move (factory));

    std::atomic_int source_handler_calls{0};
    entry_spot->handlers.push_back (
      spot_handler_descriptor_t{spot_handler_kind_t::actor_request, "NoFollowProbe", "",
                                std::type_index (typeid (int)), std::type_index (typeid (void)),
                                std::type_index (typeid (int)), std::type_index (typeid (void))});
    entry_spot->handler_invokers.push_back (
      [&] (void *, void *, service_provider_t &, serializer_registry_t &, const zlink::message_t &,
           const spot_inbound_message_t &) -> task_t<zlink::message_t> {
          ++source_handler_calls;
          co_return zlink::message_t{};
      });
    const auto native_source = std::make_shared<host::public_host_runtime_t> (host::host_options_t{
      .mesh = {.descriptor = {.mesh_name = "no-follow",
                              .node_routing_id = zlink::routing_id_t::from ("actor-a").to_bytes (),
                              .lifecycle_generation = 1,
                              .descriptor_revision = 1,
                              .advertised_endpoint = "tcp://127.0.0.1:0"},
               .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()}});
    if (no_follow) {
        node->native_node = native_source;
        auto &objects = native_source->objects ();
        objects.configure_relocation_state (
          [] (const stateful::object_ref_t &, const std::string &, std::stop_token)
            -> zlink::framework::task_t<
              zlink::framework::runtime::stateful::frozen_object_state_t> {
              stateful::frozen_object_state_t captured;
              captured.application_state = std::vector<std::uint8_t>{1};
              co_return captured;
          },
          [] (const stateful::frozen_object_state_t &, const stateful::object_ref_t &,
              std::stop_token) { return true; });
        const stateful::object_ref_t source_object{
          stateful::object_kind_t::actor, "early-leave-actor", 7, 1, "no-follow", "actor-a"};
        const auto reserved = objects.begin_reserved_object (source_object, "player", {});
        if (objects.commit_create (reserved.attempt) != stateful::stateful_error_t::none)
            return finish (20);
        const auto seal =
          objects.try_seal_relocation_aggregate ({source_object}).result ().value ();
        if (seal.error != stateful::stateful_error_t::none
            || objects.begin_relocation_boundary (seal.seal.token, {}).first
                 != stateful::stateful_error_t::none
            || objects.finalize_relocation_cutover (seal.seal.token).result ().value ()
                 != stateful::stateful_error_t::none)
            return finish (21);
    }
    constexpr auto transfer_id = "transfer-early-leave";
    const auto actor = test_actor_ref ("actor-a", "player", "early-leave-actor", 7);
    const auto target_actor = test_actor_ref ("actor-b", "player", "early-leave-actor", 7);
    const auto key = std::string ("player:early-leave-actor");
    node->actor_instances.emplace (key, std::make_shared<int> (1));
    node->actor_spot_ids.emplace (key, entry_spot->spot_id);
    node->actor_generations.emplace (key, 7);
    if (!node->actor_transfer_coordinator->try_reserve_source (key, transfer_id)
        || !node->actor_transfer_coordinator->try_begin_source_remote (key, transfer_id)) {
        return finish (1);
    }

    class fixed_authority_store_t final : public stateful::authority_relocation_port_t
    {
      public:
        zlink::framework::runtime::stateful::source_relocation_settlement_t
        settle_source_relocation (
          const zlink::framework::runtime::stateful::relocation_authority_fence_t &fence,
          const std::vector<zlink::framework::runtime::stateful::object_ref_t> &participants,
          bool preserve) override
        {
            namespace sf = zlink::framework::runtime::stateful;
            (void) preserve;
            sf::source_relocation_settlement_t settled{observe_relocation (fence), {}};
            if (settled.outcome == sf::relocation_authority_t::target_committed) {
                for (const auto &participant : participants) {
                    if (const auto current = read (participant.kind, participant.key))
                        settled.follow_generations.emplace (
                          std::pair{participant.kind, participant.key},
                          current->target.authority_owner_generation);
                }
            }
            return settled;
        }

        std::optional<stateful::authority_relocation_reference_t> record;

        stateful::authority_publish_result_t
        publish (const stateful::object_ref_t &,
                 const stateful::object_ref_t &,
                 location_owner_token_t,
                 object_creation_target_t,
                 std::string,
                 std::uint32_t,
                 stateful::inventory_digest_t,
                 std::vector<std::byte> = {},
                 std::string = {},
                 zlink::framework::runtime::protocol::relocation_id_t = {},
                 location_owner_token_t = {}) override
        {
            return {};
        }
        std::optional<stateful::authority_relocation_reference_t>
        read (stateful::object_kind_t, const std::string &) override
        {
            return record;
        }
    };
    auto authority = std::make_shared<fixed_authority_store_t> ();
    authority->record = stateful::authority_relocation_reference_t{
      .source = stateful::object_ref_t{stateful::object_kind_t::actor, "early-leave-actor", 7, 0,
                                       "", "actor-a"},
      .target = stateful::object_ref_t{stateful::object_kind_t::actor, "early-leave-actor", 7, 5,
                                       "", "actor-b"},
      .relocation_reference = transfer_id,
      .checksum_crc32c = 0,
      .inventory_digest = {},
      .target_owner = location_owner_token_t{"owner-b", 9},
      .application_payload = {}};
    node->relocation_authority = authority;

    const auto source_fence = runtime::protocol::actor_route_fence_t{
      "early-leave-actor", 7, zlink::routing_id_t::from ("actor-a").to_bytes (), 1, 1, 1};
    const auto target_fence = runtime::protocol::actor_route_fence_t{
      "early-leave-actor", 7, zlink::routing_id_t::from ("actor-b").to_bytes (), 1, 5, 9};
    const auto leave_command =
      spot_actor_leave_route_command_t{.transfer_id = transfer_id,
                                       .actor_node_rid = "actor-a",
                                       .actor_type = "player",
                                       .actor_id = "early-leave-actor",
                                       .actor_generation = 7,
                                       .source_spot_id = "actor-a-entry",
                                       .source_spot_generation = 0,
                                       .target_spot_id = "spot-b-target",
                                       .target_node_rid = "actor-b",
                                       .target_node_generation = 1,
                                       .target_authority_owner_generation = 5,
                                       .target_owner_lease_generation = 9};
    runtime::messaging::envelope_header_t leave_header;
    leave_header.kind = runtime::messaging::message_kind_t::command;
    leave_header.channel_name = "node";
    leave_header.message_name = spot_actor_leave_route_command_t::packet_name;

    spot_node_runtime_t spots (node);
    service_collection_t services;
    services.add_singleton<actor_gateway_runtime_t> ();
    auto provider = services.build_provider ();
    spots.set_route_client (route_client_t{});
    const runtime::host::ready_record_t owner{.owner_kind = runtime::host::owner_kind_t::node,
                                              .domain = runtime::host::ready_domain_t::application};
    const auto dispatch_leave = [&] (const spot_actor_leave_route_command_t &command,
                                     std::function<void ()> transfer_owner_reservation = {},
                                     std::size_t transferred_owner_byte_cost = 0) {
        auto encoded =
          runtime::messaging::envelope_codec_t{}.encode_parts (leave_header, command, serializers);
        auto parts = std::move (encoded).take_items ();
        runtime::host::receive_record_t record{.kind = runtime::host::record_kind_t::node_send,
                                               .domain =
                                                 runtime::host::ready_domain_t::application};
        record.source_node_rid = zlink::routing_id_t::from ("actor-b");
        record.transferred_owner_byte_cost = transferred_owner_byte_cost;
        const bool owns_terminal = static_cast<bool> (transfer_owner_reservation);
        bool terminal_deferred = false;
        const auto handled = spots.dispatch_mesh_record (
          owner, record, parts,
          zlink::framework::runtime::decode_mesh_record_header (record, parts, false).value (),
          provider, serializers, std::move (transfer_owner_reservation), &terminal_deferred);
        return handled && (!owns_terminal || terminal_deferred);
    };

    const auto lifecycle =
      runtime::serial_work_options_t{runtime::serial_work_lane_t::lifecycle,
                                     runtime::serial_execution_queue_t::fixed_work_byte_cost};
    if (!entry_spot->serial_queue->try_post_async (
          "block-source-lifecycle",
          [&] (auto complete) {
              std::lock_guard lock (blocker_mutex);
              complete_blocker.emplace (std::move (complete));
              blocker_entered = true;
              blocker_changed.notify_all ();
          },
          lifecycle)) {
        return finish (10);
    }
    {
        std::unique_lock lock (blocker_mutex);
        if (!blocker_changed.wait_for (lock, std::chrono::seconds (2),
                                       [&] { return blocker_entered; })) {
            return finish (11);
        }
    }
    if (!entry_spot->serial_queue->try_post (
          "untransferred-source-lifecycle-behind-blocker", [] {}, lifecycle)) {
        return finish (12);
    }

    std::atomic_int owner_reservation_settles{0};
    constexpr auto transferred_owner_byte_cost =
      runtime::serial_execution_queue_t::fixed_work_byte_cost * 2;
    const auto early = dispatch_leave (
      leave_command, [&] { owner_reservation_settles.fetch_add (1, std::memory_order_release); },
      transferred_owner_byte_cost);
    const auto early_state =
      node->lane
        .run ([&] {
            const auto reservation_retained =
              !node->pending_remote_actor_leaves.empty ()
              && static_cast<bool> (
                node->pending_remote_actor_leaves.front ().transfer_owner_reservation)
              && node->pending_remote_actor_leaves.front ().transferred_owner_byte_cost
                   == transferred_owner_byte_cost;
            return std::make_tuple (node->pending_remote_source_cleanups.size (),
                                    node->pending_remote_actor_leaves.size (),
                                    reservation_retained);
        })
        .get ();
    if (!early || leave_calls.load (std::memory_order_acquire) != 0
        || owner_reservation_settles.load (std::memory_order_acquire) != 0
        || std::get<0> (early_state) != 0 || std::get<1> (early_state) != 1
        || !std::get<2> (early_state)) {
        return finish (2);
    }

    std::atomic_int exact_duplicate_reservation_settles{0};
    const auto exact_duplicate = dispatch_leave (
      leave_command,
      [&] { exact_duplicate_reservation_settles.fetch_add (1, std::memory_order_release); },
      transferred_owner_byte_cost);
    if (!exact_duplicate
        || exact_duplicate_reservation_settles.load (std::memory_order_acquire) != 1
        || owner_reservation_settles.load (std::memory_order_acquire) != 0) {
        return finish (13);
    }

    auto conflicting_command = leave_command;
    conflicting_command.target_spot_id = "spot-b-conflict";
    conflicting_command.target_node_generation = 2;
    std::atomic_int conflicting_reservation_settles{0};
    const auto conflict = dispatch_leave (
      conflicting_command,
      [&] { conflicting_reservation_settles.fetch_add (1, std::memory_order_release); },
      transferred_owner_byte_cost);
    const auto pending_after_conflict =
      node->lane.run ([&] { return node->pending_remote_actor_leaves.size (); }).get ();
    if (!conflict || pending_after_conflict != 1
        || conflicting_reservation_settles.load (std::memory_order_acquire) != 1)
        return finish (8);

    const auto swept_active = spots.cleanup_expired_actor_admissions_at (
      std::chrono::steady_clock::now () + std::chrono::seconds (31));
    const auto pending_after_active_sweep =
      node->lane.run ([&] { return node->pending_remote_actor_leaves.size (); }).get ();
    if (swept_active != 0 || pending_after_active_sweep != 1)
        return finish (9);

    const auto completed = finite_task_result (spots.complete_remote_actor_transfer (
      actor, target_actor,
      spot_route_t{node_rid_t::from_string ("actor-b"), spot_id_t ("spot-b-target"), "game"},
      source_fence,
      {target_fence.target_node_routing_id,
       target_fence.target_node_generation,
       {},
       target_fence.owner_lease_generation},
      no_follow ? std::nullopt
                : std::optional<std::uint64_t>{target_fence.authority_owner_generation},
      transfer_id));
    if (!completed || !*completed)
        return finish (3);

    if (no_follow) {
        service_collection_t services;
        auto provider = services.build_provider ();
        actor_gateway_runtime_t gateway;
        const auto late = finite_task_result (spots.relay_actor_packet (
          actor, gateway.actor_context (actor), stream_message_kind_t::request, "NoFollowProbe",
          zlink::message_t{}, provider, serializers));
        if (!late || *late
            || (!late->error () || late->error ()->kind () != framework_error_kind_t::unavailable)
            || source_handler_calls.load () != 0
            || node->actor_transfer_coordinator->phase (key).has_value ())
            return finish (22);
    }
    const auto admitted_leave_state =
      node->lane
        .run ([&] {
            const auto cleanup = std::find_if (
              node->pending_remote_source_cleanups.begin (),
              node->pending_remote_source_cleanups.end (),
              [&] (const auto &candidate) { return candidate.transfer_id == transfer_id; });
            return std::make_tuple (
              cleanup != node->pending_remote_source_cleanups.end () && cleanup->leave_submitted,
              cleanup != node->pending_remote_source_cleanups.end () && cleanup->leave_completed,
              node->pending_remote_actor_leaves.size ());
        })
        .get ();
    if (owner_reservation_settles.load (std::memory_order_acquire) != 1
        || leave_calls.load (std::memory_order_acquire) != 0
        || entry_spot->serial_queue->pending_count (runtime::serial_work_lane_t::lifecycle) != 3
        || !std::get<0> (admitted_leave_state) || std::get<1> (admitted_leave_state)
        || std::get<2> (admitted_leave_state) != 0) {
        return finish (14);
    }
    release_blocker ();

    const auto leave_deadline = std::chrono::steady_clock::now () + std::chrono::seconds (2);
    struct leave_state_t
    {
        bool cleanup_present = false;
        bool leave_submitted = false;
        bool leave_completed = false;
        std::size_t pending_leaves = 0;
    };
    leave_state_t leave_state;
    do {
        leave_state =
          node->lane
            .run ([&] {
                leave_state_t state;
                const auto cleanup = std::find_if (
                  node->pending_remote_source_cleanups.begin (),
                  node->pending_remote_source_cleanups.end (),
                  [&] (const auto &candidate) { return candidate.transfer_id == transfer_id; });
                state.cleanup_present = cleanup != node->pending_remote_source_cleanups.end ();
                if (state.cleanup_present) {
                    state.leave_submitted = cleanup->leave_submitted;
                    state.leave_completed = cleanup->leave_completed;
                }
                state.pending_leaves = node->pending_remote_actor_leaves.size ();
                return state;
            })
            .get ();
        if (leave_calls.load (std::memory_order_acquire) == 1 && leave_state.cleanup_present
            && leave_state.leave_submitted && leave_state.leave_completed
            && leave_state.pending_leaves == 0) {
            break;
        }
        std::this_thread::yield ();
    } while (std::chrono::steady_clock::now () < leave_deadline);
    if (leave_calls.load (std::memory_order_acquire) != 1 || !leave_state.cleanup_present
        || !leave_state.leave_submitted || !leave_state.leave_completed
        || leave_state.pending_leaves != 0) {
        return finish (4);
    }

    const auto duplicate = dispatch_leave (leave_command);
    if (!duplicate)
        return finish (5);
    node->worker_executor->drain ();
    if (leave_calls.load (std::memory_order_acquire) != 1)
        return finish (6);

    (void) spots.cleanup_expired_actor_admissions_at (std::chrono::steady_clock::now ()
                                                      + std::chrono::seconds (31));
    const auto late_duplicate = dispatch_leave (leave_command);
    const auto pending_after_late_duplicate =
      node->lane.run ([&] { return node->pending_remote_actor_leaves.size (); }).get ();
    return finish (late_duplicate && pending_after_late_duplicate == 0 ? 0 : 7);
}

int rebound_session_keeps_prior_ingress_exact_fence ()
{
    using namespace zlink::framework::runtime::stateful;

    const object_ref_t actor{object_kind_t::actor, "reconnect-player", 1, 7, "player", "node-a"};
    stream_session_registry_t sessions (
      [actor] (const std::string &actor_id) -> std::optional<object_ref_t> {
          return actor_id == actor.key ? std::make_optional (actor) : std::nullopt;
      });
    const auto connection = sessions.open ("reconnect-session");
    const auto [first_error, first] = sessions.bind (connection, actor, 11, 13);
    if (first_error != stateful_error_t::none)
        return 1;
    const auto [admit_error, admitted] = sessions.admit_inbound (first);
    if (admit_error != stateful_error_t::none || !admitted)
        return 2;

    const auto [rebind_error, rebound] = sessions.bind (connection, actor, 11, 13);
    if (rebind_error != stateful_error_t::none
        || rebound.binding_generation == first.binding_generation) {
        return 3;
    }
    const auto seal =
      sessions.seal_remote_route (connection.connection_id, rebound.binding_generation, actor.key,
                                  actor.object_generation, rebound.owner_lease_generation);
    if (seal.error != stateful_error_t::backpressured
        || sessions.remote_route_seal_ready (seal.barrier)) {
        return 4;
    }
    if (sessions.complete_inbound (*admitted) != stateful_error_t::none
        || !sessions.remote_route_seal_ready (seal.barrier)) {
        return 5;
    }
    return sessions.abort_barrier (seal.barrier) == stateful_error_t::none ? 0 : 6;
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
    const auto actor = test_actor_ref ("actor-owner", "player", "two-host-reconnect", 7);
    const auto session_owner_rid = zlink::routing_id_t::from ("session-owner");
    const zlink::framework::runtime::stateful::object_ref_t session_actor{
      zlink::framework::runtime::stateful::object_kind_t::actor,
      "two-host-reconnect",
      7,
      13,
      "player",
      "actor-owner"};
    zlink::framework::runtime::stateful::stream_session_registry_t stream_sessions (
      [session_actor] (const std::string &actor_id)
        -> std::optional<zlink::framework::runtime::stateful::object_ref_t> {
          return actor_id == session_actor.key ? std::make_optional (session_actor) : std::nullopt;
      });
    const auto old_connection = stream_sessions.open ("same-session-rid");
    const auto [old_bind_error, old_binding] =
      stream_sessions.bind_remote (old_connection, session_actor, 11, 17);
    const auto new_connection = stream_sessions.open ("same-session-rid");
    const auto [new_bind_error, new_binding] =
      stream_sessions.bind_remote (new_connection, session_actor, 11, 17, {});
    if (old_bind_error != zlink::framework::runtime::stateful::stateful_error_t::none
        || new_bind_error != zlink::framework::runtime::stateful::stateful_error_t::none
        || new_binding.binding_generation <= old_binding.binding_generation) {
        return 1;
    }
    const actor_bound_session_route_t old_route{session_owner_rid,
                                                zlink::routing_id_t::from ("same-session-rid"),
                                                7,
                                                11,
                                                13,
                                                17,
                                                old_binding.binding_generation,
                                                0,
                                                0};
    const actor_bound_session_route_t new_route{session_owner_rid,
                                                zlink::routing_id_t::from ("same-session-rid"),
                                                7,
                                                11,
                                                13,
                                                17,
                                                new_binding.binding_generation,
                                                0,
                                                0};

    // The sinks can run after this scope ends, so they own their counters and gateway handles.
    auto old_client_received = std::make_shared<std::atomic_int> (0);
    auto new_client_received = std::make_shared<std::atomic_int> (0);
    if (!session_owner.replace_session_route (
          actor,
          [old_client_received] (std::string, stream_codec_t, const zlink::message_t &) {
              old_client_received->fetch_add (1, std::memory_order_acq_rel);
              return task_t<void> (result_t<void>::success ());
          },
          old_route)) {
        return 2;
    }

    // The gateway state outlives this scope, so the observer owns what it reads
    // and writes.
    auto saw_correlated_split = std::make_shared<std::atomic_bool> (false);
    const auto expected_current = "current=session_rid=" + new_route.session_rid->to_hex ()
                                  + "/bg=" + std::to_string (new_binding.binding_generation);
    const auto expected_staged = "staged=session_rid=" + old_route.session_rid->to_hex ()
                                 + "/bg=" + std::to_string (old_binding.binding_generation);
    dispatch_options_t trace_options;
    trace_options.message_flow (message_flow_log_mode_t::detailed);
    dispatch_options_access_t::set_observer_for_tests (
      trace_options, [saw_correlated_split, expected_current,
                      expected_staged] (const message_flow_event_t &event) {
          if (event.outcome == message_flow_outcome_t::admitted
              && event.surface == dispatch_error_surface_t::stream_session
              && event.message_kind == dispatch_message_kind_t::send
              && event.detail_stage == std::optional<std::string> ("actor_owner_push_target")
              && event.detail_result
              && event.detail_result->find (expected_current) != std::string::npos
              && event.detail_result->find (expected_staged) != std::string::npos) {
              saw_correlated_split->store (true, std::memory_order_release);
          }
      });
    actor_owner.set_dispatch (std::move (trace_options));

    // The sink lives in the actor owner's state, so it refers back to that
    // state weakly instead of holding it.
    const std::weak_ptr<actor_gateway_state_t> weak_actor_owner = actor_owner_state;
    const auto make_remote_sink = [weak_actor_owner, session_owner,
                                   actor] (actor_bound_session_route_t staged_route) {
        return [weak_actor_owner, session_owner, actor, staged_route = std::move (staged_route)] (
                 std::string packet_name, stream_codec_t codec,
                 const zlink::message_t &payload) mutable -> task_t<void> {
            const auto owner_state = weak_actor_owner.lock ();
            if (!owner_state) {
                throw framework_exception_t (framework_error_kind_t::not_configured,
                                             "actor owner is gone");
            }
            const auto current = actor_gateway_runtime_t (owner_state)
                                   .resolve_bound_session_push_route (actor, staged_route);
            if (!current) {
                throw framework_exception_t (framework_error_kind_t::not_configured,
                                             "current reconnect route is unavailable");
            }
            auto delivery =
              session_owner.admit_bound_session_delivery (actor, current->binding_generation);
            if (!delivery) {
                throw framework_exception_t (framework_error_kind_t::not_configured,
                                             "new Session host rejected the push");
            }
            const auto delivered = (*delivery) (std::move (packet_name), codec, payload);
            if (!delivered) {
                throw delivered.error ()
                  ? *delivered.error ()
                  : framework_exception_t (framework_error_kind_t::internal_failure,
                                           "new Session client delivery failed");
            }
            co_return;
        };
    };

    if (!actor_owner.replace_session_route (actor, make_remote_sink (old_route), old_route)) {
        return 3;
    }
    const auto staged_old_sink = actor_owner_state->sync (
      [&] { return actor_owner_state->bound_session_sinks.at ("two-host-reconnect"); });

    if (!session_owner.replace_session_route (
          actor,
          [new_client_received] (std::string, stream_codec_t, const zlink::message_t &) {
              new_client_received->fetch_add (1, std::memory_order_acq_rel);
              return task_t<void> (result_t<void>::success ());
          },
          new_route)
        || !actor_owner.replace_session_route (actor, make_remote_sink (new_route), new_route)) {
        return 4;
    }
    const auto stale_capability_push =
      (*staged_old_sink) ("reconnected-push", stream_codec_t::message_pack,
                          zlink::message_t::from ("payload"))
        .result ();
    if (!stale_capability_push)
        return 6;

    const auto public_push = actor_owner.actor_context (actor)
                               .bound_session ()
                               .send (std::string ("public-push"))
                               .async ()
                               .result ();
    if (!public_push)
        return 7;
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (1);
    while ((new_client_received->load (std::memory_order_acquire) != 2
            || !saw_correlated_split->load (std::memory_order_acquire))
           && std::chrono::steady_clock::now () < deadline) {
        std::this_thread::yield ();
    }
    return old_client_received->load (std::memory_order_acquire) == 0
               && new_client_received->load (std::memory_order_acquire) == 2
               && saw_correlated_split->load (std::memory_order_acquire)
             ? 0
             : 8;
}

int old_stream_disconnect_does_not_retire_reconnected_binding ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    using namespace zlink::framework::runtime::stateful;

    const object_ref_t native_actor{
      object_kind_t::actor, "disconnect-fenced-actor", 7, 13, "player", "actor-owner"};
    stream_session_registry_t sessions (
      [native_actor] (const std::string &actor_id) -> std::optional<object_ref_t> {
          return actor_id == native_actor.key ? std::make_optional (native_actor) : std::nullopt;
      });
    const auto old_connection = sessions.open ("old-stream-rid");
    const auto [old_error, old_binding] =
      sessions.bind_remote (old_connection, native_actor, 11, 17);
    const auto disconnect_snapshot = sessions.bindings (old_connection);
    const auto new_connection = sessions.open ("new-stream-rid");
    const auto [new_error, new_binding] =
      sessions.bind_remote (new_connection, native_actor, 11, 17);
    const auto current_binding = sessions.current_binding (native_actor.key);
    if (old_error != stateful_error_t::none || new_error != stateful_error_t::none
        || disconnect_snapshot.size () != 1 || !sessions.bindings (old_connection).empty ()
        || sessions.bindings (new_connection).size () != 1 || !current_binding
        || *current_binding != new_binding || current_binding->connection != new_connection
        || current_binding->connection == old_connection
        || new_binding.binding_generation <= old_binding.binding_generation) {
        return 1;
    }

    actor_gateway_runtime_t gateway;
    const auto actor = test_actor_ref ("actor-owner", "player", native_actor.key, 7);
    const auto session_owner = zlink::routing_id_t::from ("session-owner");
    const auto old_rid = zlink::routing_id_t::from ("old-stream-rid");
    const auto new_rid = zlink::routing_id_t::from ("new-stream-rid");
    std::atomic_int new_stream_received{0};
    if (!gateway.replace_session_route (
          actor,
          [] (std::string, stream_codec_t, const zlink::message_t &) {
              return task_t<void> (result_t<void>::success ());
          },
          actor_bound_session_route_t{session_owner, old_rid, 7, 11, 13, 17,
                                      old_binding.binding_generation, 0, 0})
        || !gateway.replace_session_route (
          actor,
          [&new_stream_received] (std::string, stream_codec_t, const zlink::message_t &) {
              ++new_stream_received;
              return task_t<void> (result_t<void>::success ());
          },
          actor_bound_session_route_t{session_owner, new_rid, 7, 11, 13, 17,
                                      new_binding.binding_generation, 0, 0})) {
        return 2;
    }

    for (const auto &retiring : disconnect_snapshot) {
        if (sessions.is_current (retiring)) {
            (void) gateway.retire_bound_session_route (actor, session_owner, old_rid,
                                                       retiring.binding_generation);
        }
    }
    if (gateway.retire_bound_session_route (actor, session_owner, old_rid,
                                            old_binding.binding_generation)) {
        return 3;
    }
    const auto current = sessions.current_binding (native_actor.key);
    const auto route = gateway.bound_session_route_async (actor).result ().value ();
    const auto delivered = gateway.dispatch_bound_session_send (
      actor, "after-reconnect", stream_codec_t::message_pack, zlink::message_t::from ("payload"));
    return current && *current == new_binding && route && route->session_rid == new_rid
               && route->binding_generation == new_binding.binding_generation && delivered
               && new_stream_received.load () == 1
             ? 0
             : 4;
}

int command_38_rebind_is_owned_only_by_new_connection ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    using namespace zlink::framework::runtime;

    const stateful::object_ref_t native_actor{
      stateful::object_kind_t::actor, "command-38-rebind-actor", 7, 13, "player", "actor-owner"};
    stateful::stream_session_registry_t sessions (
      [native_actor] (const std::string &actor_id) -> std::optional<stateful::object_ref_t> {
          return actor_id == native_actor.key ? std::make_optional (native_actor) : std::nullopt;
      });
    actor_gateway_runtime_t gateway;
    const auto actor = test_actor_ref ("actor-owner", "player", native_actor.key, 7);
    const auto session_owner = zlink::routing_id_t::from ("session-owner");

    const auto receive_command_38 = [&] (const protocol::bound_session_bind_t &outbound) {
        const auto received =
          protocol::decode_bound_session_bind (protocol::encode_bound_session_bind (outbound));
        const auto session_rid = zlink::routing_id_t::from (received.session_routing_id);
        return gateway.record_bound_session_route (
          actor, session_owner, session_rid, 11, received.actor.authority_owner_generation,
          received.actor.owner_lease_generation, received.binding.generation, 0, 0);
    };
    const auto bind_through_native_and_38 =
      [&] (const stateful::stream_connection_t &connection, const zlink::routing_id_t &session_rid,
           std::uint64_t binding_generation) -> std::optional<stateful::stream_binding_t> {
        auto [error, binding] =
          sessions.bind_remote (connection, native_actor, 11, 17, {}, binding_generation);
        if (error != stateful::stateful_error_t::none)
            return std::nullopt;
        const protocol::bound_session_bind_t command_38{
          binding.binding_generation,
          protocol::actor_route_fence_t{
            native_actor.key, native_actor.object_generation,
            zlink::routing_id_t::from (native_actor.node_id).to_bytes (), 11,
            native_actor.authority_owner_generation, 17},
          session_rid.to_bytes (),
          {protocol::bound_session_binding_state_t::active, binding.binding_generation}};
        if (!receive_command_38 (command_38))
            return std::nullopt;
        return binding;
    };

    const auto old_connection = sessions.open ("command-38-old");
    const auto old_binding =
      bind_through_native_and_38 (old_connection, zlink::routing_id_t::from ("session-old"), 1);
    const auto new_connection = sessions.open ("command-38-new");
    const auto new_binding =
      bind_through_native_and_38 (new_connection, zlink::routing_id_t::from ("session-new"), 2);
    if (!old_binding || !new_binding)
        return 1;
    const auto current_binding = sessions.current_binding (native_actor.key);
    const auto route = gateway.bound_session_route_async (actor).result ().value ();
    return sessions.bindings (old_connection).empty ()
               && sessions.bindings (new_connection).size () == 1 && current_binding
               && *current_binding == *new_binding && current_binding->connection == new_connection
               && current_binding->connection != old_connection && route
               && route->session_rid == zlink::routing_id_t::from ("session-new")
               && route->binding_generation == 2
             ? 0
             : 2;
}

int late_lower_generation_bind_and_record_are_rejected ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    using namespace zlink::framework::runtime::stateful;

    const object_ref_t native_actor{
      object_kind_t::actor, "monotonic-binding-actor", 7, 13, "player", "actor-owner"};
    stream_session_registry_t sessions (
      [native_actor] (const std::string &actor_id) -> std::optional<object_ref_t> {
          return actor_id == native_actor.key ? std::make_optional (native_actor) : std::nullopt;
      });
    const auto current_connection = sessions.open ("current-stream-rid");
    const auto [current_error, current_binding] =
      sessions.bind_remote (current_connection, native_actor, 11, 17, {}, 22);
    const auto stale_connection = sessions.open ("stale-stream-rid");
    const auto [stale_error, stale_binding] =
      sessions.bind_remote (stale_connection, native_actor, 11, 17, {}, 21);
    const auto registry_current = sessions.current_binding (native_actor.key);
    if (current_error != stateful_error_t::none || stale_error != stateful_error_t::conflict
        || stale_binding.binding_generation != 0 || !registry_current
        || *registry_current != current_binding) {
        return 1;
    }

    actor_gateway_runtime_t gateway;
    const auto actor = test_actor_ref ("actor-owner", "player", native_actor.key, 7);
    const auto session_owner = zlink::routing_id_t::from ("session-owner");
    const auto current_rid = zlink::routing_id_t::from ("current-stream-rid");
    const auto stale_rid = zlink::routing_id_t::from ("stale-stream-rid");
    std::atomic_int current_stream_received{0};
    std::atomic_int stale_stream_received{0};
    if (!gateway.replace_session_route (
          actor,
          [&current_stream_received] (std::string, stream_codec_t, const zlink::message_t &) {
              ++current_stream_received;
              return task_t<void> (result_t<void>::success ());
          },
          actor_bound_session_route_t{session_owner, current_rid, 7, 11, 13, 17, 22, 0, 0})) {
        return 2;
    }
    const auto stale_publish = gateway.replace_session_route (
      actor,
      [&stale_stream_received] (std::string, stream_codec_t, const zlink::message_t &) {
          ++stale_stream_received;
          return task_t<void> (result_t<void>::success ());
      },
      actor_bound_session_route_t{session_owner, stale_rid, 7, 11, 13, 17, 21, 0, 0});
    const auto stale_record = gateway.record_bound_session_route_transition (
      actor, actor_bound_session_route_t{session_owner, stale_rid, 7, 11, 13, 17, 20, 0, 0});
    const auto route = gateway.bound_session_route_async (actor).result ().value ();
    const auto delivered = gateway.dispatch_bound_session_send (actor, "after-stale-publish",
                                                                stream_codec_t::message_pack,
                                                                zlink::message_t::from ("payload"));
    return !stale_publish && stale_publish.error_kind () == framework_error_kind_t::unavailable
               && !stale_record && stale_record.error_kind () == framework_error_kind_t::unavailable
               && route && route->session_rid == current_rid && route->binding_generation == 22
               && delivered && current_stream_received.load () == 1
               && stale_stream_received.load () == 0
             ? 0
             : 3;
}

int same_identity_registration_keeps_logical_binding_current ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    using namespace zlink::framework::runtime::stateful;

    const object_ref_t native_actor{object_kind_t::actor, "defensive-rebind", 7, 13, "player",
                                    "actor-owner"};
    stream_session_registry_t sessions (
      [native_actor] (const std::string &actor_id) -> std::optional<object_ref_t> {
          return actor_id == native_actor.key ? std::make_optional (native_actor) : std::nullopt;
      });
    const auto old_connection = sessions.open ("reused-session-rid");
    const auto [old_error, old_binding] =
      sessions.bind_remote (old_connection, native_actor, 11, 17, {}, 41);
    const auto new_connection = sessions.open ("reused-session-rid");
    const auto [new_error, new_binding] =
      sessions.bind_remote (new_connection, native_actor, 11, 17, {}, 41);
    const auto current = sessions.current_binding (native_actor.key);
    if (old_error != stateful_error_t::none || new_error != stateful_error_t::none || !current
        || current->connection != new_connection || current->binding_generation != 41)
        return 1;

    actor_gateway_runtime_t gateway;
    const auto actor = test_actor_ref ("actor-owner", "player", "defensive-rebind", 7);
    const auto owner_rid = zlink::routing_id_t::from ("session-owner");
    const auto session_rid = zlink::routing_id_t::from ("reused-session-rid");
    std::atomic_int old_stream_received{0};
    std::atomic_int new_stream_received{0};
    const actor_bound_session_route_t old_route{owner_rid, session_rid, 7, 11, 13, 17, 41, 1, 0};
    const actor_bound_session_route_t new_route{owner_rid, session_rid, 7, 11, 13, 17, 41, 2, 0};
    if (!gateway.replace_session_route (
          actor,
          [&old_stream_received] (std::string, stream_codec_t, const zlink::message_t &) {
              ++old_stream_received;
              return task_t<void> (result_t<void>::success ());
          },
          old_route))
        return 2;
    const auto replaced = gateway.replace_session_route (
      actor,
      [&new_stream_received] (std::string, stream_codec_t, const zlink::message_t &) {
          ++new_stream_received;
          return task_t<void> (result_t<void>::success ());
      },
      new_route);
    // Session-Actor binding §5: a local delivery token is not another binding
    // identity. Refreshing its sink does not replace the logical binding.
    if (!replaced || replaced.value ().changed || replaced.value ().previous
        || !replaced.value ().current
        || replaced.value ().current->binding_token != old_route.binding_token)
        return 3;
    const auto delivered = gateway.dispatch_bound_session_send (
      actor, "defensive-push", stream_codec_t::message_pack, zlink::message_t::from ("payload"));
    return delivered && old_stream_received.load () == 0 && new_stream_received.load () == 1 ? 0
                                                                                             : 4;
}

int same_rid_registration_retains_retired_close_handler ()
{
    using namespace zlink::framework::detail;

    actor_gateway_runtime_t gateway;
    const auto session_rid = zlink::routing_id_t::from ("reused-handler-rid");
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
    replacement.retired_session.session_routing_id = session_rid.to_bytes ();
    if (!gateway.dispatch_bound_session_replaced (replacement) || retired_calls.load () != 1
        || current_calls.load () != 0)
        return 1;
    gateway.unregister_bound_session_replacement_handler (session_rid, retired);
    if (!gateway.dispatch_bound_session_replaced (replacement) || retired_calls.load () != 1
        || current_calls.load () != 1)
        return 2;
    gateway.unregister_bound_session_replacement_handler (session_rid, current);
    return !gateway.dispatch_bound_session_replaced (replacement) ? 0 : 3;
}

int remote_actor_join_resolves_store_type_and_reports_typed_terminals ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace runtime = zlink::framework::runtime;

    enum class scenario_t
    {
        matching,
        no_store,
        fence_mismatch,
        missing,
        unreadable,
        forged,
        no_factory,
        reject_while_source_remote,
        accept_while_source_remote
    };
    const auto run = [] (scenario_t scenario,
                         std::atomic_int &admission_calls) -> result_t<spot_actor_join_result_t> {
        serializer_registry_t serializers;
        auto node = zlink::framework::test::runtime_failure_fixture<
          zlink::framework::detail::spot_node_builder_state_t> ("actor-join-target");
        node->worker_executor =
          std::make_shared<runtime::offload_executor_t> (1, "actor-join-store-resolution");
        node->channel_runtime =
          zlink::framework::test::runtime_failure_fixture<channel_runtime_state_t> ();
        node->channel_runtime->serializers = &serializers;

        auto spot = std::make_shared<spot_context_state_t> ();
        spot->node = node;
        spot->node_rid = node_rid_t::from_string ("actor-join-target");
        spot->spot_id = spot_id_t ("actor-join-target-spot");
        spot->spot_name = "actor-join-target";
        spot->spot_instance = std::make_shared<int> (1);
        spot->channel_runtime =
          zlink::framework::test::runtime_failure_fixture<channel_runtime_state_t> ();
        spot->channel_runtime->serializers = &serializers;
        spot->serial_executor = node->worker_executor;
        node->spot_contexts_by_id.emplace (spot->spot_id, spot_context_access_t::create (spot));

        spot_actor_admission_callbacks_t callbacks;
        callbacks.join = [&admission_calls, scenario] (void *, std::string_view,
                                                       const zlink::message_t &,
                                                       serializer_registry_t &) {
            admission_calls.fetch_add (1, std::memory_order_acq_rel);
            return scenario == scenario_t::reject_while_source_remote
                     ? spot_actor_join_result_t::reject ()
                     : spot_actor_join_result_t::accept ();
        };
        spot->actor_admissions.emplace (std::type_index (typeid (int)), std::move (callbacks));

        if (scenario != scenario_t::no_factory) {
            spot_node_builder_state_t::actor_factory_registration_t factory;
            factory.actor_type = std::type_index (typeid (int));
            factory.create_instance = [] (std::string) { return std::make_shared<int> (1); };
            factory.configure_instance = [] (void *, const actor_ref_t &, void *) {};
            node->actor_factories.emplace ("StoreActor", std::move (factory));
        }

        const auto stored_actor =
          test_actor_ref ("actor-owner", "StoreActor", "store-resolved-actor", 17);
        auto store = std::make_shared<actor_join_authority_store_t> ();
        if (scenario != scenario_t::missing) {
            store->snapshot = authority_snapshot_t{
              .store_version = "v1",
              .payload = runtime::encode_actor_authority_payload (stored_actor, "source-spot", 1),
              .object_generation = 17,
              .authority_owner_generation = 23,
              .owner = location_owner_token_t{"source-owner", 29},
              .store_now = std::chrono::system_clock::now (),
              .allocation = {.state = placement_allocation_state_t::active,
                             .object_kind = placement_object_kind_t::actor,
                             .stable_type = "StoreActor",
                             .target = {.mesh_name = "mesh",
                                        .node_rid = node_rid_t::from_string ("actor-owner"),
                                        .node_lifecycle_generation = 19,
                                        .owner = location_owner_token_t{"source-owner", 29}}}};
            if (scenario == scenario_t::fence_mismatch)
                store->snapshot->allocation.target.node_lifecycle_generation = 21;
        }
        store->unreadable = scenario == scenario_t::unreadable;

        spot_node_runtime_t spots (node);
        if (scenario != scenario_t::no_store) {
            service_collection_t services;
            services.add_factory<runtime::live_location_reader_t> (
              [store] (service_provider_t &) {
                  return std::make_unique<runtime::live_location_reader_t> (*store);
              },
              service_lifetime_t::singleton);
            auto provider = services.build_provider ();
            spots.bind_service_provider (provider);
        }

        const auto wire_actor = test_actor_ref (
          "actor-owner", scenario == scenario_t::forged ? "ForgedActor" : "StoreActor",
          "store-resolved-actor", 17);
        if (scenario == scenario_t::reject_while_source_remote
            || scenario == scenario_t::accept_while_source_remote) {
            if (!node->actor_transfer_coordinator->try_begin_source_remote (
                  "StoreActor:store-resolved-actor", "previous-source-transfer"))
                return result_t<spot_actor_join_result_t>::failure (
                  framework_error_kind_t::internal_failure, "source transfer setup failed");
        }
        return spots
          .admit_remote_actor_to_spot (
            "store-resolution-" + std::to_string (static_cast<int> (scenario)), wire_actor,
            spot_id_t ("source-spot"), spot->spot_id, zlink::message_t{}, 1, 2, 23, 19, 29)
          .result ();
    };

    std::atomic_int admission_calls{0};
    const auto matching = run (scenario_t::matching, admission_calls);
    if (!matching || !matching.value ().accepted || admission_calls.load () != 1)
        return 1;

    const auto expect_terminal =
      [&run, &admission_calls] (scenario_t scenario, framework_error_kind_t expected, int failure) {
          const auto result = run (scenario, admission_calls);
          return !result && result.error_kind () == expected ? 0 : failure;
      };
    if (const auto mismatch =
          expect_terminal (scenario_t::no_store, framework_error_kind_t::unavailable, 2);
        mismatch != 0)
        return mismatch;
    if (const auto mismatch =
          expect_terminal (scenario_t::fence_mismatch, framework_error_kind_t::protocol_error, 3);
        mismatch != 0)
        return mismatch;
    if (const auto missing =
          expect_terminal (scenario_t::missing, framework_error_kind_t::not_found, 4);
        missing != 0)
        return missing;
    if (const auto unreadable =
          expect_terminal (scenario_t::unreadable, framework_error_kind_t::unavailable, 5);
        unreadable != 0)
        return unreadable;
    if (const auto forged =
          expect_terminal (scenario_t::forged, framework_error_kind_t::type_mismatch, 6);
        forged != 0)
        return forged;
    if (const auto no_factory =
          expect_terminal (scenario_t::no_factory, framework_error_kind_t::rejected, 7);
        no_factory != 0)
        return no_factory;
    const auto rejected = run (scenario_t::reject_while_source_remote, admission_calls);
    if (!rejected || rejected.value ().accepted)
        return 9;
    if (const auto conflict = expect_terminal (scenario_t::accept_while_source_remote,
                                               framework_error_kind_t::unavailable, 10);
        conflict != 0)
        return conflict;
    return admission_calls.load () == 3 ? 0 : 8;
}

/* A Location Store whose reads complete on the provider's own thread, as a
 * networked provider does. */
class provider_thread_location_store_t final : public zlink::framework::location_store_t
{
  public:
    ~provider_thread_location_store_t () override
    {
        for (auto &completer : _completers)
            completer.join ();
    }

    zlink::framework::task_t<zlink::framework::store_read_result_t>
    read (zlink::framework::store_key_t key) override
    {
        using namespace zlink::framework;
        auto completion = std::make_shared<task_completion_source_t<store_read_result_t>> ();
        auto pending = completion->task ();
        auto value = inner.read (std::move (key)).result ();
        std::lock_guard lock (_mutex);
        _completers.emplace_back ([completion, value] { completion->complete (value); });
        return pending;
    }
    zlink::framework::task_t<zlink::framework::store_scan_result_t>
    scan (zlink::framework::store_scan_request_t request) override
    {
        return inner.scan (std::move (request));
    }
    zlink::framework::task_t<zlink::framework::store_write_result_t>
    write (zlink::framework::store_write_request_t request) override
    {
        return inner.write (std::move (request));
    }

    zlink::framework::runtime::in_memory_location_store_t inner;

  private:
    std::mutex _mutex;
    std::vector<std::thread> _completers;
};

/* Execution gate §1, §13: the actorJoin admission reads the Authority row
 * from an infrastructure thread. That read must finish while every handler
 * executor worker is busy; before #1380's follow-up its Store continuations
 * were queued behind those workers and the admission never returned. */
int remote_actor_join_store_read_does_not_wait_for_handler_workers ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace runtime = zlink::framework::runtime;
    using namespace std::chrono_literals;

    serializer_registry_t serializers;
    auto node = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::spot_node_builder_state_t> ("actor-join-infrastructure");
    node->worker_executor =
      std::make_shared<runtime::offload_executor_t> (1, "actor-join-infrastructure");
    node->channel_runtime =
      zlink::framework::test::runtime_failure_fixture<channel_runtime_state_t> ();
    node->channel_runtime->serializers = &serializers;

    provider_thread_location_store_t provider;
    runtime::provider_location_repository_t repository (provider);
    spot_node_runtime_t spots (node);
    service_collection_t services;
    services.add_factory<runtime::live_location_reader_t> (
      [&repository] (service_provider_t &) {
          return std::make_unique<runtime::live_location_reader_t> (repository);
      },
      service_lifetime_t::singleton);
    auto provider_services = services.build_provider ();
    spots.bind_service_provider (provider_services);

    runtime::configure_handler_coroutine_executor (1);
    runtime::install_host_context_hooks ();
    std::promise<void> worker_entered;
    std::promise<void> release_worker;
    auto released = release_worker.get_future ().share ();
    runtime::handler_coroutine_executor ().post_native_continuation ([&worker_entered, released] {
        worker_entered.set_value ();
        released.wait ();
    });
    worker_entered.get_future ().wait ();

    std::promise<std::pair<framework_error_kind_t, framework_error_kind_t>> outcome;
    auto finished = outcome.get_future ();
    std::thread infrastructure ([&] {
        const auto admitted =
          spots
            .admit_remote_actor_to_spot ("infrastructure-store-read",
                                         test_actor_ref ("actor-owner", "", "absent-actor", 17),
                                         spot_id_t ("source-spot"), spot_id_t ("target-spot"),
                                         zlink::message_t{}, 1, 2, 23, 19, 29, true)
            .result ();
        runtime::protocol::spot_route_fence_t fence;
        fence.spot_id = "absent-spot";
        fence.object_generation = 3;
        fence.target_node_routing_id = {1};
        fence.target_node_generation = 5;
        fence.authority_owner_generation = 7;
        fence.owner_lease_generation = 9;
        const auto target = spots.resolve_wire_actor_join_target (fence);
        outcome.set_value (
          {admitted ? framework_error_kind_t::internal_failure : admitted.error_kind (),
           target ? framework_error_kind_t::internal_failure : target.error_kind ()});
    });
    if (finished.wait_for (10s) != std::future_status::ready) {
        // The infrastructure thread waits for a continuation queued behind the
        // held worker; the executor cannot drain, so end the process.
        std::cerr << "infrastructure Store read waited for a handler worker\n";
        std::_Exit (1);
    }
    release_worker.set_value ();
    infrastructure.join ();
    runtime::shutdown_handler_coroutine_executor ();
    const auto [admission, target] = finished.get ();
    if (admission != framework_error_kind_t::not_found)
        return 2;
    return target == framework_error_kind_t::not_found ? 0 : 3;
}

} // namespace

enum class parked_request_case_t
{
    replay,
    direct_replay,
    former_pending_capacity
};

// All relay cases enter through the native receive-record path and observe
// its original reply token. The terminal callback marks parking completion.
struct native_actor_relay_fixture_t
{
    using host_t = zlink::framework::runtime::host::public_host_runtime_t;
    zlink::framework::logging_builder_t logging;
    zlink::framework::serializer_registry_t serializers;
    zlink::framework::service_provider_t provider;
    std::shared_ptr<host_t> reply_host;
    zlink::framework::runtime::host::ready_record_t owner;
    struct reply_probe_t
    {
        std::promise<std::vector<zlink::message_t>> replied;
        std::atomic_int calls{0};
    };
    std::shared_ptr<reply_probe_t> probe = std::make_shared<reply_probe_t> ();
    std::future<std::vector<zlink::message_t>> reply = probe->replied.get_future ();

    native_actor_relay_fixture_t (
      const std::shared_ptr<zlink::framework::detail::spot_node_builder_state_t> &node,
      const zlink::framework::actor_ref_t &actor) :
        owner{.owner_kind = zlink::framework::runtime::host::owner_kind_t::actor,
              .domain = zlink::framework::runtime::host::ready_domain_t::application,
              .actor = actor}
    {
        logging.use_file ("native-relay.flow").set_min_level (zlink::framework::log_level_t::debug);
        auto dispatch = node->dispatch;
        dispatch.message_flow (zlink::framework::message_flow_log_mode_t::detailed);
        zlink::framework::detail::dispatch_options_access_t::set_logger (
          dispatch, logging.create_logger ("native-relay"));
        zlink::framework::runtime::host::host_options_t options{
          .mesh = {.descriptor = {.mesh_name = "native-relay",
                                  .node_routing_id =
                                    zlink::routing_id_t::from ("actor-a").to_bytes (),
                                  .lifecycle_generation = 2,
                                  .descriptor_revision = 1,
                                  .advertised_endpoint = "tcp://127.0.0.1:0"},
                   .runtime_failures =
                     std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()},
          .object_stable_types = {"player"}};

        options.mesh.dispatch = dispatch;
        reply_host = std::make_shared<host_t> (std::move (options));
        node->dispatch = dispatch;
        zlink::framework::service_collection_t services;
        services.add_singleton<zlink::framework::detail::actor_gateway_runtime_t> ();
        services.add_singleton<zlink::framework::serializer_registry_t> ();
        provider = services.build_provider ();
        node->root_services = provider;
        node->actor_types_by_id.emplace (std::string (actor.actor_id ().value ()), "player");
        node->native_node = reply_host;
    }

    ~native_actor_relay_fixture_t () { reply_host->close (); }

    void observe_reply (zlink::framework::runtime::host::reply_token_t &token)
    {
        auto original = std::move (token.local_reply);
        token.local_reply = [probe = probe, original = std::move (original)] (const auto &parts) {
            const auto invocation = probe->calls.fetch_add (1, std::memory_order_acq_rel);
            const auto accepted = !original || original (parts);
            if (invocation == 0)
                probe->replied.set_value (parts);
            return accepted;
        };
    }

    zlink::framework::runtime::host::receive_record_t
    request_record (const zlink::routing_id_t &source,
                    zlink::framework::runtime::host::call_id_t operation,
                    std::uint64_t reply_route)
    {
        namespace host = zlink::framework::runtime::host;
        host::receive_record_t record{.kind = host::record_kind_t::actor_request,
                                      .domain = host::ready_domain_t::application};
        record.operation_id = operation;
        record.source_node_rid = source;
        record.reply_route_id = reply_route;
        record.reply_token.host = reply_host;
        observe_reply (record.reply_token);
        return record;
    }

    bool dispatch_parts (zlink::framework::detail::spot_node_runtime_t &spots,
                         const zlink::framework::runtime::host::receive_record_t &record,
                         std::vector<zlink::message_t> &parts)
    {
        auto parked = std::make_shared<std::promise<void>> ();
        auto parking = parked->get_future ();
        bool deferred = false;
        const auto header =
          zlink::framework::runtime::decode_mesh_record_header (record, parts, false);
        if (!header
            || !spots.dispatch_mesh_record (
              owner, record, parts, header.value (), provider, serializers,
              [parked] { parked->set_value (); }, &deferred))
            return false;
        return !deferred
               || parking.wait_for (std::chrono::seconds (2)) == std::future_status::ready;
    }

    bool dispatch (zlink::framework::detail::spot_node_runtime_t &spots,
                   const zlink::framework::runtime::host::receive_record_t &record,
                   const zlink::framework::runtime::messaging::envelope_header_t &header,
                   std::string payload)
    {
        auto encoded =
          zlink::framework::detail::encode_actor_packet (header, zlink::message_t::from (payload));
        auto parts = std::move (encoded).take_items ();
        return dispatch_parts (spots, record, parts);
    }
};

int local_relay_failure_returns_without_waiting_for_handoff_deadline ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace runtime = zlink::framework::runtime;
    auto node =
      zlink::framework::test::runtime_failure_fixture<spot_node_builder_state_t> ("actor-a");
    spot_node_builder_state_t::actor_factory_registration_t factory;
    factory.actor_type = std::type_index (typeid (int));
    factory.create_instance = [] (std::string) { return std::make_shared<int> (7); };
    factory.configure_instance = [] (void *, const actor_ref_t &, void *) {};
    node->actor_factories.emplace ("player", std::move (factory));
    const auto actor = test_actor_ref ("actor-a", "player", "missing-actor", 1);
    spot_node_runtime_t spots (node);
    native_actor_relay_fixture_t fixture (node, actor);
    runtime::messaging::envelope_header_t header;
    header.kind = runtime::messaging::message_kind_t::request;
    header.message_name = "MissingActorProbe";
    auto record = fixture.request_record (zlink::routing_id_t::from ("actor-a"), {41, 43}, 47);
    if (!fixture.dispatch (spots, record, header, "ping")
        || fixture.reply.wait_for (std::chrono::seconds (2)) != std::future_status::ready)
        return 1;
    const auto failed =
      decode_actor_reply (runtime::messaging::message_parts_t (fixture.reply.get ()));
    const auto pending_empty =
      node->lane.run ([&] { return node->pending_handoff_requests.empty (); }).get ();
    return !failed && failed.error_kind () == framework_error_kind_t::not_found
               && fixture.probe->calls.load (std::memory_order_acquire) == 1 && pending_empty
             ? 0
             : 2;
}

int local_relay_preserves_handoff_request_route (bool has_original_operation,
                                                 bool expire_native = false)
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace runtime = zlink::framework::runtime;

    auto node = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::spot_node_builder_state_t> ("actor-a");
    node->worker_executor =
      std::make_shared<runtime::offload_executor_t> (1, "local-relay-route-worker");
    const auto actor = test_actor_ref ("actor-a", "player", "local-relay-actor", 1);
    const auto key = std::string ("player:local-relay-actor");
    if (!node->actor_transfer_coordinator->try_begin_local (key))
        return 1;

    runtime::messaging::envelope_header_t header;
    header.kind = runtime::messaging::message_kind_t::request;
    header.message_name = "LocalRelayProbe";
    header.content_type = "application/octet-stream";
    header.correlation_id = "local-relay-correlation";
    const auto source = has_original_operation ? zlink::routing_id_t::from ("upstream-node")
                                               : zlink::routing_id_t::from (std::uint32_t{0});
    const auto operation = has_original_operation ? runtime::protocol::wire_operation_id_t{41, 43}
                                                  : runtime::protocol::wire_operation_id_t{};
    const auto source_fence =
      has_original_operation
        ? runtime::protocol::actor_route_fence_t{"local-relay-actor",
                                                 1,
                                                 zlink::routing_id_t::from ("actor-a").to_bytes (),
                                                 2,
                                                 3,
                                                 5}
        : runtime::protocol::actor_route_fence_t{};
    spot_node_runtime_t spots (node);
    native_actor_relay_fixture_t fixture (node, actor);
    runtime::host::pending_operation_t native_operation;
    auto native_completion = native_operation.completion->task ();
    const auto native_terminals = std::make_shared<std::atomic_int> (0);
    const auto native_terminal_observed =
      std::make_shared<task_completion_source_t<runtime::host::operation_completion_t>> ();
    detail::observe_task_terminal (
      native_completion, [native_terminals, native_terminal_observed] (const auto &result) {
          native_terminals->fetch_add (1, std::memory_order_acq_rel);
          native_terminal_observed->complete (result);
      });
    using operation_clock_t = runtime::foundation::operation_registry_t::clock_t;
    operation_clock_t::time_point registered_before;
    operation_clock_t::time_point registered_after;
    auto record = fixture.request_record (source, {operation.high, operation.low},
                                          has_original_operation ? 47 : 0);
    if (has_original_operation) {
        record.actor_route = source_fence;
        record.message_follow_hop_count = 3;
        if (!fixture.dispatch (spots, record, header, "payload"))
            return 6;
    } else {
        // The caller provides no operation identity. Use the real local host
        // registry and retain its original completion from before submission.
        fixture.reply_host->configure_stateful_dispatch (
          [] (const runtime::stateful::accepted_record_authority_query_t &query)
            -> task_t<std::optional<runtime::stateful::accepted_record_authority_t>> {
              co_return runtime::stateful::accepted_record_authority_t{
                {"native-relay-source-owner", 17, query.source_node_routing_id,
                 query.source_node_generation},
                37};
          });
        fixture.reply_host->start ();
        const auto native_actor = fixture.reply_host->create_actor ("player", "local-relay-actor");
        auto encoded =
          encode_actor_packet (header, zlink::message_t::from (std::string ("payload")));
        registered_before = operation_clock_t::now ();
        const auto submitted = finite_task_result (fixture.reply_host->request_to_actor (
          native_actor.ref (), encoded.items (), native_operation, std::chrono::seconds (30)));
        registered_after = operation_clock_t::now ();
        if (!submitted || !*submitted || submitted->value () != zlink::submit_result_t::ok)
            return 7;
        bool received = false;
        const auto dispatched = finite_task_result (
          fixture.reply_host->dispatch_ready ([&] (const auto &, const auto &incoming, auto parts) {
              if (incoming.kind != runtime::host::record_kind_t::actor_request)
                  return;
              record = incoming;
              fixture.observe_reply (record.reply_token);
              received = fixture.dispatch_parts (spots, record, parts);
          }));
        if (!dispatched || !*dispatched || !received
            || native_operation.id == runtime::host::call_id_t{}
            || record.operation_id != native_operation.id || record.reply_route_id == 0)
            return 8;
    }
    auto backlog = spots.take_actor_handoff_backlog (actor);
    if (backlog.size () != 1 || !backlog.front ().is_request
        || backlog.front ().packet_name != "LocalRelayProbe"
        || backlog.front ().metadata.at ("__zlink.actorRequestId") != header.correlation_id
        // Relocation flow §9 keeps the absolute deadline at the client.
        || backlog.front ().metadata.contains (std::string (actor_handoff_deadline_key)))
        return 2;
    const auto &metadata = backlog.front ().metadata;
    if (has_original_operation
          ? metadata.at (std::string (actor_handoff_source_node_key)) != source.to_hex ()
              || metadata.at (std::string (actor_handoff_operation_high_key)) != "41"
              || metadata.at (std::string (actor_handoff_operation_low_key)) != "43"
              || metadata.at (std::string (actor_handoff_reply_route_key)) != "47"
              || metadata.at (std::string (actor_handoff_route_actor_id_key))
                   != source_fence.actor_id
              || metadata.at (std::string (actor_handoff_route_lease_generation_key)) != "5"
              || metadata.at (std::string (actor_handoff_hop_count_key)) != "3"
          : metadata.at (std::string (actor_handoff_source_node_key))
                != metadata.at (std::string (actor_handoff_parking_node_key))
              || metadata.at (std::string (actor_handoff_operation_high_key))
                   != std::to_string (record.operation_id.high)
              || metadata.at (std::string (actor_handoff_operation_low_key))
                   != std::to_string (record.operation_id.low)
              || metadata.at (std::string (actor_handoff_reply_route_key))
                   != std::to_string (record.reply_route_id))
        return 3;
    if (fixture.reply.wait_for (std::chrono::milliseconds::zero ()) == std::future_status::ready
        || (!has_original_operation && native_completion.await_ready ()))
        return 10;
    if (has_original_operation) {
        auto send_header = header;
        send_header.kind = runtime::messaging::message_kind_t::command;
        send_header.message_name = "LocalRelaySendProbe";
        auto send_record = record;
        send_record.kind = runtime::host::record_kind_t::actor_send;
        send_record.operation_id = {};
        send_record.reply_route_id = 0;
        send_record.reply_token = {};
        if (!fixture.dispatch (spots, send_record, send_header, "send-payload"))
            return 9;
        // The follow handler can run after an early return, so it owns its flag and fence.
        auto replay_preserved_route = std::make_shared<std::atomic_bool> (false);
        spots.on_actor_message_follow (
          [replay_preserved_route, source_fence] (
            const actor_ref_t &, const runtime::messaging::envelope_header_t &replay_header,
            const zlink::message_t &, std::chrono::milliseconds, const zlink::routing_id_t &,
            const runtime::protocol::actor_route_fence_t &route, std::uint8_t hop,
            const runtime::protocol::wire_operation_id_t &,
            std::uint64_t) -> task_t<std::optional<zlink::message_t>> {
              replay_preserved_route->store (replay_header.message_name == "LocalRelaySendProbe"
                                               && route == source_fence && hop == 3,
                                             std::memory_order_release);
              co_return result_t<std::optional<zlink::message_t>>::success (std::nullopt);
          });
        const auto target = test_actor_ref ("actor-b", "player", "local-relay-actor", 1);
        const runtime::protocol::actor_route_fence_t target_fence{
          "local-relay-actor", 1, zlink::routing_id_t::from ("actor-b").to_bytes (), 2, 4, 6};
        const auto completed_move = finite_task_result (spots.complete_remote_actor_transfer (
          actor, target,
          spot_route_t{node_rid_t::from_string ("actor-b"), spot_id_t ("actor-b-user"), "user"},
          source_fence,
          {target_fence.target_node_routing_id,
           target_fence.target_node_generation,
           {},
           target_fence.owner_lease_generation},
          target_fence.authority_owner_generation, "local-relay-hop"));
        if (!completed_move || !*completed_move
            || !replay_preserved_route->load (std::memory_order_acquire))
            return 11;
    }
    if (!has_original_operation) {
        // The real native registry owns this request's terminal; cancellation
        // must use that owner without a second Framework reply reservation.
        const auto native_record = backlog.front ().application_record;
        if (!native_record || !native_record->native_reply
            || !node->lane.run ([&] { return node->pending_handoff_requests.empty (); }).get ())
            return 4;
        node->actor_transfer_coordinator->prepend_source_suffix (key, std::move (backlog));
        if (expire_native) {
            const auto original_operation = native_operation.id;
            const auto original_reply_route = record.reply_route_id;
            if (fixture.reply_host->transport ().expire_requests (
                  registered_before + std::chrono::seconds (30) - operation_clock_t::duration (1))
                  != 0
                || native_completion.await_ready ())
                return 14;
            // Only the observation includes the wheel's 1ms rounding bound;
            // the request's registered timeout remains 30s.
            const auto expiry_time =
              registered_after + std::chrono::seconds (30) + std::chrono::milliseconds (1);
            const auto expiry_count =
              fixture.reply_host->transport ().expire_requests (expiry_time);
            if (expiry_count != 1) {
                std::cerr << "native deadline expiry count=" << expiry_count
                          << " registration_span_ns="
                          << std::chrono::duration_cast<std::chrono::nanoseconds> (
                               registered_after - registered_before)
                               .count ()
                          << " observation_ticks=" << expiry_time.time_since_epoch ().count ()
                          << " elapsed_since_registration_ns="
                          << std::chrono::duration_cast<std::chrono::nanoseconds> (
                               operation_clock_t::now () - registered_after)
                               .count ()
                          << '\n';
                return 15;
            }
            const auto terminal = finite_task_result (native_terminal_observed->task ());
            if (!terminal || *terminal
                || terminal->error_kind () != framework_error_kind_t::deadline_exceeded
                || native_operation.id != original_operation
                || record.operation_id != original_operation
                || record.reply_route_id != original_reply_route
                || native_terminals->load (std::memory_order_acquire) != 1)
                return 16;
            spots.cancel_local_actor_handoff (actor);
            const auto expired_at =
              std::chrono::steady_clock::now () + std::chrono::hours (24 * 365 * 100);
            return !native_record->native_reply && !native_record->complete_native (std::nullopt)
                       && native_terminals->load (std::memory_order_acquire) == 1
                       && fixture.probe->calls.load (std::memory_order_acquire) == 0
                       && spots.cleanup_expired_actor_admissions_at (expired_at) == 0
                       && fixture.reply_host->transport ().expire_requests (expiry_time) == 0
                       && node->lane.run ([&] { return node->pending_handoff_requests.empty (); })
                            .get ()
                     ? 0
                     : 17;
        }
        spots.cancel_local_actor_handoff (actor);
        const auto terminal = finite_task_result (native_terminal_observed->task ());
        if (!terminal || !*terminal
            || terminal->value ().record.operation_id != native_operation.id)
            return 12;
        const auto native_reply =
          decode_actor_reply (runtime::messaging::message_parts_t (terminal->value ().parts));
        if (native_reply || native_reply.error_kind () != framework_error_kind_t::unavailable
            || native_terminals->load (std::memory_order_acquire) != 1)
            return 13;
        const auto expired_at =
          std::chrono::steady_clock::now () + std::chrono::hours (24 * 365 * 100);
        return !native_record->native_reply && !native_record->complete_native (std::nullopt)
                   && fixture.probe->calls.load (std::memory_order_acquire) == 0
                   && native_terminals->load (std::memory_order_acquire) == 1
                   && spots.cleanup_expired_actor_admissions_at (expired_at) == 0
                   && node->lane.run ([&] { return node->pending_handoff_requests.empty (); })
                        .get ()
                 ? 0
                 : 8;
    }
    const auto recorded =
      node->lane
        .run ([&] {
            if (node->pending_handoff_requests.size () != 1)
                return false;
            const auto &pending = *node->pending_handoff_requests.begin ();
            return pending.first.source_fence
                     == record.actor_route.value_or (runtime::protocol::actor_route_fence_t{})
                   && pending.first.operation_high == record.operation_id.high
                   && pending.first.operation_low == record.operation_id.low
                   && pending.second.reply_route_id == record.reply_route_id
                   && pending.second.reply_token.has_value ()
                   && pending.second.request_header.correlation_id == header.correlation_id
                   && pending.second.request_header.message_name == header.message_name;
        })
        .get ();
    if (fixture.reply.wait_for (std::chrono::milliseconds::zero ()) == std::future_status::ready)
        return 10;
    if (!recorded)
        return 4;
    const auto expired_at = std::chrono::steady_clock::now () + std::chrono::hours (24 * 365 * 100);
    if (spots.cleanup_expired_actor_admissions_at (expired_at) == 0)
        return 5;
    if (fixture.reply.wait_for (std::chrono::seconds (2)) != std::future_status::ready)
        return 8;
    auto completed = fixture.reply.get ();
    const auto reply = decode_actor_reply (runtime::messaging::message_parts_t (completed));

    return !reply && reply.error_kind () == framework_error_kind_t::unavailable
               && fixture.probe->calls.load (std::memory_order_acquire) == 1
               && spots.cleanup_expired_actor_admissions_at (expired_at) == 0
               && fixture.probe->calls.load (std::memory_order_acquire) == 1
               && node->lane.run ([&] { return node->pending_handoff_requests.empty (); }).get ()
             ? 0
             : 8;
}

/* Actor requests parked during relocation retain their original reply route.
 * The same production dispatch fixture pins that a fresh request still enters
 * the backlog beyond the former 1024 pending-reply boundary. */
int parked_request_reply_case (const std::string &requester_rid,
                               parked_request_case_t test_case = parked_request_case_t::replay)
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace runtime = zlink::framework::runtime;
    namespace host = zlink::framework::runtime::host;
    namespace messaging = zlink::framework::runtime::messaging;

    // Regression pin (SF-F2 hang / SF-F7 cross-node timeout): an
    // actor_request that arrives while the actor's transfer is still open
    // parks in the handoff backlog and is replayed with a handoff terminal
    // route. The pending entry that maps the terminal back to the original
    // reply token must be recorded even when the requester attached no
    // actor_route fence, and the terminal must return to THIS parking node
    // even when the requester lives on another node — before the fix the
    // terminal route named the requester, so a cross-node requester's
    // terminal never found the pending entry and the reply was lost.
    serializer_registry_t serializers;
    auto node = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::spot_node_builder_state_t> ("actor-a");
    node->worker_executor =
      std::make_shared<runtime::offload_executor_t> (1, "parked-replay-worker");
    node->channel_runtime =
      zlink::framework::test::runtime_failure_fixture<channel_runtime_state_t> ();
    node->channel_runtime->serializers = &serializers;

    auto spot = std::make_shared<spot_context_state_t> ();
    spot->node = node;
    spot->node_rid = node_rid_t::from_string ("actor-a");
    spot->spot_id = spot_id_t ("actor-a-user");
    spot->spot_name = "user";
    spot->spot_instance = std::make_shared<int> (1);
    spot->channel_runtime =
      zlink::framework::test::runtime_failure_fixture<channel_runtime_state_t> ();
    spot->channel_runtime->serializers = &serializers;
    spot->serial_executor = node->worker_executor;
    node->spot_contexts_by_id.emplace (spot->spot_id, spot_context_access_t::create (spot));

    spot_node_builder_state_t::actor_factory_registration_t factory;
    factory.actor_type = std::type_index (typeid (int));
    factory.create_instance = [] (std::string) { return std::make_shared<int> (7); };
    factory.configure_instance = [] (void *, const actor_ref_t &, void *) {};
    node->actor_factories.emplace ("player", std::move (factory));

    const auto actor = test_actor_ref ("actor-a", "player", "parked-actor", 1);
    const auto key = std::string ("player:parked-actor");
    node->actor_instances.emplace (key, std::make_shared<int> (7));
    node->actor_spot_ids.emplace (key, spot->spot_id);
    node->actor_generations.emplace (key, 1);
    node->actor_types_by_id.emplace ("parked-actor", "player");

    // The handler may run after an early return; it owns the flag.
    const auto handler_ran_owner = std::make_shared<std::atomic_bool> (false);
    auto &handler_ran = *handler_ran_owner;
    spot->handlers.push_back (
      spot_handler_descriptor_t{spot_handler_kind_t::actor_request, "ParkedProbe", "",
                                std::type_index (typeid (int)), std::type_index (typeid (void)),
                                std::type_index (typeid (int)), std::type_index (typeid (void))});
    spot->handler_invokers.push_back (
      [handler_ran_owner] (void *, void *, service_provider_t &, serializer_registry_t &,
                           const zlink::message_t &,
                           const spot_inbound_message_t &) -> task_t<zlink::message_t> {
          handler_ran_owner->store (true, std::memory_order_release);
          co_return zlink::message_t::from (std::string ("pong"));
      });

    service_collection_t services;
    services.add_singleton<actor_gateway_runtime_t> ();
    services.add_singleton<serializer_registry_t> ();
    node->root_services = services.build_provider ();
    auto provider = services.build_provider ();

    // The transfer is open when the request arrives: it must park.
    if (!node->actor_transfer_coordinator->try_begin_local (key))
        return 1;
    spot_node_runtime_t spots (node);

    constexpr std::size_t pending_handoff_capacity = 1024;
    const auto capacity_sentinel_source =
      zlink::routing_id_t::from ("pending-capacity-holder").to_hex ();
    if (test_case == parked_request_case_t::former_pending_capacity) {
        const auto sentinel_deadline = std::chrono::steady_clock::now () + std::chrono::hours (1);
        const auto filled =
          node->lane
            .run ([&] {
                for (std::uint64_t index = 1; index <= pending_handoff_capacity; ++index) {
                    const spot_node_builder_state_t::pending_handoff_request_key_t sentinel_key{
                      capacity_sentinel_source, 900, index, {}};
                    const auto [_, inserted] = node->pending_handoff_requests.emplace (
                      sentinel_key, spot_node_builder_state_t::pending_handoff_request_t{
                                      actor, {}, 10'000 + index, {}, {}, sentinel_deadline});
                    if (!inserted)
                        return false;
                }
                return node->pending_handoff_requests.size () == pending_handoff_capacity;
            })
            .get ();
        if (!filled) {
            spots.fail_remote_actor_transfer (actor, false, std::nullopt);
            return 9;
        }
    }

    // The reply callback may run after an early return; it owns its state.
    struct reply_state_t
    {
        std::mutex mutex;
        std::vector<zlink::message_t> parts;
        std::atomic_int calls{0};
    };
    const auto reply_state = std::make_shared<reply_state_t> ();
    auto &reply_mutex = reply_state->mutex;
    auto &reply_parts = reply_state->parts;
    const auto reply_host = std::make_shared<host::public_host_runtime_t> (host::host_options_t{
      .mesh = {.descriptor = {.mesh_name = "parked-replay",
                              .node_routing_id =
                                zlink::routing_id_t::from (std::string ("actor-a")).to_bytes (),
                              .lifecycle_generation = 1,
                              .descriptor_revision = 1,
                              .advertised_endpoint = "tcp://127.0.0.1:0"},
               .runtime_failures =
                 std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()}});
    host::receive_record_t record{.kind = host::record_kind_t::actor_request,
                                  .domain = host::ready_domain_t::application};
    record.operation_id = host::call_id_t{41, 43};
    record.source_node_rid = zlink::routing_id_t::from (requester_rid);
    record.reply_route_id = 57;
    record.reply_token.host = reply_host;
    record.reply_token.local_reply = [reply_state] (const std::vector<zlink::message_t> &parts) {
        const std::lock_guard lock (reply_state->mutex);
        reply_state->parts = parts;
        reply_state->calls.fetch_add (1, std::memory_order_release);
        return true;
    };
    // Deliberately NO record.actor_route: the requester attached no fence.

    const host::ready_record_t owner{.owner_kind = host::owner_kind_t::actor,
                                     .domain = host::ready_domain_t::application,
                                     .spot_id = std::string (spot->spot_id),
                                     .actor = actor};

    auto encoded = zlink::framework::detail::encode_actor_packet (
      messaging::envelope_header_t{.kind = messaging::message_kind_t::request,
                                   .channel_name = "actor",
                                   .message_name = "ParkedProbe",
                                   .correlation_id = "parked-request-1"},
      zlink::message_t::from (std::string ("ping")));
    auto request_parts = std::move (encoded).take_items ();

    auto parked = std::make_shared<std::promise<void>> ();
    auto parking = parked->get_future ();
    auto replied = std::make_shared<std::promise<void>> ();
    auto reply_ready = replied->get_future ();
    const auto original_reply = record.reply_token.local_reply;
    record.reply_token.local_reply = [original_reply, replied] (const auto &parts) {
        const auto accepted = original_reply (parts);
        replied->set_value ();
        return accepted;
    };
    bool terminal_deferred = false;
    if (!spots.dispatch_mesh_record (
          owner, record, request_parts,
          zlink::framework::runtime::decode_mesh_record_header (record, request_parts, false)
            .value (),
          provider, serializers, [parked] { parked->set_value (); }, &terminal_deferred)
        || !terminal_deferred
        || parking.wait_for (std::chrono::seconds (2)) != std::future_status::ready)
        return 2;
    const auto pending_count =
      node->lane.run_checked ([&] { return node->pending_handoff_requests.size (); }).get ();
    if (pending_count
          != (test_case == parked_request_case_t::former_pending_capacity
                ? pending_handoff_capacity + 1
                : 1)
        || handler_ran.load (std::memory_order_acquire)
        || reply_ready.wait_for (std::chrono::milliseconds::zero ()) == std::future_status::ready)
        return 3;
    spots.fail_remote_actor_transfer (actor, false, std::nullopt);
    if (reply_ready.wait_for (std::chrono::seconds (2)) != std::future_status::ready)
        return 4;
    if (!handler_ran.load (std::memory_order_acquire))
        return 5;
    std::vector<zlink::message_t> delivered;
    {
        const std::lock_guard lock (reply_mutex);
        delivered = reply_parts;
    }
    messaging::message_parts_t reply_envelope (std::move (delivered));
    const auto reply_body = zlink::framework::detail::decode_actor_reply (reply_envelope);
    if (!reply_body || !reply_body.value () || reply_body.value ()->to_string () != "pong")
        return 9;
    const auto pending_settled =
      node->lane
        .run_checked ([&] {
            return test_case != parked_request_case_t::former_pending_capacity
                     ? node->pending_handoff_requests.empty ()
                     : node->pending_handoff_requests.size () == pending_handoff_capacity;
        })
        .get ();
    if (!pending_settled || reply_state->calls.load (std::memory_order_acquire) != 1)
        return 8;
    return 0;
}

int parked_request_without_route_fence_receives_reply_after_replay ()
{
    // Requester local to the parking node (the SF-F2 shape).
    return parked_request_reply_case ("actor-a");
}

int pending_handoff_accepts_beyond_former_capacity ()
{
    return parked_request_reply_case ("actor-a", parked_request_case_t::former_pending_capacity);
}

int same_operation_from_distinct_source_lifecycles_has_distinct_pending_terminal ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace runtime = zlink::framework::runtime;

    auto node = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::spot_node_builder_state_t> ("parking-node");
    const auto actor = test_actor_ref ("parking-node", "player", "same-operation", 1);
    const auto first_fence = runtime::protocol::actor_route_fence_t{
      "same-operation", 1, zlink::routing_id_t::from ("owner-a").to_bytes (), 4, 9, 12};
    const auto second_fence = runtime::protocol::actor_route_fence_t{
      "same-operation", 1, zlink::routing_id_t::from ("owner-b").to_bytes (), 5, 10, 13};
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (1);
    const spot_node_builder_state_t::pending_handoff_request_key_t first{
      zlink::routing_id_t::from ("source-lifecycle-a").to_hex (), 71, 73, first_fence};
    const spot_node_builder_state_t::pending_handoff_request_key_t second{
      zlink::routing_id_t::from ("source-lifecycle-b").to_hex (), 71, 73, second_fence};
    node->pending_handoff_requests.emplace (first,
                                            spot_node_builder_state_t::pending_handoff_request_t{
                                              actor, first_fence, 101, {}, {}, deadline});
    node->pending_handoff_requests.emplace (second,
                                            spot_node_builder_state_t::pending_handoff_request_t{
                                              actor, second_fence, 202, {}, {}, deadline});

    // A terminal reconstructing each source lifecycle identity reaches only
    // its own reply route, despite sharing the exact OperationId pair.
    const auto first_terminal = node->pending_handoff_requests.find (first);
    const auto second_terminal = node->pending_handoff_requests.find (second);
    if (node->pending_handoff_requests.size () != 2
        || first_terminal == node->pending_handoff_requests.end ()
        || second_terminal == node->pending_handoff_requests.end ()
        || first_terminal->second.reply_route_id != 101
        || second_terminal->second.reply_route_id != 202) {
        return 1;
    }
    node->pending_handoff_requests.erase (first_terminal);
    return node->pending_handoff_requests.size () == 1
               && node->pending_handoff_requests.contains (second)
             ? 0
             : 2;
}

int parked_cross_node_request_terminal_returns_to_parking_node ()
{
    // Requester on a different node (the SF-F7/SF-F11 shape): the handoff
    // terminal must be routed by the parking-node key, not the requester's
    // source-node key, or the pending reply token is never answered.
    return parked_request_reply_case ("actor-b");
}

int relayed_request_without_deferred_terminal_receives_follow_reply ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace runtime = zlink::framework::runtime;
    namespace host = zlink::framework::runtime::host;
    namespace messaging = zlink::framework::runtime::messaging;

    // Regression pin (SF-F7/SF-F11 timeout): a cross-node actor_request that
    // reaches the committed Message Follow source right after the relocation
    // commit is relayed to the new owner and the relayed reply comes back
    // inline. The reply must reach the original reply token even when the
    // record was dispatched without a deferred terminal (the direct
    // transfer-window dispatch path passes none). Before the fix
    // dispatch_mesh_record returned before observing the relay task, so the
    // relayed reply was discarded and the requester's wire operation timed
    // out.
    serializer_registry_t serializers;
    auto node = zlink::framework::test::runtime_failure_fixture<
      zlink::framework::detail::spot_node_builder_state_t> ("actor-a");
    node->worker_executor =
      std::make_shared<runtime::offload_executor_t> (1, "follow-relay-worker");
    node->channel_runtime =
      zlink::framework::test::runtime_failure_fixture<channel_runtime_state_t> ();
    node->channel_runtime->serializers = &serializers;

    service_collection_t services;
    services.add_singleton<actor_gateway_runtime_t> ();
    node->root_services = services.build_provider ();
    auto provider = services.build_provider ();

    const auto local_host = std::make_shared<host::public_host_runtime_t> (host::host_options_t{
      .mesh = {.descriptor = {.mesh_name = "follow-relay",
                              .node_routing_id =
                                zlink::routing_id_t::from (std::string ("actor-a")).to_bytes (),
                              .lifecycle_generation = 1,
                              .descriptor_revision = 1,
                              .advertised_endpoint = "tcp://127.0.0.1:0"},
               .runtime_failures =
                 std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> ()}});
    node->native_node = local_host;

    const auto actor = test_actor_ref ("actor-a", "player", "moved-actor", 1);
    const auto key = std::string ("player:moved-actor");
    node->actor_types_by_id.emplace ("moved-actor", "player");

    // The requester's stale route names this node as the committed source.
    const runtime::protocol::actor_route_fence_t source_fence{
      "moved-actor",
      1,
      zlink::routing_id_t::from (std::string ("actor-a")).to_bytes (),
      local_host->status ().lifecycle_generation (),
      3,
      5};
    const runtime::protocol::actor_route_fence_t target_fence{
      "moved-actor", 1, zlink::routing_id_t::from (std::string ("actor-b")).to_bytes (), 1, 4, 6};
    node->actor_transfer_coordinator->activate_message_follow (
      key, source_fence, test_actor_ref ("actor-b", "player", "moved-actor", 1),
      spot_route_t{node_rid_t::from_string ("actor-b"), spot_id_t ("actor-b-user"), "user"},
      target_fence, std::chrono::steady_clock::now () + std::chrono::seconds (30));

    spot_node_runtime_t spots (node);
    // The relay handler and the reply sink can run after an early return, so they own their state.
    auto relay_ran = std::make_shared<std::atomic_bool> (false);
    spots.on_actor_message_follow (
      [relay_ran] (const actor_ref_t &, const runtime::messaging::envelope_header_t &,
                   const zlink::message_t &, std::chrono::milliseconds, const zlink::routing_id_t &,
                   const runtime::protocol::actor_route_fence_t &, std::uint8_t,
                   const runtime::protocol::wire_operation_id_t &,
                   std::uint64_t) -> task_t<std::optional<zlink::message_t>> {
          relay_ran->store (true, std::memory_order_release);
          co_return result_t<std::optional<zlink::message_t>>::success (
            std::make_optional (zlink::message_t::from (std::string ("relayed-pong"))));
      });

    struct reply_state_t
    {
        std::mutex mutex;
        std::vector<zlink::message_t> parts;
    };
    auto reply = std::make_shared<reply_state_t> ();
    host::receive_record_t record{.kind = host::record_kind_t::actor_request,
                                  .domain = host::ready_domain_t::application};
    record.operation_id = host::call_id_t{11, 13};
    record.source_node_rid = zlink::routing_id_t::from (std::string ("actor-b"));
    record.actor_route = source_fence;
    record.reply_route_id = 17;
    record.reply_token.host = local_host;
    record.reply_token.local_reply = [reply] (const std::vector<zlink::message_t> &parts) {
        const std::lock_guard lock (reply->mutex);
        reply->parts = parts;
        return true;
    };

    const host::ready_record_t owner{.owner_kind = host::owner_kind_t::actor,
                                     .domain = host::ready_domain_t::application,
                                     .spot_id = std::string ("actor-a-user"),
                                     .actor = actor};

    auto encoded = zlink::framework::detail::encode_actor_packet (
      messaging::envelope_header_t{.kind = messaging::message_kind_t::request,
                                   .channel_name = "actor",
                                   .message_name = "FollowProbe",
                                   .correlation_id = "follow-request-1"},
      zlink::message_t::from (std::string ("ping")));
    auto request_parts = std::move (encoded).take_items ();

    // Deliberately the deferred-terminal-less overload: this is the direct
    // dispatch shape used while the transfer window is still open.
    (void) spots.dispatch_mesh_record (
      owner, record, request_parts,
      zlink::framework::runtime::decode_mesh_record_header (record, request_parts, false).value (),
      provider, serializers);

    const auto reply_deadline = std::chrono::steady_clock::now () + std::chrono::seconds (2);
    for (;;) {
        {
            const std::lock_guard lock (reply->mutex);
            if (!reply->parts.empty ())
                break;
        }
        if (std::chrono::steady_clock::now () >= reply_deadline)
            return 1;
        std::this_thread::yield ();
    }
    if (!relay_ran->load (std::memory_order_acquire))
        return 2;
    std::vector<zlink::message_t> delivered;
    {
        const std::lock_guard lock (reply->mutex);
        delivered = reply->parts;
    }
    messaging::message_parts_t reply_envelope (std::move (delivered));
    const auto reply_body = zlink::framework::detail::decode_actor_reply (reply_envelope);
    if (!reply_body || !reply_body.value () || reply_body.value ()->to_string () != "relayed-pong")
        return 5;
    return 0;
}

int session_relay_waiter_accepts_beyond_former_capacity ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    constexpr std::size_t relay_count = 1026;
    using reply_source_t = task_completion_source_t<std::optional<zlink::message_t>>;
    struct relay_fixture_t
    {
        std::vector<std::shared_ptr<reply_source_t>> sources;
        std::vector<std::promise<void>> arrived;
        std::vector<std::future<void>> arrivals;
        std::vector<std::string> packet_names;
        std::size_t dispatch_count = 0;
        std::promise<std::size_t> &released;

        relay_fixture_t (std::size_t count, std::promise<std::size_t> &released) :
            sources (count), arrived (count), packet_names (count), released (released)
        {
            arrivals.reserve (count);
            for (std::size_t index = 0; index != count; ++index) {
                sources[index] = std::make_shared<reply_source_t> ();
                arrivals.push_back (arrived[index].get_future ());
            }
        }

        ~relay_fixture_t () { released.set_value (dispatch_count); }
    };

    actor_gateway_runtime_t gateway;
    auto manager = gateway.manager ();
    session_actor_manager_access_t::attach (manager, stream_t{});
    const actor_ref_t actor = test_actor_ref ("actor-node-a", "player", "actor-capacity", 1);
    auto binding = manager.bind (actor).async ().result ().value ();
    std::promise<std::size_t> released;
    auto release = released.get_future ();
    {
        auto fixture = std::make_shared<relay_fixture_t> (relay_count, released);
        std::vector<task_t<void>> accepted;
        accepted.reserve (relay_count);
        struct relay_scope_t
        {
            actor_gateway_runtime_t &gateway;
            std::shared_ptr<relay_fixture_t> &fixture;
            std::vector<task_t<void>> &accepted;
            std::future<std::size_t> &release;
            ~relay_scope_t ()
            {
                for (const auto &source : fixture->sources)
                    source->complete (
                      result_t<std::optional<zlink::message_t>>::success (std::nullopt));
                for (auto &task : accepted)
                    (void) task.result ();
                gateway.on_relay ({});
                fixture.reset ();
                // The final drain observer owns its dispatch closure until it returns.
                release.wait ();
            }
        } relay_scope{gateway, fixture, accepted, release};
        gateway.on_relay ([fixture] (const actor_ref_t &, const actor_context_t &,
                                     const stream_header_t &header, const zlink::message_t &,
                                     std::optional<bound_session_relay_source_t>,
                                     std::chrono::milliseconds) {
            const auto index = fixture->dispatch_count++;
            fixture->packet_names.at (index) = std::string (header.packet_name ());
            const auto source = fixture->sources.at (index);
            fixture->arrived.at (index).set_value ();
            return test::accepted_request_task (source->task ());
        });

        /* One relay occupies the drain turn; the rest cross the former boundary. */
        for (std::size_t index = 0; index != relay_count; ++index)
            accepted.push_back (binding.relay (std::to_string (index), zlink::message_t{}));
        auto completed = std::make_shared<std::promise<void>> ();
        auto completion = completed->get_future ();
        observe_task_completion (accepted.back (),
                                 [completed] (const auto &) { completed->set_value (); });

        /* Each arrival is observed before its source is released; order remains FIFO. */
        for (std::size_t index = 0; index != relay_count; ++index) {
            fixture->arrivals[index].get ();
            if (fixture->packet_names[index] != std::to_string (index))
                return 5;
            fixture->sources[index]->complete (
              result_t<std::optional<zlink::message_t>>::success (std::nullopt));
        }
        // Result delivery precedes the final drain frame's destruction.
        completion.get ();
        for (auto &admitted : accepted) {
            if (!admitted.await_ready () || !admitted.result ())
                return 4;
        }
    }
    if (release.get () != relay_count)
        return 3;
    return 0;
}

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
    if (!next_submitted.result () || next_delivery_started.load (std::memory_order_acquire)) {
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

int actor_unbound_waits_for_accepted_push_fifo ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    auto state = std::make_shared<actor_gateway_state_t> ();
    serializer_registry_t serializers;
    state->serializers = &serializers;
    actor_gateway_runtime_t gateway (state);
    zlink_builder_t builder = zlink::framework::test::runtime_failure_builder ();
    builder.stream ("actor-unbound-fifo").bind ("tcp://127.0.0.1:0");
    auto runtime = stream_runtime_t::from (builder);
    auto stream = runtime.open_session ("actor-unbound-fifo");
    auto push_terminal = std::make_shared<task_completion_source_t<void>> ();
    std::mutex events_mutex;
    std::vector<std::string> events;
    runtime.attach_transport_writer (
      stream, [push_terminal, &events_mutex, &events] (const stream_header_t &header,
                                                       const zlink::message_t &) {
          {
              std::lock_guard lock (events_mutex);
              events.emplace_back (header.packet_name ());
          }
          return header.packet_name () != "$zlink.actor.unbound"
                   ? push_terminal->task ()
                   : task_t<void> (result_t<void>::success ());
      });
    const auto actor = test_actor_ref ("actor-node", "player", "fifo-actor", 7);
    gateway.bind_session_stream ("fifo-actor", stream, stream_codec_t::message_pack, "fifo-session",
                                 23, actor, 1);
    const auto accepted = gateway.actor_context (actor)
                            .bound_session ()
                            .send (std::string ("payload"))
                            .async ()
                            .result ();
    if (!accepted)
        return 1;
    for (int attempt = 0; attempt < 100; ++attempt) {
        {
            std::lock_guard lock (events_mutex);
            if (!events.empty ())
                break;
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    gateway.unbind_session_stream ("fifo-actor", "fifo-session", 23);
    bool unbound_overtook_push = false;
    {
        std::lock_guard lock (events_mutex);
        unbound_overtook_push = events.size () != 1 || events.front () == "$zlink.actor.unbound";
    }
    if (unbound_overtook_push) {
        push_terminal->complete (result_t<void>::success ());
        return 2;
    }
    push_terminal->complete (result_t<void>::success ());
    for (int attempt = 0; attempt < 100; ++attempt) {
        {
            std::lock_guard lock (events_mutex);
            if (events.size () == 2)
                break;
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    std::lock_guard lock (events_mutex);
    return events.size () == 2 && events.back () == "$zlink.actor.unbound" ? 0 : 3;
}

int actor_unbound_follows_session_termination (bool physical, bool disconnect_then_shutdown = false)
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    constexpr auto stream_name = "unbound-termination";
    constexpr auto submission_wait_timeout = std::chrono::seconds (5);
    auto state = std::make_shared<actor_gateway_state_t> ();
    actor_gateway_runtime_t gateway (state);
    auto manager = gateway.manager ();
    auto builder = zlink::framework::test::runtime_failure_builder ();
    builder.stream (stream_name).bind ("tcp://127.0.0.1:0");
    auto runtime = stream_runtime_t::from (builder);
    auto stream = runtime.open_session (stream_name);
    auto unbound = std::make_shared<std::atomic_int> (0);
    auto unbound_submitted = std::make_shared<std::promise<void>> ();
    auto submitted = unbound_submitted->get_future ();
    runtime.attach_transport_writer (
      stream,
      [unbound, unbound_submitted] (const stream_header_t &header, const zlink::message_t &) {
          if (header.packet_name () == "$zlink.actor.unbound" && ++(*unbound) == 1)
              unbound_submitted->set_value ();
          return task_t<void> (result_t<void>::success ());
      });
    session_actor_manager_access_t::attach (manager, stream);
    session_actor_manager_access_t::bind_native (
      manager, [] (actor_ref_t, std::uint64_t, std::uint16_t) {
          return request_task_t<void> (result_t<void>::success ());
      });
    auto disconnect_completion = std::make_shared<task_completion_source_t<void>> ();
    if (!disconnect_then_shutdown)
        disconnect_completion->complete (result_t<void>::success ());
    state->disconnect_dispatcher =
      [disconnect_completion] (const actor_ref_t &,
                               const std::optional<runtime::stateful::stream_binding_t> &) {
          return disconnect_completion->task ();
      };
    auto binding =
      manager.bind (test_actor_ref ("node", "player", "actor-a", 1)).async ().result ();
    if (!binding)
        return 1;
    if (physical) {
        if (!stream.close ().result ())
            return 2;
        session_actor_manager_access_t::disconnect (manager);
        if (disconnect_then_shutdown) {
            // The physical disconnect callback is still pending when shutdown closes the stream.
            if (!stream.close ().result ())
                return 5;
            disconnect_completion->complete (result_t<void>::success ());
            session_actor_manager_access_t::disconnect (manager);
        }
    } else {
        if (!binding.value ().notify_disconnected ().result ())
            return 3;
        if (submitted.wait_for (submission_wait_timeout) != std::future_status::ready)
            return 4;
    }
    return unbound->load () == (physical ? 0 : 1) ? 0 : 4;
}

int session_disconnect_uses_actor_slot_issue_order ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    auto state = std::make_shared<actor_gateway_state_t> ();
    actor_gateway_runtime_t gateway (state);
    auto manager = gateway.manager ();
    zlink_builder_t builder = zlink::framework::test::runtime_failure_builder ();
    builder.stream ("disconnect-slot-order").bind ("tcp://127.0.0.1:0");
    auto runtime = stream_runtime_t::from (builder);
    auto stream = runtime.open_session ("disconnect-slot-order");
    runtime.attach_transport_writer (stream,
                                     [] (const stream_header_t &, const zlink::message_t &) {
                                         return task_t<void> (result_t<void>::success ());
                                     });
    session_actor_manager_access_t::attach (manager, stream);
    session_actor_manager_access_t::bind_native (
      manager, [] (actor_ref_t, std::uint64_t, std::uint16_t) {
          return request_task_t<void> (result_t<void>::success ());
      });
    std::vector<std::string> disconnected;
    state->disconnect_dispatcher =
      [&disconnected] (const actor_ref_t &actor,
                       const std::optional<runtime::stateful::stream_binding_t> &) {
          disconnected.emplace_back (actor.actor_id ().value ());
          return task_t<void> (result_t<void>::success ());
      };
    if (!manager.bind (test_actor_ref ("node", "player", "z-issued-first", 1)).async ().result ()
        || !manager.bind (test_actor_ref ("node", "player", "a-issued-second", 1))
              .async ()
              .result ())
        return 1;
    session_actor_manager_access_t::disconnect (manager);
    return disconnected == std::vector<std::string>{"z-issued-first", "a-issued-second"} ? 0 : 2;
}

int close_unbound_session_reports_invalid_operation ()
{
    using namespace zlink::framework;
    detail::actor_gateway_runtime_t gateway;
    const auto actor = test_actor_ref ("actor-owner", "player", "unbound-close", 1);
    const auto result = gateway.actor_context (actor).bound_session ().disconnect ().result ();
    return !result && result.error_kind () == framework_error_kind_t::invalid_operation ? 0 : 1;
}

int close_exact_session_binding_uses_physical_terminal ()
{
    using namespace zlink::framework;
    namespace sf = runtime::stateful;
    namespace wire = runtime::protocol;
    const sf::object_ref_t actor{
      sf::object_kind_t::actor, "close-actor", 7, 11, "game", "actor-owner"};
    sf::stream_session_registry_t sessions (
      [&] (const std::string &) { return std::optional (actor); });
    const auto rid = zlink::routing_id_t::from ("session-rid");
    const auto owner = zlink::routing_id_t::from ("session-owner");
    unsigned closes = 0;
    const auto connection = sessions.open (rid.to_hex (), {}, owner, 13, [&] () -> task_t<void> {
        ++closes;
        co_return;
    });
    const auto [error, binding] = sessions.bind (connection, actor, 17, 19);
    if (error != sf::stateful_error_t::none)
        return 1;
    const wire::retired_bound_session_route_fence_t fence{
      owner.to_bytes (), 13, owner.to_string (), 13, rid.to_bytes (), binding.binding_generation};
    const wire::bound_session_close_t request{23, actor.key, actor.object_generation, fence};
    const auto decoded =
      wire::decode_bound_session_close (wire::encode_bound_session_close (request));
    if (decoded.actor_id != request.actor_id || decoded.session != fence)
        return 2;
    auto stale = request;
    ++stale.session.retired_binding_generation;
    auto denied = sessions.close_current (stale, fence).result ();
    if (denied || denied.error_kind () != framework_error_kind_t::unavailable || closes != 0)
        return 3;
    stale = request;
    ++stale.object_generation;
    denied = sessions.close_current (stale, fence).result ();
    if (denied || denied.error_kind () != framework_error_kind_t::unavailable || closes != 0)
        return 4;
    stale = request;
    ++stale.session.session_owner_node_generation;
    denied = sessions.close_current (stale, fence).result ();
    if (denied || denied.error_kind () != framework_error_kind_t::unavailable || closes != 0)
        return 5;
    auto closed = sessions.close_current (request, fence).result ();
    if (!closed || closes != 1 || !sessions.current_binding (actor.key))
        return 6;
    const auto replacement_rid = zlink::routing_id_t::from ("replacement-session");
    unsigned replacement_closes = 0;
    const auto replacement =
      sessions.open (replacement_rid.to_hex (), {}, owner, 13, [&] () -> task_t<void> {
          ++replacement_closes;
          co_return;
      });
    const auto [replacement_error, current] = sessions.bind (replacement, actor, 17, 19);
    if (replacement_error != sf::stateful_error_t::none)
        return 7;
    denied = sessions.close_current (request, fence).result ();
    if (denied || denied.error_kind () != framework_error_kind_t::unavailable
        || replacement_closes != 0 || closes != 1)
        return 8;
    auto current_request = request;
    current_request.session.session_routing_id = replacement_rid.to_bytes ();
    current_request.session.retired_binding_generation = current.binding_generation;
    closed = sessions.close_current (current_request, fence).result ();
    if (!closed || replacement_closes != 1)
        return 9;
    sessions.close (connection);
    if (!sessions.current_binding (actor.key))
        return 10;
    sessions.close (replacement);
    return sessions.current_binding (actor.key) ? 11 : 0;
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

int relocation_prepare_preserves_runtime_failure ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    namespace protocol = zlink::framework::runtime::protocol;
    auto state = std::make_shared<actor_gateway_state_t> ();
    actor_gateway_runtime_t gateway (state);
    const protocol::session_relocation_route_t route{
      .sender_role = protocol::relocation_role_t::target,
      .actor = {"actor", 7},
      .session_owner_node_routing_id = zlink::routing_id_t::from ("owner").to_bytes (),
      .session_owner_node_generation = 11,
      .session_owner_id = "owner-id",
      .session_owner_lease_generation = 13,
      .session_routing_id = zlink::routing_id_t::from ("session").to_bytes (),
      .binding_generation = 17,
      .route = {protocol::session_relocation_route_action_t::commit, 19, 23,
                zlink::routing_id_t::from ("target").to_bytes (), 29, 0, 31}};
    for (int field = 0; field < 3; ++field) {
        auto malformed = route;
        auto &rid = field == 0   ? malformed.session_owner_node_routing_id
                    : field == 1 ? malformed.session_routing_id
                                 : malformed.route.target_node_routing_id;
        rid.assign (protocol::ridBytes + 1, 1);
        if (gateway.prepare_session_relocation_target_route (malformed, 31))
            return 1;
        rid.clear ();
        if (gateway.prepare_session_relocation_target_route (malformed, 31))
            return 2;
    }
    state->lane.close ();
    try {
        (void) gateway.prepare_session_relocation_target_route (route, 31);
    }
    catch (const std::runtime_error &error) {
        return std::string_view (error.what ()) == "state lane is closed" ? 0 : 3;
    }
    return 4;
}

int join_completion_callback_failure_is_reported ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    auto state = std::make_shared<actor_gateway_state_t> ();
    std::vector<message_dispatch_error_event_t> errors;
    state->dispatch.message_flow (message_flow_log_mode_t::normal);
    dispatch_options_access_t::set_dispatch_error_observer_for_tests (
      state->dispatch, [&errors] (const auto &event) { errors.push_back (event); });
    actor_gateway_runtime_t gateway (state);
    auto actor = test_actor_ref ("node", "player", "callback-failure", 7);
    auto fence = gateway.begin_join_completion_delivery_fence (actor);
    gateway.settle_join_completion_delivery_fence (
      actor, fence, result_t<void>::success (),
      [] (result_t<void>) { throw std::runtime_error ("completion failure"); });
    return errors.size () == 1
               && errors.front ().reason == dispatch_error_reason_t::handler_exception
               && errors.front ().error_message == "completion failure"
             ? 0
             : 1;
}

int actor_typed_wire_header_contract ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::runtime;
    // STREAM JSON Request with request_seq=1, as emitted by Java/.NET/Node.
    const std::vector<std::uint8_t> request_header{
      0xf2, 2,   1,   1,   0,   0,   0,   0,   0,   0,   0,   1,   20,  'B', 'e', 'g', 'i',
      'n',  'U', 's', 'e', 'r', 'S', 'p', 'o', 't', 'J', 'o', 'i', 'n', 'R', 'e', 'q'};
    for (const auto kind : {host::record_kind_t::actor_request, host::record_kind_t::actor_send}) {
        auto bytes = request_header;
        if (kind == host::record_kind_t::actor_send) {
            bytes[1] = 1;
            bytes[3] = 0;
            bytes.erase (bytes.begin () + 4, bytes.begin () + 12);
        }
        std::vector<zlink::message_t> parts{zlink::message_t::from (bytes),
                                            zlink::message_t::from ("{}")};
        host::receive_record_t record{.kind = kind};
        auto decoded = decode_mesh_record_header (record, parts, false);
        if (!decoded || !decoded.value ()
            || decoded.value ()->message_name != "BeginUserSpotJoinReq" || parts.size () != 2
            || parts[1].to_string () != "{}") {
            std::cerr << "Actor STREAM header was not accepted as BeginUserSpotJoinReq\n";
            return 1;
        }
        auto wrong_kind = request_header;
        if (kind == host::record_kind_t::actor_request) {
            wrong_kind[1] = 1;
            wrong_kind[3] = 0;
            wrong_kind.erase (wrong_kind.begin () + 4, wrong_kind.begin () + 12);
        }
        parts = {zlink::message_t::from (wrong_kind), zlink::message_t::from ("{}")};
        if (decode_mesh_record_header (record, parts, false))
            return 12;
        for (const auto invalid : {std::string ("BeginUserSpotJoinReq"),
                                   std::string ("{\"messageName\":\"BeginUserSpotJoinReq\"}")}) {
            parts = {zlink::message_t::from (invalid), zlink::message_t::from ("{}")};
            if (decode_mesh_record_header (record, parts, false)) {
                std::cerr << "Actor non-STREAM header was accepted\n";
                return 2;
            }
        }
    }
    using namespace zlink::framework::detail;
    messaging::envelope_header_t request;
    request.kind = messaging::message_kind_t::request;
    request.message_name = "BeginUserSpotJoinReq";
    auto outgoing = encode_actor_packet (request, zlink::message_t::from ("{}"));
    if (outgoing[0].to_bytes () != request_header)
        return 3;
    // Independent complete STREAM frames, with seq=1 and no packet name.
    const std::vector<std::uint8_t> response{0, 13, 0, 0, 0, 2, 0xf2, 3, 1,   1,  0,
                                             0, 0,  0, 0, 0, 0, 1,    0, '{', '}'};
    auto decoded_reply = decode_actor_reply (messaging::message_parts_t (
      std::vector<zlink::message_t>{zlink::message_t::from (response)}));
    if (!decoded_reply || !decoded_reply.value () || decoded_reply.value ()->to_string () != "{}")
        return 4;
    messaging::envelope_header_t reply;
    reply.kind = messaging::message_kind_t::response;
    if (encode_actor_reply (reply, zlink::message_t::from ("{}"))[0].to_bytes () != response)
        return 5;
    for (auto bad : {std::vector<zlink::message_t>{},
                     std::vector<zlink::message_t>{zlink::message_t::from ("{}")},
                     std::vector<zlink::message_t>{outgoing[0], outgoing[1]}}) {
        auto invalid = decode_actor_reply (messaging::message_parts_t (std::move (bad)));
        if (invalid || invalid.error_kind () != framework_error_kind_t::protocol_error)
            return 6;
    }
    const std::string foreign_error = "{\"code\":\"not_found\",\"message\":\"missing\"}";
    auto foreign_error_frame = response;
    foreign_error_frame.resize (19);
    foreign_error_frame[5] = static_cast<std::uint8_t> (foreign_error.size ());
    foreign_error_frame[7] = 4;
    foreign_error_frame.insert (foreign_error_frame.end (), foreign_error.begin (),
                                foreign_error.end ());
    auto foreign_failure = decode_actor_reply (messaging::message_parts_t (
      std::vector<zlink::message_t>{zlink::message_t::from (foreign_error_frame)}));
    if (foreign_failure || foreign_failure.error_kind () != framework_error_kind_t::not_found)
        return 10;
    for (const auto offset : {0u, 5u, 7u, 8u, 18u}) {
        auto malformed = response;
        malformed[offset] = 0xff;
        auto rejected = decode_actor_reply (messaging::message_parts_t (
          std::vector<zlink::message_t>{zlink::message_t::from (malformed)}));
        if (rejected || rejected.error_kind () != framework_error_kind_t::protocol_error)
            return 11;
    }
    for (const auto kind :
         {framework_error_kind_t::not_found, framework_error_kind_t::not_configured,
          framework_error_kind_t::rejected, framework_error_kind_t::deadline_exceeded}) {
        const auto error = channel_reply_writer_t{}.create_error_header (
          "actor", request, framework_exception_t (kind, "missing"));
        auto framed = encode_actor_reply (error, zlink::message_t{});
        if (framed.size () != 1)
            return 7;
        auto [header, body] = stream_runtime_t::decode_frame (framed.items (), false);
        if (header.kind () != stream_message_kind_t::error || !header.packet_name ().empty ()
            || header.codec () != stream_codec_t::json)
            return 8;
        auto decoded_error = decode_actor_reply (framed);
        if (decoded_error || decoded_error.error_kind () != kind
            || std::string (decoded_error.error ()->what ()) != "missing")
            return 9;
    }
    {
        const std::string flow_id = "01890a5d-ac96-774b-bcce-b302099a8057";
        runtime::flow_context_t::scope_t flow (runtime::flow_value_t{
          flow_id, flow_origin_t::application, message_flow_log_mode_t::normal});
        auto flowed = encode_actor_packet (request, zlink::message_t::from ("{}"));
        auto header = stream_runtime_t::decode_header (flowed[0].to_bytes (), true);
        if (!header || header.value ().flow_id () != flow_id
            || header.value ().flow_origin () != flow_origin_t::application) {
            std::cerr << "Actor STREAM header lost the ambient flow\n";
            return 13;
        }
        auto framed = encode_actor_reply (reply, zlink::message_t::from ("{}"));
        const auto reply_header = stream_runtime_t::decode_frame (framed.items (), true).first;
        if (reply_header.flow_id () != flow_id
            || reply_header.flow_origin () != flow_origin_t::application)
            return 14;
    }
    {
        runtime::flow_context_t::scope_t flow (
          runtime::flow_value_t{"01890a5d-ac96-774b-bcce-b302099a8057", flow_origin_t::application,
                                message_flow_log_mode_t::off});
        auto off = encode_actor_packet (request, zlink::message_t::from ("{}"));
        if (off[0].to_bytes () != request_header)
            return 15;
    }
    return 0;
}

int main (int argc, char **argv)
{
    zlink::framework::completion_owner_t completion_owner;
    if (argc == 2 && std::string (argv[1]) == "--actor-typed-wire")
        return actor_typed_wire_header_contract ();
    if (const auto wire = actor_typed_wire_header_contract (); wire != 0)
        return wire;
    if (argc == 1) {
        if (const auto registration = bind_registration_contract_vectors (); registration != 0)
            return registration;
    }
    if (argc == 2) {
        const std::string test (argv[1]);
        if (test == "--bind-registration")
            return bind_registration_contract_vectors ();

        if (test == "--prepare-runtime-failure")
            return relocation_prepare_preserves_runtime_failure ();
        if (test == "--join-completion-callback")
            return join_completion_callback_failure_is_reported ();
        if (test == "--close-unbound")
            return close_unbound_session_reports_invalid_operation ();
        if (test == "--close-binding")
            return close_exact_session_binding_uses_physical_terminal ();
        if (test == "--known-generation-source-leave") {
            const auto result = known_generation_source_leave_claims_transfer ();
            std::cerr << "Known generation source leave result=" << result << '\n';
            return result;
        }
        if (test == "--session-disconnected-public")
            return session_disconnect_resolves_context_without_ingress_metadata (false);
        if (test == "--session-disconnected-replay")
            return session_disconnect_resolves_context_without_ingress_metadata (true);
        if (test == "--session-disconnected-terminal")
            return physical_disconnect_submits_all_before_waiting_for_terminal ();
        if (test == "--session-disconnected-gate")
            return session_disconnect_callback_owns_spot_lifecycle_gate ();
        if (test == "--session-disconnected-wire") {
            const auto identity = common_session_disconnect_uses_actor_packet_route ();
            std::cerr << "Session disconnect identity/turn/capture: " << identity << std::endl;
            if (identity != 0)
                return identity;
            const auto followed =
              old_owner_forwards_cold_probe_via_active_message_follow_route (true);
            std::cerr << "Session disconnect Message Follow: " << followed << std::endl;
            if (followed != 0)
                return followed;
            const auto all_settled = physical_disconnect_submits_all_before_waiting_for_terminal ();
            std::cerr << "Session disconnect all-settled submission: " << all_settled << std::endl;
            if (all_settled != 0)
                return all_settled;
            const auto snapshot = session_disconnect_dispatcher_preserves_identity_snapshot ();
            std::cerr << "Session disconnect source snapshot: " << snapshot << std::endl;
            return snapshot;
        }
        if (test == "--unbound-physical")
            return actor_unbound_follows_session_termination (true);
        if (test == "--unbound-logical")
            return actor_unbound_follows_session_termination (false);
        if (test == "--unbound-disconnect-then-shutdown") {
            constexpr auto stress_iterations = 100;
            for (auto iteration = 0; iteration < stress_iterations; ++iteration) {
                if (const auto result = actor_unbound_follows_session_termination (true, true);
                    result != 0)
                    return result;
            }
            return 0;
        }
        if (test == "--reconcile-source")
            return reconcile_deadline_fast_fails_when_store_shows_source ();
        if (test == "--reconcile-no-commit")
            return reconcile_deadline_fast_fails_when_store_has_no_commit ();
        if (test == "--reconcile-target")
            return reconcile_deadline_adopts_target_when_store_shows_committed ();
        if (test == "--reconcile-indeterminate")
            return reconcile_deadline_fast_fails_when_store_is_indeterminate ();
        if (test == "--infrastructure-store-wait")
            return remote_actor_join_store_read_does_not_wait_for_handler_workers ();
        if (test == "--fifo-request-suffix") {
            const auto result = relocation_barrier_fences_late_actor_request_without_deadlock ();
            std::cerr << test << " result=" << result << '\n';
            return result;
        }
        if (test == "--fifo-reserved-suffix" || test == "--fifo-committed-before-post") {
            const auto result = relocation_barrier_fences_late_actor_fifo_admission (
              test == "--fifo-committed-before-post");
            std::cerr << test << " result=" << result << '\n';
            return result;
        }
        if (test == "--native-relay-route")
            return local_relay_preserves_handoff_request_route (true);
        if (test == "--native-relay-operation")
            return local_relay_preserves_handoff_request_route (false);
        if (test == "--native-relay-deadline")
            return local_relay_preserves_handoff_request_route (false, true);
        if (test == "--native-relay-missing")
            return local_relay_failure_returns_without_waiting_for_handoff_deadline ();
        if (test == "--native-relay-replay")
            return parked_request_reply_case ("actor-a", parked_request_case_t::direct_replay);
        return 1;
    }
    if (const auto gate = session_disconnect_callback_owns_spot_lifecycle_gate (); gate != 0)
        return 207;
    if (const auto all_settled = physical_disconnect_submits_all_before_waiting_for_terminal ();
        all_settled != 0)
        return 206;
    if (const auto snapshot = session_disconnect_dispatcher_preserves_identity_snapshot ();
        snapshot != 0)
        return 570 + snapshot;
    if (const auto followed = old_owner_forwards_cold_probe_via_active_message_follow_route (true);
        followed != 0)
        return 560 + followed;
    for (const bool replay : {false, true}) {
        if (const auto context =
              session_disconnect_resolves_context_without_ingress_metadata (replay);
            context != 0)
            return context;
    }
    if (const auto disconnected = common_session_disconnect_uses_actor_packet_route ();
        disconnected != 0)
        return 550 + disconnected;
    if (const auto route = local_relay_preserves_handoff_request_route (true); route != 0)
        return 450 + route;
    if (const auto route = local_relay_preserves_handoff_request_route (false); route != 0)
        return 455 + route;
    if (const auto deadline = local_relay_preserves_handoff_request_route (false, true);
        deadline != 0)
        return 465 + deadline;
    if (const auto failure = local_relay_failure_returns_without_waiting_for_handoff_deadline ();
        failure != 0)
        return 460 + failure;
    if (const auto ordered = terminal_reply_precedes_session_route_publication (); ordered != 0)
        return 430 + ordered;
    if (const auto drained = actor_unbound_waits_for_accepted_push_fifo (); drained != 0)
        return 435 + drained;
    if (const auto ordered = session_disconnect_uses_actor_slot_issue_order (); ordered != 0)
        return 440 + ordered;
    if (const auto protobuf_bound = generated_protobuf_bound_session_uses_typed_serializer_codec ();
        protobuf_bound != 0) {
        return 420 + protobuf_bound;
    }
    if (const auto handoff_fence = relocation_barrier_fences_late_actor_fifo_admission ();
        handoff_fence != 0) {
        std::cerr << "--fifo-reserved-suffix result=" << handoff_fence << '\n';
        return 400 + handoff_fence;
    }
    if (const auto completed_commit = relocation_barrier_fences_late_actor_fifo_admission (true);
        completed_commit != 0)
        return 470 + completed_commit;
    if (const auto handoff_fence_request =
          relocation_barrier_fences_late_actor_request_without_deadlock ();
        handoff_fence_request != 0) {
        return 410 + handoff_fence_request;
    }
    if (const auto completion_delivery =
          join_completion_waits_for_bound_session_delivery_terminal ();
        completion_delivery != 0) {
        return 395 + completion_delivery;
    }
    if (const auto store_resolution =
          remote_actor_join_resolves_store_type_and_reports_typed_terminals ();
        store_resolution != 0) {
        return 390 + store_resolution;
    }
    if (const auto pending_capacity = pending_handoff_accepts_beyond_former_capacity ();
        pending_capacity != 0) {
        return 385 + pending_capacity;
    }
    if (const auto parked_reply = parked_request_without_route_fence_receives_reply_after_replay ();
        parked_reply != 0) {
        return 360 + parked_reply;
    }
    if (const auto direct_reply =
          parked_request_reply_case ("actor-a", parked_request_case_t::direct_replay);
        direct_reply != 0) {
        return 375 + direct_reply;
    }
    if (const auto scoped_terminal =
          same_operation_from_distinct_source_lifecycles_has_distinct_pending_terminal ();
        scoped_terminal != 0) {
        return 365 + scoped_terminal;
    }
    if (const auto follow_reply =
          relayed_request_without_deferred_terminal_receives_follow_reply ();
        follow_reply != 0) {
        return 370 + follow_reply;
    }
    if (const auto cross_node_terminal =
          parked_cross_node_request_terminal_returns_to_parking_node ();
        cross_node_terminal != 0) {
        return 380 + cross_node_terminal;
    }
    if (const auto prepare = relocation_prepare_preserves_runtime_failure (); prepare != 0)
        return prepare;
    if (const auto callback = join_completion_callback_failure_is_reported (); callback != 0)
        return callback;
    if (const auto capacity = session_relay_waiter_accepts_beyond_former_capacity ();
        capacity != 0) {
        return 290 + capacity;
    }
    if (const auto finite = bind_or_get_all_exit_paths_complete_within_deadline (); finite != 0) {
        return 285 + finite;
    }
    if (const auto reentrant = bind_or_get_completion_can_reenter_same_manager (); reentrant != 0) {
        return 280 + reentrant;
    }
    if (const auto command_38 = command_38_rebind_is_owned_only_by_new_connection ();
        command_38 != 0) {
        return 270 + command_38;
    }
    if (const auto monotonic = late_lower_generation_bind_and_record_are_rejected ();
        monotonic != 0) {
        return 265 + monotonic;
    }
    if (const auto disconnect_fence = old_stream_disconnect_does_not_retire_reconnected_binding ();
        disconnect_fence != 0) {
        return 260 + disconnect_fence;
    }
    if (const auto handlers = same_rid_registration_retains_retired_close_handler ();
        handlers != 0) {
        return 250 + handlers;
    }
    if (const auto defensive = same_identity_registration_keeps_logical_binding_current ();
        defensive != 0) {
        return 245 + defensive;
    }
    if (const auto reconnect = reconnect_push_reaches_new_session_across_two_hosts ();
        reconnect != 0) {
        return 240 + reconnect;
    }
    if (const auto detached = bound_session_push_detaches_before_direct_sink_entry ();
        detached != 0) {
        return 220 + detached;
    }
    if (const auto admitted =
          admitted_bound_session_delivery_keeps_original_session_after_rebind ();
        admitted != 0) {
        return 225 + admitted;
    }
    if (const auto fence = rebound_session_keeps_prior_ingress_exact_fence (); fence != 0) {
        return 210 + fence;
    }
    if (const auto admission =
          actor_request_completion_keeps_dedup_state_owned_after_runtime_wrapper_unwinds ();
        admission != 0) {
        return 200 + admission;
    }
    if (const auto cold_forward = old_owner_forwards_cold_probe_via_active_message_follow_route ();
        cold_forward != 0) {
        return 300 + cold_forward;
    }
    if (const auto returned_owner =
          returned_owner_serves_locally_past_retained_message_follow_route ();
        returned_owner != 0) {
        return 305 + returned_owner;
    }
    if (const auto leave_race = source_cleanup_waits_for_leave_completion_before_erasing_actor ();
        leave_race != 0) {
        return 310 + leave_race;
    }
    if (const auto sweep_race = sweep_preserves_pending_leave_added_after_snapshot ();
        sweep_race != 0) {
        return 315 + sweep_race;
    }
    if (const auto reconcile_trap = reconcile_deadline_fast_fails_when_store_shows_source ();
        reconcile_trap != 0) {
        return 320 + reconcile_trap;
    }
    if (const auto no_commit = reconcile_deadline_fast_fails_when_store_has_no_commit ();
        no_commit != 0) {
        return 360 + no_commit;
    }
    if (const auto reconcile_adopt = reconcile_deadline_adopts_target_when_store_shows_committed ();
        reconcile_adopt != 0) {
        return 340 + reconcile_adopt;
    }
    if (const auto reconcile_indeterminate =
          reconcile_deadline_fast_fails_when_store_is_indeterminate ();
        reconcile_indeterminate != 0) {
        return 350 + reconcile_indeterminate;
    }
    if (const auto leave_notify =
          leave_notification_travels_node_level_and_reaches_source_entry_spot_once ();
        leave_notify != 0) {
        return 330 + leave_notify;
    }
    if (const auto no_follow_leave =
          early_zero_generation_leave_waits_for_source_transfer_completion (true);
        no_follow_leave != 0)
        return 500 + no_follow_leave;
    if (const auto known_leave = known_generation_source_leave_claims_transfer ();
        known_leave != 0) {
        return 380 + known_leave;
    }
    if (const auto early_leave =
          early_zero_generation_leave_waits_for_source_transfer_completion ();
        early_leave != 0) {
        return 390 + early_leave;
    }
    if (const auto pending = disconnect_notification_survives_pending_dispatcher_completion ();
        pending != 0) {
        return 190 + pending;
    }
    if (const auto pending = session_manager_disconnect_survives_pending_dispatcher_completion ();
        pending != 0) {
        return 195 + pending;
    }
    if (const auto pending = relay_request_survives_pending_dispatcher_completion ();
        pending != 0) {
        return 160 + pending;
    }
    if (const auto pending = relay_send_survives_pending_dispatcher_completion (); pending != 0) {
        return 170 + pending;
    }
    if (const auto queue = session_relay_queue_is_ordered_without_blocking_other_actors ();
        queue != 0) {
        return 150 + queue;
    }
    if (const auto execution = session_relay_does_not_start_actor_dispatch_on_session_thread ();
        execution != 0) {
        return 155 + execution;
    }
    if (const auto admission = bound_session_relay_admission_is_exact_and_monotonic ();
        admission != 0) {
        return 140 + admission;
    }
    if (const auto dispatch =
          bound_session_actor_dispatch_uses_current_binding_without_location_reread ();
        dispatch != 0) {
        return 360 + dispatch;
    }
    if (const auto transition = bound_session_transition_is_atomic_and_idempotent ();
        transition != 0) {
        return 130 + transition;
    }
    if (const auto route_update = authority_only_route_update_keeps_physical_session_current ();
        route_update != 0) {
        return 180 + route_update;
    }
    if (const auto identity = actor_identity_validation_is_bounded_and_utf8_exact ();
        identity != 0) {
        return 120 + identity;
    }
    if (const auto relay_scope = relay_dispatch_scope_restores_nested_and_exception_state ();
        relay_scope != 0) {
        return 110 + relay_scope;
    }
    if (const auto relay_await = relay_dispatch_header_follows_handler_await (); relay_await != 0) {
        return 465 + relay_await;
    }
    if (const auto route_fence = bound_session_route_preserves_private_fences ();
        route_fence != 0) {
        return 100 + route_fence;
    }
    if (const auto pending_route = bound_session_route_suspends_until_owner_lane_runs ();
        pending_route != 0) {
        return 100 + pending_route;
    }
    if (const auto bound_send = bound_session_send_does_not_publish_caller_location ();
        bound_send != 0) {
        return 95 + bound_send;
    }
    if (const auto lifetime =
          app_bound_session_sender_keeps_arguments_alive_after_route_lookup_suspends ();
        lifetime != 0) {
        return 390 + lifetime;
    }
    if (const auto relocation_prewarm =
          relocation_target_prewarm_publishes_store_confirmed_actor_and_session_fence_atomically ();
        relocation_prewarm != 0) {
        return 370 + relocation_prewarm;
    }
    if (const auto stale_binding =
          stale_binding_registration_and_relocation_prepare_preserve_current_tenure ();
        stale_binding != 0) {
        return 380 + stale_binding;
    }
    if (const auto normalized =
          bound_session_ref_normalization_preserves_type_and_rejects_conflicts ();
        normalized != 0) {
        return 80 + normalized;
    }
    if (const auto atomic_route = bound_session_route_installs_sink_and_fence_together ();
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
    if (const auto bound = bound_actors_are_current_ordered_snapshot (); bound != 0) {
        return 10 + bound;
    }
    if (const auto idempotent = bind_or_get_reuses_same_physical_session_generation ();
        idempotent != 0) {
        return 155 + idempotent;
    }
    if (const auto atomic_rebind =
          direct_rebind_publication_is_atomic_and_old_disconnect_is_fenced ();
        atomic_rebind != 0) {
        return 160 + atomic_rebind;
    }
    if (const auto stale_destroy = destroyed_or_recreated_actor_ignores_stale_disconnect_handle ();
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
    const auto disconnected = session_disconnect_is_all_settled_and_token_fenced ();
    if (disconnected != 0)
        return 20 + disconnected;
    const auto logical = logical_disconnect_is_selected_and_keeps_session_live ();
    if (logical != 0)
        return 30 + logical;
    const auto route = route_update_preserves_object_generation ();
    return route == 0 ? 0 : 40 + route;
}
