/* SPDX-License-Identifier: MPL-2.0 */
#include "support.hpp"

#include <coroutine>

namespace
{
template <typename T> struct task_t
{
    struct promise_type
    {
        std::promise<T> result;
        task_t get_return_object () { return {result.get_future ()}; }
        std::suspend_never initial_suspend () noexcept { return {}; }
        std::suspend_never final_suspend () noexcept { return {}; }
        void return_value (T value_) { result.set_value (std::move (value_)); }
        void unhandled_exception ()
        {
            result.set_exception (std::current_exception ());
        }
    };
    std::future<T> result;
    bool ready () const
    {
        return result.wait_for (std::chrono::seconds (0))
               == std::future_status::ready;
    }
    T get ()
    {
        assert (result.wait_for (std::chrono::seconds (5))
                == std::future_status::ready);
        return result.get ();
    }
};

task_t<bool> await_send (zlink::async_result_t<void> result_)
{
    co_await std::move (result_);
    co_return true;
}

task_t<std::vector<zlink::message_t>> await_request (
  zlink::async_result_t<std::vector<zlink::message_t>> result_)
{
    co_return co_await std::move (result_);
}

void test_ownerless_async_completion_fails_fast_then_public_poller_progresses ()
{
    {
        zlink::context_t context;
        zlink::dealer_socket_t dealer (context);
        zlink::router_socket_t router (context);
        const auto endpoint =
          zlink_cpp_contract::unique_inproc ("ownerless-request");
        router.bind (endpoint);
        dealer.connect (endpoint);
        zlink::message_t request = zlink::message_t::from ("ownerless-request");
        bool rejected = false;
        try {
            (void) dealer.request ().message (request)
              .timeout (std::chrono::seconds (5)).async ();
        }
        catch (const zlink::submit_error_t &error) {
            rejected = error.result () == zlink::submit_result_t::invalid_state;
        }
        assert (rejected && request.valid ());
        zlink::received_t received;
        assert (router.recv (received, zlink::recv_flags_t::dontwait) != 0);

        zlink::poller_t poller;
        poller.add (dealer, zlink::poll_event_flag_t::pollcompletion, 1);
        assert (poller.remove (dealer));
        rejected = false;
        try {
            (void) dealer.request ().message (request)
              .timeout (std::chrono::seconds (5)).async ();
        }
        catch (const zlink::submit_error_t &error) {
            rejected = error.result () == zlink::submit_result_t::invalid_state;
        }
        assert (rejected && request.valid ());
        assert (router.recv (received, zlink::recv_flags_t::dontwait) != 0);
    }

    {
        zlink::context_t context;
        zlink::dealer_socket_t dealer (context);
        dealer.options ().linger (std::chrono::milliseconds (0));
        dealer.options ().immediate (true);
        dealer.connect (zlink_cpp_contract::unique_tcp ("ownerless-send"));
        zlink::message_t send = zlink::message_t::from ("ownerless-send");
        bool rejected = false;
        try {
            (void) dealer.send ().message (send).async ();
        }
        catch (const zlink::submit_error_t &error) {
            rejected = error.result () == zlink::submit_result_t::invalid_state;
        }
        assert (rejected && send.valid ());
    }

    {
        zlink::context_t context;
        zlink::dealer_socket_t dealer (context);
        zlink::router_socket_t router (context);
        dealer.options ().linger (std::chrono::milliseconds (0));
        router.options ().linger (std::chrono::milliseconds (0));
        dealer.options ().immediate (true);
        router.options ().recv_timeout (std::chrono::seconds (5));
        const auto endpoint =
          zlink_cpp_contract::unique_tcp ("completion-public-owner");
        dealer.connect (endpoint);
        zlink::poller_t poller;
        poller.add (dealer, zlink::poll_event_flag_t::pollcompletion, 1);
        auto request = await_request (
          dealer.request ().message (zlink::message_t::from ("request"))
            .timeout (std::chrono::seconds (5)).async ().reply);
        auto send = await_send (
          dealer.send ().message (zlink::message_t::from ("send")).async ().admitted);
        assert (!request.ready () && !send.ready ());

        router.bind (endpoint);
        std::thread responder ([&] {
            bool saw_send = false;
            bool saw_request = false;
            for (int i = 0; i < 2; ++i) {
                zlink::received_t received;
                assert (router.recv (received) == 0);
                if (received.reply_token ().has_value ()) {
                    assert (!saw_request);
                    assert (received.first_part ().to_string () == "request");
                    auto reply = zlink::message_t::from ("reply");
                    received.reply ().message (reply).submit ();
                    saw_request = true;
                } else {
                    assert (!saw_send);
                    assert (received.first_part ().to_string () == "send");
                    saw_send = true;
                }
            }
            assert (saw_request && saw_send);
        });
        const auto deadline =
          std::chrono::steady_clock::now () + std::chrono::seconds (5);
        while ((!request.ready () || !send.ready ())
               && std::chrono::steady_clock::now () < deadline) {
            zlink::poll_event_t event{};
            assert (poller.wait (&event, 1, std::chrono::seconds (5)) == 1);
            assert (event.slot == 1);
            assert ((static_cast<short> (event.revents)
                     & static_cast<short> (
                       zlink::poll_event_flag_t::pollcompletion)) != 0);
        }
        assert (send.get ());
        auto reply = request.get ();
        assert (reply.size () == 1 && reply[0].to_string () == "reply");
        responder.join ();
    }
}
} // namespace

int main ()
{
    test_ownerless_async_completion_fails_fast_then_public_poller_progresses ();
}
