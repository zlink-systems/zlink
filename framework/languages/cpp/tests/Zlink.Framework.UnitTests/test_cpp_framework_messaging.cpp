/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/messaging/pending_submit.hpp"
#include "runtime/messaging/async_submit_runtime.hpp"
#include "runtime/messaging/client_call_codec.hpp"
#include "runtime/diagnostics/flow_context.hpp"
#include "runtime/messaging/envelope_codec.hpp"
#include "runtime/messaging/request_failure_mapper.hpp"
#include "runtime/messaging/submit_result_mapper.hpp"
#include "runtime/messaging/submit_queue.hpp"
#include "runtime/spots/spot_route_packets.hpp"

#include <service_wire_constants.hpp>

#include <zlink/framework/contracts/detail/call_facade.hpp>
#include <zlink/framework/contracts/errors/result.hpp>
#include <zlink/framework/contracts/monitoring/route_mesh_runtime.hpp>

#include <algorithm>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{

class sample_call_t : public zlink::framework::detail::call_facade_t<sample_call_t, int>
{
  public:
    explicit sample_call_t (int value) :
        call_facade_t (zlink::framework::result_t<int>::success (value))
    {
    }
};

struct envelope_payload_t
{
    int value{};
};

} // namespace

int main ()
{
    {
        zlink::framework::serializer_registry_t serializers;
        zlink::framework::detail::
          register_spot_route_packet_serializers (serializers);
        const auto admission_root =
          serializers
            .get<zlink::framework::detail::
                   spot_actor_admission_route_reply_t> ()
            .deserialize (
              serializers
                .get<zlink::framework::detail::
                       spot_actor_admission_route_reply_t> ()
                .serialize (
                  zlink::framework::detail::
                    spot_actor_admission_route_reply_t{
                      true,
                      {1, 2},
                      "join-root",
                      0x12345678}));
        if (admission_root.completion_root_reference
              != "join-root"
            || admission_root.completion_root_checksum
                 != 0x12345678) {
            return 150;
        }
        const auto commit_root =
          serializers
            .get<zlink::framework::detail::
                   spot_actor_commit_route_request_t> ()
            .deserialize (
              serializers
                .get<zlink::framework::detail::
                       spot_actor_commit_route_request_t> ()
                .serialize (
                  zlink::framework::detail::
                    spot_actor_commit_route_request_t{
                      .transfer_id = "transfer-root",
                      .completion_root_reference =
                        "join-root",
                      .completion_root_checksum =
                        0x12345678}));
        if (commit_root.completion_root_reference
              != "join-root"
            || commit_root.completion_root_checksum
                 != 0x12345678) {
            return 151;
        }
        auto oversized_count =
          zlink::framework::detail::spot_actor_commit_route_request_t{};
        oversized_count.handoff_backlog.resize (
          zlink::framework::runtime::protocol::messageFollowMessages + 1);
        for (auto &packet : oversized_count.handoff_backlog)
            packet.packet_name_value = "handoff";
        try {
            (void) serializers
              .get<zlink::framework::detail::spot_actor_commit_route_request_t> ()
              .deserialize (
                serializers
                  .get<zlink::framework::detail::spot_actor_commit_route_request_t> ()
                  .serialize (oversized_count));
            return 152;
        }
        catch (const std::exception &) {
        }

        auto oversized_bytes =
          zlink::framework::detail::spot_actor_commit_route_request_t{};
        oversized_bytes.handoff_backlog.push_back (
          zlink::framework::detail::spot_actor_handoff_packet_t{
            .packet_name_value = "handoff",
            .payload = std::vector<std::uint8_t> (
              zlink::framework::runtime::protocol::messageFollowBytes + 1)});
        try {
            (void) serializers
              .get<zlink::framework::detail::spot_actor_commit_route_request_t> ()
              .deserialize (
                serializers
                  .get<zlink::framework::detail::spot_actor_commit_route_request_t> ()
                  .serialize (oversized_bytes));
            return 153;
        }
        catch (const std::exception &) {
        }
    }
    {
        zlink::framework::serializer_registry_t serializers;
        serializers.add<envelope_payload_t> (
          [] (const envelope_payload_t &payload) {
              return zlink::framework::encoded_payload_t::from_string (
                std::to_string (payload.value));
          },
          [] (const zlink::framework::encoded_payload_t &payload) {
              return envelope_payload_t{std::stoi (payload.to_string ())};
          });

        zlink::framework::runtime::messaging::client_call_codec_t client_codec;
        const auto header = client_codec.create_envelope (
          zlink::framework::runtime::messaging::message_kind_t::request, "profile",
          "EnvelopePayload", std::chrono::milliseconds (10), std::string ("lookup"),
          std::string ("client"));
        const auto parts =
          client_codec.encode_envelope_parts (header, envelope_payload_t{42}, serializers);
        zlink::framework::runtime::messaging::envelope_codec_t envelope_codec;
        const auto decoded_header = envelope_codec.decode_header (parts);
        if (!decoded_header
            || decoded_header.value ().kind
                 != zlink::framework::runtime::messaging::message_kind_t::request
            || decoded_header.value ().channel_name != "profile"
            || decoded_header.value ().message_name != "EnvelopePayload"
            || decoded_header.value ().content_type != "application/octet-stream"
            || decoded_header.value ().correlation_id.empty () || !decoded_header.value ().deadline
            || decoded_header.value ().topic.value_or ("") != "lookup"
            || decoded_header.value ().source.value_or ("") != "client") {
            return 11;
        }
        const auto decoded_body = envelope_codec.decode_body (parts);
        if (!decoded_body
            || serializers.get<envelope_payload_t> ()
                   .deserialize (zlink::framework::detail::encoded_payload_from_raw (
                     decoded_body.value ()))
                   .value
                 != 42) {
            return 12;
        }

        zlink::framework::runtime::messaging::envelope_header_t reply_header;
        reply_header.kind = zlink::framework::runtime::messaging::message_kind_t::response;
        reply_header.channel_name = "profile";
        reply_header.message_name = "EnvelopePayload";
        reply_header.correlation_id = header.correlation_id;
        const auto reply = envelope_codec.encode_raw_body_parts (
          reply_header, zlink::message_t::from (std::string ("7")));
        const auto reply_result = client_codec.decode_envelope_reply<envelope_payload_t> (
          reply, serializers, "empty reply", "reply failed", "profile request");
        if (!reply_result || reply_result.value ().value != 7) {
            return 13;
        }

        zlink::framework::runtime::messaging::envelope_header_t error_header;
        error_header.kind = zlink::framework::runtime::messaging::message_kind_t::error;
        error_header.channel_name = "profile";
        error_header.message_name = "EnvelopePayload";
        error_header.error_code = "route_not_connected";
        error_header.error_message = "route is down";
        const auto error_reply = envelope_codec.encode_raw_body_parts (
          error_header, zlink::message_t::from (std::string{}));
        const auto error_result = client_codec.decode_envelope_reply<envelope_payload_t> (
          error_reply, serializers, "empty reply", "reply failed", "profile request");
        if (error_result
            || error_result.error_kind ()
                 != zlink::framework::framework_error_kind_t::unavailable
            || error_result.error () == nullptr) {
            return 14;
        }

        zlink::framework::runtime::messaging::envelope_header_t wire_error_header;
        wire_error_header.kind = zlink::framework::runtime::messaging::message_kind_t::error;
        wire_error_header.channel_name = "profile";
        wire_error_header.message_name = "MissingProfileReq";
        wire_error_header.correlation_id = "request-2";
        wire_error_header.error_code = "handler_not_found";
        wire_error_header.error_message = "missing handler";
        const auto wire_error_json = envelope_codec.encode_header (wire_error_header).to_string ();
        if (wire_error_json.find (R"("kind":5)") == std::string::npos
            || wire_error_json.find (R"("errorCode":"handler_not_found")")
                 == std::string::npos
            || wire_error_json.find (R"("errorMessage":"missing handler")")
                 == std::string::npos
            || wire_error_json.find (R"("status")") != std::string::npos) {
            return 53;
        }
        const auto decoded_wire_error = envelope_codec.decode_header (
          zlink::message_t::from (wire_error_json));
        if (!decoded_wire_error
            || decoded_wire_error.value ().kind
                 != zlink::framework::runtime::messaging::message_kind_t::error
            || decoded_wire_error.value ().error_code != "handler_not_found"
            || decoded_wire_error.value ().error_message != "missing handler") {
            return 54;
        }

        zlink::framework::runtime::messaging::envelope_header_t wire_response_header;
        wire_response_header.kind = zlink::framework::runtime::messaging::message_kind_t::response;
        wire_response_header.channel_name = "profile";
        wire_response_header.message_name = "ProfileReq";
        wire_response_header.correlation_id = "request-3";
        const auto wire_response_json =
          envelope_codec.encode_header (wire_response_header).to_string ();
        if (wire_response_json.find (R"("kind":2)") == std::string::npos
            || wire_response_json.find (R"("errorCode":null)") == std::string::npos
            || wire_response_json.find (R"("errorMessage":null)") == std::string::npos
            || wire_response_json.find (R"("status")") != std::string::npos) {
            return 55;
        }

        zlink::framework::runtime::messaging::request_failure_mapper_t mapper;
        using zlink::framework::framework_error_kind_t;
        using zlink::framework::runtime::messaging::map_request_result_exception;
        using zlink::framework::runtime::messaging::map_submit_result_exception;
        using zlink::framework::runtime::messaging::map_submit_result_error_kind;
        if (map_submit_result_error_kind (zlink::submit_result_t::backpressured)
                != framework_error_kind_t::capacity_exceeded
            || map_submit_result_error_kind (zlink::submit_result_t::not_connected)
                 != framework_error_kind_t::unavailable
            || map_submit_result_error_kind (zlink::submit_result_t::not_found)
                 != framework_error_kind_t::not_found
            || map_submit_result_error_kind (zlink::submit_result_t::not_admitted)
                 != framework_error_kind_t::rejected
            || map_submit_result_error_kind (zlink::submit_result_t::terminated)
                 != framework_error_kind_t::shutting_down
            || map_submit_result_error_kind (zlink::submit_result_t::invalid_argument)
                 != framework_error_kind_t::invalid_operation
            || map_submit_result_error_kind (zlink::submit_result_t::invalid_state)
                 != framework_error_kind_t::invalid_operation) {
            return 25;
        }
        const auto submit_backpressured = map_submit_result_exception (
          zlink::submit_result_t::backpressured, "native submit");
        const auto submit_disconnected = map_submit_result_exception (
          zlink::submit_result_t::not_connected, "native submit");
        const auto submit_shutdown = map_submit_result_exception (
          zlink::submit_result_t::terminated, "native submit");
        const auto submit_not_found = map_submit_result_exception (
          zlink::submit_result_t::not_found, "native submit");
        if (submit_backpressured.kind () != framework_error_kind_t::capacity_exceeded
            || submit_disconnected.kind () != framework_error_kind_t::unavailable
            || zlink::framework::detail::boundary_state (submit_disconnected)
                 != zlink::framework::detail::boundary_error_t::disconnected
            || submit_shutdown.kind () != framework_error_kind_t::shutting_down
            || zlink::framework::detail::boundary_state (submit_shutdown)
                 != zlink::framework::detail::boundary_error_t::shutdown
            || submit_not_found.kind () != framework_error_kind_t::not_found) {
            return 26;
        }
        const auto native_timeout = map_request_result_exception (
          zlink::request_result_t::timed_out, "native request");
        const auto native_disconnected = map_request_result_exception (
          zlink::request_result_t::not_connected, "native request");
        const auto native_shutdown = map_request_result_exception (
          zlink::request_result_t::terminated, "native request");
        const auto native_rejected = map_request_result_exception (
          zlink::request_result_t::rejected, "native request");
        const auto native_busy = map_request_result_exception (
          zlink::request_result_t::busy, "native request");
        if (zlink::framework::detail::boundary_state (native_timeout)
                != zlink::framework::detail::boundary_error_t::timed_out
            || zlink::framework::detail::boundary_state (native_disconnected)
                 != zlink::framework::detail::boundary_error_t::disconnected
            || native_disconnected.code ()
                 != std::make_error_code (std::errc::not_connected)
            || zlink::framework::detail::boundary_state (native_shutdown)
                 != zlink::framework::detail::boundary_error_t::shutdown
            || native_shutdown.kind () != framework_error_kind_t::shutting_down
            || native_rejected.kind () != framework_error_kind_t::rejected
            || native_busy.kind () != framework_error_kind_t::capacity_exceeded
            || mapper.reply_header_exception (113, 0, "native request").kind ()
                 != framework_error_kind_t::capacity_exceeded) {
            return 26;
        }
        const auto not_connected = mapper.completion_exception (
          zlink::framework::runtime::messaging::request_result_t::not_connected, "profile request");
        if (not_connected.kind () != zlink::framework::framework_error_kind_t::unavailable
            || zlink::framework::detail::boundary_state (not_connected)
                 != zlink::framework::detail::boundary_error_t::disconnected
            || not_connected.code ()
                 != std::make_error_code (std::errc::not_connected)) {
            return 16;
        }
        const auto not_found = mapper.completion_exception (
          zlink::framework::runtime::messaging::request_result_t::not_found, "profile request");
        if (not_found.kind () != zlink::framework::framework_error_kind_t::not_found) {
            return 17;
        }
        const auto timed_out = mapper.completion_exception (
          zlink::framework::runtime::messaging::request_result_t::timed_out, "profile request");
        if (zlink::framework::detail::boundary_state (timed_out) != zlink::framework::detail::boundary_error_t::timed_out
            || timed_out.kind () != zlink::framework::framework_error_kind_t::deadline_exceeded) {
            return 18;
        }
        const auto busy = mapper.completion_exception (
          zlink::framework::runtime::messaging::request_result_t::busy, "profile request");
        if (busy.kind () != zlink::framework::framework_error_kind_t::capacity_exceeded) {
            return 15;
        }
        const auto conflict = mapper.completion_exception (
          zlink::framework::runtime::messaging::request_result_t::conflict, "profile request");
        const auto rejected = mapper.completion_exception (
          zlink::framework::runtime::messaging::request_result_t::rejected, "profile request");
        const auto protocol = mapper.completion_exception (
          zlink::framework::runtime::messaging::request_result_t::protocol_error,
          "profile request");
        const auto invalid_argument = mapper.completion_exception (
          zlink::framework::runtime::messaging::request_result_t::invalid_argument,
          "profile request");
        const auto invalid_state = mapper.completion_exception (
          zlink::framework::runtime::messaging::request_result_t::invalid_state, "profile request");
        const auto not_supported = mapper.completion_exception (
          zlink::framework::runtime::messaging::request_result_t::not_supported, "profile request");
        const auto terminated = mapper.completion_exception (
          zlink::framework::runtime::messaging::request_result_t::terminated, "profile request");
        const auto internal_error = mapper.completion_exception (
          zlink::framework::runtime::messaging::request_result_t::internal_error,
          "profile request");
        if (conflict.kind () != zlink::framework::framework_error_kind_t::capacity_exceeded
            || rejected.kind () != zlink::framework::framework_error_kind_t::rejected
            || protocol.kind () != zlink::framework::framework_error_kind_t::protocol_error
            || invalid_argument.kind () != zlink::framework::framework_error_kind_t::invalid_operation
            || invalid_state.kind () != zlink::framework::framework_error_kind_t::invalid_operation
            || not_supported.kind () != zlink::framework::framework_error_kind_t::internal_failure
            || terminated.kind () != zlink::framework::framework_error_kind_t::shutting_down
            || zlink::framework::detail::boundary_state (terminated)
                 != zlink::framework::detail::boundary_error_t::shutdown
            || terminated.code ()
                 != std::make_error_code (std::errc::operation_canceled)
            || internal_error.kind () != zlink::framework::framework_error_kind_t::internal_failure) {
            return 22;
        }
        const auto timeout_header =
          mapper.error_header_exception ("timeout", "", "profile request");
        const auto timeout_with_message =
          mapper.error_header_exception ("timeout", "explicit timeout", "profile request");
        const auto route_header =
          mapper.error_header_exception ("route_not_connected", "", "profile request");
        const auto unavailable_header =
          mapper.error_header_exception ("unavailable", "", "profile request");
        const auto shutdown_header =
          mapper.error_header_exception ("shutting_down", "", "profile request");
        const auto deadline_header =
          mapper.error_header_exception ("deadline_exceeded", "", "profile request");
        const auto not_found_header =
          mapper.error_header_exception ("request_target_not_found", "", "profile request");
        const auto unknown_header =
          mapper.error_header_exception ("unknown", "", "profile request");
        const auto unknown_with_message =
          mapper.error_header_exception ("unknown", "explicit", "profile request");
        if (zlink::framework::detail::boundary_state (timeout_header) != zlink::framework::detail::boundary_error_t::timed_out
            || std::string (timeout_with_message.what ()) != "explicit timeout"
            || route_header.kind () != zlink::framework::framework_error_kind_t::unavailable
            || zlink::framework::detail::boundary_state (route_header)
                 != zlink::framework::detail::boundary_error_t::disconnected
            || unavailable_header.code ()
                 != std::make_error_code (std::errc::not_connected)
            || zlink::framework::detail::boundary_state (shutdown_header)
                 != zlink::framework::detail::boundary_error_t::shutdown
            || shutdown_header.kind ()
                 != zlink::framework::framework_error_kind_t::shutting_down
            || zlink::framework::detail::boundary_state (deadline_header)
                 != zlink::framework::detail::boundary_error_t::timed_out
            || not_found_header.kind ()
                 != zlink::framework::framework_error_kind_t::not_found
            || unknown_header.kind () != zlink::framework::framework_error_kind_t::internal_failure
            || std::string (unknown_with_message.what ()) != "explicit") {
            return 23;
        }
        const auto rejected_header =
          mapper.error_header_exception ("request_rejected", "", "profile request");
        if (rejected_header.kind () != zlink::framework::framework_error_kind_t::rejected) {
            return 19;
        }
        const auto protocol_header =
          mapper.error_header_exception ("request_protocol_error", "", "profile request");
        if (protocol_header.kind ()
            != zlink::framework::framework_error_kind_t::protocol_error) {
            return 20;
        }
        const auto missing_handler_header =
          mapper.error_header_exception ("handler_not_found", "", "profile request");
        if (missing_handler_header.kind ()
            != zlink::framework::framework_error_kind_t::not_found) {
            return 21;
        }
        const auto decode_header =
          mapper.error_header_exception ("payload_decode_failed", "", "profile request");
        if (decode_header.kind ()
            != zlink::framework::framework_error_kind_t::protocol_error) {
            return 24;
        }
    }

    using zlink::framework::runtime::messaging::pending_submit_t;
    using zlink::framework::runtime::messaging::submit_queue_t;

    auto sample_task = sample_call_t (42).submit ();
    if (sample_task.result ().value () != 42) {
        return 1;
    }

    int wake_count = 0;
    bool command_submitted = false;
    auto command = pending_submit_t::create_command (
      [&] {
          command_submitted = true;
          return true;
      },
      std::nullopt, [&] { ++wake_count; });
    auto command_operation = command.operation ();
    if (!command.try_submit () || !command_submitted || !command_operation.completed ()
        || wake_count != 1) {
        return 2;
    }

    bool request_submitted = false;
    auto request = pending_submit_t::create_request (
      [&] {
          request_submitted = true;
          return true;
      },
      std::nullopt, [&] { ++wake_count; });
    auto request_operation = request.operation ();
    if (!request.try_submit () || !request_submitted || request_operation.completed ()) {
        return 3;
    }
    request.complete ();
    if (!request_operation.completed () || wake_count != 2) {
        return 4;
    }

    submit_queue_t queue (2);
    std::vector<int> submitted;
    auto first = pending_submit_t::create_command ([&] {
        submitted.push_back (1);
        return true;
    });
    auto first_operation = first.operation ();
    auto second = pending_submit_t::create_command ([&] {
        submitted.push_back (2);
        return true;
    });
    auto second_operation = second.operation ();
    queue.enqueue (std::move (first));
    queue.enqueue (std::move (second));
    bool capacity_error = false;
    try {
        queue.enqueue (pending_submit_t::create_command ([] { return true; }));
    }
    catch (const std::runtime_error &) {
        capacity_error = true;
    }
    if (!capacity_error || queue.pending_count () != 2) {
        return 5;
    }

    pending_submit_t current = pending_submit_t::create_command ([] { return false; });
    if (!queue.try_peek (current) || queue.try_dequeue_expected (second_operation, current)
        || !queue.try_dequeue_expected (first_operation, current) || !current.try_submit ()) {
        return 6;
    }
    if (!queue.try_dequeue_expected (second_operation, current) || !current.try_submit ()
        || submitted != std::vector<int>{1, 2} || queue.pending_count () != 0) {
        return 7;
    }

    auto expired = pending_submit_t::create_command (
      [] { return true; }, pending_submit_t::clock_t::now () - std::chrono::milliseconds (1));
    auto expired_operation = expired.operation ();
    if (expired.try_submit () || !expired_operation.completed ()
        || expired_operation.cancelled ()) {
        return 8;
    }

    submit_queue_t disposable (2);
    disposable.enqueue (pending_submit_t::create_command ([] { return true; }));
    if (disposable.dispose_all ().size () != 1 || !disposable.dispose_all ().empty ()) {
        return 9;
    }
    bool disposed_error = false;
    try {
        disposable.enqueue (pending_submit_t::create_command ([] { return true; }));
    }
    catch (const std::runtime_error &) {
        disposed_error = true;
    }

    if (!disposed_error) {
        return 10;
    }

    /* Async one-way admission performs the first non-blocking attempt inline.
     * Each matching capacity signal permits one retry; unrelated signals and
     * timeout completion cannot create a late admission. */
    {
        namespace msg = zlink::framework::runtime::messaging;
        msg::reset_async_submit_runtime_for_tests ();
        std::atomic_int attempts{0};
        auto admitted = zlink::framework::detail::submit_one_way_task ([&] {
            msg::note_submit_attempt ("mesh:node:01", &attempts,
                                      std::chrono::milliseconds (100));
            const auto attempt = attempts.fetch_add (1) + 1;
            if (attempt < 3) {
                return zlink::framework::result_t<void>::failure (
                  zlink::framework::framework_error_kind_t::capacity_exceeded,
                  "capacity is not available");
            }
            return zlink::framework::result_t<void>::success ();
        });
        if (attempts.load () != 1 || admitted.await_ready ()
            || msg::pending_submit_count_for_tests () != 1) {
            return 60;
        }
        msg::notify_submit_ready ("mesh:node:02", &attempts);
        if (attempts.load () != 1 || admitted.await_ready ()) {
            return 61;
        }
        msg::notify_submit_ready ("mesh:node:01", &attempts);
        const auto retry_deadline = std::chrono::steady_clock::now ()
                                    + std::chrono::milliseconds (100);
        while (attempts.load () != 2
               && std::chrono::steady_clock::now () < retry_deadline) {
            std::this_thread::yield ();
        }
        if (attempts.load () != 2 || admitted.await_ready ()) {
            return 62;
        }
        msg::notify_submit_ready ("mesh:node:01", &attempts);
        const auto &admitted_result = admitted.result ();
        if (attempts.load () != 3) {
            return 63;
        }
        if (!admitted.await_ready ()) {
            return 67;
        }
        admitted_result.value ();
        if (msg::pending_submit_count_for_tests () != 0) {
            return 69;
        }

        int timed_out_attempts = 0;
        auto timed_out = zlink::framework::detail::submit_one_way_task ([&] {
            msg::note_submit_attempt ("mesh:spot:01:02", &timed_out_attempts,
                                      std::chrono::milliseconds (10));
            ++timed_out_attempts;
            return zlink::framework::result_t<void>::failure (
              zlink::framework::framework_error_kind_t::capacity_exceeded,
              "capacity is not available");
        });
        if (timed_out.result ().error_kind ()
              != zlink::framework::framework_error_kind_t::deadline_exceeded
            || timed_out_attempts != 1) {
            return 64;
        }
        msg::notify_submit_ready ("mesh:spot:01:02", &timed_out_attempts);
        if (timed_out_attempts != 1 || msg::pending_submit_count_for_tests () != 0) {
            return 65;
        }

        int shutdown_owner = 0;
        auto shutdown = zlink::framework::detail::submit_one_way_task ([&] {
            msg::note_submit_attempt ("stream:session-1", &shutdown_owner,
                                      std::chrono::milliseconds (100));
            return zlink::framework::result_t<void>::failure (
              zlink::framework::framework_error_kind_t::capacity_exceeded,
              "capacity is not available");
        });
        msg::shutdown_submit_owner (&shutdown_owner);
        if (shutdown.result ().error_kind ()
            != zlink::framework::framework_error_kind_t::shutting_down) {
            return 66;
        }

        /* Stop/start of the same runtime object advances its owner epoch. An
         * operation whose first attempt crossed that boundary cannot reserve
         * a waiter in the new lifecycle. */
        msg::reset_async_submit_runtime_for_tests ();
        int epoch_owner = 0;
        std::mutex epoch_mutex;
        std::condition_variable epoch_changed;
        bool epoch_attempt_entered = false;
        bool epoch_attempt_release = false;
        std::promise<zlink::framework::framework_error_kind_t> epoch_terminal_source;
        auto epoch_terminal = epoch_terminal_source.get_future ();
        std::thread epoch_submitter ([&] {
            auto operation = zlink::framework::detail::submit_one_way_task ([&] {
                msg::note_submit_attempt ("mesh:node:epoch", &epoch_owner,
                                          std::chrono::milliseconds (500), 1);
                std::unique_lock lock (epoch_mutex);
                epoch_attempt_entered = true;
                epoch_changed.notify_all ();
                epoch_changed.wait (lock, [&] { return epoch_attempt_release; });
                return zlink::framework::result_t<void>::failure (
                  zlink::framework::framework_error_kind_t::capacity_exceeded,
                  "capacity is not available");
            });
            epoch_terminal_source.set_value (operation.result ().error_kind ());
        });
        {
            std::unique_lock lock (epoch_mutex);
            if (!epoch_changed.wait_for (
                  lock, std::chrono::milliseconds (250),
                  [&] { return epoch_attempt_entered; })) {
                epoch_attempt_release = true;
                lock.unlock ();
                epoch_changed.notify_all ();
                epoch_submitter.join ();
                return 82;
            }
        }
        msg::shutdown_submit_owner (&epoch_owner);
        msg::activate_submit_owner (&epoch_owner);
        {
            std::lock_guard lock (epoch_mutex);
            epoch_attempt_release = true;
        }
        epoch_changed.notify_all ();
        epoch_submitter.join ();
        if (epoch_terminal.get ()
              != zlink::framework::framework_error_kind_t::shutting_down
            || msg::pending_submit_count_for_tests () != 0) {
            return 83;
        }

        /* A pending slot remains reserved while its retry is in flight. Once
         * the bounded waiter capacity is exhausted, a second operation does
         * not retain its submit closure and completes with DeadlineExceeded. */
        msg::reset_async_submit_runtime_for_tests ();
        int reservation_owner = 0;
        std::mutex reservation_mutex;
        std::condition_variable reservation_changed;
        bool retry_entered = false;
        bool retry_release = false;
        std::atomic_int first_attempts{0};
        auto first_reserved = zlink::framework::detail::submit_one_way_task ([&] {
            msg::note_submit_attempt ("mesh:node:reserved", &reservation_owner,
                                      std::chrono::milliseconds (500), 1);
            if (first_attempts.fetch_add (1) == 0) {
                return zlink::framework::result_t<void>::failure (
                  zlink::framework::framework_error_kind_t::capacity_exceeded,
                  "capacity is not available");
            }
            std::unique_lock lock (reservation_mutex);
            retry_entered = true;
            reservation_changed.notify_all ();
            reservation_changed.wait (lock, [&] { return retry_release; });
            return zlink::framework::result_t<void>::success ();
        });
        msg::notify_submit_ready ("mesh:node:reserved", &reservation_owner);
        {
            std::unique_lock lock (reservation_mutex);
            if (!reservation_changed.wait_for (
                  lock, std::chrono::milliseconds (250), [&] { return retry_entered; })) {
                return 70;
            }
        }
        std::atomic_int second_attempts{0};
        auto second_reserved = zlink::framework::detail::submit_one_way_task ([&] {
            msg::note_submit_attempt ("mesh:node:reserved", &reservation_owner,
                                      std::chrono::milliseconds (30), 1);
            ++second_attempts;
            return zlink::framework::result_t<void>::failure (
              zlink::framework::framework_error_kind_t::capacity_exceeded,
              "capacity is not available");
        });
        if (!second_reserved.await_ready () || second_attempts.load () != 1
            || second_reserved.result ().error_kind ()
                 != zlink::framework::framework_error_kind_t::deadline_exceeded) {
            {
                std::lock_guard lock (reservation_mutex);
                retry_release = true;
            }
            reservation_changed.notify_all ();
            return 71;
        }
        std::atomic_int hard_overflow_attempts{0};
        for (int index = 0; index < 5000; ++index) {
            auto overflow = zlink::framework::detail::submit_one_way_task ([&] {
                msg::note_submit_attempt (
                  "mesh:node:reserved", &reservation_owner,
                  std::chrono::milliseconds (500), 1);
                ++hard_overflow_attempts;
                return zlink::framework::result_t<void>::failure (
                  zlink::framework::framework_error_kind_t::capacity_exceeded,
                  "capacity is not available");
            });
            if (!overflow.await_ready ()
                || overflow.result ().error_kind ()
                     != zlink::framework::framework_error_kind_t::deadline_exceeded) {
                return 72;
            }
        }
        if (hard_overflow_attempts.load () != 5000
            || msg::pending_submit_count_for_tests () != 1) {
            return 85;
        }
        {
            std::lock_guard lock (reservation_mutex);
            retry_release = true;
        }
        reservation_changed.notify_all ();
        first_reserved.result ().value ();

        /* A readiness signal received while retrying belongs to that
         * operation. It must not become an extra retry credit for a later
         * operation that happens to use the same owner and target. */
        msg::reset_async_submit_runtime_for_tests ();
        int credit_owner = 0;
        std::mutex credit_mutex;
        std::condition_variable credit_changed;
        bool credit_retry_entered = false;
        bool credit_retry_release = false;
        std::atomic_int credit_first_attempts{0};
        auto credit_first = zlink::framework::detail::submit_one_way_task ([&] {
            msg::note_submit_attempt ("mesh:node:credit", &credit_owner,
                                      std::chrono::milliseconds (500), 1);
            if (credit_first_attempts.fetch_add (1) == 0) {
                return zlink::framework::result_t<void>::failure (
                  zlink::framework::framework_error_kind_t::capacity_exceeded,
                  "capacity is not available");
            }
            std::unique_lock lock (credit_mutex);
            credit_retry_entered = true;
            credit_changed.notify_all ();
            credit_changed.wait (lock, [&] { return credit_retry_release; });
            return zlink::framework::result_t<void>::success ();
        });
        msg::notify_submit_ready ("mesh:node:credit", &credit_owner);
        {
            std::unique_lock lock (credit_mutex);
            if (!credit_changed.wait_for (
                  lock, std::chrono::milliseconds (250),
                  [&] { return credit_retry_entered; })) {
                credit_retry_release = true;
                lock.unlock ();
                credit_changed.notify_all ();
                return 73;
            }
        }
        msg::notify_submit_ready ("mesh:node:credit", &credit_owner);
        {
            std::lock_guard lock (credit_mutex);
            credit_retry_release = true;
        }
        credit_changed.notify_all ();
        credit_first.result ().value ();

        std::atomic_int credit_second_attempts{0};
        auto credit_second = zlink::framework::detail::submit_one_way_task ([&] {
            msg::note_submit_attempt ("mesh:node:credit", &credit_owner,
                                      std::chrono::milliseconds (500), 1);
            const auto attempt = credit_second_attempts.fetch_add (1) + 1;
            if (attempt < 3) {
                return zlink::framework::result_t<void>::failure (
                  zlink::framework::framework_error_kind_t::capacity_exceeded,
                  "capacity is not available");
            }
            return zlink::framework::result_t<void>::success ();
        });
        msg::notify_submit_ready ("mesh:node:credit", &credit_owner);
        const auto single_signal_deadline = std::chrono::steady_clock::now ()
                                            + std::chrono::milliseconds (100);
        while (credit_second_attempts.load () < 2
               && std::chrono::steady_clock::now () < single_signal_deadline) {
            std::this_thread::yield ();
        }
        if (credit_second_attempts.load () != 2 || credit_second.await_ready ()) {
            return 75;
        }
        msg::notify_submit_ready ("mesh:node:credit", &credit_owner);
        credit_second.result ().value ();
        if (credit_second_attempts.load () != 3) {
            return 76;
        }

        /* Copies refer to the same logical call. Exactly one public
         * terminator may start transport admission. */
        std::atomic_int duplicate_attempts{0};
        zlink::framework::send_call_t duplicate_call (
          "duplicate",
          [&] (const std::string &,
               const zlink::framework::send_call_t::metadata_map_t &) {
              ++duplicate_attempts;
              return zlink::framework::result_t<void>::success ();
          });
        auto duplicate_copy = duplicate_call;
        duplicate_call.submit ().result ().value ();
        bool duplicate_rejected = false;
        try {
            (void) duplicate_copy.submit ().result ().value ();
        }
        catch (const zlink::framework::framework_exception_t &error) {
            duplicate_rejected =
              error.kind ()
              == zlink::framework::framework_error_kind_t::invalid_operation;
        }
        if (!duplicate_rejected || duplicate_attempts.load () != 1) {
            return 78;
        }

        std::atomic_int multicast_calls{0};
        zlink::framework::publish_call_t multicast_call (
          [&] (const zlink::framework::publish_call_t::metadata_map_t &) {
              ++multicast_calls;
              return zlink::framework::result_t<void>::success ();
          });
        auto multicast_copy = multicast_call;
        multicast_call.submit ().result ().value ();
        bool duplicate_multicast_rejected = false;
        try {
            (void) multicast_copy.submit ().result ().value ();
        }
        catch (const zlink::framework::framework_exception_t &error) {
            duplicate_multicast_rejected =
              error.kind ()
              == zlink::framework::framework_error_kind_t::invalid_operation;
        }
        /* The accepted publish completed at worker dequeue, so its publisher
         * body may still be pending. The call count is settled once the worker
         * slot has been returned. */
        msg::wait_for_idle_multicast_executor_for_tests ();
        if (!duplicate_multicast_rejected || multicast_calls.load () != 1) {
            return 80;
        }
        /* Logical Multicast uses direct worker handoff. Saturation must not
         * retain an unbounded queue of publish payloads.
         *
         * A publish task completes at worker dequeue, but the worker slot is
         * returned only after the job body has run. The slot held by the
         * preceding publish can therefore still be in use here, which would
         * turn one of the occupying calls below into an immediate overflow and
         * saturate the executor one call early. Saturation starts from an
         * observed idle executor instead of an assumed one. */
        msg::wait_for_idle_multicast_executor_for_tests ();
        const auto worker_count = msg::multicast_worker_count_for_tests ();
        std::mutex multicast_gate_mutex;
        std::condition_variable multicast_gate_changed;
        bool release_multicast_workers = false;
        std::vector<zlink::framework::task_t<void>> occupied_workers;
        occupied_workers.reserve (worker_count);
        for (std::size_t index = 0; index < worker_count; ++index) {
            zlink::framework::publish_call_t occupied (
              [&] (const zlink::framework::publish_call_t::metadata_map_t &) {
                  std::unique_lock lock (multicast_gate_mutex);
                  multicast_gate_changed.wait (
                    lock, [&] { return release_multicast_workers; });
                  return zlink::framework::result_t<void>::success ();
              });
            occupied_workers.push_back (occupied.submit ());
        }
        std::atomic_int handoff_calls{0};
        zlink::framework::publish_call_t handoff (
          [&] (const zlink::framework::publish_call_t::metadata_map_t &) {
              ++handoff_calls;
              return zlink::framework::result_t<void>::success ();
          },
          std::chrono::seconds (1));
        auto handoff_task = handoff.submit ();
        std::atomic_int overflow_calls{0};
        zlink::framework::publish_call_t overflow (
          [&] (const zlink::framework::publish_call_t::metadata_map_t &) {
              ++overflow_calls;
              return zlink::framework::result_t<void>::success ();
          },
          std::chrono::milliseconds (25));
        bool overflow_timed_out = false;
        auto overflow_task = overflow.submit ();
        const bool overflow_completed_immediately = overflow_task.await_ready ();
        try {
            overflow_task.result ().value ();
        }
        catch (const zlink::framework::framework_exception_t &error) {
            overflow_timed_out =
              error.kind ()
              == zlink::framework::framework_error_kind_t::deadline_exceeded;
        }
        if (!overflow_completed_immediately || !overflow_timed_out
            || overflow_calls.load () != 0) {
            return 82;
        }
        {
            std::lock_guard lock (multicast_gate_mutex);
            release_multicast_workers = true;
        }
        multicast_gate_changed.notify_all ();
        for (auto &task : occupied_workers) {
            task.result ().value ();
        }
        handoff_task.result ().value ();
        /* The handoff task completed at dequeue, which is before its publisher
         * body ran. The call count is only settled once the worker slot has
         * been returned. */
        msg::wait_for_idle_multicast_executor_for_tests ();
        if (handoff_calls.load () != 1) {
            return 83;
        }

        bool release_deadline_workers = false;
        std::vector<zlink::framework::task_t<void>> deadline_workers;
        deadline_workers.reserve (worker_count);
        for (std::size_t index = 0; index < worker_count; ++index) {
            zlink::framework::publish_call_t occupied (
              [&] (const zlink::framework::publish_call_t::metadata_map_t &) {
                  std::unique_lock lock (multicast_gate_mutex);
                  multicast_gate_changed.wait (
                    lock, [&] { return release_deadline_workers; });
                  return zlink::framework::result_t<void>::success ();
              });
            deadline_workers.push_back (occupied.submit ());
        }
        std::atomic_int expired_handoff_calls{0};
        zlink::framework::publish_call_t expired_handoff (
          [&] (const zlink::framework::publish_call_t::metadata_map_t &) {
              ++expired_handoff_calls;
              return zlink::framework::result_t<void>::success ();
          },
          std::chrono::milliseconds (25));
        auto expired_handoff_task = expired_handoff.submit ();
        std::this_thread::sleep_for (std::chrono::milliseconds (35));
        {
            std::lock_guard lock (multicast_gate_mutex);
            release_deadline_workers = true;
        }
        multicast_gate_changed.notify_all ();
        bool handoff_timed_out = false;
        try {
            expired_handoff_task.result ().value ();
        }
        catch (const zlink::framework::framework_exception_t &error) {
            handoff_timed_out =
              error.kind ()
              == zlink::framework::framework_error_kind_t::deadline_exceeded;
        }
        for (auto &task : deadline_workers) {
            task.result ().value ();
        }
        if (!handoff_timed_out || expired_handoff_calls.load () != 0) {
            return 84;
        }

        msg::wait_for_idle_multicast_executor_for_tests ();
        zlink::framework::publish_call_t recovered (
          [] (const zlink::framework::publish_call_t::metadata_map_t &) {
              return zlink::framework::result_t<void>::success ();
          });
        recovered.submit ().result ().value ();
        msg::reset_async_submit_runtime_for_tests ();
    }

    /* flow-correlation: envelope marker + optional flow pair round-trip,
     * ambient stamping, create-if-absent gate and off-mode propagation. */
    {
        namespace msg = zlink::framework::runtime::messaging;
        namespace rt = zlink::framework::runtime;
        msg::envelope_codec_t codec;
        msg::envelope_header_t header;
        header.kind = msg::message_kind_t::command;
        header.channel_name = "flows";
        header.message_name = "flow.msg";

        const auto plain = codec.decode_header (codec.encode_header (header));
        if (!plain || plain.value ().flow_id || plain.value ().flow_origin) {
            return 40;
        }
        if (codec.encode_header (header).to_string ().find ("\"formatMarker\":242")
            == std::string::npos) {
            return 41;
        }

        const auto created = rt::flow_id_t::create ();
        if (!rt::flow_id_t::is_valid (created) || created.size () != 36 || created[14] != '7') {
            return 42;
        }
        if (rt::flow_id_t::is_valid ("01890a5d-ac96-474b-bcce-b302099a8057")   // v4
            || rt::flow_id_t::is_valid ("01890A5D-AC96-774B-BCCE-B302099A8057") // uppercase
            || rt::flow_id_t::is_valid ("short")) {
            return 43;
        }

        {
            auto scope = rt::flow_context_t::enter (created, zlink::framework::flow_origin_t::inbound,
                                                    false, zlink::framework::flow_origin_t::inbound);
            const auto stamped = codec.decode_header (codec.encode_header (header));
            if (!stamped || stamped.value ().flow_id != created
                || stamped.value ().flow_origin != zlink::framework::flow_origin_t::inbound) {
                return 44;
            }
        }
        if (rt::flow_context_t::current ()) {
            return 45;
        }

        /* create-if-absent: no inbound id + capture on → new id; inbound id
         * present → reused even when capture is off (propagation is
         * unconditional). */
        {
            auto scope =
              rt::flow_context_t::enter (std::nullopt, std::nullopt, true,
                                         zlink::framework::flow_origin_t::inbound);
            if (!rt::flow_context_t::current ()
                || !rt::flow_id_t::is_valid (rt::flow_context_t::current ()->flow_id)) {
                return 46;
            }
        }
        {
            auto scope = rt::flow_context_t::enter (created, zlink::framework::flow_origin_t::timer,
                                                    false, zlink::framework::flow_origin_t::inbound);
            if (!rt::flow_context_t::current ()
                || rt::flow_context_t::current ()->flow_id != created
                || rt::flow_context_t::current ()->origin
                     != zlink::framework::flow_origin_t::timer) {
                return 47;
            }
        }
        {
            auto scope =
              rt::flow_context_t::enter (std::nullopt, std::nullopt, false,
                                         zlink::framework::flow_origin_t::inbound);
            if (rt::flow_context_t::current ()) {
                return 48;
            }
        }

        /* Marker/pair validation is fail-closed. */
        auto missing_marker = codec.decode_header (
          zlink::message_t::from ("{\"kind\":3,\"channelName\":\"c\",\"messageName\":\"m\"}"));
        if (missing_marker
            || missing_marker.error_kind ()
                 != zlink::framework::framework_error_kind_t::protocol_error) {
            return 49;
        }
        auto lonely_flow = codec.decode_header (zlink::message_t::from (
          "{\"formatMarker\":242,\"kind\":3,\"channelName\":\"c\",\"messageName\":\"m\",\"flowId\":\""
          + created + "\"}"));
        if (lonely_flow
            || lonely_flow.error_kind ()
                 != zlink::framework::framework_error_kind_t::protocol_error) {
            return 50;
        }

        /* MFLOW-EXT-014: an async continuation re-enters the flow captured at
         * registration even when the task completes from a flow-less thread,
         * and nothing leaks into the completing thread afterwards. */
        {
            zlink::framework::detail::task_completion_source_t<int> source;
            std::string observed_in_callback;
            bool leaked_on_completer = false;
            {
                auto scope =
                  rt::flow_context_t::enter (created, zlink::framework::flow_origin_t::inbound,
                                             false, zlink::framework::flow_origin_t::inbound);
                auto task = source.task ();
                zlink::framework::detail::observe_task_completion (
                  task, [&observed_in_callback] (const zlink::framework::result_t<int> &) {
                      if (const auto &flow = rt::flow_context_t::current ()) {
                          observed_in_callback = flow->flow_id;
                      }
                  });
            }
            std::thread completer ([&source, &leaked_on_completer] {
                source.complete (zlink::framework::result_t<int>::success (1));
                leaked_on_completer = rt::flow_context_t::current ().has_value ();
            });
            completer.join ();
            if (observed_in_callback != created) {
                return 51;
            }
            if (leaked_on_completer || rt::flow_context_t::current ()) {
                return 52;
            }
        }
    }

    return 0;
}
