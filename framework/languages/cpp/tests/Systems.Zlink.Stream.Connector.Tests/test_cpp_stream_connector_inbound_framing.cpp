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
#include <boost/beast/core/buffers_to_string.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/websocket.hpp>
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
#include "../support/stream_connector_typed_messages.hpp"
#include "../support/stream_connector_scripted_transport.hpp"
#include "../support/stream_connector_callback_sync.hpp"
#include "../support/stream_connector_failure.hpp"

using namespace stream_connector_fixture;

namespace
{
zlink::message_t make_frame_prefix (std::size_t header_size, std::size_t payload_size)
{
    auto prefix =
      zlink::stream_connector::detail::frame_codec_t::encode_prefix (header_size, payload_size);
    return zlink::message_t::from (std::string (prefix.value ().begin (), prefix.value ().end ()));
}
} // namespace

using zlink::stream_connector::codec_t;
using zlink::stream_connector::header_flags_t;
using zlink::stream_connector::message_kind_t;
using zlink::stream_connector::detail::header_codec_t;
using zlink::stream_connector::detail::stream_header_t;

namespace stream_connector_tests
{
int verify_inbound_framing ()
{
    // stream-connector §4.7: retain an incomplete frame after a multi-frame read.
    for (const bool split_payload : {false, true}) {
        using namespace zlink::stream_connector;
        connector_options_t options;
        options.dispatch_mode = dispatch_mode_t::manual;
        options.heartbeat.enabled = false;
        options.reconnect.enabled = false;
        auto connector = connector_factory_t::create (options);
        auto state = std::static_pointer_cast<detail::connector_state_t> (
          connector_internal_handle (connector));
        auto connection = std::make_shared<fed_read_connection_t> ();
        state->connection = connection;
        detail::change_state (state, connection_state_t::connected);
        detail::start_read_loop (state);
        const auto first =
          make_server_frame (message_kind_t::send, 0, "batch", "first").to_string ();
        const auto second =
          make_server_frame (message_kind_t::send, 0, "batch", "second").to_string ();
        const auto third =
          make_server_frame (message_kind_t::send, 0, "batch", "third").to_string ();
        const auto split = split_payload ? third.size () - 2 : std::size_t{3};
        connection->feed (zlink::message_t::from (first + second + third.substr (0, split)));
        if (!eventually ([&] {
                std::lock_guard lock (state->transport_mutex);
                return state->dispatch_queue.size () == 2;
            })) {
            (void) connector.close ();
            return fail (408);
        }
        bool retained;
        {
            std::lock_guard lock (state->transport_mutex);
            retained = std::string (state->inbound_buffer.begin (), state->inbound_buffer.end ())
                       == third.substr (0, split);
        }
        connection->feed (zlink::message_t::from (third.substr (split)));
        const bool completed = eventually ([&] {
            std::lock_guard lock (state->transport_mutex);
            return state->dispatch_queue.size () == 3;
        });
        bool ordered = false;
        if (completed) {
            std::lock_guard lock (state->transport_mutex);
            ordered = state->inbound_buffer.empty ()
                      && as_string (state->dispatch_queue[0].packet.payload) == "first"
                      && as_string (state->dispatch_queue[1].packet.payload) == "second"
                      && as_string (state->dispatch_queue[2].packet.payload) == "third";
        }
        (void) connector.close ();
        if (!retained || !completed || !ordered)
            return fail (409);
    }
    /* stream-connector §4.5: 진단 바이트는 엄격한 UTF-8이다.
     * 잘못된 서버 제어 프레임의 결과를 공개 연결 종료 이벤트로 관찰한다. */
    for (const auto &[diagnostic, valid] :
         std::vector<std::pair<std::string, bool>>{{"valid", true},
                                                   {std::string ("\xed\x9f\xbf", 3), true},
                                                   {std::string ("\xee\x80\x80", 3), true},
                                                   {std::string ("\xf4\x8f\xbf\xbf", 4), true},
                                                   {std::string ("\xe0\x80\x80", 3), false},
                                                   {std::string ("\xed\xa0\x80", 3), false},
                                                   {std::string ("\xf4\x90\x80\x80", 4), false}}) {
        boost::asio::io_context closing_io;
        boost::asio::ip::tcp::acceptor closing_acceptor (
          closing_io, {boost::asio::ip::make_address ("127.0.0.1"), 0});
        callback_latch_t connected;
        callback_latch_t release_server;
        callback_latch_t disconnected;
        joining_thread_t closing_server ([&] {
            boost::asio::ip::tcp::socket accepted (closing_acceptor.get_executor ());
            closing_acceptor.accept (accepted);
            if (connected.wait_for (std::chrono::seconds (5))) {
                std::string payload;
                payload.push_back (1);
                payload.push_back (
                  static_cast<char> (zlink::stream_connector::close_reason_t::server_drain));
                payload.push_back (0);
                payload.push_back (static_cast<char> (diagnostic.size ()));
                payload += diagnostic;
                const auto frame = make_server_frame (
                  zlink::stream_connector::message_kind_t::control, 0, "session-closing", payload);
                boost::system::error_code write_error;
                boost::asio::write (accepted, boost::asio::buffer (frame.to_string ()),
                                    write_error);
                accepted.shutdown (boost::asio::ip::tcp::socket::shutdown_both, write_error);
                accepted.close (write_error);
            }
            (void) release_server.wait_for (std::chrono::seconds (5));
        });
        zlink::stream_connector::connector_options_t options;
        options.endpoint =
          "tcp://127.0.0.1:" + std::to_string (closing_acceptor.local_endpoint ().port ());
        options.heartbeat.enabled = false;
        options.reconnect.enabled = false;
        options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;
        auto connector = zlink::stream_connector::connector_factory_t::create (options);
        std::optional<zlink::stream_connector::close_reason_t> observed_reason;
        auto subscription = connector.on_disconnected ([&] (auto reason) {
            observed_reason = reason;
            disconnected.signal ();
        });
        const bool did_connect = static_cast<bool> (connector.connect ());
        connected.signal ();
        const bool did_disconnect = did_connect && disconnected.wait_for (std::chrono::seconds (5));
        release_server.signal ();
        closing_server.join ();
        // §7: close does not wait for callbacks; end this observation before cleanup.
        subscription.unsubscribe ();
        (void) connector.close ();
        const auto expected = valid ? zlink::stream_connector::close_reason_t::server_drain
                                    : zlink::stream_connector::close_reason_t::protocol_error;
        if (!did_disconnect || observed_reason != expected) {
            std::cerr << "session-closing UTF-8 " << (valid ? "valid" : "invalid")
                      << " diagnostic check failed\n";
            return fail (407);
        }
    }

    using zlink::stream_connector::codec_t;
    using zlink::stream_connector::header_flags_t;
    using zlink::stream_connector::message_kind_t;
    using zlink::stream_connector::detail::header_codec_t;
    using zlink::stream_connector::detail::stream_header_t;

    return 0;
}

int verify_read_terminal_order ()
{
    {
        auto state = std::make_shared<zlink::stream_connector::detail::connector_state_t> (
          zlink::stream_connector::connector_options_t{});
        std::optional<zlink::stream_connector::error_code_t> callback_error;
        state->packet_handlers["throwing.handler"].push_back (
          {1, [] (const zlink::stream_connector::detail::dispatch_envelope_t &) {
               throw std::runtime_error ("handler failed");
           }});
        state->error_handlers.push_back ({2, [&] (const zlink::stream_connector::error_t &error) {
                                              callback_error = error.code;
                                          }});
        const zlink::stream_connector::detail::dispatch_envelope_t envelope{
          zlink::stream_connector::packet_t{.name = "throwing.handler"}, std::nullopt};
        auto selected = zlink::stream_connector::detail::select_packet_handlers (*state, envelope);
        zlink::stream_connector::detail::dispatch_packet (*state, envelope, selected);
        (void) zlink::stream_connector::detail::dispatch_pending (state);
        if (callback_error != zlink::stream_connector::error_code_t::user_callback_failed) {
            return fail (319);
        }
    }

    {
        /* A read that returns bytes together with EOF delivers the frame
     * those bytes complete before the connection ends. */
        auto frame = make_server_frame (zlink::stream_connector::message_kind_t::send, 0,
                                        "server.before.eof", "packet")
                       .to_string ();
        auto connection = std::make_shared<scripted_read_connection_t> (
          std::vector<std::uint8_t> (frame.begin (), frame.end ()), true);
        zlink::stream_connector::connector_options_t eof_options;
        eof_options.reconnect.enabled = false;
        auto state =
          std::make_shared<zlink::stream_connector::detail::connector_state_t> (eof_options);
        state->connection = connection;
        state->state = zlink::stream_connector::connection_state_t::connected;
        zlink::stream_connector::detail::start_read_loop (state);
        bool delivered_before_end = false;
        {
            std::lock_guard<std::mutex> lock (state->transport_mutex);
            delivered_before_end =
              state->dispatch_queue.size () == 1
              && state->dispatch_queue.front ().packet.name == "server.before.eof"
              && state->state != zlink::stream_connector::connection_state_t::connected;
        }
        if (!delivered_before_end) {
            return fail (318);
        }
    }

    /* stream-connector §5.2: request_seq correlation decides every terminal.
   * A matched Response completes its request, an unmatched one is
   * discarded, and neither becomes a push - also while a Manual pump,
   * receive or wait is running. Only the read pump reads the socket. */
    {
        using zlink::stream_connector::detail::connector_state_t;
        const auto terminal_text =
          make_server_frame (zlink::stream_connector::message_kind_t::response, 7, "", "matched")
            .to_string ()
          + make_server_frame (zlink::stream_connector::message_kind_t::response, 998, "", "late")
              .to_string ();
        const std::vector<std::uint8_t> terminal_bytes (terminal_text.begin (),
                                                        terminal_text.end ());
        int pushed_paths = 0;
        for (const int path : {0, 2}) {
            auto state =
              std::make_shared<connector_state_t> (zlink::stream_connector::connector_options_t{});
            auto connection = std::make_shared<scripted_read_connection_t> (terminal_bytes, false);
            state->connection = connection;
            state->state = zlink::stream_connector::connection_state_t::connected;
            bool request_completed = false;
            bool request_succeeded = false;
            state->pending_requests.emplace (
              7, zlink::stream_connector::detail::pending_request_t{
                   7, zlink::stream_connector::packet_t{.name = "terminal.request"},
                   [&] (zlink::stream_connector::result_t<
                        zlink::stream_connector::detail::request_reply_t> result) {
                       request_completed = true;
                       request_succeeded = static_cast<bool> (result);
                   },
                   nullptr, /*deliver_direct=*/true, nullptr});
            bool pushed = false;
            state->packet_handlers[""].push_back (
              {1, [&] (const zlink::stream_connector::detail::dispatch_envelope_t &) {
                   pushed = true;
               }});
            if (path == 0) {
                (void) zlink::stream_connector::detail::dispatch_pending (state);
            } else {
                pushed = pushed
                         || static_cast<bool> (zlink::stream_connector::detail::wait_for_packet (
                           state, "", {}, std::chrono::milliseconds (20)));
            }
            {
                std::lock_guard<std::mutex> lock (state->transport_mutex);
                pushed = pushed || !state->dispatch_queue.empty ();
            }
            if (pushed) {
                connection->shutdown_and_close ();
                pushed_paths |= 1 << path;
                continue;
            }
            zlink::stream_connector::detail::start_read_loop (state);
            bool queue_empty = false;
            {
                std::lock_guard<std::mutex> lock (state->transport_mutex);
                queue_empty = state->dispatch_queue.empty ();
            }
            connection->shutdown_and_close ();
            if (!request_completed || !request_succeeded || !queue_empty) {
                return fail (333);
            }
        }
        if (pushed_paths != 0) {
            return fail (340 + pushed_paths);
        }
    }

    return 0;
}

int verify_protocol_close ()
{
    /* stream-connector §9 (spec D): a frame or header the connector cannot
   * decode and a frame over the receive limit end the connection with the
   * close reason ProtocolError; a transport read failure leaves
   * TransportError. */
    for (int ending = 0; ending < 3; ++ending) {
        zlink::stream_connector::connector_options_t ending_options;
        ending_options.reconnect.enabled = false;
        ending_options.heartbeat.enabled = false;
        ending_options.max_receive_payload_size = 16;
        auto ending_connector =
          zlink::stream_connector::connector_factory_t::create (ending_options);
        auto state = std::static_pointer_cast<zlink::stream_connector::detail::connector_state_t> (
          connector_internal_handle (ending_connector));
        auto connection = std::make_shared<fed_read_connection_t> ();
        state->connection = connection;
        zlink::stream_connector::detail::change_state (
          state, zlink::stream_connector::connection_state_t::connected);
        zlink::stream_connector::detail::start_read_loop (state);
        /* Spec F: operations the ending fails see Disconnected whatever the cause.
     */
        std::optional<zlink::stream_connector::error_code_t> ending_request_code;
        zlink::stream_connector::detail::submit_request_async (
          state,
          zlink::stream_connector::packet_t{.name = "ending.request", .payload = as_bytes ("q")},
          std::chrono::seconds (5),
          [&] (zlink::stream_connector::result_t<zlink::stream_connector::detail::request_reply_t>
                 result) { ending_request_code = result.error_code (); },
          /*deliver_direct=*/true);
        std::promise<std::optional<zlink::stream_connector::error_code_t>> ending_wait_code;
        auto ending_wait_future = ending_wait_code.get_future ();
        zlink::stream_connector::detail::submit_wait_async (
          state, "ending.wait", {}, std::chrono::seconds (5),
          [&] (zlink::stream_connector::result_t<zlink::stream_connector::packet_t> result) {
              ending_wait_code.set_value (result.error_code ());
          });
        auto expected = zlink::stream_connector::close_reason_t::protocol_error;
        if (ending == 0) {
            connection->feed (zlink::message_t::from (make_frame_prefix (3, 0).to_string ()
                                                      + std::string ("\xff\xff\xff")));
        } else if (ending == 1) {
            connection->feed (make_frame_prefix (0, 17));
        } else {
            connection->end ();
            expected = zlink::stream_connector::close_reason_t::transport_error;
        }
        const auto ended = ending_connector.state ();
        const auto reason = ending_connector.close_reason ();
        const bool wait_settled =
          ending_wait_future.wait_for (std::chrono::seconds (5)) == std::future_status::ready;
        (void) ending_connector.close ();
        if (ended != zlink::stream_connector::connection_state_t::disconnected
            || reason != expected) {
            return fail (375);
        }
        if (ending_request_code != zlink::stream_connector::error_code_t::disconnected
            || !wait_settled
            || ending_wait_future.get () != zlink::stream_connector::error_code_t::disconnected) {
            return fail (379);
        }
    }

    /* The same rule over WebSocket: a message over the receive limit is
   * FrameTooLarge and ends the connection as ProtocolError. */
    {
        boost::asio::io_context oversized_ws_io;
        boost::asio::ip::tcp::acceptor oversized_ws_acceptor (
          oversized_ws_io, {boost::asio::ip::make_address ("127.0.0.1"), 0});
        const auto oversized_ws_endpoint =
          std::string ("ws://127.0.0.1:")
          + std::to_string (oversized_ws_acceptor.local_endpoint ().port ()) + "/stream";
        callback_latch_t oversized_ws_release;
        joining_thread_t oversized_ws_server ([&] {
            boost::asio::ip::tcp::socket socket (oversized_ws_acceptor.get_executor ());
            oversized_ws_acceptor.accept (socket);
            boost::beast::websocket::stream<boost::asio::ip::tcp::socket> websocket (
              std::move (socket));
            websocket.accept ();
            websocket.binary (true);
            const std::string oversized (70000, 'x');
            boost::system::error_code ignored;
            websocket.write (boost::asio::buffer (oversized), ignored);
            (void) oversized_ws_release.wait_for (std::chrono::seconds (5));
        });
        zlink::stream_connector::connector_options_t oversized_ws_options;
        oversized_ws_options.endpoint = oversized_ws_endpoint;
        oversized_ws_options.transport = zlink::stream_connector::transport_t::websocket;
        oversized_ws_options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;
        oversized_ws_options.reconnect.enabled = false;
        oversized_ws_options.heartbeat.enabled = false;
        oversized_ws_options.max_receive_payload_size = 16;
        auto oversized_ws_connector =
          zlink::stream_connector::connector_factory_t::create (oversized_ws_options);
        std::atomic<int> oversized_ws_error{-1};
        callback_latch_t oversized_ws_disconnected;
        auto oversized_ws_error_subscription =
          oversized_ws_connector.on_error ([&] (const zlink::stream_connector::error_t &error) {
              oversized_ws_error = static_cast<int> (error.code);
          });
        auto oversized_ws_disconnect_subscription = oversized_ws_connector.on_disconnected (
          [&] (std::optional<zlink::stream_connector::close_reason_t>) {
              oversized_ws_disconnected.signal ();
          });
        const bool oversized_ws_connected = static_cast<bool> (oversized_ws_connector.connect ());
        const bool oversized_ws_ended =
          oversized_ws_connected && oversized_ws_disconnected.wait_for (std::chrono::seconds (5));
        const auto oversized_ws_reason = oversized_ws_connector.close_reason ();
        (void) oversized_ws_connector.close ();
        oversized_ws_release.signal ();
        oversized_ws_server.join ();
        if (!oversized_ws_ended
            || oversized_ws_reason != zlink::stream_connector::close_reason_t::protocol_error
            || oversized_ws_error.load ()
                 != static_cast<int> (zlink::stream_connector::error_code_t::frame_too_large)) {
            return fail (376);
        }
    }

    return 0;
}

int verify_receive_frame_limits (
  zlink::context_t &context,
  zlink::stream_connector::connector_t &connector,
  zlink::stream_connector::detail::connector_runtime_t &runtime,
  zlink::stream_connector::result_t<stream_connector_fixture::login_reply_t> &request)
{
    zlink::stream_socket_t large_receive_server (context);
    large_receive_server.options ().recv_mode (zlink::stream_recv_mode_t::raw);
    large_receive_server.options ().notify (false);
    large_receive_server.bind ("tcp://127.0.0.1:0");
    const auto large_receive_endpoint = large_receive_server.options ().last_endpoint ();
    const std::string large_receive_payload (70 * 1024, 'l');
    joining_thread_t large_receive_server_thread ([&large_receive_server, &large_receive_payload] {
        zlink::received_t inbound;
        if (large_receive_server.recv (inbound) != 0) {
            return;
        }
        auto frame = make_server_frame (zlink::stream_connector::message_kind_t::send, 0,
                                        "server.large", large_receive_payload);
        inbound.send ().message (frame).submit ();
        inbound.close ();
    });
    zlink::stream_connector::connector_options_t large_receive_options;
    large_receive_options.endpoint = large_receive_endpoint;
    large_receive_options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::manual;
    large_receive_options.max_send_payload_size = 16;
    large_receive_options.max_receive_payload_size = 80 * 1024;
    auto large_receive_connector =
      zlink::stream_connector::connector_factory_t::create (large_receive_options);
    if (!large_receive_connector.connect ()) {
        return fail (70);
    }
    large_receive_connector.send (login_request_t{})
      .packet_name ("large.receive.trigger")
      .submit ();
    auto large_received =
      large_receive_connector.wait_for ("server.large", std::chrono::milliseconds (100));
    large_receive_server_thread.join ();
    if (!large_received || large_received.value ().name != "server.large"
        || as_string (large_received.value ().payload).size () != large_receive_payload.size ()) {
        return fail (72);
    }
    large_receive_connector.close ();

    zlink::stream_socket_t oversized_receive_server (context);
    oversized_receive_server.options ().recv_mode (zlink::stream_recv_mode_t::raw);
    oversized_receive_server.options ().notify (false);
    oversized_receive_server.bind ("tcp://127.0.0.1:0");
    const auto oversized_receive_endpoint = oversized_receive_server.options ().last_endpoint ();
    joining_thread_t oversized_receive_server_thread ([&oversized_receive_server] {
        zlink::received_t inbound;
        if (oversized_receive_server.recv (inbound) != 0) {
            return;
        }
        inbound.send ().message (make_frame_prefix (0, 17)).submit ();
        // Allow the frame to reach the connector before closing the test peer.
        std::this_thread::sleep_for (std::chrono::milliseconds (50));
        inbound.close ();
    });
    zlink::stream_connector::connector_options_t oversized_receive_options;
    oversized_receive_options.endpoint = oversized_receive_endpoint;
    oversized_receive_options.max_receive_payload_size = 16;
    auto oversized_receive_connector =
      zlink::stream_connector::connector_factory_t::create (oversized_receive_options);
    if (!oversized_receive_connector.connect ()) {
        return fail (120);
    }
    auto oversized_receive_result = oversized_receive_connector.request (login_request_t{})
                                      .packet_name ("oversized.receive.request")
                                      .timeout (std::chrono::milliseconds (100))
                                      .submit<login_reply_t> ();
    oversized_receive_server_thread.join ();
    /* Spec F (§9): the request the ending fails sees Disconnected; FrameTooLarge
   * stays the cause, in the close reason and the error handler. */
    if (oversized_receive_result
        || oversized_receive_result.error_code ()
             != zlink::stream_connector::error_code_t::disconnected) {
        return fail (121);
    }
    oversized_receive_connector.close ();

    zlink::stream_socket_t async_oversized_receive_server (context);
    async_oversized_receive_server.options ().recv_mode (zlink::stream_recv_mode_t::raw);
    async_oversized_receive_server.options ().notify (false);
    async_oversized_receive_server.bind ("tcp://127.0.0.1:0");
    const auto async_oversized_receive_endpoint =
      async_oversized_receive_server.options ().last_endpoint ();
    joining_thread_t async_oversized_receive_server_thread ([&async_oversized_receive_server] {
        zlink::received_t inbound;
        if (async_oversized_receive_server.recv (inbound) != 0) {
            return;
        }
        inbound.send ().message (make_frame_prefix (0, 17)).submit ();
        inbound.close ();
    });
    zlink::stream_connector::connector_options_t async_oversized_receive_options;
    async_oversized_receive_options.endpoint = async_oversized_receive_endpoint;
    async_oversized_receive_options.max_receive_payload_size = 16;
    async_oversized_receive_options.dispatch_mode =
      zlink::stream_connector::dispatch_mode_t::immediate;
    auto async_oversized_receive_connector =
      zlink::stream_connector::connector_factory_t::create (async_oversized_receive_options);
    if (!async_oversized_receive_connector.connect ()) {
        return fail (122);
    }
    auto async_oversized_receive = std::make_shared<callback_outcome_t> ();
    async_oversized_receive_connector.request (login_request_t{})
      .packet_name ("async.oversized.receive.request")
      .timeout (std::chrono::milliseconds (100))
      .submit<login_reply_t> ([async_oversized_receive] (
                                zlink::stream_connector::result_t<login_reply_t> result) {
          async_oversized_receive->seen =
            !result && result.error_code () == zlink::stream_connector::error_code_t::disconnected;
          async_oversized_receive->latch.signal ();
      });
    async_oversized_receive_server_thread.join ();
    async_oversized_receive->latch.wait_for (std::chrono::milliseconds (100));
    if (!async_oversized_receive->seen) {
        return fail (123);
    }
    async_oversized_receive_connector.close ();

    zlink::stream_socket_t oversized_wait_server (context);
    oversized_wait_server.options ().recv_mode (zlink::stream_recv_mode_t::raw);
    oversized_wait_server.options ().notify (false);
    oversized_wait_server.bind ("tcp://127.0.0.1:0");
    const auto oversized_wait_endpoint = oversized_wait_server.options ().last_endpoint ();
    std::atomic_bool oversized_wait_release{false};
    joining_thread_t oversized_wait_server_thread (
      [&oversized_wait_server, &oversized_wait_release] {
          zlink::received_t inbound;
          if (oversized_wait_server.recv (inbound) != 0) {
              return;
          }
          inbound.send ().message (make_frame_prefix (0, 17)).submit ();
          const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (3);
          while (!oversized_wait_release && std::chrono::steady_clock::now () < deadline) {
              std::this_thread::sleep_for (std::chrono::milliseconds (1));
          }
          inbound.close ();
      });
    zlink::stream_connector::connector_options_t oversized_wait_options;
    oversized_wait_options.endpoint = oversized_wait_endpoint;
    oversized_wait_options.max_receive_payload_size = 16;
    auto oversized_wait_connector =
      zlink::stream_connector::connector_factory_t::create (oversized_wait_options);
    std::atomic_bool oversized_wait_frame_error_published{false};
    auto oversized_wait_error_subscription = oversized_wait_connector.on_error (
      [&oversized_wait_frame_error_published] (const zlink::stream_connector::error_t &error) {
          if (error.code == zlink::stream_connector::error_code_t::frame_too_large) {
              oversized_wait_frame_error_published = true;
          }
      });
    if (!oversized_wait_connector.connect ()) {
        return fail (124);
    }
    oversized_wait_connector.send (login_request_t{})
      .packet_name ("oversized.wait.trigger")
      .submit ();
    auto oversized_wait_result =
      oversized_wait_connector.wait_for ("oversized.wait", std::chrono::milliseconds (1500));
    oversized_wait_release = true;
    oversized_wait_server_thread.join ();
    /* stream-connector §9: frame_too_large ends the connection and reaches the
   * error handler; §10.1.1: the wait that lost its connection ends as
   * disconnected, not with the cause. */
    if (oversized_wait_result
        || oversized_wait_result.error_code ()
             != zlink::stream_connector::error_code_t::disconnected) {
        return fail (126);
    }
    {
        const auto published_deadline =
          std::chrono::steady_clock::now () + std::chrono::milliseconds (1000);
        while (!oversized_wait_frame_error_published
               && std::chrono::steady_clock::now () < published_deadline) {
            /* Manual dispatch: the error handler runs on the pump. */
            (void) oversized_wait_connector.dispatch ();
            std::this_thread::sleep_for (std::chrono::milliseconds (1));
        }
        if (!oversized_wait_frame_error_published) {
            return fail (127);
        }
    }
    oversized_wait_connector.close ();

    return verify_heartbeat (context, connector, runtime, request);
}
} // namespace stream_connector_tests
