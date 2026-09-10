/* SPDX-License-Identifier: MPL-2.0 */

#include "support.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <coroutine>
#include <future>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace
{

constexpr uint64_t hwm_bytes = 512;
constexpr size_t payload_bytes = 64;
constexpr size_t max_fill_records = 4096;
constexpr uint64_t completion_slot = 91;

template <typename T> class task_t
{
  public:
    struct promise_type
    {
        std::promise<T> promise;
        task_t get_return_object () { return task_t (promise.get_future ()); }
        std::suspend_never initial_suspend () noexcept { return {}; }
        std::suspend_never final_suspend () noexcept { return {}; }
        void return_value (T value_) { promise.set_value (std::move (value_)); }
        void unhandled_exception ()
        {
            promise.set_exception (std::current_exception ());
        }
    };

    explicit task_t (std::future<T> future_) : _future (std::move (future_)) {}
    task_t (task_t &&) noexcept = default;
    task_t &operator= (task_t &&) noexcept = default;

    bool ready () const
    {
        return _future.wait_for (std::chrono::milliseconds::zero ())
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
        return _future.wait_for (std::chrono::milliseconds::zero ())
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

void_task_t await_admitted (zlink::async_result_t<void> admitted_)
{
    co_await std::move (admitted_);
}

task_t<reply_parts_t> await_reply (zlink::async_result_t<reply_parts_t> reply_)
{
    co_return co_await std::move (reply_);
}

std::string payload (size_t sequence_)
{
    std::string value = std::to_string (sequence_) + ":";
    value.resize (payload_bytes, 'z');
    return value;
}

void configure_small_hwm (zlink::socket_t &socket_)
{
    socket_.options ().linger (std::chrono::milliseconds::zero ());
    socket_.options ().send_hwm (zlink::byte_count_t::bytes (hwm_bytes));
    socket_.options ().recv_hwm (zlink::byte_count_t::bytes (hwm_bytes));
}

void connect_ready (zlink::router_socket_t &server_,
                    zlink::dealer_socket_t &client_, const char *label_)
{
    const std::string endpoint = zlink_cpp_contract::unique_inproc (label_);
    server_.bind (endpoint);
    client_.connect (endpoint);
    client_.options ().send_timeout (std::chrono::seconds (5));
    zlink::message_t probe = zlink_cpp_contract::make_message ("ready");
    client_.send ().message (probe).submit ();
    zlink::received_t received;
    assert (server_.recv (received) == 0);
    assert (received.first_part ().to_string () == "ready");
}

void reply_once (zlink::router_socket_t &server_, const std::string &expected_,
                 const std::string &reply_)
{
    zlink::received_t received;
    assert (server_.recv (received) == 0);
    assert (received.first_part ().to_string () == expected_);
    assert (received.reply_token ().has_value ());
    zlink::message_t reply = zlink_cpp_contract::make_message (reply_);
    received.reply ().message (reply).submit ();
}

void wait_for_stage (zlink::poller_t &poller_, const void_task_t &stage_)
{
    for (size_t attempt = 0; attempt != 32 && !stage_.ready (); ++attempt) {
        zlink::poll_event_t event{};
        assert (poller_.wait (&event, 1, std::chrono::seconds (5)) == 1);
        assert (event.slot == completion_slot);
        assert ((static_cast<short> (event.revents)
                 & static_cast<short> (
                   zlink::poll_event_flag_t::pollcompletion)) != 0);
    }
    assert (stage_.ready ());
}

void wait_for_reply (zlink::poller_t &poller_,
                     const task_t<reply_parts_t> &stage_)
{
    for (size_t attempt = 0; attempt != 32 && !stage_.ready (); ++attempt) {
        zlink::poll_event_t event{};
        assert (poller_.wait (&event, 1, std::chrono::seconds (5)) == 1);
        assert (event.slot == completion_slot);
        assert ((static_cast<short> (event.revents)
                 & static_cast<short> (
                   zlink::poll_event_flag_t::pollcompletion)) != 0);
    }
    assert (stage_.ready ());
}

void immediate_admission_returns_completed_stage_before_reply ()
{
    zlink::context_t context;
    zlink::router_socket_t server (context);
    zlink::dealer_socket_t client (context);
    server.options ().recv_timeout (std::chrono::seconds (5));
    connect_ready (server, client, "submit-result-immediate");

    zlink::message_t send_payload = zlink_cpp_contract::make_message ("send-ok");
    zlink::send_submission_t send =
      client.send ().message (send_payload).async ();
    assert (send.result == ZLINK_SUBMIT_OK);
    void_task_t send_admitted = await_admitted (std::move (send.admitted));
    assert (send_admitted.ready ());
    send_admitted.get ();
    zlink::received_t sent;
    assert (server.recv (sent) == 0);
    assert (sent.first_part ().to_string () == "send-ok");

    zlink::message_t request_payload =
      zlink_cpp_contract::make_message ("request-ok");
    zlink::request_submission_t request =
      client.request ().message (request_payload)
        .timeout (std::chrono::seconds (30)).async ();
    assert (request.result == ZLINK_SUBMIT_OK);
    void_task_t request_admitted =
      await_admitted (std::move (request.admitted));
    task_t<reply_parts_t> reply = await_reply (std::move (request.reply));
    assert (request_admitted.ready ());
    request_admitted.get ();
    assert (!reply.ready ());

    reply_once (server, "request-ok", "reply-ok");
    reply_parts_t reply_parts = reply.get ();
    assert (reply_parts.size () == 1);
    assert (reply_parts.front ().to_string () == "reply-ok");
}

void backpressured_send_admits_after_writable ()
{
    zlink::context_t context;
    context.options ().auto_hwm_enabled (false);
    zlink::router_socket_t server (context);
    zlink::dealer_socket_t client (context);
    configure_small_hwm (server);
    configure_small_hwm (client);
    server.options ().recv_timeout (std::chrono::seconds (5));
    connect_ready (server, client, "submit-result-send-hwm");
    zlink::poller_t poller;
    poller.add (client, zlink::poll_event_flag_t::pollcompletion,
                completion_slot);

    size_t admitted_count = 0;
    std::optional<zlink::send_submission_t> waiting;
    for (size_t sequence = 0; sequence != max_fill_records; ++sequence) {
        zlink::message_t part =
          zlink_cpp_contract::make_message (payload (sequence));
        zlink::send_submission_t submission =
          client.send ().message (part).async ();
        if (submission.result == ZLINK_SUBMIT_BACKPRESSURED) {
            waiting.emplace (std::move (submission));
            break;
        }
        assert (submission.result == ZLINK_SUBMIT_OK);
        void_task_t admitted =
          await_admitted (std::move (submission.admitted));
        assert (admitted.ready ());
        admitted.get ();
        ++admitted_count;
    }
    assert (admitted_count > 0);
    assert (waiting.has_value ());
    assert (waiting->result == ZLINK_SUBMIT_BACKPRESSURED);
    void_task_t admitted = await_admitted (std::move (waiting->admitted));
    assert (!admitted.ready ());

    for (size_t sequence = 0; sequence != admitted_count; ++sequence) {
        zlink::received_t received;
        assert (server.recv (received) == 0);
        assert (received.first_part ().to_string () == payload (sequence));
    }
    wait_for_stage (poller, admitted);
    admitted.get ();
    assert (waiting->result == ZLINK_SUBMIT_BACKPRESSURED);
    zlink::received_t received;
    assert (server.recv (received) == 0);
    assert (received.first_part ().to_string () == payload (admitted_count));
}

void backpressured_request_admits_before_reply ()
{
    zlink::context_t context;
    context.options ().auto_hwm_enabled (false);
    zlink::router_socket_t server (context);
    zlink::dealer_socket_t client (context);
    configure_small_hwm (server);
    configure_small_hwm (client);
    server.options ().recv_timeout (std::chrono::seconds (5));
    connect_ready (server, client, "submit-result-request-hwm");
    zlink::poller_t poller;
    poller.add (client, zlink::poll_event_flag_t::pollcompletion,
                completion_slot);

    std::vector<task_t<reply_parts_t>> replies;
    std::optional<zlink::request_submission_t> waiting;
    for (size_t sequence = 0; sequence != max_fill_records; ++sequence) {
        zlink::message_t part =
          zlink_cpp_contract::make_message (payload (sequence));
        zlink::request_submission_t submission =
          client.request ().message (part)
            .timeout (std::chrono::seconds (30)).async ();
        if (submission.result == ZLINK_SUBMIT_BACKPRESSURED) {
            waiting.emplace (std::move (submission));
            break;
        }
        assert (submission.result == ZLINK_SUBMIT_OK);
        void_task_t admitted =
          await_admitted (std::move (submission.admitted));
        assert (admitted.ready ());
        admitted.get ();
        replies.push_back (await_reply (std::move (submission.reply)));
    }
    assert (!replies.empty ());
    assert (waiting.has_value ());
    assert (waiting->result == ZLINK_SUBMIT_BACKPRESSURED);
    void_task_t admitted = await_admitted (std::move (waiting->admitted));
    task_t<reply_parts_t> waiting_reply =
      await_reply (std::move (waiting->reply));
    assert (!admitted.ready ());
    assert (!waiting_reply.ready ());

    for (size_t sequence = 0; sequence != replies.size (); ++sequence)
        reply_once (server, payload (sequence), "reply-" + std::to_string (sequence));
    wait_for_stage (poller, admitted);
    admitted.get ();
    assert (waiting->result == ZLINK_SUBMIT_BACKPRESSURED);
    assert (!waiting_reply.ready ());

    reply_once (server, payload (replies.size ()), "reply-waiting");
    wait_for_reply (poller, waiting_reply);
    for (size_t sequence = 0; sequence != replies.size (); ++sequence) {
        reply_parts_t parts = replies[sequence].get ();
        assert (parts.size () == 1);
        assert (parts.front ().to_string ()
                == "reply-" + std::to_string (sequence));
    }
    reply_parts_t parts = waiting_reply.get ();
    assert (parts.size () == 1);
    assert (parts.front ().to_string () == "reply-waiting");
}

} // namespace

int main ()
{
    for (int repetition = 0; repetition != 5; ++repetition) {
        immediate_admission_returns_completed_stage_before_reply ();
        backpressured_send_admits_after_writable ();
        backpressured_request_admits_before_reply ();
    }
    return 0;
}
