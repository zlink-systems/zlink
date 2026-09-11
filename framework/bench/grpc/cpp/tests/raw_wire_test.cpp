/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#include "../common/bench_common.hpp"
#include "bench.pb.h"

#include <cstdio>
#include <stdexcept>

// Pre-#66 manual encoder, retained only as the compatibility oracle.
namespace legacy_wire
{
using namespace zlink_cpp_bench;
inline size_t varint_size (size_t value)
{
    size_t n = 1;
    while (value >= 0x80) {
        value >>= 7;
        ++n;
    }
    return n;
}

inline unsigned char *write_varint (unsigned char *dst, size_t value)
{
    while (value >= 0x80) {
        *dst++ = static_cast<unsigned char> ((value & 0x7fU) | 0x80U);
        value >>= 7;
    }
    *dst++ = static_cast<unsigned char> (value);
    return dst;
}

inline size_t encoded_bench_payload_size (size_t payload_size)
{
    const size_t body_size = std::max (payload_size, k_header_size);
    return 1 + varint_size (body_size) + body_size;
}

// Encodes BenchPayload{body} into final writable storage and stamps the spec 6
// header into the body. Returns the offset of the body inside `out`.
inline size_t encode_bench_payload (std::span<unsigned char> out,
                                    size_t payload_size,
                                    uint32_t run_id,
                                    phase_t phase,
                                    uint64_t seq)
{
    const size_t body_size = std::max (payload_size, k_header_size);
    if (out.size () != encoded_bench_payload_size (body_size))
        throw std::invalid_argument ("encoded BenchPayload storage has the wrong size");
    std::fill (out.begin (), out.end (), 0xab);
    out[0] = 0x0a;
    unsigned char *body = write_varint (out.data () + 1, body_size);
    std::memset (body, 0xab, body_size);
    stamp_payload (body, body_size, run_id, phase, seq);
    return static_cast<size_t> (body - out.data ());
}

inline size_t encode_bench_payload (std::vector<unsigned char> &out,
                                    size_t payload_size,
                                    uint32_t run_id,
                                    phase_t phase,
                                    uint64_t seq)
{
    out.resize (encoded_bench_payload_size (payload_size));
    return encode_bench_payload (
      std::span<unsigned char> (out.data (), out.size ()), payload_size, run_id, phase, seq);
}

}

int main ()
{
    using namespace zlink_cpp_bench;
    constexpr uint32_t run_id = 0x01020304;
    constexpr uint64_t sequence = 0x0102030405060708;
    constexpr uint64_t sent_ns = 0x1112131415161718;
    // Captured pre-#66 encoder dump: tag, length, and the fixed 29-byte header.
    const std::string golden =
      "0a1d4b4e4c5a04030201011d00000008070605040302011817161514131211";
    for (const size_t size : {29U, 127U, 128U, 1024U}) {
        std::vector<unsigned char> legacy;
        const size_t offset = legacy_wire::encode_bench_payload (legacy, size, run_id, phase_active, sequence);
        write_u64_le (legacy.data () + offset + 21, sent_ns);
        zlink::framework::bench::withgrpc::BenchPayload payload;
        payload.set_body (legacy.data () + offset, size);
        const std::string encoded = encode_bench_payload (payload);
        if (encoded.size () != legacy.size ()
            || std::memcmp (encoded.data (), legacy.data (), legacy.size ()) != 0)
            throw std::runtime_error ("legacy/protobuf wire mismatch");
        zlink::framework::bench::withgrpc::BenchPayload decoded;
        if (!decoded.ParseFromString (encoded) || decoded.body () != payload.body ()
            || encode_bench_payload (decoded) != encoded)
            throw std::runtime_error ("protobuf roundtrip mismatch");
        if (size == 29) {
            std::string hex;
            for (const unsigned char byte : encoded) {
                char pair[3];
                std::snprintf (pair, sizeof pair, "%02x", byte);
                hex += pair;
            }
            if (hex != golden)
                throw std::runtime_error ("fixed pre-change dump mismatch");
            std::printf ("cpp raw wire 29B: %s\n", hex.c_str ());
        }
    }
    std::puts ("cpp raw wire identity: 29, 127, 128, 1024 bytes PASS");
}
