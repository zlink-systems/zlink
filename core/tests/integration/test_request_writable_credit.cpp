/* SPDX-License-Identifier: MPL-2.0 */
// Public C API reproduction of repeated WRITABLE without request credit.
#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <algorithm>
#include <chrono>
#include <stdio.h>
#include <vector>

static void *completion_poller = NULL;
void setUp () { setup_test_context (); }
void tearDown ()
{
    if (completion_poller)
        zlink_poller_destroy (&completion_poller);
    teardown_test_context ();
}

namespace
{
typedef std::chrono::steady_clock clock_type;
const int request_timeout_ms = 5000;
const int attempt_limit = 1000;
const int repetitions = 5;

zlink_submit_result_t request (void *dealer_, size_t size_,
                               zlink_send_flags_t flags_, void *context_,
                               zlink_completion_id_t *id_,
                               const zlink_routing_id_t *target_ = NULL,
                               int timeout_ms_ = request_timeout_ms)
{
    zlink_msg_t body;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_msg_init_size (&body, size_));
    memset (zlink_msg_data (&body), 0x5a, size_);
    zlink_submit_result_t rc = zlink_request_part (
      dealer_, target_, &body, flags_, ZLINK_PART_MORE, 0, NULL, NULL);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&body));
    if (rc != ZLINK_SUBMIT_OK)
        return rc;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&body));
    rc = zlink_request_part (dealer_, target_, &body, flags_, ZLINK_PART_FINAL,
                             timeout_ms_, context_, id_);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&body));
    return rc;
}

void receive_request (void *router_, size_t size_, zlink_routing_id_t *rid_,
                      zlink_reply_token_t *token_)
{
    for (int i = 0; i != 2; ++i) {
        zlink_msg_t part;
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&part));
        const zlink_routing_id_t *rid = NULL;
        zlink_part_flag_t more = ZLINK_PART_MORE;
        zlink_reply_token_t token = 0;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_RECV_OK,
          zlink_router_recv_part (router_, &rid, &token, &part, &more,
                                  ZLINK_RECV_FLAGS_NONE));
        TEST_ASSERT_NOT_NULL (rid);
        TEST_ASSERT_NOT_EQUAL (0, token);
        TEST_ASSERT_EQUAL_INT (i == 0 ? ZLINK_PART_MORE : ZLINK_PART_FINAL,
                               more);
        TEST_ASSERT_EQUAL_UINT64 (i == 0 ? size_ : 0, zlink_msg_size (&part));
        *rid_ = *rid;
        *token_ = token;
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
    }
}

void reply (void *router_, const zlink_routing_id_t *rid_,
            zlink_reply_token_t token_)
{
    zlink_msg_t part;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&part));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_reply_part (router_, rid_, token_, &part, ZLINK_PART_FINAL));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
}

void receive_reply_completion (void *dealer_, zlink_completion_id_t id_,
                               void *context_,
                               zlink_completion_id_t writable_id_ = 0,
                               void *writable_context_ = NULL,
                               zlink_request_result_t result_ = ZLINK_REQUEST_OK,
                               int terminal_errno_ = 0,
                               const zlink_routing_id_t *target_ = NULL)
{
    bool reply_seen = false;
    bool writable_seen = writable_id_ == 0;
    completion_poller = zlink_poller_new ();
    TEST_ASSERT_NOT_NULL (completion_poller);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_poller_add (completion_poller, dealer_, NULL, ZLINK_POLLCOMPLETION));
    const clock_type::time_point deadline =
      clock_type::now () + std::chrono::seconds (3);
    for (;;) {
        const long remaining = static_cast<long> (
          std::chrono::duration_cast<std::chrono::milliseconds> (
            deadline - clock_type::now ()).count ());
        TEST_ASSERT_GREATER_THAN_INT (0, remaining);
        zlink_poller_event_t event = {};
        zlink_config_result_t error = ZLINK_CONFIG_OK;
        TEST_ASSERT_EQUAL_INT (
          1, zlink_poller_wait (completion_poller, &event, 1, remaining, &error));
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, error);
        zlink_completion_t completion = {};
        completion.struct_size = sizeof (completion);
        TEST_ASSERT_EQUAL_INT (
          ZLINK_RECV_OK,
          zlink_completion_recv (dealer_, &completion,
                                 ZLINK_RECV_FLAGS_DONTWAIT));
        const bool matched = completion.completion_id == id_;
        if (matched) {
            TEST_ASSERT_FALSE (reply_seen);
            reply_seen = true;
            TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_REQUEST, completion.kind);
            TEST_ASSERT_EQUAL_INT (result_, completion.request_result);
            TEST_ASSERT_EQUAL_PTR (context_, completion.user_context);
        } else {
            TEST_ASSERT_FALSE (writable_seen);
            writable_seen = true;
            TEST_ASSERT_EQUAL_UINT64 (writable_id_, completion.completion_id);
            TEST_ASSERT_EQUAL_PTR (writable_context_, completion.user_context);
            TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_WRITABLE, completion.kind);
            TEST_ASSERT_EQUAL_INT (terminal_errno_ ? ZLINK_SEND_TERMINAL
                                                   : ZLINK_SEND_ADMITTED,
                                   completion.send_result);
            TEST_ASSERT_EQUAL_INT (terminal_errno_, completion.send_terminal_errno);
            if (target_) {
                TEST_ASSERT_EQUAL_UINT (target_->size, completion.peer_rid.size);
                TEST_ASSERT_EQUAL_MEMORY (target_->data, completion.peer_rid.data,
                                          target_->size);
            }
        }
        zlink_completion_close (&completion);
        if (reply_seen && writable_seen) {
            completion.struct_size = sizeof (completion);
            TEST_ASSERT_EQUAL_INT (
              ZLINK_RECV_NO_DATA,
              zlink_completion_recv (dealer_, &completion,
                                     ZLINK_RECV_FLAGS_DONTWAIT));
            TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                                   zlink_poller_remove (completion_poller, dealer_));
            TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK,
                                   zlink_poller_destroy (&completion_poller));
            return;
        }
    }
}

void run_case (size_t size_, bool tcp_)
{
    for (int run = 0; run != repetitions; ++run) {
        void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
        void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
        const int receive_timeout_ms = 3000;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONFIG_OK,
          zlink_set_option (router, ZLINK_OPT_RCVTIMEO, &receive_timeout_ms,
                            sizeof (receive_timeout_ms)));
        char endpoint[256];
        if (tcp_)
            bind_loopback_ipv4 (router, endpoint, sizeof (endpoint));
        else {
            snprintf (endpoint, sizeof (endpoint), "inproc://request-writable-%d", run);
            TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK, zlink_bind (router, endpoint));
        }
        TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (dealer, endpoint));
        int contexts[2] = {};
        zlink_completion_id_t first_id = 0;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK,
          request (dealer, size_, ZLINK_SEND_FLAGS_NONE, &contexts[0], &first_id));
        TEST_ASSERT_NOT_EQUAL (0, first_id);
        zlink_routing_id_t first_rid = {};
        zlink_reply_token_t first_token = 0;
        receive_request (router, size_, &first_rid, &first_token);

        // No reply, timeout or competing sender can return/consume correlation
        // credit during this interval. Drain each token before resubmitting the
        // complete multipart request, as required by the public contract.
        const clock_type::time_point start = clock_type::now ();
        clock_type::time_point previous = start;
        std::vector<double> intervals;
        int rejected = 0;
        int writable = 0;
        bool admitted = false;
        bool parked = false;
        zlink_completion_id_t second_id = 0;
        for (int attempt = 0; attempt != attempt_limit; ++attempt) {
            const zlink_submit_result_t rc = request (
              dealer, size_, ZLINK_SEND_FLAGS_DONTWAIT, &contexts[1], &second_id);
            if (rc == ZLINK_SUBMIT_OK) {
                admitted = true;
                break;
            }
            TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_BACKPRESSURED, rc);
            TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
            ++rejected;
            TEST_ASSERT_NOT_EQUAL (0, second_id);
            zlink_completion_t completion = {};
            completion.struct_size = sizeof (completion);
            const zlink_recv_result_t received = zlink_completion_recv (
              dealer, &completion, ZLINK_RECV_FLAGS_DONTWAIT);
            if (received == ZLINK_RECV_NO_DATA) {
                parked = true;
                break;
            }
            TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, received);
            TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_WRITABLE, completion.kind);
            TEST_ASSERT_EQUAL_UINT64 (second_id, completion.completion_id);
            TEST_ASSERT_EQUAL_PTR (&contexts[1], completion.user_context);
            TEST_ASSERT_EQUAL_INT (ZLINK_SEND_ADMITTED, completion.send_result);
            TEST_ASSERT_EQUAL_INT (0, completion.send_terminal_errno);
            zlink_completion_close (&completion);
            const clock_type::time_point now = clock_type::now ();
            intervals.push_back (
              std::chrono::duration<double, std::micro> (now - previous).count ());
            previous = now;
            ++writable;
            completion.struct_size = sizeof (completion);
            TEST_ASSERT_EQUAL_INT (
              ZLINK_RECV_NO_DATA,
              zlink_completion_recv (dealer, &completion,
                                     ZLINK_RECV_FLAGS_DONTWAIT));
            // End the observation well before the first request's timeout.
            if (now - start > std::chrono::milliseconds (200))
                break;
        }
        const double elapsed_ms =
          std::chrono::duration<double, std::milli> (clock_type::now () - start).count ();
        const bool immediate_success = admitted;
        if (size_ == 65536) {
            TEST_ASSERT_FALSE (immediate_success);
            TEST_ASSERT_TRUE (parked);
            TEST_ASSERT_EQUAL_INT (1, rejected);
            TEST_ASSERT_EQUAL_INT (0, writable);
            // Process delayed physical-credit activations while correlation
            // remains reserved. None may publish this token (D-B119).
            completion_poller = zlink_poller_new ();
            TEST_ASSERT_NOT_NULL (completion_poller);
            TEST_ASSERT_EQUAL_INT (
              ZLINK_CONFIG_OK,
              zlink_poller_add (completion_poller, dealer, NULL,
                                ZLINK_POLLCOMPLETION));
            zlink_poller_event_t event = {};
            zlink_config_result_t error = ZLINK_CONFIG_OK;
            TEST_ASSERT_EQUAL_INT (
              0, zlink_poller_wait (completion_poller, &event, 1, 50, &error));
            TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, error);
            TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK,
                                   zlink_poller_destroy (&completion_poller));
        } else {
            TEST_ASSERT_TRUE (immediate_success);
            TEST_ASSERT_EQUAL_INT (0, writable);
        }
        reply (router, &first_rid, first_token);
        receive_reply_completion (dealer, first_id, &contexts[0],
                                  parked ? second_id : 0, &contexts[1]);
        if (!admitted) {
            TEST_ASSERT_EQUAL_INT (
              ZLINK_SUBMIT_OK,
              request (dealer, size_, ZLINK_SEND_FLAGS_DONTWAIT,
                       &contexts[1], &second_id));
            admitted = true;
        }
        zlink_routing_id_t second_rid = {};
        zlink_reply_token_t second_token = 0;
        receive_request (router, size_, &second_rid, &second_token);
        reply (router, &second_rid, second_token);
        receive_reply_completion (dealer, second_id, &contexts[1]);
        std::sort (intervals.begin (), intervals.end ());
        printf ("request_writable transport=%s bytes=%zu run=%d rejected=%d "
                "writable=%d parked=%d immediate_success=%d final_success=%d "
                "elapsed_ms=%.3f interval_us[min,p50,p95,max]=[%.3f,%.3f,%.3f,%.3f]\n",
                tcp_ ? "tcp" : "inproc", size_, run, rejected, writable,
                parked, immediate_success, admitted, elapsed_ms,
                intervals.empty () ? 0 : intervals.front (),
                intervals.empty () ? 0 : intervals[intervals.size () / 2],
                intervals.empty () ? 0 : intervals[(intervals.size () - 1) * 95 / 100],
                intervals.empty () ? 0 : intervals.back ());
        fflush (stdout);
        test_context_socket_close_zero_linger (dealer);
        test_context_socket_close_zero_linger (router);
    }
}

void test_correlation_release_terminal_events (int event_)
{
    void *server = test_context_socket (ZLINK_SOCKET_ROUTER);
    const bool routed = event_ == 2;
    void *client = test_context_socket (routed ? ZLINK_SOCKET_ROUTER
                                              : ZLINK_SOCKET_DEALER);
    const zlink_routing_id_t target = {1, {'s'}};
    if (routed) {
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                               zlink_set_routing_id (server, "s", 1));
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONFIG_OK,
          zlink_set_router_option (client, ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID,
                                   "s", 1));
    }
    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK,
                           zlink_bind (server, "inproc://credit-terminal"));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                           zlink_connect (client, "inproc://credit-terminal"));
    const zlink_routing_id_t *rid = routed ? &target : NULL;
    int contexts[2] = {};
    zlink_completion_id_t accepted = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      request (client, 65536, ZLINK_SEND_FLAGS_NONE, &contexts[0], &accepted,
               rid, event_ == 0 ? 100 : request_timeout_ms));
    zlink_routing_id_t source = {};
    zlink_reply_token_t reply_token = 0;
    receive_request (server, 65536, &source, &reply_token);
    zlink_completion_id_t waiter = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_BACKPRESSURED,
      request (client, 65536, ZLINK_SEND_FLAGS_DONTWAIT, &contexts[1], &waiter,
               rid));
    TEST_ASSERT_NOT_EQUAL (0, waiter);
    if (event_ == 1) {
        test_context_socket_close_zero_linger (server);
        server = NULL;
    } else if (event_ == 2) {
        TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                               zlink_disconnect_rid (client, &target));
    }
    if (event_ != 3)
        receive_reply_completion (
          client, accepted, &contexts[0], waiter, &contexts[1],
          event_ == 0 ? ZLINK_REQUEST_TIMED_OUT
          : event_ == 2 ? ZLINK_REQUEST_NOT_FOUND
                        : ZLINK_REQUEST_NOT_CONNECTED,
          event_ == 2 ? ENOENT : 0, rid);
    test_context_socket_close_zero_linger (client);
    if (server)
        test_context_socket_close_zero_linger (server);
}

void test_timeout_returns_correlation () { test_correlation_release_terminal_events (0); }
void test_disconnect_returns_correlation () { test_correlation_release_terminal_events (1); }
void test_explicit_removal_terminates_correlation_wait () { test_correlation_release_terminal_events (2); }
void test_close_reclaims_correlation_wait () { test_correlation_release_terminal_events (3); }

void test_router_pair_correlation_isolation ()
{
    void *client = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *servers[2];
    zlink_routing_id_t targets[2] = {{1, {'a'}}, {1, {'b'}}};
    zlink_routing_id_t sources[2] = {};
    zlink_reply_token_t replies[2] = {};
    zlink_completion_id_t accepted[2] = {}, waiters[2] = {};
    int contexts[4] = {};
    for (int i = 0; i != 2; ++i) {
        servers[i] = test_context_socket (ZLINK_SOCKET_ROUTER);
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONFIG_OK,
          zlink_set_routing_id (servers[i], targets[i].data, targets[i].size));
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONFIG_OK,
          zlink_set_router_option (client, ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID,
                                   targets[i].data, targets[i].size));
        const char *endpoint = i == 0 ? "inproc://credit-a" : "inproc://credit-b";
        TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK, zlink_bind (servers[i], endpoint));
        TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (client, endpoint));
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK,
          request (client, 65536, ZLINK_SEND_FLAGS_NONE, &contexts[i], &accepted[i],
                   &targets[i]));
        receive_request (servers[i], 65536, &sources[i], &replies[i]);
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_BACKPRESSURED,
          request (client, 65536, ZLINK_SEND_FLAGS_DONTWAIT,
                   &contexts[2 + i], &waiters[i], &targets[i]));
        TEST_ASSERT_NOT_EQUAL (0, waiters[i]);
    }
    for (int i = 0; i != 2; ++i) {
        reply (servers[i], &sources[i], replies[i]);
        // The other target's reservation has not returned. Its token must
        // remain parked even though both physical application pipes are empty.
        receive_reply_completion (client, accepted[i], &contexts[i], waiters[i],
                                  &contexts[2 + i], ZLINK_REQUEST_OK, 0, &targets[i]);
    }
    test_context_socket_close_zero_linger (client);
    for (int i = 0; i != 2; ++i)
        test_context_socket_close_zero_linger (servers[i]);
}

void test_inproc_64b () { run_case (64, false); }
void test_inproc_4k () { run_case (4096, false); }
void test_inproc_64k () { run_case (65536, false); }
void test_tcp_64b () { run_case (64, true); }
void test_tcp_4k () { run_case (4096, true); }
void test_tcp_64k () { run_case (65536, true); }
}

int main ()
{
    setup_test_environment ();
    UNITY_BEGIN ();
    RUN_TEST (test_inproc_64b);
    RUN_TEST (test_inproc_4k);
    RUN_TEST (test_inproc_64k);
    RUN_TEST (test_tcp_64b);
    RUN_TEST (test_tcp_4k);
    RUN_TEST (test_tcp_64k);
    RUN_TEST (test_timeout_returns_correlation);
    RUN_TEST (test_disconnect_returns_correlation);
    RUN_TEST (test_explicit_removal_terminates_correlation_wait);
    RUN_TEST (test_close_reclaims_correlation_wait);
    RUN_TEST (test_router_pair_correlation_isolation);
    return UNITY_END ();
}
