/* SPDX-License-Identifier: MPL-2.0 */

#include "support.hpp"
#include <Runtime/Messaging/completion_owner.hpp>
#include <Runtime/Sockets/socket_access.hpp>
#include <Runtime/Sockets/socket_runtime_state.hpp>

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <tuple>
#include <utility>

namespace
{
struct send_fixture_t
{
    std::span<const std::string> expected_parts;
    bool accept = false;
    bool wait_on_first_attempt = false;
    unsigned attempts = 0;
    zlink_completion_t writable{};
};

send_fixture_t *active = nullptr;

zlink_submit_result_t submit_send (
  zlink_msg_t *parts_, size_t part_count_, zlink_send_flags_t flags_,
  void *context_, zlink_completion_id_t *id_)
{
    auto &fixture = *active;
    assert (flags_ == ZLINK_SEND_FLAGS_DONTWAIT && context_ && id_);
    assert (part_count_ == fixture.expected_parts.size ());
    for (size_t i = 0; i < part_count_; ++i) {
        const auto &expected = fixture.expected_parts[i];
        assert (zlink_msg_size (&parts_[i]) == expected.size ());
        assert (std::memcmp (zlink_msg_data (&parts_[i]), expected.data (),
                             expected.size ()) == 0);
        assert (zlink_msg_close (&parts_[i]) == ZLINK_CONFIG_OK);
        assert (zlink_msg_init (&parts_[i]) == ZLINK_CONFIG_OK);
    }

    *id_ = 0;
    ++fixture.attempts;
    if (fixture.accept)
        return ZLINK_SUBMIT_OK;
    if (fixture.attempts == 1 && fixture.wait_on_first_attempt) {
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

template <typename Socket, typename SubmitFn, size_t N>
bool test_tokenless_backpressure (Socket &socket_, SubmitFn submit_,
                                 const std::array<std::string, N> &payloads_,
                                 const char *initial_path_, const char *retry_path_)
{
    const auto owner = zlink::detail::runtime_state (socket_)->completion;
    send_fixture_t fixture{payloads_};
    active = &fixture;
    owner->transfer_to_public (&fixture);

    std::array<zlink::message_t, N> parts;
    std::array<const std::byte *, N> data;
    for (size_t i = 0; i < N; ++i) {
        parts[i] = zlink::message_t::from (payloads_[i]);
        data[i] = parts[i].data ();
    }
    const bool initial = expect_tokenless_backpressure (initial_path_, [&] {
        (void) std::apply (submit_, parts);
    });
    assert (fixture.attempts == 1);
    for (size_t i = 0; i < N; ++i) {
        assert (parts[i].valid ());
        assert (parts[i].data () == data[i]);
        assert (parts[i].to_string () == payloads_[i]);
    }

    fixture.accept = true;
    auto resent = std::apply (submit_, parts);
    assert (fixture.attempts == 2 && resent.result == ZLINK_SUBMIT_OK);
    auto resent_admitted = std::move (resent.admitted).operator co_await ();
    assert (resent_admitted.await_ready ());
    resent_admitted.await_resume ();

    fixture.accept = false;
    for (size_t i = 0; i < N; ++i)
        parts[i] = zlink::message_t::from (payloads_[i]);
    const bool moved = expect_tokenless_backpressure (initial_path_, [&] {
        (void) std::apply (submit_, std::move (parts));
    });
    assert (fixture.attempts == 3);
    for (const auto &part : parts)
        assert (!part.valid ());

    fixture.wait_on_first_attempt = true;
    fixture.attempts = 0;
    for (size_t i = 0; i < N; ++i)
        parts[i] = zlink::message_t::from (payloads_[i]);
    auto submission = std::apply (submit_, std::move (parts));
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
    return initial && moved && retry;
}
} // namespace

extern "C" zlink_submit_result_t __wrap_zlink_send (
  void *, zlink_msg_t *parts_, size_t part_count_, zlink_send_flags_t flags_,
  void *context_, zlink_completion_id_t *id_)
{
    return submit_send (parts_, part_count_, flags_, context_, id_);
}

extern "C" zlink_submit_result_t __wrap_zlink_send_rid (
  void *, const zlink_routing_id_t *, zlink_msg_t *parts_, size_t part_count_,
  zlink_send_flags_t flags_, void *context_, zlink_completion_id_t *id_)
{
    return submit_send (parts_, part_count_, flags_, context_, id_);
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
      pair, [&] (auto &&part_) {
          return pair.send ().message (std::forward<decltype (part_)> (part_)).async ();
      },
      std::to_array<std::string> ({"single-part payload backed by native storage"}),
      "raw SEND initial", "raw SEND retry");

    zlink::router_socket_t router (context);
    const auto target = zlink::routing_id_t::from ("target");
    const bool routed = test_tokenless_backpressure (
      router, [&] (auto &&first_, auto &&second_) {
          return router.send (target)
            .message (std::forward<decltype (first_)> (first_))
            .message (std::forward<decltype (second_)> (second_)).async ();
      },
      std::to_array<std::string> ({"first routed part backed by native storage",
                                       "second routed part backed by native storage"}),
      "routed SEND initial", "routed SEND retry");
    return raw && routed ? 0 : 1;
}
