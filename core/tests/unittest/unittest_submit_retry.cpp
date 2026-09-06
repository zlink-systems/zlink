/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_monitoring.hpp"
#include "testutil_unity.hpp"
#include "contract_socket_pair_fixture.hpp"
#include "core/send_internal.hpp"

#include <unity.h>
#include <atomic>
#include <chrono>
#include <thread>

extern "C" void zlink_test_set_submit_retry_fault (int count_, int err_);

namespace
{


void make_rid (const char *value_, zlink_routing_id_t *out_)
{
    memset (out_, 0, sizeof (*out_));
    out_->size = static_cast<uint8_t> (strlen (value_));
    memcpy (out_->data, value_, out_->size);
}



void recv_router_payload_expect_success (void *router_, const char *payload_)
{
    recv_routed_string_expect_success (router_, payload_);
}

void configure_send_timeout (void *socket_, int timeout_ms_)
{
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (socket_, ZLINK_OPT_SNDTIMEO, &timeout_ms_,
                        sizeof (timeout_ms_)));
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

}

void blocking_directed_send_times_out_as_backpressured ()
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
    configure_send_timeout (client, 40);
    zlink_test_set_submit_retry_fault (64, ENOTCONN);

    void *server = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (server, "S", 1));
    contract_socket_pair_t pair (client, server);

    zlink_routing_id_t rid;
    make_rid ("S", &rid);

    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now ();
    TEST_ASSERT_EQUAL_INT (-1, test_stream_send_bytes (client, &rid, "expired", 7, 0));
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);
    const long elapsed_ms = elapsed_ms_since (start);
    TEST_ASSERT_TRUE_MESSAGE (elapsed_ms >= 20, "SNDTIMEO should wait for local reconnect");
    TEST_ASSERT_TRUE_MESSAGE (elapsed_ms < 200, "SNDTIMEO should expire promptly");

    zlink_test_set_submit_retry_fault (0, 0);
    test_context_socket_close_zero_linger (client);
    test_context_socket_close_zero_linger (server);
}

void blocking_directed_send_retries_multipart_final_frame ()
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
    configure_send_timeout (client, 200);

    contract_socket_pair_t pair (client, server);
    zlink_routing_id_t rid;
    make_rid ("S", &rid);

    zlink_msg_t first;
    init_string_msg (&first, "one");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK, zlink_send_part_rid (client, &rid, &first,
                                            static_cast<zlink_send_flags_t> (0), ZLINK_PART_MORE,
                                            NULL, NULL));

    zlink_test_set_submit_retry_fault (1, ENOTCONN);
    zlink_msg_t second;
    init_string_msg (&second, "two");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK, zlink_send_part_rid (client, &rid, &second,
                                            static_cast<zlink_send_flags_t> (0), ZLINK_PART_FINAL,
                                            NULL, NULL));

    recv_routed_string_expect_success (server, "one", NULL, ZLINK_PART_MORE);
    recv_routed_string_expect_success (server, "two");

    zlink_test_set_submit_retry_fault (0, 0);
    test_context_socket_close_zero_linger (client);
    test_context_socket_close_zero_linger (server);
}

void dontwait_local_admission_wakes_when_first_target_attaches ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);

    int one = 1;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (dealer, ZLINK_OPT_IMMEDIATE, &one, sizeof (one)));
    contract_socket_pair_t pair (router, dealer, 1, 1, false);

    zlink_msg_t before;
    init_string_msg (&before, "before");
    zlink_test_set_submit_retry_fault (1, ECONNREFUSED);
    errno = 0;
    TEST_ASSERT_EQUAL_INT (-1,
      zlink::send_msg_internal (pair.cores[1], &before, ZLINK_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (ECONNREFUSED, errno);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&before));

    pair.attach ();

    //  The readiness hint that used to be observed here is gone. The
    //  observable that actually mattered survives unchanged: once the first
    //  target attaches, the armed local-admission recovery lets a DONTWAIT
    //  submit through instead of pinning ECONNREFUSED forever.
    zlink_msg_t after;
    init_string_msg (&after, "after");
    const bool submitted =
      zlink::send_msg_internal (pair.cores[1], &after, ZLINK_DONTWAIT) == 5;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&after));
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
int main ()
{
    setup_test_environment ();
    UNITY_BEGIN ();
    RUN_TEST (blocking_directed_send_times_out_as_backpressured);
    RUN_TEST (blocking_directed_send_retries_multipart_final_frame);
    RUN_TEST (dontwait_local_admission_wakes_when_first_target_attaches);
    return UNITY_END ();
}
