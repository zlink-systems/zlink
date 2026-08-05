/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_monitoring.hpp"
#include "testutil_unity.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <string.h>
#include <vector>

#if defined(ZLINK_HAVE_WINDOWS)
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

SETUP_TEARDOWN_TESTCONTEXT

namespace
{
static const size_t stream_routing_id_size = 4;

enum monitor_mode_t
{
    monitor_recv_mode = 0,
    monitor_callback_mode = 1
};

enum socket_mode_t
{
    socket_recv_mode = 0,
    socket_callback_mode = 1
};

struct socket_monitor_probe_t
{
    socket_monitor_probe_t () :
        ready_seen (false),
        sub_delivery_ready_seen (false),
        pub_delivery_ready_seen (false),
        error_seen (false)
    {
        memset (&ready_event, 0, sizeof (ready_event));
        memset (&sub_delivery_ready_event, 0, sizeof (sub_delivery_ready_event));
        memset (&pub_delivery_ready_event, 0, sizeof (pub_delivery_ready_event));
        memset (&error_event, 0, sizeof (error_event));
    }

    std::mutex mutex;
    std::condition_variable cv;
    bool ready_seen;
    bool sub_delivery_ready_seen;
    bool pub_delivery_ready_seen;
    bool error_seen;
    zlink_monitor_event_t ready_event;
    zlink_monitor_event_t sub_delivery_ready_event;
    zlink_monitor_event_t pub_delivery_ready_event;
    zlink_monitor_event_t error_event;
};

struct delivery_ready_value_probe_t
{
    delivery_ready_value_probe_t () : error_seen (false), event_count (0), last_value (0) {}

    std::mutex mutex;
    std::condition_variable cv;
    bool error_seen;
    size_t event_count;
    uint64_t last_value;
};

struct pair_callback_probe_t
{
    pair_callback_probe_t () : socket (NULL), request_calls (0), reply_calls (0)
    {
        memset (request_payload, 0, sizeof (request_payload));
        memset (reply_payload, 0, sizeof (reply_payload));
    }

    void *socket;
    std::mutex mutex;
    std::condition_variable cv;
    int request_calls;
    int reply_calls;
    char request_payload[32];
    char reply_payload[32];
};

pair_callback_probe_t *g_pair_server_probe = NULL;
pair_callback_probe_t *g_pair_client_probe = NULL;

struct raw_callback_probe_t
{
    raw_callback_probe_t () : socket (NULL), calls (0), rid_size (0), part_count (0)
    {
        memset (rid, 0, sizeof (rid));
        memset (parts, 0, sizeof (parts));
        memset (recorded_part_counts, 0, sizeof (recorded_part_counts));
        memset (recorded_parts, 0, sizeof (recorded_parts));
    }

    void *socket;
    std::mutex mutex;
    std::condition_variable cv;
    int calls;
    size_t rid_size;
    size_t part_count;
    unsigned char rid[255];
    char parts[3][64];
    size_t recorded_part_counts[4];
    char recorded_parts[4][3][64];
};

struct stream_callback_probe_t
{
    stream_callback_probe_t () : socket (NULL), calls (0), send_ok (false)
    {
        memset (routing_id, 0, sizeof (routing_id));
        memset (payload, 0, sizeof (payload));
    }

    void *socket;
    std::mutex mutex;
    std::condition_variable cv;
    int calls;
    bool send_ok;
    unsigned char routing_id[stream_routing_id_size];
    char payload[64];
};

raw_callback_probe_t *g_router_server_probe = NULL;
raw_callback_probe_t *g_router_client_probe = NULL;
raw_callback_probe_t *g_sub_probe = NULL;
stream_callback_probe_t *g_stream_probe = NULL;

void close_message_parts (zlink_msg_t *parts_, size_t part_count_)
{
    for (size_t i = 0; i < part_count_; ++i)
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&parts_[i]));
}

void init_text_part (zlink_msg_t *part_, const char *text_)
{
    const size_t size = strlen (text_);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (part_, size));
    memcpy (zlink_msg_data (part_), text_, size);
}

void socket_monitor_handler (const zlink_monitor_event_t *event_, void *userdata_)
{
    socket_monitor_probe_t *probe = static_cast<socket_monitor_probe_t *> (userdata_);
    if (!probe || !event_)
        return;

    std::lock_guard<std::mutex> lock (probe->mutex);
    if (event_->event == ZLINK_EVENT_CONNECTION_READY && !probe->ready_seen) {
        probe->ready_event = *event_;
        probe->ready_seen = true;
        probe->sub_delivery_ready_event = *event_;
        probe->sub_delivery_ready_seen = true;
        probe->pub_delivery_ready_seen = true;
    } else if (event_->event == ZLINK_EVENT_MONITOR_STOPPED
               || event_->event == ZLINK_EVENT_HANDSHAKE_FAILED_NO_DETAIL) {
        probe->error_event = *event_;
        probe->error_seen = true;
    }
    probe->cv.notify_all ();
}

void delivery_ready_value_handler (const zlink_monitor_event_t *event_, void *userdata_)
{
    delivery_ready_value_probe_t *probe = static_cast<delivery_ready_value_probe_t *> (userdata_);
    if (!probe || !event_)
        return;

    std::lock_guard<std::mutex> lock (probe->mutex);
    if (event_->event == ZLINK_EVENT_CONNECTION_READY) {
        probe->last_value = probe->event_count + 1;
        ++probe->event_count;
    } else if (event_->event == ZLINK_EVENT_MONITOR_STOPPED
               || event_->event == ZLINK_EVENT_HANDSHAKE_FAILED_NO_DETAIL) {
        probe->error_seen = true;
    }
    probe->cv.notify_all ();
}

bool wait_for_pubsub_delivery_ready_callback (socket_monitor_probe_t *pub_probe_,
                                              socket_monitor_probe_t *sub_probe_,
                                              int timeout_ms_)
{
    if (!pub_probe_ || !sub_probe_)
        return false;

    const auto deadline =
      std::chrono::steady_clock::now () + std::chrono::milliseconds (timeout_ms_);

    {
        std::unique_lock<std::mutex> lock (sub_probe_->mutex);
        if (!sub_probe_->cv.wait_until (lock, deadline, [sub_probe_] () {
                return sub_probe_->ready_seen || sub_probe_->error_seen;
            })) {
            return false;
        }
        if (sub_probe_->error_seen || !sub_probe_->ready_seen)
            return false;
    }

    {
        std::unique_lock<std::mutex> lock (pub_probe_->mutex);
        if (!pub_probe_->cv.wait_until (lock, deadline, [pub_probe_] () {
                return pub_probe_->ready_seen || pub_probe_->error_seen;
            })) {
            return false;
        }
        return pub_probe_->ready_seen && !pub_probe_->error_seen;
    }
}

bool wait_for_monitor_ready_callback (socket_monitor_probe_t *probe_, int timeout_ms_)
{
    if (!probe_)
        return false;

    std::unique_lock<std::mutex> lock (probe_->mutex);
    return probe_->cv.wait_for (lock, std::chrono::milliseconds (timeout_ms_),
                                [probe_] () { return probe_->ready_seen || probe_->error_seen; })
           && probe_->ready_seen && !probe_->error_seen;
}

bool wait_for_pub_delivery_ready_value (delivery_ready_value_probe_t *probe_,
                                        uint64_t expected_value_,
                                        int timeout_ms_)
{
    if (!probe_)
        return false;

    std::unique_lock<std::mutex> lock (probe_->mutex);
    const bool signaled = probe_->cv.wait_for (
      lock, std::chrono::milliseconds (timeout_ms_), [probe_, expected_value_] () {
          return probe_->error_seen
                 || (probe_->event_count > 0 && probe_->last_value == expected_value_);
      });
    return signaled && !probe_->error_seen && probe_->event_count > 0
           && probe_->last_value == expected_value_;
}

bool wait_for_pub_delivery_ready_value_at_least (delivery_ready_value_probe_t *probe_,
                                                 uint64_t expected_min_value_,
                                                 int timeout_ms_)
{
    if (!probe_)
        return false;

    std::unique_lock<std::mutex> lock (probe_->mutex);
    const bool signaled = probe_->cv.wait_for (
      lock, std::chrono::milliseconds (timeout_ms_), [probe_, expected_min_value_] () {
          return probe_->error_seen
                 || (probe_->event_count > 0 && probe_->last_value >= expected_min_value_);
      });
    return signaled && !probe_->error_seen && probe_->event_count > 0
           && probe_->last_value >= expected_min_value_;
}

bool wait_for_monitor_ready_recv (void *monitor_, int timeout_ms_)
{
    const auto deadline =
      std::chrono::steady_clock::now () + std::chrono::milliseconds (timeout_ms_);
    while (std::chrono::steady_clock::now () < deadline) {
        zlink_pollitem_t item = {monitor_, 0, ZLINK_POLLIN, 0};
        if (zlink_poll (&item, 1, 100, NULL) <= 0 || (item.revents & ZLINK_POLLIN) == 0)
            continue;

        for (;;) {
            zlink_monitor_event_t event;
            if (recv_monitor_event_from_socket (monitor_, &event, ZLINK_DONTWAIT) != 0) {
                break;
            }
            if (event.event == ZLINK_EVENT_CONNECTION_READY)
                return true;
        }
    }
    return false;
}

bool wait_for_monitor_ready_recv_with_routing_id (void *monitor_,
                                                  int timeout_ms_,
                                                  unsigned char routing_id_[255],
                                                  size_t *routing_id_size_)
{
    const auto deadline =
      std::chrono::steady_clock::now () + std::chrono::milliseconds (timeout_ms_);
    while (std::chrono::steady_clock::now () < deadline) {
        zlink_pollitem_t item = {monitor_, 0, ZLINK_POLLIN, 0};
        if (zlink_poll (&item, 1, 100, NULL) <= 0 || (item.revents & ZLINK_POLLIN) == 0)
            continue;

        for (;;) {
            zlink_monitor_event_t event;
            if (recv_monitor_event_from_socket (monitor_, &event, ZLINK_DONTWAIT) != 0) {
                break;
            }
            if (event.event != ZLINK_EVENT_CONNECTION_READY)
                continue;

            if (routing_id_ && routing_id_size_) {
                *routing_id_size_ = event.routing_id.size;
                if (event.routing_id.size > 0) {
                    memcpy (routing_id_, event.routing_id.data, event.routing_id.size);
                }
            }
            return true;
        }
    }
    return false;
}

bool wait_for_monitor_ready_recv_with_activity (void *monitor_,
                                                void *activity_socket_,
                                                int timeout_ms_,
                                                unsigned char routing_id_[255],
                                                size_t *routing_id_size_)
{
    const int slice_ms = 200;
    return zlink_test_wait_until_step (timeout_ms_, slice_ms, [=] {
        zlink_pollitem_t items[] = {{monitor_, 0, ZLINK_POLLIN, 0},
                                    {activity_socket_, 0, ZLINK_POLLIN, 0}};
        const int rc = zlink_poll (items, 2, slice_ms, NULL);
        if (rc <= 0 || (items[0].revents & ZLINK_POLLIN) == 0)
            return false;

        for (;;) {
            zlink_monitor_event_t event;
            if (recv_monitor_event_from_socket (monitor_, &event, ZLINK_DONTWAIT) != 0) {
                break;
            }
            if (event.event != ZLINK_EVENT_CONNECTION_READY)
                continue;
            if (routing_id_ && routing_id_size_) {
                *routing_id_size_ = event.routing_id.size;
                if (event.routing_id.size > 0)
                    memcpy (routing_id_, event.routing_id.data, event.routing_id.size);
            }
            return true;
        }
        return false;
    });
}

bool wait_for_pubsub_delivery_ready_recv (void *pub_monitor_, void *sub_monitor_, int timeout_ms_)
{
    bool pub_ready = false;
    bool sub_ready = false;
    const auto deadline =
      std::chrono::steady_clock::now () + std::chrono::milliseconds (timeout_ms_);

    while (std::chrono::steady_clock::now () < deadline) {
        zlink_pollitem_t items[] = {{pub_monitor_, 0, ZLINK_POLLIN, 0},
                                    {sub_monitor_, 0, ZLINK_POLLIN, 0}};
        if (zlink_poll (items, 2, 100, NULL) <= 0)
            continue;

        for (int i = 0; i < 2; ++i) {
            if ((items[i].revents & ZLINK_POLLIN) == 0)
                continue;
            for (;;) {
                zlink_monitor_event_t event;
                if (recv_monitor_event_from_socket (i == 0 ? pub_monitor_ : sub_monitor_, &event,
                                                    ZLINK_DONTWAIT)
                    != 0) {
                    break;
                }
                if (event.event == ZLINK_EVENT_CONNECTION_READY) {
                    pub_ready = true;
                }
                if (event.event == ZLINK_EVENT_CONNECTION_READY) {
                    sub_ready = true;
                }
            }
        }

        if (pub_ready && sub_ready)
            return true;
    }

    return false;
}

bool wait_for_monitor_ready_callback_with_routing_id (socket_monitor_probe_t *probe_,
                                                      int timeout_ms_,
                                                      unsigned char routing_id_[255],
                                                      size_t *routing_id_size_)
{
    if (!wait_for_monitor_ready_callback (probe_, timeout_ms_))
        return false;

    if (routing_id_ && routing_id_size_) {
        *routing_id_size_ = probe_->ready_event.routing_id.size;
        if (probe_->ready_event.routing_id.size > 0) {
            memcpy (routing_id_, probe_->ready_event.routing_id.data,
                    probe_->ready_event.routing_id.size);
        }
    }
    return true;
}

void pair_server_handler (const zlink_routing_id_t *,
                          zlink_msg_t *parts_,
                          size_t part_count_,
                          void *)
{
    pair_callback_probe_t *probe = g_pair_server_probe;
    if (!probe || part_count_ != 1) {
        close_message_parts (parts_, part_count_);
        return;
    }

    {
        std::lock_guard<std::mutex> lock (probe->mutex);
        const size_t size = zlink_msg_size (&parts_[0]);
        const size_t copy_size =
          size < sizeof (probe->request_payload) - 1 ? size : sizeof (probe->request_payload) - 1;
        memcpy (probe->request_payload, zlink_msg_data (&parts_[0]), copy_size);
        probe->request_payload[copy_size] = '\0';
        ++probe->request_calls;
    }

    close_message_parts (parts_, part_count_);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_send (probe->socket, "pong", 4, 0));

    {
        std::lock_guard<std::mutex> lock (probe->mutex);
        probe->cv.notify_all ();
    }
}

void pair_client_handler (const zlink_routing_id_t *,
                          zlink_msg_t *parts_,
                          size_t part_count_,
                          void *)
{
    pair_callback_probe_t *probe = g_pair_client_probe;
    if (!probe || part_count_ != 1) {
        close_message_parts (parts_, part_count_);
        return;
    }

    {
        std::lock_guard<std::mutex> lock (probe->mutex);
        const size_t size = zlink_msg_size (&parts_[0]);
        const size_t copy_size =
          size < sizeof (probe->reply_payload) - 1 ? size : sizeof (probe->reply_payload) - 1;
        memcpy (probe->reply_payload, zlink_msg_data (&parts_[0]), copy_size);
        probe->reply_payload[copy_size] = '\0';
        ++probe->reply_calls;
    }

    close_message_parts (parts_, part_count_);
    probe->cv.notify_all ();
}

bool wait_for_pair_callback (pair_callback_probe_t *probe_, bool request_side_, int timeout_ms_)
{
    if (!probe_)
        return false;

    std::unique_lock<std::mutex> lock (probe_->mutex);
    return probe_->cv.wait_for (
      lock, std::chrono::milliseconds (timeout_ms_), [probe_, request_side_] () {
          return request_side_ ? probe_->request_calls > 0 : probe_->reply_calls > 0;
      });
}

void router_server_handler (const zlink_routing_id_t *source_rid_,
                            zlink_msg_t *parts_,
                            size_t part_count_,
                            void *)
{
    raw_callback_probe_t *probe = g_router_server_probe;
    if (!probe || !source_rid_ || part_count_ != 1) {
        close_message_parts (parts_, part_count_);
        return;
    }

    {
        std::lock_guard<std::mutex> lock (probe->mutex);
        probe->rid_size = source_rid_->size;
        memcpy (probe->rid, source_rid_->data, source_rid_->size);
        probe->part_count = part_count_;
        const size_t size = zlink_msg_size (&parts_[0]);
        const size_t copy_size =
          size < sizeof (probe->parts[0]) - 1 ? size : sizeof (probe->parts[0]) - 1;
        memcpy (probe->parts[0], zlink_msg_data (&parts_[0]), copy_size);
        probe->parts[0][copy_size] = '\0';
        ++probe->calls;
    }

    close_message_parts (parts_, part_count_);
    TEST_ASSERT_EQUAL_INT (static_cast<int> (source_rid_->size),
                           TEST_ASSERT_SUCCESS_ERRNO (zlink_send (
                             probe->socket, source_rid_->data, source_rid_->size, ZLINK_SNDMORE)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_send (probe->socket, "pong", 4, 0));
    probe->cv.notify_all ();
}

void raw_client_handler (const zlink_routing_id_t *source_rid_,
                         zlink_msg_t *parts_,
                         size_t part_count_,
                         void *)
{
    raw_callback_probe_t *probe = g_router_client_probe ? g_router_client_probe : g_sub_probe;
    if (!probe || part_count_ == 0) {
        close_message_parts (parts_, part_count_);
        return;
    }

    {
        std::lock_guard<std::mutex> lock (probe->mutex);
        probe->rid_size = source_rid_ ? source_rid_->size : 0;
        if (source_rid_ && source_rid_->size > 0)
            memcpy (probe->rid, source_rid_->data, source_rid_->size);
        probe->part_count = part_count_;
        const int slot = probe->calls < 4 ? probe->calls : 3;
        probe->recorded_part_counts[slot] = part_count_;
        for (size_t i = 0; i < part_count_ && i < 3; ++i) {
            const size_t size = zlink_msg_size (&parts_[i]);
            const size_t copy_size =
              size < sizeof (probe->parts[i]) - 1 ? size : sizeof (probe->parts[i]) - 1;
            memcpy (probe->parts[i], zlink_msg_data (&parts_[i]), copy_size);
            probe->parts[i][copy_size] = '\0';
            memcpy (probe->recorded_parts[slot][i], probe->parts[i], copy_size + 1);
        }
        ++probe->calls;
    }

    close_message_parts (parts_, part_count_);
    probe->cv.notify_all ();
}

void pubsub_subscribe_handler (const zlink_routing_id_t *source_rid_,
                               const char *topic_,
                               size_t topic_len_,
                               zlink_msg_t *parts_,
                               size_t part_count_,
                               void *)
{
    raw_callback_probe_t *probe = g_sub_probe;
    if (!probe || !topic_ || part_count_ == 0) {
        close_message_parts (parts_, part_count_);
        return;
    }

    {
        std::lock_guard<std::mutex> lock (probe->mutex);
        probe->rid_size = source_rid_ ? source_rid_->size : 0;
        if (source_rid_ && source_rid_->size > 0)
            memcpy (probe->rid, source_rid_->data, source_rid_->size);
        probe->part_count = 2;
        const int slot = probe->calls < 4 ? probe->calls : 3;
        probe->recorded_part_counts[slot] = 2;

        const size_t topic_copy =
          topic_len_ < sizeof (probe->parts[0]) - 1 ? topic_len_ : sizeof (probe->parts[0]) - 1;
        memcpy (probe->parts[0], topic_, topic_copy);
        probe->parts[0][topic_copy] = '\0';
        memcpy (probe->recorded_parts[slot][0], probe->parts[0], topic_copy + 1);

        const size_t size = zlink_msg_size (&parts_[0]);
        const size_t payload_copy =
          size < sizeof (probe->parts[1]) - 1 ? size : sizeof (probe->parts[1]) - 1;
        memcpy (probe->parts[1], zlink_msg_data (&parts_[0]), payload_copy);
        probe->parts[1][payload_copy] = '\0';
        memcpy (probe->recorded_parts[slot][1], probe->parts[1], payload_copy + 1);
        ++probe->calls;
    }

    close_message_parts (parts_, part_count_);
    probe->cv.notify_all ();
}

bool wait_for_raw_callback (raw_callback_probe_t *probe_, int timeout_ms_)
{
    if (!probe_)
        return false;

    std::unique_lock<std::mutex> lock (probe_->mutex);
    return probe_->cv.wait_for (lock, std::chrono::milliseconds (timeout_ms_),
                                [probe_] () { return probe_->calls > 0; });
}

bool wait_for_raw_callback_count (raw_callback_probe_t *probe_,
                                  int expected_calls_,
                                  int timeout_ms_)
{
    if (!probe_)
        return false;

    std::unique_lock<std::mutex> lock (probe_->mutex);
    return probe_->cv.wait_for (
      lock, std::chrono::milliseconds (timeout_ms_),
      [probe_, expected_calls_] () { return probe_->calls >= expected_calls_; });
}

void configure_pair_socket (void *socket_)
{
    const int zero = 0;
    const int timeout_ms = 3000;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (socket_, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (socket_, ZLINK_OPT_SNDTIMEO, &timeout_ms, sizeof (timeout_ms)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (socket_, ZLINK_OPT_RCVTIMEO, &timeout_ms, sizeof (timeout_ms)));
}

#if defined(ZLINK_HAVE_WINDOWS)
int connect_raw_tcp (const char *)
{
    errno = EOPNOTSUPP;
    return -1;
}

int set_raw_fd_timeout (int, int)
{
    errno = EOPNOTSUPP;
    return -1;
}

int send_stream_packet (int, const void *, size_t)
{
    errno = EOPNOTSUPP;
    return -1;
}

int recv_stream_packet (int, void *, size_t)
{
    errno = EOPNOTSUPP;
    return -1;
}

int recv_exact (int, void *, size_t)
{
    errno = EOPNOTSUPP;
    return -1;
}

void close_raw_fd (int)
{
}
#else
bool parse_tcp_endpoint (const char *endpoint_, char host_[64], int *port_)
{
    char proto[8] = {0};
    if (sscanf (endpoint_, "%7[^:]://%63[^:]:%d", proto, host_, port_) != 3)
        return false;
    return strcmp (proto, "tcp") == 0;
}

int connect_raw_tcp (const char *endpoint_)
{
    char host[64];
    int port = 0;
    if (!parse_tcp_endpoint (endpoint_, host, &port)) {
        errno = EINVAL;
        return -1;
    }

    const int fd = socket (AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0)
        return -1;

    struct sockaddr_in addr;
    memset (&addr, 0, sizeof (addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons (static_cast<uint16_t> (port));
    if (inet_pton (AF_INET, host, &addr.sin_addr) != 1) {
        close (fd);
        errno = EINVAL;
        return -1;
    }
    if (connect (fd, reinterpret_cast<const struct sockaddr *> (&addr), sizeof (addr)) != 0) {
        const int err = errno;
        close (fd);
        errno = err;
        return -1;
    }
    return fd;
}

int send_stream_packet (int fd_, const void *data_, size_t size_)
{
    size_t off = 0;
    const unsigned char *buf = static_cast<const unsigned char *> (data_);
    while (off < size_) {
        const ssize_t rc = send (fd_, buf + off, size_ - off, 0);
        if (rc > 0) {
            off += static_cast<size_t> (rc);
            continue;
        }
        if (rc < 0 && errno == EINTR)
            continue;
        return -1;
    }
    return 0;
}

int recv_stream_packet (int fd_, void *buf_, size_t cap_)
{
    const ssize_t rc = recv (fd_, static_cast<unsigned char *> (buf_), cap_, 0);
    return rc > 0 ? static_cast<int> (rc) : -1;
}

int recv_exact (int fd_, void *buf_, size_t size_)
{
    size_t off = 0;
    unsigned char *dst = static_cast<unsigned char *> (buf_);
    while (off < size_) {
        const ssize_t rc = recv (fd_, dst + off, size_ - off, 0);
        if (rc > 0) {
            off += static_cast<size_t> (rc);
            continue;
        }
        if (rc < 0 && errno == EINTR)
            continue;
        return -1;
    }
    return 0;
}

int set_raw_fd_timeout (int fd_, int timeout_ms_)
{
    struct timeval tv;
    tv.tv_sec = timeout_ms_ / 1000;
    tv.tv_usec = (timeout_ms_ % 1000) * 1000;
    if (setsockopt (fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof (tv)) != 0)
        return -1;
    if (setsockopt (fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof (tv)) != 0)
        return -1;
    return 0;
}

void close_raw_fd (int fd_)
{
    if (fd_ >= 0)
        close (fd_);
}
#endif

void send_stream_msg (void *socket_,
                      const unsigned char routing_id_[stream_routing_id_size],
                      const char *text_)
{
    TEST_ASSERT_EQUAL_INT (static_cast<int> (stream_routing_id_size),
                           TEST_ASSERT_SUCCESS_ERRNO (zlink_send (
                             socket_, routing_id_, stream_routing_id_size, ZLINK_SNDMORE)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_send (socket_, text_, strlen (text_), 0));
}

bool recv_stream_routing_id_and_payload (void *socket_,
                                         zlink_routing_id_t *rid_out_,
                                         zlink_msg_t *payload_out_)
{
    zlink_msg_t rid_msg;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&rid_msg));
    if (test_recv_single_msg (&rid_msg, socket_, 0) < 0) {
        zlink_msg_close (&rid_msg);
        return false;
    }

    if (zlink_msg_size (&rid_msg) != stream_routing_id_size) {
        zlink_msg_close (&rid_msg);
        errno = EPROTO;
        return false;
    }

    rid_out_->size = static_cast<uint8_t> (stream_routing_id_size);
    memcpy (rid_out_->data, zlink_msg_data (&rid_msg), stream_routing_id_size);
    const bool more = test_msg_has_more (&rid_msg);
    zlink_msg_close (&rid_msg);
    if (!more) {
        errno = EPROTO;
        return false;
    }
    return test_recv_single_msg (payload_out_, socket_, 0) >= 0;
}

void stream_handler (const zlink_routing_id_t *rid_,
                     zlink_msg_t *parts_,
                     size_t part_count_,
                     void *)
{
    stream_callback_probe_t *probe = g_stream_probe;
    if (!probe || !rid_ || rid_->size != stream_routing_id_size || part_count_ == 0) {
        close_message_parts (parts_, part_count_);
        return;
    }

    size_t size = 0;
    {
        std::lock_guard<std::mutex> lock (probe->mutex);
        memcpy (probe->routing_id, rid_->data, stream_routing_id_size);
        size = zlink_msg_size (&parts_[0]);
        const size_t copy_size =
          size < sizeof (probe->payload) - 1 ? size : sizeof (probe->payload) - 1;
        memcpy (probe->payload, zlink_msg_data (&parts_[0]), copy_size);
        probe->payload[copy_size] = '\0';
        ++probe->calls;
    }
    probe->cv.notify_all ();

    const bool send_ok =
      test_stream_send_single_msg (probe->socket, rid_, &parts_[0], 0) == static_cast<int> (size);

    {
        std::lock_guard<std::mutex> lock (probe->mutex);
        probe->send_ok = send_ok;
    }

    for (size_t i = 1; i < part_count_; ++i)
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&parts_[i]));
    parts_[0] = zlink_msg_t ();
    probe->cv.notify_all ();
}

bool wait_for_stream_callback (stream_callback_probe_t *probe_, int timeout_ms_)
{
    if (!probe_)
        return false;

    std::unique_lock<std::mutex> lock (probe_->mutex);
    return probe_->cv.wait_for (lock, std::chrono::milliseconds (timeout_ms_),
                                [probe_] () { return probe_->calls > 0; });
}

bool wait_for_stream_send_ok (stream_callback_probe_t *probe_, int timeout_ms_)
{
    if (!probe_)
        return false;

    std::unique_lock<std::mutex> lock (probe_->mutex);
    return probe_->cv.wait_for (lock, std::chrono::milliseconds (timeout_ms_),
                                [probe_] () { return probe_->send_ok; });
}

void run_pair_ready_matrix (monitor_mode_t monitor_mode_, socket_mode_t socket_mode_)
{
    void *server = test_context_socket (ZLINK_SOCKET_PAIR);
    void *client = test_context_socket (ZLINK_SOCKET_PAIR);
    TEST_ASSERT_NOT_NULL (server);
    TEST_ASSERT_NOT_NULL (client);
    configure_pair_socket (server);
    configure_pair_socket (client);

    pair_callback_probe_t server_probe;
    pair_callback_probe_t client_probe;
    server_probe.socket = server;
    g_pair_server_probe = &server_probe;
    g_pair_client_probe = &client_probe;
    (void) socket_mode_;

    zlink_socket_monitor_open_options_t monitor_opts;
    memset (&monitor_opts, 0, sizeof (monitor_opts));
    monitor_opts.events = ZLINK_EVENT_CONNECTION_READY;
    void *monitor = zlink_socket_monitor_open (server, &monitor_opts);
    TEST_ASSERT_NOT_NULL (monitor);
    configure_pair_socket (monitor);

    socket_monitor_probe_t monitor_probe;
    if (monitor_mode_ == monitor_callback_mode) {
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_socket_monitor_handler (monitor, &socket_monitor_handler, &monitor_probe));
    }

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint));

    const bool ready = monitor_mode_ == monitor_recv_mode
                         ? wait_for_monitor_ready_recv (monitor, 3000)
                         : wait_for_monitor_ready_callback (&monitor_probe, 3000);
    TEST_ASSERT_TRUE (ready);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_send (client, "ping", 4, 0));

    char request_buf[16] = {0};
    TEST_ASSERT_EQUAL_INT (4, zlink_recv (server, request_buf, sizeof (request_buf), 0));
    TEST_ASSERT_EQUAL_STRING ("ping", request_buf);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_send (server, "pong", 4, 0));

    char reply_buf[16] = {0};
    TEST_ASSERT_EQUAL_INT (4, zlink_recv (client, reply_buf, sizeof (reply_buf), 0));
    TEST_ASSERT_EQUAL_STRING ("pong", reply_buf);

    g_pair_server_probe = NULL;
    g_pair_client_probe = NULL;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_monitor_close (&monitor));
    test_context_socket_close_zero_linger (client);
    test_context_socket_close_zero_linger (server);
}

void run_dealer_router_ready_matrix (monitor_mode_t monitor_mode_, socket_mode_t socket_mode_)
{
    void *server = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *client = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (server);
    TEST_ASSERT_NOT_NULL (client);
    configure_pair_socket (server);
    configure_pair_socket (client);

    const char dealer_id[] = "MRX01";
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (client, dealer_id, sizeof (dealer_id) - 1));

    zlink_socket_monitor_open_options_t monitor_opts;
    memset (&monitor_opts, 0, sizeof (monitor_opts));
    monitor_opts.events = ZLINK_EVENT_CONNECTION_READY;
    void *monitor = zlink_socket_monitor_open (server, &monitor_opts);
    TEST_ASSERT_NOT_NULL (monitor);
    configure_pair_socket (monitor);

    socket_monitor_probe_t monitor_probe;
    if (monitor_mode_ == monitor_callback_mode) {
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_socket_monitor_handler (monitor, &socket_monitor_handler, &monitor_probe));
    }

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint));

    unsigned char routing_id[255];
    size_t routing_id_size = 0;
    const bool ready = monitor_mode_ == monitor_recv_mode
                         ? wait_for_monitor_ready_recv_with_activity (monitor, server, 3000,
                                                                      routing_id, &routing_id_size)
                         : wait_for_monitor_ready_callback_with_routing_id (
                             &monitor_probe, 3000, routing_id, &routing_id_size);
    TEST_ASSERT_TRUE (ready);
    TEST_ASSERT_TRUE (routing_id_size > 0);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_send (client, "ping", 4, 0));

    const zlink_routing_id_t *source_node_rid = NULL;
    uint64_t request_seq = 0;
    zlink_msg_t *parts = NULL;
    size_t part_count = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_router_recv (server, &source_node_rid,
                                                  &request_seq, &parts, &part_count, 0));
    TEST_ASSERT_EQUAL_UINT64 (0, request_seq);
    TEST_ASSERT_EQUAL_UINT64 (static_cast<uint64_t> (routing_id_size), source_node_rid->size);
    TEST_ASSERT_EQUAL_MEMORY (routing_id, source_node_rid->data, routing_id_size);
    TEST_ASSERT_EQUAL_UINT64 (1, part_count);
    TEST_ASSERT_EQUAL_UINT64 (4, zlink_msg_size (&parts[0]));
    TEST_ASSERT_EQUAL_MEMORY ("ping", zlink_msg_data (&parts[0]), 4);
    zlink_multipart_close (parts, part_count);

    TEST_ASSERT_EQUAL_INT (static_cast<int> (source_node_rid->size),
                           TEST_ASSERT_SUCCESS_ERRNO (zlink_send (
                             server, source_node_rid->data, source_node_rid->size, ZLINK_SNDMORE)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_send (server, "pong", 4, 0));

    char reply_buf[16] = {0};
    TEST_ASSERT_EQUAL_INT (4, zlink_recv (client, reply_buf, sizeof (reply_buf), 0));
    TEST_ASSERT_EQUAL_STRING ("pong", reply_buf);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_monitor_close (&monitor));
    test_context_socket_close_zero_linger (client);
    test_context_socket_close_zero_linger (server);
}

void run_router_router_ready_matrix (monitor_mode_t monitor_mode_, socket_mode_t socket_mode_)
{
    void *server = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *client = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_NOT_NULL (server);
    TEST_ASSERT_NOT_NULL (client);
    configure_pair_socket (server);
    configure_pair_socket (client);

    const char server_id[] = "SRV01";
    const char client_id[] = "CLT01";
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (server, server_id, sizeof (server_id) - 1));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (client, client_id, sizeof (client_id) - 1));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_router_option (client, ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID,
                                                        server_id, sizeof (server_id) - 1));

    zlink_socket_monitor_open_options_t monitor_opts;
    memset (&monitor_opts, 0, sizeof (monitor_opts));
    monitor_opts.events = ZLINK_EVENT_CONNECTION_READY;
    void *monitor = zlink_socket_monitor_open (server, &monitor_opts);
    TEST_ASSERT_NOT_NULL (monitor);
    configure_pair_socket (monitor);

    socket_monitor_probe_t monitor_probe;
    if (monitor_mode_ == monitor_callback_mode) {
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_socket_monitor_handler (monitor, &socket_monitor_handler, &monitor_probe));
    }

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint));

    unsigned char routing_id[255];
    size_t routing_id_size = 0;
    const bool ready = monitor_mode_ == monitor_recv_mode
                         ? wait_for_monitor_ready_recv_with_activity (monitor, server, 3000,
                                                                      routing_id, &routing_id_size)
                         : wait_for_monitor_ready_callback_with_routing_id (
                             &monitor_probe, 3000, routing_id, &routing_id_size);
    TEST_ASSERT_TRUE (ready);
    TEST_ASSERT_TRUE (routing_id_size > 0);
    TEST_ASSERT_EQUAL_INT (sizeof (client_id) - 1, static_cast<int> (routing_id_size));
    TEST_ASSERT_EQUAL_MEMORY (client_id, routing_id, routing_id_size);

    TEST_ASSERT_EQUAL_INT (static_cast<int> (sizeof (server_id) - 1),
                           TEST_ASSERT_SUCCESS_ERRNO (zlink_send (
                             client, server_id, sizeof (server_id) - 1, ZLINK_SNDMORE)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_send (client, "ping", 4, 0));

    const zlink_routing_id_t *source_node_rid = NULL;
    uint64_t request_seq = 0;
    zlink_msg_t *parts = NULL;
    size_t part_count = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_router_recv (server, &source_node_rid,
                                                  &request_seq, &parts, &part_count, 0));
    TEST_ASSERT_EQUAL_UINT64 (0, request_seq);
    TEST_ASSERT_EQUAL_UINT64 (sizeof (client_id) - 1, source_node_rid->size);
    TEST_ASSERT_EQUAL_MEMORY (client_id, source_node_rid->data, source_node_rid->size);
    TEST_ASSERT_EQUAL_UINT64 (1, part_count);
    TEST_ASSERT_EQUAL_UINT64 (4, zlink_msg_size (&parts[0]));
    TEST_ASSERT_EQUAL_MEMORY ("ping", zlink_msg_data (&parts[0]), 4);
    zlink_multipart_close (parts, part_count);

    TEST_ASSERT_EQUAL_INT (static_cast<int> (source_node_rid->size),
                           TEST_ASSERT_SUCCESS_ERRNO (zlink_send (
                             server, source_node_rid->data, source_node_rid->size, ZLINK_SNDMORE)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_send (server, "pong", 4, 0));

    const zlink_routing_id_t *reply_source_node_rid = NULL;
    uint64_t reply_request_seq = 0;
    zlink_msg_t *reply_parts = NULL;
    size_t reply_part_count = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_router_recv (client, &reply_source_node_rid, &reply_request_seq,
                                                  &reply_parts, &reply_part_count, 0));
    TEST_ASSERT_EQUAL_UINT64 (0, reply_request_seq);
    TEST_ASSERT_EQUAL_UINT64 (sizeof (server_id) - 1, reply_source_node_rid->size);
    TEST_ASSERT_EQUAL_MEMORY (server_id, reply_source_node_rid->data, reply_source_node_rid->size);
    TEST_ASSERT_EQUAL_UINT64 (1, reply_part_count);
    TEST_ASSERT_EQUAL_UINT64 (4, zlink_msg_size (&reply_parts[0]));
    TEST_ASSERT_EQUAL_MEMORY ("pong", zlink_msg_data (&reply_parts[0]), 4);
    zlink_multipart_close (reply_parts, reply_part_count);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_monitor_close (&monitor));
    test_context_socket_close_zero_linger (client);
    test_context_socket_close_zero_linger (server);
}

void run_pubsub_ready_matrix (monitor_mode_t monitor_mode_, socket_mode_t socket_mode_)
{
    void *pub = test_context_socket (ZLINK_SOCKET_PUB);
    void *sub = test_context_socket (ZLINK_SOCKET_SUB);
    TEST_ASSERT_NOT_NULL (pub);
    TEST_ASSERT_NOT_NULL (sub);
    configure_pair_socket (pub);
    configure_pair_socket (sub);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub, "topic"));

    zlink_socket_monitor_open_options_t sub_monitor_opts;
    memset (&sub_monitor_opts, 0, sizeof (sub_monitor_opts));
    sub_monitor_opts.events = ZLINK_EVENT_CONNECTION_READY;
    void *sub_monitor = zlink_socket_monitor_open (sub, &sub_monitor_opts);
    TEST_ASSERT_NOT_NULL (sub_monitor);
    configure_pair_socket (sub_monitor);

    zlink_socket_monitor_open_options_t pub_monitor_opts;
    memset (&pub_monitor_opts, 0, sizeof (pub_monitor_opts));
    pub_monitor_opts.events = ZLINK_EVENT_CONNECTION_READY;
    void *pub_monitor = zlink_socket_monitor_open (pub, &pub_monitor_opts);
    TEST_ASSERT_NOT_NULL (pub_monitor);
    configure_pair_socket (pub_monitor);

    socket_monitor_probe_t sub_monitor_probe;
    socket_monitor_probe_t pub_monitor_probe;
    if (monitor_mode_ == monitor_callback_mode) {
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_socket_monitor_handler (sub_monitor, &socket_monitor_handler, &sub_monitor_probe));
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_socket_monitor_handler (pub_monitor, &socket_monitor_handler, &pub_monitor_probe));
    }

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (pub, endpoint, sizeof (endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sub, endpoint));

    const bool ready =
      monitor_mode_ == monitor_recv_mode
        ? wait_for_pubsub_delivery_ready_recv (pub_monitor, sub_monitor, 3000)
        : wait_for_pubsub_delivery_ready_callback (&pub_monitor_probe, &sub_monitor_probe, 3000);
    TEST_ASSERT_TRUE (ready);

    s_send_seq (pub, "topic", "payload-1", SEQ_END);
    s_send_seq (pub, "topic", "payload-2", SEQ_END);
    s_send_seq (pub, "topic", "payload-3", SEQ_END);

    for (int i = 0; i < 3; ++i) {
        zlink_msg_t *parts = NULL;
        size_t part_count = 0;
        char topic[32] = {0};
        size_t topic_len = sizeof (topic);
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_subscribe (sub, NULL, &parts, &part_count, topic, &topic_len, 0));
        TEST_ASSERT_EQUAL_UINT64 (1, part_count);
        TEST_ASSERT_EQUAL_STRING ("topic", topic);
        if (i == 0)
            TEST_ASSERT_EQUAL_MEMORY ("payload-1", zlink_msg_data (&parts[0]), 9);
        else if (i == 1)
            TEST_ASSERT_EQUAL_MEMORY ("payload-2", zlink_msg_data (&parts[0]), 9);
        else
            TEST_ASSERT_EQUAL_MEMORY ("payload-3", zlink_msg_data (&parts[0]), 9);
        zlink_multipart_close (parts, part_count);
    }

    TEST_ASSERT_SUCCESS_ERRNO (zlink_monitor_close (&pub_monitor));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_monitor_close (&sub_monitor));
    test_context_socket_close_zero_linger (sub);
    test_context_socket_close_zero_linger (pub);
}

void run_stream_ready_matrix (monitor_mode_t monitor_mode_, socket_mode_t socket_mode_)
{
#if defined(ZLINK_HAVE_WINDOWS)
    TEST_IGNORE_MESSAGE ("STREAM raw TCP regression skipped on Windows");
#else
    void *server = test_context_socket (ZLINK_SOCKET_STREAM);
    TEST_ASSERT_NOT_NULL (server);
    configure_pair_socket (server);

    zlink_socket_monitor_open_options_t monitor_opts;
    memset (&monitor_opts, 0, sizeof (monitor_opts));
    monitor_opts.events = ZLINK_EVENT_CONNECTION_READY;
    void *monitor = zlink_socket_monitor_open (server, &monitor_opts);
    TEST_ASSERT_NOT_NULL (monitor);
    configure_pair_socket (monitor);

    socket_monitor_probe_t monitor_probe;
    if (monitor_mode_ == monitor_callback_mode) {
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_socket_monitor_handler (monitor, &socket_monitor_handler, &monitor_probe));
    }

    stream_callback_probe_t stream_probe;
    stream_probe.socket = server;
    g_stream_probe = &stream_probe;
    if (socket_mode_ == socket_callback_mode) {
        TEST_ASSERT_SUCCESS_ERRNO (zlink_recv_handler (server, &stream_handler, NULL));
    }

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));

    const int client_fd = connect_raw_tcp (endpoint);
    TEST_ASSERT_TRUE (client_fd >= 0);
    TEST_ASSERT_EQUAL_INT (0, set_raw_fd_timeout (client_fd, 3000));

    TEST_ASSERT_EQUAL_INT (0, send_stream_packet (client_fd, "cli", 3));

    if (socket_mode_ == socket_recv_mode) {
        unsigned char routing_id[stream_routing_id_size];
        size_t routing_id_size = 0;
        zlink_routing_id_t rid;
        zlink_msg_t payload;
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&payload));
        TEST_ASSERT_TRUE (recv_stream_routing_id_and_payload (server, &rid, &payload));
        const bool ready = monitor_mode_ == monitor_recv_mode
                             ? wait_for_monitor_ready_recv_with_activity (
                                 monitor, server, 3000, routing_id, &routing_id_size)
                             : wait_for_monitor_ready_callback_with_routing_id (
                                 &monitor_probe, 3000, routing_id, &routing_id_size);
        TEST_ASSERT_TRUE (ready);
        TEST_ASSERT_EQUAL_UINT64 (stream_routing_id_size, routing_id_size);
        TEST_ASSERT_EQUAL_UINT64 (stream_routing_id_size, rid.size);
        TEST_ASSERT_EQUAL_MEMORY (routing_id, rid.data, stream_routing_id_size);
        TEST_ASSERT_EQUAL_MEMORY ("cli", zlink_msg_data (&payload), 3);
        TEST_ASSERT_EQUAL_INT (
          3, TEST_ASSERT_SUCCESS_ERRNO (test_stream_send_single_msg (server, &rid, &payload, 0)));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&payload));
    } else {
        unsigned char routing_id[stream_routing_id_size];
        size_t routing_id_size = 0;
        const bool ready =
          monitor_mode_ == monitor_recv_mode
            ? (socket_mode_ == socket_recv_mode
                 ? wait_for_monitor_ready_recv_with_activity (monitor, server, 3000, routing_id,
                                                              &routing_id_size)
                 : wait_for_monitor_ready_recv_with_routing_id (monitor, 3000, routing_id,
                                                                &routing_id_size))
            : wait_for_monitor_ready_callback_with_routing_id (&monitor_probe, 3000, routing_id,
                                                               &routing_id_size);
        TEST_ASSERT_TRUE (ready);
        TEST_ASSERT_EQUAL_UINT64 (stream_routing_id_size, routing_id_size);
        TEST_ASSERT_TRUE (wait_for_stream_callback (&stream_probe, 3000));
        TEST_ASSERT_EQUAL_MEMORY (routing_id, stream_probe.routing_id, stream_routing_id_size);
        TEST_ASSERT_EQUAL_STRING ("cli", stream_probe.payload);
        TEST_ASSERT_TRUE (wait_for_stream_send_ok (&stream_probe, 3000));
    }

    unsigned char echo_buf[4];
    TEST_ASSERT_EQUAL_INT (0, recv_exact (client_fd, echo_buf, 3));
    TEST_ASSERT_EQUAL_UINT8_ARRAY (reinterpret_cast<const unsigned char *> ("cli"), echo_buf, 3);

    close_raw_fd (client_fd);
    g_stream_probe = NULL;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_monitor_close (&monitor));
    test_context_socket_close_zero_linger (server);
#endif
}

size_t count_extra_ready_events (void *monitor_)
{
    //  Readiness has already been observed, so any further ready event is a
    //  second report of the same connection.
    msleep (200);
    size_t extra = 0;
    for (;;) {
        zlink_monitor_event_t event;
        if (recv_monitor_event_from_socket (monitor_, &event, ZLINK_DONTWAIT) != 0)
            break;
        if (event.event == ZLINK_EVENT_CONNECTION_READY)
            ++extra;
    }
    return extra;
}

//  inproc has no engine handshake, so a paired transport reports its
//  Application and Completion lane readiness from the socket itself. Both
//  sockets must observe exactly one ready event per connection, whichever of
//  bind and connect happens first.
void run_inproc_dealer_router_ready (bool connect_before_bind_, const char *endpoint_)
{
    void *server = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *client = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (server);
    TEST_ASSERT_NOT_NULL (client);
    configure_pair_socket (server);
    configure_pair_socket (client);

    const char dealer_id[] = "MRXIN";
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (client, dealer_id, sizeof (dealer_id) - 1));

    zlink_socket_monitor_open_options_t monitor_opts;
    memset (&monitor_opts, 0, sizeof (monitor_opts));
    monitor_opts.events = ZLINK_EVENT_CONNECTION_READY;
    void *server_monitor = zlink_socket_monitor_open (server, &monitor_opts);
    void *client_monitor = zlink_socket_monitor_open (client, &monitor_opts);
    TEST_ASSERT_NOT_NULL (server_monitor);
    TEST_ASSERT_NOT_NULL (client_monitor);
    configure_pair_socket (server_monitor);
    configure_pair_socket (client_monitor);

    if (connect_before_bind_) {
        TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint_));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (server, endpoint_));
    } else {
        TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (server, endpoint_));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint_));
    }

    unsigned char routing_id[255];
    size_t routing_id_size = 0;
    TEST_ASSERT_TRUE (wait_for_monitor_ready_recv_with_activity (
      server_monitor, server, 3000, routing_id, &routing_id_size));
    TEST_ASSERT_TRUE (wait_for_monitor_ready_recv (client_monitor, 3000));

    TEST_ASSERT_EQUAL_UINT64 (0, count_extra_ready_events (server_monitor));
    TEST_ASSERT_EQUAL_UINT64 (0, count_extra_ready_events (client_monitor));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_send (client, "ping", 4, 0));

    const zlink_routing_id_t *source_node_rid = NULL;
    uint64_t request_seq = 0;
    zlink_msg_t *parts = NULL;
    size_t part_count = 0;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_router_recv (server, &source_node_rid, &request_seq, &parts, &part_count, 0));
    TEST_ASSERT_EQUAL_UINT64 (1, part_count);
    TEST_ASSERT_EQUAL_UINT64 (4, zlink_msg_size (&parts[0]));
    TEST_ASSERT_EQUAL_MEMORY ("ping", zlink_msg_data (&parts[0]), 4);
    TEST_ASSERT_EQUAL_UINT64 (sizeof (dealer_id) - 1, source_node_rid->size);
    TEST_ASSERT_EQUAL_MEMORY (dealer_id, source_node_rid->data, sizeof (dealer_id) - 1);
    zlink_multipart_close (parts, part_count);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_monitor_close (&client_monitor));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_monitor_close (&server_monitor));
    test_context_socket_close_zero_linger (client);
    test_context_socket_close_zero_linger (server);
}

//  Two paired peers on one inproc endpoint must be reported as two separate
//  ready connections. A pair readiness key that mixed up the two peers would
//  either merge their lanes into one event or leave one peer unreported.
void run_inproc_two_dealers_ready ()
{
    const char *endpoint = "inproc://monitor_inproc_ready_two_dealers";
    void *server = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_NOT_NULL (server);
    configure_pair_socket (server);

    zlink_socket_monitor_open_options_t monitor_opts;
    memset (&monitor_opts, 0, sizeof (monitor_opts));
    monitor_opts.events = ZLINK_EVENT_CONNECTION_READY;
    void *server_monitor = zlink_socket_monitor_open (server, &monitor_opts);
    TEST_ASSERT_NOT_NULL (server_monitor);
    configure_pair_socket (server_monitor);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (server, endpoint));

    void *clients[2] = {NULL, NULL};
    const char *client_ids[2] = {"MRXI1", "MRXI2"};
    for (size_t i = 0; i != 2; ++i) {
        clients[i] = test_context_socket (ZLINK_SOCKET_DEALER);
        TEST_ASSERT_NOT_NULL (clients[i]);
        configure_pair_socket (clients[i]);
        TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (clients[i], client_ids[i], 5));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (clients[i], endpoint));
        TEST_ASSERT_TRUE (wait_for_monitor_ready_recv (server_monitor, 3000));
    }

    TEST_ASSERT_EQUAL_UINT64 (0, count_extra_ready_events (server_monitor));

    for (size_t i = 0; i != 2; ++i) {
        TEST_ASSERT_SUCCESS_ERRNO (zlink_send (clients[i], "ping", 4, 0));

        const zlink_routing_id_t *source_node_rid = NULL;
        uint64_t request_seq = 0;
        zlink_msg_t *parts = NULL;
        size_t part_count = 0;
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_router_recv (server, &source_node_rid, &request_seq, &parts, &part_count, 0));
        TEST_ASSERT_EQUAL_UINT64 (1, part_count);
        TEST_ASSERT_EQUAL_UINT64 (5, source_node_rid->size);
        TEST_ASSERT_EQUAL_MEMORY (client_ids[i], source_node_rid->data, 5);
        zlink_multipart_close (parts, part_count);
    }

    TEST_ASSERT_SUCCESS_ERRNO (zlink_monitor_close (&server_monitor));
    for (size_t i = 0; i != 2; ++i)
        test_context_socket_close_zero_linger (clients[i]);
    test_context_socket_close_zero_linger (server);
}

#if defined ZLINK_HAVE_WS
void run_ws_dealer_router_ready (bool secure_)
{
    void *server = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *client = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (server);
    TEST_ASSERT_NOT_NULL (client);
    configure_pair_socket (server);
    configure_pair_socket (client);

    const char dealer_id[] = "MRXWS";
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (client, dealer_id, sizeof (dealer_id) - 1));

#if defined ZLINK_HAVE_WSS
    tls_test_files_t files;
    if (secure_) {
        files = make_tls_test_files ();
        const int trust_system = 0;
        TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
          client, ZLINK_OPT_TLS_TRUST_SYSTEM, &trust_system, sizeof (trust_system)));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
          server, ZLINK_OPT_TLS_CERT, files.server_cert.c_str (), files.server_cert.size ()));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
          server, ZLINK_OPT_TLS_KEY, files.server_key.c_str (), files.server_key.size ()));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
          client, ZLINK_OPT_TLS_CA, files.ca_cert.c_str (), files.ca_cert.size ()));
        const char hostname[] = "localhost";
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_set_option (client, ZLINK_OPT_TLS_HOSTNAME, hostname, strlen (hostname)));
    }
#else
    TEST_ASSERT_FALSE (secure_);
#endif

    zlink_socket_monitor_open_options_t monitor_opts;
    memset (&monitor_opts, 0, sizeof (monitor_opts));
    monitor_opts.events = ZLINK_EVENT_CONNECTION_READY;
    void *server_monitor = zlink_socket_monitor_open (server, &monitor_opts);
    void *client_monitor = zlink_socket_monitor_open (client, &monitor_opts);
    TEST_ASSERT_NOT_NULL (server_monitor);
    TEST_ASSERT_NOT_NULL (client_monitor);
    configure_pair_socket (server_monitor);
    configure_pair_socket (client_monitor);

    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (server, secure_ ? "wss://127.0.0.1:*" : "ws://127.0.0.1:*"));
    char endpoint[256];
    size_t endpoint_size = sizeof (endpoint);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_option (server, ZLINK_OPT_LAST_ENDPOINT, endpoint, &endpoint_size));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint));

    unsigned char routing_id[255];
    size_t routing_id_size = 0;
    TEST_ASSERT_TRUE (wait_for_monitor_ready_recv_with_activity (
      server_monitor, server, 5000, routing_id, &routing_id_size));
    TEST_ASSERT_TRUE (wait_for_monitor_ready_recv (client_monitor, 5000));
    TEST_ASSERT_EQUAL_UINT64 (sizeof (dealer_id) - 1, routing_id_size);
    TEST_ASSERT_EQUAL_MEMORY (dealer_id, routing_id, routing_id_size);
    TEST_ASSERT_EQUAL_UINT64 (0, count_extra_ready_events (server_monitor));
    TEST_ASSERT_EQUAL_UINT64 (0, count_extra_ready_events (client_monitor));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_send (client, "ping", 4, 0));
    const zlink_routing_id_t *source_node_rid = NULL;
    uint64_t request_seq = 0;
    zlink_msg_t *parts = NULL;
    size_t part_count = 0;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_router_recv (server, &source_node_rid, &request_seq, &parts, &part_count, 0));
    TEST_ASSERT_EQUAL_UINT64 (1, part_count);
    TEST_ASSERT_EQUAL_MEMORY ("ping", zlink_msg_data (&parts[0]), 4);
    zlink_multipart_close (parts, part_count);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_monitor_close (&client_monitor));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_monitor_close (&server_monitor));
    test_context_socket_close_zero_linger (client);
    test_context_socket_close_zero_linger (server);
#if defined ZLINK_HAVE_WSS
    if (secure_)
        cleanup_tls_test_files (files);
#endif
}
#endif
} // namespace

void test_pair_ready_with_monitor_recv_and_socket_recv ()
{
    run_pair_ready_matrix (monitor_recv_mode, socket_recv_mode);
}

void test_pair_ready_with_monitor_recv_and_socket_callback ()
{
    run_pair_ready_matrix (monitor_recv_mode, socket_callback_mode);
}

void test_pair_ready_with_monitor_callback_and_socket_recv ()
{
    run_pair_ready_matrix (monitor_callback_mode, socket_recv_mode);
}

void test_pair_ready_with_monitor_callback_and_socket_callback ()
{
    run_pair_ready_matrix (monitor_callback_mode, socket_callback_mode);
}

void test_dealer_router_ready_with_monitor_recv_and_socket_recv ()
{
    run_dealer_router_ready_matrix (monitor_recv_mode, socket_recv_mode);
}

void test_dealer_router_ready_with_monitor_recv_and_socket_callback ()
{
    run_dealer_router_ready_matrix (monitor_recv_mode, socket_callback_mode);
}

void test_dealer_router_ready_with_monitor_callback_and_socket_recv ()
{
    run_dealer_router_ready_matrix (monitor_callback_mode, socket_recv_mode);
}

void test_dealer_router_ready_with_monitor_callback_and_socket_callback ()
{
    run_dealer_router_ready_matrix (monitor_callback_mode, socket_callback_mode);
}

void test_inproc_dealer_router_ready_after_bind ()
{
    run_inproc_dealer_router_ready (false, "inproc://monitor_inproc_ready_after_bind");
}

void test_inproc_dealer_router_ready_after_pending_connect ()
{
    run_inproc_dealer_router_ready (true, "inproc://monitor_inproc_ready_pending_connect");
}

void test_inproc_two_dealers_ready_once_each ()
{
    run_inproc_two_dealers_ready ();
}

#if defined ZLINK_HAVE_WS
void test_ws_dealer_router_ready_once_after_both_lanes ()
{
    run_ws_dealer_router_ready (false);
}
#if defined ZLINK_HAVE_WSS
void test_wss_dealer_router_ready_once_after_both_lanes ()
{
    run_ws_dealer_router_ready (true);
}
#endif
#endif

void test_router_router_ready_with_monitor_recv_and_socket_recv ()
{
    run_router_router_ready_matrix (monitor_recv_mode, socket_recv_mode);
}

void test_router_router_ready_with_monitor_recv_and_socket_callback ()
{
    run_router_router_ready_matrix (monitor_recv_mode, socket_callback_mode);
}

void test_router_router_ready_with_monitor_callback_and_socket_recv ()
{
    run_router_router_ready_matrix (monitor_callback_mode, socket_recv_mode);
}

void test_router_router_ready_with_monitor_callback_and_socket_callback ()
{
    run_router_router_ready_matrix (monitor_callback_mode, socket_callback_mode);
}

void test_pubsub_ready_with_monitor_recv_and_socket_recv ()
{
    run_pubsub_ready_matrix (monitor_recv_mode, socket_recv_mode);
}

void test_pubsub_ready_with_monitor_recv_and_socket_callback ()
{
    run_pubsub_ready_matrix (monitor_recv_mode, socket_callback_mode);
}

void test_pubsub_ready_with_monitor_callback_and_socket_recv ()
{
    run_pubsub_ready_matrix (monitor_callback_mode, socket_recv_mode);
}

void test_pubsub_ready_with_monitor_callback_and_socket_callback ()
{
    run_pubsub_ready_matrix (monitor_callback_mode, socket_callback_mode);
}

void test_pubsub_delivery_ready_snapshot_and_reopen_after_ready ()
{
    void *pub = test_context_socket (ZLINK_SOCKET_PUB);
    void *sub = test_context_socket (ZLINK_SOCKET_SUB);
    TEST_ASSERT_NOT_NULL (pub);
    TEST_ASSERT_NOT_NULL (sub);
    configure_pair_socket (pub);
    configure_pair_socket (sub);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub, "topic"));

    zlink_socket_monitor_open_options_t sub_ready_opts;
    memset (&sub_ready_opts, 0, sizeof (sub_ready_opts));
    sub_ready_opts.events = ZLINK_EVENT_CONNECTION_READY;
    void *sub_ready_monitor = zlink_socket_monitor_open (sub, &sub_ready_opts);
    TEST_ASSERT_NOT_NULL (sub_ready_monitor);
    configure_pair_socket (sub_ready_monitor);

    zlink_socket_monitor_open_options_t pub_ready_opts;
    memset (&pub_ready_opts, 0, sizeof (pub_ready_opts));
    pub_ready_opts.events = ZLINK_EVENT_CONNECTION_READY;
    void *pub_ready_monitor = zlink_socket_monitor_open (pub, &pub_ready_opts);
    TEST_ASSERT_NOT_NULL (pub_ready_monitor);
    configure_pair_socket (pub_ready_monitor);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (pub, "inproc://monitor_pubsub_ready_reopen"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sub, "inproc://monitor_pubsub_ready_reopen"));
    TEST_ASSERT_TRUE (
      wait_for_pubsub_delivery_ready_recv (pub_ready_monitor, sub_ready_monitor, 3000));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_monitor_close (&pub_ready_monitor));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_monitor_close (&sub_ready_monitor));

    zlink_socket_monitor_open_options_t snapshot_opts;
    memset (&snapshot_opts, 0, sizeof (snapshot_opts));
    snapshot_opts.events = 0;
    void *snapshot_monitor = zlink_socket_monitor_open (pub, &snapshot_opts);
    TEST_ASSERT_NOT_NULL (snapshot_monitor);
    configure_pair_socket (snapshot_monitor);

    zlink_monitor_status_t status;
    memset (&status, 0, sizeof (status));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_monitor_status (snapshot_monitor, &status));
    TEST_ASSERT_TRUE ((status.state_flags & ZLINK_MONITOR_STATE_READY) != 0);
    TEST_ASSERT_EQUAL_UINT (1u, status.auto_hwm_enabled);
    TEST_ASSERT_TRUE ((status.detail_flags & ZLINK_MONITOR_STATUS_DETAIL_AUTO_HWM_BUDGET) != 0);
    TEST_ASSERT_TRUE ((status.detail_flags & ZLINK_MONITOR_STATUS_DETAIL_AUTO_HWM_BUFFERS) != 0);
    TEST_ASSERT_GREATER_THAN_UINT64 (0, status.auto_hwm_effective_message_bytes);
    TEST_ASSERT_EQUAL_INT (-1, status.auto_hwm_effective_sndbuf);
    TEST_ASSERT_EQUAL_INT (-1, status.auto_hwm_effective_rcvbuf);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_monitor_close (&snapshot_monitor));

    zlink_socket_monitor_open_options_t late_pub_opts;
    memset (&late_pub_opts, 0, sizeof (late_pub_opts));
    late_pub_opts.events = ZLINK_EVENT_CONNECTION_READY;
    void *late_pub_monitor = zlink_socket_monitor_open (pub, &late_pub_opts);
    TEST_ASSERT_NOT_NULL (late_pub_monitor);
    configure_pair_socket (late_pub_monitor);

    memset (&status, 0, sizeof (status));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_monitor_status (late_pub_monitor, &status));
    TEST_ASSERT_TRUE ((status.state_flags & ZLINK_MONITOR_STATE_READY) != 0);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_monitor_close (&late_pub_monitor));
    test_context_socket_close_zero_linger (sub);
    test_context_socket_close_zero_linger (pub);
}

void test_pubsub_delivery_ready_reaches_1000_subscribers ()
{
    const size_t expected_subscribers = 1000;
    const int timeout_ms = 60000;
    void *ctx = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_ctx_set (ctx, ZLINK_MAX_SOCKETS, static_cast<int> (expected_subscribers + 32)));

    std::vector<void *> subs;
    subs.reserve (expected_subscribers);
    void *pub = zlink_socket (ctx, ZLINK_SOCKET_PUB);
    TEST_ASSERT_NOT_NULL (pub);
    configure_pair_socket (pub);

    zlink_socket_monitor_open_options_t pub_ready_opts;
    memset (&pub_ready_opts, 0, sizeof (pub_ready_opts));
    pub_ready_opts.events = ZLINK_EVENT_CONNECTION_READY;
    void *pub_ready_monitor = zlink_socket_monitor_open (pub, &pub_ready_opts);
    TEST_ASSERT_NOT_NULL (pub_ready_monitor);
    configure_pair_socket (pub_ready_monitor);

    delivery_ready_value_probe_t pub_probe;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_socket_monitor_handler (pub_ready_monitor, &delivery_ready_value_handler, &pub_probe));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (pub, "tcp://127.0.0.1:*"));
    char endpoint[MAX_SOCKET_STRING];
    size_t endpoint_size = sizeof (endpoint);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_option (pub, ZLINK_OPT_LAST_ENDPOINT, endpoint, &endpoint_size));

    for (size_t i = 0; i < expected_subscribers; ++i) {
        void *sub = zlink_socket (ctx, ZLINK_SOCKET_SUB);
        TEST_ASSERT_NOT_NULL (sub);
        configure_pair_socket (sub);
        TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub, "topic.bulk"));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sub, endpoint));
        subs.push_back (sub);
    }

    TEST_ASSERT_TRUE (
      wait_for_pub_delivery_ready_value_at_least (&pub_probe, expected_subscribers, timeout_ms));
    TEST_ASSERT_EQUAL_UINT64 (expected_subscribers, pub_probe.last_value);

    zlink_monitor_status_t status;
    memset (&status, 0, sizeof (status));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_monitor_status (pub_ready_monitor, &status));
    TEST_ASSERT_TRUE ((status.state_flags & ZLINK_MONITOR_STATE_READY) != 0);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_monitor_close (&pub_ready_monitor));
    for (size_t i = 0; i < subs.size (); ++i)
        close_zero_linger (subs[i]);
    close_zero_linger (pub);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (ctx));
}

void test_stream_ready_with_monitor_recv_and_socket_recv ()
{
    run_stream_ready_matrix (monitor_recv_mode, socket_recv_mode);
}

void test_stream_ready_with_monitor_recv_and_socket_callback ()
{
    run_stream_ready_matrix (monitor_recv_mode, socket_callback_mode);
}

void test_stream_ready_with_monitor_callback_and_socket_recv ()
{
    run_stream_ready_matrix (monitor_callback_mode, socket_recv_mode);
}

void test_stream_ready_with_monitor_callback_and_socket_callback ()
{
    run_stream_ready_matrix (monitor_callback_mode, socket_callback_mode);
}

namespace
{
struct self_close_probe_t
{
    self_close_probe_t () : monitor (NULL), callbacks (0), close_rc (-1) {}
    std::atomic<void *> monitor;
    std::atomic<int> callbacks;
    std::atomic<int> close_rc;
};

void self_close_monitor_handler (const zlink_monitor_event_t *, void *userdata_)
{
    self_close_probe_t *probe = static_cast<self_close_probe_t *> (userdata_);
    probe->callbacks.fetch_add (1, std::memory_order_relaxed);
    void *monitor = probe->monitor.exchange (NULL, std::memory_order_acq_rel);
    if (monitor)
        probe->close_rc.store (static_cast<int> (zlink_monitor_close (&monitor)),
                               std::memory_order_release);
}
}

void test_monitor_handler_attach_with_queued_events_and_self_close ()
{
    //  Regression for S5-13-01: attaching a handler arms an immediate
    //  dispatch task, and with events already queued its first tick races the
    //  registration commit. A self-close from that first callback must
    //  observe the completed task identity, remove the dispatch task and tear
    //  the monitor down exactly once instead of leaving a periodic task that
    //  reuses freed state.
    for (int i = 0; i != 10; i++) {
        void *server = test_context_socket (ZLINK_SOCKET_PAIR);
        void *client = test_context_socket (ZLINK_SOCKET_PAIR);
        TEST_ASSERT_NOT_NULL (server);
        TEST_ASSERT_NOT_NULL (client);
        configure_pair_socket (server);
        configure_pair_socket (client);

        zlink_socket_monitor_open_options_t monitor_opts;
        memset (&monitor_opts, 0, sizeof (monitor_opts));
        monitor_opts.events = ZLINK_EVENT_ALL;
        void *monitor = zlink_socket_monitor_open (server, &monitor_opts);
        TEST_ASSERT_NOT_NULL (monitor);
        configure_pair_socket (monitor);

        char endpoint[MAX_SOCKET_STRING];
        bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint));
        //  Let listen/accept/ready events queue up before the handler exists.
        msleep (50);

        self_close_probe_t probe;
        probe.monitor.store (monitor, std::memory_order_release);
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_socket_monitor_handler (monitor, &self_close_monitor_handler, &probe));

        void *watch = zlink_stopwatch_start ();
        while (probe.monitor.load (std::memory_order_acquire) != NULL) {
            msleep (1);
            TEST_ASSERT_LESS_OR_EQUAL_MESSAGE (
              3000000UL, zlink_stopwatch_intermediate (watch),
              "Timeout waiting for the self-closing monitor callback");
        }
        zlink_stopwatch_stop (watch);
        TEST_ASSERT_EQUAL_INT (static_cast<int> (ZLINK_CLOSE_OK),
                               probe.close_rc.load (std::memory_order_acquire));
        //  The dispatch task must be gone with the state; give a stray tick
        //  time to crash before teardown if the regression returns.
        msleep (50);

        test_context_socket_close_zero_linger (client);
        test_context_socket_close_zero_linger (server);
    }
}

int main ()
{
    setup_test_environment (120);

    UNITY_BEGIN ();
    RUN_TEST (test_pair_ready_with_monitor_recv_and_socket_recv);
    RUN_TEST (test_pair_ready_with_monitor_recv_and_socket_callback);
    RUN_TEST (test_dealer_router_ready_with_monitor_recv_and_socket_recv);
    RUN_TEST (test_dealer_router_ready_with_monitor_recv_and_socket_callback);
    RUN_TEST (test_inproc_dealer_router_ready_after_bind);
    RUN_TEST (test_inproc_dealer_router_ready_after_pending_connect);
    RUN_TEST (test_inproc_two_dealers_ready_once_each);
#if defined ZLINK_HAVE_WS
    RUN_TEST (test_ws_dealer_router_ready_once_after_both_lanes);
#if defined ZLINK_HAVE_WSS
    RUN_TEST (test_wss_dealer_router_ready_once_after_both_lanes);
#endif
#endif
    RUN_TEST (test_router_router_ready_with_monitor_recv_and_socket_recv);
    RUN_TEST (test_router_router_ready_with_monitor_recv_and_socket_callback);
    RUN_TEST (test_pubsub_ready_with_monitor_recv_and_socket_recv);
    RUN_TEST (test_pubsub_ready_with_monitor_recv_and_socket_callback);
    RUN_TEST (test_pubsub_delivery_ready_snapshot_and_reopen_after_ready);
    RUN_TEST (test_pubsub_delivery_ready_reaches_1000_subscribers);
    RUN_TEST (test_stream_ready_with_monitor_recv_and_socket_recv);
    RUN_TEST (test_stream_ready_with_monitor_recv_and_socket_callback);
    RUN_TEST (test_monitor_handler_attach_with_queued_events_and_self_close);
    return UNITY_END ();
}
