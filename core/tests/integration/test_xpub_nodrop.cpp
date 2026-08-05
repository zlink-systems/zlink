/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <chrono>
#include <cstring>
#include <future>
#include <atomic>

SETUP_TEARDOWN_TESTCONTEXT

namespace
{
const int kTcpSendTimeoutMs = 2000;
const int kPubSendTimeoutMs = 200;
const int kTcpDrainCompletionTimeoutSec = 15;
const char *kPubsubTopic = "bench";
const size_t kPubsubPayloadSize = 256;
const uint64_t kTcpPipeHwm =
  100u * (kPubsubPayloadSize + sizeof (zlink_msg_t));
const uint64_t kTcpSocketHwm = kTcpPipeHwm / 2;

struct delivery_ready_state_t
{
    delivery_ready_state_t () : ready (false), error_code (0) {}

    std::mutex sync;
    std::condition_variable cv;
    bool ready;
    int error_code;
};

struct delivery_ready_monitor_t
{
    delivery_ready_monitor_t () : monitor (NULL), state (NULL) {}

    void *monitor;
    delivery_ready_state_t *state;
};

struct pubsub_callback_counter_t
{
    pubsub_callback_counter_t () : received (0), error (0) {}

    std::atomic<int> received;
    std::atomic<int> error;
    std::mutex sync;
    std::condition_variable cv;
};

void delivery_ready_monitor_handler (const zlink_monitor_event_t *event_, void *userdata_)
{
    delivery_ready_state_t *state = static_cast<delivery_ready_state_t *> (userdata_);
    if (!state || !event_)
        return;

    std::unique_lock<std::mutex> lock (state->sync);
    switch (event_->event) {
        case ZLINK_EVENT_CONNECTION_READY:
            state->ready = true;
            break;
        case ZLINK_EVENT_BIND_FAILED:
        case ZLINK_EVENT_ACCEPT_FAILED:
        case ZLINK_EVENT_CLOSE_FAILED:
        case ZLINK_EVENT_HANDSHAKE_FAILED_NO_DETAIL:
        case ZLINK_EVENT_HANDSHAKE_FAILED_PROTOCOL:
        case ZLINK_EVENT_HANDSHAKE_FAILED_AUTH:
            state->error_code = event_->value != 0 ? static_cast<int> (event_->value) : EIO;
            break;
        default:
            break;
    }
    lock.unlock ();
    state->cv.notify_all ();
}

void configure_pubsub_hwm (void *socket_)
{
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (socket_, ZLINK_OPT_SNDHWM, &kTcpSocketHwm, sizeof (kTcpSocketHwm)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (socket_, ZLINK_OPT_RCVHWM, &kTcpSocketHwm, sizeof (kTcpSocketHwm)));
}

bool open_delivery_ready_monitor (void *socket_, uint64_t event_, delivery_ready_monitor_t *out_)
{
    if (!socket_ || !out_)
        return false;

    delivery_ready_state_t *state = new (std::nothrow) delivery_ready_state_t;
    if (!state)
        return false;

    zlink_socket_monitor_open_options_t opts;
    memset (&opts, 0, sizeof (opts));
    opts.events = event_ | ZLINK_EVENT_BIND_FAILED | ZLINK_EVENT_ACCEPT_FAILED
                  | ZLINK_EVENT_CLOSE_FAILED | ZLINK_EVENT_HANDSHAKE_FAILED_NO_DETAIL
                  | ZLINK_EVENT_HANDSHAKE_FAILED_PROTOCOL | ZLINK_EVENT_HANDSHAKE_FAILED_AUTH;
    void *monitor = zlink_socket_monitor_open (socket_, &opts);
    if (!monitor
        || zlink_socket_monitor_handler (monitor, &delivery_ready_monitor_handler, state) != 0) {
        if (monitor)
            (void) zlink_monitor_close (&monitor);
        delete state;
        return false;
    }

    out_->monitor = monitor;
    out_->state = state;
    return true;
}

bool wait_delivery_ready (delivery_ready_monitor_t *monitor_, int timeout_ms_)
{
    if (!monitor_ || !monitor_->state)
        return false;

    delivery_ready_state_t *state = monitor_->state;
    std::unique_lock<std::mutex> lock (state->sync);
    if (state->error_code != 0)
        return false;
    if (state->ready)
        return true;

    const bool signaled =
      state->cv.wait_for (lock, std::chrono::milliseconds (timeout_ms_ > 0 ? timeout_ms_ : 1),
                          [state] () { return state->error_code != 0 || state->ready; });
    return signaled && state->error_code == 0 && state->ready;
}

void close_delivery_ready_monitor (delivery_ready_monitor_t *monitor_)
{
    if (!monitor_)
        return;

    void *monitor = monitor_->monitor;
    delivery_ready_state_t *state = monitor_->state;
    if (monitor)
        (void) zlink_monitor_close (&monitor);
    delete state;
    monitor_->monitor = NULL;
    monitor_->state = NULL;
}

} // namespace

void test ()
{
    //  Create a publisher
    void *pub = test_context_socket (ZLINK_SOCKET_XPUB);

    const uint64_t hwm = 2000u * sizeof (zlink_msg_t);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (pub, ZLINK_OPT_SNDHWM, &hwm, sizeof (hwm)));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (pub, "inproc://soname"));

    //  set pub socket options
    int wait = 1;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_pub_option (pub, ZLINK_PUB_OPT_NODROP, &wait, sizeof (wait)));

    //  Create a subscriber
    void *sub = test_context_socket (ZLINK_SOCKET_SUB);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sub, "inproc://soname"));

    //  Subscribe for all messages.
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub, ""));

    //  we must wait for the subscription to be processed here, otherwise some
    //  or all published messages might be lost
    recv_string_expect_success (pub, "\1", 0);

    int hwmlimit = 1999;
    int send_count = 0;

    //  Send an empty message
    for (int i = 0; i < hwmlimit; i++) {
        TEST_ASSERT_SUCCESS_ERRNO (zlink_send (pub, static_cast<const void *> (NULL), 0, 0));
        send_count++;
    }

    int recv_count = 0;
    do {
        //  Receive the message in the subscriber
        int rc = zlink_recv (sub, NULL, 0, 0);
        if (rc == -1) {
            TEST_ASSERT_EQUAL_INT (EAGAIN, errno);
            break;
        }
        TEST_ASSERT_EQUAL_INT (0, rc);
        recv_count++;

        if (recv_count == 1) {
            const int sub_rcvtimeo = 250;
            TEST_ASSERT_SUCCESS_ERRNO (
              zlink_set_option (sub, ZLINK_OPT_RCVTIMEO, &sub_rcvtimeo, sizeof (sub_rcvtimeo)));
        }

    } while (true);

    TEST_ASSERT_EQUAL_INT (send_count, recv_count);

    //  Now test real blocking behavior
    //  Set a timeout, default is infinite
    int timeout = 0;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (pub, ZLINK_OPT_SNDTIMEO, &timeout, sizeof (timeout)));

    send_count = 0;
    recv_count = 0;
    hwmlimit = 2000;

    //  Send an empty message until we get an error, which must be EAGAIN
    while (zlink_send (pub, "", 0, 0) == 0)
        send_count++;
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);

    if (send_count > 0) {
        //  Receive first message with blocking
        TEST_ASSERT_SUCCESS_ERRNO (zlink_recv (sub, NULL, 0, 0));
        recv_count++;

        while (zlink_recv (sub, NULL, 0, ZLINK_DONTWAIT) == 0)
            recv_count++;
    }

    TEST_ASSERT_EQUAL_INT (send_count, recv_count);

    //  Clean up.
    test_context_socket_close (pub);
    test_context_socket_close (sub);
}

void test_pub_blocking_publish_succeeds_while_subscriber_drains_tcp ()
{
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_set (get_test_context (), ZLINK_IO_THREADS, 1));

    void *pub = test_context_socket (ZLINK_SOCKET_PUB);
    void *sub = test_context_socket (ZLINK_SOCKET_SUB);
    configure_pubsub_hwm (pub);
    configure_pubsub_hwm (sub);

    int wait = 1;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_pub_option (pub, ZLINK_PUB_OPT_NODROP, &wait, sizeof (wait)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (pub, ZLINK_OPT_SNDTIMEO, &kPubSendTimeoutMs, sizeof (kPubSendTimeoutMs)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub, ""));
    pubsub_callback_counter_t callback_state;
    const int target_messages = 10000;

    std::future<int> recv_future = std::async (std::launch::async, [&] () -> int {
        while (callback_state.received.load (std::memory_order_acquire) < target_messages) {
            zlink_msg_t *parts = NULL;
            size_t part_count = 0;
            char topic[64] = {0};
            size_t topic_len = sizeof (topic);
            if (zlink_subscribe (sub, NULL, &parts, &part_count, topic, &topic_len, ZLINK_DONTWAIT)
                == 0) {
                const bool ok = part_count == 1 && topic_len == std::strlen (kPubsubTopic)
                                && memcmp (topic, kPubsubTopic, topic_len) == 0
                                && zlink_msg_size (&parts[0]) == kPubsubPayloadSize;
                zlink_multipart_close (parts, part_count);
                if (!ok) {
                    callback_state.error.store (EPROTO, std::memory_order_release);
                    return -1;
                }
                callback_state.received.fetch_add (1, std::memory_order_acq_rel);
                continue;
            }

            const int err = zlink_errno ();
            if (err != EAGAIN && err != EINTR) {
                callback_state.error.store (err, std::memory_order_release);
                return -1;
            }
            msleep (1);
        }

        return callback_state.received.load (std::memory_order_acquire);
    });

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (pub, endpoint, sizeof (endpoint));

    delivery_ready_monitor_t pub_monitor;
    delivery_ready_monitor_t sub_monitor;
    TEST_ASSERT_TRUE (
      open_delivery_ready_monitor (pub, ZLINK_EVENT_CONNECTION_READY, &pub_monitor));
    TEST_ASSERT_TRUE (
      open_delivery_ready_monitor (sub, ZLINK_EVENT_CONNECTION_READY, &sub_monitor));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sub, endpoint));
    TEST_ASSERT_TRUE (wait_delivery_ready (&pub_monitor, 5000));
    TEST_ASSERT_TRUE (wait_delivery_ready (&sub_monitor, 5000));

    std::atomic<int> sent_progress (0);

    std::future<int> send_future = std::async (std::launch::async, [&] () -> int {
        for (int i = 0; i < target_messages; ++i) {
            zlink_msg_t payload;
            if (zlink_msg_init_size (&payload, kPubsubPayloadSize) != 0)
                return -10;
            memset (zlink_msg_data (&payload), '0' + (i % 10), kPubsubPayloadSize);
            if (zlink_publish (pub, kPubsubTopic, &payload, 1, 0) != 0)
                return -(1000 + errno);
            sent_progress.store (i + 1, std::memory_order_release);
        }
        return target_messages;
    });

    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (kTcpDrainCompletionTimeoutSec);
    const std::future_status send_status = send_future.wait_until (deadline);
    const std::future_status recv_status = recv_future.wait_until (deadline);

    if (send_status != std::future_status::ready || recv_status != std::future_status::ready) {
        char detail[160];
        snprintf (detail, sizeof (detail), "blocking publish timeout: sent=%d recv=%d",
                  sent_progress.load (std::memory_order_acquire),
                  callback_state.received.load (std::memory_order_acquire));
        close_delivery_ready_monitor (&sub_monitor);
        close_delivery_ready_monitor (&pub_monitor);
        test_context_socket_close_zero_linger (sub);
        test_context_socket_close_zero_linger (pub);
        TEST_FAIL_MESSAGE (detail);
    }

    const int send_result = send_future.get ();
    const int recv_result = recv_future.get ();
    TEST_ASSERT_EQUAL_INT_MESSAGE (target_messages, send_result,
                                   "blocking publish timed out or failed while subscriber drained");
    TEST_ASSERT_EQUAL_INT_MESSAGE (0, callback_state.error.load (std::memory_order_acquire),
                                   "subscriber recv observed malformed topic/payload shape");
    TEST_ASSERT_EQUAL_INT_MESSAGE (
      target_messages, recv_result,
      "subscriber did not drain the expected number of published messages");

    close_delivery_ready_monitor (&sub_monitor);
    close_delivery_ready_monitor (&pub_monitor);
    test_context_socket_close (sub);
    test_context_socket_close (pub);
}

//  PUB and XPUB default to lossy fanout: ZLINK_PUB_OPT_NODROP is 0 unless the
//  caller sets it. Reliable delivery is opt-in, not the default.
void test_pub_nodrop_default_is_zero ()
{
    const int socket_types[] = {ZLINK_SOCKET_XPUB, ZLINK_SOCKET_PUB};
    for (size_t i = 0; i < sizeof (socket_types) / sizeof (socket_types[0]); i++) {
        void *pub = test_context_socket (socket_types[i]);
        int value = -1;
        size_t size = sizeof (value);
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_get_pub_option (pub, ZLINK_PUB_OPT_NODROP, &value, &size));
        TEST_ASSERT_EQUAL_INT (0, value);
        test_context_socket_close (pub);
    }
}

//  With the default option the publisher never reports backpressure. Messages
//  past the HWM are dropped for the slow subscriber and publish keeps
//  succeeding.
void test_default_publish_drops_instead_of_backpressuring ()
{
    void *pub = test_context_socket (ZLINK_SOCKET_XPUB);

    const uint64_t hwm = 200u * sizeof (zlink_msg_t);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (pub, ZLINK_OPT_SNDHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (pub, "inproc://nodrop-default"));

    void *sub = test_context_socket (ZLINK_SOCKET_SUB);
    //  Bound both ends: an unbounded receive queue would absorb every message
    //  and the publisher pipe would never reach its HWM.
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (sub, ZLINK_OPT_RCVHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sub, "inproc://nodrop-default"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub, ""));

    //  Wait for the subscription so the count below is not lost to the race.
    recv_string_expect_success (pub, "\1", 0);

    //  Do not drain the subscriber. Every send must still succeed.
    const int send_target = 4000;
    for (int i = 0; i < send_target; i++)
        TEST_ASSERT_SUCCESS_ERRNO (zlink_send (pub, static_cast<const void *> (NULL), 0, 0));

    const int sub_rcvtimeo = 250;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (sub, ZLINK_OPT_RCVTIMEO, &sub_rcvtimeo, sizeof (sub_rcvtimeo)));

    int recv_count = 0;
    while (zlink_recv (sub, NULL, 0, 0) == 0)
        recv_count++;
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);

    //  The HWM is far below the send count, so the socket must have dropped.
    TEST_ASSERT_TRUE (recv_count < send_target);

    test_context_socket_close (sub);
    test_context_socket_close (pub);
}

int main ()
{
    setup_test_environment ();
    UNITY_BEGIN ();
    RUN_TEST (test_pub_nodrop_default_is_zero);
    RUN_TEST (test_default_publish_drops_instead_of_backpressuring);
    RUN_TEST (test);
    RUN_TEST (test_pub_blocking_publish_succeeds_while_subscriber_drains_tcp);
    return UNITY_END ();
}
