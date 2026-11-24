/* SPDX-License-Identifier: Elastic-2.0 */
#include "../support/loopback_tcp_endpoint.hpp"
#include "../support/runtime_failure_fixture.hpp"
#include "metric_test_reader.hpp"
#include "metric_test_store.hpp"

#include <zlink/framework.hpp>

#include "runtime/actors/actor_gateway_runtime.hpp"
#include "runtime/diagnostics/listener_status_registry.hpp"
#include "runtime/dispatch/host_capacity_runtime.hpp"
#include "runtime/mesh/mesh_node_runtime.hpp"
#include "runtime/streams/stream_host_service.hpp"
#include "runtime/streams/stream_host_ingress.hpp"
#include "runtime/streams/stream_runtime.hpp"

#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/websocket.hpp>
#include <nlohmann/json.hpp>

#include <atomic>
#include <functional>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <future>
#include <map>
#include <mutex>
#include <set>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
namespace fw = zlink::framework;
namespace rt = fw::runtime;
namespace detail = fw::detail;
namespace asio = boost::asio;
using asio::ip::tcp;
using detail::stream_message_kind_t;
namespace websocket = boost::beast::websocket;
using json = nlohmann::json;
using namespace std::chrono_literals;

void require (bool condition, const std::string &message)
{
    if (!condition)
        throw std::runtime_error (message);
}

template <typename Predicate> void wait_for (Predicate predicate, const std::string &message)
{
    const auto deadline = std::chrono::steady_clock::now () + 3s;
    while (!predicate ()) {
        require (std::chrono::steady_clock::now () < deadline, message);
        std::this_thread::sleep_for (1ms);
    }
}

void configure_admission (fw::stream_node_options_builder_t &options, const json &given)
{
    if (const auto &name = given.at ("authenticationPacketName"); !name.is_null ())
        options.set_authentication_packet_name (name.get<std::string> ());
    if (const auto &limit = given.at ("maxConcurrentAuthentications"); !limit.is_null ())
        options.set_max_concurrent_authentications (limit.get<std::int64_t> ());
    if (given.contains ("authenticationRetryDelayMs"))
        options.set_authentication_retry_delay (
          std::chrono::milliseconds{given.at ("authenticationRetryDelayMs").get<std::int64_t> ()});
}

detail::stream_header_t fixture_header (const json &step)
{
    const std::string kind = step.at ("kind");
    if (kind == "Heartbeat")
        return {detail::stream_message_kind_t::control, fw::stream_codec_t::raw,
                detail::stream_header_flags_t::none, std::nullopt, detail::heartbeat_ping_name};
    if (kind == "Send")
        return {detail::stream_message_kind_t::send, fw::stream_codec_t::raw,
                detail::stream_header_flags_t::none, std::nullopt, step.at ("packetName")};
    require (kind == "Request", "unknown STREAM server fixture packet kind " + kind);
    return {detail::stream_message_kind_t::request, fw::stream_codec_t::raw,
            detail::stream_header_flags_t::has_request_seq,
            std::stoull (step.at ("requestSeq").get<std::string> ()), step.at ("packetName")};
}

struct wire_frame_t
{
    detail::stream_header_t header;
    std::string payload;
};

class wire_peer_t
{
  public:
    explicit wire_peer_t (const std::string &endpoint)
    {
        const auto port = fw::tests::endpoint_port (endpoint);
        socket ().connect ({asio::ip::address_v4::loopback (), port});
        _websocket = endpoint.starts_with ("ws:");
        if (_websocket) {
            _transport.handshake ("127.0.0.1:" + std::to_string (port), "/");
            _transport.binary (true);
        }
    }

    void send (const detail::stream_runtime_t &runtime,
               const json &step,
               std::optional<std::uint16_t> actor_slot = {},
               bool malformed_header = false)
    {
        auto header = fixture_header (step);
        if (actor_slot) {
            header.with_actor_slot (*actor_slot);
            header.with_correlation_id ("admission-probe");
        }
        auto bytes = runtime
                       .encode_frame (
                         header, zlink::message_t::from (step.at ("requestId").get<std::string> ()))
                       .value ();
        if (malformed_header)
            bytes.at (6) = 0;
        if (_websocket)
            _transport.write (asio::buffer (bytes));
        else
            asio::write (socket (), asio::buffer (bytes));
    }

    wire_frame_t read (const detail::stream_runtime_t &runtime)
    {
        if (_websocket) {
            boost::beast::flat_buffer received;
            run_read ([&] (auto completed) { _transport.async_read (received, completed); });
            const auto bytes = received.data ();
            const auto *begin = static_cast<const std::uint8_t *> (bytes.data ());
            require (bytes.size () >= 6, "truncated STREAM WebSocket frame");
            std::vector<std::uint8_t> response (begin, begin + bytes.size ());
            const auto header_size = parse_header_size (response);
            require (6 + header_size <= response.size (), "invalid STREAM WebSocket header length");
            const auto header_end = response.begin () + 6 + header_size;
            auto header = runtime.decode_header ({response.begin () + 6, header_end}).value ();
            return {std::move (header), {header_end, response.end ()}};
        }
        const auto prefix = read_exact (6);
        const auto header_size = parse_header_size (prefix);
        std::size_t payload_size = 0;
        for (std::size_t index = 2; index != prefix.size (); ++index)
            payload_size = (payload_size << 8) | prefix[index];
        require (payload_size <= 64 * 1024, "oversized test response");
        auto header = runtime.decode_header (read_exact (header_size)).value ();
        auto payload = read_exact (payload_size);
        return {std::move (header), {payload.begin (), payload.end ()}};
    }

    void close () noexcept
    {
        boost::system::error_code ignored;
        socket ().close (ignored);
    }

  private:
    static std::size_t parse_header_size (const std::vector<std::uint8_t> &prefix)
    {
        return (static_cast<std::size_t> (prefix[0]) << 8) | prefix[1];
    }

    template <typename Start> void run_read (Start start)
    {
        boost::system::error_code result;
        bool finished = false;
        start ([&] (boost::system::error_code error, std::size_t) {
            result = error;
            finished = true;
        });
        _io.restart ();
        _io.run_for (3s);
        if (!finished) {
            close ();
            _io.restart ();
            _io.run ();
            throw std::runtime_error ("timed out waiting for STREAM response");
        }
        if (result)
            throw boost::system::system_error (result);
    }

    std::vector<std::uint8_t> read_exact (std::size_t size)
    {
        std::vector<std::uint8_t> bytes (size);
        run_read (
          [&] (auto completed) { asio::async_read (socket (), asio::buffer (bytes), completed); });
        return bytes;
    }

    tcp::socket &socket () { return _transport.next_layer (); }

    asio::io_context _io;
    websocket::stream<tcp::socket> _transport{_io};
    bool _websocket = false;
};

class wire_session_t final : public fw::packet_stream_session_t
{
  public:
    fw::task_t<void> on_connected (fw::stream_t &) override
    {
        ++connected;
        co_return;
    }
    fw::task_t<void> on_disconnected (fw::stream_t &) override { co_return; }
    fw::task_t<void> on_error (fw::stream_t &, const fw::stream_error_t &) override { co_return; }

    fw::task_t<void> on_packet (fw::stream_t &stream,
                                const fw::session_message_context_t &,
                                const fw::message_t &payload) override
    {
        ++calls;
        const auto id = detail::message_to_raw (payload, _serializers).to_string ();
        if (id == "a")
            co_await release_a.task ();
        if (id == "b") {
            ++actor_lookups;
            stream.actors ().get_or_create ("login-admission", "login-b").value ();
            ++store_reads;
            fw::store_key_t key{"login-b"};
            const auto read = co_await store.read (std::move (key));
            ++store_writes;
            fw::store_write_request_t write;
            write.mutations.emplace_back (fw::store_put_t{{"login-b"}, {std::byte{1}}, {}});
            const auto written = co_await store.write (std::move (write));
            require (std::holds_alternative<fw::store_missing_t> (read)
                       && std::holds_alternative<fw::store_write_applied_t> (written),
                     "admitted handler did not perform Location Store work");
        }
        if (id == "cancel" || id == "shutdown") {
            co_await (id == "cancel" ? release_cancel.task () : release_shutdown.task ());
            ++cancelled_callbacks_exited;
            co_return;
        }
        co_await stream.reply_packet (payload).async ();
    }

    void unblock ()
    {
        for (auto *source : {&release_a, &release_cancel, &release_shutdown})
            source->complete (fw::result_t<void>::success ());
    }

    std::atomic<int> connected{0};
    std::atomic<int> calls{0};
    std::atomic<int> actor_lookups{0};
    std::atomic<int> store_reads{0};
    std::atomic<int> store_writes{0};
    std::atomic<int> cancelled_callbacks_exited{0};
    fw::task_completion_source_t<void> release_a;
    fw::task_completion_source_t<void> release_cancel;
    fw::task_completion_source_t<void> release_shutdown;
    metric_test::store_t store;

  private:
    fw::serializer_registry_t _serializers;
};

void verify_rejection (const wire_frame_t &frame, const json &expected)
{
    require (frame.header.kind () == detail::stream_message_kind_t::error,
             "saturated authentication entered the handler and returned a response");
    require (!frame.header.actor_slot (), "admission Error inherited the rejected Actor slot");
    require (frame.header.codec () == fw::stream_codec_t::json, "admission Error is not JSON");
    require (frame.header.packet_name () == expected.at ("packetName").get<std::string> (),
             "admission Error has a packet name");
    require (frame.header.request_seq ()
               == std::stoull (expected.at ("requestSeq").get<std::string> ()),
             "admission Error did not preserve the request sequence");
    require (json::parse (frame.payload) == expected.at ("body"),
             "unexpected admission Error body");
    for (const auto &[key, value] : expected.at ("metadata").items ())
        require (frame.header.metadata (key) == value.get<std::string> (),
                 "admission Error did not carry " + key);
}

constexpr auto in_flight_metric = "zlink.host.authentication_admission.in_flight";
constexpr auto rejection_metric = "zlink.host.authentication_admission.rejections";

void check_metrics (metric_test::provider_t &metrics, int in_flight, int rejections)
{
    bool found_gauge = false;
    bool found_counter = false;
    for (const auto &metric : metrics.collect ()) {
        const auto &descriptor = metric.instrument_descriptor;
        if (descriptor.name_ != in_flight_metric && descriptor.name_ != rejection_metric)
            continue;
        require (descriptor.unit_ == "{request}", "wrong admission metric unit");
        require (metric.point_data_attr_.size () == 1,
                 "admission metric must have exactly one node point");
        const auto &point = metric.point_data_attr_.front ();
        require (point.attributes.empty (), "admission metrics have unexpected dimensions");
        if (descriptor.name_ == in_flight_metric) {
            const auto &gauge =
              opentelemetry::nostd::get<metric_test::sdk::LastValuePointData> (point.point_data);
            require (opentelemetry::nostd::get<double> (gauge.value_) == in_flight,
                     "gauge did not observe the admission count");
            found_gauge = true;
        } else {
            const auto &counter =
              opentelemetry::nostd::get<metric_test::sdk::SumPointData> (point.point_data);
            require (counter.is_monotonic_, "admission rejection counter is not monotonic");
            require (opentelemetry::nostd::get<std::int64_t> (counter.value_) == rejections,
                     "admission rejection counter does not match rejections");
            found_counter = true;
        }
    }
    require (found_gauge, "admission gauge was not registered");
    require (found_counter, "admission counter was not registered");
}

enum class wire_transport_t
{
    tcp,
    websocket,
    core
};

void run_wire_rejection (const json &scenario, wire_transport_t transport)
{
    metric_test::provider_t metrics;
    fw::service_collection_t services;
    fw::handler_registry_t handlers;
    fw::serializer_registry_t serializers;
    auto builder = fw::test::runtime_failure_builder ();
    auto context = std::make_shared<zlink::context_t> ();
    if (transport == wire_transport_t::core)
        detail::zlink_builder_access_t::bind_shared_core_context (builder, context);
    services.add_singleton<detail::actor_gateway_runtime_t> ();
    services.add_factory<fw::session_actor_manager_t> (
      [] (fw::service_provider_t &provider) {
          return std::make_unique<fw::session_actor_manager_t> (
            provider.get_required<detail::actor_gateway_runtime_t> ().manager ());
      },
      fw::service_lifetime_t::scoped);
    fw::zlink_framework_options_t options (services, handlers, serializers, builder);
    if (transport == wire_transport_t::core) {
        options.add_route_mesh ("login-admission-mesh")
          .set_routing_id (zlink::routing_id_t::from ("login-admission-node"))
          .listen (fw::tests::loopback_tcp_any_port);
    }
    const auto first_name = "login-admission-first";
    const auto second_name = "login-admission-second";
    const auto first_session = "login-admission-first-session";
    const auto second_session = "login-admission-second-session";
    auto first_registration = options.add_stream_node (first_name);
    first_registration.bind (fw::tests::loopback_tcp_any_port)
      .set_heartbeat (10s, 30s)
      .register_session (first_session);
    configure_admission (first_registration, scenario.at ("given"));
    auto second_registration = options.add_stream_node (second_name);
    second_registration
      .bind (transport == wire_transport_t::websocket ? "ws://127.0.0.1:*"
                                                      : fw::tests::loopback_tcp_any_port)
      .set_heartbeat (10s, 30s)
      .register_session (second_session);
    configure_admission (second_registration, scenario.at ("given"));
    options.apply ();
    detail::bind_stream_serializers (builder, serializers);
    auto runtime = detail::stream_runtime_t::from (builder);
    auto provider = services.build_provider ();
    std::atomic<int> actor_creates = 0;
    auto &actor_gateway = provider.get_required<detail::actor_gateway_runtime_t> ();
    actor_gateway.on_create ([&] (const std::string &type, const std::string &id, const auto &) {
        ++actor_creates;
        return fw::result_t<fw::actor_ref_t>::success (detail::actor_ref_access_t::make (
          fw::node_rid_t::from_string ("login-admission-node"), type, id, 1));
    });
    std::shared_ptr<detail::mesh_node_runtime_t> mesh;
    if (transport == wire_transport_t::core) {
        for (const auto &registration : detail::mesh_node_runtime_t::registrations (builder))
            registration->core_context =
              detail::zlink_builder_access_t::shared_core_context (builder);
        mesh = detail::mesh_node_runtime_t::from (builder, "login-admission-mesh");
        require (static_cast<bool> (mesh), "Core STREAM mesh was not registered");
        mesh->bind_serializers (serializers);
        mesh->start ().result ().value ();
    }
    auto listeners = std::make_shared<rt::listener_status_registry_t> ();
    wire_session_t session;
    auto jobs =
      std::make_shared<rt::application_job_queue_t> (rt::application_job_queue_configuration_t{});
    auto monitoring = fw::test::runtime_failure_monitoring ();
    auto capacity = std::make_shared<rt::host_capacity_runtime_t> (
      context, jobs, std::nullopt, std::nullopt, fw::core_hwm_profile_t::balanced, monitoring);
    const auto factory = [&] (fw::service_provider_t &) -> fw::packet_stream_session_t & {
        return session;
    };
    rt::stream_host_service_t host (
      std::make_shared<rt::runtime_failure_collector_t> (), runtime, runtime.snapshots (),
      {{first_session, factory}, {second_session, factory}},
      options.session_replacement_callback_timeout (), mesh, {}, listeners, jobs, capacity);
    host.bind_monitoring (monitoring);
    struct unblock_t
    {
        wire_session_t &session;
        ~unblock_t () { session.unblock (); }
    } unblock{session};
    host.start (provider).result ().value ();
    wire_peer_t first (
      fw::tests::listener_endpoint (*listeners, fw::listener_kind_t::stream, first_name));
    wait_for ([&] { return session.connected == 1; }, "first STREAM session did not connect");
    wire_peer_t second (
      fw::tests::listener_endpoint (*listeners, fw::listener_kind_t::stream, second_name));
    wait_for ([&] { return session.connected == 2; }, "two STREAM sessions did not connect");
    const auto no_admissions = [&] {
        for (const auto &metric : metrics.collect ()) {
            if (metric.instrument_descriptor.name_ == in_flight_metric) {
                const auto &value =
                  opentelemetry::nostd::get<metric_test::sdk::LastValuePointData> (
                    metric.point_data_attr_.front ().point_data);
                return opentelemetry::nostd::get<double> (value.value_) == 0;
            }
        }
        return false;
    };
    const auto &steps = scenario.at ("steps");
    {
        // A malformed header closes the reader if it is decoded. Holding
        // the only permit must make the reader wait for supply first.
        auto permit = jobs->try_reserve_supply ();
        require (permit.has_value (), "unable to reserve the only ingress permit");
        wire_peer_t invalid (
          fw::tests::listener_endpoint (*listeners, fw::listener_kind_t::stream, second_name));
        wait_for ([&] { return session.connected == 3; }, "probe session did not connect");
        invalid.send (runtime, steps.at (0), {}, true);
        wait_for ([&] { return jobs->snapshot ().capacity_waiters == 1; },
                  "STREAM decoded a malformed header before reserving its ingress permit");
        permit.reset ();
        wait_for ([&] { return jobs->snapshot ().permits_in_use == 0; },
                  "malformed frame did not return its ingress permit");
    }
    first.send (runtime, steps.at (0));
    wait_for ([&] { return session.calls == 1; }, "first authentication did not enter its handler");
    check_metrics (metrics, 1, 0);
    second.send (runtime, steps.at (2), 1);
    const auto &rejected = steps.at (2).at ("expect");
    const auto rejection = second.read (runtime);
    verify_rejection (rejection, rejected.at ("error"));
    require (rejection.header.correlation_id () == "admission-probe",
             "admission Error did not preserve the original correlation ID");
    require (session.calls == rejected.at ("handlerCalls").get<int> (),
             "rejected authentication entered the handler");
    const auto after_rejection = jobs->snapshot ();
    require (after_rejection.permits_in_use == 0 && after_rejection.queued_application_jobs == 0,
             "wire rejection retained an ingress permit or application job");
    require (session.actor_lookups == 0 && actor_creates == rejected.at ("actorCreates").get<int> ()
               && session.store_reads == 0
               && session.store_writes == rejected.at ("storeWrites").get<int> (),
             "rejected authentication reached Actor or Location Store work");
    check_metrics (metrics, 1, 1);
    if (transport == wire_transport_t::tcp) {
        const auto epoch = capacity->snapshot ().measurement_epoch;
        capacity->reset_metrics ();
        require (capacity->snapshot ().measurement_epoch > epoch,
                 "capacity reset did not advance the measurement epoch");
        check_metrics (metrics, 1, 0);
        second.send (runtime, steps.at (2), 1);
        verify_rejection (second.read (runtime), rejected.at ("error"));
        check_metrics (metrics, 1, 1);
        require (session.calls == 1, "metrics reset changed the admission limit");
    }
    session.release_a.complete (fw::result_t<void>::success ());
    require (first.read (runtime).header.kind () == stream_message_kind_t::response,
             "admitted authentication did not reply");
    wait_for (no_admissions, "first reply did not settle the authentication lease");
    second.send (runtime, steps.at (2));
    require (second.read (runtime).header.kind () == stream_message_kind_t::response,
             "rejected peer did not remain connected");
    require (session.actor_lookups == 1 && actor_creates == 1 && session.store_reads == 1
               && session.store_writes == 1,
             "admitted authentication did not exercise the Actor and Location Store probes");
    wait_for (no_admissions, "second reply did not settle the authentication lease");
    check_metrics (metrics, 0, 1);

    auto pending = steps.at (0);
    pending["requestId"] = "cancel";
    first.send (runtime, pending);
    wait_for ([&] { return session.calls == 3; }, "cancellation handler did not enter");
    auto unrelated_permit = jobs->try_reserve_supply ();
    require (unrelated_permit.has_value (), "unable to reserve unrelated application capacity");
    first.close ();
    wait_for (no_admissions, "peer close waited for an application permit");
    require (jobs->snapshot ().permits_in_use == 1,
             "peer close changed unrelated application capacity");
    require (session.cancelled_callbacks_exited == 0, "peer close test unblocked its callback");
    unrelated_permit.reset ();
    session.release_cancel.complete (fw::result_t<void>::success ());
    wait_for ([&] { return session.cancelled_callbacks_exited == 1; },
              "cancelled callback did not exit");

    pending["requestId"] = "shutdown";
    second.send (runtime, pending);
    wait_for ([&] { return session.calls == 4; }, "shutdown handler did not enter");
    check_metrics (metrics, 1, 1);
    auto stopping = std::async (std::launch::async, [&] { host.stop (); });
    struct stop_guard_t
    {
        fw::task_completion_source_t<void> &release;
        std::future<void> &stopping;
        ~stop_guard_t ()
        {
            release.complete (fw::result_t<void>::success ());
            if (stopping.valid ())
                stopping.wait ();
        }
    } stop_guard{session.release_shutdown, stopping};
    wait_for (no_admissions, "host shutdown did not return the active authentication lease");
    require (session.cancelled_callbacks_exited == 1, "shutdown test unblocked its callback");
    session.release_shutdown.complete (fw::result_t<void>::success ());
    require (stopping.wait_for (3s) == std::future_status::ready,
             "host did not stop after authentication callback exit");
    stopping.get ();
    require (session.cancelled_callbacks_exited == 2, "shutdown callback did not exit");
    check_metrics (metrics, 0, 1);
    if (mesh)
        mesh->stop ();
}

detail::stream_runtime_t
build_fixture_runtime (const json &given,
                       const json *second_registration = nullptr,
                       fw::serializer_registry_t *stream_serializers = nullptr)
{
    fw::service_collection_t services;
    fw::handler_registry_t handlers;
    fw::serializer_registry_t serializers;
    fw::zlink_builder_t builder;
    fw::zlink_framework_options_t options (services, handlers, serializers, builder);
    auto first = options.add_stream_node ("first");
    first.bind (fw::tests::loopback_tcp_any_port).register_session ("first");
    configure_admission (first, given);
    if (second_registration) {
        auto second = options.add_stream_node ("second");
        second.bind (fw::tests::loopback_tcp_any_port).register_session ("second");
        configure_admission (second, *second_registration);
    }
    options.apply ();
    if (stream_serializers)
        detail::bind_stream_serializers (builder, *stream_serializers);
    return detail::stream_runtime_t::from (builder);
}

class fixture_session_t final : public fw::packet_stream_session_t
{
  public:
    fixture_session_t (detail::stream_runtime_t &runtime, const std::string &reason) :
        stream (runtime.open_session ("first")), _reason (reason)
    {
    }

    fw::task_t<void> on_connected (fw::stream_t &) override
    {
        ++connected_calls;
        co_await allow_connected.task ();
    }

    fw::task_t<void> on_disconnected (fw::stream_t &) override { co_return; }
    fw::task_t<void> on_error (fw::stream_t &, const fw::stream_error_t &) override { co_return; }

    fw::task_t<void> on_packet (fw::stream_t &current,
                                const fw::session_message_context_t &context,
                                const fw::message_t &payload) override
    {
        ++calls;
        co_await allow_terminal.task ();
        if (_reason == "replySuccess" && context.can_reply) {
            auto first = current.reply_packet (payload);
            auto duplicate = current.reply_packet (payload);
            co_await first.async ();
            try {
                co_await duplicate.async ();
            }
            catch (const fw::framework_exception_t &error) {
                duplicate_rejected.store (error.kind ()
                                          == fw::framework_error_kind_t::protocol_error);
            }
            // Keep the callback active after the winning reply.
            co_await allow_callback_exit.task ();
        } else if (_reason == "replyFailure") {
            co_await fw::task_t<void> (fw::result_t<void>::failure (
              fw::framework_error_kind_t::unavailable, "authentication failed"));
        } else if (_reason == "handlerException") {
            throw std::runtime_error ("authentication handler threw");
        }
    }

    void unblock ()
    {
        allow_connected.complete (fw::result_t<void>::success ());
        allow_terminal.complete (fw::result_t<void>::success ());
        reply_write.complete (fw::result_t<void>::success ());
        allow_callback_exit.complete (fw::result_t<void>::success ());
    }

    void record_frame (const detail::stream_header_t &header,
                       const zlink::message_t &payload,
                       std::shared_ptr<detail::stream_reply_submission_t> submission = {})
    {
        std::lock_guard lock (_frames_mutex);
        _frames.push_back ({header, payload.to_string ()});
        if (submission)
            _pending_reply = std::move (submission);
    }

    std::shared_ptr<detail::stream_reply_submission_t> pending_reply () const
    {
        std::lock_guard lock (_frames_mutex);
        return _pending_reply;
    }

    std::vector<wire_frame_t> frames () const
    {
        std::lock_guard lock (_frames_mutex);
        return _frames;
    }

    fw::stream_t stream;
    std::atomic<int> connected_calls{0};
    std::atomic<int> calls{0};
    std::atomic<int> completions{0};
    std::atomic<bool> duplicate_rejected{false};
    fw::task_completion_source_t<void> allow_connected;
    fw::task_completion_source_t<void> allow_terminal;
    fw::task_completion_source_t<void> reply_write;
    fw::task_completion_source_t<void> allow_callback_exit;
    fw::task_completion_source_t<void> client_completion;

  private:
    std::string _reason;
    mutable std::mutex _frames_mutex;
    std::vector<wire_frame_t> _frames;
    std::shared_ptr<detail::stream_reply_submission_t> _pending_reply;
};

std::string terminal_reason (const json &steps)
{
    for (const auto &step : steps) {
        if (step.at ("action") == "terminal")
            return step.at ("reason").get<std::string> ();
    }
    return {};
}

class fixture_driver_t
{
  public:
    explicit fixture_driver_t (const json &scenario) :
        _runtime (build_fixture_runtime (scenario.at ("given"), nullptr, &_serializers)),
        _ingress (_runtime.snapshots (), scenario.at ("given").at ("effectiveCpuCores")),
        _terminal (terminal_reason (scenario.at ("steps"))),
        _enabled (!scenario.at ("given").at ("authenticationPacketName").is_null ())
    {
        _ingress.connect_metrics (_metrics.api ());
        require (_ingress.limit () == scenario.at ("expectedEffectiveLimit").get<unsigned> (),
                 "effective admission limit differs from fixture");
    }

    ~fixture_driver_t ()
    {
        if (!_enqueue_refusal_signalled)
            _refuse_enqueue.set_value ();
        if (_enqueue_result.valid ())
            _enqueue_result.wait ();
        for (auto &[_, session] : _requests)
            session->unblock ();
        for (auto &[_, session] : _requests)
            _runtime.drain_async_dispatch (session->stream);
    }

    void run_step (const json &step)
    {
        const std::string action = step.at ("action");
        const std::string request = step.at ("requestId");
        if (action == "receive")
            receive (request, step);
        else if (action == "handlerStart")
            enter_handler (*_requests.at (request));
        else if (action == "clientTimeout")
            require (!_requests.at (request)->client_completion.task ().result_for (0ms),
                     "client waited past its deadline");
        else if (action == "await")
            require (_requests.at (request)->completions == 0,
                     "handler completed while its barrier was closed");
        else if (action == "terminal") {
            const auto before = _ingress.in_flight ();
            if (_lease_returns == 0)
                finish (request);
            else
                _runtime.mark_disconnected (_requests.at (request)->stream);
            _lease_returns += before - _ingress.in_flight ();
        } else
            throw std::runtime_error ("unknown server fixture action " + action);
        check (step.at ("expect"));
    }

    void check_response ()
    {
        const auto before = _ingress.in_flight ();
        const auto queued = _jobs.snapshot ().queued_application_jobs;
        auto &session = *_requests.at ("d");
        const auto receive =
          detail::stream_header_t (stream_message_kind_t::response, fw::stream_codec_t::raw,
                                   detail::stream_header_flags_t::has_request_seq, 42, "");
        _ingress
          .publish (_runtime, session, session.stream, receive, zlink::message_t{}, {}, {}, {},
                    [] (const auto &, const auto &) {
                        throw std::runtime_error ("admission rejected a Response");
                    })
          .value ();
        require (_ingress.in_flight () == before
                   && _jobs.snapshot ().queued_application_jobs == queued,
                 "a Response consumed an authentication slot or application permit");
    }

  private:
    void receive (const std::string &request, const json &step)
    {
        auto session = std::make_unique<fixture_session_t> (_runtime, _terminal);
        auto &current = *session;
        require (_requests.emplace (request, std::move (session)).second,
                 "duplicate fixture request identity");
        require (_session_ids.emplace (step.at ("sessionId").get<std::string> ()).second,
                 "fixture reused a server session");
        _runtime.attach_transport_writer (
          current.stream,
          [&current] (const auto &header, const auto &payload, const auto &submission) {
              current.record_frame (header, payload, submission);
              if (submission)
                  return current.reply_write.task ();
              return fw::task_t<void> (fw::result_t<void>::success ());
          });
        _runtime.dispatch_connected_async (current, current.stream).value ();
        wait_for ([&] { return current.connected_calls == 1; }, "session entry did not run");
        const auto header = fixture_header (step);
        if (header.kind () == stream_message_kind_t::control) {
            _runtime.dispatch_control_frame (current.stream, header, zlink::message_t::from (""));
            const auto frames = current.frames ();
            require (frames.size () == 1
                       && frames.front ().header.packet_name () == detail::heartbeat_pong_name,
                     "heartbeat did not take the infrastructure path");
            return;
        }
        const auto before = _jobs.snapshot ().permits_in_use;
        auto permit = _jobs.try_reserve_supply ();
        require (permit.has_value (), "application ingress permit was not available");
        const auto reserved = _jobs.snapshot ().permits_in_use;
        auto write_error = [&current, header] (const auto &result,
                                               detail::stream_metadata_t metadata) {
            current.record_frame (detail::stream_runtime_t::make_terminal_header (
                                    stream_message_kind_t::error, header, std::move (metadata)),
                                  detail::stream_error_payload (result));
        };
        auto completed = [&current, write_error] (const fw::result_t<void> &result) {
            if (!result)
                write_error (result, {});
            ++current.completions;
            current.client_completion.complete (result);
        };
        auto publish = [this, &current, header, permit = std::move (*permit),
                        payload = zlink::message_t::from (request), completed, write_error] (
                         detail::stream_runtime_t::async_dispatch_cancel_t cancelled) mutable {
            return _ingress.publish (_runtime, current, current.stream, header, payload,
                                     std::move (permit), completed, std::move (cancelled),
                                     write_error);
        };
        if (request == "a" && _terminal == "enqueueRejected") {
            const auto gate = _refuse_enqueue.get_future ().share ();
            auto entered = _enqueue_entered.get_future ();
            _enqueue_result = std::async (std::launch::async,
                                          [this, publish = std::move (publish), gate] () mutable {
                                              return publish ([this, gate] {
                                                  _enqueue_entered.set_value ();
                                                  gate.wait ();
                                                  return true;
                                              });
                                          });
            require (entered.wait_for (3s) == std::future_status::ready,
                     "application queue did not reach its enqueue decision");
        } else {
            publish ({}).value ();
        }
        const auto &expect = step.at ("expect");
        const auto frames = current.frames ();
        if (expect.at ("decision") == "rejected") {
            ++_rejections;
            require (frames.size () == 1, "admission did not send exactly one Error");
            verify_rejection (frames.front (), expect.at ("error"));
            require (reserved - before == expect.at ("permitReservations").get<unsigned> (),
                     "rejection did not reserve one ingress permit");
            require (reserved - _jobs.snapshot ().permits_in_use
                       == expect.at ("permitReturns").get<unsigned> (),
                     "rejection did not return the ingress permit");
            require (_jobs.snapshot ().queued_application_jobs
                       == expect.at ("applicationQueuedJobs").get<unsigned> (),
                     "admission rejection published an application job");
            require (current.calls == expect.at ("handlerCallsForRequest").get<int> (),
                     "rejected request entered the handler");
            current.allow_connected.complete (fw::result_t<void>::success ());
            _runtime.drain_async_dispatch (current.stream);
            require (current.calls == 0, "rejected request was queued behind Connected");
        } else {
            require (frames.empty (), "request unexpectedly sent an Error");
        }
    }

    void enter_handler (fixture_session_t &session)
    {
        session.allow_connected.complete (fw::result_t<void>::success ());
        wait_for ([&] { return session.calls == 1; }, "authentication handler did not enter");
    }

    void finish (const std::string &request)
    {
        auto &session = *_requests.at (request);
        if (_terminal == "enqueueRejected") {
            if (_enqueue_result.valid ()) {
                _refuse_enqueue.set_value ();
                _enqueue_refusal_signalled = true;
                require (_enqueue_result.wait_for (3s) == std::future_status::ready,
                         "application queue did not reject publish");
                require (!_enqueue_result.get (), "application queue accepted stopped publish");
            }
        } else if (_terminal == "cancellation" || _terminal == "shutdown") {
            // Runtime shutdown retires every session through this same edge.
            // Keep the node alive here to exercise the fixture's later receive.
            _runtime.mark_disconnected (session.stream);
            require (session.completions == 0,
                     "disconnect test did not overlap with the active callback");
        } else {
            session.allow_terminal.complete (fw::result_t<void>::success ());
            if (_terminal == "replySuccess") {
                wait_for ([&] { return static_cast<bool> (session.pending_reply ()); },
                          "handler did not submit its reply");
                require (_ingress.in_flight () == 1,
                         "reply claim returned the lease before transport acceptance");
                session.pending_reply ()->complete ();
                wait_for ([&] { return _ingress.in_flight () == 0; },
                          "accepted reply held its lease until transport completion");
                require (!session.duplicate_rejected && session.completions == 0,
                         "reply test did not hold the transport write open");
                session.reply_write.complete (fw::result_t<void>::success ());
                wait_for ([&] { return session.duplicate_rejected.load (); },
                          "duplicate reply was not rejected");
                require (session.completions == 0,
                         "the callback must outlive its successful reply");
                const auto frames = session.frames ();
                require (frames.size () == 1
                           && frames.front ().header.kind () == stream_message_kind_t::response,
                         "reply success did not write exactly one response");
            } else {
                wait_for ([&] { return session.completions == 1; },
                          "authentication callback did not terminate");
                const auto frames = session.frames ();
                if (_terminal == "callbackWithoutReply") {
                    require (frames.empty (), "callback without reply produced a frame");
                } else {
                    require (
                      frames.size () == 1
                        && frames.front ().header.kind () == stream_message_kind_t::error
                        && !frames.front ().header.metadata ("$zlink.admission.retryAfterMs"),
                      "handler failures must produce an ordinary Error");
                    const auto code =
                      _terminal == "replyFailure" ? "unavailable" : "internal_failure";
                    require (json::parse (frames.front ().payload).at ("code") == code,
                             "handler failure produced the wrong Error code");
                }
            }
        }
    }

    void check (const json &expect)
    {
        const int in_flight = expect.at ("inFlight");
        const auto observed = _ingress.in_flight ();
        require (observed == static_cast<unsigned> (in_flight),
                 "live admission count differs from fixture, expected " + std::to_string (in_flight)
                   + ", observed " + std::to_string (observed));
        require (_jobs.snapshot ().permits_in_use == expect.at ("heldPermits").get<unsigned> (),
                 "held application permits differ from fixture");
        int handlers = 0;
        for (const auto &[_, session] : _requests)
            handlers += session->calls;
        require (handlers == expect.at ("handlerCalls"), "handler call count differs from fixture");
        if (expect.contains ("leaseReturns"))
            require (_lease_returns == expect.at ("leaseReturns").get<unsigned> (),
                     "lease did not return at exactly one terminal");
        if (_enabled)
            check_metrics (_metrics, in_flight, _rejections);
    }

    fw::serializer_registry_t _serializers;
    detail::stream_runtime_t _runtime;
    metric_test::provider_t _metrics{false};
    rt::stream_host_ingress_t _ingress;
    rt::application_job_queue_t _jobs{{fw::application_job_queue_profile_t::balanced, 8, 2, 8}};
    std::string _terminal;
    std::map<std::string, std::unique_ptr<fixture_session_t>> _requests;
    std::set<std::string> _session_ids;
    std::promise<void> _enqueue_entered;
    std::promise<void> _refuse_enqueue;
    std::future<fw::result_t<void>> _enqueue_result;
    bool _enqueue_refusal_signalled = false;
    int _rejections = 0;
    unsigned _lease_returns = 0;
    bool _enabled;
};

bool startup_rejects (const json &first, const json *second = nullptr)
{
    try {
        auto runtime = build_fixture_runtime (first, second);
        fw::service_collection_t services;
        auto provider = services.build_provider ();
        rt::stream_host_service_t host (std::make_shared<rt::runtime_failure_collector_t> (),
                                        runtime, runtime.snapshots (), {}, 30s);
        host.start (provider).result ().value ();
        return false;
    }
    catch (const fw::framework_exception_t &error) {
        return error.kind () == fw::framework_error_kind_t::protocol_error;
    }
}

void verify_settings ()
{
    const json defaults{{"authenticationPacketName", "Login"},
                        {"maxConcurrentAuthentications", nullptr}};
    auto runtime = build_fixture_runtime (defaults, &defaults);
    auto snapshots = runtime.snapshots ();
    require (snapshots.size () == 2, "expected two STREAM registrations");
    require (snapshots.front ().authentication.retry_delay == 500ms,
             "default authentication retry delay is not 500 ms");
    const json max_options{{"authenticationPacketName", "Login"},
                           {"maxConcurrentAuthentications", INT32_MAX},
                           {"authenticationRetryDelayMs", INT32_MAX}};
    const auto max_runtime = build_fixture_runtime (max_options);
    const rt::stream_host_ingress_t maximum (max_runtime.snapshots (), 2);
    require (maximum.limit () == INT32_MAX, "maximum admission limit was rejected");
    const rt::stream_host_ingress_t default_ingress (snapshots, 2);
    require (default_ingress.limit () == 16,
             "admission default depends on effective processor count");
    const rt::stream_host_ingress_t saturated_default (snapshots, UINT32_MAX);
    require (saturated_default.limit () == INT32_MAX, "default admission limit must saturate");
    for (const auto *field : {"maxConcurrentAuthentications", "authenticationRetryDelayMs"}) {
        for (const std::int64_t value :
             {std::int64_t{-1}, std::int64_t{0}, std::int64_t{INT32_MAX} + 1}) {
            auto bad = defaults;
            bad[field] = value;
            require (startup_rejects (bad),
                     std::string ("invalid admission setting allowed for ") + field);
            bad["authenticationPacketName"] = nullptr;
            require (startup_rejects (bad),
                     std::string ("invalid setting allowed with admission disabled for ") + field);
        }
    }
    const json conflicts[] = {
      {{"authenticationPacketName", "Different"}, {"maxConcurrentAuthentications", nullptr}},
      {{"authenticationPacketName", "Login"}, {"maxConcurrentAuthentications", 1}},
      {{"authenticationPacketName", "Login"},
       {"maxConcurrentAuthentications", nullptr},
       {"authenticationRetryDelayMs", 501}}};
    for (const auto &conflict : conflicts)
        require (startup_rejects (defaults, &conflict),
                 "node accepted conflicting STREAM admission options");
}
} // namespace

int main ()
{
    std::string scenario_name = "load login admission fixture";
    try {
        const fw::completion_owner_t completion_owner;
        detail::configure_stream_dispatch_executor ();
        std::ifstream file (ZLINK_LOGIN_ADMISSION_CONFORMANCE_PATH);
        require (file.good (), "unable to open login admission fixture");
        const auto fixture = json::parse (file);
        require (fixture.at ("version") == 1, "unexpected login admission fixture version");
        for (const auto &scenario : fixture.at ("serverScenarios")) {
            scenario_name = scenario.at ("name");
            const auto stage = scenario.value ("stage", 1);
            // Stage 2 requires the login queue and ticket protocol.
            if (stage == 2) {
                std::cout << "SKIP " << scenario_name << " (Stage 2)\n";
                continue;
            }
            require (stage == 1, "unknown login admission fixture stage");
            fixture_driver_t driver (scenario);
            for (const auto &step : scenario.at ("steps"))
                driver.run_step (step);
            if (scenario_name == "non-authentication-packets-unaffected")
                driver.check_response ();
            std::cout << "PASS " << scenario_name << '\n';
            if (scenario_name == "over-limit-rejected-before-handler") {
                for (const auto &[name, transport] :
                     {std::pair{"TCP", wire_transport_t::tcp},
                      std::pair{"WebSocket", wire_transport_t::websocket},
                      std::pair{"Core", wire_transport_t::core}}) {
                    scenario_name =
                      std::string (name) + " node admission, cancellation, and shutdown";
                    run_wire_rejection (scenario, transport);
                    std::cout << "PASS " << scenario_name << '\n';
                }
            }
        }
        scenario_name = "admission configuration";
        verify_settings ();
        std::cout << "PASS " << scenario_name << '\n';
        detail::shutdown_stream_dispatch_executor ();
        return 0;
    }
    catch (const std::exception &error) {
        std::cerr << "FAIL " << scenario_name << ' ' << error.what () << '\n';
        detail::shutdown_stream_dispatch_executor ();
        return 1;
    }
}
