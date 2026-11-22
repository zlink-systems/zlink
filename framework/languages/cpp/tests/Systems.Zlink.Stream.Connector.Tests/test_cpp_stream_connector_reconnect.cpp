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

#include "../support/stream_connector_wire_frames.hpp"
#include "../support/stream_connector_typed_messages.hpp"
#include "../support/stream_connector_callback_sync.hpp"
#include "../support/stream_connector_failure.hpp"

using namespace stream_connector_fixture;


namespace stream_connector_tests
{
int verify_reconnect_and_endpoint_scheme (
  zlink::context_t &context,
  zlink::stream_connector::connector_t &connector,
  zlink::stream_connector::detail::connector_runtime_t &runtime,
  const std::string &websocket_endpoint)
{
    boost::asio::io_context reconnect_success_io;
    boost::asio::ip::tcp::acceptor reserved_reconnect_acceptor (reconnect_success_io);
    reserved_reconnect_acceptor.open (boost::asio::ip::tcp::v4 ());
    reserved_reconnect_acceptor.set_option (boost::asio::socket_base::reuse_address (true));
    reserved_reconnect_acceptor.bind ({boost::asio::ip::make_address ("127.0.0.1"), 0});
    const auto reconnect_success_port = reserved_reconnect_acceptor.local_endpoint ().port ();
    const auto reconnect_success_endpoint =
      std::string ("tcp://127.0.0.1:") + std::to_string (reconnect_success_port);
    std::atomic_bool reconnect_success_send_seen{false};
    zlink::stream_connector::connector_options_t reconnect_success_options;
    reconnect_success_options.endpoint = reconnect_success_endpoint;
    reconnect_success_options.reconnect.initial_delay = std::chrono::milliseconds (10);
    reconnect_success_options.reconnect.max_delay = std::chrono::milliseconds (10);
    reconnect_success_options.reconnect.max_attempts = 4;
    auto reconnect_success_connector =
      zlink::stream_connector::connector_factory_t::create (reconnect_success_options);
    std::atomic_bool reconnect_success_connect_finished{false};
    joining_thread_t reconnect_success_server_thread (
      [acceptor = std::move (reserved_reconnect_acceptor), &reconnect_success_connector,
       &reconnect_success_connect_finished, &reconnect_success_send_seen] () mutable {
          while (!reconnect_success_connect_finished
                 && reconnect_success_connector.state ()
                      != zlink::stream_connector::connection_state_t::reconnecting) {
              std::this_thread::yield ();
          }
          if (reconnect_success_connect_finished) {
              return;
          }
          acceptor.listen ();
          boost::asio::ip::tcp::socket socket (acceptor.get_executor ());
          acceptor.accept (socket);
          std::array<char, 512> request_buffer{};
          boost::system::error_code error;
          const auto read_size = socket.read_some (boost::asio::buffer (request_buffer), error);
          if (error) {
              return;
          }
          std::string frame_text (request_buffer.data (), read_size);
          if (auto frame = try_read_server_frame (frame_text)) {
              reconnect_success_send_seen =
                frame->header.kind == zlink::stream_connector::message_kind_t::send
                && frame->header.name == login_request_t::packet_name;
          }
      });
    auto reconnect_success_states =
      std::make_shared<std::vector<zlink::stream_connector::connection_state_t>> ();
    auto reconnect_success_subscription = reconnect_success_connector.on_connection_state_changed (
      [reconnect_success_states] (
        const zlink::stream_connector::connection_state_changed_t &state) {
          reconnect_success_states->push_back (state.current);
      });
    const auto reconnect_success_result = reconnect_success_connector.connect ();
    reconnect_success_connect_finished = true;
    while (reconnect_success_connector.pending_dispatch_count () != 0) {
        (void) reconnect_success_connector.dispatch ();
    }
    if (!reconnect_success_result || !reconnect_success_connector.dispatch ()
        || std::find (reconnect_success_states->begin (), reconnect_success_states->end (),
                      zlink::stream_connector::connection_state_t::reconnecting)
             == reconnect_success_states->end ()) {
        reconnect_success_server_thread.join ();
        return fail (67);
    }
    reconnect_success_connector.send (login_request_t{}).submit ();
    const auto reconnect_success_send_deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (2);
    while (!reconnect_success_send_seen
           && std::chrono::steady_clock::now () < reconnect_success_send_deadline) {
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    reconnect_success_connector.close ();
    reconnect_success_server_thread.join ();
    if (!reconnect_success_send_seen) {
        return fail (69);
    }

    zlink::stream_connector::connector_options_t reconnect_options;
    reconnect_options.endpoint = "tcp://127.0.0.1:1";
    reconnect_options.reconnect.initial_delay = std::chrono::milliseconds (1);
    reconnect_options.reconnect.max_delay = std::chrono::milliseconds (1);
    reconnect_options.reconnect.max_attempts = 2;
    auto reconnect_connector =
      zlink::stream_connector::connector_factory_t::create (reconnect_options);
    auto reconnect_states =
      std::make_shared<std::vector<zlink::stream_connector::connection_state_t>> ();
    auto reconnect_states_subscription = reconnect_connector.on_connection_state_changed (
      [reconnect_states] (const zlink::stream_connector::connection_state_changed_t &state) {
          reconnect_states->push_back (state.current);
      });
    auto reconnect_result = reconnect_connector.connect ();
    // §9: connection refusal is Disconnected; ConnectTimeout requires expiry.
    while (reconnect_connector.pending_dispatch_count () != 0) {
        (void) reconnect_connector.dispatch ();
    }
    if (reconnect_result
        || reconnect_result.error_code () != zlink::stream_connector::error_code_t::disconnected
        || !reconnect_connector.dispatch ()
        || std::find (reconnect_states->begin (), reconnect_states->end (),
                      zlink::stream_connector::connection_state_t::reconnecting)
             == reconnect_states->end ()) {
        return fail (39);
    }

    struct endpoint_mismatch_case_t
    {
        zlink::stream_connector::transport_t transport;
        const char *endpoint;
        const char *message_part;
    };

    /* stream-connector §3.1: a transport that disagrees with the endpoint
   * scheme is a configuration_error, whichever of the two the caller wrote. */
    const endpoint_mismatch_case_t endpoint_mismatch_cases[] = {
      {zlink::stream_connector::transport_t::tcp, "ws://127.0.0.1:1/stream",
       "does not match the endpoint scheme"},
      {zlink::stream_connector::transport_t::websocket, "tcp://127.0.0.1:1",
       "does not match the endpoint scheme"},
      {zlink::stream_connector::transport_t::tls, "tcp://127.0.0.1:1",
       "does not match the endpoint scheme"},
      {zlink::stream_connector::transport_t::websocket_secure, "tcp://127.0.0.1:1",
       "does not match the endpoint scheme"}};
    for (const auto &mismatch_case : endpoint_mismatch_cases) {
        zlink::stream_connector::connector_options_t invalid_transport_options;
        invalid_transport_options.endpoint = mismatch_case.endpoint;
        invalid_transport_options.transport = mismatch_case.transport;
        auto invalid_transport =
          zlink::stream_connector::connector_factory_t::create (invalid_transport_options);
        auto invalid_transport_result = invalid_transport.connect ();
        if (invalid_transport_result
            || invalid_transport_result.error_code ()
                 != zlink::stream_connector::error_code_t::configuration_error
            || !invalid_transport_result.error ()
            || invalid_transport_result.error ()->message.find (mismatch_case.message_part)
                 == std::string::npos
            || invalid_transport.is_connected ()) {
            return fail (42);
        }
    }

    /* stream-connector §3.1: with no transport option the endpoint scheme
   * decides, so a ws:// endpoint on its own is not a configuration error. */
    {
        zlink::stream_connector::connector_options_t inferred_options;
        inferred_options.endpoint = websocket_endpoint;
        if (!zlink::stream_connector::detail::transport_from_scheme (inferred_options.endpoint)
            || *zlink::stream_connector::detail::transport_from_scheme (inferred_options.endpoint)
                 != zlink::stream_connector::transport_t::websocket
            || inferred_options.transport.has_value ()) {
            return fail (210);
        }
        for (const auto *scheme_case : {"tcp://127.0.0.1:1", "tls://127.0.0.1:1",
                                        "ws://127.0.0.1:1/stream", "wss://127.0.0.1:1/stream"}) {
            zlink::stream_connector::connector_options_t scheme_options;
            scheme_options.endpoint = scheme_case;
            auto resolved = zlink::stream_connector::detail::resolve_transport (scheme_options);
            if (!resolved) {
                return fail (211);
            }
        }
        zlink::stream_connector::connector_options_t unknown_scheme_options;
        unknown_scheme_options.endpoint = "amqp://127.0.0.1:1";
        auto unknown_scheme =
          zlink::stream_connector::connector_factory_t::create (unknown_scheme_options).connect ();
        if (unknown_scheme
            || unknown_scheme.error_code ()
                 != zlink::stream_connector::error_code_t::configuration_error) {
            return fail (212);
        }
    }

    auto request_after_reconnect_failure = reconnect_connector.request (login_request_t{})
                                             .packet_name ("after.reconnect.failure")
                                             .submit<login_reply_t> ();
    if (request_after_reconnect_failure
        || request_after_reconnect_failure.error_code ()
             != zlink::stream_connector::error_code_t::disconnected) {
        return fail (43);
    }

    if (const int failure = verify_type_name_and_result (); failure != 0)
        return failure;
    if (const int failure = verify_connection_option_validation (context); failure != 0)
        return failure;
    if (const int failure = verify_arrival_count (); failure != 0)
        return failure;
    if (const int failure = verify_subscription_lifetime (connector); failure != 0)
        return failure;
    if (const int failure = verify_typed_injection (); failure != 0)
        return failure;
    if (const int failure = verify_unlocked_receive_predicate (runtime); failure != 0)
        return failure;
    if (const int failure = verify_actor_packet_dispatch (); failure != 0)
        return failure;
    if (const int failure = verify_reconnect_jitter (); failure != 0)
        return failure;
    return 0;
}

int verify_reconnect_jitter ()
{
    /* stream-connector §6: the wait between attempts lands between 50% and 100%
   * of the base delay. */
    {
        const auto base = std::chrono::milliseconds (400);
        bool below_base_seen = false;
        for (int index = 0; index < 200; ++index) {
            const auto delay = zlink::stream_connector::detail::jittered_delay (base);
            if (delay < base / 2 || delay > base) {
                return fail (248);
            }
            if (delay < base) {
                below_base_seen = true;
            }
        }
        if (!below_base_seen) {
            return fail (249);
        }
    }

    return 0;
}
} // namespace stream_connector_tests
