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

#include "../support/stream_connector_packet_message.hpp"
#include "../support/stream_connector_wire_frames.hpp"
#include "../support/stream_connector_typed_messages.hpp"
#include "../support/stream_connector_scripted_transport.hpp"
#include "../support/stream_connector_callback_sync.hpp"
#include "../support/stream_connector_failure.hpp"

using namespace stream_connector_fixture;

namespace
{
class early_reply_connection_t final : public zlink::stream_connector::detail::stream_connection_t
{
  public:
    explicit early_reply_connection_t (std::vector<std::uint8_t> reply) : _reply (std::move (reply))
    {
    }

    void async_read_some (std::size_t,
                          std::function<void (boost::system::error_code, std::vector<std::uint8_t>)>
                            completion) override
    {
        // A real socket keeps an idle read pending. Completing exhausted reads
        // inline with empty data re-enters the read pump on the caller stack
        // until it overflows, so hold the completion once the scripted reply
        // has been consumed.
        std::unique_lock lock (_mutex);
        if (_reply.empty ()) {
            _pending_read = std::move (completion);
            return;
        }
        auto reply = std::move (_reply);
        _reply.clear ();
        lock.unlock ();
        completion (boost::system::error_code{}, std::move (reply));
    }

    void async_write (std::vector<std::uint8_t> bytes,
                      std::function<void (boost::system::error_code)> completion) override
    {
        std::lock_guard lock (_mutex);
        _written.push_back (std::move (bytes));
        _write_completion = std::move (completion);
    }

    void complete_write ()
    {
        std::function<void (boost::system::error_code)> completion;
        {
            std::lock_guard lock (_mutex);
            completion = std::move (_write_completion);
        }
        if (completion) {
            completion (boost::system::error_code{});
        }
    }

    boost::system::error_code shutdown_and_close () override
    {
        std::lock_guard lock (_mutex);
        _pending_read = {};
        _write_completion = {};
        return {};
    }

    bool write_pending () const
    {
        std::lock_guard lock (_mutex);
        return !_written.empty () && static_cast<bool> (_write_completion);
    }

  private:
    mutable std::mutex _mutex;
    std::vector<std::vector<std::uint8_t>> _written;
    std::function<void (boost::system::error_code)> _write_completion;
    std::vector<std::uint8_t> _reply;
    std::function<void (boost::system::error_code, std::vector<std::uint8_t>)> _pending_read;
};

static_assert (!has_core_async_terminator<zlink::stream_connector::request_call_t>);

static_assert (
  std::is_same_v<decltype (std::declval<zlink::stream_e2e_client::coroutine_connector_t &> ()
                             .expect_none<auto_payload_t> ()
                             .within (std::chrono::milliseconds (1))
                             .async ()),
                 zlink::stream_e2e_client::task_t<void>>);

static_assert (
  std::is_same_v<decltype (std::declval<zlink::stream_e2e_client::coroutine_connector_t &> ()
                             .request (std::declval<auto_payload_t> ())
                             .async<auto_payload_t> ()),
                 zlink::stream_e2e_client::task_t<auto_payload_t>>);
} // namespace

namespace stream_connector_tests
{
int verify_request_terminal_races ()
{
    {
        // Check pending state after asynchronous transport work completes.
        // Callbacks follow dispatch mode without requiring a separate delivery runner.
        const auto no_pending_requests = [] (const auto &state) {
            std::lock_guard<std::mutex> lock (state->transport_mutex);
            return state->pending_requests.empty ();
        };
        const auto no_pending_waits = [] (const auto &state) {
            std::lock_guard<std::mutex> lock (state->transport_mutex);
            return state->pending_waits.empty ();
        };
        const auto release_state = [] (const auto &state) {
            if (!state)
                return;
            std::shared_ptr<zlink::stream_connector::detail::stream_connection_t> connection;
            {
                std::lock_guard<std::mutex> lock (state->transport_mutex);
                connection = std::move (state->connection);
                state->pending_requests.clear ();
                state->pending_waits.clear ();
                state->write_queue.clear ();
                state->dispatch_queue.clear ();
                state->delivery_queue.clear ();
            }
            if (connection)
                connection->shutdown_and_close ();
        };
        zlink::stream_connector::connector_options_t async_send_options;
        async_send_options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;
        async_send_options.reconnect.enabled = false;
        auto async_send_state =
          std::make_shared<zlink::stream_connector::detail::connector_state_t> (async_send_options);
        async_send_state->state = zlink::stream_connector::connection_state_t::connected;
        auto async_send_connection = std::make_shared<async_write_connection_t> ();
        async_send_state->connection = async_send_connection;
        std::atomic<bool> async_send_seen{false};
        zlink::stream_connector::detail::submit_send_async (
          async_send_state,
          zlink::stream_connector::packet_t{.name = "async.send", .payload = as_bytes ("payload")},
          [&] (zlink::stream_connector::result_t<void> result) {
              async_send_seen = static_cast<bool> (result);
          });
        if (!eventually ([&] { return async_send_seen.load (); })
            || async_send_connection->written ().size () != 1) {
            return fail (155);
        }
        const auto async_send_written = async_send_connection->written ();
        std::string async_send_wire (async_send_written[0].begin (), async_send_written[0].end ());
        const auto async_send_frame = try_read_server_frame (async_send_wire);
        if (async_send_connection->written ().size () != 1 || !async_send_frame
            || async_send_frame->header.name != "async.send"
            || async_send_frame->payload != "payload") {
            return fail (155);
        }

        auto async_write_failure_state =
          std::make_shared<zlink::stream_connector::detail::connector_state_t> (async_send_options);
        async_write_failure_state->state = zlink::stream_connector::connection_state_t::connected;
        auto async_write_failure_connection = std::make_shared<async_write_connection_t> (true);
        async_write_failure_state->connection = async_write_failure_connection;
        std::atomic<bool> async_write_failure_seen{false};
        zlink::stream_connector::detail::submit_send_async (
          async_write_failure_state,
          zlink::stream_connector::packet_t{.name = "async.write.fail",
                                            .payload = as_bytes ("payload")},
          [&] (zlink::stream_connector::result_t<void> result) {
              async_write_failure_seen =
                !result
                && result.error_code () == zlink::stream_connector::error_code_t::send_failed;
          });
        const auto async_write_failure_settled = eventually ([&] {
            std::lock_guard<std::mutex> lock (async_write_failure_state->lifecycle_mutex);
            if (!async_write_failure_seen.load ()
                || async_write_failure_state->state
                     != zlink::stream_connector::connection_state_t::disconnected) {
                return false;
            }
            // §6.2: the write result can precede publication of the ending reason.
            return async_write_failure_state->last_close_reason.has_value ();
        });
        {
            std::lock_guard<std::mutex> async_write_failure_lock (
              async_write_failure_state->lifecycle_mutex);
            if (!async_write_failure_settled
                || async_write_failure_connection->written ().size () != 1
                || async_write_failure_state->last_close_reason
                     != zlink::stream_connector::close_reason_t::transport_error) {
                return fail (158);
            }
        }

        /* §9 and §12: the request whose write completed remains pending
     * until a later write failure ends the connection. */
        auto write_ending_state =
          std::make_shared<zlink::stream_connector::detail::connector_state_t> (async_send_options);
        write_ending_state->state = zlink::stream_connector::connection_state_t::connected;
        auto write_ending_connection = std::make_shared<held_async_write_connection_t> ();
        write_ending_state->connection = write_ending_connection;
        std::atomic<int> pending_code{-1};
        std::atomic<int> failing_code{-1};
        zlink::stream_connector::detail::submit_request_async (
          write_ending_state,
          zlink::stream_connector::packet_t{.name = "pending.before.failed.write",
                                            .payload = as_bytes ("pending")},
          std::chrono::seconds (5),
          [&] (zlink::stream_connector::result_t<zlink::stream_connector::detail::request_reply_t>
                 result) {
              if (!result)
                  pending_code = static_cast<int> (result.error_code ().value ());
          });
        if (!write_ending_connection->wait_for_write_count (1))
            return fail (398);
        write_ending_connection->complete_write ();
        zlink::stream_connector::detail::submit_send_async (
          write_ending_state,
          zlink::stream_connector::packet_t{.name = "failed.write", .payload = as_bytes ("failed")},
          [&] (zlink::stream_connector::result_t<void> result) {
              if (!result)
                  failing_code = static_cast<int> (result.error_code ().value ());
          });
        if (!write_ending_connection->wait_for_write_count (2))
            return fail (399);
        write_ending_connection->fail_write ();
        if (!eventually ([&] { return pending_code.load () >= 0 && failing_code.load () >= 0; })
            || pending_code
                 != static_cast<int> (zlink::stream_connector::error_code_t::disconnected)
            || failing_code != static_cast<int> (zlink::stream_connector::error_code_t::send_failed)
            || write_ending_state->last_close_reason
                 != zlink::stream_connector::close_reason_t::transport_error) {
            return fail (400);
        }

        auto async_closed_state =
          std::make_shared<zlink::stream_connector::detail::connector_state_t> (async_send_options);
        async_closed_state->state = zlink::stream_connector::connection_state_t::connected;
        async_closed_state->connection = std::make_shared<async_write_connection_t> ();
        async_closed_state->close_requested.store (true);
        std::atomic<bool> async_closed_seen{false};
        zlink::stream_connector::detail::submit_send_async (
          async_closed_state,
          zlink::stream_connector::packet_t{.name = "async.closed",
                                            .payload = as_bytes ("payload")},
          [&] (zlink::stream_connector::result_t<void> result) {
              async_closed_seen =
                !result
                && result.error_code () == zlink::stream_connector::error_code_t::disconnected;
          });
        if (!eventually ([&] { return async_closed_seen.load (); })) {
            return fail (159);
        }

        auto async_disconnected_state =
          std::make_shared<zlink::stream_connector::detail::connector_state_t> (async_send_options);
        std::atomic<bool> async_disconnected_seen{false};
        zlink::stream_connector::detail::submit_send_async (
          async_disconnected_state,
          zlink::stream_connector::packet_t{.name = "async.disconnected",
                                            .payload = as_bytes ("payload")},
          [&] (zlink::stream_connector::result_t<void> result) {
              async_disconnected_seen =
                !result
                && result.error_code () == zlink::stream_connector::error_code_t::disconnected;
          });
        if (!eventually ([&] { return async_disconnected_seen.load (); })) {
            return fail (160);
        }

        async_send_options.max_send_payload_size = 1;
        auto async_validation_state =
          std::make_shared<zlink::stream_connector::detail::connector_state_t> (async_send_options);
        async_validation_state->state = zlink::stream_connector::connection_state_t::connected;
        async_validation_state->connection = std::make_shared<async_write_connection_t> ();
        std::atomic<bool> async_validation_seen{false};
        zlink::stream_connector::detail::submit_send_async (
          async_validation_state,
          zlink::stream_connector::packet_t{.name = "async.validation",
                                            .payload = as_bytes ("too-large")},
          [&] (zlink::stream_connector::result_t<void> result) {
              /* stream-connector 32 §4.7/§9: over the send payload bound is a
           * pre-write validation failure, not the receive-side
           * frame_too_large. */
              async_validation_seen =
                !result
                && result.error_code () == zlink::stream_connector::error_code_t::validation_failed;
          });
        if (!eventually ([&] { return async_validation_seen.load (); })) {
            return fail (161);
        }

        std::atomic<bool> async_request_unbound_seen{false};
        zlink::stream_connector::detail::submit_request_async (
          {}, zlink::stream_connector::packet_t{.name = "unbound.request"},
          std::chrono::milliseconds (1),
          [&] (zlink::stream_connector::result_t<zlink::stream_connector::detail::request_reply_t>
                 result) {
              async_request_unbound_seen =
                !result
                && result.error_code ()
                     == zlink::stream_connector::error_code_t::configuration_error;
          });
        if (!eventually ([&] { return async_request_unbound_seen.load (); })) {
            return fail (162);
        }

        std::atomic<bool> async_request_closed_seen{false};
        zlink::stream_connector::detail::submit_request_async (
          async_closed_state, zlink::stream_connector::packet_t{.name = "closed.request"},
          std::chrono::milliseconds (1),
          [&] (zlink::stream_connector::result_t<zlink::stream_connector::detail::request_reply_t>
                 result) {
              async_request_closed_seen =
                !result
                && result.error_code () == zlink::stream_connector::error_code_t::disconnected;
          });
        if (!eventually ([&] { return async_request_closed_seen.load (); })) {
            return fail (163);
        }

        std::atomic<bool> async_request_disconnected_seen{false};
        zlink::stream_connector::detail::submit_request_async (
          async_disconnected_state,
          zlink::stream_connector::packet_t{.name = "disconnected.request"},
          std::chrono::milliseconds (1),
          [&] (zlink::stream_connector::result_t<zlink::stream_connector::detail::request_reply_t>
                 result) {
              async_request_disconnected_seen =
                !result
                && result.error_code () == zlink::stream_connector::error_code_t::disconnected;
          });
        if (!eventually ([&] { return async_request_disconnected_seen.load (); })) {
            return fail (164);
        }

        std::atomic<bool> async_request_validation_seen{false};
        zlink::stream_connector::detail::submit_request_async (
          async_validation_state,
          /* Validation fails at once; the request timeout must not race it. */
          zlink::stream_connector::packet_t{.name = "validation.request",
                                            .payload = as_bytes ("too-large")},
          std::chrono::seconds (5),
          [&] (zlink::stream_connector::result_t<zlink::stream_connector::detail::request_reply_t>
                 result) {
              async_request_validation_seen =
                !result
                && result.error_code () == zlink::stream_connector::error_code_t::validation_failed;
          });
        if (!eventually ([&] { return async_request_validation_seen.load (); })) {
            return fail (165);
        }

        auto async_request_write_failure_state =
          std::make_shared<zlink::stream_connector::detail::connector_state_t> (
            zlink::stream_connector::connector_options_t{
              .dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate});
        async_request_write_failure_state->state =
          zlink::stream_connector::connection_state_t::connected;
        async_request_write_failure_state->connection =
          std::make_shared<async_write_connection_t> (true);
        std::atomic<bool> async_request_write_failure_seen{false};
        zlink::stream_connector::detail::submit_request_async (
          async_request_write_failure_state,
          /* The write fails at once; the request timeout must not race it. */
          zlink::stream_connector::packet_t{.name = "write.failure.request",
                                            .payload = as_bytes ("payload")},
          std::chrono::seconds (5),
          [&] (zlink::stream_connector::result_t<zlink::stream_connector::detail::request_reply_t>
                 result) {
              async_request_write_failure_seen =
                !result
                && result.error_code () == zlink::stream_connector::error_code_t::send_failed;
          });
        if (!eventually ([&] {
                if (!async_request_write_failure_seen.load ()
                    || !no_pending_requests (async_request_write_failure_state)) {
                    return false;
                }
                // §6.2: the write result can precede publication of the ending reason.
                std::lock_guard<std::mutex> lock (
                  async_request_write_failure_state->lifecycle_mutex);
                return async_request_write_failure_state->last_close_reason.has_value ();
            })
            || async_request_write_failure_state->last_close_reason
                 != zlink::stream_connector::close_reason_t::transport_error) {
            return fail (166);
        }

        auto early_reply_frame =
          make_server_frame (zlink::stream_connector::message_kind_t::response, 1,
                             "early.reply.request", "early-reply");
        const auto early_reply_text = early_reply_frame.to_string ();
        auto early_reply_connection = std::make_shared<early_reply_connection_t> (
          std::vector<std::uint8_t> (early_reply_text.begin (), early_reply_text.end ()));
        auto early_reply_state =
          std::make_shared<zlink::stream_connector::detail::connector_state_t> (
            zlink::stream_connector::connector_options_t{
              .dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate});
        early_reply_state->state = zlink::stream_connector::connection_state_t::connected;
        early_reply_state->connection = early_reply_connection;
        std::atomic<int> early_reply_callback_count{0};
        std::atomic<bool> early_reply_seen{false};
        zlink::stream_connector::detail::submit_request_async (
          early_reply_state,
          zlink::stream_connector::packet_t{.name = "early.reply.request",
                                            .payload = as_bytes ("payload")},
          std::chrono::milliseconds (25),
          [&] (zlink::stream_connector::result_t<zlink::stream_connector::detail::request_reply_t>
                 result) {
              ++early_reply_callback_count;
              early_reply_seen =
                result && as_string (result.value ().packet.payload) == "early-reply";
          });
        if (!eventually ([&] {
                return early_reply_connection->write_pending () && early_reply_seen.load ()
                       && early_reply_callback_count.load () == 1
                       && no_pending_requests (early_reply_state);
            })) {
            return fail (171);
        }
        early_reply_connection->complete_write ();
        std::this_thread::sleep_for (std::chrono::milliseconds (500));
        if (!early_reply_seen.load () || early_reply_callback_count.load () != 1
            || !no_pending_requests (early_reply_state)) {
            return fail (173);
        }

        /* §5.2: pending request 매칭은 request_seq가 정본이다. 응답의 packet name이
     * request와 달라도 그 응답으로 완료한다 — 이름은 대조 조건이 아니다. */
        auto mismatched_reply_frame = make_server_frame (
          zlink::stream_connector::message_kind_t::response, 1, "unexpected.reply", "payload");
        const auto mismatched_reply_text = mismatched_reply_frame.to_string ();
        auto mismatched_reply_connection = std::make_shared<early_reply_connection_t> (
          std::vector<std::uint8_t> (mismatched_reply_text.begin (), mismatched_reply_text.end ()));
        auto mismatched_reply_state =
          std::make_shared<zlink::stream_connector::detail::connector_state_t> (
            zlink::stream_connector::connector_options_t{
              .dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate});
        mismatched_reply_state->state = zlink::stream_connector::connection_state_t::connected;
        mismatched_reply_state->connection = mismatched_reply_connection;
        std::atomic<bool> mismatched_reply_completed{false};
        zlink::stream_connector::detail::submit_request_async (
          mismatched_reply_state,
          zlink::stream_connector::packet_t{.name = "expected.reply",
                                            .payload = as_bytes ("payload")},
          std::chrono::milliseconds (25),
          [&] (zlink::stream_connector::result_t<zlink::stream_connector::detail::request_reply_t>
                 result) {
              mismatched_reply_completed =
                result && as_string (result.value ().packet.payload) == "payload";
          });
        if (!eventually ([&] {
                return mismatched_reply_completed.load ()
                       && no_pending_requests (mismatched_reply_state);
            })) {
            return fail (179);
        }
        (void) zlink::stream_connector::detail::connection_ended (
          mismatched_reply_state,
          {zlink::stream_connector::error_code_t::disconnected, "reply fixture ended"},
          mismatched_reply_connection);

        /* A terminal for an already completed request never becomes a push.
     * An unmatched Error reports the stream error independently. */
        auto late_response =
          make_server_frame (zlink::stream_connector::message_kind_t::response, 998, "", "late");
        auto unrelated_error =
          make_server_frame (zlink::stream_connector::message_kind_t::error, 999, "",
                             "{\"code\":\"InvalidOperation\",\"message\":\"stream error\"}");
        const auto terminal_text = late_response.to_string () + unrelated_error.to_string ();
        auto terminal_connection = std::make_shared<early_reply_connection_t> (
          std::vector<std::uint8_t> (terminal_text.begin (), terminal_text.end ()));
        auto terminal_state = std::make_shared<zlink::stream_connector::detail::connector_state_t> (
          zlink::stream_connector::connector_options_t{});
        terminal_state->state = zlink::stream_connector::connection_state_t::connected;
        terminal_state->connection = terminal_connection;
        int stream_error_count = 0;
        std::optional<zlink::stream_connector::error_code_t> stream_error_code;
        terminal_state->error_handlers.push_back (
          {1, [&] (const zlink::stream_connector::error_t &error) {
               ++stream_error_count;
               stream_error_code = error.code;
           }});
        zlink::stream_connector::detail::start_read_loop (terminal_state);
        {
            std::lock_guard<std::mutex> lock (terminal_state->transport_mutex);
            if (!terminal_state->dispatch_queue.empty ()) {
                return fail (304);
            }
        }
        if (!zlink::stream_connector::detail::dispatch_pending (terminal_state)
            || stream_error_count != 1
            || stream_error_code != zlink::stream_connector::error_code_t::remote_error) {
            return fail (305);
        }
        terminal_connection->shutdown_and_close ();

        /* A matched wait is an observation, so Manual callback delivery does
     * not gate its completion. */
        auto waited_frame = make_server_frame (zlink::stream_connector::message_kind_t::send, 0,
                                               "manual.observed", "observed");
        const auto waited_text = waited_frame.to_string ();
        auto waited_connection = std::make_shared<early_reply_connection_t> (
          std::vector<std::uint8_t> (waited_text.begin (), waited_text.end ()));
        auto waited_state = std::make_shared<zlink::stream_connector::detail::connector_state_t> (
          zlink::stream_connector::connector_options_t{});
        waited_state->state = zlink::stream_connector::connection_state_t::connected;
        waited_state->connection = waited_connection;
        std::atomic_bool wait_completed_without_pump{false};
        callback_latch_t waited_done;
        waited_state->pending_waits.emplace (
          1, zlink::stream_connector::detail::pending_wait_t{
               1,
               "manual.observed",
               {},
               [&] (zlink::stream_connector::result_t<zlink::stream_connector::packet_t> result) {
                   wait_completed_without_pump =
                     result && as_string (result.value ().payload) == "observed";
                   waited_done.signal ();
               }});
        zlink::stream_connector::detail::start_read_loop (waited_state);
        /* SC §7: observe unregistered wait results without a Manual pump. */
        if (!waited_done.wait_for (std::chrono::seconds (5)) || !wait_completed_without_pump
            || !waited_state->delivery_queue.empty ()) {
            return fail (306);
        }
        waited_connection->shutdown_and_close ();

        auto invalid_error_frame =
          make_server_frame (zlink::stream_connector::message_kind_t::error, 1,
                             "invalid.error.request", "{\"error\":\"missing code and message\"}");
        const auto invalid_error_text = invalid_error_frame.to_string ();
        auto invalid_error_connection = std::make_shared<early_reply_connection_t> (
          std::vector<std::uint8_t> (invalid_error_text.begin (), invalid_error_text.end ()));
        auto invalid_error_state =
          std::make_shared<zlink::stream_connector::detail::connector_state_t> (
            zlink::stream_connector::connector_options_t{
              .dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate});
        invalid_error_state->state = zlink::stream_connector::connection_state_t::connected;
        invalid_error_state->connection = invalid_error_connection;
        std::atomic<bool> invalid_error_rejected{false};
        zlink::stream_connector::detail::submit_request_async (
          invalid_error_state,
          zlink::stream_connector::packet_t{.name = "invalid.error.request",
                                            .payload = as_bytes ("payload")},
          std::chrono::milliseconds (25),
          [&] (zlink::stream_connector::result_t<zlink::stream_connector::detail::request_reply_t>
                 result) {
              invalid_error_rejected =
                !result
                && result.error_code ()
                     == zlink::stream_connector::error_code_t::frame_decode_failed;
          });
        if (!eventually ([&] {
                return invalid_error_rejected.load () && no_pending_requests (invalid_error_state);
            })) {
            return fail (180);
        }

        auto interleaved_push_frame = make_server_frame (
          zlink::stream_connector::message_kind_t::send, 0, "interleaved.push", "push-payload");
        auto interleaved_response_frame =
          make_server_frame (zlink::stream_connector::message_kind_t::response, 1,
                             "interleaved.request", "reply-payload");
        const auto interleaved_text =
          interleaved_push_frame.to_string () + interleaved_response_frame.to_string ();
        auto interleaved_connection = std::make_shared<early_reply_connection_t> (
          std::vector<std::uint8_t> (interleaved_text.begin (), interleaved_text.end ()));
        auto interleaved_state =
          std::make_shared<zlink::stream_connector::detail::connector_state_t> (
            zlink::stream_connector::connector_options_t{
              .dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate});
        interleaved_state->state = zlink::stream_connector::connection_state_t::connected;
        interleaved_state->connection = interleaved_connection;
        std::atomic<int> interleaved_wait_callback_count{0};
        std::atomic<bool> interleaved_push_seen{false};
        interleaved_state->pending_waits.emplace (
          1, zlink::stream_connector::detail::pending_wait_t{
               1,
               "interleaved.push",
               {},
               [&] (zlink::stream_connector::result_t<zlink::stream_connector::packet_t> result) {
                   ++interleaved_wait_callback_count;
                   interleaved_push_seen =
                     result && as_string (result.value ().payload) == "push-payload";
               }});
        std::atomic<bool> interleaved_reply_seen{false};
        zlink::stream_connector::detail::submit_request_async (
          interleaved_state,
          zlink::stream_connector::packet_t{.name = "interleaved.request",
                                            .payload = as_bytes ("payload")},
          std::chrono::milliseconds (25),
          [&] (zlink::stream_connector::result_t<zlink::stream_connector::detail::request_reply_t>
                 result) {
              interleaved_reply_seen =
                result && as_string (result.value ().packet.payload) == "reply-payload";
          });
        if (!eventually ([&] {
                return interleaved_connection->write_pending () && interleaved_push_seen.load ()
                       && interleaved_wait_callback_count.load () == 1
                       && interleaved_reply_seen.load () && no_pending_requests (interleaved_state)
                       && no_pending_waits (interleaved_state);
            })) {
            return fail (174);
        }
        interleaved_connection->complete_write ();
        if (!interleaved_push_seen.load () || interleaved_wait_callback_count.load () != 1
            || !interleaved_reply_seen.load () || !no_pending_requests (interleaved_state)
            || !no_pending_waits (interleaved_state)) {
            return fail (175);
        }

        async_send_state->dispatch_queue.push_back (
          {zlink::stream_connector::packet_t{.name = "queued.wait", .payload = as_bytes ("queued")},
           std::nullopt});
        const auto wait_matched = zlink::stream_connector::detail::wait_for_packet (
          async_send_state, "queued.wait",
          [] (const zlink::stream_connector::packet_t &packet) {
              return as_string (packet.payload) == "queued";
          },
          std::chrono::milliseconds (1));
        auto receive_closed_state =
          std::make_shared<zlink::stream_connector::detail::connector_state_t> (
            zlink::stream_connector::connector_options_t{});
        receive_closed_state->close_requested.store (true);
        const auto wait_closed = zlink::stream_connector::detail::wait_for_packet (
          receive_closed_state, "missing", {}, std::chrono::milliseconds (1));
        if (!wait_matched || wait_closed
            || wait_closed.error_code () != zlink::stream_connector::error_code_t::disconnected) {
            return fail (170);
        }
        release_state (async_send_state);
        release_state (async_write_failure_state);
        release_state (async_closed_state);
        release_state (async_disconnected_state);
        release_state (async_validation_state);
        release_state (async_request_write_failure_state);
        release_state (early_reply_state);
        release_state (mismatched_reply_state);
        release_state (invalid_error_state);
        release_state (interleaved_state);
        release_state (receive_closed_state);
    }

    return 0;
}

int verify_request_error_and_close (
  zlink::context_t &context,
  const zlink::stream_connector::connector_options_t &options,
  zlink::stream_connector::connector_t &connector,
  zlink::stream_connector::detail::connector_runtime_t &runtime,
  zlink::stream_connector::result_t<stream_connector_fixture::login_reply_t> &request,
  zlink::stream_connector::connector_t &immediate)
{
    zlink::stream_socket_t timeout_server (context);
    timeout_server.options ().recv_mode (zlink::stream_recv_mode_t::raw);
    timeout_server.options ().notify (false);
    timeout_server.bind ("tcp://127.0.0.1:0");
    const auto timeout_endpoint = timeout_server.options ().last_endpoint ();
    std::atomic_bool timed_request_seen{false};
    joining_thread_t timeout_server_thread ([&timeout_server, &timed_request_seen] {
        zlink::received_t inbound;
        if (timeout_server.recv (inbound) != 0) {
            return;
        }
        timed_request_seen = true;
        inbound.close ();
    });
    // §5.2: a request that times out before its write starts is never sent, so the
    // timeout must leave room for the write to reach the server this check observes.
    zlink::stream_connector::connector_options_t timeout_options;
    timeout_options.endpoint = timeout_endpoint;
    timeout_options.request_timeout = std::chrono::milliseconds (200);
    auto timeout_connector = zlink::stream_connector::connector_factory_t::create (timeout_options);
    if (!timeout_connector.connect ()) {
        return fail (35);
    }
    auto timeout_reply = timeout_connector.request (login_request_t{})
                           .packet_name ("timeout.request")
                           .timeout (std::chrono::milliseconds (200))
                           .submit<login_reply_t> ();
    timeout_server_thread.join ();
    if (timeout_reply
        || timeout_reply.error_code () != zlink::stream_connector::error_code_t::request_timeout
        || !timed_request_seen) {
        return fail (36);
    }
    timeout_connector.close ();

    zlink::stream_socket_t error_reply_server (context);
    error_reply_server.options ().recv_mode (zlink::stream_recv_mode_t::raw);
    error_reply_server.options ().notify (false);
    error_reply_server.bind ("tcp://127.0.0.1:0");
    const auto error_reply_endpoint = error_reply_server.options ().last_endpoint ();
    std::atomic_bool manual_hook_metadata_seen{false};
    joining_thread_t error_reply_server_thread ([&] {
        zlink::received_t inbound;
        if (error_reply_server.recv (inbound) != 0) {
            return;
        }
        std::string buffer =
          inbound.parts ().empty () ? std::string{} : inbound.parts ()[0].to_string ();
        if (auto frame = try_read_server_frame (buffer)) {
            manual_hook_metadata_seen.store (
              frame->header.metadata.values.contains ("manual-hook"));
            auto reply = make_server_frame (zlink::stream_connector::message_kind_t::error,
                                            frame->header.request_seq.value (), frame->header.name,
                                            "{\"code\":\"InvalidOperation\","
                                            "\"message\":\"server closed request\"}");
            inbound.send ().message (reply).submit ();
        }
        inbound.close ();
    });
    zlink::stream_connector::connector_options_t error_reply_options;
    error_reply_options.endpoint = error_reply_endpoint;
    error_reply_options.request_timeout = std::chrono::milliseconds (100);
    error_reply_options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::manual;
    auto error_reply_connector =
      zlink::stream_connector::connector_factory_t::create (error_reply_options);
    if (!error_reply_connector.connect ()) {
        return fail (150);
    }
    int early_reply_hook_calls = 0;
    int late_reply_hook_calls = 0;
    const auto manual_request_thread = std::this_thread::get_id ();
    bool manual_sending_hook_seen = false;
    auto manual_sending_hook = error_reply_connector.on_request_sending (
      [&] (zlink::stream_connector::request_sending_context_t &ctx) {
          manual_sending_hook_seen = std::this_thread::get_id () == manual_request_thread
                                     && ctx.request_packet_name == "error.reply.request"
                                     && !ctx.actor_id;
          ctx.set_metadata ("manual-hook", "present");
      });
    auto early_reply_hook = error_reply_connector.on_reply_received (
      [&] (const zlink::stream_connector::reply_received_context_t &) {
          ++early_reply_hook_calls;
      });
    auto error_reply = error_reply_connector.request (login_request_t{})
                         .packet_name ("error.reply.request")
                         .timeout (std::chrono::milliseconds (100))
                         .submit<login_reply_t> ();
    error_reply_server_thread.join ();
    if (!manual_sending_hook_seen || !manual_hook_metadata_seen.load ()
        || early_reply_hook_calls != 0) {
        return fail (286);
    }
    auto late_reply_hook = error_reply_connector.on_reply_received (
      [&] (const zlink::stream_connector::reply_received_context_t &) { ++late_reply_hook_calls; });
    while (error_reply_connector.pending_dispatch_count () != 0) {
        (void) error_reply_connector.dispatch ();
    }
    if (error_reply
        || error_reply.error_code () != zlink::stream_connector::error_code_t::remote_error
        || !error_reply.error ()
        || error_reply.error ()->message.find ("server closed request") == std::string::npos
        || early_reply_hook_calls != 1 || late_reply_hook_calls != 0) {
        return fail (151);
    }
    error_reply_connector.close ();

    /* An immediate request failure is already a queued completion in Manual
   * mode. Closing before dispatch must preserve its reply hook and callback. */
    zlink::stream_connector::connector_options_t queued_failure_options;
    queued_failure_options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::manual;
    auto queued_failure_connector =
      zlink::stream_connector::connector_factory_t::create (queued_failure_options);
    int queued_failure_reply_hooks = 0;
    int queued_failure_callbacks = 0;
    auto queued_failure_subscription = queued_failure_connector.on_reply_received (
      [&] (const zlink::stream_connector::reply_received_context_t &ctx) {
          if (ctx.request_packet_name == "queued.failure.before.close" && !ctx.succeeded
              && ctx.error
              && ctx.error->code == zlink::stream_connector::error_code_t::disconnected) {
              ++queued_failure_reply_hooks;
          }
      });
    queued_failure_connector.request (login_request_t{})
      .packet_name ("queued.failure.before.close")
      .submit<login_reply_t> ([&] (zlink::stream_connector::result_t<login_reply_t> result) {
          if (!result
              && result.error_code () == zlink::stream_connector::error_code_t::disconnected) {
              ++queued_failure_callbacks;
          }
      });
    if (queued_failure_connector.pending_dispatch_count () != 2 || queued_failure_reply_hooks != 0
        || queued_failure_callbacks != 0 || !queued_failure_connector.close ()
        || !queued_failure_connector.dispatch () || queued_failure_reply_hooks != 1
        || queued_failure_callbacks != 0 || queued_failure_connector.pending_dispatch_count () != 1
        || !queued_failure_connector.dispatch () || queued_failure_callbacks != 1
        || queued_failure_connector.pending_dispatch_count () != 0) {
        return fail (302);
    }

    zlink::stream_socket_t callback_response_server (context);
    callback_response_server.options ().recv_mode (zlink::stream_recv_mode_t::raw);
    callback_response_server.options ().notify (false);
    callback_response_server.bind ("tcp://127.0.0.1:0");
    const auto callback_response_endpoint = callback_response_server.options ().last_endpoint ();
    bool request_hook_metadata_seen = false;
    joining_thread_t callback_response_thread ([&] {
        zlink::received_t inbound;
        if (callback_response_server.recv (inbound) != 0) {
            return;
        }
        std::string buffer =
          inbound.parts ().empty () ? std::string{} : inbound.parts ()[0].to_string ();
        if (auto frame = try_read_server_frame (buffer)) {
            request_hook_metadata_seen = frame->header.metadata.values.contains ("hook-first")
                                         && frame->header.metadata.values.contains ("hook-second");
            zlink::stream_connector::detail::stream_header_t reply_header;
            reply_header.kind = zlink::stream_connector::message_kind_t::response;
            reply_header.codec = zlink::stream_connector::codec_t::raw;
            reply_header.request_seq = frame->header.request_seq;
            reply_header.metadata.with ("reply-tag", "present");
            const auto encoded_header =
              zlink::stream_connector::detail::header_codec_t{}.encode (reply_header);
            const std::vector<std::uint8_t> reply_payload{'o', 'k'};
            const auto encoded_reply = zlink::stream_connector::detail::frame_codec_t::encode (
              encoded_header.value (), reply_payload,
              zlink::stream_connector::connector_options_t{});
            auto reply = zlink::message_t::from (
              std::string (encoded_reply.value ().begin (), encoded_reply.value ().end ()));
            inbound.send ().message (reply).submit ();
        }
        inbound.close ();
    });
    zlink::stream_connector::connector_options_t callback_response_options;
    callback_response_options.endpoint = callback_response_endpoint;
    callback_response_options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;
    auto callback_response_connector =
      zlink::stream_connector::connector_factory_t::create (callback_response_options);
    if (!callback_response_connector.connect ()) {
        return fail (60);
    }
    // Record hook registration order and reply/result observation order.
    auto request_hook_order = std::make_shared<std::vector<std::string>> ();
    auto sending_first = callback_response_connector.on_request_sending (
      [request_hook_order] (zlink::stream_connector::request_sending_context_t &ctx) {
          request_hook_order->push_back ("sending-first");
          if (ctx.request_packet_name == "callback.response.request" && !ctx.actor_id) {
              ctx.set_metadata ("hook-first", "one");
          }
      });
    auto sending_second = callback_response_connector.on_request_sending (
      [request_hook_order] (zlink::stream_connector::request_sending_context_t &ctx) {
          request_hook_order->push_back ("sending-second");
          ctx.set_metadata ("hook-second", "two");
      });
    auto reply_first = callback_response_connector.on_reply_received (
      [request_hook_order] (const zlink::stream_connector::reply_received_context_t &ctx) {
          request_hook_order->push_back ("reply-first");
          if (ctx.request_packet_name != "callback.response.request" || ctx.actor_id
              || !ctx.succeeded || !ctx.reply || ctx.error || ctx.elapsed.count () < 0
              || !ctx.reply->name.empty ()
              || ctx.reply->codec != zlink::stream_connector::codec_t::raw
              || as_string (ctx.reply->payload) != "ok"
              || ctx.reply->metadata.values.find ("reply-tag") == ctx.reply->metadata.values.end ()
              || ctx.reply->metadata.values.at ("reply-tag") != "present") {
              request_hook_order->push_back ("invalid-reply-context");
          }
      });
    auto reply_hook_error = std::make_shared<callback_outcome_t> ();
    auto reply_hook_error_subscription = callback_response_connector.on_error (
      [reply_hook_error] (const zlink::stream_connector::error_t &error) {
          reply_hook_error->seen =
            error.code == zlink::stream_connector::error_code_t::user_callback_failed;
          reply_hook_error->latch.signal ();
      });
    auto reply_second = callback_response_connector.on_reply_received (
      [request_hook_order] (const zlink::stream_connector::reply_received_context_t &) {
          request_hook_order->push_back ("reply-second");
          throw std::runtime_error ("expected reply hook failure");
      });
    auto request_callback_response = std::make_shared<callback_outcome_t> ();
    callback_response_connector.request (login_request_t{})
      .packet_name ("callback.response.request")
      .timeout (std::chrono::milliseconds (100))
      .submit<login_reply_t> (
        [request_callback_response] (zlink::stream_connector::result_t<login_reply_t> result) {
            request_callback_response->seen = static_cast<bool> (result);
            request_callback_response->latch.signal ();
        });
    callback_response_thread.join ();
    request_callback_response->latch.wait_for (std::chrono::milliseconds (100));
    reply_hook_error->latch.wait_for (std::chrono::milliseconds (100));
    const std::vector<std::string> expected_request_hook_order{"sending-first", "sending-second",
                                                               "reply-first", "reply-second"};
    if (!request_callback_response->seen || !request_hook_metadata_seen || !reply_hook_error->seen
        || *request_hook_order != expected_request_hook_order) {
        return fail (61);
    }
    callback_response_connector.close ();

    zlink::stream_socket_t invalid_reply_server (context);
    invalid_reply_server.options ().recv_mode (zlink::stream_recv_mode_t::raw);
    invalid_reply_server.options ().notify (false);
    invalid_reply_server.bind ("tcp://127.0.0.1:0");
    joining_thread_t invalid_reply_thread ([&] {
        zlink::received_t inbound;
        if (invalid_reply_server.recv (inbound) != 0)
            return;
        std::string buffer =
          inbound.parts ().empty () ? std::string{} : inbound.parts ()[0].to_string ();
        if (auto frame = try_read_server_frame (buffer)) {
            zlink::stream_connector::detail::stream_header_t invalid_header;
            invalid_header.kind = zlink::stream_connector::message_kind_t::response;
            invalid_header.codec = zlink::stream_connector::codec_t::json;
            invalid_header.request_seq = frame->header.request_seq;
            const auto encoded_header =
              zlink::stream_connector::detail::header_codec_t{}.encode (invalid_header);
            const std::string invalid_json = "not-json";
            const std::vector<std::uint8_t> invalid_payload (invalid_json.begin (),
                                                             invalid_json.end ());
            const auto encoded_reply = zlink::stream_connector::detail::frame_codec_t::encode (
              encoded_header.value (), invalid_payload,
              zlink::stream_connector::connector_options_t{});
            auto reply = zlink::message_t::from (
              std::string (encoded_reply.value ().begin (), encoded_reply.value ().end ()));
            inbound.send ().message (reply).submit ();
        }
        inbound.close ();
    });
    zlink::stream_connector::connector_options_t invalid_reply_options;
    invalid_reply_options.endpoint = invalid_reply_server.options ().last_endpoint ();
    invalid_reply_options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;
    auto invalid_reply_connector =
      zlink::stream_connector::connector_factory_t::create (invalid_reply_options);
    if (!invalid_reply_connector.connect ()) {
        return fail (283);
    }
    auto invalid_reply_hook_seen = std::make_shared<callback_outcome_t> ();
    auto invalid_reply_hook = invalid_reply_connector.on_reply_received (
      [invalid_reply_hook_seen] (const zlink::stream_connector::reply_received_context_t &ctx) {
          invalid_reply_hook_seen->seen =
            ctx.request_packet_name == "invalid.typed.reply" && !ctx.succeeded && !ctx.reply
            && ctx.error
            && ctx.error->code == zlink::stream_connector::error_code_t::frame_decode_failed;
      });
    auto invalid_reply = std::make_shared<callback_outcome_t> ();
    invalid_reply_connector.request (login_request_t{})
      .packet_name ("invalid.typed.reply")
      .timeout (std::chrono::milliseconds (100))
      .submit<auto_payload_t> (
        [invalid_reply] (zlink::stream_connector::result_t<auto_payload_t> result) {
            invalid_reply->seen =
              !result
              && result.error_code () == zlink::stream_connector::error_code_t::frame_decode_failed;
            invalid_reply->latch.signal ();
        });
    invalid_reply_thread.join ();
    invalid_reply->latch.wait_for (std::chrono::milliseconds (100));
    if (!invalid_reply->seen || !invalid_reply_hook_seen->seen) {
        return fail (284);
    }
    invalid_reply_connector.close ();

    zlink::stream_socket_t callback_timeout_server (context);
    callback_timeout_server.options ().recv_mode (zlink::stream_recv_mode_t::raw);
    callback_timeout_server.options ().notify (false);
    callback_timeout_server.bind ("tcp://127.0.0.1:0");
    const auto callback_timeout_endpoint = callback_timeout_server.options ().last_endpoint ();
    std::atomic_bool callback_timeout_request_seen{false};
    joining_thread_t callback_timeout_thread (
      [&callback_timeout_server, &callback_timeout_request_seen] {
          zlink::received_t inbound;
          if (callback_timeout_server.recv (inbound) != 0) {
              return;
          }
          callback_timeout_request_seen = true;
          inbound.close ();
      });
    // Same as the timeout.request check: leave room for the write to start.
    zlink::stream_connector::connector_options_t callback_timeout_options;
    callback_timeout_options.endpoint = callback_timeout_endpoint;
    callback_timeout_options.request_timeout = std::chrono::milliseconds (200);
    callback_timeout_options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;
    auto callback_timeout_connector =
      zlink::stream_connector::connector_factory_t::create (callback_timeout_options);
    if (!callback_timeout_connector.connect ()) {
        return fail (62);
    }
    auto timeout_hook_seen = std::make_shared<callback_outcome_t> ();
    auto timeout_hook = callback_timeout_connector.on_reply_received (
      [timeout_hook_seen] (const zlink::stream_connector::reply_received_context_t &ctx) {
          timeout_hook_seen->seen =
            ctx.request_packet_name == "callback.timeout.request" && !ctx.actor_id && !ctx.succeeded
            && !ctx.reply && ctx.error
            && ctx.error->code == zlink::stream_connector::error_code_t::request_timeout
            && ctx.elapsed.count () >= 0;
      });
    auto request_callback_timeout = std::make_shared<callback_outcome_t> ();
    callback_timeout_connector.request (login_request_t{})
      .packet_name ("callback.timeout.request")
      .timeout (std::chrono::milliseconds (200))
      .submit<login_reply_t> (
        [request_callback_timeout] (zlink::stream_connector::result_t<login_reply_t> result) {
            request_callback_timeout->seen =
              !result
              && result.error_code () == zlink::stream_connector::error_code_t::request_timeout;
            request_callback_timeout->latch.signal ();
        });
    callback_timeout_thread.join ();
    request_callback_timeout->latch.wait_for (std::chrono::seconds (2));
    if (!request_callback_timeout->seen || !callback_timeout_request_seen
        || !timeout_hook_seen->seen) {
        return fail (63);
    }
    callback_timeout_connector.close ();

    zlink::stream_socket_t async_pump_server (context);
    async_pump_server.options ().recv_mode (zlink::stream_recv_mode_t::raw);
    async_pump_server.options ().notify (false);
    async_pump_server.bind ("tcp://127.0.0.1:0");
    const auto async_pump_endpoint = async_pump_server.options ().last_endpoint ();
    joining_thread_t async_pump_thread ([&async_pump_server] {
        zlink::received_t inbound;
        if (async_pump_server.recv (inbound) != 0) {
            return;
        }
        std::string buffer =
          inbound.parts ().empty () ? std::string{} : inbound.parts ()[0].to_string ();
        if (auto frame = try_read_server_frame (buffer)) {
            auto push = make_server_frame (zlink::stream_connector::message_kind_t::send, 0,
                                           "async.pump.push", "push");
            inbound.send ().message (push).submit ();
            auto reply = make_server_frame (zlink::stream_connector::message_kind_t::response,
                                            frame->header.request_seq.value (), frame->header.name,
                                            "async-pump-reply");
            inbound.send ().message (reply).submit ();
        }
        inbound.close ();
    });
    zlink::stream_connector::connector_options_t async_pump_options;
    async_pump_options.endpoint = async_pump_endpoint;
    async_pump_options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;
    async_pump_options.request_timeout = std::chrono::milliseconds (100);
    auto async_pump_connector =
      zlink::stream_connector::connector_factory_t::create (async_pump_options);
    if (!async_pump_connector.connect ()) {
        async_pump_thread.join ();
        return fail (156);
    }
    auto async_pump_wait = std::make_shared<callback_outcome_t> ();
    async_pump_connector.wait_for<zlink::stream_connector::packet_t> ("async.pump.push")
      .timeout (std::chrono::milliseconds (100))
      .submit ([async_pump_wait] (zlink::stream_connector::result_t<packet_message_t> result) {
          async_pump_wait->seen = result && as_string (result.value ().payload.payload) == "push";
          async_pump_wait->latch.signal ();
      });
    auto async_pump_reply = async_pump_connector.request (login_request_t{})
                              .packet_name ("async.pump.request")
                              .timeout (std::chrono::milliseconds (100))
                              .submit<login_reply_t> ();
    async_pump_wait->latch.wait_for (std::chrono::milliseconds (100));
    async_pump_thread.join ();
    async_pump_connector.close ();
    if (!async_pump_reply || !async_pump_wait->seen) {
        return fail (157);
    }

    zlink::stream_socket_t close_cleanup_server (context);
    close_cleanup_server.options ().recv_mode (zlink::stream_recv_mode_t::raw);
    close_cleanup_server.options ().notify (false);
    close_cleanup_server.bind ("tcp://127.0.0.1:0");
    const auto close_cleanup_endpoint = close_cleanup_server.options ().last_endpoint ();
    callback_latch_t close_cleanup_request_latch;
    callback_latch_t close_cleanup_release_latch;
    joining_thread_t close_cleanup_thread (
      [&close_cleanup_server, &close_cleanup_request_latch, &close_cleanup_release_latch] {
          zlink::received_t inbound;
          if (close_cleanup_server.recv (inbound) != 0) {
              return;
          }
          close_cleanup_request_latch.signal ();
          close_cleanup_release_latch.wait_for (std::chrono::milliseconds (100));
          inbound.close ();
      });
    zlink::stream_connector::connector_options_t close_cleanup_options;
    close_cleanup_options.endpoint = close_cleanup_endpoint;
    close_cleanup_options.request_timeout = std::chrono::milliseconds (1000);
    close_cleanup_options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;
    auto close_cleanup_connector =
      zlink::stream_connector::connector_factory_t::create (close_cleanup_options);
    if (!close_cleanup_connector.connect ()) {
        close_cleanup_release_latch.signal ();
        close_cleanup_thread.join ();
        return fail (80);
    }
    auto close_cleanup = std::make_shared<callback_outcome_t> ();
    const auto close_cleanup_submit_started = std::chrono::steady_clock::now ();
    close_cleanup_connector.request (login_request_t{})
      .packet_name ("close.cleanup.request")
      .timeout (std::chrono::milliseconds (1000))
      .submit<login_reply_t> ([close_cleanup] (
                                zlink::stream_connector::result_t<login_reply_t> result) {
          close_cleanup->seen =
            !result && result.error_code () == zlink::stream_connector::error_code_t::disconnected;
          close_cleanup->latch.signal ();
      });
    const auto close_cleanup_submit_elapsed =
      std::chrono::steady_clock::now () - close_cleanup_submit_started;
    if (close_cleanup_submit_elapsed > std::chrono::milliseconds (50)) {
        close_cleanup_release_latch.signal ();
        close_cleanup_thread.join ();
        return fail (96);
    }
    if (!close_cleanup_request_latch.wait_for (std::chrono::milliseconds (100))) {
        close_cleanup_release_latch.signal ();
        close_cleanup_thread.join ();
        return fail (81);
    }
    close_cleanup_connector.close ();
    close_cleanup_release_latch.signal ();
    close_cleanup_thread.join ();
    close_cleanup->latch.wait_for (std::chrono::milliseconds (100));
    if (!close_cleanup->seen) {
        return fail (82);
    }

    return verify_coroutine_operations (context, options, connector, runtime, request, immediate);
}

int verify_completion_from_callback ()
{
    /* SC §7 and §10.1.1: unregistered waits complete without a pump.
     * The callback submits a request and returns; the test thread waits for completion. */
    for (const auto mode : {zlink::stream_connector::dispatch_mode_t::manual,
                            zlink::stream_connector::dispatch_mode_t::immediate}) {
        zlink::stream_connector::connector_options_t nested_options;
        nested_options.dispatch_mode = mode;
        nested_options.reconnect.enabled = false;
        nested_options.heartbeat.enabled = false;
        auto nested_connector =
          zlink::stream_connector::connector_factory_t::create (nested_options);
        auto state = std::static_pointer_cast<zlink::stream_connector::detail::connector_state_t> (
          connector_internal_handle (nested_connector));
        auto connection = std::make_shared<fed_read_connection_t> ();
        state->connection = connection;
        zlink::stream_connector::detail::change_state (
          state, zlink::stream_connector::connection_state_t::connected);
        zlink::stream_connector::detail::start_read_loop (state);
        callback_latch_t nested_done;
        std::optional<
          zlink::stream_connector::result_t<zlink::stream_connector::detail::request_reply_t>>
          nested_reply;
        std::atomic_int wait_callback_calls{0};
        std::atomic_int request_callback_calls{0};
        zlink::stream_connector::detail::submit_wait_async (
          state, "wait.trigger", {}, std::chrono::seconds (5),
          [&] (zlink::stream_connector::result_t<zlink::stream_connector::packet_t> trigger) {
              ++wait_callback_calls;
              if (!trigger) {
                  nested_done.signal ();
                  return;
              }
              zlink::stream_connector::detail::submit_request_async (
                state,
                zlink::stream_connector::packet_t{.name = "nested.request",
                                                  .payload = as_bytes ("q")},
                std::chrono::seconds (2),
                [&] (zlink::stream_connector::result_t<
                     zlink::stream_connector::detail::request_reply_t> reply) {
                    ++request_callback_calls;
                    nested_reply = std::move (reply);
                    nested_done.signal ();
                },
                /*deliver_direct=*/true);
          });
        connection->feed (make_server_frame (zlink::stream_connector::message_kind_t::send, 0,
                                             "wait.trigger", "go"));
        std::optional<std::uint64_t> nested_seq;
        if (const auto written = connection->wait_for_written (0)) {
            std::string text (written->begin (), written->end ());
            if (const auto frame = try_read_server_frame (text)) {
                nested_seq = frame->header.request_seq;
            }
        }
        if (nested_seq) {
            connection->feed (make_server_frame (zlink::stream_connector::message_kind_t::response,
                                                 *nested_seq, "", "reply"));
        }
        const bool nested_finished = nested_done.wait_for (std::chrono::seconds (5));
        (void) nested_connector.close ();
        if (!nested_seq || !nested_finished || !nested_reply || !*nested_reply
            || wait_callback_calls.load () != 1 || request_callback_calls.load () != 1
            || as_string (nested_reply->value ().packet.payload) != "reply") {
            return fail (363);
        }
    }

    return 0;
}
} // namespace stream_connector_tests
