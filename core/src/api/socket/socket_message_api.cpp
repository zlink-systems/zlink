/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "api/socket/socket_api_internal.hpp"
#include "api/socket/socket_message_api_internal.hpp"
#include "api/socket/part_helper_internal.hpp"
#include "api/message/recv_result_internal.hpp"
#include "api/socket/request_reply_protocol_internal.hpp"
#include "api/socket/socket_request_reply_internal.hpp"
#include "core/recv_internal.hpp"
#include "core/scoped_msg.hpp"

namespace
{
int validate_basic_recv_entry (void *s_, const void *parts_out_,
                               const void *count_out_,
                               zlink_recv_flags_t flags_,
                               socket_handle_t *handle_out_, int *type_out_)
{
    if (!s_ || !handle_out_ || !type_out_) {
        errno = EFAULT;
        return -1;
    }

    socket_handle_t handle = as_socket_handle (s_);
    if (!handle.socket)
        return -1;
    handle.socket->clear_last_recv_source_rid ();
    if (!parts_out_ || !count_out_) {
        errno = EFAULT;
        return -1;
    }
    if (validate_recv_flags (flags_) != 0)
        return -1;

    *type_out_ = socket_type (handle);
    *handle_out_ = std::move (handle);
    return 0;
}

void export_socket_owned_source_rid (
  zlink::socket_base_t *socket_,
  const zlink_routing_id_t *source_rid_,
  const zlink_routing_id_t **source_rid_out_)
{
    if (!source_rid_out_)
        return;
    if (!socket_ || !source_rid_ || source_rid_->size == 0) {
        *source_rid_out_ = NULL;
        return;
    }

    socket_->store_last_recv_source_rid (source_rid_);
    *source_rid_out_ = socket_->last_recv_source_rid_view ();
}

void close_buffered_recv_parts (
  zlink::part_helper_internal::recv_part_buffer_t *parts_)
{
    if (!parts_)
        return;
    for (size_t i = 0; i < parts_->size (); ++i)
        zlink_msg_close (&(*parts_)[i]);
    parts_->clear ();
}

void discard_subscribe_payload_tail (zlink::socket_base_t *socket_)
{
    while (socket_) {
        zlink::msg_t part;
        const int init_rc = part.init ();
        errno_assert (init_rc == 0);
        if (socket_->recv (&part, ZLINK_DONTWAIT) != 0) {
            const int close_rc = part.close ();
            errno_assert (close_rc == 0);
            return;
        }
        const bool more = (part.flags () & zlink::msg_t::more) != 0;
        const int close_rc = part.close ();
        errno_assert (close_rc == 0);
        if (!more)
            return;
    }
}

class public_part_delivery_hold_owner_t
{
  public:
    explicit public_part_delivery_hold_owner_t (
      zlink::socket_base_t *socket_) :
        _socket (socket_),
        _active (false)
    {
    }

    ~public_part_delivery_hold_owner_t ()
    {
        if (_active && _socket)
            _socket->end_public_part_receive_delivery_hold ();
    }

    void activate (bool active_) { _active = active_; }

    int transfer_to (
      const std::shared_ptr<zlink::part_helper_internal::handle_state_t>
        &state_)
    {
        if (!_active)
            return 0;
        if (zlink::part_helper_internal::adopt_recv_public_delivery_hold (
              state_)
            != 0)
            return -1;
        _active = false;
        return 0;
    }

  private:
    zlink::socket_base_t *_socket;
    bool _active;

    ZLINK_NON_COPYABLE_NOR_MOVABLE (public_part_delivery_hold_owner_t)
};

}

zlink_recv_result_t zlink_recv (
  void *s_, const zlink_routing_id_t **source_rid_out_,
  zlink_msg_t *parts_out_, size_t parts_capacity_, size_t *part_count_out_,
  zlink_recv_flags_t flags_)
{
    socket_handle_t handle;
    int type = -1;
    if (validate_basic_recv_entry (s_, parts_out_, part_count_out_, flags_,
                                   &handle, &type)
        != 0)
        return zlink::recv_result_internal::from_errno (errno);
    if (type != ZLINK_CORE_SOCKET_PAIR
        && type != ZLINK_CORE_SOCKET_DEALER
        && type != ZLINK_CORE_SOCKET_STREAM) {
        errno = ENOTSUP;
        return zlink::recv_result_internal::from_errno (errno);
    }

    if (type == ZLINK_CORE_SOCKET_STREAM
        && handle.socket->stream_mark_raw_part_receive () != 0)
        return zlink::recv_result_internal::from_errno (errno);

    std::shared_ptr<zlink::part_helper_internal::handle_state_t> helper_state =
      handle.socket->part_helper_state ();
    zlink::part_helper_internal::recv_record_metadata_t staged_metadata;
    size_t staged_part_count = 0;
    const zlink::part_helper_internal::staged_recv_record_result_t staged_rc =
      zlink::part_helper_internal::try_take_staged_recv_record (
        helper_state, zlink::part_helper_internal::recv_family_basic,
        parts_out_, parts_capacity_, &staged_part_count, &staged_metadata);
    if (staged_rc == zlink::part_helper_internal::staged_recv_record_error) {
        if (errno == ENOBUFS)
            *part_count_out_ = staged_part_count;
        return zlink::recv_result_internal::from_errno (errno);
    }
    if (staged_rc == zlink::part_helper_internal::staged_recv_record_taken) {
        export_socket_owned_source_rid (
          handle.socket, staged_metadata.return_source_rid_as_null
            ? NULL : &staged_metadata.source_node_rid, source_rid_out_);
        *part_count_out_ = staged_part_count;
        return ZLINK_RECV_OK;
    }

    bool public_part_delivery_hold_acquired = false;
    public_part_delivery_hold_owner_t public_delivery_hold_owner (
      handle.socket);
    zlink_msg_t *parts = NULL;
    size_t part_count = 0;
    // Only STREAM returns a source RID; PAIR/DEALER never use this storage.
    zlink_routing_id_t source_rid;
    if (type == ZLINK_CORE_SOCKET_STREAM)
        memset (&source_rid, 0, sizeof (source_rid));
    int recv_rc;
    if (type == ZLINK_CORE_SOCKET_DEALER) {
        // Keep caller slots untouched until the complete record is accepted.
        // A single part needs no intermediate TLS multipart export.
        zlink::scoped_msg_t terminal_part;
        bool terminal_part_returned = false;
        recv_rc = zlink::socket_reqrep_internal::recv_dealer_record (
          handle, &parts, &part_count, static_cast<int> (flags_),
          parts_capacity_ > 0 ? terminal_part.get () : NULL,
          &terminal_part_returned, true,
          &public_part_delivery_hold_acquired);
        public_delivery_hold_owner.activate (
          public_part_delivery_hold_acquired);
        if (recv_rc == 0 && terminal_part_returned) {
            const int adopt_rc =
              zlink_msg_adopt (&parts_out_[0], terminal_part.get ());
            errno_assert (adopt_rc == 0);
            if (source_rid_out_)
                *source_rid_out_ = NULL;
            *part_count_out_ = 1;
            errno = 0;
            return ZLINK_RECV_OK;
        }
    } else {
        recv_rc = zlink_socket_recv_handle_internal (
          handle, type == ZLINK_CORE_SOCKET_STREAM ? &source_rid : NULL,
          &parts, &part_count, static_cast<zlink_send_flags_t> (flags_));
    }
    if (recv_rc != 0)
        return zlink::recv_result_internal::from_errno (errno);

    if (!parts || part_count == 0) {
        zlink_multipart_close (parts, part_count);
        errno = EPROTO;
        return zlink::recv_result_internal::from_errno (errno);
    }

    if (parts_capacity_ < part_count) {
        if (!helper_state)
            helper_state =
              zlink::part_helper_internal::find_or_create_socket_state (
                handle.socket);
        if (!helper_state) {
            zlink_multipart_close (parts, part_count);
            return zlink::recv_result_internal::from_errno (errno);
        }
        const int stage_rc =
          zlink::part_helper_internal::stage_recv_sequence (
            helper_state, zlink::part_helper_internal::recv_family_basic,
            handle.socket, type == ZLINK_CORE_SOCKET_STREAM ? &source_rid : NULL,
            0, parts, part_count,
            std::this_thread::get_id ());
        zlink_multipart_close (parts, part_count);
        if (stage_rc != 0) {
            zlink::part_helper_internal::abort_recv_step (helper_state);
            return zlink::recv_result_internal::from_errno (errno);
        }
        if (public_delivery_hold_owner.transfer_to (helper_state) != 0) {
            zlink::part_helper_internal::abort_recv_step (helper_state);
            return zlink::recv_result_internal::from_errno (errno);
        }
        *part_count_out_ = part_count;
        errno = ENOBUFS;
        return ZLINK_RECV_BUFFER_TOO_SMALL;
    }

    // Caller slots are uninitialized (the contract does not require init), so
    // adopt into them rather than move: adopt overwrites without inspecting or
    // closing the destination, avoiding an uninitialized read.
    for (size_t i = 0; i < part_count; ++i) {
        const int adopt_rc = zlink_msg_adopt (&parts_out_[i], &parts[i]);
        errno_assert (adopt_rc == 0);
    }
    zlink_multipart_close (parts, part_count);
    export_socket_owned_source_rid (
      handle.socket, type == ZLINK_CORE_SOCKET_STREAM ? &source_rid : NULL,
      source_rid_out_);
    *part_count_out_ = part_count;
    errno = 0;
    return ZLINK_RECV_OK;
}

static zlink_recv_result_t subscribe_recv (void *subject_,
                                          const zlink_routing_id_t **source_rid_out_,
                                          char *topic_id_buf_,
                                          size_t topic_id_capacity_,
                                          size_t *topic_id_len_out_,
                                          zlink_msg_t *part_out_,
                                          zlink_recv_flags_t flags_,
                                          size_t parts_capacity_,
                                          size_t *part_count_out_)
{
    if (!subject_) {
        errno = EFAULT;
        return zlink::recv_result_internal::from_errno (errno);
    }

    socket_handle_t handle = as_socket_handle (subject_);
    if (!handle.socket)
        return zlink::recv_result_internal::from_errno (errno);
    handle.socket->clear_last_recv_source_rid ();

    if (!topic_id_len_out_ || !part_out_
        || !part_count_out_
        || (topic_id_capacity_ > 0 && !topic_id_buf_)) {
        errno = EFAULT;
        return zlink::recv_result_internal::from_errno (errno);
    }
    if (validate_recv_flags (flags_) != 0)
        return zlink::recv_result_internal::from_errno (errno);

    const int type = socket_type (handle);
    if (type != ZLINK_CORE_SOCKET_SUB && type != ZLINK_CORE_SOCKET_XSUB) {
        errno = ENOTSUP;
        return zlink::recv_result_internal::from_errno (errno);
    }

    std::shared_ptr<zlink::part_helper_internal::handle_state_t> helper_state =
      zlink::part_helper_internal::find_socket_state (handle.socket);
    bool sequence_active = false;
    if (helper_state) {
        std::lock_guard<std::mutex> lock (helper_state->mutex);
        sequence_active = helper_state->recv.active;
        if (sequence_active
            && (helper_state->recv.family != zlink::part_helper_internal::recv_family_subscribe
                || helper_state->recv.owner_thread != std::this_thread::get_id ()
                || helper_state->recv.next_part_index != 0)) {
            errno = EBUSY;
            return zlink::recv_result_internal::from_errno (errno);
        }
    }

    if (!sequence_active) {
        zlink_msg_t topic_frame;
        zlink_msg_init (&topic_frame);
        if (handle.socket->recv (reinterpret_cast<zlink::msg_t *> (&topic_frame), flags_) != 0) {
            zlink_msg_close (&topic_frame);
            zlink::part_helper_internal::abort_recv_step (helper_state);
            return zlink::recv_result_internal::from_errno (errno);
        }

        const bool topic_has_more =
          (reinterpret_cast<const zlink::msg_t *> (&topic_frame)->flags () & zlink::msg_t::more)
          != 0;
        std::string topic_id;
        try {
            topic_id.assign (
              static_cast<const char *> (zlink_msg_data (&topic_frame)),
              zlink_msg_size (&topic_frame));
        } catch (...) {
            zlink_msg_close (&topic_frame);
            if (topic_has_more)
                discard_subscribe_payload_tail (handle.socket);
            errno = ENOMEM;
            return zlink::recv_result_internal::from_errno (errno);
        }
        zlink_msg_close (&topic_frame);

        if (!topic_has_more) {
            errno = EPROTO;
            return zlink::recv_result_internal::from_errno (errno);
        }

        zlink_msg_t first_payload;
        zlink_msg_init (&first_payload);
        if (handle.socket->recv (
              reinterpret_cast<zlink::msg_t *> (&first_payload), flags_)
            != 0) {
            zlink_msg_close (&first_payload);
            return zlink::recv_result_internal::from_errno (errno);
        }

        const bool first_payload_has_more =
          (reinterpret_cast<const zlink::msg_t *> (&first_payload)->flags ()
           & zlink::msg_t::more)
          != 0;
        if (!first_payload_has_more && topic_id_capacity_ >= topic_id.size ()
            && parts_capacity_ >= 1) {
            // A terminal payload has no continuation state to own. Return it
            // directly and reserve the buffered sequence machinery for actual
            // multipart subscriptions.
            *topic_id_len_out_ = topic_id.size ();
            if (!topic_id.empty ())
                memcpy (topic_id_buf_, topic_id.data (), topic_id.size ());
            const int transfer_rc = zlink_msg_adopt (part_out_, &first_payload);
            if (transfer_rc != 0) {
                zlink_msg_close (&first_payload);
                errno = EFAULT;
                return zlink::recv_result_internal::from_errno (errno);
            }
            if (source_rid_out_)
                *source_rid_out_ = NULL;
            *part_count_out_ = 1;
            errno = 0;
            return ZLINK_RECV_OK;
        }

        zlink::part_helper_internal::recv_part_buffer_t buffered_parts;
        try {
            buffered_parts.resize (1);
        } catch (...) {
            zlink_msg_close (&first_payload);
            if (first_payload_has_more)
                discard_subscribe_payload_tail (handle.socket);
            errno = ENOMEM;
            return zlink::recv_result_internal::from_errno (errno);
        }
        zlink_msg_init (&buffered_parts[0]);
        if (zlink_msg_move (&buffered_parts[0], &first_payload) != 0) {
            zlink_msg_close (&first_payload);
            zlink_msg_close (&buffered_parts[0]);
            errno = EFAULT;
            return zlink::recv_result_internal::from_errno (errno);
        }
        bool payload_has_more = first_payload_has_more;
        while (payload_has_more) {
            try {
                buffered_parts.resize (buffered_parts.size () + 1);
            } catch (...) {
                close_buffered_recv_parts (&buffered_parts);
                discard_subscribe_payload_tail (handle.socket);
                errno = ENOMEM;
                return zlink::recv_result_internal::from_errno (errno);
            }
            zlink_msg_t &slot = buffered_parts.back ();
            zlink_msg_init (&slot);
            if (handle.socket->recv (reinterpret_cast<zlink::msg_t *> (&slot), flags_) != 0) {
                const int saved_errno = errno;
                close_buffered_recv_parts (&buffered_parts);
                errno = saved_errno;
                return zlink::recv_result_internal::from_errno (errno);
            }

            payload_has_more =
              (reinterpret_cast<const zlink::msg_t *> (&slot)->flags () & zlink::msg_t::more) != 0;
        }

        if (!helper_state)
            helper_state = zlink::part_helper_internal::find_or_create_socket_state (
              handle.socket);
        if (!helper_state) {
            const int saved_errno = errno;
            close_buffered_recv_parts (&buffered_parts);
            errno = saved_errno;
            return zlink::recv_result_internal::from_errno (errno);
        }

        bool first_part = false;
        zlink::socket_base_t *source_socket = NULL;
        if (zlink::part_helper_internal::prepare_recv_step (
              zlink::part_helper_internal::recv_family_subscribe, handle.socket,
              helper_state, &first_part, &source_socket)
            != 0) {
            const int saved_errno = errno;
            close_buffered_recv_parts (&buffered_parts);
            errno = saved_errno;
            return zlink::recv_result_internal::from_errno (errno);
        }

        if (!first_part) {
            close_buffered_recv_parts (&buffered_parts);
            zlink::part_helper_internal::abort_recv_step (helper_state);
            errno = EBUSY;
            return zlink::recv_result_internal::from_errno (errno);
        }

        int buffer_rc = 0;
        {
            std::lock_guard<std::mutex> lock (helper_state->mutex);
            helper_state->recv.topic_id.swap (topic_id);
            buffer_rc = zlink::part_helper_internal::buffer_recv_parts (
              &helper_state->recv, buffered_parts.data (), buffered_parts.size ());
        }
        close_buffered_recv_parts (&buffered_parts);
        if (buffer_rc != 0) {
            const int saved_errno = errno;
            zlink::part_helper_internal::abort_recv_step (helper_state);
            errno = saved_errno;
            return zlink::recv_result_internal::from_errno (errno);
        }
    }

    int copy_errno = 0;
    {
        std::lock_guard<std::mutex> lock (helper_state->mutex);
        *topic_id_len_out_ = helper_state->recv.topic_id.size ();
        *part_count_out_ = helper_state->recv.buffered_parts.size ();
        if (topic_id_capacity_ < helper_state->recv.topic_id.size ()
            || parts_capacity_ < helper_state->recv.buffered_parts.size ())
            copy_errno = ENOBUFS;
        else if (!helper_state->recv.topic_id.empty ())
            memcpy (topic_id_buf_, helper_state->recv.topic_id.data (),
                    helper_state->recv.topic_id.size ());
    }
    if (copy_errno != 0) {
        errno = copy_errno;
        return zlink::recv_result_internal::from_errno (errno);
    }

    zlink::part_helper_internal::recv_record_metadata_t metadata;
    const zlink::part_helper_internal::staged_recv_record_result_t rc =
      zlink::part_helper_internal::try_take_staged_recv_record (
        helper_state, zlink::part_helper_internal::recv_family_subscribe,
        part_out_, parts_capacity_, part_count_out_, &metadata);
    if (rc != zlink::part_helper_internal::staged_recv_record_taken)
        return zlink::recv_result_internal::from_errno (errno);
    for (size_t i = 0; i < *part_count_out_; ++i)
        zlink::request_reply::clear_request_reply_metadata (&part_out_[i]);
    if (source_rid_out_)
        *source_rid_out_ = NULL;
    errno = 0;
    return ZLINK_RECV_OK;
}

zlink_recv_result_t zlink_subscribe (
  void *sub_, const zlink_routing_id_t **source_rid_out_,
  char *topic_id_buf_, size_t topic_id_capacity_, size_t *topic_id_len_out_,
  zlink_msg_t *parts_out_, size_t parts_capacity_, size_t *part_count_out_,
  zlink_recv_flags_t flags_)
{
    return subscribe_recv (
      sub_, source_rid_out_, topic_id_buf_, topic_id_capacity_,
      topic_id_len_out_, parts_out_, flags_, parts_capacity_,
      part_count_out_);
}
