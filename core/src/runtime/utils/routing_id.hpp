/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_UTILS_ROUTING_ID_HPP_INCLUDED__
#define __ZLINK_UTILS_ROUTING_ID_HPP_INCLUDED__

#include "zlink.h"
#include "utils/random.hpp"

#include <string>
#include <string.h>

namespace zlink
{
static const size_t random_uuid_routing_id_size = 16;

inline void generate_random_uuid_routing_id (zlink_routing_id_t *out_)
{
    if (!out_)
        return;

    memset (out_, 0, sizeof (*out_));
    out_->size = static_cast<uint8_t> (random_uuid_routing_id_size);
    generate_random_bytes (out_->data, random_uuid_routing_id_size);

    // RFC 4122 version 4 UUID bits. The value remains a binary routing id.
    out_->data[6] = static_cast<uint8_t> ((out_->data[6] & 0x0fu) | 0x40u);
    out_->data[8] = static_cast<uint8_t> ((out_->data[8] & 0x3fu) | 0x80u);

    bool all_zero = true;
    for (size_t i = 0; i < random_uuid_routing_id_size; ++i) {
        if (out_->data[i] != 0) {
            all_zero = false;
            break;
        }
    }
    if (all_zero)
        out_->data[random_uuid_routing_id_size - 1] = 1;
}

inline bool valid_routing_id (const zlink_routing_id_t *rid_)
{
    return rid_ && rid_->size > 0 && rid_->size <= sizeof (rid_->data);
}

inline bool valid_routing_id (const zlink_routing_id_t &rid_)
{
    return valid_routing_id (&rid_);
}

inline std::string routing_id_key (const zlink_routing_id_t *rid_)
{
    if (!valid_routing_id (rid_))
        return std::string ();
    return std::string (reinterpret_cast<const char *> (rid_->data), rid_->size);
}

inline std::string routing_id_key (const zlink_routing_id_t &rid_)
{
    return routing_id_key (&rid_);
}

inline bool routing_id_from_key (const std::string &value_, zlink_routing_id_t *out_)
{
    if (!out_ || value_.empty () || value_.size () > sizeof (out_->data))
        return false;
    memset (out_, 0, sizeof (*out_));
    out_->size = static_cast<uint8_t> (value_.size ());
    memcpy (out_->data, value_.data (), value_.size ());
    return true;
}

inline void optional_routing_id_from_key (const std::string &value_, zlink_routing_id_t *out_)
{
    if (!out_)
        return;
    memset (out_, 0, sizeof (*out_));
    if (value_.empty ())
        return;
    const size_t size = value_.size () > sizeof (out_->data) ? sizeof (out_->data) : value_.size ();
    memcpy (out_->data, value_.data (), size);
    out_->size = static_cast<uint8_t> (size);
}
}

#endif
