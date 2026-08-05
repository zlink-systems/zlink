#ifndef BENCH_MULTI_STREAM_FRAME_HPP
#define BENCH_MULTI_STREAM_FRAME_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

static const size_t FRAME_PREFIX_SIZE = 4;
static const size_t MAX_STREAM_FRAME_SIZE = 16 * 1024 * 1024;

struct stream_buffer_t
{
    std::vector<char> data;
    size_t offset;

    stream_buffer_t () : offset (0) {}

    size_t available () const { return data.size () - offset; }

    void append (const char *buf, size_t len)
    {
        if (len == 0)
            return;
        data.insert (data.end (), buf, buf + len);
    }

    void compact ()
    {
        if (offset == 0)
            return;
        if (offset >= data.size ()) {
            data.clear ();
            offset = 0;
            return;
        }
        if (offset > 4096) {
            data.erase (data.begin (), data.begin () + offset);
            offset = 0;
        }
    }

    void reset ()
    {
        data.clear ();
        offset = 0;
    }
};

inline int stream_decode_available_frames (stream_buffer_t &stash, int max_frames)
{
    if (max_frames <= 0)
        return 0;

    int decoded = 0;
    while (decoded < max_frames) {
        if (stash.available () < FRAME_PREFIX_SIZE)
            break;

        uint32_t net_len = 0;
        std::memcpy (&net_len, &stash.data[stash.offset], FRAME_PREFIX_SIZE);
        const size_t frame_len = static_cast<size_t> (ntohl (net_len));
        if (frame_len > MAX_STREAM_FRAME_SIZE) {
            stash.reset ();
            break;
        }

        const size_t required = FRAME_PREFIX_SIZE + frame_len;
        if (stash.available () < required)
            break;

        stash.offset += required;
        ++decoded;
    }

    stash.compact ();
    return decoded;
}

inline bool stream_decode_one_frame (stream_buffer_t &stash, std::vector<char> *payload_out)
{
    if (!payload_out)
        return false;
    if (stash.available () < FRAME_PREFIX_SIZE)
        return false;

    uint32_t net_len = 0;
    std::memcpy (&net_len, &stash.data[stash.offset], FRAME_PREFIX_SIZE);
    const size_t frame_len = static_cast<size_t> (ntohl (net_len));
    if (frame_len > MAX_STREAM_FRAME_SIZE) {
        stash.reset ();
        return false;
    }

    const size_t required = FRAME_PREFIX_SIZE + frame_len;
    if (stash.available () < required)
        return false;

    payload_out->assign (frame_len, 0);
    if (frame_len > 0) {
        std::memcpy (&(*payload_out)[0], &stash.data[stash.offset + FRAME_PREFIX_SIZE], frame_len);
    }

    stash.offset += required;
    stash.compact ();
    return true;
}

inline void stream_build_framed_payload (const std::vector<char> &payload,
                                         std::vector<char> *framed_out)
{
    if (!framed_out)
        return;

    const uint32_t net_len = htonl (static_cast<uint32_t> (payload.size ()));
    framed_out->assign (FRAME_PREFIX_SIZE + payload.size (), 0);
    std::memcpy (&(*framed_out)[0], &net_len, FRAME_PREFIX_SIZE);
    if (!payload.empty ()) {
        std::memcpy (&(*framed_out)[FRAME_PREFIX_SIZE], &payload[0], payload.size ());
    }
}

#endif
