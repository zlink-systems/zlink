/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"
#include "protocol/raw_decoder.hpp"
#include "utils/err.hpp"

#include <cstdint>
#include <cstring>

zlink::raw_decoder_t::raw_decoder_t (size_t bufsize_, int64_t maxmsgsize_) :
    _allocator (bufsize_, 1), _max_msg_size (maxmsgsize_)
{
    const int rc = _in_progress.init ();
    errno_assert (rc == 0);
}

zlink::raw_decoder_t::raw_decoder_t (size_t bufsize_,
                                     int64_t maxmsgsize_,
                                     size_t max_buffer_size_) :
    _allocator (bufsize_, 1, max_buffer_size_), _max_msg_size (maxmsgsize_)
{
    const int rc = _in_progress.init ();
    errno_assert (rc == 0);
}

zlink::raw_decoder_t::~raw_decoder_t ()
{
    const int rc = _in_progress.close ();
    errno_assert (rc == 0);
}

void zlink::raw_decoder_t::get_buffer (unsigned char **data_, size_t *size_)
{
    *data_ = _allocator.allocate ();
    *size_ = _allocator.size ();
}

int zlink::raw_decoder_t::decode (const unsigned char *data_, size_t size_, size_t &bytes_used_)
{
    if (_max_msg_size >= 0 && size_ > static_cast<size_t> (_max_msg_size)) {
        errno = EMSGSIZE;
        return -1;
    }

    // STREAM fast-path is zero-copy from decoder allocator buffer.
    // When decode input is not inside allocator memory (e.g. drained/pending
    // chunks), fallback to owned-copy message to keep hint/data coupling valid.
    const unsigned char *allocator_data = _allocator.data ();
    const size_t allocator_size = _allocator.size ();
    const uintptr_t base = reinterpret_cast<uintptr_t> (allocator_data);
    const uintptr_t end = base + allocator_size;
    const uintptr_t ptr = reinterpret_cast<uintptr_t> (data_);
    const bool in_allocator =
      allocator_data && ptr >= base && ptr <= end && size_ <= static_cast<size_t> (end - ptr);

    int rc = 0;
    if (!in_allocator) {
        rc = _in_progress.init_size (size_);
        if (rc != 0) {
            errno_assert (errno == ENOMEM);
            return -1;
        }
        if (size_ > 0) {
            std::memcpy (_in_progress.data (), const_cast<unsigned char *> (data_), size_);
        }
    } else {
        rc = _in_progress.init (const_cast<unsigned char *> (data_), size_,
                                shared_message_memory_allocator::call_dec_ref, _allocator.buffer (),
                                _allocator.provide_content ());
    }

    // If the message became zero-copy backed by allocator memory,
    // hand off the current content slot and release allocator ownership.
    if (_in_progress.is_zcmsg ()) {
        _allocator.advance_content ();
        _allocator.release ();
    }

    errno_assert (rc != -1);
    bytes_used_ = size_;
    return 1;
}
