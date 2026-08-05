/* SPDX-License-Identifier: MPL-2.0 */
#ifndef ZLINK_CPP_RUNTIME_NATIVE_RECEIVE_HPP_INCLUDED
#define ZLINK_CPP_RUNTIME_NATIVE_RECEIVE_HPP_INCLUDED

#include "native_message_guard.hpp"
#include "native_message_parts.hpp"
#include "../Core/routing_id_access.hpp"

#include <zlink/Contracts/Sockets/results.hpp>

#include <optional>
#include <utility>
#include <vector>

namespace zlink
{
namespace detail
{

struct recv_envelope_t
{
    routing_id_t source_rid;
    bool has_request_seq;
    uint64_t request_seq;
    std::optional<message_t> single_part;
    std::vector<message_t> parts;

    void reset () noexcept
    {
        source_rid = zlink::detail::unchecked_empty_routing_id ();
        has_request_seq = false;
        request_seq = 0;
        single_part.reset ();
        parts.clear ();
    }

    recv_envelope_t () :
        source_rid (zlink::detail::unchecked_empty_routing_id ()),
        has_request_seq (false),
        request_seq (0),
        single_part (),
        parts ()
    {
    }
};

inline int recv_envelope (void *socket_,
                          recv_flags_t flags_,
                          recv_envelope_t &envelope_,
                          bool use_router_recv_)
{
    envelope_.reset ();

    if (use_router_recv_) {
        const zlink_routing_id_t *source_rid = nullptr;
        uint64_t request_seq = 0;
        zlink_part_flag_t has_more = ZLINK_PART_FINAL;
        message_t first_msg;
        if (!first_msg.valid ()) {
            errno = EFAULT;
            return -1;
        }

        const int first_rc = zlink_router_recv_part (
          socket_, &source_rid, &request_seq, detail::native_handle (first_msg),
          &has_more, static_cast<zlink_recv_flags_t> (static_cast<int> (flags_)));
        if (first_rc != ZLINK_RECV_OK)
            return -1;

        if (source_rid && source_rid->size > 0)
            envelope_.source_rid = zlink::detail::native_routing_id (*source_rid);
        if (request_seq != 0) {
            envelope_.has_request_seq = true;
            envelope_.request_seq = request_seq;
        }

        if (has_more == ZLINK_PART_FINAL) {
            envelope_.single_part.emplace (std::move (first_msg));
            return 0;
        }

        envelope_.parts.reserve (2);
        envelope_.parts.emplace_back (std::move (first_msg));
        for (;;) {
            message_t next_msg;
            if (!next_msg.valid ()) {
                errno = EFAULT;
                return -1;
            }

            has_more = ZLINK_PART_FINAL;
            const int rc =
              zlink_router_recv_part (socket_, &source_rid, &request_seq,
                                      detail::native_handle (next_msg), &has_more,
                                      static_cast<zlink_recv_flags_t> (static_cast<int> (flags_)));
            if (rc != ZLINK_RECV_OK)
                return -1;

            envelope_.parts.emplace_back (std::move (next_msg));
            if (has_more == ZLINK_PART_FINAL)
                return 0;
        }
    } else {
        const zlink_routing_id_t *source_rid = nullptr;
        scoped_native_message_t first_part;
        if (!first_part.init ())
            return -1;

        zlink_part_flag_t has_more = ZLINK_PART_FINAL;
        const int first_rc =
          zlink_recv_part (socket_, &source_rid, first_part.get (), &has_more,
                           static_cast<zlink_recv_flags_t> (static_cast<int> (flags_)));
        if (first_rc != ZLINK_RECV_OK)
            return first_rc;

        message_t first_msg;
        first_part.adopt_into (first_msg);

        if (has_more == ZLINK_PART_FINAL) {
            envelope_.single_part.emplace (std::move (first_msg));
            if (source_rid && source_rid->size > 0)
                envelope_.source_rid = zlink::detail::native_routing_id (*source_rid);
            return 0;
        }

        envelope_.parts.reserve (2);
        envelope_.parts.emplace_back (std::move (first_msg));
        while (has_more != ZLINK_PART_FINAL) {
            scoped_native_message_t next_part;
            if (!next_part.init ())
                return -1;

            const int rc =
              zlink_recv_part (socket_, &source_rid, next_part.get (), &has_more,
                               static_cast<zlink_recv_flags_t> (static_cast<int> (flags_)));
            if (rc != ZLINK_RECV_OK)
                return rc;

            message_t next_msg;
            next_part.adopt_into (next_msg);
            envelope_.parts.emplace_back (std::move (next_msg));
        }

        if (source_rid && source_rid->size > 0)
            envelope_.source_rid = zlink::detail::native_routing_id (*source_rid);
    }

    return 0;
}

} // namespace detail
} // namespace zlink

#endif
