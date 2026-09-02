/* SPDX-License-Identifier: MPL-2.0 */

#include "support.hpp"

#include <cassert>
#include <chrono>
#include <coroutine>
#include <future>

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

} // namespace

int main ()
{
    zlink::context_t context;
    zlink::pair_socket_t sender (context);
    zlink::pair_socket_t receiver (context);
    const std::string endpoint = zlink_cpp_contract::unique_inproc ("async-ready");
    receiver.bind (endpoint);
    sender.connect (endpoint);

    zlink::message_t outbound = zlink_cpp_contract::make_message ("async");
    await_send (sender.send ().message (outbound).async ()).get ();
    assert (!outbound.valid ());
    zlink::message_t inbound;
    assert (receiver.recv (inbound) == 0);
    assert (inbound.to_string () == "async");
    return 0;
}
