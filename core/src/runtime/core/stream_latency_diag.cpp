/* SPDX-License-Identifier: MPL-2.0 */
#include "core/stream_latency_diag.hpp"
#include <cstdlib>
#include <cstdio>
#include <cerrno>
#include <ctime>
#include <atomic>
#if defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#include <sys/syscall.h>
#endif
namespace zlink
{
namespace stream_latency_diag
{
namespace
{
bool env_enabled ()
{
    const char *p = std::getenv ("ZLINK_DIAG_STREAM_LATENCY");
    return p && p[0] == '1' && !p[1];
}
uint64_t env_sample ()
{
    const char *p = std::getenv ("ZLINK_DIAG_STREAM_LATENCY_SAMPLE");
    if (!p || !*p)
        return 1;
    char *end = NULL;
    const unsigned long long n = std::strtoull (p, &end, 10);
    return *p != '-' && end && !*end && n ? n : 1;
}
const char *const output = std::getenv ("ZLINK_DIAG_STREAM_LATENCY_OUT");
struct record_t
{
    uint64_t conn, seq, ns, value;
    unsigned stage;
};
struct buffer_t
{
    record_t rows[4096];
    unsigned count;
    uint64_t lost, last, active, handlers, pending_max;
    long tid;
    int fd;
    bool opened;
    void flush ()
    {
#if defined(__linux__)
        const int saved = errno;
        if (!opened) {
            opened = true;
            fd = -1;
            char path[4096];
            const int n =
              std::snprintf (path, sizeof path, "%s.%ld.%ld.csv",
                             output ? output : "/tmp/zlink-stream-latency", (long) getpid (), tid);
            if (n > 0 && size_t (n) < sizeof path)
                fd = ::open (path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
            if (fd < 0)
                std::fprintf (stderr, "STREAM latency CSV open failed: errno=%d\n", errno);
        }
        // Stack formatting and write(2), with no stdio file lock or heap buffer.
        if (lost && count < 4096) {
            rows[count++] = {0, 0, last, lost, loss};
            lost = 0;
        }
        char text[65536];
        size_t buffered = 0;
        const auto drain = [&] () {
            size_t at = 0;
            while (at < buffered) {
                const ssize_t rc = ::write (fd, text + at, buffered - at);
                if (rc < 0 && errno == EINTR)
                    continue;
                if (rc <= 0) {
                    ++lost;
                    break;
                }
                at += size_t (rc);
            }
            buffered = 0;
        };
        if (fd >= 0) {
            for (unsigned i = 0; i < count; ++i) {
                const record_t &r = rows[i];
                static const char *const names[] = {
                  "",           "read",  "rx_enqueue", "deliver", "accept",
                  "tx_enqueue", "write", "io_summary", "loss",    "pending_bytes"};
                const char *direction = r.stage <= deliver ? "rx" : r.stage <= write ? "tx" : "io";
                if (sizeof text - buffered < 256)
                    drain ();
                const int n = std::snprintf (
                  text + buffered, sizeof text - buffered, "%llu,%llu,%s,%s,%llu,%ld,%llu\n",
                  (unsigned long long) r.conn, (unsigned long long) r.seq, direction,
                  names[r.stage], (unsigned long long) r.ns, tid, (unsigned long long) r.value);
                buffered += size_t (n);
            }
        }
        if (fd >= 0)
            drain ();
        count = 0;
        errno = saved;
#endif
    }
    void finish ()
    {
        if (count || lost || opened)
            flush ();
#if defined(__linux__)
        if (opened && fd >= 0)
            ::close (fd);
#endif
    }
};
// Reserve buffers in BSS so the first event never allocates or registers a
// TLS destructor. The existing thread owns its slot until library teardown.
struct buffers_t
{
    buffer_t slots[256];
    std::atomic<unsigned> used;
    buffers_t () : used (0) {}
    ~buffers_t ()
    {
        const unsigned n = used.load (std::memory_order_relaxed);
        if (n > 256)
            slots[0].lost += n - 256;
        for (unsigned i = 0; i < n && i < 256; ++i)
            slots[i].finish ();
    }
} buffers;
thread_local unsigned slot = 256;
buffer_t *thread_buffer ()
{
    if (slot == 256) {
        const int saved = errno;
        slot = buffers.used.fetch_add (1, std::memory_order_relaxed);
#if defined(__linux__)
        if (slot < 256)
            buffers.slots[slot].tid = syscall (SYS_gettid);
#endif
        if (slot >= 256) {
            slot = 257;
            std::fprintf (stderr,
                          "STREAM latency: more than 256 traced threads; trace incomplete\n");
        }
        errno = saved;
    }
    return slot < 256 ? &buffers.slots[slot] : NULL;
}
}
const bool enabled = env_enabled ();
const uint64_t sample = env_sample ();
uint64_t now ()
{
    const int saved = errno;
    timespec t;
    clock_gettime (CLOCK_MONOTONIC, &t);
    errno = saved;
    return uint64_t (t.tv_sec) * 1000000000ULL + t.tv_nsec;
}
uint64_t cpu_now ()
{
    const int saved = errno;
    timespec t;
    clock_gettime (CLOCK_THREAD_CPUTIME_ID, &t);
    errno = saved;
    return uint64_t (t.tv_sec) * 1000000000ULL + t.tv_nsec;
}
void pending (uint64_t bytes)
{
    buffer_t *ptr = thread_buffer ();
    if (!ptr)
        return;
    buffer_t &buffer = *ptr;
    if (bytes > buffer.pending_max)
        buffer.pending_max = bytes;
}
bool selected (uint64_t seq)
{
    return seq && (seq - 1) % sample == 0;
}
void event (uint64_t conn, uint64_t seq, stage_t stage, uint64_t ns, uint64_t value)
{
    buffer_t *ptr = thread_buffer ();
    if (!ptr)
        return;
    buffer_t &buffer = *ptr;
    if (buffer.count == 4095)
        ++buffer.lost;
    else
        buffer.rows[buffer.count++] = {conn, seq, ns, value, unsigned (stage)};
    if (!buffer.last)
        buffer.last = ns;
    if (ns - buffer.last >= 1000000000ULL) {
        buffer.flush ();
        buffer.last = ns;
    }
}
void delivered (uint64_t conn, uint64_t seq)
{
    if (seq)
        event (conn, seq, deliver, now ());
}
void tick (uint64_t active_ns, uint64_t handlers)
{
    buffer_t *ptr = thread_buffer ();
    if (!ptr)
        return;
    buffer_t &buffer = *ptr;
    buffer.active += active_ns;
    buffer.handlers += handlers;
    const uint64_t ns = now ();
    if (!buffer.last)
        buffer.last = ns;
    if (ns - buffer.last >= 1000000000ULL) {
        // Queue observation covers buffered STREAM RX bytes, not Asio's
        // private ready-handler queue (which has no public size accessor).
        event (0, 0, pending_bytes, ns, buffer.pending_max);
        buffer.pending_max = 0;
        event (0, buffer.handlers, io_summary, ns, buffer.active);
    }
}
void parser_t::feed (uint64_t connection, const void *data, size_t size, bool packet, stage_t stage)
{
    if (conn != connection) {
        *this = parser_t ();
        conn = connection;
    }
    if (!size)
        return;
    if (!packet) {
        if (selected (++seq))
            event (conn, seq, stage, now (), size);
        return;
    }
    const unsigned char *p = static_cast<const unsigned char *> (data);
    uint64_t stamp = 0;
    while (size) {
        if (used < 6) {
            prefix[used++] = *p++;
            --size;
            if (used != 6)
                continue;
            remaining = (uint64_t (prefix[0]) << 8) + prefix[1] + (uint64_t (prefix[2]) << 24)
                        + (uint64_t (prefix[3]) << 16) + (uint64_t (prefix[4]) << 8) + prefix[5];
        }
        const size_t n = remaining < size ? size_t (remaining) : size;
        p += n;
        size -= n;
        remaining -= n;
        if (!remaining) {
            ++seq;
            if (selected (seq)) {
                if (!stamp)
                    stamp = now ();
                event (conn, seq, stage, stamp);
            }
            used = 0;
        }
    }
}
void engine_t::pulled_msg (size_t size, uint64_t seq)
{
    pulled += size;
    if (!seq)
        return;
    if (count == 256) {
        buffer_t *ptr = thread_buffer ();
        if (ptr)
            ++ptr->lost;
        return;
    }
    boundaries[(head + count++) % 256] = {pulled, seq};
}
void engine_t::wrote (uint64_t conn, size_t size)
{
    written += size;
    uint64_t stamp = 0;
    while (count && boundaries[head].end <= written) {
        if (!stamp)
            stamp = now ();
        event (conn, boundaries[head].seq, write, stamp);
        head = (head + 1) % 256;
        --count;
    }
}
}
}
