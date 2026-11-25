/* SPDX-License-Identifier: Elastic-2.0 */
#include "../support/allocation_counter.hpp"
#include "../support/stream_connector_scripted_transport.hpp"
#include "../support/stream_connector_wire_frames.hpp"
#include "runtime/connector_runtime.hpp"

#include <zlink/stream_connector.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <utility>
#include <vector>

int main ()
{
    namespace sc = zlink::stream_connector;
    using namespace stream_connector_fixture;
    using namespace std::chrono_literals;

    std::atomic_bool succeeded{false};
    sc::connector_options_t options;
    options.dispatch_mode = sc::dispatch_mode_t::immediate;
    options.reconnect.enabled = false;
    auto connector = sc::connector_factory_t::create (options);
    auto state = std::static_pointer_cast<sc::detail::connector_state_t> (
      connector_internal_handle (connector));
    auto transport = std::make_shared<fed_read_connection_t> ();
    state->connection = transport;
    sc::detail::change_state (state, sc::connection_state_t::connected);
    sc::detail::start_read_loop (state);

    using reply_t = std::vector<std::uint8_t>;
    const auto complete = [&succeeded] (sc::result_t<reply_t> result) {
        succeeded = static_cast<bool> (result);
    };

    auto submit = [&] {
        connector.request (sc::packet_t{.name = "request"}).timeout (5s).submit<reply_t> (complete);
    };
    auto reply_frame = [] (std::uint64_t seq) {
        return make_server_frame (sc::message_kind_t::response, seq, "reply", "");
    };

    // Warm up the connector before measuring submission and reply handling on this thread.
    submit ();
    if (!transport->wait_for_written (0)) {
        connector.close ();
        return 1;
    }
    transport->feed (reply_frame (1));
    if (!succeeded.load ()) {
        connector.close ();
        return 1;
    }

    succeeded = false;
    const auto submit_allocations = zlink::framework::tests::count_allocations (submit);
    if (!transport->wait_for_written (1)) {
        connector.close ();
        return 1;
    }
    const auto response = reply_frame (2).to_string ();
    std::vector<std::uint8_t> bytes (response.begin (), response.end ());
    const auto reply_allocations =
      zlink::framework::tests::count_allocations ([&] { transport->feed (std::move (bytes)); });
    connector.close ();

    std::cout << "Ordinary Request allocations: submit=" << submit_allocations
              << " reply=" << reply_allocations << '\n';
    // Measured using the same test against origin/main (6752099006).
    constexpr std::size_t main_request_allocations = 16;
    return succeeded.load () && submit_allocations + reply_allocations <= main_request_allocations
             ? 0
             : 1;
}
