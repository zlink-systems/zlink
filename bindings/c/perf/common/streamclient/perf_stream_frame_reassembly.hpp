#ifndef PERF_STREAM_FRAME_REASSEMBLY_HPP
#define PERF_STREAM_FRAME_REASSEMBLY_HPP

#include "perf_stream_common.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace perf_stream_frame
{

struct buffer_t
{
    buffer_t () : bytes (), consumed (0) {}

    std::vector<unsigned char> bytes;
    size_t consumed;
};

struct frame_view_t
{
    frame_view_t () :
        data (NULL), size (0), header (NULL), header_size (0), payload (NULL), payload_size (0)
    {
    }

    const unsigned char *data;
    size_t size;
    const unsigned char *header;
    size_t header_size;
    const unsigned char *payload;
    size_t payload_size;
};

inline void reset (buffer_t *buffer)
{
    if (!buffer)
        return;

    buffer->bytes.clear ();
    buffer->consumed = 0;
}

inline void append (buffer_t *buffer, const unsigned char *data, size_t size)
{
    if (!buffer || !data || size == 0)
        return;

    const size_t old_size = buffer->bytes.size ();
    buffer->bytes.resize (old_size + size);
    std::memcpy (&buffer->bytes[old_size], data, size);
}

inline bool has_invalid_declared_size (const buffer_t *buffer)
{
    if (!buffer)
        return false;

    const size_t available = buffer->bytes.size () - buffer->consumed;
    if (available < perf_stream_common::k_stream_packet_prefix_size)
        return false;

    const unsigned char *frame = &buffer->bytes[buffer->consumed];
    const uint16_t header_size = perf_stream_common::perf_stream_load_u16_be (frame);
    const uint32_t payload_size = perf_stream_common::perf_stream_load_u32_be (frame + 2);
    const size_t max_size = perf_stream_common::k_stream_max_chunk_size;
    return static_cast<size_t> (header_size) > max_size
           || payload_size > static_cast<uint32_t> (max_size)
           || static_cast<size_t> (header_size) > max_size - payload_size;
}

inline bool try_peek (const buffer_t *buffer, frame_view_t *frame_out)
{
    if (!buffer || !frame_out)
        return false;

    frame_out->data = NULL;
    frame_out->size = 0;
    frame_out->header = NULL;
    frame_out->header_size = 0;
    frame_out->payload = NULL;
    frame_out->payload_size = 0;

    const size_t available = buffer->bytes.size () - buffer->consumed;
    if (available < perf_stream_common::k_stream_packet_prefix_size)
        return false;

    const unsigned char *frame = &buffer->bytes[buffer->consumed];
    const uint16_t header_size = perf_stream_common::perf_stream_load_u16_be (frame);
    const uint32_t payload_size = perf_stream_common::perf_stream_load_u32_be (frame + 2);
    const size_t max_size = perf_stream_common::k_stream_max_chunk_size;
    if (static_cast<size_t> (header_size) > max_size
        || payload_size > static_cast<uint32_t> (max_size)
        || static_cast<size_t> (header_size) > max_size - payload_size)
        return false;

    const size_t frame_size = perf_stream_common::k_stream_packet_prefix_size
                              + static_cast<size_t> (header_size)
                              + static_cast<size_t> (payload_size);
    if (available < frame_size)
        return false;

    frame_out->data = frame;
    frame_out->size = frame_size;
    frame_out->header = frame + perf_stream_common::k_stream_packet_prefix_size;
    frame_out->header_size = header_size;
    frame_out->payload = frame_out->header + header_size;
    frame_out->payload_size = payload_size;
    return true;
}

inline void consume (buffer_t *buffer, const frame_view_t &frame)
{
    if (!buffer || frame.size == 0)
        return;

    buffer->consumed += frame.size;
}

inline void compact (buffer_t *buffer)
{
    if (!buffer || buffer->consumed == 0)
        return;

    if (buffer->consumed >= buffer->bytes.size ()) {
        reset (buffer);
        return;
    }

    // Avoid a memmove on every partially-consumed chunk. Compact only after
    // enough headroom has been reclaimed to amortize the copy cost.
    if (buffer->consumed < 4096 && (buffer->consumed * 2) < buffer->bytes.size ())
        return;

    const size_t remaining = buffer->bytes.size () - buffer->consumed;
    std::memmove (&buffer->bytes[0], &buffer->bytes[buffer->consumed], remaining);
    buffer->bytes.resize (remaining);
    buffer->consumed = 0;
}

} // namespace perf_stream_frame

#endif
