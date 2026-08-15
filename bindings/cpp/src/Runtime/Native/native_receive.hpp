/* SPDX-License-Identifier: MPL-2.0 */
#ifndef ZLINK_CPP_RUNTIME_NATIVE_RECEIVE_HPP_INCLUDED
#define ZLINK_CPP_RUNTIME_NATIVE_RECEIVE_HPP_INCLUDED

#include "native_message_guard.hpp"
#include "native_message_parts.hpp"
#include "hwm_budget_lease.hpp"
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
    std::shared_ptr<hwm_budget_lease_set_t> leases;

    void reset () noexcept
    {
        source_rid = zlink::detail::unchecked_empty_routing_id ();
        has_request_seq = false;
        request_seq = 0;
        single_part.reset ();
        parts.clear ();
        leases.reset ();
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

inline int recv_router_part (
  void *socket_, bool retain_credit_, const zlink_routing_id_t **source_rid_,
  uint64_t *request_seq_, zlink_msg_t *part_,
  zlink_hwm_budget_lease_t **lease_, zlink_part_flag_t *has_more_,
  recv_flags_t flags_)
{
    *lease_ = nullptr;
    if (!retain_credit_)
        return zlink_router_recv_part (
          socket_, source_rid_, request_seq_, part_, has_more_,
          static_cast<zlink_recv_flags_t> (static_cast<int> (flags_)));
    uint64_t transport_pair_id = 0;
    uint64_t transport_pair_generation = 0;
    return zlink_router_recv_part_v2_with_hwm_budget_lease (
      socket_, source_rid_, request_seq_, &transport_pair_id,
      &transport_pair_generation, part_, lease_, has_more_,
      static_cast<zlink_recv_flags_t> (static_cast<int> (flags_)));
}

inline int recv_basic_part (
  void *socket_, bool use_dealer_recv_, bool retain_credit_,
  const zlink_routing_id_t **source_rid_, uint8_t *message_type_,
  uint64_t *request_seq_, zlink_msg_t *part_,
  zlink_hwm_budget_lease_t **lease_, zlink_part_flag_t *has_more_,
  recv_flags_t flags_)
{
    *lease_ = nullptr;
    const zlink_recv_flags_t native_flags =
      static_cast<zlink_recv_flags_t> (static_cast<int> (flags_));
    if (use_dealer_recv_)
        return retain_credit_
          ? zlink_dealer_recv_part_with_hwm_budget_lease (
              socket_, message_type_, request_seq_, part_, lease_, has_more_,
              native_flags)
          : zlink_dealer_recv_part (socket_, message_type_, request_seq_,
                                    part_, has_more_, native_flags);
    return retain_credit_
      ? zlink_recv_part_with_hwm_budget_lease (
          socket_, source_rid_, part_, lease_, has_more_, native_flags)
      : zlink_recv_part (socket_, source_rid_, part_, has_more_, native_flags);
}

inline int recv_envelope (void *socket_,
                          recv_flags_t flags_,
                          recv_envelope_t &envelope_,
                          bool use_router_recv_,
                          bool use_dealer_recv_,
                          bool retain_credit_)
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

        zlink_hwm_budget_lease_t *lease = nullptr;
        const int first_rc = recv_router_part (
          socket_, retain_credit_, &source_rid, &request_seq,
          detail::native_handle (first_msg), &lease, &has_more, flags_);
        if (first_rc != ZLINK_RECV_OK) {
            release_hwm_budget_lease (lease);
            return -1;
        }
        adopt_hwm_budget_lease (envelope_.leases, lease);
        refresh_payload_presence (first_msg);

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
            lease = nullptr;
            const int rc = recv_router_part (
              socket_, retain_credit_, &source_rid, &request_seq,
              detail::native_handle (next_msg), &lease, &has_more, flags_);
            if (rc != ZLINK_RECV_OK) {
                release_hwm_budget_lease (lease);
                return -1;
            }
            adopt_hwm_budget_lease (envelope_.leases, lease);
            refresh_payload_presence (next_msg);

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
        zlink_hwm_budget_lease_t *lease = nullptr;
        uint8_t message_type = 0;
        uint64_t dealer_request_seq = 0;
        const int first_rc = recv_basic_part (
          socket_, use_dealer_recv_, retain_credit_, &source_rid,
          &message_type, &dealer_request_seq, first_part.get (), &lease,
          &has_more, flags_);
        if (first_rc != ZLINK_RECV_OK) {
            release_hwm_budget_lease (lease);
            return first_rc;
        }
        adopt_hwm_budget_lease (envelope_.leases, lease);
        if (dealer_request_seq != 0) {
            envelope_.has_request_seq = true;
            envelope_.request_seq = dealer_request_seq;
        }

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

            lease = nullptr;
            const int rc = recv_basic_part (
              socket_, use_dealer_recv_, retain_credit_, &source_rid,
              &message_type, &dealer_request_seq, next_part.get (), &lease,
              &has_more, flags_);
            if (rc != ZLINK_RECV_OK) {
                release_hwm_budget_lease (lease);
                return rc;
            }
            adopt_hwm_budget_lease (envelope_.leases, lease);

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
