/* SPDX-License-Identifier: MPL-2.0 */

#include "support.hpp"
#include <Runtime/Messaging/completion_owner.hpp>

#include <atomic>
#include <cassert>
#include <chrono>
#include <coroutine>
#include <future>
#include <thread>

namespace
{

template <typename T> class test_task_t
{
  public:
    struct promise_type
    {
        std::promise<T> promise;
        test_task_t get_return_object () { return test_task_t (promise.get_future ()); }
        std::suspend_never initial_suspend () noexcept { return {}; }
        std::suspend_never final_suspend () noexcept { return {}; }
        void return_value (T value_) { promise.set_value (std::move (value_)); }
        void unhandled_exception () { promise.set_exception (std::current_exception ()); }
    };

    explicit test_task_t (std::future<T> future_) : _future (std::move (future_)) {}
    T get ()
    {
        assert (_future.wait_for (std::chrono::seconds (5)) == std::future_status::ready);
        return _future.get ();
    }

  private:
    std::future<T> _future;
};

test_task_t<std::vector<zlink::message_t>> await_reply (
  zlink::async_result_t<std::vector<zlink::message_t>> result_)
{
    co_return co_await std::move (result_);
}

class close_task_t
{
  public:
    struct promise_type
    {
        std::promise<void> promise;
        close_task_t get_return_object () { return close_task_t (promise.get_future ()); }
        std::suspend_never initial_suspend () noexcept { return {}; }
        std::suspend_never final_suspend () noexcept { return {}; }
        void return_void () { promise.set_value (); }
        void unhandled_exception () { promise.set_exception (std::current_exception ()); }
    };
    explicit close_task_t (std::future<void> future_) : _future (std::move (future_)) {}
    void get ()
    {
        assert (_future.wait_for (std::chrono::seconds (5)) == std::future_status::ready);
        _future.get ();
    }
  private:
    std::future<void> _future;
};

close_task_t close_from_runtime_continuation (
  zlink::dealer_socket_t &dealer_,
  zlink::async_result_t<std::vector<zlink::message_t>> result_)
{
    auto reply = co_await std::move (result_);
    assert (!reply.empty ());
    dealer_.close ();
}

void receive_and_reply (zlink::router_socket_t &router_,
                        const std::string &expected_,
                        const std::string &reply_)
{
    zlink::received_t received;
    while (router_.recv (received, zlink::recv_flags_t::dontwait) != 0)
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    assert (received.first_part ().to_string () == expected_);
    assert (received.reply_token ().has_value ());
    zlink::message_t response = zlink_cpp_contract::make_message (reply_);
    received.reply ().message (response).submit ();
    assert (!response.valid ());
}

void test_blocking_request_and_reply_token_owner ()
{
    zlink::context_t context;
    zlink::dealer_socket_t dealer (context);
    zlink::router_socket_t router (context);
    zlink::router_socket_t other_router (context);
    dealer.set_routing_id (zlink::routing_id_t::from ("blocking-client"));
    router.set_routing_id (zlink::routing_id_t::from ("blocking-server"));
    const std::string endpoint = zlink_cpp_contract::unique_inproc ("blocking-request");
    router.bind (endpoint);
    dealer.connect (endpoint);

    std::atomic<bool> owner_mismatch_rejected{false};
    std::thread responder ([&] {
        zlink::received_t received;
        while (router.recv (received, zlink::recv_flags_t::dontwait) != 0)
            std::this_thread::sleep_for (std::chrono::milliseconds (1));
        assert (received.reply_token ().has_value ());
        zlink::message_t rejected = zlink_cpp_contract::make_message ("wrong-owner");
        try {
            other_router.reply (*received.routing_id (), *received.reply_token ())
              .message (rejected).submit ();
        }
        catch (const zlink::submit_error_t &error) {
            owner_mismatch_rejected.store (
              error.result () == zlink::submit_result_t::invalid_argument,
              std::memory_order_release);
        }
        assert (rejected.valid ());
        zlink::message_t response = zlink_cpp_contract::make_message ("blocking-reply");
        received.reply ().message (response).submit ();
    });

    zlink::message_t request = zlink_cpp_contract::make_message ("blocking-request");
    auto reply = dealer.request ().message (request)
                   .timeout (std::chrono::seconds (2)).submit ();
    responder.join ();
    assert (owner_mismatch_rejected.load (std::memory_order_acquire));
    assert (!request.valid ());
    assert (reply.size () == 1);
    assert (reply[0].to_string () == "blocking-reply");
}

void test_async_request_public_poller_progress_and_owner_transfer ()
{
    zlink::context_t context;
    zlink::dealer_socket_t dealer (context);
    zlink::router_socket_t router (context);
    dealer.set_routing_id (zlink::routing_id_t::from ("async-client"));
    router.set_routing_id (zlink::routing_id_t::from ("async-server"));
    const std::string endpoint = zlink_cpp_contract::unique_inproc ("async-request");
    router.bind (endpoint);
    dealer.connect (endpoint);

    zlink::poller_t poller;
    poller.add (dealer, zlink::poll_event_flag_t::pollcompletion, 17);
    std::thread responder ([&] {
        receive_and_reply (router, "async-request", "async-reply");
    });

    zlink::message_t request = zlink_cpp_contract::make_message ("async-request");
    auto result = dealer.request ().message (request)
                    .timeout (std::chrono::seconds (2)).async ();
    zlink::poll_event_t event{};
    assert (poller.wait (&event, 1, std::chrono::seconds (5)) == 1);
    assert (event.slot == 17);
    assert ((static_cast<short> (event.revents)
             & static_cast<short> (zlink::poll_event_flag_t::pollcompletion)) != 0);
    auto reply = await_reply (std::move (result)).get ();
    assert (reply.size () == 1 && reply[0].to_string () == "async-reply");
    responder.join ();

    poller.modify (dealer, zlink::poll_event_flag_t::pollin);
    poller.modify (dealer, zlink::poll_event_flag_t::pollcompletion);
    assert (poller.remove (dealer));
    poller.close ();
}

void test_blocking_request_progresses_with_public_poller_wait_thread ()
{
    zlink::context_t context;
    zlink::dealer_socket_t dealer (context);
    zlink::router_socket_t router (context);
    dealer.set_routing_id (zlink::routing_id_t::from ("public-blocking-client"));
    router.set_routing_id (zlink::routing_id_t::from ("public-blocking-server"));
    const std::string endpoint = zlink_cpp_contract::unique_inproc ("public-blocking");
    router.bind (endpoint);
    dealer.connect (endpoint);

    zlink::poller_t poller;
    poller.add (dealer, zlink::poll_event_flag_t::pollcompletion, 23);
    std::thread responder ([&] {
        receive_and_reply (router, "public-blocking-request", "public-blocking-reply");
    });
    std::thread waiter ([&] {
        zlink::poll_event_t event{};
        assert (poller.wait (&event, 1, std::chrono::seconds (5)) == 1);
        assert (event.slot == 23);
        assert ((static_cast<short> (event.revents)
                 & static_cast<short> (zlink::poll_event_flag_t::pollcompletion)) != 0);
    });

    zlink::message_t request =
      zlink_cpp_contract::make_message ("public-blocking-request");
    auto reply = dealer.request ().message (request)
                   .timeout (std::chrono::seconds (2)).submit ();
    waiter.join ();
    responder.join ();
    assert (reply.size () == 1 && reply[0].to_string () == "public-blocking-reply");
    assert (poller.remove (dealer));
}

void test_dropped_async_result_late_completion_cleanup ()
{
    zlink::context_t context;
    zlink::dealer_socket_t dealer (context);
    zlink::router_socket_t router (context);
    dealer.set_routing_id (zlink::routing_id_t::from ("drop-client"));
    router.set_routing_id (zlink::routing_id_t::from ("drop-server"));
    const std::string endpoint = zlink_cpp_contract::unique_inproc ("drop-request");
    router.bind (endpoint);
    dealer.connect (endpoint);

    std::thread responder ([&] {
        receive_and_reply (router, "drop-request", "late-reply");
    });
    {
        zlink::message_t request = zlink_cpp_contract::make_message ("drop-request");
        auto dropped = dealer.request ().message (request)
                         .timeout (std::chrono::seconds (2)).async ();
        (void) dropped;
    }
    responder.join ();
    std::this_thread::sleep_for (std::chrono::milliseconds (50));
}

void test_request_completion_publish_and_capture_join_once ()
{
    auto result = std::make_shared<
      zlink::detail::async_operation_state_t<std::vector<zlink::message_t>>> ();
    zlink::detail::completion_entry_t entry (result);
    zlink_completion_t completion{};
    completion.struct_size = sizeof (completion);
    completion.kind = ZLINK_COMPLETION_REQUEST;
    completion.completion_id = 41;
    completion.user_context = &entry;
    completion.request_result = ZLINK_REQUEST_OK;
    entry.publish (41);
    entry.capture (completion);
    assert (result->ready ());
    assert (result->take ().empty ());
}

void test_non_ok_request_is_typed_without_payload ()
{
    zlink::context_t context;
    zlink::dealer_socket_t dealer (context);
    zlink::router_socket_t router (context);
    dealer.set_routing_id (zlink::routing_id_t::from ("timeout-client"));
    router.set_routing_id (zlink::routing_id_t::from ("timeout-server"));
    const std::string endpoint = zlink_cpp_contract::unique_inproc ("request-timeout");
    router.bind (endpoint);
    dealer.connect (endpoint);

    zlink::message_t request = zlink_cpp_contract::make_message ("timeout");
    bool timed_out = false;
    try {
        (void) dealer.request ().message (request)
          .timeout (std::chrono::milliseconds (30)).submit ();
    }
    catch (const zlink::request_error_t &error) {
        timed_out = error.result () == zlink::request_result_t::timed_out;
    }
    assert (timed_out);
    assert (!request.valid ());
}

void test_runtime_continuation_can_close_socket ()
{
    zlink::context_t context;
    zlink::dealer_socket_t dealer (context);
    zlink::router_socket_t router (context);
    dealer.set_routing_id (zlink::routing_id_t::from ("close-client"));
    router.set_routing_id (zlink::routing_id_t::from ("close-server"));
    const std::string endpoint = zlink_cpp_contract::unique_inproc ("close-continuation");
    router.bind (endpoint);
    dealer.connect (endpoint);
    std::thread responder ([&] {
        receive_and_reply (router, "close-request", "close-reply");
    });
    zlink::message_t request = zlink_cpp_contract::make_message ("close-request");
    close_task_t task = close_from_runtime_continuation (
      dealer, dealer.request ().message (request)
                .timeout (std::chrono::seconds (2)).async ());
    task.get ();
    responder.join ();
    assert (!dealer.valid ());
}

} // namespace

int main ()
{
    test_blocking_request_and_reply_token_owner ();
    test_async_request_public_poller_progress_and_owner_transfer ();
    test_blocking_request_progresses_with_public_poller_wait_thread ();
    test_dropped_async_result_late_completion_cleanup ();
    test_request_completion_publish_and_capture_join_once ();
    test_non_ok_request_is_typed_without_payload ();
    test_runtime_continuation_can_close_socket ();
    return 0;
}
