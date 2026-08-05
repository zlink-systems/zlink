/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_ZMP_METADATA_HPP_INCLUDED__
#define __ZLINK_ZMP_METADATA_HPP_INCLUDED__

#include <limits.h>
#include <map>
#include <string.h>
#include <string>
#include <vector>

#include "utils/err.hpp"
#include "protocol/metadata.hpp"
#include "core/internal_defs.hpp"
#include "core/options.hpp"
#include "protocol/wire.hpp"

namespace zlink
{
namespace zmp_metadata
{
typedef std::map<std::string, std::string> properties_t;

static inline const char *socket_type_string (int socket_type_)
{
    switch (socket_type_) {
        case ZLINK_CORE_SOCKET_PAIR:
            return "PAIR";
        case ZLINK_CORE_SOCKET_PUB:
            return "PUB";
        case ZLINK_CORE_SOCKET_SUB:
            return "SUB";
        case ZLINK_CORE_SOCKET_DEALER:
            return "DEALER";
        case ZLINK_CORE_SOCKET_ROUTER:
            return "ROUTER";
        case ZLINK_CORE_SOCKET_XPUB:
            return "XPUB";
        case ZLINK_CORE_SOCKET_XSUB:
            return "XSUB";
        case ZLINK_CORE_SOCKET_STREAM:
            return "STREAM";
        default:
            zlink_assert (false);
            return NULL;
    }
}

static inline void append_u32 (std::vector<unsigned char> &buf_, uint32_t value_)
{
    const size_t offset = buf_.size ();
    buf_.resize (offset + 4);
    put_uint32 (&buf_[offset], value_);
}

static inline void append_property (std::vector<unsigned char> &buf_,
                                    const char *name_,
                                    const void *value_,
                                    size_t value_len_)
{
    const size_t name_len = strlen (name_);
    zlink_assert (name_len <= UCHAR_MAX);
    buf_.push_back (static_cast<unsigned char> (name_len));
    buf_.insert (buf_.end (), name_, name_ + name_len);
    zlink_assert (value_len_ <= 0x7FFFFFFF);
    append_u32 (buf_, static_cast<uint32_t> (value_len_));
    if (value_len_ > 0) {
        const unsigned char *src = static_cast<const unsigned char *> (value_);
        buf_.insert (buf_.end (), src, src + value_len_);
    }
}

static inline void add_basic_properties (const options_t &options_,
                                         std::vector<unsigned char> &buf_)
{
    const char *socket_type = socket_type_string (options_.type);
    append_property (buf_, "Socket-Type", socket_type, strlen (socket_type));

    if (options_.type == ZLINK_CORE_SOCKET_DEALER || options_.type == ZLINK_CORE_SOCKET_ROUTER) {
        append_property (buf_, "Routing-Id", options_.routing_id, options_.routing_id_size);
    }

    unsigned char max_message_size[8];
    const uint64_t finite_max_message_size =
      options_.maxmsgsize > 0 ? static_cast<uint64_t> (options_.maxmsgsize) : 0;
    put_uint64 (max_message_size, finite_max_message_size);
    append_property (buf_, "Zlink-Max-Message-Size", max_message_size,
                     sizeof (max_message_size));

    if (options_.transport_pair_id != 0) {
        unsigned char pair_id[8];
        unsigned char generation[8];
        put_uint64 (pair_id, options_.transport_pair_id);
        put_uint64 (generation, options_.transport_pair_generation);
        const unsigned char lane =
          options_.transport_lane == transport_lane_completion ? 1u : 0u;
        append_property (buf_, "Zlink-Pair-Id", pair_id, sizeof (pair_id));
        append_property (buf_, "Zlink-Pair-Generation", generation, sizeof (generation));
        append_property (buf_, "Zlink-Lane", &lane, sizeof (lane));
    }
}

static inline int parse_max_message_size (const properties_t &properties_,
                                          uint64_t *max_message_size_out_)
{
    if (max_message_size_out_)
        *max_message_size_out_ = 0;
    const properties_t::const_iterator it =
      properties_.find ("Zlink-Max-Message-Size");
    if (it == properties_.end ())
        return 0;
    if (it->second.size () != 8) {
        errno = EPROTO;
        return -1;
    }
    if (max_message_size_out_)
        *max_message_size_out_ = get_uint64 (
          reinterpret_cast<const unsigned char *> (it->second.data ()));
    return 1;
}

static inline int parse_transport_pair (const properties_t &properties_,
                                        transport_lane_t *lane_out_,
                                        uint64_t *pair_id_out_,
                                        uint64_t *generation_out_)
{
    const properties_t::const_iterator pair_it = properties_.find ("Zlink-Pair-Id");
    const properties_t::const_iterator generation_it =
      properties_.find ("Zlink-Pair-Generation");
    const properties_t::const_iterator lane_it = properties_.find ("Zlink-Lane");
    const bool any = pair_it != properties_.end ()
                     || generation_it != properties_.end ()
                     || lane_it != properties_.end ();
    if (!any)
        return 0;
    if (pair_it == properties_.end () || pair_it->second.size () != 8
        || generation_it == properties_.end () || generation_it->second.size () != 8
        || lane_it == properties_.end () || lane_it->second.size () != 1) {
        errno = EPROTO;
        return -1;
    }
    const unsigned char lane =
      static_cast<unsigned char> (lane_it->second[0]);
    if (lane > 1) {
        errno = EPROTO;
        return -1;
    }
    if (lane_out_)
        *lane_out_ =
          lane == 1 ? transport_lane_completion : transport_lane_application;
    if (pair_id_out_)
        *pair_id_out_ = get_uint64 (
          reinterpret_cast<const unsigned char *> (pair_it->second.data ()));
    if (generation_out_)
        *generation_out_ = get_uint64 (
          reinterpret_cast<const unsigned char *> (generation_it->second.data ()));
    if ((pair_id_out_ && *pair_id_out_ == 0)
        || (generation_out_ && *generation_out_ == 0)) {
        errno = EPROTO;
        return -1;
    }
    return 1;
}

static inline int parse (const unsigned char *ptr_, size_t length_, properties_t &out_)
{
    size_t bytes_left = length_;
    while (bytes_left > 1) {
        const size_t name_length = static_cast<size_t> (*ptr_);
        ptr_ += 1;
        bytes_left -= 1;
        if (bytes_left < name_length)
            break;

        const std::string name (reinterpret_cast<const char *> (ptr_), name_length);
        ptr_ += name_length;
        bytes_left -= name_length;
        if (bytes_left < 4)
            break;

        const size_t value_length = static_cast<size_t> (get_uint32 (ptr_));
        ptr_ += 4;
        bytes_left -= 4;
        if (bytes_left < value_length)
            break;

        const char *value = reinterpret_cast<const char *> (ptr_);
        ptr_ += value_length;
        bytes_left -= value_length;

        out_[name] = std::string (value, value_length);
    }

    if (bytes_left > 0) {
        errno = EPROTO;
        return -1;
    }
    return 0;
}
}
}

#endif
