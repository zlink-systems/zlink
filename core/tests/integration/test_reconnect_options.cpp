/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_monitoring.hpp"
#include "testutil_unity.hpp"

#include <unity.h>
#include <atomic>
#include <chrono>
#include <thread>

extern "C" void zlink_test_set_submit_retry_fault (int count_, int err_);

namespace
{
void expect_monitor_sequence (test_monitor_probe_t *probe_,
                              const uint64_t *expected_,
                              int count_,
                              int timeout_ms_)
{
    TEST_ASSERT_TRUE (test_monitor_probe_wait_count (probe_, count_, timeout_ms_));
    for (int i = 0; i < count_; ++i)
        TEST_ASSERT_EQUAL_UINT64 (expected_[i], test_monitor_probe_event_at (probe_, i));
}

void make_rid (const char *value_, zlink_routing_id_t *out_)
{
    memset (out_, 0, sizeof (*out_));
    out_->size = static_cast<uint8_t> (strlen (value_));
    memcpy (out_->data, value_, out_->size);
}

void wait_router_send_ready (void *router_, const zlink_routing_id_t *rid_, void *receiver_)
{
    for (int i = 0; i < 100; ++i) {
        if (test_stream_send_bytes (router_, rid_, "ready", 5, ZLINK_DONTWAIT) == 5) {
            recv_string_expect_success (receiver_, "ready", 0);
            return;
        }
        msleep (10);
    }
    TEST_FAIL_MESSAGE ("router did not become ready for dealer route");
}

void recv_router_payload_expect_success (void *router_, const char *payload_)
{
    char source[256];
    TEST_ASSERT_GREATER_THAN (0, zlink_recv (router_, source, sizeof (source), 0));
    recv_string_expect_success (router_, payload_, 0);
}

void configure_submit_retry (void *socket_, int timeout_ms_)
{
    int retry_mode = ZLINK_SUBMIT_RETRY_LOCAL_FAILURE;
    int retry_attempts = 8;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (socket_, ZLINK_OPT_SUBMIT_RETRY_MODE, &retry_mode, sizeof (retry_mode)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (socket_, ZLINK_OPT_SUBMIT_RETRY_TIMEOUT,
                                                 &timeout_ms_, sizeof (timeout_ms_)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (socket_, ZLINK_OPT_SUBMIT_RETRY_ATTEMPTS,
                                                 &retry_attempts, sizeof (retry_attempts)));
}

long elapsed_ms_since (std::chrono::steady_clock::time_point start_)
{
    return static_cast<long> (std::chrono::duration_cast<std::chrono::milliseconds> (
                                std::chrono::steady_clock::now () - start_)
                                .count ());
}

void init_string_msg (zlink_msg_t *msg_, const char *value_)
{
    const size_t size = strlen (value_);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (msg_, size));
    memcpy (zlink_msg_data (msg_), value_, size);
}

struct send_ready_probe_t
{
    send_ready_probe_t () : count (0) {}

    std::atomic<int> count;
};

void capture_send_ready (void *, void *userdata_)
{
    send_ready_probe_t *probe = static_cast<send_ready_probe_t *> (userdata_);
    if (probe)
        probe->count.fetch_add (1, std::memory_order_release);
}

}

// test behavior with (mostly) default values
void reconnect_default ()
{
    void *pub = test_context_socket (ZLINK_SOCKET_PUB);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (pub, ENDPOINT_0));

    void *sub = test_context_socket (ZLINK_SOCKET_SUB);
    test_monitor_probe_t probe;
    void *monitor = open_test_monitor_probe (sub, ZLINK_EVENT_ALL, &probe);

    int interval = 60 * 1000;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (sub, ZLINK_OPT_RECONNECT_IVL, &interval, sizeof (interval)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sub, ENDPOINT_0));

    const uint64_t initial_events[] = {ZLINK_EVENT_CONNECT_DELAYED, ZLINK_EVENT_CONNECTED,
                                       ZLINK_EVENT_CONNECTION_READY};
    expect_monitor_sequence (&probe, initial_events,
                             sizeof (initial_events) / sizeof (initial_events[0]), 3000);

    test_context_socket_close_zero_linger (pub);

    const uint64_t disconnect_events[] = {
      ZLINK_EVENT_CONNECT_DELAYED, ZLINK_EVENT_CONNECTED,        ZLINK_EVENT_CONNECTION_READY,
      ZLINK_EVENT_DISCONNECTED,    ZLINK_EVENT_CONNECTION_READY, ZLINK_EVENT_CONNECT_RETRIED};
    expect_monitor_sequence (&probe, disconnect_events,
                             sizeof (disconnect_events) / sizeof (disconnect_events[0]), 3000);
    TEST_ASSERT_TRUE (test_monitor_probe_wait_no_additional (&probe, 6, 2000));

    close_test_monitor_probe (&monitor, &probe);
    test_context_socket_close_zero_linger (sub);
}

// test successful reconnect
void reconnect_success ()
{
    void *pub = test_context_socket (ZLINK_SOCKET_PUB);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (pub, ENDPOINT_0));

    void *sub = test_context_socket (ZLINK_SOCKET_SUB);
    test_monitor_probe_t probe;
    void *monitor = open_test_monitor_probe (sub, ZLINK_EVENT_ALL, &probe);

    int interval = 1 * 1000;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (sub, ZLINK_OPT_RECONNECT_IVL, &interval, sizeof (interval)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sub, ENDPOINT_0));

    const uint64_t initial_events[] = {ZLINK_EVENT_CONNECT_DELAYED, ZLINK_EVENT_CONNECTED,
                                       ZLINK_EVENT_CONNECTION_READY};
    expect_monitor_sequence (&probe, initial_events,
                             sizeof (initial_events) / sizeof (initial_events[0]), 3000);

    test_context_socket_close_zero_linger (pub);

    const uint64_t reconnect_wait_events[] = {
      ZLINK_EVENT_CONNECT_DELAYED, ZLINK_EVENT_CONNECTED,        ZLINK_EVENT_CONNECTION_READY,
      ZLINK_EVENT_DISCONNECTED,    ZLINK_EVENT_CONNECTION_READY, ZLINK_EVENT_CONNECT_RETRIED};
    expect_monitor_sequence (&probe, reconnect_wait_events,
                             sizeof (reconnect_wait_events) / sizeof (reconnect_wait_events[0]),
                             3000);
    TEST_ASSERT_TRUE (test_monitor_probe_wait_no_additional (&probe, 6, SETTLE_TIME));

    pub = test_context_socket (ZLINK_SOCKET_PUB);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (pub, ENDPOINT_0));

    const uint64_t reconnect_success_events[] = {
      ZLINK_EVENT_CONNECT_DELAYED, ZLINK_EVENT_CONNECTED,        ZLINK_EVENT_CONNECTION_READY,
      ZLINK_EVENT_DISCONNECTED,    ZLINK_EVENT_CONNECTION_READY, ZLINK_EVENT_CONNECT_RETRIED,
      ZLINK_EVENT_CONNECT_DELAYED, ZLINK_EVENT_CONNECTED,        ZLINK_EVENT_CONNECTION_READY};
    expect_monitor_sequence (
      &probe, reconnect_success_events,
      sizeof (reconnect_success_events) / sizeof (reconnect_success_events[0]), 3000);
    TEST_ASSERT_TRUE (test_monitor_probe_wait_no_additional (&probe, 9, SETTLE_TIME));

    close_test_monitor_probe (&monitor, &probe);
    test_context_socket_close_zero_linger (sub);
    test_context_socket_close_zero_linger (pub);
}

void submit_retry_absorbs_short_active_router_disconnect ()
{
    void *server = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *client = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *server_rebind = test_context_socket (ZLINK_SOCKET_ROUTER);

    int zero = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (server, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (client, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (server_rebind, ZLINK_OPT_LINGER, &zero, sizeof (zero)));

    int reconnect_ivl = 20;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (client, ZLINK_OPT_RECONNECT_IVL, &reconnect_ivl, sizeof (reconnect_ivl)));
    int one = 1;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_router_option (client, ZLINK_ROUTER_OPT_MANDATORY, &one, sizeof (one)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (server, "S", 1));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (server_rebind, "S", 1));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_router_option (client, ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID, "S", 1));
    configure_submit_retry (client, 500);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (server, ENDPOINT_1));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, ENDPOINT_1));

    zlink_routing_id_t rid;
    make_rid ("S", &rid);
    for (int i = 0; i < 100; ++i) {
        if (test_stream_send_bytes (client, &rid, "ready", 5, ZLINK_DONTWAIT) == 5) {
            recv_router_payload_expect_success (server, "ready");
            break;
        }
        msleep (10);
        if (i == 99)
            TEST_FAIL_MESSAGE ("client router did not become ready");
    }

    test_context_socket_close_zero_linger (server);
    msleep (SETTLE_TIME * 2);

    int rebind_rc = -1;
    std::thread rebind_thread ([server_rebind, &rebind_rc] () {
        msleep (30);
        rebind_rc = zlink_bind (server_rebind, ENDPOINT_1);
    });

    const int send_rc = test_stream_send_bytes (client, &rid, "retry", 5, 0);
    rebind_thread.join ();
    TEST_ASSERT_SUCCESS_ERRNO (rebind_rc);
    TEST_ASSERT_EQUAL_INT (5, send_rc);
    recv_router_payload_expect_success (server_rebind, "retry");

    test_context_socket_close_zero_linger (client);
    test_context_socket_close_zero_linger (server_rebind);
}

void submit_retry_returns_not_connected_when_budget_expires ()
{
    void *client = test_context_socket (ZLINK_SOCKET_ROUTER);

    int zero = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (client, ZLINK_OPT_LINGER, &zero, sizeof (zero)));

    int reconnect_ivl = 20;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (client, ZLINK_OPT_RECONNECT_IVL, &reconnect_ivl, sizeof (reconnect_ivl)));
    int one = 1;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_router_option (client, ZLINK_ROUTER_OPT_MANDATORY, &one, sizeof (one)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_router_option (client, ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID, "S", 1));
    configure_submit_retry (client, 40);
    zlink_test_set_submit_retry_fault (64, ENOTCONN);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, ENDPOINT_1));

    zlink_routing_id_t rid;
    make_rid ("S", &rid);

    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now ();
    TEST_ASSERT_EQUAL_INT (-1, test_stream_send_bytes (client, &rid, "expired", 7, 0));
    TEST_ASSERT_TRUE (errno == ENOTCONN || errno == EHOSTUNREACH);
    const long elapsed_ms = elapsed_ms_since (start);
    TEST_ASSERT_TRUE_MESSAGE (elapsed_ms >= 20, "retry budget should wait for local reconnect");
    TEST_ASSERT_TRUE_MESSAGE (elapsed_ms < 200, "retry budget should expire promptly");

    zlink_test_set_submit_retry_fault (0, 0);
    test_context_socket_close_zero_linger (client);
}

void submit_retry_retries_multipart_final_frame ()
{
    void *server = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *client = test_context_socket (ZLINK_SOCKET_ROUTER);

    int zero = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (server, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (client, ZLINK_OPT_LINGER, &zero, sizeof (zero)));

    int reconnect_ivl = 20;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (client, ZLINK_OPT_RECONNECT_IVL, &reconnect_ivl, sizeof (reconnect_ivl)));
    int one = 1;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_router_option (client, ZLINK_ROUTER_OPT_MANDATORY, &one, sizeof (one)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (server, "S", 1));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_router_option (client, ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID, "S", 1));
    configure_submit_retry (client, 200);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (server, ENDPOINT_1));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, ENDPOINT_1));

    zlink_routing_id_t rid;
    make_rid ("S", &rid);
    for (int i = 0; i < 100; ++i) {
        if (test_stream_send_bytes (client, &rid, "ready", 5, ZLINK_DONTWAIT) == 5) {
            recv_router_payload_expect_success (server, "ready");
            break;
        }
        msleep (10);
        if (i == 99)
            TEST_FAIL_MESSAGE ("client router did not become ready");
    }

    zlink_msg_t first;
    init_string_msg (&first, "one");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK, zlink_send_part_rid (client, &rid, &first,
                                            static_cast<zlink_send_flags_t> (0), ZLINK_PART_MORE));

    zlink_test_set_submit_retry_fault (1, ENOTCONN);
    zlink_msg_t second;
    init_string_msg (&second, "two");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK, zlink_send_part_rid (client, &rid, &second,
                                            static_cast<zlink_send_flags_t> (0), ZLINK_PART_FINAL));

    char source[256];
    TEST_ASSERT_GREATER_THAN (0, zlink_recv (server, source, sizeof (source), 0));
    recv_string_expect_success (server, "one", 0);
    recv_string_expect_success (server, "two", 0);

    zlink_test_set_submit_retry_fault (0, 0);
    test_context_socket_close_zero_linger (client);
    test_context_socket_close_zero_linger (server);
}

void submit_retry_does_not_wait_for_passive_router_route ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);

    int zero = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (router, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (dealer, ZLINK_OPT_LINGER, &zero, sizeof (zero)));

    int one = 1;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_router_option (router, ZLINK_ROUTER_OPT_MANDATORY, &one, sizeof (one)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer, "D", 1));
    configure_submit_retry (router, 300);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (router, ENDPOINT_1));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer, ENDPOINT_1));

    zlink_routing_id_t rid;
    make_rid ("D", &rid);
    wait_router_send_ready (router, &rid, dealer);

    test_context_socket_close_zero_linger (dealer);
    msleep (SETTLE_TIME * 2);

    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now ();
    TEST_ASSERT_EQUAL_INT (-1, test_stream_send_bytes (router, &rid, "retry", 5, 0));
    const long elapsed_ms = elapsed_ms_since (start);
    TEST_ASSERT_TRUE_MESSAGE (elapsed_ms < 100,
                              "passive router route should not consume retry budget");

    test_context_socket_close_zero_linger (router);
}

void submit_retry_does_not_wait_for_dontwait ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);

    int zero = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (router, ZLINK_OPT_LINGER, &zero, sizeof (zero)));

    int one = 1;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_router_option (router, ZLINK_ROUTER_OPT_MANDATORY, &one, sizeof (one)));
    configure_submit_retry (router, 300);

    zlink_routing_id_t rid;
    make_rid ("missing", &rid);

    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now ();
    TEST_ASSERT_EQUAL_INT (-1,
                           test_stream_send_bytes (router, &rid, "dontwait", 8, ZLINK_DONTWAIT));
    const long elapsed_ms = elapsed_ms_since (start);
    TEST_ASSERT_TRUE_MESSAGE (elapsed_ms < 100, "DONTWAIT should not consume retry budget");

    test_context_socket_close_zero_linger (router);
}

void submit_retry_does_not_wait_for_unknown_route ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);

    int zero = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (router, ZLINK_OPT_LINGER, &zero, sizeof (zero)));

    int one = 1;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_router_option (router, ZLINK_ROUTER_OPT_MANDATORY, &one, sizeof (one)));

    configure_submit_retry (router, 300);

    zlink_routing_id_t rid;
    make_rid ("missing", &rid);

    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now ();
    TEST_ASSERT_EQUAL_INT (-1, test_stream_send_bytes (router, &rid, "drop", 4, 0));
    const long elapsed_ms = elapsed_ms_since (start);
    TEST_ASSERT_TRUE_MESSAGE (elapsed_ms < 100, "unknown route should not consume retry budget");

    test_context_socket_close_zero_linger (router);
}

void dontwait_local_admission_wakes_when_first_target_attaches ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    const char endpoint[] = "inproc://dontwait-first-target-ready";

    int one = 1;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (dealer, ZLINK_OPT_IMMEDIATE, &one, sizeof (one)));
    send_ready_probe_t probe;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_HANDLER_OK,
      zlink_send_ready_handler (dealer, &capture_send_ready, &probe));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer, endpoint));

    zlink_test_set_submit_retry_fault (1, ECONNREFUSED);
    errno = 0;
    TEST_ASSERT_EQUAL_INT (-1,
                           zlink_send (dealer, "before", 6, ZLINK_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (ECONNREFUSED, errno);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (router, endpoint));
    for (int i = 0; i < 300
                    && probe.count.load (std::memory_order_acquire) == 0;
         ++i)
        msleep (10);
    TEST_ASSERT_GREATER_THAN (
      0, probe.count.load (std::memory_order_acquire));

    bool submitted = false;
    for (int i = 0; i < 100 && !submitted; ++i) {
        submitted = zlink_send (dealer, "after", 5, ZLINK_DONTWAIT) == 5;
        if (!submitted)
            msleep (10);
    }
    TEST_ASSERT_TRUE (submitted);
    recv_router_payload_expect_success (router, "after");

    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
}

void setUp ()
{
    setup_test_context ();
}

void tearDown ()
{
    zlink_test_set_submit_retry_fault (0, 0);
    teardown_test_context ();
}

int main (void)
{
    setup_test_environment ();

    UNITY_BEGIN ();

    RUN_TEST (reconnect_default);
    RUN_TEST (reconnect_success);
    RUN_TEST (submit_retry_absorbs_short_active_router_disconnect);
    RUN_TEST (submit_retry_returns_not_connected_when_budget_expires);
    RUN_TEST (submit_retry_retries_multipart_final_frame);
    RUN_TEST (submit_retry_does_not_wait_for_passive_router_route);
    RUN_TEST (submit_retry_does_not_wait_for_dontwait);
    RUN_TEST (submit_retry_does_not_wait_for_unknown_route);
    RUN_TEST (dontwait_local_admission_wakes_when_first_target_attaches);
    return UNITY_END ();
}
