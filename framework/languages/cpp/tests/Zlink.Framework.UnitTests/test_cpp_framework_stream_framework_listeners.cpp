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

class prefix_stream_compression_codec_t final : public zlink::framework::stream_compression_codec_t
{
  public:
    explicit prefix_stream_compression_codec_t (std::string prefix) : _prefix (std::move (prefix))
    {
    }

    zlink::message_t compress (const zlink::message_t &payload) const override
    {
        return zlink::message_t::from (_prefix + payload.to_string ());
    }

    zlink::message_t decompress (const zlink::message_t &payload,
                                 std::size_t max_decompressed_size) const override
    {
        const auto text = payload.to_string ();
        if (text.rfind (_prefix, 0) != 0) {
            throw std::runtime_error ("custom stream compression marker is invalid");
        }
        auto decoded = text.substr (_prefix.size ());
        if (decoded.size () > max_decompressed_size) {
            throw std::runtime_error ("custom stream payload exceeds receive limit");
        }
        return zlink::message_t::from (decoded);
    }

  private:
    std::string _prefix;
};

class oversized_stream_compression_codec_t final
    : public zlink::framework::stream_compression_codec_t
{
  public:
    zlink::message_t compress (const zlink::message_t &payload) const override { return payload; }

    zlink::message_t decompress (const zlink::message_t &, std::size_t) const override
    {
        return zlink::message_t::from (std::string (64 * 1024 + 1, 'x'));
    }
};

class transport_error_session_t final : public zlink::framework::packet_stream_session_t
{
  public:
    static constexpr char async_write_probe_name[] = "async-write-probe";
    static constexpr std::size_t async_write_probe_size = 8 * 1024 * 1024;
    static constexpr int async_write_receive_buffer_size = 4096;
    std::promise<void> async_write_submitted;

    zlink::framework::task_t<void> on_connected (zlink::framework::stream_t &stream) override
    {
        {
            const std::lock_guard lock (_mutex);
            _identity_available =
              stream.local_address ().has_value () && stream.remote_address ().has_value ();
            if (_identity_available) {
                _last_local = *stream.local_address ();
                _last_remote = *stream.remote_address ();
            }
        }
        record (_connected);
        co_return;
    }

    zlink::framework::task_t<void> on_disconnected (zlink::framework::stream_t &) override
    {
        record (_disconnected);
        co_return;
    }

    zlink::framework::task_t<void> on_error (zlink::framework::stream_t &,
                                             const zlink::framework::stream_error_t &error) override
    {
        {
            const std::lock_guard lock (_mutex);
            ++_errors;
            _last_error = error.error ();
        }
        _changed.notify_all ();
        co_return;
    }

    zlink::framework::task_t<void>
    on_packet (zlink::framework::stream_t &stream,
               const zlink::framework::session_message_context_t &dispatch,
               const zlink::framework::message_t &payload) override
    {
        record (_packets);
        if (dispatch.packet_name == async_write_probe_name) {
            auto writing = stream
                             .reply_packet (zlink::framework::message_t::from (
                               std::string (async_write_probe_size, 'x')))
                             .async ();
            async_write_submitted.set_value ();
            co_await std::move (writing);
            co_return;
        }
        if (dispatch.can_reply) {
            (void) co_await stream.reply_packet (payload).async ();
        }
        co_return;
    }

    bool wait_connected (int count) { return wait_for (_connected, count); }
    bool wait_disconnected (int count) { return wait_for (_disconnected, count); }
    bool wait_errors (int count) { return wait_for (_errors, count); }
    bool wait_packets (int count) { return wait_for (_packets, count); }

    bool identity_available () const
    {
        const std::lock_guard lock (_mutex);
        return _identity_available;
    }

    std::string last_local () const
    {
        const std::lock_guard lock (_mutex);
        return _last_local;
    }

    std::string last_remote () const
    {
        const std::lock_guard lock (_mutex);
        return _last_remote;
    }

    int errors () const
    {
        const std::lock_guard lock (_mutex);
        return _errors;
    }

    int packets () const
    {
        const std::lock_guard lock (_mutex);
        return _packets;
    }

    zlink::framework::stream_session_error_t last_error () const
    {
        const std::lock_guard lock (_mutex);
        return _last_error;
    }

  private:
    void record (int &counter)
    {
        {
            const std::lock_guard lock (_mutex);
            ++counter;
        }
        _changed.notify_all ();
    }

    bool wait_for (int &counter, int count)
    {
        std::unique_lock lock (_mutex);
        return _changed.wait_for (lock, std::chrono::seconds (2), [&] { return counter >= count; });
    }

    mutable std::mutex _mutex;
    std::condition_variable _changed;
    int _connected = 0;
    int _disconnected = 0;
    int _errors = 0;
    int _packets = 0;
    bool _identity_available = false;
    std::string _last_local;
    std::string _last_remote;
    zlink::framework::stream_session_error_t _last_error =
      zlink::framework::stream_session_error_t::internal;
};

class permit_wait_session_t final : public zlink::framework::packet_stream_session_t
{
  public:
    zlink::framework::task_t<void> on_connected (zlink::framework::stream_t &) override
    {
        connected.set_value ();
        co_return;
    }

    zlink::framework::task_t<void> on_disconnected (zlink::framework::stream_t &) override
    {
        disconnected.set_value ();
        co_return;
    }

    zlink::framework::task_t<void> on_error (zlink::framework::stream_t &,
                                             const zlink::framework::stream_error_t &) override
    {
        ++errors;
        co_return;
    }

    zlink::framework::task_t<void> on_packet (zlink::framework::stream_t &,
                                              const zlink::framework::session_message_context_t &,
                                              const zlink::framework::message_t &) override
    {
        ++packets;
        co_return;
    }

    std::promise<void> connected;
    std::promise<void> disconnected;
    std::atomic_int packets{0};
    std::atomic_int errors{0};
};

class rejected_connected_session_t final : public zlink::framework::packet_stream_session_t
{
  public:
    zlink::framework::task_t<void> on_connected (zlink::framework::stream_t &stream) override
    {
        {
            const std::lock_guard lock (_mutex);
            _stream = stream;
            _manager_was_attached = &stream.actors () != nullptr;
        }
        _changed.notify_all ();
        return zlink::framework::task_t<void> (zlink::framework::result_t<void>::failure (
          zlink::framework::framework_error_kind_t::internal_failure,
          "connection callback rejected"));
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

    bool wait_until_actor_manager_is_detached ()
    {
        {
            std::unique_lock lock (_mutex);
            if (!_changed.wait_for (lock, std::chrono::seconds (2),
                                    [&] { return _stream.has_value (); })) {
                return false;
            }
            if (!_manager_was_attached) {
                return false;
            }
        }
        const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (2);
        while (std::chrono::steady_clock::now () < deadline) {
            zlink::framework::stream_t observed;
            {
                const std::lock_guard lock (_mutex);
                observed = *_stream;
            }
            try {
                (void) observed.actors ();
            }
            catch (const zlink::framework::framework_exception_t &error) {
                return error.kind () == zlink::framework::framework_error_kind_t::not_configured;
            }
            std::this_thread::sleep_for (std::chrono::milliseconds (10));
        }
        return false;
    }

  private:
    std::mutex _mutex;
    std::condition_variable _changed;
    std::optional<zlink::framework::stream_t> _stream;
    bool _manager_was_attached = false;
};
} // namespace

int stream_listener_cases (metric_test::provider_t &metric_provider)
{
    using zlink::framework::framework_error_kind_t;
    using zlink::framework::stream_codec_t;
    using zlink::framework::detail::stream_header_flags_t;
    using zlink::framework::detail::stream_message_kind_t;

#ifdef ZLINK_FRAMEWORK_STREAM_TEST_WITH_OPENSSL
    zlink::framework::service_collection_t mutual_tls_services;
    zlink::framework::handler_registry_t mutual_tls_handlers;
    zlink::framework::serializer_registry_t mutual_tls_serializers;
    zlink::framework::zlink_builder_t mutual_tls_zlink =
      zlink::framework::test::runtime_failure_builder ();
    zlink::framework::zlink_framework_options_t mutual_tls_options (
      mutual_tls_services, mutual_tls_handlers, mutual_tls_serializers, mutual_tls_zlink);
    mutual_tls_options.add_stream_node ("mutual-tls-listener")
      .bind ("tls://127.0.0.1:0")
      .set_tls_server (ZLINK_FRAMEWORK_STREAM_TEST_CERT, ZLINK_FRAMEWORK_STREAM_TEST_KEY, true)
      .register_session ("mutual-tls-listener-session");
    mutual_tls_options.apply ();
    auto mutual_tls_provider = mutual_tls_services.build_provider ();
    sample_session_t mutual_tls_session;
    auto mutual_tls_listeners =
      std::make_shared<zlink::framework::runtime::listener_status_registry_t> ();
    zlink::framework::runtime::stream_host_service_t mutual_tls_host (
      std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> (),
      zlink::framework::detail::stream_runtime_t::from (mutual_tls_zlink),
      zlink::framework::detail::stream_runtime_t::from (mutual_tls_zlink).snapshots (),
      {{"mutual-tls-listener-session",
        [&mutual_tls_session] (zlink::framework::service_provider_t &)
          -> zlink::framework::packet_stream_session_t & { return mutual_tls_session; }}},
      std::chrono::milliseconds{30'000}, nullptr, {}, mutual_tls_listeners);
    mutual_tls_host.start (mutual_tls_provider);
    const auto mutual_tls_port = endpoint_port (listener_endpoint (
      *mutual_tls_listeners, zlink::framework::listener_kind_t::stream, "mutual-tls-listener"));
    boost::asio::io_context mutual_tls_io;
    boost::asio::ssl::context mutual_tls_client_context (boost::asio::ssl::context::tls_client);
    mutual_tls_client_context.set_verify_mode (boost::asio::ssl::verify_none);
    boost::asio::ssl::stream<boost::asio::ip::tcp::socket> mutual_tls_client (
      mutual_tls_io, mutual_tls_client_context);
    boost::system::error_code mutual_tls_error;
    mutual_tls_client.next_layer ().connect (
      {boost::asio::ip::make_address ("127.0.0.1"), mutual_tls_port}, mutual_tls_error);
    if (mutual_tls_error) {
        mutual_tls_host.stop ();
        return 26;
    }
    mutual_tls_client.handshake (boost::asio::ssl::stream_base::client, mutual_tls_error);
    mutual_tls_host.stop ();
    // TLS 1.3 clients may observe the server's certificate-required alert only
    // on their next I/O; the server-side contract is that no Session exists.
    if (!mutual_tls_session.events.empty ()) {
        return 26;
    }
#endif

    auto custom_codec = std::make_shared<prefix_stream_compression_codec_t> ("custom-stream:");
    zlink::framework::service_collection_t custom_services;
    zlink::framework::handler_registry_t custom_handlers;
    zlink::framework::serializer_registry_t custom_serializers;
    custom_serializers.add<std::string> (
      [] (const std::string &value) {
          return zlink::framework::encoded_payload_t::from_string (value);
      },
      [] (const zlink::framework::encoded_payload_t &payload) { return payload.to_string (); });
    zlink::framework::zlink_builder_t custom_zlink =
      zlink::framework::test::runtime_failure_builder ();
    zlink::framework::zlink_framework_options_t custom_options (custom_services, custom_handlers,
                                                                custom_serializers, custom_zlink);
    custom_options.configure_stream_compression ().use (custom_codec);
    custom_options.add_stream_node ("custom-stream")
      .bind ("tcp://0.0.0.0:9201")
      .register_session ("custom-session");
    custom_options.add_stream_node ("configured-mutual-tls")
      .bind ("tls://127.0.0.1:9204")
      .set_tls_server ("server.crt", "server.key", true)
      .register_session ("configured-mutual-tls-session");
    custom_options.apply ();
    zlink::framework::detail::bind_stream_serializers (custom_zlink, custom_serializers);
    auto custom_runtime = zlink::framework::detail::stream_runtime_t::from (custom_zlink);
    const auto configured_tls = custom_runtime.snapshots ();
    const auto configured_tls_snapshot =
      std::find_if (configured_tls.begin (), configured_tls.end (), [] (const auto &candidate) {
          return candidate.name == "configured-mutual-tls";
      });
    if (configured_tls_snapshot == configured_tls.end ()
        || !configured_tls_snapshot->tls_require_client_certificate) {
        return 26;
    }
    auto custom_stream = custom_runtime.open_session ("custom-stream");
    custom_stream.write_packet (zlink::framework::message_t::from (std::string ("custom-outbound")))
      .compress ()
      .async ()
      .result ()
      .value ();
    if (custom_runtime.written_payloads (custom_stream).size () != 1
        || custom_runtime.written_payloads (custom_stream)[0].to_string ()
             != "custom-stream:custom-outbound") {
        return 24;
    }
    zlink::framework::detail::stream_header_t custom_inbound_header (
      stream_message_kind_t::request, stream_codec_t::raw,
      stream_header_flags_t::has_request_seq | stream_header_flags_t::payload_compressed, 88,
      "custom-inbound");
    sample_session_t custom_session;
    const auto custom_dispatch = custom_runtime.dispatch_packet (
      custom_session, custom_stream, custom_inbound_header,
      custom_codec->compress (zlink::message_t::from (std::string ("custom-inbound-payload"))));
    if (!custom_dispatch || custom_session.events.size () != 1
        || custom_session.events[0] != "packet:custom-inbound:custom-inbound-payload") {
        return 25;
    }

    zlink::framework::zlink_builder_t disabled_zlink =
      zlink::framework::test::runtime_failure_builder ();
    zlink::framework::zlink_framework_options_t disabled_options (
      custom_services, custom_handlers, custom_serializers, disabled_zlink);
    disabled_options.configure_stream_compression ().disable ();
    disabled_options.add_stream_node ("disabled-stream")
      .bind ("tcp://0.0.0.0:9202")
      .register_session ("disabled-session");
    disabled_options.apply ();
    zlink::framework::detail::bind_stream_serializers (disabled_zlink, custom_serializers);
    auto disabled_runtime = zlink::framework::detail::stream_runtime_t::from (disabled_zlink);
    auto disabled_stream = disabled_runtime.open_session ("disabled-stream");
    bool disabled_compress_rejected = false;
    try {
        disabled_stream.write_packet (zlink::framework::message_t::from (std::string ("disabled")))
          .compress ()
          .async ()
          .result ()
          .value ();
    }
    catch (const zlink::framework::framework_exception_t &) {
        disabled_compress_rejected = true;
    }
    if (!disabled_compress_rejected) {
        return 27;
    }
    sample_session_t disabled_session;
    const auto disabled_receive = disabled_runtime.dispatch_packet (
      disabled_session, disabled_stream, custom_inbound_header,
      custom_codec->compress (zlink::message_t::from (std::string ("disabled-inbound"))));
    if (disabled_receive || disabled_receive.error_kind () != framework_error_kind_t::protocol_error
        || std::string (disabled_receive.error ()->what ())
               .find ("compression codec is not configured")
             == std::string::npos
        || !disabled_session.events.empty ()) {
        return 27;
    }

    zlink::framework::zlink_builder_t oversized_zlink =
      zlink::framework::test::runtime_failure_builder ();
    zlink::framework::zlink_framework_options_t oversized_options (
      custom_services, custom_handlers, custom_serializers, oversized_zlink);
    oversized_options.configure_stream_compression ().use (
      std::make_shared<oversized_stream_compression_codec_t> ());
    oversized_options.add_stream_node ("oversized-stream")
      .bind ("tcp://0.0.0.0:9203")
      .register_session ("oversized-session");
    oversized_options.apply ();
    auto oversized_runtime = zlink::framework::detail::stream_runtime_t::from (oversized_zlink);
    auto oversized_stream = oversized_runtime.open_session ("oversized-stream");
    sample_session_t oversized_session;
    const auto oversized_receive =
      oversized_runtime.dispatch_packet (oversized_session, oversized_stream, custom_inbound_header,
                                         zlink::message_t::from (std::string ("compressed")));
    if (oversized_receive
        || oversized_receive.error_kind () != framework_error_kind_t::protocol_error
        || !oversized_session.events.empty ()) {
        return 28;
    }

    zlink::framework::service_collection_t transport_services;
    zlink::framework::handler_registry_t transport_handlers;
    zlink::framework::serializer_registry_t transport_serializers;
    transport_serializers.add<std::string> (
      [] (const std::string &value) {
          return zlink::framework::encoded_payload_t::from_string (value);
      },
      [] (const zlink::framework::encoded_payload_t &payload) { return payload.to_string (); });
    zlink::framework::zlink_builder_t transport_zlink =
      zlink::framework::test::runtime_failure_builder ();
    transport_services.add_singleton<zlink::framework::detail::actor_gateway_runtime_t> ();
    transport_services.add_factory<zlink::framework::session_actor_manager_t> (
      [] (zlink::framework::service_provider_t &provider) {
          return std::make_unique<zlink::framework::session_actor_manager_t> (
            provider.get_required<zlink::framework::detail::actor_gateway_runtime_t> ().manager ());
      },
      zlink::framework::service_lifetime_t::scoped);
    zlink::framework::zlink_framework_options_t transport_options (
      transport_services, transport_handlers, transport_serializers, transport_zlink);
    auto transport_stream_options = transport_options.add_stream_node ("transport-stream");
    transport_stream_options.configure_socket ().max_message_size = 0;
    transport_stream_options.bind (loopback_tcp_any_port).register_session ("transport-session");
    transport_options.apply ();
    zlink::framework::detail::bind_stream_serializers (transport_zlink, transport_serializers);
    const auto stale_errors = std::make_shared<zlink::framework::test::telemetry_observations_t<
      zlink::framework::message_dispatch_error_event_t>> ();
    const auto stale_flows = std::make_shared<
      zlink::framework::test::telemetry_observations_t<zlink::framework::message_flow_event_t>> ();
    zlink::framework::dispatch_options_t transport_dispatch_options;
    transport_dispatch_options.message_flow (zlink::framework::message_flow_log_mode_t::errors);
    zlink::framework::detail::dispatch_options_access_t::set_dispatch_error_observer_for_tests (
      transport_dispatch_options,
      [stale_errors] (const auto &event) { stale_errors->record (event); });
    zlink::framework::detail::dispatch_options_access_t::set_observer_for_tests (
      transport_dispatch_options,
      [stale_flows] (const auto &event) { stale_flows->record (event); });
    zlink::framework::detail::apply_dispatch_options (transport_zlink, transport_dispatch_options);
    auto transport_provider = transport_services.build_provider ();
    transport_error_session_t transport_session;
    auto transport_listeners =
      std::make_shared<zlink::framework::runtime::listener_status_registry_t> ();
    zlink::framework::runtime::stream_host_service_t transport_host (
      std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> (),
      zlink::framework::detail::stream_runtime_t::from (transport_zlink),
      zlink::framework::detail::stream_runtime_t::from (transport_zlink).snapshots (),
      {{"transport-session",
        [&transport_session] (zlink::framework::service_provider_t &)
          -> zlink::framework::packet_stream_session_t & { return transport_session; }}},
      std::chrono::milliseconds{30'000}, nullptr, {}, transport_listeners);
    auto transport_drain = std::make_shared<std::atomic_bool> (false);
    transport_host.bind_drain_flag (transport_drain);
    transport_host.bind_monitoring (zlink::framework::test::runtime_failure_monitoring ());
    transport_host.start (transport_provider);
    const auto transport_endpoint = listener_endpoint (
      *transport_listeners, zlink::framework::listener_kind_t::stream, "transport-stream");
    const auto transport_port = endpoint_port (transport_endpoint);

    auto graceful_client = connect_loopback (transport_port);
    if (!graceful_client || !transport_session.wait_connected (1)
        || !transport_session.identity_available ()
        || transport_session.last_local ().find (":") == std::string::npos
        || transport_session.last_remote ().find (":") == std::string::npos) {
        transport_host.stop ();
        return 29;
    }
    close_native_client (graceful_client, true);
    if (!transport_session.wait_disconnected (1) || transport_session.errors () != 0) {
        transport_host.stop ();
        return 30;
    }

    auto failed_client = connect_loopback (transport_port);
    if (!failed_client || !transport_session.wait_connected (2)) {
        transport_host.stop ();
        return 31;
    }
    boost::system::error_code ignored;
    failed_client->socket.set_option (boost::asio::socket_base::linger (true, 0), ignored);
    close_native_client (failed_client);
    const bool transport_failure_reported = transport_session.wait_errors (1);
    const bool transport_disconnect_reported = transport_session.wait_disconnected (2);
    if (!transport_failure_reported || !transport_disconnect_reported
        || transport_session.last_error ()
             != zlink::framework::stream_session_error_t::transport_error) {
        transport_host.stop ();
        return 32;
    }

    zlink::stream_connector::connector_options_t connector_options;
    connector_options.endpoint = transport_endpoint;
    connector_options.connect_timeout = std::chrono::seconds (2);
    auto connector = zlink::stream_connector::connector_factory_t::create (connector_options);
    if (!connector.connect () || !transport_session.wait_connected (3)) {
        transport_host.stop ();
        return 36;
    }
    const auto connector_payload = zlink::message_t::from ("connector-payload").to_bytes ();
    auto connector_reply = connector
                             .request (zlink::stream_connector::packet_t{
                               .name = "connector-probe", .payload = connector_payload})
                             .timeout (std::chrono::seconds (2))
                             .submit<std::vector<std::uint8_t>> ();
    if (!connector_reply || connector_reply.value () != connector_payload
        || !transport_session.wait_packets (1) || !connector.close ()
        || !transport_session.wait_disconnected (3)) {
        transport_host.stop ();
        return 36;
    }

    /* common internals §7.6 / transport liveness §4: a connection that has
     * only part of a frame must release the listener turn while it waits for
     * more bytes. A second connection must therefore deliver its complete
     * frame before the first connection completes. */
    auto transport_runtime = zlink::framework::detail::stream_runtime_t::from (transport_zlink);
    const auto transport_snapshots = transport_runtime.snapshots ();
    if (transport_snapshots.size () != 1 || transport_snapshots[0].max_message_size != 0) {
        transport_host.stop ();
        return 45;
    }
    const zlink::framework::detail::stream_header_t fairness_header (
      zlink::framework::detail::stream_message_kind_t::send, zlink::framework::stream_codec_t::raw,
      zlink::framework::detail::stream_header_flags_t::none, std::nullopt, "fairness-probe");
    const auto fairness_frame = make_native_stream_frame (
      transport_runtime, fairness_header, zlink::message_t::from (std::string ("fairness")));
    auto partial_client = connect_loopback (transport_port);
    if (!partial_client || !transport_session.wait_connected (4)) {
        close_native_client (partial_client);
        transport_host.stop ();
        return 39;
    }
    send_native_bytes (partial_client, fairness_frame, 0, 1);
    /* Leave only the first byte at the listener. The second client must not
     * wait for the rest of this frame before it can be dispatched. */
    auto ready_client = connect_loopback (transport_port);
    if (!ready_client || !transport_session.wait_connected (5)) {
        close_native_client (partial_client);
        close_native_client (ready_client);
        transport_host.stop ();
        return 40;
    }
    send_native_bytes (ready_client, fairness_frame);
    if (!transport_session.wait_packets (2)) {
        close_native_client (partial_client);
        close_native_client (ready_client);
        transport_host.stop ();
        return 41;
    }
    send_native_bytes (partial_client, fairness_frame, 1, fairness_frame.size () - 1);
    if (!transport_session.wait_packets (3)) {
        close_native_client (partial_client);
        close_native_client (ready_client);
        transport_host.stop ();
        return 42;
    }
    close_native_client (partial_client, true);
    close_native_client (ready_client, true);

    /* The receive batch is limited to 64 frames. Keep 65 complete frames in
     * one user-space read so the final frame has no new kernel readability
     * event to wake the scheduler; the buffered-ready handoff must schedule
     * it again without another client write. */
    auto buffered_client = connect_loopback (transport_port);
    if (!buffered_client || !transport_session.wait_connected (6)) {
        close_native_client (buffered_client);
        transport_host.stop ();
        return 43;
    }
    const auto buffered_payload = zlink::message_t::from (std::string (512, 'x'));
    std::vector<std::uint8_t> buffered_frames;
    for (int index = 0; index < 65; ++index) {
        const auto frame =
          make_native_stream_frame (transport_runtime, fairness_header, buffered_payload);
        buffered_frames.insert (buffered_frames.end (), frame.begin (), frame.end ());
    }
    send_native_bytes (buffered_client, buffered_frames);
    if (!transport_session.wait_packets (68)) {
        close_native_client (buffered_client);
        transport_host.stop ();
        return 44;
    }
    close_native_client (buffered_client, true);

    auto stale_client = connect_loopback (transport_port);
    if (!stale_client || !transport_session.wait_connected (7)) {
        close_native_client (stale_client);
        transport_host.stop ();
        return 325;
    }
    zlink::framework::detail::stream_header_t stale_wire_send (
      stream_message_kind_t::send, stream_codec_t::raw, stream_header_flags_t::none, std::nullopt,
      "stale-wire-send");
    stale_wire_send.with_actor_slot (42);
    send_native_bytes (stale_client,
                       make_native_stream_frame (transport_runtime, stale_wire_send,
                                                 zlink::message_t::from ("stale-send")));
    zlink::framework::detail::stream_header_t stale_wire_request (
      stream_message_kind_t::request, stream_codec_t::raw, stream_header_flags_t::has_request_seq,
      91, "stale-wire-request");
    stale_wire_request.with_actor_slot (42);
    send_native_bytes (stale_client,
                       make_native_stream_frame (transport_runtime, stale_wire_request,
                                                 zlink::message_t::from ("stale-request")));
    boost::system::error_code stale_read_error;
    stale_client->socket.non_blocking (true, stale_read_error);
    std::vector<std::uint8_t> stale_reply_bytes;
    const auto stale_reply_deadline = std::chrono::steady_clock::now () + std::chrono::seconds (2);
    while (!stale_read_error && std::chrono::steady_clock::now () < stale_reply_deadline) {
        std::array<std::uint8_t, 4096> chunk{};
        const auto size =
          stale_client->socket.read_some (boost::asio::buffer (chunk), stale_read_error);
        if (!stale_read_error) {
            stale_reply_bytes.insert (stale_reply_bytes.end (), chunk.begin (),
                                      chunk.begin () + static_cast<std::ptrdiff_t> (size));
            if (stale_reply_bytes.size () >= 6) {
                const auto header_size =
                  (static_cast<std::size_t> (stale_reply_bytes[0]) << 8) | stale_reply_bytes[1];
                const auto payload_size = (static_cast<std::size_t> (stale_reply_bytes[2]) << 24)
                                          | (static_cast<std::size_t> (stale_reply_bytes[3]) << 16)
                                          | (static_cast<std::size_t> (stale_reply_bytes[4]) << 8)
                                          | stale_reply_bytes[5];
                if (stale_reply_bytes.size () >= 6 + header_size + payload_size)
                    break;
            }
        } else if (stale_read_error == boost::asio::error::would_block
                   || stale_read_error == boost::asio::error::try_again) {
            stale_read_error.clear ();
            std::this_thread::sleep_for (std::chrono::milliseconds (1));
        }
    }
    close_native_client (stale_client, true);
    if (stale_read_error || stale_reply_bytes.size () < 6 || transport_session.packets () != 68) {
        transport_host.stop ();
        return 326;
    }
    const auto stale_header_size =
      (static_cast<std::size_t> (stale_reply_bytes[0]) << 8) | stale_reply_bytes[1];
    const auto stale_payload_size = (static_cast<std::size_t> (stale_reply_bytes[2]) << 24)
                                    | (static_cast<std::size_t> (stale_reply_bytes[3]) << 16)
                                    | (static_cast<std::size_t> (stale_reply_bytes[4]) << 8)
                                    | stale_reply_bytes[5];
    if (stale_reply_bytes.size () < 6 + stale_header_size + stale_payload_size) {
        transport_host.stop ();
        return 327;
    }
    const std::vector<std::uint8_t> stale_header_bytes (
      stale_reply_bytes.begin () + 6, stale_reply_bytes.begin () + 6 + stale_header_size);
    const auto stale_reply_header = transport_runtime.decode_header (stale_header_bytes);
    const std::string stale_reply_payload (stale_reply_bytes.begin () + 6 + stale_header_size,
                                           stale_reply_bytes.begin () + 6 + stale_header_size
                                             + stale_payload_size);
    if (!stale_reply_header || stale_reply_header.value ().kind () != stream_message_kind_t::error
        || stale_reply_header.value ().request_seq () != 91
        || stale_reply_payload.find ("\"code\":\"invalid_operation\"") == std::string::npos) {
        transport_host.stop ();
        return 328;
    }
    zlink::framework::test::acknowledge_delivery ();
    const auto stale_dispatch_errors = stale_errors->snapshot ();
    const auto stale_flow_events = stale_flows->snapshot ();
    bool stale_diagnostics_recorded = false;
    {
        stale_diagnostics_recorded =
          std::any_of (
            stale_dispatch_errors.begin (), stale_dispatch_errors.end (),
            [] (const auto &event) {
                return event.surface == zlink::framework::dispatch_error_surface_t::stream_session
                       && event.message_kind == zlink::framework::dispatch_message_kind_t::request
                       && event.reason == zlink::framework::dispatch_error_reason_t::stale_target
                       && event.action == zlink::framework::dispatch_error_action_t::reply_error;
            })
          && std::any_of (
            stale_flow_events.begin (), stale_flow_events.end (), [] (const auto &event) {
                return event.outcome == zlink::framework::message_flow_outcome_t::dropped
                       && event.message_kind == zlink::framework::dispatch_message_kind_t::send
                       && event.reason == zlink::framework::message_flow_reason_t::stale_target;
            });
    }
    if (!stale_diagnostics_recorded) {
        transport_host.stop ();
        return 329;
    }

    // The peer deliberately does not read a reply larger than the socket
    // buffers. async() must return a pending task before transport completion;
    // otherwise the application worker becomes a second connection io pump.
    auto async_write_client = connect_loopback (transport_port);
    if (!async_write_client) {
        transport_host.stop ();
        return 330;
    }
    async_write_client->socket.set_option (boost::asio::socket_base::receive_buffer_size (
      transport_error_session_t::async_write_receive_buffer_size));
    auto async_write_submitted = transport_session.async_write_submitted.get_future ();
    const zlink::framework::detail::stream_header_t async_write_header (
      stream_message_kind_t::request, stream_codec_t::raw,
      zlink::framework::detail::stream_header_flags_t::has_request_seq, 1,
      transport_error_session_t::async_write_probe_name);
    send_native_bytes (async_write_client,
                       make_native_stream_frame (transport_runtime, async_write_header,
                                                 zlink::message_t::from ("probe")));
    const bool submission_returned =
      async_write_submitted.wait_for (std::chrono::seconds (2)) == std::future_status::ready;
    close_native_client (async_write_client);
    if (!submission_returned || !transport_session.wait_disconnected (8)) {
        transport_host.stop ();
        return 331;
    }

    transport_drain->store (true, std::memory_order_release);
    auto draining_client = connect_loopback (transport_port);
    const auto draining_bytes = read_until_peer_close (draining_client, std::chrono::seconds (5));
    const zlink::framework::detail::stream_header_t draining_header (
      stream_message_kind_t::control, stream_codec_t::raw, stream_header_flags_t::none,
      std::nullopt, "session-closing", {});
    const auto draining_payload =
      zlink::framework::detail::stream_runtime_t::encode_session_closing_payload (
        zlink::framework::stream_close_reason_t::server_drain, "node is draining");
    const auto expected_draining_bytes =
      transport_runtime.encode_frame (draining_header, zlink::message_t::from (draining_payload));
    close_native_client (draining_client);
    if (!draining_bytes || !expected_draining_bytes
        || *draining_bytes != expected_draining_bytes.value ()) {
        transport_host.stop ();
        return 332;
    }

    transport_host.stop ();

    // Permit 대기를 확정한 뒤 종료합니다. 시간 임계값 대신 completion을
    // 관찰하며, io가 깨지 않는 결함은 CTest의 deadlock 제한이 검출합니다.
    enum class permit_wait_end_t
    {
        host_stop,
        source_close
    };
    constexpr auto permit_stream_name = "permit-wait-stream";
    constexpr auto permit_session_name = "permit-wait-session";
    for (const auto end : {permit_wait_end_t::host_stop, permit_wait_end_t::source_close}) {
        zlink::framework::zlink_builder_t permit_zlink =
          zlink::framework::test::runtime_failure_builder ();
        zlink::framework::zlink_framework_options_t permit_options (
          transport_services, transport_handlers, transport_serializers, permit_zlink);
        permit_options.add_stream_node (permit_stream_name)
          .bind (loopback_tcp_any_port)
          .register_session (permit_session_name);
        permit_options.apply ();
        zlink::framework::detail::apply_dispatch_options (permit_zlink, transport_dispatch_options);
        auto permit_runtime = zlink::framework::detail::stream_runtime_t::from (permit_zlink);
        auto application_jobs =
          std::make_shared<zlink::framework::runtime::application_job_queue_t> (
            zlink::framework::runtime::application_job_queue_configuration_t{});
        auto held_permit = application_jobs->try_reserve_supply ();
        if (!held_permit)
            return 333;
        permit_wait_session_t permit_session;
        auto connected = permit_session.connected.get_future ();
        auto disconnected = permit_session.disconnected.get_future ();
        auto permit_listeners =
          std::make_shared<zlink::framework::runtime::listener_status_registry_t> ();
        zlink::framework::runtime::stream_host_service_t permit_host (
          std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> (),
          permit_runtime, permit_runtime.snapshots (),
          {{permit_session_name,
            [&permit_session] (zlink::framework::service_provider_t &)
              -> zlink::framework::packet_stream_session_t & { return permit_session; }}},
          permit_options.session_replacement_callback_timeout (), nullptr, {}, permit_listeners,
          application_jobs);
        permit_host.start (transport_provider);
        const auto permit_port = endpoint_port (listener_endpoint (
          *permit_listeners, zlink::framework::listener_kind_t::stream, permit_stream_name));
        auto permit_client = connect_loopback (permit_port);
        if (!permit_client) {
            permit_host.stop ();
            return 334;
        }
        connected.wait ();
        const zlink::framework::detail::stream_header_t permit_header (
          stream_message_kind_t::send, stream_codec_t::raw, stream_header_flags_t::none,
          std::nullopt, "permit-wait", {});
        send_native_bytes (permit_client,
                           make_native_stream_frame (permit_runtime, permit_header,
                                                     zlink::message_t::from ("pending")));
        while (application_jobs->snapshot ().capacity_waiters != 1)
            std::this_thread::yield ();
        if (end == permit_wait_end_t::source_close) {
            permit_host.force_close_sessions (zlink::framework::stream_close_reason_t::server_drain,
                                              "permit wait close");
            disconnected.wait ();
        }
        permit_host.stop ();
        disconnected.wait ();
        close_native_client (permit_client);
        if (permit_session.packets.load () != 0 || permit_session.errors.load () != 0
            || application_jobs->snapshot ().capacity_waiters != 0)
            return 335;
    }

    zlink::framework::zlink_builder_t limited_zlink =
      zlink::framework::test::runtime_failure_builder ();
    zlink::framework::zlink_framework_options_t limited_options (
      transport_services, transport_handlers, transport_serializers, limited_zlink);
    auto limited_stream_options = limited_options.add_stream_node ("limited-stream");
    limited_stream_options.configure_socket ().max_message_size = 128;
    limited_stream_options.bind (loopback_tcp_any_port).register_session ("limited-session");
    limited_options.apply ();
    auto limited_runtime = zlink::framework::detail::stream_runtime_t::from (limited_zlink);
    const auto limited_snapshots = limited_runtime.snapshots ();
    if (limited_snapshots.size () != 1 || limited_snapshots[0].max_message_size != 128) {
        return 46;
    }
    transport_error_session_t limited_session;
    auto limited_listeners =
      std::make_shared<zlink::framework::runtime::listener_status_registry_t> ();
    zlink::framework::runtime::stream_host_service_t limited_host (
      std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> (), limited_runtime,
      limited_snapshots,
      {{"limited-session",
        [&limited_session] (zlink::framework::service_provider_t &)
          -> zlink::framework::packet_stream_session_t & { return limited_session; }}},
      std::chrono::milliseconds{30'000}, nullptr, {}, limited_listeners);
    limited_host.start (transport_provider);
    const auto limited_port = endpoint_port (listener_endpoint (
      *limited_listeners, zlink::framework::listener_kind_t::stream, "limited-stream"));
    auto limited_client = connect_loopback (limited_port);
    if (!limited_client || !limited_session.wait_connected (1)) {
        close_native_client (limited_client);
        limited_host.stop ();
        return 47;
    }
    const zlink::framework::detail::stream_header_t limited_header (
      zlink::framework::detail::stream_message_kind_t::send, zlink::framework::stream_codec_t::raw,
      zlink::framework::detail::stream_header_flags_t::none, std::nullopt, "limited-probe");
    const auto limited_frame = make_native_stream_frame (
      limited_runtime, limited_header, zlink::message_t::from (std::string (512, 'x')));
    send_native_bytes (limited_client, limited_frame);
    const bool limited_disconnected = limited_session.wait_disconnected (1);
    close_native_client (limited_client, true);
    limited_host.stop ();
    if (!limited_disconnected || limited_session.packets () != 0) {
        return 48;
    }

    zlink::framework::zlink_builder_t rejected_zlink =
      zlink::framework::test::runtime_failure_builder ();
    zlink::framework::zlink_framework_options_t rejected_options (
      transport_services, transport_handlers, transport_serializers, rejected_zlink);
    rejected_options.add_stream_node ("rejected-stream")
      .bind (loopback_tcp_any_port)
      .register_session ("rejected-session");
    rejected_options.apply ();
    rejected_connected_session_t rejected_session;
    auto rejected_listeners =
      std::make_shared<zlink::framework::runtime::listener_status_registry_t> ();
    zlink::framework::runtime::stream_host_service_t rejected_host (
      std::make_shared<zlink::framework::runtime::runtime_failure_collector_t> (),
      zlink::framework::detail::stream_runtime_t::from (rejected_zlink),
      zlink::framework::detail::stream_runtime_t::from (rejected_zlink).snapshots (),
      {{"rejected-session",
        [&rejected_session] (zlink::framework::service_provider_t &)
          -> zlink::framework::packet_stream_session_t & { return rejected_session; }}},
      std::chrono::milliseconds{30'000}, nullptr, {}, rejected_listeners);
    rejected_host.start (transport_provider);
    const auto rejected_port = endpoint_port (listener_endpoint (
      *rejected_listeners, zlink::framework::listener_kind_t::stream, "rejected-stream"));
    auto rejected_client = connect_loopback (rejected_port);
    if (!rejected_client || !rejected_session.wait_until_actor_manager_is_detached ()) {
        close_native_client (rejected_client);
        rejected_host.stop ();
        return 37;
    }
    close_native_client (rejected_client);
    rejected_host.stop ();

    return stream_core_shutdown_cases (metric_provider);
}
} // namespace zlink::framework::test::stream_framework
