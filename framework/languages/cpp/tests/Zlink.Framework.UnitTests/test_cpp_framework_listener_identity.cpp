/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/diagnostics/dispatch_options_access.hpp"
#include "runtime/client_server/raw_client_server_owner.hpp"
#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include "runtime/protocol/service_wire_codec.hpp"
#include "runtime/transport/listener_identity.hpp"

#include <zlink/framework.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{
namespace framework = zlink::framework;
namespace mesh = zlink::framework::runtime::mesh;
namespace protocol = zlink::framework::runtime::protocol;
namespace transport = zlink::framework::runtime::transport;
using namespace std::chrono_literals;

class no_op_stream_session_t final : public framework::packet_stream_session_t
{
  public:
    framework::task_t<void> on_connected (framework::stream_t &) override { co_return; }

    framework::task_t<void> on_disconnected (framework::stream_t &) override { co_return; }

    framework::task_t<void> on_error (framework::stream_t &,
                                      const framework::stream_error_t &) override
    {
        co_return;
    }

    framework::task_t<void> on_packet (framework::stream_t &,
                                       const framework::session_message_context_t &,
                                       const zlink::message_t &) override
    {
        co_return;
    }
};

class listener_status_probe_t final : public framework::hosted_service_t
{
  public:
    explicit listener_status_probe_t (framework::app_t &app) : _app (&app) {}

    framework::task_t<void> start (framework::service_provider_t &services) override
    {
        auto &runtime = services.get_required<framework::framework_runtime_t> ();
        fanout_endpoint =
          runtime.listener_status (framework::listener_kind_t::fanout, "wildcard-fanout").endpoint;
        stream_endpoint =
          runtime.listener_status (framework::listener_kind_t::stream, "wildcard-stream").endpoint;
        _app->stop ();
        co_return;
    }

    void stop () noexcept override {}

    std::string fanout_endpoint;
    std::string stream_endpoint;

  private:
    framework::app_t *_app;
};

std::vector<std::uint8_t> bytes (std::string value)
{
    return {value.begin (), value.end ()};
}

mesh::raw_mesh_node_options_t
mesh_options (std::string rid, std::string endpoint, framework::dispatch_options_t dispatch = {})
{
    return mesh::raw_mesh_node_options_t{
      .descriptor = mesh::service_node_descriptor_t{.mesh_name = "listener-identity",
                                                    .node_routing_id = bytes (std::move (rid)),
                                                    .lifecycle_generation = 1,
                                                    .descriptor_revision = 1,
                                                    .advertised_endpoint = std::move (endpoint),
                                                    .channels = {{"contract", 100}},
                                                    .state = mesh::service_node_state_t::preparing},
      .dispatch = std::move (dispatch)};
}

void pump (mesh::raw_mesh_node_owner_t &node)
{
    const auto now = mesh::service_liveness_registry_t::clock_t::now ();
    ASSERT_TRUE (node.drain_monitor_events (now).result ());
    const auto pumped = node.pump_one (now).result ();
    ASSERT_TRUE (pumped);
    EXPECT_NE (pumped.value (), mesh::raw_mesh_pump_result_t::protocol_error);
}

bool wait_for_ready_pair (mesh::raw_mesh_node_owner_t &source, mesh::raw_mesh_node_owner_t &target)
{
    const auto source_rid = source.topology ().local_descriptor ().node_routing_id;
    const auto target_rid = target.topology ().local_descriptor ().node_routing_id;
    const auto deadline = std::chrono::steady_clock::now () + 2s;
    while (std::chrono::steady_clock::now () < deadline) {
        if (source.topology ().peer (target_rid) && target.topology ().peer (source_rid))
            return true;
        pump (source);
        pump (target);
        std::this_thread::sleep_for (1ms);
    }
    return false;
}

bool wait_for_application_messages (mesh::raw_mesh_node_owner_t &source,
                                    mesh::raw_mesh_node_owner_t &target,
                                    std::size_t count)
{
    const auto deadline = std::chrono::steady_clock::now () + 2s;
    while (std::chrono::steady_clock::now () < deadline) {
        if (target.mailbox ().pending_messages (mesh::service_mailbox_domain_t::application)
            == count)
            return true;
        pump (source);
        pump (target);
        std::this_thread::sleep_for (1ms);
    }
    return false;
}

TEST (ListenerIdentity, OmittedAdvertiseHostMapsWildcardsToFamilyLoopback)
{
    for (const std::string_view listener : {"MeshNode", "ClientServer", "Fanout", "STREAM"}) {
        EXPECT_EQ (
          transport::advertised_tcp_endpoint ("tcp://0.0.0.0:7412", std::nullopt, listener),
          "tcp://127.0.0.1:7412");
        EXPECT_EQ (transport::advertised_tcp_endpoint ("tcp://[::]:7412", std::nullopt, listener),
                   "tcp://[::1]:7412");
    }
}

TEST (ListenerIdentity, ExplicitWildcardAdvertiseHostRemainsInvalid)
{
    EXPECT_THROW (transport::advertised_tcp_endpoint (
                    "tcp://127.0.0.1:7412", std::optional<std::string> ("0.0.0.0"), "Fanout"),
                  std::invalid_argument);
    EXPECT_THROW (transport::advertised_tcp_endpoint ("tcp://[::1]:7412",
                                                      std::optional<std::string> ("::"), "STREAM"),
                  std::invalid_argument);
}

TEST (ListenerIdentity, WildcardClientServerPublishesLoopback)
{
    namespace client_server = framework::runtime::client_server;
    client_server::raw_client_server_server_t server (
      client_server::raw_client_server_server_options_t{
        .descriptor =
          protocol::client_server_server_admission_t{.channel_name = "listener-identity",
                                                     .server_routing_id = bytes ("client-server"),
                                                     .lifecycle_generation = 1,
                                                     .descriptor_revision = 1,
                                                     .weight = 100,
                                                     .state = mesh::service_node_state_t::preparing,
                                                     .security_identity = "test",
                                                     .effective_max_message_bytes = 1024,
                                                     .advertised_endpoint = "tcp://0.0.0.0:0"}});
    server.start ();
    EXPECT_TRUE (server.endpoint ().starts_with ("tcp://127.0.0.1:"));
    server.close ();
}

TEST (ListenerIdentity, WildcardIpv6MeshPublishesIpv6Loopback)
{
    mesh::raw_mesh_node_owner_t node (mesh_options ("ipv6-target", "tcp://[::]:0"));
    try {
        node.start ();
    } catch (const std::exception &error) {
        // Core reports ENODEV when the host has no usable IPv6 listener; the
        // loopback mapping itself is covered by the helper-level test above.
        if (std::string (error.what ()).find ("errno=19") != std::string::npos)
            GTEST_SKIP () << "IPv6 bind unavailable on this host: " << error.what ();
        throw;
    }
    EXPECT_TRUE (node.endpoint ().starts_with ("tcp://[::1]:"));
    node.close ();
}

TEST (ListenerIdentity, WildcardFanoutAndStreamPublishLoopback)
{
    auto app = framework::app_t::create ();
    app.add_zlink_framework ([] (framework::zlink_framework_options_t &options) {
        options.add_fanout_channel ("wildcard-fanout").enable_publisher ("tcp://0.0.0.0:0");
        options.add_stream_node ("wildcard-stream")
          .bind ("tcp://0.0.0.0:0")
          .register_session<no_op_stream_session_t> ();
    });
    auto probe = std::make_unique<listener_status_probe_t> (app);
    auto *probe_view = probe.get ();
    app.add_hosted_service (std::move (probe));

    EXPECT_EQ (app.run (0, nullptr), 0);
    ASSERT_NE (probe_view, nullptr);
    EXPECT_TRUE (probe_view->fanout_endpoint.starts_with ("tcp://127.0.0.1:"));
    EXPECT_TRUE (probe_view->stream_endpoint.starts_with ("tcp://127.0.0.1:"));
}

TEST (ListenerIdentity, WildcardMeshAdvertisesLoopbackAndSupportsExpectedRidRouting)
{
    mesh::raw_mesh_node_owner_t source (mesh_options ("expected-source", "tcp://127.0.0.1:0"));
    mesh::raw_mesh_node_owner_t target (mesh_options ("expected-target", "tcp://0.0.0.0:0"));
    source.start ();
    target.start ();

    const auto target_descriptor = target.topology ().local_descriptor ();
    ASSERT_TRUE (target_descriptor.advertised_endpoint.starts_with ("tcp://127.0.0.1:"));
    ASSERT_TRUE (source.connect_peer (target.endpoint (), target_descriptor));
    ASSERT_TRUE (wait_for_ready_pair (source, target));

    const protocol::application_payload_t payload{"ListenerIdentityProbe", "application/json",
                                                  bytes ("payload")};
    ASSERT_TRUE (
      source.send_to_node (target_descriptor.node_routing_id, payload).result ().value ());
    ASSERT_TRUE (source.send_to_channel ("contract", payload).result ().value ());
    ASSERT_TRUE (wait_for_application_messages (source, target, 2));

    // A claim hands out one owner's records at a time; the node-direct and the
    // channel message belong to different owners, so claim until both arrived.
    std::size_t claimed_records = 0;
    for (int attempt = 0; attempt != 8 && claimed_records < 2; ++attempt) {
        const auto claimed = target.mailbox ().try_claim (
          mesh::service_mailbox_domain_t::application, 2, 4096);
        if (!claimed)
            break;
        claimed_records += claimed->records.size ();
        EXPECT_TRUE (target.mailbox ().release (*claimed));
    }
    EXPECT_EQ (claimed_records, 2u);
    source.close ();
    target.close ();
}

TEST (ListenerIdentity, EndpointOnlyPeerBecomesNodeDirectTargetAfterHandshake)
{
    mesh::raw_mesh_node_owner_t source (mesh_options ("endpoint-source", "tcp://127.0.0.1:0"));
    mesh::raw_mesh_node_owner_t target (mesh_options ("endpoint-target", "tcp://127.0.0.1:0"));
    source.start ();
    target.start ();

    const auto target_descriptor = target.topology ().local_descriptor ();
    ASSERT_TRUE (source.connect_peer (target.endpoint ()));
    ASSERT_TRUE (wait_for_ready_pair (source, target));
    const protocol::application_payload_t payload{"EndpointOnlyProbe", "application/json",
                                                  bytes ("payload")};
    ASSERT_TRUE (
      source.send_to_node (target_descriptor.node_routing_id, payload).result ().value ());
    ASSERT_TRUE (wait_for_application_messages (source, target, 1));
    source.close ();
    target.close ();
}

TEST (ListenerIdentity, ExpectedRouteMismatchWritesAdmissionWarning)
{
    framework::logging_builder_t logging;
    framework::dispatch_options_t dispatch;
    framework::detail::dispatch_options_access_t::set_logger (
      dispatch, logging.create_logger ("listener-identity-test"));
    mesh::raw_mesh_node_owner_t source (
      mesh_options ("warning-source", "tcp://127.0.0.1:0", dispatch));
    mesh::raw_mesh_node_owner_t target (mesh_options ("warning-target", "tcp://127.0.0.1:0"));
    source.start ();
    target.start ();

    const auto intent_endpoint = target.endpoint ();
    auto expected = target.topology ().local_descriptor ();
    expected.advertised_endpoint = "tcp://127.0.0.1:1";
    ASSERT_TRUE (source.connect_peer (intent_endpoint, expected));
    const auto deadline = std::chrono::steady_clock::now () + 2s;
    const auto has_rejection_warning = [&] {
        const auto records = logging.captured_records ();
        return std::any_of (records.begin (), records.end (),
                            [] (const framework::log_record_t &record) {
                                return record.level == framework::log_level_t::warn
                                       && record.message == "RouteMesh peer admission rejected";
                            });
    };
    while (!has_rejection_warning () && std::chrono::steady_clock::now () < deadline) {
        pump (source);
        pump (target);
        std::this_thread::sleep_for (1ms);
    }

    const auto records = logging.captured_records ();
    const auto found = std::find_if (
      records.begin (), records.end (), [&] (const framework::log_record_t &record) {
          if (record.level != framework::log_level_t::warn
              || record.message != "RouteMesh peer admission rejected")
              return false;
          std::map<std::string, std::string> fields;
          for (const auto &field : record.fields)
              fields.emplace (field.key, field.value);
          return fields["reason"] == "expected_route_mismatch"
                 && fields["intent_endpoint"] == intent_endpoint
                 && fields["advertised_endpoint"].starts_with ("tcp://127.0.0.1:");
      });
    EXPECT_NE (found, records.end ());
    source.close ();
    target.close ();
}

TEST (ListenerIdentity, StartupValidationFailureReturnsMessageInsteadOfEscapingRun)
{
    auto app = framework::app_t::create ();
    app.add_zlink_framework ([] (framework::zlink_framework_options_t &options) {
        options.add_fanout_channel ("invalid-advertise")
          .enable_publisher ("tcp://127.0.0.1:0")
          .set_advertise_host ("0.0.0.0");
    });

    testing::internal::CaptureStderr ();
    const auto exit_code = app.run (0, nullptr);
    const auto error_output = testing::internal::GetCapturedStderr ();
    EXPECT_NE (exit_code, 0);
    EXPECT_NE (error_output.find ("advertise host"), std::string::npos);
    EXPECT_NE (error_output.find ("non-wildcard"), std::string::npos);
    const auto records = app.logging ().captured_records ();
    const auto found =
      std::find_if (records.begin (), records.end (), [] (const framework::log_record_t &record) {
          return record.level == framework::log_level_t::critical
                 && record.message.find ("advertise host") != std::string::npos
                 && record.message.find ("non-wildcard") != std::string::npos;
      });
    EXPECT_NE (found, records.end ());
}

} // namespace
