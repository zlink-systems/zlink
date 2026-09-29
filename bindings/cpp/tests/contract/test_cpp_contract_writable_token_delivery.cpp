/* SPDX-License-Identifier: MPL-2.0 */
#include "completion_native_fixture.hpp"
#include <cerrno>

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

int main ()
{
    admitted_writable_retries ();
    terminal_writable_uses_core_result (ZLINK_SEND_NOT_FOUND, ENOENT,
                                        zlink::submit_result_t::not_found);
    terminal_writable_uses_core_result (ZLINK_SEND_NOT_CONNECTED, ENOTCONN,
                                        zlink::submit_result_t::not_connected);
    unknown_writable_uses_protocol_error ();
}
