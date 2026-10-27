/* SPDX-License-Identifier: Apache-2.0 */
#include <zlink/stream_connector.hpp>
#include "runtime/connector_runtime.hpp"
#include "runtime/protocol/framing.hpp"
#include "../../tests/support/allocation_counter.hpp"

#include <atomic>
#include <iostream>
#include <future>
#include <thread>
#include <vector>

using namespace zlink::stream_connector;

int main (int argc, char **argv)
{
    int failures = 0;
    auto check = [&] (bool value, const char *name) {
        std::cout << name << ": " << (value ? "PASS" : "FAIL") << '\n';
        failures += !value;
    };
    const std::string mode = argc > 1 ? argv[1] : "manual";
    connector_options_t options;
    options.dispatch_mode = (mode == "manual" || mode == "allocations")
                              ? dispatch_mode_t::manual
                              : dispatch_mode_t::immediate;
    auto connector = connector_factory_t::create (options);
    auto state =
      std::static_pointer_cast<detail::connector_state_t> (connector_internal_handle (connector));
    if (mode == "close-handoff") {
        auto started = std::make_shared<std::promise<void>> ();
        auto entered = started->get_future ();
        auto release = std::make_shared<std::promise<void>> ();
        auto released = release->get_future ().share ();
        auto finished = std::make_shared<std::promise<void>> ();
        auto delivered = finished->get_future ();
        auto subscription = connector.on_connection_state_changed (
          [started, released, finished] (const connection_state_changed_t &event) {
              if (event.current == connection_state_t::closed) {
                  started->set_value ();
                  released.wait ();
                  finished->set_value ();
              }
          });
        auto closed = std::async (std::launch::async, [&] { return connector.close (); });
        const bool callback_started =
          entered.wait_for (std::chrono::seconds (2)) == std::future_status::ready;
        const bool close_finished =
          closed.wait_for (std::chrono::seconds (2)) == std::future_status::ready;
        release->set_value ();
        const auto result = closed.get ();
        check (callback_started && close_finished && static_cast<bool> (result),
               "V17 close completes while its Immediate callback remains blocked");
        check (delivered.wait_for (std::chrono::seconds (2)) == std::future_status::ready,
               "V17 close callback finishes after explicit release");
        return failures ? 1 : 0;
    }
    if (mode == "immediate") {
        const auto receiver = std::this_thread::get_id ();
        auto packet_inline = std::make_shared<std::atomic_bool> (false);
        auto subscription =
          connector.on<packet_t> ("push", [packet_inline, receiver] (const message_t<packet_t> &) {
              packet_inline->store (std::this_thread::get_id () == receiver);
          });
        detail::connector_runtime_t::from (connector).receive_packet (packet_t{"push"});
        check (packet_inline->load (), "V17 packet runs inline on receiver thread");
        auto delivery_inline = std::make_shared<std::atomic_bool> (false);
        detail::schedule_delivery (state, [delivery_inline, receiver] {
            delivery_inline->store (std::this_thread::get_id () == receiver);
        });
        check (delivery_inline->load (), "V17 lifecycle delivery runs inline on deciding thread");
        return failures ? 1 : 0;
    }
    if (mode == "allocations") {
        using zlink::framework::tests::count_allocations;
        std::size_t calls = 0;
        auto first = connector.on<packet_t> ("push", [&] (const auto &) { ++calls; });
        auto second = connector.on<packet_t> ("push", [&] (const auto &) { ++calls; });
        void *probe = nullptr;
        check (count_allocations ([&] { probe = ::operator new (64); }) == 1,
               "allocation counter positive control");
        ::operator delete (probe);
        // Handler selection already copies the registration snapshot. Measure that
        // existing cost separately; the Manual cursor must add no allocations.
        detail::dispatch_envelope_t envelope{packet_t{"push"}};
        const auto selection = count_allocations (
          [&] { auto handlers = detail::select_packet_handlers (*state, envelope); });
        std::size_t extra = 0;
        for (std::size_t i = 0; i < 64; ++i) {
            detail::connector_runtime_t::from (connector).receive_packet (packet_t{"push"});
            const auto allocated = count_allocations ([&] {
                connector.dispatch ();
                connector.dispatch ();
            });
            std::cout << "packet=" << i << " selection=" << selection << " dispatch=" << allocated
                      << '\n';
            extra += allocated != selection;
        }
        check (extra == 0 && calls == 128, "Manual dispatch adds zero allocations per packet");
        return failures ? 1 : 0;
    }
    std::vector<int> calls;
    auto first =
      connector.on<packet_t> ("push", [&] (const message_t<packet_t> &) { calls.push_back (1); });
    auto second =
      connector.on<packet_t> ("push", [&] (const message_t<packet_t> &) { calls.push_back (2); });
    detail::connector_runtime_t::from (connector).receive_packet (packet_t{"push"});
    detail::schedule_delivery (state, [&] { calls.push_back (3); });
    for (int expected = 1; expected <= 3; ++expected) {
        connector.dispatch ();
        check (calls.size () == static_cast<std::size_t> (expected),
               "V20 exactly one callback per pump");
        if (calls.size () >= static_cast<std::size_t> (expected))
            check (calls[expected - 1] == expected, "V20 FIFO across packet handlers and delivery");
    }
    connector.dispatch ();
    check (calls == std::vector<int> ({1, 2, 3}), "V20 empty pump does nothing");
    auto error1 =
      connector.on_error ([&] (const zlink::stream_connector::error_t &) { calls.push_back (4); });
    auto error2 =
      connector.on_error ([&] (const zlink::stream_connector::error_t &) { calls.push_back (5); });
    detail::publish_error (*state, {error_code_t::send_failed, "fixture"});
    connector.dispatch ();
    check (calls == std::vector<int> ({1, 2, 3, 4}), "V20 one error subscriber per pump");
    connector.dispatch ();
    check (calls == std::vector<int> ({1, 2, 3, 4, 5}), "V20 next error subscriber remains queued");
    std::vector<int> events;
    auto state1 = connector.on_connection_state_changed (
      [&] (const connection_state_changed_t &) { events.push_back (1); });
    auto state2 = connector.on_connection_state_changed (
      [&] (const connection_state_changed_t &) { events.push_back (2); });
    detail::change_state (state, connection_state_t::connecting);
    connector.dispatch ();
    check (events == std::vector<int> ({1}), "V20 one lifecycle subscriber per pump");
    state2.unsubscribe ();
    detail::schedule_delivery (state, [&] { events.push_back (3); });
    connector.dispatch ();
    check (events == std::vector<int> ({1, 3}),
           "V20 removed subscriber skipped without consuming pump");
    auto hook1 = connector.on_reply_received (
      [&] (const reply_received_context_t &) { events.push_back (4); });
    auto hook2 = connector.on_reply_received (
      [&] (const reply_received_context_t &) { events.push_back (5); });
    packet_t request_packet;
    request_packet.name = "request";
    connector.request (request_packet)
      .submit<std::vector<std::uint8_t>> (
        [&] (result_t<std::vector<std::uint8_t>>) { events.push_back (6); });
    for (const auto expected : {4, 5, 6}) {
        const auto before = events.size ();
        connector.dispatch ();
        check (events.size () == before + 1 && events.back () == expected,
               "V20 reply hooks and request result each take one pump");
    }
    detail::schedule_delivery (state, [&] {
        events.push_back (7);
        detail::schedule_delivery (state, [&] { events.push_back (8); });
    });
    connector.dispatch ();
    check (events.back () == 7, "V20 callback enqueued during pump waits for next pump");
    connector.dispatch ();
    check (events.back () == 8, "V20 reentrant enqueue is preserved");
    {
        auto cursor_connector = connector_factory_t::create (options);
        auto cursor_state = std::static_pointer_cast<detail::connector_state_t> (
          connector_internal_handle (cursor_connector));
        std::vector<int> order;
        auto retained =
          cursor_connector.on<packet_t> ("cursor", [&] (const auto &) { order.push_back (1); });
        auto removed =
          cursor_connector.on<packet_t> ("cursor", [&] (const auto &) { order.push_back (2); });
        detail::connector_runtime_t::from (cursor_connector).receive_packet (packet_t{"cursor"});
        detail::schedule_delivery (cursor_state, [&] { order.push_back (3); });
        cursor_connector.dispatch ();
        check (order == std::vector<int> ({1}) && cursor_connector.pending_dispatch_count () == 2,
               "V20 cursor retains one handler before the later delivery");
        removed.unsubscribe ();
        auto added =
          cursor_connector.on<packet_t> ("cursor", [&] (const auto &) { order.push_back (4); });
        cursor_connector.dispatch ();
        check (order == std::vector<int> ({1, 3})
                 && cursor_connector.pending_dispatch_count () == 0,
               "V20 cursor skips removed handlers and excludes later registrations");
        detail::connector_runtime_t::from (cursor_connector).receive_packet (packet_t{"cursor"});
        cursor_connector.dispatch ();
        cursor_connector.dispatch ();
        check (order == std::vector<int> ({1, 3, 1, 4}),
               "V20 next packet uses the current handler registrations");
    }
    {
        auto lease_connector = connector_factory_t::create (options);
        auto lease_state = std::static_pointer_cast<detail::connector_state_t> (
          connector_internal_handle (lease_connector));
        auto first_lease = lease_connector.on<packet_t> ("lease", [] (const auto &) {});
        auto last_lease = lease_connector.on<packet_t> ("lease", [] (const auto &) {});
        std::promise<void> started;
        auto entered = started.get_future ();
        std::promise<void> release;
        auto released = release.get_future ().share ();
        bool packet_retained = false;
        {
            std::lock_guard<std::mutex> lock (lease_state->lifecycle_mutex);
            // Observe the core envelope after a concurrent pump takes its last handler.
            lease_state->packet_handlers["lease"][0].handler =
              [&] (const detail::dispatch_envelope_t &envelope) {
                  started.set_value ();
                  released.wait ();
                  packet_retained = envelope.packet.name == "lease";
              };
        }
        detail::connector_runtime_t::from (lease_connector).receive_packet (packet_t{"lease"});
        auto first_pump =
          std::async (std::launch::async, [&] { return lease_connector.dispatch (); });
        const bool callback_started =
          entered.wait_for (std::chrono::seconds (2)) == std::future_status::ready;
        if (callback_started)
            lease_connector.dispatch ();
        release.set_value ();
        const auto result = first_pump.get ();
        check (callback_started && result && packet_retained,
               "V20 concurrent pumps retain the packet until every selected handler returns");
        check (lease_connector.pending_dispatch_count () == 0,
               "V20 completed concurrent cursor has no pending callback");
    }
    return failures ? 1 : 0;
}
