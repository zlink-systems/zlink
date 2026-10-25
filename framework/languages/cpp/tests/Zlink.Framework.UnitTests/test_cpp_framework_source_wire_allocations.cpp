/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include <zlink/framework.hpp>
#include <gtest/gtest.h>

#include "../support/allocation_counter.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <future>
#include <memory>
#include <optional>
#include <thread>
#include <utility>
#include <vector>
namespace
{
constexpr char channel_name[] = "source-wire-channel";
}
#if !defined(_WIN32)
#include <dlfcn.h>
#include <service_wire_constants.hpp>
#include <zlink/message/api.h>

namespace
{
namespace wire = zlink::framework::runtime::protocol;
constexpr std::string_view observed_channel_name = channel_name;
// Service wire has a five-byte prefix and a text8 length. Requests add a u64.
constexpr std::size_t service_prefix_size =
  sizeof (wire::magic) + sizeof (wire::wire_major) + sizeof (wire::command) + sizeof (wire::flag);
constexpr std::size_t command_offset = sizeof (wire::magic) + sizeof (wire::wire_major);
constexpr std::size_t channel_send_header_size =
  service_prefix_size + sizeof (std::uint8_t) + observed_channel_name.size ();
constexpr std::size_t channel_request_header_size =
  channel_send_header_size + sizeof (std::uint64_t);
std::atomic_bool observing_channel_header{false};
std::atomic_size_t channel_external_headers{0};

bool is_observed_channel_header (const void *data, std::size_t size)
{
    if (!data || (size != channel_send_header_size && size != channel_request_header_size))
        return false;
    const auto *bytes = static_cast<const std::uint8_t *> (data);
    const bool request = size == channel_request_header_size;
    if (std::memcmp (bytes, wire::magic, sizeof (wire::magic)) != 0
        || bytes[sizeof (wire::magic)] != wire::wire_major || bytes[service_prefix_size - 1] != 0
        || bytes[command_offset]
             != static_cast<std::uint8_t> (request ? wire::command::channelRequest
                                                   : wire::command::channelSend))
        return false;
    const auto name_offset = service_prefix_size + (request ? sizeof (std::uint64_t) : 0);
    if (request
        && std::all_of (bytes + service_prefix_size, bytes + name_offset,
                        [] (std::uint8_t byte) { return byte == 0; }))
        return false;
    return bytes[name_offset] == observed_channel_name.size ()
           && std::memcmp (bytes + name_offset + 1, observed_channel_name.data (),
                           observed_channel_name.size ())
                == 0;
}

template <std::size_t Size> std::array<std::uint8_t, Size> channel_probe_storage ()
{
    std::array<std::uint8_t, Size> bytes{};
    constexpr bool request = Size == channel_request_header_size;
    std::copy (std::begin (wire::magic), std::end (wire::magic), bytes.begin ());
    bytes[sizeof (wire::magic)] = wire::wire_major;
    bytes[command_offset] = static_cast<std::uint8_t> (request ? wire::command::channelRequest
                                                               : wire::command::channelSend);
    constexpr auto name_offset = service_prefix_size + (request ? sizeof (std::uint64_t) : 0);
    if constexpr (request)
        bytes[name_offset - 1] = 1;
    bytes[name_offset] = static_cast<std::uint8_t> (observed_channel_name.size ());
    std::copy (observed_channel_name.begin (), observed_channel_name.end (),
               bytes.begin () + name_offset + 1);
    return bytes;
}

class channel_header_scope_t
{
  public:
    explicit channel_header_scope_t (std::size_t &count) : _count (count)
    {
        channel_external_headers.store (0, std::memory_order_relaxed);
        observing_channel_header.store (true, std::memory_order_release);
    }
    ~channel_header_scope_t ()
    {
        observing_channel_header.store (false, std::memory_order_release);
        _count = channel_external_headers.load (std::memory_order_acquire);
    }

  private:
    std::size_t &_count;
};

struct native_copy_observation_t
{
    const void *payload = nullptr;
    std::size_t size = 0;
    std::size_t serializations = 0;
    std::size_t copies = 0;
    std::size_t external_initializations = 0;
};

thread_local native_copy_observation_t *native_copy_observation = nullptr;

class native_copy_scope_t
{
  public:
    explicit native_copy_scope_t (native_copy_observation_t &observation) :
        _previous (std::exchange (native_copy_observation, &observation))
    {
    }
    ~native_copy_scope_t () { native_copy_observation = _previous; }
    native_copy_scope_t (const native_copy_scope_t &) = delete;
    native_copy_scope_t &operator= (const native_copy_scope_t &) = delete;

  private:
    native_copy_observation_t *_previous;
};

using native_copy_fn_t = zlink_config_result_t (*) (zlink_msg_t *, zlink_msg_t *);

native_copy_fn_t native_copy ()
{
    static const auto function =
      reinterpret_cast<native_copy_fn_t> (dlsym (RTLD_NEXT, "zlink_msg_copy"));
    if (!function)
        std::abort ();
    return function;
}

using native_init_data_fn_t =
  zlink_config_result_t (*) (zlink_msg_t *, void *, std::size_t, zlink_free_fn *, void *);

native_init_data_fn_t native_init_data ()
{
    static const auto function =
      reinterpret_cast<native_init_data_fn_t> (dlsym (RTLD_NEXT, "zlink_msg_init_data"));
    if (!function)
        std::abort ();
    return function;
}
} // namespace

// Use the same Core C API interposition as the MeshNode runtime contract test.
extern "C" zlink_config_result_t zlink_msg_copy (zlink_msg_t *destination, zlink_msg_t *source)
{
    auto *observation = native_copy_observation;
    if (observation && observation->payload == zlink_msg_data (source)
        && observation->size == zlink_msg_size (source))
        ++observation->copies;
    return native_copy () (destination, source);
}

extern "C" zlink_config_result_t zlink_msg_init_data (
  zlink_msg_t *message, void *data, std::size_t size, zlink_free_fn *release, void *hint)
{
    if (native_copy_observation)
        ++native_copy_observation->external_initializations;
    if (observing_channel_header.load (std::memory_order_acquire)
        && is_observed_channel_header (data, size))
        channel_external_headers.fetch_add (1, std::memory_order_relaxed);
    return native_init_data () (message, data, size, release, hint);
}
#endif

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
    auto encoded =
      fw::encoded_payload_t::from_bytes (std::span<const std::uint8_t> (*payload.bytes));
#if !defined(_WIN32)
    if (auto *observation = native_copy_observation) {
        observation->payload = encoded.bytes ().data ();
        observation->size = encoded.size ();
        ++observation->serializations;
    }
#endif
    return encoded;
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
    std::string diagnostics;
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
            events[count++] = events[index];
    log.count = count;
    return {count, measurement.largest, log.overflow,
            count == 0 ? std::string{} : fw::tests::describe_allocations (log)};
}

#if !defined(_WIN32)
TEST (SourceWireAllocations, NativeCopyProbeObservesOnlyTheWatchedCallerStorage)
{
    auto payload = zlink::message_t::allocate (64u * 1024u);
    ASSERT_TRUE (payload.valid ());
    native_copy_observation_t observation;
    native_copy_scope_t capture (observation);
    observation.payload = payload.data ();
    observation.size = payload.size ();
    {
        auto unrelated = zlink::message_t::allocate (payload.size ());
        ASSERT_TRUE (unrelated.valid ());
        auto copy = unrelated.copy ();
        ASSERT_TRUE (copy.valid ());
    }
    EXPECT_EQ (observation.copies, 0u);
    {
        auto copy = payload.copy ();
        ASSERT_TRUE (copy.valid ());
        EXPECT_EQ (copy.data (), payload.data ());
    }
    // A disabled or unbound interposer must fail this positive/negative probe.
    EXPECT_EQ (observation.copies, 1u);
    std::thread other ([&payload] { auto copy = payload.copy (); });
    other.join ();
    EXPECT_EQ (observation.copies, 1u);
}

TEST (SourceWireAllocations, ExternalOwnerProbeObservesOnlyTheCaller)
{
    std::array<std::uint8_t, 64> bytes{};
    native_copy_observation_t observation;
    native_copy_scope_t capture (observation);
    auto external = zlink::advanced::external_message_t::from (
      std::span<std::uint8_t> (bytes), [] (void *, void *) {}, nullptr);
    ASSERT_TRUE (external.valid ());
    EXPECT_EQ (observation.external_initializations, 1u);
    std::thread other ([] {
        std::array<std::uint8_t, 64> other_bytes{};
        auto other_external = zlink::advanced::external_message_t::from (
          std::span<std::uint8_t> (other_bytes), [] (void *, void *) {}, nullptr);
    });
    other.join ();
    EXPECT_EQ (observation.external_initializations, 1u);
}

TEST (SourceWireAllocations, ChannelHeaderProbeObservesMatchingStorageOnAnotherThread)
{
    std::size_t headers = 0;
    {
        channel_header_scope_t capture (headers);
        std::thread other ([] {
            auto bytes = channel_probe_storage<channel_request_header_size> ();
            auto external = zlink::advanced::external_message_t::from (
              std::span<std::uint8_t> (bytes), [] (void *, void *) {}, nullptr);
        });
        other.join ();
        std::array<std::uint8_t, channel_send_header_size> unrelated{};
        auto external = zlink::advanced::external_message_t::from (
          std::span<std::uint8_t> (unrelated), [] (void *, void *) {}, nullptr);
        auto zero_correlation = channel_probe_storage<channel_request_header_size> ();
        std::fill_n (zero_correlation.begin () + service_prefix_size, sizeof (std::uint64_t), 0);
        auto invalid_request = zlink::advanced::external_message_t::from (
          std::span<std::uint8_t> (zero_correlation), [] (void *, void *) {}, nullptr);
        auto other_command = channel_probe_storage<channel_send_header_size> ();
        other_command[command_offset] = static_cast<std::uint8_t> (wire::command::nodeSend);
        auto control = zlink::advanced::external_message_t::from (
          std::span<std::uint8_t> (other_command), [] (void *, void *) {}, nullptr);
    }
    EXPECT_EQ (headers, 1u);
}
#endif

TEST (SourceWireAllocations, PublicRouteSubmissionHasNoPayloadSizedIntermediateCppBuffer)
{
#if !defined(_WIN32)
    // Check binding-internal symbol interception in this public-path test too.
    // A zero counter is meaningful only after an actual copy has been observed.
    {
        auto payload = zlink::message_t::allocate (64u * 1024u);
        ASSERT_TRUE (payload.valid ());
        native_copy_observation_t observation;
        native_copy_scope_t capture (observation);
        observation.payload = payload.data ();
        observation.size = payload.size ();
        auto copy = payload.copy ();
        ASSERT_TRUE (copy.valid ());
        ASSERT_EQ (observation.copies, 1u);
        std::array<std::uint8_t, 64> external_bytes{};
        auto external = zlink::advanced::external_message_t::from (
          std::span<std::uint8_t> (external_bytes), [] (void *, void *) {}, nullptr);
        ASSERT_TRUE (external.valid ());
        ASSERT_EQ (observation.external_initializations, 1u);
    }
    {
        std::size_t headers = 0;
        {
            channel_header_scope_t capture (headers);
            auto bytes = channel_probe_storage<channel_send_header_size> ();
            auto external = zlink::advanced::external_message_t::from (
              std::span<std::uint8_t> (bytes), [] (void *, void *) {}, nullptr);
            ASSERT_TRUE (external.valid ());
        }
        ASSERT_EQ (headers, 1u);
    }
#endif
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
    auto target_mesh = target_config.add_route_mesh (mesh_name);
    target_mesh.set_object_role (fw::object_role_t::none)
      .listen ("tcp://127.0.0.1:*")
      .set_routing_id (target_id)
      .add_route_send_handler<wire_handler_t, wire_payload_t> ()
      .add_route_request_handler<wire_echo_handler_t, wire_payload_t, wire_payload_t> ();
    target_mesh.channel (channel_name)
      .server ()
      .add_send_handler<wire_handler_t, wire_payload_t> ()
      .add_request_handler<wire_echo_handler_t, wire_payload_t, wire_payload_t> ();
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
    source_mesh.channel (channel_name).client ();
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
    ASSERT_TRUE (source.client->request_to_channel (channel_name, warmup)
                   .timeout (500ms)
                   .async<wire_payload_t> ()
                   .result ());

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
#if !defined(_WIN32)
        native_copy_observation_t send_copies;
#endif
        const auto send_buffers = measure_payload_buffers (payload_size, [&] {
#if !defined(_WIN32)
            native_copy_scope_t capture (send_copies);
#endif
            send.emplace (source.client->send_to_node (mesh_name, target_id, payload).async ());
        });
#if !defined(_WIN32)
        EXPECT_EQ (send_copies.serializations, 1u);
        EXPECT_EQ (send_copies.copies, 0u) << "send payload=" << payload_size;
#endif
        ASSERT_TRUE (send->result ());
        expected_send_bytes += payload_size;
        EXPECT_FALSE (send_buffers.overflow);
        EXPECT_EQ (send_buffers.count, reference.count)
          << "send payload=" << payload_size << " largest=" << send_buffers.largest << '\n'
          << send_buffers.diagnostics;

        std::optional<fw::request_task_t<wire_payload_t>> request;
#if !defined(_WIN32)
        native_copy_observation_t request_copies;
#endif
        const auto request_buffers = measure_payload_buffers (payload_size, [&] {
#if !defined(_WIN32)
            native_copy_scope_t capture (request_copies);
#endif
            request.emplace (source.client->request_to_node (mesh_name, target_id, payload)
                               .timeout (1s)
                               .async<wire_payload_t> ());
        });
#if !defined(_WIN32)
        EXPECT_EQ (request_copies.serializations, 1u);
        EXPECT_EQ (request_copies.copies, 0u) << "request payload=" << payload_size;
#endif
        const auto reply = request->result ();
        ASSERT_TRUE (reply);
        EXPECT_EQ (*reply.value ().bytes, *payload.bytes);
        EXPECT_FALSE (request_buffers.overflow);
        EXPECT_EQ (request_buffers.count, reference.count)
          << "request payload=" << payload_size << " largest=" << request_buffers.largest << '\n'
          << request_buffers.diagnostics;

        std::optional<fw::task_t<void>> channel_send;
        measured_buffers_t channel_send_buffers{};
#if !defined(_WIN32)
        native_copy_observation_t channel_send_owners;
        std::size_t channel_send_headers = 0;
#endif
        {
#if !defined(_WIN32)
            channel_header_scope_t capture_header (channel_send_headers);
#endif
            channel_send_buffers = measure_payload_buffers (payload_size, [&] {
#if !defined(_WIN32)
                native_copy_scope_t capture (channel_send_owners);
#endif
                channel_send.emplace (
                  source.client->send_to_channel (channel_name, payload).async ());
            });
            ASSERT_TRUE (channel_send->result ());
            expected_send_bytes += payload_size;
        }
#if !defined(_WIN32)
        EXPECT_EQ (channel_send_owners.serializations, 1u);
        EXPECT_EQ (channel_send_owners.copies, 0u);
        EXPECT_EQ (channel_send_headers, 0u) << "Channel send payload=" << payload_size;
#endif
        EXPECT_FALSE (channel_send_buffers.overflow);
        EXPECT_EQ (channel_send_buffers.count, reference.count)
          << "Channel send payload=" << payload_size << '\n'
          << channel_send_buffers.diagnostics;

        std::optional<fw::request_task_t<wire_payload_t>> channel_request;
        measured_buffers_t channel_request_buffers{};
#if !defined(_WIN32)
        native_copy_observation_t channel_request_owners;
        std::size_t channel_request_headers = 0;
#endif
        {
#if !defined(_WIN32)
            channel_header_scope_t capture_header (channel_request_headers);
#endif
            channel_request_buffers = measure_payload_buffers (payload_size, [&] {
#if !defined(_WIN32)
                native_copy_scope_t capture (channel_request_owners);
#endif
                channel_request.emplace (source.client->request_to_channel (channel_name, payload)
                                           .timeout (1s)
                                           .async<wire_payload_t> ());
            });
            const auto channel_reply = channel_request->result ();
            ASSERT_TRUE (channel_reply);
            EXPECT_EQ (*channel_reply.value ().bytes, *payload.bytes);
        }
#if !defined(_WIN32)
        EXPECT_EQ (channel_request_owners.serializations, 1u);
        EXPECT_EQ (channel_request_owners.copies, 0u);
        EXPECT_EQ (channel_request_headers, 0u) << "Channel request payload=" << payload_size;
#endif
        EXPECT_FALSE (channel_request_buffers.overflow);
        EXPECT_EQ (channel_request_buffers.count, reference.count)
          << "Channel request payload=" << payload_size << '\n'
          << channel_request_buffers.diagnostics;
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
