/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_CORE_C_API_COPY_INTERNAL_HPP_INCLUDED__
#define __ZLINK_CORE_C_API_COPY_INTERNAL_HPP_INCLUDED__

#include <zlink.h>

#include <errno.h>
#include <string.h>

namespace zlink
{

inline void copy_routing_id_from_bytes (const void *data_, size_t size_, zlink_routing_id_t *out_)
{
    if (!out_)
        return;

    const size_t copy_size = size_ > sizeof (out_->data) ? sizeof (out_->data) : size_;
    out_->size = static_cast<uint8_t> (copy_size);
    if (copy_size > 0 && data_)
        memcpy (out_->data, data_, copy_size);
}

inline void copy_routing_id_from_msg (const zlink_msg_t &msg_, zlink_routing_id_t *out_)
{
    copy_routing_id_from_bytes (zlink_msg_data (&const_cast<zlink_msg_t &> (msg_)),
                                zlink_msg_size (&msg_), out_);
}

inline void
copy_fixed_c_string_from_bytes (char *dst_, size_t dst_size_, const void *src_, size_t src_size_)
{
    if (!dst_ || dst_size_ == 0)
        return;

    const size_t copy_size = src_size_ >= dst_size_ ? dst_size_ - 1 : src_size_;
    if (copy_size > 0 && src_)
        memcpy (dst_, src_, copy_size);
    dst_[copy_size] = '\0';
}

inline void copy_fixed_c_string_from_cstr (char *dst_, size_t dst_size_, const char *src_)
{
    const size_t src_size = src_ ? strlen (src_) : 0;
    copy_fixed_c_string_from_bytes (dst_, dst_size_, src_, src_size);
}

inline int
copy_bytes_to_sized_output (const void *src_, size_t src_size_, char *dst_, size_t *dst_size_inout_)
{
    if (!dst_size_inout_) {
        errno = EFAULT;
        return -1;
    }

    const size_t capacity = *dst_size_inout_;
    *dst_size_inout_ = src_size_;

    if (!dst_) {
        errno = 0;
        return 0;
    }
    if (capacity < src_size_) {
        errno = EMSGSIZE;
        return -1;
    }
    if (src_size_ > 0 && !src_) {
        errno = EFAULT;
        return -1;
    }
    if (src_size_ > 0)
        memcpy (dst_, src_, src_size_);

    errno = 0;
    return 0;
}

}

#endif
