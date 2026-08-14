/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

SETUP_TEARDOWN_TESTCONTEXT

// DEBUG shouldn't be defined in sources as it will cause a redefined symbol
// error when it is defined in the build configuration. It appears that the
// intent here is to semi-permanently disable DEBUG tracing statements, so the
// implementation is changed to accommodate that intent.
//#define DEBUG 0
#define TRACE_ENABLED 0

namespace
{
struct routed_ready_event_copy_t
{
    std::string rid;
    uint64_t pair_id;
    uint64_t pair_generation;
    zlink_routed_send_ready_state_t state;
    int terminal_errno;
};

struct routed_ready_probe_t
{
    std::mutex sync;
    std::condition_variable changed;
    std::vector<routed_ready_event_copy_t> events;
};

struct routed_ready_self_close_probe_t
{
    routed_ready_self_close_probe_t () : close_started (false), close_rc (-1) {}
    routed_ready_probe_t events;
    std::atomic<bool> close_started;
    std::atomic<int> close_rc;
};

void capture_routed_ready (void *,
                           const zlink_routed_send_ready_event_t *event_,
                           void *userdata_)
{
    routed_ready_probe_t *probe = static_cast<routed_ready_probe_t *> (userdata_);
    if (!probe || !event_)
        return;

    routed_ready_event_copy_t copy;
    copy.rid.assign (reinterpret_cast<const char *> (event_->peer_rid.data),
                     event_->peer_rid.size);
    copy.pair_id = event_->transport_pair_id;
    copy.pair_generation = event_->transport_pair_generation;
    copy.state = event_->state;
    copy.terminal_errno = event_->terminal_errno;
    {
        std::lock_guard<std::mutex> lock (probe->sync);
        probe->events.push_back (copy);
    }
    probe->changed.notify_all ();
}

void capture_routed_ready_and_close_on_first_terminal (
  void *socket_, const zlink_routed_send_ready_event_t *event_, void *userdata_)
{
    routed_ready_self_close_probe_t *probe =
      static_cast<routed_ready_self_close_probe_t *> (userdata_);
    if (!probe || !event_)
        return;

    capture_routed_ready (socket_, event_, &probe->events);
    if (event_->state == ZLINK_ROUTED_SEND_TERMINAL
        && !probe->close_started.exchange (true, std::memory_order_acq_rel))
        probe->close_rc.store (static_cast<int> (zlink_close (socket_)),
                               std::memory_order_release);
}

bool wait_for_routed_event (routed_ready_probe_t *probe_,
                            const char *rid_,
                            zlink_routed_send_ready_state_t state_,
                            int terminal_errno_,
                            routed_ready_event_copy_t *out_ = NULL)
{
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (3);
    std::unique_lock<std::mutex> lock (probe_->sync);
    while (true) {
        for (size_t i = 0; i < probe_->events.size (); ++i) {
            if (probe_->events[i].rid == rid_
                && probe_->events[i].state == state_
                && probe_->events[i].terminal_errno == terminal_errno_) {
                if (out_)
                    *out_ = probe_->events[i];
                return true;
            }
        }
        if (probe_->changed.wait_until (lock, deadline)
            == std::cv_status::timeout)
            return false;
    }
}

zlink_routing_id_t make_rid (const char *value_)
{
    zlink_routing_id_t rid;
    memset (&rid, 0, sizeof (rid));
    const size_t size = strlen (value_);
    zlink_assert (size <= sizeof (rid.data));
    rid.size = static_cast<uint8_t> (size);
    memcpy (rid.data, value_, size);
    return rid;
}

zlink_submit_result_t send_routed_bytes (void *router_,
                                         const zlink_routing_id_t *rid_,
                                         size_t size_)
{
    zlink_msg_t part;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init_size (&part, size_));
    memset (zlink_msg_data (&part), 0x5a, size_);
    return zlink_send_part_rid (router_, rid_, &part,
                                ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL);
}

void drain_one_part (void *socket_)
{
    const zlink_routing_id_t *source_rid = NULL;
    zlink_msg_t part;
    zlink_part_flag_t has_more = ZLINK_PART_FINAL;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_recv_part (socket_, &source_rid, &part, &has_more,
                       static_cast<zlink_recv_flags_t> (0)));
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, has_more);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
}

int send_routed_payload_expect_maybe_eagain (
  void *router_, const zlink_routing_id_t *rid_, const void *buf_, size_t size_, int flags_)
{
    zlink_msg_t msg;
    if (zlink_msg_init_size (&msg, size_) != 0)
        return -1;
    if (size_ > 0 && buf_)
        memcpy (zlink_msg_data (&msg), buf_, size_);

    const int rc =
      zlink_send_rid (router_, rid_, &msg, 1, static_cast<zlink_send_flags_t> (flags_));
    if (rc != 0) {
        const int err = errno;
        zlink_msg_close (&msg);
        errno = err;
        return -1;
    }
    return 0;
}

int send_routed_multipart_expect_maybe_eagain (
  void *router_, const zlink_routing_id_t *rid_, const void *buf_, size_t size_)
{
    zlink_msg_t envelope;
    if (zlink_msg_init_size (&envelope, 32) != 0)
        return -1;
    memset (zlink_msg_data (&envelope), 0xA5, zlink_msg_size (&envelope));
    zlink_submit_result_t rc =
      zlink_send_part_rid (router_, rid_, &envelope, ZLINK_SEND_FLAGS_DONTWAIT,
                           ZLINK_PART_MORE);
    if (rc != ZLINK_SUBMIT_OK)
        return -1;

    zlink_msg_t payload;
    if (zlink_msg_init_size (&payload, size_) != 0)
        return -1;
    if (size_ > 0 && buf_)
        memcpy (zlink_msg_data (&payload), buf_, size_);
    rc = zlink_send_part_rid (router_, rid_, &payload, ZLINK_SEND_FLAGS_DONTWAIT,
                              ZLINK_PART_FINAL);
    return rc == ZLINK_SUBMIT_OK ? 0 : -1;
}
}

void test_router_mandatory_hwm ()
{
    if (TRACE_ENABLED)
        fprintf (stderr, "Staring router mandatory HWM test ...\n");
    char my_endpoint[MAX_SOCKET_STRING];
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);

    // Configure router socket to mandatory routing and set HWM and linger
    int mandatory = 1;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_router_option (router, ZLINK_ROUTER_OPT_MANDATORY, &mandatory, sizeof (mandatory)));
    const uint64_t sndhwm = 65536u + sizeof (zlink_msg_t);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (router, ZLINK_OPT_SNDHWM, &sndhwm, sizeof (sndhwm)));
    int linger = 1;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (router, ZLINK_OPT_LINGER, &linger, sizeof (linger)));

    bind_loopback_ipv4 (router, my_endpoint, sizeof my_endpoint);

    //  Create dealer called "X" and connect it to our router, configure HWM
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer, "X", 1));
    const uint64_t rcvhwm = sndhwm;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (dealer, ZLINK_OPT_RCVHWM, &rcvhwm, sizeof (rcvhwm)));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer, my_endpoint));

    //  Get message from dealer to know when connection is ready
    send_string_expect_success (dealer, "Hello", 0);
    recv_string_expect_success (router, "X", 0);

    int i;
    const int buf_size = 65536;
    const uint8_t buf[buf_size] = {0};
    // Send first batch of messages
    for (i = 0; i < 100000; ++i) {
        if (TRACE_ENABLED)
            fprintf (stderr, "Sending message %d ...\n", i);
        const int rc = zlink_send (router, "X", 1, ZLINK_DONTWAIT | ZLINK_SNDMORE);
        if (rc == -1 && zlink_errno () == EAGAIN)
            break;
        TEST_ASSERT_EQUAL_INT (1, rc);
        send_array_expect_success (router, buf, ZLINK_DONTWAIT);
    }
    // This should fail after one message but kernel buffering could
    // skew results
    TEST_ASSERT_LESS_THAN_INT (10, i);
    msleep (1000);
    // Send second batch of messages
    for (; i < 100000; ++i) {
        if (TRACE_ENABLED)
            fprintf (stderr, "Sending message %d (part 2) ...\n", i);
        const int rc = zlink_send (router, "X", 1, ZLINK_DONTWAIT | ZLINK_SNDMORE);
        if (rc == -1 && zlink_errno () == EAGAIN)
            break;
        TEST_ASSERT_EQUAL_INT (1, rc);
        send_array_expect_success (router, buf, ZLINK_DONTWAIT);
    }
    // This should fail after two messages but kernel buffering could
    // skew results
    TEST_ASSERT_LESS_THAN_INT (20, i);

    if (TRACE_ENABLED)
        fprintf (stderr, "Done sending messages.\n");

    test_context_socket_close (router);
    test_context_socket_close (dealer);
}

void test_router_send_rid_mandatory_hwm ()
{
    char my_endpoint[MAX_SOCKET_STRING];
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);

    int mandatory = 1;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_router_option (router, ZLINK_ROUTER_OPT_MANDATORY, &mandatory, sizeof (mandatory)));
    const uint64_t sndhwm = 65536u + sizeof (zlink_msg_t);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (router, ZLINK_OPT_SNDHWM, &sndhwm, sizeof (sndhwm)));
    int linger = 1;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (router, ZLINK_OPT_LINGER, &linger, sizeof (linger)));

    bind_loopback_ipv4 (router, my_endpoint, sizeof my_endpoint);

    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer, "X", 1));
    const uint64_t rcvhwm = sndhwm;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (dealer, ZLINK_OPT_RCVHWM, &rcvhwm, sizeof (rcvhwm)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer, my_endpoint));

    send_string_expect_success (dealer, "Hello", 0);
    recv_string_expect_success (router, "X", 0);

    zlink_routing_id_t rid;
    memset (&rid, 0, sizeof (rid));
    rid.size = 1;
    rid.data[0] = 'X';

    const int buf_size = 65536;
    const uint8_t buf[buf_size] = {0};
    int i = 0;
    for (; i < 100000; ++i) {
        const int rc =
          send_routed_payload_expect_maybe_eagain (router, &rid, buf, sizeof (buf), ZLINK_DONTWAIT);
        if (rc == -1 && zlink_errno () == EAGAIN)
            break;
        TEST_ASSERT_EQUAL_INT (0, rc);
    }
    TEST_ASSERT_LESS_THAN_INT (10, i);

    msleep (1000);

    for (; i < 100000; ++i) {
        const int rc =
          send_routed_payload_expect_maybe_eagain (router, &rid, buf, sizeof (buf), ZLINK_DONTWAIT);
        if (rc == -1 && zlink_errno () == EAGAIN)
            break;
        TEST_ASSERT_EQUAL_INT (0, rc);
    }
    TEST_ASSERT_LESS_THAN_INT (20, i);

    test_context_socket_close (router);
    test_context_socket_close (dealer);
}

void test_router_send_rid_multipart_hwm_is_backpressure ()
{
    char endpoint[MAX_SOCKET_STRING];
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);

    int mandatory = 1;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_router_option (router, ZLINK_ROUTER_OPT_MANDATORY, &mandatory,
                               sizeof (mandatory)));
    const uint64_t sndhwm = 65536u + 2u * sizeof (zlink_msg_t) + 32u;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (router, ZLINK_OPT_SNDHWM, &sndhwm, sizeof (sndhwm)));
    bind_loopback_ipv4 (router, endpoint, sizeof endpoint);

    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer, "X", 1));
    const uint64_t rcvhwm = sndhwm;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (dealer, ZLINK_OPT_RCVHWM, &rcvhwm, sizeof (rcvhwm)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer, endpoint));
    send_string_expect_success (dealer, "Hello", 0);
    recv_string_expect_success (router, "X", 0);

    zlink_routing_id_t rid;
    memset (&rid, 0, sizeof (rid));
    rid.size = 1;
    rid.data[0] = 'X';

    const uint8_t payload[65536] = {0};
    bool backpressured = false;
    for (int i = 0; i < 1000; ++i) {
        const int rc = send_routed_multipart_expect_maybe_eagain (
          router, &rid, payload, sizeof (payload));
        if (rc == 0)
            continue;
        TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
        backpressured = true;
        break;
    }
    TEST_ASSERT_TRUE_MESSAGE (
      backpressured, "multipart routed send did not reach HWM");

    const int repeated_rc = send_routed_multipart_expect_maybe_eagain (
      router, &rid, payload, sizeof (payload));
    TEST_ASSERT_EQUAL_INT (-1, repeated_rc);
    TEST_ASSERT_EQUAL_INT_MESSAGE (
      EAGAIN, zlink_errno (),
      "a route held inactive by HWM must remain backpressured");

    test_context_socket_close (router);
    test_context_socket_close (dealer);
}

void test_routed_send_ready_isolated_by_exact_target_and_terminal_cause ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer_a = test_context_socket (ZLINK_SOCKET_DEALER);
    void *dealer_b = test_context_socket (ZLINK_SOCKET_DEALER);
    routed_ready_probe_t probe;

    TEST_ASSERT_EQUAL_INT (
      ZLINK_HANDLER_OK,
      zlink_routed_send_ready_handler (router, &capture_routed_ready, &probe));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer_a, "A", 1));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer_b, "B", 1));

    const uint64_t hwm = 65536u + sizeof (zlink_msg_t);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (router, ZLINK_OPT_SNDHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (dealer_a, ZLINK_OPT_RCVHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (dealer_b, ZLINK_OPT_RCVHWM, &hwm, sizeof (hwm)));

    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (router, "inproc://routed-ready-exact-target"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (dealer_a, "inproc://routed-ready-exact-target"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (dealer_b, "inproc://routed-ready-exact-target"));

    send_string_expect_success (dealer_a, "ready-a", 0);
    recv_string_expect_success (router, "A", 0);
    recv_string_expect_success (router, "ready-a", 0);
    send_string_expect_success (dealer_b, "ready-b", 0);
    recv_string_expect_success (router, "B", 0);
    recv_string_expect_success (router, "ready-b", 0);

    const zlink_routing_id_t rid_a = make_rid ("A");
    const zlink_routing_id_t rid_b = make_rid ("B");
    bool a_backpressured = false;
    for (int i = 0; i < 16; ++i) {
        const zlink_submit_result_t result =
          send_routed_bytes (router, &rid_a, 65536);
        if (result == ZLINK_SUBMIT_BACKPRESSURED) {
            a_backpressured = true;
            break;
        }
        TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, result);
    }
    TEST_ASSERT_TRUE_MESSAGE (
      a_backpressured, "target A did not reach its manual byte HWM");

    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK, send_routed_bytes (router, &rid_b, 65536));
    drain_one_part (dealer_a);

    routed_ready_event_copy_t writable_a;
    TEST_ASSERT_TRUE_MESSAGE (
      wait_for_routed_event (&probe, "A", ZLINK_ROUTED_SEND_WRITABLE, 0,
                             &writable_a),
      "target A credit recovery did not emit exact readiness");
    TEST_ASSERT_TRUE (writable_a.pair_id != 0);
    TEST_ASSERT_TRUE (writable_a.pair_generation != 0);
    {
        std::lock_guard<std::mutex> lock (probe.sync);
        for (size_t i = 0; i < probe.events.size (); ++i)
            TEST_ASSERT_FALSE_MESSAGE (
              probe.events[i].rid == "B"
                && probe.events[i].state == ZLINK_ROUTED_SEND_WRITABLE,
              "target B writable was incorrectly reported for target A");
    }

    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                           zlink_disconnect_rid (router, &rid_a));
    routed_ready_event_copy_t terminal_a;
    TEST_ASSERT_TRUE_MESSAGE (
      wait_for_routed_event (&probe, "A", ZLINK_ROUTED_SEND_TERMINAL,
                             ENOTCONN, &terminal_a),
      "target A detach did not emit terminal readiness");
    TEST_ASSERT_EQUAL_UINT64 (writable_a.pair_id, terminal_a.pair_id);
    TEST_ASSERT_EQUAL_UINT64 (writable_a.pair_generation,
                              terminal_a.pair_generation);

    test_context_socket_close (router);
    TEST_ASSERT_TRUE_MESSAGE (
      wait_for_routed_event (&probe, "B", ZLINK_ROUTED_SEND_TERMINAL,
                             ECANCELED),
      "socket close did not terminate the remaining target");
    test_context_socket_close (dealer_b);
    test_context_socket_close (dealer_a);
}

void test_routed_send_terminal_batch_survives_callback_self_close ()
{
    void *ctx = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx);
    void *router = zlink_socket (ctx, ZLINK_SOCKET_ROUTER);
    void *dealer_a = zlink_socket (ctx, ZLINK_SOCKET_DEALER);
    void *dealer_b = zlink_socket (ctx, ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer_a);
    TEST_ASSERT_NOT_NULL (dealer_b);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer_a, "A", 1));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer_b, "B", 1));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (router, "inproc://routed-ready-self-close"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (dealer_a, "inproc://routed-ready-self-close"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (dealer_b, "inproc://routed-ready-self-close"));

    send_string_expect_success (dealer_a, "ready-a", 0);
    recv_string_expect_success (router, "A", 0);
    recv_string_expect_success (router, "ready-a", 0);
    send_string_expect_success (dealer_b, "ready-b", 0);
    recv_string_expect_success (router, "B", 0);
    recv_string_expect_success (router, "ready-b", 0);

    routed_ready_self_close_probe_t probe;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_HANDLER_OK,
      zlink_routed_send_ready_handler (
        router, &capture_routed_ready_and_close_on_first_terminal, &probe));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_ctx_shutdown (ctx));
    TEST_ASSERT_TRUE_MESSAGE (
      wait_for_routed_event (&probe.events, "A", ZLINK_ROUTED_SEND_TERMINAL,
                             ETERM),
      "target A context terminal was lost during callback self-close");
    TEST_ASSERT_TRUE_MESSAGE (
      wait_for_routed_event (&probe.events, "B", ZLINK_ROUTED_SEND_TERMINAL,
                             ETERM),
      "target B context terminal was lost during callback self-close");
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK,
                           probe.close_rc.load (std::memory_order_acquire));
    {
        std::lock_guard<std::mutex> lock (probe.events.sync);
        size_t terminal_a = 0;
        size_t terminal_b = 0;
        for (size_t i = 0; i < probe.events.events.size (); ++i) {
            if (probe.events.events[i].state != ZLINK_ROUTED_SEND_TERMINAL)
                continue;
            terminal_a += probe.events.events[i].rid == "A" ? 1 : 0;
            terminal_b += probe.events.events[i].rid == "B" ? 1 : 0;
        }
        TEST_ASSERT_EQUAL_UINT64 (1, terminal_a);
        TEST_ASSERT_EQUAL_UINT64 (1, terminal_b);
    }

    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_close (dealer_b));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_close (dealer_a));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_ctx_term (ctx));
}

int main ()
{
    setup_test_environment ();

    UNITY_BEGIN ();
    RUN_TEST (test_router_mandatory_hwm);
    RUN_TEST (test_router_send_rid_mandatory_hwm);
    RUN_TEST (test_router_send_rid_multipart_hwm_is_backpressure);
    RUN_TEST (test_routed_send_ready_isolated_by_exact_target_and_terminal_cause);
    RUN_TEST (test_routed_send_terminal_batch_survives_callback_self_close);
    return UNITY_END ();
}
