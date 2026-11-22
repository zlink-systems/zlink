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
#include <boost/beast/core/stream_traits.hpp>
#include <boost/beast/websocket/stream.hpp>
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
#include "../support/stream_connector_compression_fixture.hpp"
#include "../support/stream_connector_scripted_transport.hpp"
#include "../support/stream_connector_callback_sync.hpp"
#include "../support/stream_connector_failure.hpp"

using namespace stream_connector_fixture;


namespace stream_connector_tests
{
int verify_default_connector ()
{
    {
        zlink::stream_connector::connector_t default_connector;
        bool disconnected_callback_registered = false;
        auto disconnected_subscription = default_connector.on_disconnected (
          [&] (std::optional<zlink::stream_connector::close_reason_t>) {
              disconnected_callback_registered = true;
          });
        if (default_connector.is_connected () || default_connector.pending_dispatch_count () != 0) {
            return fail (144);
        }
        (void) disconnected_callback_registered;
        zlink::stream_connector::metadata_t metadata;
        metadata.with ("trace", "unbound");
        zlink::stream_connector::send_call_t unbound_send;
        unbound_send.metadata (metadata).submit ();
    }

    return 0;
}

int verify_connection_lifecycle ()
{
    {
        boost::asio::io_context lifecycle_io;
        boost::asio::ip::tcp::acceptor lifecycle_acceptor (
          lifecycle_io, {boost::asio::ip::make_address ("127.0.0.1"), 0});
        const auto lifecycle_endpoint =
          std::string ("tcp://127.0.0.1:")
          + std::to_string (lifecycle_acceptor.local_endpoint ().port ());
        callback_latch_t accepted_latch;
        callback_latch_t release_latch;
        joining_thread_t lifecycle_server ([&lifecycle_acceptor, &accepted_latch, &release_latch] {
            boost::asio::ip::tcp::socket socket (lifecycle_acceptor.get_executor ());
            lifecycle_acceptor.accept (socket);
            accepted_latch.signal ();
            release_latch.wait_for (std::chrono::seconds (2));
        });

        zlink::stream_connector::connector_options_t lifecycle_options;
        lifecycle_options.endpoint = lifecycle_endpoint;
        lifecycle_options.reconnect.enabled = false;
        lifecycle_options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;
        auto lifecycle_connector =
          zlink::stream_connector::connector_factory_t::create (lifecycle_options);
        struct lifecycle_states_t
        {
            std::atomic_size_t count{0};
            callback_latch_t initial_states_delivered;
        };
        auto lifecycle_states = std::make_shared<lifecycle_states_t> ();
        auto lifecycle_state_subscription = lifecycle_connector.on_connection_state_changed (
          [lifecycle_states] (const zlink::stream_connector::connection_state_changed_t &) {
              if (lifecycle_states->count.fetch_add (1) + 1 == 2) {
                  lifecycle_states->initial_states_delivered.signal ();
              }
          });
        const auto first_connect = lifecycle_connector.connect ();
        const auto initial_states_completed =
          lifecycle_states->initial_states_delivered.wait_for (std::chrono::seconds (1));
        const auto first_state_count = lifecycle_states->count.load ();
        const auto repeated_connect = lifecycle_connector.connect ();
        const auto repeated_state_count = lifecycle_states->count.load ();
        callback_latch_t repeated_async_latch;
        std::atomic_bool repeated_async_succeeded{false};
        lifecycle_connector.connect ([&repeated_async_latch, &repeated_async_succeeded] (
                                       zlink::stream_connector::result_t<void> result) {
            repeated_async_succeeded.store (static_cast<bool> (result));
            repeated_async_latch.signal ();
        });
        const auto repeated_async_completed =
          repeated_async_latch.wait_for (std::chrono::seconds (1));
        const auto repeated_async_state_count = lifecycle_states->count.load ();
        const auto closed = lifecycle_connector.close ();
        const auto connect_after_close = lifecycle_connector.connect ();
        if (connect_after_close) {
            (void) lifecycle_connector.close ();
        }
        release_latch.signal ();
        lifecycle_server.join ();
        if (!first_connect || !initial_states_completed
            || !accepted_latch.wait_for (std::chrono::seconds (1)) || !repeated_connect
            || repeated_state_count != first_state_count || !closed || !repeated_async_completed
            || !repeated_async_succeeded.load () || repeated_async_state_count != first_state_count
            || connect_after_close
            || connect_after_close.error_code ()
                 != zlink::stream_connector::error_code_t::disconnected) {
            return fail (168);
        }
    }

    {
        boost::asio::io_context timeout_io;
        boost::asio::ip::tcp::acceptor timeout_acceptor (
          timeout_io, {boost::asio::ip::make_address ("127.0.0.1"), 0});
        const auto timeout_endpoint = std::string ("ws://127.0.0.1:")
                                      + std::to_string (timeout_acceptor.local_endpoint ().port ())
                                      + "/stream";
        callback_latch_t timeout_server_accepted;
        callback_latch_t timeout_server_release;
        joining_thread_t timeout_server (
          [&timeout_acceptor, &timeout_server_accepted, &timeout_server_release] {
              boost::asio::ip::tcp::socket socket (timeout_acceptor.get_executor ());
              timeout_acceptor.accept (socket);
              timeout_server_accepted.signal ();
              timeout_server_release.wait_for (std::chrono::milliseconds (500));
          });

        zlink::stream_connector::connector_options_t timeout_options;
        timeout_options.endpoint = timeout_endpoint;
        timeout_options.transport = zlink::stream_connector::transport_t::websocket;
        timeout_options.connect_timeout = std::chrono::milliseconds (50);
        timeout_options.reconnect.enabled = false;
        auto timeout_connector =
          zlink::stream_connector::connector_factory_t::create (timeout_options);
        const auto connect_started_at = std::chrono::steady_clock::now ();
        const auto timed_connect = timeout_connector.connect ();
        const auto connect_elapsed = std::chrono::steady_clock::now () - connect_started_at;
        timeout_server_release.signal ();
        timeout_server.join ();
        if (!timeout_server_accepted.wait_for (std::chrono::milliseconds (100)) || timed_connect
            || timed_connect.error_code () != zlink::stream_connector::error_code_t::connect_timeout
            || connect_elapsed >= std::chrono::milliseconds (250)) {
            return fail (169);
        }
    }

    {
        boost::asio::io_context timeout_io;
        boost::asio::ip::tcp::acceptor timeout_acceptor (
          timeout_io, {boost::asio::ip::make_address ("127.0.0.1"), 0});
        const auto timeout_endpoint = std::string ("ws://127.0.0.1:")
                                      + std::to_string (timeout_acceptor.local_endpoint ().port ())
                                      + "/stream";
        callback_latch_t timeout_server_release;
        joining_thread_t timeout_server ([&timeout_acceptor, &timeout_server_release] {
            boost::asio::ip::tcp::socket socket (timeout_acceptor.get_executor ());
            timeout_acceptor.accept (socket);
            timeout_server_release.wait_for (std::chrono::milliseconds (500));
        });

        zlink::stream_connector::connector_options_t timeout_options;
        timeout_options.endpoint = timeout_endpoint;
        timeout_options.transport = zlink::stream_connector::transport_t::websocket;
        timeout_options.connect_timeout = std::chrono::milliseconds (50);
        timeout_options.reconnect.enabled = false;
        timeout_options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;
        auto timeout_connector =
          zlink::stream_connector::connector_factory_t::create (timeout_options);
        callback_latch_t timeout_completed;
        std::optional<zlink::stream_connector::error_code_t> timeout_error;
        const auto connect_started_at = std::chrono::steady_clock::now ();
        timeout_connector.connect ([&timeout_completed, &timeout_error] (auto result) {
            timeout_error = result.error_code ();
            timeout_completed.signal ();
        });
        const auto callback_arrived = timeout_completed.wait_for (std::chrono::milliseconds (250));
        const auto connect_elapsed = std::chrono::steady_clock::now () - connect_started_at;
        timeout_server_release.signal ();
        timeout_server.join ();
        if (!callback_arrived
            || timeout_error != zlink::stream_connector::error_code_t::connect_timeout
            || connect_elapsed >= std::chrono::milliseconds (250)) {
            return fail (170);
        }
    }

    {
        boost::asio::io_context heartbeat_io;
        boost::asio::ip::tcp::acceptor heartbeat_acceptor (
          heartbeat_io, {boost::asio::ip::make_address ("127.0.0.1"), 0});
        const auto heartbeat_endpoint =
          std::string ("tcp://127.0.0.1:")
          + std::to_string (heartbeat_acceptor.local_endpoint ().port ());
        std::atomic_bool heartbeat_seen_without_dispatch{false};
        joining_thread_t heartbeat_server ([&heartbeat_acceptor, &heartbeat_seen_without_dispatch] {
            boost::asio::ip::tcp::socket socket (heartbeat_acceptor.get_executor ());
            heartbeat_acceptor.accept (socket);
            socket.non_blocking (true);
            std::string buffer;
            std::array<char, 512> chunk{};
            const auto deadline =
              std::chrono::steady_clock::now () + std::chrono::milliseconds (250);
            while (std::chrono::steady_clock::now () < deadline) {
                boost::system::error_code error;
                const auto size = socket.read_some (boost::asio::buffer (chunk), error);
                if (!error) {
                    buffer.append (chunk.data (), size);
                    if (auto frame = try_read_server_frame (buffer)) {
                        heartbeat_seen_without_dispatch =
                          frame->header.kind == zlink::stream_connector::message_kind_t::control
                          && frame->header.name == "$zlink.heartbeat.ping";
                        break;
                    }
                } else if (error != boost::asio::error::would_block
                           && error != boost::asio::error::try_again) {
                    break;
                }
                std::this_thread::sleep_for (std::chrono::milliseconds (1));
            }
        });

        zlink::stream_connector::connector_options_t heartbeat_options;
        heartbeat_options.endpoint = heartbeat_endpoint;
        heartbeat_options.heartbeat.interval = std::chrono::milliseconds (20);
        heartbeat_options.heartbeat.timeout = std::chrono::milliseconds (500);
        heartbeat_options.reconnect.enabled = false;
        auto heartbeat_connector =
          zlink::stream_connector::connector_factory_t::create (heartbeat_options);
        if (!heartbeat_connector.connect ()) {
            return fail (171);
        }
        heartbeat_server.join ();
        heartbeat_connector.close ();
        if (!heartbeat_seen_without_dispatch) {
            return fail (171);
        }
    }

    {
        boost::asio::io_context lifecycle_io;
        boost::asio::ip::tcp::acceptor lifecycle_acceptor (
          lifecycle_io, {boost::asio::ip::make_address ("127.0.0.1"), 0});
        const auto lifecycle_endpoint =
          std::string ("tcp://127.0.0.1:")
          + std::to_string (lifecycle_acceptor.local_endpoint ().port ());
        callback_latch_t lifecycle_server_accepted;
        callback_latch_t lifecycle_server_release;
        joining_thread_t lifecycle_server (
          [&lifecycle_acceptor, &lifecycle_server_accepted, &lifecycle_server_release] {
              boost::asio::ip::tcp::socket socket (lifecycle_acceptor.get_executor ());
              lifecycle_acceptor.accept (socket);
              lifecycle_server_accepted.signal ();
              lifecycle_server_release.wait_for (std::chrono::milliseconds (500));
          });

        zlink::stream_connector::connector_options_t lifecycle_options;
        lifecycle_options.endpoint = lifecycle_endpoint;
        lifecycle_options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::manual;
        lifecycle_options.heartbeat.enabled = false;
        lifecycle_options.reconnect.enabled = false;
        auto lifecycle_connector =
          zlink::stream_connector::connector_factory_t::create (lifecycle_options);
        if (!lifecycle_connector.connect ()
            || !lifecycle_server_accepted.wait_for (std::chrono::milliseconds (100))) {
            lifecycle_server_release.signal ();
            lifecycle_server.join ();
            return fail (172);
        }

        std::mutex callback_mutex;
        int state_callback_count = 0;
        int error_callback_count = 0;
        int disconnected_callback_count = 0;
        std::vector<std::thread::id> callback_threads;
        auto lifecycle_state_handler =
          lifecycle_connector.on_connection_state_changed ([&] (const auto &) {
              std::lock_guard<std::mutex> lock (callback_mutex);
              ++state_callback_count;
              callback_threads.push_back (std::this_thread::get_id ());
          });
        auto lifecycle_error_handler = lifecycle_connector.on_error ([&] (const auto &) {
            std::lock_guard<std::mutex> lock (callback_mutex);
            ++error_callback_count;
            callback_threads.push_back (std::this_thread::get_id ());
        });
        auto lifecycle_disconnected_handler = lifecycle_connector.on_disconnected (
          [&] (std::optional<zlink::stream_connector::close_reason_t>) {
              std::lock_guard<std::mutex> lock (callback_mutex);
              ++disconnected_callback_count;
              callback_threads.push_back (std::this_thread::get_id ());
          });
        lifecycle_server_release.signal ();
        lifecycle_server.join ();
        const auto disconnected_deadline =
          std::chrono::steady_clock::now () + std::chrono::milliseconds (250);
        while (lifecycle_connector.state ()
                 != zlink::stream_connector::connection_state_t::disconnected
               && std::chrono::steady_clock::now () < disconnected_deadline) {
            std::this_thread::sleep_for (std::chrono::milliseconds (1));
        }
        bool callbacks_ran_before_dispatch = false;
        {
            std::lock_guard<std::mutex> lock (callback_mutex);
            callbacks_ran_before_dispatch = state_callback_count != 0 || error_callback_count != 0
                                            || disconnected_callback_count != 0;
        }
        if (callbacks_ran_before_dispatch) {
            lifecycle_connector.close ();
            return fail (173);
        }
        if (lifecycle_connector.pending_dispatch_count () == 0 || !lifecycle_connector.dispatch ()
            || !lifecycle_connector.dispatch () || !lifecycle_connector.dispatch ()) {
            lifecycle_connector.close ();
            return fail (174);
        }
        bool callbacks_invalid = false;
        {
            std::lock_guard<std::mutex> lock (callback_mutex);
            const auto dispatch_thread = std::this_thread::get_id ();
            callbacks_invalid = state_callback_count != 1 || error_callback_count != 1
                                || disconnected_callback_count != 1 || callback_threads.size () != 3
                                || std::any_of (callback_threads.begin (), callback_threads.end (),
                                                [dispatch_thread] (auto thread) {
                                                    return thread != dispatch_thread;
                                                });
        }
        if (callbacks_invalid) {
            lifecycle_connector.close ();
            return fail (175);
        }
        lifecycle_connector.close ();
    }

    {
        boost::asio::io_context reconnect_io;
        boost::asio::ip::tcp::acceptor reconnect_acceptor (
          reconnect_io, {boost::asio::ip::make_address ("127.0.0.1"), 0});
        const auto reconnect_endpoint =
          std::string ("tcp://127.0.0.1:")
          + std::to_string (reconnect_acceptor.local_endpoint ().port ());
        callback_latch_t first_request_seen;
        callback_latch_t release_first_connection;
        std::atomic_bool second_connection_seen{false};
        std::atomic_bool second_connection_received_fresh_send{false};
        joining_thread_t reconnect_server ([&reconnect_acceptor, &first_request_seen,
                                            &release_first_connection, &second_connection_seen,
                                            &second_connection_received_fresh_send] {
            boost::asio::ip::tcp::socket first (reconnect_acceptor.get_executor ());
            reconnect_acceptor.accept (first);
            std::array<char, 512> first_chunk{};
            boost::system::error_code error;
            const auto first_size = first.read_some (boost::asio::buffer (first_chunk), error);
            if (!error) {
                std::string first_buffer (first_chunk.data (), first_size);
                if (auto frame = try_read_server_frame (first_buffer);
                    frame && frame->header.kind == zlink::stream_connector::message_kind_t::request
                    && frame->header.name == "pending.before.reconnect") {
                    first_request_seen.signal ();
                }
            }
            release_first_connection.wait_for (std::chrono::milliseconds (500));
            first.shutdown (boost::asio::ip::tcp::socket::shutdown_both, error);
            first.close (error);

            reconnect_acceptor.non_blocking (true);
            boost::asio::ip::tcp::socket second (reconnect_acceptor.get_executor ());
            const auto accept_deadline =
              std::chrono::steady_clock::now () + std::chrono::milliseconds (700);
            while (std::chrono::steady_clock::now () < accept_deadline) {
                reconnect_acceptor.accept (second, error);
                if (!error) {
                    second_connection_seen = true;
                    break;
                }
                if (error != boost::asio::error::would_block
                    && error != boost::asio::error::try_again) {
                    return;
                }
                std::this_thread::sleep_for (std::chrono::milliseconds (1));
            }
            if (!second_connection_seen) {
                return;
            }
            second.non_blocking (true);
            std::string second_buffer;
            std::array<char, 512> second_chunk{};
            const auto read_deadline =
              std::chrono::steady_clock::now () + std::chrono::milliseconds (500);
            while (std::chrono::steady_clock::now () < read_deadline) {
                const auto size = second.read_some (boost::asio::buffer (second_chunk), error);
                if (!error) {
                    second_buffer.append (second_chunk.data (), size);
                    if (auto frame = try_read_server_frame (second_buffer)) {
                        second_connection_received_fresh_send =
                          frame->header.kind == zlink::stream_connector::message_kind_t::send
                          && frame->header.name == "fresh.after.reconnect";
                        return;
                    }
                } else if (error != boost::asio::error::would_block
                           && error != boost::asio::error::try_again) {
                    return;
                }
                std::this_thread::sleep_for (std::chrono::milliseconds (1));
            }
        });

        zlink::stream_connector::connector_options_t reconnect_options;
        reconnect_options.endpoint = reconnect_endpoint;
        reconnect_options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;
        reconnect_options.heartbeat.enabled = false;
        reconnect_options.connect_timeout = std::chrono::milliseconds (1000);
        reconnect_options.reconnect.initial_delay = std::chrono::milliseconds (100);
        reconnect_options.reconnect.max_delay = std::chrono::milliseconds (100);
        reconnect_options.reconnect.max_attempts = 3;
        auto reconnect_connector =
          zlink::stream_connector::connector_factory_t::create (reconnect_options);
        std::atomic_bool reconnecting_seen{false};
        auto reconnect_state_subscription = reconnect_connector.on_connection_state_changed (
          [&reconnecting_seen] (const auto &state) {
              if (state.current == zlink::stream_connector::connection_state_t::reconnecting) {
                  reconnecting_seen.store (true, std::memory_order_release);
              }
          });
        if (!reconnect_connector.connect ()) {
            release_first_connection.signal ();
            reconnect_server.join ();
            return fail (176);
        }
        // The callback owns its state: it may run after this frame ended.
        struct pending_request_state_t
        {
            callback_latch_t completed;
            std::atomic_int callback_count{0};
            std::optional<zlink::stream_connector::error_code_t> error;
        };
        auto pending_request = std::make_shared<pending_request_state_t> ();
        reconnect_connector.request (login_request_t{})
          .packet_name ("pending.before.reconnect")
          .timeout (std::chrono::seconds (2))
          .submit<login_reply_t> ([pending_request] (auto result) {
              ++pending_request->callback_count;
              pending_request->error = result.error_code ();
              pending_request->completed.signal ();
          });
        if (!first_request_seen.wait_for (std::chrono::milliseconds (250))) {
            release_first_connection.signal ();
            reconnect_server.join ();
            return fail (176);
        }
        release_first_connection.signal ();
        const auto reconnecting_deadline =
          std::chrono::steady_clock::now () + std::chrono::milliseconds (250);
        while (reconnect_connector.state ()
                 != zlink::stream_connector::connection_state_t::reconnecting
               && std::chrono::steady_clock::now () < reconnecting_deadline) {
            std::this_thread::sleep_for (std::chrono::milliseconds (1));
        }
        const auto request_during_reconnect = reconnect_connector.request (login_request_t{})
                                                .packet_name ("must.not.queue.during.reconnect")
                                                .timeout (std::chrono::seconds (1))
                                                .submit<login_reply_t> ();
        const auto connected_deadline =
          std::chrono::steady_clock::now () + std::chrono::milliseconds (700);
        while (reconnect_connector.state ()
                 != zlink::stream_connector::connection_state_t::connected
               && std::chrono::steady_clock::now () < connected_deadline) {
            std::this_thread::sleep_for (std::chrono::milliseconds (1));
        }
        if (reconnect_connector.state ()
            == zlink::stream_connector::connection_state_t::connected) {
            reconnect_connector.send (login_request_t{})
              .packet_name ("fresh.after.reconnect")
              .submit ();
        }
        reconnect_server.join ();
        const auto pending_completed =
          pending_request->completed.wait_for (std::chrono::milliseconds (250));
        reconnect_connector.close ();
        if (!reconnecting_seen.load (std::memory_order_acquire) || !second_connection_seen
            || !second_connection_received_fresh_send || !pending_completed
            || pending_request->callback_count != 1
            || pending_request->error != zlink::stream_connector::error_code_t::disconnected
            || request_during_reconnect
            || request_during_reconnect.error_code ()
                 != zlink::stream_connector::error_code_t::disconnected) {
            return fail (176);
        }
    }

    return 0;
}

int verify_connection_callbacks ()
{
    {
        zlink::stream_connector::connector_options_t lifecycle_options;
        lifecycle_options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::immediate;
        auto connect_lifetime = std::make_shared<callback_outcome_t> ();
        {
            auto lifecycle_connector =
              zlink::stream_connector::connector_factory_t::create (lifecycle_options);
            /* stream-connector §6.3: an empty endpoint is one value outside its
       * range, so it is validation_failed, not configuration_error. */
            lifecycle_connector.connect (
              [connect_lifetime] (zlink::stream_connector::result_t<void> result) {
                  connect_lifetime->seen =
                    !result
                    && result.error_code ()
                         == zlink::stream_connector::error_code_t::validation_failed;
                  connect_lifetime->latch.signal ();
              });
        }
        if (!connect_lifetime->latch.wait_for (std::chrono::milliseconds (100))
            || !connect_lifetime->seen) {
            return fail (97);
        }

        auto close_lifetime = std::make_shared<callback_outcome_t> ();
        {
            auto lifecycle_connector =
              zlink::stream_connector::connector_factory_t::create (lifecycle_options);
            lifecycle_connector.close (
              [close_lifetime] (zlink::stream_connector::result_t<void> result) {
                  close_lifetime->seen = static_cast<bool> (result);
                  close_lifetime->latch.signal ();
              });
        }
        if (!close_lifetime->latch.wait_for (std::chrono::milliseconds (100))
            || !close_lifetime->seen) {
            return fail (98);
        }

        boost::asio::io_context async_connect_io;
        boost::asio::ip::tcp::acceptor async_connect_acceptor (
          async_connect_io, {boost::asio::ip::make_address ("127.0.0.1"), 0});
        const auto async_connect_endpoint =
          std::string ("tcp://127.0.0.1:")
          + std::to_string (async_connect_acceptor.local_endpoint ().port ());
        callback_latch_t async_connect_server_latch;
        /* accept한 소켓은 이 블록이 끝날 때까지 살아 있어야 한다. accept 스레드의
     * 지역 변수로 두면 스레드가 끝나는 즉시 연결이 닫히고, 클라이언트가 EOF로
     * disconnected가 되어 is_connected() 검사와 경합한다. */
        boost::asio::ip::tcp::socket async_connect_accepted (async_connect_io);
        joining_thread_t async_connect_server_thread ([&] {
            boost::system::error_code error;
            async_connect_acceptor.accept (async_connect_accepted, error);
            async_connect_server_latch.signal ();
        });
        lifecycle_options.endpoint = async_connect_endpoint;
        auto async_connect_connector =
          zlink::stream_connector::connector_factory_t::create (lifecycle_options);
        auto async_connect = std::make_shared<callback_outcome_t> ();
        const auto async_connect_started = std::chrono::steady_clock::now ();
        async_connect_connector.connect (
          [async_connect] (zlink::stream_connector::result_t<void> result) {
              async_connect->seen = static_cast<bool> (result);
              async_connect->latch.signal ();
          });
        const auto async_connect_submit_elapsed =
          std::chrono::steady_clock::now () - async_connect_started;
        if (async_connect_submit_elapsed > std::chrono::milliseconds (50)) {
            async_connect_connector.close ();
            boost::system::error_code ignored;
            async_connect_acceptor.close (ignored);
            async_connect_server_thread.join ();
            return fail (99);
        }
        /* 비차단 여부는 위의 submit 경과 시간이 단언한다. 여기서는 완료 콜백이
     * 실제로 오는지만 본다. 완료 대기에 짧은 시한을 걸면 부하가 걸린 머신에서
     * 그대로 flake가 된다. */
        if (!async_connect->latch.wait_for (std::chrono::seconds (5)) || !async_connect->seen
            || !async_connect_connector.is_connected ()) {
            async_connect_connector.close ();
            boost::system::error_code ignored;
            async_connect_acceptor.close (ignored);
            async_connect_server_thread.join ();
            return fail (100);
        }
        async_connect_connector.close ();
        async_connect_server_latch.wait_for (std::chrono::milliseconds (100));
        async_connect_server_thread.join ();

        boost::asio::io_context websocket_connect_io;
        boost::asio::ip::tcp::acceptor websocket_connect_acceptor (
          websocket_connect_io, {boost::asio::ip::make_address ("127.0.0.1"), 0});
        const auto websocket_connect_endpoint =
          std::string ("ws://127.0.0.1:")
          + std::to_string (websocket_connect_acceptor.local_endpoint ().port ()) + "/stream";
        callback_latch_t websocket_connect_server_latch;
        callback_latch_t websocket_connect_close_latch;
        joining_thread_t websocket_connect_server_thread ([&] {
            auto accepted = std::make_shared<boost::asio::ip::tcp::socket> (websocket_connect_io);
            boost::system::error_code accept_error;
            websocket_connect_acceptor.async_accept (*accepted,
                                                     [&] (boost::system::error_code error) {
                                                         accept_error = error;
                                                         websocket_connect_server_latch.signal ();
                                                     });
            websocket_connect_io.run_for (std::chrono::milliseconds (200));
            if (accept_error || !accepted->is_open ()) {
                websocket_connect_server_latch.signal ();
                websocket_connect_close_latch.wait_for (std::chrono::seconds (1));
                return;
            }
            boost::beast::websocket::stream<boost::asio::ip::tcp::socket> websocket (
              std::move (*accepted));
            boost::system::error_code error;
            websocket.accept (error);
            websocket_connect_server_latch.signal ();
            // Keep the peer open until the client observes the successful callback.
            websocket_connect_close_latch.wait_for (std::chrono::seconds (1));
            boost::system::error_code ignored;
            boost::beast::get_lowest_layer (websocket).close (ignored);
        });
        zlink::stream_connector::connector_options_t websocket_connect_options;
        websocket_connect_options.endpoint = websocket_connect_endpoint;
        websocket_connect_options.transport = zlink::stream_connector::transport_t::websocket;
        websocket_connect_options.dispatch_mode =
          zlink::stream_connector::dispatch_mode_t::immediate;
        auto websocket_connect_connector =
          zlink::stream_connector::connector_factory_t::create (websocket_connect_options);
        auto websocket_connect = std::make_shared<callback_outcome_t> ();
        const auto websocket_connect_started = std::chrono::steady_clock::now ();
        websocket_connect_connector.connect (
          [websocket_connect] (zlink::stream_connector::result_t<void> result) {
              websocket_connect->seen = static_cast<bool> (result);
              websocket_connect->latch.signal ();
          });
        const auto websocket_connect_submit_elapsed =
          std::chrono::steady_clock::now () - websocket_connect_started;
        if (websocket_connect_submit_elapsed > std::chrono::milliseconds (50)) {
            websocket_connect_close_latch.signal ();
            websocket_connect_connector.close ();
            boost::system::error_code ignored;
            websocket_connect_acceptor.close (ignored);
            websocket_connect_server_thread.join ();
            return fail (101);
        }
        // The elapsed assertion above checks that connect() submits asynchronously.
        // Allow the server thread enough time to be scheduled before judging the
        // separate completion callback contract on a loaded test host.
        if (!websocket_connect->latch.wait_for (std::chrono::seconds (1))
            || !websocket_connect->seen || !websocket_connect_connector.is_connected ()) {
            websocket_connect_close_latch.signal ();
            websocket_connect_connector.close ();
            boost::system::error_code ignored;
            websocket_connect_acceptor.close (ignored);
            websocket_connect_server_thread.join ();
            return fail (102);
        }
        websocket_connect_close_latch.signal ();
        websocket_connect_connector.close ();
        websocket_connect_server_latch.wait_for (std::chrono::milliseconds (100));
        websocket_connect_server_thread.join ();
    }

    return 0;
}

int verify_public_close (
  zlink::context_t &context,
  const zlink::stream_connector::connector_options_t &options,
  zlink::stream_connector::connector_t &connector,
  zlink::stream_connector::detail::connector_runtime_t &runtime,
  zlink::stream_connector::result_t<stream_connector_fixture::login_reply_t> &request,
  zlink::stream_connector::connector_t &immediate)
{
    /* SC §7/§12: close delivers Closed state before disconnection, but does not
     * wait for the blocked state handler to finish. Verify both dispatch modes. */
    for (const auto mode : {zlink::stream_connector::dispatch_mode_t::manual,
                            zlink::stream_connector::dispatch_mode_t::immediate}) {
        zlink::stream_connector::connector_options_t close_handler_options;
        close_handler_options.dispatch_mode = mode;
        auto close_handler_connector =
          zlink::stream_connector::connector_factory_t::create (close_handler_options);
        // Handlers own their state: they may run after this frame ended.
        struct close_handler_state_t
        {
            callback_latch_t closed_state_started;
            callback_latch_t release_closed_state;
            callback_latch_t closed_state_completed;
            callback_latch_t disconnected_started;
            std::atomic_bool closed_state_finished{false};
            std::atomic_bool disconnected_reason_valid{false};
            std::atomic_bool disconnected_ordered{false};
            std::atomic_int closed_state_calls{0};
            std::atomic_int disconnected_calls{0};
        };
        auto close_handler = std::make_shared<close_handler_state_t> ();
        auto close_handler_state_subscription =
          close_handler_connector.on_connection_state_changed ([close_handler] (const auto &state) {
              if (state.current != zlink::stream_connector::connection_state_t::closed) {
                  return;
              }
              ++close_handler->closed_state_calls;
              close_handler->closed_state_started.signal ();
              close_handler->release_closed_state.wait_for (std::chrono::seconds (2));
              close_handler->closed_state_finished.store (true, std::memory_order_release);
              close_handler->closed_state_completed.signal ();
          });
        auto close_handler_disconnected_subscription = close_handler_connector.on_disconnected (
          [close_handler] (std::optional<zlink::stream_connector::close_reason_t> reason) {
              ++close_handler->disconnected_calls;
              close_handler->disconnected_reason_valid.store (
                reason == zlink::stream_connector::close_reason_t::client_close,
                std::memory_order_release);
              close_handler->disconnected_ordered.store (
                close_handler->closed_state_finished.load (std::memory_order_acquire),
                std::memory_order_release);
              close_handler->disconnected_started.signal ();
          });
        auto close_handler_future =
          std::async (std::launch::async, [&] { return close_handler_connector.close (); });
        std::future<zlink::stream_connector::result_t<void>> manual_pump;
        if (mode == zlink::stream_connector::dispatch_mode_t::manual) {
            if (close_handler_future.wait_for (std::chrono::milliseconds (100))
                  != std::future_status::ready
                || close_handler->closed_state_calls.load () != 0
                || close_handler->disconnected_calls.load () != 0) {
                close_handler->release_closed_state.signal ();
                (void) close_handler_future.get ();
                return fail (369);
            }
            manual_pump =
              std::async (std::launch::async, [&] { return close_handler_connector.dispatch (); });
        }
        if (!close_handler->closed_state_started.wait_for (std::chrono::milliseconds (100))
            || close_handler_future.wait_for (std::chrono::milliseconds (100))
                 != std::future_status::ready
            || close_handler->closed_state_finished.load (std::memory_order_acquire)
            || close_handler->disconnected_calls.load () != 0) {
            close_handler->release_closed_state.signal ();
            (void) close_handler_future.get ();
            return fail (369);
        }
        const auto close_handler_result = close_handler_future.get ();
        close_handler->release_closed_state.signal ();
        if (mode == zlink::stream_connector::dispatch_mode_t::manual) {
            const auto first_pump = manual_pump.get ();
            if (!first_pump || close_handler->closed_state_calls.load () != 1
                || close_handler->disconnected_calls.load () != 0) {
                return fail (257);
            }
            close_handler_connector.dispatch ();
        }
        if (!close_handler_result
            || !close_handler->closed_state_completed.wait_for (std::chrono::milliseconds (100))
            || !close_handler->disconnected_started.wait_for (std::chrono::milliseconds (100))
            || !close_handler->closed_state_finished.load (std::memory_order_acquire)
            || !close_handler->disconnected_reason_valid.load (std::memory_order_acquire)
            || !close_handler->disconnected_ordered.load (std::memory_order_acquire)
            || close_handler->closed_state_calls.load () != 1
            || close_handler->disconnected_calls.load () != 1) {
            return fail (257);
        }
    }

    auto disconnected = connector.close ();
    if (!disconnected
        || connector.state () != zlink::stream_connector::connection_state_t::closed) {
        return fail (12);
    }

    connector.send (login_request_t{}).packet_name ("after.close").submit ();
    auto request_after_close = std::make_shared<callback_outcome_t> ();
    connector.request (login_request_t{})
      .packet_name ("after.close.request")
      .submit<login_reply_t> ([request_after_close] (
                                zlink::stream_connector::result_t<login_reply_t> result) {
          request_after_close->seen =
            !result && result.error_code () == zlink::stream_connector::error_code_t::disconnected;
          request_after_close->latch.signal ();
      });
    dispatch_until (connector, request_after_close->latch, std::chrono::milliseconds (100));
    if (!request_after_close->seen) {
        return fail (59);
    }
    auto wait_after_close = std::make_shared<callback_outcome_t> ();
    connector.wait_for<zlink::stream_connector::packet_t> ("after.close.wait")
      .timeout (std::chrono::milliseconds (5))
      .submit ([wait_after_close] (zlink::stream_connector::result_t<packet_message_t> result) {
          wait_after_close->seen =
            !result && result.error_code () == zlink::stream_connector::error_code_t::disconnected;
          wait_after_close->latch.signal ();
      });
    dispatch_until (connector, wait_after_close->latch, std::chrono::milliseconds (100));
    if (!wait_after_close->seen) {
        return fail (158);
    }
    auto missing_endpoint = zlink::stream_connector::connector_factory_t::create (
      zlink::stream_connector::connector_options_t{});
    if (missing_endpoint.connect ()
        || missing_endpoint.connect ().error_code ()
             != zlink::stream_connector::error_code_t::validation_failed
        || missing_endpoint.state () != zlink::stream_connector::connection_state_t::created
        || missing_endpoint.close_reason ()) {
        return fail (14);
    }

    return verify_automatic_codec (context, options, connector, runtime, request, immediate);
}

int verify_close_during_error_callback (zlink::stream_connector::connector_t &immediate)
{
    /* SC §7 and LC:60: close does not wait for a running error handler.
     * Immediate close callbacks run in the close operation; Manual pumps each one. */
    for (const auto mode : {zlink::stream_connector::dispatch_mode_t::manual,
                            zlink::stream_connector::dispatch_mode_t::immediate}) {
        for (const bool close_inside : {false, true}) {
            const bool immediate = mode == zlink::stream_connector::dispatch_mode_t::immediate;
            if (close_inside && !immediate) {
                continue;
            }
            zlink::stream_connector::connector_options_t close_options;
            close_options.dispatch_mode = mode;
            close_options.reconnect.enabled = false;
            auto close_connector =
              zlink::stream_connector::connector_factory_t::create (close_options);
            auto state =
              std::static_pointer_cast<zlink::stream_connector::detail::connector_state_t> (
                connector_internal_handle (close_connector));
            state->connection = std::make_shared<async_write_connection_t> ();
            zlink::stream_connector::detail::change_state (
              state, zlink::stream_connector::connection_state_t::connected);
            if (!immediate) {
                (void) close_connector.dispatch ();
            }
            std::atomic_int closed_calls{0};
            std::atomic_int disconnect_calls{0};
            callback_latch_t close_callbacks_ran;
            callback_latch_t blocker_entered;
            callback_latch_t inside_close_returned;
            callback_latch_t blocker_release;
            std::atomic_bool blocker_finished{false};
            std::atomic_bool blocker_released{false};
            std::atomic_int blocker_calls{0};
            auto closed_subscription =
              close_connector.on_connection_state_changed ([&] (const auto &change) {
                  if (change.current == zlink::stream_connector::connection_state_t::closed) {
                      ++closed_calls;
                  }
              });
            auto disconnect_subscription = close_connector.on_disconnected ([&] (const auto &) {
                ++disconnect_calls;
                close_callbacks_ran.signal ();
            });
            /* Verify that a handler in another receive context does not block close. */
            auto blocker_subscription = close_connector.on_error ([&] (const auto &) {
                ++blocker_calls;
                if (close_inside) {
                    (void) close_connector.close ();
                    inside_close_returned.signal ();
                }
                blocker_entered.signal ();
                blocker_released = blocker_release.wait_for (std::chrono::seconds (10));
                blocker_finished = true;
            });
            joining_thread_t publisher ([&] {
                if (immediate) {
                    zlink::stream_connector::detail::publish_error (
                      *state, {zlink::stream_connector::error_code_t::remote_error, "blocker"});
                }
            });
            if (immediate) {
                if (!blocker_entered.wait_for (std::chrono::seconds (5))) {
                    blocker_release.signal ();
                    return fail (360);
                }
            }
            callback_latch_t outside_close_returned;
            std::atomic_int closed_at_return{-1};
            std::atomic_int disconnected_at_return{-1};
            std::atomic_bool close_succeeded{false};
            std::atomic_bool state_closed_at_return{false};
            joining_thread_t closer ([&] {
                close_succeeded = static_cast<bool> (close_connector.close ());
                state_closed_at_return =
                  close_connector.state () == zlink::stream_connector::connection_state_t::closed;
                closed_at_return = closed_calls.load ();
                disconnected_at_return = disconnect_calls.load ();
                outside_close_returned.signal ();
            });
            const bool outside_returned =
              outside_close_returned.wait_for (std::chrono::seconds (5));
            const bool inside_returned =
              !close_inside || inside_close_returned.wait_for (std::chrono::seconds (0));
            const bool blocker_running_at_close_return = !blocker_finished.load ();
            blocker_release.signal ();
            closer.join ();
            if (!outside_returned || !inside_returned) {
                return fail (361);
            }
            publisher.join ();
            // Immediate handlers may overlap; Manual invokes none before the pump.
            if (!close_succeeded.load () || !state_closed_at_return.load ()
                || closed_at_return.load () < 0 || closed_at_return.load () > 1
                || disconnected_at_return.load () < 0 || disconnected_at_return.load () > 1
                || (!immediate
                    && (closed_at_return.load () != 0 || disconnected_at_return.load () != 0))
                || (immediate
                    && (!blocker_running_at_close_return || !blocker_released.load ()
                        || !blocker_finished.load ()))
                || blocker_calls.load () != (immediate ? 1 : 0)) {
                return fail (371);
            }
            if (!immediate) {
                (void) close_connector.dispatch ();
                (void) close_connector.dispatch ();
            }
            if (!close_callbacks_ran.wait_for (std::chrono::seconds (5))
                || closed_calls.load () != 1 || disconnect_calls.load () != 1) {
                return fail (362);
            }
            if (!close_connector.close () || closed_calls.load () != 1
                || disconnect_calls.load () != 1) {
                return fail (371);
            }
        }
    }

    return 0;
}

int verify_repeated_close_reason (zlink::stream_connector::connector_t &connector)
{
    /* §6.2: closing an already ended connection retains its last reason. */
    {
        auto connector = zlink::stream_connector::connector_factory_t::create ({});
        auto state = std::static_pointer_cast<zlink::stream_connector::detail::connector_state_t> (
          connector_internal_handle (connector));
        zlink::stream_connector::detail::change_state (
          state, zlink::stream_connector::connection_state_t::connected);
        zlink::stream_connector::detail::change_state (
          state, zlink::stream_connector::connection_state_t::disconnected,
          zlink::stream_connector::error_t{zlink::stream_connector::error_code_t::disconnected,
                                           "transport ended"});
        const auto before = connector.close_reason ();
        (void) connector.close ();
        if (before != zlink::stream_connector::close_reason_t::transport_error
            || connector.close_reason () != before) {
            return fail (404);
        }
    }

    return 0;
}

int verify_close_from_callback (
  zlink::context_t &context,
  zlink::stream_connector::connector_t &connector,
  zlink::stream_connector::detail::connector_runtime_t &runtime,
  zlink::stream_connector::result_t<stream_connector_fixture::login_reply_t> &request,
  zlink::stream_connector::connector_t &immediate)
{
    /* stream-connector §7: a close called inside a callback returns right after
   * starting the close work; a close called outside waits for the result of
   * that close. Neither waits on the other in a cycle. */
    for (const auto mode : {zlink::stream_connector::dispatch_mode_t::manual,
                            zlink::stream_connector::dispatch_mode_t::immediate}) {
        const bool immediate = mode == zlink::stream_connector::dispatch_mode_t::immediate;
        const int deadlock_code = immediate ? 362 : 365;
        zlink::stream_connector::connector_options_t inner_options;
        inner_options.dispatch_mode = mode;
        inner_options.reconnect.enabled = false;
        auto inner_connector = zlink::stream_connector::connector_factory_t::create (inner_options);
        auto state = std::static_pointer_cast<zlink::stream_connector::detail::connector_state_t> (
          connector_internal_handle (inner_connector));
        state->connection = std::make_shared<async_write_connection_t> ();
        zlink::stream_connector::detail::change_state (
          state, zlink::stream_connector::connection_state_t::connected);
        callback_latch_t inner_returned;
        /* Immediate runs the callback on the dispatch context, Manual on the
     * thread that pumps. */
        auto inner_state_subscription =
          inner_connector.on_connection_state_changed ([] (const auto &) {});
        auto inner_subscription = inner_connector.on_error ([&] (const auto &) {
            (void) inner_connector.close ();
            inner_returned.signal ();
        });
        callback_latch_t outside_done;
        std::thread outside ([&] {
            zlink::stream_connector::detail::publish_error (
              *state, {zlink::stream_connector::error_code_t::remote_error, "close inside"});
            if (!immediate) {
                (void) inner_connector.dispatch ();
            }
            if (inner_returned.wait_for (std::chrono::seconds (2))) {
                (void) inner_connector.close ();
                outside_done.signal ();
            }
        });
        if (!outside_done.wait_for (std::chrono::seconds (3))) {
            outside.detach ();
            std::_Exit (deadlock_code % 256);
        }
        outside.join ();
        if (inner_connector.state () != zlink::stream_connector::connection_state_t::closed) {
            return deadlock_code;
        }
    }

    boost::asio::io_context partial_io;
    boost::asio::ip::tcp::acceptor partial_acceptor (
      partial_io, {boost::asio::ip::make_address ("127.0.0.1"), 0});
    const auto partial_endpoint = std::string ("tcp://127.0.0.1:")
                                  + std::to_string (partial_acceptor.local_endpoint ().port ());
    std::atomic_bool partial_write_seen{false};
    joining_thread_t partial_server_thread ([&partial_acceptor, &partial_write_seen] {
        boost::asio::ip::tcp::socket socket (partial_acceptor.get_executor ());
        partial_acceptor.accept (socket);
        std::array<char, 256> request_buffer{};
        boost::system::error_code error;
        socket.read_some (boost::asio::buffer (request_buffer), error);
        if (error) {
            return;
        }
        const auto frame = make_server_frame (zlink::stream_connector::message_kind_t::send, 0,
                                              "server.partial", "split")
                             .to_string ();
        socket.write_some (boost::asio::buffer (frame.data (), 3), error);
        if (error) {
            return;
        }
        callback_latch_t partial_frame_delay;
        partial_frame_delay.wait_for (std::chrono::milliseconds (2));
        socket.write_some (boost::asio::buffer (frame.data () + 3, frame.size () - 3), error);
        partial_write_seen = !error;
    });
    zlink::stream_connector::connector_options_t partial_options;
    partial_options.endpoint = partial_endpoint;
    partial_options.dispatch_mode = zlink::stream_connector::dispatch_mode_t::manual;
    auto partial_connector = zlink::stream_connector::connector_factory_t::create (partial_options);
    if (!partial_connector.connect ()) {
        return fail (64);
    }
    partial_connector.send (login_request_t{}).packet_name ("partial.trigger").submit ();
    auto partial_packet =
      partial_connector.wait_for ("server.partial", std::chrono::milliseconds (100));
    partial_server_thread.join ();
    if (!partial_packet) {
        return partial_packet.error_code () == zlink::stream_connector::error_code_t::disconnected
                 ? 66
                 : 312;
    }
    if (!partial_write_seen) {
        return fail (310);
    }
    if (partial_packet.value ().name != "server.partial"
        || as_string (partial_packet.value ().payload) != "split") {
        return fail (311);
    }
    partial_connector.close ();

    return verify_receive_frame_limits (context, connector, runtime, request);
}

int verify_connection_option_validation (zlink::context_t &context)
{
    /* stream-connector §6.3: every option item is checked before a connection
   * is made, and only the attempt fails. */
    {
        struct invalid_option_case_t
        {
            void (*apply) (zlink::stream_connector::connector_options_t &);
            zlink::stream_connector::error_code_t expected;
        };
        const invalid_option_case_t invalid_option_cases[] = {
          {[] (zlink::stream_connector::connector_options_t &o) {
               o.request_timeout = std::chrono::milliseconds (0);
           },
           zlink::stream_connector::error_code_t::validation_failed},
          {[] (zlink::stream_connector::connector_options_t &o) {
               o.wait_timeout = std::chrono::milliseconds (-1);
           },
           zlink::stream_connector::error_code_t::validation_failed},
          {[] (zlink::stream_connector::connector_options_t &o) {
               o.heartbeat.interval = std::chrono::milliseconds (0);
           },
           zlink::stream_connector::error_code_t::validation_failed},
          {[] (zlink::stream_connector::connector_options_t &o) {
               o.heartbeat.timeout = std::chrono::milliseconds (0);
           },
           zlink::stream_connector::error_code_t::validation_failed},
          {[] (zlink::stream_connector::connector_options_t &o) {
               o.heartbeat.enabled = false;
               o.heartbeat.interval = std::chrono::milliseconds (0);
           },
           zlink::stream_connector::error_code_t::validation_failed},
          {[] (zlink::stream_connector::connector_options_t &o) {
               o.reconnect.initial_delay = std::chrono::milliseconds (0);
           },
           zlink::stream_connector::error_code_t::validation_failed},
          {[] (zlink::stream_connector::connector_options_t &o) {
               o.reconnect.enabled = false;
               o.reconnect.initial_delay = std::chrono::milliseconds (0);
           },
           zlink::stream_connector::error_code_t::validation_failed},
          {[] (zlink::stream_connector::connector_options_t &o) {
               o.reconnect.enabled = false;
               o.reconnect.backoff_factor = std::numeric_limits<double>::infinity ();
           },
           zlink::stream_connector::error_code_t::validation_failed},
          {[] (zlink::stream_connector::connector_options_t &o) { o.reconnect.max_attempts = 0; },
           zlink::stream_connector::error_code_t::validation_failed},
          {[] (zlink::stream_connector::connector_options_t &o) { o.max_send_payload_size = 0; },
           zlink::stream_connector::error_code_t::validation_failed},
          {[] (zlink::stream_connector::connector_options_t &o) { o.max_receive_payload_size = 0; },
           zlink::stream_connector::error_code_t::validation_failed},
          {[] (zlink::stream_connector::connector_options_t &o) {
               o.compression = zlink::stream_connector::compression_t::none;
               o.compression_codec = std::make_shared<prefix_compression_codec_t> ("x");
           },
           zlink::stream_connector::error_code_t::configuration_error}};
        for (const auto &invalid_case : invalid_option_cases) {
            zlink::stream_connector::connector_options_t invalid_options;
            invalid_options.endpoint = "tcp://127.0.0.1:1";
            invalid_case.apply (invalid_options);
            auto invalid_connector =
              zlink::stream_connector::connector_factory_t::create (invalid_options);
            auto rejected = invalid_connector.connect ();
            if (rejected || rejected.error_code () != invalid_case.expected
                || invalid_connector.state ()
                     != zlink::stream_connector::connection_state_t::created
                || invalid_connector.close_reason ()) {
                return fail (224);
            }
        }
        /* §6.3: positive option values have no ordering constraint. */
        zlink::stream_connector::connector_options_t positive_options;
        positive_options.endpoint = "tcp://127.0.0.1:1";
        positive_options.connect_timeout = std::chrono::milliseconds (1);
        positive_options.heartbeat.interval = std::chrono::milliseconds (5000);
        positive_options.heartbeat.timeout = std::chrono::milliseconds (1000);
        positive_options.reconnect.initial_delay = std::chrono::milliseconds (1000);
        positive_options.reconnect.max_delay = std::chrono::milliseconds (100);
        positive_options.reconnect.backoff_factor = 0.5;
        auto positive_connector =
          zlink::stream_connector::connector_factory_t::create (positive_options);
        auto connection_result = positive_connector.connect ();
        if (connection_result.error_code ()
              == zlink::stream_connector::error_code_t::validation_failed
            || connection_result.error_code ()
                 == zlink::stream_connector::error_code_t::configuration_error) {
            return fail (301);
        }
        /* Compression off with only the built-in default left in place is not a
     * disagreement: the connector drops the default. */
        zlink::stream_connector::connector_options_t compression_off;
        compression_off.endpoint = "tcp://127.0.0.1:1";
        compression_off.compression = zlink::stream_connector::compression_t::none;
        if (zlink::stream_connector::connector_factory_t::create (compression_off)
              .options ()
              .compression_codec) {
            return fail (225);
        }
    }

    /* stream-connector §6.2: the close reason is readable at any time and is
   * recorded even when the first connect never succeeded. */
    {
        zlink::stream_connector::connector_t fresh_connector;
        if (fresh_connector.close_reason ()) {
            return fail (226);
        }
        zlink::stream_connector::connector_options_t refused_options;
        refused_options.endpoint = "tcp://127.0.0.1:1";
        refused_options.connect_timeout = std::chrono::milliseconds (150);
        refused_options.reconnect.enabled = false;
        auto refused = zlink::stream_connector::connector_factory_t::create (refused_options);
        const auto refused_result = refused.connect ();
        if (refused_result
            || refused.close_reason ()
                 != zlink::stream_connector::close_reason_t::transport_error) {
            return fail (227);
        }

        zlink::stream_socket_t close_reason_server (context);
        close_reason_server.options ().recv_mode (zlink::stream_recv_mode_t::raw);
        close_reason_server.options ().notify (false);
        close_reason_server.bind ("tcp://127.0.0.1:0");
        zlink::stream_connector::connector_options_t close_reason_options;
        close_reason_options.endpoint = close_reason_server.options ().last_endpoint ();
        auto close_reason_connector =
          zlink::stream_connector::connector_factory_t::create (close_reason_options);
        if (!close_reason_connector.connect ()) {
            return fail (228);
        }
        if (close_reason_connector.close_reason ()) {
            return fail (229);
        }
        close_reason_connector.close ();
        if (close_reason_connector.close_reason ()
            != zlink::stream_connector::close_reason_t::client_close) {
            return fail (231);
        }
    }

    return 0;
}
} // namespace stream_connector_tests
