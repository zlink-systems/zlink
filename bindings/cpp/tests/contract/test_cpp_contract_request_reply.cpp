/* SPDX-License-Identifier: MPL-2.0 */

#include "support.hpp"

#include <Runtime/Messaging/request_submitter.hpp>

#include <cerrno>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <optional>
#include <thread>

namespace
{

zlink::message_t make_request_message (const std::string &text_)
{
    return zlink_cpp_contract::make_message (text_);
}

struct recorded_request_part_t
{
    zlink_part_flag_t flag = ZLINK_PART_FINAL;
    zlink_reply_handler_fn callback = nullptr;
    void *userdata = nullptr;
};

std::vector<zlink::message_t> make_multipart_request ()
{
    std::vector<zlink::message_t> parts;
    parts.push_back (make_request_message ("request:part-1"));
    parts.push_back (make_request_message ("request:part-2"));
    parts.push_back (make_request_message ("request:part-3"));
    return parts;
}

// The core takes the reply handler as the marker of the FINAL part: the submission that
// carries it is the one that builds the request spec. The raw socket path requires the
// handler on the final part.
// A submitter that attaches the handler to every part therefore cannot send a multipart
// request at all ??which is what this asserts against.
void assert_multipart_request_submitter_attaches_reply_handler_to_final_part_only (
  const std::vector<recorded_request_part_t> &submissions_)
{
    assert (submissions_.size () == 3);
    assert (submissions_[0].flag == ZLINK_PART_MORE);
    assert (submissions_[1].flag == ZLINK_PART_MORE);
    assert (submissions_[2].flag == ZLINK_PART_FINAL);

    assert (submissions_[0].callback == nullptr);
    assert (submissions_[0].userdata == nullptr);
    assert (submissions_[1].callback == nullptr);
    assert (submissions_[1].userdata == nullptr);
    assert (submissions_[2].callback != nullptr);
    assert (submissions_[2].userdata != nullptr);
}

void test_multipart_request_callback_submitter_attaches_reply_handler_to_final_part_only ()
{
    std::vector<zlink::message_t> parts = make_multipart_request ();
    std::vector<recorded_request_part_t> submissions;
    bool completed = false;

    const bool submitted = zlink::detail::submit_request_parts_callback (
      parts,
      [&completed] (zlink::request_result_t result, std::vector<zlink::message_t> replies) {
          assert (result == zlink::request_result_t::not_found);
          assert (replies.empty ());
          completed = true;
      },
      zlink::send_flags_t::none,
      [&submissions] (zlink_msg_t *part_out, zlink_part_flag_t part_flag,
                      zlink_reply_handler_fn callback, void *userdata) {
          submissions.push_back ({part_flag, callback, userdata});
          (void) zlink_msg_close (part_out);
          return ZLINK_SUBMIT_OK;
      });

    assert (submitted);
    assert_multipart_request_submitter_attaches_reply_handler_to_final_part_only (submissions);

    submissions.back ().callback (ZLINK_REQUEST_NOT_FOUND, nullptr, 0, submissions.back ().userdata);
    assert (completed);
}

void test_multipart_request_awaitable_submitter_attaches_reply_handler_to_final_part_only ()
{
    std::vector<zlink::message_t> parts = make_multipart_request ();
    std::vector<recorded_request_part_t> submissions;

    zlink::async_result_t<std::vector<zlink::message_t>> result =
      zlink::detail::submit_request_parts_awaitable (
        parts, [] {},
        [&submissions] (zlink_msg_t *part_out, zlink_part_flag_t part_flag,
                        zlink_reply_handler_fn callback, void *userdata) {
            submissions.push_back ({part_flag, callback, userdata});
            (void) zlink_msg_close (part_out);
            return ZLINK_SUBMIT_OK;
        });

    assert_multipart_request_submitter_attaches_reply_handler_to_final_part_only (submissions);

    submissions.back ().callback (ZLINK_REQUEST_NOT_FOUND, nullptr, 0, submissions.back ().userdata);
    try {
        (void) result.get ();
        assert (false && "request failure must complete the awaitable with an exception");
    }
    catch (const zlink::request_error_t &error) {
        assert (error.result () == zlink::request_result_t::not_found);
    }
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
    assert (dealer_socket.send ().message (warmup).submit ());
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

    auto request_operation = dealer_socket.request ().message (request);
    assert (request.valid ());
    zlink::async_result_t<std::vector<zlink::message_t>> future =
      std::move (request_operation).timeout (std::chrono::milliseconds (5000)).async ();
    assert (!request.valid ());
    const std::vector<zlink::message_t> reply = future.get ();
    assert (reply.size () == 1);
    assert (reply[0].to_string () == "reply:ok");
    router_done.get ();
}

void test_request_wait_for_zero_pumps_progress ()
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

    zlink::async_result_t<std::vector<zlink::message_t>> future =
      dealer_socket.request ()
        .message (request)
        .timeout (std::chrono::milliseconds (5000))
        .async ();

    bool ready = false;
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (2);
    while (std::chrono::steady_clock::now () < deadline) {
        if (future.wait_for (std::chrono::milliseconds (0)) == std::future_status::ready) {
            ready = true;
            break;
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    assert (ready && "wait_for(0) must pump request progress");

    const std::vector<zlink::message_t> reply = future.get ();
    assert (reply.size () == 1);
    assert (reply[0].to_string () == "reply:wait-zero");
    router_done.get ();
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
    assert (dealer_socket.send ().message (data).submit ());

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

    zlink::async_result_t<std::vector<zlink::message_t>> future =
      dealer_socket.request ()
        .message (request)
        .timeout (std::chrono::milliseconds (5000))
        .async ();
    const std::vector<zlink::message_t> reply = future.get ();
    assert (reply.size () == 1);
    assert (reply[0].to_string () == "reply:ok");
    router_done.get ();
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

void test_router_completion_control_uses_existing_completion_connection ()
{
    zlink::context_t ctx;
    zlink::router_socket_t server (ctx);
    zlink::router_socket_t client (ctx);
    const zlink::routing_id_t server_rid = zlink::routing_id_t::from ("control-server");
    const zlink::routing_id_t client_rid = zlink::routing_id_t::from ("control-client");
    server.set_routing_id (server_rid);
    client.set_routing_id (client_rid);
    client.options ().connect_routing_id (server_rid);

    std::optional<zlink::routing_id_t> delivered_source;
    std::vector<zlink::message_t> delivered_parts;
    server.set_completion_control_handler (
      [&] (const zlink::routing_id_t &source_, std::vector<zlink::message_t> parts_) {
          delivered_source = source_;
          delivered_parts = std::move (parts_);
      });

    zlink::poller_t poller;
    poller.add (server, zlink::poll_event_flag_t::pollcompletion, 1);
    const std::string endpoint = zlink_cpp_contract::unique_inproc ("completion-control");
    server.bind (endpoint);
    client.connect (endpoint);
    std::this_thread::sleep_for (std::chrono::milliseconds (50));

    zlink::message_t application = make_request_message ("application-unread");
    assert (client.send (server_rid).message (application).submit ());

    const std::vector<zlink::message_t> control = {
      make_request_message ("opaque-command"),
      make_request_message ("generation-1")};
    assert (client.try_send_completion_control (server_rid, control));
    assert (control[0].valid ());
    assert (control[1].valid ());

    zlink::poll_event_t event;
    assert (poller.wait (&event, 1, std::chrono::seconds (2)) == 1);
    assert (delivered_source.has_value ());
    assert (*delivered_source == client_rid);
    assert (delivered_parts.size () == 2);
    assert (delivered_parts[0].to_string () == "opaque-command");
    assert (delivered_parts[1].to_string () == "generation-1");

    zlink::received_t application_received;
    assert (server.recv (application_received) == 0);
    assert (application_received.parts ().size () == 1);
    assert (application_received.parts ()[0].to_string () == "application-unread");

    std::mutex callback_mutex;
    std::condition_variable callback_cv;
    bool callback_entered = false;
    bool callback_release = false;
    server.set_completion_control_handler (
      [&] (const zlink::routing_id_t &, std::vector<zlink::message_t>) {
          std::unique_lock<std::mutex> lock (callback_mutex);
          callback_entered = true;
          callback_cv.notify_all ();
          callback_cv.wait (lock, [&] { return callback_release; });
      });

    std::atomic<size_t> wait_result{0};
    std::thread poll_thread ([&] {
        zlink::poll_event_t close_event;
        wait_result.store (
          poller.wait (&close_event, 1, std::chrono::seconds (2)),
          std::memory_order_release);
    });
    const std::vector<zlink::message_t> close_control = {
      make_request_message ("close-race")};
    assert (client.try_send_completion_control (server_rid, close_control));

    {
        std::unique_lock<std::mutex> lock (callback_mutex);
        assert (callback_cv.wait_for (
          lock, std::chrono::seconds (2), [&] { return callback_entered; }));
    }
    try {
        server.close ();
        assert (false && "close during completion callback must be busy");
    }
    catch (const zlink::close_error_t &error) {
        assert (error.result () == zlink::close_result_t::busy);
        assert (error.internal_errno () == EBUSY);
    }

    std::atomic<bool> replacement_called{false};
    server.set_completion_control_handler (
      [&] (const zlink::routing_id_t &, std::vector<zlink::message_t>) {
          replacement_called.store (true, std::memory_order_release);
      });

    {
        std::lock_guard<std::mutex> lock (callback_mutex);
        callback_release = true;
    }
    callback_cv.notify_all ();
    poll_thread.join ();
    assert (wait_result.load (std::memory_order_acquire) == 1);

    const std::vector<zlink::message_t> replacement_control = {
      make_request_message ("replacement")};
    assert (client.try_send_completion_control (server_rid,
                                                replacement_control));
    zlink::poll_event_t replacement_event;
    assert (poller.wait (&replacement_event, 1, std::chrono::seconds (2)) == 1);
    assert (replacement_called.load (std::memory_order_acquire));

    assert (poller.remove (server));
    poller.close ();
    server.close ();
    client.close ();
}

} // namespace

int main ()
{
    test_multipart_request_callback_submitter_attaches_reply_handler_to_final_part_only ();
    test_multipart_request_awaitable_submitter_attaches_reply_handler_to_final_part_only ();
    test_request_dealer_router_roundtrip ();
    test_request_wait_for_zero_pumps_progress ();
    test_request_router_preserves_data_recv_surface ();
    test_received_reply_rejects_non_none_flags ();
    test_raw_router_reply_maps_submit_result ();
    test_router_completion_control_uses_existing_completion_connection ();
    std::quick_exit (0);
}
