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
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
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

static_assert (
  std::is_same_v<decltype (std::declval<zlink::stream_connector::connector_t &> ().wait_for (
                   "packet", std::chrono::milliseconds (1))),
                 zlink::stream_connector::result_t<zlink::stream_connector::packet_t>>);

static_assert (std::is_same_v<decltype (std::declval<zlink::stream_connector::connector_t &> ()
                                          .expect_none<zlink::stream_connector::packet_t> ("packet")
                                          .within (std::chrono::milliseconds (1))
                                          .submit ()),
                              zlink::stream_connector::result_t<void>>);

static_assert (
  std::is_same_v<decltype (std::declval<zlink::stream_connector::connector_t &> ()
                             .wait_for_sequence<zlink::stream_connector::packet_t> ("packet")
                             .expect ([] (const zlink::stream_connector::message_t<
                                          zlink::stream_connector::packet_t> &) { return true; })
                             .timeout (std::chrono::milliseconds (1))
                             .submit ()),
                 zlink::stream_connector::result_t<std::vector<
                   zlink::stream_connector::message_t<zlink::stream_connector::packet_t>>>>);

namespace
{
static_assert (!has_core_async_terminator<zlink::stream_connector::wait_call_t<auto_payload_t>>);

static_assert (
  std::is_same_v<decltype (std::declval<zlink::stream_e2e_client::coroutine_connector_t &> ()
                             .wait_for_sequence<auto_payload_t> ()
                             .expect ([] (const zlink::stream_connector::message_t<auto_payload_t>
                                            &) { return true; })
                             .async ()),
                 zlink::stream_e2e_client::task_t<
                   std::vector<zlink::stream_connector::message_t<auto_payload_t>>>>);
} // namespace

namespace stream_connector_tests
{
int verify_wait_connection_loss ()
{
    /* stream-connector §10.1.1: a wait ends as disconnected when the connection
   * it observed ends - not when the next connection is established. With
   * reconnect off there is no next connection, so a release bound to it
   * would leave the wait hanging until its own timeout; with reconnect on
   * the release still precedes the reconnect. */
    for (const bool reconnect_enabled : {false, true}) {
        boost::asio::io_context ending_io;
        boost::asio::ip::tcp::acceptor ending_acceptor (
          ending_io, {boost::asio::ip::make_address ("127.0.0.1"), 0});
        const auto ending_endpoint = std::string ("tcp://127.0.0.1:")
                                     + std::to_string (ending_acceptor.local_endpoint ().port ());
        callback_latch_t release_ending_connection;
        joining_thread_t ending_server ([&ending_acceptor, &release_ending_connection] {
            boost::asio::ip::tcp::socket accepted (ending_acceptor.get_executor ());
            ending_acceptor.accept (accepted);
            release_ending_connection.wait_for (std::chrono::seconds (5));
            boost::system::error_code error;
            accepted.shutdown (boost::asio::ip::tcp::socket::shutdown_both, error);
            accepted.close (error);
        });

        zlink::stream_connector::connector_options_t ending_options;
        ending_options.endpoint = ending_endpoint;
        ending_options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;
        ending_options.heartbeat.enabled = false;
        ending_options.connect_timeout = std::chrono::milliseconds (1000);
        ending_options.reconnect.enabled = reconnect_enabled;
        /* Jittered to at least 1 s, so no reconnect can complete before the
     * release below is measured. */
        ending_options.reconnect.initial_delay = std::chrono::milliseconds (2000);
        ending_options.reconnect.max_delay = std::chrono::milliseconds (2000);
        ending_options.reconnect.max_attempts = 3;
        auto ending_connector =
          zlink::stream_connector::connector_factory_t::create (ending_options);
        if (!ending_connector.connect ()) {
            release_ending_connection.signal ();
            ending_server.join ();
            return fail (183);
        }
        // The callback owns its state: it may run after this frame ended.
        struct wait_state_t
        {
            callback_latch_t completed;
            std::atomic_bool succeeded{false};
            std::optional<zlink::stream_connector::error_code_t> error;
        };
        auto wait_state = std::make_shared<wait_state_t> ();
        ending_connector.wait_for<zlink::stream_connector::packet_t> ("never.arrives")
          .timeout (std::chrono::seconds (5))
          .submit ([wait_state] (zlink::stream_connector::result_t<packet_message_t> result) {
              wait_state->succeeded = static_cast<bool> (result);
              wait_state->error = result.error_code ();
              wait_state->completed.signal ();
          });
        const auto ended_at = std::chrono::steady_clock::now ();
        release_ending_connection.signal ();
        /* Released by the ending: well inside the 5 s wait timeout and inside
     * the reconnect delay. */
        const bool released = wait_state->completed.wait_for (std::chrono::milliseconds (1000));
        const auto release_took = std::chrono::steady_clock::now () - ended_at;
        const auto state_at_release = ending_connector.state ();
        ending_server.join ();
        ending_connector.close ();
        if (!released || wait_state->succeeded
            || wait_state->error != zlink::stream_connector::error_code_t::disconnected) {
            return reconnect_enabled ? 185 : 184;
        }
        if (release_took >= std::chrono::milliseconds (1000)
            || state_at_release == zlink::stream_connector::connection_state_t::connected) {
            return reconnect_enabled ? 187 : 186;
        }
    }

    /* stream-connector §10.1.1: a synchronous wait whose connection ends
   * because of a frame error ends as disconnected too. The frame error
   * itself reaches the error handler; the wait reports only that it has no
   * place left to observe. */
    {
        boost::asio::io_context bad_frame_io;
        boost::asio::ip::tcp::acceptor bad_frame_acceptor (
          bad_frame_io, {boost::asio::ip::make_address ("127.0.0.1"), 0});
        const auto bad_frame_endpoint =
          std::string ("tcp://127.0.0.1:")
          + std::to_string (bad_frame_acceptor.local_endpoint ().port ());
        callback_latch_t send_bad_frame;
        callback_latch_t release_bad_frame_connection;
        joining_thread_t bad_frame_server (
          [&bad_frame_acceptor, &send_bad_frame, &release_bad_frame_connection] {
              boost::asio::ip::tcp::socket accepted (bad_frame_acceptor.get_executor ());
              bad_frame_acceptor.accept (accepted);
              send_bad_frame.wait_for (std::chrono::seconds (5));
              /* header_size 1, payload_size 0, and a header byte no header
       * codec accepts: a frame that decodes to nothing. */
              const std::array<std::uint8_t, 7> bad_frame{0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0xFF};
              boost::system::error_code error;
              boost::asio::write (accepted, boost::asio::buffer (bad_frame), error);
              /* The socket stays open, so the ending is the frame error and
       * not an EOF. */
              release_bad_frame_connection.wait_for (std::chrono::seconds (5));
              accepted.shutdown (boost::asio::ip::tcp::socket::shutdown_both, error);
              accepted.close (error);
          });

        zlink::stream_connector::connector_options_t bad_frame_options;
        bad_frame_options.endpoint = bad_frame_endpoint;
        bad_frame_options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;
        bad_frame_options.heartbeat.enabled = false;
        bad_frame_options.reconnect.enabled = false;
        bad_frame_options.connect_timeout = std::chrono::milliseconds (1000);
        auto bad_frame_connector =
          zlink::stream_connector::connector_factory_t::create (bad_frame_options);
        std::mutex published_mutex;
        std::vector<zlink::stream_connector::error_code_t> published_codes;
        auto bad_frame_error_subscription =
          bad_frame_connector.on_error ([&] (const zlink::stream_connector::error_t &error) {
              std::lock_guard<std::mutex> lock (published_mutex);
              published_codes.push_back (error.code);
          });
        if (!bad_frame_connector.connect ()) {
            send_bad_frame.signal ();
            release_bad_frame_connection.signal ();
            bad_frame_server.join ();
            return fail (183);
        }
        send_bad_frame.signal ();
        const auto sync_wait =
          bad_frame_connector.wait_for ("never.arrives", std::chrono::seconds (2));
        const auto published_deadline =
          std::chrono::steady_clock::now () + std::chrono::milliseconds (1000);
        bool frame_error_published = false;
        while (!frame_error_published && std::chrono::steady_clock::now () < published_deadline) {
            {
                std::lock_guard<std::mutex> lock (published_mutex);
                for (const auto code : published_codes) {
                    if (code != zlink::stream_connector::error_code_t::disconnected) {
                        frame_error_published = true;
                    }
                }
            }
            std::this_thread::sleep_for (std::chrono::milliseconds (1));
        }
        release_bad_frame_connection.signal ();
        bad_frame_server.join ();
        bad_frame_connector.close ();
        if (sync_wait
            || sync_wait.error_code () != zlink::stream_connector::error_code_t::disconnected) {
            return fail (188);
        }
        if (!frame_error_published) {
            return fail (189);
        }
    }

    return 0;
}

int verify_wait_observations (zlink::context_t &context,
                              const std::string &endpoint,
                              std::atomic_bool &compressed_send_seen,
                              std::atomic_bool &compressible_large_send_seen,
                              std::atomic_bool &uncompressed_send_seen,
                              stream_connector_fixture::joining_thread_t &server_thread,
                              const zlink::stream_connector::connector_options_t &options,
                              zlink::stream_connector::connector_t &connector)
{
    if (connector.options ().typed_codec
        && connector.options ().typed_codec->codec_id ()
             != zlink::stream_connector::codec_t::json) {
        return fail (3);
    }

    connector.send (login_request_t{}).metadata ("trace", "t1").compress ().submit ();
    auto runtime = zlink::stream_connector::detail::connector_runtime_t::from (connector);
    auto none_observed =
      connector.expect_none ("test.none").within (std::chrono::milliseconds (5)).submit ();
    if (!none_observed) {
        return fail (193);
    }
    auto async_none = std::make_shared<callback_outcome_t> ();
    connector.expect_none ("test.none.async")
      .within (std::chrono::milliseconds (5))
      .submit ([async_none] (zlink::stream_connector::result_t<void> result) {
          async_none->seen = static_cast<bool> (result);
          async_none->latch.signal ();
      });
    dispatch_until (connector, async_none->latch, std::chrono::milliseconds (100));
    if (!async_none->seen) {
        return fail (201);
    }
    runtime.receive_packet (
      zlink::stream_connector::packet_t{"test.unexpected",
                                        {},
                                        zlink::stream_connector::codec_t::raw,
                                        false,
                                        as_bytes (std::string ("unexpected"))});
    auto unexpected =
      connector.expect_none ("test.unexpected").within (std::chrono::milliseconds (5)).submit ();
    if (unexpected
        || unexpected.error_code () != zlink::stream_connector::error_code_t::validation_failed) {
        return fail (194);
    }
    for (const auto *payload : {"first", "second", "third"}) {
        runtime.receive_packet (
          zlink::stream_connector::packet_t{"test.sequence",
                                            {},
                                            zlink::stream_connector::codec_t::raw,
                                            false,
                                            as_bytes (std::string (payload))});
    }
    auto sequence = connector.wait_for_sequence ("test.sequence")
                      .expect ([] (const packet_message_t &message) {
                          const auto &packet = message.payload;
                          return as_string (packet.payload) == "first";
                      })
                      .expect ([] (const packet_message_t &message) {
                          const auto &packet = message.payload;
                          return as_string (packet.payload) == "second";
                      })
                      .expect ([] (const packet_message_t &message) {
                          const auto &packet = message.payload;
                          return as_string (packet.payload) == "third";
                      })
                      .timeout (std::chrono::milliseconds (20))
                      .submit ();
    if (!sequence || sequence.value ().size () != 3) {
        return fail (195);
    }
    runtime.receive_packet (zlink::stream_connector::packet_t{"test.out-of-order",
                                                              {},
                                                              zlink::stream_connector::codec_t::raw,
                                                              false,
                                                              as_bytes (std::string ("second"))});
    auto out_of_order = connector.wait_for_sequence ("test.out-of-order")
                          .expect ([] (const packet_message_t &message) {
                              const auto &packet = message.payload;
                              return as_string (packet.payload) == "first";
                          })
                          .timeout (std::chrono::milliseconds (20))
                          .submit ();
    if (out_of_order
        || out_of_order.error_code () != zlink::stream_connector::error_code_t::validation_failed) {
        return fail (196);
    }
    int action_invocations = 0;
    auto asserted_error = zlink::stream_connector::assertions::expect_failure (
      [&] {
          ++action_invocations;
          return zlink::stream_connector::result_t<void>::failure (
            zlink::stream_connector::error_code_t::validation_failed, "expected failure");
      },
      zlink::stream_connector::error_code_t::validation_failed);
    if (action_invocations != 1
        || asserted_error.code != zlink::stream_connector::error_code_t::validation_failed) {
        return fail (197);
    }
    auto timeout_error = zlink::stream_connector::assertions::expect_timeout ([] {
        return zlink::stream_connector::result_t<void>::failure (
          zlink::stream_connector::error_code_t::request_timeout, "expected timeout");
    });
    if (timeout_error.code != zlink::stream_connector::error_code_t::request_timeout) {
        return fail (198);
    }
    bool non_timeout_rethrown = false;
    try {
        (void) zlink::stream_connector::assertions::expect_timeout ([] {
            return zlink::stream_connector::result_t<void>::failure (
              zlink::stream_connector::error_code_t::disconnected, "not a timeout");
        });
    }
    catch (const zlink::stream_connector::assertions::failure_t &failure) {
        non_timeout_rethrown =
          failure.error ().code == zlink::stream_connector::error_code_t::disconnected;
    }
    if (!non_timeout_rethrown) {
        return fail (199);
    }
    bool ensure_message_required = false;
    try {
        zlink::stream_connector::assertions::ensure (true, "");
    }
    catch (const std::invalid_argument &) {
        ensure_message_required = true;
    }
    if (!ensure_message_required) {
        return fail (200);
    }
    const auto typed_send_deadline = std::chrono::steady_clock::now () + std::chrono::seconds (2);
    while (!compressed_send_seen && std::chrono::steady_clock::now () < typed_send_deadline) {
        std::this_thread::yield ();
    }
    if (!compressed_send_seen) {
        return fail (5);
    }
    connector
      .send (zlink::stream_connector::packet_t{"compressible.large",
                                               {},
                                               zlink::stream_connector::codec_t::raw,
                                               false,
                                               as_bytes (std::string (128, 'a'))})
      .compress ()
      .submit ();
    const auto compressible_large_send_deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (2);
    while (!compressible_large_send_seen
           && std::chrono::steady_clock::now () < compressible_large_send_deadline) {
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    const bool compressible_large_send_accepted = compressible_large_send_seen;
    if (!compressible_large_send_accepted) {
        connector
          .send (zlink::stream_connector::packet_t{"compressible.fallback",
                                                   {},
                                                   zlink::stream_connector::codec_t::raw,
                                                   false,
                                                   as_bytes (std::string ("ok"))})
          .submit ();
    }

    if (const int failure = verify_request_terminal_races (); failure != 0)
        return failure;
    return verify_typed_send_and_request (
      context, endpoint, compressed_send_seen, compressible_large_send_seen, uncompressed_send_seen,
      server_thread, options, connector, compressible_large_send_accepted, runtime);
}

int verify_manual_receive (
  zlink::context_t &context,
  const std::string &endpoint,
  const zlink::stream_connector::connector_options_t &options,
  zlink::stream_connector::connector_t &connector,
  zlink::stream_connector::detail::connector_runtime_t &runtime,
  zlink::stream_connector::result_t<stream_connector_fixture::login_reply_t> &request)
{
    zlink::stream_socket_t receive_server (context);
    receive_server.options ().recv_mode (zlink::stream_recv_mode_t::raw);
    receive_server.options ().notify (false);
    receive_server.bind ("tcp://127.0.0.1:0");
    const auto receive_endpoint = receive_server.options ().last_endpoint ();
    joining_thread_t receive_server_thread ([&receive_server] {
        zlink::received_t inbound;
        if (receive_server.recv (inbound) != 0) {
            return;
        }
        auto first = make_server_frame (zlink::stream_connector::message_kind_t::send, 0,
                                        "server.receive.one", "one");
        auto second = make_server_frame (zlink::stream_connector::message_kind_t::send, 0,
                                         "server.receive.two", "two");
        inbound.send ()
          .message (zlink::message_t::from (first.to_string () + second.to_string ()))
          .submit ();
        inbound.close ();
    });
    zlink::stream_connector::connector_options_t receive_options;
    receive_options.endpoint = receive_endpoint;
    receive_options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::manual;
    receive_options.request_timeout = std::chrono::milliseconds (100);
    auto receive_connector = zlink::stream_connector::connector_factory_t::create (receive_options);
    if (!receive_connector.connect ()) {
        return fail (56);
    }
    receive_connector.send (login_request_t{}).packet_name ("receive.trigger").submit ();
    auto received_first =
      receive_connector.wait_for ("server.receive.one", std::chrono::milliseconds (100));
    auto received_second =
      receive_connector.wait_for ("server.receive.two", std::chrono::milliseconds (100));
    receive_server_thread.join ();
    if (!received_first || !received_second || received_first.value ().name != "server.receive.one"
        || as_string (received_first.value ().payload) != "one"
        || received_second.value ().name != "server.receive.two"
        || as_string (received_second.value ().payload) != "two"
        || receive_connector.pending_dispatch_count () != 0) {
        return fail (58);
    }
    receive_connector.close ();

    connector
      .send (zlink::stream_connector::packet_t{"oversized.payload",
                                               {},
                                               zlink::stream_connector::codec_t::raw,
                                               false,
                                               as_bytes (std::string (17, 'x'))})
      .submit ();

    zlink::stream_connector::metadata_t oversized_metadata;
    oversized_metadata.with ("trace", std::string (9 * 1024, 'm'));
    connector
      .send (zlink::stream_connector::packet_t{"oversized.metadata", std::move (oversized_metadata),
                                               zlink::stream_connector::codec_t::raw, false,
                                               as_bytes (std::string ("ok"))})
      .submit ();

    int manual_dispatch_count = 0;
    auto push_subscription = connector.on<zlink::stream_connector::packet_t> (
      "server.push", [&] (const packet_message_t &message) {
          const auto &packet = message.payload;
          if (as_string (packet.payload) == "payload") {
              ++manual_dispatch_count;
          }
      });
    runtime.receive_packet (zlink::stream_connector::packet_t{"server.push",
                                                              {},
                                                              zlink::stream_connector::codec_t::raw,
                                                              false,
                                                              as_bytes (std::string ("payload"))});
    if (manual_dispatch_count != 0 || connector.pending_dispatch_count () != 1) {
        return fail (7);
    }
    if (!connector.dispatch () || manual_dispatch_count != 1
        || connector.pending_dispatch_count () != 0) {
        return fail (8);
    }

    auto immediate_wait_callback = std::make_shared<callback_outcome_t> ();
    runtime.receive_packet (
      zlink::stream_connector::packet_t{"server.wait.immediate",
                                        {},
                                        zlink::stream_connector::codec_t::raw,
                                        false,
                                        as_bytes (std::string ("immediate"))});
    connector.wait_for<zlink::stream_connector::packet_t> ("server.wait.immediate")
      .timeout (std::chrono::milliseconds (100))
      .submit (
        [immediate_wait_callback] (zlink::stream_connector::result_t<packet_message_t> result) {
            immediate_wait_callback->seen =
              result && as_string (result.value ().payload.payload) == "immediate";
            immediate_wait_callback->latch.signal ();
        });
    dispatch_until (connector, immediate_wait_callback->latch, std::chrono::milliseconds (100));
    if (!immediate_wait_callback->seen) {
        return fail (176);
    }

    auto pending_wait_callback = std::make_shared<callback_outcome_t> ();
    connector.wait_for<zlink::stream_connector::packet_t> ("server.wait.callback")
      .timeout (std::chrono::milliseconds (100))
      .submit (
        [pending_wait_callback] (zlink::stream_connector::result_t<packet_message_t> result) {
            pending_wait_callback->seen =
              result && as_string (result.value ().payload.payload) == "wait-callback";
            pending_wait_callback->latch.signal ();
        });
    runtime.receive_packet (
      zlink::stream_connector::packet_t{"server.wait.callback",
                                        {},
                                        zlink::stream_connector::codec_t::raw,
                                        false,
                                        as_bytes (std::string ("wait-callback"))});
    dispatch_until (connector, pending_wait_callback->latch, std::chrono::milliseconds (100));
    if (!pending_wait_callback->seen) {
        return fail (83);
    }

    auto pending_wait_timeout = std::make_shared<callback_outcome_t> ();
    /* stream-connector §10.1: nothing arriving inside the window is a rejected
   * observation, so the wait surface reports validation_failed. */
    connector.wait_for<zlink::stream_connector::packet_t> ("server.wait.missing")
      .timeout (std::chrono::milliseconds (5))
      .submit ([pending_wait_timeout] (zlink::stream_connector::result_t<packet_message_t> result) {
          pending_wait_timeout->seen =
            !result
            && result.error_code () == zlink::stream_connector::error_code_t::validation_failed;
          pending_wait_timeout->latch.signal ();
      });
    dispatch_until (connector, pending_wait_timeout->latch, std::chrono::milliseconds (100));
    if (!pending_wait_timeout->seen) {
        return fail (84);
    }

    return verify_immediate_typed_receive (context, endpoint, options, connector, runtime, request);
}

int verify_receive_count_and_close_retention ()
{
    /* stream-connector §10: a packet is counted when it arrives, in the same
   * step that makes it observable. While a wait predicate still decides
   * where the packet goes, it is neither counted nor queued; afterwards it
   * is both. */
    {
        zlink::stream_connector::connector_t count_gap_connector;
        auto state = std::static_pointer_cast<zlink::stream_connector::detail::connector_state_t> (
          connector_internal_handle (count_gap_connector));
        const auto frame_text =
          make_server_frame (zlink::stream_connector::message_kind_t::send, 0, "count.gap", "x")
            .to_string ();
        auto connection = std::make_shared<scripted_read_connection_t> (
          std::vector<std::uint8_t> (frame_text.begin (), frame_text.end ()), false);
        {
            std::lock_guard<std::mutex> lock (state->transport_mutex);
            state->connection = connection;
            state->state = zlink::stream_connector::connection_state_t::connected;
        }
        std::size_t count_while_deciding = 0;
        bool queued_while_deciding = false;
        bool predicate_ran = false;
        {
            std::lock_guard<std::mutex> lock (state->transport_mutex);
            state->pending_waits.emplace (
              1, zlink::stream_connector::detail::pending_wait_t{
                   1,
                   "count.gap",
                   [&] (const zlink::stream_connector::packet_t &) {
                       predicate_ran = true;
                       count_while_deciding = count_gap_connector.received_count ("count.gap");
                       std::lock_guard<std::mutex> queue_lock (state->transport_mutex);
                       queued_while_deciding = !state->dispatch_queue.empty ();
                       return false;
                   },
                   {}});
            ++state->pending_waits_version;
        }
        zlink::stream_connector::detail::start_read_loop (state);
        bool queued_after = false;
        {
            std::lock_guard<std::mutex> lock (state->transport_mutex);
            queued_after = state->dispatch_queue.size () == 1;
            state->pending_waits.clear ();
            ++state->pending_waits_version;
        }
        const auto count_after = count_gap_connector.received_count ("count.gap");
        connection->shutdown_and_close ();
        if (!predicate_ran
            || count_while_deciding != static_cast<std::size_t> (queued_while_deciding)) {
            return fail (351);
        }
        if (!queued_after || count_after != 1) {
            return fail (352);
        }
    }

    /* stream-connector §10: undispatched received messages are dropped only
   * when a connection is established, not at close. */
    {
        zlink::stream_connector::connector_t kept_connector;
        auto state = std::static_pointer_cast<zlink::stream_connector::detail::connector_state_t> (
          connector_internal_handle (kept_connector));
        int kept_calls = 0;
        state->packet_handlers["kept.push"].push_back (
          {1,
           [&] (const zlink::stream_connector::detail::dispatch_envelope_t &) { ++kept_calls; }});
        zlink::stream_connector::detail::connector_runtime_t::from (kept_connector)
          .receive_packet (zlink::stream_connector::packet_t{.name = "kept.push"});
        (void) kept_connector.close ();
        (void) kept_connector.dispatch ();
        if (kept_calls != 1) {
            return fail (359);
        }
    }

    return 0;
}

int verify_immediate_receive_queue ()
{
    /* stream-connector §10: in Immediate too, a received packet that no wait
   * accepts and no handler takes stays in the receive queue until a wait
   * surface takes it. The synchronous wait_for sees it whether it arrived
   * before the wait (372) or after the wait was registered (373).
   * Under SC §7 the handler returns and the test thread waits for the next packet (374). */
    {
        zlink::stream_connector::connector_options_t queue_options;
        queue_options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;
        queue_options.reconnect.enabled = false;
        queue_options.heartbeat.enabled = false;
        auto queue_connector = zlink::stream_connector::connector_factory_t::create (queue_options);
        auto state = std::static_pointer_cast<zlink::stream_connector::detail::connector_state_t> (
          connector_internal_handle (queue_connector));
        auto connection = std::make_shared<fed_read_connection_t> ();
        state->connection = connection;
        zlink::stream_connector::detail::change_state (
          state, zlink::stream_connector::connection_state_t::connected);
        zlink::stream_connector::detail::start_read_loop (state);

        connection->feed (make_server_frame (zlink::stream_connector::message_kind_t::send, 0,
                                             "unhandled.early", "early"));
        const auto early = queue_connector.wait_for ("unhandled.early", std::chrono::seconds (2));
        if (!early || as_string (early.value ().payload) != "early") {
            (void) queue_connector.close ();
            return fail (372);
        }

        std::optional<zlink::stream_connector::result_t<zlink::stream_connector::packet_t>> late;
        joining_thread_t late_waiter (
          [&] { late = queue_connector.wait_for ("unhandled.late", std::chrono::seconds (2)); });
        const auto registered_deadline =
          std::chrono::steady_clock::now () + std::chrono::seconds (2);
        bool registered = false;
        while (!registered && std::chrono::steady_clock::now () < registered_deadline) {
            {
                std::lock_guard<std::mutex> lock (state->transport_mutex);
                registered = !state->pending_waits.empty ();
            }
            if (!registered) {
                std::this_thread::yield ();
            }
        }
        connection->feed (make_server_frame (zlink::stream_connector::message_kind_t::send, 0,
                                             "unhandled.late", "late"));
        late_waiter.join ();
        if (!late || !*late || as_string (late->value ().payload) != "late") {
            (void) queue_connector.close ();
            return fail (373);
        }

        callback_latch_t trigger_done;
        std::atomic_int trigger_calls{0};
        auto trigger_subscription = queue_connector.on<zlink::stream_connector::packet_t> (
          "handled.trigger", [&] (const packet_message_t &) {
              ++trigger_calls;
              trigger_done.signal ();
          });
        connection->feed (make_server_frame (zlink::stream_connector::message_kind_t::send, 0,
                                             "handled.trigger", "go"));
        const bool trigger_finished = trigger_done.wait_for (std::chrono::seconds (5));
        std::optional<zlink::stream_connector::result_t<zlink::stream_connector::packet_t>> nested;
        joining_thread_t follow_waiter (
          [&] { nested = queue_connector.wait_for ("handled.follow", std::chrono::seconds (2)); });
        const auto follow_deadline = std::chrono::steady_clock::now () + std::chrono::seconds (2);
        bool follow_registered = false;
        while (!follow_registered && std::chrono::steady_clock::now () < follow_deadline) {
            {
                std::lock_guard<std::mutex> lock (state->transport_mutex);
                follow_registered = std::any_of (
                  state->pending_waits.begin (), state->pending_waits.end (),
                  [] (const auto &entry) { return entry.second.packet_name == "handled.follow"; });
            }
            if (!follow_registered) {
                std::this_thread::yield ();
            }
        }
        connection->feed (make_server_frame (zlink::stream_connector::message_kind_t::send, 0,
                                             "handled.follow", "follow"));
        follow_waiter.join ();
        (void) queue_connector.close ();
        if (!trigger_finished || trigger_calls.load () != 1 || !follow_registered || !nested
            || !*nested || as_string (nested->value ().payload) != "follow"
            || queue_connector.received_count ("handled.trigger") != 1
            || queue_connector.received_count ("handled.follow") != 1
            || queue_connector.pending_dispatch_count () != 0) {
            return fail (374);
        }
    }

    return 0;
}

int verify_late_receive_handler ()
{
    /* stream-connector §10: the Manual pump does not consume a packet no
   * handler takes; it stays in the queue for a wait surface. */
    {
        zlink::stream_connector::connector_options_t manual_keep_options;
        manual_keep_options.reconnect.enabled = false;
        manual_keep_options.heartbeat.enabled = false;
        auto manual_keep_connector =
          zlink::stream_connector::connector_factory_t::create (manual_keep_options);
        auto state = std::static_pointer_cast<zlink::stream_connector::detail::connector_state_t> (
          connector_internal_handle (manual_keep_connector));
        auto connection = std::make_shared<fed_read_connection_t> ();
        state->connection = connection;
        zlink::stream_connector::detail::change_state (
          state, zlink::stream_connector::connection_state_t::connected);
        zlink::stream_connector::detail::start_read_loop (state);
        int handled_calls = 0;
        auto handled_subscription = manual_keep_connector.on<zlink::stream_connector::packet_t> (
          "manual.handled", [&] (const packet_message_t &) { ++handled_calls; });
        connection->feed (make_server_frame (zlink::stream_connector::message_kind_t::send, 0,
                                             "manual.unhandled", "kept"));
        connection->feed (make_server_frame (zlink::stream_connector::message_kind_t::send, 0,
                                             "manual.handled", "run"));
        const bool dispatched = static_cast<bool> (manual_keep_connector.dispatch ());
        const auto kept =
          manual_keep_connector.wait_for ("manual.unhandled", std::chrono::milliseconds (200));
        (void) manual_keep_connector.close ();
        if (!dispatched || handled_calls != 1 || !kept
            || as_string (kept.value ().payload) != "kept") {
            return fail (378);
        }
    }

    /* stream-connector §10: a packet no handler took stays queued until a
   * handler or a wait surface takes it, so a handler registered later
   * receives it - at the next pump in Manual, at registration in Immediate. */
    for (const auto mode : {zlink::stream_connector::dispatch_mode_t::manual,
                            zlink::stream_connector::dispatch_mode_t::immediate}) {
        zlink::stream_connector::connector_options_t late_options;
        late_options.dispatch_mode = mode;
        late_options.reconnect.enabled = false;
        late_options.heartbeat.enabled = false;
        auto late_connector = zlink::stream_connector::connector_factory_t::create (late_options);
        auto state = std::static_pointer_cast<zlink::stream_connector::detail::connector_state_t> (
          connector_internal_handle (late_connector));
        auto connection = std::make_shared<fed_read_connection_t> ();
        state->connection = connection;
        zlink::stream_connector::detail::change_state (
          state, zlink::stream_connector::connection_state_t::connected);
        zlink::stream_connector::detail::start_read_loop (state);
        connection->feed (make_server_frame (zlink::stream_connector::message_kind_t::send, 0,
                                             "late.handler", "early"));
        (void) late_connector.dispatch ();
        std::string late_payload;
        callback_latch_t late_received;
        auto late_subscription = late_connector.on<zlink::stream_connector::packet_t> (
          "late.handler", [&] (const packet_message_t &message) {
              late_payload = as_string (message.payload.payload);
              late_received.signal ();
          });
        (void) late_connector.dispatch ();
        const bool received = late_received.wait_for (std::chrono::seconds (5));
        (void) late_connector.close ();
        if (!received || late_payload != "early") {
            return fail (380);
        }
    }

    return 0;
}

int verify_actor_wait_and_dispatch_order (
  const zlink::stream_connector::connector_options_t &options,
  zlink::stream_connector::connector_t &connector)
{
    /* stream-connector §10: a packet only another Actor's handle handler would
   * see stays queued, and a wait can take it. A later handler receives it. */
    {
        zlink::stream_connector::connector_options_t count_options;
        count_options.reconnect.enabled = false;
        auto count_connector = zlink::stream_connector::connector_factory_t::create (count_options);
        auto state = std::static_pointer_cast<zlink::stream_connector::detail::connector_state_t> (
          connector_internal_handle (count_connector));
        int other_actor_calls = 0;
        int own_actor_calls = 0;
        auto other_actor =
          zlink::stream_connector::detail::actor_access_t::create (state, "other-actor", 9);
        auto own_actor =
          zlink::stream_connector::detail::actor_access_t::create (state, "own-actor", 7);
        auto other_subscription = other_actor->on<zlink::stream_connector::packet_t> (
          "actor.push", [&] (const packet_message_t &) { ++other_actor_calls; });
        const auto queue_actor_push = [&] (std::string payload) {
            std::lock_guard<std::mutex> lock (state->transport_mutex);
            zlink::stream_connector::detail::enqueue_received_message (
              *state, zlink::stream_connector::detail::dispatch_envelope_t{
                        zlink::stream_connector::packet_t{.name = "actor.push",
                                                          .payload = as_bytes (payload)},
                        std::uint16_t{7}});
        };
        queue_actor_push ("first");
        (void) count_connector.dispatch ();
        {
            std::lock_guard<std::mutex> lock (state->transport_mutex);
            state->state = zlink::stream_connector::connection_state_t::connected;
            state->connection = std::make_shared<async_write_connection_t> ();
        }
        const auto kept = count_connector.wait_for ("actor.push", std::chrono::milliseconds (200));
        queue_actor_push ("second");
        auto own_first = own_actor->on<zlink::stream_connector::packet_t> (
          "actor.push", [&] (const packet_message_t &) { ++own_actor_calls; });
        auto own_second = own_actor->on<zlink::stream_connector::packet_t> (
          "actor.push", [&] (const packet_message_t &) { ++own_actor_calls; });
        (void) count_connector.dispatch ();
        (void) count_connector.dispatch ();
        (void) count_connector.close ();
        if (!kept || as_string (kept.value ().payload) != "first" || other_actor_calls != 0
            || own_actor_calls != 2) {
            return fail (382);
        }
    }

    /* §7: Manual dispatch runs packet and lifecycle deliveries in arrival
   * order, including when both were pending before the pump started. */
    {
        zlink::stream_connector::connector_options_t order_options;
        order_options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::manual;
        auto ordered = zlink::stream_connector::connector_factory_t::create (order_options);
        auto state = std::static_pointer_cast<zlink::stream_connector::detail::connector_state_t> (
          connector_internal_handle (ordered));
        std::vector<std::string> seen;
        auto packet_subscription = ordered.on<zlink::stream_connector::packet_t> (
          "arrival.packet", [&] (const packet_message_t &) { seen.push_back ("packet"); });
        {
            std::lock_guard<std::mutex> lock (state->transport_mutex);
            zlink::stream_connector::detail::enqueue_received_message (
              *state, zlink::stream_connector::detail::dispatch_envelope_t{
                        zlink::stream_connector::packet_t{.name = "arrival.packet",
                                                          .payload = as_bytes ("first")}});
        }
        zlink::stream_connector::detail::schedule_delivery (state,
                                                            [&] { seen.push_back ("lifecycle"); });
        (void) ordered.dispatch ();
        if (seen != std::vector<std::string>{"packet"}) {
            return fail (401);
        }
        (void) ordered.dispatch ();
        if (seen != std::vector<std::string>{"packet", "lifecycle"}) {
            return fail (401);
        }
    }

    /* §7/§10: a public lifecycle callback can remove the last packet handler
   * before the next packet's dispatch step. The packet remains available to a
   * wait. */
    {
        zlink::stream_connector::connector_options_t options;
        options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::manual;
        auto connector = zlink::stream_connector::connector_factory_t::create (options);
        auto state = std::static_pointer_cast<zlink::stream_connector::detail::connector_state_t> (
          connector_internal_handle (connector));
        auto packet_subscription = connector.on<zlink::stream_connector::packet_t> (
          "removed.before.dispatch", [] (const packet_message_t &) {});
        bool removed = false;
        auto state_subscription = connector.on_connection_state_changed ([&] (const auto &change) {
            if (change.current == zlink::stream_connector::connection_state_t::connected) {
                packet_subscription.unsubscribe ();
                removed = true;
            }
        });
        state->connection = std::make_shared<async_write_connection_t> ();
        zlink::stream_connector::detail::change_state (
          state, zlink::stream_connector::connection_state_t::connected);
        {
            std::lock_guard<std::mutex> lock (state->transport_mutex);
            zlink::stream_connector::detail::enqueue_received_message (
              *state, zlink::stream_connector::detail::dispatch_envelope_t{
                        zlink::stream_connector::packet_t{.name = "removed.before.dispatch",
                                                          .payload = as_bytes ("kept")}});
        }
        (void) connector.dispatch ();
        const auto kept =
          connector.wait_for ("removed.before.dispatch", std::chrono::milliseconds (0));
        (void) connector.close ();
        if (!removed || !kept || as_string (kept.value ().payload) != "kept") {
            return fail (403);
        }
    }

    return 0;
}

int verify_late_manual_handler (const zlink::stream_connector::connector_options_t &options,
                                zlink::stream_connector::connector_t &connector)
{
    /* §10: a handler registered by a later callback receives an earlier
     * handlerless packet on the next Manual pump. */
    {
        zlink::stream_connector::connector_options_t options;
        options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::manual;
        auto connector = zlink::stream_connector::connector_factory_t::create (options);
        auto state = std::static_pointer_cast<zlink::stream_connector::detail::connector_state_t> (
          connector_internal_handle (connector));
        {
            std::lock_guard<std::mutex> lock (state->transport_mutex);
            zlink::stream_connector::detail::enqueue_received_message (
              *state, zlink::stream_connector::detail::dispatch_envelope_t{
                        zlink::stream_connector::packet_t{.name = "registered.in.callback"}});
        }
        int calls = 0;
        std::optional<zlink::stream_connector::subscription_t> packet_subscription;
        zlink::stream_connector::detail::schedule_delivery (state, [&] {
            packet_subscription.emplace (connector.on<zlink::stream_connector::packet_t> (
              "registered.in.callback", [&] (const packet_message_t &) { ++calls; }));
        });
        (void) connector.dispatch ();
        const bool deferred = calls == 0;
        (void) connector.dispatch ();
        (void) connector.close ();
        if (!deferred || calls != 1) {
            return fail (405);
        }
    }

    return 0;
}

int verify_arrival_count ()
{
    /* stream-connector §10: received_count counts arrivals, is not lowered by
   * consumption and ignores the dispatch mode. */
    {
        for (const auto mode : {zlink::stream_connector::dispatch_mode_t::manual,
                                zlink::stream_connector::dispatch_mode_t::immediate}) {
            zlink::stream_connector::connector_options_t count_options;
            count_options.dispatch_mode = mode;
            auto count_connector =
              zlink::stream_connector::connector_factory_t::create (count_options);
            auto count_runtime =
              zlink::stream_connector::detail::connector_runtime_t::from (count_connector);
            if (count_connector.received_count ("count.push") != 0) {
                return fail (232);
            }
            std::atomic_int dispatched{0};
            auto count_subscription = count_connector.on<zlink::stream_connector::packet_t> (
              "count.push", [&] (const packet_message_t &) { ++dispatched; });
            for (int index = 0; index < 3; ++index) {
                count_runtime.receive_packet (
                  zlink::stream_connector::packet_t{"count.push",
                                                    {},
                                                    zlink::stream_connector::codec_t::raw,
                                                    false,
                                                    as_bytes (std::string ("payload"))});
            }
            if (count_connector.received_count ("count.push") != 3
                || count_connector.received_count ("count.other") != 0) {
                return fail (233);
            }
            if (mode == zlink::stream_connector::dispatch_mode_t::manual) {
                while (count_connector.pending_dispatch_count () != 0) {
                    if (!count_connector.dispatch ()) {
                        return fail (234);
                    }
                }
            } else {
                const auto dispatch_deadline =
                  std::chrono::steady_clock::now () + std::chrono::seconds (2);
                while (dispatched.load () != 3
                       && std::chrono::steady_clock::now () < dispatch_deadline) {
                    std::this_thread::sleep_for (std::chrono::milliseconds (1));
                }
                if (dispatched.load () != 3) {
                    return fail (235);
                }
            }
            /* Consuming does not lower the count. */
            if (count_connector.received_count ("count.push") != 3) {
                return fail (236);
            }
            count_connector.close ();
        }
    }

    return 0;
}

int verify_unlocked_receive_predicate (
  zlink::stream_connector::detail::connector_runtime_t &runtime)
{
    /* Issue #583: no user callable runs under transport_mutex.
   *
   * std::mutex is not recursive, so a wait predicate that calls back into the
   * connector surface used to deadlock against the very lock its caller held.
   * .NET's lock and Java's synchronized are reentrant and let the same code
   * through, so the defect was C++-only. Each case below calls
   * pending_dispatch_count() - which takes transport_mutex - from inside the
   * predicate. A regression hangs the process, and the ctest timeout reports
   * it. */
    {
        using zlink::stream_connector::connector_options_t;
        using zlink::stream_connector::dispatch_mode_t;
        using zlink::stream_connector::packet_t;

        const auto push = [] (zlink::stream_connector::detail::connector_runtime_t &runtime,
                              const std::string &name, const std::string &payload) {
            runtime.receive_packet (packet_t{name,
                                             {},
                                             zlink::stream_connector::codec_t::raw,
                                             false,
                                             as_bytes (std::string (payload))});
        };

        /* The synchronous wait scans the queue with the caller's predicate. */
        {
            connector_options_t reentrant_options;
            reentrant_options.dispatch_mode = dispatch_mode_t::manual;
            auto reentrant =
              zlink::stream_connector::connector_factory_t::create (reentrant_options);
            auto reentrant_runtime =
              zlink::stream_connector::detail::connector_runtime_t::from (reentrant);
            push (reentrant_runtime, "reentrant.wait", "first");
            push (reentrant_runtime, "reentrant.wait", "second");
            int predicate_calls = 0;
            int reentrant_calls_returned = 0;
            auto matched =
              reentrant.wait_for<packet_t> ("reentrant.wait", std::chrono::milliseconds (500))
                .where ([&] (const packet_message_t &message) {
                    ++predicate_calls;
                    /* The call that used to deadlock. The value is not
                 * asserted: the scan holds the candidates outside the
                 * queue while it runs, exactly as dispatch() does. */
                    (void) reentrant.pending_dispatch_count ();
                    ++reentrant_calls_returned;
                    return as_string (message.payload.payload) == "second";
                })
                .submit ();
            if (!matched || as_string (matched.value ().payload.payload) != "second") {
                return fail (250);
            }
            /* Both queued packets were offered; the one the predicate rejected
       * stayed queued in its original position, which the next wait shows.
       * It is not pending dispatch: no handler takes it (§7). */
            if (predicate_calls != 2 || reentrant_calls_returned != 2
                || reentrant.pending_dispatch_count () != 0) {
                return fail (251);
            }
            auto leftover =
              reentrant.wait_for<packet_t> ("reentrant.wait", std::chrono::milliseconds (500))
                .submit ();
            if (!leftover || as_string (leftover.value ().payload.payload) != "first"
                || reentrant.pending_dispatch_count () != 0) {
                return fail (252);
            }
            reentrant.close ();
        }

        /* A wait registered before the packet arrives is matched by dispatch(),
     * which runs the predicate from the dispatching thread. */
        {
            connector_options_t registered_options;
            registered_options.dispatch_mode = dispatch_mode_t::manual;
            auto registered =
              zlink::stream_connector::connector_factory_t::create (registered_options);
            auto registered_runtime =
              zlink::stream_connector::detail::connector_runtime_t::from (registered);
            std::atomic_int predicate_calls{0};
            // The completion callback may run after this scope ended (early return).
            auto registered_promise = std::make_shared<std::promise<std::string>> ();
            auto registered_future = registered_promise->get_future ();
            registered.wait_for<packet_t> ("registered.wait", std::chrono::seconds (5))
              .where ([&] (const packet_message_t &message) {
                  ++predicate_calls;
                  (void) registered.pending_dispatch_count ();
                  return as_string (message.payload.payload) == "wanted";
              })
              .submit ([registered_promise] (
                         zlink::stream_connector::result_t<packet_message_t> result) {
                  registered_promise->set_value (
                    result ? as_string (result.value ().payload.payload) : std::string ("<none>"));
              });
            push (registered_runtime, "registered.wait", "unwanted");
            push (registered_runtime, "registered.wait", "wanted");
            const auto registered_deadline =
              std::chrono::steady_clock::now () + std::chrono::seconds (5);
            while (registered_future.wait_for (std::chrono::milliseconds (0))
                     != std::future_status::ready
                   && std::chrono::steady_clock::now () < registered_deadline) {
                if (!registered.dispatch ()) {
                    return fail (253);
                }
                std::this_thread::sleep_for (std::chrono::milliseconds (1));
            }
            if (registered_future.wait_for (std::chrono::milliseconds (0))
                != std::future_status::ready) {
                return fail (254);
            }
            if (registered_future.get () != "wanted" || predicate_calls.load () < 2) {
                return fail (255);
            }
            registered.close ();
        }

        /* Issue #583: connector_runtime_t::receive_packet reaches
     * dispatch_queue from the caller's thread. Concurrent injections must
     * not lose or duplicate a message. */
        {
            connector_options_t concurrent_options;
            concurrent_options.dispatch_mode = dispatch_mode_t::manual;
            auto concurrent =
              zlink::stream_connector::connector_factory_t::create (concurrent_options);
            auto concurrent_runtime =
              zlink::stream_connector::detail::connector_runtime_t::from (concurrent);
            /* Pending dispatch observes the queued packets after concurrent
       * injection. */
            auto concurrent_subscription =
              concurrent.on<packet_t> ("concurrent.push", [] (const packet_message_t &) {});
            constexpr int producer_count = 4;
            constexpr int per_producer = 64;
            std::vector<std::thread> producers;
            producers.reserve (producer_count);
            for (int producer = 0; producer < producer_count; ++producer) {
                producers.emplace_back ([&concurrent_runtime, &push] {
                    for (int index = 0; index < per_producer; ++index) {
                        push (concurrent_runtime, "concurrent.push", "payload");
                    }
                });
            }
            for (auto &producer : producers) {
                producer.join ();
            }
            if (concurrent.pending_dispatch_count ()
                  != static_cast<std::size_t> (producer_count * per_producer)
                || concurrent.received_count ("concurrent.push")
                     != static_cast<std::size_t> (producer_count * per_producer)) {
                return fail (246);
            }
            concurrent.close ();
        }
    }

    return 0;
}
} // namespace stream_connector_tests
