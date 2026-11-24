/* SPDX-License-Identifier: Elastic-2.0 */

#include "runtime/dispatch/coroutine_executor.hpp"
#include "../support/session_disconnect_probe_spot.hpp"
#include "../support/request_admission_fixture.hpp"
#include "../support/actor_gateway_cases.hpp"
#include "../support/actor_gateway_fixture.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/protocol/service_wire_codec.hpp"
#include "runtime/spots/spot_route_packets.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include <nlohmann/json.hpp>
#include <zlink/framework/contracts/dispatch/request_task.hpp>
#include <zlink/framework/contracts/dispatch/task.hpp>
#include <zlink/framework/contracts/streams/stream.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <typeindex>
#include <utility>
#include <vector>

namespace zlink::framework::test::actor_gateway
{
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

int session_relay_queue_is_ordered_without_blocking_other_actors ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    serializer_registry_t relay_serializers;
    actor_gateway_runtime_t gateway;
    gateway.bind_serializers (relay_serializers);
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

    auto first = bound_a.relay ("A1", zlink::framework::message_t{});
    auto second = bound_a.relay ("A2", zlink::framework::message_t{});
    auto independent = bound_b.relay ("B1", zlink::framework::message_t{});
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

    serializer_registry_t relay_serializers;
    actor_gateway_runtime_t gateway;
    gateway.bind_serializers (relay_serializers);
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

    const auto delivered = binding.relay ("execution", zlink::framework::message_t{}).result ();
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

    serializer_registry_t relay_serializers;
    actor_gateway_runtime_t gateway;
    gateway.bind_serializers (relay_serializers);
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
        return test::accepted_request_task (inspect_pending_relay_arguments (
          relayed_actor, header, payload, pending, relay_started, actor_id, packet_name,
          nlohmann::json (payload_text).dump ()));
    });

    auto request =
      bound.relay_request (packet_name, zlink::framework::message_t::from (payload_text)).async ();
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
    return completed && completed.value ().encoded ()
               && detail::message_to_raw (completed.value (), relay_serializers).to_string ()
                    == "delayed-reply"
             ? 0
             : 2;
}

int relay_send_survives_pending_dispatcher_completion ()
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;

    serializer_registry_t relay_serializers;
    actor_gateway_runtime_t gateway;
    gateway.bind_serializers (relay_serializers);
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
        return test::accepted_request_task (inspect_pending_relay_arguments (
          relayed_actor, header, payload, pending, relay_started, actor_id, packet_name,
          nlohmann::json (payload_text).dump ()));
    });

    auto relayed = bound.relay (packet_name, zlink::framework::message_t::from (payload_text));
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
    const auto passed =
      disconnected_result && disconnected_started.load (std::memory_order_acquire) ? 0 : 6;
    zlink::framework::detail::spot_node_runtime_t (node).release_native_handles ();
    return passed;
}
} // namespace zlink::framework::test::actor_gateway
