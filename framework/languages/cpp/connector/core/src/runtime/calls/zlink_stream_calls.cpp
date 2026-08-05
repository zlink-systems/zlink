/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/connector_runtime.hpp"

#include "runtime/protocol/compression/lz4_compression_codec.hpp"
#include "runtime/protocol/framing/frame_codec.hpp"
#include "runtime/protocol/framing.hpp"
#include "runtime/protocol/header_codec.hpp"
#include "runtime/protocol/metadata_codec.hpp"
#include "runtime/transport/stream_connection.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string_view>

namespace zlink::stream_connector::detail
{

namespace
{

// Process-wide monotonic correlation id for outbound stream packets, mirroring
// the framework channel codec. The sending client generates it; the server only
// echoes it back, so a request and its reply share one id end to end.
std::string next_correlation_id ()
{
    static std::atomic_uint64_t next{1};
    std::uint64_t value = next.fetch_add (1, std::memory_order_relaxed);
    // Cheap uint->hex (no ostringstream): this runs per outbound packet, so it must
    // stay light even when tracing is off.
    char buffer[17];
    int index = static_cast<int> (sizeof (buffer));
    buffer[--index] = '\0';
    do {
        buffer[--index] = "0123456789abcdef"[value & 0xfu];
        value >>= 4u;
    } while (value != 0);
    return std::string (buffer + index);
}

bool stream_trace_enabled ()
{
    static const bool enabled = [] {
        const char *value = std::getenv ("ZLINK_CPP_STREAM_TRACE");
        return value != nullptr && value[0] != '\0' && std::string (value) != "0";
    }();
    return enabled;
}

const char *message_kind_name (message_kind_t kind)
{
    switch (kind) {
        case message_kind_t::send:
            return "send";
        case message_kind_t::request:
            return "request";
        case message_kind_t::response:
            return "response";
        case message_kind_t::error:
            return "error";
        case message_kind_t::control:
            return "control";
    }
    return "unknown";
}

void trace_request (const char *stage,
                    std::optional<std::uint64_t> seq,
                    const std::string &name,
                    const std::string &detail = {})
{
    if (!stream_trace_enabled ()) {
        return;
    }
    static std::mutex trace_mutex;
    std::lock_guard<std::mutex> lock (trace_mutex);
    std::cerr << "zlink-cpp-stream-trace stage=" << stage << " seq=";
    if (seq) {
        std::cerr << *seq;
    } else {
        std::cerr << "-";
    }
    if (!name.empty ()) {
        std::cerr << " name=" << name;
    }
    if (!detail.empty ()) {
        std::cerr << " " << detail;
    }
    std::cerr << '\n';
}

void trace_connector_write (const connector_state_t &state,
                            const char *stage,
                            std::string_view detail = {})
{
    if (!stream_trace_enabled ()) {
        return;
    }
    static std::mutex trace_mutex;
    std::lock_guard<std::mutex> lock (trace_mutex);
    std::cerr << "zlink-cpp-stream-trace side=client connector=" << state.connector_id
              << " stage=" << stage;
    if (!detail.empty ()) {
        std::cerr << " " << detail;
    }
    std::cerr << '\n';
}

result_t<void> validate_packet_limits (const connector_state_t &state, const packet_t &packet)
{
    if (metadata_codec_t::encoded_size (packet.metadata) > max_metadata_size) {
        return result_t<void>::failure (error_code_t::validation_failed,
                                        "stream connector metadata is too large");
    }
    if (packet.codec != codec_t::raw
        && state.enabled_codecs.find (packet.codec) == state.enabled_codecs.end ()) {
        return result_t<void>::failure (error_code_t::unsupported_codec,
                                        "stream connector codec is not enabled");
    }
    if (packet.compressed) {
        if (!state.compression_codec) {
            return result_t<void>::failure (error_code_t::compression_failed,
                                            "stream connector compression codec is not configured");
        }
        if (state.options.compression == compression_t::lz4 && !state.lz4_enabled) {
            return result_t<void>::failure (error_code_t::compression_failed,
                                            "LZ4 compression is not enabled");
        }
    }
    return result_t<void>::success ();
}

result_t<std::string> decode_remote_error_message (const packet_t &packet)
{
    try {
        const auto payload = nlohmann::json::parse (packet.payload.to_string ());
        if (!payload.is_object () || !payload.contains ("code") || !payload["code"].is_string ()
            || !payload.contains ("message") || !payload["message"].is_string ()) {
            return result_t<std::string>::failure (
              error_code_t::frame_decode_failed,
              "Remote error payload must contain string code and message fields.");
        }
        return result_t<std::string>::success (payload["message"].get<std::string> ());
    }
    catch (const nlohmann::json::exception &) {
        return result_t<std::string>::failure (error_code_t::frame_decode_failed,
                                               "Remote error payload must be a JSON object.");
    }
}

bool enqueue_received_message (connector_state_t &state, packet_t packet)
{
    if (state.dispatch_queue.size () >= state.options.max_received_messages) {
        publish_error (
          state, error_t{error_code_t::received_message_dropped,
                         "Received message was dropped because the receive queue is full."});
        state.state_changed.notify_all ();
        return false;
    }
    state.dispatch_queue.push_back (std::move (packet));
    state.state_changed.notify_all ();
    return true;
}

std::vector<std::uint8_t> message_to_bytes (const zlink::message_t &message)
{
    return message.to_bytes ();
}

zlink::message_t message_from_bytes (const std::vector<std::uint8_t> &bytes)
{
    return zlink::message_t::from (bytes);
}

bool has_flag (header_flags_t flags, header_flags_t flag) noexcept
{
    return (static_cast<std::uint8_t> (flags) & static_cast<std::uint8_t> (flag)) != 0;
}

using steady_clock_t = std::chrono::steady_clock;

struct inbound_frame_t
{
    message_kind_t kind = message_kind_t::send;
    std::optional<std::uint64_t> request_seq;
    packet_t packet;
    std::size_t payload_length = 0;
    std::vector<std::uint8_t> payload_preview;
};

result_t<packet_t> decode_packet (connector_state_t &state,
                                  const stream_header_t &header,
                                  std::vector<std::uint8_t> payload_bytes)
{
    auto payload = message_from_bytes (payload_bytes);
    state.last_inbound_received = steady_clock_t::now ();
    const bool compressed = has_flag (header.flags, header_flags_t::payload_compressed);
    if (compressed) {
        if (!state.compression_codec) {
            return result_t<packet_t>::failure (
              error_code_t::decompression_failed,
              "stream connector compression codec is not configured");
        }
        if (state.options.compression == compression_t::lz4 && !state.lz4_enabled) {
            return result_t<packet_t>::failure (error_code_t::decompression_failed,
                                                "LZ4 compression is not enabled");
        }
        try {
            payload =
              state.compression_codec->decompress (payload, state.options.max_receive_payload_size);
            if (payload.size () > state.options.max_receive_payload_size) {
                return result_t<packet_t>::failure (
                  error_code_t::decompression_failed,
                  "decompressed stream payload exceeds maximum stream payload size");
            }
        }
        catch (const std::exception &ex) {
            return result_t<packet_t>::failure (error_code_t::decompression_failed, ex.what ());
        }
    }
    if (header.kind == message_kind_t::control && header.name == "$zlink.heartbeat.ping") {
        /* Server liveness ping (graceful-drain-handoff §7.2): answer with a
         * pong on the next pump pass. Control packets stay out of the
         * application inbound surface. */
        state.heartbeat_pong_due = true;
    }
    if (header.kind == message_kind_t::control
        && header.name == session_closing_codec_t::control_name) {
        /* graceful-drain-handoff §7.1: store the close reason before the
         * server closes the connection; malformed controls close as a
         * protocol error. */
        auto closing = session_closing_codec_t::decode (message_to_bytes (payload));
        if (!closing) {
            state.last_close_reason = close_reason_t::protocol_error;
            return result_t<packet_t>::failure (closing.error_code (),
                                                closing.error ()->message);
        }
        state.last_close_reason = closing.value ().reason;
    }
    return result_t<packet_t>::success (
      packet_t{header.name, header.metadata, header.codec, compressed, payload});
}

result_t<std::vector<std::uint8_t>>
read_exact_until (connector_state_t &state, std::size_t size, steady_clock_t::time_point deadline)
{
    std::vector<std::uint8_t> bytes (size);
    std::size_t offset = 0;
    while (offset < size) {
        if (state.close_requested.load ()) {
            return result_t<std::vector<std::uint8_t>>::failure (error_code_t::closed,
                                                                 "stream connector is closed");
        }
        if (steady_clock_t::now () >= deadline) {
            return result_t<std::vector<std::uint8_t>>::failure (
              error_code_t::request_timeout, "stream connector request timed out");
        }
        if (!state.connection) {
            return result_t<std::vector<std::uint8_t>>::failure (error_code_t::disconnected,
                                                                 "stream connector is closed");
        }
        boost::system::error_code error;
        const auto wait_deadline =
          std::min (deadline, steady_clock_t::now () + std::chrono::milliseconds (5));
        if (!state.connection->wait_readable_until (wait_deadline, error)) {
            if (error) {
                return result_t<std::vector<std::uint8_t>>::failure (error_code_t::disconnected,
                                                                     error.message ());
            }
            if (steady_clock_t::now () >= deadline) {
                return result_t<std::vector<std::uint8_t>>::failure (
                  error_code_t::request_timeout, "stream connector request timed out");
            }
            continue;
        }
        const auto to_read = bytes.size () - offset;
        const auto read = state.connection->read_some (bytes.data () + offset, to_read, error);
        if (error) {
            return result_t<std::vector<std::uint8_t>>::failure (error_code_t::disconnected,
                                                                 error.message ());
        }
        offset += read;
    }
    return result_t<std::vector<std::uint8_t>>::success (std::move (bytes));
}

result_t<void> validate_inbound_frame_limits (const connector_state_t &state,
                                              std::size_t header_size,
                                              std::size_t payload_size)
{
    if (!frame_codec_t::validate_receive_frame_size (header_size, payload_size, state.options)) {
        return result_t<void>::failure (error_code_t::frame_too_large,
                                        "Inbound stream frame exceeds configured limits.");
    }
    return result_t<void>::success ();
}

std::optional<error_t> take_inbound_error (connector_state_t &state)
{
    if (!state.inbound_error) {
        return std::nullopt;
    }
    auto error = std::move (*state.inbound_error);
    state.inbound_error.reset ();
    return error;
}

void enqueue_inbound_observer_notification (std::shared_ptr<connector_state_t> state,
                                            const stream_header_t &header,
                                            std::size_t payload_length,
                                            std::vector<std::uint8_t> payload_preview)
{
    std::vector<std::shared_ptr<inbound_observer_entry_t>> observers;
    {
        observers.reserve (state->inbound_observers.size ());
        for (const auto &observer : state->inbound_observers) {
            if (observer && observer->active.load () && observer->callback) {
                observers.push_back (observer);
            }
        }
    }
    if (observers.empty ()) {
        return;
    }
    const auto pending =
      state->pending_inbound_observer_notifications.fetch_add (1) + static_cast<std::size_t> (1);
    if (pending > state->options.max_inbound_observer_notifications) {
        state->pending_inbound_observer_notifications.fetch_sub (1);
        if (!state->inbound_observer_drop_report_pending.exchange (true)) {
            publish_error (
              *state,
              error_t{
                error_code_t::observer_dropped,
                "Inbound observer notification was dropped because the observer queue is full."});
            state->inbound_observer_drop_report_pending.store (false);
        }
        return;
    }
    inbound_observation_t observation;
    observation.kind = header.kind;
    observation.name = header.name;
    observation.codec = header.codec;
    observation.request_seq = header.request_seq;
    observation.metadata = header.metadata;
    observation.payload_length = payload_length;
    observation.compressed = has_flag (header.flags, header_flags_t::payload_compressed);
    observation.received_at = steady_clock_t::now ();
    observation.payload_preview = std::move (payload_preview);
    post_runtime_operation ([state = std::move (state), observers = std::move (observers),
                             observation = std::move (observation)] () mutable {
        for (const auto &observer : observers) {
            if (!observer || !observer->active.load ()) {
                continue;
            }
            try {
                observer->callback (observation);
            }
            catch (const std::exception &ex) {
                publish_error (
                  *state, error_t{error_code_t::observer_failed,
                                  "Inbound observer callback failed: " + std::string (ex.what ())});
            }
            catch (...) {
                publish_error (*state, error_t{error_code_t::observer_failed,
                                               "Inbound observer callback failed."});
            }
        }
        state->pending_inbound_observer_notifications.fetch_sub (1);
    });
}

result_t<std::vector<std::uint8_t>> encode_packet_frame (connector_state_t &state,
                                                         message_kind_t kind,
                                                         const packet_t &packet,
                                                         std::optional<std::uint64_t> request_seq)
{
    header_flags_t flags = header_flags_t::none;
    if (packet.compressed) {
        flags = flags | header_flags_t::payload_compressed;
    }
    header_codec_t header_codec;
    stream_header_t header_data{kind,        packet.codec, flags,
                                request_seq, packet.name,  packet.metadata};
    if (kind != message_kind_t::control) {
        header_data.correlation_id = next_correlation_id ();
        /* Client-originated flows are created without any configuration
         * (flow-correlation §2.1): the connector is the first hop, so every
         * outbound send/request without an id starts a new flow. */
        header_data.flow_id = flow_id_codec_t::create ();
        header_data.flow_origin = flow_origin_t::application;
    }
    auto header = header_codec.encode (header_data);
    if (!header) {
        return result_t<std::vector<std::uint8_t>>::failure (header.error_code (),
                                                             header.error ()->message);
    }
    const zlink::message_t *payload_message = &packet.payload;
    std::optional<zlink::message_t> compressed_payload;
    if (packet.compressed) {
        if (!state.compression_codec) {
            return result_t<std::vector<std::uint8_t>>::failure (
              error_code_t::compression_failed,
              "stream connector compression codec is not configured");
        }
        try {
            compressed_payload = state.compression_codec->compress (packet.payload);
            payload_message = &*compressed_payload;
        }
        catch (const std::exception &ex) {
            return result_t<std::vector<std::uint8_t>>::failure (error_code_t::compression_failed,
                                                                 ex.what ());
        }
    }
    auto payload = message_to_bytes (*payload_message);
    auto frame = frame_codec_t::encode (header.value (), payload, state.options);
    if (!frame) {
        return result_t<std::vector<std::uint8_t>>::failure (frame.error_code (),
                                                             frame.error ()->message);
    }
    return result_t<std::vector<std::uint8_t>>::success (std::move (frame.value ()));
}

result_t<void> write_packet_frame (connector_state_t &state,
                                   message_kind_t kind,
                                   const packet_t &packet,
                                   std::optional<std::uint64_t> request_seq)
{
    auto frame = encode_packet_frame (state, kind, packet, request_seq);
    if (!frame) {
        return result_t<void>::failure (frame.error_code (), frame.error ()->message);
    }
    try {
        write_bytes (state, frame.value ());
    }
    catch (const std::exception &ex) {
        return result_t<void>::failure (error_code_t::send_failed, ex.what ());
    }
    return result_t<void>::success ();
}

result_t<inbound_frame_t> read_inbound_frame (std::shared_ptr<connector_state_t> state,
                                              steady_clock_t::time_point deadline)
{
    auto prefix_result = read_exact_until (*state, 6, deadline);
    if (!prefix_result) {
        return result_t<inbound_frame_t>::failure (
          prefix_result.error_code (), prefix_result.error () ? prefix_result.error ()->message
                                                              : "stream connector read failed");
    }
    auto prefix = std::move (prefix_result.value ());
    const auto header_size = static_cast<std::size_t> ((prefix[0] << 8) | prefix[1]);
    const auto payload_size =
      (static_cast<std::size_t> (prefix[2]) << 24) | (static_cast<std::size_t> (prefix[3]) << 16)
      | (static_cast<std::size_t> (prefix[4]) << 8) | static_cast<std::size_t> (prefix[5]);
    if (auto limits = validate_inbound_frame_limits (*state, header_size, payload_size); !limits) {
        return result_t<inbound_frame_t>::failure (
          limits.error_code (),
          limits.error () ? limits.error ()->message : "stream connector frame is too large");
    }
    auto header_bytes = read_exact_until (*state, header_size, deadline);
    if (!header_bytes) {
        return result_t<inbound_frame_t>::failure (header_bytes.error_code (),
                                                   header_bytes.error ()
                                                     ? header_bytes.error ()->message
                                                     : "stream connector header read failed");
    }
    auto payload_bytes = read_exact_until (*state, payload_size, deadline);
    if (!payload_bytes) {
        return result_t<inbound_frame_t>::failure (payload_bytes.error_code (),
                                                   payload_bytes.error ()
                                                     ? payload_bytes.error ()->message
                                                     : "stream connector payload read failed");
    }
    header_codec_t header_codec;
    auto decoded = header_codec.decode (header_bytes.value ());
    if (!decoded) {
        return result_t<inbound_frame_t>::failure (decoded.error_code (),
                                                   decoded.error ()->message);
    }
    auto header = decoded.value ();
    const auto preview_length = std::min (state->options.max_inbound_observer_payload_preview_bytes,
                                          payload_bytes.value ().size ());
    std::vector<std::uint8_t> payload_preview (payload_bytes.value ().begin (),
                                               payload_bytes.value ().begin ()
                                                 + static_cast<std::ptrdiff_t> (preview_length));
    enqueue_inbound_observer_notification (state, header, payload_size,
                                           std::move (payload_preview));
    auto packet = decode_packet (*state, header, std::move (payload_bytes.value ()));
    if (!packet) {
        return result_t<inbound_frame_t>::failure (packet.error_code (), packet.error ()->message);
    }
    return result_t<inbound_frame_t>::success (inbound_frame_t{
      header.kind, header.request_seq, std::move (packet.value ()), payload_size, {}});
}

result_t<void> send_due_pong (connector_state_t &state)
{
    if (!state.heartbeat_pong_due || !is_transport_connected (state)) {
        return result_t<void>::success ();
    }
    state.heartbeat_pong_due = false;
    packet_t pong;
    pong.name = "$zlink.heartbeat.pong";
    pong.codec = codec_t::raw;
    pong.payload = zlink::message_t::from (std::string{});
    return write_packet_frame (state, message_kind_t::control, pong, std::nullopt);
}

bool packet_matches_wait (const pending_wait_t &wait, const packet_t &packet)
{
    return (wait.packet_name.empty () || wait.packet_name == packet.name)
           && (!wait.predicate || wait.predicate (packet));
}

bool is_control_packet (const packet_t &packet)
{
    return packet.name.rfind ("$zlink.", 0) == 0;
}

void cancel_timer (const std::shared_ptr<boost::asio::steady_timer> &timer)
{
    if (!timer) {
        return;
    }
    try {
        (void) timer->cancel ();
    } catch (const boost::system::system_error &) {
    }
}

std::optional<pending_wait_t> take_matching_wait (connector_state_t &state, const packet_t &packet)
{
    for (auto iter = state.pending_waits.begin (); iter != state.pending_waits.end (); ++iter) {
        if (packet_matches_wait (iter->second, packet)) {
            auto wait = std::move (iter->second);
            cancel_timer (wait.timeout_timer);
            state.pending_waits.erase (iter);
            return wait;
        }
    }
    return std::nullopt;
}

std::optional<result_t<inbound_frame_t>> try_take_inbound_frame (connector_state_t &state)
{
    if (state.inbound_buffer.size () < 6) {
        return std::nullopt;
    }
    const auto header_size =
      static_cast<std::size_t> ((state.inbound_buffer[0] << 8) | state.inbound_buffer[1]);
    const auto payload_size = (static_cast<std::size_t> (state.inbound_buffer[2]) << 24)
                              | (static_cast<std::size_t> (state.inbound_buffer[3]) << 16)
                              | (static_cast<std::size_t> (state.inbound_buffer[4]) << 8)
                              | static_cast<std::size_t> (state.inbound_buffer[5]);
    if (auto limits = validate_inbound_frame_limits (state, header_size, payload_size); !limits) {
        return result_t<inbound_frame_t>::failure (
          limits.error_code (),
          limits.error () ? limits.error ()->message : "stream connector frame is too large");
    }
    const auto frame_size = 6 + header_size + payload_size;
    if (state.inbound_buffer.size () < frame_size) {
        return std::nullopt;
    }

    std::vector<std::uint8_t> header_bytes (state.inbound_buffer.begin () + 6,
                                            state.inbound_buffer.begin () + 6
                                              + static_cast<std::ptrdiff_t> (header_size));
    std::vector<std::uint8_t> payload_bytes (
      state.inbound_buffer.begin () + 6 + static_cast<std::ptrdiff_t> (header_size),
      state.inbound_buffer.begin () + static_cast<std::ptrdiff_t> (frame_size));
    state.inbound_buffer.erase (state.inbound_buffer.begin (),
                                state.inbound_buffer.begin ()
                                  + static_cast<std::ptrdiff_t> (frame_size));

    header_codec_t header_codec;
    auto decoded = header_codec.decode (header_bytes);
    if (!decoded) {
        return result_t<inbound_frame_t>::failure (decoded.error_code (),
                                                   decoded.error ()->message);
    }
    auto header = decoded.value ();
    const auto preview_length =
      std::min (state.options.max_inbound_observer_payload_preview_bytes, payload_bytes.size ());
    std::vector<std::uint8_t> payload_preview (payload_bytes.begin (),
                                               payload_bytes.begin ()
                                                 + static_cast<std::ptrdiff_t> (preview_length));
    auto packet = decode_packet (state, header, std::move (payload_bytes));
    if (!packet) {
        return result_t<inbound_frame_t>::failure (packet.error_code (), packet.error ()->message);
    }
    return result_t<inbound_frame_t>::success (
      inbound_frame_t{header.kind, header.request_seq, std::move (packet.value ()), payload_size,
                      std::move (payload_preview)});
}

void complete_pending_request (std::shared_ptr<connector_state_t> state,
                               std::uint64_t request_seq,
                               result_t<request_reply_t> result)
{
    std::function<void (result_t<request_reply_t>)> callback;
    std::string packet_name;
    bool deliver_direct = false;
    const bool succeeded = static_cast<bool> (result);
    const auto error_code = result ? error_code_t{} : result.error_code ();
    {
        std::lock_guard<std::mutex> lock (state->transport_mutex);
        auto found = state->pending_requests.find (request_seq);
        if (found == state->pending_requests.end ()) {
            trace_request ("pending-complete-missing", request_seq, {});
            return;
        }
        packet_name = found->second.packet.name;
        callback = std::move (found->second.callback);
        deliver_direct = found->second.deliver_direct;
        cancel_timer (found->second.timeout_timer);
        state->pending_requests.erase (found);
    }
    trace_request ("pending-complete", request_seq, packet_name,
                   succeeded
                     ? "result=success"
                     : "result=failure error=" + std::to_string (static_cast<int> (error_code)));
    if (deliver_direct) {
        if (callback) {
            callback (std::move (result));
        }
        return;
    }
    schedule_delivery (state,
                       [callback = std::move (callback), result = std::move (result)] () mutable {
                           if (callback) {
                               callback (std::move (result));
                           }
                       });
}

void schedule_request_pump (std::shared_ptr<connector_state_t> state);

void route_inbound_packet (std::shared_ptr<connector_state_t> state, packet_t packet)
{
    if (is_control_packet (packet)) {
        return;
    }
    std::optional<pending_wait_t> matched_wait;
    bool dispatch_immediately = false;
    {
        std::lock_guard<std::mutex> lock (state->transport_mutex);
        if (auto wait = take_matching_wait (*state, packet)) {
            matched_wait = std::move (*wait);
        } else if (state->options.dispatch_mode == dispatch_mode_t::immediate) {
            dispatch_immediately = true;
        } else {
            (void) enqueue_received_message (*state, std::move (packet));
            return;
        }
    }

    if (matched_wait) {
        schedule_delivery (
          state, [wait = std::move (*matched_wait), packet = std::move (packet)] () mutable {
              if (wait.callback) {
                  wait.callback (result_t<packet_t>::success (std::move (packet)));
              }
          });
        return;
    }
    if (dispatch_immediately) {
        schedule_delivery (
          state, [state, packet = std::move (packet)] { dispatch_packet (*state, packet); });
    }
}

void enqueue_async_write (std::shared_ptr<connector_state_t> state,
                          std::vector<std::uint8_t> frame,
                          std::function<void (result_t<void>)> callback);

std::chrono::milliseconds heartbeat_maintenance_delay (const heartbeat_options_t &options)
{
    const auto interval = std::max (options.interval, std::chrono::milliseconds (1));
    const auto timeout = std::max (options.timeout, std::chrono::milliseconds (1));
    return std::min (interval, timeout);
}

void schedule_heartbeat_maintenance (const std::shared_ptr<connector_state_t> &state,
                                     std::uint64_t generation);

void run_heartbeat_maintenance (std::shared_ptr<connector_state_t> state,
                                std::uint64_t generation)
{
    std::vector<std::uint8_t> heartbeat_frame;
    std::optional<error_t> timeout_error;
    std::shared_ptr<stream_connection_t> timed_out_connection;
    {
        std::lock_guard<std::mutex> lock (state->transport_mutex);
        if (generation != state->heartbeat_generation || state->close_requested.load ()
            || !is_transport_connected (*state)) {
            return;
        }
        const auto now = steady_clock_t::now ();
        const auto heartbeat_timed_out =
          state->last_inbound_received != steady_clock_t::time_point{}
          && now - state->last_inbound_received >= state->options.heartbeat.timeout;
        if (heartbeat_timed_out) {
            timed_out_connection = state->connection;
            state->write_in_progress = false;
            ++state->heartbeat_generation;
            state->heartbeat_timer.reset ();
            timeout_error =
              error_t{error_code_t::disconnected, "stream connector heartbeat timed out"};
        } else if (state->last_heartbeat_sent == steady_clock_t::time_point{}
                   || now - state->last_heartbeat_sent >= state->options.heartbeat.interval) {
            packet_t heartbeat;
            heartbeat.name = "$zlink.heartbeat.ping";
            heartbeat.codec = codec_t::raw;
            heartbeat.payload = zlink::message_t::from (std::string{});
            auto encoded =
              encode_packet_frame (*state, message_kind_t::control, heartbeat, std::nullopt);
            if (encoded) {
                heartbeat_frame = std::move (encoded.value ());
                state->last_heartbeat_sent = now;
            }
        }
    }
    if (timed_out_connection) {
        publish_error (*state, *timeout_error);
        change_state (state, connection_state_t::disconnected, *timeout_error);
        timed_out_connection->shutdown_and_close_async ();
        schedule_reconnect (state);
        return;
    }
    if (!heartbeat_frame.empty ()) {
        enqueue_async_write (state, std::move (heartbeat_frame), {});
    }
    schedule_heartbeat_maintenance (state, generation);
}

void schedule_heartbeat_maintenance (const std::shared_ptr<connector_state_t> &state,
                                     std::uint64_t generation)
{
    auto timer = post_runtime_operation_after (
      heartbeat_maintenance_delay (state->options.heartbeat),
      [state, generation] { run_heartbeat_maintenance (state, generation); });
    std::lock_guard<std::mutex> lock (state->transport_mutex);
    if (generation != state->heartbeat_generation || state->close_requested.load ()
        || !is_transport_connected (*state)) {
        cancel_timer (timer);
        return;
    }
    state->heartbeat_timer = std::move (timer);
}

/* 수신 콜백(io 스레드) 문맥에서 due pong을 write pump에 싣는다. 이 문맥에서는 동기 write를
 * 쓰면 안 된다. in-flight async_write와 같은 소켓에서 바이트가 섞이고, 상대가 읽지 않으면
 * io 스레드 자체가 blocking write에 갇힌다. */
void queue_due_pong (const std::shared_ptr<connector_state_t> &state)
{
    std::vector<std::uint8_t> frame;
    {
        std::lock_guard<std::mutex> lock (state->transport_mutex);
        if (!state->heartbeat_pong_due || !is_transport_connected (*state)) {
            return;
        }
        packet_t pong;
        pong.name = "$zlink.heartbeat.pong";
        pong.codec = codec_t::raw;
        pong.payload = zlink::message_t::from (std::string{});
        auto encoded = encode_packet_frame (*state, message_kind_t::control, pong, std::nullopt);
        if (!encoded) {
            return;
        }
        state->heartbeat_pong_due = false;
        frame = std::move (encoded.value ());
    }
    enqueue_async_write (state, std::move (frame), {});
}

void process_inbound_buffer (std::shared_ptr<connector_state_t> state,
                             std::optional<error_t> transport_error)
{
    std::vector<std::pair<std::uint64_t, result_t<request_reply_t>>> completed_requests;
    std::vector<packet_t> pushed_packets;
    bool reschedule = false;
    {
        std::lock_guard<std::mutex> lock (state->transport_mutex);
        if (state->close_requested.load () || !is_transport_connected (*state)) {
            return;
        }

        // A stream read can report both bytes and EOF. Decode the bytes that
        // arrived before applying the terminal transport error.
        while (true) {
            auto frame = try_take_inbound_frame (*state);
            if (!frame) {
                break;
            }
            if (!*frame) {
                transport_error = error_t{frame->error_code (),
                                          frame->error () ? frame->error ()->message
                                                          : "stream connector frame decode failed"};
                break;
            }
            auto value = std::move (frame->value ());
            stream_header_t observation_header{
              value.kind,
              value.packet.codec,
              value.packet.compressed ? header_flags_t::payload_compressed : header_flags_t::none,
              value.request_seq,
              value.packet.name,
              value.packet.metadata};
            enqueue_inbound_observer_notification (state, observation_header, value.payload_length,
                                                   std::move (value.payload_preview));
            trace_connector_write (
              *state, "read-dispatch",
              "seq="
                + (value.request_seq ? std::to_string (*value.request_seq) : std::string ("-"))
                + " name=" + value.packet.name + " kind=" + message_kind_name (value.kind));
            /* §5.2: pending request 매칭은 request_seq가 정본이다. packet name은 대조 조건이
             * 아니므로 이름이 달라도 응답을 버리지 않는다. */
            const auto pending = value.request_seq
                                   ? state->pending_requests.find (*value.request_seq)
                                   : state->pending_requests.end ();
            if ((value.kind == message_kind_t::response || value.kind == message_kind_t::error)
                && pending != state->pending_requests.end ()) {
                if (value.kind == message_kind_t::response) {
                    completed_requests.emplace_back (
                      *value.request_seq, result_t<request_reply_t>::success (request_reply_t{
                                            value.packet.codec, std::move (value.packet.payload)}));
                } else if (auto remote_error = decode_remote_error_message (value.packet)) {
                    completed_requests.emplace_back (
                      *value.request_seq,
                      result_t<request_reply_t>::failure (error_code_t::remote_error,
                                                          std::move (remote_error.value ())));
                } else {
                    completed_requests.emplace_back (
                      *value.request_seq,
                      result_t<request_reply_t>::failure (remote_error.error_code (),
                                                          remote_error.error ()->message));
                }
            } else {
                pushed_packets.push_back (std::move (value.packet));
            }
        }
        reschedule =
          is_transport_connected (*state) && !state->close_requested.load () && !transport_error;
        if (!reschedule) {
            trace_connector_write (*state, "read-pump-stop",
                                   std::string ("connected=")
                                     + (is_transport_connected (*state) ? "true" : "false")
                                     + " close=" + (state->close_requested.load () ? "true" : "false")
                                     + " transport_error=" + (transport_error ? "true" : "false"));
        }
    }

    /* graceful-drain-handoff §7.2: server liveness ping의 pong을 application이 dispatch()를
     * 부를 때까지 미룰 수 없다. 동기 request가 응답을 기다리는 동안에는 dispatch()가 돌지
     * 않으므로, pong을 그 경로에만 두면 응답이 heartbeat timeout보다 오래 걸리는 정상 요청에서도
     * 서버가 세션을 heartbeat timeout으로 끊는다. 수신 프레임을 처리한 직후 바로 답한다. */
    queue_due_pong (state);

    for (auto &packet : pushed_packets) {
        route_inbound_packet (state, std::move (packet));
    }
    for (auto &[request_seq, result] : completed_requests) {
        complete_pending_request (state, request_seq, std::move (result));
    }
    if (transport_error) {
        stop_heartbeat_monitor (state);
        std::vector<std::uint64_t> request_ids;
        std::shared_ptr<stream_connection_t> failed_connection;
        {
            std::lock_guard<std::mutex> lock (state->transport_mutex);
            for (const auto &[request_seq, _] : state->pending_requests) {
                request_ids.push_back (request_seq);
            }
            failed_connection = state->connection;
            state->write_in_progress = false;
        }
        // The state transition must precede socket cancellation. The
        // cancellation completion can otherwise overwrite the original
        // protocol or transport error with Operation canceled.
        publish_error (*state, *transport_error);
        change_state (state, connection_state_t::disconnected, *transport_error);
        if (failed_connection) {
            failed_connection->shutdown_and_close_async ();
        }
        for (auto request_seq : request_ids) {
            complete_pending_request (
              state, request_seq,
              result_t<request_reply_t>::failure (transport_error->code, transport_error->message));
        }
        schedule_reconnect (state);
    } else if (reschedule) {
        schedule_request_pump (state);
    }
}

void schedule_request_pump (std::shared_ptr<connector_state_t> state)
{
    std::shared_ptr<stream_connection_t> connection;
    {
        std::lock_guard<std::mutex> lock (state->transport_mutex);
        if (state->read_in_progress || state->close_requested.load ()
            || !is_transport_connected (*state)) {
            trace_connector_write (*state, "read-start-skip",
                                   std::string ("in_progress=")
                                     + (state->read_in_progress ? "true" : "false")
                                     + " close=" + (state->close_requested.load () ? "true" : "false")
                                     + " connected="
                                     + (is_transport_connected (*state) ? "true" : "false"));
            return;
        }
        state->read_in_progress = true;
        connection = state->connection;
    }
    trace_connector_write (*state, "read-start");
    connection->async_read_some (
      8192,
      [state, connection] (boost::system::error_code error,
                           std::vector<std::uint8_t> bytes) mutable {
        std::optional<error_t> transport_error;
        trace_connector_write (*state, "read-completion",
                               error ? "result=failure error=" + error.message ()
                                     : "result=success bytes=" + std::to_string (bytes.size ()));
        {
            std::lock_guard<std::mutex> lock (state->transport_mutex);
            // A close can leave the previous connection's completion queued
            // while a reconnect installs a new connection. Do not let that
            // stale completion consume the new connection's read state.
            if (state->connection != connection) {
                return;
            }
            state->read_in_progress = false;
            if (!bytes.empty ()) {
                state->inbound_buffer.insert (state->inbound_buffer.end (), bytes.begin (),
                                              bytes.end ());
            }
            if (error) {
                transport_error = error_t{
                  state->close_requested.load () ? error_code_t::closed
                                                 : error_code_t::disconnected,
                  state->close_requested.load () ? "stream connector is closed" : error.message ()};
            }
            state->state_changed.notify_all ();
        }
        process_inbound_buffer (state, std::move (transport_error));
    });
}

void start_next_async_write (std::shared_ptr<connector_state_t> state);

void kick_async_write (std::shared_ptr<connector_state_t> state, std::string reason)
{
    trace_connector_write (*state, "write-kick", "reason=" + reason);
    auto executor = state->write_strand;
    auto work_state = std::move (state);
    boost::asio::post (executor,
                       [state = std::move (work_state), reason = std::move (reason)] () mutable {
                           trace_connector_write (*state, "write-kick-dispatch",
                                                  "reason=" + reason);
                           start_next_async_write (std::move (state));
                       });
}

void enqueue_async_write (std::shared_ptr<connector_state_t> state,
                          std::vector<std::uint8_t> frame,
                          std::function<void (result_t<void>)> callback)
{
    const auto frame_size = frame.size ();
    std::size_t queued = 0;
    bool connected = false;
    {
        std::lock_guard<std::mutex> lock (state->transport_mutex);
        state->pending_writes.push_back (pending_write_t{std::move (frame), std::move (callback)});
        queued = state->pending_writes.size ();
        connected = is_transport_connected (*state);
    }
    trace_connector_write (*state, "write-queued",
                           "bytes=" + std::to_string (frame_size)
                             + " pending_writes=" + std::to_string (queued)
                             + " connected=" + (connected ? "true" : "false"));
    kick_async_write (std::move (state), "queued");
}

void finish_async_write (std::shared_ptr<connector_state_t> state,
                         std::function<void (result_t<void>)> callback,
                         result_t<void> result)
{
    {
        std::lock_guard<std::mutex> lock (state->transport_mutex);
        state->write_in_progress = false;
        state->state_changed.notify_all ();
    }
    if (callback) {
        callback (std::move (result));
    }
    kick_async_write (std::move (state), "completion");
}

void start_next_async_write (std::shared_ptr<connector_state_t> state)
{
    pending_write_t write;
    std::shared_ptr<stream_connection_t> connection;
    std::optional<result_t<void>> immediate_failure;
    {
        std::lock_guard<std::mutex> lock (state->transport_mutex);
        if (state->write_in_progress || state->pending_writes.empty ()) {
            trace_connector_write (*state, "write-start-skip",
                                   "in_progress="
                                     + std::string (state->write_in_progress ? "true" : "false")
                                     + " pending_writes="
                                     + std::to_string (state->pending_writes.size ()));
            return;
        }
        state->write_in_progress = true;
        write = std::move (state->pending_writes.front ());
        state->pending_writes.pop_front ();
        if (state->close_requested.load ()) {
            immediate_failure =
              result_t<void>::failure (error_code_t::closed, "stream connector is closed");
        } else if (!is_transport_connected (*state)) {
            immediate_failure = result_t<void>::failure (error_code_t::disconnected,
                                                         "stream connector is not connected");
        } else {
            connection = state->connection;
        }
    }

    if (immediate_failure) {
        trace_connector_write (
          *state, "write-start",
          "result=skipped error="
            + std::to_string (static_cast<int> (immediate_failure->error_code ())));
        finish_async_write (state, std::move (write.callback), std::move (*immediate_failure));
        return;
    }

    try {
        const auto frame_size = write.frame.size ();
        trace_connector_write (*state, "write-start", "bytes=" + std::to_string (frame_size));
        connection->async_write (
          std::move (write.frame),
          [state, connection, callback = std::move (write.callback),
           frame_size] (boost::system::error_code error) mutable {
              boost::asio::post (
                state->write_strand,
                [state, connection, callback = std::move (callback), frame_size, error] () mutable {
                    bool stale_connection = false;
                    {
                        std::lock_guard<std::mutex> lock (state->transport_mutex);
                        stale_connection = state->connection != connection;
                    }
                    if (stale_connection) {
                        if (callback) {
                            callback (result_t<void>::failure (
                              error_code_t::disconnected,
                              "stream connector connection was replaced"));
                        }
                        return;
                    }
                    trace_connector_write (
                      *state, "write-completion",
                      error ? "result=failure bytes=" + std::to_string (frame_size)
                                + " error=" + error.message ()
                            : "result=success bytes=" + std::to_string (frame_size));
                    if (error) {
                        finish_async_write (state, std::move (callback),
                                            result_t<void>::failure (
                                              state->close_requested.load () ? error_code_t::closed
                                                                            : error_code_t::send_failed,
                                              state->close_requested.load ()
                                                ? "stream connector is closed"
                                                : error.message ()));
                        return;
                    }
                    finish_async_write (state, std::move (callback), result_t<void>::success ());
                });
          });
    }
    catch (const std::exception &ex) {
        finish_async_write (state, std::move (write.callback),
                            result_t<void>::failure (error_code_t::send_failed, ex.what ()));
    }
}

void start_next_async_send (std::shared_ptr<connector_state_t> state);

void finish_async_send (std::shared_ptr<connector_state_t> state,
                        packet_t packet,
                        std::function<void (result_t<void>)> callback,
                        result_t<void> result)
{
    {
        std::lock_guard<std::mutex> lock (state->transport_mutex);
        if (result) {
            state->sent_packets.push_back (std::move (packet));
        }
        state->send_in_progress = false;
    }
    schedule_delivery (state,
                       [callback = std::move (callback), result = std::move (result)] () mutable {
                           if (callback) {
                               callback (std::move (result));
                           }
                       });
    start_next_async_send (state);
}

void fail_async_send (std::shared_ptr<connector_state_t> state,
                      packet_t packet,
                      std::function<void (result_t<void>)> callback,
                      result_t<void> result)
{
    finish_async_send (std::move (state), std::move (packet), std::move (callback),
                       std::move (result));
}

void start_next_async_send (std::shared_ptr<connector_state_t> state)
{
    pending_send_t send;
    std::vector<std::uint8_t> frame;
    std::optional<result_t<void>> immediate_failure;
    {
        std::lock_guard<std::mutex> lock (state->transport_mutex);
        if (state->send_in_progress || state->pending_sends.empty ()) {
            return;
        }
        state->send_in_progress = true;
        send = std::move (state->pending_sends.front ());
        state->pending_sends.pop_front ();
        if (state->close_requested.load ()) {
            immediate_failure =
              result_t<void>::failure (error_code_t::closed, "stream connector is closed");
        } else if (!is_transport_connected (*state)) {
            immediate_failure = result_t<void>::failure (error_code_t::disconnected,
                                                         "stream connector is not connected");
        } else if (auto validation = validate_packet_limits (*state, send.packet); !validation) {
            publish_error (*state, *validation.error ());
            immediate_failure = result_t<void>::failure (
              validation.error_code (),
              validation.error () ? validation.error ()->message : "stream send validation failed");
        } else if (auto encoded =
                     encode_packet_frame (*state, message_kind_t::send, send.packet, std::nullopt);
                   !encoded) {
            immediate_failure = result_t<void>::failure (
              encoded.error_code (),
              encoded.error () ? encoded.error ()->message : "stream send encode failed");
        } else {
            frame = std::move (encoded.value ());
        }
    }

    if (immediate_failure) {
        fail_async_send (state, std::move (send.packet), std::move (send.callback),
                         std::move (*immediate_failure));
        return;
    }

    trace_request ("send-submit", std::nullopt, send.packet.name, "kind=send");
    enqueue_async_write (state, std::move (frame),
                         [state, packet = std::move (send.packet),
                          callback = std::move (send.callback)] (result_t<void> result) mutable {
                             trace_request (
                               "send-write-completion", std::nullopt, packet.name,
                               result ? "result=success"
                                      : "result=failure error="
                                          + std::to_string (
                                            static_cast<int> (result.error_code ())));
                             if (!result) {
                                 finish_async_send (state, std::move (packet), std::move (callback),
                                                    std::move (result));
                                 return;
                             }
                             finish_async_send (state, std::move (packet), std::move (callback),
                                                result_t<void>::success ());
                         });
}

} // namespace

void start_read_loop (std::shared_ptr<connector_state_t> state)
{
    schedule_request_pump (std::move (state));
}

void start_heartbeat_monitor (std::shared_ptr<connector_state_t> state)
{
    if (!state->options.heartbeat.enabled) {
        return;
    }
    std::uint64_t generation;
    {
        std::lock_guard<std::mutex> lock (state->transport_mutex);
        if (state->heartbeat_timer) {
            cancel_timer (state->heartbeat_timer);
        }
        generation = ++state->heartbeat_generation;
    }
    schedule_heartbeat_maintenance (state, generation);
}

void stop_heartbeat_monitor (std::shared_ptr<connector_state_t> state)
{
    std::shared_ptr<boost::asio::steady_timer> timer;
    {
        std::lock_guard<std::mutex> lock (state->transport_mutex);
        ++state->heartbeat_generation;
        timer = std::move (state->heartbeat_timer);
    }
    cancel_timer (timer);
}

void resume_pending_writes_after_connect (std::shared_ptr<connector_state_t> state)
{
    std::size_t queued = 0;
    bool connected = false;
    {
        std::lock_guard<std::mutex> lock (state->transport_mutex);
        queued = state->pending_writes.size ();
        connected = is_transport_connected (*state);
    }
    trace_connector_write (*state, "flush-on-connect",
                           "pending_writes=" + std::to_string (queued)
                             + " connected=" + (connected ? "true" : "false"));
    kick_async_write (std::move (state), "connect");
}

void submit_request_async (std::shared_ptr<void> state_handle,
                           packet_t packet,
                           std::chrono::milliseconds timeout,
                           std::function<void (result_t<request_reply_t>)> callback,
                           bool deliver_direct);

result_t<void> submit_send (std::shared_ptr<connector_state_t> state, packet_t packet)
{
    std::lock_guard<std::mutex> lock (state->transport_mutex);
    if (state->close_requested.load ()) {
        return result_t<void>::failure (error_code_t::closed, "stream connector is closed");
    }
    if (!is_transport_connected (*state)) {
        return result_t<void>::failure (error_code_t::disconnected,
                                        "stream connector is not connected");
    }
    if (auto validation = validate_packet_limits (*state, packet); !validation) {
        publish_error (*state, *validation.error ());
        return validation;
    }
    if (auto written = write_packet_frame (*state, message_kind_t::send, packet, std::nullopt);
        !written) {
        publish_error (*state, *written.error ());
        return written;
    }
    state->sent_packets.push_back (std::move (packet));
    return result_t<void>::success ();
}

void submit_send_async (std::shared_ptr<connector_state_t> state,
                        packet_t packet,
                        std::function<void (result_t<void>)> callback)
{
    {
        std::lock_guard<std::mutex> lock (state->transport_mutex);
        state->pending_sends.push_back (pending_send_t{std::move (packet), std::move (callback)});
    }
    start_next_async_send (std::move (state));
}

result_t<request_reply_t> submit_request (std::shared_ptr<void> state_handle,
                                          packet_t packet,
                                          std::chrono::milliseconds timeout)
{
    auto state = std::static_pointer_cast<connector_state_t> (std::move (state_handle));
    bool use_async_request_pump = false;
    {
        std::lock_guard<std::mutex> lock (state->transport_mutex);
        use_async_request_pump =
          state->connect_started || state->read_in_progress || !state->pending_waits.empty ();
    }
    if (use_async_request_pump) {
        auto promise = std::make_shared<std::promise<result_t<request_reply_t>>> ();
        auto future = promise->get_future ();
        submit_request_async (
          state, std::move (packet), timeout,
          [promise] (result_t<request_reply_t> result) mutable {
              promise->set_value (std::move (result));
          },
          /*deliver_direct=*/true);
        return future.get ();
    }
    std::unique_lock<std::mutex> lock (state->transport_mutex);
    if (state->close_requested.load ()) {
        return result_t<request_reply_t>::failure (error_code_t::closed,
                                                   "stream connector is closed");
    }
    if (!is_transport_connected (*state)) {
        return result_t<request_reply_t>::failure (error_code_t::disconnected,
                                                   "stream connector is not connected");
    }
    if (auto validation = validate_packet_limits (*state, packet); !validation) {
        publish_error (*state, *validation.error ());
        return result_t<request_reply_t>::failure (
          validation.error_code (),
          validation.error () ? validation.error ()->message : "stream request validation failed");
    }
    const auto seq = state->next_request_seq++;
    state->pending_requests.emplace (seq, pending_request_t{seq, std::move (packet)});
    const auto &request_packet = state->pending_requests.at (seq).packet;
    const std::string request_packet_name = request_packet.name;
    trace_request ("submit", seq, request_packet_name, "mode=sync");
    if (auto written = write_packet_frame (*state, message_kind_t::request, request_packet, seq);
        !written) {
        trace_request ("request-write-completion", seq, request_packet_name,
                       "result=failure error="
                         + std::to_string (static_cast<int> (written.error_code ())));
        publish_error (*state, *written.error ());
        state->pending_requests.erase (seq);
        return result_t<request_reply_t>::failure (written.error_code (),
                                                   written.error ()->message);
    }
    trace_request ("request-write-completion", seq, request_packet_name, "result=success");
    std::deque<std::function<void ()>> deliveries;
    auto complete_request = [&lock, &deliveries] (result_t<request_reply_t> result) mutable {
        lock.unlock ();
        while (!deliveries.empty ()) {
            auto delivery = std::move (deliveries.front ());
            deliveries.pop_front ();
            if (delivery) {
                delivery ();
            }
        }
        return result;
    };
    const auto deadline = steady_clock_t::now () + timeout;
    for (;;) {
        auto received = read_inbound_frame (state, deadline);
        if (!received) {
            state->pending_requests.erase (seq);
            trace_request ("pending-complete", seq, request_packet_name,
                           "result=failure error="
                             + std::to_string (static_cast<int> (received.error_code ())));
            return complete_request (result_t<request_reply_t>::failure (
              received.error_code (), received.error ()->message));
        }
        auto frame = std::move (received.value ());
        trace_request ("read-dispatch", frame.request_seq, frame.packet.name,
                       std::string ("kind=") + message_kind_name (frame.kind));
        /* graceful-drain-handoff §7.2: 응답을 기다리는 동안 도착한 server liveness ping에 바로
         * 답한다. pong을 dispatch() 경로에만 두면 응답이 heartbeat 창보다 오래 걸리는 정상
         * 요청에서 서버가 세션을 heartbeat timeout으로 끊는다. 이 루프는 transport mutex를 쥔
         * 동기 전송 문맥이므로 동기 write가 맞다. */
        (void) send_due_pong (*state);
        if (frame.kind == message_kind_t::response && frame.request_seq == seq) {
            state->pending_requests.erase (seq);
            trace_request ("pending-complete", seq, request_packet_name, "result=success");
            return complete_request (result_t<request_reply_t>::success (
              request_reply_t{frame.packet.codec, std::move (frame.packet.payload)}));
        }
        if (frame.kind == message_kind_t::error && frame.request_seq == seq) {
            state->pending_requests.erase (seq);
            auto remote_error = decode_remote_error_message (frame.packet);
            if (remote_error) {
                trace_request ("pending-complete", seq, request_packet_name,
                               "result=failure error=remote");
                return complete_request (result_t<request_reply_t>::failure (
                  error_code_t::remote_error, std::move (remote_error.value ())));
            }
            trace_request ("pending-complete", seq, request_packet_name,
                           "result=failure error=invalid-remote-error");
            return complete_request (result_t<request_reply_t>::failure (
              remote_error.error_code (), remote_error.error ()->message));
        }
        if (frame.kind == message_kind_t::response || frame.kind == message_kind_t::error) {
            continue;
        }
        auto packet = std::move (frame.packet);
        if (auto wait = take_matching_wait (*state, packet)) {
            deliveries.push_back (
              [wait = std::move (*wait), packet = std::move (packet)] () mutable {
                  if (wait.callback) {
                      wait.callback (result_t<packet_t>::success (std::move (packet)));
                  }
              });
            continue;
        }
        (void) enqueue_received_message (*state, std::move (packet));
        continue;
    }
}

void submit_request_async (std::shared_ptr<void> state_handle,
                           packet_t packet,
                           std::chrono::milliseconds timeout,
                           std::function<void (result_t<request_reply_t>)> callback,
                           bool deliver_direct)
{
    if (!state_handle) {
        if (callback) {
            callback (result_t<request_reply_t>::failure (error_code_t::configuration_error,
                                                          "request call has no connector"));
        }
        return;
    }

    auto state = std::static_pointer_cast<connector_state_t> (std::move (state_handle));
    std::uint64_t seq = 0;
    std::vector<std::uint8_t> outbound_frame;
    std::optional<result_t<request_reply_t>> immediate_result;
    std::string request_packet_name;
    {
        std::lock_guard<std::mutex> lock (state->transport_mutex);
        if (state->close_requested.load ()) {
            immediate_result = result_t<request_reply_t>::failure (error_code_t::closed,
                                                                   "stream connector is closed");
        } else if (!is_transport_connected (*state)) {
            immediate_result = result_t<request_reply_t>::failure (
              error_code_t::disconnected, "stream connector is not connected");
        } else if (auto validation = validate_packet_limits (*state, packet); !validation) {
            publish_error (*state, *validation.error ());
            immediate_result = result_t<request_reply_t>::failure (
              validation.error_code (), validation.error () ? validation.error ()->message
                                                            : "stream request validation failed");
        } else {
            seq = state->next_request_seq++;
            request_packet_name = packet.name;
            trace_request ("submit", seq, request_packet_name, "mode=async");
            auto timeout_timer =
              post_runtime_operation_after (timeout, [state, seq, request_packet_name] {
                  trace_request ("request-timeout", seq, request_packet_name);
                  complete_pending_request (
                    state, seq,
                    result_t<request_reply_t>::failure (error_code_t::request_timeout,
                                                        "stream connector request timed out"));
              });
            state->pending_requests.emplace (
              seq,
              pending_request_t{seq, packet, std::move (callback), timeout_timer, deliver_direct});
            if (auto encoded = encode_packet_frame (*state, message_kind_t::request, packet, seq);
                !encoded) {
                auto found = state->pending_requests.find (seq);
                if (found != state->pending_requests.end ()) {
                    cancel_timer (found->second.timeout_timer);
                    callback = std::move (found->second.callback);
                    state->pending_requests.erase (found);
                }
                immediate_result = result_t<request_reply_t>::failure (
                  encoded.error_code (),
                  encoded.error () ? encoded.error ()->message : "stream request encode failed");
            } else {
                outbound_frame = std::move (encoded.value ());
            }
        }
    }

    if (immediate_result) {
        if (deliver_direct) {
            if (callback) {
                callback (std::move (*immediate_result));
            }
            return;
        }
        schedule_delivery (state, [callback = std::move (callback),
                                   result = std::move (*immediate_result)] () mutable {
            if (callback) {
                callback (std::move (result));
            }
        });
        return;
    }

    schedule_request_pump (state);
    enqueue_async_write (
      state, std::move (outbound_frame),
      [state, seq, request_packet_name] (result_t<void> written) mutable {
          trace_request ("request-write-completion", seq, request_packet_name,
                         written ? "result=success"
                                 : "result=failure error="
                                     + std::to_string (static_cast<int> (written.error_code ())));
          if (!written) {
              complete_pending_request (
                state, seq,
                result_t<request_reply_t>::failure (
                  written.error_code (),
                  written.error () ? written.error ()->message : "stream request write failed"));
              return;
          }
          schedule_request_pump (state);
      });
}

result_t<std::vector<packet_t>>
drain_sync_available_pushes (const std::shared_ptr<connector_state_t> &state,
                             std::shared_ptr<stream_connection_t> &connection)
{
    {
        std::lock_guard<std::mutex> lock (state->transport_mutex);
        if (state->close_requested.load () || !is_transport_connected (*state)
            || state->read_in_progress) {
            return result_t<std::vector<packet_t>>::success ({});
        }
        state->read_in_progress = true;
        connection = state->connection;
    }

    // `available()` and the following reads are serialized by the transport's
    // own strand. They must run without transport_mutex: a transport
    // completion can otherwise wait for this state mutex while this caller is
    // waiting for the transport strand.
    auto result = drain_available_pushes (*state, connection);
    {
        std::lock_guard<std::mutex> lock (state->transport_mutex);
        if (state->connection == connection) {
            state->read_in_progress = false;
        }
        state->state_changed.notify_all ();
    }
    return result;
}

result_t<void> dispatch_pending (std::shared_ptr<connector_state_t> state)
{
    std::deque<std::function<void ()>> deliveries;
    std::deque<std::function<void ()>> packet_deliveries;
    std::optional<error_t> inbound_error;
    std::shared_ptr<stream_connection_t> inbound_error_connection;
    std::shared_ptr<stream_connection_t> sync_connection;
    auto drained = drain_sync_available_pushes (state, sync_connection);
    {
        std::lock_guard<std::mutex> lock (state->transport_mutex);
        if (!drained) {
            inbound_error = error_t{
              drained.error_code (),
              drained.error () ? drained.error ()->message : "stream connector frame read failed"};
            inbound_error_connection = sync_connection;
        } else {
            for (auto &packet : drained.value ()) {
                if (!is_control_packet (packet)) {
                    (void) enqueue_received_message (*state, std::move (packet));
                }
            }
        }
        if (!inbound_error) {
            inbound_error = take_inbound_error (*state);
            if (inbound_error) {
                inbound_error_connection = state->connection;
            }
        }
        std::deque<packet_t> packets;
        packets.swap (state->dispatch_queue);
        while (!packets.empty ()) {
            auto packet = std::move (packets.front ());
            packets.pop_front ();
            if (auto wait = take_matching_wait (*state, packet)) {
                packet_deliveries.push_back (
                  [wait = std::move (*wait), packet = std::move (packet)] () mutable {
                      if (wait.callback) {
                          wait.callback (result_t<packet_t>::success (std::move (packet)));
                      }
                  });
            } else {
                packet_deliveries.push_back (
                  [state, packet = std::move (packet)] { dispatch_packet (*state, packet); });
            }
        }
    }
    if (inbound_error) {
        publish_error (*state, *inbound_error);
        change_state (state, connection_state_t::disconnected, *inbound_error);
        if (inbound_error_connection) {
            inbound_error_connection->shutdown_and_close_async ();
        }
        return result_t<void>::failure (inbound_error->code, inbound_error->message);
    }
    queue_due_pong (state);
    {
        std::lock_guard<std::mutex> lock (state->delivery_mutex);
        deliveries.swap (state->delivery_queue);
    }
    while (!packet_deliveries.empty ()) {
        deliveries.push_back (std::move (packet_deliveries.front ()));
        packet_deliveries.pop_front ();
    }
    while (!deliveries.empty ()) {
        auto delivery = std::move (deliveries.front ());
        deliveries.pop_front ();
        if (delivery) {
            delivery ();
        }
    }
    return result_t<void>::success ();
}

result_t<packet_t> receive_next (std::shared_ptr<connector_state_t> state,
                                 std::chrono::milliseconds timeout)
{
    const auto deadline = steady_clock_t::now () + timeout;
    for (;;) {
        std::optional<error_t> inbound_error;
        std::shared_ptr<stream_connection_t> inbound_error_connection;
        std::shared_ptr<stream_connection_t> sync_connection;
        auto drained = drain_sync_available_pushes (state, sync_connection);
        {
            std::unique_lock<std::mutex> lock (state->transport_mutex);
            if (!drained) {
                inbound_error = error_t{
                  drained.error_code (),
                  drained.error () ? drained.error ()->message
                                    : "stream connector frame read failed"};
                inbound_error_connection = sync_connection;
            } else {
                for (auto &packet : drained.value ()) {
                    if (!is_control_packet (packet)) {
                        (void) enqueue_received_message (*state, std::move (packet));
                    }
                }
            }
            if (!inbound_error) {
                inbound_error = take_inbound_error (*state);
                if (inbound_error) {
                    inbound_error_connection = state->connection;
                }
            }
            if (!inbound_error) {
                if (!state->dispatch_queue.empty ()) {
                    auto packet = std::move (state->dispatch_queue.front ());
                    state->dispatch_queue.pop_front ();
                    return result_t<packet_t>::success (std::move (packet));
                }
                if (!is_transport_connected (*state)) {
                    if (state->close_requested.load ()) {
                        return result_t<packet_t>::failure (error_code_t::closed,
                                                            "stream connector is closed");
                    }
                    if (state->last_disconnect_error) {
                        return result_t<packet_t>::failure (state->last_disconnect_error->code,
                                                            state->last_disconnect_error->message);
                    }
                    return result_t<packet_t>::failure (error_code_t::disconnected,
                                                        "stream connector is not connected");
                }
                const auto next_check =
                  std::min (deadline, steady_clock_t::now () + std::chrono::milliseconds (1));
                state->state_changed.wait_until (lock, next_check, [&] {
                    return !state->dispatch_queue.empty () || !is_transport_connected (*state);
                });
            }
        }

        if (inbound_error) {
            publish_error (*state, *inbound_error);
            change_state (state, connection_state_t::disconnected, *inbound_error);
            if (inbound_error_connection) {
                inbound_error_connection->shutdown_and_close_async ();
            }
            return result_t<packet_t>::failure (inbound_error->code, inbound_error->message);
        }

        queue_due_pong (state);

        if (steady_clock_t::now () >= deadline) {
            return result_t<packet_t>::failure (error_code_t::request_timeout,
                                                "stream connector receive timed out");
        }
    }
}

result_t<packet_t> wait_for_packet (std::shared_ptr<connector_state_t> state,
                                    std::string packet_name,
                                    std::function<bool (const packet_t &)> predicate,
                                    std::chrono::milliseconds timeout)
{
    const auto deadline = steady_clock_t::now () + timeout;
    for (;;) {
        std::optional<error_t> inbound_error;
        std::shared_ptr<stream_connection_t> inbound_error_connection;
        std::shared_ptr<stream_connection_t> sync_connection;
        auto drained = drain_sync_available_pushes (state, sync_connection);
        {
            std::unique_lock<std::mutex> lock (state->transport_mutex);
            if (!drained) {
                inbound_error = error_t{
                  drained.error_code (),
                  drained.error () ? drained.error ()->message
                                    : "stream connector frame read failed"};
                inbound_error_connection = sync_connection;
            } else {
                for (auto &packet : drained.value ()) {
                    if (!is_control_packet (packet)) {
                        (void) enqueue_received_message (*state, std::move (packet));
                    }
                }
            }
            if (!inbound_error) {
                inbound_error = take_inbound_error (*state);
                if (inbound_error) {
                    inbound_error_connection = state->connection;
                }
            }
            if (!inbound_error) {
                for (auto iter = state->dispatch_queue.begin (); iter != state->dispatch_queue.end ();
                     ++iter) {
                    if ((packet_name.empty () || iter->name == packet_name)
                        && (!predicate || predicate (*iter))) {
                        auto packet = std::move (*iter);
                        state->dispatch_queue.erase (iter);
                        return result_t<packet_t>::success (std::move (packet));
                    }
                }
                if (!is_transport_connected (*state)) {
                    if (state->close_requested.load ()) {
                        return result_t<packet_t>::failure (error_code_t::closed,
                                                            "stream connector is closed");
                    }
                    if (state->last_disconnect_error) {
                        return result_t<packet_t>::failure (state->last_disconnect_error->code,
                                                            state->last_disconnect_error->message);
                    }
                    return result_t<packet_t>::failure (error_code_t::disconnected,
                                                        "stream connector is not connected");
                }
                const auto next_check =
                  std::min (deadline, steady_clock_t::now () + std::chrono::milliseconds (1));
                state->state_changed.wait_until (lock, next_check, [&] {
                    return !state->dispatch_queue.empty () || !is_transport_connected (*state);
                });
            }
        }

        if (inbound_error) {
            publish_error (*state, *inbound_error);
            change_state (state, connection_state_t::disconnected, *inbound_error);
            if (inbound_error_connection) {
                inbound_error_connection->shutdown_and_close_async ();
            }
            return result_t<packet_t>::failure (inbound_error->code, inbound_error->message);
        }

        queue_due_pong (state);

        if (steady_clock_t::now () >= deadline) {
            return result_t<packet_t>::failure (error_code_t::request_timeout,
                                                "stream connector wait timed out");
        }
    }
}

result_t<packet_t> submit_wait (std::shared_ptr<void> state,
                                std::string packet_name,
                                std::function<bool (const packet_t &)> predicate,
                                std::chrono::milliseconds timeout)
{
    if (!state) {
        return result_t<packet_t>::failure (error_code_t::configuration_error,
                                            "wait call has no connector");
    }
    return wait_for_packet (std::static_pointer_cast<connector_state_t> (std::move (state)),
                            std::move (packet_name), std::move (predicate), timeout);
}

void submit_wait_async (std::shared_ptr<void> state_handle,
                        std::string packet_name,
                        std::function<bool (const packet_t &)> predicate,
                        std::chrono::milliseconds timeout,
                        std::function<void (result_t<packet_t>)> callback)
{
    if (!state_handle) {
        if (callback) {
            callback (result_t<packet_t>::failure (error_code_t::configuration_error,
                                                   "wait call has no connector"));
        }
        return;
    }

    auto state = std::static_pointer_cast<connector_state_t> (state_handle);
    std::optional<packet_t> matched_packet;
    std::optional<error_t> inbound_error;
    std::shared_ptr<stream_connection_t> inbound_error_connection;
    std::uint64_t wait_id = 0;
    {
        std::lock_guard<std::mutex> lock (state->transport_mutex);
        /* The asynchronous read pump owns transport reads after connect. Do
         * not synchronously query the socket while holding transport_mutex:
         * the query is serialized on the transport strand and may need an I/O
         * worker that is waiting for this mutex. */
        inbound_error = take_inbound_error (*state);
        if (inbound_error) {
            inbound_error_connection = state->connection;
        }
        for (auto iter = state->dispatch_queue.begin (); iter != state->dispatch_queue.end ();
             ++iter) {
            if ((packet_name.empty () || iter->name == packet_name)
                && (!predicate || predicate (*iter))) {
                matched_packet = std::move (*iter);
                state->dispatch_queue.erase (iter);
                break;
            }
        }
        if (!matched_packet && !inbound_error) {
            if (state->close_requested.load ()) {
                matched_packet = packet_t{};
            } else {
                wait_id = state->next_wait_id++;
                state->pending_waits.emplace (
                  wait_id, pending_wait_t{wait_id, std::move (packet_name), std::move (predicate),
                                          std::move (callback)});
            }
        }
    }

    if (inbound_error) {
        publish_error (*state, *inbound_error);
        change_state (state, connection_state_t::disconnected, *inbound_error);
        if (inbound_error_connection) {
            inbound_error_connection->shutdown_and_close_async ();
        }
    }

    if (wait_id != 0) {
        start_read_loop (state);
    }

    if (inbound_error) {
        schedule_delivery (
          state, [callback = std::move (callback), error = std::move (*inbound_error)] () mutable {
              if (callback) {
                  callback (result_t<packet_t>::failure (error.code, error.message));
              }
          });
        return;
    }

    if (matched_packet) {
        if (state->close_requested.load () && matched_packet->name.empty ()) {
            schedule_delivery (state, [callback = std::move (callback)] () mutable {
                if (callback) {
                    callback (result_t<packet_t>::failure (error_code_t::closed,
                                                           "stream connector is closed"));
                }
            });
        } else {
            schedule_delivery (state, [callback = std::move (callback),
                                       packet = std::move (*matched_packet)] () mutable {
                if (callback) {
                    callback (result_t<packet_t>::success (std::move (packet)));
                }
            });
        }
        return;
    }

    auto timeout_timer = post_runtime_operation_after (timeout, [state, wait_id] {
        std::function<void (result_t<packet_t>)> callback;
        {
            std::lock_guard<std::mutex> lock (state->transport_mutex);
            auto found = state->pending_waits.find (wait_id);
            if (found == state->pending_waits.end ()) {
                return;
            }
            callback = std::move (found->second.callback);
            state->pending_waits.erase (found);
        }
        schedule_delivery (state, [callback = std::move (callback)] () mutable {
            if (callback) {
                callback (result_t<packet_t>::failure (error_code_t::request_timeout,
                                                       "stream connector wait timed out"));
            }
        });
    });
    {
        std::lock_guard<std::mutex> lock (state->transport_mutex);
        auto found = state->pending_waits.find (wait_id);
        if (found != state->pending_waits.end ()) {
            found->second.timeout_timer = timeout_timer;
        } else if (timeout_timer) {
            cancel_timer (timeout_timer);
        }
    }
}

} // namespace zlink::stream_connector::detail
