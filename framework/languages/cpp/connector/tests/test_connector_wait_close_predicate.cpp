/* SPDX-License-Identifier: Elastic-2.0 */
#include <zlink/stream_connector.hpp>
#include "runtime/protocol/header_codec.hpp"
#include "runtime/protocol/framing/frame_codec.hpp"
#include <boost/asio.hpp>
#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <optional>
#include <string>

using namespace zlink::stream_connector;
using tcp = boost::asio::ip::tcp;

namespace
{

bool run_case (dispatch_mode_t mode, bool packet_before_wait)
{
    using namespace std::chrono_literals;

    boost::asio::io_context peer_io;
    tcp::acceptor acceptor (peer_io, {boost::asio::ip::address_v4::loopback (), 0});
    connector_options_t options;
    options.endpoint = "tcp://127.0.0.1:" + std::to_string (acceptor.local_endpoint ().port ());
    options.dispatch_mode = mode;
    options.heartbeat.enabled = false;
    options.reconnect.enabled = false;

    auto connector = connector_factory_t::create (options);
    if (!connector.connect ()) {
        std::cerr << "wait close predicate fixture could not connect\n";
        return false;
    }
    tcp::socket peer (peer_io);
    acceptor.accept (peer);

    const auto send = [&] (const std::string &name) {
        detail::stream_header_t header;
        header.name = name;
        const auto encoded = detail::header_codec_t{}.encode (header);
        const auto frame = detail::frame_codec_t::encode (encoded.value (), {'x'}, options);
        boost::asio::write (peer, boost::asio::buffer (frame.value ()));
    };

    std::promise<bool> close_returned_promise;
    auto close_returned = close_returned_promise.get_future ();
    std::promise<std::optional<error_code_t>> wait_result_promise;
    auto wait_result = wait_result_promise.get_future ();
    std::atomic_size_t predicate_calls{0};

    std::promise<bool> barrier_completed_promise;
    auto barrier_completed = barrier_completed_promise.get_future ();
    if (packet_before_wait) {
        connector.wait_for<packet_t> ("barrier").timeout (5s).submit (
          [&] (result_t<message_t<packet_t>> result) {
              barrier_completed_promise.set_value (result
                                                   && result.value ().payload.name == "barrier");
          });
        send ("target");
        send ("barrier");
        if (barrier_completed.wait_for (5s) != std::future_status::ready
            || !barrier_completed.get ()) {
            std::cerr << "wait close predicate barrier did not complete after target\n";
            (void) connector.close ();
            return false;
        }
        // The registered barrier wait completes only after the preceding target frame
        // has been routed, so this wait starts with the target already queued.
    }

    auto wait = connector.wait_for<packet_t> ("target");
    wait.where ([&] (const message_t<packet_t> &) {
        if (predicate_calls.fetch_add (1, std::memory_order_relaxed) == 0) {
            const auto result = connector.close ();
            close_returned_promise.set_value (static_cast<bool> (result));
        }
        // Closing while evaluating the condition ends the observation as Disconnected.
        return false;
    });
    wait.submit ([&] (result_t<message_t<packet_t>> result) {
        wait_result_promise.set_value (result.error_code ());
    });

    if (!packet_before_wait) {
        // submit(callback) registered the wait synchronously before it returned.
        send ("target");
    }

    if (close_returned.wait_for (5s) != std::future_status::ready) {
        std::cerr << "close in wait predicate did not return\n";
        return false;
    }
    const bool close_returned_successfully = close_returned.get ();
    if (wait_result.wait_for (5s) != std::future_status::ready) {
        std::cerr << "wait did not complete after close in predicate\n";
        return false;
    }
    const auto observed_error = wait_result.get ();

    // A close from the predicate starts the close work and returns; an outside
    // close waits for that work, so this also observes completed termination.
    const auto completed_close = connector.close ();
    const bool ended = !connector.is_connected ();
    const bool passed = close_returned_successfully
                        && predicate_calls.load (std::memory_order_relaxed) == 1
                        && observed_error == error_code_t::disconnected && completed_close && ended;
    if (!passed) {
        std::cerr << "close_returned=" << close_returned_successfully
                  << " predicate_calls=" << predicate_calls.load (std::memory_order_relaxed)
                  << " wait_disconnected=" << (observed_error == error_code_t::disconnected)
                  << " connector_ended=" << ended << '\n';
    }
    return passed;
}

} // namespace

int main (int argc, char **argv)
{
    if (argc != 3) {
        std::cerr << "usage: test_connector_wait_close_predicate <immediate|manual> "
                     "<after-registration|queued-before-registration>\n";
        return 2;
    }
    const std::string mode_argument (argv[1]);
    const std::string order_argument (argv[2]);
    if ((mode_argument != "immediate" && mode_argument != "manual")
        || (order_argument != "after-registration"
            && order_argument != "queued-before-registration")) {
        std::cerr << "invalid wait close predicate test case\n";
        return 2;
    }

    const auto mode =
      mode_argument == "immediate" ? dispatch_mode_t::immediate : dispatch_mode_t::manual;
    const bool packet_before_wait = order_argument == "queued-before-registration";
    const bool passed = run_case (mode, packet_before_wait);
    std::cout << "mode=" << mode_argument << " order=" << order_argument
              << " close in wait predicate returns and disconnects the wait: "
              << (passed ? "PASS" : "FAIL") << '\n';
    return passed ? 0 : 1;
}
