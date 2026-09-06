/* SPDX-License-Identifier: MPL-2.0 */

#ifndef ZLINK_TESTUTIL_ZMP_WIRE_HPP_INCLUDED
#define ZLINK_TESTUTIL_ZMP_WIRE_HPP_INCLUDED

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

// Independent wire fixtures for core/doc/spec/core/protocol/01-zmp.ko.md
// sections 3 and 4. Production encoders/parsers must not generate their own
// integration-test inputs or interpret their own expected output.
namespace test_zmp_wire
{
const unsigned char zmp_magic = 0x5a;
const unsigned char zmp_version = 1;
const size_t zmp_header_size = 8;
const size_t zmp_request_sequence_size = 8;
const size_t zmp_request_reply_header_size = 16;
enum {
    zmp_kind_data = 0,
    zmp_kind_request = 1,
    zmp_kind_reply = 2,
    zmp_kind_error_reply = 3,
    zmp_flag_more = 0x01,
    zmp_flag_control = 0x02,
    zmp_flag_identity = 0x04,
    zmp_flag_subscribe = 0x08,
    zmp_flag_cancel = 0x10,
    zmp_control_hello = 1,
    zmp_control_ready = 4,
    zmp_control_error = 5,
    zmp_error_invalid_magic = 1,
    socket_pair = 0,
    socket_pub = 1,
    socket_sub = 2,
    socket_dealer = 5,
    socket_router = 6,
    socket_xpub = 9,
    socket_xsub = 10,
    socket_stream = 11
};

inline void put_uint64 (unsigned char *bytes_, uint64_t value_)
{
    for (size_t i = 8; i != 0; --i) {
        bytes_[i - 1] = static_cast<unsigned char> (value_ & 0xff);
        value_ >>= 8;
    }
}

inline uint64_t get_uint64 (const unsigned char *bytes_)
{
    uint64_t result = 0;
    for (size_t i = 0; i != 8; ++i)
        result = result * 256 + bytes_[i];
    return result;
}

inline void put_uint32 (unsigned char *bytes_, uint32_t value_)
{
    for (size_t i = 4; i != 0; --i) {
        bytes_[i - 1] = static_cast<unsigned char> (value_ & 0xff);
        value_ >>= 8;
    }
}

inline uint32_t get_uint32 (const unsigned char *bytes_)
{
    uint32_t result = 0;
    for (size_t i = 0; i != 4; ++i)
        result = result * 256 + bytes_[i];
    return result;
}

inline bool zmp_is_request_reply_kind (unsigned char kind_)
{
    return kind_ >= zmp_kind_request && kind_ <= zmp_kind_error_reply;
}

inline std::vector<unsigned char> make_zmp_wire_frame (
  unsigned char flags_, unsigned char kind_, uint64_t sequence_,
  const unsigned char *body_, size_t body_len_)
{
    const bool extended = test_zmp_wire::zmp_is_request_reply_kind (kind_);
    const size_t header_size =
      extended ? test_zmp_wire::zmp_request_reply_header_size
               : test_zmp_wire::zmp_header_size;
    std::vector<unsigned char> frame (header_size + body_len_);
    frame[0] = test_zmp_wire::zmp_magic;
    frame[1] = test_zmp_wire::zmp_version;
    frame[2] = flags_;
    frame[3] = kind_;
    test_zmp_wire::put_uint32 (&frame[4], static_cast<uint32_t> (body_len_));
    if (extended)
        test_zmp_wire::put_uint64 (&frame[test_zmp_wire::zmp_header_size], sequence_);
    if (body_len_ > 0)
        memcpy (&frame[header_size], body_, body_len_);
    return frame;
}

inline void append_wire_frame (std::vector<unsigned char> *stream_,
                        const std::vector<unsigned char> &frame_)
{
    stream_->insert (stream_->end (), frame_.begin (), frame_.end ());
}

namespace zmp_metadata
{
typedef std::map<std::string, std::string> properties_t;

inline void append_property (std::vector<unsigned char> &bytes_,
                             const char *name_, const void *value_,
                             size_t size_)
{
    const size_t offset = bytes_.size ();
    const size_t name_size = std::strlen (name_);
    bytes_.resize (offset + 1 + name_size + 4 + size_);
    bytes_[offset] = static_cast<unsigned char> (name_size);
    std::memcpy (&bytes_[offset + 1], name_, name_size);
    put_uint32 (&bytes_[offset + 1 + name_size], static_cast<uint32_t> (size_));
    if (size_ != 0)
        std::memcpy (&bytes_[offset + 5 + name_size], value_, size_);
}

inline int parse (const unsigned char *bytes_, size_t size_, properties_t &out_)
{
    out_.clear ();
    size_t cursor = 0;
    while (cursor < size_) {
        const size_t name_size = bytes_[cursor++];
        if (name_size > size_ - cursor || size_ - cursor - name_size < 4)
            break;
        const std::string name (reinterpret_cast<const char *> (bytes_ + cursor),
                                name_size);
        cursor += name_size;
        const size_t value_size = get_uint32 (bytes_ + cursor);
        cursor += 4;
        if (value_size > size_ - cursor)
            break;
        out_[name] = std::string (
          reinterpret_cast<const char *> (bytes_ + cursor), value_size);
        cursor += value_size;
        if (cursor == size_)
            return 0;
    }
    if (size_ == 0)
        return 0;
    errno = EPROTO;
    return -1;
}
}

inline std::vector<unsigned char> control_frame (
  const std::vector<unsigned char> &body_)
{
    std::vector<unsigned char> frame (8 + body_.size (), 0);
    frame[0] = 0x5a;
    frame[1] = 1;
    frame[2] = 2;
    put_uint32 (&frame[4], static_cast<uint32_t> (body_.size ()));
    if (!body_.empty ())
        std::memcpy (&frame[8], &body_[0], body_.size ());
    return frame;
}

inline std::vector<unsigned char> pair_hello_frame ()
{
    const unsigned char hello[] = {1, socket_pair, 0};
    return control_frame (std::vector<unsigned char> (hello, hello + sizeof (hello)));
}

inline std::vector<unsigned char> pair_ready_frame ()
{
    std::vector<unsigned char> body (1, zmp_control_ready);
    zmp_metadata::append_property (body, "Socket-Type", "PAIR", 4);
    const unsigned char unlimited[8] = {};
    zmp_metadata::append_property (body, "Zlink-Max-Message-Size", unlimited, 8);
    return control_frame (body);
}

namespace flow_state
{
const char frame_name[] = "FLOWSTATE";
const size_t frame_name_size = 9;
const uint8_t frame_protocol_version = 1;
const size_t frame_size = 19;
enum { receive_flow_running = 0, receive_flow_paused = 1 };
inline void put_uint64_be (unsigned char *bytes_, uint64_t value_)
{
    put_uint64 (bytes_, value_);
}
inline uint64_t get_uint64_be (const unsigned char *bytes_)
{
    return get_uint64 (bytes_);
}
}
}

#endif
