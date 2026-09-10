/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <cstring>

SETUP_TEARDOWN_TESTCONTEXT

namespace
{
void init_part (zlink_msg_t *part_, const char *text_)
{
    const size_t size = strlen (text_);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_msg_init_size (part_, size));
    memcpy (zlink_msg_data (part_), text_, size);
}

void assert_part (const zlink_msg_t *part_, const char *expected_)
{
    const size_t expected_size = strlen (expected_);
    TEST_ASSERT_EQUAL_UINT64 (expected_size, zlink_msg_size (part_));
    TEST_ASSERT_EQUAL_MEMORY (expected_, zlink_msg_data (
                                           const_cast<zlink_msg_t *> (part_)),
                              expected_size);
}

void send_record (void *socket_, const char *const *parts_, size_t part_count_)
{
    for (size_t i = 0; i < part_count_; ++i) {
        zlink_msg_t part;
        init_part (&part, parts_[i]);
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK,
          zlink_send_part (socket_, &part, ZLINK_SEND_FLAGS_NONE,
                           i + 1 < part_count_ ? ZLINK_PART_MORE
                                             : ZLINK_PART_FINAL,
                           NULL, NULL));
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
    }
}

void send_routed_record (void *router_, const zlink_routing_id_t *target_,
                         const char *const *parts_, size_t part_count_)
{
    for (size_t i = 0; i < part_count_; ++i) {
        zlink_msg_t part;
        init_part (&part, parts_[i]);
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK,
          zlink_send_part_rid (router_, target_, &part,
                               ZLINK_SEND_FLAGS_NONE,
                               i + 1 < part_count_ ? ZLINK_PART_MORE
                                                 : ZLINK_PART_FINAL,
                               NULL, NULL));
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
    }
}

zlink_completion_id_t send_request_record (void *dealer_,
                                           const char *const *parts_,
                                           size_t part_count_)
{
    zlink_completion_id_t completion_id = 0;
    for (size_t i = 0; i < part_count_; ++i) {
        zlink_msg_t part;
        init_part (&part, parts_[i]);
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK,
          zlink_request_part (
            dealer_, NULL, &part, ZLINK_SEND_FLAGS_NONE,
            i + 1 < part_count_ ? ZLINK_PART_MORE : ZLINK_PART_FINAL,
            i + 1 < part_count_ ? 0 : 3000, NULL,
            i + 1 < part_count_ ? NULL : &completion_id));
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
    }
    TEST_ASSERT_NOT_EQUAL (0, completion_id);
    return completion_id;
}

void wait_readable (void *socket_)
{
    zlink_pollitem_t item = {socket_, 0, ZLINK_POLLIN, 0};
    zlink_config_result_t error = ZLINK_CONFIG_INTERNAL_ERROR;
    TEST_ASSERT_EQUAL_INT (1, zlink_poll (&item, 1, 3000, &error));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, error);
    TEST_ASSERT_BITS_HIGH (ZLINK_POLLIN, item.revents);
}

void assert_rid (const zlink_routing_id_t *rid_, const char *expected_)
{
    TEST_ASSERT_NOT_NULL (rid_);
    TEST_ASSERT_EQUAL_UINT64 (strlen (expected_), rid_->size);
    TEST_ASSERT_EQUAL_MEMORY (expected_, rid_->data, rid_->size);
}

zlink_routing_id_t establish_router_route (void *router_, void *dealer_)
{
    const char *const prime[] = {"prime"};
    send_record (dealer_, prime, 1);
    wait_readable (router_);

    zlink_msg_t received[1];
    memset (received, 0xa5, sizeof (received));
    const zlink_routing_id_t *source = NULL;
    zlink_reply_token_t token = UINT64_MAX;
    size_t count = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      ::zlink_router_recv (router_, &source, &token, received, 1, &count,
                           ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_UINT64 (1, count);
    TEST_ASSERT_EQUAL_UINT64 (0, token);
    assert_part (&received[0], "prime");
    zlink_routing_id_t source_copy = *source;
    zlink_multipart_close (received, count);
    return source_copy;
}

void assert_recv_part (void *socket_, const char *expected_,
                       zlink_part_flag_t expected_more_)
{
    zlink_msg_t part;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&part));
    const zlink_routing_id_t *source =
      reinterpret_cast<const zlink_routing_id_t *> (1);
    zlink_part_flag_t more = ZLINK_PART_FINAL;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_recv_part (socket_, &source, &part, &more,
                       ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_NULL (source);
    TEST_ASSERT_EQUAL_INT (expected_more_, more);
    assert_part (&part, expected_);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
}

void assert_router_recv_part (void *router_, const char *expected_,
                              const char *expected_rid_,
                              zlink_part_flag_t expected_more_)
{
    zlink_msg_t part;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&part));
    const zlink_routing_id_t *source = NULL;
    zlink_reply_token_t token = UINT64_MAX;
    zlink_part_flag_t more = ZLINK_PART_FINAL;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_router_recv_part (router_, &source, &token, &part, &more,
                              ZLINK_RECV_FLAGS_DONTWAIT));
    assert_rid (source, expected_rid_);
    TEST_ASSERT_EQUAL_UINT64 (0, token);
    TEST_ASSERT_EQUAL_INT (expected_more_, more);
    assert_part (&part, expected_);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
}
}

void test_pair_whole_recv_is_atomic_and_supports_single_part ()
{
    void *receiver = test_context_socket (ZLINK_SOCKET_PAIR);
    void *sender = test_context_socket (ZLINK_SOCKET_PAIR);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_BIND_OK,
      zlink_bind (receiver, "inproc://whole-recv-pair-atomic"));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONNECT_OK,
      zlink_connect (sender, "inproc://whole-recv-pair-atomic"));

    zlink_msg_t unchanged;
    init_part (&unchanged, "unchanged");
    const zlink_routing_id_t *source =
      reinterpret_cast<const zlink_routing_id_t *> (1);
    size_t count = 41;
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_NO_DATA,
      ::zlink_recv (receiver, &source, &unchanged, 1, &count,
                    ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);
    TEST_ASSERT_EQUAL_PTR (reinterpret_cast<const void *> (1), source);
    TEST_ASSERT_EQUAL_UINT64 (41, count);
    assert_part (&unchanged, "unchanged");
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&unchanged));

    const char *const multipart[] = {"alpha", "beta", "gamma"};
    send_record (sender, multipart, 3);
    wait_readable (receiver);
    zlink_msg_t received[3];
    memset (received, 0xa5, sizeof (received));
    source = reinterpret_cast<const zlink_routing_id_t *> (1);
    count = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      ::zlink_recv (receiver, &source, received, 3, &count,
                    ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_NULL (source);
    TEST_ASSERT_EQUAL_UINT64 (3, count);
    for (size_t i = 0; i < count; ++i)
        assert_part (&received[i], multipart[i]);
    zlink_multipart_close (received, count);

    const char *const single[] = {"solo"};
    send_record (sender, single, 1);
    wait_readable (receiver);
    zlink_msg_t one[1];
    memset (one, 0xa5, sizeof (one));
    count = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      ::zlink_recv (receiver, NULL, one, 1, &count,
                    ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_UINT64 (1, count);
    assert_part (&one[0], "solo");
    zlink_multipart_close (one, count);

    test_context_socket_close_zero_linger (sender);
    test_context_socket_close_zero_linger (receiver);
}

void test_dealer_whole_recv_capacity_retry_preserves_record ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_set_routing_id (dealer, "dealer-whole", 12));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_BIND_OK,
      zlink_bind (router, "inproc://whole-recv-dealer-capacity"));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONNECT_OK,
      zlink_connect (dealer, "inproc://whole-recv-dealer-capacity"));
    const zlink_routing_id_t route = establish_router_route (router, dealer);

    const char *const payloads[] = {"dealer-a", "dealer-b", "dealer-c"};
    send_routed_record (router, &route, payloads, 3);
    wait_readable (dealer);

    zlink_msg_t too_small[2];
    init_part (&too_small[0], "keep-0");
    init_part (&too_small[1], "keep-1");
    const zlink_routing_id_t *source =
      reinterpret_cast<const zlink_routing_id_t *> (1);
    size_t count = 77;
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_BUFFER_TOO_SMALL,
      ::zlink_recv (dealer, &source, too_small, 2, &count,
                    ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (ENOBUFS, errno);
    TEST_ASSERT_EQUAL_UINT64 (3, count);
    TEST_ASSERT_EQUAL_PTR (reinterpret_cast<const void *> (1), source);
    assert_part (&too_small[0], "keep-0");
    assert_part (&too_small[1], "keep-1");
    zlink_multipart_close (too_small, 2);

    zlink_msg_t received[3];
    memset (received, 0xa5, sizeof (received));
    count = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      ::zlink_recv (dealer, &source, received, 3, &count,
                    ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_NULL (source);
    TEST_ASSERT_EQUAL_UINT64 (3, count);
    for (size_t i = 0; i < count; ++i)
        assert_part (&received[i], payloads[i]);
    zlink_multipart_close (received, count);

    zlink_msg_t absent;
    init_part (&absent, "still-here");
    count = 19;
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_NO_DATA,
      ::zlink_recv (dealer, NULL, &absent, 1, &count,
                    ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);
    TEST_ASSERT_EQUAL_UINT64 (19, count);
    assert_part (&absent, "still-here");
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&absent));

    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
}

void test_router_whole_recv_returns_data_and_request_metadata ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_set_routing_id (dealer, "requester", 9));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_BIND_OK,
      zlink_bind (router, "inproc://whole-recv-router-metadata"));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONNECT_OK,
      zlink_connect (dealer, "inproc://whole-recv-router-metadata"));

    const char *const data[] = {"data-head", "data-tail"};
    send_record (dealer, data, 2);
    wait_readable (router);
    zlink_msg_t received_data[2];
    memset (received_data, 0xa5, sizeof (received_data));
    const zlink_routing_id_t *data_source = NULL;
    zlink_reply_token_t data_token = UINT64_MAX;
    size_t data_count = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      ::zlink_router_recv (router, &data_source, &data_token, received_data,
                           2, &data_count, ZLINK_RECV_FLAGS_DONTWAIT));
    assert_rid (data_source, "requester");
    TEST_ASSERT_EQUAL_UINT64 (0, data_token);
    TEST_ASSERT_EQUAL_UINT64 (2, data_count);
    assert_part (&received_data[0], data[0]);
    assert_part (&received_data[1], data[1]);
    const zlink_routing_id_t data_source_copy = *data_source;
    zlink_multipart_close (received_data, data_count);

    const char *const request[] = {"request-head", "request-tail"};
    (void) send_request_record (dealer, request, 2);
    wait_readable (router);
    zlink_msg_t received_request[2];
    memset (received_request, 0xa5, sizeof (received_request));
    const zlink_routing_id_t *request_source = NULL;
    zlink_reply_token_t request_token = 0;
    size_t request_count = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      ::zlink_router_recv (router, &request_source, &request_token,
                           received_request, 2, &request_count,
                           ZLINK_RECV_FLAGS_DONTWAIT));
    assert_rid (request_source, "requester");
    TEST_ASSERT_TRUE (request_token != 0);
    TEST_ASSERT_EQUAL_UINT64 (data_source_copy.size, request_source->size);
    TEST_ASSERT_EQUAL_MEMORY (data_source_copy.data, request_source->data,
                              request_source->size);
    TEST_ASSERT_EQUAL_UINT64 (2, request_count);
    assert_part (&received_request[0], request[0]);
    assert_part (&received_request[1], request[1]);
    zlink_multipart_close (received_request, request_count);

    zlink_msg_t reply;
    init_part (&reply, "done");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_reply_part (router, request_source, request_token, &reply,
                        ZLINK_PART_FINAL));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&reply));

    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
}

void test_pair_whole_recv_mixes_with_recv_part_without_loss ()
{
    void *receiver = test_context_socket (ZLINK_SOCKET_PAIR);
    void *sender = test_context_socket (ZLINK_SOCKET_PAIR);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_BIND_OK,
      zlink_bind (receiver, "inproc://whole-recv-pair-mixing"));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONNECT_OK,
      zlink_connect (sender, "inproc://whole-recv-pair-mixing"));

    const char *const first[] = {"first-0", "first-1", "first-2"};
    send_record (sender, first, 3);
    wait_readable (receiver);
    assert_recv_part (receiver, first[0], ZLINK_PART_MORE);

    zlink_msg_t busy_parts[3];
    init_part (&busy_parts[0], "busy-keep");
    size_t busy_count = 23;
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_BUSY,
      ::zlink_recv (receiver, NULL, busy_parts, 3, &busy_count,
                    ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EBUSY, errno);
    TEST_ASSERT_EQUAL_UINT64 (23, busy_count);
    assert_part (&busy_parts[0], "busy-keep");
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&busy_parts[0]));
    assert_recv_part (receiver, first[1], ZLINK_PART_MORE);
    assert_recv_part (receiver, first[2], ZLINK_PART_FINAL);

    const char *const second[] = {"second-0", "second-1", "second-2"};
    send_record (sender, second, 3);
    wait_readable (receiver);
    zlink_msg_t too_small[1];
    init_part (&too_small[0], "capacity-keep");
    size_t needed = 0;
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_BUFFER_TOO_SMALL,
      ::zlink_recv (receiver, NULL, too_small, 1, &needed,
                    ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (ENOBUFS, errno);
    TEST_ASSERT_EQUAL_UINT64 (3, needed);
    assert_part (&too_small[0], "capacity-keep");
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&too_small[0]));
    assert_recv_part (receiver, second[0], ZLINK_PART_MORE);
    assert_recv_part (receiver, second[1], ZLINK_PART_MORE);
    assert_recv_part (receiver, second[2], ZLINK_PART_FINAL);

    test_context_socket_close_zero_linger (sender);
    test_context_socket_close_zero_linger (receiver);
}

void test_router_whole_recv_capacity_retry_and_recv_part_mix ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_set_routing_id (dealer, "router-mix", 10));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_BIND_OK,
      zlink_bind (router, "inproc://whole-recv-router-mixing"));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONNECT_OK,
      zlink_connect (dealer, "inproc://whole-recv-router-mixing"));

    const char *const capacity_payloads[] = {"cap-0", "cap-1", "cap-2"};
    send_record (dealer, capacity_payloads, 3);
    wait_readable (router);
    zlink_msg_t too_small[2];
    init_part (&too_small[0], "keep-a");
    init_part (&too_small[1], "keep-b");
    const zlink_routing_id_t *source =
      reinterpret_cast<const zlink_routing_id_t *> (1);
    zlink_reply_token_t token = UINT64_MAX;
    size_t count = 29;
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_BUFFER_TOO_SMALL,
      ::zlink_router_recv (router, &source, &token, too_small, 2, &count,
                           ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (ENOBUFS, errno);
    TEST_ASSERT_EQUAL_UINT64 (3, count);
    TEST_ASSERT_EQUAL_PTR (reinterpret_cast<const void *> (1), source);
    TEST_ASSERT_EQUAL_UINT64 (UINT64_MAX, token);
    assert_part (&too_small[0], "keep-a");
    assert_part (&too_small[1], "keep-b");
    zlink_multipart_close (too_small, 2);

    zlink_msg_t received[3];
    memset (received, 0xa5, sizeof (received));
    count = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      ::zlink_router_recv (router, &source, &token, received, 3, &count,
                           ZLINK_RECV_FLAGS_DONTWAIT));
    assert_rid (source, "router-mix");
    TEST_ASSERT_EQUAL_UINT64 (0, token);
    TEST_ASSERT_EQUAL_UINT64 (3, count);
    for (size_t i = 0; i < count; ++i)
        assert_part (&received[i], capacity_payloads[i]);
    zlink_multipart_close (received, count);

    const char *const mixed[] = {"mix-0", "mix-1", "mix-2"};
    send_record (dealer, mixed, 3);
    wait_readable (router);
    assert_router_recv_part (router, mixed[0], "router-mix",
                             ZLINK_PART_MORE);
    zlink_msg_t busy[3];
    init_part (&busy[0], "busy-router");
    count = 31;
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_BUSY,
      ::zlink_router_recv (router, &source, &token, busy, 3, &count,
                           ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EBUSY, errno);
    TEST_ASSERT_EQUAL_UINT64 (31, count);
    assert_part (&busy[0], "busy-router");
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&busy[0]));
    assert_router_recv_part (router, mixed[1], "router-mix",
                             ZLINK_PART_MORE);
    assert_router_recv_part (router, mixed[2], "router-mix",
                             ZLINK_PART_FINAL);

    const char *const staged[] = {"stage-0", "stage-1"};
    send_record (dealer, staged, 2);
    wait_readable (router);
    zlink_msg_t preserve;
    init_part (&preserve, "preserve");
    count = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_BUFFER_TOO_SMALL,
      ::zlink_router_recv (router, &source, &token, &preserve, 1, &count,
                           ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_UINT64 (2, count);
    assert_part (&preserve, "preserve");
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&preserve));
    assert_router_recv_part (router, staged[0], "router-mix",
                             ZLINK_PART_MORE);
    assert_router_recv_part (router, staged[1], "router-mix",
                             ZLINK_PART_FINAL);

    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
}

void test_whole_recv_validates_required_outputs_flags_and_socket_type ()
{
    void *pair = test_context_socket (ZLINK_SOCKET_PAIR);
    void *pub = test_context_socket (ZLINK_SOCKET_PUB);
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    zlink_msg_t part;
    init_part (&part, "untouched");
    size_t count = 17;
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_INVALID_HANDLE,
      ::zlink_recv (pair, NULL, NULL, 1, &count,
                    ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EFAULT, errno);
    TEST_ASSERT_EQUAL_UINT64 (17, count);
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_INVALID_STATE,
      ::zlink_recv (pair, NULL, &part, 1, &count,
                    static_cast<zlink_recv_flags_t> (0x40)));
    TEST_ASSERT_EQUAL_INT (EINVAL, errno);
    TEST_ASSERT_EQUAL_UINT64 (17, count);
    assert_part (&part, "untouched");
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_NOT_SUPPORTED,
      ::zlink_recv (pub, NULL, &part, 1, &count,
                    ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (ENOTSUP, errno);
    TEST_ASSERT_EQUAL_UINT64 (17, count);
    assert_part (&part, "untouched");

    const zlink_routing_id_t *source =
      reinterpret_cast<const zlink_routing_id_t *> (1);
    zlink_reply_token_t token = UINT64_MAX;
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_INVALID_HANDLE,
      ::zlink_router_recv (router, NULL, &token, &part, 1, &count,
                           ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EFAULT, errno);
    TEST_ASSERT_EQUAL_UINT64 (UINT64_MAX, token);
    TEST_ASSERT_EQUAL_UINT64 (17, count);
    assert_part (&part, "untouched");
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_NOT_SUPPORTED,
      ::zlink_router_recv (pair, &source, &token, &part, 1, &count,
                           ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (ENOTSUP, errno);
    TEST_ASSERT_EQUAL_PTR (reinterpret_cast<const void *> (1), source);
    TEST_ASSERT_EQUAL_UINT64 (UINT64_MAX, token);
    TEST_ASSERT_EQUAL_UINT64 (17, count);
    assert_part (&part, "untouched");
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));

    test_context_socket_close_zero_linger (router);
    test_context_socket_close_zero_linger (pub);
    test_context_socket_close_zero_linger (pair);
}

int main ()
{
    setup_test_environment ();
    UNITY_BEGIN ();
    RUN_TEST (test_pair_whole_recv_is_atomic_and_supports_single_part);
    RUN_TEST (test_dealer_whole_recv_capacity_retry_preserves_record);
    RUN_TEST (test_router_whole_recv_returns_data_and_request_metadata);
    RUN_TEST (test_pair_whole_recv_mixes_with_recv_part_without_loss);
    RUN_TEST (test_router_whole_recv_capacity_retry_and_recv_part_mix);
    RUN_TEST (
      test_whole_recv_validates_required_outputs_flags_and_socket_type);
    const int rc = UNITY_END ();
    fflush (NULL);
    std::_Exit (rc);
}
