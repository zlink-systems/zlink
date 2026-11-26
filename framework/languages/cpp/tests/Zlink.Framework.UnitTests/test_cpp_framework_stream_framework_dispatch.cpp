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
#include "../support/stream_framework_session_fixture.hpp"

namespace zlink::framework::test::stream_framework
{
namespace
{
using zlink::framework::tests::endpoint_port;
using zlink::framework::tests::listener_endpoint;
using zlink::framework::tests::loopback_tcp_any_port;

class pending_connected_session_t final : public zlink::framework::packet_stream_session_t
{
  public:
    zlink::framework::task_t<void> on_connected (zlink::framework::stream_t &) override
    {
        entered.set_value ();
        co_await resume.task ();
        connected.set_value ();
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
    zlink::framework::task_t<void> on_packet (zlink::framework::stream_t &,
                                              const zlink::framework::session_message_context_t &,
                                              const zlink::framework::message_t &) override
    {
        packet.set_value ();
        co_return;
    }
    zlink::framework::task_completion_source_t<void> resume;
    std::promise<void> entered;
    std::promise<void> connected;
    std::promise<void> packet;
};

class reentrant_session_t final : public zlink::framework::packet_stream_session_t
{
  public:
    explicit reentrant_session_t (zlink::framework::detail::stream_runtime_t &runtime) :
        _runtime (&runtime)
    {
    }

    zlink::framework::task_t<void> on_connected (zlink::framework::stream_t &stream) override
    {
        events.push_back ("connected");
        nested_submission = _runtime->dispatch_disconnected_async (
          *this, stream, [this] (const zlink::framework::result_t<void> &result) {
              nested_completion.set_value (static_cast<bool> (result));
          });
        co_return;
    }

    zlink::framework::task_t<void> on_disconnected (zlink::framework::stream_t &) override
    {
        events.push_back ("disconnected");
        co_return;
    }

    zlink::framework::task_t<void> on_error (zlink::framework::stream_t &,
                                             const zlink::framework::stream_error_t &) override
    {
        co_return;
    }

    zlink::framework::task_t<void> on_packet (zlink::framework::stream_t &,
                                              const zlink::framework::session_message_context_t &,
                                              const zlink::framework::message_t &) override
    {
        co_return;
    }

    zlink::framework::detail::stream_runtime_t *_runtime;
    zlink::framework::result_t<void> nested_submission = zlink::framework::result_t<void>::failure (
      zlink::framework::framework_error_kind_t::internal_failure,
      "nested dispatch was not submitted");
    std::promise<bool> nested_completion;
    std::vector<std::string> events;
};

class duplicate_reply_session_t final : public zlink::framework::packet_stream_session_t
{
  public:
    zlink::framework::task_t<void> on_connected (zlink::framework::stream_t &) override
    {
        co_return;
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

    zlink::framework::task_t<void> on_packet (zlink::framework::stream_t &stream,
                                              const zlink::framework::session_message_context_t &,
                                              const zlink::framework::message_t &payload) override
    {
        auto winner = stream.reply_packet (payload);
        auto loser = stream.reply_packet (payload);
        co_await winner.async ();
        winner_completed = true;
        try {
            (void) co_await loser.async ();
        }
        catch (const zlink::framework::framework_exception_t &error) {
            loser_rejected =
              error.kind () == zlink::framework::framework_error_kind_t::protocol_error;
        }
        co_return;
    }

    bool winner_completed = false;
    bool loser_rejected = false;
};

class failed_reply_session_t final : public zlink::framework::packet_stream_session_t
{
  public:
    zlink::framework::task_t<void> on_connected (zlink::framework::stream_t &) override
    {
        co_return;
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

    zlink::framework::task_t<void> on_packet (zlink::framework::stream_t &stream,
                                              const zlink::framework::session_message_context_t &,
                                              const zlink::framework::message_t &payload) override
    {
        const auto first = stream.reply_packet (payload).async ().result ();
        first_failed =
          !first
          && first.error_kind () == zlink::framework::framework_error_kind_t::internal_failure;
        const auto second = stream.reply_packet (payload).async ().result ();
        second_rejected =
          !second
          && second.error_kind () == zlink::framework::framework_error_kind_t::protocol_error;
        co_return;
    }

    bool first_failed = false;
    bool second_rejected = false;
};

class throwing_packet_session_t final : public zlink::framework::packet_stream_session_t
{
  public:
    zlink::framework::task_t<void> on_connected (zlink::framework::stream_t &) override
    {
        return zlink::framework::task_t<void> (zlink::framework::result_t<void>::success ());
    }

    zlink::framework::task_t<void> on_disconnected (zlink::framework::stream_t &) override
    {
        return zlink::framework::task_t<void> (zlink::framework::result_t<void>::success ());
    }

    zlink::framework::task_t<void> on_error (zlink::framework::stream_t &,
                                             const zlink::framework::stream_error_t &) override
    {
        on_error_called = true;
        return zlink::framework::task_t<void> (zlink::framework::result_t<void>::success ());
    }

    zlink::framework::task_t<void> on_packet (zlink::framework::stream_t &,
                                              const zlink::framework::session_message_context_t &,
                                              const zlink::framework::message_t &) override
    {
        return zlink::framework::task_t<void> (zlink::framework::result_t<void>::failure (
          zlink::framework::framework_error_kind_t::internal_failure,
          "application packet failure"));
    }

    bool on_error_called = false;
};

class delayed_reply_session_t final : public zlink::framework::packet_stream_session_t
{
  public:
    delayed_reply_session_t () : _entered_future (_entered.get_future ()) {}

    zlink::framework::task_t<void> on_connected (zlink::framework::stream_t &) override
    {
        co_return;
    }

    zlink::framework::task_t<void> on_disconnected (zlink::framework::stream_t &) override
    {
        callbacks.push_back ("disconnected");
        co_return;
    }

    zlink::framework::task_t<void> on_error (zlink::framework::stream_t &,
                                             const zlink::framework::stream_error_t &error) override
    {
        callbacks.push_back ("error:" + std::string (error.message ()));
        co_return;
    }

    zlink::framework::task_t<void> on_packet (zlink::framework::stream_t &stream,
                                              const zlink::framework::session_message_context_t &,
                                              const zlink::framework::message_t &payload) override
    {
        callbacks.push_back ("packet");
        _entered.set_value ();
        co_await _resume.task ();
        try {
            (void) co_await stream.reply_packet (payload).async ();
            reply_result = zlink::framework::result_t<void>::success ();
        }
        catch (const zlink::framework::framework_exception_t &error) {
            reply_result = zlink::framework::detail::result_access_t::failure<void> (error);
        }
    }

    void wait_until_suspended () { _entered_future.wait (); }

    void resume () { _resume.complete (zlink::framework::result_t<void>::success ()); }

    std::optional<zlink::framework::result_t<void>> reply_result;
    std::vector<std::string> callbacks;

  private:
    std::promise<void> _entered;
    std::future<void> _entered_future;
    zlink::framework::task_completion_source_t<void> _resume;
};
} // namespace

int stream_dispatch_cases (metric_test::provider_t &metric_provider)
{
    using zlink::framework::framework_error_kind_t;
    using zlink::framework::stream_codec_t;
    using zlink::framework::detail::stream_header_flags_t;
    using zlink::framework::detail::stream_message_kind_t;

    zlink::monitor_event_t ready_snapshot;
    ready_snapshot.event = zlink::monitor_event::connection_ready;
    ready_snapshot.routing_id = zlink::routing_id_t::from (std::uint32_t{1});
    if (zlink::framework::runtime::stream_host_connection_ready (ready_snapshot))
        return 321;
    ready_snapshot.flags =
      static_cast<std::uint32_t> (zlink::monitor_event_flag_t::connection_ready_edge);
    if (!zlink::framework::runtime::stream_host_connection_ready (ready_snapshot))
        return 322;

    const auto codec_mapping_is_consistent = [] (stream_codec_t codec,
                                                 std::string_view content_type) {
        return zlink::framework::detail::stream_content_type (codec) == content_type
               && zlink::framework::detail::stream_codec_from_content_type (content_type) == codec;
    };
    if (!codec_mapping_is_consistent (stream_codec_t::raw, "application/octet-stream")
        || !codec_mapping_is_consistent (stream_codec_t::json, "application/json")
        || !codec_mapping_is_consistent (stream_codec_t::message_pack, "application/x-msgpack")
        || !codec_mapping_is_consistent (stream_codec_t::protobuf, "application/x-protobuf")) {
        return 81;
    }

    struct stream_dispatch_executor_guard_t
    {
        stream_dispatch_executor_guard_t ()
        {
            zlink::framework::detail::configure_stream_dispatch_executor ();
        }

        ~stream_dispatch_executor_guard_t ()
        {
            zlink::framework::detail::shutdown_stream_dispatch_executor ();
        }
    } stream_dispatch_executor_guard;

    zlink::framework::zlink_builder_t zlink = zlink::framework::test::runtime_failure_builder ();
    zlink.stream ("client-stream").bind ("tcp://0.0.0.0:9200").register_session ("client");
    // The stream dispatch executor can emit after this scope ends, so the observer owns the state.
    struct flow_state_t
    {
        std::atomic_bool saw_stream_session_flow{false};
        std::mutex mutex;
        std::vector<zlink::framework::message_flow_event_t> events;

        std::vector<zlink::framework::message_flow_event_t> snapshot ()
        {
            const std::lock_guard lock (mutex);
            return events;
        }

        bool none_of (
          const std::function<bool (const zlink::framework::message_flow_event_t &)> &predicate)
        {
            const std::lock_guard lock (mutex);
            return std::none_of (events.begin (), events.end (), predicate);
        }
    };
    const auto flow_state = std::make_shared<flow_state_t> ();
    zlink::framework::dispatch_options_t flow_options;
    flow_options.message_flow (zlink::framework::message_flow_log_mode_t::normal);
    zlink::framework::detail::dispatch_options_access_t::set_observer_for_tests (
      flow_options, [flow_state] (const zlink::framework::message_flow_event_t &event) {
          {
              const std::lock_guard lock (flow_state->mutex);
              flow_state->events.push_back (event);
          }
          if (event.surface == zlink::framework::dispatch_error_surface_t::stream_session
              && event.outcome == zlink::framework::message_flow_outcome_t::received
              && event.stream_session_id
              && *event.stream_session_id == zlink::routing_id_t::from ("stream-rid").to_hex ())
              flow_state->saw_stream_session_flow.store (true, std::memory_order_release);
      });
    zlink::framework::detail::apply_dispatch_options (zlink, flow_options);
    zlink::framework::serializer_registry_t serializers;
    serializers.add<std::string> (
      [] (const std::string &value) {
          return zlink::framework::encoded_payload_t::from_string (value);
      },
      [] (const zlink::framework::encoded_payload_t &payload) { return payload.to_string (); });
    zlink::framework::detail::bind_stream_serializers (zlink, serializers);

    const auto snapshots = zlink::framework::detail::stream_runtime_t::from (zlink).snapshots ();
    if (snapshots.size () != 1 || snapshots[0].name != "client-stream"
        || snapshots[0].bind_endpoint != "tcp://0.0.0.0:9200"
        || snapshots[0].packet_session_name != "client")
        return 1;

    auto runtime = zlink::framework::detail::stream_runtime_t::from (zlink);
    zlink::framework::detail::stream_metadata_t metadata;
    metadata.with ("trace", "42").with ("content_type", "application/json");
    zlink::framework::detail::stream_header_t request_header (
      stream_message_kind_t::request, stream_codec_t::json, stream_header_flags_t::has_request_seq,
      77, "move", metadata);
    // correlation_id is now a first-class stream-header field (not a metadata key).
    request_header.with_correlation_id ("abc");
    request_header.with_actor_slot (9);

    const auto encoded = runtime.encode_header (request_header);
    if (!encoded) {
        return 2;
    }
    const auto decoded = runtime.decode_header (encoded.value ());
    if (!decoded || decoded.value ().kind () != stream_message_kind_t::request
        || decoded.value ().codec () != stream_codec_t::json
        || decoded.value ().request_seq () != 77 || decoded.value ().packet_name () != "move"
        || decoded.value ().correlation_id () != "abc" || decoded.value ().actor_slot () != 9
        || decoded.value ().content_type () != "application/json"
        || decoded.value ().metadata ("trace") != "42") {
        return 3;
    }

    zlink::framework::detail::stream_header_t reply_header (
      stream_message_kind_t::response, stream_codec_t::json, stream_header_flags_t::has_request_seq,
      77, "");
    const auto encoded_reply = runtime.encode_header (reply_header);
    if (!encoded_reply || encoded_reply.value ().size () < 13 || encoded_reply.value ()[12] != 0
        || !runtime.decode_header (encoded_reply.value ())) {
        return 33;
    }
    auto legacy_reply = encoded_reply.value ();
    legacy_reply[12] = 6;
    legacy_reply.insert (legacy_reply.begin () + 13, {'l', 'e', 'g', 'a', 'c', 'y'});
    const auto decoded_legacy_reply = runtime.decode_header (legacy_reply);
    if (!decoded_legacy_reply || decoded_legacy_reply.value ().packet_name () != "legacy") {
        return 35;
    }
    zlink::framework::detail::stream_header_t named_reply_header (
      stream_message_kind_t::response, stream_codec_t::json, stream_header_flags_t::has_request_seq,
      77, "legacy.reply");
    if (runtime.encode_header (named_reply_header)) {
        return 34;
    }

    zlink::framework::detail::stream_header_t invalid_send (
      stream_message_kind_t::send, stream_codec_t::json, stream_header_flags_t::has_request_seq, 1,
      "bad");
    if (runtime.validate_header (invalid_send)
        || runtime.validate_header (invalid_send).error_kind ()
             != framework_error_kind_t::protocol_error) {
        return 4;
    }

    zlink::framework::detail::stream_header_t reserved (
      stream_message_kind_t::send, stream_codec_t::raw, stream_header_flags_t::none, std::nullopt,
      "$zlink.internal");
    if (runtime.validate_header (reserved)) {
        return 5;
    }

    zlink::framework::detail::stream_header_t valid_control (
      stream_message_kind_t::control, stream_codec_t::raw, stream_header_flags_t::none,
      std::nullopt, "$zlink.ping");
    if (!runtime.validate_header (valid_control)) {
        return 6;
    }
    zlink::framework::detail::stream_header_t missing_request_seq (
      stream_message_kind_t::request, stream_codec_t::json, stream_header_flags_t::none,
      std::nullopt, "missing-seq");
    zlink::framework::detail::stream_header_t zero_request_seq (
      stream_message_kind_t::request, stream_codec_t::json, stream_header_flags_t::has_request_seq,
      0, "zero-seq");
    zlink::framework::detail::stream_header_t invalid_error (
      stream_message_kind_t::error, stream_codec_t::raw, stream_header_flags_t::has_request_seq, 1,
      "error");
    zlink::framework::detail::stream_header_t invalid_control (
      stream_message_kind_t::control, stream_codec_t::json, stream_header_flags_t::none,
      std::nullopt, "__zlink.bad");
    if (runtime.validate_header (missing_request_seq) || runtime.validate_header (zero_request_seq)
        || runtime.validate_header (invalid_error) || runtime.validate_header (invalid_control)) {
        return 20;
    }
    zlink::framework::detail::stream_header_t zero_actor_slot (
      stream_message_kind_t::send, stream_codec_t::raw, stream_header_flags_t::none, std::nullopt,
      "zero-actor");
    zero_actor_slot.with_actor_slot (0);
    zlink::framework::detail::stream_header_t control_actor_slot (
      stream_message_kind_t::control, stream_codec_t::raw, stream_header_flags_t::none,
      std::nullopt, "$zlink.actor.bound");
    control_actor_slot.with_actor_slot (1);
    auto truncated_actor_slot = encoded.value ();
    truncated_actor_slot.pop_back ();
    if (runtime.validate_header (zero_actor_slot) || runtime.validate_header (control_actor_slot)
        || runtime.decode_header (truncated_actor_slot)) {
        return 294;
    }
    if (zlink::framework::detail::stream_runtime_t::encode_actor_bound_payload (7, "actor")
          != std::vector<std::uint8_t> ({1, 0, 7, 5, 'a', 'c', 't', 'o', 'r'})
        || zlink::framework::detail::stream_runtime_t::encode_actor_unbound_payload (7)
             != std::vector<std::uint8_t> ({1, 0, 7})) {
        return 295;
    }
    zlink::framework::detail::stream_metadata_t large_metadata;
    large_metadata.with ("trace", std::string (65536, 'x'));
    zlink::framework::detail::stream_header_t too_large_metadata (
      stream_message_kind_t::send, stream_codec_t::json, stream_header_flags_t::none, std::nullopt,
      "large", large_metadata);
    if (runtime.encode_header (too_large_metadata)
        || runtime.encode_header (too_large_metadata).error_kind ()
             != framework_error_kind_t::protocol_error) {
        return 21;
    }
    zlink::framework::detail::stream_metadata_t maximum_frame_metadata;
    maximum_frame_metadata.with ("k", std::string (65522, 'x'));
    zlink::framework::detail::stream_header_t maximum_frame_header (
      stream_message_kind_t::send, stream_codec_t::json, stream_header_flags_t::none, std::nullopt,
      "x", maximum_frame_metadata);
    const auto maximum_encoded_header = runtime.encode_header (maximum_frame_header);
    if (!maximum_encoded_header
        || maximum_encoded_header.value ().size () != std::numeric_limits<std::uint16_t>::max ()) {
        return 282;
    }
    const auto maximum_encoded_frame =
      runtime.encode_frame (maximum_frame_header, zlink::message_t::from (std::string ("p")));
    if (!maximum_encoded_frame
        || maximum_encoded_frame.value ().size ()
             != 6 + std::numeric_limits<std::uint16_t>::max () + 1
        || maximum_encoded_frame.value ()[0] != 0xff || maximum_encoded_frame.value ()[1] != 0xff
        || maximum_encoded_frame.value ()[2] != 0 || maximum_encoded_frame.value ()[3] != 0
        || maximum_encoded_frame.value ()[4] != 0 || maximum_encoded_frame.value ()[5] != 1
        || maximum_encoded_frame.value ().back () != 'p') {
        return 283;
    }

    zlink::framework::detail::stream_metadata_t overflowing_frame_metadata;
    overflowing_frame_metadata.with ("k", std::string (65523, 'x'));
    zlink::framework::detail::stream_header_t overflowing_frame_header (
      stream_message_kind_t::send, stream_codec_t::json, stream_header_flags_t::none, std::nullopt,
      "x", overflowing_frame_metadata);
    const auto overflowing_encoded_header = runtime.encode_header (overflowing_frame_header);
    const auto overflowing_encoded_frame =
      runtime.encode_frame (overflowing_frame_header, zlink::message_t::from (std::string ("p")));
    if (overflowing_encoded_header
        || overflowing_encoded_header.error_kind () != framework_error_kind_t::protocol_error
        || overflowing_encoded_frame
        || overflowing_encoded_frame.error_kind () != framework_error_kind_t::protocol_error) {
        return 284;
    }

    const auto maximum_payload_representation =
      zlink::framework::detail::stream_runtime_t::validate_frame_representation (
        0, std::numeric_limits<std::uint32_t>::max ());
    const auto overflowing_payload_representation =
      zlink::framework::detail::stream_runtime_t::validate_frame_representation (
        0, static_cast<std::uint64_t> (std::numeric_limits<std::uint32_t>::max ()) + 1);
    if (!maximum_payload_representation || overflowing_payload_representation
        || overflowing_payload_representation.error_kind ()
             != framework_error_kind_t::protocol_error) {
        return 285;
    }
    const std::vector<std::vector<std::uint8_t>> invalid_headers{
      {},
      {0xF2, static_cast<std::uint8_t> (stream_message_kind_t::request),
       static_cast<std::uint8_t> (stream_codec_t::json),
       static_cast<std::uint8_t> (stream_header_flags_t::has_request_seq), 1},
      {0xF2, static_cast<std::uint8_t> (stream_message_kind_t::send),
       static_cast<std::uint8_t> (stream_codec_t::json),
       static_cast<std::uint8_t> (stream_header_flags_t::none), 0},
      {0xF2, static_cast<std::uint8_t> (stream_message_kind_t::send),
       static_cast<std::uint8_t> (stream_codec_t::json),
       static_cast<std::uint8_t> (stream_header_flags_t::none), 4, 'n'},
      {0xF2, static_cast<std::uint8_t> (stream_message_kind_t::send),
       static_cast<std::uint8_t> (stream_codec_t::json),
       static_cast<std::uint8_t> (stream_header_flags_t::has_metadata), 4, 'n', 'a', 'm', 'e'},
      {0xF2, static_cast<std::uint8_t> (stream_message_kind_t::send),
       static_cast<std::uint8_t> (stream_codec_t::json),
       static_cast<std::uint8_t> (stream_header_flags_t::has_metadata), 4, 'n', 'a', 'm', 'e', 1},
      {0xF2, static_cast<std::uint8_t> (stream_message_kind_t::send),
       static_cast<std::uint8_t> (stream_codec_t::json),
       static_cast<std::uint8_t> (stream_header_flags_t::has_metadata), 4, 'n', 'a', 'm', 'e', 1, 3,
       'k'},
      {0xF2, static_cast<std::uint8_t> (stream_message_kind_t::send),
       static_cast<std::uint8_t> (stream_codec_t::json),
       static_cast<std::uint8_t> (stream_header_flags_t::has_metadata), 4, 'n', 'a', 'm', 'e', 1, 1,
       'k'},
      {0xF2, static_cast<std::uint8_t> (stream_message_kind_t::send),
       static_cast<std::uint8_t> (stream_codec_t::json),
       static_cast<std::uint8_t> (stream_header_flags_t::has_metadata), 4, 'n', 'a', 'm', 'e', 1, 1,
       'k', 3, 'v'},
      {0xF2, static_cast<std::uint8_t> (stream_message_kind_t::send),
       static_cast<std::uint8_t> (stream_codec_t::json),
       static_cast<std::uint8_t> (stream_header_flags_t::none), 4, 'n', 'a', 'm', 'e', 0}};
    for (const auto &invalid_header : invalid_headers) {
        if (runtime.decode_header (invalid_header)
            || runtime.decode_header (invalid_header).error_kind ()
                 != framework_error_kind_t::protocol_error) {
            return 22;
        }
    }

    /* flow-correlation §3.2/§3.4: missing/wrong marker and malformed flow
     * fields fail as protocol errors; a valid flow pair round-trips. */
    const std::vector<std::uint8_t> no_marker_header{
      static_cast<std::uint8_t> (stream_message_kind_t::send),
      static_cast<std::uint8_t> (stream_codec_t::json),
      static_cast<std::uint8_t> (stream_header_flags_t::none),
      4,
      'n',
      'a',
      'm',
      'e'};
    auto no_marker = runtime.decode_header (no_marker_header);
    if (no_marker || no_marker.error_kind () != framework_error_kind_t::protocol_error) {
        return 220;
    }
    const std::vector<std::uint8_t> truncated_flow_header{
      0xF2,
      static_cast<std::uint8_t> (stream_message_kind_t::send),
      static_cast<std::uint8_t> (stream_codec_t::json),
      static_cast<std::uint8_t> (stream_header_flags_t::has_flow_id),
      4,
      'n',
      'a',
      'm',
      'e',
      'x'};
    if (runtime.decode_header (truncated_flow_header)) {
        return 221;
    }
    const std::string sample_flow_id = "01890a5d-ac96-774b-bcce-b302099a8057";
    zlink::framework::detail::stream_header_t flow_header (
      stream_message_kind_t::send, stream_codec_t::json, stream_header_flags_t::none, std::nullopt,
      "flowed", {});
    flow_header.with_flow (sample_flow_id, zlink::framework::flow_origin_t::inbound);
    auto flow_encoded = runtime.encode_header (flow_header);
    if (!flow_encoded || flow_encoded.value ()[0] != 0xF2) {
        return 222;
    }
    auto flow_decoded = runtime.decode_header (flow_encoded.value ());
    if (!flow_decoded || !flow_decoded.value ().flow_id ()
        || *flow_decoded.value ().flow_id () != sample_flow_id
        || flow_decoded.value ().flow_origin () != zlink::framework::flow_origin_t::inbound) {
        return 223;
    }
    if (runtime.encode_header (flow_decoded.value ()).value () != flow_encoded.value ()) {
        return 224;
    }
    zlink::framework::detail::stream_header_t bad_flow_header (
      stream_message_kind_t::send, stream_codec_t::json, stream_header_flags_t::none, std::nullopt,
      "flowed", {});
    bad_flow_header.with_flow ("UPPERCASE-not-a-uuid7-value-000000000", // 37 bytes, invalid
                               zlink::framework::flow_origin_t::inbound);
    if (runtime.encode_header (bad_flow_header)) {
        return 225;
    }

    request_header = zlink::framework::detail::stream_header_t (
      stream_message_kind_t::request, stream_codec_t::json, stream_header_flags_t::has_request_seq,
      77, "move", metadata);
    request_header.with_correlation_id ("abc");
    auto stream = runtime.open_session ("client-stream");
    if (stream.routing_id () || stream.local_address () || stream.remote_address ()) {
        return 226;
    }
    runtime.set_session_identity (stream, zlink::routing_id_t::from ("stream-rid"),
                                  std::string ("127.0.0.1:7101"), std::string ("127.0.0.1:48210"));
    if (!stream.routing_id () || stream.routing_id ()->to_string () != "stream-rid"
        || !stream.local_address () || *stream.local_address () != "127.0.0.1:7101"
        || !stream.remote_address () || *stream.remote_address () != "127.0.0.1:48210") {
        return 227;
    }
    sample_session_t session;
    if (!runtime.dispatch_connected (session, stream)) {
        return 7;
    }
    if (!runtime.dispatch_packet (session, stream, request_header,
                                  zlink::message_t::from (std::string ("payload")))) {
        return 8;
    }
    zlink::framework::test::acknowledge_delivery ();
    if (!flow_state->saw_stream_session_flow.load (std::memory_order_acquire))
        return 229;
    // Spec 26 §2.1: the typed handler start of a STREAM packet records `dispatched` once, after `received`.
    {
        std::vector<zlink::framework::message_flow_outcome_t> move_phases;
        const auto flow_events = flow_state->snapshot ();
        for (const auto &event : flow_events) {
            if (event.surface == zlink::framework::dispatch_error_surface_t::stream_session
                && event.packet_name == std::optional<std::string> ("move"))
                move_phases.push_back (event.outcome);
        }
        const auto received = std::find (move_phases.begin (), move_phases.end (),
                                         zlink::framework::message_flow_outcome_t::received);
        const auto dispatched = std::find (move_phases.begin (), move_phases.end (),
                                           zlink::framework::message_flow_outcome_t::dispatched);
        if (dispatched == move_phases.end () || received > dispatched
            || std::count (move_phases.begin (), move_phases.end (),
                           zlink::framework::message_flow_outcome_t::dispatched)
                 != 1)
            return 336;
    }
    if (!session.last_can_reply || session.last_actor_id
        || session.last_metadata.find ("trace") != "42"
        || session.last_metadata.find ("content_type") != "application/json") {
        return 228;
    }
    if (!runtime.dispatch_disconnected (session, stream)) {
        return 9;
    }
    if (session.events.size () != 3 || session.events[0] != "connected:client-stream:1"
        || session.events[1] != "packet:move:payload"
        || session.events[2] != "disconnected:client-stream:1"
        || runtime.written_headers (stream).size () != 1) {
        return 10;
    }

    auto reentrant_stream = runtime.open_session ("client-stream");
    reentrant_session_t reentrant_session (runtime);
    auto nested_completed = reentrant_session.nested_completion.get_future ();
    if (!runtime.dispatch_connected (reentrant_session, reentrant_stream)
        || !reentrant_session.nested_submission
        || nested_completed.wait_for (std::chrono::seconds (2)) != std::future_status::ready
        || !nested_completed.get ()
        || reentrant_session.events != std::vector<std::string>{"connected", "disconnected"}) {
        return 285;
    }
    /* stream connector §5.2: Response는 request의 packet name을 그대로 되돌린다. */
    if (runtime.written_headers (stream)[0].kind () != stream_message_kind_t::response
        || runtime.written_headers (stream)[0].request_seq () != 77
        || runtime.written_headers (stream)[0].actor_slot ()
        || !runtime.written_headers (stream)[0].packet_name ().empty ()) {
        return 15;
    }
    const auto stale_error_header =
      zlink::framework::detail::stream_runtime_t::make_terminal_header (
        stream_message_kind_t::error, request_header);
    if (stale_error_header.actor_slot ()) {
        return 321;
    }

    /* Session Actor binding §5: the owner issues one stable slot, publishes
     * bound before the first slotted packet, resolves current ingress into the
     * dispatch context, and copies the slot to the terminal reply. */
    auto actor_stream = runtime.open_session ("client-stream");
    zlink::framework::detail::actor_gateway_runtime_t actor_gateway;
    auto actor_manager = actor_gateway.manager ();
    zlink::framework::detail::session_actor_manager_access_t::attach (actor_manager, actor_stream);
    auto actor_ref = zlink::framework::detail::actor_ref_access_t::make (
      zlink::framework::node_rid_t::from_string ("actor-node"), "PlayerActor", "slot-actor", 1);
    const auto first_bind = actor_manager.bind (actor_ref).async ().result ();
    const auto repeated_bind = actor_manager.bind (actor_ref).async ().result ();
    const auto bound_control_deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (2);
    auto bound_headers = runtime.written_headers (actor_stream);
    while (bound_headers.empty () && std::chrono::steady_clock::now () < bound_control_deadline) {
        std::this_thread::yield ();
        bound_headers = runtime.written_headers (actor_stream);
    }
    if (!first_bind || !repeated_bind || bound_headers.size () != 1
        || bound_headers[0].kind () != stream_message_kind_t::control
        || bound_headers[0].packet_name () != "$zlink.actor.bound"
        || bound_headers[0].actor_slot ()) {
        std::cerr << "bound control count=" << bound_headers.size () << '\n';
        return 296;
    }
    const auto bound_payload = runtime.written_payloads (actor_stream)[0].to_string ();
    if (std::vector<std::uint8_t> (bound_payload.begin (), bound_payload.end ())
        != zlink::framework::detail::stream_runtime_t::encode_actor_bound_payload (1,
                                                                                   "slot-actor")) {
        return 297;
    }
    auto actor_request_header = request_header;
    actor_request_header.with_actor_slot (1);
    sample_session_t actor_session;
    if (!runtime.dispatch_packet (actor_session, actor_stream, actor_request_header,
                                  zlink::message_t::from (std::string ("actor-payload")))) {
        return 298;
    }
    const auto actor_written_headers = runtime.written_headers (actor_stream);
    if (actor_session.last_actor_id != "slot-actor" || actor_written_headers.size () != 2
        || actor_written_headers[1].kind () != stream_message_kind_t::response
        || actor_written_headers[1].actor_slot () != 1) {
        return 299;
    }
    const auto actor_error_header =
      zlink::framework::detail::stream_runtime_t::make_terminal_header (
        stream_message_kind_t::error, actor_request_header);
    if (actor_error_header.actor_slot () != 1) {
        return 322;
    }
    auto stale_request_header = actor_request_header;
    stale_request_header.with_actor_slot (2);
    const auto stale_request =
      runtime.dispatch_packet (actor_session, actor_stream, stale_request_header,
                               zlink::message_t::from (std::string ("stale-request")));
    if (stale_request || stale_request.error_kind () != framework_error_kind_t::invalid_operation
        || actor_session.events.size () != 1) {
        return 323;
    }
    zlink::framework::detail::stream_header_t stale_send_header (
      stream_message_kind_t::send, stream_codec_t::json, stream_header_flags_t::none, std::nullopt,
      "move", {});
    stale_send_header.with_actor_slot (2);
    const auto stale_send_result =
      runtime.dispatch_packet (actor_session, actor_stream, stale_send_header,
                               zlink::message_t::from (std::string ("stale-send")));
    zlink::framework::test::acknowledge_delivery ();
    if (!stale_send_result || actor_session.events.size () != 1
        || flow_state->none_of ([] (const auto &event) {
               return event.outcome == zlink::framework::message_flow_outcome_t::dropped
                      && event.message_kind == zlink::framework::dispatch_message_kind_t::send
                      && event.reason == zlink::framework::message_flow_reason_t::stale_target;
           })) {
        return 324;
    }
    bool hidden_until_bound = false;
    zlink::framework::detail::session_actor_manager_access_t::bind_native (
      actor_manager, [&actor_manager, &hidden_until_bound] (zlink::framework::actor_ref_t actor,
                                                            std::uint64_t, std::uint16_t slot) {
          hidden_until_bound =
            slot == 2
            && !actor_manager.find (std::string (actor.actor_id ().value ())).has_value ();
          return zlink::framework::request_task_t<void> (
            zlink::framework::result_t<void>::success ());
      });
    auto publish_actor = zlink::framework::detail::actor_ref_access_t::make (
      zlink::framework::node_rid_t::from_string ("actor-node"), "PlayerActor",
      "publish-after-bound", 1);
    if (!actor_manager.bind (std::move (publish_actor)).async ().result () || !hidden_until_bound
        || !actor_manager.find ("publish-after-bound"))
        return 300;
    for (std::uint32_t slot = 3; slot <= std::numeric_limits<std::uint16_t>::max (); ++slot) {
        auto next_actor = zlink::framework::detail::actor_ref_access_t::make (
          zlink::framework::node_rid_t::from_string ("actor-node"), "PlayerActor",
          "slot-actor-" + std::to_string (slot), 1);
        if (!actor_manager.bind (std::move (next_actor)).async ().result ())
            return 301;
    }
    auto exhausted_actor = zlink::framework::detail::actor_ref_access_t::make (
      zlink::framework::node_rid_t::from_string ("actor-node"), "PlayerActor", "slot-overflow", 1);
    const auto exhausted = actor_manager.bind (std::move (exhausted_actor)).async ().result ();
    if (exhausted || exhausted.error_kind () != framework_error_kind_t::invalid_operation)
        return 302;

    auto push_codec_stream = runtime.open_session ("client-stream");
    sample_session_t push_codec_session;
    if (!runtime.dispatch_packet (push_codec_session, push_codec_stream, request_header,
                                  zlink::message_t::from (std::string ("codec-selection")))) {
        return 292;
    }
    push_codec_stream.write_packet (zlink::framework::message_t::from (std::string ("json-push")))
      .packet_name ("JsonPush")
      .async ()
      .result ()
      .value ();
    const auto push_codec_headers = runtime.written_headers (push_codec_stream);
    if (push_codec_headers.size () != 2
        || push_codec_headers.back ().kind () != stream_message_kind_t::send
        || push_codec_headers.back ().codec () != stream_codec_t::json) {
        return 293;
    }

    auto duplicate_reply_stream = runtime.open_session ("client-stream");
    duplicate_reply_session_t duplicate_reply_session;
    if (!runtime.dispatch_packet (duplicate_reply_session, duplicate_reply_stream, request_header,
                                  zlink::message_t::from (std::string ("duplicate-reply")))
        || !duplicate_reply_session.winner_completed || !duplicate_reply_session.loser_rejected
        || runtime.written_headers (duplicate_reply_stream).size () != 1) {
        return 236;
    }

    auto failed_reply_stream = runtime.open_session ("client-stream");
    std::size_t failed_reply_attempts = 0;
    runtime.attach_transport_writer (
      failed_reply_stream, [&failed_reply_attempts] (const auto &, const auto &) {
          ++failed_reply_attempts;
          return zlink::framework::task_t<void> (zlink::framework::result_t<void>::failure (
            framework_error_kind_t::internal_failure, "stream transport rejected reply"));
      });
    failed_reply_session_t failed_reply_session;
    if (!runtime.dispatch_packet (failed_reply_session, failed_reply_stream, request_header,
                                  zlink::message_t::from (std::string ("failed-reply")))
        || !failed_reply_session.first_failed || !failed_reply_session.second_rejected
        || failed_reply_attempts != 1 || !runtime.written_headers (failed_reply_stream).empty ()) {
        return 237;
    }

    auto heartbeat_stream = runtime.open_session ("client-stream");
    runtime.dispatch_control_frame (heartbeat_stream,
                                    zlink::framework::detail::stream_header_t (
                                      stream_message_kind_t::control, stream_codec_t::raw,
                                      zlink::framework::detail::stream_header_flags_t::none,
                                      std::nullopt, zlink::framework::detail::heartbeat_ping_name,
                                      {}),
                                    zlink::message_t{});
    const auto heartbeat_headers = runtime.written_headers (heartbeat_stream);
    if (heartbeat_headers.size () != 1
        || heartbeat_headers[0].kind () != stream_message_kind_t::control
        || heartbeat_headers[0].codec () != stream_codec_t::raw
        || heartbeat_headers[0].request_seq ()
        || heartbeat_headers[0].packet_name () != "$zlink.heartbeat.pong") {
        return 235;
    }

    {
        pending_connected_session_t pending_connected;
        auto pending_stream = runtime.open_session ("client-stream");
        auto entered = pending_connected.entered.get_future ();
        auto connected = pending_connected.connected.get_future ();
        auto packet = pending_connected.packet.get_future ();
        const auto submitted =
          runtime.dispatch_connected_async (pending_connected, pending_stream, {});
        if (!submitted || entered.wait_for (std::chrono::seconds (2)) != std::future_status::ready)
            return 323;
        using liveness = zlink::framework::runtime::session_liveness_t;
        const auto established = liveness::clock_t::time_point{};
        liveness pending_policy (established, established + std::chrono::seconds (10));
        if (pending_policy.evaluate (established + std::chrono::seconds (5))
            != liveness::decision_t::heartbeat_timeout)
            return 324;
        bool closed_while_connected_pending = false;
        runtime.send_session_closing (
          pending_stream, zlink::framework::stream_close_reason_t::heartbeat_timeout,
          "timeout during on_connected", [&] { closed_while_connected_pending = true; });
        if (!closed_while_connected_pending
            || connected.wait_for (std::chrono::seconds (0)) == std::future_status::ready)
            return 328;
        if (!runtime.dispatch_packet_async (pending_connected, pending_stream, request_header,
                                            zlink::message_t::from ("queued"), {}))
            return 325;
        if (packet.wait_for (std::chrono::seconds (0)) == std::future_status::ready)
            return 326;
        pending_connected.resume.complete (zlink::framework::result_t<void>::success ());
        if (connected.wait_for (std::chrono::seconds (2)) != std::future_status::ready
            || packet.wait_for (std::chrono::seconds (2)) != std::future_status::ready)
            return 327;
    }

    // Physical close follows control-frame completion, including send failure.
    for (const bool success : {true, false}) {
        auto closing_stream = runtime.open_session ("client-stream");
        zlink::framework::task_completion_source_t<void> admission;
        runtime.attach_transport_writer (
          closing_stream, [&admission] (const auto &, const auto &) { return admission.task (); });
        bool physically_closed = false;
        std::promise<void> lifetime_released;
        auto lifetime_released_future = lifetime_released.get_future ();
        auto lifetime = std::shared_ptr<int> (new int (0), [&lifetime_released] (int *value) {
            delete value;
            lifetime_released.set_value ();
        });
        std::weak_ptr<int> retained = lifetime;
        runtime.send_session_closing (
          closing_stream, zlink::framework::stream_close_reason_t::heartbeat_timeout,
          "no inbound frame", [&, lifetime] { physically_closed = true; });
        lifetime.reset ();
        if (retained.expired ())
            return 329;
        if (physically_closed)
            return 319;
        admission.complete (
          success ? zlink::framework::result_t<void>::success ()
                  : zlink::framework::result_t<void>::failure (
                      zlink::framework::framework_error_kind_t::unavailable, "send rejected"));
        // The close callback runs in a new execution turn after complete() returns.
        lifetime_released_future.wait ();
        if (!physically_closed || !retained.expired ())
            return 320;
    }

    auto delayed_stream = runtime.open_session ("client-stream");
    delayed_reply_session_t delayed_session;
    std::optional<zlink::framework::result_t<void>> delayed_dispatch;
    std::thread delayed_dispatch_thread ([&] {
        delayed_dispatch =
          runtime.dispatch_packet (delayed_session, delayed_stream, request_header,
                                   zlink::message_t::from (std::string ("delayed-payload")));
    });
    delayed_session.wait_until_suspended ();
    delayed_session.resume ();
    delayed_dispatch_thread.join ();
    if (!delayed_dispatch || !*delayed_dispatch || !delayed_session.reply_result
        || !*delayed_session.reply_result || runtime.written_headers (delayed_stream).size () != 1
        || runtime.written_headers (delayed_stream)[0].request_seq () != 77
        || !runtime.written_headers (delayed_stream)[0].packet_name ().empty ()
        || runtime.written_payloads (delayed_stream)[0].to_string () != "delayed-payload") {
        return 29;
    }

    /* Native Core callbacks must return after enqueueing. The suspended packet
     * keeps the per-session turn, so disconnect cannot overtake it and close
     * the stream before its reply is written. */
    auto async_stream = runtime.open_session ("client-stream");
    delayed_reply_session_t async_session;
    std::promise<zlink::framework::result_t<void>> packet_completion_source;
    auto packet_completion = packet_completion_source.get_future ();
    const auto async_submitted = runtime.dispatch_packet_async (
      async_session, async_stream, request_header,
      zlink::message_t::from (std::string ("async-payload")),
      [&packet_completion_source] (const zlink::framework::result_t<void> &result) {
          packet_completion_source.set_value (result);
      });
    if (!async_submitted) {
        return 230;
    }
    async_session.wait_until_suspended ();
    if (packet_completion.wait_for (std::chrono::milliseconds::zero ())
        == std::future_status::ready) {
        return 231;
    }
    std::promise<zlink::framework::result_t<void>> error_completion_source;
    auto error_completion = error_completion_source.get_future ();
    const auto error_submitted = runtime.dispatch_error_async (
      async_session, async_stream,
      zlink::framework::stream_error_t (zlink::framework::stream_session_error_t::transport_error,
                                        "async-transport"),
      [&error_completion_source] (const zlink::framework::result_t<void> &result) {
          error_completion_source.set_value (result);
      });
    if (!error_submitted
        || error_completion.wait_for (std::chrono::milliseconds::zero ())
             == std::future_status::ready)
        return 349;
    std::promise<zlink::framework::result_t<void>> disconnect_completion_source;
    auto disconnect_completion = disconnect_completion_source.get_future ();
    const auto disconnect_submitted = runtime.dispatch_disconnected_async (
      async_session, async_stream,
      [&disconnect_completion_source] (const zlink::framework::result_t<void> &result) {
          disconnect_completion_source.set_value (result);
      });
    if (!disconnect_submitted
        || disconnect_completion.wait_for (std::chrono::milliseconds::zero ())
             == std::future_status::ready) {
        return 232;
    }
    async_session.resume ();
    if (packet_completion.wait_for (std::chrono::seconds (2)) != std::future_status::ready
        || !packet_completion.get ()
        || error_completion.wait_for (std::chrono::seconds (2)) != std::future_status::ready
        || !error_completion.get ()
        || disconnect_completion.wait_for (std::chrono::seconds (2)) != std::future_status::ready
        || !disconnect_completion.get ()) {
        return 233;
    }
    runtime.drain_async_dispatch (async_stream);
    if (async_session.callbacks
          != std::vector<std::string>{"packet", "error:async-transport", "disconnected"}
        || !async_session.reply_result || !*async_session.reply_result
        || runtime.written_payloads (async_stream).size () != 1
        || runtime.written_payloads (async_stream)[0].to_string () != "async-payload") {
        return 234;
    }

    /* Session Actor binding §5: resolve a slot when its packet turn starts.
     * A binding published while an earlier packet owns the turn is current
     * for the queued packet. */
    auto slot_stream = runtime.open_session ("client-stream");
    zlink::framework::detail::actor_gateway_runtime_t slot_gateway;
    auto slot_manager = slot_gateway.manager ();
    zlink::framework::detail::session_actor_manager_access_t::attach (slot_manager, slot_stream);
    delayed_reply_session_t slot_blocker;
    auto slot_blocker_source = std::make_shared<std::promise<zlink::framework::result_t<void>>> ();
    auto slot_blocker_completion = slot_blocker_source->get_future ();
    if (!runtime.dispatch_packet_async (
          slot_blocker, slot_stream, request_header, zlink::message_t::from ("slot-blocker"),
          [slot_blocker_source] (const auto &result) { slot_blocker_source->set_value (result); }))
        return 326;
    slot_blocker.wait_until_suspended ();
    auto queued_slot_header = request_header;
    queued_slot_header.with_actor_slot (1);
    sample_session_t slot_session;
    auto slot_packet_source = std::make_shared<std::promise<zlink::framework::result_t<void>>> ();
    auto slot_packet_completion = slot_packet_source->get_future ();
    const auto slot_submitted = runtime.dispatch_packet_async (
      slot_session, slot_stream, queued_slot_header, zlink::message_t::from ("queued-slot"),
      [slot_packet_source] (const auto &result) { slot_packet_source->set_value (result); });
    auto slot_ref = zlink::framework::detail::actor_ref_access_t::make (
      zlink::framework::node_rid_t::from_string ("actor-node"), "PlayerActor", "queued-actor", 1);
    const auto slot_bound = slot_manager.bind (std::move (slot_ref)).async ().result ();
    slot_blocker.resume ();
    const auto blocker_done =
      slot_blocker_completion.wait_for (std::chrono::seconds (2)) == std::future_status::ready
      && slot_blocker_completion.get ();
    const auto packet_done =
      slot_submitted
      && slot_packet_completion.wait_for (std::chrono::seconds (2)) == std::future_status::ready
      && slot_packet_completion.get ();
    if (!slot_submitted || !slot_bound || !blocker_done || !packet_done
        || slot_session.last_actor_id != "queued-actor")
        return 327;

    /* Actor binding replacement is queued behind the same session lane. A
     * normal callback retains its owner through completion and executes once. */
    auto replacement_stream = runtime.open_session ("client-stream");
    auto replacement_owner = std::make_shared<int> (1);
    const auto replacement_owner_weak = std::weak_ptr<int> (replacement_owner);
    auto replacement_calls = std::make_shared<std::atomic_size_t> (0);
    bool replacement_owner_released_on_completion = false;
    std::promise<zlink::framework::result_t<void>> replacement_completion_source;
    auto replacement_completion = replacement_completion_source.get_future ();
    const auto replacement_submitted = runtime.dispatch_actor_binding_replaced_async (
      replacement_stream, "actor-exact-once",
      [owner = std::move (replacement_owner), replacement_calls] (zlink::framework::stream_t &,
                                                                  std::string actor_id) {
          if (*owner == 1 && actor_id == "actor-exact-once") {
              replacement_calls->fetch_add (1, std::memory_order_relaxed);
          }
          return zlink::framework::task_t<void> (zlink::framework::result_t<void>::success ());
      },
      [&replacement_completion_source, &replacement_owner_released_on_completion,
       &replacement_owner_weak] (const zlink::framework::result_t<void> &result) {
          replacement_owner_released_on_completion = replacement_owner_weak.expired ();
          replacement_completion_source.set_value (result);
      });
    if (!replacement_submitted
        || replacement_completion.wait_for (std::chrono::seconds (2)) != std::future_status::ready
        || !replacement_completion.get ()) {
        return 286;
    }
    runtime.drain_async_dispatch (replacement_stream);
    if (!replacement_owner_released_on_completion
        || replacement_calls->load (std::memory_order_relaxed) != 1
        || !replacement_owner_weak.expired ()) {
        std::cerr << "actor-binding-replacement result=287 calls="
                  << replacement_calls->load (std::memory_order_relaxed)
                  << " owner_released_on_completion=" << replacement_owner_released_on_completion
                  << " owner_expired=" << replacement_owner_weak.expired () << '\n';
        return 287;
    }

    /* Teardown first disables replacement admission. Even when an earlier
     * packet keeps the serial turn, the queued replacement must not enter a
     * released session owner after close. */
    auto replacement_race_stream = runtime.open_session ("client-stream");
    delayed_reply_session_t replacement_blocker;
    std::promise<zlink::framework::result_t<void>> blocker_completion_source;
    auto blocker_completion = blocker_completion_source.get_future ();
    const auto blocker_submitted = runtime.dispatch_packet_async (
      replacement_blocker, replacement_race_stream, request_header,
      zlink::message_t::from (std::string ("replacement-blocker")),
      [&blocker_completion_source] (const zlink::framework::result_t<void> &result) {
          blocker_completion_source.set_value (result);
      });
    if (!blocker_submitted) {
        return 288;
    }
    replacement_blocker.wait_until_suspended ();
    auto replacement_admission = std::make_shared<std::atomic_bool> (true);
    auto closing_owner = std::make_shared<int> (2);
    const auto closing_owner_weak = std::weak_ptr<int> (closing_owner);
    auto closed_session_calls = std::make_shared<std::atomic_size_t> (0);
    std::promise<zlink::framework::result_t<void>> closed_completion_source;
    auto closed_completion = closed_completion_source.get_future ();
    const auto closed_submitted = runtime.dispatch_actor_binding_replaced_async (
      replacement_race_stream, "actor-close-race",
      [weak = closing_owner_weak, replacement_admission,
       closed_session_calls] (zlink::framework::stream_t &, std::string) {
          if (!replacement_admission->load (std::memory_order_acquire)) {
              return zlink::framework::task_t<void> (zlink::framework::result_t<void>::success ());
          }
          if (weak.lock ()) {
              closed_session_calls->fetch_add (1, std::memory_order_relaxed);
          }
          return zlink::framework::task_t<void> (zlink::framework::result_t<void>::success ());
      },
      [&closed_completion_source] (const zlink::framework::result_t<void> &result) {
          closed_completion_source.set_value (result);
      });
    if (!closed_submitted) {
        return 289;
    }
    replacement_admission->store (false, std::memory_order_release);
    closing_owner.reset ();
    replacement_blocker.resume ();
    if (blocker_completion.wait_for (std::chrono::seconds (2)) != std::future_status::ready
        || !blocker_completion.get ()
        || closed_completion.wait_for (std::chrono::seconds (2)) != std::future_status::ready
        || !closed_completion.get ()) {
        return 290;
    }
    runtime.drain_async_dispatch (replacement_race_stream);
    if (closed_session_calls->load (std::memory_order_relaxed) != 0
        || !closing_owner_weak.expired ()) {
        return 291;
    }

    auto fluent_stream = runtime.open_session ("client-stream");
    auto send_call =
      fluent_stream.write_packet (zlink::framework::message_t::from (std::string ("send-payload")));
    send_call.packet_name ("original");
    if (!runtime.written_headers (fluent_stream).empty ()) {
        return 17;
    }
    send_call.metadata ("trace", "send-trace")
      .packet_name ("renamed")
      .compress ()
      .async ()
      .result ()
      .value ();
    bool duplicate_send_rejected = false;
    try {
        (void) send_call.async ().result ().value ();
    }
    catch (const zlink::framework::framework_exception_t &error) {
        duplicate_send_rejected =
          error.kind () == zlink::framework::framework_error_kind_t::invalid_operation;
    }
    if (runtime.written_headers (fluent_stream).size () != 1 || !duplicate_send_rejected
        || runtime.written_headers (fluent_stream)[0].packet_name () != "renamed"
        || runtime.written_headers (fluent_stream)[0].metadata ("trace") != "send-trace"
        || (runtime.written_headers (fluent_stream)[0].flags ()
            & stream_header_flags_t::payload_compressed)
             != stream_header_flags_t::payload_compressed) {
        return 18;
    }
    if (runtime.written_payloads (fluent_stream).empty ()
        || runtime.written_payloads (fluent_stream)[0].to_string () == "send-payload") {
        return 23;
    }

    const auto close_result = fluent_stream.close ().result ();
    const auto write_rejected_disconnected = [] (auto &&write_fn) {
        try {
            write_fn ();
            return false;
        }
        catch (const zlink::framework::framework_exception_t &error) {
            return error.kind () == zlink::framework::framework_error_kind_t::unavailable;
        }
    };
    if (!write_rejected_disconnected ([&] {
            return fluent_stream
              .write_packet (zlink::framework::message_t::from (std::string ("after-close")))
              .async ()
              .result ()
              .value ();
        })) {
        return 24;
    }
    if (!close_result || runtime.written_headers (fluent_stream).size () != 1) {
        return 19;
    }
    if (!write_rejected_disconnected ([&] {
            return stream
              .write_packet (zlink::framework::message_t::from (std::string ("after-disconnect")))
              .async ()
              .result ()
              .value ();
        })) {
        return 25;
    }
    if (runtime.written_headers (stream).size () != 1) {
        return 16;
    }

    sample_session_t validation_session;
    auto validation_stream = runtime.open_session ("client-stream");
    const auto rejected =
      runtime.dispatch_packet (validation_session, validation_stream, invalid_send,
                               zlink::message_t::from (std::string ("bad")));
    if (rejected || rejected.error_kind () != framework_error_kind_t::protocol_error
        || !validation_session.events.empty ()) {
        return 12;
    }

    throwing_packet_session_t throwing_session;
    auto throwing_stream = runtime.open_session ("client-stream");
    const auto handler_failure =
      runtime.dispatch_packet (throwing_session, throwing_stream, request_header,
                               zlink::message_t::from (std::string ("payload")));
    if (handler_failure || handler_failure.error_kind () != framework_error_kind_t::internal_failure
        || throwing_session.on_error_called) {
        return 13;
    }

    sample_session_t error_session;
    auto error_stream = runtime.open_session ("client-stream");
    const auto transport_error = runtime.dispatch_error (
      error_session, error_stream,
      zlink::framework::stream_error_t (zlink::framework::stream_session_error_t::transport_error,
                                        "transport"));
    if (!transport_error || error_session.events.size () != 1
        || error_session.events[0] != "error:transport") {
        return 14;
    }

    return stream_listener_cases (metric_provider);
}
} // namespace zlink::framework::test::stream_framework
