/* SPDX-License-Identifier: Elastic-2.0 */
#include <zlink/stream_connector.hpp>
#include "runtime/protocol/header_codec.hpp"
#include "runtime/protocol/framing/frame_codec.hpp"
#include <boost/asio.hpp>
#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <string>
#include <thread>

using namespace zlink::stream_connector;
using tcp = boost::asio::ip::tcp;

/* stream-connector §10.1.1: a wait that starts while a matching packet is
 * already queued must also see a matching packet that arrives while its
 * predicate runs on the queued ones. The first queued packet is rejected by the
 * predicate, which blocks on a latch until the second packet has been routed by
 * the read pump; the wait must then complete with the second packet instead of
 * registering and timing out with that packet left in the queue. */
int main (int argc, char **argv)
{
    using namespace std::chrono_literals;
    const auto mode = argc > 1 && std::string (argv[1]) == "immediate" ? dispatch_mode_t::immediate
                                                                       : dispatch_mode_t::manual;
    boost::asio::io_context peer_io;
    tcp::acceptor acceptor (peer_io, {boost::asio::ip::address_v4::loopback (), 0});
    connector_options_t options;
    options.endpoint = "tcp://127.0.0.1:" + std::to_string (acceptor.local_endpoint ().port ());
    options.dispatch_mode = mode;
    options.heartbeat.enabled = false;
    options.reconnect.enabled = false;
    auto connector = connector_factory_t::create (options);
    if (!connector.connect ()) {
        std::cerr << "wait queue race fixture could not connect\n";
        return 1;
    }
    tcp::socket peer (peer_io);
    acceptor.accept (peer);
    const auto send = [&] (const std::string &name, std::uint8_t body) {
        detail::stream_header_t header;
        header.name = name;
        const auto encoded = detail::header_codec_t{}.encode (header);
        const auto frame = detail::frame_codec_t::encode (encoded.value (), {body}, options);
        boost::asio::write (peer, boost::asio::buffer (frame.value ()));
    };
    // Bounded spin on an observable count; no timing is assumed.
    const auto received = [&] (std::size_t count) {
        const auto deadline = std::chrono::steady_clock::now () + 5s;
        while (connector.received_count ("target") < count) {
            if (std::chrono::steady_clock::now () > deadline)
                return false;
            std::this_thread::yield ();
        }
        return true;
    };

    send ("target", 'a');
    const bool stale_queued = received (1);

    std::promise<void> entered_promise;
    auto entered = entered_promise.get_future ();
    std::promise<void> release_promise;
    auto released = release_promise.get_future ().share ();
    std::atomic_bool entered_once{false};
    auto waited = std::async (std::launch::async, [&] {
        return connector.wait_for<packet_t> ("target")
          .timeout (2s)
          .where ([&] (const message_t<packet_t> &message) {
              if (message.payload.payload.empty () || message.payload.payload[0] != 'b') {
                  if (!entered_once.exchange (true))
                      entered_promise.set_value ();
                  released.wait ();
                  return false;
              }
              return true;
          })
          .submit ();
    });
    const bool predicate_running = entered.wait_for (5s) == std::future_status::ready;
    send ("target", 'b');
    const bool fresh_routed = received (2);
    // Always release the predicate, including on the expected pre-fix failure.
    release_promise.set_value ();
    const auto result = waited.get ();
    const bool matched = result && result.value ().payload.payload.size () == 1
                         && result.value ().payload.payload[0] == 'b';
    connector.close ();
    std::cout << "stale_queued=" << stale_queued << " predicate_running=" << predicate_running
              << " fresh_routed=" << fresh_routed << " matched=" << matched << '\n';
    const bool passed = stale_queued && predicate_running && fresh_routed && matched;
    std::cout << "wait sees a packet that arrives while its queue predicate runs: "
              << (passed ? "PASS" : "FAIL") << '\n';
    return passed ? 0 : 1;
}
