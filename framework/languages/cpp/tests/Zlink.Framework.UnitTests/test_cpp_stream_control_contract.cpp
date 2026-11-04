/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include <zlink/framework.hpp>
#include "runtime/streams/stream_host_service.hpp"
#include "runtime/streams/stream_runtime.hpp"
#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/stateful/public_host_runtime.hpp"
#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "../support/telemetry_observation.hpp"
#include "../support/loopback_tcp_endpoint.hpp"
#include "../../connector/tests/control_frame_contract.hpp"

namespace fw = zlink::framework;
namespace wire = control_contract;

class session_t final : public fw::packet_stream_session_t
{
  public:
    fw::task_t<void> on_connected (fw::stream_t &) override { co_return; }
    fw::task_t<void> on_disconnected (fw::stream_t &) override { co_return; }
    fw::task_t<void> on_error (fw::stream_t &, const fw::stream_error_t &) override { co_return; }
    fw::task_t<void> on_packet (fw::stream_t &stream,
                                const fw::session_message_context_t &,
                                const fw::message_t &) override
    {
        co_await stream.write_packet (fw::message_t{}).packet_name ("control.barrier").async ();
    }
};

int server_cases (bool debug, bool core)
{
    int failures = 0;
    auto check = [&] (bool ok, const char *label) {
        std::cout << label << ": " << (ok ? "PASS" : "FAIL") << '\n';
        failures += !ok;
    };
    auto observed = std::make_shared<fw::test::telemetry_observations_t<fw::log_record_t>> ();
    fw::logging_builder_t logs;
    logs.disable_record_capture ().use_callback_sink (
      [observed] (const auto &record) { observed->record (record); });
    if (debug)
        logs.set_min_level (fw::log_level_t::debug);
    auto builder = fw::test::runtime_failure_builder ();
    fw::dispatch_options_t dispatch;
    fw::detail::dispatch_options_access_t::set_logger (dispatch, logs.create_logger ("stream"));
    fw::detail::apply_dispatch_options (builder, dispatch);
    builder.stream ("controls").bind (fw::tests::loopback_tcp_any_port).register_session ("test");
    fw::service_collection_t services;
    services.add_singleton<fw::detail::actor_gateway_runtime_t> ();
    services.add_factory<fw::session_actor_manager_t> (
      [] (fw::service_provider_t &provider) {
          return std::make_unique<fw::session_actor_manager_t> (
            provider.get_required<fw::detail::actor_gateway_runtime_t> ().manager ());
      },
      fw::service_lifetime_t::scoped);
    fw::handler_registry_t handlers;
    fw::serializer_registry_t serializers;
    std::shared_ptr<fw::detail::mesh_node_runtime_t> mesh;
    if (core) {
        fw::detail::zlink_builder_access_t::bind_shared_core_context (
          builder, std::make_shared<zlink::context_t> ());
        fw::zlink_framework_options_t options (services, handlers, serializers, builder);
        options.add_route_mesh ("control-mesh")
          .set_routing_id (zlink::routing_id_t::from ("control-node"))
          .listen (fw::tests::loopback_tcp_any_port);
        options.apply ();
        for (const auto &registration : fw::detail::mesh_node_runtime_t::registrations (builder))
            registration->core_context =
              fw::detail::zlink_builder_access_t::shared_core_context (builder);
        mesh = fw::detail::mesh_node_runtime_t::from (builder, "control-mesh");
        mesh->bind_serializers (serializers);
        mesh->start ().result ().value ();
    }
    fw::detail::bind_stream_serializers (builder, serializers);
    auto runtime = fw::detail::stream_runtime_t::from (builder);
    auto provider = services.build_provider ();
    session_t session;
    auto listeners = std::make_shared<fw::runtime::listener_status_registry_t> ();
    fw::runtime::stream_host_service_t host (
      std::make_shared<fw::runtime::runtime_failure_collector_t> (), runtime, runtime.snapshots (),
      {{"test",
        [&] (fw::service_provider_t &) -> fw::packet_stream_session_t & { return session; }}},
      std::chrono::milliseconds (30000), mesh, {}, listeners);
    host.bind_monitoring (fw::test::runtime_failure_monitoring ());
    host.start (provider).result ().value ();
    const auto endpoint =
      fw::tests::listener_endpoint (*listeners, fw::listener_kind_t::stream, "controls");
    wire::asio::io_context io;
    const wire::tcp::endpoint address (wire::asio::ip::address_v4::loopback (),
                                       fw::tests::endpoint_port (endpoint));
    {
        wire::tcp::socket peer (io);
        peer.connect (address);
        std::vector<std::uint8_t> batch;
        for (int i = 0; i < 1000; ++i) {
            auto bytes = wire::frame ("$zlink.future", {255, 1, 2});
            batch.insert (batch.end (), bytes.begin (), bytes.end ());
        }
        for (const auto &bytes :
             {wire::frame ("$zlink.heartbeat.pong", {1, 2, 3}), wire::barrier ()})
            batch.insert (batch.end (), bytes.begin (), bytes.end ());
        wire::asio::write (peer, wire::asio::buffer (batch));
        const auto ignored = wire::read_server_frame (peer);
        check (!ignored.first && ignored.second == wire::barrier (),
               "server ignores unknown control and nonempty pong without a reply");
        batch = wire::frame ("$zlink.heartbeat.ping", {255, 1, 2});
        const auto after = wire::barrier ();
        batch.insert (batch.end (), after.begin (), after.end ());
        wire::asio::write (peer, wire::asio::buffer (batch));
        const auto pong = wire::read_server_frame (peer);
        check (!pong.first && pong.second == wire::frame ("$zlink.heartbeat.pong"),
               "server unknown ping version returns empty pong");
        const auto response = wire::read_server_frame (peer);
        check (!response.first && response.second == wire::barrier (),
               "server unknown ping produces exactly one pong");
        const auto records = observed->snapshot ();
        if (records.size () != (debug ? 1002u : 0u)) {
            std::cout << "server captured records=" << records.size () << '\n';
            for (std::size_t i = 0; i < std::min<std::size_t> (records.size (), 3); ++i) {
                std::cout << "record level=" << static_cast<int> (records[i].level)
                          << " message=" << records[i].message << '\n';
                for (const auto &field : records[i].fields)
                    std::cout << field.key << '=' << field.value << '\n';
            }
        }
        check (records.size () == (debug ? 1002u : 0u), debug
                                                          ? "server debug control record count"
                                                          : "server default 1000 frames zero logs");
        if (debug) {
            bool fields = records.size () == 1002;
            for (const auto &record : records) {
                bool name = false, size = false, rid = false;
                for (const auto &field : record.fields) {
                    name |= field.key == "control_name" && field.value.starts_with ("$zlink.");
                    size |= field.key == "payload_bytes" && field.value == "3";
                    rid |= field.key == "peer_session_rid" && !field.value.empty ();
                }
                fields &= record.level == fw::log_level_t::debug && name && size && rid;
            }
            check (fields, "server debug records name length and peer RID");
        }
    }
    if (!debug) {
        for (const auto &name : {"future", "session-closing", "$zlink.actor.bound",
                                 "$zlink.actor.unbound", "$zlink.future"}) {
            wire::tcp::socket peer (io);
            peer.connect (address);
            auto bytes = wire::frame (name, {1, 0}, std::string_view (name) == "$zlink.future");
            const auto ping = wire::frame ("$zlink.heartbeat.ping");
            bytes.insert (bytes.end (), ping.begin (), ping.end ());
            wire::asio::write (peer, wire::asio::buffer (bytes));
            std::array<std::uint8_t, 1> response{};
            const auto error = wire::read_exact (peer, wire::asio::buffer (response));
            check (error == wire::asio::error::eof || error == wire::asio::error::connection_reset,
                   name);
        }
    }
    host.stop ();
    if (mesh)
        mesh->stop ();
    return failures;
}

int main (int argc, char **argv)
{
    fw::completion_owner_t completion_owner;
    struct executor_t
    {
        executor_t () { fw::detail::configure_stream_dispatch_executor (); }
        ~executor_t () { fw::detail::shutdown_stream_dispatch_executor (); }
    } executor;
    if (!wire::server_peer_heartbeat_contract ()) {
        std::cerr << "raw peer must answer periodic and unknown-version server pings\n";
        return 1;
    }
    const std::string mode = argc > 1 ? argv[1] : "server";
    if (mode == "connector")
        return wire::connector_cases ([] (const wire::sc::connector_options_t &options, auto test) {
            auto connector = wire::sc::connector_factory_t::create (options);
            test (connector);
        })
                 ? 1
                 : 0;
    return server_cases (mode == "debug", mode == "server-core") ? 1 : 0;
}
