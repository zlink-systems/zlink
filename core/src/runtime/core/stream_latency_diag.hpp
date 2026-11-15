/* SPDX-License-Identifier: MPL-2.0 */
#ifndef ZLINK_STREAM_LATENCY_DIAG_HPP
#define ZLINK_STREAM_LATENCY_DIAG_HPP
#include <cstddef>
#include <cstdint>
namespace zlink
{
namespace stream_latency_diag
{
// Diagnostic-only, process-lifetime environment snapshot. No transport policy.
extern const bool enabled;
extern const uint64_t sample;
enum stage_t
{
    read = 1,
    rx_enqueue,
    deliver,
    accept,
    tx_enqueue,
    write,
    io_summary,
    loss,
    pending_bytes
};
uint64_t now ();
bool selected (uint64_t seq);
void event (uint64_t conn, uint64_t seq, stage_t stage, uint64_t ns, uint64_t value = 0);
uint64_t cpu_now ();
void pending (uint64_t bytes);
void tick (uint64_t active_ns, uint64_t handlers);
void delivered (uint64_t conn, uint64_t seq);
// STREAM packet prefix is uint16 header size + uint32 body size (network order).
// This observer only counts bytes; the production decoder alone validates data.
struct parser_t
{
    uint64_t conn = 0, seq = 0, remaining = 0;
    unsigned used = 0;
    unsigned char prefix[6] = {};
    void feed (uint64_t connection, const void *data, size_t size, bool packet, stage_t stage);
};
struct engine_t
{
    parser_t rx;
    uint64_t pulled = 0, written = 0;
    struct boundary_t
    {
        uint64_t end, seq;
    };
    boundary_t boundaries[256];
    unsigned head = 0, count = 0;
    void pulled_msg (size_t size, uint64_t seq);
    void wrote (uint64_t conn, size_t size);
};
}
}
#endif
