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
#include "../support/stream_connector_callback_sync.hpp"
#include "../support/stream_connector_failure.hpp"

using namespace stream_connector_fixture;


using zlink::stream_connector::codec_t;
using zlink::stream_connector::header_flags_t;
using zlink::stream_connector::message_kind_t;
using zlink::stream_connector::detail::header_codec_t;
using zlink::stream_connector::detail::stream_header_t;

namespace stream_connector_tests
{
int verify_actor_close_order ()
{
    /* stream-connector §5.6/§7: close queues unbound callbacks in issue order
   * before the disconnected callback in both dispatch modes. */
    for (const auto mode : {zlink::stream_connector::dispatch_mode_t::manual,
                            zlink::stream_connector::dispatch_mode_t::immediate}) {
        boost::asio::io_context close_actor_io;
        boost::asio::ip::tcp::acceptor close_actor_acceptor (
          close_actor_io, {boost::asio::ip::make_address ("127.0.0.1"), 0});
        const auto close_actor_endpoint =
          std::string ("tcp://127.0.0.1:")
          + std::to_string (close_actor_acceptor.local_endpoint ().port ());
        callback_latch_t close_actor_release;
        joining_thread_t close_actor_server ([&] {
            boost::asio::ip::tcp::socket socket (close_actor_acceptor.get_executor ());
            close_actor_acceptor.accept (socket);
            for (const auto [slot, name] :
                 {std::pair<std::uint16_t, std::string_view>{7, "first"}, {2, "second"}}) {
                std::string payload{static_cast<char> (1), static_cast<char> (0),
                                    static_cast<char> (slot), static_cast<char> (name.size ())};
                payload += name;
                const auto frame =
                  make_server_frame (message_kind_t::control, 0, "$zlink.actor.bound", payload);
                boost::asio::write (socket, boost::asio::buffer (frame.to_string ()));
            }
            close_actor_release.wait_for (std::chrono::seconds (2));
        });
        zlink::stream_connector::connector_options_t close_actor_options;
        close_actor_options.endpoint = close_actor_endpoint;
        close_actor_options.dispatch_mode = mode;
        close_actor_options.reconnect.enabled = false;
        auto close_actor_connector =
          zlink::stream_connector::connector_factory_t::create (close_actor_options);
        std::mutex close_actor_events_mutex;
        std::vector<std::string> close_actor_events;
        callback_latch_t close_actor_disconnected;
        auto close_actor_unbound_subscription = close_actor_connector.on_actor_unbound (
          [&] (const std::shared_ptr<zlink::stream_connector::actor_t> &actor) {
              std::lock_guard<std::mutex> lock (close_actor_events_mutex);
              close_actor_events.push_back ("unbound:" + actor->actor_id ());
          });
        auto close_actor_disconnected_subscription = close_actor_connector.on_disconnected (
          [&] (std::optional<zlink::stream_connector::close_reason_t>) {
              {
                  std::lock_guard<std::mutex> lock (close_actor_events_mutex);
                  close_actor_events.push_back ("disconnected");
              }
              close_actor_disconnected.signal ();
          });
        if (!close_actor_connector.connect ()) {
            close_actor_release.signal ();
            return fail (287);
        }
        const auto close_actor_deadline =
          std::chrono::steady_clock::now () + std::chrono::seconds (1);
        while ((!close_actor_connector.actor ("first") || !close_actor_connector.actor ("second"))
               && std::chrono::steady_clock::now () < close_actor_deadline) {
            std::this_thread::sleep_for (std::chrono::milliseconds (1));
        }
        auto first_actor = close_actor_connector.actor ("first");
        auto second_actor = close_actor_connector.actor ("second");
        if (!first_actor || !second_actor || !close_actor_connector.close ()) {
            close_actor_release.signal ();
            return fail (288);
        }
        if (mode == zlink::stream_connector::dispatch_mode_t::manual) {
            while (close_actor_connector.pending_dispatch_count () != 0)
                (void) close_actor_connector.dispatch ();
        } else if (!close_actor_disconnected.wait_for (std::chrono::seconds (1))) {
            close_actor_release.signal ();
            return fail (289);
        }
        close_actor_release.signal ();
        close_actor_server.join ();
        const std::vector<std::string> expected_close_actor_events{
          "unbound:first", "unbound:second", "disconnected"};
        std::lock_guard<std::mutex> lock (close_actor_events_mutex);
        if (first_actor->is_bound () || second_actor->is_bound ()
            || close_actor_events != expected_close_actor_events) {
            return fail (290);
        }
    }

    return 0;
}

int verify_actor_packet_dispatch ()
{
    /* stream-connector §5.6: Actor controls update the table before callbacks,
   * a slotted packet exposes actor_id, handle handlers are scoped to that
   * Actor, and an explicit unbound closes the handle before its callback. */
    {
        using zlink::stream_connector::actor_t;
        using zlink::stream_connector::connector_factory_t;
        using zlink::stream_connector::connector_options_t;
        using zlink::stream_connector::dispatch_mode_t;
        using zlink::stream_connector::packet_t;

        boost::asio::io_context actor_io;
        boost::asio::ip::tcp::acceptor actor_acceptor (
          actor_io, {boost::asio::ip::make_address ("127.0.0.1"), 0});
        const auto actor_endpoint = std::string ("tcp://127.0.0.1:")
                                    + std::to_string (actor_acceptor.local_endpoint ().port ());
        callback_latch_t actor_bound_sent;
        callback_latch_t actor_packets_allowed;
        callback_latch_t actor_rebind_allowed;
        callback_latch_t actor_rebound_sent;
        callback_latch_t actor_server_release;
        std::atomic_bool actor_hook_metadata_seen{false};
        joining_thread_t actor_server ([&] {
            boost::asio::ip::tcp::socket socket (actor_acceptor.get_executor ());
            actor_acceptor.accept (socket);
            std::string bound_payload{static_cast<char> (1), static_cast<char> (0),
                                      static_cast<char> (7), static_cast<char> (5)};
            bound_payload += "actor";
            const auto bound =
              make_server_frame (message_kind_t::control, 0, "$zlink.actor.bound", bound_payload);
            boost::asio::write (socket, boost::asio::buffer (bound.to_string ()));
            actor_bound_sent.signal ();
            if (!actor_packets_allowed.wait_for (std::chrono::seconds (2)))
                return;
            std::array<std::uint8_t, 6> request_prefix{};
            boost::asio::read (socket, boost::asio::buffer (request_prefix));
            const auto request_header_size =
              static_cast<std::size_t> ((request_prefix[0] << 8) | request_prefix[1]);
            const auto request_payload_size =
              static_cast<std::size_t> ((request_prefix[2] << 24) | (request_prefix[3] << 16)
                                        | (request_prefix[4] << 8) | request_prefix[5]);
            std::vector<std::uint8_t> request_bytes (request_header_size + request_payload_size);
            boost::asio::read (socket, boost::asio::buffer (request_bytes));
            std::vector<std::uint8_t> request_header_bytes (
              request_bytes.begin (), request_bytes.begin () + request_header_size);
            const auto actor_request_header = header_codec_t{}.decode (request_header_bytes);
            if (!actor_request_header || !actor_request_header.value ().request_seq)
                return;
            actor_hook_metadata_seen.store (
              actor_request_header.value ().name == "actor.hook.request"
              && actor_request_header.value ().actor_slot == 7
              && actor_request_header.value ().metadata.values.contains ("actor-hook"));
            const auto actor_reply =
              make_server_frame (message_kind_t::response,
                                 *actor_request_header.value ().request_seq, "", "actor-reply");
            boost::asio::write (socket, boost::asio::buffer (actor_reply.to_string ()));
            const auto packet = make_server_frame (message_kind_t::send, 0, "actor.push", "payload",
                                                   false, std::uint16_t{7});
            boost::asio::write (socket, boost::asio::buffer (packet.to_string ()));
            const auto failed_wait =
              make_server_frame (message_kind_t::send, 0, "typed.wait", "invalid");
            boost::asio::write (socket, boost::asio::buffer (failed_wait.to_string ()));
            const std::string unbound_payload{static_cast<char> (1), static_cast<char> (0),
                                              static_cast<char> (7)};
            const auto unbound = make_server_frame (message_kind_t::control, 0,
                                                    "$zlink.actor.unbound", unbound_payload);
            boost::asio::write (socket, boost::asio::buffer (unbound.to_string ()));
            if (!actor_rebind_allowed.wait_for (std::chrono::seconds (2)))
                return;
            std::string rebound_payload{static_cast<char> (1), static_cast<char> (0),
                                        static_cast<char> (8), static_cast<char> (5)};
            rebound_payload += "actor";
            const auto rebound =
              make_server_frame (message_kind_t::control, 0, "$zlink.actor.bound", rebound_payload);
            const auto rebound_packet = make_server_frame (message_kind_t::send, 0, "actor.push",
                                                           "rebound", false, std::uint16_t{8});
            boost::asio::write (socket, boost::asio::buffer (rebound.to_string ()));
            boost::asio::write (socket, boost::asio::buffer (rebound_packet.to_string ()));
            actor_rebound_sent.signal ();
            actor_server_release.wait_for (std::chrono::seconds (2));
        });

        connector_options_t actor_options;
        actor_options.endpoint = actor_endpoint;
        actor_options.dispatch_mode = dispatch_mode_t::manual;
        actor_options.reconnect.enabled = false;
        auto actor_connector = connector_factory_t::create (actor_options);
        std::vector<std::string> actor_events;
        std::optional<std::string> received_actor_id;
        int canceled_bound_calls = 0;
        auto throwing_bound_subscription =
          actor_connector.on_actor_bound ([] (const std::shared_ptr<actor_t> &) {
              throw std::runtime_error ("expected lifecycle callback failure");
          });
        auto canceled_bound_subscription = actor_connector.on_actor_bound (
          [&] (const std::shared_ptr<actor_t> &) { ++canceled_bound_calls; });
        auto bound_subscription =
          actor_connector.on_actor_bound ([&] (const std::shared_ptr<actor_t> &actor) {
              actor_events.push_back ("bound:" + actor->actor_id ());
          });
        callback_latch_t actor_unbound_delivered;
        auto unbound_subscription =
          actor_connector.on_actor_unbound ([&] (const std::shared_ptr<actor_t> &actor) {
              actor_events.push_back ("unbound:" + actor->actor_id ());
              actor_unbound_delivered.signal ();
          });
        auto packet_subscription =
          actor_connector.on<packet_t> ("actor.push", [&] (const packet_message_t &message) {
              received_actor_id = message.actor_id;
              actor_events.push_back ("packet");
          });
        if (!actor_connector.connect () || !actor_bound_sent.wait_for (std::chrono::seconds (1))) {
            actor_packets_allowed.signal ();
            actor_server_release.signal ();
            return fail (263);
        }
        std::shared_ptr<actor_t> actor_handle;
        const auto actor_deadline = std::chrono::steady_clock::now () + std::chrono::seconds (1);
        while (!(actor_handle = actor_connector.actor ("actor"))
               && std::chrono::steady_clock::now () < actor_deadline) {
            // Spec 32 §5.6: receive updates the Actor list before it queues the
            // bound callbacks. Dispatching here could run the callback that is
            // unsubscribed below; §7 stops it only for later dispatches.
            std::this_thread::yield ();
        }
        if (!actor_handle || actor_connector.actors ().size () != 1 || !actor_handle->is_bound ()) {
            actor_packets_allowed.signal ();
            actor_server_release.signal ();
            return fail (264);
        }
        canceled_bound_subscription.unsubscribe ();
        auto actor_packet_subscription =
          actor_handle->on<packet_t> ("actor.push", [&] (const packet_message_t &) {
              actor_events.push_back ("actor-packet");
          });
        int typed_decode_errors = 0;
        int typed_callback_calls = 0;
        auto typed_error_subscription = actor_connector.on_error ([&] (const auto &error) {
            if (error.code == zlink::stream_connector::error_code_t::frame_decode_failed)
                ++typed_decode_errors;
        });
        auto typed_connector_subscription = actor_connector.on<failed_protobuf_t> (
          "actor.push", [&] (const auto &) { ++typed_callback_calls; });
        auto typed_actor_subscription = actor_handle->on<failed_protobuf_t> (
          "actor.push", [&] (const auto &) { ++typed_callback_calls; });
        bool actor_sending_hook_seen = false;
        bool actor_reply_hook_seen = false;
        auto actor_sending_hook = actor_connector.on_request_sending (
          [&] (zlink::stream_connector::request_sending_context_t &ctx) {
              actor_sending_hook_seen =
                ctx.request_packet_name == "actor.hook.request" && ctx.actor_id == "actor";
              ctx.set_metadata ("actor-hook", "present");
          });
        auto actor_reply_hook = actor_connector.on_reply_received (
          [&] (const zlink::stream_connector::reply_received_context_t &ctx) {
              actor_reply_hook_seen = ctx.request_packet_name == "actor.hook.request"
                                      && ctx.actor_id == "actor" && ctx.succeeded && ctx.reply
                                      && !ctx.error;
          });
        actor_packets_allowed.signal ();
        auto actor_request_result = actor_handle
                                      ->request (packet_t{.name = "actor.hook.request",
                                                          .payload = as_bytes ("actor-request")})
                                      .timeout (std::chrono::seconds (1))
                                      .submit<std::vector<std::uint8_t>> ();
        const auto typed_wait = actor_connector.wait_for<failed_protobuf_t> ("typed.wait")
                                  .timeout (std::chrono::seconds (1))
                                  .submit ();
        while (actor_connector.pending_dispatch_count () != 0) {
            (void) actor_connector.dispatch ();
        }
        if (!actor_request_result || as_string (actor_request_result.value ()) != "actor-reply"
            || !actor_sending_hook_seen || !actor_reply_hook_seen
            || !actor_hook_metadata_seen.load ()) {
            actor_server_release.signal ();
            return fail (282);
        }
        const auto unbound_deadline = std::chrono::steady_clock::now () + std::chrono::seconds (1);
        while (!actor_unbound_delivered.wait_for (std::chrono::milliseconds (0))
               && std::chrono::steady_clock::now () < unbound_deadline) {
            if (!actor_connector.dispatch ()) {
                actor_server_release.signal ();
                return fail (265);
            }
        }
        if (!actor_unbound_delivered.wait_for (std::chrono::milliseconds (0))) {
            actor_server_release.signal ();
            return fail (265);
        }
        const std::vector<std::string> expected_actor_events{"bound:actor", "packet",
                                                             "actor-packet", "unbound:actor"};
        if (actor_connector.actor ("actor") || actor_handle->is_bound ()
            || received_actor_id != "actor" || actor_events != expected_actor_events
            || canceled_bound_calls != 0) {
            actor_server_release.signal ();
            return fail (266);
        }
        while (actor_connector.pending_dispatch_count () != 0)
            (void) actor_connector.dispatch ();
        if (typed_decode_errors != 2 || typed_callback_calls != 0 || typed_wait
            || typed_wait.error_code ()
                 != zlink::stream_connector::error_code_t::frame_decode_failed) {
            actor_server_release.signal ();
            return fail (322);
        }
        typed_connector_subscription.unsubscribe ();
        typed_actor_subscription.unsubscribe ();
        typed_error_subscription.unsubscribe ();
        actor_rebind_allowed.signal ();
        if (!actor_rebound_sent.wait_for (std::chrono::seconds (1))) {
            actor_server_release.signal ();
            return fail (277);
        }
        const auto rebound_deadline = std::chrono::steady_clock::now () + std::chrono::seconds (1);
        std::shared_ptr<actor_t> rebound_handle;
        while ((!(rebound_handle = actor_connector.actor ("actor"))
                || actor_connector.received_count ("actor.push") < 2)
               && std::chrono::steady_clock::now () < rebound_deadline) {
            if (!actor_connector.dispatch ()) {
                actor_server_release.signal ();
                return fail (278);
            }
        }
        (void) actor_connector.dispatch ();
        if (!rebound_handle || rebound_handle == actor_handle || actor_handle->is_bound ()
            || std::count (actor_events.begin (), actor_events.end (), "actor-packet") != 1) {
            actor_server_release.signal ();
            return fail (279);
        }
        std::optional<zlink::stream_connector::error_code_t> closed_actor_error;
        auto closed_actor_error_subscription = actor_connector.on_error (
          [&] (const zlink::stream_connector::error_t &error) { closed_actor_error = error.code; });
        actor_handle->send (packet_t{.name = "closed.actor", .payload = as_bytes ("payload")})
          .submit ();
        const auto closed_actor_error_deadline =
          std::chrono::steady_clock::now () + std::chrono::seconds (1);
        while (!closed_actor_error
               && std::chrono::steady_clock::now () < closed_actor_error_deadline) {
            if (!actor_connector.dispatch ()) {
                actor_server_release.signal ();
                return fail (267);
            }
        }
        if (closed_actor_error != zlink::stream_connector::error_code_t::validation_failed) {
            actor_server_release.signal ();
            return fail (267);
        }
        actor_server_release.signal ();
        actor_server.join ();
        actor_connector.close ();
    }

    /* A packet that names a slot before its bound control is a decode failure,
   * not an unscoped application packet. */
    {
        boost::asio::io_context bad_actor_io;
        boost::asio::ip::tcp::acceptor bad_actor_acceptor (
          bad_actor_io, {boost::asio::ip::make_address ("127.0.0.1"), 0});
        const auto bad_actor_endpoint =
          std::string ("tcp://127.0.0.1:")
          + std::to_string (bad_actor_acceptor.local_endpoint ().port ());
        callback_latch_t bad_actor_release;
        joining_thread_t bad_actor_server ([&] {
            boost::asio::ip::tcp::socket socket (bad_actor_acceptor.get_executor ());
            bad_actor_acceptor.accept (socket);
            const auto packet = make_server_frame (message_kind_t::send, 0, "unknown.actor",
                                                   "payload", false, std::uint16_t{11});
            boost::asio::write (socket, boost::asio::buffer (packet.to_string ()));
            bad_actor_release.wait_for (std::chrono::seconds (2));
        });
        zlink::stream_connector::connector_options_t bad_actor_options;
        bad_actor_options.endpoint = bad_actor_endpoint;
        bad_actor_options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::manual;
        bad_actor_options.reconnect.enabled = false;
        auto bad_actor_connector =
          zlink::stream_connector::connector_factory_t::create (bad_actor_options);
        std::optional<zlink::stream_connector::error_code_t> actor_decode_error;
        auto error_subscription = bad_actor_connector.on_error (
          [&] (const zlink::stream_connector::error_t &error) { actor_decode_error = error.code; });
        if (!bad_actor_connector.connect ()) {
            bad_actor_release.signal ();
            return fail (268);
        }
        const auto disconnected_deadline =
          std::chrono::steady_clock::now () + std::chrono::seconds (1);
        while (bad_actor_connector.state ()
                 != zlink::stream_connector::connection_state_t::disconnected
               && std::chrono::steady_clock::now () < disconnected_deadline) {
            std::this_thread::sleep_for (std::chrono::milliseconds (1));
        }
        while (bad_actor_connector.pending_dispatch_count () != 0) {
            if (!bad_actor_connector.dispatch ()) {
                bad_actor_release.signal ();
                return fail (269);
            }
        }
        if (actor_decode_error != zlink::stream_connector::error_code_t::frame_decode_failed) {
            bad_actor_release.signal ();
            return fail (270);
        }
        bad_actor_release.signal ();
        bad_actor_server.join ();
        bad_actor_connector.close ();
    }

    return 0;
}
} // namespace stream_connector_tests
