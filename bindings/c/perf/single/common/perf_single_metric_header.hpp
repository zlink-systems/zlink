#ifndef PERF_SINGLE_METRIC_HEADER_HPP
#define PERF_SINGLE_METRIC_HEADER_HPP

#include <chrono>
#include <cstdint>
#include <cstring>

namespace perf_single_metric
{

static const uint32_t k_magic = 0x5A4C4E4BU; // "ZLNK"

enum phase_t
{
    phase_unknown = 255,
    phase_warmup = 0,
    phase_active = 1,
    phase_cooldown = 2
};

struct header_t
{
    uint32_t magic;
    uint32_t run_id;
    uint8_t phase;
    uint32_t msg_size;
    uint64_t seq;
    int64_t sent_ts_ns;
};

inline size_t header_size ()
{
    return 29;
}

inline uint64_t now_ns ()
{
    return static_cast<uint64_t> (std::chrono::duration_cast<std::chrono::nanoseconds> (
                                    std::chrono::system_clock::now ().time_since_epoch ())
                                    .count ());
}

inline void init_header (
  header_t *out, uint32_t run_id, phase_t phase, size_t msg_size, uint64_t seq, uint64_t sent_ts_ns)
{
    if (!out)
        return;

    out->magic = k_magic;
    out->run_id = run_id;
    out->phase = static_cast<uint8_t> (phase);
    out->msg_size = static_cast<uint32_t> (msg_size);
    out->seq = seq;
    out->sent_ts_ns = static_cast<int64_t> (sent_ts_ns);
}

inline void write_u32_le (unsigned char *dst, uint32_t value)
{
    dst[0] = static_cast<unsigned char> (value & 0xffU);
    dst[1] = static_cast<unsigned char> ((value >> 8) & 0xffU);
    dst[2] = static_cast<unsigned char> ((value >> 16) & 0xffU);
    dst[3] = static_cast<unsigned char> ((value >> 24) & 0xffU);
}

inline void write_u64_le (unsigned char *dst, uint64_t value)
{
    for (size_t i = 0; i < 8; ++i)
        dst[i] = static_cast<unsigned char> ((value >> (i * 8)) & 0xffU);
}

inline uint32_t read_u32_le (const unsigned char *src)
{
    return static_cast<uint32_t> (src[0]) | (static_cast<uint32_t> (src[1]) << 8)
           | (static_cast<uint32_t> (src[2]) << 16) | (static_cast<uint32_t> (src[3]) << 24);
}

inline uint64_t read_u64_le (const unsigned char *src)
{
    uint64_t value = 0;
    for (size_t i = 0; i < 8; ++i)
        value |= static_cast<uint64_t> (src[i]) << (i * 8);
    return value;
}

inline bool encode_header (void *dst, size_t dst_size, const header_t &h)
{
    if (!dst || dst_size < header_size ())
        return false;

    unsigned char *p = static_cast<unsigned char *> (dst);
    write_u32_le (p + 0, h.magic);
    write_u32_le (p + 4, h.run_id);
    p[8] = h.phase;
    write_u32_le (p + 9, h.msg_size);
    write_u64_le (p + 13, h.seq);
    write_u64_le (p + 21, static_cast<uint64_t> (h.sent_ts_ns));
    return true;
}

inline bool decode_header (const void *src, size_t src_size, header_t *out)
{
    if (!src || src_size < header_size () || !out)
        return false;

    const unsigned char *p = static_cast<const unsigned char *> (src);
    out->magic = read_u32_le (p + 0);
    out->run_id = read_u32_le (p + 4);
    out->phase = p[8];
    out->msg_size = read_u32_le (p + 9);
    out->seq = read_u64_le (p + 13);
    out->sent_ts_ns = static_cast<int64_t> (read_u64_le (p + 21));
    return true;
}

inline bool stamp_payload (void *payload,
                           size_t payload_size,
                           uint32_t run_id,
                           phase_t phase,
                           size_t msg_size,
                           uint64_t seq,
                           uint64_t sent_ts_ns)
{
    if (!payload || payload_size < header_size ())
        return false;

    header_t h;
    init_header (&h, run_id, phase, msg_size, seq, sent_ts_ns);
    return encode_header (payload, payload_size, h);
}

inline bool decode_payload_header (const void *payload, size_t payload_size, header_t *out)
{
    if (!decode_header (payload, payload_size, out))
        return false;
    return out->magic == k_magic;
}

inline bool is_expected (const header_t &h, uint32_t run_id, phase_t phase, size_t msg_size)
{
    return h.magic == k_magic && h.run_id == run_id && h.phase == static_cast<uint8_t> (phase)
           && h.msg_size == static_cast<uint32_t> (msg_size);
}

} // namespace perf_single_metric

#endif
