/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/connector_runtime.hpp"

#include "runtime/protocol/compression/lz4_compression_codec.hpp"
#include "runtime/protocol/framing.hpp"
#include "runtime/protocol/framing/frame_codec.hpp"
#include "runtime/protocol/header_codec.hpp"
#include "runtime/protocol/metadata_codec.hpp"
#include "runtime/transport/stream_connection.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <future>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_set>

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
    // Cheap uint->hex (no ostringstream): this runs per outbound packet, so it
    // must stay light even when tracing is off.
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
    /* Debug-only stderr trace, opt-in via ZLINK_CPP_STREAM_TRACE. */
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
        return result_t<void>::failure (error_code_t::validation_failed,
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
        const auto payload = nlohmann::json::parse (packet.payload.begin (), packet.payload.end ());
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

bool has_flag (header_flags_t flags, header_flags_t flag) noexcept
{
    return (static_cast<std::uint8_t> (flags) & static_cast<std::uint8_t> (flag)) != 0;
}

using steady_clock_t = std::chrono::steady_clock;

struct inbound_frame_t
{
    message_kind_t kind = message_kind_t::send;
    std::optional<std::uint64_t> request_seq;
    bool reply_to_pending = false;
    dispatch_envelope_t envelope;
    /* Set when only this packet failed and the connection stays (§9:
   * DecompressionFailed). */
    std::optional<error_t> packet_error;
};

result_t<dispatch_envelope_t> decode_packet (connector_state_t &state,
                                             const stream_header_t &header,
                                             std::vector<std::uint8_t> payload_bytes)
{
    auto payload = std::move (payload_bytes);
    std::function<void ()> actor_event;
    const bool compressed = has_flag (header.flags, header_flags_t::payload_compressed);
    if (compressed) {
        if (!state.compression_codec) {
            return result_t<dispatch_envelope_t>::failure (
              error_code_t::decompression_failed,
              "stream connector compression codec is not configured");
        }
        if (state.options.compression == compression_t::lz4 && !state.lz4_enabled) {
            return result_t<dispatch_envelope_t>::failure (error_code_t::decompression_failed,
                                                           "LZ4 compression is not enabled");
        }
        /* stream-connector §4.7: a payload over the receive limit after
     * decompression is FrameTooLarge, whether the codec refuses it
     * (std::length_error) or returns it. */
        try {
            payload =
              state.compression_codec->decompress (payload, state.options.max_receive_payload_size);
        }
        catch (const std::length_error &ex) {
            return result_t<dispatch_envelope_t>::failure (error_code_t::frame_too_large,
                                                           ex.what ());
        }
        catch (const std::exception &ex) {
            return result_t<dispatch_envelope_t>::failure (error_code_t::decompression_failed,
                                                           ex.what ());
        }
        if (payload.size () > state.options.max_receive_payload_size) {
            return result_t<dispatch_envelope_t>::failure (
              error_code_t::frame_too_large,
              "decompressed stream payload exceeds maximum stream payload size");
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
        auto closing = session_closing_codec_t::decode (payload);
        if (!closing) {
            return result_t<dispatch_envelope_t>::failure (
              closing.error_code ().value_or (error_code_t::frame_decode_failed),
              closing.error ()->message);
        }
        state.pending_close_reason = closing.value ().reason;
    }
    if (header.kind == message_kind_t::control
        && header.name == actor_binding_control_codec_t::bound_name) {
        auto bound = actor_binding_control_codec_t::decode_bound (payload);
        if (!bound)
            return result_t<dispatch_envelope_t>::failure (bound.error ()->code,
                                                           bound.error ()->message);
        std::shared_ptr<actor_t> actor;
        std::vector<std::uint64_t> handler_ids;
        {
            std::lock_guard<std::mutex> lock (state.lifecycle_mutex);
            if (std::any_of (
                  state.actors_by_slot.begin (), state.actors_by_slot.end (),
                  [&] (const auto &entry) { return entry.first == bound.value ().actor_slot; })
                || state.actors_by_id.contains (bound.value ().actor_id)) {
                return result_t<dispatch_envelope_t>::failure (
                  error_code_t::frame_decode_failed, "Actor bound control is duplicated.");
            }
            actor = actor_access_t::create (state.shared_from_this (), bound.value ().actor_id,
                                            bound.value ().actor_slot);
            state.actors_by_slot.emplace_back (bound.value ().actor_slot, actor);
            state.actors_by_id.emplace (bound.value ().actor_id, actor);
            handler_ids = registered_handler_ids_locked (state.actor_bound_handlers);
        }
        actor_event = [connector_state = state.shared_from_this (),
                       handler_ids = std::move (handler_ids), actor] () mutable {
            schedule_actor_delivery (connector_state, &connector_state_t::actor_bound_handlers,
                                     std::move (handler_ids), std::move (actor));
        };
    }
    if (header.kind == message_kind_t::control
        && header.name == actor_binding_control_codec_t::unbound_name) {
        auto slot = actor_binding_control_codec_t::decode_unbound (payload);
        if (!slot)
            return result_t<dispatch_envelope_t>::failure (slot.error ()->code,
                                                           slot.error ()->message);
        std::shared_ptr<actor_t> actor;
        std::vector<std::uint64_t> handler_ids;
        {
            std::lock_guard<std::mutex> lock (state.lifecycle_mutex);
            const auto found =
              std::find_if (state.actors_by_slot.begin (), state.actors_by_slot.end (),
                            [&] (const auto &entry) { return entry.first == slot.value (); });
            if (found == state.actors_by_slot.end ()) {
                return result_t<dispatch_envelope_t>::failure (
                  error_code_t::frame_decode_failed, "Actor unbound slot is not registered.");
            }
            actor = found->second;
            actor_access_t::close (actor);
            state.actors_by_id.erase (actor->actor_id ());
            state.actors_by_slot.erase (found);
            handler_ids = registered_handler_ids_locked (state.actor_unbound_handlers);
        }
        actor_event = [connector_state = state.shared_from_this (),
                       handler_ids = std::move (handler_ids), actor] () mutable {
            schedule_actor_delivery (connector_state, &connector_state_t::actor_unbound_handlers,
                                     std::move (handler_ids), std::move (actor));
        };
    }
    packet_t packet{header.name, header.metadata, header.codec, compressed, payload};
    std::optional<std::uint16_t> actor_slot;
    if (header.actor_slot) {
        std::lock_guard<std::mutex> lock (state.lifecycle_mutex);
        const auto actor =
          std::find_if (state.actors_by_slot.begin (), state.actors_by_slot.end (),
                        [&] (const auto &entry) { return entry.first == *header.actor_slot; });
        if (actor == state.actors_by_slot.end ()) {
            return result_t<dispatch_envelope_t>::failure (error_code_t::frame_decode_failed,
                                                           "Packet Actor slot is not registered.");
        }
        packet.actor_id = actor->second->actor_id ();
        actor_slot = *header.actor_slot;
    }
    dispatch_envelope_t envelope{std::move (packet), actor_slot};
    envelope.actor_event = std::move (actor_event);
    return result_t<dispatch_envelope_t>::success (std::move (envelope));
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

/* Builds one outbound frame. Callers must run this with transport_mutex
 * released: LZ4 compression of a large payload takes as long as the payload is
 * big, and the read pump needs that same lock to make progress. It is safe to
 * run unlocked because it reads only state that is fixed at construction
 * (options, compression_codec), and mutates
 * nothing in connector_state_t. The packet it is handed must be owned by the
 * caller, not borrowed from a container the lock protects. */
result_t<std::vector<std::uint8_t>>
encode_packet_frame (connector_state_t &state,
                     message_kind_t kind,
                     const packet_t &packet,
                     std::optional<std::uint64_t> request_seq,
                     const std::optional<actor_binding_ref_t> &actor_binding = std::nullopt)
{
    header_flags_t flags = header_flags_t::none;
    if (packet.compressed) {
        flags = flags | header_flags_t::payload_compressed;
    }
    header_codec_t header_codec;
    stream_header_t header_data{kind,        packet.codec, flags,
                                request_seq, packet.name,  packet.metadata};
    if (actor_binding) {
        if (!actor_binding->bound || !actor_binding->bound->load (std::memory_order_acquire)) {
            return result_t<std::vector<std::uint8_t>>::failure (error_code_t::validation_failed,
                                                                 "Actor handle is not bound.");
        }
        header_data.actor_slot = actor_binding->slot;
    }
    if (kind == message_kind_t::request) {
        /* correlation_id links a request to its terminal reply. */
        header_data.correlation_id = next_correlation_id ();
    }
    auto header = header_codec.encode (header_data);
    if (!header) {
        return result_t<std::vector<std::uint8_t>>::failure (header.error ()->code,
                                                             header.error ()->message);
    }
    const std::vector<std::uint8_t> *payload_message = &packet.payload;
    std::optional<std::vector<std::uint8_t>> compressed_payload;
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
    auto frame = frame_codec_t::encode (header.value (), *payload_message, state.options);
    if (!frame) {
        return result_t<std::vector<std::uint8_t>>::failure (frame.error ()->code,
                                                             frame.error ()->message);
    }
    return result_t<std::vector<std::uint8_t>>::success (std::move (frame.value ()));
}

/* Name matching only. The predicate half is deliberately absent: it is user
 * code and must not run under transport_mutex. */
bool wait_name_matches (const pending_wait_t &wait, const packet_t &packet)
{
    return wait.packet_name.empty () || wait.packet_name == packet.name;
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
    }
    catch (const boost::system::system_error &) {
    }
}

/* stream-connector §7·§10.1.1: every result of an asynchronous wait - a match
 * of a queued packet, a match of a newly arrived packet, the timeout and the
 * release when the connection ends - runs on the delivery strand in both
 * dispatch modes. A wait is not a registered callback, so Manual needs no pump
 * for it; and it never runs on the read pump, so a wait callback that makes a
 * synchronous request does not hold the reads that complete that request. The
 * synchronous wait_for alone resolves its promise where the result is decided
 * (pending_wait_t::deliver_direct). */
void deliver_wait_result (const std::shared_ptr<connector_state_t> &state,
                          pending_wait_t wait,
                          result_t<packet_t> result)
{
    auto callback = std::move (wait.callback);
    if (!callback) {
        return;
    }
    if (wait.deliver_direct) {
        callback (std::move (result));
        return;
    }
    run_on_delivery_strand (
      state, [callback = std::move (callback), result = std::move (result)] () mutable {
          callback (std::move (result));
      });
}

/* Removes a registered wait under the lock and hands it to the caller. */
std::optional<pending_wait_t> claim_wait (connector_state_t &state, std::uint64_t wait_id)
{
    auto found = state.pending_waits.find (wait_id);
    if (found == state.pending_waits.end ()) {
        return std::nullopt;
    }
    auto wait = std::move (found->second);
    cancel_timer (wait.timeout_timer);
    state.pending_waits.erase (found);
    ++state.pending_waits_version;
    return wait;
}

/* Finds the registered wait that claims this packet and removes it.
 *
 * `lock` must own state.transport_mutex on entry and owns it again on return.
 * A wait predicate is user code: it can call back into the connector surface
 * (send, request, pending_dispatch_count, ...), and every one of those takes
 * transport_mutex. std::mutex is not recursive, so running the predicate under
 * the lock is undefined behaviour and in practice a deadlock. The predicates
 * of the name-matching candidates are therefore copied out, the lock is
 * released for the evaluation, and the decision is revalidated against
 * pending_waits_version afterwards - the shape ZLinkStreamDispatchQueue
 * .addMessage already uses on the Java side. */
std::optional<pending_wait_t> take_matching_wait (connector_state_t &state,
                                                  std::unique_lock<std::mutex> &lock,
                                                  const packet_t &packet)
{
    struct candidate_t
    {
        std::uint64_t wait_id = 0;
        std::function<bool (const packet_t &)> predicate;
    };
    for (;;) {
        const auto observed_version = state.pending_waits_version;
        std::vector<candidate_t> candidates;
        for (const auto &[wait_id, wait] : state.pending_waits) {
            if (!wait_name_matches (wait, packet)) {
                continue;
            }
            if (!wait.predicate) {
                /* Matches without running user code, and it precedes every
         * candidate still unevaluated, so take it under the lock. */
                if (candidates.empty ()) {
                    return claim_wait (state, wait_id);
                }
                candidates.push_back (candidate_t{wait_id, {}});
                continue;
            }
            candidates.push_back (candidate_t{wait_id, wait.predicate});
        }
        if (candidates.empty ()) {
            return std::nullopt;
        }

        lock.unlock ();
        std::optional<std::uint64_t> matched;
        for (const auto &candidate : candidates) {
            if (!candidate.predicate || candidate.predicate (packet)) {
                matched = candidate.wait_id;
                break;
            }
        }
        lock.lock ();

        if (matched) {
            if (auto wait = claim_wait (state, *matched)) {
                return wait;
            }
            /* The wait was completed or cancelled while the lock was down.
       * Re-read the set and decide again. */
            continue;
        }
        if (state.pending_waits_version != observed_version) {
            continue;
        }
        return std::nullopt;
    }
}

/* Takes the first queued packet a wait accepts, without running the predicate
 * under transport_mutex.
 *
 * `lock` must own state.transport_mutex on entry and owns it again on return.
 * dispatch_queue is moved out wholesale for the evaluation and the packets
 * that were not taken are pushed back to the front, so the queue keeps FIFO
 * order against anything that arrived while the lock was down. */
std::optional<packet_t>
take_matching_queued_packet (connector_state_t &state,
                             std::unique_lock<std::mutex> &lock,
                             const std::string &packet_name,
                             const std::function<bool (const packet_t &)> &predicate)
{
    const auto name_matches = [&packet_name] (const dispatch_envelope_t &envelope) {
        return packet_name.empty () || envelope.packet.name == packet_name;
    };
    if (!predicate) {
        for (auto iter = state.dispatch_queue.begin (); iter != state.dispatch_queue.end ();
             ++iter) {
            if (name_matches (*iter)) {
                auto packet = std::move (*iter);
                state.dispatch_queue.erase (iter);
                return std::move (packet.packet);
            }
        }
        return std::nullopt;
    }
    if (state.dispatch_queue.empty ()) {
        return std::nullopt;
    }

    const auto observed_generation = state.dispatch_queue_generation;
    std::deque<dispatch_envelope_t> candidates;
    candidates.swap (state.dispatch_queue);

    /* The queue reads empty to anyone who looks while the predicate runs, the
   * same way it does while dispatch() delivers a batch. */
    std::optional<packet_t> matched;
    std::deque<dispatch_envelope_t> untaken;
    lock.unlock ();
    for (auto &candidate : candidates) {
        if (!matched && name_matches (candidate) && predicate (candidate.packet)) {
            matched = std::move (candidate.packet);
            continue;
        }
        untaken.push_back (std::move (candidate));
    }
    lock.lock ();

    if (state.dispatch_queue_generation != observed_generation) {
        /* A new connection or a close dropped the queue while the predicate
     * ran. These packets belong to the connection that ended, so they are
     * dropped too (stream-connector §10). */
        return std::nullopt;
    }
    while (!untaken.empty ()) {
        state.dispatch_queue.push_front (std::move (untaken.back ()));
        untaken.pop_back ();
    }
    state.state_changed.notify_all ();
    return matched;
}

std::optional<result_t<inbound_frame_t>>
try_take_inbound_frame (connector_state_t &state,
                        const std::unordered_set<std::uint64_t> &claimed_replies)
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
          limits.error ()->code,
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
        return result_t<inbound_frame_t>::failure (decoded.error ()->code,
                                                   decoded.error ()->message);
    }
    auto header = decoded.value ();
    state.last_inbound_received = steady_clock_t::now ();
    const bool reply_to_pending =
      (header.kind == message_kind_t::response || header.kind == message_kind_t::error)
      && header.request_seq
      && state.pending_requests.find (*header.request_seq) != state.pending_requests.end ()
      && !claimed_replies.contains (*header.request_seq);
    if (header.kind == message_kind_t::response && !reply_to_pending) {
        return result_t<inbound_frame_t>::success (
          inbound_frame_t{header.kind, header.request_seq, false, {}});
    }
    auto packet = decode_inbound_packet (state, header, std::move (payload_bytes));
    /* stream-connector §9: the one place that decides whether a decode failure
   * ends the connection. A payload that fails to decompress fails only its
   * packet; every other decode failure (frame, header, metadata, over the
   * receive limit) is returned as a failure and ends the connection. */
    if (!packet && packet.error ()->code == error_code_t::decompression_failed) {
        return result_t<inbound_frame_t>::success (
          inbound_frame_t{header.kind, header.request_seq, reply_to_pending, {}, *packet.error ()});
    }
    if (!packet) {
        return result_t<inbound_frame_t>::failure (packet.error ()->code, packet.error ()->message);
    }
    return result_t<inbound_frame_t>::success (inbound_frame_t{
      header.kind, header.request_seq, reply_to_pending, std::move (packet.value ())});
}

void kick_async_write (std::shared_ptr<connector_state_t> state, std::string reason);

void complete_pending_request (std::shared_ptr<connector_state_t> state,
                               std::uint64_t request_seq,
                               result_t<request_reply_t> result)
{
    std::function<void (result_t<request_reply_t>)> callback;
    std::shared_ptr<std::vector<std::uint64_t>> reply_hook_ids;
    std::string packet_name;
    bool deliver_direct = false;
    const bool succeeded = static_cast<bool> (result);
    const auto error_code = result ? error_code_t{} : result.error ()->code;
    {
        std::lock_guard<std::mutex> lock (state->transport_mutex);
        auto found = state->pending_requests.find (request_seq);
        if (found == state->pending_requests.end ()) {
            trace_request ("pending-complete-missing", request_seq, {});
            return;
        }
        packet_name = found->second.packet.name;
        callback = std::move (found->second.callback);
        reply_hook_ids = found->second.reply_hook_ids;
        deliver_direct = found->second.deliver_direct;
        cancel_timer (found->second.timeout_timer);
        state->pending_requests.erase (found);
    }
    trace_request ("pending-complete", request_seq, packet_name,
                   succeeded
                     ? "result=success"
                     : "result=failure error=" + std::to_string (static_cast<int> (error_code)));
    if (reply_hook_ids) {
        *reply_hook_ids = capture_reply_hook_ids (state);
    }
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

/* Selects the handlers and consumes one packet in the same dispatch step.
 * A packet with no recipient remains available to a later handler or wait. */
struct selected_packet_t
{
    dispatch_envelope_t envelope;
    std::vector<packet_handler_entry_t> handlers;
};

std::optional<selected_packet_t> take_dispatchable_locked (connector_state_t &state,
                                                           std::uint64_t through_arrival,
                                                           std::uint64_t after_arrival = 0)
{
    for (auto it = state.dispatch_queue.begin (); it != state.dispatch_queue.end (); ++it) {
        if (it->arrival <= after_arrival) {
            continue;
        }
        if (it->arrival > through_arrival) {
            break;
        }
        auto handlers = select_packet_handlers (state, *it);
        if (handlers.empty ()) {
            continue;
        }
        selected_packet_t selected{std::move (*it), std::move (handlers)};
        state.dispatch_queue.erase (it);
        return selected;
    }
    return std::nullopt;
}

/* stream-connector §7·§10: the read pump decides where a received packet goes,
 * in arrival order and in both dispatch modes. A registered wait that accepts
 * it takes it first; its result is delivered by deliver_wait_result. Otherwise
 * the handlers registered for its name decide, each time a dispatch step looks
 * at it: on arrival in Immediate, at each pump in Manual, and in Immediate
 * again when a handler for its name is registered. With a handler the packet
 * goes to it; without one it stays in the receive queue until a handler or a
 * wait surface takes it. */
void route_inbound_packet (std::shared_ptr<connector_state_t> state, dispatch_envelope_t envelope)
{
    if (is_control_packet (envelope.packet)) {
        return;
    }
    std::optional<pending_wait_t> wait;
    {
        std::unique_lock<std::mutex> lock (state->transport_mutex);
        wait = take_matching_wait (*state, lock, envelope.packet);
        /* stream-connector §10: counted in the step that hands the packet to
     * its consumer, so a count never runs ahead of an observable packet. */
        count_received_locked (*state, envelope.packet);
        if (!wait) {
            enqueue_received_message (*state, std::move (envelope));
        }
    }
    if (wait) {
        deliver_wait_result (state, std::move (*wait),
                             result_t<packet_t>::success (std::move (envelope.packet)));
        return;
    }
    if (state->options.dispatch_mode == dispatch_mode_t::immediate) {
        deliver_queued_to_handlers (state);
    }
}

void enqueue_async_write (std::shared_ptr<connector_state_t> state,
                          std::vector<std::uint8_t> frame);

std::chrono::milliseconds heartbeat_maintenance_delay (const heartbeat_options_t &options)
{
    const auto interval = std::max (options.interval, std::chrono::milliseconds (1));
    const auto timeout = std::max (options.timeout, std::chrono::milliseconds (1));
    return std::min (interval, timeout);
}

void schedule_heartbeat_maintenance (const std::shared_ptr<connector_state_t> &state,
                                     std::uint64_t generation);

void run_heartbeat_maintenance (std::shared_ptr<connector_state_t> state, std::uint64_t generation)
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
            timeout_error =
              error_t{error_code_t::disconnected, "stream connector heartbeat timed out"};
        } else if (state->last_heartbeat_sent == steady_clock_t::time_point{}
                   || now - state->last_heartbeat_sent >= state->options.heartbeat.interval) {
            packet_t heartbeat;
            heartbeat.name = "$zlink.heartbeat.ping";
            heartbeat.codec = codec_t::raw;
            heartbeat.payload.clear ();
            auto encoded =
              encode_packet_frame (*state, message_kind_t::control, heartbeat, std::nullopt);
            if (encoded) {
                heartbeat_frame = std::move (encoded.value ());
                state->last_heartbeat_sent = now;
            }
        }
    }
    if (timed_out_connection) {
        if (connection_ended (state, *timeout_error, timed_out_connection)) {
            timed_out_connection->shutdown_and_close_async ();
            schedule_reconnect (state);
        }
        return;
    }
    if (!heartbeat_frame.empty ()) {
        enqueue_async_write (state, std::move (heartbeat_frame));
    }
    schedule_heartbeat_maintenance (state, generation);
}

void schedule_heartbeat_maintenance (const std::shared_ptr<connector_state_t> &state,
                                     std::uint64_t generation)
{
    auto timer = post_runtime_operation_after (
      state, heartbeat_maintenance_delay (state->options.heartbeat),
      [state, generation] { run_heartbeat_maintenance (state, generation); });
    std::lock_guard<std::mutex> lock (state->transport_mutex);
    if (generation != state->heartbeat_generation || state->close_requested.load ()
        || !is_transport_connected (*state)) {
        cancel_timer (timer);
        return;
    }
    state->heartbeat_timer = std::move (timer);
}

/* 수신 콜백(io 스레드) 문맥에서 due pong을 write pump에 싣는다. 이 문맥에서는
 * 동기 write를 쓰면 안 된다. in-flight async_write와 같은 소켓에서 바이트가
 * 섞이고, 상대가 읽지 않으면 io 스레드 자체가 blocking write에 갇힌다. */
void queue_due_pong (const std::shared_ptr<connector_state_t> &state)
{
    {
        std::lock_guard<std::mutex> lock (state->transport_mutex);
        if (!state->heartbeat_pong_due || !is_transport_connected (*state)) {
            return;
        }
        /* Claimed here so a second pass does not encode the same pong twice;
     * restored below if the encode fails, so the next pass retries. */
        state->heartbeat_pong_due = false;
    }
    packet_t pong;
    pong.name = "$zlink.heartbeat.pong";
    pong.codec = codec_t::raw;
    pong.payload.clear ();
    auto encoded = encode_packet_frame (*state, message_kind_t::control, pong, std::nullopt);
    if (!encoded) {
        std::lock_guard<std::mutex> lock (state->transport_mutex);
        state->heartbeat_pong_due = true;
        return;
    }
    enqueue_async_write (state, std::move (encoded.value ()));
}

void process_inbound_buffer (std::shared_ptr<connector_state_t> state,
                             boost::system::error_code read_error,
                             std::vector<std::uint8_t> bytes,
                             const std::shared_ptr<stream_connection_t> &observed_connection)
{
    std::optional<error_t> transport_error;
    std::vector<std::pair<std::uint64_t, result_t<request_reply_t>>> completed_requests;
    std::vector<dispatch_envelope_t> pushed_packets;
    std::vector<error_t> stream_errors;
    std::unordered_set<std::uint64_t> claimed_replies;
    bool reschedule = false;
    {
        std::lock_guard<std::mutex> lock (state->transport_mutex);
        // A close can leave the previous connection's completion queued
        // while a reconnect installs a new connection. That stale completion
        // does not touch the new connection's read state or buffer.
        if (state->connection != observed_connection) {
            return;
        }
        state->read_in_progress = false;
        state->inbound_buffer.insert (state->inbound_buffer.end (), bytes.begin (), bytes.end ());
        state->state_changed.notify_all ();
        if (state->close_requested.load () || !is_transport_connected (*state)) {
            return;
        }
        if (read_error) {
            /* stream-connector §9: a message over the receive limit is FrameTooLarge
       * on every transport; any other read failure is a lost connection. */
            transport_error =
              error_t{read_error == boost::asio::error::message_size ? error_code_t::frame_too_large
                                                                     : error_code_t::disconnected,
                      read_error.message ()};
        }

        // A stream read can report both bytes and EOF. Decode the bytes that
        // arrived before applying the terminal transport error.
        while (true) {
            auto frame = try_take_inbound_frame (*state, claimed_replies);
            if (!frame) {
                break;
            }
            if (!*frame) {
                transport_error = error_t{frame->error ()->code,
                                          frame->error () ? frame->error ()->message
                                                          : "stream connector frame decode failed"};
                break;
            }
            auto value = std::move (frame->value ());
            if (value.reply_to_pending) {
                claimed_replies.insert (*value.request_seq);
            }
            trace_connector_write (
              *state, "read-dispatch",
              "seq=" + (value.request_seq ? std::to_string (*value.request_seq) : std::string ("-"))
                + " name=" + value.envelope.packet.name
                + " kind=" + message_kind_name (value.kind));
            /* §5.2: pending request 매칭은 request_seq가 정본이다. packet name은 대조
       * 조건이 아니므로 이름이 달라도 응답을 버리지 않는다. */
            if (value.packet_error) {
                /* §9 DecompressionFailed: only this packet fails - the pending
         * request it answers, or the error handler for any other packet
         * (a Response that answers nothing is dropped, §5.2). */
                if (value.reply_to_pending) {
                    completed_requests.emplace_back (
                      *value.request_seq, result_t<request_reply_t>::failure (
                                            value.packet_error->code, value.packet_error->message));
                } else if (value.kind != message_kind_t::response) {
                    stream_errors.push_back (*value.packet_error);
                }
            } else if (value.reply_to_pending) {
                if (value.kind == message_kind_t::response) {
                    completed_requests.emplace_back (
                      *value.request_seq, result_t<request_reply_t>::success (
                                            request_reply_t{std::move (value.envelope.packet)}));
                } else if (auto remote_error =
                             decode_remote_error_message (value.envelope.packet)) {
                    completed_requests.emplace_back (
                      *value.request_seq,
                      result_t<request_reply_t>::failure (error_code_t::remote_error,
                                                          std::move (remote_error.value ())));
                } else {
                    completed_requests.emplace_back (
                      *value.request_seq,
                      result_t<request_reply_t>::failure (remote_error.error ()->code,
                                                          remote_error.error ()->message));
                }
            } else if (value.kind == message_kind_t::error) {
                auto remote_error = decode_remote_error_message (value.envelope.packet);
                stream_errors.push_back (remote_error ? error_t{error_code_t::remote_error,
                                                                std::move (remote_error.value ())}
                                                      : *remote_error.error ());
            } else if (value.kind != message_kind_t::response) {
                pushed_packets.push_back (std::move (value.envelope));
            }
        }
        reschedule =
          is_transport_connected (*state) && !state->close_requested.load () && !transport_error;
        if (!reschedule) {
            trace_connector_write (
              *state, "read-pump-stop",
              std::string ("connected=") + (is_transport_connected (*state) ? "true" : "false")
                + " close=" + (state->close_requested.load () ? "true" : "false")
                + " transport_error=" + (transport_error ? "true" : "false"));
        }
    }

    /* graceful-drain-handoff §7.2: server liveness ping의 pong을 application이
   * dispatch()를 부를 때까지 미룰 수 없다. 동기 request가 응답을 기다리는
   * 동안에는 dispatch()가 돌지 않으므로, pong을 그 경로에만 두면 응답이
   * heartbeat timeout보다 오래 걸리는 정상 요청에서도 서버가 세션을 heartbeat
   * timeout으로 끊는다. 수신 프레임을 처리한 직후 바로 답한다. */
    queue_due_pong (state);

    for (auto &error : stream_errors) {
        publish_error (*state, std::move (error));
    }
    for (auto &packet : pushed_packets) {
        if (packet.actor_event) {
            packet.actor_event ();
            continue;
        }
        route_inbound_packet (state, std::move (packet));
    }
    for (auto &[request_seq, result] : completed_requests) {
        complete_pending_request (state, request_seq, std::move (result));
    }
    if (transport_error) {
        // The state transition must precede socket cancellation. The
        // cancellation completion can otherwise overwrite the original
        // protocol or transport error with Operation canceled.
        if (connection_ended (state, *transport_error, observed_connection)) {
            observed_connection->shutdown_and_close_async ();
            schedule_reconnect (state);
        }
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
            trace_connector_write (
              *state, "read-start-skip",
              std::string ("in_progress=") + (state->read_in_progress ? "true" : "false")
                + " close=" + (state->close_requested.load () ? "true" : "false")
                + " connected=" + (is_transport_connected (*state) ? "true" : "false"));
            return;
        }
        state->read_in_progress = true;
        connection = state->connection;
    }
    trace_connector_write (*state, "read-start");
    connection->async_read_some (
      8192, [state, connection] (boost::system::error_code error,
                                 std::vector<std::uint8_t> bytes) mutable {
          trace_connector_write (*state, "read-completion",
                                 error ? "result=failure error=" + error.message ()
                                       : "result=success bytes=" + std::to_string (bytes.size ()));
          process_inbound_buffer (state, error, std::move (bytes), connection);
      });
}

void start_next_async_write (std::shared_ptr<connector_state_t> state);
void kick_async_write (std::shared_ptr<connector_state_t> state, std::string reason);

/* §5.2: an accepted operation takes the next place in the connection's write
 * order. The frames are written one at a time in that order, so an operation
 * beyond the first 4,096 waits behind the ones accepted before it without a
 * second queue: which place counts as "in the queue" and which as "waiting for
 * a place" changes nothing a caller can observe. */
std::uint64_t accept_write_locked (connector_state_t &state, pending_write_t write)
{
    write.write_id = state.next_write_id++;
    const auto write_id = write.write_id;
    state.write_queue.push_back (std::move (write));
    return write_id;
}

std::uint64_t reserve_write_locked (connector_state_t &state,
                                    std::function<void (result_t<void>)> callback)
{
    return accept_write_locked (state, pending_write_t{{}, std::move (callback), 0, false});
}

void finish_reserved_write (std::shared_ptr<connector_state_t> state,
                            std::uint64_t write_id,
                            result_t<std::vector<std::uint8_t>> encoded)
{
    std::function<void (result_t<void>)> callback;
    std::optional<error_t> failure;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock (state->transport_mutex);
        auto &queue = state->write_queue;
        const auto write =
          std::find_if (queue.begin (), queue.end (), [write_id] (const pending_write_t &entry) {
              return entry.write_id == write_id;
          });
        found = write != queue.end ();
        if (found && encoded) {
            write->frame = std::move (encoded.value ());
            write->ready = true;
        } else if (found) {
            callback = std::move (write->callback);
            failure = *encoded.error ();
            queue.erase (write);
            state->state_changed.notify_all ();
        }
    }
    if (callback) {
        callback (result_t<void>::failure (failure->code, failure->message));
    }
    if (found) {
        kick_async_write (std::move (state), "reserved");
    }
}

void kick_async_write (std::shared_ptr<connector_state_t> state, std::string reason)
{
    trace_connector_write (*state, "write-kick", "reason=" + reason);
    auto executor = state->write_strand;
    auto work_state = std::move (state);
    boost::asio::post (
      executor, [state = std::move (work_state), reason = std::move (reason)] () mutable {
          trace_connector_write (*state, "write-kick-dispatch", "reason=" + reason);
          start_next_async_write (std::move (state));
      });
}

void enqueue_async_write (std::shared_ptr<connector_state_t> state, std::vector<std::uint8_t> frame)
{
    const auto frame_size = frame.size ();
    std::size_t queued = 0;
    {
        std::lock_guard<std::mutex> lock (state->transport_mutex);
        /* A control frame is accepted like Send and Request: only on a
     * connected transport. connection_ended fails every write accepted on
     * the connection it ends, so no write outlives its connection. */
        if (!is_transport_connected (*state)) {
            trace_connector_write (*state, "write-dropped",
                                   "bytes=" + std::to_string (frame_size) + " connected=false");
            return;
        }
        (void) accept_write_locked (*state, pending_write_t{std::move (frame)});
        queued = state->write_queue.size ();
    }
    trace_connector_write (*state, "write-queued",
                           "bytes=" + std::to_string (frame_size)
                             + " write_queue=" + std::to_string (queued));
    kick_async_write (std::move (state), "queued");
}

void finish_async_write (std::shared_ptr<connector_state_t> state,
                         std::uint64_t write_id,
                         std::shared_ptr<stream_connection_t> expected_connection,
                         result_t<void> result)
{
    std::function<void (result_t<void>)> callback;
    std::optional<error_t> write_failure;
    {
        std::lock_guard<std::mutex> lock (state->transport_mutex);
        if (!state->active_write || state->active_write->write_id != write_id) {
            // close(), a transport error, or heartbeat timeout already
            // completed this write. A late Asio completion is intentionally
            // ignored and cannot consume the next queued write.
            return;
        }
        if (expected_connection && state->connection != expected_connection) {
            result = result_t<void>::failure (error_code_t::disconnected,
                                              "stream connector connection was replaced");
        }
        if (!result && result.error ()->code == error_code_t::send_failed) {
            write_failure = *result.error ();
        }
        callback = std::move (state->active_write->callback);
        state->active_write.reset ();
        state->state_changed.notify_all ();
    }
    if (callback) {
        callback (std::move (result));
    }
    /* The failed write has its own SendFailed result. Ending the connection
   * next fails every other operation as Disconnected (spec 32 §9). */
    if (write_failure && expected_connection
        && connection_ended (state, *write_failure, expected_connection)) {
        expected_connection->shutdown_and_close_async ();
        schedule_reconnect (state);
    }
    kick_async_write (std::move (state), "completion");
}

void start_next_async_write (std::shared_ptr<connector_state_t> state)
{
    std::shared_ptr<stream_connection_t> connection;
    std::vector<std::uint8_t> frame;
    std::uint64_t write_id = 0;
    std::optional<result_t<void>> immediate_failure;
    {
        std::lock_guard<std::mutex> lock (state->transport_mutex);
        /* stream-connector §7: once close is requested no write starts; the close
     * work fails the writes still queued without writing them. */
        if (state->active_write || state->write_queue.empty () || !state->write_queue.front ().ready
            || state->close_requested.load (std::memory_order_acquire)) {
            trace_connector_write (
              *state, "write-start-skip",
              "in_progress=" + std::string (state->active_write ? "true" : "false")
                + " write_queue=" + std::to_string (state->write_queue.size ()));
            return;
        }
        state->active_write = std::move (state->write_queue.front ());
        state->write_queue.pop_front ();
        write_id = state->active_write->write_id;
        if (!is_transport_connected (*state)) {
            immediate_failure = result_t<void>::failure (error_code_t::disconnected,
                                                         "stream connector is not connected");
        } else {
            connection = state->connection;
            frame = std::move (state->active_write->frame);
        }
    }

    if (immediate_failure) {
        trace_connector_write (
          *state, "write-start",
          "result=skipped error="
            + std::to_string (static_cast<int> (immediate_failure->error ()->code)));
        finish_async_write (state, write_id, {}, std::move (*immediate_failure));
        return;
    }

    try {
        const auto frame_size = frame.size ();
        trace_connector_write (*state, "write-start", "bytes=" + std::to_string (frame_size));
        connection->async_write (std::move (frame), [state, connection, write_id, frame_size] (
                                                      boost::system::error_code error) mutable {
            auto complete = [state, connection, write_id, frame_size, error] () mutable {
                trace_connector_write (*state, "write-completion",
                                       error
                                         ? "result=failure bytes=" + std::to_string (frame_size)
                                             + " error=" + error.message ()
                                         : "result=success bytes=" + std::to_string (frame_size));
                if (error) {
                    finish_async_write (state, write_id, connection,
                                        result_t<void>::failure (state->close_requested.load ()
                                                                   ? error_code_t::disconnected
                                                                   : error_code_t::send_failed,
                                                                 state->close_requested.load ()
                                                                   ? "stream connector is closed"
                                                                   : error.message ()));
                    return;
                }
                finish_async_write (state, write_id, connection, result_t<void>::success ());
            };
            try {
                boost::asio::post (state->write_strand, std::move (complete));
            }
            catch (const std::exception &exception) {
                finish_async_write (
                  state, write_id, connection,
                  result_t<void>::failure (error_code_t::send_failed, exception.what ()));
            }
        });
    }
    catch (const std::exception &ex) {
        finish_async_write (state, write_id, connection,
                            result_t<void>::failure (error_code_t::send_failed, ex.what ()));
    }
}

} // namespace

/* Immediate: a handler registered for packets already queued receives them on
 * the delivery strand, as it would have on arrival (stream-connector §10). */
void deliver_queued_to_handlers (const std::shared_ptr<connector_state_t> &state)
{
    if (state->options.dispatch_mode != dispatch_mode_t::immediate) {
        return;
    }
    run_on_delivery_strand (state, [state] {
        for (;;) {
            std::optional<selected_packet_t> packet;
            {
                std::lock_guard<std::mutex> lock (state->transport_mutex);
                packet =
                  take_dispatchable_locked (*state, std::numeric_limits<std::uint64_t>::max ());
            }
            if (!packet) {
                return;
            }
            dispatch_packet (*state, packet->envelope, packet->handlers);
        }
    });
}

/* An injected packet takes the same route as one the read pump received. */
void deliver_received_packet (connector_state_t &state, packet_t packet)
{
    route_inbound_packet (state.shared_from_this (),
                          dispatch_envelope_t{std::move (packet), std::nullopt});
}

result_t<dispatch_envelope_t> decode_inbound_packet (connector_state_t &state,
                                                     const stream_header_t &header,
                                                     std::vector<std::uint8_t> payload)
{
    return decode_packet (state, header, std::move (payload));
}

std::vector<pending_wait_t> take_pending_waits_locked (connector_state_t &state)
{
    std::vector<pending_wait_t> waits;
    if (state.pending_waits.empty ()) {
        return waits;
    }
    waits.reserve (state.pending_waits.size ());
    for (auto &[_, wait] : state.pending_waits) {
        cancel_timer (wait.timeout_timer);
        waits.push_back (std::move (wait));
    }
    state.pending_waits.clear ();
    ++state.pending_waits_version;
    return waits;
}

/* stream-connector §10.1.1: the connection a wait observed has ended, so the
 * wait ends now, as disconnected. The release belongs to the ending, not to
 * the next connection: released here, a wait does not hang until its own
 * timeout when no next connection comes (reconnect off, attempts spent) and
 * does not rebind to the next one when it does. The dispatch queue and the
 * receive counts stay; the next established connection rebaselines them
 * (§10). */
connection_operations_t take_connection_operations_locked (connector_state_t &state)
{
    connection_operations_t operations;
    operations.waits = take_pending_waits_locked (state);
    for (const auto &[request_seq, _] : state.pending_requests) {
        operations.requests.push_back (request_seq);
    }
    if (state.active_write) {
        if (state.active_write->callback) {
            operations.writes.push_back (std::move (state.active_write->callback));
        }
        state.active_write.reset ();
    }
    for (auto &write : state.write_queue) {
        if (write.callback) {
            operations.writes.push_back (std::move (write.callback));
        }
    }
    state.write_queue.clear ();
    state.state_changed.notify_all ();
    return operations;
}

/* stream-connector §9: an operation that fails because its connection ended
 * fails as Disconnected whatever ended the connection; the cause stays in the
 * close reason and reaches the error handler. (The one exception, a write whose
 * own failure ended the connection, is completed as SendFailed by
 * finish_async_write before the connection ends.) */
void fail_connection_operations (const std::shared_ptr<connector_state_t> &state,
                                 connection_operations_t operations,
                                 const std::string &message)
{
    for (auto &callback : operations.writes) {
        callback (result_t<void>::failure (error_code_t::disconnected, message));
    }
    for (auto request_seq : operations.requests) {
        complete_pending_request (
          state, request_seq,
          result_t<request_reply_t>::failure (error_code_t::disconnected, message));
    }
    for (auto &wait : operations.waits) {
        deliver_wait_result (state, std::move (wait),
                             result_t<packet_t>::failure (error_code_t::disconnected, message));
    }
}

bool connection_ended (const std::shared_ptr<connector_state_t> &state,
                       const error_t &error,
                       const std::shared_ptr<stream_connection_t> &observed_connection)
{
    connection_operations_t operations;
    std::shared_ptr<boost::asio::steady_timer> heartbeat_timer;
    {
        std::lock_guard<std::mutex> lock (state->transport_mutex);
        if (state->connection != observed_connection) {
            return false;
        }
        close_bound_actors (state);
        publish_error (*state, error);
        change_state (state, connection_state_t::disconnected, error);
        ++state->heartbeat_generation;
        heartbeat_timer = std::move (state->heartbeat_timer);
        operations = take_connection_operations_locked (*state);
    }
    cancel_timer (heartbeat_timer);
    fail_connection_operations (state, std::move (operations), error.message);
    return true;
}

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

void submit_request_async (std::shared_ptr<void> state_handle,
                           packet_t packet,
                           std::chrono::milliseconds timeout,
                           std::function<void (result_t<request_reply_t>)> callback,
                           bool deliver_direct,
                           std::optional<actor_binding_ref_t> actor_binding,
                           std::shared_ptr<std::vector<std::uint64_t>> reply_hook_ids);
namespace
{

/* The one Send path (stream-connector §5.2, §9): acceptance, validation,
 * encoding and the write each end the operation with one result, handed to
 * `complete` where it is decided. The two public forms differ only in what
 * `complete` does with it. */
void submit_send_with (std::shared_ptr<connector_state_t> state,
                       packet_t packet,
                       std::function<void (result_t<void>)> complete,
                       std::optional<actor_binding_ref_t> actor_binding)
{
    auto outbound = std::make_shared<packet_t> (std::move (packet));
    std::optional<result_t<void>> immediate_result;
    std::uint64_t write_id = 0;
    {
        std::lock_guard<std::mutex> lock (state->transport_mutex);
        if (state->close_requested.load ()) {
            immediate_result =
              result_t<void>::failure (error_code_t::disconnected, "stream connector is closed");
        } else if (!is_transport_connected (*state)) {
            immediate_result = result_t<void>::failure (error_code_t::disconnected,
                                                        "stream connector is not connected");
        } else if (auto validation = validate_packet_limits (*state, *outbound); !validation) {
            immediate_result = validation;
        } else {
            write_id = reserve_write_locked (
              *state, [state, outbound, complete] (result_t<void> result) mutable {
                  trace_request ("send-write-completion", std::nullopt, outbound->name,
                                 result
                                   ? "result=success"
                                   : "result=failure error="
                                       + std::to_string (static_cast<int> (result.error ()->code)));
                  if (result) {
                      std::lock_guard<std::mutex> lock (state->transport_mutex);
                      state->sent_packets.push_back (std::move (*outbound));
                  }
                  complete (std::move (result));
              });
        }
    }
    if (immediate_result) {
        complete (std::move (*immediate_result));
        return;
    }
    auto encoded =
      encode_packet_frame (*state, message_kind_t::send, *outbound, std::nullopt, actor_binding);
    trace_request ("send-submit", std::nullopt, outbound->name, "kind=send");
    finish_reserved_write (std::move (state), write_id, std::move (encoded));
}

} // namespace

/* cpp stream-connector: the Send without a callback reports its failures as
 * error events. */
void submit_send (std::shared_ptr<connector_state_t> state,
                  packet_t packet,
                  std::optional<actor_binding_ref_t> actor_binding)
{
    auto error_target = state;
    submit_send_with (
      std::move (state), std::move (packet),
      [state = std::move (error_target)] (result_t<void> result) {
          if (!result) {
              publish_error (*state, *result.error ());
          }
      },
      std::move (actor_binding));
}

/* The Send with a callback delivers its result like every other callback. */
void submit_send_async (std::shared_ptr<connector_state_t> state,
                        packet_t packet,
                        std::function<void (result_t<void>)> callback,
                        std::optional<actor_binding_ref_t> actor_binding)
{
    auto delivery_target = state;
    submit_send_with (
      std::move (state), std::move (packet),
      [state = std::move (delivery_target),
       callback = std::move (callback)] (result_t<void> result) mutable {
          schedule_delivery (state, [callback, result = std::move (result)] () mutable {
              if (callback) {
                  callback (std::move (result));
              }
          });
      },
      std::move (actor_binding));
}

result_t<request_reply_t> submit_request (std::shared_ptr<void> state_handle,
                                          packet_t packet,
                                          std::chrono::milliseconds timeout)
{
    return submit_request (std::move (state_handle), std::move (packet), timeout, std::nullopt);
}

result_t<request_reply_t> submit_request (std::shared_ptr<void> state_handle,
                                          packet_t packet,
                                          std::chrono::milliseconds timeout,
                                          std::optional<actor_binding_ref_t> actor_binding)
{
    auto promise = std::make_shared<std::promise<result_t<request_reply_t>>> ();
    auto future = promise->get_future ();
    submit_request_async (std::move (state_handle), std::move (packet), timeout,
                          [promise] (result_t<request_reply_t> result) mutable {
                              promise->set_value (std::move (result));
                          },
                          /*deliver_direct=*/true, actor_binding, {});
    return future.get ();
}

void submit_request_async (std::shared_ptr<void> state_handle,
                           packet_t packet,
                           std::chrono::milliseconds timeout,
                           std::function<void (result_t<request_reply_t>)> callback,
                           bool deliver_direct,
                           std::optional<actor_binding_ref_t> actor_binding,
                           std::shared_ptr<std::vector<std::uint64_t>> reply_hook_ids)
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
    std::uint64_t write_id = 0;
    std::optional<result_t<request_reply_t>> immediate_result;
    std::string request_packet_name;
    {
        std::unique_lock<std::mutex> lock (state->transport_mutex);
        if (state->close_requested.load ()) {
            immediate_result = result_t<request_reply_t>::failure (error_code_t::disconnected,
                                                                   "stream connector is closed");
        } else if (!is_transport_connected (*state)) {
            immediate_result = result_t<request_reply_t>::failure (
              error_code_t::disconnected, "stream connector is not connected");
        } else if (auto validation = validate_packet_limits (*state, packet); !validation) {
            immediate_result = result_t<request_reply_t>::failure (validation.error ()->code,
                                                                   validation.error ()->message);
        } else if (state->next_request_seq == 0) {
            immediate_result = result_t<request_reply_t>::failure (
              error_code_t::send_failed, "stream connector request sequence exhausted");
        } else {
            seq = state->next_request_seq++;
            request_packet_name = packet.name;
            trace_request ("submit", seq, request_packet_name, "mode=async");
            auto timeout_timer =
              post_runtime_operation_after (state, timeout, [state, seq, request_packet_name] {
                  trace_request ("request-timeout", seq, request_packet_name);
                  complete_pending_request (
                    state, seq,
                    result_t<request_reply_t>::failure (error_code_t::request_timeout,
                                                        "stream connector request timed out"));
              });
            state->pending_requests.emplace (
              seq, pending_request_t{seq, packet, std::move (callback), timeout_timer,
                                     deliver_direct, reply_hook_ids});
            write_id = reserve_write_locked (*state, [state, seq, request_packet_name] (
                                                       result_t<void> written) mutable {
                trace_request ("request-write-completion", seq, request_packet_name,
                               written
                                 ? "result=success"
                                 : "result=failure error="
                                     + std::to_string (static_cast<int> (written.error ()->code)));
                if (!written) {
                    complete_pending_request (
                      state, seq,
                      result_t<request_reply_t>::failure (
                        written.error ()->code, written.error () ? written.error ()->message
                                                                 : "stream request write failed"));
                    return;
                }
                schedule_request_pump (state);
            });
        }
    }

    if (immediate_result) {
        if (reply_hook_ids) {
            *reply_hook_ids = capture_reply_hook_ids (state);
        }
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
    auto encoded =
      encode_packet_frame (*state, message_kind_t::request, packet, seq, actor_binding);
    finish_reserved_write (std::move (state), write_id, std::move (encoded));
}

result_t<void> dispatch_pending (std::shared_ptr<connector_state_t> state)
{
    std::deque<delivery_t> deliveries;
    const auto through_arrival = state->next_arrival.load () - 1;
    /* The read pump is the only socket reader (stream-connector §5.2): it
   * decides every inbound frame's kind and queues the pushes that a Manual
   * pump runs here. */
    callback_scope_t scope (*state);
    {
        std::lock_guard<std::mutex> lock (state->delivery_mutex);
        while (!state->delivery_queue.empty ()
               && state->delivery_queue.front ().arrival <= through_arrival) {
            deliveries.push_back (std::move (state->delivery_queue.front ()));
            state->delivery_queue.pop_front ();
        }
    }
    /* Both queues use the same arrival sequence. A lifecycle callback that
   * arrived after a packet cannot run before that packet in Manual mode. */
    std::uint64_t passed_arrival = 0;
    while (true) {
        std::optional<selected_packet_t> packet;
        {
            std::lock_guard<std::mutex> lock (state->transport_mutex);
            const auto limit = deliveries.empty ()
                                 ? through_arrival
                                 : std::min (through_arrival, deliveries.front ().arrival - 1);
            packet = take_dispatchable_locked (*state, limit, passed_arrival);
        }
        if (!packet && deliveries.empty ()) {
            break;
        }
        if (!packet) {
            auto delivery = std::move (deliveries.front ());
            deliveries.pop_front ();
            /* A packet with no recipient before this callback remains for the
             * next Manual pump even if the callback registers its handler. */
            passed_arrival = delivery.arrival - 1;
            if (delivery.run) {
                try {
                    delivery.run ();
                }
                catch (...) {
                }
            }
            continue;
        }
        try {
            dispatch_packet (*state, packet->envelope, packet->handlers);
        }
        catch (const std::exception &error) {
            publish_error (*state, {error_code_t::user_callback_failed, error.what ()});
        }
        catch (...) {
            publish_error (*state, {error_code_t::user_callback_failed, "packet callback failed"});
        }
        passed_arrival = packet->envelope.arrival;
    }
    return result_t<void>::success ();
}

namespace
{

/* A connection was established and has ended: an ending leaves its close
 * reason, which is never cleared (stream-connector §6.2). Caller holds
 * transport_mutex; lifecycle_mutex nests under it. */
bool connection_has_ended (connector_state_t &state)
{
    std::lock_guard<std::mutex> lock (state.lifecycle_mutex);
    return state.last_close_reason.has_value ();
}

/* The one wait path (stream-connector §10.1.1). A queued packet the wait
 * accepts completes it at once. After a connection has ended and before the
 * next is established there is no place left to observe, so the wait ends as
 * Disconnected instead of running into its timeout. A wait started before the
 * first connection observes that connection. Otherwise the wait is registered:
 * the read pump completes it with the first packet it accepts, the timeout
 * completes it as timed out, and the end of the connection releases it as
 * Disconnected. */
void start_wait (const std::shared_ptr<connector_state_t> &state,
                 std::string packet_name,
                 std::function<bool (const packet_t &)> predicate,
                 std::chrono::milliseconds timeout,
                 std::function<void (result_t<packet_t>)> callback,
                 bool deliver_direct)
{
    pending_wait_t wait{0,  std::move (packet_name), std::move (predicate), std::move (callback),
                        {}, deliver_direct};
    std::optional<result_t<packet_t>> immediate_result;
    std::uint64_t wait_id = 0;
    {
        std::unique_lock<std::mutex> lock (state->transport_mutex);
        /* The predicate is the caller's own code and runs with transport_mutex
     * released; it may call back into the connector surface. */
        if (auto matched =
              take_matching_queued_packet (*state, lock, wait.packet_name, wait.predicate)) {
            immediate_result = result_t<packet_t>::success (std::move (*matched));
        } else if (state->close_requested.load ()) {
            immediate_result = result_t<packet_t>::failure (error_code_t::disconnected,
                                                            "stream connector is closed");
        } else if (!is_transport_connected (*state) && connection_has_ended (*state)) {
            immediate_result = result_t<packet_t>::failure (
              error_code_t::disconnected, state->last_disconnect_error
                                            ? state->last_disconnect_error->message
                                            : "stream connector is not connected");
        } else {
            wait_id = state->next_wait_id++;
            wait.wait_id = wait_id;
            state->pending_waits.emplace (wait_id, std::move (wait));
            ++state->pending_waits_version;
        }
    }

    if (immediate_result) {
        deliver_wait_result (state, std::move (wait), std::move (*immediate_result));
        return;
    }
    start_read_loop (state);

    auto timeout_timer = post_runtime_operation_after (state, timeout, [state, wait_id] {
        std::optional<pending_wait_t> expired;
        {
            std::lock_guard<std::mutex> lock (state->transport_mutex);
            expired = claim_wait (*state, wait_id);
        }
        if (expired) {
            deliver_wait_result (state, std::move (*expired),
                                 result_t<packet_t>::failure (error_code_t::request_timeout,
                                                              "stream connector wait timed out"));
        }
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

} // namespace

/* The synchronous wait is the asynchronous one with a callback that resolves
 * the promise its caller blocks on - the shape of submit_request. */
result_t<packet_t> wait_for_packet (std::shared_ptr<connector_state_t> state,
                                    std::string packet_name,
                                    std::function<bool (const packet_t &)> predicate,
                                    std::chrono::milliseconds timeout)
{
    auto promise = std::make_shared<std::promise<result_t<packet_t>>> ();
    auto future = promise->get_future ();
    start_wait (
      state, std::move (packet_name), std::move (predicate), timeout,
      [promise] (result_t<packet_t> result) { promise->set_value (std::move (result)); },
      /*deliver_direct=*/true);
    return future.get ();
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
    start_wait (std::static_pointer_cast<connector_state_t> (state_handle), std::move (packet_name),
                std::move (predicate), timeout, std::move (callback), /*deliver_direct=*/false);
}

} // namespace zlink::stream_connector::detail
