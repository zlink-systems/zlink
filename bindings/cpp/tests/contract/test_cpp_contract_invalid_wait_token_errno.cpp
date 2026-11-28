/* SPDX-License-Identifier: MPL-2.0 */
#include "completion_native_fixture.hpp"
#include <Runtime/Messaging/operation_state.hpp>

namespace
{
void assert_unregistered (completion_test::fixture_t &fixture, void *context)
{
    // The sentinel occupies the inline slot. Registration fails if the
    // original context still has an entry in the map.
    auto probe = std::make_shared<zlink::detail::completion_entry_t> (
      nullptr, nullptr, context, 0);
    fixture.owner->register_blocking_entry (probe);
    fixture.owner->unregister_entry (context);
}

template <typename Submit>
void assert_protocol_error (Submit submit)
{
    try {
        submit ();
        assert (false && "invalid wait token must fail");
    }
    catch (const zlink::submit_error_t &error) {
        assert (error.result () == zlink::submit_result_t::internal_error);
        assert (error.internal_errno () == EPROTO);
    }
}
} // namespace

int main ()
{
    for (bool request : {false, true}) {
        // Keep the payload alive through the fixture's cleanup on a failure.
        std::string payload (256, 'x');
        unsigned releases = 0;
        auto message = zlink::advanced::external_message_t::from (
          std::as_writable_bytes (std::span (payload)),
          [] (void *, void *hint) { ++*static_cast<unsigned *> (hint); },
          &releases);
        completion_test::fixture_t fixture;
        completion_test::active = &fixture;
        auto sentinel = std::make_shared<zlink::detail::completion_entry_t> (
          nullptr, nullptr);
        fixture.owner->register_blocking_entry (sentinel);
        const auto target = zlink::routing_id_t::from ("submit-target");

        fixture.backpressure_errno = EINVAL;
        assert_protocol_error ([&] {
            if (request)
                (void) fixture.socket.request (target).message (message).async ();
            else
                (void) fixture.socket.send (target).message (message).async ();
        });
        assert (message.to_string () == payload);
        assert (releases == 0);
        assert (fixture.attempts.size () == 1);
        assert_unregistered (fixture, fixture.attempts.back ().context);
        fixture.counts.clear ();

        fixture.backpressure_errno = EAGAIN;
        const auto check_retry = [&] (auto &submission) {
            assert (submission.result == ZLINK_SUBMIT_BACKPRESSURED);
            auto wait = std::move (submission.admitted).operator co_await ();
            assert (!wait.await_ready ());
            const auto attempt = fixture.attempts.size () - 1;
            const auto context = fixture.attempts[attempt].context;
            fixture.retry_backpressured = true;
            fixture.backpressure_errno = EINVAL;
            fixture.writable (attempt, "submit-target");
            assert (fixture.owner->drain () == 1);
            assert (wait.await_ready ());
            assert_protocol_error ([&] { wait.await_resume (); });
            assert (releases == 1);
            assert (fixture.attempts.size () == attempt + 2);
            assert_unregistered (fixture, context);
        };
        if (request) {
            auto submission = fixture.socket.request (target).message (message).async ();
            check_retry (submission);
        } else {
            auto submission = fixture.socket.send (target).message (message).async ();
            check_retry (submission);
        }
    }
}
