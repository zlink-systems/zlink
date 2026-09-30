/* SPDX-License-Identifier: MPL-2.0 */

#include "support.hpp"

#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <coroutine>
#include <cstdint>
#include <future>
#include <optional>
#include <string>
#include <thread>

namespace
{

class void_task_t
{
  public:
    struct promise_type
    {
        std::promise<void> promise;
        void_task_t get_return_object () { return void_task_t (promise.get_future ()); }
        std::suspend_never initial_suspend () noexcept { return {}; }
        std::suspend_never final_suspend () noexcept { return {}; }
        void return_void () { promise.set_value (); }
        void unhandled_exception () { promise.set_exception (std::current_exception ()); }
    };

    explicit void_task_t (std::future<void> future_) : _future (std::move (future_)) {}
    bool ready () const
    {
        return _future.wait_for (std::chrono::milliseconds (0))
               == std::future_status::ready;
    }
    void get ()
    {
        assert (_future.wait_for (std::chrono::seconds (5)) == std::future_status::ready);
        _future.get ();
    }

  private:
    std::future<void> _future;
};

void_task_t await_send (zlink::async_result_t<void> result_)
{
    co_await std::move (result_);
}

void test_immediately_admitted_async_send ()
{
    zlink::context_t context;
    zlink::pair_socket_t sender (context);
    zlink::pair_socket_t receiver (context);
    const std::string endpoint = zlink_cpp_contract::unique_inproc ("async-ready");
    receiver.bind (endpoint);
    sender.connect (endpoint);

    // Establish the route with a bounded blocking send so this case isolates
    // the ID-0 immediate-admission path without relying on a settle delay.
    sender.options ().send_timeout (std::chrono::seconds (5));
    zlink::message_t probe = zlink_cpp_contract::make_message ("ready");
    sender.send ().message (probe).submit ();
    zlink::received_t received_probe;
    assert (receiver.recv (received_probe) == 0);
    assert (received_probe.is_single_part ());
    assert (received_probe.first_part ().to_string () == "ready");

    zlink::message_t outbound = zlink_cpp_contract::make_message ("async");
    await_send (sender.send ().message (outbound).async ().admitted).get ();
    assert (!outbound.valid ());
    zlink::received_t inbound;
    assert (receiver.recv (inbound) == 0);
    assert (inbound.is_single_part ());
    assert (inbound.first_part ().to_string () == "async");
}

bool has_event (zlink::poll_event_flag_t actual_,
                zlink::poll_event_flag_t expected_)
{
    return (static_cast<short> (actual_) & static_cast<short> (expected_))
           == static_cast<short> (expected_);
}

void test_backpressured_async_send_retries_from_public_poller ()
{
    zlink::context_t context;
    zlink::pair_socket_t sender (context);
    zlink::pair_socket_t receiver (context);
    sender.options ().linger (std::chrono::milliseconds (0));
    receiver.options ().linger (std::chrono::milliseconds (0));
    sender.options ().immediate (true);
    sender.options ().send_timeout (std::chrono::milliseconds (-1));

    constexpr size_t payload_size = 64;
    const uint64_t hwm = 512;
    sender.options ().send_hwm (zlink::byte_count_t::bytes (hwm));
    receiver.options ().recv_hwm (zlink::byte_count_t::bytes (hwm));

    const std::string endpoint =
      zlink_cpp_contract::unique_inproc ("async-backpressure-ready");
    receiver.bind (endpoint);
    sender.connect (endpoint);

    const zlink::poll_event_flag_t retry_events =
      zlink::poll_event_flag_t::pollcompletion;
    zlink::poller_t poller;
    poller.add (sender, retry_events, 91);

    // The probe synchronizes the inproc connection within a five-second bound
    // and is removed before the HWM scenario begins.
    const auto probe_deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (5);
    zlink::message_t probe = zlink_cpp_contract::make_message ("pair-ready");
    auto probe_submission = sender.send ().message (probe).async ();
    assert (probe_submission.result == ZLINK_SUBMIT_OK
            || probe_submission.result == ZLINK_SUBMIT_BACKPRESSURED);
    assert (!probe.valid ());
    void_task_t probe_admitted =
      await_send (std::move (probe_submission.admitted));
    while (!probe_admitted.ready ()) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds> (
          probe_deadline - std::chrono::steady_clock::now ());
        assert (remaining > std::chrono::milliseconds (0));
        zlink::poll_event_t probe_event{};
        assert (poller.wait (&probe_event, 1, remaining) == 1);
        assert (probe_event.slot == 91);
        assert (has_event (probe_event.revents, retry_events));
    }
    probe_admitted.get ();
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds> (
      probe_deadline - std::chrono::steady_clock::now ());
    assert (remaining > std::chrono::milliseconds (0));
    zlink::poll_item_t probe_item = zlink::poll_item_t::from_socket (
      receiver, zlink::poll_event_flag_t::pollin);
    assert (zlink::poll (&probe_item, 1, remaining) == 1);
    assert (has_event (probe_item.revents, zlink::poll_event_flag_t::pollin));
    zlink::received_t received_probe;
    assert (receiver.recv (received_probe, zlink::recv_flags_t::dontwait) == 0);
    assert (received_probe.is_single_part ());
    assert (received_probe.first_part ().to_string () == "pair-ready");

    // Registration installs this public poller as the completion owner before
    // async(). Exact-token completion readiness is consumed only after credit
    // is restored.
    zlink::poll_event_t event{};

    const std::string filler (payload_size, 'f');
    size_t accepted = 0;
    std::string expected;
    std::optional<void_task_t> pending;
    for (; accepted != 512; ++accepted) {
        std::string payload = "fill-" + std::to_string (accepted);
        payload.resize (payload_size, 'f');
        zlink::message_t part = zlink_cpp_contract::make_message (payload);
        auto submission = sender.send ().message (part).async ();
        assert (!part.valid ());
        if (submission.result == ZLINK_SUBMIT_BACKPRESSURED) {
            expected = std::move (payload);
            pending.emplace (await_send (std::move (submission.admitted)));
            break;
        }
        assert (submission.result == ZLINK_SUBMIT_OK);
    }
    assert (accepted != 0 && accepted != 512 && pending.has_value ());
    assert (!pending->ready ());

    event = zlink::poll_event_t{};
    assert (poller.wait (&event, 1, std::chrono::milliseconds (0)) == 0);

    // Restore credit without touching the owning poller. Core publishes the
    // first WRITABLE token as soon as the sender observes the credit edge; the
    // refill below consumes that credit again before the owner drains it.
    zlink::received_t received;
    for (size_t index = 0; index != accepted; ++index) {
        assert (receiver.recv (received) == 0);
        assert (received.is_single_part ());
        std::string payload = "fill-" + std::to_string (index);
        payload.resize (payload_size, 'f');
        assert (received.first_part ().to_string () == payload);
    }
    size_t refilled = 0;
    for (; refilled != accepted; ++refilled) {
        zlink::message_t part = zlink_cpp_contract::make_message (filler);
        sender.send ().message (part).submit ();
        assert (!part.valid ());
    }
    assert (refilled != 0 && refilled != 512);

    // The owner drains the first token now. Its exact retry is backpressured
    // again, so the async result stays pending on a fresh token instead of
    // completing or delivering the retained packet.
    event = zlink::poll_event_t{};
    assert (poller.wait (&event, 1, std::chrono::seconds (5)) == 1);
    assert (event.slot == 91);
    assert (has_event (event.revents,
                       zlink::poll_event_flag_t::pollcompletion));
    assert (!pending->ready ());
    event = zlink::poll_event_t{};
    assert (poller.wait (&event, 1, std::chrono::milliseconds (0)) == 0);

    for (size_t index = 0; index != refilled; ++index) {
        assert (receiver.recv (received) == 0);
        assert (received.is_single_part ());
        assert (received.first_part ().to_string () == filler);
    }

    // The binding has no private send-progress thread. Until the public poller
    // drains WRITABLE, it must neither retry nor deliver the retained packet.
    zlink::poll_item_t before_retry = zlink::poll_item_t::from_socket (
      receiver, zlink::poll_event_flag_t::pollin);
    assert (zlink::poll (&before_retry, 1, std::chrono::milliseconds (20)) == 0);
    assert (before_retry.revents == zlink::poll_event_flag_t::none);

    // The second exact-token event makes the public poller retry the retained
    // packet and settle the future.
    event = zlink::poll_event_t{};
    assert (poller.wait (&event, 1, std::chrono::seconds (5)) == 1);
    assert (event.slot == 91);
    assert (has_event (event.revents,
                       zlink::poll_event_flag_t::pollcompletion));
    assert (pending->ready ());
    pending->get ();

    assert (receiver.recv (received) == 0);
    assert (received.is_single_part ());
    assert (received.first_part ().to_string () == expected);

    zlink::poll_item_t no_duplicate = zlink::poll_item_t::from_socket (
      receiver, zlink::poll_event_flag_t::pollin);
    assert (zlink::poll (&no_duplicate, 1, std::chrono::milliseconds (20)) == 0);
    assert (no_duplicate.revents == zlink::poll_event_flag_t::none);
    assert (poller.remove (sender));
    poller.close ();
}

void test_routed_no_route_is_immediate_typed_failure ()
{
    const zlink::routing_id_t missing = zlink::routing_id_t::from ("missing-peer");

    {
        zlink::context_t context;
        zlink::router_socket_t router (context);
        router.options ().mandatory (true);
        zlink::message_t payload = zlink_cpp_contract::make_message ("router-no-route");
        bool rejected = false;
        try {
            (void) router.send (missing).message (payload).async ();
        }
        catch (const zlink::submit_error_t &error) {
            rejected = error.result () == zlink::submit_result_t::not_connected
              && error.internal_errno () == EHOSTUNREACH;
        }
        assert (rejected);
        assert (payload.valid ());
    }

    {
        // STREAM routing IDs are Core's 4-byte peer handles; a well-formed ID
        // with no live route is the no-route case here.
        const zlink::routing_id_t missing_stream = zlink::routing_id_t::from ("none");
        zlink::context_t context;
        zlink::stream_socket_t stream (context);
        zlink::message_t payload = zlink_cpp_contract::make_message ("stream-no-route");
        bool rejected = false;
        try {
            (void) stream.send (missing_stream).message (payload).async ();
        }
        catch (const zlink::submit_error_t &error) {
            rejected = error.result () == zlink::submit_result_t::not_connected
              && error.internal_errno () == EHOSTUNREACH;
        }
        assert (rejected);
        assert (payload.valid ());
    }
}

void test_pending_send_socket_close_is_typed_terminal ()
{
    zlink::context_t context;
    zlink::pair_socket_t sender (context);
    zlink::pair_socket_t receiver (context);
    sender.options ().linger (std::chrono::milliseconds (0));
    receiver.options ().linger (std::chrono::milliseconds (0));
    sender.options ().immediate (true);
    sender.options ().send_hwm (zlink::byte_count_t::bytes (512));
    receiver.options ().recv_hwm (zlink::byte_count_t::bytes (512));
    sender.options ().send_timeout (std::chrono::milliseconds (-1));
    const std::string endpoint =
      zlink_cpp_contract::unique_inproc ("async-close-terminal");
    receiver.bind (endpoint);
    sender.connect (endpoint);

    zlink::message_t probe = zlink_cpp_contract::make_message ("ready");
    sender.send ().message (probe).submit ();
    zlink::received_t received_probe;
    assert (receiver.recv (received_probe) == 0);
    assert (received_probe.is_single_part ());
    assert (received_probe.first_part ().to_string () == "ready");

    zlink::poller_t poller;
    poller.add (sender, zlink::poll_event_flag_t::pollcompletion, 92);

    const std::string filler (64, 'f');
    size_t accepted = 0;
    std::optional<void_task_t> pending;
    for (; accepted != 512; ++accepted) {
        zlink::message_t part = zlink_cpp_contract::make_message (filler);
        auto submission = sender.send ().message (part).async ();
        if (submission.result == ZLINK_SUBMIT_BACKPRESSURED) {
            pending.emplace (await_send (std::move (submission.admitted)));
            break;
        }
        assert (submission.result == ZLINK_SUBMIT_OK);
    }
    assert (accepted != 0 && accepted != 512 && pending.has_value ());

    assert (!pending->ready ());
    sender.close ();

    bool terminated = false;
    try {
        pending->get ();
    }
    catch (const zlink::submit_error_t &error) {
        terminated = error.result () == zlink::submit_result_t::terminated
          && error.internal_errno () == ESHUTDOWN;
    }
    assert (terminated);
    poller.close ();
}

void test_pending_routed_send_target_removal_is_not_found ()
{
    zlink::context_t context;
    zlink::router_socket_t router (context);
    zlink::dealer_socket_t dealer (context);
    router.options ().linger (std::chrono::milliseconds (0));
    dealer.options ().linger (std::chrono::milliseconds (0));
    router.options ().mandatory (true);
    router.options ().send_hwm (zlink::byte_count_t::bytes (512));
    dealer.options ().recv_hwm (zlink::byte_count_t::bytes (512));
    router.options ().send_timeout (std::chrono::milliseconds (-1));
    dealer.options ().send_timeout (std::chrono::seconds (5));
    const zlink::routing_id_t dealer_id =
      zlink::routing_id_t::from ("terminal-peer");
    dealer.set_routing_id (dealer_id);
    const std::string endpoint =
      zlink_cpp_contract::unique_inproc ("async-target-removal");
    router.bind (endpoint);
    dealer.connect (endpoint);

    zlink::message_t probe = zlink_cpp_contract::make_message ("route-ready");
    dealer.send ().message (probe).submit ();
    zlink::received_t received_probe;
    assert (router.recv (received_probe) == 0);
    assert (received_probe.routing_id ().has_value ());
    assert (*received_probe.routing_id () == dealer_id);

    zlink::poller_t poller;
    poller.add (router, zlink::poll_event_flag_t::pollcompletion, 93);

    const std::string filler (64, 'r');
    size_t accepted = 0;
    std::optional<void_task_t> pending;
    for (; accepted != 512; ++accepted) {
        zlink::message_t part = zlink_cpp_contract::make_message (filler);
        auto submission = router.send (dealer_id).message (part).async ();
        if (submission.result == ZLINK_SUBMIT_BACKPRESSURED) {
            pending.emplace (await_send (std::move (submission.admitted)));
            break;
        }
        assert (submission.result == ZLINK_SUBMIT_OK);
    }
    assert (accepted != 0 && accepted != 512 && pending.has_value ());

    assert (!pending->ready ());

    zlink::socket_monitor_t monitor = router.monitor_open (zlink::monitor_event::disconnected);
    router.disconnect_rid (dealer_id);
    zlink::poll_event_t event{};
    assert (poller.wait (&event, 1, std::chrono::seconds (5)) == 1);
    assert (pending->ready ());
    bool not_found = false;
    try {
        pending->get ();
    }
    catch (const zlink::submit_error_t &error) {
        not_found = error.result () == zlink::submit_result_t::not_found
          && error.internal_errno () == ENOENT;
    }
    assert (not_found);

    assert (zlink_cpp_contract::wait_for_socket_monitor_event (
      monitor, static_cast<uint64_t> (zlink::monitor_event::disconnected), 5000));
    bool missing_rid_is_connect_error = false;
    try {
        router.disconnect_rid (dealer_id);
    }
    catch (const zlink::connect_error_t &error) {
        missing_rid_is_connect_error =
          error.result () == zlink::connect_result_t::not_found && error.code () == 605;
    }
    assert (missing_rid_is_connect_error);
    poller.close ();
}

} // namespace

int main ()
{
    test_immediately_admitted_async_send ();
    test_backpressured_async_send_retries_from_public_poller ();
    test_routed_no_route_is_immediate_typed_failure ();
    test_pending_send_socket_close_is_typed_terminal ();
    test_pending_routed_send_target_removal_is_not_found ();
    return 0;
}
