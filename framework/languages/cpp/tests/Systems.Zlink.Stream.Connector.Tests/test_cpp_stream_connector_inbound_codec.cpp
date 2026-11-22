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

using namespace stream_connector_fixture;

namespace
{
class observing_compression_codec_t final : public zlink::stream_connector::compression_codec_t
{
  public:
    std::function<void (const std::vector<std::uint8_t> &)> observe;

    std::vector<std::uint8_t> compress (const std::vector<std::uint8_t> &payload) const override
    {
        return zlink::stream_connector::lz4_compression_codec ()->compress (payload);
    }

    std::vector<std::uint8_t> decompress (const std::vector<std::uint8_t> &payload,
                                          std::size_t max_decompressed_size) const override
    {
        observe (payload);
        return zlink::stream_connector::lz4_compression_codec ()->decompress (
          payload, max_decompressed_size);
    }
};
} // namespace

namespace stream_connector_tests
{
int test_inbound_codec (const std::string &mode)
{
    using namespace zlink::stream_connector;
    std::promise<void> finished;
    auto done = finished.get_future ();
    std::thread watchdog ([&] {
        if (done.wait_for (std::chrono::seconds (5)) != std::future_status::ready) {
            std::cerr << "inbound codec watchdog: " << mode << " did not finish\n";
            std::_Exit (1);
        }
    });
    auto codec = std::make_shared<observing_compression_codec_t> ();
    connector_options_t options;
    options.compression_codec = codec;
    options.dispatch_mode = dispatch_mode_t::manual;
    options.heartbeat.enabled = false;
    options.reconnect.enabled = false;
    auto connector = connector_factory_t::create (options);
    auto state =
      std::static_pointer_cast<detail::connector_state_t> (connector_internal_handle (connector));
    auto connection = std::make_shared<fed_read_connection_t> ();
    state->connection = connection;
    detail::change_state (state, connection_state_t::connected);
    detail::start_read_loop (state);
    std::atomic_int decoded{0};
    std::promise<void> first_entered;
    auto entered = first_entered.get_future ();
    std::promise<void> release_first;
    auto release = release_first.get_future ().share ();
    codec->observe = [&] (const auto &) {
        (void) connector.pending_dispatch_count ();
        const auto index = ++decoded;
        if ((mode == "codec-order" || mode == "codec-reconnect") && index == 1) {
            first_entered.set_value ();
            release.wait ();
        }
        if (mode == "codec-close") {
            (void) connector.close ();
        }
    };
    bool passed = true;
    if (mode == "codec-close") {
        const std::string bound_payload{1, 0, 7, 5, 'a', 'c', 't', 'o', 'r'};
        connection->feed (
          make_server_frame (message_kind_t::control, 0, "$zlink.actor.bound", bound_payload));
        connection->feed (
          make_server_frame (message_kind_t::send, 0, "codec.close", "closing", true, 7));
        passed = decoded == 1 && connector.state () == connection_state_t::closed
                 && state->actors_by_slot.empty () && state->actors_by_id.empty ();
    } else if (mode == "codec-reconnect") {
        const auto old_frame =
          make_server_frame (message_kind_t::send, 0, "old.frame", "old", true);
        const auto trailing = make_server_frame (message_kind_t::send, 0, "old.partial", "x");
        const auto old_batch =
          zlink::message_t::from (old_frame.to_string () + trailing.to_string ().substr (0, 3));
        auto reading = std::async (std::launch::async, [&] { connection->feed (old_batch); });
        entered.wait ();
        detail::connection_ended (state, {error_code_t::disconnected, "old connection ended"},
                                  connection);
        auto next_connection = std::make_unique<fed_read_connection_t> ();
        auto *next_reader = next_connection.get ();
        passed = detail::change_state (state, connection_state_t::connected, std::nullopt,
                                       std::move (next_connection));
        detail::start_read_loop (state);
        next_reader->feed (make_server_frame (message_kind_t::send, 0, "new.frame", "new"));
        auto next = connector.wait_for ("new.frame", std::chrono::seconds (1));
        passed = passed && next && next.value ().payload == as_bytes ("new")
                 && connector.received_count ("new.frame") == 1
                 && connector.received_count ("old.frame") == 0;
        release_first.set_value ();
        reading.get ();
    } else {
        const auto batch = zlink::message_t::from (
          make_server_frame (message_kind_t::send, 0, "codec.order", "1", true).to_string ()
          + make_server_frame (message_kind_t::send, 0, "codec.order", "2", true).to_string ());
        if (mode == "codec-order") {
            auto reading = std::async (std::launch::async, [&] { connection->feed (batch); });
            entered.wait ();
            connection->feed (
              make_server_frame (message_kind_t::send, 0, "codec.order", "3", true));
            // Request submission also invokes this pump while a read is being decoded.
            detail::start_read_loop (state);
            passed = decoded == 1;
            release_first.set_value ();
            reading.get ();
        } else {
            connection->feed (batch);
        }
        const int expected = mode == "codec-order" ? 3 : 2;
        for (int index = 1; index <= expected; ++index) {
            auto packet = connector.wait_for ("codec.order", std::chrono::seconds (1));
            passed =
              passed && packet && packet.value ().payload == as_bytes (std::to_string (index));
        }
        passed = passed && decoded == expected;
    }
    (void) connector.close ();
    if (mode == "codec-reconnect") {
        // A closed connector cannot install another connection. The next
        // connection reset above belongs to reconnect, not to close work.
        passed = passed
                 && !detail::change_state (state, connection_state_t::connected, std::nullopt,
                                           std::make_unique<fed_read_connection_t> ());
    }
    finished.set_value ();
    watchdog.join ();
    std::cout << mode << ": " << (passed ? "passed" : "failed") << '\n';
    return passed ? 0 : 1;
}
} // namespace stream_connector_tests
