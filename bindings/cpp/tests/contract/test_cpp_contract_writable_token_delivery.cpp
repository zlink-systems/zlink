/* SPDX-License-Identifier: MPL-2.0 */
#include "completion_native_fixture.hpp"
#include <cerrno>
#include <future>
#include <thread>

void admitted_writable_retries ()
{
    completion_test::fixture_t fixture;
    completion_test::active = &fixture;
    const auto target = zlink::routing_id_t::from ("submit-target");
    auto send = fixture.socket.send (target)
                  .message (zlink::message_t::from ("send")).async ();
    auto request = fixture.socket.request (target)
                     .message (zlink::message_t::from ("request"))
                     .timeout (std::chrono::seconds (5)).async ();
    auto send_wait = std::move (send.admitted).operator co_await ();
    auto request_wait = std::move (request.reply).operator co_await ();
    assert (!send_wait.await_ready () && !request_wait.await_ready ());

    // Core owns the RID echo contract. The binding dispatches the exact token
    // and context even when this boundary fixture changes the echoed RID.
    fixture.writable (0, "submit-target");
    fixture.completions.back ().completion_id += 100;
    assert (fixture.owner->drain () == 1);
    assert (!send_wait.await_ready () && !request_wait.await_ready ());
    assert (fixture.attempts.size () == 2);

    fixture.writable (0, "different-peer");
    fixture.writable (1, "");
    assert (fixture.owner->drain () >= 2);
    assert (send_wait.await_ready ());
    send_wait.await_resume ();
    fixture.owner->drain ();
    assert (request_wait.await_ready ());
    assert (request_wait.await_resume ().empty ());
    assert (fixture.attempts.size () == 4);
    assert (fixture.attempts[2].target == "submit-target");
    assert (fixture.attempts[3].target == "submit-target");
    assert (fixture.attempts[2].payload == "send");
    assert (fixture.attempts[3].payload == "request");
}

void terminal_writable_uses_core_result (zlink_send_complete_result_t result_,
                                         int terminal_errno_,
                                         zlink::submit_result_t expected_)
{
    completion_test::fixture_t fixture;
    completion_test::active = &fixture;
    const auto target = zlink::routing_id_t::from ("submit-target");
    auto send = fixture.socket.send (target)
                  .message (zlink::message_t::from ("send")).async ();
    auto request = fixture.socket.request (target)
                     .message (zlink::message_t::from ("request"))
                     .timeout (std::chrono::seconds (5)).async ();
    auto send_wait = std::move (send.admitted).operator co_await ();
    auto request_wait = std::move (request.reply).operator co_await ();
    assert (!send_wait.await_ready () && !request_wait.await_ready ());

    fixture.writable (0, "submit-target", result_, terminal_errno_);
    fixture.writable (1, "submit-target", result_, terminal_errno_);
    assert (fixture.owner->drain () == 2);
    assert (send_wait.await_ready () && request_wait.await_ready ());
    try {
        send_wait.await_resume ();
        assert (false);
    }
    catch (const zlink::submit_error_t &error) {
        assert (error.result () == expected_);
        assert (error.internal_errno () == terminal_errno_);
    }
    try {
        request_wait.await_resume ();
        assert (false);
    }
    catch (const zlink::submit_error_t &error) {
        assert (error.result () == expected_);
        assert (error.internal_errno () == terminal_errno_);
    }
    assert (fixture.attempts.size () == 2);
}

void unknown_writable_uses_protocol_error ()
{
    completion_test::fixture_t fixture;
    completion_test::active = &fixture;
    const auto target = zlink::routing_id_t::from ("submit-target");
    auto send = fixture.socket.send (target)
                  .message (zlink::message_t::from ("send")).async ();
    auto request = fixture.socket.request (target)
                     .message (zlink::message_t::from ("request"))
                     .timeout (std::chrono::seconds (5)).async ();
    auto send_wait = std::move (send.admitted).operator co_await ();
    auto request_wait = std::move (request.reply).operator co_await ();
    assert (!send_wait.await_ready () && !request_wait.await_ready ());
    fixture.writable (0, "submit-target",
                      static_cast<zlink_send_complete_result_t> (999), ENOENT);
    fixture.writable (1, "submit-target",
                      static_cast<zlink_send_complete_result_t> (999), ENOENT);
    assert (fixture.owner->drain () == 2);
    const auto assert_protocol = [] (auto &wait) {
        assert (wait.await_ready ());
        try {
            wait.await_resume ();
            assert (false);
        }
        catch (const zlink::submit_error_t &error) {
            assert (error.result () == zlink::submit_result_t::internal_error);
            assert (error.internal_errno () == EPROTO);
        }
    };
    assert_protocol (send_wait);
    assert_protocol (request_wait);
}

void abandoned_send_does_not_resubmit ()
{
    completion_test::fixture_t fixture;
    completion_test::active = &fixture;
    const auto target = zlink::routing_id_t::from ("submit-target");
    {
        auto send = fixture.socket.send (target)
                      .message (zlink::message_t::from ("abandoned")).async ();
        assert (send.result == ZLINK_SUBMIT_BACKPRESSURED);
    }
    fixture.writable (0, "submit-target");
    assert (fixture.owner->drain () == 1);
    assert (fixture.attempts.size () == 1);
}

void abandonment_during_resubmit_completes ()
{
    completion_test::fixture_t fixture;
    completion_test::active = &fixture;
    const auto target = zlink::routing_id_t::from ("submit-target");
    auto send = fixture.socket.send (target)
                  .message (zlink::message_t::from ("racing")).async ();
    std::promise<void> retry_entered;
    std::promise<void> release_retry;
    auto release = release_retry.get_future ();
    fixture.retry_backpressured = true;
    fixture.retry_submit = [&] {
        retry_entered.set_value ();
        release.wait ();
    };
    fixture.writable (0, "submit-target");
    std::thread drain ([&] { assert (fixture.owner->drain () == 1); });
    retry_entered.get_future ().wait ();
    std::promise<void> detach_done;
    std::thread detach ([admitted = std::move (send.admitted),
                         &detach_done] () mutable {
        {
            auto dropped = std::move (admitted);
            (void) dropped;
        }
        detach_done.set_value ();
    });
    detach_done.get_future ().wait ();
    release_retry.set_value ();
    drain.join ();
    detach.join ();
    assert (fixture.attempts.size () == 2);
    fixture.writable (1, "submit-target");
    assert (fixture.owner->drain () == 1);
    assert (fixture.attempts.size () == 2);
}

int main ()
{
    admitted_writable_retries ();
    terminal_writable_uses_core_result (ZLINK_SEND_NOT_FOUND, ENOENT,
                                        zlink::submit_result_t::not_found);
    terminal_writable_uses_core_result (ZLINK_SEND_NOT_CONNECTED, ENOTCONN,
                                        zlink::submit_result_t::not_connected);
    terminal_writable_uses_core_result (ZLINK_SEND_TIMED_OUT, EAGAIN,
                                        zlink::submit_result_t::backpressured);
    unknown_writable_uses_protocol_error ();
    abandoned_send_does_not_resubmit ();
    abandonment_during_resubmit_completes ();
}
