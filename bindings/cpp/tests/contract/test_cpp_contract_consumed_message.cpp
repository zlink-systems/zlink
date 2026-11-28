/* SPDX-License-Identifier: MPL-2.0 */

#include "support.hpp"

#include <zlink.h>

#include <cstdio>
#include <utility>

namespace
{
using namespace std::chrono_literals;
using zlink_cpp_contract::make_message;

size_t submissions = 0;
size_t failures = 0;

void check_empty (zlink::message_t &message_)
{
    assert (!message_.valid ());
    assert (message_.data () == nullptr);
    assert (std::as_const (message_).data () == nullptr);
    assert (message_.size () == 0);
    assert (message_.bytes ().empty ());
}

template <typename Invoke>
void expect_invalid_handle (const char *label_, Invoke invoke_)
{
    const size_t before = submissions;
    try {
        invoke_ ();
        std::fprintf (stderr, "%s accepted a consumed message\n", label_);
        ++failures;
    }
    catch (const zlink::submit_error_t &error) {
        if (error.result () != zlink::submit_result_t::invalid_handle) {
            std::fprintf (stderr, "%s returned %d (errno %d), expected INVALID_HANDLE\n",
                          label_, error.code (), error.internal_errno ());
            ++failures;
        }
    }
    assert (submissions == before);
}

template <typename Factory, typename Terminal>
void check_terminal (const char *label_, Factory factory_, Terminal terminal_,
                     zlink::message_t &consumed_)
{
    expect_invalid_handle (label_, [&] { terminal_ (factory_ ().message (consumed_)); });

    auto first = make_message ("first");
    auto last = make_message ("last");
    const auto *first_data = first.data ();
    const auto *last_data = last.data ();
    expect_invalid_handle (label_, [&] {
        terminal_ (factory_ ().message (first).message (consumed_).message (last));
    });
    assert (first.valid () && first.data () == first_data && first.to_string () == "first");
    assert (last.valid () && last.data () == last_data && last.to_string () == "last");
    check_empty (consumed_);
}

template <typename Factory>
void check_sync_and_async (const char *label_, Factory factory_, zlink::message_t &consumed_)
{
    check_terminal (label_, factory_, [] (auto &&operation_) { (void) std::move (operation_).submit (); },
                    consumed_);
    check_terminal (label_, factory_, [] (auto &&operation_) { (void) std::move (operation_).async (); },
                    consumed_);
}
} // namespace

extern "C" {
decltype (zlink_send) __real_zlink_send;
decltype (zlink_send_rid) __real_zlink_send_rid;
decltype (zlink_request) __real_zlink_request;
decltype (zlink_reply) __real_zlink_reply;
decltype (zlink_publish) __real_zlink_publish;

zlink_submit_result_t __wrap_zlink_send (
  void *socket_, zlink_msg_t *parts_, size_t count_, zlink_send_flags_t flags_,
  void *context_, zlink_completion_id_t *id_)
{
    ++submissions;
    return __real_zlink_send (socket_, parts_, count_, flags_, context_, id_);
}

zlink_submit_result_t __wrap_zlink_send_rid (
  void *socket_, const zlink_routing_id_t *rid_, zlink_msg_t *parts_, size_t count_,
  zlink_send_flags_t flags_, void *context_, zlink_completion_id_t *id_)
{
    ++submissions;
    return __real_zlink_send_rid (socket_, rid_, parts_, count_, flags_, context_, id_);
}

zlink_submit_result_t __wrap_zlink_request (
  void *socket_, const zlink_routing_id_t *rid_, zlink_msg_t *parts_, size_t count_,
  zlink_send_flags_t flags_, uint32_t timeout_, void *context_, zlink_completion_id_t *id_)
{
    ++submissions;
    return __real_zlink_request (socket_, rid_, parts_, count_, flags_, timeout_, context_, id_);
}

zlink_submit_result_t __wrap_zlink_reply (
  void *socket_, const zlink_routing_id_t *rid_, zlink_reply_token_t token_,
  zlink_msg_t *parts_, size_t count_)
{
    ++submissions;
    return __real_zlink_reply (socket_, rid_, token_, parts_, count_);
}

zlink_submit_result_t __wrap_zlink_publish (
  void *socket_, const char *topic_, zlink_msg_t *parts_, size_t count_, zlink_send_flags_t flags_)
{
    ++submissions;
    return __real_zlink_publish (socket_, topic_, parts_, count_, flags_);
}
}

int main ()
{
    zlink::context_t context;
    zlink::dealer_socket_t dealer (context);
    zlink::router_socket_t router (context);
    zlink::pub_socket_t pub (context);
    dealer.options ().send_timeout (2s);
    router.options ().recv_timeout (2s);
    const auto endpoint = zlink_cpp_contract::unique_inproc ("consumed-message");
    router.bind (endpoint);
    dealer.connect (endpoint);

    auto consumed = make_message ("data");
    dealer.send ().message (consumed).submit ();
    zlink::received_t received;
    assert (router.recv (received) == 0);
    assert (received.first_part ().to_string () == "data");
    assert (submissions == 1);
    check_empty (consumed);
    const auto rid = received.routing_id ().value ();
    zlink::poller_t poller;
    poller.add (dealer, zlink::poll_event_flag_t::pollcompletion, 1);
    poller.add (router, zlink::poll_event_flag_t::pollcompletion, 2);

    check_sync_and_async ("send", [&] { return dealer.send (); }, consumed);
    check_sync_and_async ("routed send", [&] { return router.send (rid); }, consumed);
    check_sync_and_async ("request", [&] { return dealer.request (); }, consumed);
    check_sync_and_async ("routed request", [&] { return router.request (rid); }, consumed);

    auto request = make_message ("request");
    auto pending = dealer.request ().message (request).timeout (2s).async ();
    assert (pending.result == ZLINK_SUBMIT_OK);
    assert (router.recv (received) == 0);
    assert (received.reply_token ().has_value ());
    const auto submit = [] (auto &&operation_) { (void) std::move (operation_).submit (); };
    check_terminal ("reply", [&] { return received.reply (); }, submit, consumed);
    auto reply = make_message ("reply");
    received.reply ().message (reply).submit ();

    const auto publish = [&] { return pub.publish ("topic"); };
    check_terminal ("publish", publish, submit, consumed);
    check_terminal ("publish DONTWAIT", publish, [] (auto &&operation_) {
        (void) std::move (operation_).flags (ZLINK_DONTWAIT).submit ();
    }, consumed);

    auto closed = make_message ("closed");
    auto builder = dealer.send ().message (closed);
    closed.close ();
    check_empty (closed);
    expect_invalid_handle ("close after adding message", [&] { std::move (builder).submit (); });

    zlink::message_t empty;
    dealer.send ().message (empty).submit ();
    assert (router.recv (received) == 0);
    assert (received.first_part ().size () == 0);
    assert (failures == 0);
}
