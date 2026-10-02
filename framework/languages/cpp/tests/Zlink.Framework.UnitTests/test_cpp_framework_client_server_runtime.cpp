/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/client_server/raw_client_server_owner.hpp"
#include "runtime/client_server/client_server_failure_mapper.hpp"
#include "runtime/channels/channel_runtime.hpp"
#include "runtime/diagnostics/listener_status_registry.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/host/hosted_service_lifecycle.hpp"
#include "runtime/diagnostics/topology_projection.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/streams/stream_runtime.hpp"
#include "test_completion_poller_driver.hpp"
#include <zlink/Contracts/Sockets/routed_socket_contracts.hpp>
#include <zlink/Contracts/Messaging/operation_contracts.hpp>

#include <zlink/framework.hpp>

#include <array>
#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace client_server = zlink::framework::runtime::client_server;
namespace protocol = zlink::framework::runtime::protocol;
using namespace std::chrono_literals;

namespace
{

std::vector<std::uint8_t> bytes (const std::string &value)
{
    return {value.begin (), value.end ()};
}

struct network_probe_message_t
{
    static constexpr const char *packet_name = "metadata.send";
};

void to_json (nlohmann::json &json, const network_probe_message_t &)
{
    json = nlohmann::json::object ();
}

void from_json (const nlohmann::json &, network_probe_message_t &)
{
}

struct network_probe_handler_t
{
    using message_type = network_probe_message_t;

    void handle (const network_probe_message_t &) {}
};

struct slow_send_handler_t
{
    using message_type = network_probe_message_t;
    void handle (const network_probe_message_t &)
    {
        std::this_thread::sleep_for (150ms);
        completed.store (true, std::memory_order_release);
    }
    inline static std::atomic_bool completed{false};
};

void verify_client_server_send_does_not_wait_on_infrastructure_worker ()
{
    slow_send_handler_t::completed.store (false, std::memory_order_release);
    auto app = zlink::framework::app_t::create ();
    app.add_zlink_framework ([] (zlink::framework::zlink_framework_options_t &options) {
        options.handlers ().group ("slow-send").add_send<slow_send_handler_t> ();
        auto channel = options.add_client_server_channel ("slow-send");
        channel.server ().listen ().add_handler_group ("slow-send");
        channel.client ();
    });
    auto provider = app.advanced ().services ().build_provider ();
    auto &runtime = provider.get_required<zlink::framework::client_server_runtime_t> ();
    auto &channels = provider.get_required<zlink::framework::channel_client_t> ();
    char program[] = "client-server-slow-send";
    char *arguments[] = {program, nullptr};
    std::thread app_thread ([&] { (void) app.run (1, arguments); });
    const auto deadline = std::chrono::steady_clock::now () + 5s;
    while (!runtime.snapshot ("slow-send").is_ready && std::chrono::steady_clock::now () < deadline)
        std::this_thread::sleep_for (1ms);
    assert (runtime.snapshot ("slow-send").is_ready);
    const auto submitted =
      channels.send ("slow-send", network_probe_message_t{}).async ().result ();
    assert (submitted);
    while (!slow_send_handler_t::completed.load (std::memory_order_acquire)
           && std::chrono::steady_clock::now () < deadline)
        std::this_thread::sleep_for (1ms);
    assert (slow_send_handler_t::completed.load (std::memory_order_acquire));
    app.stop ();
    app_thread.join ();
}

struct metadata_send_handler_t
{
    using message_type = network_probe_message_t;
    void handle (const network_probe_message_t &,
                 const zlink::framework::message_context_t &context)
    {
        received.store (context.metadata.find ("tenant-id") == "tenant-42",
                        std::memory_order_release);
        completed.store (true, std::memory_order_release);
        calls.fetch_add (1, std::memory_order_release);
    }
    inline static std::atomic_bool received{false};
    inline static std::atomic_bool completed{false};
    inline static std::atomic_int calls{0};
};

struct metadata_request_t
{
    static constexpr const char *packet_name = "metadata.request";
};
void to_json (nlohmann::json &json, const metadata_request_t &)
{
    json = nlohmann::json::object ();
}
void from_json (const nlohmann::json &, metadata_request_t &)
{
}

struct metadata_empty_request_t
{
    static constexpr const char *packet_name = "metadata.empty-request";
};
void to_json (nlohmann::json &json, const metadata_empty_request_t &)
{
    json = nlohmann::json::object ();
}
void from_json (const nlohmann::json &, metadata_empty_request_t &)
{
}

struct metadata_empty_request_handler_t
{
    using request_type = metadata_empty_request_t;
    using reply_type = std::string;
    std::string handle (const metadata_empty_request_t &,
                        const zlink::framework::message_context_t &context)
    {
        return context.metadata.empty () ? "empty" : "copied";
    }
};

struct metadata_request_handler_t
{
    using request_type = metadata_request_t;
    using reply_type = std::string;
    explicit metadata_request_handler_t (zlink::framework::channel_client_t &client) :
        client (client)
    {
    }
    zlink::framework::task_t<std::string>
    handle (const metadata_request_t &, const zlink::framework::message_context_t &context)
    {
        calls.fetch_add (1);
        const auto tenant = std::string (context.metadata.find ("tenant-id").value_or ("missing"));
        const auto nested =
          co_await client.request_to_channel ("metadata-empty", metadata_empty_request_t{})
            .async<std::string> ();
        assert (nested == "empty");
        co_return tenant;
    }
    zlink::framework::channel_client_t &client;
    inline static std::atomic_int calls{0};
};

zlink::framework::task_t<std::vector<zlink::message_t>>
await_metadata_wire_reply (zlink::async_result_t<std::vector<zlink::message_t>> pending)
{
    co_return co_await std::move (pending);
}

std::vector<std::string> invalid_metadata_headers (bool request = false,
                                                   bool metadata_first = false)
{
    std::ifstream input (ZLINK_CLIENT_SERVER_METADATA_FIXTURE_PATH);
    assert (input.good ());
    const auto fixture = nlohmann::json::parse (input);
    std::vector<std::string> headers;
    for (const auto &test : fixture.at ("cases")) {
        if (test.at ("valid").get<bool> ())
            continue;
        nlohmann::json header = {{"formatMarker", 242},
                                 {"kind", request ? 1 : 3},
                                 {"channelName", "metadata"},
                                 {"messageName", request ? "metadata.request" : "metadata.send"},
                                 {"correlationId", "malformed-metadata"},
                                 {"contentType", "application/json"}};
        const auto metadata = test.contains ("receivedEncoded")
                                ? test.at ("receivedEncoded").get<std::string> ()
                                : test.at ("metadata").dump ();
        auto encoded = header.dump ();
        encoded.pop_back ();
        headers.push_back (metadata_first
                             ? "{\"metadata\":" + metadata + "," + encoded.substr (1) + "}"
                             : encoded + ",\"metadata\":" + metadata + "}");
    }
    return headers;
}

void verify_invalid_metadata_never_dispatches (const std::string &endpoint)
{
    zlink::context_t context;
    zlink::dealer_socket_t source (context);
    source.set_routing_id (zlink::routing_id_t::from ("metadata-malformed-peer"));
    source.options ().linger (0ms);
    zlink::framework::test::completion_poller_driver_t completions (source);
    source.connect (endpoint);
    const auto hello = protocol::encode_client_server_client_admission (
      protocol::command::hello, {"metadata", "default", 16 * 1024 * 1024});
    const auto admitted =
      await_metadata_wire_reply (
        source.request ().message (zlink::message_t::from (hello)).timeout (5s).async ().reply)
        .result ();
    assert (admitted && admitted.value ().size () == 1);
    assert (protocol::decode_client_server_server_admission (
              bytes (admitted.value ().front ().to_string ()), protocol::command::admit)
              .channel_name
            == "metadata");
    const std::string valid =
      R"({"formatMarker":242,"kind":3,"channelName":"metadata","messageName":"metadata.send","contentType":"application/json","metadata":{"tenant-id":"tenant-42"}})";
    for (const auto &bad : invalid_metadata_headers ()) {
        const auto previous = metadata_send_handler_t::calls.load (std::memory_order_acquire);
        (void) source.send ()
          .message (zlink::message_t::from (bad))
          .message (zlink::message_t::from ("{}"))
          .async ();
        (void) source.send ()
          .message (zlink::message_t::from (valid))
          .message (zlink::message_t::from ("{}"))
          .async ();
        const auto deadline = std::chrono::steady_clock::now () + 5s;
        while (metadata_send_handler_t::calls.load (std::memory_order_acquire) == previous
               && std::chrono::steady_clock::now () < deadline)
            std::this_thread::sleep_for (1ms);
        assert (metadata_send_handler_t::calls.load (std::memory_order_acquire) == previous + 1);
        assert (metadata_send_handler_t::received.load ());
    }
    const std::string valid_request =
      R"({"formatMarker":242,"kind":1,"channelName":"metadata","messageName":"metadata.request","correlationId":"valid-metadata","contentType":"application/json","metadata":{"tenant-id":"tenant-42"}})";
    const auto response =
      await_metadata_wire_reply (source.request ()
                                   .message (zlink::message_t::from (valid_request))
                                   .message (zlink::message_t::from ("{}"))
                                   .timeout (5s)
                                   .async ()
                                   .reply)
        .result ();
    assert (response && response.value ().size () == 2);
    const auto reply_header =
      zlink::framework::runtime::messaging::envelope_codec_t{}.decode_header (
        response.value ().front (), false);
    assert (reply_header && reply_header.value ().metadata.empty ());
    assert (nlohmann::json::parse (response.value ()[1].to_string ()) == "tenant-42");
    for (const bool first : {false, true}) {
        for (const auto &bad_request : invalid_metadata_headers (true, first)) {
            const auto calls = metadata_request_handler_t::calls.load ();
            const auto rejected =
              await_metadata_wire_reply (source.request ()
                                           .message (zlink::message_t::from (bad_request))
                                           .message (zlink::message_t::from ("{}"))
                                           .timeout (5s)
                                           .async ()
                                           .reply)
                .result ();
            assert (rejected && rejected.value ().size () == 2);
            const auto error =
              zlink::framework::runtime::messaging::envelope_codec_t{}.decode_header (
                rejected.value ().front (), false);
            assert (error
                    && error.value ().kind
                         == zlink::framework::runtime::messaging::message_kind_t::error);
            assert (error.value ().error_code == "protocol_error");
            assert (error.value ().metadata.empty ());
            assert (metadata_request_handler_t::calls.load () == calls);
        }
    }
}

void verify_invalid_metadata_is_a_protocol_error ()
{
    protocol::client_server_server_admission_t descriptor{
      "metadata",
      bytes ("metadata-raw-server"),
      1,
      1,
      100,
      zlink::framework::runtime::mesh::service_node_state_t::serving,
      "default",
      16 * 1024 * 1024,
      "tcp://127.0.0.1:0"};
    client_server::raw_client_server_server_t server ({{descriptor}});
    server.start ();
    zlink::context_t context;
    zlink::dealer_socket_t source (context);
    source.set_routing_id (zlink::routing_id_t::from ("metadata-raw-peer"));
    source.options ().linger (0ms);
    zlink::framework::test::completion_poller_driver_t completions (source);
    source.connect (server.endpoint ());
    const auto pump = [&] (client_server::client_server_pump_result_t expected) {
        const auto deadline = std::chrono::steady_clock::now () + 5s;
        auto result = client_server::client_server_pump_result_t::no_data;
        while (result != expected && std::chrono::steady_clock::now () < deadline) {
            const auto now = std::chrono::steady_clock::now ();
            (void) server.drain_monitor_events (now);
            result = server.pump_one (now).result ().value ();
            if (result == client_server::client_server_pump_result_t::no_data)
                std::this_thread::sleep_for (1ms);
        }
        assert (result == expected);
    };
    const auto hello = protocol::encode_client_server_client_admission (
      protocol::command::hello, {"metadata", "default", 16 * 1024 * 1024});
    auto admission = await_metadata_wire_reply (
      source.request ().message (zlink::message_t::from (hello)).timeout (5s).async ().reply);
    pump (client_server::client_server_pump_result_t::infrastructure);
    assert (admission.result ());
    const std::string valid =
      R"({"formatMarker":242,"kind":3,"channelName":"metadata","messageName":"metadata.send","contentType":"application/json","metadata":{"tenant-id":"tenant-42"}})";
    (void) source.send ()
      .message (zlink::message_t::from (valid))
      .message (zlink::message_t::from ("{}"))
      .async ();
    pump (client_server::client_server_pump_result_t::application);
    using zlink::framework::runtime::mesh::service_mailbox_domain_t;
    const auto claim =
      server.mailbox ().try_claim (service_mailbox_domain_t::application, 1, 1024 * 1024);
    assert (claim && claim->records.size () == 1);
    assert (server.mailbox ().release (*claim));
    for (const auto &bad : invalid_metadata_headers ()) {
        (void) source.send ()
          .message (zlink::message_t::from (bad))
          .message (zlink::message_t::from ("{}"))
          .async ();
        pump (client_server::client_server_pump_result_t::protocol_error);
        assert (server.mailbox ().pending_messages (service_mailbox_domain_t::application) == 0);
    }
    server.close ();
}

void verify_client_server_metadata_snapshot ()
{
    metadata_send_handler_t::completed.store (false);
    metadata_send_handler_t::received.store (false);
    auto app = zlink::framework::app_t::create ();
    if (const auto *log = std::getenv ("ZLINK_CPP_METADATA_FLOW_LOG"))
        app.logging ().use_file (log);
    app.add_zlink_framework ([] (zlink::framework::zlink_framework_options_t &options) {
        if (std::getenv ("ZLINK_CPP_METADATA_FLOW_LOG"))
            options.configure_dispatch ().message_flow (
              zlink::framework::message_flow_log_mode_t::normal);
        options.handlers ()
          .group ("metadata")
          .add_send<metadata_send_handler_t> ()
          .add<metadata_request_handler_t> ()
          .add<metadata_empty_request_handler_t> ();
        auto channel = options.add_client_server_channel ("metadata");
        channel.server ().listen ().add_handler_group ("metadata");
        channel.client ();
        auto empty = options.add_client_server_channel ("metadata-empty");
        empty.server ().listen ().add_handler_group ("metadata");
        empty.client ();
    });
    auto provider = app.advanced ().services ().build_provider ();
    auto &runtime = provider.get_required<zlink::framework::client_server_runtime_t> ();
    auto &routes = provider.get_required<zlink::framework::channel_client_t> ();
    char program[] = "client-server-metadata";
    char *arguments[] = {program, nullptr};
    std::thread app_thread ([&] { (void) app.run (1, arguments); });
    const auto deadline = std::chrono::steady_clock::now () + 5s;
    while (!runtime.snapshot ("metadata").is_ready && std::chrono::steady_clock::now () < deadline)
        std::this_thread::sleep_for (1ms);
    assert (runtime.snapshot ("metadata").is_ready);
    while (!runtime.snapshot ("metadata-empty").is_ready
           && std::chrono::steady_clock::now () < deadline)
        std::this_thread::sleep_for (1ms);
    assert (runtime.snapshot ("metadata-empty").is_ready);
    const auto &framework = provider.get_required<zlink::framework::framework_runtime_t> ();
    verify_invalid_metadata_never_dispatches (
      framework.listener_status (zlink::framework::listener_kind_t::client_server, "metadata")
        .endpoint);
    metadata_send_handler_t::completed.store (false);
    metadata_send_handler_t::received.store (false);
    const auto public_calls = metadata_send_handler_t::calls.load (std::memory_order_acquire);
    const auto send = routes.send ("metadata", network_probe_message_t{})
                        .metadata ("tenant-id", "tenant-42")
                        .async ()
                        .result ();
    assert (send);
    const auto send_deadline = std::chrono::steady_clock::now () + 5s;
    while (metadata_send_handler_t::calls.load (std::memory_order_acquire) == public_calls
           && std::chrono::steady_clock::now () < send_deadline)
        std::this_thread::sleep_for (1ms);
    const auto reply = routes.request_to_channel ("metadata", metadata_request_t{})
                         .metadata ("tenant-id", "tenant-42")
                         .async<std::string> ()
                         .result ();
    app.stop ();
    app_thread.join ();
    assert (metadata_send_handler_t::completed.load ());
    assert (metadata_send_handler_t::received.load ());
    assert (metadata_send_handler_t::calls.load (std::memory_order_acquire) == public_calls + 1);
    assert (reply && reply.value () == "tenant-42");
}

struct readiness_case_t
{
    const char *channel_name;
    zlink::framework::client_server_role_t role;
    int ready_target_count;
    bool is_ready;
};

constexpr std::array readiness_cases{
  readiness_case_t{"ready-server", zlink::framework::client_server_role_t::server, 1, true},
  readiness_case_t{"zero-weight-server", zlink::framework::client_server_role_t::server, 1, false},
  readiness_case_t{"client-without-server", zlink::framework::client_server_role_t::client, 0,
                   false},
  readiness_case_t{"client-and-server", zlink::framework::client_server_role_t::client_and_server,
                   1, true}};

class preparing_readiness_probe_t final : public zlink::framework::hosted_service_t
{
  public:
    zlink::framework::task_t<void> start (zlink::framework::service_provider_t &services) override
    {
        const auto &host = services.get_required<zlink::framework::framework_runtime_t> ();
        const auto &runtime = services.get_required<zlink::framework::client_server_runtime_t> ();
        assert (host.status ().state == zlink::framework::framework_runtime_state_t::preparing);
        for (const auto &test : readiness_cases) {
            const auto snapshot = runtime.snapshot (test.channel_name);
            assert (snapshot.ready_target_count == test.ready_target_count);
            assert (!runtime.is_ready (test.channel_name));
        }
        co_return;
    }

    void stop () noexcept override {}
};

void verify_client_server_readiness_counts_local_ready_servers ()
{
    auto app = zlink::framework::app_t::create ();
    app.add_zlink_framework ([] (zlink::framework::zlink_framework_options_t &options) {
        options.handlers ().group ("readiness").add_send<network_probe_handler_t> ();
        options.add_client_server_channel ("ready-server")
          .server ()
          .listen ()
          .set_weight (100)
          .add_handler_group ("readiness");
        options.add_client_server_channel ("zero-weight-server")
          .server ()
          .listen ()
          .set_weight (0)
          .add_handler_group ("readiness");
        options.add_client_server_channel ("client-without-server")
          .client ()
          .connect ("tcp://127.0.0.1:1");
        auto both = options.add_client_server_channel ("client-and-server");
        both.server ().listen ().set_weight (100).add_handler_group ("readiness");
        both.client ();
    });
    app.add_hosted_service (std::make_unique<preparing_readiness_probe_t> ());
    auto provider = app.advanced ().services ().build_provider ();
    const auto &runtime = provider.get_required<zlink::framework::client_server_runtime_t> ();
    for (const auto &test : readiness_cases) {
        assert (!runtime.is_ready (test.channel_name));
        assert (runtime.snapshot (test.channel_name).ready_target_count == 0);
    }

    char program[] = "client-server-readiness";
    char *arguments[] = {program, nullptr};
    std::atomic_int exit_code{-1};
    std::thread app_thread (
      [&] { exit_code.store (app.run (1, arguments), std::memory_order_release); });
    const auto deadline = std::chrono::steady_clock::now () + 5s;
    while (!app.is_ready () && std::chrono::steady_clock::now () < deadline)
        std::this_thread::sleep_for (1ms);
    assert (app.runtime_state () == zlink::framework::framework_runtime_state_t::serving);

    while (!runtime.snapshot ("client-and-server").is_ready
           && std::chrono::steady_clock::now () < deadline)
        std::this_thread::sleep_for (1ms);

    for (const auto &test : readiness_cases) {
        const auto snapshot = runtime.snapshot (test.channel_name);
        assert (snapshot.local_role == test.role);
        assert (snapshot.ready_target_count == test.ready_target_count);
        assert (runtime.is_ready (test.channel_name) == test.is_ready);
        assert (snapshot.is_ready == test.is_ready);
    }

    auto &channels = provider.get_required<zlink::framework::channel_client_t> ();
    const auto send = channels.send ("ready-server", network_probe_message_t{}).async ().result ();
    assert (!send);
    assert (send.error_kind () == zlink::framework::framework_error_kind_t::not_configured);
    const auto request = channels.request_to_channel ("ready-server", network_probe_message_t{})
                           .async<network_probe_message_t> ()
                           .result ();
    assert (!request);
    assert (request.error_kind () == zlink::framework::framework_error_kind_t::not_configured);

    app.request_stop ();
    app_thread.join ();
    assert (exit_code.load (std::memory_order_acquire) == 0);
    for (const auto &test : readiness_cases) {
        assert (!runtime.is_ready (test.channel_name));
        assert (runtime.snapshot (test.channel_name).ready_target_count == 0);
    }
}

void verify_network_defaults_are_deferred_until_apply ()
{
    zlink::framework::service_collection_t services;
    zlink::framework::handler_registry_t handlers;
    zlink::framework::serializer_registry_t serializers;
    zlink::framework::zlink_builder_t zlink;
    zlink::framework::zlink_framework_options_t options (services, handlers, serializers, zlink);

    auto client_server = options.add_client_server_channel ("network-client-server");
    options.handlers ().group ("network").add_send<network_probe_handler_t> ();
    client_server.server ().listen ().add_handler_group ("network");
    options.add_fanout_channel ("network-fanout").enable_publisher ();
    auto mesh = options.add_route_mesh ("network-mesh");
    mesh.set_object_role (zlink::framework::object_role_t::none)
      .set_routing_id (zlink::routing_id_t::from ("network-mesh-node"))
      .listen ();
    options.add_stream_node ("network-stream").bind ().register_session ("network-session");

    auto &network = options.configure_network ();
    assert (network.bind_host () == "127.0.0.1");
    assert (!network.advertise_host ());
    network.set_bind_host ("127.0.0.2").set_advertise_host (std::string ("network.example"));
    options.apply ();

    const auto snapshots =
      zlink::framework::detail::channel_runtime_t::from (zlink.message_bus ()).channel_snapshots ();
    const auto find_channel = [&snapshots] (const std::string &name) {
        return std::find_if (snapshots.begin (), snapshots.end (),
                             [&name] (const auto &snapshot) { return snapshot.name == name; });
    };
    const auto client_server_snapshot = find_channel ("network-client-server");
    const auto fanout_snapshot = find_channel ("network-fanout");
    assert (client_server_snapshot != snapshots.end ());
    assert (fanout_snapshot != snapshots.end ());
    assert (client_server_snapshot->server.bind_endpoints.size () == 1);
    assert (client_server_snapshot->server.bind_endpoints.front () == "tcp://127.0.0.2:*");
    assert (fanout_snapshot->publisher.bind_endpoints.size () == 1);
    assert (fanout_snapshot->publisher.bind_endpoints.front () == "tcp://127.0.0.2:0");
    const auto stream_snapshots =
      zlink::framework::detail::stream_runtime_t::from (zlink).snapshots ();
    assert (stream_snapshots.size () == 1);
    assert (stream_snapshots.front ().bind_endpoint == "tcp://127.0.0.2:0");
    assert (zlink::framework::detail::mesh_node_runtime_t::from (zlink, "network-mesh")
              ->listen_endpoint ()
            == "tcp://127.0.0.2:0");
}

void verify_client_server_runtime_projection_and_observation ()
{
    protocol::client_server_server_admission_t descriptor{
      "client-server-runtime-unit",
      bytes ("client-server-runtime-unit-server"),
      17,
      1,
      100,
      zlink::framework::runtime::mesh::service_node_state_t::serving,
      "default",
      16 * 1024 * 1024,
      "tcp://127.0.0.1:0"};
    client_server::raw_client_server_server_t server ({{descriptor}});
    server.start ();
    const auto endpoint = server.descriptor ().advertised_endpoint;

    auto app = zlink::framework::app_t::create ();
    app.add_zlink_framework ([endpoint] (zlink::framework::zlink_framework_options_t &options) {
        options.add_client_server_channel ("client-server-runtime-unit")
          .client ()
          .connect (endpoint);
    });
    auto provider = app.advanced ().services ().build_provider ();
    auto &runtime = provider.get_required<zlink::framework::client_server_runtime_t> ();

    const auto before = runtime.snapshot ("client-server-runtime-unit");
    assert (before.local_role == zlink::framework::client_server_role_t::client);
    assert (!before.is_ready);
    assert (before.ready_target_count == 0);

    std::atomic_int event_count{0};
    std::mutex event_mutex;
    std::condition_variable event_changed;
    auto observation =
      runtime.observe ("client-server-runtime-unit", 8,
                       [&event_count, &event_changed] (
                         const zlink::framework::observed_status_t<
                           zlink::framework::client_server_channel_snapshot_t> &observed) {
                           assert (observed.status.channel_name == "client-server-runtime-unit");
                           event_count.fetch_add (1, std::memory_order_relaxed);
                           event_changed.notify_all ();
                       });
    {
        std::unique_lock lock (event_mutex);
        const auto observation_deadline = std::chrono::steady_clock::now () + 5s;
        assert (event_changed.wait_until (lock, observation_deadline, [&] {
            return event_count.load (std::memory_order_relaxed) != 0;
        }));
    }
    char program[] = "client-server-runtime-unit";
    char *arguments[] = {program, nullptr};
    std::atomic_int exit_code{-1};
    std::thread app_thread (
      [&] { exit_code.store (app.run (1, arguments), std::memory_order_release); });

    const auto deadline = std::chrono::steady_clock::now () + 5s;
    while (!runtime.is_ready ("client-server-runtime-unit")
           && std::chrono::steady_clock::now () < deadline) {
        const auto now = std::chrono::steady_clock::now ();
        (void) server.drain_monitor_events (now);
        const auto server_pump = server.pump_one (now).result ().value ();
        assert (server_pump != client_server::client_server_pump_result_t::protocol_error);
        std::this_thread::sleep_for (1ms);
    }

    const auto after = runtime.snapshot ("client-server-runtime-unit");
    assert (after.is_ready);
    assert (after.ready_target_count == 1);
    assert (after.targets.size () == 1);
    assert (after.targets.front ().state == zlink::framework::peer_state_t::ready);
    assert (!after.targets.front ().unavailable_reason);

    observation->close ();
    app.request_stop ();
    app_thread.join ();
    assert (exit_code.load (std::memory_order_acquire) == 0);
    server.close ();
}

void verify_listener_status_is_not_configured (zlink::framework::framework_runtime_t &runtime,
                                               const std::string &name)
{
    try {
        (void) runtime.listener_status (zlink::framework::listener_kind_t::client_server, name);
    }
    catch (const zlink::framework::framework_exception_t &error) {
        if (error.kind () == zlink::framework::framework_error_kind_t::not_configured)
            return;
    }
    throw std::runtime_error ("listener status after stop did not return NotConfigured");
}

void verify_public_listener_status_reports_bound_endpoint ()
{
    auto app = zlink::framework::app_t::create ();
    app.add_zlink_framework ([] (zlink::framework::zlink_framework_options_t &options) {
        options.handlers ().group ("listener-status").add_send<network_probe_handler_t> ();
        options.add_client_server_channel ("listener-status")
          .server ()
          .listen ()
          .add_handler_group ("listener-status");
    });

    auto provider = app.advanced ().services ().build_provider ();
    auto &runtime = provider.get_required<zlink::framework::framework_runtime_t> ();

    char program[] = "listener-status";
    char *arguments[] = {program, nullptr};
    std::atomic_int exit_code{-1};
    std::thread app_thread (
      [&] { exit_code.store (app.run (1, arguments), std::memory_order_release); });

    std::optional<zlink::framework::listener_status_t> status;
    const auto deadline = std::chrono::steady_clock::now () + 5s;
    while (std::chrono::steady_clock::now () < deadline) {
        try {
            status = runtime.listener_status (zlink::framework::listener_kind_t::client_server,
                                              "listener-status");
            break;
        }
        catch (const zlink::framework::framework_exception_t &) {
            std::this_thread::sleep_for (1ms);
        }
    }

    assert (status.has_value ());
    assert (status->kind == zlink::framework::listener_kind_t::client_server);
    assert (status->name == "listener-status");
    assert (status->endpoint.rfind ("tcp://", 0) == 0);
    assert (status->endpoint.find (":0") == std::string::npos);

    auto &channels = provider.get_required<zlink::framework::channel_client_t> ();
    const auto server_only_send =
      channels.send ("listener-status", network_probe_message_t{}).async ().result ();
    if (server_only_send
        || server_only_send.error_kind ()
             != zlink::framework::framework_error_kind_t::not_configured) {
        throw std::runtime_error ("server-only ClientServer send did not return NotConfigured");
    }
    const auto server_only_request =
      channels.request_to_channel ("listener-status", network_probe_message_t{})
        .async<network_probe_message_t> ()
        .result ();
    if (server_only_request
        || server_only_request.error_kind ()
             != zlink::framework::framework_error_kind_t::not_configured) {
        throw std::runtime_error ("server-only ClientServer request did not return NotConfigured");
    }

    app.request_stop ();
    app_thread.join ();
    assert (exit_code.load (std::memory_order_acquire) == 0);
    verify_listener_status_is_not_configured (runtime, "listener-status");
}

void verify_listener_status_during_stop_reads_record_or_not_configured ()
{
    for (int round = 0; round < 5; ++round) {
        auto app = zlink::framework::app_t::create ();
        app.add_zlink_framework ([] (zlink::framework::zlink_framework_options_t &options) {
            options.handlers ().group ("listener-status-stop").add_send<network_probe_handler_t> ();
            options.add_client_server_channel ("listener-status-stop")
              .server ()
              .listen ()
              .add_handler_group ("listener-status-stop");
        });

        auto provider = app.advanced ().services ().build_provider ();
        auto &runtime = provider.get_required<zlink::framework::framework_runtime_t> ();

        char program[] = "listener-status-stop";
        char *arguments[] = {program, nullptr};
        std::atomic_int exit_code{-1};
        std::thread app_thread (
          [&] { exit_code.store (app.run (1, arguments), std::memory_order_release); });

        const auto deadline = std::chrono::steady_clock::now () + 5s;
        bool bound = false;
        while (!bound && std::chrono::steady_clock::now () < deadline) {
            try {
                (void) runtime.listener_status (zlink::framework::listener_kind_t::client_server,
                                                "listener-status-stop");
                bound = true;
            }
            catch (const zlink::framework::framework_exception_t &) {
                std::this_thread::sleep_for (1ms);
            }
        }
        assert (bound);

        std::atomic_bool querying{true};
        std::atomic_int unexpected{0};
        std::thread querier ([&] {
            while (querying.load (std::memory_order_acquire)) {
                try {
                    (void) runtime.listener_status (
                      zlink::framework::listener_kind_t::client_server, "listener-status-stop");
                }
                catch (const zlink::framework::framework_exception_t &error) {
                    if (error.kind () != zlink::framework::framework_error_kind_t::not_configured)
                        unexpected.fetch_add (1, std::memory_order_relaxed);
                }
                catch (...) {
                    unexpected.fetch_add (1, std::memory_order_relaxed);
                }
            }
        });

        app.request_stop ();
        app_thread.join ();
        querying.store (false, std::memory_order_release);
        querier.join ();
        assert (exit_code.load (std::memory_order_acquire) == 0);
        if (unexpected.load (std::memory_order_relaxed) != 0)
            throw std::runtime_error (
              "listener status during stop returned neither the record nor NotConfigured");
        verify_listener_status_is_not_configured (runtime, "listener-status-stop");
    }
}

void verify_client_server_terminal_errors_preserve_public_boundaries ()
{
    using client_server::client_server_operation_exception;
    using zlink::framework::framework_error_kind_t;
    using zlink::framework::detail::boundary_error_t;
    using zlink::framework::runtime::foundation::operation_terminal_t;

    const auto timed_out =
      client_server_operation_exception (operation_terminal_t::timed_out, "request");
    assert (timed_out.kind () == framework_error_kind_t::deadline_exceeded);
    assert (zlink::framework::detail::boundary_state (timed_out) == boundary_error_t::timed_out);

    const auto cancelled =
      client_server_operation_exception (operation_terminal_t::cancelled, "request");
    assert (zlink::framework::detail::boundary_state (cancelled) == boundary_error_t::cancelled);
    const auto cancelled_result =
      zlink::framework::detail::result_access_t::failure<void> (cancelled);
    assert (!cancelled_result && cancelled_result.error () == nullptr);
    try {
        cancelled_result.value ();
        assert (false);
    }
    catch (const std::system_error &error) {
        assert (error.code () == std::errc::operation_canceled);
    }

    const auto disconnected =
      client_server_operation_exception (operation_terminal_t::transport_failed, "request");
    assert (disconnected.kind () == framework_error_kind_t::unavailable);
    assert (zlink::framework::detail::boundary_state (disconnected)
            == boundary_error_t::disconnected);

    const auto shutdown =
      client_server_operation_exception (operation_terminal_t::shutdown, "request");
    assert (shutdown.kind () == framework_error_kind_t::shutting_down);
    assert (zlink::framework::detail::boundary_state (shutdown) == boundary_error_t::shutdown);

    const auto invalid =
      client_server_operation_exception (operation_terminal_t::completed, "request");
    assert (invalid.kind () == framework_error_kind_t::internal_failure);
    assert (zlink::framework::detail::boundary_state (invalid) == boundary_error_t::none);
}

} // namespace

namespace
{

class observation_1295_drain_probe_t final
    : public zlink::framework::hosted_service_t,
      public zlink::framework::runtime::hosted_service_lifecycle_t
{
  public:
    explicit observation_1295_drain_probe_t (std::shared_ptr<std::atomic_bool> called) :
        _called (std::move (called))
    {
    }

    zlink::framework::task_t<void> start (zlink::framework::service_provider_t &services) override
    {
        _runtime = &services.get_required<zlink::framework::client_server_runtime_t> ();
        co_return;
    }
    void stop () noexcept override {}
    bool drain_sessions_until (std::chrono::steady_clock::time_point) noexcept override
    {
        _called->store (true, std::memory_order_release);
        const auto status = _runtime->snapshot ("1295-zero-weight");
        assert (status.state == zlink::framework::topology_state_t::stopping);
        assert (!status.is_ready);
        assert (status.targets.size () == 1);
        assert (status.targets.front ().state == zlink::framework::peer_state_t::draining);
        assert (status.targets.front ().unavailable_reason
                == zlink::framework::topology_reason_t::draining);
        return true;
    }

  private:
    std::shared_ptr<std::atomic_bool> _called;
    zlink::framework::client_server_runtime_t *_runtime = nullptr;
};

template <typename TAction>
void with_observation_runtime (TAction action,
                               std::unique_ptr<zlink::framework::hosted_service_t> probe = {})
{
    using namespace zlink::framework;
    auto app = app_t::create ();
    app.add_zlink_framework ([] (zlink_framework_options_t &options) {
        options.handlers ().group ("1295-observation").add_send<network_probe_handler_t> ();
        options.add_client_server_channel ("1295-zero-weight")
          .server ()
          .listen ()
          .set_weight (0)
          .add_handler_group ("1295-observation");
        options.add_client_server_channel ("1295-disconnected")
          .client ()
          .connect ("tcp://127.0.0.1:1");
    });
    if (probe)
        app.add_hosted_service (std::move (probe));
    auto provider = app.advanced ().services ().build_provider ();
    auto &host = provider.get_required<framework_runtime_t> ();
    auto &runtime = provider.get_required<client_server_runtime_t> ();
    const auto before = runtime.snapshot ("1295-zero-weight");
    std::mutex mutex;
    std::condition_variable changed;
    bool serving = false;
    auto host_observation = host.observe (8, [&] (const auto &item) {
        std::lock_guard lock (mutex);
        serving = item.status.state == framework_runtime_state_t::serving;
        changed.notify_all ();
    });
    char program[] = "1295-observation";
    char *arguments[] = {program, nullptr};
    std::thread worker ([&] { assert (app.run (1, arguments) == 0); });
    {
        std::unique_lock lock (mutex);
        assert (changed.wait_for (lock, 5s, [&] { return serving; }));
    }
    action (runtime, app, before);
    host_observation->close ();
    app.request_stop ();
    worker.join ();
}

void verify_zero_weight_target_stays_ready ()
{
    with_observation_runtime ([] (auto &runtime, auto &, const auto &) {
        const auto status = runtime.snapshot ("1295-zero-weight");
        assert (status.targets.size () == 1);
        assert (status.targets.front ().weight == 0);
        assert (status.targets.front ().state == zlink::framework::peer_state_t::ready);
        assert (!status.targets.front ().unavailable_reason);
        assert (status.ready_target_count == 1);
        assert (!status.is_ready);
        assert (status.state == zlink::framework::topology_state_t::degraded);
    });
}

void verify_first_observation_contains_current_status ()
{
    with_observation_runtime ([] (auto &runtime, auto &, const auto &) {
        std::mutex mutex;
        std::condition_variable changed;
        std::optional<zlink::framework::client_server_channel_snapshot_t> first;
        auto observation =
          runtime.observe ("1295-zero-weight", 8,
                           [&] (const zlink::framework::observed_status_t<
                                zlink::framework::client_server_channel_snapshot_t> &item) {
                               std::lock_guard lock (mutex);
                               if (!first)
                                   first = item.status;
                               changed.notify_all ();
                           });
        {
            std::unique_lock lock (mutex);
            assert (changed.wait_for (lock, 5s, [&] { return first.has_value (); }));
            assert (first->targets.size () == 1);
            assert (first->targets.front ().weight == 0);
            assert (first->targets.front ().state == zlink::framework::peer_state_t::ready);
            assert (!first->targets.front ().unavailable_reason);
            assert (first->ready_target_count == 1);
            assert (!first->is_ready);
        }
        observation->close ();
    });
}

void verify_reconnecting_target_status ()
{
    with_observation_runtime ([] (auto &runtime, auto &, const auto &) {
        const auto status = runtime.snapshot ("1295-disconnected");
        assert (status.targets.size () == 1);
        assert (status.targets.front ().state == zlink::framework::peer_state_t::connecting);
        assert (status.targets.front ().unavailable_reason
                == zlink::framework::topology_reason_t::no_ready_target);
        assert (status.ready_target_count == 0);
    });
}

void verify_draining_host_target_status ()
{
    auto called = std::make_shared<std::atomic_bool> (false);
    with_observation_runtime ([] (auto &, auto &, const auto &) {},
                              std::make_unique<observation_1295_drain_probe_t> (called));
    assert (called->load (std::memory_order_acquire));
}

void verify_status_sequence_tracks_current_readiness ()
{
    with_observation_runtime ([] (auto &runtime, auto &, const auto &before) {
        const auto current = runtime.snapshot ("1295-zero-weight");
        assert (before.targets.empty ());
        assert (current.targets.size () == 1);
        assert (current.sequence > before.sequence);
        const auto unchanged = runtime.snapshot ("1295-zero-weight");
        assert (unchanged.sequence == current.sequence);
        assert (unchanged.ready_target_count == current.ready_target_count);
        assert (unchanged.targets.front ().state == current.targets.front ().state);
    });
}

void verify_terminal_observation_matches_current_status ()
{
    with_observation_runtime ([] (auto &runtime, auto &app, const auto &) {
        std::mutex mutex;
        std::condition_variable changed;
        std::optional<zlink::framework::client_server_channel_snapshot_t> terminal;
        auto observation = runtime.observe ("1295-zero-weight", 8, [&] (const auto &item) {
            std::lock_guard lock (mutex);
            if (item.status.state == zlink::framework::topology_state_t::stopped)
                terminal = item.status;
            changed.notify_all ();
        });
        app.request_stop ();
        zlink::framework::client_server_channel_snapshot_t terminal_status;
        {
            std::unique_lock lock (mutex);
            assert (changed.wait_for (lock, 5s, [&] { return terminal.has_value (); }));
            assert (!terminal->is_ready);
            assert (terminal->ready_target_count == 0);
            assert (terminal->targets.empty ());
            terminal_status = *terminal;
        }
        observation->close ();
        const auto current = runtime.snapshot ("1295-zero-weight");
        assert (current.state == zlink::framework::topology_state_t::stopped);
        assert (current.sequence == terminal_status.sequence);
        assert (current.ready_target_count == terminal_status.ready_target_count);
        assert (current.targets.empty ());
    });
}

void verify_unregistered_and_other_topology_channels_are_rejected ()
{
    using namespace zlink::framework;
    auto app = app_t::create ();
    app.add_zlink_framework ([] (zlink_framework_options_t &options) {
        options.add_client_server_channel ("1295-known").client ().connect ("tcp://127.0.0.1:1");
        options.add_fanout_channel ("1295-fanout").enable_publisher ();
    });
    auto provider = app.advanced ().services ().build_provider ();
    auto &runtime = provider.get_required<client_server_runtime_t> ();
    for (const auto *channel : {"1295-unregistered", "1295-fanout"}) {
        bool snapshot_rejected = false;
        try {
            (void) runtime.snapshot (channel);
        }
        catch (const framework_exception_t &error) {
            snapshot_rejected = error.kind () == framework_error_kind_t::not_configured;
        }
        assert (snapshot_rejected);
        bool observation_rejected = false;
        try {
            (void) runtime.observe (channel, 8, [] (const auto &) {});
        }
        catch (const framework_exception_t &error) {
            observation_rejected = error.kind () == framework_error_kind_t::not_configured;
        }
        assert (observation_rejected);
    }
}

void verify_unready_peer_projection_reason ()
{
    using namespace zlink::framework;
    const auto target = detail::project_topology_peer (zlink::routing_id_t::from ("1295-target"),
                                                       framework_runtime_state_t::stopped, false,
                                                       false, topology_reason_t::no_ready_target);
    assert (target.state == peer_state_t::not_connected);
    assert (target.unavailable_reason == topology_reason_t::no_ready_target);
    const auto publisher = detail::project_topology_peer (
      zlink::routing_id_t::from ("1295-publisher"), runtime::mesh::service_node_state_t::stopped,
      false, false, topology_reason_t::no_ready_peer);
    assert (publisher.state == peer_state_t::not_connected);
    assert (publisher.unavailable_reason == topology_reason_t::no_ready_peer);
}

class observation_1295_publish_handler_t
{
  public:
    using event_type = network_probe_message_t;
    void handle (const event_type &) {}
};

void verify_fanout_first_observation_is_complete ()
{
    using namespace zlink::framework;
    auto app = app_t::create ();
    app.add_zlink_framework ([] (zlink_framework_options_t &options) {
        options.add_location_store (std::make_shared<runtime::in_memory_location_store_t> ());
        options.handlers ()
          .group ("1295-observation-publish")
          .add_publish<observation_1295_publish_handler_t> ();
        options.add_fanout_channel ("1295-automatic-fanout")
          .enable_subscriber ()
          .use_handler_group ("1295-observation-publish");
    });
    auto provider = app.advanced ().services ().build_provider ();
    auto &runtime = provider.get_required<fanout_runtime_t> ();
    std::promise<fanout_channel_snapshot_t> first;
    std::once_flag first_only;
    auto observation = runtime.observe (
      "1295-automatic-fanout", 8, [&] (const observed_status_t<fanout_channel_snapshot_t> &item) {
          std::call_once (first_only, [&] { first.set_value (item.status); });
      });
    auto future = first.get_future ();
    assert (future.wait_for (5s) == std::future_status::ready);
    const auto status = future.get ();
    assert (status.channel_name == "1295-automatic-fanout");
    assert (status.state == topology_state_t::starting);
    assert (!status.is_ready);
    assert (status.ready_publisher_count == 0);
    assert (status.publishers.empty ());
    assert (status.sequence == runtime.snapshot ("1295-automatic-fanout").sequence);
    observation->close ();
}

} // namespace

int main ()
{
    verify_zero_weight_target_stays_ready ();
    verify_first_observation_contains_current_status ();
    verify_reconnecting_target_status ();
    verify_draining_host_target_status ();
    verify_status_sequence_tracks_current_readiness ();
    verify_terminal_observation_matches_current_status ();
    verify_unregistered_and_other_topology_channels_are_rejected ();
    verify_unready_peer_projection_reason ();
    verify_fanout_first_observation_is_complete ();
    verify_invalid_metadata_is_a_protocol_error ();
    verify_client_server_metadata_snapshot ();
    verify_client_server_send_does_not_wait_on_infrastructure_worker ();
    verify_client_server_readiness_counts_local_ready_servers ();
    verify_network_defaults_are_deferred_until_apply ();
    verify_client_server_terminal_errors_preserve_public_boundaries ();
    verify_client_server_runtime_projection_and_observation ();
    verify_public_listener_status_reports_bound_endpoint ();
    verify_listener_status_during_stop_reads_record_or_not_configured ();
    return 0;
}
