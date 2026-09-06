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

void test_runtime_progress (bool transfer_from_public_, bool concurrent_request_)
{
    zlink::context_t context;
    zlink::dealer_socket_t dealer (context);
    zlink::router_socket_t router (context);
    dealer.options ().linger (std::chrono::milliseconds (0));
    router.options ().linger (std::chrono::milliseconds (0));
    dealer.options ().immediate (true);
    router.options ().recv_timeout (std::chrono::seconds (5));
    zlink::poller_t public_poller;
    if (transfer_from_public_)
        public_poller.add (dealer, zlink::poll_event_flag_t::pollcompletion, 1);
    const auto endpoint = zlink_cpp_contract::unique_tcp ("send-runtime-owner");
    dealer.connect (endpoint);

    // No bound peer exists, so both async submissions must own wait tokens.
    // REQUEST first also exercises SEND registration with an existing owner.
    std::optional<task_t<std::vector<zlink::message_t>>> request;
    if (concurrent_request_)
        request.emplace (await_request (
          dealer.request ().message (zlink::message_t::from ("request"))
            .timeout (std::chrono::seconds (5)).async ()));
    auto send = await_send (
      dealer.send ().message (zlink::message_t::from ("send")).async ());
    assert ((!request || !request->ready ()) && !send.ready ());
    if (transfer_from_public_)
        public_poller.remove (dealer);

    router.bind (endpoint);
    bool saw_send = false;
    bool saw_request = false;
    for (int i = 0; i < (concurrent_request_ ? 2 : 1); ++i) {
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
    assert (saw_request == concurrent_request_ && saw_send);
    assert (send.get ());
    if (request) {
        auto reply = request->get ();
        assert (reply.size () == 1 && reply[0].to_string () == "reply");
    }
}
} // namespace

int main ()
{
    test_runtime_progress (false, false);
    test_runtime_progress (false, true);
    test_runtime_progress (true, true);
}
