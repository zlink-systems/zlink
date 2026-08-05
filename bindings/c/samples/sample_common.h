/* SPDX-License-Identifier: MPL-2.0 */
#ifndef ZLINK_C_SAMPLES_COMMON_SAMPLE_COMMON_H_INCLUDED
#define ZLINK_C_SAMPLES_COMMON_SAMPLE_COMMON_H_INCLUDED

#include <zlink.h>

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef _WIN32
#include <arpa/inet.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netinet/in.h>
#endif

/* ---- Constants ----------------------------------------------------------- */

static const char *const k_pair_payload = "hello-pair";
static const char *const k_dealer_router_request = "ping";
static const char *const k_dealer_router_reply = "pong";
static const char *const k_stream_payload = "hello-stream";
static const char *const k_pubsub_topic = "prices";
static const char *const k_pubsub_payload = "101.25";

/* ---- Message helpers ----------------------------------------------------- */

static inline void make_message (zlink_msg_t *msg, const char *text)
{
    const size_t len = strlen (text);
    int rc = zlink_msg_init_size (msg, len);
    assert (rc == 0);
    memcpy (zlink_msg_data (msg), text, len);
}

/* ---- Endpoint helpers ---------------------------------------------------- */

static inline void get_last_endpoint (void *socket, char *buf, size_t buf_size)
{
    size_t len = buf_size;
    int rc = zlink_get_option (socket, ZLINK_OPT_LAST_ENDPOINT, buf, &len);
    assert (rc == 0);
    assert (len > 0);
}

static inline void reserve_tcp_endpoint (char *buf, size_t buf_size)
{
#ifndef _WIN32
    int fd = socket (AF_INET, SOCK_STREAM, 0);
    assert (fd >= 0);

    int reuse = 1;
    assert (setsockopt (fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof (reuse)) == 0);

    struct sockaddr_in addr;
    memset (&addr, 0, sizeof (addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl (INADDR_LOOPBACK);
    addr.sin_port = 0;
    assert (bind (fd, (const struct sockaddr *) &addr, sizeof (addr)) == 0);

    socklen_t addr_len = sizeof (addr);
    assert (getsockname (fd, (struct sockaddr *) &addr, &addr_len) == 0);
    assert (close (fd) == 0);

    snprintf (buf, buf_size, "tcp://127.0.0.1:%u", (unsigned) ntohs (addr.sin_port));
#else
    snprintf (buf, buf_size, "tcp://127.0.0.1:%u",
              (unsigned) (30000u + ((unsigned) time (NULL) % 20000u)));
#endif
}

#ifndef _WIN32
static inline int parse_tcp_endpoint (const char *endpoint, char *host, size_t host_size, int *port)
{
    char proto[8] = {0};
    char addr[64] = {0};
    int p = 0;
    if (sscanf (endpoint, "%7[^:]://%63[^:]:%d", proto, addr, &p) != 3)
        return 0;
    if (strcmp (proto, "tcp") != 0 || p <= 0 || p > 65535)
        return 0;
    strncpy (host, addr, host_size - 1);
    host[host_size - 1] = '\0';
    *port = p;
    return 1;
}

static inline int raw_tcp_connect (const char *endpoint)
{
    char host[64];
    int port;
    assert (parse_tcp_endpoint (endpoint, host, sizeof (host), &port));

    struct sockaddr_in addr;
    memset (&addr, 0, sizeof (addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons ((uint16_t) port);
    int rc = inet_pton (AF_INET, host, &addr.sin_addr);
    assert (rc == 1);

    int fd = socket (AF_INET, SOCK_STREAM, 0);
    assert (fd >= 0);
    rc = connect (fd, (struct sockaddr *) &addr, sizeof (addr));
    assert (rc == 0);
    return fd;
}
#endif

/* ---- Monitor helpers ----------------------------------------------------- */

static inline void *open_socket_monitor (void *socket, zlink_socket_monitor_event_mask_t mask)
{
    zlink_socket_monitor_open_options_t opts;
    memset (&opts, 0, sizeof (opts));
    opts.events = mask;
    void *monitor = zlink_socket_monitor_open (socket, &opts);
    assert (monitor != NULL);
    return monitor;
}

static inline int
wait_for_socket_monitor_event (void *monitor, uint64_t event_type, int64_t value, int timeout_ms)
{
    void *poller = zlink_poller_new ();
    assert (poller != NULL);
    int rc = zlink_poller_add (poller, monitor, NULL, ZLINK_POLLIN);
    assert (rc == 0);

    struct timespec start;
    clock_gettime (CLOCK_MONOTONIC, &start);

    for (;;) {
        struct timespec now;
        clock_gettime (CLOCK_MONOTONIC, &now);
        long elapsed_ms = (long) (now.tv_sec - start.tv_sec) * 1000
                          + (long) (now.tv_nsec - start.tv_nsec) / 1000000;
        long remaining = (long) timeout_ms - elapsed_ms;
        if (remaining <= 0)
            break;

        zlink_poller_event_t pe;
        rc = zlink_poller_wait (poller, &pe, 1, remaining, NULL);
        if (rc <= 0)
            continue;

        zlink_socket_monitor_event_t event;
        rc = zlink_socket_monitor_recv (monitor, &event, ZLINK_DONTWAIT);
        if (rc != 0)
            continue;
        if (event.event != event_type)
            continue;
        if (value >= 0 && (int64_t) event.value != value)
            continue;
        zlink_poller_destroy (&poller);
        return 1;
    }

    zlink_poller_destroy (&poller);
    return 0;
}

static inline int wait_for_socket_monitor_event_with_activity (
  void *monitor, void *activity_socket, uint64_t event_type, int64_t value, int timeout_ms)
{
    void *poller = zlink_poller_new ();
    assert (poller != NULL);
    int rc = zlink_poller_add (poller, monitor, monitor, ZLINK_POLLIN);
    assert (rc == 0);
    if (activity_socket != NULL) {
        rc = zlink_poller_add (poller, activity_socket, activity_socket, ZLINK_POLLIN);
        assert (rc == 0);
    }

    struct timespec start;
    clock_gettime (CLOCK_MONOTONIC, &start);

    for (;;) {
        struct timespec now;
        clock_gettime (CLOCK_MONOTONIC, &now);
        long elapsed_ms = (long) (now.tv_sec - start.tv_sec) * 1000
                          + (long) (now.tv_nsec - start.tv_nsec) / 1000000;
        long remaining = (long) timeout_ms - elapsed_ms;
        if (remaining <= 0)
            break;

        zlink_poller_event_t pe;
        rc = zlink_poller_wait (poller, &pe, 1, remaining, NULL);
        if (rc <= 0 || pe.socket != monitor)
            continue;

        zlink_socket_monitor_event_t event;
        rc = zlink_socket_monitor_recv (monitor, &event, ZLINK_DONTWAIT);
        if (rc != 0)
            continue;
        if (event.event != event_type)
            continue;
        if (value >= 0 && (int64_t) event.value != value)
            continue;
        zlink_poller_destroy (&poller);
        return 1;
    }

    zlink_poller_destroy (&poller);
    return 0;
}

static inline int wait_connected (void *server_monitor, void *client_monitor, int timeout_ms)
{
    if (!wait_for_socket_monitor_event (server_monitor, ZLINK_SOCKET_MONITOR_EVENT_CONNECTION_READY,
                                        -1, timeout_ms))
        return 0;
    if (!wait_for_socket_monitor_event (client_monitor, ZLINK_SOCKET_MONITOR_EVENT_CONNECTION_READY,
                                        -1, timeout_ms))
        return 0;
    return 1;
}

static inline int wait_stream_connected (void *server_monitor, int timeout_ms)
{
    return wait_for_socket_monitor_event (server_monitor, ZLINK_SOCKET_MONITOR_EVENT_ACCEPTED, -1,
                                          timeout_ms);
}

static inline int wait_for_subscription_event (void *subject_,
                                               int *subscribed_out_,
                                               char *topic_id_out_,
                                               size_t *topic_id_len_out_,
                                               int timeout_ms_)
{
    void *poller = zlink_poller_new ();
    assert (poller != NULL);
    int rc = zlink_poller_add (poller, subject_, subject_, ZLINK_POLLIN);
    assert (rc == 0);

    struct timespec start;
    clock_gettime (CLOCK_MONOTONIC, &start);

    for (;;) {
        struct timespec now;
        clock_gettime (CLOCK_MONOTONIC, &now);
        long elapsed_ms = (long) (now.tv_sec - start.tv_sec) * 1000
                          + (long) (now.tv_nsec - start.tv_nsec) / 1000000;
        long remaining = (long) timeout_ms_ - elapsed_ms;
        if (remaining <= 0)
            break;

        zlink_poller_event_t pe;
        rc = zlink_poller_wait (poller, &pe, 1, remaining, NULL);
        if (rc <= 0)
            continue;

        const zlink_routing_id_t *source_rid = NULL;
        rc = zlink_xpub_recv_part (subject_, &source_rid, subscribed_out_, topic_id_out_,
                                   *topic_id_len_out_, topic_id_len_out_, ZLINK_DONTWAIT);
        if (rc == ZLINK_RECV_OK) {
            zlink_poller_destroy (&poller);
            return 1;
        }
    }

    zlink_poller_destroy (&poller);
    return 0;
}

static inline void sample_pause_ms (int timeout_ms_)
{
#ifndef _WIN32
    struct timespec req;
    req.tv_sec = timeout_ms_ / 1000;
    req.tv_nsec = (long) (timeout_ms_ % 1000) * 1000000L;
    while (nanosleep (&req, &req) != 0) {
    }
#else
    zlink_pollitem_t item = {NULL, 0, 0, 0};
    (void) zlink_poll (&item, 0, timeout_ms_, NULL);
#endif
}

/* ---- Callback synchronization primitives (POSIX) ------------------------- */

#ifndef _WIN32

typedef struct
{
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int ready;
} callback_signal_t;

static inline void callback_signal_init (callback_signal_t *sig)
{
    pthread_mutex_init (&sig->mutex, NULL);
    pthread_cond_init (&sig->cond, NULL);
    sig->ready = 0;
}

static inline void callback_signal_destroy (callback_signal_t *sig)
{
    pthread_cond_destroy (&sig->cond);
    pthread_mutex_destroy (&sig->mutex);
}

static inline void callback_signal_set (callback_signal_t *sig)
{
    pthread_mutex_lock (&sig->mutex);
    sig->ready = 1;
    pthread_cond_signal (&sig->cond);
    pthread_mutex_unlock (&sig->mutex);
}

static inline int callback_signal_wait (callback_signal_t *sig, int timeout_ms)
{
    struct timespec ts;
    clock_gettime (CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock (&sig->mutex);
    while (!sig->ready) {
        if (pthread_cond_timedwait (&sig->cond, &sig->mutex, &ts) != 0) {
            pthread_mutex_unlock (&sig->mutex);
            return 0;
        }
    }
    pthread_mutex_unlock (&sig->mutex);
    return 1;
}

#endif /* _WIN32 */

#endif /* ZLINK_C_SAMPLES_COMMON_SAMPLE_COMMON_H_INCLUDED */
