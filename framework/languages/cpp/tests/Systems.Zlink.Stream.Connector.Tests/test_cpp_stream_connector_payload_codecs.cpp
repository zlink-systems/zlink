/* SPDX-License-Identifier: Elastic-2.0 */
#include <zlink/Contracts/Messaging/operation_contracts.hpp>
#include <zlink/Contracts/Sockets/stream_socket.hpp>
#include <zlink/stream_connector.hpp>
#include <zlink/stream_connector/codecs/auto_codec.hpp>
#include <zlink/stream_e2e_client.hpp>
#include <zlink/stream_e2e_client/codecs/auto_codec.hpp>
#include "runtime/connector_runtime.hpp"
#include "runtime/protocol/framing.hpp"
#include "runtime/protocol/framing/frame_codec.hpp"
#include "runtime/protocol/header_codec.hpp"
#include "runtime/protocol/metadata_codec.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <zlink/json.hpp>
#include <optional>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>
#include "../support/stream_connector_cases.hpp"

#include "../support/stream_connector_wire_frames.hpp"
#include "../support/stream_connector_typed_messages.hpp"
#include "../support/stream_connector_compression_fixture.hpp"
#include "../support/stream_connector_scripted_transport.hpp"
#include "../support/stream_connector_callback_sync.hpp"
#include "../support/stream_connector_failure.hpp"

using namespace stream_connector_fixture;

namespace
{
class counting_compression_codec_t final : public zlink::stream_connector::compression_codec_t
{
  public:
    mutable std::atomic_int decompressions{0};

    std::vector<std::uint8_t> compress (const std::vector<std::uint8_t> &payload) const override
    {
        return payload;
    }

    std::vector<std::uint8_t> decompress (const std::vector<std::uint8_t> &payload,
                                          std::size_t) const override
    {
        ++decompressions;
        return payload;
    }
};

class oversized_compression_codec_t final : public zlink::stream_connector::compression_codec_t
{
  public:
    std::vector<std::uint8_t> compress (const std::vector<std::uint8_t> &payload) const override
    {
        return payload;
    }

    std::vector<std::uint8_t> decompress (const std::vector<std::uint8_t> &,
                                          std::size_t max_decompressed_size) const override
    {
        return as_bytes (std::string (max_decompressed_size + 1, 'x'));
    }
};
} // namespace

namespace stream_connector_tests
{
int verify_encode_failure ()
{
    /* §9.2: encode failures retain their code on send and request surfaces. */
    {
        using namespace zlink::stream_connector;
        connector_t connector;
        int errors = 0;
        auto subscription =
          connector.on_error ([&] (const zlink::stream_connector::error_t &error) {
              if (error.code == error_code_t::send_failed)
                  ++errors;
          });
        connector.send (failed_protobuf_t{}).submit ();
        auto result = connector.request (failed_protobuf_t{}).submit<failed_protobuf_t> ();
        int callbacks = 0;
        connector.request (failed_protobuf_t{})
          .submit<failed_protobuf_t> ([&] (result_t<failed_protobuf_t> value) {
              if (!value && value.error_code () == error_code_t::send_failed)
                  ++callbacks;
          });
        (void) connector.dispatch ();
        (void) connector.dispatch ();
        if (result || result.error_code () != error_code_t::send_failed || errors != 1
            || callbacks != 1)
            return fail (323);
        connector.close ();
    }

    return 0;
}

int verify_metadata_compression_and_payload_limits ()
{
    {
        zlink::stream_connector::detail::metadata_codec_t metadata_codec;
        zlink::stream_connector::metadata_t metadata;
        metadata.with ("trace", "abc");
        auto encoded_metadata = metadata_codec.encode (metadata);
        auto decoded_metadata =
          encoded_metadata
            ? metadata_codec.decode (encoded_metadata.value ())
            : zlink::stream_connector::result_t<zlink::stream_connector::metadata_t>::failure (
                zlink::stream_connector::error_code_t::validation_failed, "metadata encode failed");
        const auto empty_value_metadata = metadata_codec.decode ({1, 1, 'k', 0, 0});
        if (!decoded_metadata || decoded_metadata.value ().values.at ("trace") != "abc"
            || !empty_value_metadata || empty_value_metadata.value ().values.at ("k") != ""
            || metadata_codec.decode ({}).error_code ()
                 != zlink::stream_connector::error_code_t::frame_decode_failed
            || metadata_codec.decode ({1}).error_code ()
                 != zlink::stream_connector::error_code_t::frame_decode_failed
            || metadata_codec.decode ({1, 0}).error_code ()
                 != zlink::stream_connector::error_code_t::frame_decode_failed
            || metadata_codec.decode ({1, 1, 'k'}).error_code ()
                 != zlink::stream_connector::error_code_t::frame_decode_failed
            || metadata_codec.decode ({1, 1, 'k', 0, 1, 'v', 1}).error_code ()
                 != zlink::stream_connector::error_code_t::frame_decode_failed) {
            return fail (70);
        }
        zlink::stream_connector::metadata_t invalid_metadata;
        invalid_metadata.with ("", "value");
        if (metadata_codec.encode (invalid_metadata).error_code ()
            != zlink::stream_connector::error_code_t::validation_failed) {
            return fail (71);
        }
        zlink::stream_connector::metadata_t too_large_metadata;
        too_large_metadata.with ("key", std::string (65536, 'v'));
        if (metadata_codec.encode (too_large_metadata).error_code ()
            != zlink::stream_connector::error_code_t::validation_failed) {
            return fail (260);
        }
        zlink::stream_connector::metadata_t boundary_metadata;
        boundary_metadata.with ("k", std::string (1019, 'v'));
        zlink::stream_connector::metadata_t over_limit_metadata;
        over_limit_metadata.with ("k", std::string (1020, 'v'));
        if (!metadata_codec.encode (boundary_metadata)
            || metadata_codec.encode (over_limit_metadata).error_code ()
                 != zlink::stream_connector::error_code_t::validation_failed) {
            return fail (176);
        }
    }

    {
        zlink::stream_connector::connector_options_t frame_options;
        frame_options.max_send_payload_size = 2;
        frame_options.max_receive_payload_size = 4;
        const auto oversized_header =
          static_cast<std::size_t> (std::numeric_limits<std::uint16_t>::max ()) + 1;
        if (zlink::stream_connector::detail::frame_codec_t::validate_receive_frame_size (
              oversized_header, 1, frame_options)
            || zlink::stream_connector::detail::frame_codec_t::validate_receive_frame_size (
              1, 5, frame_options)
            || zlink::stream_connector::detail::frame_codec_t::encode_prefix (
                 static_cast<std::size_t> (std::numeric_limits<std::uint16_t>::max ()) + 1, 0)
                   .error_code ()
                 != zlink::stream_connector::error_code_t::frame_too_large
            || zlink::stream_connector::detail::frame_codec_t::encode_prefix (
                 0, static_cast<std::size_t> (std::numeric_limits<std::uint32_t>::max ()) + 1)
                   .error_code ()
                 != zlink::stream_connector::error_code_t::frame_too_large) {
            return fail (73);
        }
        /* stream-connector 32 §4.7 and §9: a send payload over
     * max_send_payload_size fails before the transport write and is
     * reported as validation_failed, the same code .NET reports from
     * ZlinkStreamFrameCodec.ValidateSendPayload. frame_too_large names the
     * receive bound (§9) and must never appear on the send path. */
        const std::vector<std::uint8_t> send_header{0x01};
        const std::vector<std::uint8_t> over_limit_payload (frame_options.max_send_payload_size + 1,
                                                            0x7f);
        const auto over_limit = zlink::stream_connector::detail::frame_codec_t::encode (
          send_header, over_limit_payload, frame_options);
        if (over_limit
            || over_limit.error_code ()
                 != zlink::stream_connector::error_code_t::validation_failed) {
            return fail (181);
        }
        const std::vector<std::uint8_t> at_limit_payload (frame_options.max_send_payload_size,
                                                          0x7f);
        if (!zlink::stream_connector::detail::frame_codec_t::encode (send_header, at_limit_payload,
                                                                     frame_options)) {
            return fail (182);
        }
    }

    {
        const auto lz4 = zlink::stream_connector::lz4_compression_codec ();
        if (!lz4) {
            return fail (19);
        }
        const auto source =
          as_bytes (std::string ("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
        const auto compressed = lz4->compress (source);
        const auto restored = lz4->decompress (compressed, source.size ());
        if (restored != source || compressed == source) {
            return fail (20);
        }
        /* LZ4 pickle frame (the cross-language wire): header 0xC0 = 4-byte
     * size difference, so the declared decompressed size is the body plus
     * the difference — here far past the receive limit. */
        const std::string oversized_declared_size{
          static_cast<char> (0xC0), static_cast<char> (0x40), static_cast<char> (0x42),
          static_cast<char> (0x0f), static_cast<char> (0x00), static_cast<char> (0x01)};
        bool oversized_rejected = false;
        try {
            (void) lz4->decompress (as_bytes (oversized_declared_size), 1024);
        }
        /* compression_codec_t: a payload over the limit is refused with
     * std::length_error, which the connector reports as frame_too_large. */
        catch (const std::length_error &) {
            oversized_rejected = true;
        }
        if (!oversized_rejected) {
            return fail (141);
        }
    }

    {
        zlink::stream_connector::detail::header_codec_t header_codec;
        zlink::stream_connector::detail::stream_header_t header;
        header.kind = zlink::stream_connector::message_kind_t::request;
        header.codec = zlink::stream_connector::codec_t::json;
        header.request_seq = 42;
        header.name = "profile.get";
        header.metadata.with ("traceId", "abc");
        auto encoded = header_codec.encode (header);
        if (!encoded) {
            return fail (21);
        }
        auto decoded = header_codec.decode (encoded.value ());
        if (!decoded || decoded.value ().kind != zlink::stream_connector::message_kind_t::request
            || decoded.value ().codec != zlink::stream_connector::codec_t::json
            || decoded.value ().request_seq.value_or (0) != 42
            || decoded.value ().name != "profile.get"
            || decoded.value ().metadata.values.at ("traceId") != "abc") {
            return fail (22);
        }
        const std::vector<std::uint8_t> invalid_flag{1, 1, 0x80, 1,
                                                     static_cast<std::uint8_t> ('x')};
        if (header_codec.decode (invalid_flag).error_code ()
            != zlink::stream_connector::error_code_t::frame_decode_failed) {
            return fail (23);
        }
        zlink::stream_connector::detail::stream_header_t control;
        control.kind = zlink::stream_connector::message_kind_t::control;
        control.codec = zlink::stream_connector::codec_t::raw;
        control.name = "$zlink.heartbeat.ping";
        if (!header_codec.encode (control)) {
            return fail (24);
        }
        control.codec = zlink::stream_connector::codec_t::json;
        if (header_codec.encode (control)
            || header_codec.encode (control).error_code ()
                 != zlink::stream_connector::error_code_t::frame_decode_failed) {
            return fail (25);
        }
        zlink::stream_connector::detail::stream_header_t application_name;
        application_name.kind = zlink::stream_connector::message_kind_t::send;
        application_name.codec = zlink::stream_connector::codec_t::raw;
        application_name.name = "$application.event";
        if (!header_codec.encode (application_name)) {
            return fail (177);
        }
        application_name.name = "$zlink.private";
        if (header_codec.encode (application_name).error_code ()
            != zlink::stream_connector::error_code_t::frame_decode_failed) {
            return fail (178);
        }
        zlink::stream_connector::connector_options_t frame_options;
        frame_options.max_send_payload_size = 16;
        auto frame = zlink::stream_connector::detail::frame_codec_t::encode (
          encoded.value (), std::vector<std::uint8_t>{'o', 'k'}, frame_options);
        if (!frame || frame.value ().size () != encoded.value ().size () + 8
            || frame.value ()[0] != 0 || frame.value ()[1] != encoded.value ().size ()
            || frame.value ()[2] != 0 || frame.value ()[3] != 0 || frame.value ()[4] != 0
            || frame.value ()[5] != 2) {
            return fail (26);
        }
    }

    return 0;
}

int verify_automatic_codec (
  zlink::context_t &context,
  const zlink::stream_connector::connector_options_t &options,
  zlink::stream_connector::connector_t &connector,
  zlink::stream_connector::detail::connector_runtime_t &runtime,
  zlink::stream_connector::result_t<stream_connector_fixture::login_reply_t> &request,
  zlink::stream_connector::connector_t &immediate)
{
    zlink::stream_socket_t auto_server (context);
    auto_server.options ().recv_mode (zlink::stream_recv_mode_t::raw);
    auto_server.options ().notify (false);
    auto_server.bind ("tcp://127.0.0.1:0");
    const auto auto_endpoint = auto_server.options ().last_endpoint ();
    std::atomic_bool auto_json_seen{false};
    joining_thread_t auto_server_thread ([&auto_server, &auto_json_seen] {
        zlink::received_t inbound;
        if (auto_server.recv (inbound) != 0) {
            return;
        }
        std::string buffer =
          inbound.parts ().empty () ? std::string{} : inbound.parts ()[0].to_string ();
        if (auto frame = try_read_server_frame (buffer)) {
            auto_json_seen =
              frame->header.kind == zlink::stream_connector::message_kind_t::send
              && frame->header.codec == zlink::stream_connector::codec_t::json
              && frame->header.name == auto_payload_t::packet_name
              && nlohmann::json::parse (frame->payload).at ("text").get<std::string> () == "auto";
        }
        inbound.close ();
    });

    zlink::stream_connector::connector_options_t auto_options;
    auto_options.endpoint = auto_endpoint;
    auto_options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::manual;
    auto auto_connector = zlink::stream_connector::connector_factory_t::create (auto_options);
    if (!auto_connector.connect ()) {
        return fail (30);
    }
    auto_connector.send (auto_payload_t{"auto"}).submit ();
    auto_server_thread.join ();
    if (!auto_json_seen) {
        return fail (32);
    }

    int auto_dispatch_count = 0;
    auto auto_codec_subscription = zlink::stream_connector::codecs::on<auto_payload_t> (
      auto_connector, [&] (const auto_payload_t &payload) {
          if (payload.text == "callback") {
              ++auto_dispatch_count;
          }
      });
    zlink::stream_connector::detail::connector_runtime_t::from (auto_connector)
      .receive_packet (
        zlink::stream_connector::packet_t{auto_payload_t::packet_name,
                                          {},
                                          zlink::stream_connector::codec_t::json,
                                          false,
                                          to_stream_payload (auto_payload_t{"callback"})});
    if (auto_connector.received_count<auto_payload_t> () != 1
        || auto_connector.received_count<auto_payload_t> ()
             != auto_connector.received_count (auto_payload_t::packet_name)) {
        return fail (285);
    }
    if (auto_connector.pending_dispatch_count () != 1 || auto_dispatch_count != 0) {
        return fail (33);
    }
    if (!auto_connector.dispatch () || auto_dispatch_count != 1
        || auto_connector.pending_dispatch_count () != 0) {
        return fail (34);
    }
    zlink::stream_connector::detail::connector_runtime_t::from (auto_connector)
      .receive_packet (
        zlink::stream_connector::packet_t{auto_payload_t::packet_name,
                                          {},
                                          zlink::stream_connector::codec_t::json,
                                          false,
                                          to_stream_payload (auto_payload_t{"typed-wait"})});
    auto auto_typed_wait = auto_connector.wait_for<auto_payload_t> ().submit ();
    if (!auto_typed_wait || auto_typed_wait.value ().payload.text != "typed-wait") {
        return fail (78);
    }
    zlink::stream_connector::detail::connector_runtime_t::from (auto_connector)
      .receive_packet (zlink::stream_connector::packet_t{
        auto_payload_t::packet_name,
        {},
        zlink::stream_connector::codec_t::json,
        false,
        to_stream_payload (auto_payload_t{"typed-filter-skipped"})});
    zlink::stream_connector::detail::connector_runtime_t::from (auto_connector)
      .receive_packet (
        zlink::stream_connector::packet_t{auto_payload_t::packet_name,
                                          {},
                                          zlink::stream_connector::codec_t::json,
                                          false,
                                          to_stream_payload (auto_payload_t{"typed-filtered"})});
    auto auto_filtered_wait =
      auto_connector.wait_for<auto_payload_t> ()
        .where ([] (const zlink::stream_connector::message_t<auto_payload_t> &message) {
            return message.payload.text == "typed-filtered";
        })
        .submit ();
    if (!auto_filtered_wait || auto_filtered_wait.value ().payload.text != "typed-filtered") {
        return fail (79);
    }
    zlink::stream_connector::detail::connector_runtime_t::from (auto_connector)
      .receive_packet (zlink::stream_connector::packet_t{
        auto_payload_t::packet_name,
        {},
        zlink::stream_connector::codec_t::json,
        false,
        to_stream_payload (auto_payload_t{"member-filter-skipped"})});
    zlink::stream_connector::detail::connector_runtime_t::from (auto_connector)
      .receive_packet (
        zlink::stream_connector::packet_t{auto_payload_t::packet_name,
                                          {},
                                          zlink::stream_connector::codec_t::json,
                                          false,
                                          to_stream_payload (auto_payload_t{"member-filtered"})});
    auto auto_member_filtered_wait =
      auto_connector.wait_for<auto_payload_t> ()
        .where (&auto_payload_t::text, std::string ("member-filtered"))
        .submit ();
    if (!auto_member_filtered_wait
        || auto_member_filtered_wait.value ().payload.text != "member-filtered") {
        return fail (107);
    }
    auto_connector.close ();

    return verify_request_error_and_close (context, options, connector, runtime, request,
                                           immediate);
}

int verify_inbound_decompression (const zlink::stream_connector::connector_options_t &options,
                                  zlink::stream_connector::connector_t &connector)
{
    /* §5.2: a response without a pending request is discarded before its
     * compressed payload reaches the codec. A later packet confirms that the
     * read pump has passed the response. */
    {
        auto codec = std::make_shared<counting_compression_codec_t> ();
        zlink::stream_connector::connector_options_t options;
        options.compression = zlink::stream_connector::compression_t::lz4;
        options.compression_codec = codec;
        options.reconnect.enabled = false;
        options.heartbeat.enabled = false;
        auto connector = zlink::stream_connector::connector_factory_t::create (options);
        auto state = std::static_pointer_cast<zlink::stream_connector::detail::connector_state_t> (
          connector_internal_handle (connector));
        auto connection = std::make_shared<fed_read_connection_t> ();
        state->connection = connection;
        zlink::stream_connector::detail::change_state (
          state, zlink::stream_connector::connection_state_t::connected);
        zlink::stream_connector::detail::start_read_loop (state);
        connection->feed (make_server_frame (zlink::stream_connector::message_kind_t::response,
                                             918273, "", "unused", true));
        connection->feed (make_server_frame (zlink::stream_connector::message_kind_t::send, 0,
                                             "after.unmatched", "ready"));
        auto next = connector.wait_for ("after.unmatched", std::chrono::seconds (5));
        (void) connector.close ();
        if (!next || codec->decompressions != 0) {
            return fail (406);
        }
    }

    /* stream-connector §4.7: a payload over the receive limit after
   * decompression is FrameTooLarge and ends the connection as ProtocolError,
   * whether the codec refuses it (LZ4) or returns it. */
    for (const bool refusing_codec : {true, false}) {
        zlink::stream_connector::connector_options_t inflated_options;
        inflated_options.reconnect.enabled = false;
        inflated_options.heartbeat.enabled = false;
        inflated_options.max_receive_payload_size = 64;
        inflated_options.compression = zlink::stream_connector::compression_t::lz4;
        if (!refusing_codec) {
            inflated_options.compression_codec = std::make_shared<oversized_compression_codec_t> ();
        }
        auto inflated_connector =
          zlink::stream_connector::connector_factory_t::create (inflated_options);
        auto state = std::static_pointer_cast<zlink::stream_connector::detail::connector_state_t> (
          connector_internal_handle (inflated_connector));
        std::optional<zlink::stream_connector::error_code_t> inflated_error;
        auto inflated_error_subscription = inflated_connector.on_error (
          [&] (const zlink::stream_connector::error_t &error) { inflated_error = error.code; });
        auto connection = std::make_shared<fed_read_connection_t> ();
        state->connection = connection;
        zlink::stream_connector::detail::change_state (
          state, zlink::stream_connector::connection_state_t::connected);
        zlink::stream_connector::detail::start_read_loop (state);
        connection->feed (make_server_frame (zlink::stream_connector::message_kind_t::send, 0,
                                             "inflated.push", std::string (1000, 'x'), true));
        const auto ended = inflated_connector.state ();
        const auto reason = inflated_connector.close_reason ();
        (void) inflated_connector.dispatch ();
        (void) inflated_connector.close ();
        if (ended != zlink::stream_connector::connection_state_t::disconnected
            || reason != zlink::stream_connector::close_reason_t::protocol_error
            || inflated_error != zlink::stream_connector::error_code_t::frame_too_large) {
            return fail (381);
        }
    }

    return 0;
}

int verify_decompression_failure ()
{
    /* stream-connector §9: a payload that fails to decompress fails only its
   * packet - the pending request it answers, or the error handler for a push -
   * and the connection stays. */
    {
        zlink::stream_connector::connector_options_t broken_options;
        broken_options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;
        broken_options.reconnect.enabled = false;
        broken_options.heartbeat.enabled = false;
        broken_options.compression = zlink::stream_connector::compression_t::lz4;
        broken_options.compression_codec = std::make_shared<prefix_compression_codec_t> ("other");
        auto broken_connector =
          zlink::stream_connector::connector_factory_t::create (broken_options);
        auto state = std::static_pointer_cast<zlink::stream_connector::detail::connector_state_t> (
          connector_internal_handle (broken_connector));
        std::promise<zlink::stream_connector::error_code_t> push_error;
        auto push_error_future = push_error.get_future ();
        std::atomic_bool push_error_set{false};
        auto broken_error_subscription =
          broken_connector.on_error ([&] (const zlink::stream_connector::error_t &error) {
              if (!push_error_set.exchange (true)) {
                  push_error.set_value (error.code);
              }
          });
        auto connection = std::make_shared<fed_read_connection_t> ();
        state->connection = connection;
        zlink::stream_connector::detail::change_state (
          state, zlink::stream_connector::connection_state_t::connected);
        zlink::stream_connector::detail::start_read_loop (state);
        std::optional<zlink::stream_connector::error_code_t> request_code;
        zlink::stream_connector::detail::submit_request_async (
          state,
          zlink::stream_connector::packet_t{.name = "broken.request", .payload = as_bytes ("q")},
          std::chrono::seconds (5),
          [&] (zlink::stream_connector::result_t<zlink::stream_connector::detail::request_reply_t>
                 result) { request_code = result.error_code (); },
          /*deliver_direct=*/true);
        std::optional<std::uint64_t> request_seq;
        if (const auto written = connection->wait_for_written (0)) {
            std::string text (written->begin (), written->end ());
            if (const auto frame = try_read_server_frame (text)) {
                request_seq = frame->header.request_seq;
            }
        }
        if (request_seq) {
            connection->feed (make_server_frame (zlink::stream_connector::message_kind_t::response,
                                                 *request_seq, "", "reply", true));
        }
        connection->feed (make_server_frame (zlink::stream_connector::message_kind_t::send, 0,
                                             "broken.push", "push", true));
        const bool push_reported =
          push_error_future.wait_for (std::chrono::seconds (5)) == std::future_status::ready;
        const auto still_connected = broken_connector.state ();
        connection->feed (make_server_frame (zlink::stream_connector::message_kind_t::send, 0,
                                             "after.broken", "after"));
        const auto after = broken_connector.wait_for ("after.broken", std::chrono::seconds (2));
        (void) broken_connector.close ();
        if (!request_seq
            || request_code != zlink::stream_connector::error_code_t::decompression_failed
            || !push_reported
            || push_error_future.get ()
                 != zlink::stream_connector::error_code_t::decompression_failed
            || still_connected != zlink::stream_connector::connection_state_t::connected
            || !after) {
            return fail (383);
        }
    }

    return 0;
}
} // namespace stream_connector_tests
