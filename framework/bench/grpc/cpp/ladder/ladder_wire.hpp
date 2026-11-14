/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once
#include "../common/bench_common.hpp"
#include "../common/binding_wire.hpp"
#include "ladder_payload.hpp"
#include <zlink/codecs/protobuf.hpp>
#include <zlink/framework/detail/ablation_ladder.hpp>
#include "runtime/dispatch/application_job_queue.hpp"
#include "runtime/dispatch/application_job_queue_capacity.hpp"
#include "runtime/dispatch/receive_batch_budget.hpp"
#include "runtime/channels/channel_reply_writer.hpp"
#include "runtime/messaging/client_call_codec.hpp"
#include "runtime/protocol/service_wire_codec.hpp"
#include "runtime/mesh/raw_mesh_node_owner.hpp"
#include "runtime/host/runtime_failure_collector.hpp"
#include <zlink.hpp>
#include <array>
#include <limits>

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
inline bool game_stamp_matches (const game_message_t &value, uint32_t run, phase_t phase,
                                uint64_t seq)
{
    return value.run == run && value.phase == phase && value.seq == static_cast<std::int64_t> (seq)
           && value.sent_ns > 0;
}
inline auto protobuf_serializer ()
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
                            std::atomic<bool> *admitted = nullptr,
                            std::optional<std::uint64_t> *correlation = nullptr)
{
    if (received.parts ().empty () || received.parts ()[0].size () == 0
        || *reinterpret_cast<const char *> (received.parts ()[0].data ()) == '{')
        return false;
    const auto wire = protocol::decode_header (protocol::message_bytes (received.parts ()[0]));
    const auto kind = wire.kind;
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
    if (correlation) {
        if (kind == protocol::command::nodeRequest) {
            if (!received.reply_token ())
                throw protocol::service_wire_error_t ("node request has no binding reply token");
            *correlation = protocol::decode_node_request_header (
              protocol::message_bytes (received.parts ()[0]));
        } else if (kind != protocol::command::nodeSend || wire.flags != 0
                   || received.parts ()[0].size () != protocol::prefix_size) {
            throw protocol::service_wire_error_t ("invalid ladder node send/request header");
        }
    }
    return false;
}


inline fw::runtime::mesh::raw_mesh_node_options_t owner_options (
  const zlink::routing_id_t &rid, std::string endpoint,
  std::shared_ptr<fw::runtime::application_job_queue_t> application_jobs = {})
{
    fw::runtime::mesh::raw_mesh_node_options_t options;
    options.descriptor = descriptor (rid, std::move (endpoint));
    options.descriptor.state = fw::runtime::mesh::service_node_state_t::preparing;
    options.runtime_failures = std::make_shared<fw::runtime::runtime_failure_collector_t> ();
    options.application_jobs = std::move (application_jobs);
    return options;
}

// L0 uses the existing raw bench's protobuf bytes and envelope. Only the
// measurement stamp changes. L2/L3/L4 have independent real-code entry points.
class encoder_t
{
  public:
    explicit encoder_t (int level, payload_shape_t shape = payload_shape_t::bytes) :
        _level (level), _shape (shape)
    {
        if (level >= 4) {
            if (_shape == payload_shape_t::mixed)
                _type = fw::detail::serializer_registry_access_t::request_plan (game_serializer ());
            else
                _type = fw::detail::serializer_registry_access_t::request_plan (typed_serializer ());
        }
        if (level >= 2 || level < 0) {
            if (!_type)
                _type = std::make_shared<fw::detail::request_type_plan_t> (
                  fw::detail::prepare_request_type ("BenchPayload", "application/x-protobuf"));
            _channel = std::make_shared<fw::detail::prepared_header_fragment_t> (
              fw::detail::prepare_request_channel ("bench"));
        }
    }
    std::array<zlink::message_t, 2> encode (size_t size, uint32_t run, phase_t phase,
                                       uint64_t seq, bool command = true,
                                       std::chrono::milliseconds timeout = std::chrono::milliseconds (0))
    {
        zlink::message_t body;
        // L4 restores the Framework typed codec. L0 still performs all of
        // the raw binding benchmark's payload allocation and protobuf encode.
        if (_level >= 4 && _shape == payload_shape_t::mixed) {
            auto value = make_game_message (size, run, phase, seq);
            const auto selected = game_serializer ();
            _type = fw::detail::serializer_registry_access_t::request_plan (selected);
            body = fw::detail::encoded_payload_to_raw (selected.serialize (value));
        } else if (_level >= 4) {
            auto value = make_typed_payload (size, run, phase, seq);
            const auto selected = typed_serializer ();
            _type = fw::detail::serializer_registry_access_t::request_plan (selected);
            body = fw::detail::encoded_payload_to_raw (selected.serialize (value));
        } else {
            payload_t value;
            const auto body_size = _shape == payload_shape_t::mixed
              ? game_wire_size (size, run, phase, seq) : size;
            value.mutable_body ()->assign (body_size, '\xab');
            stamp_payload (value.mutable_body ()->data (), body_size, run, phase, seq);
            body = zlink_cpp_bench::encode_bench_payload_message (value);
        }
        if (_level >= 0 && _level < 2)
            return {message (request_envelope ()), std::move (body)};
        messaging::client_call_codec_t codec;
        auto encoded = fw::detail::encoded_payload_from_raw (std::move (body));
        messaging::message_parts_t envelope;
        if (command) {
            // The production caller borrows names and moves the serializer's
            // content type into the header in encode_node_send_parts.
            envelope = codec.encode_node_send_parts (
              _channel_name, _packet_name, _target, {}, _dispatch, _serializers,
              [&] (fw::serializer_registry_t &) {
                  return fw::serialized_payload_t{std::move (encoded), _type->content_type};
              });
        } else {
            // route_client_t::submit_request_reply_message_erased leaves names
            // and content type in the prepared plans, which outlive encoding.
            auto header = codec.create_envelope (
              messaging::message_kind_t::request, {}, {}, timeout, {}, {}, {});
            envelope = (_level >= 3 || _level < 0)
              ? codec.encode_service_payload_parts (std::move (header), std::move (encoded), _type, _channel)
              : codec.encode_payload_parts (std::move (header), std::move (encoded), _type, _channel);
        }
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
    payload_shape_t _shape;
    std::shared_ptr<const fw::detail::request_type_plan_t> _type;
    std::shared_ptr<const fw::detail::prepared_header_fragment_t> _channel;
    const std::string _channel_name = "bench";
    const std::string _packet_name = "BenchPayload";
    const zlink::routing_id_t _target = zlink::routing_id_t::from ("bench-server");
    const fw::dispatch_options_t _dispatch;
    fw::serializer_registry_t _serializers;
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

inline void verify_wire (payload_shape_t shape = payload_shape_t::bytes)
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
    for (int level : {0, 1, 2, 3, 4}) {
        for (size_t size : {1024, 4096}) {
            for (bool command : {true, false}) {
                encoder_t encoder (level, shape);
                auto parts = encoder.encode (size, 123, phase_active, 456, command);
                auto views = application_views (parts);
                validate_envelope (level, views[0]);
                const auto expected_size = shape == payload_shape_t::mixed
                  ? game_payload_plan (size).json_size : size;
                const auto validate_payload = [&] (const auto &payload) {
                    decoded_header_t header;
                    if (payload.body ().size () != expected_size
                        || !decode_payload (payload.body ().data (), expected_size, &header)
                        || header.run_id != 123 || header.phase != phase_active || header.seq != 456)
                        throw std::runtime_error ("ladder wire roundtrip failed");
                };
                if (level >= 4 && shape == payload_shape_t::mixed) {
                    const auto envelope = messaging::envelope_codec_t{}.decode_header (views[0], false).value ();
                    if (envelope.content_type != game_serializer ().content_type ())
                        throw std::runtime_error ("mixed payload did not select default JSON");
                    if (views[1].size () != game_wire_size (size, 123, phase_active, 456))
                        throw std::runtime_error ("mixed JSON body length differs from L3 payload length");
                    const auto decoded = game_serializer ().deserialize (
                      fw::detail::encoded_payload_from_raw (views[1]));
                    if (!game_stamp_matches (decoded, 123, phase_active, 456))
                        throw std::runtime_error ("mixed game payload stamp roundtrip failed");
                } else if (level >= 4) {
                    const auto envelope = messaging::envelope_codec_t{}.decode_header (views[0], false).value ();
                    if (envelope.content_type != typed_serializer ().content_type ())
                        throw std::runtime_error ("typed payload did not select default JSON");
                    validate_payload (typed_serializer ().deserialize (fw::detail::encoded_payload_from_raw (views[1])));
                } else
                    validate_payload (protobuf_serializer ().deserialize (fw::detail::encoded_payload_from_raw (views[1])));
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
    std::puts ("negative checks: malformed raw/framework envelope rejected");
}

template <typename Metrics, typename Payload = payload_t> struct fixed_handler_t
{
    Metrics &metrics;
    int level;
    Payload handle (zlink::received_t &received,
                      std::optional<messaging::envelope_header_t> &header)
    {
        auto &parts = received.parts ();
        if (parts.size () != 2 || parts[0].size () == 0)
            throw std::runtime_error ("invalid ladder wire part shape");
        std::vector<zlink::message_t> unpacked;
        std::span<const zlink::message_t> application = parts;
        if (level >= 3 || level < 0) {
            // Production retained Message slices own the received frame. No
            // application bytes are copied while removing the service wrapper.
            unpacked = protocol::decode_application_parts (std::move (parts[1]), false);
            application = unpacked;
        }
        if (application.size () != 2)
            throw std::runtime_error ("ladder application must have two parts");
        if (level >= 2 || level < 0) {
            auto decoded = messaging::envelope_codec_t{}.decode_header_view (application[0], false);
            header.emplace (std::move (decoded.value ()));
        }
        Payload payload;
        if constexpr (std::is_same_v<Payload, game_message_t>) {
            payload = fw::detail::deserialize_typed_payload (
              game_serializer (), application[1], header->content_type);
        } else if constexpr (std::is_same_v<Payload, typed_payload_t>) {
            payload = fw::detail::deserialize_typed_payload (
              typed_serializer (), application[1], header->content_type);
        } else if (!payload.ParseFromArray (application[1].data (), static_cast<int> (application[1].size ()))) {
            throw std::runtime_error ("invalid raw protobuf payload");
        }
        if constexpr (std::is_same_v<Payload, game_message_t>)
            metrics.record_game (payload.run, payload.phase, payload.seq,
                                 payload.sent_ns - static_cast<std::int64_t> (game_timestamp_offset));
        else
            metrics.record (payload.body ().data (), payload.body ().size ());
        // Same retained-header transfer as mesh_record_dispatcher.cpp:47.
        if (!unpacked.empty ())
            header->retained_header.message = std::move (unpacked[0]);
        return payload;
    }
};

template <typename Payload>
std::array<zlink::message_t, 2> reply_parts (
  const Payload &payload, const std::optional<messaging::envelope_header_t> &request)
{
    // Every level creates a fresh reply body and fresh parts. L4 selects the
    // default typed JSON serializer; L0-L3 retain the raw protobuf encoder.
    Payload reply;
    zlink::message_t echo;
    if constexpr (std::is_same_v<Payload, game_message_t>)
        echo = fw::detail::encoded_payload_to_raw (game_serializer ().serialize (payload));
    else if constexpr (std::is_same_v<Payload, typed_payload_t>) {
        reply.set_body (payload.body ());
        echo = fw::detail::encoded_payload_to_raw (typed_serializer ().serialize (reply));
    } else {
        reply.set_body (payload.body ());
        echo = zlink_cpp_bench::encode_bench_payload_message (reply);
    }
    if (request) {
        fw::detail::channel_reply_writer_t writer;
        auto response = writer.reply_raw_envelope (
          writer.create_reply_header (messaging::message_kind_t::response, "bench", *request),
          std::move (echo));
        auto items = std::move (response).take_items ();
        return {std::move (items[0]), std::move (items[1])};
    }
    return {message (response_envelope ()), std::move (echo)};
}

template <typename Payload>
void raw_reply (zlink::received_t &received, const Payload &payload,
                       const std::optional<messaging::envelope_header_t> &request,
                       const std::optional<std::uint64_t> &correlation)
{
    auto response = reply_parts (payload, request);
    if (!correlation) {
        received.reply ().message (response[0]).message (response[1]).submit ();
        return;
    }
    auto service_header = protocol::encode_reply_header_message (*correlation, 0, 0);
    auto service_body = protocol::encode_application_payload_message (
      protocol::application_payload_t::from_parts (
        messaging::message_parts_t (std::move (response[0]), std::move (response[1])).take_items ()));
    received.reply ().message (service_header).message (service_body).submit ();
}

template <typename Payload = payload_t>
Payload decode_reply (const std::vector<zlink::message_t> &parts, int level,
                                std::uint64_t sequence)
{
    if (parts.size () != 2)
        throw std::runtime_error ("invalid ladder reply part count");
    if (level >= 3 || level < 0) {
        const auto wire = protocol::decode_reply_header (protocol::message_bytes (parts[0]));
        if (parts[0].size () != protocol::reply_header_t::encoded_size
            || wire.correlation != sequence + 1 || wire.terminal_result != 0)
            throw std::runtime_error ("invalid service reply correlation/terminal");
    }
    const std::array<std::span<const std::byte>, 2> legacy{parts[0].bytes (), parts[1].bytes ()};
    std::vector<std::span<const std::byte>> packed;
    std::span<const std::span<const std::byte>> views = legacy;
    if (level >= 3 || level < 0) {
        packed = application_views (parts);
        views = packed;
    }
    if (views.size () != 2)
        throw std::runtime_error ("ladder reply must have two application parts");
    if (level >= 2 || level < 0) {
        auto decoded = messaging::envelope_codec_t{}.decode_header_view (
          messaging::message_parts_t::borrow (views), false);
        const auto &header = decoded.value ();
        if (header.kind == messaging::message_kind_t::error)
            throw messaging::request_failure_mapper_t{}.error_header_exception (
              header.error_code.value_or ("request_failed"),
              header.error_message.value_or ("route request failed"), "route request");
    }
    if constexpr (std::is_same_v<Payload, game_message_t>)
        return game_serializer ().deserialize (fw::detail::encoded_payload_from_raw (views[1]));
    else if constexpr (std::is_same_v<Payload, typed_payload_t>)
        return typed_serializer ().deserialize (fw::detail::encoded_payload_from_raw (views[1]));
    else {
        Payload payload;
        if (!payload.ParseFromArray (views[1].data (), static_cast<int> (views[1].size ())))
            throw std::runtime_error ("invalid raw reply protobuf");
        return payload;
    }
}

// L1-L4 share the production host's persistent supply and receive-turn budget.
// The fixed callback replaces mailbox/dispatch work, which starts at L5_7.
struct receive_permit_evidence_t
{
    std::uint64_t batches = 0;
    std::uint64_t reserved = 0;
    std::uint64_t received = 0;
};

template <typename Handler>
receive_permit_evidence_t receive_reserved (
  fw::runtime::mesh::raw_mesh_node_owner_t &owner,
  std::shared_ptr<fw::runtime::application_job_queue_t> application_jobs,
  std::atomic<bool> &stop, Handler handler)
{
    fw::runtime::application_supply_slot_t supply (
      std::move (application_jobs), [&owner] { owner.signal_activity (); });
    std::optional<zlink::poll_event_flag_t> readiness;
    receive_permit_evidence_t evidence;
    zlink::received_t received;
    while (!stop.load ()) {
        // mesh_node_host_service_t::node_receive_role_t::rotate, in production order.
        supply.ensure_waiter (fw::runtime::dispatch_limits::receive_batch_messages);
        auto budget = supply.take_budget ();
        auto application_permit = budget
          ? std::optional<fw::runtime::application_job_queue_t::permit_t> (budget->take ())
          : std::nullopt;
        auto application_permit_budget = budget
          ? std::move (*budget) : fw::runtime::application_job_queue_t::permit_batch_t{};
        const bool accept_application_receive = static_cast<bool> (application_permit);
        if (accept_application_receive) {
            ++evidence.batches;
            evidence.reserved += 1 + application_permit_budget.size ();
        }
        fw::runtime::receive_batch_budget_t receive_budget;
        const auto turn = owner.begin_receive_turn (readiness);
        if (!turn)
            throw std::runtime_error ("ladder receive turn failed");
        const bool receive_ready = accept_application_receive
          && (static_cast<short> (*turn) & static_cast<short> (zlink::poll_event_flag_t::pollin)) != 0;
        std::size_t count = 0;
        while (receive_ready && receive_budget.can_receive ()) {
            // The same raw port call used by raw_mesh_node_owner_t::receive_next.
            if (!turn.port || !turn.port->receive_if_ready (received, zlink::poll_event_flag_t::pollin)) {
                receive_budget.account (0);
                break;
            }
            std::size_t bytes = 0;
            for (const auto &part : received.parts ()) {
                if (part.size () > std::numeric_limits<std::size_t>::max () - bytes) {
                    bytes = std::numeric_limits<std::size_t>::max ();
                    break;
                }
                bytes += part.size ();
            }
            receive_budget.account (bytes);
            ++count;
            ++evidence.received;
            handler (received, &*application_permit);
            if (receive_budget.exhausted ())
                break;
            application_permit.reset ();
            if (stop.load () || !application_permit_budget)
                break;
            application_permit.emplace (application_permit_budget.take ());
        }
        application_permit.reset ();
        application_permit_budget.release_without_handler ();
        readiness.reset ();
        if (count == 0 && !stop.load () && !supply.has_supply ())
            readiness = owner.wait_for_activity (std::chrono::milliseconds (50),
                                                  accept_application_receive);
    }
    return evidence;
}

template <typename Metrics, typename Payload = payload_t>
int direct_server (const std::string &endpoint, Metrics &metrics, std::atomic<bool> &stop,
                   int level, bool raw_mesh, payload_shape_t shape = payload_shape_t::bytes)
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
                std::optional<std::uint64_t> correlation;
                if (handle_control (socket, received, local, nullptr, &correlation))
                    continue;
                std::optional<messaging::envelope_header_t> header;
                auto payload = handler.handle (received, header);
                if (received.reply_token ())
                    raw_reply (received, payload, header, correlation);
            }
        }
        return 0;
    }
    std::shared_ptr<fw::runtime::application_job_queue_t> application_jobs;
    if (level >= 1) {
        application_jobs = std::make_shared<fw::runtime::application_job_queue_t> (
          fw::runtime::resolve_application_job_queue_configuration (
            fw::application_job_queue_profile_t::balanced, std::nullopt,
            fw::runtime::detect_application_job_queue_processor_limits ()));
    }
    fw::runtime::mesh::raw_mesh_node_owner_t owner (
      owner_options (zlink::routing_id_t::from ("bench-server"), endpoint, application_jobs));
    if (application_jobs)
        owner.start ();
    else
        owner.ablation_start ();
    if (shape == payload_shape_t::mixed) {
        for (const auto size : {1024U, 4096U}) {
            const auto plan = game_payload_plan (size);
            std::fprintf (stderr, "ladder target shape=mixed target=%zu json_body=%zu items=%zu\n",
                          static_cast<std::size_t> (size), plan.json_size, plan.item_count);
        }
    }
    std::fprintf (stderr, "#4350 target L%d: real raw_mesh_node_owner_t; fixed handler\n", level);
    struct state_t {
        fw::runtime::mesh::raw_mesh_node_owner_t &owner;
        fixed_handler_t<Metrics, Payload> handler;
        std::uint64_t handler_releases = 0;
    } state{owner, {metrics, level}};
    const auto handle = [] (void *opaque, zlink::received_t &received,
                            fw::runtime::application_job_queue_t::permit_t *permit) {
        auto &state = *static_cast<state_t *> (opaque);
        std::optional<std::uint64_t> correlation;
        if (handle_control (state.owner.ablation_socket (), received,
                            state.owner.startup_descriptor (), nullptr, &correlation))
            return;
        // L0-L4: one fixed handler; activation/scope starts at L5_7.
        if (permit) {
            permit->release_for_handler_entry ();
            ++state.handler_releases;
        }
        std::optional<messaging::envelope_header_t> header;
        auto payload = state.handler.handle (received, header);
        if (received.reply_token ())
            raw_reply (received, payload, header, correlation);
    };
    if (application_jobs) {
        const auto evidence = receive_reserved (owner, application_jobs, stop,
          [&] (zlink::received_t &received, auto *permit) { handle (&state, received, permit); });
        const auto status = application_jobs->snapshot ();
        const auto pressure = application_jobs->pressure_metrics_snapshot ();
        std::fprintf (stderr,
          "#4350 permit L%d: persistent_supply=1 receive_flow=owner_options "
          "batches=%llu reserved=%llu received=%llu handler_releases=%llu "
          "in_use=%u capacity_waits=%llu flow_failures=%llu\n", level,
          static_cast<unsigned long long> (evidence.batches),
          static_cast<unsigned long long> (evidence.reserved),
          static_cast<unsigned long long> (evidence.received),
          static_cast<unsigned long long> (state.handler_releases),
          status.permits_in_use,
          static_cast<unsigned long long> (status.capacity_wait_count),
          static_cast<unsigned long long> (pressure.flow_state_config_failure_count));
    } else
        owner.ablation_receive (stop, handle, &state);
    return 0;
}
}
