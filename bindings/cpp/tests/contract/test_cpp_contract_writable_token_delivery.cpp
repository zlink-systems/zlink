/* SPDX-License-Identifier: MPL-2.0 */
#include "completion_native_fixture.hpp"

int main ()
{
    completion_test::fixture_t fixture;
    completion_test::active = &fixture;
    const auto target = zlink::routing_id_t::from ("submit-target");
    auto send = fixture.socket.send (target)
                  .message (zlink::message_t::from ("send")).async ();
    auto request = fixture.socket.request (target)
                     .message (zlink::message_t::from ("request"))
                     .timeout (std::chrono::seconds (5)).async ();
    auto send_wait = std::move (send).operator co_await ();
    auto request_wait = std::move (request).operator co_await ();
    assert (!send_wait.await_ready () && !request_wait.await_ready ());

    // Core owns the RID echo contract. The binding dispatches the exact token
    // and context even when this boundary fixture changes the echoed RID.
    fixture.writable (0, "submit-target");
    fixture.completions.back ().completion_id += 100;
    assert (fixture.owner->drain (true) == 1);
    assert (!send_wait.await_ready () && !request_wait.await_ready ());
    assert (fixture.attempts.size () == 2);

    fixture.writable (0, "different-peer");
    fixture.writable (1, "");
    assert (fixture.owner->drain (true) >= 2);
    assert (send_wait.await_ready ());
    send_wait.await_resume ();
    fixture.owner->drain (true);
    assert (request_wait.await_ready ());
    assert (request_wait.await_resume ().empty ());
    assert (fixture.attempts.size () == 4);
    assert (fixture.attempts[2].target == "submit-target");
    assert (fixture.attempts[3].target == "submit-target");
    assert (fixture.attempts[2].payload == "send");
    assert (fixture.attempts[3].payload == "request");
}
