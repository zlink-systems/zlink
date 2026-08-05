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
    if (maxmsgsize_ >= 0 && static_cast<uint64_t> (maxmsgsize_) < limit)
        limit = static_cast<uint64_t> (maxmsgsize_);
    return static_cast<uint32_t> (limit);
}

static const unsigned char zmp_flag_sub_or_cancel =
  zlink::zmp_flag_subscribe | zlink::zmp_flag_cancel;
}

zlink::zmp_decoder_t::zmp_decoder_t (size_t bufsize_, int64_t maxmsgsize_) :
    decoder_base_t<zmp_decoder_t, shared_message_memory_allocator> (bufsize_),
    _msg_flags (0),
    _error_code (0),
    _max_msg_size_effective (compute_effective_max (maxmsgsize_))
{
    int rc = _in_progress.init ();
    errno_assert (rc == 0);

    next_step (_tmpbuf, zmp_header_size, &zmp_decoder_t::header_ready);
}

zlink::zmp_decoder_t::~zmp_decoder_t ()
{
    const int rc = _in_progress.close ();
    errno_assert (rc == 0);
}

int zlink::zmp_decoder_t::header_ready (unsigned char const *read_from_)
{
    _error_code = 0;

    if (_tmpbuf[0] != zmp_magic) {
        _error_code = zmp_error_invalid_magic;
        errno = EPROTO;
        return -1;
    }

    if (_tmpbuf[1] != zmp_version) {
        _error_code = zmp_error_version_mismatch;
        errno = EPROTO;
        return -1;
    }

    if (_tmpbuf[3] != 0) {
        _error_code = zmp_error_flags_invalid;
        errno = EPROTO;
        return -1;
    }

    const unsigned char flags = _tmpbuf[2];
    if (flags & ~zmp_flag_mask) {
        _error_code = zmp_error_flags_invalid;
        errno = EPROTO;
        return -1;
    }

    if ((flags & zmp_flag_control) && (flags & zmp_flag_identity)) {
        _error_code = zmp_error_flags_invalid;
        errno = EPROTO;
        return -1;
    }

    if ((flags & zmp_flag_control) && (flags & zmp_flag_more)) {
        _error_code = zmp_error_flags_invalid;
        errno = EPROTO;
        return -1;
    }

    if (flags & zmp_flag_sub_or_cancel) {
        if ((flags & zmp_flag_sub_or_cancel) == zmp_flag_sub_or_cancel) {
            _error_code = zmp_error_flags_invalid;
            errno = EPROTO;
            return -1;
        }
        if (flags & ~(zmp_flag_sub_or_cancel)) {
            _error_code = zmp_error_flags_invalid;
            errno = EPROTO;
            return -1;
        }
    }

    _msg_flags = 0;
    if (flags & zmp_flag_more)
        _msg_flags |= msg_t::more;
    if (flags & zmp_flag_control)
        _msg_flags |= msg_t::command;
    if (flags & zmp_flag_identity)
        _msg_flags |= msg_t::routing_id;
    if (flags & zmp_flag_subscribe)
        _msg_flags |= msg_t::subscribe;
    else if (flags & zmp_flag_cancel)
        _msg_flags |= msg_t::cancel;

    const uint32_t msg_size = get_uint32 (_tmpbuf + 4);

    return size_ready (msg_size, read_from_);
}

int zlink::zmp_decoder_t::size_ready (uint32_t msg_size_, unsigned char const *read_from_)
{
    if (unlikely (msg_size_ > _max_msg_size_effective)) {
        _error_code = zmp_error_body_too_large;
        errno = EMSGSIZE;
        return -1;
    }

    if (unlikely (msg_size_ != static_cast<size_t> (msg_size_))) {
        _error_code = zmp_error_body_too_large;
        errno = EMSGSIZE;
        return -1;
    }

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
        rc = _in_progress.init ();
        errno_assert (rc == 0);
        errno = ENOMEM;
        return -1;
    }

    _in_progress.set_flags (_msg_flags);
    next_step (_in_progress.data (), _in_progress.size (), &zmp_decoder_t::body_ready);

    return 0;
}

int zlink::zmp_decoder_t::body_ready (unsigned char const *)
{
    next_step (_tmpbuf, zmp_header_size, &zmp_decoder_t::header_ready);
    return 1;
}
