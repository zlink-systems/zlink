/* SPDX-License-Identifier: MPL-2.0 */

#include "support.hpp"

#include <Runtime/Messaging/async_operation_state.hpp>
#include <Runtime/Core/routing_id_access.hpp>
#include <Runtime/Sockets/socket_access.hpp>
#include <zlink/message/api.h>

#include <cerrno>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>

namespace
{

zlink::message_t make_request_message (const std::string &text_)
{
    return zlink_cpp_contract::make_message (text_);
}

template <typename T> struct blocking_coroutine_state_t
{
    std::mutex mutex;
    std::condition_variable changed;
    std::optional<T> value;
    std::exception_ptr failure;
    bool done = false;
};

template <> struct blocking_coroutine_state_t<void>
{
    std::mutex mutex;
    std::condition_variable changed;
    std::exception_ptr failure;
    bool done = false;
};

template <typename T> class blocking_coroutine_t
{
  public:
    struct promise_type
    {
        std::shared_ptr<blocking_coroutine_state_t<T>> state =
          std::make_shared<blocking_coroutine_state_t<T>> ();

        blocking_coroutine_t get_return_object () { return blocking_coroutine_t (state); }
        std::suspend_never initial_suspend () noexcept { return {}; }
        std::suspend_never final_suspend () noexcept { return {}; }
        void unhandled_exception () noexcept
        {
            finish ([&] { state->failure = std::current_exception (); });
        }
        void return_value (T value_) noexcept
        {
            finish ([&] { state->value.emplace (std::move (value_)); });
        }

      private:
        template <typename TStore> void finish (TStore store_) noexcept
        {
            {
                std::lock_guard<std::mutex> lock (state->mutex);
                try {
                    store_ ();
                }
                catch (...) {
                    state->failure = std::current_exception ();
                }
                state->done = true;
            }
            state->changed.notify_all ();
        }
    };

    template <typename TRep, typename TPeriod>
    bool wait_for (std::chrono::duration<TRep, TPeriod> timeout_)
    {
        std::unique_lock<std::mutex> lock (_state->mutex);
        return _state->changed.wait_for (lock, timeout_, [&] { return _state->done; });
    }

    T take ()
    {
        std::unique_lock<std::mutex> lock (_state->mutex);
        _state->changed.wait (lock, [&] { return _state->done; });
        if (_state->failure)
            std::rethrow_exception (_state->failure);
        return std::move (*_state->value);
    }

  private:
    explicit blocking_coroutine_t (std::shared_ptr<blocking_coroutine_state_t<T>> state_) :
        _state (std::move (state_))
    {
    }
    std::shared_ptr<blocking_coroutine_state_t<T>> _state;
};

template <> class blocking_coroutine_t<void>
{
  public:
    struct promise_type
    {
        std::shared_ptr<blocking_coroutine_state_t<void>> state =
          std::make_shared<blocking_coroutine_state_t<void>> ();

        blocking_coroutine_t get_return_object () { return blocking_coroutine_t (state); }
        std::suspend_never initial_suspend () noexcept { return {}; }
        std::suspend_never final_suspend () noexcept { return {}; }
        void unhandled_exception () noexcept { finish (std::current_exception ()); }
        void return_void () noexcept { finish (nullptr); }

      private:
        void finish (std::exception_ptr failure_) noexcept
        {
            {
                std::lock_guard<std::mutex> lock (state->mutex);
                state->failure = std::move (failure_);
                state->done = true;
            }
            state->changed.notify_all ();
        }
    };

    template <typename TRep, typename TPeriod>
    bool wait_for (std::chrono::duration<TRep, TPeriod> timeout_)
    {
        std::unique_lock<std::mutex> lock (_state->mutex);
        return _state->changed.wait_for (lock, timeout_, [&] { return _state->done; });
    }

    void take ()
    {
        std::unique_lock<std::mutex> lock (_state->mutex);
        _state->changed.wait (lock, [&] { return _state->done; });
        if (_state->failure)
            std::rethrow_exception (_state->failure);
    }

  private:
    explicit blocking_coroutine_t (std::shared_ptr<blocking_coroutine_state_t<void>> state_) :
        _state (std::move (state_))
    {
    }
    std::shared_ptr<blocking_coroutine_state_t<void>> _state;
};

template <typename T>
blocking_coroutine_t<T> await_result (zlink::async_result_t<T> result_)
{
    co_return co_await std::move (result_);
}

blocking_coroutine_t<void> await_result (zlink::async_result_t<void> result_)
{
    co_await std::move (result_);
}

void await_send (zlink::async_result_t<void> result_)
{
    auto completion = await_result (std::move (result_));
    assert (completion.wait_for (std::chrono::seconds (2)));
    completion.take ();
}

struct continuation_gate_t
{
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool released = false;
};

blocking_coroutine_t<void> await_send_and_hold_continuation (
  zlink::async_result_t<void> result_, continuation_gate_t &gate_)
{
    co_await std::move (result_);
    std::unique_lock<std::mutex> lock (gate_.mutex);
    gate_.entered = true;
    gate_.changed.notify_all ();
    gate_.changed.wait (lock, [&] { return gate_.released; });
}

bool submit_dealer_dontwait (zlink::dealer_socket_t &dealer_,
                             const std::string &payload_)
{
    zlink_msg_t part;
    assert (zlink_msg_init_size (&part, payload_.size ()) == ZLINK_CONFIG_OK);
    if (!payload_.empty ())
        std::memcpy (zlink_msg_data (&part), payload_.data (), payload_.size ());
    const zlink_submit_result_t result = zlink_send_part (
      zlink::detail::native_handle (dealer_), &part,
      ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL);
    assert (zlink_msg_close (&part) == ZLINK_CONFIG_OK);
    assert (result == ZLINK_SUBMIT_OK || result == ZLINK_SUBMIT_BACKPRESSURED);
    return result == ZLINK_SUBMIT_OK;
}

bool submit_router_dontwait (zlink::router_socket_t &router_,
                             const zlink::routing_id_t &target_,
                             const std::string &payload_)
{
    zlink_msg_t part;
    assert (zlink_msg_init_size (&part, payload_.size ()) == ZLINK_CONFIG_OK);
    if (!payload_.empty ())
        std::memcpy (zlink_msg_data (&part), payload_.data (), payload_.size ());
    const zlink_routing_id_t target =
      zlink::detail::routing_id_native_value (target_);
    const zlink_submit_result_t result = zlink_send_part_rid (
      zlink::detail::native_handle (router_), &target, &part,
      ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL);
    assert (zlink_msg_close (&part) == ZLINK_CONFIG_OK);
    assert (result == ZLINK_SUBMIT_OK || result == ZLINK_SUBMIT_BACKPRESSURED);
    return result == ZLINK_SUBMIT_OK;
}

template <typename T>
concept has_async_terminal_t = requires (T &&operation_) {
    std::move (operation_).async ();
};

static_assert (!has_async_terminal_t<zlink::reply_submit_operation_t>);
static_assert (std::is_same_v<
               decltype (std::declval<zlink::reply_submit_operation_t &&> ().submit ()),
               void>);

class owned_coroutine_t
{
  public:
    static inline std::deque<std::function<void ()>> *resume_queue = nullptr;
    static inline std::mutex resume_mutex;
    static inline std::condition_variable resume_changed;

    struct promise_type
    {
        owned_coroutine_t get_return_object ()
        {
            return owned_coroutine_t (
              std::coroutine_handle<promise_type>::from_promise (*this));
        }
        std::suspend_never initial_suspend () noexcept { return {}; }
        std::suspend_always final_suspend () noexcept { return {}; }
        zlink::detail::async_continuation_scheduler_t
        zlink_continuation_scheduler ()
        {
            if (!resume_queue)
                return {};
            return [] (std::function<void ()> work) {
                {
                    std::lock_guard<std::mutex> lock (resume_mutex);
                    resume_queue->push_back (std::move (work));
                }
                resume_changed.notify_all ();
            };
        }
        void return_void () noexcept {}
        void unhandled_exception () noexcept { std::terminate (); }
    };

    owned_coroutine_t (owned_coroutine_t &&other_) noexcept :
        _handle (std::exchange (other_._handle, {}))
    {
    }
    ~owned_coroutine_t ()
    {
        if (_handle)
            _handle.destroy ();
    }

  private:
    explicit owned_coroutine_t (std::coroutine_handle<promise_type> handle_) :
        _handle (handle_)
    {
    }
    std::coroutine_handle<promise_type> _handle;
};

owned_coroutine_t suspend_owned_coroutine (
  zlink::async_result_t<void> result_, std::atomic<bool> &resumed_)
{
    co_await std::move (result_);
    resumed_.store (true, std::memory_order_release);
}

void test_direct_awaitable_fast_completion_and_abandon ()
{
    auto ready_state =
      std::make_shared<zlink::detail::async_operation_state_t<int>> ();
    assert (ready_state->complete (42));
    auto ready_completion = await_result (
      zlink::detail::async_result_access_t::make<int> (ready_state));
    assert (ready_completion.take () == 42);

    auto pending_state =
      std::make_shared<zlink::detail::async_operation_state_t<void>> ();
    std::atomic<int> cancel_count{0};
    std::atomic<bool> resumed{false};
    const std::weak_ptr<zlink::detail::async_operation_state_t<void>> weak_state =
      pending_state;
    pending_state->set_cancel ([weak_state, &cancel_count] {
        cancel_count.fetch_add (1, std::memory_order_relaxed);
        if (const auto state = weak_state.lock ()) {
            state->fail (std::make_exception_ptr (zlink::submit_error_t (
              zlink::submit_result_t::terminated, ECANCELED)));
        }
        return true;
    });
    {
        auto operation = suspend_owned_coroutine (
          zlink::detail::async_result_access_t::make<void> (pending_state),
          resumed);
    }
    assert (cancel_count.load (std::memory_order_relaxed) == 1);
    assert (!resumed.load (std::memory_order_acquire));
    assert (pending_state->ready ());

    auto queued_state =
      std::make_shared<zlink::detail::async_operation_state_t<void>> ();
    std::deque<std::function<void ()>> delayed_resumes;
    owned_coroutine_t::resume_queue = &delayed_resumes;
    {
        auto operation = suspend_owned_coroutine (
          zlink::detail::async_result_access_t::make<void> (queued_state),
          resumed);
        assert (queued_state->complete ());
        std::unique_lock<std::mutex> lock (owned_coroutine_t::resume_mutex);
        assert (owned_coroutine_t::resume_changed.wait_for (
          lock, std::chrono::seconds (2),
          [&] { return delayed_resumes.size () == 1; }));
    }
    std::function<void ()> delayed_resume;
    {
        std::lock_guard<std::mutex> lock (owned_coroutine_t::resume_mutex);
        assert (delayed_resumes.size () == 1);
        delayed_resume = std::move (delayed_resumes.front ());
        delayed_resumes.pop_front ();
    }
    delayed_resume ();
    assert (!resumed.load (std::memory_order_acquire));
    owned_coroutine_t::resume_queue = nullptr;
}

void test_request_dealer_router_roundtrip ()
{
    zlink::context_t ctx;
    zlink::dealer_socket_t dealer_socket (ctx);
    zlink::router_socket_t router_socket (ctx);
    zlink::socket_monitor_t router_monitor = router_socket.monitor_open ();
    zlink::socket_monitor_t dealer_monitor = dealer_socket.monitor_open ();
    const std::string routing_id_text = "request-reply-client";
    zlink::routing_id_t routing_id = zlink::routing_id_t::from (
      reinterpret_cast<const uint8_t *> (routing_id_text.data ()), routing_id_text.size ());

    const std::string endpoint = zlink_cpp_contract::unique_inproc ("rr-cpp");
    dealer_socket.set_routing_id (routing_id);
    router_socket.bind (endpoint);
    dealer_socket.connect (endpoint);
    assert (zlink_cpp_contract::wait_for_socket_monitor_event (
      router_monitor, static_cast<uint64_t> (zlink::monitor_event::connection_ready), 2000));
    assert (zlink_cpp_contract::wait_for_socket_monitor_event (
      dealer_monitor, static_cast<uint64_t> (zlink::monitor_event::connection_ready), 2000));
    std::this_thread::sleep_for (std::chrono::milliseconds (50));

    zlink::message_t warmup = make_request_message ("warmup");
    await_send (dealer_socket.send ().message (warmup).async ());
    std::this_thread::sleep_for (std::chrono::milliseconds (50));
    zlink::received_t warmup_received;
    assert (router_socket.recv (warmup_received) == 0);
    assert (warmup_received.parts ().size () == 1);
    assert (warmup_received.parts ()[0].to_string () == "warmup");

    zlink::message_t request = make_request_message ("request:ping");
    std::future<void> router_done = std::async (std::launch::async, [&router_socket] () {
        zlink::received_t request;
        assert (router_socket.recv (request) == 0);
        assert (request.parts ().size () == 1);
        assert (request.request_seq ().has_value ());
        assert (*request.request_seq () != 0u);

        zlink::message_t reply = make_request_message ("reply:ok");
        auto reply_operation = request.reply ().message (reply);
        assert (reply.valid ());
        std::move (reply_operation).submit ();
        assert (!reply.valid ());
    });

    auto request_operation = dealer_socket.request ().message (std::move (request));
    assert (!request.valid ());
    auto request_completion = await_result (
      std::move (request_operation).timeout (std::chrono::milliseconds (5000)).async ());
    assert (!request.valid ());
    const std::vector<zlink::message_t> reply = request_completion.take ();
    assert (reply.size () == 1);
    assert (reply[0].to_string () == "reply:ok");
    router_done.get ();
}

void test_request_direct_await_suspends_until_reply ()
{
    zlink::context_t ctx;
    zlink::dealer_socket_t dealer_socket (ctx);
    zlink::router_socket_t router_socket (ctx);
    zlink::socket_monitor_t router_monitor = router_socket.monitor_open ();
    zlink::socket_monitor_t dealer_monitor = dealer_socket.monitor_open ();

    const std::string endpoint = zlink_cpp_contract::unique_inproc ("rr-cpp-wait-zero");
    router_socket.bind (endpoint);
    dealer_socket.connect (endpoint);
    assert (zlink_cpp_contract::wait_for_socket_monitor_event (
      router_monitor, static_cast<uint64_t> (zlink::monitor_event::connection_ready), 2000));
    assert (zlink_cpp_contract::wait_for_socket_monitor_event (
      dealer_monitor, static_cast<uint64_t> (zlink::monitor_event::connection_ready), 2000));
    std::this_thread::sleep_for (std::chrono::milliseconds (50));

    zlink::message_t request = make_request_message ("request:wait-zero");
    std::future<void> router_done = std::async (std::launch::async, [&router_socket] () {
        zlink::received_t request;
        assert (router_socket.recv (request) == 0);
        assert (request.parts ().size () == 1);
        assert (request.request_seq ().has_value ());

        zlink::message_t reply = make_request_message ("reply:wait-zero");
        request.reply ().message (reply).submit ();
    });

    auto request_completion = await_result (
      dealer_socket.request ()
        .message (request)
        .timeout (std::chrono::milliseconds (5000))
        .async ());
    assert (request_completion.wait_for (std::chrono::seconds (2)));

    const std::vector<zlink::message_t> reply = request_completion.take ();
    assert (reply.size () == 1);
    assert (reply[0].to_string () == "reply:wait-zero");
    router_done.get ();
}

void test_dealer_request_without_initial_routed_target_is_terminal ()
{
    zlink::context_t ctx;
    zlink::dealer_socket_t dealer (ctx);
    zlink::router_socket_t router (ctx);
    zlink::socket_monitor_t dealer_monitor = dealer.monitor_open ();
    dealer.set_routing_id (
      zlink::routing_id_t::from ("late-target-dealer"));
    const std::string endpoint =
      zlink_cpp_contract::unique_tcp ("dealer-first-target");
    dealer.connect (endpoint);

    zlink::message_t payload = make_request_message ("request:late-target");
    bool terminal_without_target = false;
    try {
        (void) dealer.request ()
          .message (payload)
          .timeout (std::chrono::milliseconds (3000))
          .async ();
        assert (false && "request without an initial exact target must fail");
    }
    catch (const zlink::submit_error_t &error) {
        terminal_without_target = true;
        assert (error.result () == zlink::submit_result_t::not_connected);
        assert (error.internal_errno () == ENOTCONN);
    }
    assert (terminal_without_target);
    assert (payload.valid ());

    router.bind (endpoint);
    assert (zlink_cpp_contract::wait_for_socket_monitor_event (
      dealer_monitor,
      static_cast<uint64_t> (zlink::monitor_event::connection_ready),
      2000));
    zlink::received_t ghost;
    assert (router.recv (ghost, zlink::recv_flags_t::dontwait) != 0);

    zlink::message_t fresh = make_request_message ("request:fresh-target");
    auto completion = await_result (
      dealer.request ()
        .message (std::move (fresh))
        .timeout (std::chrono::milliseconds (3000))
        .async ());
    std::future<void> router_done = std::async (
      std::launch::async, [&router] {
          zlink::received_t received;
          bool received_request = false;
          for (int attempt = 0; attempt < 200 && !received_request;
               ++attempt) {
              const int result =
                router.recv (received, zlink::recv_flags_t::dontwait);
              received_request = result == 0;
              if (!received_request)
                  std::this_thread::sleep_for (
                    std::chrono::milliseconds (10));
          }
          assert (received_request);
          assert (received.request_seq ().has_value ());
          assert (received.first_part ().to_string ()
                  == "request:fresh-target");
          zlink::message_t reply =
            make_request_message ("reply:fresh-target");
          received.reply ().message (reply).submit ();
      });

    assert (completion.wait_for (std::chrono::seconds (5)));
    const auto reply = completion.take ();
    assert (reply.size () == 1);
    assert (reply.front ().to_string () == "reply:fresh-target");
    router_done.get ();
}

void test_dealer_send_without_initial_routed_target_is_terminal ()
{
    zlink::context_t ctx;
    zlink::dealer_socket_t dealer (ctx);
    zlink::router_socket_t router (ctx);
    zlink::socket_monitor_t dealer_monitor = dealer.monitor_open ();
    dealer.set_routing_id (
      zlink::routing_id_t::from ("late-send-dealer"));
    const std::string endpoint =
      zlink_cpp_contract::unique_tcp ("dealer-first-send-target");
    dealer.connect (endpoint);

    zlink::message_t payload = make_request_message ("send:late-target");
    bool terminal_without_target = false;
    try {
        (void) dealer.send ().message (payload).async ();
        assert (false && "send without an initial exact target must fail");
    }
    catch (const zlink::submit_error_t &error) {
        terminal_without_target = true;
        assert (error.result () == zlink::submit_result_t::not_connected);
        assert (error.internal_errno () == ENOTCONN);
    }
    assert (terminal_without_target);
    assert (payload.valid ());

    router.bind (endpoint);
    assert (zlink_cpp_contract::wait_for_socket_monitor_event (
      dealer_monitor,
      static_cast<uint64_t> (zlink::monitor_event::connection_ready),
      2000));

    zlink::received_t received;
    assert (router.recv (received, zlink::recv_flags_t::dontwait) != 0);

    zlink::message_t fresh = make_request_message ("send:fresh-target");
    auto fresh_completion = await_result (
      dealer.send ().message (std::move (fresh)).async ());
    assert (fresh_completion.wait_for (std::chrono::seconds (2)));
    fresh_completion.take ();

    bool received_message = false;
    for (int attempt = 0; attempt < 200 && !received_message; ++attempt) {
        received_message =
          router.recv (received, zlink::recv_flags_t::dontwait) == 0;
        if (!received_message)
            std::this_thread::sleep_for (std::chrono::milliseconds (10));
    }
    assert (received_message);
    assert (received.first_part ().to_string () == "send:fresh-target");
}

void test_routed_send_direct_await_and_cancel_are_event_driven ()
{
    zlink::context_t ctx;
    zlink::dealer_socket_t dealer (ctx);
    zlink::router_socket_t router (ctx);
    dealer.set_routing_id (zlink::routing_id_t::from ("async-dealer"));
    router.set_routing_id (zlink::routing_id_t::from ("async-router"));
    const std::string endpoint =
      zlink_cpp_contract::unique_inproc ("routed-send-async");
    router.bind (endpoint);
    dealer.connect (endpoint);
    std::this_thread::sleep_for (std::chrono::milliseconds (50));

    zlink::message_t future_payload = make_request_message ("awaited-send");
    auto send_completion = await_result (
      dealer.send ().message (std::move (future_payload)).async ());
    assert (send_completion.wait_for (std::chrono::seconds (2)));
    send_completion.take ();
    zlink::received_t received;
    assert (router.recv (received) == 0);
    assert (received.first_part ().to_string () == "awaited-send");

    zlink::message_t cancel_payload = make_request_message ("cancel-race");
    zlink::async_result_t<void> cancelled =
      dealer.send ().message (std::move (cancel_payload)).async ();
    const bool cancellation_won = cancelled.cancel ();
    auto cancelled_completion = await_result (std::move (cancelled));
    if (cancellation_won) {
        try {
            cancelled_completion.take ();
            assert (false && "cancelled send must fail its completion");
        }
        catch (const zlink::submit_error_t &error) {
            assert (error.result () == zlink::submit_result_t::terminated);
            assert (error.internal_errno () == ECANCELED);
        }
    } else {
        cancelled_completion.take ();
    }
}

void test_routed_async_builder_does_not_outlive_socket_anchor ()
{
    zlink::context_t ctx;
    zlink::message_t payload = make_request_message ("expired-builder");
    std::optional<zlink::routed_send_submit_operation_t> builder;
    {
        zlink::dealer_socket_t dealer (ctx);
        builder.emplace (dealer.send ().message (payload));
    }
    try {
        (void) std::move (*builder).async ();
        assert (false && "builder must not dereference a destroyed socket anchor");
    }
    catch (const zlink::submit_error_t &error) {
        assert (error.result () == zlink::submit_result_t::invalid_state);
        assert (error.internal_errno () == EINVAL);
    }
    assert (payload.valid ());

    zlink::dealer_socket_t closed_dealer (ctx);
    zlink::message_t closed_payload = make_request_message ("closed-builder");
    auto closed_builder = closed_dealer.send ().message (closed_payload);
    closed_dealer.close ();
    try {
        (void) std::move (closed_builder).async ();
        assert (false && "builder must not submit through a closed socket anchor");
    }
    catch (const zlink::submit_error_t &error) {
        assert (error.result () == zlink::submit_result_t::invalid_state);
        assert (error.internal_errno () == EINVAL);
    }
    assert (closed_payload.valid ());
}

void test_routed_send_async_isolates_a_backpressure_from_b ()
{
    zlink::context_t ctx;
    zlink::router_socket_t router (ctx);
    zlink::dealer_socket_t dealer_a (ctx);
    zlink::dealer_socket_t dealer_b (ctx);
    zlink::socket_monitor_t router_monitor = router.monitor_open ();
    zlink::socket_monitor_t dealer_a_monitor = dealer_a.monitor_open ();
    zlink::socket_monitor_t dealer_b_monitor = dealer_b.monitor_open ();
    const zlink::routing_id_t rid_a = zlink::routing_id_t::from ("async-a");
    const zlink::routing_id_t rid_b = zlink::routing_id_t::from ("async-b");
    dealer_a.set_routing_id (rid_a);
    dealer_b.set_routing_id (rid_b);

    const uint64_t hwm = UINT64_C (65536) + sizeof (zlink_msg_t);
    router.options ().send_hwm (zlink::byte_count_t::bytes (hwm));
    dealer_a.options ().recv_hwm (zlink::byte_count_t::bytes (hwm));
    dealer_b.options ().recv_hwm (zlink::byte_count_t::bytes (hwm));

    const std::string endpoint =
      zlink_cpp_contract::unique_inproc ("routed-send-target-isolation");
    router.bind (endpoint);
    dealer_a.connect (endpoint);
    dealer_b.connect (endpoint);
    assert (zlink_cpp_contract::wait_for_socket_monitor_event (
      router_monitor,
      static_cast<uint64_t> (zlink::monitor_event::connection_ready),
      2000));
    assert (zlink_cpp_contract::wait_for_socket_monitor_event (
      router_monitor,
      static_cast<uint64_t> (zlink::monitor_event::connection_ready),
      2000));
    assert (zlink_cpp_contract::wait_for_socket_monitor_event (
      dealer_a_monitor,
      static_cast<uint64_t> (zlink::monitor_event::connection_ready),
      2000));
    assert (zlink_cpp_contract::wait_for_socket_monitor_event (
      dealer_b_monitor,
      static_cast<uint64_t> (zlink::monitor_event::connection_ready),
      2000));

    const std::string filler_text (65536, 'a');
    bool a_backpressured = false;
    for (int i = 0; i < 16; ++i) {
        if (!submit_router_dontwait (router, rid_a, filler_text)) {
            a_backpressured = true;
            break;
        }
    }
    assert (a_backpressured);

    zlink::message_t pending_a_first =
      make_request_message ("a-after-credit-1");
    zlink::message_t pending_a_second =
      make_request_message ("a-after-credit-2");
    const auto submit_started = std::chrono::steady_clock::now ();
    auto pending_a_first_completion = await_result (
      router.send (rid_a).message (std::move (pending_a_first)).async ());
    auto pending_a_second_completion = await_result (
      router.send (rid_a).message (std::move (pending_a_second)).async ());
    assert (std::chrono::steady_clock::now () - submit_started
            < std::chrono::milliseconds (250));
    assert (!pending_a_first_completion.wait_for (std::chrono::milliseconds (50)));
    assert (!pending_a_second_completion.wait_for (std::chrono::milliseconds (0)));

    router.options ().send_timeout (std::chrono::milliseconds (0));
    zlink::message_t progress_b = make_request_message ("b-progress");
    auto b_completion = await_result (
      router.send (rid_b).message (std::move (progress_b)).async ());
    assert (b_completion.wait_for (std::chrono::seconds (2)));
    b_completion.take ();
    zlink::received_t received_b;
    assert (dealer_b.recv (received_b) == 0);
    assert (received_b.first_part ().to_string () == "b-progress");
    router.options ().send_timeout (std::chrono::milliseconds (1000));

    zlink::received_t received_a;
    std::vector<std::string> resumed_payloads;
    for (int i = 0; i < 20 && resumed_payloads.size () != 2; ++i) {
        assert (dealer_a.recv (received_a) == 0);
        const std::string payload = received_a.first_part ().to_string ();
        if (payload.rfind ("a-after-credit-", 0) == 0)
            resumed_payloads.push_back (payload);
        received_a.close ();
    }
    assert (resumed_payloads.size () == 2);
    assert (pending_a_first_completion.wait_for (std::chrono::seconds (2)));
    assert (pending_a_second_completion.wait_for (std::chrono::seconds (2)));
    pending_a_first_completion.take ();
    pending_a_second_completion.take ();

    a_backpressured = false;
    for (int i = 0; i < 16; ++i) {
        if (!submit_router_dontwait (router, rid_a, filler_text)) {
            a_backpressured = true;
            break;
        }
    }
    assert (a_backpressured);

    zlink::message_t cancel_payload = make_request_message ("cancel-pending");
    zlink::async_result_t<void> cancel_pending =
      router.send (rid_a).message (cancel_payload).async ();
    std::atomic<int> cancel_winners{0};
    std::thread cancel_one ([&] {
        if (cancel_pending.cancel ())
            cancel_winners.fetch_add (1, std::memory_order_relaxed);
    });
    std::thread cancel_two ([&] {
        if (cancel_pending.cancel ())
            cancel_winners.fetch_add (1, std::memory_order_relaxed);
    });
    cancel_one.join ();
    cancel_two.join ();
    assert (cancel_winners.load (std::memory_order_relaxed) == 1);
    auto cancelled_completion = await_result (std::move (cancel_pending));
    try {
        cancelled_completion.take ();
        assert (false && "exactly one cancellation must win");
    }
    catch (const zlink::submit_error_t &error) {
        assert (error.result () == zlink::submit_result_t::terminated);
        assert (error.internal_errno () == ECANCELED);
    }

    zlink::message_t close_pending = make_request_message ("close-pending");
    auto close_completion = await_result (
      router.send (rid_a).message (std::move (close_pending)).async ());
    assert (!close_completion.wait_for (std::chrono::milliseconds (50)));
    router.close ();
    assert (close_completion.wait_for (std::chrono::seconds (2)));
    try {
        close_completion.take ();
        assert (false && "socket close must terminate pending admission");
    }
    catch (const zlink::submit_error_t &error) {
        assert (error.result () == zlink::submit_result_t::terminated);
    }
}

void test_routed_send_async_progress_is_independent_of_another_continuation ()
{
    zlink::context_t ctx;
    zlink::router_socket_t router (ctx);
    zlink::dealer_socket_t dealer_a (ctx);
    zlink::dealer_socket_t dealer_b (ctx);
    zlink::socket_monitor_t router_monitor = router.monitor_open ();
    zlink::socket_monitor_t dealer_a_monitor = dealer_a.monitor_open ();
    zlink::socket_monitor_t dealer_b_monitor = dealer_b.monitor_open ();
    const zlink::routing_id_t rid_a =
      zlink::routing_id_t::from ("continuation-a");
    const zlink::routing_id_t rid_b =
      zlink::routing_id_t::from ("continuation-b");
    dealer_a.set_routing_id (rid_a);
    dealer_b.set_routing_id (rid_b);
    router.options ().send_timeout (std::chrono::seconds (10));

    const std::string endpoint =
      zlink_cpp_contract::unique_inproc ("routed-send-continuation-isolation");
    router.bind (endpoint);
    dealer_a.connect (endpoint);
    dealer_b.connect (endpoint);
    assert (zlink_cpp_contract::wait_for_socket_monitor_event (
      router_monitor,
      static_cast<uint64_t> (zlink::monitor_event::connection_ready),
      2000));
    assert (zlink_cpp_contract::wait_for_socket_monitor_event (
      router_monitor,
      static_cast<uint64_t> (zlink::monitor_event::connection_ready),
      2000));
    assert (zlink_cpp_contract::wait_for_socket_monitor_event (
      dealer_a_monitor,
      static_cast<uint64_t> (zlink::monitor_event::connection_ready),
      2000));
    assert (zlink_cpp_contract::wait_for_socket_monitor_event (
      dealer_b_monitor,
      static_cast<uint64_t> (zlink::monitor_event::connection_ready),
      2000));

    continuation_gate_t gate;
    zlink::message_t first_payload =
      make_request_message ("first-continuation-held");
    auto first_completion = await_send_and_hold_continuation (
      router.send (rid_a)
        .message (std::move (first_payload))
        .async (),
      gate);
    {
        std::unique_lock<std::mutex> lock (gate.mutex);
        assert (gate.changed.wait_for (
          lock, std::chrono::seconds (2), [&] { return gate.entered; }));
    }

    zlink::message_t second_payload =
      make_request_message ("second-target-progress");
    auto second_completion = await_result (
      router.send (rid_b)
        .message (std::move (second_payload))
        .async ());
    const bool second_completed_while_first_continuation_was_held =
      second_completion.wait_for (std::chrono::seconds (2));

    {
        std::lock_guard<std::mutex> lock (gate.mutex);
        gate.released = true;
    }
    gate.changed.notify_all ();
    assert (first_completion.wait_for (std::chrono::seconds (2)));
    first_completion.take ();
    assert (second_completion.wait_for (std::chrono::seconds (2)));
    second_completion.take ();

    zlink::received_t received_a;
    zlink::received_t received_b;
    assert (dealer_a.recv (received_a) == 0);
    assert (dealer_b.recv (received_b) == 0);
    assert (received_a.first_part ().to_string ()
            == "first-continuation-held");
    assert (received_b.first_part ().to_string ()
            == "second-target-progress");
    assert (second_completed_while_first_continuation_was_held
            && "RID B admission must not run on RID A's continuation");
}

void test_routed_request_deadline_includes_admission_wait ()
{
    zlink::context_t ctx;
    zlink::dealer_socket_t dealer (ctx);
    zlink::router_socket_t router (ctx);
    dealer.set_routing_id (zlink::routing_id_t::from ("deadline-dealer"));
    router.set_routing_id (zlink::routing_id_t::from ("deadline-router"));
    const uint64_t hwm = UINT64_C (65536) + sizeof (zlink_msg_t);
    dealer.options ().send_hwm (zlink::byte_count_t::bytes (hwm));
    router.options ().recv_hwm (zlink::byte_count_t::bytes (hwm));

    const std::string endpoint =
      zlink_cpp_contract::unique_inproc ("request-admission-deadline");
    router.bind (endpoint);
    dealer.connect (endpoint);
    std::this_thread::sleep_for (std::chrono::milliseconds (100));

    const std::string filler_text (65536, 'd');
    bool backpressured = false;
    for (int i = 0; i < 16; ++i) {
        if (!submit_dealer_dontwait (dealer, filler_text)) {
            backpressured = true;
            break;
        }
    }
    assert (backpressured);

    dealer.options ().send_timeout (std::chrono::milliseconds (100));
    zlink::message_t send = make_request_message ("deadline-send");
    auto send_completion = await_result (
      dealer.send ().message (std::move (send)).async ());
    assert (send_completion.wait_for (std::chrono::seconds (2)));
    try {
        send_completion.take ();
        assert (false && "send admission must honor the original SNDTIMEO");
    }
    catch (const zlink::submit_error_t &error) {
        assert (error.result () == zlink::submit_result_t::backpressured);
        assert (error.internal_errno () == ETIMEDOUT);
    }

    zlink::message_t request = make_request_message ("deadline-request");
    const auto started = std::chrono::steady_clock::now ();
    auto request_completion = await_result (
      dealer.request ()
        .message (request)
        .timeout (std::chrono::milliseconds (100))
        .async ());
    assert (std::chrono::steady_clock::now () - started
            < std::chrono::milliseconds (250));
    assert (request_completion.wait_for (std::chrono::seconds (2)));
    try {
        (void) request_completion.take ();
        assert (false && "admission wait must consume the original deadline");
    }
    catch (const zlink::request_error_t &error) {
        assert (error.result () == zlink::request_result_t::timed_out);
        assert (error.internal_errno () == ETIMEDOUT);
    }
}

void test_request_router_preserves_data_recv_surface ()
{
    zlink::context_t ctx;
    zlink::dealer_socket_t dealer_socket (ctx);
    zlink::router_socket_t router_socket (ctx);
    zlink::socket_monitor_t router_monitor = router_socket.monitor_open ();
    zlink::socket_monitor_t dealer_monitor = dealer_socket.monitor_open ();

    const std::string endpoint = zlink_cpp_contract::unique_inproc ("rr-cpp-data");
    router_socket.bind (endpoint);
    dealer_socket.connect (endpoint);
    assert (zlink_cpp_contract::wait_for_socket_monitor_event (
      router_monitor, static_cast<uint64_t> (zlink::monitor_event::connection_ready), 2000));
    assert (zlink_cpp_contract::wait_for_socket_monitor_event (
      dealer_monitor, static_cast<uint64_t> (zlink::monitor_event::connection_ready), 2000));
    std::this_thread::sleep_for (std::chrono::milliseconds (50));

    zlink::message_t data = make_request_message ("plain-data");
    await_send (dealer_socket.send ().message (data).async ());

    zlink::received_t received;
    assert (router_socket.recv (received) == 0);
    assert (received.parts ().size () == 1);
    assert (received.parts ()[0].to_string () == "plain-data");
    assert (!received.request_seq ().has_value ());
}

void test_received_reply_rejects_non_none_flags ()
{
    zlink::context_t ctx;
    zlink::dealer_socket_t dealer_socket (ctx);
    zlink::router_socket_t router_socket (ctx);
    zlink::socket_monitor_t router_monitor = router_socket.monitor_open ();
    zlink::socket_monitor_t dealer_monitor = dealer_socket.monitor_open ();
    const std::string routing_id_text = "request-reply-flags-client";
    zlink::routing_id_t routing_id = zlink::routing_id_t::from (
      reinterpret_cast<const uint8_t *> (routing_id_text.data ()), routing_id_text.size ());

    const std::string endpoint = zlink_cpp_contract::unique_inproc ("rr-cpp-reply-flags");
    dealer_socket.set_routing_id (routing_id);
    router_socket.bind (endpoint);
    dealer_socket.connect (endpoint);
    assert (zlink_cpp_contract::wait_for_socket_monitor_event (
      router_monitor, static_cast<uint64_t> (zlink::monitor_event::connection_ready), 2000));
    assert (zlink_cpp_contract::wait_for_socket_monitor_event (
      dealer_monitor, static_cast<uint64_t> (zlink::monitor_event::connection_ready), 2000));
    std::this_thread::sleep_for (std::chrono::milliseconds (50));

    zlink::message_t request = make_request_message ("request:flags");
    std::future<void> router_done = std::async (std::launch::async, [&router_socket] () {
        zlink::received_t received;
        assert (router_socket.recv (received) == 0);
        assert (received.request_seq ().has_value ());

        zlink::message_t rejected = make_request_message ("reply:rejected");
        try {
            received.reply ().message (rejected).flags (zlink::recv_flags_t::dontwait).submit ();
            assert (false && "reply flags must be rejected");
        }
        catch (const zlink::submit_error_t &error) {
            assert (error.result () == zlink::submit_result_t::not_supported);
            assert (error.internal_errno () == ENOTSUP);
        }

        zlink::message_t accepted = make_request_message ("reply:ok");
        received.reply ().message (accepted).submit ();
    });

    auto request_completion = await_result (
      dealer_socket.request ()
        .message (request)
        .timeout (std::chrono::milliseconds (5000))
        .async ());
    const std::vector<zlink::message_t> reply = request_completion.take ();
    assert (reply.size () == 1);
    assert (reply[0].to_string () == "reply:ok");
    router_done.get ();
}

void test_reply_submit_is_one_shot_without_ghost_retry ()
{
    zlink::context_t ctx;
    zlink::router_socket_t router (ctx);
    zlink::dealer_socket_t dealer (ctx);
    const zlink::routing_id_t dealer_rid =
      zlink::routing_id_t::from ("reply-route-dealer");

    const std::string endpoint =
      zlink_cpp_contract::unique_inproc ("reply-one-shot-route");
    router.bind (endpoint);

    zlink::message_t one_shot = make_request_message ("reply:one-shot");
    try {
        router.reply (dealer_rid, 41).message (one_shot).submit ();
        assert (false && "reply without a completion route must backpressure");
    }
    catch (const zlink::submit_error_t &error) {
        assert (error.result () == zlink::submit_result_t::backpressured);
    }

    dealer.set_routing_id (dealer_rid);
    dealer.connect (endpoint);
    std::this_thread::sleep_for (std::chrono::milliseconds (50));

    zlink::received_t ghost;
    assert (dealer.recv (ghost, zlink::recv_flags_t::dontwait) != 0);
}

void test_raw_router_reply_maps_submit_result ()
{
    zlink::context_t ctx;
    zlink::router_socket_t router_socket (ctx);
    const zlink::routing_id_t routing_id = zlink::routing_id_t::from ("raw-reply-client");
    zlink::message_t reply = make_request_message ("reply:invalid-sequence");

    try {
        // Core reports this invalid request sequence as a submit result, not as a native errno.
        router_socket.reply (routing_id, 0).message (reply).submit ();
        assert (false && "invalid raw reply sequence must fail");
    }
    catch (const zlink::submit_error_t &error) {
        assert (error.result () == zlink::submit_result_t::invalid_argument);
        assert (error.internal_errno () == EINVAL);
    }
}

} // namespace

int main ()
{
    test_direct_awaitable_fast_completion_and_abandon ();
    test_request_dealer_router_roundtrip ();
    test_request_direct_await_suspends_until_reply ();
    test_dealer_request_without_initial_routed_target_is_terminal ();
    test_dealer_send_without_initial_routed_target_is_terminal ();
    test_routed_send_direct_await_and_cancel_are_event_driven ();
    test_routed_async_builder_does_not_outlive_socket_anchor ();
    test_routed_send_async_isolates_a_backpressure_from_b ();
    test_routed_send_async_progress_is_independent_of_another_continuation ();
    test_routed_request_deadline_includes_admission_wait ();
    test_request_router_preserves_data_recv_surface ();
    test_received_reply_rejects_non_none_flags ();
    test_reply_submit_is_one_shot_without_ghost_retry ();
    test_raw_router_reply_maps_submit_result ();
    std::quick_exit (0);
}
