/* SPDX-License-Identifier: MPL-2.0 */
#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <chrono>
#include <stdio.h>
#include <string.h>

static void *poller = NULL;
static void *monitor = NULL;

void setUp ()
{
    setup_test_context ();
}

void tearDown ()
{
    if (poller)
        zlink_poller_destroy (&poller);
    if (monitor) {
        zlink_close (monitor);
        monitor = NULL;
    }
    teardown_test_context ();
}

namespace
{
typedef std::chrono::steady_clock clock_type;
const int progress_bound_ms = 200;
const int request_timeout_ms = 1000;
const int request_count = 3;

enum removal_t { endpoint_removal, rid_removal, target_close };

void wait_ready (void *monitor_)
{
    const clock_type::time_point deadline =
      clock_type::now () + std::chrono::seconds (3);
    while (clock_type::now () < deadline) {
        zlink_pollitem_t item = {monitor_, 0, ZLINK_POLLIN, 0};
        zlink_config_result_t error = ZLINK_CONFIG_OK;
        const long remaining = static_cast<long> (
          std::chrono::duration_cast<std::chrono::milliseconds> (
            deadline - clock_type::now ()).count ());
        TEST_ASSERT_GREATER_OR_EQUAL_INT (
          0, zlink_poll (&item, 1, remaining > 0 ? remaining : 0, &error));
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, error);
        zlink_monitor_event_t event = {};
        const zlink_recv_result_t rc = zlink_socket_monitor_recv (
          monitor_, &event, ZLINK_RECV_FLAGS_DONTWAIT);
        if (rc == ZLINK_RECV_NO_DATA)
            continue;
        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, rc);
        if (event.event == ZLINK_EVENT_CONNECTION_READY
            && (event.flags & ZLINK_MONITOR_EVENT_FLAG_CONNECTION_READY_EDGE))
            return;
    }
    TEST_FAIL_MESSAGE ("connection did not become ready");
}

void run_case (bool router_, bool tcp_, removal_t removal_)
{
    void *source = test_context_socket (router_ ? ZLINK_SOCKET_ROUTER
                                               : ZLINK_SOCKET_DEALER);
    void *target = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_set_routing_id (source, "source", 6));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_set_routing_id (target, "target", 6));
    const int receive_timeout_ms = 3000;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (target, ZLINK_OPT_RCVTIMEO, &receive_timeout_ms,
                        sizeof (receive_timeout_ms)));
    if (router_)
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONFIG_OK,
          zlink_set_router_option (source, ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID,
                                   "target", 6));

    zlink_socket_monitor_open_options_t options = {};
    options.events = ZLINK_EVENT_CONNECTION_READY;
    monitor = zlink_socket_monitor_open (source, &options);
    TEST_ASSERT_NOT_NULL (monitor);
    char endpoint[256];
    if (tcp_)
        bind_loopback_ipv4 (target, endpoint, sizeof (endpoint));
    else {
        snprintf (endpoint, sizeof (endpoint), "inproc://explicit-removal-%d-%d",
                  router_, static_cast<int> (removal_));
        TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK, zlink_bind (target, endpoint));
    }
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (source, endpoint));
    wait_ready (monitor);
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_close (monitor));
    monitor = NULL;

    zlink_routing_id_t target_rid = {};
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_get_routing_id (target, &target_rid));
    zlink_completion_id_t ids[request_count] = {};
    int contexts[request_count] = {};
    for (int i = 0; i != request_count; ++i) {
        zlink_msg_t part;
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init_size (&part, 1));
        *static_cast<unsigned char *> (zlink_msg_data (&part)) =
          static_cast<unsigned char> (i);
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK,
          zlink_request_part (source, router_ ? &target_rid : NULL, &part,
                              ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL,
                              request_timeout_ms, &contexts[i], &ids[i]));
        TEST_ASSERT_NOT_EQUAL (0, ids[i]);
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));

        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&part));
        const zlink_routing_id_t *rid = NULL;
        zlink_reply_token_t token = 0;
        zlink_part_flag_t more = ZLINK_PART_MORE;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_RECV_OK,
          zlink_router_recv_part (target, &rid, &token, &part, &more,
                                  ZLINK_RECV_FLAGS_NONE));
        TEST_ASSERT_NOT_EQUAL (0, token);
        TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, more);
        TEST_ASSERT_EQUAL_UINT64 (1, zlink_msg_size (&part));
        TEST_ASSERT_EQUAL_INT (i, *static_cast<unsigned char *> (
                                   zlink_msg_data (&part)));
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
    }

    poller = zlink_poller_new ();
    TEST_ASSERT_NOT_NULL (poller);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_poller_add (poller, source, NULL, ZLINK_POLLCOMPLETION));
    const clock_type::time_point started = clock_type::now ();
    if (removal_ == endpoint_removal)
        TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                               zlink_disconnect (source, endpoint));
    else if (removal_ == rid_removal)
        TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                               zlink_disconnect_rid (source, &target_rid));
    else
        test_context_socket_close_zero_linger (target);

    bool seen[request_count] = {};
    for (int n = 0; n != request_count; ++n) {
        const long remaining = progress_bound_ms - static_cast<long> (
          std::chrono::duration_cast<std::chrono::milliseconds> (
            clock_type::now () - started).count ());
        TEST_ASSERT_GREATER_THAN_INT (0, remaining);
        zlink_poller_event_t event = {};
        zlink_config_result_t error = ZLINK_CONFIG_OK;
        TEST_ASSERT_EQUAL_INT (
          1, zlink_poller_wait (poller, &event, 1, remaining, &error));
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, error);
        zlink_completion_t completion = {};
        completion.struct_size = sizeof (completion);
        TEST_ASSERT_EQUAL_INT (
          ZLINK_RECV_OK,
          zlink_completion_recv (source, &completion, ZLINK_RECV_FLAGS_DONTWAIT));
        TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_REQUEST, completion.kind);
        TEST_ASSERT_EQUAL_INT (removal_ == target_close
                                 ? ZLINK_REQUEST_NOT_CONNECTED
                                 : ZLINK_REQUEST_NOT_FOUND,
                               completion.request_result);
        int index = 0;
        while (index != request_count && ids[index] != completion.completion_id)
            ++index;
        TEST_ASSERT_LESS_THAN_INT (request_count, index);
        TEST_ASSERT_FALSE (seen[index]);
        seen[index] = true;
        TEST_ASSERT_EQUAL_PTR (&contexts[index], completion.user_context);
        zlink_completion_close (&completion);
    }
    const long elapsed = static_cast<long> (
      std::chrono::duration_cast<std::chrono::milliseconds> (
        clock_type::now () - started).count ());
    TEST_ASSERT_LESS_OR_EQUAL_INT (progress_bound_ms, elapsed);

    // Observe beyond the original deadlines: neither pair teardown nor the
    // timeout owner may publish a second terminal record for a removed request.
    zlink_poller_event_t event = {};
    zlink_config_result_t error = ZLINK_CONFIG_OK;
    TEST_ASSERT_EQUAL_INT (
      0, zlink_poller_wait (poller, &event, 1, request_timeout_ms, &error));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, error);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_poller_remove (poller, source));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_poller_destroy (&poller));
    if (removal_ != target_close)
        test_context_socket_close_zero_linger (target);
    test_context_socket_close_zero_linger (source);
}

#define REMOVAL_CASE(name, router, tcp, removal)                                 \
    void name () { run_case (router, tcp, removal); }
REMOVAL_CASE (test_router_tcp_endpoint, true, true, endpoint_removal)
REMOVAL_CASE (test_router_inproc_endpoint, true, false, endpoint_removal)
REMOVAL_CASE (test_dealer_tcp_endpoint, false, true, endpoint_removal)
REMOVAL_CASE (test_dealer_inproc_endpoint, false, false, endpoint_removal)
REMOVAL_CASE (test_router_tcp_rid, true, true, rid_removal)
REMOVAL_CASE (test_router_inproc_rid, true, false, rid_removal)
REMOVAL_CASE (test_dealer_tcp_rid, false, true, rid_removal)
REMOVAL_CASE (test_dealer_inproc_rid, false, false, rid_removal)
REMOVAL_CASE (test_router_tcp_close, true, true, target_close)
REMOVAL_CASE (test_router_inproc_close, true, false, target_close)
REMOVAL_CASE (test_dealer_tcp_close, false, true, target_close)
REMOVAL_CASE (test_dealer_inproc_close, false, false, target_close)
#undef REMOVAL_CASE
}

int main ()
{
    setup_test_environment ();
    UNITY_BEGIN ();
    RUN_TEST (test_router_tcp_endpoint);
    RUN_TEST (test_router_inproc_endpoint);
    RUN_TEST (test_dealer_tcp_endpoint);
    RUN_TEST (test_dealer_inproc_endpoint);
    RUN_TEST (test_router_tcp_rid);
    RUN_TEST (test_router_inproc_rid);
    RUN_TEST (test_dealer_tcp_rid);
    RUN_TEST (test_dealer_inproc_rid);
    RUN_TEST (test_router_tcp_close);
    RUN_TEST (test_router_inproc_close);
    RUN_TEST (test_dealer_tcp_close);
    RUN_TEST (test_dealer_inproc_close);
    return UNITY_END ();
}
