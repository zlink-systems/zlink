/* SPDX-License-Identifier: MPL-2.0 */
#ifndef ZLINK_CPP_TEST_COMPLETION_NATIVE_FIXTURE_HPP_INCLUDED
#define ZLINK_CPP_TEST_COMPLETION_NATIVE_FIXTURE_HPP_INCLUDED

#include "support.hpp"
#include <Runtime/Messaging/completion_owner.hpp>
#include <Runtime/Sockets/socket_access.hpp>
#include <Runtime/Sockets/socket_runtime_state.hpp>

#include <deque>
#include <map>

// Link-time native boundary fixture: submissions use public builders and real
// message ownership. Only admission and completion order are controlled here.
namespace completion_test
{
struct attempt_t
{
    void *context;
    uint64_t token;
    std::string target;
    std::string payload;
    bool request;
};

struct fixture_t
{
    zlink::context_t context;
    zlink::router_socket_t socket{context};
    std::shared_ptr<zlink::detail::completion_owner_t> owner =
      zlink::detail::runtime_state (socket)->completion;
    std::deque<zlink_completion_t> completions;
    std::vector<attempt_t> attempts;
    std::vector<std::string> events;
    std::map<void *, unsigned> counts;
    std::function<void ()> first_submit;
    std::function<void ()> before_recv;
    std::function<void ()> after_recv;

    fixture_t () { owner->transfer_to_public (this); }
    ~fixture_t ()
    {
        owner->shutdown ();
        for (auto &completion : completions)
            zlink_completion_close (&completion);
    }

    void writable (size_t attempt_, const std::string &rid_)
    {
        const auto &attempt = attempts.at (attempt_);
        zlink_completion_t completion{};
        completion.struct_size = sizeof (completion);
        completion.kind = ZLINK_COMPLETION_WRITABLE;
        completion.completion_id = attempt.token;
        completion.user_context = attempt.context;
        completion.send_result = ZLINK_SEND_ADMITTED;
        completion.peer_rid.size = static_cast<uint8_t> (rid_.size ());
        std::memcpy (completion.peer_rid.data, rid_.data (), rid_.size ());
        completions.push_back (completion);
    }
};

inline fixture_t *active = nullptr;

inline zlink_submit_result_t submit (
  void *socket_, const zlink_routing_id_t *rid_, zlink_msg_t *part_,
  zlink_send_flags_t flags_, zlink_part_flag_t part_flag_, void *context_,
  zlink_completion_id_t *id_, bool request_)
{
    auto &fixture = *active;
    assert (socket_ == zlink::detail::native_handle (fixture.socket));
    assert (flags_ == ZLINK_SEND_FLAGS_DONTWAIT);
    assert (part_flag_ == ZLINK_PART_FINAL);
    assert (context_ && id_);
    const std::string target = rid_
      ? std::string (reinterpret_cast<const char *> (rid_->data), rid_->size)
      : std::string ();
    const std::string payload (
      static_cast<const char *> (zlink_msg_data (part_)), zlink_msg_size (part_));
    assert (zlink_msg_close (part_) == ZLINK_CONFIG_OK);
    assert (zlink_msg_init (part_) == ZLINK_CONFIG_OK);
    const bool first = ++fixture.counts[context_] == 1;
    *id_ = first || request_ ? fixture.attempts.size () + 1 : 0;
    fixture.attempts.push_back ({context_, *id_, target, payload, request_});
    fixture.events.push_back ("submit:" + payload);
    if (first) {
        if (fixture.first_submit)
            fixture.first_submit ();
        errno = EAGAIN;
        return ZLINK_SUBMIT_BACKPRESSURED;
    }
    if (request_) {
        zlink_completion_t completion{};
        completion.struct_size = sizeof (completion);
        completion.kind = ZLINK_COMPLETION_REQUEST;
        completion.completion_id = *id_;
        completion.user_context = context_;
        completion.request_result = ZLINK_REQUEST_OK;
        fixture.completions.push_back (completion);
    }
    return ZLINK_SUBMIT_OK;
}
} // namespace completion_test

extern "C" zlink_submit_result_t __wrap_zlink_send_part_rid (
  void *socket_, const zlink_routing_id_t *rid_, zlink_msg_t *part_,
  zlink_send_flags_t flags_, zlink_part_flag_t part_flag_, void *context_,
  zlink_completion_id_t *id_)
{
    return completion_test::submit (socket_, rid_, part_, flags_, part_flag_,
                                     context_, id_, false);
}

extern "C" zlink_submit_result_t __wrap_zlink_request_part (
  void *socket_, const zlink_routing_id_t *rid_, zlink_msg_t *part_,
  zlink_send_flags_t flags_, zlink_part_flag_t part_flag_, uint32_t,
  void *context_, zlink_completion_id_t *id_)
{
    return completion_test::submit (socket_, rid_, part_, flags_, part_flag_,
                                     context_, id_, true);
}

extern "C" zlink_recv_result_t __wrap_zlink_completion_recv (
  void *socket_, zlink_completion_t *completion_, zlink_recv_flags_t flags_)
{
    auto &fixture = *completion_test::active;
    assert (socket_ == zlink::detail::native_handle (fixture.socket));
    assert (flags_ == ZLINK_RECV_FLAGS_DONTWAIT);
    if (fixture.before_recv)
        fixture.before_recv ();
    if (fixture.completions.empty ()) {
        fixture.events.push_back ("NO_DATA");
        errno = EAGAIN;
        return ZLINK_RECV_NO_DATA;
    }
    *completion_ = fixture.completions.front ();
    fixture.completions.pop_front ();
    fixture.events.push_back ("recv:" + std::to_string (completion_->completion_id));
    if (fixture.after_recv)
        fixture.after_recv ();
    return ZLINK_RECV_OK;
}

#endif
