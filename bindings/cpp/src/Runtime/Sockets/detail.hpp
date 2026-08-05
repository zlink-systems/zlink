/* SPDX-License-Identifier: MPL-2.0 */
#ifndef ZLINK_CPP_SOCKETS_DETAIL_HPP_INCLUDED
#define ZLINK_CPP_SOCKETS_DETAIL_HPP_INCLUDED

#include <zlink/Contracts/Sockets/message_socket_contracts.hpp>
#include <zlink/Contracts/Sockets/pubsub_socket_contracts.hpp>
#include <zlink/Contracts/Errors/errors.hpp>
#include <zlink.h>
#include "../Core/operation_detail.hpp"
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

class recv_part_out_guard_t
{
  public:
    explicit recv_part_out_guard_t (message_t &part_) noexcept :
        _part (part_), _has_saved (false), _committed (false)
    {
        // HOT PATH: caller-provided single-part recv must preserve a non-empty
        // output message when the native receive fails, but an empty output
        // message has no payload to restore. Skipping save/restore for the
        // empty case avoids one native message init/close pair per receive
        // while keeping the public failure contract for non-empty messages.
        if (_part.valid () && zlink_msg_size (native_handle (_part)) > 0) {
            move_to_native (_part, &_saved);
            _has_saved = true;
        }
    }

    ~recv_part_out_guard_t ()
    {
        if (_committed)
            return;
        _part.close ();
        if (_has_saved)
            adopt_native_message (_part, &_saved);
    }

    bool prepare ()
    {
        _part.init ();
        return _part.valid ();
    }

    void commit () noexcept
    {
        if (_has_saved)
            (void) zlink_msg_close (&_saved);
        _has_saved = false;
        _committed = true;
    }

  private:
    message_t &_part;
    zlink_msg_t _saved;
    bool _has_saved;
    bool _committed;
};

inline int recv_single_part_message (void *handle_,
                                     routing_id_t *source_rid_out_,
                                     message_t &part_out_,
                                     recv_flags_t flags_)
{
    recv_part_out_guard_t part_guard (part_out_);
    if (!part_guard.prepare ())
        return -1;

    const zlink_routing_id_t *source_rid = nullptr;
    zlink_part_flag_t has_more = ZLINK_PART_FINAL;
    const int rc =
      zlink_recv_part (handle_, &source_rid, detail::native_handle (part_out_), &has_more,
                       static_cast<zlink_recv_flags_t> (static_cast<int> (flags_)));
    if (rc != 0)
        return rc;
    if (has_more != ZLINK_PART_FINAL) {
        errno = EMSGSIZE;
        return -1;
    }

    if (source_rid_out_) {
        if (source_rid && source_rid->size > 0)
            assign_routing_id_native (*source_rid_out_, *source_rid);
        else
            *source_rid_out_ = unchecked_empty_routing_id ();
    }
    part_guard.commit ();
    return 0;
}

inline int recv_single_part_routed_message (void *handle_,
                                            routing_id_t &source_rid_out_,
                                            message_t &part_out_,
                                            recv_flags_t flags_)
{
    recv_part_out_guard_t part_guard (part_out_);
    if (!part_guard.prepare ())
        return -1;

    const zlink_routing_id_t *source_node_rid = nullptr;
    uint64_t request_seq = 0;
    zlink_part_flag_t has_more = ZLINK_PART_FINAL;
    const int rc = zlink_router_recv_part (
      handle_, &source_node_rid, &request_seq, detail::native_handle (part_out_),
      &has_more, static_cast<zlink_recv_flags_t> (static_cast<int> (flags_)));
    if (rc != 0)
        return rc;
    if (has_more != ZLINK_PART_FINAL || request_seq != 0 || !source_node_rid
        || source_node_rid->size == 0) {
        errno = has_more != ZLINK_PART_FINAL ? EMSGSIZE : EPROTO;
        return -1;
    }

    assign_routing_id_native (source_rid_out_, *source_node_rid);
    part_guard.commit ();
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
