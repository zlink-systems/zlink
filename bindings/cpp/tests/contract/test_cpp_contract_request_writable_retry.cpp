/* SPDX-License-Identifier: MPL-2.0 */

#include "support.hpp"

#include <cassert>
#include <cerrno>
#include <chrono>
#include <coroutine>
#include <cstdint>
#include <future>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace
{

template <typename T> class value_task_t
{
  public:
    struct promise_type
    {
        std::promise<T> promise;
        value_task_t get_return_object ()
        {
            return value_task_t (promise.get_future ());
        }
        std::suspend_never initial_suspend () noexcept { return {}; }
        std::suspend_never final_suspend () noexcept { return {}; }
        void return_value (T value_) { promise.set_value (std::move (value_)); }
        void unhandled_exception ()
        {
            promise.set_exception (std::current_exception ());
        }
    };

    explicit value_task_t (std::future<T> future_) : _future (std::move (future_)) {}
    value_task_t (value_task_t &&) noexcept = default;
    value_task_t &operator= (value_task_t &&) noexcept = default;

    bool ready () const
    {
        return _future.wait_for (std::chrono::milliseconds (0))
               == std::future_status::ready;
    }

    T get ()
    {
        assert (_future.wait_for (std::chrono::seconds (5))
                == std::future_status::ready);
        return _future.get ();
    }

  private:
    std::future<T> _future;
};

class void_task_t
{
  public:
    struct promise_type
    {
        std::promise<void> promise;
        void_task_t get_return_object ()
        {
            return void_task_t (promise.get_future ());
        }
        std::suspend_never initial_suspend () noexcept { return {}; }
        std::suspend_never final_suspend () noexcept { return {}; }
        void return_void () { promise.set_value (); }
        void unhandled_exception ()
        {
            promise.set_exception (std::current_exception ());
        }
    };

    explicit void_task_t (std::future<void> future_) : _future (std::move (future_)) {}

    bool ready () const
    {
        return _future.wait_for (std::chrono::milliseconds (0))
               == std::future_status::ready;
    }

    void get ()
    {
        assert (_future.wait_for (std::chrono::seconds (5))
                == std::future_status::ready);
        _future.get ();
    }

  private:
    std::future<void> _future;
};

using reply_parts_t = std::vector<zlink::message_t>;

value_task_t<reply_parts_t> await_reply (zlink::async_result_t<reply_parts_t> result_)
{
    co_return co_await std::move (result_);
}

void_task_t await_send (zlink::async_result_t<void> result_)
{
    co_await std::move (result_);
}

bool all_ready (const std::vector<value_task_t<reply_parts_t>> &tasks_)
{
    for (const auto &task : tasks_) {
        if (!task.ready ())
            return false;
    }
    return true;
}

size_t drain_and_reply (zlink::router_socket_t &router_,
                        std::set<std::string> &requests_)
{
    size_t count = 0;
    for (;;) {
        zlink::received_t received;
        if (router_.recv (received, zlink::recv_flags_t::dontwait) != 0)
            return count;
        assert (received.reply_token ().has_value ());
        requests_.insert (received.first_part ().to_string ());
        zlink::message_t reply = zlink_cpp_contract::make_message ("reply");
        received.reply ().message (reply).submit ();
        ++count;
    }
}

void test_hwm_request_waits_for_its_writable_and_retries ()
{
    constexpr size_t request_count = 32;
    constexpr size_t payload_size = 64;

    zlink::context_t context;
    zlink::dealer_socket_t dealer (context);
    zlink::router_socket_t router (context);
    dealer.options ().linger (std::chrono::milliseconds (0));
    router.options ().linger (std::chrono::milliseconds (0));
    dealer.options ().immediate (true);
    dealer.options ().send_timeout (std::chrono::seconds (5));
    router.options ().recv_timeout (std::chrono::seconds (5));
    const uint64_t hwm = 512;
    dealer.options ().send_hwm (zlink::byte_count_t::bytes (hwm));
    router.options ().recv_hwm (zlink::byte_count_t::bytes (hwm));

    const std::string endpoint =
      zlink_cpp_contract::unique_inproc ("request-writable-hwm");
    router.bind (endpoint);
    dealer.connect (endpoint);

    zlink::message_t prime = zlink_cpp_contract::make_message ("prime");
    dealer.send ().message (prime).submit ();
    zlink::received_t prime_received;
    assert (router.recv (prime_received) == 0);

    zlink::poller_t poller;
    poller.add (dealer, zlink::poll_event_flag_t::pollcompletion, 1);
    poller.add (router, zlink::poll_event_flag_t::pollin, 2);

    std::vector<value_task_t<reply_parts_t>> tasks;
    tasks.reserve (request_count);
    for (size_t index = 0; index != request_count; ++index) {
        std::string payload = "request-" + std::to_string (index);
        payload.resize (payload_size, 'x');
        zlink::message_t request = zlink_cpp_contract::make_message (payload);
        tasks.push_back (await_reply (
          dealer.request ().message (request)
            .timeout (std::chrono::seconds (30)).async ()));
        assert (!request.valid ());
    }

    zlink::poll_event_t events[2]{};
    assert (poller.wait (events, 2, std::chrono::seconds (5)) != 0);
    std::set<std::string> requests;
    const size_t initially_admitted = drain_and_reply (router, requests);
    assert (initially_admitted > 0);
    assert (initially_admitted < request_count);

    const auto deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (10);
    while (!all_ready (tasks) && std::chrono::steady_clock::now () < deadline) {
        assert (poller.wait (events, 2, std::chrono::seconds (5)) != 0);
        (void) drain_and_reply (router, requests);
    }

    assert (all_ready (tasks));
    assert (requests.size () == request_count);
    for (auto &task : tasks) {
        reply_parts_t reply = task.get ();
        assert (reply.size () == 1);
        assert (reply.front ().to_string () == "reply");
    }
}

void test_connect_before_bind_mixes_request_and_send_tokens ()
{
    zlink::context_t context;
    zlink::dealer_socket_t dealer (context);
    zlink::router_socket_t router (context);
    dealer.options ().linger (std::chrono::milliseconds (0));
    router.options ().linger (std::chrono::milliseconds (0));
    dealer.options ().immediate (true);
    router.options ().recv_timeout (std::chrono::seconds (5));

    zlink::poller_t poller;
    poller.add (dealer, zlink::poll_event_flag_t::pollcompletion, 3);
    poller.add (router, zlink::poll_event_flag_t::pollin, 4);

    const std::string endpoint =
      zlink_cpp_contract::unique_tcp ("request-connect-before-bind");
    dealer.connect (endpoint);

    zlink::message_t request =
      zlink_cpp_contract::make_message ("connect-before-bind-request");
    value_task_t<reply_parts_t> request_task = await_reply (
      dealer.request ().message (request)
        .timeout (std::chrono::seconds (10)).async ());
    zlink::message_t data =
      zlink_cpp_contract::make_message ("connect-before-bind-send");
    void_task_t send_task = await_send (
      dealer.send ().message (data).async ());
    assert (!request_task.ready ());
    assert (!send_task.ready ());
    assert (!request.valid () && !data.valid ());

    router.bind (endpoint);
    bool saw_request = false;
    bool saw_send = false;
    zlink::poll_event_t events[2]{};
    const auto deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (10);
    while ((!request_task.ready () || !send_task.ready () || !saw_request
            || !saw_send)
           && std::chrono::steady_clock::now () < deadline) {
        assert (poller.wait (events, 2, std::chrono::seconds (5)) != 0);
        for (;;) {
            zlink::received_t received;
            if (router.recv (received, zlink::recv_flags_t::dontwait) != 0)
                break;
            const std::string payload = received.first_part ().to_string ();
            if (received.reply_token ().has_value ()) {
                assert (payload == "connect-before-bind-request");
                zlink::message_t reply =
                  zlink_cpp_contract::make_message ("connect-before-bind-reply");
                received.reply ().message (reply).submit ();
                saw_request = true;
            } else {
                assert (payload == "connect-before-bind-send");
                saw_send = true;
            }
        }
    }

    assert (saw_request && saw_send);
    send_task.get ();
    reply_parts_t reply = request_task.get ();
    assert (reply.size () == 1);
    assert (reply.front ().to_string () == "connect-before-bind-reply");
}

void test_close_settles_request_wait_token_as_typed_terminal ()
{
    zlink::context_t context;
    zlink::dealer_socket_t dealer (context);
    dealer.options ().linger (std::chrono::milliseconds (0));
    dealer.options ().immediate (true);

    zlink::poller_t poller;
    poller.add (dealer, zlink::poll_event_flag_t::pollcompletion, 5);
    dealer.connect (zlink_cpp_contract::unique_tcp ("request-close-token"));

    zlink::message_t request = zlink_cpp_contract::make_message ("close-token");
    value_task_t<reply_parts_t> task = await_reply (
      dealer.request ().message (request)
        .timeout (std::chrono::seconds (10)).async ());
    assert (!task.ready ());
    assert (!request.valid ());

    dealer.close ();
    assert (task.ready ());
    bool terminated = false;
    try {
        (void) task.get ();
    }
    catch (const zlink::submit_error_t &error) {
        terminated = error.result () == zlink::submit_result_t::terminated
          && error.internal_errno () == ESHUTDOWN;
    }
    assert (terminated);
}

} // namespace

int main ()
{
    test_hwm_request_waits_for_its_writable_and_retries ();
    test_connect_before_bind_mixes_request_and_send_tokens ();
    test_close_settles_request_wait_token_as_typed_terminal ();
    return 0;
}
