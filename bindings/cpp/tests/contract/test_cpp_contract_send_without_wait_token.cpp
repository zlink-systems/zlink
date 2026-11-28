/* SPDX-License-Identifier: MPL-2.0 */

#include "support.hpp"
#include <Runtime/Messaging/completion_owner.hpp>
#include <Runtime/Sockets/socket_access.hpp>
#include <Runtime/Sockets/socket_runtime_state.hpp>

#include <cerrno>
#include <cstdio>
#include <utility>

namespace
{
struct send_fixture_t
{
    bool wait_on_first_attempt;
    size_t expected_parts;
    unsigned attempts = 0;
    zlink_completion_t writable{};
};

send_fixture_t *active = nullptr;

zlink_submit_result_t reject_send (
  zlink_msg_t *parts_, size_t part_count_, zlink_send_flags_t flags_,
  void *context_, zlink_completion_id_t *id_)
{
    auto &fixture = *active;
    assert (flags_ == ZLINK_SEND_FLAGS_DONTWAIT && context_ && id_);
    assert (part_count_ == fixture.expected_parts);
    for (size_t i = 0; i < part_count_; ++i) {
        assert (zlink_msg_close (&parts_[i]) == ZLINK_CONFIG_OK);
        assert (zlink_msg_init (&parts_[i]) == ZLINK_CONFIG_OK);
    }

    *id_ = 0;
    if (++fixture.attempts == 1 && fixture.wait_on_first_attempt) {
        *id_ = 1;
        fixture.writable.struct_size = sizeof (zlink_completion_t);
        fixture.writable.kind = ZLINK_COMPLETION_WRITABLE;
        fixture.writable.completion_id = *id_;
        fixture.writable.user_context = context_;
        fixture.writable.send_result = ZLINK_SEND_ADMITTED;
    }
    errno = EAGAIN;
    return ZLINK_SUBMIT_BACKPRESSURED;
}

template <typename SendFn>
bool expect_tokenless_backpressure (const char *path_, SendFn send_)
{
    try {
        send_ ();
    }
    catch (const zlink::submit_error_t &error) {
        if (error.result () == zlink::submit_result_t::backpressured
            && error.internal_errno () == EAGAIN)
            return true;
        std::fprintf (stderr, "%s: expected BACKPRESSURED/EAGAIN, got %s\n",
                      path_, error.what ());
        return false;
    }
    std::fprintf (stderr, "%s: expected a submit error\n", path_);
    return false;
}

template <typename Socket, typename SubmitFn>
bool test_tokenless_backpressure (Socket &socket_, SubmitFn submit_,
                                 size_t expected_parts_, const char *initial_path_,
                                 const char *retry_path_)
{
    const auto owner = zlink::detail::runtime_state (socket_)->completion;
    send_fixture_t fixture{false, expected_parts_};
    active = &fixture;
    owner->transfer_to_public (&fixture);

    const bool initial = expect_tokenless_backpressure (initial_path_, [&] {
        (void) submit_ ();
    });
    assert (fixture.attempts == 1);

    fixture.wait_on_first_attempt = true;
    fixture.attempts = 0;
    auto submission = submit_ ();
    assert (submission.result == ZLINK_SUBMIT_BACKPRESSURED);
    auto admitted = std::move (submission.admitted).operator co_await ();
    assert (!admitted.await_ready ());
    assert (owner->drain () == 1);
    assert (fixture.attempts == 2 && admitted.await_ready ());
    const bool retry = expect_tokenless_backpressure (retry_path_, [&] {
        admitted.await_resume ();
    });

    owner->shutdown ();
    active = nullptr;
    return initial && retry;
}
} // namespace

extern "C" zlink_submit_result_t __wrap_zlink_send (
  void *, zlink_msg_t *parts_, size_t part_count_, zlink_send_flags_t flags_,
  void *context_, zlink_completion_id_t *id_)
{
    return reject_send (parts_, part_count_, flags_, context_, id_);
}

extern "C" zlink_submit_result_t __wrap_zlink_send_rid (
  void *, const zlink_routing_id_t *, zlink_msg_t *parts_, size_t part_count_,
  zlink_send_flags_t flags_, void *context_, zlink_completion_id_t *id_)
{
    return reject_send (parts_, part_count_, flags_, context_, id_);
}

extern "C" zlink_recv_result_t __wrap_zlink_completion_recv (
  void *, zlink_completion_t *completion_, zlink_recv_flags_t flags_)
{
    assert (flags_ == ZLINK_RECV_FLAGS_DONTWAIT);
    if (active && active->writable.completion_id != 0) {
        *completion_ = std::exchange (active->writable, zlink_completion_t{});
        return ZLINK_RECV_OK;
    }
    errno = EAGAIN;
    return ZLINK_RECV_NO_DATA;
}

int main ()
{
    zlink::context_t context;
    zlink::pair_socket_t pair (context);
    const bool raw = test_tokenless_backpressure (
      pair, [&] { return pair.send ().message (zlink::message_t::from ("send")).async (); },
      1, "raw SEND initial", "raw SEND retry");

    zlink::router_socket_t router (context);
    const auto target = zlink::routing_id_t::from ("target");
    const bool routed = test_tokenless_backpressure (
      router, [&] {
          return router.send (target)
            .message (zlink::message_t::from ("first"))
            .message (zlink::message_t::from ("second")).async ();
      },
      2, "routed SEND initial", "routed SEND retry");
    return raw && routed ? 0 : 1;
}
