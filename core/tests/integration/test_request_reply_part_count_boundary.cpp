/* SPDX-License-Identifier: MPL-2.0 */

//  Reply part-count boundary regression (D-BP43).
//
//  A ROUTER reply whose part count crosses the count-1 Application lane's
//  private-head batch boundary must be delivered as one REQUEST completion.
//  The count-1 completion drain claim/release must stay balanced for any part
//  count, on every completion pull shape (blocking, DONTWAIT, poller).

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <chrono>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

SETUP_TEARDOWN_TESTCONTEXT

namespace
{
const int kWaitMilliseconds = 20000;

enum pull_shape_t
{
    pull_dontwait,
    pull_blocking,
    pull_poller
};

void init_part (zlink_msg_t *part_, const std::string &payload_)
{
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_msg_init_size (part_, payload_.size ()));
    if (!payload_.empty ())
        memcpy (zlink_msg_data (part_), payload_.data (), payload_.size ());
}

std::string part_string (zlink_msg_t *part_)
{
    return std::string (static_cast<const char *> (zlink_msg_data (part_)),
                        zlink_msg_size (part_));
}

void assert_part_consumed (zlink_msg_t *part_)
{
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (part_));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (part_));
}

void set_routing_id_text (void *socket_, const char *value_)
{
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_routing_id (socket_, value_, strlen (value_)));
}

struct router_part_t
{
    zlink_routing_id_t source_rid;
    zlink_reply_token_t reply_token;
    std::string payload;
};

router_part_t receive_router_part_eventually (void *router_)
{
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now ()
      + std::chrono::milliseconds (kWaitMilliseconds);
    while (std::chrono::steady_clock::now () < deadline) {
        const zlink_routing_id_t *source_rid = NULL;
        zlink_reply_token_t reply_token = 0;
        zlink_msg_t part;
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&part));
        size_t part_flag = 1;
        errno = 0;
        const zlink_recv_result_t result = zlink_router_recv (router_, &source_rid, &reply_token, &part, 1, &part_flag, ZLINK_RECV_FLAGS_DONTWAIT);
        if (result == ZLINK_RECV_OK) {
            TEST_ASSERT_NOT_NULL (source_rid);
            router_part_t received;
            received.source_rid = *source_rid;
            received.reply_token = reply_token;
            received.payload = part_string (&part);
            TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
            return received;
        }
        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA, result);
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
        msleep (1);
    }
    TEST_FAIL_MESSAGE ("timed out waiting for ROUTER request part");
    return router_part_t ();
}

std::string expected_reply_payload (size_t index_)
{
    char buffer[32];
    snprintf (buffer, sizeof (buffer), "reply-%06u",
              static_cast<unsigned> (index_));
    return std::string (buffer);
}

void send_reply_with_parts (void *router_, const router_part_t &request_,
                            size_t part_count_)
{
    TEST_ASSERT_TRUE (part_count_ >= 1);
    std::vector<zlink_msg_t> parts (part_count_);
    for (size_t i = 0; i < part_count_; ++i) {
        init_part (&parts[i], expected_reply_payload (i));
    }
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_reply (router_, &request_.source_rid, request_.reply_token,
                   parts.data (), parts.size ()));
    for (size_t i = 0; i < part_count_; ++i)
        assert_part_consumed (&parts[i]);
}

zlink_completion_t receive_completion_eventually (void *dealer_,
                                                  pull_shape_t shape_)
{
    zlink_completion_t completion;
    memset (&completion, 0, sizeof (completion));
    completion.struct_size = sizeof (completion);

    if (shape_ == pull_blocking) {
        errno = 0;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_RECV_OK,
          zlink_completion_recv (dealer_, &completion,
                                 ZLINK_RECV_FLAGS_NONE));
        return completion;
    }

    void *poller = NULL;
    if (shape_ == pull_poller) {
        poller = zlink_poller_new ();
        TEST_ASSERT_NOT_NULL (poller);
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONFIG_OK,
          zlink_poller_add (poller, dealer_, NULL, ZLINK_POLLCOMPLETION));
    }

    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now ()
      + std::chrono::milliseconds (kWaitMilliseconds);
    while (std::chrono::steady_clock::now () < deadline) {
        if (poller) {
            zlink_poller_event_t event;
            memset (&event, 0, sizeof (event));
            zlink_config_result_t poller_error = ZLINK_CONFIG_OK;
            if (zlink_poller_wait (poller, &event, 1, 50, &poller_error) < 1)
                continue;
        }
        errno = 0;
        const zlink_recv_result_t result = zlink_completion_recv (
          dealer_, &completion, ZLINK_RECV_FLAGS_DONTWAIT);
        if (result == ZLINK_RECV_OK) {
            if (poller) {
                TEST_ASSERT_EQUAL_INT (
                  ZLINK_CONFIG_OK, zlink_poller_remove (poller, dealer_));
                TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK,
                                       zlink_poller_destroy (&poller));
            }
            return completion;
        }
        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA, result);
        if (!poller)
            msleep (1);
    }
    if (poller) {
        (void) zlink_poller_remove (poller, dealer_);
        (void) zlink_poller_destroy (&poller);
    }
    TEST_FAIL_MESSAGE ("timed out waiting for REQUEST completion");
    return completion;
}

void run_case (const char *endpoint_kind_, size_t part_count_,
               pull_shape_t shape_, bool concurrent_reply_ = false)
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer);
    set_routing_id_text (dealer, "part-count-boundary-dealer");

    char endpoint[256];
    if (strcmp (endpoint_kind_, "inproc") == 0)
        snprintf (endpoint, sizeof (endpoint),
                  "inproc://part-count-boundary-%u-%d",
                  static_cast<unsigned> (part_count_),
                  static_cast<int> (shape_));
    else
        snprintf (endpoint, sizeof (endpoint), "tcp://127.0.0.1:*");

    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK, zlink_bind (router, endpoint));
    char connect_endpoint[256];
    size_t connect_size = sizeof (connect_endpoint);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_get_option (router, ZLINK_OPT_LAST_ENDPOINT, connect_endpoint,
                        &connect_size));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK,
                           zlink_connect (dealer, connect_endpoint));
    msleep (SETTLE_TIME);

    zlink_msg_t request;
    init_part (&request, "part-count-boundary-request");
    zlink_completion_id_t request_id = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_request (dealer, NULL, &request, 1, ZLINK_SEND_FLAGS_NONE, 120000, NULL, &request_id));
    TEST_ASSERT_NOT_EQUAL (0, request_id);
    assert_part_consumed (&request);

    zlink_completion_t completion;
    if (concurrent_reply_) {
        //  Mirror the reported Rust reproduction: the reply is submitted from
        //  a second thread while the requester is already parked on its
        //  completion pull.
        std::thread server ([router, part_count_] () {
            const router_part_t received =
              receive_router_part_eventually (router);
            TEST_ASSERT_NOT_EQUAL (0, received.reply_token);
            send_reply_with_parts (router, received, part_count_);
        });
        completion = receive_completion_eventually (dealer, shape_);
        server.join ();
    } else {
        const router_part_t received = receive_router_part_eventually (router);
        TEST_ASSERT_NOT_EQUAL (0, received.reply_token);
        send_reply_with_parts (router, received, part_count_);
        completion = receive_completion_eventually (dealer, shape_);
    }
    TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_REQUEST, completion.kind);
    TEST_ASSERT_EQUAL_UINT64 (request_id, completion.completion_id);
    TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, completion.request_result);
    TEST_ASSERT_EQUAL_UINT64 (part_count_, completion.reply_part_count);
    for (size_t i = 0; i < completion.reply_part_count; ++i)
        TEST_ASSERT_EQUAL_STRING (
          expected_reply_payload (i).c_str (),
          part_string (&completion.reply_parts[i]).c_str ());
    zlink_completion_close (&completion);

    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
}

const size_t kPartCounts[] = {1023, 1024, 1025, 2048};

void test_inproc_reply_part_count_boundary_dontwait ()
{
    for (size_t i = 0; i < sizeof (kPartCounts) / sizeof (kPartCounts[0]); ++i)
        run_case ("inproc", kPartCounts[i], pull_dontwait);
}

void test_inproc_reply_part_count_boundary_blocking ()
{
    for (size_t i = 0; i < sizeof (kPartCounts) / sizeof (kPartCounts[0]); ++i)
        run_case ("inproc", kPartCounts[i], pull_blocking);
}

void test_inproc_reply_part_count_boundary_poller ()
{
    for (size_t i = 0; i < sizeof (kPartCounts) / sizeof (kPartCounts[0]); ++i)
        run_case ("inproc", kPartCounts[i], pull_poller);
}

void test_inproc_reply_part_count_boundary_concurrent_blocking ()
{
    for (size_t i = 0; i < sizeof (kPartCounts) / sizeof (kPartCounts[0]); ++i)
        run_case ("inproc", kPartCounts[i], pull_blocking, true);
}

void test_inproc_reply_part_count_boundary_concurrent_poller ()
{
    for (size_t i = 0; i < sizeof (kPartCounts) / sizeof (kPartCounts[0]); ++i)
        run_case ("inproc", kPartCounts[i], pull_poller, true);
}

void test_tcp_reply_part_count_boundary_concurrent_blocking ()
{
    for (size_t i = 0; i < sizeof (kPartCounts) / sizeof (kPartCounts[0]); ++i)
        run_case ("tcp", kPartCounts[i], pull_blocking, true);
}




void test_tcp_reply_part_count_boundary_dontwait ()
{
    for (size_t i = 0; i < sizeof (kPartCounts) / sizeof (kPartCounts[0]); ++i)
        run_case ("tcp", kPartCounts[i], pull_dontwait);
}
}

int main ()
{
    setup_test_environment ();
    UNITY_BEGIN ();
    RUN_TEST (test_inproc_reply_part_count_boundary_dontwait);
    RUN_TEST (test_inproc_reply_part_count_boundary_blocking);
    RUN_TEST (test_inproc_reply_part_count_boundary_poller);
    RUN_TEST (test_inproc_reply_part_count_boundary_concurrent_blocking);
    RUN_TEST (test_inproc_reply_part_count_boundary_concurrent_poller);
    RUN_TEST (test_tcp_reply_part_count_boundary_dontwait);
    RUN_TEST (test_tcp_reply_part_count_boundary_concurrent_blocking);
    return UNITY_END ();
}
