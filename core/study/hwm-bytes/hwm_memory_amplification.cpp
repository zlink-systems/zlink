/* SPDX-License-Identifier: MPL-2.0 */

//  Measures the Core memory amplification of the byte high water mark:
//
//      coreMemoryAmplification = processMemoryIncrease
//                                / accountedApplicationMessageBytes
//
//  One sender fills an inproc pipe whose reader never receives. inproc keeps
//  the whole backlog inside the process, so the resident increase is Core's
//  own cost for the accounted bytes and nothing else. The accounted amount is
//  read from the monitor snapshot instead of being inferred from the payload,
//  so routing frames, metadata and the minimum per-frame charge are included.
//
//  Usage: hwm_memory_amplification <payload_bytes> [hwm_bytes]

#include <zlink.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace
{

uint64_t resident_kib ()
{
    FILE *status = fopen ("/proc/self/status", "r");
    if (!status)
        return 0;
    char line[256];
    uint64_t value = 0;
    while (fgets (line, sizeof (line), status)) {
        if (strncmp (line, "VmRSS:", 6) == 0) {
            value = strtoull (line + 6, NULL, 10);
            break;
        }
    }
    fclose (status);
    return value;
}

//  Resident size only grows while the pipe fills, so the reading is taken
//  after the fill has stopped and the sender has gone quiet.
uint64_t quiesced_resident_kib ()
{
    std::this_thread::sleep_for (std::chrono::milliseconds (300));
    return resident_kib ();
}

bool set_hwm (void *socket_, zlink_option_t option_, uint64_t bytes_)
{
    return zlink_set_option (socket_, option_, &bytes_, sizeof (bytes_)) == ZLINK_CONFIG_OK;
}

}

int main (int argc, char **argv)
{
    const size_t payload_bytes = argc > 1 ? strtoul (argv[1], NULL, 10) : 64;
    const uint64_t hwm_bytes = argc > 2 ? strtoull (argv[2], NULL, 10) : 64u * 1024u * 1024u;
    if (payload_bytes == 0) {
        fprintf (stderr, "payload_bytes must be positive\n");
        return 1;
    }

    void *ctx = zlink_ctx_new ();
    if (!ctx) {
        fprintf (stderr, "zlink_ctx_new failed\n");
        return 1;
    }

    void *sender = zlink_socket (ctx, ZLINK_SOCKET_PAIR);
    void *receiver = zlink_socket (ctx, ZLINK_SOCKET_PAIR);
    if (!sender || !receiver) {
        fprintf (stderr, "zlink_socket failed\n");
        return 1;
    }

    //  The reader is never drained, so both directions of the inproc pipe are
    //  bounded by the same configured byte limit.
    if (!set_hwm (sender, ZLINK_OPT_SNDHWM, hwm_bytes)
        || !set_hwm (sender, ZLINK_OPT_RCVHWM, hwm_bytes)
        || !set_hwm (receiver, ZLINK_OPT_SNDHWM, hwm_bytes)
        || !set_hwm (receiver, ZLINK_OPT_RCVHWM, hwm_bytes)) {
        fprintf (stderr, "byte HWM option rejected: errno=%d\n", zlink_errno ());
        return 1;
    }

    zlink_socket_monitor_open_options_t monitor_opts;
    memset (&monitor_opts, 0, sizeof (monitor_opts));
    monitor_opts.events = ZLINK_EVENT_CONNECTION_READY;
    void *monitor = zlink_socket_monitor_open (sender, &monitor_opts);
    if (!monitor) {
        fprintf (stderr, "zlink_socket_monitor_open failed\n");
        return 1;
    }

    const std::string endpoint = "inproc://hwm-memory-amplification";
    if (zlink_bind (receiver, endpoint.c_str ()) != 0
        || zlink_connect (sender, endpoint.c_str ()) != 0) {
        fprintf (stderr, "bind/connect failed: errno=%d\n", zlink_errno ());
        return 1;
    }

    //  Baseline after the connection exists: the pipe, its first chunk and the
    //  monitor are already allocated, so only the backlog is measured.
    const uint64_t baseline_kib = quiesced_resident_kib ();

    uint64_t sent = 0;
    for (;;) {
        zlink_msg_t msg;
        if (zlink_msg_init_size (&msg, payload_bytes) != 0) {
            fprintf (stderr, "zlink_msg_init_size failed: errno=%d\n", zlink_errno ());
            return 1;
        }
        memset (zlink_msg_data (&msg), 'a', payload_bytes);
        const zlink_submit_result_t rc =
          zlink_send_part (sender, &msg, ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL);
        if (rc == ZLINK_SUBMIT_BACKPRESSURED)
            break;
        if (rc != ZLINK_SUBMIT_OK) {
            fprintf (stderr, "zlink_send_part failed rc=%d errno=%d\n", (int) rc, zlink_errno ());
            return 1;
        }
        ++sent;
    }

    const uint64_t filled_kib = quiesced_resident_kib ();

    zlink_monitor_status_t status;
    memset (&status, 0, sizeof (status));
    if (zlink_monitor_status (monitor, &status) != ZLINK_CONFIG_OK) {
        fprintf (stderr, "zlink_monitor_status failed: errno=%d\n", zlink_errno ());
        return 1;
    }

    const uint64_t resident_increase = (filled_kib - baseline_kib) * 1024u;
    const uint64_t accounted = status.snd_bytes_in_flight;
    const double amplification =
      accounted > 0 ? (double) resident_increase / (double) accounted : 0.0;

    printf ("payload_bytes=%zu\n", payload_bytes);
    printf ("configured_hwm_bytes=%llu\n", (unsigned long long) hwm_bytes);
    printf ("messages_admitted=%llu\n", (unsigned long long) sent);
    printf ("payload_bytes_total=%llu\n", (unsigned long long) (sent * payload_bytes));
    printf ("accounted_bytes=%llu\n", (unsigned long long) accounted);
    printf ("minimum_core_message_charge_bytes=%llu\n",
            (unsigned long long) status.minimum_core_message_charge_bytes);
    printf ("baseline_rss_kib=%llu\n", (unsigned long long) baseline_kib);
    printf ("filled_rss_kib=%llu\n", (unsigned long long) filled_kib);
    printf ("resident_increase_bytes=%llu\n", (unsigned long long) resident_increase);
    printf ("memory_amplification=%.3f\n", amplification);

    zlink_monitor_close (&monitor);
    zlink_close (sender);
    zlink_close (receiver);
    zlink_ctx_term (ctx);
    return 0;
}
