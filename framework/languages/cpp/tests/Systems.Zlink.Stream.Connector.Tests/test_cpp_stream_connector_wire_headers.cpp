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
#include "../support/stream_connector_scripted_transport.hpp"
#include "../support/stream_connector_callback_sync.hpp"
#include "../support/stream_connector_failure.hpp"

using namespace stream_connector_fixture;

namespace
{
class held_compression_codec_t final : public zlink::stream_connector::compression_codec_t
{
  public:
    std::vector<std::uint8_t> compress (const std::vector<std::uint8_t> &payload) const override
    {
        std::unique_lock<std::mutex> lock (_mutex);
        _entered = true;
        _changed.notify_all ();
        _changed.wait (lock, [this] { return _released; });
        return payload;
    }

    std::vector<std::uint8_t> decompress (const std::vector<std::uint8_t> &payload,
                                          std::size_t) const override
    {
        return payload;
    }

    bool wait_until_entered () const
    {
        std::unique_lock<std::mutex> lock (_mutex);
        return _changed.wait_for (lock, std::chrono::seconds (1), [this] { return _entered; });
    }

    void release () const
    {
        std::lock_guard<std::mutex> lock (_mutex);
        _released = true;
        _changed.notify_all ();
    }

  private:
    mutable std::mutex _mutex;
    mutable std::condition_variable _changed;
    mutable bool _entered = false;
    mutable bool _released = false;
};
} // namespace

using zlink::stream_connector::codec_t;
using zlink::stream_connector::header_flags_t;
using zlink::stream_connector::message_kind_t;
using zlink::stream_connector::detail::header_codec_t;
using zlink::stream_connector::detail::stream_header_t;

namespace stream_connector_tests
{
int verify_reply_and_actor_headers ()
{
    for (const auto kind : {message_kind_t::response, message_kind_t::error}) {
        stream_header_t reply;
        reply.kind = kind;
        reply.codec = kind == message_kind_t::error ? codec_t::json : codec_t::raw;
        reply.flags = header_flags_t::has_request_seq;
        reply.request_seq = 7;
        const auto encoded = header_codec_t{}.encode (reply);
        if (!encoded || encoded.value ().size () < 13 || encoded.value ()[12] != 0
            || !header_codec_t{}.decode (encoded.value ())) {
            return fail (200);
        }
        reply.name = "forbidden.reply.name";
        if (header_codec_t{}.encode (reply)) {
            return fail (201);
        }
    }

    /* stream-connector §4.2/§4.5/§4.6: the slot is an optional u16 tail,
   * zero and truncated values are decode errors, and controls stay slotless. */
    {
        stream_header_t slotted;
        slotted.kind = message_kind_t::send;
        slotted.codec = codec_t::raw;
        slotted.name = "actor.packet";
        slotted.actor_slot = 23;
        auto encoded_slot = header_codec_t{}.encode (slotted);
        auto decoded_slot =
          encoded_slot
            ? header_codec_t{}.decode (encoded_slot.value ())
            : zlink::stream_connector::result_t<stream_header_t>::failure (
                zlink::stream_connector::error_code_t::frame_decode_failed, "slot encode failed");
        if (!decoded_slot || decoded_slot.value ().actor_slot != 23
            || (static_cast<std::uint8_t> (decoded_slot.value ().flags)
                & static_cast<std::uint8_t> (header_flags_t::has_actor_slot))
                 == 0) {
            return fail (271);
        }
        auto truncated_slot = encoded_slot.value ();
        truncated_slot.pop_back ();
        slotted.actor_slot = 0;
        stream_header_t slotted_control;
        slotted_control.kind = message_kind_t::control;
        slotted_control.codec = codec_t::raw;
        slotted_control.name = "$zlink.actor.bound";
        slotted_control.actor_slot = 1;
        if (header_codec_t{}.decode (truncated_slot) || header_codec_t{}.encode (slotted)
            || header_codec_t{}.encode (slotted_control)) {
            return fail (272);
        }
        zlink::stream_connector::packet_t public_packet;
        auto &[name, metadata, codec, compressed, payload, actor_id] = public_packet;
        (void) name;
        (void) metadata;
        (void) codec;
        (void) compressed;
        (void) payload;
        (void) actor_id;

        using zlink::stream_connector::detail::actor_binding_control_codec_t;
        const std::vector<std::uint8_t> valid_bound{1, 0, 23, 5, 'a', 'c', 't', 'o', 'r'};
        const std::vector<std::uint8_t> invalid_bound{1, 0, 0, 5, 'a', 'c', 't', 'o', 'r'};
        const std::vector<std::uint8_t> invalid_unbound{1, 0, 0};
        const auto bound = actor_binding_control_codec_t::decode_bound (valid_bound);
        if (!bound || bound.value ().actor_slot != 23 || bound.value ().actor_id != "actor"
            || actor_binding_control_codec_t::decode_bound (invalid_bound)
            || actor_binding_control_codec_t::decode_unbound (invalid_unbound)) {
            return fail (273);
        }

        zlink::stream_connector::connector_options_t decode_options;
        decode_options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::manual;
        auto decode_state =
          std::make_shared<zlink::stream_connector::detail::connector_state_t> (decode_options);
        stream_header_t bound_header;
        bound_header.kind = message_kind_t::control;
        bound_header.codec = codec_t::raw;
        bound_header.name = "$zlink.actor.bound";
        const std::vector<std::uint8_t> first_bound{1, 0, 7, 5, 'a', 'c', 't', 'o', 'r'};
        const std::vector<std::uint8_t> duplicate_slot{1, 0, 7, 5, 'o', 't', 'h', 'e', 'r'};
        const std::vector<std::uint8_t> duplicate_id{1, 0, 8, 5, 'a', 'c', 't', 'o', 'r'};
        if (!zlink::stream_connector::detail::decode_inbound_packet (*decode_state, bound_header,
                                                                     first_bound)
            || zlink::stream_connector::detail::decode_inbound_packet (*decode_state, bound_header,
                                                                       duplicate_slot)
            || zlink::stream_connector::detail::decode_inbound_packet (*decode_state, bound_header,
                                                                       duplicate_id)) {
            return fail (274);
        }
        stream_header_t unbound_header = bound_header;
        unbound_header.name = "$zlink.actor.unbound";
        if (zlink::stream_connector::detail::decode_inbound_packet (
              *decode_state, unbound_header, std::vector<std::uint8_t>{1, 0, 9})) {
            return fail (275);
        }
        stream_header_t unknown_packet;
        unknown_packet.kind = message_kind_t::send;
        unknown_packet.codec = codec_t::raw;
        unknown_packet.name = "actor.packet";
        unknown_packet.actor_slot = 9;
        if (zlink::stream_connector::detail::decode_inbound_packet (
              *decode_state, unknown_packet, std::vector<std::uint8_t>{'x'})) {
            return fail (276);
        }
        zlink::stream_connector::detail::close_bound_actors (decode_state);
        (void) zlink::stream_connector::detail::dispatch_pending (decode_state);
    }

    return 0;
}

int verify_flow_and_correlation_headers ()
{
    /* stream-connector §5.5: outbound flow is absent; inbound flow bytes are
   * checked for structural length and then dropped. Correlation remains. */
    {
        using zlink::stream_connector::connector_options_t;
        using zlink::stream_connector::packet_t;
        using zlink::stream_connector::detail::connector_state_t;

        const auto wait_for_written_frame =
          [] (const std::shared_ptr<async_write_connection_t> &connection) {
              for (int i = 0; i < 1000; ++i) {
                  if (!connection->written ().empty ()) {
                      return true;
                  }
                  std::this_thread::sleep_for (std::chrono::milliseconds (2));
              }
              return false;
          };
        const auto decode_frame_header = [] (const std::vector<std::uint8_t> &frame)
          -> zlink::stream_connector::result_t<stream_header_t> {
            if (frame.size () < 6) {
                return zlink::stream_connector::result_t<stream_header_t>::failure (
                  zlink::stream_connector::error_code_t::frame_decode_failed, "frame is too short");
            }
            const auto header_size = static_cast<std::size_t> ((frame[0] << 8) | frame[1]);
            std::vector<std::uint8_t> header_bytes (
              frame.begin () + 6, frame.begin () + 6 + static_cast<std::ptrdiff_t> (header_size));
            return header_codec_t{}.decode (header_bytes);
        };
        auto state = std::make_shared<connector_state_t> (connector_options_t{});
        state->state = zlink::stream_connector::connection_state_t::connected;
        auto connection = std::make_shared<async_write_connection_t> ();
        state->connection = connection;
        zlink::stream_connector::detail::submit_send (
          state, packet_t{.name = "no.flow.send", .payload = as_bytes ("payload")});
        if (!wait_for_written_frame (connection)) {
            return fail (211);
        }
        const auto sent = decode_frame_header (connection->written ().front ());
        if (!sent || sent.value ().kind != message_kind_t::send
            || !sent.value ().correlation_id.empty ()
            || (static_cast<std::uint8_t> (sent.value ().flags) & 0x18) != 0) {
            return fail (212);
        }
        connection->shutdown_and_close ();
        state = std::make_shared<connector_state_t> (connector_options_t{});
        state->state = zlink::stream_connector::connection_state_t::connected;
        connection = std::make_shared<async_write_connection_t> ();
        state->connection = connection;
        zlink::stream_connector::detail::submit_request_async (
          state, packet_t{.name = "no.flow.request", .payload = as_bytes ("payload")},
          std::chrono::milliseconds (50),
          [] (zlink::stream_connector::result_t<zlink::stream_connector::detail::request_reply_t>) {
          },
          /*deliver_direct=*/true);
        if (!wait_for_written_frame (connection)) {
            return fail (213);
        }
        const auto requested = decode_frame_header (connection->written ().front ());
        if (!requested || requested.value ().kind != message_kind_t::request
            || requested.value ().correlation_id.empty ()
            || (static_cast<std::uint8_t> (requested.value ().flags) & 0x18) != 0x08) {
            return fail (214);
        }
        {
            std::lock_guard<std::mutex> lock (state->transport_mutex);
            for (auto &[seq, pending] : state->pending_requests) {
                (void) seq;
                if (pending.timeout_timer) {
                    (void) pending.timeout_timer->cancel ();
                }
            }
            state->pending_requests.clear ();
        }
        connection->shutdown_and_close ();

        /* Exhaustion is permanent for this connector instance. The zero
     * sentinel cannot become a wire request sequence after u64 wraps. */
        auto exhausted_state = std::make_shared<connector_state_t> (connector_options_t{});
        exhausted_state->state = zlink::stream_connector::connection_state_t::connected;
        auto exhausted_connection = std::make_shared<async_write_connection_t> ();
        exhausted_state->connection = exhausted_connection;
        exhausted_state->next_request_seq = 0;
        std::optional<zlink::stream_connector::error_code_t> exhausted_error;
        zlink::stream_connector::detail::submit_request_async (
          exhausted_state, packet_t{.name = "exhausted.request", .payload = as_bytes ("x")},
          std::chrono::seconds (1),
          [&] (zlink::stream_connector::result_t<zlink::stream_connector::detail::request_reply_t>
                 result) { exhausted_error = result.error_code (); },
          /*deliver_direct=*/true);
        if (exhausted_error != zlink::stream_connector::error_code_t::send_failed
            || !exhausted_connection->written ().empty ()
            || exhausted_state->next_request_seq != 0) {
            return fail (301);
        }
        exhausted_connection->shutdown_and_close ();

        /* Admission order must survive unlocked compression of an earlier
     * operation. A later Send cannot reach the socket first. */
        auto held_codec = std::make_shared<held_compression_codec_t> ();
        connector_options_t ordered_options;
        ordered_options.compression = zlink::stream_connector::compression_t::lz4;
        ordered_options.compression_codec = held_codec;
        ordered_options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;
        auto ordered_state = std::make_shared<connector_state_t> (ordered_options);
        ordered_state->state = zlink::stream_connector::connection_state_t::connected;
        ordered_state->compression_codec = held_codec;
        ordered_state->lz4_enabled = true;
        auto ordered_connection = std::make_shared<held_async_write_connection_t> ();
        ordered_state->connection = ordered_connection;
        send_results_t ordered_results;
        joining_thread_t first_send_thread ([&] {
            zlink::stream_connector::detail::submit_send_async (
              ordered_state,
              packet_t{.name = "ordered.first",
                       .codec = codec_t::raw,
                       .compressed = true,
                       .payload = as_bytes ("first")},
              ordered_results.recorder ());
        });
        if (!held_codec->wait_until_entered ()) {
            held_codec->release ();
            first_send_thread.join ();
            return fail (314);
        }
        zlink::stream_connector::detail::submit_send_async (
          ordered_state, packet_t{.name = "ordered.second", .payload = as_bytes ("second")},
          ordered_results.recorder ());
        /* The second Send is accepted and its write start has run on the write
     * strand; the first is still being compressed, so nothing is written. */
        const bool second_overtook =
          !drain_strand (ordered_state->write_strand) || ordered_connection->write_count () != 0;
        held_codec->release ();
        first_send_thread.join ();
        const bool first_started = ordered_connection->wait_for_write_count (1);
        auto first_written =
          first_started ? decode_frame_header (ordered_connection->written (0))
                        : zlink::stream_connector::result_t<stream_header_t>::failure (
                            zlink::stream_connector::error_code_t::send_failed, "no frame written");
        ordered_connection->complete_write ();
        const bool second_started = ordered_connection->wait_for_write_count (2);
        ordered_connection->complete_write ();
        const auto ordered_settled = ordered_results.wait_for_count (2);
        ordered_connection->shutdown_and_close ();
        if (second_overtook || !first_written || first_written.value ().name != "ordered.first"
            || !second_started || !ordered_settled || !ordered_results.all_succeeded ()) {
            return fail (315);
        }

        auto request_codec = std::make_shared<held_compression_codec_t> ();
        connector_options_t request_options;
        request_options.compression = zlink::stream_connector::compression_t::lz4;
        request_options.compression_codec = request_codec;
        auto request_state = std::make_shared<connector_state_t> (request_options);
        request_state->compression_codec = request_codec;
        request_state->lz4_enabled = true;
        request_state->connection = std::make_shared<held_async_write_connection_t> ();
        request_state->state = zlink::stream_connector::connection_state_t::connected;
        joining_thread_t slow_request_thread ([&] {
            (void) zlink::stream_connector::detail::submit_request (
              request_state,
              packet_t{.name = "accepted.before.encode",
                       .codec = codec_t::raw,
                       .compressed = true,
                       .payload = as_bytes ("request")},
              std::chrono::milliseconds (200));
        });
        if (!request_codec->wait_until_entered ()) {
            request_codec->release ();
            slow_request_thread.join ();
            return fail (326);
        }
        bool accepted_before_encode = false;
        {
            std::lock_guard<std::mutex> lock (request_state->transport_mutex);
            accepted_before_encode = request_state->pending_requests.size () == 1;
        }
        request_codec->release ();
        slow_request_thread.join ();
        (void) zlink::stream_connector::detail::connection_ended (
          request_state,
          {zlink::stream_connector::error_code_t::disconnected, "request fixture ended"},
          std::shared_ptr (request_state->connection));
        if (!accepted_before_encode) {
            return fail (327);
        }
        (void) zlink::stream_connector::detail::dispatch_pending (request_state);

        stream_header_t inbound;
        inbound.kind = message_kind_t::send;
        inbound.codec = codec_t::raw;
        inbound.name = "no.flow.inbound";
        const auto encoded = header_codec_t{}.encode (inbound);
        if (!encoded) {
            return fail (215);
        }
        auto flowed = encoded.value ();
        flowed[3] |= 0x10;
        flowed.insert (flowed.end (), 37, static_cast<std::uint8_t> ('Z'));
        if (!header_codec_t{}.decode (flowed)) {
            return fail (216);
        }
        flowed.pop_back ();
        if (header_codec_t{}.decode (flowed)) {
            return fail (217);
        }
    }

    return 0;
}
} // namespace stream_connector_tests
