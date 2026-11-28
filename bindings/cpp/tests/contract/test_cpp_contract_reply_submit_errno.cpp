/* SPDX-License-Identifier: MPL-2.0 */

#include "support.hpp"

#include <zlink.h>

#include <cerrno>

namespace
{
bool overwrite_errno_on_close = false;
unsigned reply_submissions = 0;
}

extern "C" {
decltype (zlink_msg_close) __real_zlink_msg_close;

zlink_config_result_t __wrap_zlink_msg_close (zlink_msg_t *part_)
{
    const auto result = __real_zlink_msg_close (part_);
    if (overwrite_errno_on_close) {
        overwrite_errno_on_close = false;
        errno = EINVAL;
    }
    return result;
}

zlink_submit_result_t __wrap_zlink_reply (
  void *, const zlink_routing_id_t *, zlink_reply_token_t, zlink_msg_t *parts_,
  size_t part_count_)
{
    ++reply_submissions;
    assert (part_count_ == 1);
    assert (__real_zlink_msg_close (&parts_[0]) == ZLINK_CONFIG_OK);
    assert (zlink_msg_init (&parts_[0]) == ZLINK_CONFIG_OK);
    overwrite_errno_on_close = true;
    errno = ENOTCONN;
    return ZLINK_SUBMIT_NOT_CONNECTED;
}
}

int main ()
{
    using namespace std::chrono_literals;
    zlink::context_t context;
    zlink::dealer_socket_t dealer (context);
    zlink::router_socket_t router (context);
    router.options ().recv_timeout (2s);
    const auto endpoint = zlink_cpp_contract::unique_inproc ("reply-submit-errno");
    router.bind (endpoint);
    dealer.connect (endpoint);

    zlink::poller_t poller;
    poller.add (dealer, zlink::poll_event_flag_t::pollcompletion, 1);
    auto request = zlink::message_t::from ("request");
    auto pending = dealer.request ().message (request).timeout (2s).async ();
    assert (pending.result == ZLINK_SUBMIT_OK);
    zlink::received_t received;
    assert (router.recv (received) == 0);
    assert (received.reply_token ().has_value ());

    auto closed = zlink::message_t::from ("closed");
    closed.close ();
    try {
        received.reply ().message (closed).submit ();
        return 1;
    }
    catch (const zlink::submit_error_t &error) {
        assert (reply_submissions == 0);
        assert (error.result () == zlink::submit_result_t::invalid_handle);
        assert (error.internal_errno () == EFAULT);
    }

    auto reply = zlink::message_t::from ("reply");
    try {
        received.reply ().message (reply).submit ();
    }
    catch (const zlink::submit_error_t &error) {
        assert (reply_submissions == 1);
        assert (!overwrite_errno_on_close);
        assert (error.result () == zlink::submit_result_t::not_connected);
        assert (error.internal_errno () == ENOTCONN);
        return 0;
    }
    return 1;
}
