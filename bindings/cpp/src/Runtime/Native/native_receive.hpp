/* SPDX-License-Identifier: MPL-2.0 */
#ifndef ZLINK_CPP_RUNTIME_NATIVE_RECEIVE_HPP_INCLUDED
#define ZLINK_CPP_RUNTIME_NATIVE_RECEIVE_HPP_INCLUDED

#include "native_message_parts.hpp"
#include "../Core/routing_id_access.hpp"

#include <zlink/Contracts/Messaging/lazy_message_parts.hpp>
#include <zlink/Contracts/Sockets/results.hpp>

#include <utility>

namespace zlink
{
namespace detail
{

struct recv_envelope_t
{
    routing_id_t source_rid;
    bool has_reply_token;
    zlink_reply_token_t reply_token;
    lazy_message_parts_t *parts;

    void bind (lazy_message_parts_t &parts_) noexcept { parts = &parts_; }

    void reset () noexcept
    {
        source_rid = zlink::detail::unchecked_empty_routing_id ();
        has_reply_token = false;
        reply_token = 0;
        if (parts)
            parts->prepare_receive ();
    }

    void receive_single_part (message_t part_) { parts->receive_single_part (std::move (part_)); }

    void reserve_parts (size_t part_count_) { parts->reserve_receive_parts (part_count_); }

    void receive_part (message_t part_) { parts->receive_part (std::move (part_)); }

    recv_envelope_t () :
        source_rid (zlink::detail::unchecked_empty_routing_id ()),
        has_reply_token (false),
        reply_token (0),
        parts (nullptr)
    {
    }
};

inline int recv_router_part (void *socket_, const zlink_routing_id_t **source_rid_,
                             zlink_reply_token_t *reply_token_, zlink_msg_t *part_,
                             zlink_part_flag_t *has_more_, recv_flags_t flags_)
{
    return zlink_router_recv_part (
      socket_, source_rid_, reply_token_, part_, has_more_,
      static_cast<zlink_recv_flags_t> (static_cast<int> (flags_)));
}

inline int recv_basic_part (void *socket_,
  const zlink_routing_id_t **source_rid_, zlink_msg_t *part_,
  zlink_part_flag_t *has_more_, recv_flags_t flags_)
{
    const zlink_recv_flags_t native_flags =
      static_cast<zlink_recv_flags_t> (static_cast<int> (flags_));
    return zlink_recv_part (socket_, source_rid_, part_, has_more_, native_flags);
}

inline int recv_envelope (void *socket_,
                          recv_flags_t flags_,
                          recv_envelope_t &envelope_,
                          bool use_router_recv_)
{
    envelope_.reset ();

    if (use_router_recv_) {
        const zlink_routing_id_t *source_rid = nullptr;
        zlink_reply_token_t reply_token = 0;
        zlink_part_flag_t has_more = ZLINK_PART_FINAL;
        message_t first_msg;
        if (!first_msg.valid ()) {
            errno = EFAULT;
            return -1;
        }

        const int first_rc = recv_router_part (
          socket_, &source_rid, &reply_token, detail::native_handle (first_msg),
          &has_more, flags_);
        if (first_rc != ZLINK_RECV_OK) {
            return -1;
        }
        refresh_payload_presence (first_msg);

        if (source_rid && source_rid->size > 0)
            envelope_.source_rid = zlink::detail::native_routing_id (*source_rid);
        if (reply_token != 0) {
            envelope_.has_reply_token = true;
            envelope_.reply_token = reply_token;
        }

        if (has_more == ZLINK_PART_FINAL) {
            envelope_.receive_single_part (std::move (first_msg));
            return 0;
        }

        envelope_.reserve_parts (2);
        envelope_.receive_part (std::move (first_msg));
        for (;;) {
            message_t next_msg;
            if (!next_msg.valid ()) {
                errno = EFAULT;
                return -1;
            }

            has_more = ZLINK_PART_FINAL;
            const int rc = recv_router_part (
              socket_, &source_rid, &reply_token, detail::native_handle (next_msg),
              &has_more, flags_);
            if (rc != ZLINK_RECV_OK) {
                return -1;
            }
            refresh_payload_presence (next_msg);

            envelope_.receive_part (std::move (next_msg));
            if (has_more == ZLINK_PART_FINAL)
                return 0;
        }
    } else {
        const zlink_routing_id_t *source_rid = nullptr;
        message_t first_msg;
        if (!first_msg.valid ())
            return -1;

        zlink_part_flag_t has_more = ZLINK_PART_FINAL;
        const int first_rc = recv_basic_part (
          socket_, &source_rid, detail::native_handle (first_msg), &has_more, flags_);
        if (first_rc != ZLINK_RECV_OK) {
            const int saved_errno = errno;
            first_msg.close ();
            errno = saved_errno;
            return first_rc;
        }
        refresh_payload_presence (first_msg);
        if (has_more == ZLINK_PART_FINAL) {
            envelope_.receive_single_part (std::move (first_msg));
            if (source_rid && source_rid->size > 0)
                envelope_.source_rid = zlink::detail::native_routing_id (*source_rid);
            return 0;
        }

        envelope_.reserve_parts (2);
        envelope_.receive_part (std::move (first_msg));
        while (has_more != ZLINK_PART_FINAL) {
            message_t next_msg;
            if (!next_msg.valid ())
                return -1;

            const int rc = recv_basic_part (
              socket_, &source_rid, detail::native_handle (next_msg), &has_more, flags_);
            if (rc != ZLINK_RECV_OK) {
                const int saved_errno = errno;
                next_msg.close ();
                errno = saved_errno;
                return rc;
            }
            refresh_payload_presence (next_msg);
            envelope_.receive_part (std::move (next_msg));
        }

        if (source_rid && source_rid->size > 0)
            envelope_.source_rid = zlink::detail::native_routing_id (*source_rid);
    }

    return 0;
}

} // namespace detail
} // namespace zlink

#endif
