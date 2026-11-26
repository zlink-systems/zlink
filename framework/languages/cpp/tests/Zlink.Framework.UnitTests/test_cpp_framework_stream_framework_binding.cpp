/* SPDX-License-Identifier: Elastic-2.0 */
#include "../support/telemetry_observation.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "../support/runtime_metric_contract.hpp"
#include "../support/host_readiness.hpp"

#include <zlink/framework.hpp>
#include <zlink/stream_connector.hpp>

#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/diagnostics/listener_status_registry.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/streams/stream_host_service.hpp"
#include "runtime/streams/stream_runtime.hpp"
#include "runtime/streams/session_liveness.hpp"
#include "runtime/stateful/public_host_runtime.hpp"
#include "runtime/dispatch/coroutine_executor.hpp"
#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/execution/serial_execution_queue.hpp"
#include <boost/beast/websocket.hpp>
#include "../support/loopback_tcp_endpoint.hpp"


#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/write.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifdef ZLINK_FRAMEWORK_STREAM_TEST_WITH_OPENSSL
#include <boost/asio/connect.hpp>
#include <boost/asio/ssl.hpp>
#endif
#include "../support/stream_framework_cases.hpp"
#include "../support/stream_framework_native_fixture.hpp"
#include "../support/stream_framework_session_fixture.hpp"

namespace zlink::framework::test::stream_framework
{
namespace
{
using zlink::framework::tests::endpoint_port;
using zlink::framework::tests::listener_endpoint;
using zlink::framework::tests::loopback_tcp_any_port;

class descriptor_drop_session_t final : public zlink::framework::packet_stream_session_t
{
  public:
    std::promise<zlink::routing_id_t> connected;
    zlink::framework::task_t<void> on_connected (zlink::framework::stream_t &stream) override
    {
        connected.set_value (stream.routing_id ().value ());
        co_return;
    }
    zlink::framework::task_t<void> on_disconnected (zlink::framework::stream_t &) override
    {
        co_return;
    }
    zlink::framework::task_t<void> on_error (zlink::framework::stream_t &,
                                             const zlink::framework::stream_error_t &) override
    {
        co_return;
    }
    zlink::framework::task_t<void> on_packet (zlink::framework::stream_t &,
                                              const zlink::framework::session_message_context_t &,
                                              const zlink::framework::message_t &) override
    {
        co_return;
    }
};

#ifdef ZLINK_FRAMEWORK_STREAM_TEST_WITH_OPENSSL
class close_probe_session_t final : public zlink::framework::packet_stream_session_t
{
  public:
    std::promise<zlink::framework::stream_t> connected;
    zlink::framework::task_completion_source_t<void> disconnect_terminal;
    zlink::framework::task_t<void> on_disconnected (zlink::framework::stream_t &) override
    {
        return disconnect_terminal.task ();
    }
    zlink::framework::task_t<void> on_error (zlink::framework::stream_t &,
                                             const zlink::framework::stream_error_t &) override
    {
        co_return;
    }
    zlink::framework::task_t<void> on_connected (zlink::framework::stream_t &stream) override
    {
        connected.set_value (stream);
        co_return;
    }
    zlink::framework::task_t<void> on_packet (zlink::framework::stream_t &,
                                              const zlink::framework::session_message_context_t &,
                                              const zlink::framework::message_t &) override
    {
        co_return;
    }
};
#endif
} // namespace

int descriptor_unavailable_replacement_records_drop ()
{
    using namespace zlink::framework;
    detail::configure_stream_dispatch_executor ();
    service_collection_t services;
    handler_registry_t handlers;
    serializer_registry_t serializers;
    auto builder = test::runtime_failure_builder ();
    detail::zlink_builder_access_t::bind_shared_core_context (
      builder, std::make_shared<zlink::context_t> ());
    services.add_singleton<detail::actor_gateway_runtime_t> ();
    services.add_factory<session_actor_manager_t> (
      [] (service_provider_t &provider) {
          return std::make_unique<session_actor_manager_t> (
            provider.get_required<detail::actor_gateway_runtime_t> ().manager ());
      },
      service_lifetime_t::scoped);
    auto session = std::make_shared<descriptor_drop_session_t> ();
    auto connected = session->connected.get_future ();
    zlink_framework_options_t options (services, handlers, serializers, builder);
    options.add_route_mesh ("drop-mesh")
      .set_routing_id (zlink::routing_id_t::from ("drop-node"))
      .listen (loopback_tcp_any_port);
    options.add_stream_node ("drop-stream")
      .bind (loopback_tcp_any_port)
      .register_session ("drop-session");
    options.apply ();
    auto drops = std::make_shared<std::atomic_int> (0);
    dispatch_options_t dispatch;
    dispatch.message_flow (message_flow_log_mode_t::normal);
    detail::dispatch_options_access_t::set_observer_for_tests (
      dispatch, [drops] (const message_flow_event_t &event) {
          if (event.packet_name == std::optional<std::string> ("boundSessionReplaced")
              && event.outcome == message_flow_outcome_t::dropped
              && event.reason == message_flow_reason_t::location_unavailable)
              ++*drops;
      });
    detail::apply_dispatch_options (builder, dispatch);
    for (const auto &registration : detail::mesh_node_runtime_t::registrations (builder))
        registration->core_context = detail::zlink_builder_access_t::shared_core_context (builder);
    auto provider = services.build_provider ();
    auto mesh = detail::mesh_node_runtime_t::from (builder, "drop-mesh");
    mesh->bind_serializers (serializers);
    mesh->start ().result ().value ();
    auto runtime = detail::stream_runtime_t::from (builder);
    auto listeners = std::make_shared<runtime::listener_status_registry_t> ();
    runtime::stream_host_service_t host (
      std::make_shared<runtime::runtime_failure_collector_t> (), runtime, runtime.snapshots (),
      {{"drop-session",
        [session] (service_provider_t &) -> packet_stream_session_t & { return *session; }}},
      std::chrono::seconds (30), mesh, {}, listeners);
    host.start (provider);
    auto client = connect_loopback (
      endpoint_port (listener_endpoint (*listeners, listener_kind_t::stream, "drop-stream")));
    int result = 0;
    if (!client || connected.wait_for (std::chrono::seconds (5)) != std::future_status::ready)
        result = 1;
    else {
        namespace protocol = zlink::framework::runtime::protocol;
        const protocol::bound_session_replaced_t notice{
          {"actor", 7, zlink::routing_id_t::from ("actor-owner").to_bytes (), 11, 13, 17},
          {mesh->status ().routing_id ().to_bytes (), mesh->status ().lifecycle_generation (),
           "owner", 19, connected.get ().to_bytes (), 23}};
        if (provider.get_required<detail::actor_gateway_runtime_t> ()
              .dispatch_bound_session_replaced (notice))
            result = 2;
        test::acknowledge_delivery ();
        if (*drops != 1)
            result = 3;
    }
    close_native_client (client);
    host.stop ();
    mesh->stop ();
    provider.close ();
    detail::shutdown_stream_dispatch_executor ();
    return result;
}

#ifdef ZLINK_FRAMEWORK_STREAM_TEST_WITH_OPENSSL
int transport_bound_close (bool tls, bool remote)
{
    zlink::framework::detail::configure_stream_dispatch_executor ();
    struct executor_guard_t
    {
        ~executor_guard_t () { zlink::framework::detail::shutdown_stream_dispatch_executor (); }
    } executor_guard;
    using namespace zlink::framework;
    namespace stateful = runtime::stateful;
    namespace protocol = runtime::protocol;
    using namespace std::chrono_literals;
    service_collection_t services;
    handler_registry_t handlers;
    serializer_registry_t serializers;
    auto builder = test::runtime_failure_builder ();
    detail::zlink_builder_access_t::bind_shared_core_context (
      builder, std::make_shared<zlink::context_t> ());
    services.add_singleton<detail::actor_gateway_runtime_t> ();
    services.add_factory<session_actor_manager_t> (
      [] (service_provider_t &provider) {
          return std::make_unique<session_actor_manager_t> (
            provider.get_required<detail::actor_gateway_runtime_t> ().manager ());
      },
      service_lifetime_t::scoped);
    zlink_framework_options_t options (services, handlers, serializers, builder);
    options.add_route_mesh ("transport-close-mesh")
      .set_routing_id (zlink::routing_id_t::from ("transport-close-owner"))
      .listen (loopback_tcp_any_port);
    auto listener = options.add_stream_node ("transport-close-stream");
    listener.bind (tls ? "tls://127.0.0.1:0" : "ws://127.0.0.1:0/close")
      .register_session ("close-session");
    if (tls)
        listener.set_tls_server (ZLINK_FRAMEWORK_STREAM_TEST_CERT, ZLINK_FRAMEWORK_STREAM_TEST_KEY,
                                 false);
    options.apply ();
    for (const auto &registration : detail::mesh_node_runtime_t::registrations (builder))
        registration->core_context = detail::zlink_builder_access_t::shared_core_context (builder);
    auto mesh = detail::mesh_node_runtime_t::from (builder, "transport-close-mesh");
    mesh->bind_serializers (serializers);
    mesh->start ().result ().value ();
    auto provider = services.build_provider ();
    auto runtime = detail::stream_runtime_t::from (builder);
    close_probe_session_t session;
    auto connected = session.connected.get_future ();
    auto listeners = std::make_shared<runtime::listener_status_registry_t> ();
    runtime::stream_host_service_t host (
      std::make_shared<runtime::runtime_failure_collector_t> (), runtime, runtime.snapshots (),
      {{"close-session",
        [&] (service_provider_t &) -> packet_stream_session_t & { return session; }}},
      30s, mesh, {}, listeners);
    struct cleanup_t
    {
        runtime::stream_host_service_t &host;
        detail::mesh_node_runtime_t &mesh;
        close_probe_session_t &session;
        ~cleanup_t ()
        {
            session.disconnect_terminal.complete (result_t<void>::success ());
            host.stop ();
            mesh.stop ();
        }
    } cleanup{host, *mesh, session};
    host.start (provider).result ().value ();
    const auto bound_endpoint =
      listener_endpoint (*listeners, listener_kind_t::stream, "transport-close-stream");
    // Listener status keeps the configured scheme (#5460).
    if (!bound_endpoint.starts_with (tls ? "tls://127.0.0.1:" : "ws://127.0.0.1:"))
        return 360;
    const auto port = endpoint_port (bound_endpoint);
    boost::asio::io_context io;
    boost::asio::ssl::context ssl (boost::asio::ssl::context::tls_client);
    ssl.set_verify_mode (boost::asio::ssl::verify_none);
    boost::asio::ssl::stream<boost::asio::ip::tcp::socket> tls_client (io, ssl);
    boost::beast::websocket::stream<boost::asio::ip::tcp::socket> ws_client (io);
    const boost::asio::ip::tcp::endpoint endpoint{boost::asio::ip::make_address ("127.0.0.1"),
                                                  port};
    if (tls) {
        tls_client.next_layer ().connect (endpoint);
        tls_client.handshake (boost::asio::ssl::stream_base::client);
    } else {
        ws_client.next_layer ().connect (endpoint);
        ws_client.handshake ("127.0.0.1", "/close");
    }
    if (connected.wait_for (2s) != std::future_status::ready)
        return 342;
    auto stream = connected.get ();
    const auto rid = stream.routing_id ().value ();
    auto &sessions = mesh->native_node ().sessions ();
    const auto status = mesh->native_node ().status ();
    const stateful::stream_connection_t connection{rid.to_hex (), 1, mesh->routing_id (),
                                                   status.lifecycle_generation ()};
    const stateful::object_ref_t actor{
      stateful::object_kind_t::actor,   "close-actor", 7, 11, "player",
      status.routing_id ().to_string ()};
    const auto [retired_error, retired_binding] = sessions.bind_remote (connection, actor, 1, 13);
    if (retired_error != stateful::stateful_error_t::none)
        return 343;
    const auto [error, binding] = sessions.bind_remote (connection, actor, 1, 13);
    if (error != stateful::stateful_error_t::none)
        return 343;
    const protocol::retired_bound_session_route_fence_t owner{
      status.routing_id ().to_bytes (), status.lifecycle_generation (), "session-owner", 17, {}, 0};
    protocol::bound_session_close_t request{
      1,
      actor.key,
      actor.object_generation,
      {owner.session_owner_node_routing_id, owner.session_owner_node_generation,
       owner.session_owner_id, owner.session_owner_lease_generation, rid.to_bytes (),
       binding.binding_generation}};
    auto stale = request;
    stale.session.retired_binding_generation = retired_binding.binding_generation;
    auto other_actor = actor;
    other_actor.key = "preserved-actor";
    const auto other_connection = sessions.open ("preserved-connection");
    const auto [other_error, other_binding] =
      sessions.bind_remote (other_connection, other_actor, 1, 13);
    if (other_error != stateful::stateful_error_t::none)
        return 343;
    auto caller = std::make_shared<runtime::host::public_host_runtime_t> (
      runtime::host::host_options_t{runtime::mesh::raw_mesh_node_options_t{
        .descriptor = {"transport-close-mesh", {'a'}, 1, 1, "tcp://127.0.0.1:0", {}},
        .runtime_failures = std::make_shared<runtime::runtime_failure_collector_t> ()}});
    mesh->native_node ().configure_session_route_owner (
      [] { return std::make_optional (location_owner_token_t{"session-owner", 17}); });
    std::atomic_bool stop_dispatch{false};
    if (remote) {
        caller->start ();
        caller->connect_peer (status.local_endpoint (), status.routing_id (),
                              status.lifecycle_generation ());
        const auto deadline = std::chrono::steady_clock::now () + 2s;
        const auto noop = [] (const auto &, const auto &, auto, auto) {};
        for (;;) {
            mesh->native_node ().dispatch_ready (noop).result ().value ();
            caller->dispatch_ready (noop).result ().value ();
            const auto outgoing =
              caller->transport ().topology ().peer (status.routing_id ().to_bytes ());
            const auto incoming = mesh->native_node ().transport ().topology ().peer (
              caller->status ().routing_id ().to_bytes ());
            if (outgoing && outgoing->ready && incoming && incoming->ready)
                break;
            if (std::chrono::steady_clock::now () >= deadline) {
                caller->close ();
                return 347;
            }
        }
    }
    std::thread dispatcher ([&] {
        const auto noop = [] (const auto &, const auto &, auto, auto) {};
        while (!stop_dispatch.load ()) {
            mesh->native_node ().dispatch_ready (noop).result ().value ();
            if (remote)
                caller->dispatch_ready (noop).result ().value ();
            std::this_thread::yield ();
        }
    });
    auto &transport = remote ? caller->transport () : mesh->native_node ().transport ();
    const auto rejected = transport.request_bound_session_close (stale, 2s).result ();
    const bool preserved = !rejected
                           && rejected.error_kind () == framework_error_kind_t::unavailable
                           && sessions.is_current (binding) && sessions.is_current (other_binding);
    const auto closed = transport.request_bound_session_close (request, 2s).result ();
    stop_dispatch.store (true);
    if (dispatcher.joinable ())
        dispatcher.join ();
    caller->close ();
    if (!preserved)
        return 344;
    if (!closed || !sessions.is_current (other_binding))
        return 345;
    // Physical close removes the canonical binding even while the Session's
    // disconnected callback remains pending.
    const auto cleanup_deadline = std::chrono::steady_clock::now () + 2s;
    while (sessions.is_current (binding) && std::chrono::steady_clock::now () < cleanup_deadline)
        std::this_thread::yield ();
    if (sessions.is_current (binding))
        return 348;
    // The native socket must already be closed when the close task completes.
    auto &socket = tls ? tls_client.next_layer () : ws_client.next_layer ();
    socket.non_blocking (true);
    std::array<char, 1> bytes{};
    boost::system::error_code terminal;
    do {
        socket.read_some (boost::asio::buffer (bytes), terminal);
    } while (!terminal);
    if (terminal != boost::asio::error::eof && terminal != boost::asio::error::connection_reset)
        return 346;
    return 0;
}
#endif

int binding_submission_order ()
{
    using namespace zlink::framework;
    auto builder = test::runtime_failure_builder ();
    builder.stream ("binding-order").bind ("tcp://127.0.0.1:0").register_session ("session");
    auto runtime = detail::stream_runtime_t::from (builder);
    auto stream = runtime.open_session ("binding-order");
    serializer_registry_t serializers;
    detail::actor_gateway_runtime_t gateway;
    gateway.weak_state ().lock ()->serializers = &serializers;
    auto manager = gateway.manager ();
    detail::session_actor_manager_access_t::attach (manager, stream);
    auto actor = detail::actor_ref_access_t::make (node_rid_t::from_string ("actor-node"),
                                                   "PlayerActor", "binding-order-actor", 1);
    bool registrar_sent = false;
    gateway.on_bound_session ([&] (const actor_ref_t &ref) {
        auto sent = gateway.actor_context (ref)
                      .bound_session ()
                      .send (std::string ("early"))
                      .async ()
                      .result ();
        registrar_sent = bool (sent);
        return sent;
    });
    auto gateway_state = gateway.weak_state ().lock ();
    auto physical_release = std::make_shared<task_completion_source_t<void>> ();
    std::mutex observed_mutex;
    std::vector<detail::stream_header_t> observed_headers;
    const auto read_headers = [&] {
        std::lock_guard lock (observed_mutex);
        return observed_headers;
    };
    std::promise<void> bound_entered;
    auto entering_bound = bound_entered.get_future ();
    std::atomic_bool write_on_gateway_lane{false};
    runtime.attach_transport_writer (
      stream,
      [&, physical_release, gateway_state] (const detail::stream_header_t &header,
                                            const zlink::message_t &) -> task_t<void> {
          {
              std::lock_guard lock (observed_mutex);
              observed_headers.push_back (header);
          }
          if (gateway_state->lane.is_on_lane ())
              write_on_gateway_lane.store (true);
          if (header.packet_name () == "$zlink.actor.bound") {
              bound_entered.set_value ();
              co_await physical_release->task ();
          }
          co_return;
      });
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (2);
    auto binding =
      std::async (std::launch::async, [&] { return manager.bind (actor).async ().result (); });
    const bool control_started = entering_bound.wait_until (deadline) == std::future_status::ready;
    auto lane_progress =
      std::async (std::launch::async, [&] { return gateway_state->sync ([] { return true; }); });
    const bool lane_available = lane_progress.wait_until (deadline) == std::future_status::ready;
    const bool bound_without_write_wait =
      binding.wait_until (deadline) == std::future_status::ready;
    physical_release->complete (result_t<void>::success ());
    const auto bound = binding.get ();
    (void) lane_progress.get ();
    std::cout << "gateway_lane_available=" << lane_available
              << " bind_before_write_terminal=" << bound_without_write_wait
              << " write_on_gateway_lane=" << write_on_gateway_lane << '\n';
    if (!control_started || !lane_available || !bound_without_write_wait || !write_on_gateway_lane)
        return 7;
    if (!bound)
        return 1;
    auto headers = read_headers ();
    while (headers.size () < 2 && std::chrono::steady_clock::now () < deadline) {
        std::this_thread::yield ();
        headers = read_headers ();
    }
    for (const auto &header : headers)
        std::cout << "publication frame=" << header.packet_name () << '\n';
    const bool published_in_order = registrar_sent && headers.size () == 2
                                    && headers[0].packet_name () == "$zlink.actor.bound"
                                    && headers[1].actor_slot () == 1;
    auto delayed = gateway.actor_context (actor).bound_session ().send (std::string ("delayed"));
    auto delivery = gateway.admit_bound_session_delivery (actor, 0);
    if (!delivery)
        return 2;
    const auto accepted =
      gateway.dispatch_bound_session_send (actor, "accepted.push", stream_codec_t::raw,
                                           zlink::message_t::from (std::string ("accepted")));
    if (!accepted)
        return 6;
    gateway.unbind_session_stream ("binding-order-actor");
    // A retained capability has not submitted a frame and does not precede unbind.
    const auto observation = std::chrono::steady_clock::now () + std::chrono::milliseconds (200);
    do {
        headers = read_headers ();
        if (!headers.empty () && headers.back ().packet_name () == "$zlink.actor.unbound")
            break;
        std::this_thread::yield ();
    } while (std::chrono::steady_clock::now () < observation);
    if (gateway.admit_bound_session_delivery (actor, 0))
        return 3;
    if ((*delivery) ("retained.push", stream_codec_t::raw,
                     zlink::message_t::from (std::string ("retained")))
          .result ())
        return 4;
    const auto late = delayed.async ().result ();
    std::cout << "delayed_submit_accepted=" << bool (late)
              << " error=" << (late.error () ? late.error ()->what () : "none") << '\n';
    do {
        headers = read_headers ();
        if (headers.size () == 4)
            break;
        std::this_thread::yield ();
    } while (std::chrono::steady_clock::now () < deadline);
    for (const auto &header : headers)
        std::cout << "terminal frame=" << header.packet_name () << '\n';
    const bool retired_in_order =
      headers.size () == 4 && headers[2].packet_name () == "accepted.push"
      && headers[2].actor_slot () == 1 && headers[3].packet_name () == "$zlink.actor.unbound";
    std::cout << "publication_order=" << published_in_order
              << " retirement_order=" << retired_in_order << '\n';
    return published_in_order && retired_in_order && !late ? 0 : 5;
}

int reply_submission_after_unbound (bool retire)
{
    using namespace zlink::framework;
    using namespace zlink::framework::detail;
    configure_stream_dispatch_executor ();
    struct executor_guard_t
    {
        ~executor_guard_t () { shutdown_stream_dispatch_executor (); }
    } executor_guard;
    serializer_registry_t serializers;
    auto builder = test::runtime_failure_builder ();
    builder.stream ("reply-unbound").bind ("tcp://127.0.0.1:0");
    bind_stream_serializers (builder, serializers);
    auto runtime = stream_runtime_t::from (builder);
    auto stream = runtime.open_session ("reply-unbound");
    actor_gateway_runtime_t gateway;
    gateway.weak_state ().lock ()->serializers = &serializers;
    auto manager = gateway.manager ();
    session_actor_manager_access_t::attach (manager, stream);
    const auto actor = actor_ref_access_t::make (node_rid_t::from_string ("actor-owner"),
                                                 "PlayerActor", "reply-unbound-actor", 1);
    if (!manager.bind (actor).async ().result ())
        return 1;
    class deferred_reply_session_t final : public packet_stream_session_t
    {
      public:
        explicit deferred_reply_session_t (actor_gateway_runtime_t &gateway, bool retire) :
            gateway (gateway), retire (retire)
        {
        }
        task_t<void> on_connected (stream_t &) override { co_return; }
        task_t<void> on_disconnected (stream_t &) override { co_return; }
        task_t<void> on_error (stream_t &, const stream_error_t &) override { co_return; }
        task_t<void> on_packet (stream_t &stream,
                                const session_message_context_t &,
                                const message_t &payload) override
        {
            auto reply = stream.reply_packet (payload);
            if (retire)
                gateway.unbind_session_stream ("reply-unbound-actor");
            co_await reply.async ();
        }
        actor_gateway_runtime_t &gateway;
        bool retire;
    } session (gateway, retire);
    stream_header_t request (stream_message_kind_t::request, stream_codec_t::json,
                             stream_header_flags_t::has_request_seq, 1, "request", {});
    request.with_actor_slot (1);
    request.with_correlation_id ("late-reply-correlation");
    const auto dispatched =
      runtime.dispatch_packet (session, stream, request, zlink::message_t::from ("{}"));
    if (!dispatched) {
        std::cerr << "dispatch failed: " << dispatched.error ()->what () << '\n';
        return 2;
    }
    const auto headers = runtime.written_headers (stream);
    bool registered = false;
    for (const auto &header : headers) {
        std::cout << "frame=" << header.packet_name () << " kind=" << int (header.kind ())
                  << " slot=" << header.actor_slot ().value_or (0) << '\n';
        if (header.packet_name () == "$zlink.actor.bound")
            registered = true;
        if (header.packet_name () == "$zlink.actor.unbound")
            registered = false;
        if (header.actor_slot () && !registered)
            return 3;
    }
    const auto &terminal = headers.back ();
    return headers.size () == (retire ? 3 : 2)
               && terminal.kind () == stream_message_kind_t::response
               && terminal.request_seq () == 1
               && terminal.correlation_id () == "late-reply-correlation"
               && terminal.actor_slot ()
                    == (retire ? std::optional<std::uint16_t>{} : std::optional<std::uint16_t>{1})
             ? 0
             : 4;
}
} // namespace zlink::framework::test::stream_framework
