/* SPDX-License-Identifier: MPL-2.0 */
#ifndef ZLINK_CPP_SOCKETS_DETAIL_HPP_INCLUDED
#define ZLINK_CPP_SOCKETS_DETAIL_HPP_INCLUDED

#include <zlink/Contracts/Sockets/message_socket_contracts.hpp>
#include <zlink/Contracts/Sockets/pubsub_socket_contracts.hpp>
#include <zlink/Contracts/Errors/errors.hpp>
#include <zlink.h>
#include "../Core/routing_id_access.hpp"
#include "../Messaging/received_access.hpp"
#include "../Native/native_receive.hpp"

#include <cstring>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>

namespace zlink
{

// Shared implementation helpers for concrete socket entrypoint headers.

namespace detail
{

inline void assign_recv_source_rid (routing_id_t *source_rid_out_,
                                    const zlink_routing_id_t *source_rid_) noexcept
{
    if (!source_rid_out_)
        return;
    if (source_rid_ && source_rid_->size > 0)
        assign_routing_id_native (*source_rid_out_, *source_rid_);
    else
        *source_rid_out_ = unchecked_empty_routing_id ();
}

inline int recv_single_part_message (void *handle_,
                                     routing_id_t *source_rid_out_,
                                     message_t &part_out_,
                                     recv_flags_t flags_)
{
    const zlink_routing_id_t *source_rid = nullptr;
    zlink_msg_t native_part;
    size_t part_count = 0;
    const int rc = zlink_recv (
      handle_, &source_rid, &native_part, 1u, &part_count,
      static_cast<zlink_recv_flags_t> (static_cast<int> (flags_)));
    if (rc == ZLINK_RECV_BUFFER_TOO_SMALL) {
        errno = EMSGSIZE;
        return -1;
    }
    if (rc != ZLINK_RECV_OK)
        return rc;
    if (part_count != 1u) {
        if (part_count > 0)
            close_message_array (&native_part, 1u);
        errno = EPROTO;
        return -1;
    }

    assign_recv_source_rid (source_rid_out_, source_rid);
    adopt_native_message (part_out_, &native_part);
    close_message_array (&native_part, 1u);
    return 0;
}

inline int recv_single_part_routed_message (void *handle_,
                                            routing_id_t &source_rid_out_,
                                            message_t &part_out_,
                                            recv_flags_t flags_)
{
    const zlink_routing_id_t *source_node_rid = nullptr;
    zlink_reply_token_t reply_token = 0;
    zlink_msg_t native_part;
    size_t part_count = 0;
    const int rc = zlink_router_recv (
      handle_, &source_node_rid, &reply_token, &native_part, 1u, &part_count,
      static_cast<zlink_recv_flags_t> (static_cast<int> (flags_)));
    if (rc == ZLINK_RECV_BUFFER_TOO_SMALL) {
        errno = EMSGSIZE;
        return -1;
    }
    if (rc != ZLINK_RECV_OK)
        return rc;
    if (part_count != 1u || reply_token != 0 || !source_node_rid
        || source_node_rid->size == 0) {
        if (part_count > 0)
            close_message_array (&native_part, 1u);
        errno = EPROTO;
        return -1;
    }

    assign_routing_id_native (source_rid_out_, *source_node_rid);
    adopt_native_message (part_out_, &native_part);
    close_message_array (&native_part, 1u);
    return 0;
}

inline void set_routing_id_or_throw (void *handle_, const routing_id_t &routing_id_)
{
    if (zlink_set_routing_id (handle_, routing_id_.data (), routing_id_.size ()) != 0)
        throw config_error_t (config_result_from_errno (zlink_errno ()), zlink_errno ());
}

inline void get_routing_id_or_throw (void *handle_, routing_id_t &routing_id_)
{
    zlink_routing_id_t native;
    std::memset (&native, 0, sizeof (native));
    if (zlink_get_routing_id (handle_, &native) != 0)
        throw config_error_t (config_result_from_errno (zlink_errno ()), zlink_errno ());
    assign_routing_id_native (routing_id_, native);
}

} // namespace detail

} // namespace zlink

#endif
