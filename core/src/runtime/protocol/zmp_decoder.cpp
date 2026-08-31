/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"
#include "protocol/zmp_decoder.hpp"
#include "core/msg.hpp"
#include "protocol/wire.hpp"
#include "utils/err.hpp"

namespace
{
static uint32_t compute_effective_max (int64_t maxmsgsize_)
{
    uint64_t limit = zlink::zmp_max_body_size;
    if (maxmsgsize_ > 0 && static_cast<uint64_t> (maxmsgsize_) < limit)
        limit = static_cast<uint64_t> (maxmsgsize_);
    return static_cast<uint32_t> (limit);
}

static uint64_t compute_application_message_max (int64_t maxmsgsize_)
{
    return maxmsgsize_ > 0 ? static_cast<uint64_t> (maxmsgsize_)
                           : UINT64_MAX;
}

static const unsigned char zmp_flag_sub_or_cancel =
  zlink::zmp_flag_subscribe | zlink::zmp_flag_cancel;
}

zlink::zmp_decoder_t::zmp_decoder_t (size_t bufsize_, int64_t maxmsgsize_) :
    decoder_base_t<zmp_decoder_t, shared_message_memory_allocator> (bufsize_),
    _msg_flags (0),
    _wire_flags (0),
    _wire_kind (zmp_kind_data),
    _request_sequence (0),
    _error_code (0),
    _max_msg_size_effective (compute_effective_max (maxmsgsize_)),
    _max_application_message_size (
      compute_application_message_max (maxmsgsize_)),
    _frame_admission_handler (NULL),
    _frame_reservation_release_handler (NULL),
    _frame_admission_subject (NULL),
    _frame_reservation (NULL),
    _pending_msg_size (0),
    _pending_read_from (NULL),
    _allocation_backpressured (false),
    _application_multipart_in_progress (false),
    _next_application_multipart_in_progress (false),
    _application_multipart_payload_size (0),
    _next_application_multipart_payload_size (0),
    _frame_stage (reading_base_header)
{
    int rc = _in_progress.init ();
    errno_assert (rc == 0);

    next_step (_tmpbuf, zmp_header_size, &zmp_decoder_t::header_ready);
}

zlink::zmp_decoder_t::~zmp_decoder_t ()
{
    release_frame_reservation ();
    const int rc = _in_progress.close ();
    errno_assert (rc == 0);
}

void zlink::zmp_decoder_t::set_frame_admission_handler (
  frame_admission_handler_t handler_,
  frame_reservation_release_handler_t release_handler_, void *subject_)
{
    zlink_assert (!_frame_reservation);
    _frame_admission_handler = handler_;
    _frame_reservation_release_handler = release_handler_;
    _frame_admission_subject = subject_;
}

bool zlink::zmp_decoder_t::allocation_backpressured () const
{
    return _allocation_backpressured;
}

int zlink::zmp_decoder_t::retry_frame_admission ()
{
    if (!_allocation_backpressured
        || _frame_stage != waiting_frame_admission) {
        errno = EINVAL;
        return -1;
    }
    return size_ready (_pending_msg_size, _pending_read_from);
}

void **zlink::zmp_decoder_t::frame_reservation_slot ()
{
    return &_frame_reservation;
}

void zlink::zmp_decoder_t::discard_frame_reservation ()
{
    release_frame_reservation ();
}

void zlink::zmp_decoder_t::release_frame_reservation ()
{
    if (!_frame_reservation)
        return;
    if (_frame_reservation_release_handler)
        _frame_reservation_release_handler (_frame_admission_subject,
                                            _frame_reservation);
    _frame_reservation = NULL;
}

int zlink::zmp_decoder_t::header_ready (unsigned char const *read_from_)
{
    _error_code = 0;

    if (_tmpbuf[0] != zmp_magic)
        return fail_protocol (zmp_error_invalid_magic);

    if (_tmpbuf[1] != zmp_version)
        return fail_protocol (zmp_error_version_mismatch);

    _wire_kind = _tmpbuf[3];
    if (_wire_kind != zmp_kind_data
        && !zmp_is_request_reply_kind (_wire_kind))
        return fail_protocol (zmp_error_kind_invalid);

    _wire_flags = _tmpbuf[2];
    if (_wire_flags & ~zmp_flag_mask)
        return fail_protocol (zmp_error_flags_invalid);

    if ((_wire_flags & zmp_flag_control)
        && (_wire_flags & zmp_flag_identity))
        return fail_protocol (zmp_error_flags_invalid);

    if ((_wire_flags & zmp_flag_control)
        && (_wire_flags & zmp_flag_more))
        return fail_protocol (zmp_error_flags_invalid);

    if (_wire_flags & zmp_flag_sub_or_cancel) {
        if ((_wire_flags & zmp_flag_sub_or_cancel)
            == zmp_flag_sub_or_cancel)
            return fail_protocol (zmp_error_flags_invalid);
        if (_wire_flags & ~(zmp_flag_sub_or_cancel))
            return fail_protocol (zmp_error_flags_invalid);
    }

    _msg_flags = 0;
    if (_wire_flags & zmp_flag_more)
        _msg_flags |= msg_t::more;
    if (_wire_flags & zmp_flag_control)
        _msg_flags |= msg_t::command;
    if (_wire_flags & zmp_flag_identity)
        _msg_flags |= msg_t::routing_id;
    if (_wire_flags & zmp_flag_subscribe)
        _msg_flags |= msg_t::subscribe;
    else if (_wire_flags & zmp_flag_cancel)
        _msg_flags |= msg_t::cancel;

    _request_sequence = 0;
    if (zmp_is_request_reply_kind (_wire_kind)) {
        _frame_stage = reading_sequence_extension;
        next_step (_tmpbuf + zmp_header_size,
                   zmp_request_sequence_size,
                   &zmp_decoder_t::sequence_ready);
        return 0;
    }

    return validate_header_and_admit (read_from_);
}

int zlink::zmp_decoder_t::sequence_ready (
  unsigned char const *read_from_)
{
    _request_sequence = get_uint64 (_tmpbuf + zmp_header_size);
    if (_request_sequence == 0)
        return fail_protocol (zmp_error_sequence_invalid);

    return validate_header_and_admit (read_from_);
}

int zlink::zmp_decoder_t::validate_header_and_admit (
  unsigned char const *read_from_)
{
    const bool special_frame = zmp_is_special_frame (_wire_flags);

    if (zmp_is_request_reply_kind (_wire_kind) && special_frame)
        return fail_protocol (zmp_error_flags_invalid);

    if (_application_multipart_in_progress
        && (special_frame || zmp_is_request_reply_kind (_wire_kind)))
        return fail_protocol (zmp_error_multipart_invalid);

    _next_application_multipart_in_progress =
      special_frame ? _application_multipart_in_progress
                    : (_wire_flags & zmp_flag_more) != 0;
    _frame_stage = waiting_frame_admission;

    return size_ready (get_uint32 (_tmpbuf + 4), read_from_);
}

int zlink::zmp_decoder_t::size_ready (uint32_t msg_size_, unsigned char const *read_from_)
{
    const uint32_t frame_limit =
      (_msg_flags & msg_t::command) != 0
        ? zmp_max_control_body_size
        : _max_msg_size_effective;
    if (unlikely (msg_size_ > frame_limit)) {
        _error_code = zmp_error_body_too_large;
        errno = EMSGSIZE;
        return -1;
    }

    _next_application_multipart_payload_size =
      _application_multipart_payload_size;
    if (!zmp_is_special_frame (_wire_flags)) {
        if (unlikely (
              msg_size_
              > UINT64_MAX - _application_multipart_payload_size)) {
            _error_code = zmp_error_body_too_large;
            errno = EMSGSIZE;
            return -1;
        }
        const uint64_t candidate_payload_size =
          _application_multipart_payload_size + msg_size_;
        if (unlikely (candidate_payload_size
                      > _max_application_message_size)) {
            _error_code = zmp_error_body_too_large;
            errno = EMSGSIZE;
            return -1;
        }
        _next_application_multipart_payload_size =
          (_wire_flags & zmp_flag_more) != 0 ? candidate_payload_size : 0;
    }

    if (unlikely (msg_size_ != static_cast<size_t> (msg_size_))) {
        _error_code = zmp_error_body_too_large;
        errno = EMSGSIZE;
        return -1;
    }

    if ((_msg_flags & msg_t::command) == 0 && _frame_admission_handler
        && _frame_admission_handler (_frame_admission_subject, msg_size_,
                                     _msg_flags, &_frame_reservation)
             != 0) {
        zlink_assert (!_frame_reservation);
        if (errno == EAGAIN) {
            _pending_msg_size = msg_size_;
            _pending_read_from = read_from_;
            _allocation_backpressured = true;
        } else {
            _pending_msg_size = 0;
            _pending_read_from = NULL;
            _allocation_backpressured = false;
            if (errno == EMSGSIZE)
                _error_code = zmp_error_body_too_large;
        }
        return -1;
    }

    _allocation_backpressured = false;
    _pending_msg_size = 0;
    _pending_read_from = NULL;

    int rc = _in_progress.close ();
    errno_assert (rc == 0);

    shared_message_memory_allocator &allocator = get_allocator ();
    const unsigned char *allocator_data = allocator.data ();
    const size_t allocator_size = allocator.size ();
    const uintptr_t base = reinterpret_cast<uintptr_t> (allocator_data);
    const uintptr_t end = base + allocator_size;
    const uintptr_t ptr = reinterpret_cast<uintptr_t> (read_from_);
    const bool in_allocator = ptr >= base && ptr <= end;
    const size_t available =
      in_allocator ? static_cast<size_t> (allocator_data + allocator_size - read_from_) : 0;

    if (unlikely (!in_allocator || msg_size_ > available)) {
        rc = _in_progress.init_size (static_cast<size_t> (msg_size_));
    } else {
        rc = _in_progress.init (const_cast<unsigned char *> (read_from_),
                                static_cast<size_t> (msg_size_),
                                shared_message_memory_allocator::call_dec_ref, allocator.buffer (),
                                allocator.provide_content ());
        if (_in_progress.is_zcmsg ()) {
            allocator.advance_content ();
            allocator.inc_ref ();
        }
    }

    if (unlikely (rc)) {
        errno_assert (errno == ENOMEM);
        release_frame_reservation ();
        rc = _in_progress.init ();
        errno_assert (rc == 0);
        errno = ENOMEM;
        return -1;
    }

    _in_progress.set_flags (_msg_flags);
    if (zmp_is_request_reply_kind (_wire_kind)) {
        rc = _in_progress.set_request_reply_metadata (_wire_kind,
                                                      _request_sequence);
        if (unlikely (rc != 0)) {
            release_frame_reservation ();
            rc = _in_progress.close ();
            errno_assert (rc == 0);
            rc = _in_progress.init ();
            errno_assert (rc == 0);
            return fail_protocol (zmp_error_internal);
        }
    }

    if (_in_progress.size () == 0) {
        complete_frame ();
        return 1;
    }
    _frame_stage = reading_payload;
    next_step (_in_progress.data (), _in_progress.size (), &zmp_decoder_t::body_ready);

    return 0;
}

int zlink::zmp_decoder_t::body_ready (unsigned char const *)
{
    complete_frame ();
    return 1;
}

void zlink::zmp_decoder_t::complete_frame ()
{
    _application_multipart_in_progress =
      _next_application_multipart_in_progress;
    _application_multipart_payload_size =
      _next_application_multipart_payload_size;
    _frame_stage = reading_base_header;
    next_step (_tmpbuf, zmp_header_size, &zmp_decoder_t::header_ready);
}

int zlink::zmp_decoder_t::fail_protocol (uint8_t error_code_)
{
    release_frame_reservation ();
    if (_in_progress.check ()) {
        int rc = _in_progress.close ();
        errno_assert (rc == 0);
        rc = _in_progress.init ();
        errno_assert (rc == 0);
    }
    _allocation_backpressured = false;
    _pending_msg_size = 0;
    _pending_read_from = NULL;
    _error_code = error_code_;
    errno = EPROTO;
    return -1;
}

int zlink::zmp_decoder_t::stream_end ()
{
    if (_frame_stage == reading_base_header
        && bytes_left_in_step () == zmp_header_size
        && !_application_multipart_in_progress)
        return 0;
    return fail_protocol (zmp_error_frame_incomplete);
}
