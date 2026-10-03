/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include <zlink/framework.hpp>

#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/streams/stream_runtime.hpp"
#include "runtime/spots/spot_runtime.hpp"
#include "runtime/stateful/public_host_runtime.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <atomic>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace
{

using namespace zlink::framework;
using namespace zlink::framework::detail;
using namespace std::chrono_literals;
namespace stateful = zlink::framework::runtime::stateful;

struct started_service_t final : hosted_service_t
{
    std::promise<void> &started;
    explicit started_service_t (std::promise<void> &value) : started (value) {}
    task_t<void> start (service_provider_t &) override
    {
        started.set_value ();
        co_return;
    }
    void stop () noexcept override {}
};

struct running_app_t
{
    app_t &app;
    std::promise<void> started;
    std::future<void> ready;
    std::thread thread;

    explicit running_app_t (app_t &value) : app (value), ready (started.get_future ())
    {
        app.add_hosted_service (std::make_unique<started_service_t> (started));
        thread = std::thread ([this] {
            const char *arguments[] = {"session-seal-relay"};
            app.run (1, const_cast<char **> (arguments));
        });
    }

    ~running_app_t ()
    {
        app.stop ();
        thread.join ();
    }
};

struct dispatch_loop_t
{
    zlink::framework::runtime::host::public_host_runtime_t &owner;
    std::atomic<bool> stopped{false};
    std::thread thread;

    template <typename Dispatch, typename Progress>
    dispatch_loop_t (zlink::framework::runtime::host::public_host_runtime_t &value,
                     Dispatch dispatch,
                     Progress progress) :
        owner (value), thread ([this, dispatch, progress] {
            while (!stopped.load ()) {
                const auto result = owner.dispatch_ready (dispatch).result ();
                EXPECT_TRUE (result);
                if (!result)
                    return;
                progress ();
                (void) owner.wait_for_dispatch_activity (2s, true, std::nullopt);
            }
        })
    {
    }

    ~dispatch_loop_t ()
    {
        stopped.store (true);
        owner.signal_dispatch_activity ();
        thread.join ();
    }
};

void verify_relay_seal (bool close_owner, bool request_timeout = false)
{
    std::mutex events_mutex;
    std::condition_variable events_changed;
    std::vector<message_flow_event_t> events;
    const auto owner_rid = zlink::routing_id_t::from ("seal-relay-owner");
    auto app = app_t::create ();
    app.set_message_flow_mode (message_flow_log_mode_t::normal);
    app.add_zlink_framework ([&] (zlink_framework_options_t &options) {
        options.add_route_mesh ("seal-relay-mesh")
          .set_object_role (object_role_t::none)
          .set_routing_id (owner_rid)
          .listen ("tcp://127.0.0.1:0");
        dispatch_options_access_t::set_observer_for_tests (
          options.configure_dispatch (), [&] (const message_flow_event_t &event) {
              const std::lock_guard lock (events_mutex);
              events.push_back (event);
              events_changed.notify_all ();
          });
    });
    auto provider = app.advanced ().services ().build_provider ();
    auto &gateway = provider.get_required<actor_gateway_runtime_t> ();
    const auto registration =
      mesh_node_runtime_t::registrations (app.advanced ().zlink ()).front ();
    spot_node_runtime_t spots (registration->spot_state);
    running_app_t running (app);
    ASSERT_EQ (running.ready.wait_for (2s), std::future_status::ready);
    auto native = spots.native_node ();
    ASSERT_TRUE (native);

    const auto actor = actor_ref_access_t::make (node_rid_t::from_string ("seal-relay-target"),
                                                 "player", "seal-relay-actor", 7);
    const stateful::object_ref_t actor_object{stateful::object_kind_t::actor,
                                              "seal-relay-actor",
                                              7,
                                              11,
                                              "seal-relay-mesh",
                                              "seal-relay-target"};
    const auto session_rid = zlink::routing_id_t::from ("seal-relay-session");
    auto &sessions = native->sessions ();
    const auto connection = sessions.open (session_rid.to_hex ());
    const auto [error, binding] = sessions.bind_remote (connection, actor_object, 13, 17);
    ASSERT_EQ (error, stateful::stateful_error_t::none);
    auto manager = gateway.manager ();
    const auto bound_result = manager.bind (actor).async ().result ();
    ASSERT_TRUE (bound_result);
    auto bound = bound_result.value ();
    ASSERT_TRUE (gateway.record_bound_session_route (actor, owner_rid, session_rid, 1, 11, 17,
                                                     binding.binding_generation));
    ASSERT_TRUE (
      gateway.record_session_relay_source (actor, session_rid, binding.binding_generation));
    const auto seal =
      sessions.seal_remote_route (connection.connection_id, binding.binding_generation,
                                  actor_object.key, actor_object.object_generation);
    ASSERT_EQ (seal.error, stateful::stateful_error_t::none);

    std::optional<session_actor_t> other_bound;
    stateful::stream_barrier_t other_barrier;
    if (!close_owner && !request_timeout) {
        const auto other_actor = actor_ref_access_t::make (
          node_rid_t::from_string ("seal-relay-target"), "player", "seal-other-actor", 7);
        auto other_object = actor_object;
        other_object.key = "seal-other-actor";
        const auto [other_error, other_binding] =
          sessions.bind_remote (connection, other_object, 13, 17);
        ASSERT_EQ (other_error, stateful::stateful_error_t::none);
        const auto other = manager.bind (other_actor).async ().result ();
        ASSERT_TRUE (other);
        other_bound = other.value ();
        ASSERT_TRUE (gateway.record_bound_session_route (other_actor, owner_rid, session_rid, 1, 11,
                                                         17, other_binding.binding_generation));
        ASSERT_TRUE (gateway.record_session_relay_source (other_actor, session_rid,
                                                          other_binding.binding_generation));
        const auto other_seal =
          sessions.seal_remote_route (connection.connection_id, other_binding.binding_generation,
                                      other_object.key, other_object.object_generation);
        ASSERT_EQ (other_seal.error, stateful::stateful_error_t::none);
        other_barrier = other_seal.barrier;
        ASSERT_TRUE (
          other_bound->relay ("OtherSealRelay", zlink::message_t::from ("other")).result ());
    }

    auto completed = std::async (std::launch::async, [bound, request_timeout] () mutable {
        detail::stream_header_t header (request_timeout ? detail::stream_message_kind_t::request
                                                        : detail::stream_message_kind_t::send,
                                        stream_codec_t::json, detail::stream_header_flags_t::none,
                                        request_timeout ? std::optional<std::uint32_t>{17}
                                                        : std::nullopt,
                                        request_timeout ? "SealRequest" : "SealRelay");
        header.with_flow ("0199a500-0000-7000-8000-000000000001", flow_origin_t::inbound);
        header.with_correlation_id ("seal-correlation");
        const detail::stream_relay_dispatch_scope_t scope (header);
        if (request_timeout) {
            const auto reply = bound.relay_request (zlink::message_t::from ("payload"))
                                 .timeout (50ms)
                                 .async ()
                                 .result ();
            return reply ? result_t<void>::success ()
                         : result_t<void>::failure (reply.error_kind (), reply.error ()->what ());
        }
        auto relay = bound.relay (zlink::message_t::from ("payload"));
        return relay.result ();
    });
    const auto before_release = completed.wait_for (2s);
    EXPECT_EQ (before_release, std::future_status::ready)
      << "one-way relay must complete at Session owner seal retention admission";
    if (request_timeout) {
        const auto next = bound.relay ("AfterExpired", zlink::message_t::from ("next")).result ();
        ASSERT_TRUE (next) << "next relay must be accepted on the same sealed binding";
    }
    if (!close_owner && !request_timeout)
        ASSERT_TRUE (bound.relay ("SealRelaySecond", zlink::message_t::from ("second")).result ());
    if (close_owner)
        EXPECT_TRUE (sessions.close (connection));
    else
        EXPECT_EQ (sessions.abort_barrier (seal.barrier), stateful::stateful_error_t::none);
    ASSERT_EQ (completed.wait_for (2s), std::future_status::ready);
    const auto terminal = completed.get ();
    if (request_timeout) {
        EXPECT_FALSE (terminal);
        EXPECT_EQ (terminal.error_kind (), framework_error_kind_t::deadline_exceeded);
        EXPECT_EQ (native->pending_operation_count (), 0u);
        std::unique_lock lock (events_mutex);
        ASSERT_TRUE (events_changed.wait_for (lock, 2s, [&] {
            return std::any_of (events.begin (), events.end (), [] (const auto &event) {
                return event.packet_name == "AfterExpired"
                       && event.outcome == message_flow_outcome_t::dropped;
            });
        }));
        lock.unlock ();
        ASSERT_TRUE (gateway.bound_session_route_async (actor).result ().value ());
        EXPECT_EQ (gateway.bound_session_route_async (actor).result ().value ()->session_sequence,
                   2u);
        EXPECT_EQ (native->pending_operation_count (), 0u);
    } else {
        EXPECT_TRUE (terminal) << "delivery failure must not change accepted caller completion";
    }
    if (other_bound) {
        std::unique_lock lock (events_mutex);
        ASSERT_TRUE (events_changed.wait_for (lock, 2s, [&] {
            return std::count_if (events.begin (), events.end (),
                                  [] (const auto &event) {
                                      return event.outcome == message_flow_outcome_t::dropped
                                             && (event.packet_name == "SealRelay"
                                                 || event.packet_name == "SealRelaySecond");
                                  })
                   == 2;
        }));
        for (const auto &event : events) {
            if (event.outcome == message_flow_outcome_t::dropped
                && (event.packet_name == "SealRelay" || event.packet_name == "SealRelaySecond"))
                EXPECT_EQ (event.reason, message_flow_reason_t::stale_target);
            EXPECT_NE (event.packet_name, "OtherSealRelay");
        }
        lock.unlock ();
        EXPECT_TRUE (sessions.remote_route_sealed ("seal-other-actor"));
        EXPECT_EQ (gateway.bound_session_route_async (actor).result ().value ()->session_sequence,
                   2u);
        ASSERT_EQ (sessions.abort_barrier (other_barrier), stateful::stateful_error_t::none);
        lock.lock ();
        ASSERT_TRUE (events_changed.wait_for (lock, 2s, [&] {
            return std::any_of (events.begin (), events.end (), [] (const auto &event) {
                return event.packet_name == "OtherSealRelay"
                       && event.outcome == message_flow_outcome_t::dropped;
            });
        }));
        EXPECT_EQ (std::count_if (events.begin (), events.end (),
                                  [] (const auto &event) {
                                      return event.packet_name == "OtherSealRelay"
                                             && event.outcome == message_flow_outcome_t::dropped
                                             && event.reason == message_flow_reason_t::stale_target;
                                  }),
                   1);
    }
    if (close_owner) {
        const std::lock_guard lock (events_mutex);
        std::size_t drops = 0;
        for (const auto &event : events) {
            if (event.outcome == message_flow_outcome_t::dropped
                && event.surface == dispatch_error_surface_t::stream_session
                && event.packet_name == "SealRelay") {
                EXPECT_EQ (event.reason, message_flow_reason_t::target_closed);
                EXPECT_EQ (event.result, message_flow_result_t::dropped);
                EXPECT_EQ (event.flow_id, "0199a500-0000-7000-8000-000000000001");
                EXPECT_EQ (event.correlation_id, "seal-correlation");
                ++drops;
            }
        }
        EXPECT_EQ (drops, 1u);
    }
}

} // namespace

TEST (FrameworkSessionSealRelay, public_one_way_completes_before_seal_release)
{
    verify_relay_seal (false);
}

TEST (FrameworkSessionSealRelay, accepted_one_way_emits_one_drop_on_session_close)
{
    verify_relay_seal (true);
}

TEST (FrameworkSessionSealRelay, public_request_timeout_includes_seal_retention)
{
    verify_relay_seal (false, true);
}


TEST (FrameworkSessionSealRelay, public_request_preserves_wire_correlation_and_flow)
{
    namespace host = zlink::framework::runtime::host;
    namespace mesh = zlink::framework::runtime::mesh;
    namespace messaging = zlink::framework::runtime::messaging;
    const auto target_rid = zlink::routing_id_t::from ("seal-wire-target");
    auto target = std::make_shared<host::public_host_runtime_t> (
      host::host_options_t{mesh::raw_mesh_node_options_t{mesh::service_node_descriptor_t{
                             "seal-wire-mesh",
                             target_rid.to_bytes (),
                             1,
                             1,
                             "tcp://127.0.0.1:0",
                             {},
                             mesh::service_node_state_t::preparing}},
                           "entry",
                           {"player"}});
    target->configure_stateful_dispatch (
      [] (const stateful::accepted_record_authority_query_t &query)
        -> std::optional<stateful::accepted_record_authority_t> {
          return stateful::accepted_record_authority_t{{"seal-wire-source-owner", 17,
                                                        query.source_node_routing_id,
                                                        query.source_node_generation},
                                                       37};
      });
    target->start ();
    const auto target_status = target->status ();
    auto target_actor = target->create_actor ("player", "seal-wire-actor");
    const auto object = target->resolve_actor (target_actor.ref ());
    ASSERT_TRUE (object);
    auto source_state = std::make_shared<mesh_node_builder_state_t> ("seal-wire-mesh");
    source_state->core_context = std::make_shared<zlink::context_t> ();
    source_state->listen_endpoint = "tcp://127.0.0.1:0";
    source_state->routing_id = zlink::routing_id_t::from ("seal-wire-source");
    mesh_node_runtime_t source (source_state);
    source.configure_actor_route_resolver (
      [&] (const actor_ref_t &) -> std::optional<zlink::framework::runtime::spot_address_t> {
          return zlink::framework::runtime::spot_address_t{"seal-wire-mesh",
                                                           target_rid,
                                                           "entry",
                                                           1,
                                                           {},
                                                           object->object_generation,
                                                           object->authority_owner_generation,
                                                           {"seal-wire-target-owner", 37},
                                                           target_status.lifecycle_generation ()};
      });
    source.start ();
    source.connect_peer (target_rid, target_status.local_endpoint (),
                         target_status.lifecycle_generation ());
    const auto peer_ready = [&] {
        const auto target_peer =
          target->transport ().topology ().peer (source.status ().routing_id ().to_bytes ());
        return source.has_admitted_peer (target_rid, target_status.lifecycle_generation ())
               && target_peer
               && target_peer->descriptor.state == mesh::service_node_state_t::serving;
    };
    std::promise<void> admitted;
    auto admission = admitted.get_future ();
    std::once_flag admitted_once;
    const auto progress = [&] {
        if (peer_ready ())
            std::call_once (admitted_once, [&] { admitted.set_value (); });
    };
    std::promise<void> received;
    auto reception = received.get_future ();
    dispatch_loop_t source_loop (
      source.native_node (), [] (const auto &, const auto &, auto) {}, progress);
    dispatch_loop_t target_loop (
      *target,
      [&] (const host::ready_record_t &, const host::receive_record_t &record,
           std::vector<zlink::message_t> parts) {
          if (record.kind == host::record_kind_t::completion)
              return;
          messaging::message_parts_t encoded (std::move (parts));
          const auto header = messaging::envelope_codec_t{}.decode_header (encoded);
          ASSERT_TRUE (header);
          EXPECT_EQ (header.value ().correlation_id, "seal-wire-correlation");
          EXPECT_EQ (header.value ().flow_id, "0199a500-0000-7000-8000-000000000001");
          EXPECT_EQ (header.value ().flow_origin, flow_origin_t::inbound);
          auto response = header.value ();
          response.kind = messaging::message_kind_t::response;
          auto reply = messaging::envelope_codec_t{}.encode_raw_body_parts (
            response, zlink::message_t::from ("reply"));
          EXPECT_EQ (host::reply (record.reply_token, reply.items ()), zlink::submit_result_t::ok);
          received.set_value ();
      },
      progress);
    ASSERT_EQ (admission.wait_for (2s), std::future_status::ready);
    ASSERT_TRUE (peer_ready ());
    actor_gateway_runtime_t gateway;
    gateway.on_relay ([&] (const actor_ref_t &actor, const actor_context_t &,
                           const stream_header_t &header, const zlink::message_t &payload,
                           std::optional<bound_session_relay_source_t> relay_source,
                           std::chrono::milliseconds timeout) {
        return source.relay_application_actor (actor, header, payload, timeout, true,
                                               std::move (relay_source));
    });
    const auto actor = actor_ref_access_t::make (node_rid_t::from_string (target_rid.to_string ()),
                                                 "player", object->key, object->object_generation);
    auto manager = gateway.manager ();
    auto result = manager.bind (actor).async ().result ();
    ASSERT_TRUE (result);
    auto bound = result.value ();
    auto request = std::async (std::launch::async, [bound] () mutable {
        stream_header_t header (stream_message_kind_t::request, stream_codec_t::json,
                                stream_header_flags_t::none, 23, "SealWireRequest");
        header.with_flow ("0199a500-0000-7000-8000-000000000001", flow_origin_t::inbound);
        header.with_correlation_id ("seal-wire-correlation");
        const stream_relay_dispatch_scope_t scope (header);
        return bound.relay_request (zlink::message_t::from ("payload"))
          .timeout (2s)
          .async ()
          .result ();
    });

    EXPECT_EQ (reception.wait_for (2s), std::future_status::ready);
    ASSERT_EQ (request.wait_for (2s), std::future_status::ready);
    const auto terminal = request.get ();
    EXPECT_TRUE (terminal) << (terminal.error () ? terminal.error ()->what () : "");
}


TEST (FrameworkSessionSealRelay, different_sessions_do_not_share_replies_for_same_correlation)
{
    auto source_state = std::make_shared<mesh_node_builder_state_t> ("seal-dedup-mesh");
    source_state->core_context = std::make_shared<zlink::context_t> ();
    source_state->listen_endpoint = "tcp://127.0.0.1:0";
    source_state->routing_id = zlink::routing_id_t::from ("seal-dedup-owner");
    auto node = source_state->spot_state;
    node->worker_executor = std::make_shared<runtime::offload_executor_t> (1, "seal-dedup-worker");
    serializer_registry_t serializers;
    node->channel_runtime = std::make_shared<channel_runtime_state_t> ();
    node->channel_runtime->serializers = &serializers;
    auto spot = std::make_shared<spot_context_state_t> ();
    spot->node = node;
    spot->node_rid = node_rid_t::from_string ("seal-dedup-owner");
    spot->spot_id = spot_id_t ("seal-dedup-user");
    spot->spot_name = "user";
    spot->spot_instance = std::make_shared<int> (1);
    spot->channel_runtime = node->channel_runtime;
    spot->serial_executor = node->worker_executor;
    node->spot_contexts_by_id.emplace (spot->spot_id, spot_context_access_t::create (spot));
    spot_node_builder_state_t::actor_factory_registration_t factory;
    factory.actor_type = std::type_index (typeid (int));
    factory.create_instance = [] (std::string) { return std::make_shared<int> (7); };
    factory.configure_instance = [] (void *, const actor_ref_t &, void *) {};
    node->actor_factories.emplace ("player", std::move (factory));
    const auto actor = actor_ref_access_t::make (node_rid_t::from_string ("seal-dedup-owner"),
                                                 "player", "seal-dedup-actor", 1);
    const std::string actor_key = "player:seal-dedup-actor";
    node->actor_instances.emplace (actor_key, std::make_shared<int> (7));
    node->actor_spot_ids.emplace (actor_key, spot->spot_id);
    node->actor_generations.emplace (actor_key, 1);
    node->actor_types_by_id.emplace ("seal-dedup-actor", "player");
    std::atomic_uint32_t executions{0};
    runtime::protocol::wire_operation_id_t delivered_operation;
    std::uint64_t delivered_reply_route = 0;
    spot->handlers.push_back (
      spot_handler_descriptor_t{spot_handler_kind_t::actor_request, "SessionIdentityRequest", "",
                                std::type_index (typeid (int)), std::type_index (typeid (void)),
                                std::type_index (typeid (int)), std::type_index (typeid (void))});
    spot->handler_invokers.push_back (
      [&] (void *, void *, service_provider_t &, serializer_registry_t &,
           const zlink::message_t &payload,
           const spot_inbound_message_t &metadata) -> task_t<zlink::message_t> {
          delivered_operation = runtime::protocol::wire_operation_id_t{
            std::stoull (metadata.values.at (std::string (actor_handoff_operation_high_key))),
            std::stoull (metadata.values.at (std::string (actor_handoff_operation_low_key)))};
          delivered_reply_route =
            std::stoull (metadata.values.at (std::string (actor_handoff_reply_route_key)));
          executions.fetch_add (1, std::memory_order_relaxed);
          co_return payload;
      });
    service_collection_t services;
    services.add_singleton<actor_gateway_runtime_t> ();
    services.add_singleton<serializer_registry_t> ();
    node->root_services = services.build_provider ();
    mesh_node_runtime_t source (source_state);
    source.start ();
    actor_gateway_runtime_t gateway;
    gateway.on_relay ([&] (const actor_ref_t &target, const actor_context_t &,
                           const stream_header_t &header, const zlink::message_t &payload,
                           std::optional<bound_session_relay_source_t> relay_source,
                           std::chrono::milliseconds timeout) {
        return source.relay_application_actor (target, header, payload, timeout, true,
                                               std::move (relay_source));
    });
    zlink_builder_t builder;
    builder.stream ("seal-dedup-stream").bind ("tcp://127.0.0.1:0");
    auto streams = stream_runtime_t::from (builder);
    auto first_stream = streams.open_session ("seal-dedup-stream");
    auto second_stream = streams.open_session ("seal-dedup-stream");
    ASSERT_NE (first_stream.session_id (), second_stream.session_id ());
    for (auto *stream : {&first_stream, &second_stream})
        streams.attach_transport_writer (*stream,
                                         [] (const stream_header_t &, const zlink::message_t &,
                                             std::optional<std::chrono::milliseconds>) {
                                             return task_t<void> (result_t<void>::success ());
                                         });
    auto first_manager = gateway.manager ();
    auto second_manager = gateway.manager ();
    session_actor_manager_access_t::attach (first_manager, std::move (first_stream));
    session_actor_manager_access_t::attach (second_manager, std::move (second_stream));
    const auto invoke = [&] (session_actor_manager_t &manager, std::string payload) {
        auto binding = manager.bind (actor).async ().result ();
        EXPECT_TRUE (binding);
        if (!binding)
            return result_t<zlink::message_t>::failure (binding.error_kind (),
                                                        binding.error ()->what ());
        stream_header_t header (stream_message_kind_t::request, stream_codec_t::json,
                                stream_header_flags_t::none, 17, "SessionIdentityRequest");
        header.with_correlation_id ("same-upstream-correlation");
        const stream_relay_dispatch_scope_t scope (header);
        return binding.value ()
          .relay_request (zlink::message_t::from (std::move (payload)))
          .timeout (2s)
          .async ()
          .result ();
    };
    const auto first = invoke (first_manager, "first-session-result");
    ASSERT_TRUE (first) << (first.error () ? first.error ()->what () : "");
    EXPECT_EQ (first.value ().to_string (), "first-session-result");
    const auto second = invoke (second_manager, "second-session-result");
    ASSERT_TRUE (second) << (second.error () ? second.error ()->what () : "");
    EXPECT_EQ (second.value ().to_string (), "second-session-result");
    EXPECT_EQ (executions.load (std::memory_order_relaxed), 2u);
    const auto original = delivered_operation;
    const auto original_reply_route = delivered_reply_route;
    const auto replay = [&] (runtime::protocol::wire_operation_id_t operation,
                             std::string payload) {
        runtime::messaging::envelope_header_t header;
        header.kind = runtime::messaging::message_kind_t::request;
        header.message_name = "SessionIdentityRequest";
        header.content_type = "application/json";
        header.correlation_id = "same-upstream-correlation";
        return source
          .relay_application_actor (actor, header, zlink::message_t::from (std::move (payload)), 2s,
                                    source.status ().routing_id (), {}, 0, operation,
                                    original_reply_route)
          .result ();
    };
    const auto duplicate = replay (original, "second-session-result");
    ASSERT_TRUE (duplicate);
    ASSERT_TRUE (duplicate.value ());
    EXPECT_EQ (duplicate.value ()->to_string (), "second-session-result");
    EXPECT_EQ (executions.load (std::memory_order_relaxed), 2u);
    auto other_high = original;
    other_high.high ^= 0x100;
    const auto high_result = replay (other_high, "other-high-result");
    ASSERT_TRUE (high_result);
    ASSERT_TRUE (high_result.value ());
    EXPECT_EQ (high_result.value ()->to_string (), "other-high-result");
    auto other_low = original;
    other_low.low ^= 0x100;
    const auto low_result = replay (other_low, "other-low-result");
    ASSERT_TRUE (low_result);
    ASSERT_TRUE (low_result.value ());
    EXPECT_EQ (low_result.value ()->to_string (), "other-low-result");
    EXPECT_EQ (executions.load (std::memory_order_relaxed), 4u);
}
