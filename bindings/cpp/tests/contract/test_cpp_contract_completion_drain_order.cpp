/* SPDX-License-Identifier: MPL-2.0 */
#include "completion_native_fixture.hpp"

void test_queued_send_and_request ()
{
    completion_test::fixture_t fixture;
    completion_test::active = &fixture;
    const auto target = zlink::routing_id_t::from ("peer");
    auto send = fixture.socket.send (target)
                  .message (zlink::message_t::from ("send")).async ();
    auto request = fixture.socket.request (target)
                     .message (zlink::message_t::from ("request"))
                     .timeout (std::chrono::seconds (5)).async ();
    auto send_wait = std::move (send).operator co_await ();
    auto request_wait = std::move (request).operator co_await ();
    fixture.writable (0, "peer");
    fixture.writable (1, "peer");
    fixture.events.clear ();
    assert (fixture.owner->drain (true) == 2);
    const std::vector<std::string> expected = {
      "recv:1", "recv:2", "NO_DATA", "submit:send", "submit:request"};
    assert (fixture.events == expected);
    assert (send_wait.await_ready ());
    send_wait.await_resume ();
    // A completion produced by resubmission belongs to the next drain.
    assert (!request_wait.await_ready ());
    assert (fixture.owner->drain (true) == 1);
    assert (request_wait.await_ready ());
    assert (request_wait.await_resume ().empty ());
}

void test_writable_before_send_registration ()
{
    completion_test::fixture_t fixture;
    completion_test::active = &fixture;
    std::promise<void> received;
    auto received_future = received.get_future ();
    std::promise<void> at_no_data;
    auto at_no_data_future = at_no_data.get_future ();
    std::promise<void> allow_no_data;
    auto allow_no_data_future = allow_no_data.get_future ();
    unsigned receives = 0;
    fixture.before_recv = [&] {
        if (++receives == 2) {
            at_no_data.set_value ();
            assert (allow_no_data_future.wait_for (std::chrono::seconds (5))
                    == std::future_status::ready);
        }
    };
    fixture.after_recv = [&] { received.set_value (); };
    std::thread drain;
    fixture.first_submit = [&] {
        fixture.writable (0, "peer");
        drain = std::thread ([&] { assert (fixture.owner->drain (true) == 1); });
        assert (received_future.wait_for (std::chrono::seconds (5))
                == std::future_status::ready);
    };
    auto send = fixture.socket.send (zlink::routing_id_t::from ("peer"))
                  .message (zlink::message_t::from ("early-send")).async ();
    auto wait = std::move (send).operator co_await ();
    assert (at_no_data_future.wait_for (std::chrono::seconds (5))
            == std::future_status::ready);
    assert (!wait.await_ready ());
    assert (fixture.attempts.size () == 1);
    allow_no_data.set_value ();
    drain.join ();
    assert (wait.await_ready ());
    wait.await_resume ();
    const std::vector<std::string> expected = {
      "submit:early-send", "recv:1", "NO_DATA", "submit:early-send"};
    assert (fixture.events == expected);
}

int main ()
{
    test_queued_send_and_request ();
    test_writable_before_send_registration ();
}
