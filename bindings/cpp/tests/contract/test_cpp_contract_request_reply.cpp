/* SPDX-License-Identifier: MPL-2.0 */

#include "support.hpp"

#include <Runtime/Messaging/async_operation_state.hpp>
#include <Runtime/Messaging/operation_state.hpp>
#include <Runtime/Messaging/operation_submit.hpp>
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

void test_send_async_inline_completion ()
{
    zlink::context_t ctx;
    zlink::pair_socket_t sender (ctx);
    zlink::pair_socket_t receiver (ctx);
    const std::string endpoint = zlink_cpp_contract::unique_inproc ("cpp-send-async-inline");
    sender.bind (endpoint);
    receiver.connect (endpoint);
    std::this_thread::sleep_for (std::chrono::milliseconds (50));

    zlink::message_t payload = make_request_message ("send-async-inline");
    auto completion = await_result (
      std::move (sender.send ().message (payload).async ()));
    completion.take ();
    assert (!payload.valid ());

    zlink::received_t received;
    assert (receiver.recv (received) == 0);
    assert (received.first_part ().to_string () == "send-async-inline");
}

void test_routed_send_async_inline_completion ()
{
    zlink::context_t ctx;
    zlink::dealer_socket_t dealer (ctx);
    zlink::router_socket_t router (ctx);
    dealer.set_routing_id (zlink::routing_id_t::from ("cpp-routed-async-dealer"));
    const std::string endpoint = zlink_cpp_contract::unique_inproc ("cpp-routed-async-inline");
    router.bind (endpoint);
    dealer.connect (endpoint);
    std::this_thread::sleep_for (std::chrono::milliseconds (50));

    zlink::message_t payload = make_request_message ("routed-send-async-inline");
    auto completion = await_result (
      std::move (dealer.send ().message (payload).async ()));
    completion.take ();
    assert (!payload.valid ());

    zlink::received_t received;
    assert (router.recv (received) == 0);
    assert (received.first_part ().to_string () == "routed-send-async-inline");
}

bool small_hwm_contract_gate_enabled ()
{
    const char *const value = std::getenv ("ZLINK_CPP_CONTRACT_SMALL_HWM");
    return value && std::strcmp (value, "0") != 0;
}

struct small_hwm_pair_fixture_t
{
    zlink::context_t ctx;
    zlink::pair_socket_t sender{ctx};
    zlink::pair_socket_t receiver{ctx};

    explicit small_hwm_pair_fixture_t (const char *name_)
    {
        ctx.options ().auto_hwm_enabled (false);
        const uint64_t hwm = UINT64_C (128) + sizeof (zlink_msg_t);
        sender.options ().send_hwm (zlink::byte_count_t::bytes (hwm));
        receiver.options ().recv_hwm (zlink::byte_count_t::bytes (hwm));
        const std::string endpoint = zlink_cpp_contract::unique_inproc (name_);
        sender.bind (endpoint);
        receiver.connect (endpoint);
        std::this_thread::sleep_for (std::chrono::milliseconds (50));
    }

    void fill_until_backpressured ()
    {
        const std::string filler_text (64, 'h');
        for (int attempt = 0; attempt < 64; ++attempt) {
            zlink::message_t filler = make_request_message (filler_text);
            if (!sender.send ()
                   .message (filler)
                   .flags (zlink::send_flags_t::dontwait)
                   .submit ())
                return;
        }
        assert (false && "small HWM did not become backpressured");
    }
};

void test_send_async_pending_completes_after_drain ()
{
    if (!small_hwm_contract_gate_enabled ())
        return;
    small_hwm_pair_fixture_t fixture ("cpp-send-async-pending");
    fixture.fill_until_backpressured ();

    zlink::message_t payload = make_request_message ("send-async-pending");
    auto pending = fixture.sender.send ().message (payload).async ();
    auto completion = await_result (std::move (pending));
    assert (!completion.wait_for (std::chrono::milliseconds (100)));

    zlink::received_t drained;
    assert (fixture.receiver.recv (drained) == 0);
    assert (completion.wait_for (std::chrono::seconds (2)));
    completion.take ();
    assert (!payload.valid ());
}

void test_send_async_timeout_surfaces_timed_out ()
{
    if (!small_hwm_contract_gate_enabled ())
        return;
    small_hwm_pair_fixture_t fixture ("cpp-send-async-timeout");
    fixture.fill_until_backpressured ();

    zlink::message_t payload = make_request_message ("send-async-timeout");
    auto pending = fixture.sender.send ()
                     .message (payload)
                     .timeout (std::chrono::milliseconds (50))
                     .async ();
    auto completion = await_result (std::move (pending));
    assert (completion.wait_for (std::chrono::seconds (2)));
    try {
        completion.take ();
        assert (false && "timed-out send must fail its awaitable");
    }
    catch (const zlink::submit_error_t &error) {
        assert (error.result () == zlink::submit_result_t::not_admitted);
        assert (error.internal_errno () == ETIMEDOUT);
    }
}

void test_send_async_cancel_and_drop ()
{
    if (!small_hwm_contract_gate_enabled ())
        return;
    small_hwm_pair_fixture_t fixture ("cpp-send-async-cancel");
    fixture.fill_until_backpressured ();

    zlink::message_t cancelled_payload = make_request_message ("send-async-cancel");
    auto cancelled = fixture.sender.send ().message (cancelled_payload).async ();
    assert (cancelled.cancel ());
    auto cancelled_completion = await_result (std::move (cancelled));
    assert (cancelled_completion.wait_for (std::chrono::seconds (2)));
    try {
        cancelled_completion.take ();
        assert (false && "cancelled send must fail its awaitable");
    }
    catch (const zlink::submit_error_t &error) {
        assert (error.result () == zlink::submit_result_t::not_admitted);
        assert (error.internal_errno () == ECANCELED);
    }

    {
        zlink::message_t dropped_payload = make_request_message ("send-async-drop");
        auto dropped = fixture.sender.send ().message (dropped_payload).async ();
    }
}

void test_send_async_close_fails_pending_operation ()
{
    if (!small_hwm_contract_gate_enabled ())
        return;
    small_hwm_pair_fixture_t fixture ("cpp-send-async-close");
    fixture.fill_until_backpressured ();

    zlink::message_t payload = make_request_message ("send-async-close");
    auto pending = fixture.sender.send ().message (payload).async ();
    fixture.sender.close ();
    auto completion = await_result (std::move (pending));
    assert (completion.wait_for (std::chrono::seconds (2)));
    try {
        completion.take ();
        assert (false && "closing a pending send must fail it");
    }
    catch (const zlink::submit_error_t &error) {
        assert (error.result () == zlink::submit_result_t::not_admitted);
        assert (error.internal_errno () == ETERM || error.internal_errno () == ECANCELED
                || error.internal_errno () == ESHUTDOWN);
    }
}

void test_request_blocking_submit_returns_reply ()
{
    zlink::context_t ctx;
    zlink::dealer_socket_t dealer (ctx);
    zlink::router_socket_t router (ctx);
    const std::string endpoint = zlink_cpp_contract::unique_inproc ("cpp-request-blocking");
    router.bind (endpoint);
    dealer.connect (endpoint);
    std::this_thread::sleep_for (std::chrono::milliseconds (50));

    std::future<void> server = std::async (std::launch::async, [&router] {
        zlink::received_t request;
        assert (router.recv (request) == 0);
        zlink::message_t reply = make_request_message ("blocking-reply");
        request.reply ().message (reply).submit ();
    });
    zlink::message_t request = make_request_message ("blocking-request");
    const std::vector<zlink::message_t> reply = dealer.request ()
      .message (request)
      .timeout (std::chrono::seconds (2))
      .submit ();
    assert (reply.size () == 1);
    assert (reply[0].to_string () == "blocking-reply");
    server.get ();
}

void test_request_blocking_submit_times_out ()
{
    zlink::context_t ctx;
    zlink::dealer_socket_t dealer (ctx);
    zlink::router_socket_t router (ctx);
    const std::string endpoint = zlink_cpp_contract::unique_inproc ("cpp-request-timeout");
    router.bind (endpoint);
    dealer.connect (endpoint);
    std::this_thread::sleep_for (std::chrono::milliseconds (50));

    zlink::message_t request = make_request_message ("blocking-timeout");
    try {
        (void) dealer.request ().message (request)
          .timeout (std::chrono::milliseconds (50)).submit ();
        assert (false && "blocking request must surface Core timeout");
    }
    catch (const zlink::request_error_t &error) {
        assert (error.result () == zlink::request_result_t::timed_out);
        assert (error.internal_errno () == ETIMEDOUT);
    }
}

void test_request_callback_fires_exactly_once ()
{
    zlink::context_t ctx;
    zlink::dealer_socket_t dealer (ctx);
    zlink::router_socket_t router (ctx);
    const std::string endpoint = zlink_cpp_contract::unique_inproc ("cpp-request-callback");
    router.bind (endpoint);
    dealer.connect (endpoint);
    std::this_thread::sleep_for (std::chrono::milliseconds (50));

    std::mutex mutex;
    std::condition_variable changed;
    int callback_count = 0;
    std::string callback_payload;
    std::future<void> server = std::async (std::launch::async, [&router] {
        zlink::received_t request;
        assert (router.recv (request) == 0);
        zlink::message_t reply = make_request_message ("callback-reply");
        request.reply ().message (reply).submit ();
    });

    zlink::message_t request = make_request_message ("callback-request");
    assert (dealer.request ().message (request).timeout (std::chrono::seconds (2)).submit (
      [&] (zlink::request_result_t result, std::vector<zlink::message_t> parts) {
          assert (result == zlink::request_result_t::ok);
          assert (parts.size () == 1);
          {
              std::lock_guard<std::mutex> lock (mutex);
              ++callback_count;
              callback_payload = parts[0].to_string ();
          }
          for (auto &part : parts)
              part.close ();
          changed.notify_all ();
      }));

    std::unique_lock<std::mutex> lock (mutex);
    assert (changed.wait_for (lock, std::chrono::seconds (2), [&] {
        return callback_count == 1;
    }));
    lock.unlock ();
    server.get ();
    std::this_thread::sleep_for (std::chrono::milliseconds (25));
    assert (callback_count == 1);
    assert (callback_payload == "callback-reply");
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
    dealer_socket.send ().message (warmup).submit ();
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
        dealer.send ().message (payload).submit ();
        assert (false && "send without a route must fail");
    }
    catch (const zlink::submit_error_t &error) {
        // The synchronous terminal reports the Core send verdict verbatim:
        // without an admitted pipe Core has no credit to give.
        terminal_without_target = true;
        assert (error.result () == zlink::submit_result_t::backpressured);
        assert (error.internal_errno () == EAGAIN);
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
    dealer.send ().message (std::move (fresh)).submit ();

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

void test_routed_send_submit_is_synchronous_and_consumes_parts ()
{
    zlink::context_t ctx;
    zlink::dealer_socket_t dealer (ctx);
    zlink::router_socket_t router (ctx);
    dealer.set_routing_id (zlink::routing_id_t::from ("sync-dealer"));
    router.set_routing_id (zlink::routing_id_t::from ("sync-router"));
    const std::string endpoint =
      zlink_cpp_contract::unique_inproc ("routed-send-sync");
    router.bind (endpoint);
    dealer.connect (endpoint);
    std::this_thread::sleep_for (std::chrono::milliseconds (50));

    // The terminal returns on the caller thread; no suspension object is
    // handed back and nothing else has to be pumped for the send to finish.
    zlink::message_t owned = make_request_message ("submitted-send");
    dealer.send ().message (std::move (owned)).submit ();
    zlink::received_t received;
    assert (router.recv (received) == 0);
    assert (received.first_part ().to_string () == "submitted-send");
    received.close ();

    // A borrowed part is consumed by a successful submit.
    zlink::message_t borrowed = make_request_message ("borrowed-send");
    dealer.send ().message (borrowed).submit ();
    assert (!borrowed.valid ());
    assert (router.recv (received) == 0);
    assert (received.first_part ().to_string () == "borrowed-send");
    received.close ();

    // A multipart routed send keeps the whole part sequence on the caller.
    zlink::message_t first = make_request_message ("multi-1");
    zlink::message_t second = make_request_message ("multi-2");
    dealer.send ().message (first).message (second).submit ();
    assert (router.recv (received) == 0);
    assert (received.parts ().size () == 2);
    assert (received.parts ()[0].to_string () == "multi-1");
    assert (received.parts ()[1].to_string () == "multi-2");
    received.close ();
}

void test_routed_builder_does_not_outlive_socket_anchor ()
{
    zlink::context_t ctx;
    zlink::message_t payload = make_request_message ("expired-builder");
    std::optional<zlink::routed_send_submit_operation_t> builder;
    {
        zlink::dealer_socket_t dealer (ctx);
        builder.emplace (dealer.send ().message (payload));
    }
    try {
        std::move (*builder).submit ();
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
        std::move (closed_builder).submit ();
        assert (false && "builder must not submit through a closed socket anchor");
    }
    catch (const zlink::submit_error_t &error) {
        assert (error.result () == zlink::submit_result_t::invalid_state);
        assert (error.internal_errno () == EINVAL);
    }
    assert (closed_payload.valid ());
}

// Backpressure on one ROUTER target is reported to the caller as the Core
// verdict for that submit. The binding neither parks the send nor retries it,
// and a backpressured target does not poison a send to another target.
void test_routed_send_reports_core_backpressure_without_poisoning_b ()
{
    zlink::context_t ctx;
    zlink::router_socket_t router (ctx);
    zlink::dealer_socket_t dealer_a (ctx);
    zlink::dealer_socket_t dealer_b (ctx);
    zlink::socket_monitor_t router_monitor = router.monitor_open ();
    zlink::socket_monitor_t dealer_a_monitor = dealer_a.monitor_open ();
    zlink::socket_monitor_t dealer_b_monitor = dealer_b.monitor_open ();
    const zlink::routing_id_t rid_a = zlink::routing_id_t::from ("sync-a");
    const zlink::routing_id_t rid_b = zlink::routing_id_t::from ("sync-b");
    dealer_a.set_routing_id (rid_a);
    dealer_b.set_routing_id (rid_b);

    const uint64_t hwm = UINT64_C (65536) + sizeof (zlink_msg_t);
    router.options ().send_hwm (zlink::byte_count_t::bytes (hwm));
    dealer_a.options ().recv_hwm (zlink::byte_count_t::bytes (hwm));
    dealer_b.options ().recv_hwm (zlink::byte_count_t::bytes (hwm));

    const std::string endpoint =
      zlink_cpp_contract::unique_inproc ("routed-send-sync-backpressure");
    router.bind (endpoint);
    dealer_a.connect (endpoint);
    dealer_b.connect (endpoint);
    for (int i = 0; i < 2; ++i) {
        assert (zlink_cpp_contract::wait_for_socket_monitor_event (
          router_monitor,
          static_cast<uint64_t> (zlink::monitor_event::connection_ready), 2000));
    }
    assert (zlink_cpp_contract::wait_for_socket_monitor_event (
      dealer_a_monitor,
      static_cast<uint64_t> (zlink::monitor_event::connection_ready), 2000));
    assert (zlink_cpp_contract::wait_for_socket_monitor_event (
      dealer_b_monitor,
      static_cast<uint64_t> (zlink::monitor_event::connection_ready), 2000));

    const std::string filler_text (65536, 'a');
    bool a_backpressured = false;
    for (int i = 0; i < 16; ++i) {
        if (!submit_router_dontwait (router, rid_a, filler_text)) {
            a_backpressured = true;
            break;
        }
    }
    assert (a_backpressured);

    // SNDTIMEO is the only wait bound, and Core owns it. The binding adds no
    // deadline timer and no park queue.
    router.options ().send_timeout (std::chrono::milliseconds (100));
    zlink::message_t blocked = make_request_message ("a-backpressured");
    const auto started = std::chrono::steady_clock::now ();
    bool reported_backpressure = false;
    try {
        router.send (rid_a).message (blocked).submit ();
    }
    catch (const zlink::submit_error_t &error) {
        reported_backpressure = true;
        assert (error.result () == zlink::submit_result_t::backpressured);
        assert (error.internal_errno () == EAGAIN);
    }
    const auto waited = std::chrono::steady_clock::now () - started;
    assert (reported_backpressure);
    assert (waited >= std::chrono::milliseconds (50));
    assert (waited < std::chrono::seconds (2));
    // The caller keeps the message handle Core refused. Core empties the part
    // it inspected, so the handle is valid but no longer carries the payload.
    assert (blocked.valid ());

    // SNDTIMEO(0) is the DONTWAIT contract: Core answers immediately.
    router.options ().send_timeout (std::chrono::milliseconds (0));
    zlink::message_t immediate = make_request_message ("a-dontwait");
    const auto dontwait_started = std::chrono::steady_clock::now ();
    bool immediate_backpressure = false;
    try {
        router.send (rid_a).message (immediate).submit ();
    }
    catch (const zlink::submit_error_t &error) {
        immediate_backpressure = true;
        assert (error.result () == zlink::submit_result_t::backpressured);
        assert (error.internal_errno () == EAGAIN);
    }
    assert (immediate_backpressure);
    assert (std::chrono::steady_clock::now () - dontwait_started
            < std::chrono::milliseconds (100));
    assert (immediate.valid ());

    // B is untouched by A's backpressure.
    zlink::message_t progress_b = make_request_message ("b-progress");
    router.send (rid_b).message (std::move (progress_b)).submit ();
    zlink::received_t received_b;
    assert (dealer_b.recv (received_b) == 0);
    assert (received_b.first_part ().to_string () == "b-progress");
    received_b.close ();

    // Draining A restores its credit and the same submit now succeeds.
    const auto drain_a = [&] {
        int idle = 0;
        for (int i = 0; i < 4096 && idle < 20; ++i) {
            zlink::received_t received;
            if (dealer_a.recv (received, zlink::recv_flags_t::dontwait) != 0) {
                ++idle;
                std::this_thread::sleep_for (std::chrono::milliseconds (5));
                continue;
            }
            idle = 0;
            received.close ();
        }
    };
    drain_a ();
    router.options ().send_timeout (std::chrono::milliseconds (2000));
    zlink::message_t retried = make_request_message ("a-backpressured");
    router.send (rid_a).message (retried).submit ();
    assert (!retried.valid ());
    bool delivered = false;
    const auto deadline = std::chrono::steady_clock::now ()
                          + std::chrono::seconds (5);
    while (!delivered && std::chrono::steady_clock::now () < deadline) {
        zlink::received_t received;
        if (dealer_a.recv (received, zlink::recv_flags_t::dontwait) != 0) {
            std::this_thread::sleep_for (std::chrono::milliseconds (5));
            continue;
        }
        delivered = received.first_part ().to_string () == "a-backpressured";
        received.close ();
    }
    assert (delivered);
}

// Two caller threads own their own routed submits; the binding owns no queue
// that could couple them.
void test_routed_send_submits_from_concurrent_callers ()
{
    zlink::context_t ctx;
    zlink::router_socket_t router (ctx);
    zlink::dealer_socket_t dealer_a (ctx);
    zlink::dealer_socket_t dealer_b (ctx);
    zlink::socket_monitor_t router_monitor = router.monitor_open ();
    zlink::socket_monitor_t dealer_a_monitor = dealer_a.monitor_open ();
    zlink::socket_monitor_t dealer_b_monitor = dealer_b.monitor_open ();
    const zlink::routing_id_t rid_a = zlink::routing_id_t::from ("concurrent-a");
    const zlink::routing_id_t rid_b = zlink::routing_id_t::from ("concurrent-b");
    dealer_a.set_routing_id (rid_a);
    dealer_b.set_routing_id (rid_b);
    router.options ().send_timeout (std::chrono::seconds (5));

    const std::string endpoint =
      zlink_cpp_contract::unique_inproc ("routed-send-concurrent");
    router.bind (endpoint);
    dealer_a.connect (endpoint);
    dealer_b.connect (endpoint);
    for (int i = 0; i < 2; ++i) {
        assert (zlink_cpp_contract::wait_for_socket_monitor_event (
          router_monitor,
          static_cast<uint64_t> (zlink::monitor_event::connection_ready), 2000));
    }
    assert (zlink_cpp_contract::wait_for_socket_monitor_event (
      dealer_a_monitor,
      static_cast<uint64_t> (zlink::monitor_event::connection_ready), 2000));
    assert (zlink_cpp_contract::wait_for_socket_monitor_event (
      dealer_b_monitor,
      static_cast<uint64_t> (zlink::monitor_event::connection_ready), 2000));

    constexpr int k_per_thread = 64;
    std::atomic<int> failures{0};
    const auto sender = [&] (const zlink::routing_id_t &rid, const char *tag) {
        for (int i = 0; i < k_per_thread; ++i) {
            try {
                zlink::message_t part = make_request_message (tag);
                router.send (rid).message (std::move (part)).submit ();
            }
            catch (const zlink::submit_error_t &) {
                failures.fetch_add (1, std::memory_order_relaxed);
            }
        }
    };
    std::thread thread_a ([&] { sender (rid_a, "to-a"); });
    std::thread thread_b ([&] { sender (rid_b, "to-b"); });
    thread_a.join ();
    thread_b.join ();
    assert (failures.load (std::memory_order_relaxed) == 0);

    const auto drain = [] (zlink::dealer_socket_t &dealer_, const char *tag) {
        int seen = 0;
        for (int i = 0; i < k_per_thread * 4 && seen < k_per_thread; ++i) {
            zlink::received_t received;
            if (dealer_.recv (received, zlink::recv_flags_t::dontwait) != 0) {
                std::this_thread::sleep_for (std::chrono::milliseconds (5));
                continue;
            }
            if (received.first_part ().to_string () == tag)
                ++seen;
            received.close ();
        }
        return seen;
    };
    assert (drain (dealer_a, "to-a") == k_per_thread);
    assert (drain (dealer_b, "to-b") == k_per_thread);
}

// Both routed terminals delegate the send-side wait bound to Core: SNDTIMEO
// is the only deadline the binding consults, and it owns no timer of its own.
void test_routed_send_and_request_honor_core_sndtimeo ()
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
      zlink_cpp_contract::unique_inproc ("request-sndtimeo");
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
    const auto send_started = std::chrono::steady_clock::now ();
    bool send_reported = false;
    try {
        dealer.send ().message (send).submit ();
        assert (false && "send must honor SNDTIMEO");
    }
    catch (const zlink::submit_error_t &error) {
        send_reported = true;
        assert (error.result () == zlink::submit_result_t::backpressured);
        assert (error.internal_errno () == EAGAIN);
    }
    assert (send_reported);
    assert (std::chrono::steady_clock::now () - send_started
            < std::chrono::seconds (2));
    assert (send.valid ());

    // The request terminal submits through the same Core send contract before
    // it hands back a suspension, so a send that never gets credit fails at
    // submit time instead of parking in a binding-owned queue.
    zlink::message_t request = make_request_message ("deadline-request");
    const auto started = std::chrono::steady_clock::now ();
    bool request_reported = false;
    try {
        (void) dealer.request ()
          .message (request)
          .timeout (std::chrono::milliseconds (100))
          .async ();
        assert (false && "request submit must honor SNDTIMEO");
    }
    catch (const zlink::submit_error_t &error) {
        request_reported = true;
        assert (error.result () == zlink::submit_result_t::backpressured);
        assert (error.internal_errno () == EAGAIN);
    }
    assert (request_reported);
    assert (std::chrono::steady_clock::now () - started
            < std::chrono::seconds (2));
    assert (request.valid ());
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
    dealer_socket.send ().message (data).submit ();

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
        assert (false && "reply without a route must complete as not connected");
    }
    catch (const zlink::submit_error_t &error) {
        assert (error.result () == zlink::submit_result_t::not_connected);
        assert (error.internal_errno () == ENOTCONN);
    }
    assert (!one_shot.valid ());

    dealer.set_routing_id (dealer_rid);
    dealer.connect (endpoint);
    std::this_thread::sleep_for (std::chrono::milliseconds (50));

    zlink::received_t ghost;
    assert (dealer.recv (ghost, zlink::recv_flags_t::dontwait) != 0);
}

// A multipart request that fails at submit must hand every caller-owned part
// back, exactly as the single-part path does. The submit adapter only borrows
// the parts, so on failure the caller's message_t objects must still hold their
// original payloads once the builder has recycled its state.
void assert_part_intact (const zlink::message_t &part_, const std::string &text_)
{
    assert (part_.valid ());
    assert (part_.size () == text_.size ());
    assert (part_.to_string () == text_);
}

// One DEALER wired to a ROUTER that never receives, so the request terminal can
// be driven into a real Core submit failure.
struct multipart_request_fixture_t
{
    zlink::context_t ctx;
    zlink::dealer_socket_t dealer{ctx};
    zlink::router_socket_t router{ctx};

    explicit multipart_request_fixture_t (const char *name_)
    {
        dealer.set_routing_id (zlink::routing_id_t::from ("multipart-dealer"));
        router.set_routing_id (zlink::routing_id_t::from ("multipart-router"));
        const uint64_t hwm = UINT64_C (65536) + sizeof (zlink_msg_t);
        dealer.options ().send_hwm (zlink::byte_count_t::bytes (hwm));
        router.options ().recv_hwm (zlink::byte_count_t::bytes (hwm));
        const std::string endpoint = zlink_cpp_contract::unique_inproc (name_);
        router.bind (endpoint);
        dealer.connect (endpoint);
        std::this_thread::sleep_for (std::chrono::milliseconds (100));
        dealer.options ().send_timeout (std::chrono::milliseconds (100));
    }

    // Refills the send pipe until Core reports backpressure. Credit can come
    // back between the fill and the next submit, so callers retry rather than
    // assume the very next submit fails.
    void fill_until_backpressured ()
    {
        const std::string filler_text (65536, 'd');
        for (int i = 0; i < 64; ++i) {
            if (!submit_dealer_dontwait (dealer, filler_text))
                return;
        }
        assert (false && "dealer never reported send backpressure");
    }
};

// Later-error path: Core rejects the part sequence after the state handed its
// parts to the submit adapter. Every lvalue part must come back valid.
void test_multipart_request_failure_returns_every_part ()
{
    multipart_request_fixture_t fixture ("multipart-request-failure");

    const std::string first_text = "multipart:first";
    const std::string second_text = "multipart:second";
    const std::string third_text = "multipart:third";

    bool reported = false;
    for (int attempt = 0; attempt < 64 && !reported; ++attempt) {
        fixture.fill_until_backpressured ();
        zlink::message_t first = make_request_message (first_text);
        zlink::message_t second = make_request_message (second_text);
        zlink::message_t third = make_request_message (third_text);
        try {
            (void) fixture.dealer.request ()
              .message (first)
              .message (second)
              .message (third)
              .timeout (std::chrono::milliseconds (100))
              .async ();
            continue;
        }
        catch (const zlink::submit_error_t &error) {
            reported = true;
            assert (error.result () == zlink::submit_result_t::backpressured);
            assert (error.internal_errno () == EAGAIN);
        }
        // The builder state is already recycled here; each caller-owned part
        // must still be the message it was before the failed submit.
        assert_part_intact (first, first_text);
        assert_part_intact (second, second_text);
        assert_part_intact (third, third_text);
    }
    assert (reported);
}

// Same later-error path reached deterministically: an invalid part makes the
// native-view build fail after the parts were taken out of the state. The two
// caller-owned parts around it must still be returned.
void test_multipart_request_invalid_part_returns_the_others ()
{
    multipart_request_fixture_t fixture ("multipart-request-invalid");

    const std::string first_text = "multipart:invalid-first";
    const std::string third_text = "multipart:invalid-third";
    zlink::message_t first = make_request_message (first_text);
    zlink::message_t invalid = make_request_message ("multipart:invalid-second");
    zlink::message_t consumed = std::move (invalid);
    assert (!invalid.valid ());
    zlink::message_t third = make_request_message (third_text);

    bool reported = false;
    try {
        (void) fixture.dealer.request ()
          .message (first)
          .message (invalid)
          .message (third)
          .timeout (std::chrono::milliseconds (100))
          .async ();
        assert (false && "a request carrying an invalid part must fail");
    }
    catch (const zlink::submit_error_t &) {
        reported = true;
    }
    assert (reported);

    assert_part_intact (first, first_text);
    assert_part_intact (third, third_text);
    assert (!invalid.valid ());
}

// Mixed ownership: lvalue parts come back, and an rvalue part added mid-chain
// neither replaces the parts before it nor is handed back to any caller.
void test_multipart_request_failure_keeps_rvalue_parts_consumed ()
{
    multipart_request_fixture_t fixture ("multipart-request-rvalue");

    const std::string first_text = "multipart:lvalue-first";
    const std::string third_text = "multipart:lvalue-third";

    bool reported = false;
    for (int attempt = 0; attempt < 64 && !reported; ++attempt) {
        fixture.fill_until_backpressured ();
        zlink::message_t first = make_request_message (first_text);
        zlink::message_t third = make_request_message (third_text);
        try {
            (void) fixture.dealer.request ()
              .message (first)
              .message (make_request_message ("multipart:rvalue-second"))
              .message (third)
              .timeout (std::chrono::milliseconds (100))
              .async ();
            continue;
        }
        catch (const zlink::submit_error_t &error) {
            reported = true;
            assert (error.result () == zlink::submit_result_t::backpressured);
        }
        assert_part_intact (first, first_text);
        assert_part_intact (third, third_text);
    }
    assert (reported);
}

// The throwing failure path (the `catch (...)` guard around the submit adapter)
// is only reachable on an allocation failure inside the native-view builder, so
// it is covered here at the state level: it restores through the same helper,
// with the same taken-parts vector, that the return-code path uses.
void test_multipart_request_restore_helper_matches_single_part_semantics ()
{
    const std::string first_text = "restore:first";
    const std::string second_text = "restore:second";
    zlink::message_t first = make_request_message (first_text);
    zlink::message_t second = make_request_message (second_text);
    zlink::message_t consumed = make_request_message ("restore:rvalue");

    zlink::detail::operation_state_t state;
    state.kind = zlink::detail::operation_kind_t::raw_request;
    state.message.single_part_source = &first;
    zlink::detail::append_send_part (state, second);
    zlink::detail::append_send_part (state, std::move (consumed));

    std::vector<zlink::message_t> parts = zlink::detail::take_send_parts (state);
    assert (parts.size () == 3u);
    assert (!first.valid ());
    assert (!second.valid ());

    zlink::detail::restore_send_parts_to_sources (state, parts);
    assert_part_intact (first, first_text);
    assert_part_intact (second, second_text);
    // The rvalue part carries no caller source and stays consumed on submit.
    assert (parts[2].valid ());

    // Recycling the state after the restore must not disturb the caller parts.
    zlink::detail::reset_for_reuse (state);
    parts.clear ();
    assert_part_intact (first, first_text);
    assert_part_intact (second, second_text);
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
    test_send_async_inline_completion ();
    test_routed_send_async_inline_completion ();
    test_send_async_pending_completes_after_drain ();
    test_send_async_timeout_surfaces_timed_out ();
    test_send_async_cancel_and_drop ();
    test_send_async_close_fails_pending_operation ();
    test_request_blocking_submit_returns_reply ();
    test_request_blocking_submit_times_out ();
    test_request_callback_fires_exactly_once ();
    test_request_dealer_router_roundtrip ();
    test_request_direct_await_suspends_until_reply ();
    test_dealer_request_without_initial_routed_target_is_terminal ();
    test_dealer_send_without_initial_routed_target_is_terminal ();
    test_routed_send_submit_is_synchronous_and_consumes_parts ();
    test_routed_builder_does_not_outlive_socket_anchor ();
    test_routed_send_reports_core_backpressure_without_poisoning_b ();
    test_routed_send_submits_from_concurrent_callers ();
    test_routed_send_and_request_honor_core_sndtimeo ();
    test_request_router_preserves_data_recv_surface ();
    test_received_reply_rejects_non_none_flags ();
    test_reply_submit_is_one_shot_without_ghost_retry ();
    test_raw_router_reply_maps_submit_result ();
    test_multipart_request_failure_returns_every_part ();
    test_multipart_request_invalid_part_returns_the_others ();
    test_multipart_request_failure_keeps_rvalue_parts_consumed ();
    test_multipart_request_restore_helper_matches_single_part_semantics ();
    std::quick_exit (0);
}
