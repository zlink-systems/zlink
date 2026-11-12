/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once
#include "../common/bench_common.hpp"
#include <zlink/codecs/protobuf.hpp>
#include <zlink/framework/detail/ablation_ladder.hpp>
#include "runtime/dispatch/application_job_queue.hpp"
#include "runtime/channels/channel_reply_writer.hpp"
#include "runtime/messaging/client_call_codec.hpp"
#include "runtime/protocol/service_wire_codec.hpp"
#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include "runtime/host/runtime_failure_collector.hpp"
#include <zlink.hpp>
#include <array>

namespace zlink_cpp_bench::ladder
{
namespace fw = zlink::framework;
namespace protocol = fw::runtime::protocol;
namespace messaging = fw::runtime::messaging;
using payload_t = zlink::framework::bench::withgrpc::BenchPayload;

inline zlink::message_t message (std::span<const std::uint8_t> bytes)
{
    return zlink::message_t::from (std::as_bytes (bytes));
}
inline zlink::message_t message (std::string_view bytes)
{
    return zlink::message_t::from (std::as_bytes (std::span (bytes)));
}
inline auto serializer ()
{
    // This is the exact serializer selected by the Framework protobuf extension.
    return fw::detail::extension_serializer_traits_t<payload_t>::make_serializer ();
}
inline fw::runtime::mesh::service_node_descriptor_t descriptor (
  const zlink::routing_id_t &rid, std::string endpoint)
{
    return {"bench", rid.to_bytes (), 1, 1, std::move (endpoint), {{"bench", 100}},
            fw::runtime::mesh::service_node_state_t::serving};
}
inline void control (zlink::router_socket_t &socket, const zlink::routing_id_t &rid,
                     const std::vector<std::uint8_t> &bytes)
{
    auto part = message (bytes);
    socket.send (rid).message (part).submit ();
}

// A binding-only peer speaks the real admission/liveness protocol outside the
// measured data path. No mesh owner/operation/handler is constructed here.
inline bool handle_control (zlink::router_socket_t &socket, zlink::received_t &received,
                            const fw::runtime::mesh::service_node_descriptor_t &local,
                            std::atomic<bool> *admitted = nullptr)
{
    if (received.parts ().empty () || received.parts ()[0].size () == 0
        || *reinterpret_cast<const char *> (received.parts ()[0].data ()) == '{')
        return false;
    const auto kind = protocol::decode_header (protocol::message_bytes (received.parts ()[0])).kind;
    if (kind == protocol::command::hello || kind == protocol::command::admit) {
        (void) protocol::decode_route_mesh_admission (
          protocol::message_bytes (received.parts ()[0]), kind, received.routing_id ()->to_bytes ());
        if (kind == protocol::command::hello)
            control (socket, *received.routing_id (),
                     protocol::encode_route_mesh_admission (protocol::command::admit, local));
        if (admitted)
            *admitted = true;
        return true;
    }
    if (kind == protocol::command::update || kind == protocol::command::livenessAck)
        return true;
    if (kind == protocol::command::livenessProbe) {
        const auto probe = protocol::decode_liveness (protocol::message_bytes (received.parts ()[0]));
        control (socket, *received.routing_id (),
                 protocol::encode_liveness (protocol::command::livenessAck, probe.probe_id));
        return true;
    }
    if (kind == protocol::command::reject)
        throw std::runtime_error ("raw mesh peer admission rejected");
    return false;
}


inline fw::runtime::mesh::raw_mesh_node_options_t owner_options (
  const zlink::routing_id_t &rid, std::string endpoint)
{
    fw::runtime::mesh::raw_mesh_node_options_t options;
    options.descriptor = descriptor (rid, std::move (endpoint));
    options.runtime_failures = std::make_shared<fw::runtime::runtime_failure_collector_t> ();
    return options;
}

// L0 uses the existing raw bench's protobuf bytes and envelope. Only the
// measurement stamp changes. L2/L3/L4 have independent real-code entry points.
class encoder_t
{
  public:
    explicit encoder_t (int level) : _level (level), _serializer (serializer ()) {}
    std::array<zlink::message_t, 2> encode (size_t size, uint32_t run, phase_t phase,
                                       uint64_t seq, bool command = true)
    {
        zlink::message_t body;
        // L4 restores the Framework typed codec. L0 still performs all of
        // the raw binding benchmark's payload allocation and protobuf encode.
        payload_t value;
        value.mutable_body ()->assign (size, '\xab');
        stamp_payload (value.mutable_body ()->data (), size, run, phase, seq);
        body = _level >= 4
          ? fw::detail::encoded_payload_to_raw (_serializer.serialize (value))
          : message (encode_bench_payload (value));
        if (_level >= 0 && _level < 2)
            return {message (request_envelope ()), std::move (body)};
        auto header = messaging::client_call_codec_t{}.create_envelope (
          command ? messaging::message_kind_t::command : messaging::message_kind_t::request,
          "bench", "BenchPayload", std::chrono::milliseconds (0), {}, {},
          "application/x-protobuf");
        auto envelope = messaging::envelope_codec_t{}.encode_raw_body_parts (header, std::move (body));
        if (_level >= 3 || _level < 0) {
            auto application = std::move (envelope).take_application_payload ();
            return {command ? protocol::encode_node_send_header_message ()
                            : protocol::encode_node_request_header_message (seq + 1),
                    protocol::encode_application_payload_message (application)};
        }
        auto items = std::move (envelope).take_items ();
        return {std::move (items[0]), std::move (items[1])};
    }
  private:
    int _level;
    decltype (serializer ()) _serializer;
};

inline std::vector<std::span<const std::byte>> application_views (
  std::span<const zlink::message_t> parts)
{
    if (parts.size () != 2)
        throw std::runtime_error ("ladder requires two wire parts");
    if (parts[0].size () && *reinterpret_cast<const char *> (parts[0].data ()) == '{')
        return {parts[0].bytes (), parts[1].bytes ()};
    return protocol::decode_application_part_views (protocol::message_bytes (parts[1]), false);
}

inline std::span<const std::byte> fixed_body (std::span<const std::byte> bytes)
{
    if ((bytes.size () != 1027 && bytes.size () != 4099)
        || bytes[0] != std::byte{0x0a} || bytes[1] != std::byte{0x80}
        || bytes[2] != (bytes.size () == 1027 ? std::byte{0x08} : std::byte{0x20}))
        throw std::runtime_error ("unexpected fixed protobuf body");
    return bytes.subspan (3);
}

inline void verify_wire ()
{
    const auto validate_envelope = [] (int level, std::span<const std::byte> bytes) {
        if (level == 0 || level == 1) {
            // L0/L1 deliberately retain the binding benchmark's legacy header.
            const std::string_view actual (reinterpret_cast<const char *> (bytes.data ()), bytes.size ());
            if (actual != request_envelope ())
                throw std::runtime_error ("raw bench request envelope differs from binding");
        } else {
            messaging::envelope_codec_t{}.decode_header (bytes, false).value ();
        }
    };
    for (int level : {-1, 0, 1, 2, 3, 4}) {
        for (size_t size : {1024, 4096}) {
            for (bool command : {true, false}) {
                encoder_t encoder (level);
                auto parts = encoder.encode (size, 123, phase_active, 456, command);
                auto views = application_views (parts);
                validate_envelope (level, views[0]);
                auto payload = serializer ().deserialize (fw::detail::encoded_payload_from_raw (views[1]));
                decoded_header_t header;
                if (payload.body ().size () != size
                    || !decode_payload (payload.body ().data (), size, &header)
                    || header.run_id != 123 || header.phase != phase_active || header.seq != 456)
                    throw std::runtime_error ("ladder wire roundtrip failed");
                if (level >= 3 || level < 0) {
                    auto broken = parts[1].copy ();
                    broken.data ()[0] = std::byte{0xff};
                    bool rejected = false;
                    try { (void) protocol::decode_application_part_views (protocol::message_bytes (broken), false); }
                    catch (const protocol::service_wire_error_t &) { rejected = true; }
                    if (!rejected)
                        throw std::runtime_error ("corrupted service framing was accepted");
                }
            }
        }
        std::printf ("wire L%d: 1K/4K send/request roundtrip valid\n", level);
    }
    const auto invalid = message ("{broken");
    for (int level : {0, 2}) {
        bool rejected = false;
        try { validate_envelope (level, invalid.bytes ()); }
        catch (const std::exception &) { rejected = true; }
        if (!rejected)
            throw std::runtime_error ("malformed raw/framework envelope was accepted");
    }
    bool rejected = false;
    try { (void) fixed_body (invalid.bytes ()); }
    catch (const std::runtime_error &) { rejected = true; }
    if (!rejected)
        throw std::runtime_error ("invalid fixed protobuf view was accepted");
    std::puts ("negative checks: malformed envelope, corrupt framing, invalid protobuf view rejected");
}

template <typename Metrics> struct fixed_handler_t
{
    Metrics &metrics;
    int level;
    decltype (serializer ()) codec = serializer ();
    payload_t handle (zlink::received_t &received)
    {
        const auto &parts = received.parts ();
        if (parts.size () != 2 || parts[0].size () == 0)
            throw std::runtime_error ("invalid ladder wire part shape");
        std::array<std::span<const std::byte>, 2> legacy{parts.at (0).bytes (), parts.at (1).bytes ()};
        std::vector<std::span<const std::byte>> packed;
        std::span<const std::span<const std::byte>> views = legacy;
        if (*reinterpret_cast<const char *> (parts[0].data ()) != '{') {
            packed = application_views (parts);
            views = packed;
        }
        if (views.size () != 2)
            throw std::runtime_error ("ladder application must have two parts");
        if (level >= 2)
            messaging::envelope_codec_t{}.decode_header (views[0], false).value ();
        payload_t payload;
        if (level >= 4) {
            payload = codec.deserialize (fw::detail::encoded_payload_from_raw (views[1]));
        } else if (!payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ()))) {
            throw std::runtime_error ("invalid raw protobuf payload");
        }
        metrics.record (payload.body ().data (), payload.body ().size ());
        return payload;
    }
};

// Raw cross peer: only its admission/liveness and canonical wire adaptation are
// imitated. It deliberately constructs no Framework owner or handler runtime.
inline void raw_reply (zlink::received_t &received, const payload_t &payload)
{
    // #4350 correction 2026-11-12 09:00 UTC: same fresh response work as
    // zlink/bench_zlink_cpp_server.cpp request_loop; never echo request parts.
    const char *envelope = response_envelope ();
    zlink::message_t header = zlink::message_t::from (std::as_bytes (
      std::span<const char> (envelope, std::strlen (envelope))));
    payload_t reply;
    reply.set_body (payload.body ());
    const std::string encoded = encode_bench_payload (reply);
    zlink::message_t echo = zlink::message_t::from (
      std::as_bytes (std::span<const char> (encoded.data (), encoded.size ())));
    if (*reinterpret_cast<const char *> (received.parts ()[0].data ()) == '{') {
        received.reply ().message (header).message (echo).submit ();
        return;
    }
    // Canonical peers retain their existing wire contract. This compatibility
    // adapter is outside the primary L0 raw wire.
    const auto correlation = protocol::decode_node_request_header (
      protocol::message_bytes (received.parts ()[0]));
    const auto views = application_views (received.parts ());
    const auto request = messaging::envelope_codec_t{}.decode_header (views[0], false).value ();
    fw::detail::channel_reply_writer_t writer;
    auto response = writer.reply_raw_envelope (
      writer.create_reply_header (messaging::message_kind_t::response, "bench", request),
      std::move (echo));
    auto service_header = protocol::encode_reply_header_message (correlation, 0, 0);
    auto service_body = protocol::encode_application_payload_message (
      std::move (response).take_application_payload ());
    received.reply ().message (service_header).message (service_body).submit ();
}

template <typename Metrics>
int direct_server (const std::string &endpoint, Metrics &metrics, std::atomic<bool> &stop,
                   int level, bool raw_mesh)
{
    if (raw_mesh) {
        zlink::context_t context;
        zlink::router_socket_t socket (context);
        const auto rid = zlink::routing_id_t::from ("bench-server");
        socket.set_routing_id (rid);
        socket.bind (endpoint);
        const auto local = descriptor (rid, endpoint);
        zlink::poller_t poller;
        poller.add (socket, zlink::poll_event_flag_t::pollin | zlink::poll_event_flag_t::pollcompletion, 1);
        fixed_handler_t<Metrics> handler{metrics, -1};
        zlink::received_t received;
        while (!stop) {
            zlink::poll_event_t event{};
            if (!poller.wait (&event, 1, std::chrono::milliseconds (50)))
                continue;
            while (socket.recv (received, zlink::recv_flags_t::dontwait) == 0) {
                if (handle_control (socket, received, local))
                    continue;
                auto payload = handler.handle (received);
                if (received.reply_token ())
                    raw_reply (received, payload);
            }
        }
        return 0;
    }
    fw::runtime::mesh::raw_mesh_node_owner_t owner (
      owner_options (zlink::routing_id_t::from ("bench-server"), endpoint));
    owner.ablation_start ();
    std::fprintf (stderr, "#4350 target L%d: real raw_mesh_node_owner_t; fixed handler\n", level);
    struct state_t {
        fw::runtime::mesh::raw_mesh_node_owner_t &owner;
        fixed_handler_t<Metrics> handler;
    } state{owner, {metrics, level}};
    owner.ablation_receive (stop, [] (void *opaque, zlink::received_t &received) {
        auto &state = *static_cast<state_t *> (opaque);
        if (handle_control (state.owner.ablation_socket (), received, state.owner.startup_descriptor ()))
            return;
        // L6: one handler, constructed outside the receive loop, called directly.
        auto payload = state.handler.handle (received);
        if (received.reply_token ())
            raw_reply (received, payload);
    }, &state);
    return 0;
}
}
