/* SPDX-License-Identifier: MPL-2.0 */
#include "completion_native_fixture.hpp"

namespace
{
void test_late_record (bool request_, bool writable_, bool next_registered_)
{
    completion_test::fixture_t fixture;
    completion_test::active = &fixture;
    const auto target = zlink::routing_id_t::from ("peer");
    fixture.backpressure_errno = EINVAL;
    try {
        if (request_)
            (void) fixture.socket.request (target)
              .message (zlink::message_t::from ("rejected")).async ();
        else
            (void) fixture.socket.send (target)
              .message (zlink::message_t::from ("rejected")).async ();
        assert (false && "invalid wait token must fail");
    }
    catch (const zlink::submit_error_t &failure) {
        assert (failure.result () == zlink::submit_result_t::internal_error);
        assert (failure.internal_errno () == EPROTO);
    }

    fixture.counts.clear ();
    fixture.backpressure_errno = EAGAIN;
    const auto start_next = [&] {
        return fixture.socket.send (target)
          .message (zlink::message_t::from ("next")).async ();
    };
    const auto drain_late_record = [&] {
        fixture.writable (0, "peer");
        if (!writable_)
            fixture.completions.back ().kind = ZLINK_COMPLETION_REQUEST;
        const auto attempts = fixture.attempts.size ();
        auto drained = std::async (std::launch::async, [&] { return fixture.owner->drain (); });
        assert (drained.wait_for (std::chrono::seconds (1)) == std::future_status::ready
                && "late record waited for a new SEND registration");
        assert (drained.get () == 1);
        assert (fixture.attempts.size () == attempts);
    };

    if (!next_registered_)
        drain_late_record ();
    auto next = start_next ();
    assert (next.result == ZLINK_SUBMIT_BACKPRESSURED);
    if (!request_)
        assert (fixture.attempts[0].context == fixture.attempts[1].context);
    auto admitted = std::move (next.admitted).operator co_await ();
    if (next_registered_)
        drain_late_record ();
    assert (!admitted.await_ready ());
    fixture.writable (1, "peer");
    assert (fixture.owner->drain () == 1);
    assert (admitted.await_ready ());
    admitted.await_resume ();
    assert (fixture.attempts.size () == 3);
}
} // namespace

int main ()
{
    for (bool request : {false, true})
        for (bool writable : {true, false})
            for (bool next_registered : {false, true})
                test_late_record (request, writable, next_registered);
}
