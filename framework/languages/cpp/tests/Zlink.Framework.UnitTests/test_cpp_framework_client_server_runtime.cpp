/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/client_server/raw_client_server_owner.hpp"
#include "runtime/client_server/client_server_failure_mapper.hpp"
#include "runtime/channels/channel_runtime.hpp"
#include "runtime/diagnostics/listener_status_registry.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/streams/stream_runtime.hpp"
#include "test_completion_poller_driver.hpp"
#include <zlink/Contracts/Sockets/routed_socket_contracts.hpp>
#include <zlink/Contracts/Messaging/operation_contracts.hpp>

#include <zlink/framework.hpp>

#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <fstream>
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
    while (!runtime.snapshot ("slow-send").selectable
           && std::chrono::steady_clock::now () < deadline)
        std::this_thread::sleep_for (1ms);
    assert (runtime.snapshot ("slow-send").selectable);
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
    while (!runtime.snapshot ("metadata").selectable
           && std::chrono::steady_clock::now () < deadline)
        std::this_thread::sleep_for (1ms);
    assert (runtime.snapshot ("metadata").selectable);
    while (!runtime.snapshot ("metadata-empty").selectable
           && std::chrono::steady_clock::now () < deadline)
        std::this_thread::sleep_for (1ms);
    assert (runtime.snapshot ("metadata-empty").selectable);
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
    int ready_server_count;
};

constexpr std::array readiness_cases{
  readiness_case_t{"ready-server", zlink::framework::client_server_role_t::server, 1},
  readiness_case_t{"zero-weight-server", zlink::framework::client_server_role_t::server, 0},
  readiness_case_t{"client-without-server", zlink::framework::client_server_role_t::client, 0},
  readiness_case_t{"client-and-server", zlink::framework::client_server_role_t::client_and_server,
                   1}};

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
            assert (snapshot.ready_server_count == test.ready_server_count);
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
        assert (runtime.snapshot (test.channel_name).ready_server_count == 0);
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

    while (!runtime.snapshot ("client-and-server").selectable
           && std::chrono::steady_clock::now () < deadline)
        std::this_thread::sleep_for (1ms);

    for (const auto &test : readiness_cases) {
        const auto snapshot = runtime.snapshot (test.channel_name);
        assert (snapshot.local_role == test.role);
        assert (snapshot.ready_server_count == test.ready_server_count);
        assert (runtime.is_ready (test.channel_name) == (test.ready_server_count > 0));
        assert (snapshot.selectable
                == (test.role == zlink::framework::client_server_role_t::client_and_server));
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
        assert (runtime.snapshot (test.channel_name).ready_server_count == 0);
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
    assert (!before.selectable);
    assert (before.ready_server_count == 0);

    std::atomic_int event_count{0};
    std::mutex event_mutex;
    std::condition_variable event_changed;
    auto observation = runtime.observe (
      "client-server-runtime-unit", 8,
      [&event_count, &event_changed] (
        const zlink::framework::observed_status_t<zlink::framework::client_server_runtime_event_t>
          &observed) {
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
    assert (after.selectable);
    assert (after.ready_server_count == 1);
    assert (after.connection_intent_count == 1);
    assert (after.servers.size () == 1);
    assert (after.servers.front ().ready);
    assert (after.servers.front ().descriptor_source == "manual");

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
    assert (cancelled.kind () == framework_error_kind_t::invalid_operation);
    assert (zlink::framework::detail::boundary_state (cancelled) == boundary_error_t::cancelled);

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

int main ()
{
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
