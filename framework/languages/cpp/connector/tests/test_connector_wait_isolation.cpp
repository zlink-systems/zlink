/* SPDX-License-Identifier: Apache-2.0 */
#include <zlink/stream_connector.hpp>
#include "runtime/protocol/header_codec.hpp"
#include "runtime/protocol/framing/frame_codec.hpp"
#include <boost/asio.hpp>
#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <string>

using namespace zlink::stream_connector;
using tcp = boost::asio::ip::tcp;

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
    auto same = connector_factory_t::create (options);
    auto other = connector_factory_t::create (options);
    if (!same.connect () || !other.connect ()) {
        std::cerr << "wait isolation fixture could not connect\n";
        return 1;
    }
    tcp::socket same_peer (peer_io);
    tcp::socket other_peer (peer_io);
    acceptor.accept (same_peer);
    acceptor.accept (other_peer);
    auto send = [&] (tcp::socket &socket, const std::string &name) {
        detail::stream_header_t header;
        header.name = name;
        const auto encoded = detail::header_codec_t{}.encode (header);
        const auto frame = detail::frame_codec_t::encode (encoded.value (), {'b'}, options);
        boost::asio::write (socket, boost::asio::buffer (frame.value ()));
    };
    std::promise<void> started;
    auto entered = started.get_future ();
    std::promise<void> release;
    auto released = release.get_future ().share ();
    std::promise<void> finished;
    auto completed = finished.get_future ();
    std::atomic_size_t started_count{0};
    std::atomic_size_t finished_count{0};
    std::atomic_size_t failed_results{0};
    // Occupy all four default workers. Wait completion must neither use I/O
    // workers nor serialize the connector's wait callbacks on a strand.
    for (int i = 0; i < 4; ++i) {
        const auto name = "blocked" + std::to_string (i);
        same.wait_for<packet_t> (name).timeout (2s).submit (
          [&, name] (result_t<message_t<packet_t>> result) {
              if (!result || result.value ().payload.name != name)
                  ++failed_results;
              if (++started_count == 4)
                  started.set_value ();
              released.wait ();
              if (++finished_count == 4)
                  finished.set_value ();
          });
        send (same_peer, name);
    }
    const bool all_blocked = entered.wait_for (2s) == std::future_status::ready;
    auto same_read = std::async (std::launch::async, [&] { return same.wait_for ("same", 2s); });
    auto other_read = std::async (std::launch::async, [&] { return other.wait_for ("other", 2s); });
    send (same_peer, "same");
    send (other_peer, "other");
    const auto deadline = std::chrono::steady_clock::now () + 2s;
    const bool same_ready = same_read.wait_until (deadline) == std::future_status::ready;
    const bool other_ready = other_read.wait_until (deadline) == std::future_status::ready;
    const bool still_blocked = finished_count.load () == 0;
    // Always release the callbacks, including on the expected pre-fix failure.
    release.set_value ();
    const auto same_result = same_read.get ();
    const auto other_result = other_read.get ();
    const bool all_finished = completed.wait_for (2s) == std::future_status::ready;
    const bool counts = same.received_count ("same") == 1 && other.received_count ("other") == 1;
    same.close ();
    other.close ();
    std::cout << "blocked=" << all_blocked << " same_read=" << same_ready
              << " other_read=" << other_ready << " still_blocked=" << still_blocked
              << " finished=" << all_finished << " counts=" << counts << '\n';
    const bool passed = all_blocked && same_ready && other_ready && still_blocked && all_finished
                        && same_result && other_result && counts && failed_results.load () == 0;
    std::cout << "blocked wait callbacks preserve same/other connector TCP reads: "
              << (passed ? "PASS" : "FAIL") << '\n';
    return passed ? 0 : 1;
}
