/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_ROUTER_DEBUG_HPP_INCLUDED__
#define __ZLINK_ROUTER_DEBUG_HPP_INCLUDED__

#include "utils/blob.hpp"

#include <zlink.h>

#include <cstdio>

namespace zlink
{
namespace router_debug
{
extern const bool enabled_flag;

inline bool enabled ()
{
    return enabled_flag;
}

inline void format_routing_id (const unsigned char *data_, size_t size_,
                               char *buf_, size_t buf_size_)
{
    if (!buf_ || buf_size_ == 0)
        return;

    if (!data_ || size_ == 0) {
        std::snprintf (buf_, buf_size_, "<empty>");
        return;
    }

    size_t used = 0;
    for (size_t i = 0; i < size_ && used + 4 < buf_size_; ++i) {
        const unsigned char c = data_[i];
        const int rc = std::snprintf (
          buf_ + used, buf_size_ - used, "%c%02X",
          (c >= 32 && c <= 126) ? static_cast<char> (c) : '.',
          static_cast<unsigned> (c));
        if (rc <= 0)
            break;
        used += static_cast<size_t> (rc);
        if (i + 1 < size_ && used + 2 < buf_size_) {
            buf_[used++] = ' ';
            buf_[used] = '\0';
        }
    }
}

inline void format_routing_id (const zlink_routing_id_t *routing_id_,
                               char *buf_, size_t buf_size_)
{
    format_routing_id (routing_id_ ? routing_id_->data : NULL,
                       routing_id_ ? routing_id_->size : 0, buf_, buf_size_);
}

inline void format_routing_id (const blob_t &routing_id_, char *buf_,
                               size_t buf_size_)
{
    format_routing_id (routing_id_.data (), routing_id_.size (), buf_,
                       buf_size_);
}
}
}

#endif
