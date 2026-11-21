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


namespace stream_connector_tests
{
int verify_write_order ()
{
    /* §5.2: frames are written one at a time. A Send accepted while another
   * write is in progress starts only after that write completes. */
    {
        zlink::stream_connector::connector_options_t serial_options;
        serial_options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;
        auto state =
          std::make_shared<zlink::stream_connector::detail::connector_state_t> (serial_options);
        auto connection = std::make_shared<held_async_write_connection_t> ();
        state->connection = connection;
        zlink::stream_connector::detail::change_state (
          state, zlink::stream_connector::connection_state_t::connected);
        send_results_t serial_results;
        zlink::stream_connector::detail::submit_send_async (
          state,
          zlink::stream_connector::packet_t{.name = "serialized.first",
                                            .payload = as_bytes (std::string ("first"))},
          serial_results.recorder ());
        const auto first_started = connection->wait_for_write_count (1);
        zlink::stream_connector::detail::submit_send_async (
          state,
          zlink::stream_connector::packet_t{.name = "serialized.after.request",
                                            .payload = as_bytes (std::string ("second"))},
          serial_results.recorder ());
        if (!first_started || !drain_strand (state->write_strand) || connection->overlapped ()
            || connection->write_count () != 1) {
            return fail (240);
        }

        connection->complete_write ();
        if (!connection->wait_for_write_count (2) || connection->overlapped ()) {
            return fail (241);
        }
        const auto encoded = connection->written (1);
        std::string encoded_text (encoded.begin (), encoded.end ());
        const auto frame = try_read_server_frame (encoded_text);
        if (!frame || frame->header.kind != zlink::stream_connector::message_kind_t::send
            || frame->header.name != "serialized.after.request") {
            return fail (242);
        }
        connection->complete_write ();
        if (!serial_results.wait_for_count (2) || !serial_results.all_succeeded ()) {
            return fail (243);
        }
        connection->shutdown_and_close ();
    }

    {
        using zlink::stream_connector::detail::connector_state_t;
        auto state =
          std::make_shared<connector_state_t> (zlink::stream_connector::connector_options_t{});
        auto connection = std::make_shared<held_async_write_connection_t> ();
        state->connection = connection;
        zlink::stream_connector::detail::change_state (
          state, zlink::stream_connector::connection_state_t::connected);
        zlink::stream_connector::detail::submit_send_async (
          state,
          zlink::stream_connector::packet_t{.name = "wait.blocker", .payload = as_bytes ("a")}, {},
          std::nullopt);
        if (!connection->wait_for_write_count (1))
            return fail (319);
        callback_latch_t request_latch;
        std::optional<zlink::stream_connector::error_code_t> request_result;
        zlink::stream_connector::detail::submit_request_async (
          state,
          zlink::stream_connector::packet_t{.name = "wait.expire", .payload = as_bytes ("b")},
          std::chrono::milliseconds (25),
          [&] (zlink::stream_connector::result_t<zlink::stream_connector::detail::request_reply_t>
                 result) {
              request_result = result.error_code ();
              request_latch.signal ();
          },
          /*deliver_direct=*/true);
        if (!request_latch.wait_for (std::chrono::seconds (1))) {
            return fail (320);
        }
        {
            std::lock_guard<std::mutex> lock (state->transport_mutex);
            if (request_result != zlink::stream_connector::error_code_t::request_timeout
                || !state->pending_requests.empty ()
                || state->state != zlink::stream_connector::connection_state_t::connected) {
                return fail (321);
            }
        }
        connection->complete_write ();
        connection->shutdown_and_close ();
    }

    {
        auto connector = zlink::stream_connector::connector_factory_t::create (
          zlink::stream_connector::connector_options_t{});
        auto state = std::static_pointer_cast<zlink::stream_connector::detail::connector_state_t> (
          connector_internal_handle (connector));
        auto connection = std::make_shared<held_async_write_connection_t> ();
        state->connection = connection;
        zlink::stream_connector::detail::change_state (
          state, zlink::stream_connector::connection_state_t::connected);
        std::optional<zlink::stream_connector::error_code_t> active_result;
        zlink::stream_connector::detail::submit_send_async (
          state,
          zlink::stream_connector::packet_t{.name = "close.active", .payload = as_bytes ("a")},
          [&] (zlink::stream_connector::result_t<void> result) {
              active_result = result.error_code ();
          },
          std::nullopt);
        const auto active_deadline = std::chrono::steady_clock::now () + std::chrono::seconds (1);
        while (connection->write_count () == 0
               && std::chrono::steady_clock::now () < active_deadline) {
            std::this_thread::sleep_for (std::chrono::milliseconds (1));
        }
        std::optional<zlink::stream_connector::error_code_t> queued_result;
        bool queued_callback_called = false;
        zlink::stream_connector::detail::submit_send_async (
          state,
          zlink::stream_connector::packet_t{.name = "close.queued", .payload = as_bytes ("b")},
          [&] (zlink::stream_connector::result_t<void> result) {
              queued_callback_called = true;
              queued_result = result.error_code ();
          },
          std::nullopt);
        /* stream-connector §7: close does not wait for the peer to read. The
     * frame not yet written is never written, and every Send whose frame
     * did not complete on the transport fails with Disconnected; the
     * close reason is ClientClose (§9). */
        callback_latch_t close_done;
        joining_thread_t close_thread ([&] {
            (void) connector.close ();
            close_done.signal ();
        });
        const bool closed_without_peer = close_done.wait_for (std::chrono::seconds (1));
        if (!closed_without_peer) {
            connection->complete_write ();
            const auto queued_deadline =
              std::chrono::steady_clock::now () + std::chrono::seconds (1);
            while (connection->write_count () < 2
                   && std::chrono::steady_clock::now () < queued_deadline) {
                std::this_thread::sleep_for (std::chrono::milliseconds (1));
            }
            connection->complete_write ();
        }
        close_thread.join ();
        /* Pump each Send completion separately (LC §2). */
        (void) zlink::stream_connector::detail::dispatch_pending (state);
        (void) zlink::stream_connector::detail::dispatch_pending (state);
        if (!closed_without_peer) {
            return fail (323);
        }
        if (connection->write_count () != 1 || !queued_callback_called
            || queued_result != zlink::stream_connector::error_code_t::disconnected
            || active_result != zlink::stream_connector::error_code_t::disconnected
            || connector.close_reason () != zlink::stream_connector::close_reason_t::client_close
            || connector.state () != zlink::stream_connector::connection_state_t::closed) {
            return fail (324);
        }
    }

    {
        using zlink::stream_connector::detail::connector_state_t;
        auto state =
          std::make_shared<connector_state_t> (zlink::stream_connector::connector_options_t{});
        auto old_connection = std::make_shared<held_async_write_connection_t> ();
        auto new_connection = std::make_shared<held_async_write_connection_t> ();
        state->connection = old_connection;
        zlink::stream_connector::detail::change_state (
          state, zlink::stream_connector::connection_state_t::connected);
        zlink::stream_connector::detail::submit_send_async (
          state, zlink::stream_connector::packet_t{.name = "old.active", .payload = as_bytes ("a")},
          {}, std::nullopt);
        const auto active_deadline = std::chrono::steady_clock::now () + std::chrono::seconds (1);
        while (old_connection->write_count () == 0
               && std::chrono::steady_clock::now () < active_deadline) {
            std::this_thread::sleep_for (std::chrono::milliseconds (1));
        }
        std::optional<zlink::stream_connector::error_code_t> queued_result;
        zlink::stream_connector::detail::submit_send_async (
          state, zlink::stream_connector::packet_t{.name = "old.queued", .payload = as_bytes ("b")},
          [&] (zlink::stream_connector::result_t<void> result) {
              queued_result = result.error_code ();
          },
          std::nullopt);
        if (old_connection->write_count () != 1) {
            return fail (316);
        }
        zlink::stream_connector::detail::connection_ended (
          state, {zlink::stream_connector::error_code_t::disconnected, "old connection ended"},
          old_connection);
        {
            std::lock_guard<std::mutex> lock (state->transport_mutex);
            state->connection = new_connection;
        }
        zlink::stream_connector::detail::change_state (
          state, zlink::stream_connector::connection_state_t::connected);
        old_connection->complete_write ();
        const auto drained_deadline = std::chrono::steady_clock::now () + std::chrono::seconds (1);
        while (!queued_result && std::chrono::steady_clock::now () < drained_deadline) {
            (void) zlink::stream_connector::detail::dispatch_pending (state);
            std::this_thread::sleep_for (std::chrono::milliseconds (1));
        }
        if (queued_result != zlink::stream_connector::error_code_t::disconnected
            || new_connection->write_count () != 0) {
            return fail (317);
        }
        old_connection->shutdown_and_close ();
        new_connection->shutdown_and_close ();
    }

    {
        auto state = std::make_shared<zlink::stream_connector::detail::connector_state_t> (
          zlink::stream_connector::connector_options_t{});
        /* A read failure of a replaced connection, completed after the
     * replacement, does not end the replacement. */
        auto old_connection =
          std::make_shared<scripted_read_connection_t> (std::vector<std::uint8_t>{}, false);
        auto new_connection = std::make_shared<async_write_connection_t> ();
        state->connection = old_connection;
        zlink::stream_connector::detail::change_state (
          state, zlink::stream_connector::connection_state_t::connected);
        zlink::stream_connector::detail::start_read_loop (state);
        {
            std::lock_guard<std::mutex> lock (state->transport_mutex);
            state->connection = new_connection;
        }
        if (!old_connection->fail_pending_read ()) {
            return fail (328);
        }
        {
            std::lock_guard<std::mutex> lock (state->transport_mutex);
            if (state->connection != new_connection
                || state->state != zlink::stream_connector::connection_state_t::connected) {
                return fail (329);
            }
        }
        new_connection->shutdown_and_close ();
    }

    return 0;
}

int verify_write_failure_and_close ()
{
    /* A Send without a callback reports every failure as an error event, the
   * missing connection included (cpp stream-connector: one-way submit). A
   * Request reports its validation failure through its own result only. */
    {
        zlink::stream_connector::connector_options_t report_options;
        report_options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;
        report_options.reconnect.enabled = false;
        report_options.max_send_payload_size = 4;
        auto report_connector =
          zlink::stream_connector::connector_factory_t::create (report_options);
        auto state = std::static_pointer_cast<zlink::stream_connector::detail::connector_state_t> (
          connector_internal_handle (report_connector));
        std::mutex reported_mutex;
        std::vector<zlink::stream_connector::error_code_t> reported;
        callback_latch_t first_report;
        auto report_subscription =
          report_connector.on_error ([&] (const zlink::stream_connector::error_t &error) {
              {
                  std::lock_guard<std::mutex> lock (reported_mutex);
                  reported.push_back (error.code);
              }
              first_report.signal ();
          });
        const auto reported_codes = [&] {
            std::lock_guard<std::mutex> lock (reported_mutex);
            return reported;
        };
        report_connector
          .send (zlink::stream_connector::packet_t{.name = "unconnected.send",
                                                   .payload = as_bytes ("x")})
          .submit ();
        if (!first_report.wait_for (std::chrono::seconds (5))
            || reported_codes ()
                 != std::vector<zlink::stream_connector::error_code_t>{
                   zlink::stream_connector::error_code_t::disconnected}) {
            return fail (364);
        }

        state->connection = std::make_shared<async_write_connection_t> ();
        zlink::stream_connector::detail::change_state (
          state, zlink::stream_connector::connection_state_t::connected);
        callback_latch_t request_done;
        std::optional<zlink::stream_connector::error_code_t> request_code;
        zlink::stream_connector::detail::submit_request_async (
          state,
          zlink::stream_connector::packet_t{.name = "oversized.request",
                                            .payload = as_bytes ("oversized")},
          std::chrono::seconds (1),
          [&] (zlink::stream_connector::result_t<zlink::stream_connector::detail::request_reply_t>
                 result) {
              request_code = result.error_code ();
              request_done.signal ();
          },
          /*deliver_direct=*/false);
        report_connector
          .send (zlink::stream_connector::packet_t{.name = "oversized.send",
                                                   .payload = as_bytes ("oversized")})
          .submit ();
        const bool request_finished = request_done.wait_for (std::chrono::seconds (5));
        /* Check completion after the write invokes its error callback directly. */
        const bool delivered = drain_strand (state->write_strand);
        (void) report_connector.close ();
        if (!request_finished || !delivered
            || request_code != zlink::stream_connector::error_code_t::validation_failed
            || reported_codes ()
                 != std::vector<zlink::stream_connector::error_code_t>{
                   zlink::stream_connector::error_code_t::disconnected,
                   zlink::stream_connector::error_code_t::validation_failed}) {
            return fail (365);
        }
    }

    /* stream-connector §7: once close is requested no frame write starts. A close
   * called inside a callback sets the request and leaves the close work to
   * run later; a write that completes in between does not start the next
   * queued frame, which the close work then fails as Disconnected. */
    {
        zlink::stream_connector::connector_options_t pending_close_options;
        pending_close_options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;
        pending_close_options.reconnect.enabled = false;
        auto pending_close_connector =
          zlink::stream_connector::connector_factory_t::create (pending_close_options);
        auto state = std::static_pointer_cast<zlink::stream_connector::detail::connector_state_t> (
          connector_internal_handle (pending_close_connector));
        auto connection = std::make_shared<held_async_write_connection_t> ();
        state->connection = connection;
        zlink::stream_connector::detail::change_state (
          state, zlink::stream_connector::connection_state_t::connected);
        send_results_t pending_close_results;
        zlink::stream_connector::detail::submit_send_async (
          state,
          zlink::stream_connector::packet_t{.name = "close.active", .payload = as_bytes ("a")},
          pending_close_results.recorder ());
        const bool active_started = connection->wait_for_write_count (1);
        zlink::stream_connector::detail::submit_send_async (
          state,
          zlink::stream_connector::packet_t{.name = "close.queued", .payload = as_bytes ("b")},
          pending_close_results.recorder ());
        state->close_requested.store (true);
        connection->complete_write ();
        /* The completion hop and the next-write hop have both run. */
        const bool write_path_settled =
          drain_strand (state->write_strand) && drain_strand (state->write_strand);
        const bool queued_written = connection->write_count () != 1;
        /* Hand the pending close to the close work, as the posted work would. */
        state->close_requested.store (false);
        (void) pending_close_connector.close ();
        const bool settled = pending_close_results.wait_for_count (2);
        const auto codes = pending_close_results.codes ();
        if (!active_started || !write_path_settled || queued_written || !settled
            || codes.size () != 2 || codes[0].has_value ()
            || codes[1] != zlink::stream_connector::error_code_t::disconnected) {
            return fail (370);
        }
    }

    return 0;
}
} // namespace stream_connector_tests
