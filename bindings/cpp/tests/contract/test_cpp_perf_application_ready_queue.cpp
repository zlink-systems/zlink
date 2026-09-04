/* SPDX-License-Identifier: MPL-2.0 */

#include "support.hpp"

#include <cassert>
#include <cerrno>
#include <chrono>
#include <coroutine>
#include <cstdint>
#include <future>
#include <string>

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
    zlink::message_t received_probe;
    assert (receiver.recv (received_probe) == 0);
    assert (received_probe.to_string () == "ready");

    zlink::message_t outbound = zlink_cpp_contract::make_message ("async");
    await_send (sender.send ().message (outbound).async ()).get ();
    assert (!outbound.valid ());
    zlink::message_t inbound;
    assert (receiver.recv (inbound) == 0);
    assert (inbound.to_string () == "async");
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
    sender.options ().send_timeout (std::chrono::seconds (5));

    constexpr size_t payload_size = 64;
    const uint64_t hwm = 512;
    sender.options ().send_hwm (zlink::byte_count_t::bytes (hwm));
    receiver.options ().recv_hwm (zlink::byte_count_t::bytes (hwm));

    const std::string endpoint =
      zlink_cpp_contract::unique_inproc ("async-backpressure-ready");
    receiver.bind (endpoint);
    sender.connect (endpoint);

    // A bounded blocking probe synchronizes the inproc connection without a
    // settle sleep and is removed before the HWM scenario begins.
    zlink::message_t probe = zlink_cpp_contract::make_message ("pair-ready");
    sender.send ().message (probe).submit ();
    assert (!probe.valid ());
    zlink::message_t received_probe;
    assert (receiver.recv (received_probe) == 0);
    assert (received_probe.to_string () == "pair-ready");
    sender.options ().send_timeout (std::chrono::milliseconds (0));

    const zlink::poll_event_flag_t retry_events =
      zlink::poll_event_flag_t::pollout
      | zlink::poll_event_flag_t::pollcompletion;
    zlink::poller_t poller;
    poller.add (sender, retry_events, 91);

    // Registration installs this public poller as the completion owner before
    // async(). The combined event is consumed only after credit is restored.
    zlink::poll_event_t event{};

    const std::string filler (payload_size, 'f');
    size_t accepted = 0;
    for (; accepted != 512; ++accepted) {
        zlink::message_t part = zlink_cpp_contract::make_message (filler);
        try {
            sender.send ().message (part).submit ();
            assert (!part.valid ());
        }
        catch (const zlink::submit_error_t &error) {
            assert (error.result () == zlink::submit_result_t::backpressured);
            assert (error.internal_errno () == EAGAIN);
            assert (part.valid ());
            break;
        }
    }
    assert (accepted != 0 && accepted != 512);

    std::string expected = "async-backpressure-exact-retry";
    expected.resize (payload_size, 'r');
    zlink::message_t outbound = zlink_cpp_contract::make_message (expected);
    void_task_t pending = await_send (sender.send ().message (outbound).async ());
    assert (!outbound.valid ());
    assert (!pending.ready ());

    event = zlink::poll_event_t{};
    assert (poller.wait (&event, 1, std::chrono::milliseconds (0)) == 0);

    for (size_t index = 0; index != accepted; ++index) {
        zlink::message_t received;
        assert (receiver.recv (received) == 0);
        assert (received.to_string () == filler);
    }

    // The binding has no private send-progress thread. Until the public poller
    // drains WRITABLE, it must neither retry nor deliver the retained packet.
    zlink::poll_item_t before_retry = zlink::poll_item_t::from_socket (
      receiver, zlink::poll_event_flag_t::pollin);
    assert (zlink::poll (&before_retry, 1, std::chrono::milliseconds (20)) == 0);
    assert (before_retry.revents == zlink::poll_event_flag_t::none);

    // The combined readiness event makes the public poller drain WRITABLE,
    // match its token, retry the retained packet, and settle the future.
    event = zlink::poll_event_t{};
    assert (poller.wait (&event, 1, std::chrono::seconds (5)) == 1);
    assert (event.slot == 91);
    assert (has_event (event.revents, zlink::poll_event_flag_t::pollout));
    assert (has_event (event.revents,
                       zlink::poll_event_flag_t::pollcompletion));
    assert (pending.ready ());
    pending.get ();
    assert (!outbound.valid ());

    zlink::message_t received;
    assert (receiver.recv (received) == 0);
    assert (received.to_string () == expected);

    zlink::poll_item_t no_duplicate = zlink::poll_item_t::from_socket (
      receiver, zlink::poll_event_flag_t::pollin);
    assert (zlink::poll (&no_duplicate, 1, std::chrono::milliseconds (20)) == 0);
    assert (no_duplicate.revents == zlink::poll_event_flag_t::none);
    assert (poller.remove (sender));
    poller.close ();
}

} // namespace

int main ()
{
    test_immediately_admitted_async_send ();
    test_backpressured_async_send_retries_from_public_poller ();
    return 0;
}
