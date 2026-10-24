/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include <zlink/framework.hpp>
#include <gtest/gtest.h>

#include "../support/allocation_counter.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

namespace
{
namespace fw = zlink::framework;
using namespace std::chrono_literals;

// This payload uses a codec extension so the reference can distinguish the
// required native serialization allocation from an intermediate C++ byte buffer.
struct wire_payload_t
{
    static constexpr const char *packet_name = "source-wire-allocation";
    std::shared_ptr<const std::vector<std::uint8_t>> bytes;
};

fw::encoded_payload_t serialize_wire_payload (const wire_payload_t &payload)
{
    return fw::encoded_payload_t::from_bytes (std::span<const std::uint8_t> (*payload.bytes));
}

wire_payload_t deserialize_wire_payload (const fw::encoded_payload_t &payload)
{
    const auto bytes = payload.bytes ();
    auto result = std::make_shared<std::vector<std::uint8_t>> (bytes.size ());
    std::transform (bytes.begin (), bytes.end (), result->begin (),
                    [] (std::byte value) { return std::to_integer<std::uint8_t> (value); });
    return {std::move (result)};
}

struct wire_codec_t
{
    void register_framework_codecs (fw::codec_registration_context_t &codecs) const
    {
        codecs.add_serializer<wire_payload_t> (serialize_wire_payload, deserialize_wire_payload,
                                               "application/x-source-wire-allocation");
    }
};

struct wire_handler_t
{
    std::atomic_size_t sent_bytes{0};
    void handle (const wire_payload_t &payload) { sent_bytes.fetch_add (payload.bytes->size ()); }
};

struct wire_echo_handler_t
{
    wire_payload_t handle (const wire_payload_t &payload) { return payload; }
};

class capture_source_t final : public fw::hosted_service_t
{
  public:
    capture_source_t (fw::route_client_t *&client,
                      fw::framework_runtime_t *&runtime,
                      std::promise<void> &started) :
        _client (client), _runtime (runtime), _started (started)
    {
    }
    fw::task_t<void> start (fw::service_provider_t &services) override
    {
        _client = &services.get_required<fw::route_client_t> ();
        _runtime = &services.get_required<fw::framework_runtime_t> ();
        _started.set_value ();
        co_return;
    }
    void stop () noexcept override {}

  private:
    fw::route_client_t *&_client;
    fw::framework_runtime_t *&_runtime;
    std::promise<void> &_started;
};

class public_mesh_host_t
{
  public:
    fw::app_t app = fw::app_t::create ();
    fw::route_client_t *client = nullptr;
    fw::framework_runtime_t *runtime = nullptr;

    public_mesh_host_t ()
    {
        app.logging ().disable_record_capture ();
        app.add_hosted_service (std::make_unique<capture_source_t> (client, runtime, _started));
    }
    ~public_mesh_host_t () { stop (); }
    int stop ()
    {
        app.request_stop ();
        if (_thread.joinable ())
            _thread.join ();
        return _exit;
    }
    bool start ()
    {
        auto ready = _started.get_future ();
        const auto deadline = std::chrono::steady_clock::now () + 3s;
        _thread = std::thread ([this] { _exit = app.run (0, nullptr); });
        if (ready.wait_until (deadline) != std::future_status::ready)
            return false;
        while (!app.is_ready () && std::chrono::steady_clock::now () < deadline)
            std::this_thread::sleep_for (1ms);
        return app.is_ready () && client && runtime;
    }

  private:
    std::promise<void> _started;
    std::thread _thread;
    int _exit = -1;
};

struct measured_buffers_t
{
    std::size_t count;
    std::size_t largest;
    bool overflow;
};

template <typename Action>
measured_buffers_t measure_payload_buffers (std::size_t payload_size, Action &&action)
{
    std::array<fw::tests::allocation_event_t, 256> events{};
    fw::tests::allocation_event_log_t log{events.data (), events.size ()};
    fw::tests::allocation_event_scope_t capture (log);
    const auto measurement = fw::tests::measure_allocations (std::forward<Action> (action));
    std::size_t count = 0;
    for (std::size_t index = 0; index < log.count; ++index)
        if (events[index].size >= payload_size)
            ++count;
    return {count, measurement.largest, log.overflow};
}

TEST (SourceWireAllocations, PublicRouteSubmissionHasNoPayloadSizedIntermediateCppBuffer)
{
    constexpr const char *mesh_name = "source-wire-allocation";
    const auto target_id = zlink::routing_id_t::from ("source-wire-target");
    public_mesh_host_t target;
    auto &target_config = target.app.add_zlink_framework ();
    target_config.codecs ().use (wire_codec_t{});
    target_config.handler_coroutine_workers (2);
    auto handler = std::make_unique<wire_handler_t> ();
    auto *received = handler.get ();
    target_config.services ().add_singleton<wire_handler_t> (std::move (handler));
    target_config.services ().add_singleton<wire_echo_handler_t> (
      std::make_unique<wire_echo_handler_t> ());
    target_config.add_route_mesh (mesh_name)
      .set_object_role (fw::object_role_t::none)
      .listen ("tcp://127.0.0.1:*")
      .set_routing_id (target_id)
      .add_route_send_handler<wire_handler_t, wire_payload_t> ()
      .add_route_request_handler<wire_echo_handler_t, wire_payload_t, wire_payload_t> ();
    ASSERT_TRUE (target.start ());
    const auto endpoint =
      target.runtime->listener_status (fw::listener_kind_t::route_mesh, mesh_name).endpoint;
    ASSERT_FALSE (endpoint.empty ());

    public_mesh_host_t source;
    auto &source_config = source.app.add_zlink_framework ();
    source_config.codecs ().use (wire_codec_t{});
    source_config.handler_coroutine_workers (2);
    auto source_mesh = source_config.add_route_mesh (mesh_name);
    source_mesh.set_object_role (fw::object_role_t::none)
      .listen ("tcp://127.0.0.1:*")
      .set_routing_id (zlink::routing_id_t::from ("source-wire-source"));
    source_mesh.peer_connections ().connect (target_id, endpoint);
    ASSERT_TRUE (source.start ());

    auto small_bytes = std::make_shared<const std::vector<std::uint8_t>> (16, 7);
    const wire_payload_t warmup{small_bytes};
    bool ready = false;
    const auto deadline = std::chrono::steady_clock::now () + 3s;
    while (!ready && std::chrono::steady_clock::now () < deadline) {
        auto result = source.client->request_to_node (mesh_name, target_id, warmup)
                        .timeout (500ms)
                        .async<wire_payload_t> ()
                        .result ();
        ready = static_cast<bool> (result);
        if (!ready)
            std::this_thread::sleep_for (10ms);
    }
    ASSERT_TRUE (ready);
    ASSERT_TRUE (source.client->send_to_node (mesh_name, target_id, warmup).async ().result ());

    fw::serializer_registry_t serializers;
    serializers.add<wire_payload_t> (serialize_wire_payload, deserialize_wire_payload,
                                     "application/x-source-wire-allocation");
    const auto &serializer = serializers.get<wire_payload_t> ();
    std::size_t expected_send_bytes = warmup.bytes->size ();
    for (const std::size_t payload_size : {64u * 1024u, 128u * 1024u}) {
        const wire_payload_t payload{
          std::make_shared<const std::vector<std::uint8_t>> (payload_size, 42)};
        const auto reference = measure_payload_buffers (payload_size, [&] {
            auto encoded = serializer.serialize (payload);
            EXPECT_EQ (encoded.size (), payload_size);
        });
        EXPECT_FALSE (reference.overflow);
        ASSERT_EQ (reference.count, 0u);

        std::optional<fw::task_t<void>> send;
        const auto send_buffers = measure_payload_buffers (payload_size, [&] {
            send.emplace (source.client->send_to_node (mesh_name, target_id, payload).async ());
        });
        ASSERT_TRUE (send->result ());
        expected_send_bytes += payload_size;
        EXPECT_FALSE (send_buffers.overflow);
        EXPECT_EQ (send_buffers.count, reference.count)
          << "send payload=" << payload_size << " largest=" << send_buffers.largest;

        std::optional<fw::request_task_t<wire_payload_t>> request;
        const auto request_buffers = measure_payload_buffers (payload_size, [&] {
            request.emplace (source.client->request_to_node (mesh_name, target_id, payload)
                               .timeout (1s)
                               .async<wire_payload_t> ());
        });
        const auto reply = request->result ();
        ASSERT_TRUE (reply);
        EXPECT_EQ (*reply.value ().bytes, *payload.bytes);
        EXPECT_FALSE (request_buffers.overflow);
        EXPECT_EQ (request_buffers.count, reference.count)
          << "request payload=" << payload_size << " largest=" << request_buffers.largest;
    }
    const auto receive_deadline = std::chrono::steady_clock::now () + 2s;
    while (received->sent_bytes.load () != expected_send_bytes
           && std::chrono::steady_clock::now () < receive_deadline)
        std::this_thread::yield ();
    EXPECT_EQ (received->sent_bytes.load (), expected_send_bytes);
    EXPECT_EQ (source.stop (), 0);
    EXPECT_EQ (target.stop (), 0);
}
} // namespace
