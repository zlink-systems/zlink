/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <atomic>
#include <cstdlib>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
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
const char *const stream_threadsafe_smoke_cases[] = {
  "test_stream_callback_rejects_detach_and_close",
  "test_stream_send_is_thread_safe_across_app_threads",
  "test_stream_send_and_close_race_is_safe",
  "test_stream_send_to_stale_rid_after_disconnect",
};

bool should_run_stream_threadsafe_test (const char *name_)
{
    const char *selected = getenv ("ZLINK_TEST_CASE");
    if (selected && *selected)
        return strcmp (selected, name_) == 0;

    for (size_t i = 0;
         i < sizeof (stream_threadsafe_smoke_cases) / sizeof (stream_threadsafe_smoke_cases[0]);
         ++i) {
        if (strcmp (stream_threadsafe_smoke_cases[i], name_) == 0)
            return true;
    }
    return false;
}

const int kRouteIdSize = 4;
const int kLingerMs = 0;
const int kTimeoutMs = 1000;
const int kSocketBufBytes = 1 << 16;
const int kConcurrentSenders = 4;
const int kMessagesPerSender = 64;
const size_t kPayloadSize = 64;

struct route_probe_t
{
    route_probe_t () : ready (0), rid () { memset (&rid, 0, sizeof (rid)); }

    std::atomic<int> ready;
    zlink_routing_id_t rid;
};

struct lifecycle_probe_t
{
    lifecycle_probe_t () :
        socket (NULL), hits (0), detach_rc (1), detach_errno (0), close_rc (1), close_errno (0)
    {
    }

    void *socket;
    std::atomic<int> hits;
    std::atomic<int> detach_rc;
    std::atomic<int> detach_errno;
    std::atomic<int> close_rc;
    std::atomic<int> close_errno;
};

struct worker_probe_t
{
    worker_probe_t () :
        socket (NULL), has_message (false), done (false), send_rc (-1), send_errno (0)
    {
        memset (&rid, 0, sizeof (rid));
        if (zlink_msg_init (&msg) != 0)
            std::abort ();
    }

    ~worker_probe_t () { (void) zlink_msg_close (&msg); }

    void *socket;
    std::mutex mutex;
    std::condition_variable cv;
    bool has_message;
    bool done;
    int send_rc;
    int send_errno;
    zlink_routing_id_t rid;
    zlink_msg_t msg;
};

struct queued_stream_message_t
{
    queued_stream_message_t ()
    {
        memset (&rid, 0, sizeof (rid));
        if (zlink_msg_init (&msg) != 0)
            std::abort ();
    }

    ~queued_stream_message_t () { (void) zlink_msg_close (&msg); }

    queued_stream_message_t (queued_stream_message_t &&other) noexcept
    {
        memset (&rid, 0, sizeof (rid));
        if (zlink_msg_init (&msg) != 0)
            std::abort ();
        rid = other.rid;
        if (zlink_msg_move (&msg, &other.msg) != 0)
            std::abort ();
    }

    queued_stream_message_t &operator= (queued_stream_message_t &&other) noexcept
    {
        if (this == &other)
            return *this;

        rid = other.rid;
        (void) zlink_msg_close (&msg);
        if (zlink_msg_init (&msg) != 0)
            std::abort ();
        if (zlink_msg_move (&msg, &other.msg) != 0)
            std::abort ();
        return *this;
    }

    zlink_routing_id_t rid;
    zlink_msg_t msg;

  private:
    queued_stream_message_t (const queued_stream_message_t &);
    queued_stream_message_t &operator= (const queued_stream_message_t &);
};

struct stream_raw_stash_t
{
    stream_raw_stash_t () : data (), offset (0) {}

    size_t available () const { return data.size () - offset; }

    void append (const unsigned char *src_, size_t size_)
    {
        if (!src_ || size_ == 0)
            return;

        const size_t old_size = data.size ();
        data.resize (old_size + size_);
        memcpy (&data[old_size], src_, size_);
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
        if (offset > 4096 || offset * 2 >= data.size ()) {
            const size_t remain = data.size () - offset;
            memmove (&data[0], &data[offset], remain);
            data.resize (remain);
            offset = 0;
        }
    }

    void reset ()
    {
        data.clear ();
        offset = 0;
    }

    std::vector<unsigned char> data;
    size_t offset;
};

struct queued_worker_probe_t
{
    queued_worker_probe_t () :
        socket (NULL),
        done (false),
        failed (false),
        enqueued_messages (0),
        expected_messages (0),
        processed_messages (0),
        send_errors (0),
        ready_ticks (0),
        parse_errors (0),
        control_chunks (0),
        stashes (),
        stash_mu ()
    {
    }

    void *socket;
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<queued_stream_message_t> queue;
    bool done;
    bool failed;
    int enqueued_messages;
    int expected_messages;
    int processed_messages;
    int send_errors;
    int ready_ticks;
    int parse_errors;
    int control_chunks;
    std::map<std::string, stream_raw_stash_t> stashes;
    std::mutex stash_mu;
};

route_probe_t *g_route_probe = NULL;
lifecycle_probe_t *g_lifecycle_probe = NULL;
worker_probe_t *g_worker_probe = NULL;
queued_worker_probe_t *g_queued_worker_probe = NULL;

void configure_stream_socket (void *socket_)
{
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (socket_, ZLINK_OPT_LINGER, &kLingerMs, sizeof (kLingerMs)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (socket_, ZLINK_OPT_SNDBUF, &kSocketBufBytes, sizeof (kSocketBufBytes)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (socket_, ZLINK_OPT_RCVBUF, &kSocketBufBytes, sizeof (kSocketBufBytes)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (socket_, ZLINK_OPT_SNDTIMEO, &kTimeoutMs, sizeof (kTimeoutMs)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (socket_, ZLINK_OPT_RCVTIMEO, &kTimeoutMs, sizeof (kTimeoutMs)));
}

#if defined(ZLINK_HAVE_WINDOWS)
int connect_raw_tcp (const char *endpoint_)
{
    LIBZLINK_UNUSED (endpoint_);
    errno = EOPNOTSUPP;
    return -1;
}

void close_raw_fd (int fd_)
{
    LIBZLINK_UNUSED (fd_);
}

int send_all (int fd_, const unsigned char *buf_, size_t size_)
{
    LIBZLINK_UNUSED (fd_);
    LIBZLINK_UNUSED (buf_);
    LIBZLINK_UNUSED (size_);
    errno = EOPNOTSUPP;
    return -1;
}

void set_raw_timeout (int fd_, int timeout_ms_)
{
    LIBZLINK_UNUSED (fd_);
    LIBZLINK_UNUSED (timeout_ms_);
}

int recv_raw (int fd_, unsigned char *buf_, size_t cap_)
{
    LIBZLINK_UNUSED (fd_);
    LIBZLINK_UNUSED (buf_);
    LIBZLINK_UNUSED (cap_);
    errno = EOPNOTSUPP;
    return -1;
}
#else
bool parse_tcp_endpoint (const char *endpoint_, char host_[64], int *port_)
{
    if (!endpoint_ || !host_ || !port_)
        return false;

    char proto[8] = {0};
    int port = 0;
    if (sscanf (endpoint_, "%7[^:]://%63[^:]:%d", proto, host_, &port) != 3)
        return false;
    if (strcmp (proto, "tcp") != 0 || port <= 0 || port > 65535)
        return false;

    *port_ = port;
    return true;
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
        const int err = errno;
        close (fd);
        errno = err;
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

void close_raw_fd (int fd_)
{
    if (fd_ >= 0)
        close (fd_);
}

int send_all (int fd_, const unsigned char *buf_, size_t size_)
{
    size_t off = 0;
    while (off < size_) {
        const ssize_t n = send (fd_, buf_ + off, size_ - off, 0);
        if (n > 0) {
            off += static_cast<size_t> (n);
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        return -1;
    }
    return 0;
}

void set_raw_timeout (int fd_, int timeout_ms_)
{
    struct timeval tv;
    tv.tv_sec = timeout_ms_ / 1000;
    tv.tv_usec = (timeout_ms_ % 1000) * 1000;
    TEST_ASSERT_EQUAL_INT (0, setsockopt (fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof (tv)));
    TEST_ASSERT_EQUAL_INT (0, setsockopt (fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof (tv)));
}

int recv_raw (int fd_, unsigned char *buf_, size_t cap_)
{
    const ssize_t n = recv (fd_, buf_, cap_, 0);
    if (n < 0 && errno == EINTR)
        return recv_raw (fd_, buf_, cap_);
    if (n <= 0)
        return -1;
    return static_cast<int> (n);
}
#endif

bool wait_flag (std::atomic<int> *flag_, int timeout_ms_)
{
    const auto deadline =
      std::chrono::steady_clock::now () + std::chrono::milliseconds (timeout_ms_);
    while (std::chrono::steady_clock::now () < deadline) {
        if (flag_->load (std::memory_order_acquire) != 0)
            return true;
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    return flag_->load (std::memory_order_acquire) != 0;
}

int capture_route_callback (const zlink_routing_id_t *rid_, zlink_msg_t *msg_)
{
    if (g_route_probe && rid_ && msg_ && rid_->size == kRouteIdSize && zlink_msg_size (msg_) > 0
        && g_route_probe->ready.load (std::memory_order_acquire) == 0) {
        g_route_probe->rid.size = rid_->size;
        memcpy (g_route_probe->rid.data, rid_->data, kRouteIdSize);
        g_route_probe->ready.store (1, std::memory_order_release);
    }

    if (msg_)
        (void) zlink_msg_close (msg_);
    return 0;
}

int lifecycle_reject_callback (const zlink_routing_id_t *, zlink_msg_t *msg_)
{
    lifecycle_probe_t *probe = g_lifecycle_probe;
    if (!probe || !probe->socket || !msg_)
        return 0;

    probe->hits.fetch_add (1, std::memory_order_release);

    errno = 0;
    const int detach_rc = zlink_stream_detach (probe->socket);
    probe->detach_rc.store (detach_rc, std::memory_order_release);
    probe->detach_errno.store (errno, std::memory_order_release);

    errno = 0;
    const int close_rc = zlink_close (probe->socket);
    probe->close_rc.store (close_rc, std::memory_order_release);
    probe->close_errno.store (errno, std::memory_order_release);

    (void) zlink_msg_close (msg_);
    return 0;
}

void establish_route (void *server_, int raw_fd_, zlink_routing_id_t *rid_, bool keep_attached_)
{
    route_probe_t probe;
    g_route_probe = &probe;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_stream_attach_raw (server_, capture_route_callback));

    const unsigned char payload = 0xA5;
    TEST_ASSERT_EQUAL_INT (0, send_all (raw_fd_, &payload, sizeof (payload)));
    TEST_ASSERT_TRUE (wait_flag (&probe.ready, 5000));

    *rid_ = probe.rid;
    g_route_probe = NULL;

    if (!keep_attached_)
        TEST_ASSERT_SUCCESS_ERRNO (zlink_stream_detach (server_));
}

void drain_exact_bytes (int fd_,
                        size_t expected_bytes_,
                        std::atomic<size_t> *received_,
                        std::atomic<int> *errors_)
{
    unsigned char buf[4096];
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (10);

    while (received_->load (std::memory_order_acquire) < expected_bytes_
           && std::chrono::steady_clock::now () < deadline) {
        const int n = recv_raw (fd_, buf, sizeof (buf));
        if (n > 0) {
            received_->fetch_add (static_cast<size_t> (n), std::memory_order_release);
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            continue;
        errors_->fetch_add (1, std::memory_order_release);
        return;
    }

    if (received_->load (std::memory_order_acquire) < expected_bytes_)
        errors_->fetch_add (1, std::memory_order_release);
}

bool recv_exact_bytes (int fd_, unsigned char *buf_, size_t size_)
{
    size_t off = 0;
    while (off < size_) {
        const int n = recv_raw (fd_, buf_ + off, size_ - off);
        if (n > 0) {
            off += static_cast<size_t> (n);
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            continue;
        return false;
    }
    return true;
}

bool recv_exact_or_fail (int fd_, unsigned char *buf_, size_t size_)
{
    size_t off = 0;
    while (off < size_) {
        const ssize_t n = recv (fd_, buf_ + off, size_ - off, 0);
        if (n > 0) {
            off += static_cast<size_t> (n);
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        return false;
    }
    return true;
}

static bool is_stream_control_chunk (const unsigned char *data_, size_t size_)
{
    LIBZLINK_UNUSED (data_);
    return size_ == 0;
}

static uint32_t load_u32_be (const unsigned char *src_)
{
    return (static_cast<uint32_t> (src_[0]) << 24) | (static_cast<uint32_t> (src_[1]) << 16)
           | (static_cast<uint32_t> (src_[2]) << 8) | static_cast<uint32_t> (src_[3]);
}

static void store_u32_be (unsigned char *dst_, uint32_t value_)
{
    dst_[0] = static_cast<unsigned char> ((value_ >> 24) & 0xFF);
    dst_[1] = static_cast<unsigned char> ((value_ >> 16) & 0xFF);
    dst_[2] = static_cast<unsigned char> ((value_ >> 8) & 0xFF);
    dst_[3] = static_cast<unsigned char> (value_ & 0xFF);
}

static void run_stream_raw_client_load (const char *endpoint_,
                                        uint32_t client_id_,
                                        int phases_,
                                        int messages_per_phase_,
                                        size_t payload_size_,
                                        std::atomic<int> *client_failures_);

void read_last_endpoint_loop (void *socket_,
                              const char *expected_,
                              std::atomic<int> *stop_,
                              std::atomic<int> *errors_)
{
    char endpoint[MAX_SOCKET_STRING];
    while (stop_->load (std::memory_order_acquire) == 0) {
        size_t size = sizeof (endpoint);
        memset (endpoint, 0, sizeof (endpoint));
        if (zlink_get_option (socket_, ZLINK_OPT_LAST_ENDPOINT, endpoint, &size) != 0) {
            errors_->fetch_add (1, std::memory_order_release);
            return;
        }
        if (!expected_ || expected_[0] == '\0')
            continue;
        if (strncmp (endpoint, expected_, size) != 0) {
            errors_->fetch_add (1, std::memory_order_release);
            return;
        }
    }
}

void read_events_loop (void *socket_, std::atomic<int> *stop_, std::atomic<int> *errors_)
{
    while (stop_->load (std::memory_order_acquire) == 0) {
        int events = 0;
        size_t size = sizeof (events);
        if (zlink_get_option (socket_, ZLINK_OPT_EVENTS, &events, &size) != 0) {
            errors_->fetch_add (1, std::memory_order_release);
            return;
        }
    }
}

void sender_thread_run (void *server_,
                        const zlink_routing_id_t rid_,
                        unsigned char fill_,
                        int messages_,
                        std::atomic<int> *errors_)
{
    std::vector<unsigned char> payload (kPayloadSize, fill_);
    for (int i = 0; i < messages_; ++i) {
        const int rc = test_stream_send_bytes (server_, &rid_, &payload[0], payload.size (), 0);
        if (rc != static_cast<int> (payload.size ())) {
            errors_->fetch_add (1, std::memory_order_release);
            return;
        }
    }
}

int handoff_to_worker_callback (const zlink_routing_id_t *rid_, zlink_msg_t *msg_)
{
    worker_probe_t *probe = g_worker_probe;
    if (!probe || !rid_ || !msg_)
        return 0;

    const unsigned char *payload = static_cast<const unsigned char *> (zlink_msg_data (msg_));
    const size_t payload_size = zlink_msg_size (msg_);
    if (payload_size == 0
        || (payload_size == 1 && payload && (payload[0] == 0x00 || payload[0] == 0x01))) {
        (void) zlink_msg_close (msg_);
        return 0;
    }

    {
        std::lock_guard<std::mutex> lk (probe->mutex);
        if (!probe->has_message) {
            probe->rid = *rid_;
            TEST_ASSERT_EQUAL_INT (0, zlink_msg_move (&probe->msg, msg_));
            probe->has_message = true;
        }
    }
    probe->cv.notify_one ();
    (void) zlink_msg_close (msg_);
    return 0;
}

void worker_send_msg_run (worker_probe_t *probe_)
{
    std::unique_lock<std::mutex> lk (probe_->mutex);
    const bool ready = probe_->cv.wait_for (lk, std::chrono::seconds (5),
                                            [probe_] () { return probe_->has_message; });
    if (!ready) {
        probe_->send_rc = -1;
        probe_->send_errno = ETIMEDOUT;
        probe_->done = true;
        lk.unlock ();
        probe_->cv.notify_one ();
        return;
    }

    const zlink_routing_id_t rid = probe_->rid;
    lk.unlock ();

    errno = 0;
    const int rc = ::test_stream_send_single_msg (probe_->socket, &rid, &probe_->msg, 0);
    const int err = errno;

    lk.lock ();
    probe_->send_rc = rc;
    probe_->send_errno = err;
    probe_->done = true;
    lk.unlock ();
    probe_->cv.notify_one ();
}

void queued_send_ready_handler (void *subject_, void *)
{
    queued_worker_probe_t *probe = static_cast<queued_worker_probe_t *> (subject_);
    if (!probe)
        return;

    {
        std::lock_guard<std::mutex> lk (probe->mutex);
        ++probe->ready_ticks;
    }
    probe->cv.notify_all ();
}

int queue_handoff_callback (const zlink_routing_id_t *rid_, zlink_msg_t *msg_)
{
    queued_worker_probe_t *probe = g_queued_worker_probe;
    if (!probe || !rid_ || !msg_)
        return 0;

    const unsigned char *payload = static_cast<const unsigned char *> (zlink_msg_data (msg_));
    const size_t payload_size = zlink_msg_size (msg_);
    if (is_stream_control_chunk (payload, payload_size)) {
        std::lock_guard<std::mutex> lk (probe->mutex);
        ++probe->control_chunks;
        (void) zlink_msg_close (msg_);
        return 0;
    }

    std::vector<std::vector<unsigned char>> complete_frames;
    bool invalid = false;
    {
        const std::string key (reinterpret_cast<const char *> (rid_->data), rid_->size);
        std::lock_guard<std::mutex> lk (probe->stash_mu);
        stream_raw_stash_t &stash = probe->stashes[key];
        stash.append (payload, payload_size);

        size_t consumed = 0;
        const size_t available = stash.available ();
        while (consumed + 4 <= available) {
            const unsigned char *frame = &stash.data[stash.offset + consumed];
            const size_t body_size = static_cast<size_t> (load_u32_be (frame));

            if (body_size < 8 || body_size > 4 * 1024 * 1024 || body_size != kPayloadSize) {
                invalid = true;
                break;
            }

            const size_t frame_size = 4 + body_size;
            if (consumed + frame_size > available)
                break;

            complete_frames.push_back (std::vector<unsigned char> (frame_size));
            memcpy (&complete_frames.back ()[0], frame, frame_size);
            consumed += frame_size;
        }

        if (invalid) {
            stash.reset ();
        } else if (consumed > 0) {
            stash.offset += consumed;
            stash.compact ();
        }
    }

    (void) zlink_msg_close (msg_);

    if (invalid) {
        std::lock_guard<std::mutex> lk (probe->mutex);
        ++probe->parse_errors;
        return 0;
    }

    {
        std::lock_guard<std::mutex> lk (probe->mutex);
        for (size_t i = 0; i < complete_frames.size (); ++i) {
            queued_stream_message_t queued;
            queued.rid = *rid_;
            TEST_ASSERT_EQUAL_INT (0, zlink_msg_close (&queued.msg));
            TEST_ASSERT_EQUAL_INT (0,
                                   zlink_msg_init_size (&queued.msg, complete_frames[i].size ()));
            memcpy (zlink_msg_data (&queued.msg), &complete_frames[i][0],
                    complete_frames[i].size ());
            probe->queue.push_back (std::move (queued));
        }
        probe->enqueued_messages += static_cast<int> (complete_frames.size ());
    }
    probe->cv.notify_all ();
    return 0;
}

void queue_handoff_msg_handler (const zlink_routing_id_t *rid_,
                                zlink_msg_t *parts_,
                                size_t part_count_,
                                void *)
{
    if (!parts_)
        return;

    for (size_t i = 0; i < part_count_; ++i)
        (void) queue_handoff_callback (rid_, &parts_[i]);
}

void queued_worker_send_run (queued_worker_probe_t *probe_)
{
    queued_stream_message_t blocked;
    bool has_blocked = false;
    int ready_checkpoint = 0;

    for (;;) {
        if (has_blocked) {
            const size_t msg_size = zlink_msg_size (&blocked.msg);
            errno = 0;
            const int rc = test_stream_send_single_msg (probe_->socket, &blocked.rid, &blocked.msg,
                                                        ZLINK_DONTWAIT);
            if (rc == static_cast<int> (msg_size)) {
                std::lock_guard<std::mutex> lk (probe_->mutex);
                ++probe_->processed_messages;
                has_blocked = false;
                if (probe_->processed_messages >= probe_->expected_messages) {
                    probe_->done = true;
                    probe_->cv.notify_all ();
                    return;
                }
                continue;
            }

            if (errno != EAGAIN) {
                std::lock_guard<std::mutex> lk (probe_->mutex);
                ++probe_->send_errors;
                probe_->failed = true;
                probe_->done = true;
                probe_->cv.notify_all ();
                return;
            }

            std::unique_lock<std::mutex> lk (probe_->mutex);
            probe_->cv.wait_for (lk, std::chrono::milliseconds (10), [&] () {
                return probe_->failed || probe_->ready_ticks != ready_checkpoint;
            });
            ready_checkpoint = probe_->ready_ticks;
            continue;
        }

        queued_stream_message_t queued;
        {
            std::unique_lock<std::mutex> lk (probe_->mutex);
            probe_->cv.wait_for (lk, std::chrono::milliseconds (10), [&] () {
                return probe_->failed || probe_->processed_messages >= probe_->expected_messages
                       || !probe_->queue.empty ();
            });

            if (probe_->failed) {
                probe_->done = true;
                probe_->cv.notify_all ();
                return;
            }
            if (probe_->processed_messages >= probe_->expected_messages) {
                probe_->done = true;
                probe_->cv.notify_all ();
                return;
            }
            if (probe_->queue.empty ())
                continue;

            queued = std::move (probe_->queue.front ());
            probe_->queue.pop_front ();
            ready_checkpoint = probe_->ready_ticks;
        }

        errno = 0;
        const size_t msg_size = zlink_msg_size (&queued.msg);
        const int rc =
          test_stream_send_single_msg (probe_->socket, &queued.rid, &queued.msg, ZLINK_DONTWAIT);
        if (rc == static_cast<int> (msg_size)) {
            std::lock_guard<std::mutex> lk (probe_->mutex);
            ++probe_->processed_messages;
            if (probe_->processed_messages >= probe_->expected_messages) {
                probe_->done = true;
                probe_->cv.notify_all ();
                return;
            }
            continue;
        }

        if (errno == EAGAIN) {
            blocked = std::move (queued);
            has_blocked = true;
            continue;
        }

        std::lock_guard<std::mutex> lk (probe_->mutex);
        ++probe_->send_errors;
        probe_->failed = true;
        probe_->done = true;
        probe_->cv.notify_all ();
        return;
    }
}

static void run_stream_raw_client_load (const char *endpoint_,
                                        uint32_t client_id_,
                                        int phases_,
                                        int messages_per_phase_,
                                        size_t payload_size_,
                                        std::atomic<int> *client_failures_)
{
    if (!endpoint_ || !client_failures_ || payload_size_ < 8)
        return;

    std::vector<unsigned char> payload (payload_size_);
    std::vector<unsigned char> frame (payload_size_ + 4);
    std::vector<unsigned char> recv_frame (payload_size_ + 4);

    for (int phase = 0; phase < phases_; ++phase) {
        const int fd = connect_raw_tcp (endpoint_);
        if (fd < 0) {
            client_failures_->fetch_add (1, std::memory_order_release);
            return;
        }

        set_raw_timeout (fd, 5000);

        for (int i = 0; i < messages_per_phase_; ++i) {
            const uint32_t seq = static_cast<uint32_t> (phase * messages_per_phase_ + i);
            store_u32_be (&payload[0], client_id_);
            store_u32_be (&payload[4], seq);
            for (size_t j = 8; j < payload_size_; ++j)
                payload[j] = static_cast<unsigned char> ((client_id_ + seq + j) & 0xFF);

            store_u32_be (&frame[0], static_cast<uint32_t> (payload_size_));
            memcpy (&frame[4], &payload[0], payload_size_);

            const size_t split =
              1 + ((static_cast<size_t> (client_id_) + seq) % (frame.size () - 1));
            if (send_all (fd, &frame[0], split) != 0
                || send_all (fd, &frame[split], frame.size () - split) != 0
                || !recv_exact_or_fail (fd, &recv_frame[0], recv_frame.size ())
                || load_u32_be (&recv_frame[0]) != payload_size_
                || memcmp (&recv_frame[4], &payload[0], payload_size_) != 0) {
                client_failures_->fetch_add (1, std::memory_order_release);
                close_raw_fd (fd);
                return;
            }
        }

        close_raw_fd (fd);
    }
}

static void run_stream_raw_client_timed_window (const char *endpoint_,
                                                uint32_t client_id_,
                                                size_t payload_size_,
                                                std::atomic<int> *stop_,
                                                std::atomic<int> *client_failures_,
                                                std::atomic<int> *sent_messages_)
{
    if (!endpoint_ || !stop_ || !client_failures_ || !sent_messages_ || payload_size_ < 8)
        return;

    const int fd = connect_raw_tcp (endpoint_);
    if (fd < 0) {
        client_failures_->fetch_add (1, std::memory_order_release);
        return;
    }

    set_raw_timeout (fd, 5000);

    std::vector<unsigned char> payload (payload_size_);
    std::vector<unsigned char> frame (payload_size_ + 4);
    std::vector<unsigned char> recv_frame (payload_size_ + 4);
    int local_sent = 0;

    while (stop_->load (std::memory_order_acquire) == 0) {
        const uint32_t seq = static_cast<uint32_t> (local_sent);
        store_u32_be (&payload[0], client_id_);
        store_u32_be (&payload[4], seq);
        for (size_t j = 8; j < payload_size_; ++j)
            payload[j] = static_cast<unsigned char> ((client_id_ + seq + j) & 0xFF);

        store_u32_be (&frame[0], static_cast<uint32_t> (payload_size_));
        memcpy (&frame[4], &payload[0], payload_size_);

        const size_t split = 1 + ((static_cast<size_t> (client_id_) + seq) % (frame.size () - 1));
        if (send_all (fd, &frame[0], split) != 0
            || send_all (fd, &frame[split], frame.size () - split) != 0
            || !recv_exact_or_fail (fd, &recv_frame[0], recv_frame.size ())
            || load_u32_be (&recv_frame[0]) != payload_size_
            || memcmp (&recv_frame[4], &payload[0], payload_size_) != 0) {
            client_failures_->fetch_add (1, std::memory_order_release);
            close_raw_fd (fd);
            return;
        }

        ++local_sent;
    }

    sent_messages_->fetch_add (local_sent, std::memory_order_release);
    close_raw_fd (fd);
}
}

void test_stream_callback_rejects_detach_and_close ()
{
#if defined(ZLINK_HAVE_WINDOWS)
    TEST_IGNORE_MESSAGE ("raw tcp helper unavailable on Windows");
#else
    void *server = test_context_socket (ZLINK_SOCKET_STREAM);
    TEST_ASSERT_NOT_NULL (server);
    configure_stream_socket (server);

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));

    const int raw_fd = connect_raw_tcp (endpoint);
    TEST_ASSERT_GREATER_OR_EQUAL_INT (0, raw_fd);
    set_raw_timeout (raw_fd, 500);

    lifecycle_probe_t probe;
    probe.socket = server;
    g_lifecycle_probe = &probe;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_stream_attach_raw (server, lifecycle_reject_callback));

    const unsigned char payload = 0x41;
    TEST_ASSERT_EQUAL_INT (0, send_all (raw_fd, &payload, sizeof (payload)));
    TEST_ASSERT_TRUE (wait_flag (&probe.hits, 5000));

    TEST_ASSERT_EQUAL_INT (-1, probe.detach_rc.load (std::memory_order_acquire));
    TEST_ASSERT_EQUAL_INT (EBUSY, probe.detach_errno.load (std::memory_order_acquire));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_BUSY, probe.close_rc.load (std::memory_order_acquire));
    TEST_ASSERT_EQUAL_INT (EBUSY, probe.close_errno.load (std::memory_order_acquire));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_stream_detach (server));
    g_lifecycle_probe = NULL;
    close_raw_fd (raw_fd);
    test_context_socket_close_zero_linger (server);
#endif
}

void test_stream_send_is_thread_safe_across_app_threads ()
{
#if defined(ZLINK_HAVE_WINDOWS)
    TEST_IGNORE_MESSAGE ("raw tcp helper unavailable on Windows");
#else
    void *server = test_context_socket (ZLINK_SOCKET_STREAM);
    TEST_ASSERT_NOT_NULL (server);
    configure_stream_socket (server);

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));

    const int raw_fd = connect_raw_tcp (endpoint);
    TEST_ASSERT_GREATER_OR_EQUAL_INT (0, raw_fd);
    set_raw_timeout (raw_fd, 500);

    zlink_routing_id_t rid;
    establish_route (server, raw_fd, &rid, false);

    std::atomic<size_t> received_bytes (0);
    std::atomic<int> recv_errors (0);
    const size_t expected_bytes = kConcurrentSenders * kMessagesPerSender * kPayloadSize;
    std::thread reader (drain_exact_bytes, raw_fd, expected_bytes, &received_bytes, &recv_errors);

    std::atomic<int> send_errors (0);
    std::vector<std::thread> senders;
    for (int i = 0; i < kConcurrentSenders; ++i) {
        senders.push_back (std::thread (sender_thread_run, server, rid,
                                        static_cast<unsigned char> (0x30 + i), kMessagesPerSender,
                                        &send_errors));
    }

    for (size_t i = 0; i < senders.size (); ++i)
        senders[i].join ();
    reader.join ();

    TEST_ASSERT_EQUAL_INT (0, send_errors.load (std::memory_order_acquire));
    TEST_ASSERT_EQUAL_INT (0, recv_errors.load (std::memory_order_acquire));
    TEST_ASSERT_EQUAL_UINT (
      static_cast<unsigned int> (expected_bytes),
      static_cast<unsigned int> (received_bytes.load (std::memory_order_acquire)));

    close_raw_fd (raw_fd);
    test_context_socket_close_zero_linger (server);
#endif
}

void test_stream_send_and_close_race_is_safe ()
{
#if defined(ZLINK_HAVE_WINDOWS)
    TEST_IGNORE_MESSAGE ("raw tcp helper unavailable on Windows");
#else
    void *server = zlink_socket (get_test_context (), ZLINK_SOCKET_STREAM);
    TEST_ASSERT_NOT_NULL (server);
    configure_stream_socket (server);

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));

    const int raw_fd = connect_raw_tcp (endpoint);
    TEST_ASSERT_GREATER_OR_EQUAL_INT (0, raw_fd);
    set_raw_timeout (raw_fd, 200);

    zlink_routing_id_t rid;
    establish_route (server, raw_fd, &rid, false);

    std::atomic<int> send_errors (0);
    std::atomic<int> sender_done (0);
    std::atomic<int> stop_sender (0);
    std::thread sender ([&] () {
        std::vector<unsigned char> payload (kPayloadSize, 0x5A);
        while (stop_sender.load (std::memory_order_acquire) == 0) {
            const int rc = test_stream_send_bytes (server, &rid, &payload[0], payload.size (), 0);
            if (rc == static_cast<int> (payload.size ()))
                continue;

            if (errno == ENOTSOCK || errno == ETERM || errno == EAGAIN || errno == EINTR) {
                break;
            }
            send_errors.fetch_add (1, std::memory_order_release);
            break;
        }
        sender_done.store (1, std::memory_order_release);
    });

    std::this_thread::sleep_for (std::chrono::milliseconds (50));
    errno = 0;
    const zlink_close_result_t first_close = zlink_close (server);
    const int first_close_errno = errno;
    if (first_close != ZLINK_CLOSE_OK)
        stop_sender.store (1, std::memory_order_release);
    sender.join ();

    TEST_ASSERT_TRUE (first_close == ZLINK_CLOSE_OK || first_close == ZLINK_CLOSE_BUSY);
    if (first_close == ZLINK_CLOSE_BUSY) {
        TEST_ASSERT_EQUAL_INT (EBUSY, first_close_errno);
        TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_close (server));
    }

    TEST_ASSERT_EQUAL_INT (0, send_errors.load (std::memory_order_acquire));
    TEST_ASSERT_EQUAL_INT (1, sender_done.load (std::memory_order_acquire));

    close_raw_fd (raw_fd);
#endif
}

void test_stream_callback_handoff_to_worker_thread_send_msg_is_safe ()
{
#if defined(ZLINK_HAVE_WINDOWS)
    TEST_IGNORE_MESSAGE ("raw tcp helper unavailable on Windows");
#else
    void *server = test_context_socket (ZLINK_SOCKET_STREAM);
    TEST_ASSERT_NOT_NULL (server);
    configure_stream_socket (server);

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));

    const int raw_fd = connect_raw_tcp (endpoint);
    TEST_ASSERT_GREATER_OR_EQUAL_INT (0, raw_fd);
    set_raw_timeout (raw_fd, 5000);

    worker_probe_t probe;
    probe.socket = server;
    g_worker_probe = &probe;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_stream_attach_raw (server, handoff_to_worker_callback));

    std::thread worker (worker_send_msg_run, &probe);

    std::vector<unsigned char> payload (kPayloadSize, 0x61);
    TEST_ASSERT_EQUAL_INT (0, send_all (raw_fd, &payload[0], payload.size ()));

    {
        std::unique_lock<std::mutex> lk (probe.mutex);
        TEST_ASSERT_TRUE (
          probe.cv.wait_for (lk, std::chrono::seconds (10), [&probe] () { return probe.done; }));
    }

    std::vector<unsigned char> echoed (payload.size ());
    TEST_ASSERT_TRUE (recv_exact_bytes (raw_fd, &echoed[0], echoed.size ()));
    TEST_ASSERT_EQUAL_INT (0, memcmp (&payload[0], &echoed[0], payload.size ()));

    worker.join ();

    TEST_ASSERT_EQUAL_INT (static_cast<int> (payload.size ()), probe.send_rc);
    TEST_ASSERT_EQUAL_INT (0, probe.send_errno);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_stream_detach (server));
    g_worker_probe = NULL;
    close_raw_fd (raw_fd);
    test_context_socket_close_zero_linger (server);
#endif
}

void test_stream_callback_queue_handoff_with_send_ready_under_load_is_safe ()
{
#if defined(ZLINK_HAVE_WINDOWS)
    TEST_IGNORE_MESSAGE ("raw tcp helper unavailable on Windows");
#else
    const int client_count = 4;
    const int messages_per_client = 32;
    const size_t payload_size = 64;
    const int expected_messages = client_count * messages_per_client;

    void *server = test_context_socket (ZLINK_SOCKET_STREAM);
    TEST_ASSERT_NOT_NULL (server);
    configure_stream_socket (server);

    const uint64_t hwm = 10u * (payload_size + sizeof (zlink_msg_t));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (server, ZLINK_OPT_SNDHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (server, ZLINK_OPT_RCVHWM, &hwm, sizeof (hwm)));

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));

    queued_worker_probe_t probe;
    probe.socket = server;
    probe.expected_messages = expected_messages;
    g_queued_worker_probe = &probe;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_send_ready_handler (server, &queued_send_ready_handler, &probe));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_stream_attach_raw (server, queue_handoff_callback));

    std::thread worker (queued_worker_send_run, &probe);

    std::atomic<int> client_failures (0);
    std::vector<std::thread> clients;
    clients.reserve (client_count);
    for (int i = 0; i < client_count; ++i) {
        clients.push_back (std::thread (run_stream_raw_client_load, endpoint,
                                        static_cast<uint32_t> (i), 1, messages_per_client,
                                        payload_size, &client_failures));
    }

    for (size_t i = 0; i < clients.size (); ++i)
        clients[i].join ();

    {
        std::unique_lock<std::mutex> lk (probe.mutex);
        TEST_ASSERT_TRUE (
          probe.cv.wait_for (lk, std::chrono::seconds (10), [&probe] () { return probe.done; }));
    }

    worker.join ();

    {
        std::lock_guard<std::mutex> lk (probe.mutex);
        TEST_ASSERT_FALSE (probe.failed);
        TEST_ASSERT_EQUAL_INT (0, probe.send_errors);
        TEST_ASSERT_EQUAL_INT (expected_messages, probe.processed_messages);
        TEST_ASSERT_TRUE (probe.queue.empty ());
    }
    TEST_ASSERT_EQUAL_INT (0, client_failures.load (std::memory_order_acquire));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_stream_detach (server));
    g_queued_worker_probe = NULL;
    test_context_socket_close_zero_linger (server);
#endif
}

void test_stream_recv_handler_queue_handoff_with_send_ready_under_load_is_safe ()
{
#if defined(ZLINK_HAVE_WINDOWS)
    TEST_IGNORE_MESSAGE ("raw tcp helper unavailable on Windows");
#else
    const int client_count = 4;
    const int messages_per_client = 32;
    const size_t payload_size = 64;
    const int expected_messages = client_count * messages_per_client;

    void *server = test_context_socket (ZLINK_SOCKET_STREAM);
    TEST_ASSERT_NOT_NULL (server);
    configure_stream_socket (server);

    const uint64_t hwm = 10u * (payload_size + sizeof (zlink_msg_t));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (server, ZLINK_OPT_SNDHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (server, ZLINK_OPT_RCVHWM, &hwm, sizeof (hwm)));

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));

    queued_worker_probe_t probe;
    probe.socket = server;
    probe.expected_messages = expected_messages;
    g_queued_worker_probe = &probe;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_send_ready_handler (server, &queued_send_ready_handler, &probe));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_recv_handler (server, &queue_handoff_msg_handler, NULL));

    std::thread worker (queued_worker_send_run, &probe);

    std::atomic<int> client_failures (0);
    std::vector<std::thread> clients;
    clients.reserve (client_count);
    for (int i = 0; i < client_count; ++i) {
        clients.push_back (std::thread (run_stream_raw_client_load, endpoint,
                                        static_cast<uint32_t> (i), 1, messages_per_client,
                                        payload_size, &client_failures));
    }

    for (size_t i = 0; i < clients.size (); ++i)
        clients[i].join ();

    {
        std::unique_lock<std::mutex> lk (probe.mutex);
        TEST_ASSERT_TRUE (
          probe.cv.wait_for (lk, std::chrono::seconds (10), [&probe] () { return probe.done; }));
    }

    worker.join ();

    {
        std::lock_guard<std::mutex> lk (probe.mutex);
        TEST_ASSERT_FALSE (probe.failed);
        TEST_ASSERT_EQUAL_INT (0, probe.send_errors);
        TEST_ASSERT_EQUAL_INT (expected_messages, probe.processed_messages);
        TEST_ASSERT_TRUE (probe.queue.empty ());
    }
    TEST_ASSERT_EQUAL_INT (0, client_failures.load (std::memory_order_acquire));

    g_queued_worker_probe = NULL;
    test_context_socket_close_zero_linger (server);
#endif
}

void test_stream_recv_handler_queue_handoff_with_send_ready_timed_window_drains_cleanly ()
{
#if defined(ZLINK_HAVE_WINDOWS)
    TEST_IGNORE_MESSAGE ("raw tcp helper unavailable on Windows");
#else
    const int client_count = 4;
    const size_t payload_size = 64;
    const int run_ms = 1500;

    void *server = test_context_socket (ZLINK_SOCKET_STREAM);
    TEST_ASSERT_NOT_NULL (server);
    configure_stream_socket (server);

    const uint64_t hwm = 10u * (payload_size + sizeof (zlink_msg_t));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (server, ZLINK_OPT_SNDHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (server, ZLINK_OPT_RCVHWM, &hwm, sizeof (hwm)));

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));

    queued_worker_probe_t probe;
    probe.socket = server;
    probe.expected_messages = INT_MAX / 2;
    g_queued_worker_probe = &probe;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_send_ready_handler (server, &queued_send_ready_handler, &probe));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_recv_handler (server, &queue_handoff_msg_handler, NULL));

    std::thread worker (queued_worker_send_run, &probe);

    std::atomic<int> stop (0);
    std::atomic<int> client_failures (0);
    std::atomic<int> sent_messages (0);
    std::vector<std::thread> clients;
    clients.reserve (client_count);
    for (int i = 0; i < client_count; ++i) {
        clients.push_back (std::thread (run_stream_raw_client_timed_window, endpoint,
                                        static_cast<uint32_t> (i), payload_size, &stop,
                                        &client_failures, &sent_messages));
    }

    std::this_thread::sleep_for (std::chrono::milliseconds (run_ms));
    stop.store (1, std::memory_order_release);

    for (size_t i = 0; i < clients.size (); ++i)
        clients[i].join ();

    {
        std::lock_guard<std::mutex> lk (probe.mutex);
        probe.expected_messages = probe.enqueued_messages;
    }
    probe.cv.notify_all ();

    {
        std::unique_lock<std::mutex> lk (probe.mutex);
        TEST_ASSERT_TRUE (
          probe.cv.wait_for (lk, std::chrono::seconds (10), [&probe] () { return probe.done; }));
    }

    worker.join ();

    {
        std::lock_guard<std::mutex> lk (probe.mutex);
        TEST_ASSERT_FALSE (probe.failed);
        TEST_ASSERT_EQUAL_INT (0, probe.send_errors);
        TEST_ASSERT_EQUAL_INT (0, probe.parse_errors);
        TEST_ASSERT_TRUE (probe.queue.empty ());
        TEST_ASSERT_GREATER_THAN_INT (0, probe.enqueued_messages);
        TEST_ASSERT_EQUAL_INT (probe.enqueued_messages, probe.processed_messages);
    }
    TEST_ASSERT_EQUAL_INT (0, client_failures.load (std::memory_order_acquire));
    TEST_ASSERT_EQUAL_INT (sent_messages.load (std::memory_order_acquire),
                           probe.processed_messages);

    g_queued_worker_probe = NULL;
    test_context_socket_close_zero_linger (server);
#endif
}

///////////////////////////////////////////////////////////////////////////////
// Regression: send_msg from multiple app threads is thread-safe
///////////////////////////////////////////////////////////////////////////////

void test_stream_send_msg_is_thread_safe ()
{
#if defined(ZLINK_HAVE_WINDOWS)
    TEST_IGNORE_MESSAGE ("raw tcp helper unavailable on Windows");
#else
    void *server = test_context_socket (ZLINK_SOCKET_STREAM);
    TEST_ASSERT_NOT_NULL (server);
    configure_stream_socket (server);

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));

    const int raw_fd = connect_raw_tcp (endpoint);
    TEST_ASSERT_GREATER_OR_EQUAL_INT (0, raw_fd);
    set_raw_timeout (raw_fd, 500);

    zlink_routing_id_t rid;
    establish_route (server, raw_fd, &rid, false);

    const int send_msg_threads = 4;
    const int msgs_per_thread = 32;
    std::atomic<int> send_errors (0);

    auto msg_sender = [&] (unsigned char fill_) {
        for (int i = 0; i < msgs_per_thread; ++i) {
            zlink_msg_t msg;
            if (zlink_msg_init_size (&msg, kPayloadSize) != 0) {
                send_errors.fetch_add (1, std::memory_order_release);
                return;
            }
            memset (zlink_msg_data (&msg), fill_, kPayloadSize);

            const int rc = test_stream_send_single_msg (server, &rid, &msg, 0);
            if (rc != static_cast<int> (kPayloadSize)) {
                send_errors.fetch_add (1, std::memory_order_release);
                zlink_msg_close (&msg);
                return;
            }
        }
    };

    std::atomic<size_t> received_bytes (0);
    std::atomic<int> recv_errors (0);
    const size_t expected_bytes = send_msg_threads * msgs_per_thread * kPayloadSize;
    std::thread reader (drain_exact_bytes, raw_fd, expected_bytes, &received_bytes, &recv_errors);

    std::vector<std::thread> senders;
    for (int i = 0; i < send_msg_threads; ++i) {
        senders.push_back (std::thread (msg_sender, static_cast<unsigned char> (0x40 + i)));
    }

    for (size_t i = 0; i < senders.size (); ++i)
        senders[i].join ();
    reader.join ();

    TEST_ASSERT_EQUAL_INT (0, send_errors.load (std::memory_order_acquire));
    TEST_ASSERT_EQUAL_INT (0, recv_errors.load (std::memory_order_acquire));
    TEST_ASSERT_EQUAL_UINT (
      static_cast<unsigned int> (expected_bytes),
      static_cast<unsigned int> (received_bytes.load (std::memory_order_acquire)));

    close_raw_fd (raw_fd);
    test_context_socket_close_zero_linger (server);
#endif
}

void test_stream_runtime_reads_are_safe_during_send ()
{
#if defined(ZLINK_HAVE_WINDOWS)
    TEST_IGNORE_MESSAGE ("raw tcp helper unavailable on Windows");
#else
    void *server = test_context_socket (ZLINK_SOCKET_STREAM);
    TEST_ASSERT_NOT_NULL (server);
    configure_stream_socket (server);

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));

    const int raw_fd = connect_raw_tcp (endpoint);
    TEST_ASSERT_GREATER_OR_EQUAL_INT (0, raw_fd);
    set_raw_timeout (raw_fd, 500);

    zlink_routing_id_t rid;
    establish_route (server, raw_fd, &rid, false);

    std::atomic<int> stop_reads (0);
    std::atomic<int> read_errors (0);
    std::thread endpoint_reader (read_last_endpoint_loop, server, endpoint, &stop_reads,
                                 &read_errors);
    std::thread events_reader (read_events_loop, server, &stop_reads, &read_errors);

    std::atomic<size_t> received_bytes (0);
    std::atomic<int> recv_errors (0);
    const size_t expected_bytes = kConcurrentSenders * kMessagesPerSender * kPayloadSize;
    std::thread reader (drain_exact_bytes, raw_fd, expected_bytes, &received_bytes, &recv_errors);

    std::atomic<int> send_errors (0);
    std::vector<std::thread> senders;
    for (int i = 0; i < kConcurrentSenders; ++i) {
        senders.push_back (std::thread (sender_thread_run, server, rid,
                                        static_cast<unsigned char> (0x61 + i), kMessagesPerSender,
                                        &send_errors));
    }

    for (size_t i = 0; i < senders.size (); ++i)
        senders[i].join ();
    reader.join ();

    stop_reads.store (1, std::memory_order_release);
    endpoint_reader.join ();
    events_reader.join ();

    TEST_ASSERT_EQUAL_INT (0, send_errors.load (std::memory_order_acquire));
    TEST_ASSERT_EQUAL_INT (0, recv_errors.load (std::memory_order_acquire));
    TEST_ASSERT_EQUAL_INT (0, read_errors.load (std::memory_order_acquire));

    close_raw_fd (raw_fd);
    test_context_socket_close_zero_linger (server);
#endif
}

void test_socket_runtime_reads_are_safe_during_connect_disconnect ()
{
    void *server = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *client = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (server);
    TEST_ASSERT_NOT_NULL (client);
    configure_stream_socket (server);
    configure_stream_socket (client);

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint));

    std::atomic<int> stop_reads (0);
    std::atomic<int> read_errors (0);
    std::thread endpoint_reader (read_last_endpoint_loop, client, static_cast<const char *> (NULL),
                                 &stop_reads, &read_errors);

    std::atomic<int> control_errors (0);
    std::thread mutator ([&] () {
        for (int i = 0; i < 64; ++i) {
            if (zlink_disconnect (client, endpoint) != ZLINK_CONNECT_OK)
                control_errors.fetch_add (1, std::memory_order_release);
            if (zlink_connect (client, endpoint) != ZLINK_CONNECT_OK)
                control_errors.fetch_add (1, std::memory_order_release);
        }
    });

    mutator.join ();
    stop_reads.store (1, std::memory_order_release);
    endpoint_reader.join ();

    TEST_ASSERT_EQUAL_INT (0, control_errors.load (std::memory_order_acquire));
    TEST_ASSERT_EQUAL_INT (0, read_errors.load (std::memory_order_acquire));

    test_context_socket_close_zero_linger (client);
    test_context_socket_close_zero_linger (server);
}

///////////////////////////////////////////////////////////////////////////////
// Regression: many rapid client connect/disconnect during active sends
///////////////////////////////////////////////////////////////////////////////

void test_stream_rapid_client_churn_during_send ()
{
#if defined(ZLINK_HAVE_WINDOWS)
    TEST_IGNORE_MESSAGE ("raw tcp helper unavailable on Windows");
#else
    void *server = test_context_socket (ZLINK_SOCKET_STREAM);
    TEST_ASSERT_NOT_NULL (server);
    configure_stream_socket (server);

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));

    //  Establish a persistent client that sends/receives throughout
    const int persistent_fd = connect_raw_tcp (endpoint);
    TEST_ASSERT_GREATER_OR_EQUAL_INT (0, persistent_fd);
    set_raw_timeout (persistent_fd, 1000);

    zlink_routing_id_t persistent_rid;
    establish_route (server, persistent_fd, &persistent_rid, false);

    //  In one thread, send a stream of messages to the persistent client
    std::atomic<int> send_done (0);
    std::atomic<int> send_errors (0);
    const int total_sends = 128;
    std::thread sender ([&] () {
        std::vector<unsigned char> payload (kPayloadSize, 0xEE);
        for (int i = 0; i < total_sends; ++i) {
            const int rc =
              test_stream_send_bytes (server, &persistent_rid, &payload[0], payload.size (), 0);
            if (rc != static_cast<int> (payload.size ())) {
                send_errors.fetch_add (1, std::memory_order_release);
            }
        }
        send_done.store (1, std::memory_order_release);
    });

    //  In another thread, rapidly connect/disconnect ephemeral clients
    const int churn_count = 16;
    std::thread churner ([&] () {
        for (int i = 0; i < churn_count; ++i) {
            const int fd = connect_raw_tcp (endpoint);
            if (fd >= 0) {
                const unsigned char ping = 0x01;
                (void) send_all (fd, &ping, sizeof (ping));
                std::this_thread::sleep_for (std::chrono::milliseconds (1));
                close_raw_fd (fd);
            }
        }
    });

    //  Drain on the persistent client side
    std::atomic<size_t> received_bytes (0);
    std::atomic<int> recv_errors (0);
    const size_t expected_bytes = total_sends * kPayloadSize;
    std::thread reader (drain_exact_bytes, persistent_fd, expected_bytes, &received_bytes,
                        &recv_errors);

    sender.join ();
    churner.join ();
    reader.join ();

    TEST_ASSERT_EQUAL_INT (0, send_errors.load (std::memory_order_acquire));
    TEST_ASSERT_EQUAL_INT (0, recv_errors.load (std::memory_order_acquire));
    TEST_ASSERT_EQUAL_UINT (
      static_cast<unsigned int> (expected_bytes),
      static_cast<unsigned int> (received_bytes.load (std::memory_order_acquire)));

    close_raw_fd (persistent_fd);
    test_context_socket_close_zero_linger (server);
#endif
}

///////////////////////////////////////////////////////////////////////////////
// Regression: send to stale routing_id after client disconnects
///////////////////////////////////////////////////////////////////////////////

void test_stream_send_to_stale_rid_after_disconnect ()
{
#if defined(ZLINK_HAVE_WINDOWS)
    TEST_IGNORE_MESSAGE ("raw tcp helper unavailable on Windows");
#else
    void *server = test_context_socket (ZLINK_SOCKET_STREAM);
    TEST_ASSERT_NOT_NULL (server);
    configure_stream_socket (server);

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));

    const int raw_fd = connect_raw_tcp (endpoint);
    TEST_ASSERT_GREATER_OR_EQUAL_INT (0, raw_fd);
    set_raw_timeout (raw_fd, 500);

    zlink_routing_id_t rid;
    establish_route (server, raw_fd, &rid, false);

    //  Verify send works while connected
    std::vector<unsigned char> payload (kPayloadSize, 0x77);
    TEST_ASSERT_EQUAL_INT (static_cast<int> (payload.size ()),
                           test_stream_send_bytes (server, &rid, &payload[0], payload.size (), 0));

    //  Disconnect the raw client
    close_raw_fd (raw_fd);

    //  Allow disconnect to propagate
    msleep (SETTLE_TIME);

    //  Send to the stale routing_id should fail gracefully, not crash
    const int rc = test_stream_send_bytes (server, &rid, &payload[0], payload.size (), 0);
    //  Either returns error or succeeds (buffered) — the key assertion is no crash
    LIBZLINK_UNUSED (rc);

    test_context_socket_close_zero_linger (server);
#endif
}

int main ()
{
    setup_test_environment ();

    UNITY_BEGIN ();
    if (should_run_stream_threadsafe_test ("test_stream_callback_rejects_detach_and_close"))
        RUN_TEST (test_stream_callback_rejects_detach_and_close);
    if (should_run_stream_threadsafe_test ("test_stream_send_is_thread_safe_across_app_threads"))
        RUN_TEST (test_stream_send_is_thread_safe_across_app_threads);
    if (should_run_stream_threadsafe_test ("test_stream_send_and_close_race_is_safe"))
        RUN_TEST (test_stream_send_and_close_race_is_safe);
    if (should_run_stream_threadsafe_test (
          "test_stream_callback_handoff_to_worker_thread_send_msg_is_safe"))
        RUN_TEST (test_stream_callback_handoff_to_worker_thread_send_msg_is_safe);
    if (should_run_stream_threadsafe_test (
          "test_stream_callback_queue_handoff_with_send_ready_under_load_is_safe"))
        RUN_TEST (test_stream_callback_queue_handoff_with_send_ready_under_load_is_safe);
    if (should_run_stream_threadsafe_test (
          "test_stream_recv_handler_queue_handoff_with_send_ready_under_load_is_safe"))
        RUN_TEST (test_stream_recv_handler_queue_handoff_with_send_ready_under_load_is_safe);
    if (should_run_stream_threadsafe_test (
          "test_stream_recv_handler_queue_handoff_with_send_ready_timed_window_drains_cleanly"))
        RUN_TEST (
          test_stream_recv_handler_queue_handoff_with_send_ready_timed_window_drains_cleanly);
    if (should_run_stream_threadsafe_test ("test_stream_send_msg_is_thread_safe"))
        RUN_TEST (test_stream_send_msg_is_thread_safe);
    if (should_run_stream_threadsafe_test ("test_stream_runtime_reads_are_safe_during_send"))
        RUN_TEST (test_stream_runtime_reads_are_safe_during_send);
    if (should_run_stream_threadsafe_test (
          "test_socket_runtime_reads_are_safe_during_connect_disconnect"))
        RUN_TEST (test_socket_runtime_reads_are_safe_during_connect_disconnect);
    if (should_run_stream_threadsafe_test ("test_stream_rapid_client_churn_during_send"))
        RUN_TEST (test_stream_rapid_client_churn_during_send);
    if (should_run_stream_threadsafe_test ("test_stream_send_to_stale_rid_after_disconnect"))
        RUN_TEST (test_stream_send_to_stale_rid_after_disconnect);
    return UNITY_END ();
}
