/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <cstring>
#include <string>
#include <thread>
#include <vector>

SETUP_TEARDOWN_TESTCONTEXT

namespace
{
const int wait_ms = 3000;
const std::string payloads[] = {
  std::string ("head\0bytes", 10), "", std::string (512, 'm'), "four", "tail"};

void init_part (zlink_msg_t *part_, const std::string &payload_)
{
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_msg_init_size (part_, payload_.size ()));
    if (!payload_.empty ())
        memcpy (zlink_msg_data (part_), payload_.data (), payload_.size ());
}

void init_parts (zlink_msg_t *parts_, const std::string *payloads_, size_t count_)
{
    for (size_t i = 0; i < count_; ++i)
        init_part (&parts_[i], payloads_[i]);
}

void assert_parts (zlink_msg_t *parts_, const std::string *payloads_, size_t count_)
{
    for (size_t i = 0; i < count_; ++i) {
        TEST_ASSERT_EQUAL_UINT64 (payloads_[i].size (), zlink_msg_size (&parts_[i]));
        if (!payloads_[i].empty ())
            TEST_ASSERT_EQUAL_MEMORY (payloads_[i].data (),
                                      zlink_msg_data (&parts_[i]), payloads_[i].size ());
    }
}

void assert_consumed (zlink_msg_t *parts_, size_t count_)
{
    for (size_t i = 0; i < count_; ++i) {
        TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&parts_[i]));
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&parts_[i]));
    }
}

void set_option (void *socket_, zlink_option_t option_, int value_)
{
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK, zlink_set_option (socket_, option_, &value_, sizeof (value_)));
}

void set_hwm (void *socket_, zlink_option_t option_, uint64_t value_)
{
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK, zlink_set_option (socket_, option_, &value_, sizeof (value_)));
}

void *make_socket (zlink_socket_type_t type_, const char *rid_)
{
    void *socket = test_context_socket (type_);
    set_option (socket, ZLINK_OPT_RCVTIMEO, wait_ms);
    set_option (socket, ZLINK_OPT_SNDTIMEO, wait_ms);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_set_routing_id (socket, rid_, strlen (rid_)));
    return socket;
}

zlink_routing_id_t make_rid (const char *text_)
{
    zlink_routing_id_t rid = {};
    rid.size = static_cast<uint8_t> (strlen (text_));
    memcpy (rid.data, text_, rid.size);
    return rid;
}

void connect_sockets (void *sender_, void *receiver_, const char *endpoint_)
{
    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK, zlink_bind (receiver_, endpoint_));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (sender_, endpoint_));
}

void wait_event (void *socket_, short event_ = ZLINK_POLLIN)
{
    zlink_pollitem_t item = {socket_, 0, event_, 0};
    zlink_config_result_t error = ZLINK_CONFIG_INTERNAL_ERROR;
    TEST_ASSERT_EQUAL_INT (1, zlink_poll (&item, 1, wait_ms, &error));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, error);
    TEST_ASSERT_BITS_HIGH (event_, item.revents);
}

struct received_route_t
{
    zlink_routing_id_t source;
    zlink_reply_token_t token;
};

received_route_t receive_record (void *socket_, bool routed_,
                                  const std::string *payloads_, size_t expected_count_)
{
    wait_event (socket_);
    zlink_msg_t received[5];
    memset (received, 0xa5, sizeof (received));
    size_t count = 0;
    const zlink_routing_id_t *source = NULL;
    received_route_t route = {};
    const zlink_recv_result_t result =
      routed_ ? ::zlink_router_recv (socket_, &source, &route.token, received,
                                     5, &count, ZLINK_RECV_FLAGS_DONTWAIT)
              : ::zlink_recv (socket_, &source, received, 5, &count,
                              ZLINK_RECV_FLAGS_DONTWAIT);
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, result);
    TEST_ASSERT_EQUAL_UINT64 (expected_count_, count);
    assert_parts (received, payloads_, count);
    if (routed_) {
        TEST_ASSERT_NOT_NULL (source);
        route.source = *source;
    } else
        TEST_ASSERT_NULL (source);
    zlink_multipart_close (received, count);
    return route;
}

void assert_no_record (void *socket_, bool routed_)
{
    zlink_msg_t parts[5];
    size_t count = 99;
    const zlink_routing_id_t *source = NULL;
    zlink_reply_token_t token = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_NO_DATA,
      routed_ ? ::zlink_router_recv (socket_, &source, &token, parts, 5,
                                     &count, ZLINK_RECV_FLAGS_DONTWAIT)
              : ::zlink_recv (socket_, NULL, parts, 5, &count,
                              ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);
    TEST_ASSERT_EQUAL_UINT64 (99, count);
}

zlink_completion_t receive_completion (void *socket_)
{
    zlink_completion_t completion = {};
    completion.struct_size = sizeof (completion);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK, zlink_completion_recv (socket_, &completion,
                                            ZLINK_RECV_FLAGS_NONE));
    return completion;
}

void assert_reply_completion (void *socket_, zlink_completion_id_t id_,
                               void *context_, size_t count_)
{
    zlink_completion_t completion = receive_completion (socket_);
    TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_REQUEST, completion.kind);
    TEST_ASSERT_EQUAL_UINT64 (id_, completion.completion_id);
    TEST_ASSERT_EQUAL_PTR (context_, completion.user_context);
    TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, completion.request_result);
    TEST_ASSERT_EQUAL_UINT64 (count_, completion.reply_part_count);
    assert_parts (completion.reply_parts, payloads, count_);
    zlink_completion_close (&completion);
}

void reply_record (void *router_, const received_route_t &route_, size_t count_)
{
    zlink_msg_t parts[5];
    init_parts (parts, payloads, count_);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK,
                           ::zlink_reply (router_, &route_.source, route_.token,
                                          parts, count_));
    assert_consumed (parts, count_);
}

void prime_router (void *router_, void *dealer_)
{
    zlink_msg_t part;
    init_part (&part, payloads[0]);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK, ::zlink_send (dealer_, &part, 1, ZLINK_SEND_FLAGS_NONE,
                                    NULL, NULL));
    assert_consumed (&part, 1);
    receive_record (router_, true, payloads, 1);
}

void subscribe_event (void *xpub_, bool old_name_, int expected_, const char *topic_)
{
    wait_event (xpub_);
    char topic[64];
    size_t length = 0;
    int subscribed = -1;
    const zlink_routing_id_t *source = NULL;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      old_name_ ? zlink_xpub_recv_part (xpub_, &source, &subscribed, topic,
                                       sizeof (topic), &length,
                                       ZLINK_RECV_FLAGS_DONTWAIT)
                : ::zlink_xpub_recv (xpub_, &source, &subscribed, topic,
                                     sizeof (topic), &length,
                                     ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (expected_, subscribed);
    TEST_ASSERT_EQUAL_UINT64 (strlen (topic_), length);
    TEST_ASSERT_EQUAL_MEMORY (topic_, topic, length);
}
}

void test_whole_send_pair_dealer_multipart_and_single ()
{
    for (int dealer = 0; dealer != 2; ++dealer) {
        void *sender = make_socket (dealer ? ZLINK_SOCKET_DEALER : ZLINK_SOCKET_PAIR,
                                    "sender");
        void *receiver = make_socket (dealer ? ZLINK_SOCKET_ROUTER : ZLINK_SOCKET_PAIR,
                                      "receiver");
        connect_sockets (sender, receiver, "inproc://whole-send-basic");
        for (size_t count = 1; count <= 5; count += 4) {
            zlink_msg_t parts[5];
            init_parts (parts, payloads, count);
            zlink_completion_id_t id = UINT64_MAX;
            TEST_ASSERT_EQUAL_INT (
              ZLINK_SUBMIT_OK, ::zlink_send (sender, parts, count,
                                            ZLINK_SEND_FLAGS_NONE, NULL, &id));
            TEST_ASSERT_EQUAL_UINT64 (0, id);
            assert_consumed (parts, count);
            receive_record (receiver, dealer != 0, payloads, count);
        }
        test_context_socket_close_zero_linger (sender);
        test_context_socket_close_zero_linger (receiver);
    }
}

void test_whole_send_rid_multipart_and_single ()
{
    void *router = make_socket (ZLINK_SOCKET_ROUTER, "sender");
    void *dealer = make_socket (ZLINK_SOCKET_DEALER, "receiver");
    connect_sockets (dealer, router, "inproc://whole-send-rid");
    prime_router (router, dealer);
    const zlink_routing_id_t target = make_rid ("receiver");
    for (size_t count = 1; count <= 5; count += 4) {
        zlink_msg_t parts[5];
        init_parts (parts, payloads, count);
        zlink_completion_id_t id = UINT64_MAX;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK, ::zlink_send_rid (router, &target, parts, count,
                                            ZLINK_SEND_FLAGS_DONTWAIT, NULL, &id));
        TEST_ASSERT_EQUAL_UINT64 (0, id);
        assert_consumed (parts, count);
        receive_record (dealer, false, payloads, count);
    }
    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
}

void test_whole_request_reply_dealer_and_router ()
{
    for (int routed = 0; routed != 2; ++routed) {
        void *sender = make_socket (routed ? ZLINK_SOCKET_ROUTER : ZLINK_SOCKET_DEALER,
                                    "sender");
        void *receiver = make_socket (ZLINK_SOCKET_ROUTER, "receiver");
        connect_sockets (sender, receiver, "inproc://whole-request-reply");
        const zlink_routing_id_t target = make_rid ("receiver");
        for (size_t count = 1; count <= 5; count += 4) {
            zlink_msg_t parts[5];
            init_parts (parts, payloads, count);
            int context = 42;
            zlink_completion_id_t id = 0;
            TEST_ASSERT_EQUAL_INT (
              ZLINK_SUBMIT_OK,
              ::zlink_request (sender, routed ? &target : NULL, parts, count,
                                ZLINK_SEND_FLAGS_NONE, wait_ms, &context, &id));
            TEST_ASSERT_NOT_EQUAL (0, id);
            assert_consumed (parts, count);
            const received_route_t route = receive_record (receiver, true, payloads, count);
            TEST_ASSERT_NOT_EQUAL (0, route.token);
            reply_record (receiver, route, count);
            assert_reply_completion (sender, id, &context, count);
            init_parts (parts, payloads, count);
            TEST_ASSERT_EQUAL_INT (
              ZLINK_SUBMIT_NOT_FOUND,
              ::zlink_reply (receiver, &route.source, route.token, parts, count));
            TEST_ASSERT_EQUAL_INT (ENOENT, errno);
            assert_consumed (parts, count);
        }
        test_context_socket_close_zero_linger (sender);
        test_context_socket_close_zero_linger (receiver);
    }
}

namespace
{
enum family_t { plain_send, routed_send, request_send, reply_send, publish_send };

zlink_submit_result_t submit (family_t family_, void *socket_,
                              const zlink_routing_id_t *rid_, zlink_msg_t *parts_,
                              size_t count_, zlink_send_flags_t flags_,
                              void *context_ = NULL, zlink_completion_id_t *id_ = NULL)
{
    switch (family_) {
        case plain_send:
            return ::zlink_send (socket_, parts_, count_, flags_, context_, id_);
        case routed_send:
            return ::zlink_send_rid (socket_, rid_, parts_, count_, flags_, context_, id_);
        case request_send:
            return ::zlink_request (socket_, rid_, parts_, count_, flags_, wait_ms,
                                    context_, id_);
        case reply_send:
            return ::zlink_reply (socket_, rid_, 1, parts_, count_);
        case publish_send:
            return ::zlink_publish (socket_, "topic", parts_, count_, flags_);
    }
    TEST_FAIL_MESSAGE ("invalid test family");
    return ZLINK_SUBMIT_INVALID_STATE;
}
}

void test_whole_send_validation_consumes_all_slots ()
{
    const zlink_routing_id_t rid = make_rid ("target");
    for (int family = plain_send; family <= publish_send; ++family) {
        void *socket = make_socket (family == publish_send ? ZLINK_SOCKET_XPUB
                                                            : ZLINK_SOCKET_ROUTER,
                                    "sender");
        zlink_msg_t parts[5];
        init_parts (parts, payloads, 5);
        zlink_completion_id_t id = 0;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_INVALID_ARGUMENT,
          submit (static_cast<family_t> (family), socket, &rid, parts, 0,
                   ZLINK_SEND_FLAGS_DONTWAIT, NULL, &id));
        TEST_ASSERT_EQUAL_INT (EINVAL, errno);
        assert_parts (parts, payloads, 5);
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_INVALID_HANDLE,
          submit (static_cast<family_t> (family), NULL, &rid, parts, 5,
                   ZLINK_SEND_FLAGS_DONTWAIT, NULL, &id));
        TEST_ASSERT_EQUAL_INT (EFAULT, errno);
        assert_consumed (parts, 5);
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_INVALID_HANDLE,
          submit (static_cast<family_t> (family), socket, &rid, NULL, 1,
                   ZLINK_SEND_FLAGS_DONTWAIT, NULL, &id));
        TEST_ASSERT_EQUAL_INT (EFAULT, errno);
        if (family == routed_send || family == request_send || family == reply_send) {
            init_parts (parts, payloads, 5);
            TEST_ASSERT_EQUAL_INT (
              ZLINK_SUBMIT_INVALID_HANDLE,
              submit (static_cast<family_t> (family), socket, NULL, parts, 5,
                       ZLINK_SEND_FLAGS_DONTWAIT, NULL, &id));
            TEST_ASSERT_EQUAL_INT (EFAULT, errno);
            assert_consumed (parts, 5);
        }
        if (family == publish_send) {
            init_parts (parts, payloads, 5);
            TEST_ASSERT_EQUAL_INT (
              ZLINK_SUBMIT_INVALID_HANDLE,
              ::zlink_publish (socket, NULL, parts, 5, ZLINK_SEND_FLAGS_NONE));
            TEST_ASSERT_EQUAL_INT (EFAULT, errno);
            assert_consumed (parts, 5);
        }
        test_context_socket_close_zero_linger (socket);
    }
}

void test_whole_send_failed_middle_discards_prefix_and_consumes_suffix ()
{
    void *sender = make_socket (ZLINK_SOCKET_PAIR, "sender");
    void *receiver = make_socket (ZLINK_SOCKET_PAIR, "receiver");
    connect_sockets (sender, receiver, "inproc://whole-send-invalid-middle");
    zlink_msg_t parts[3];
    init_part (&parts[0], "discard-prefix");
    memset (&parts[1], 0, sizeof (parts[1]));
    init_part (&parts[2], "consume-suffix");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_INVALID_HANDLE,
      ::zlink_send (sender, parts, 3, ZLINK_SEND_FLAGS_DONTWAIT, NULL, NULL));
    TEST_ASSERT_EQUAL_INT (EFAULT, errno);
    assert_consumed (&parts[0], 1);
    assert_consumed (&parts[2], 1);
    assert_no_record (receiver, false);
    init_parts (parts, payloads, 3);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      ::zlink_send (sender, parts, 3, ZLINK_SEND_FLAGS_NONE, NULL, NULL));
    assert_consumed (parts, 3);
    receive_record (receiver, false, payloads, 3);
    test_context_socket_close_zero_linger (sender);
    test_context_socket_close_zero_linger (receiver);
}

void test_whole_send_backpressure_retries_entire_record ()
{
    for (int family = plain_send; family <= request_send; ++family) {
        const bool routed = family == routed_send;
        const bool request = family == request_send;
        void *sender = make_socket (routed ? ZLINK_SOCKET_ROUTER
                                          : request ? ZLINK_SOCKET_DEALER : ZLINK_SOCKET_PAIR,
                                    "sender");
        void *receiver = make_socket (routed ? ZLINK_SOCKET_DEALER
                                            : request ? ZLINK_SOCKET_ROUTER : ZLINK_SOCKET_PAIR,
                                      "receiver");
        set_hwm (sender, ZLINK_OPT_SNDHWM, 1024);
        set_hwm (receiver, ZLINK_OPT_RCVHWM, 1024);
        connect_sockets (sender, receiver, "inproc://whole-send-backpressure");
        if (routed)
            prime_router (sender, receiver);
        const zlink_routing_id_t target = make_rid ("receiver");
        std::vector<zlink_completion_id_t> admitted_ids;
        zlink_completion_id_t wait_id = 0;
        int context = 19;
        size_t accepted = 0;
        for (; accepted < 256; ++accepted) {
            const std::string record[] = {
              std::to_string (accepted), std::string (128, 'a'), std::string (128, 'z')};
            zlink_msg_t parts[3];
            init_parts (parts, record, 3);
            zlink_completion_id_t id = 0;
            const zlink_submit_result_t result =
              submit (static_cast<family_t> (family), sender, routed ? &target : NULL,
                       parts, 3, ZLINK_SEND_FLAGS_DONTWAIT, &context, &id);
            const int error = errno;
            assert_consumed (parts, 3);
            if (result == ZLINK_SUBMIT_BACKPRESSURED) {
                TEST_ASSERT_EQUAL_INT (EAGAIN, error);
                TEST_ASSERT_NOT_EQUAL (0, id);
                wait_id = id;
                break;
            }
            TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, result);
            admitted_ids.push_back (id);
        }
        TEST_ASSERT_TRUE (accepted > 0 && accepted < 256);
        std::vector<received_route_t> routes;
        for (size_t i = 0; i < accepted; ++i) {
            const std::string record[] = {
              std::to_string (i), std::string (128, 'a'), std::string (128, 'z')};
            routes.push_back (receive_record (receiver, request, record, 3));
        }
        assert_no_record (receiver, request);
        zlink_completion_t writable = receive_completion (sender);
        TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_WRITABLE, writable.kind);
        TEST_ASSERT_EQUAL_UINT64 (wait_id, writable.completion_id);
        TEST_ASSERT_EQUAL_PTR (&context, writable.user_context);
        TEST_ASSERT_EQUAL_INT (ZLINK_SEND_ADMITTED, writable.send_result);
        zlink_completion_close (&writable);

        const std::string retry[] = {
          std::to_string (accepted), std::string (128, 'a'), std::string (128, 'z')};
        zlink_msg_t parts[3];
        init_parts (parts, retry, 3);
        zlink_completion_id_t retry_id = 0;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK,
          submit (static_cast<family_t> (family), sender, routed ? &target : NULL,
                   parts, 3, ZLINK_SEND_FLAGS_DONTWAIT, &context, &retry_id));
        assert_consumed (parts, 3);
        routes.push_back (receive_record (receiver, request, retry, 3));
        assert_no_record (receiver, request);
        if (request) {
            admitted_ids.push_back (retry_id);
            for (size_t i = 0; i < routes.size (); ++i) {
                reply_record (receiver, routes[i], 1);
                assert_reply_completion (sender, admitted_ids[i], &context, 1);
            }
        }
        test_context_socket_close_zero_linger (sender);
        test_context_socket_close_zero_linger (receiver);
    }
}

void test_whole_publish_subscribe_capacity_retry_and_array_reuse ()
{
    for (int xsub = 0; xsub != 2; ++xsub) {
        void *pub = make_socket (ZLINK_SOCKET_XPUB, "pub");
        void *sub = make_socket (xsub ? ZLINK_SOCKET_XSUB : ZLINK_SOCKET_SUB, "sub");
        connect_sockets (sub, pub, "inproc://whole-publish-subscribe");
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_set_subscription (sub, "topic"));
        subscribe_event (pub, false, 1, "topic");
        zlink_msg_t output[5];
        for (size_t count = 1; count <= 5; count += 4) {
            zlink_msg_t parts[5];
            init_parts (parts, payloads, count);
            TEST_ASSERT_EQUAL_INT (
              ZLINK_SUBMIT_OK, ::zlink_publish (pub, "topic-data", parts, count,
                                               ZLINK_SEND_FLAGS_NONE));
            assert_consumed (parts, count);
            wait_event (sub);
            memset (output, 0xa5, sizeof (output));
            char topic[32];
            memset (topic, 'k', sizeof (topic));
            const zlink_routing_id_t *source =
              reinterpret_cast<const zlink_routing_id_t *> (1);
            size_t topic_length = 99;
            size_t part_count = 99;
            TEST_ASSERT_EQUAL_INT (
              ZLINK_RECV_BUFFER_TOO_SMALL,
              ::zlink_subscribe (sub, &source, topic, sizeof (topic), &topic_length,
                                  output, count - 1, &part_count,
                                  ZLINK_RECV_FLAGS_DONTWAIT));
            TEST_ASSERT_EQUAL_INT (ENOBUFS, errno);
            TEST_ASSERT_EQUAL_UINT64 (count, part_count);
            TEST_ASSERT_EQUAL_UINT64 (10, topic_length);
            TEST_ASSERT_EACH_EQUAL_UINT8 (0xa5, output, sizeof (output));
            TEST_ASSERT_EACH_EQUAL_UINT8 ('k', topic, sizeof (topic));
            TEST_ASSERT_EQUAL_PTR (reinterpret_cast<const void *> (1), source);
            TEST_ASSERT_EQUAL_INT (
              ZLINK_RECV_BUFFER_TOO_SMALL,
              ::zlink_subscribe (sub, &source, topic, 1, &topic_length, output,
                                  5, &part_count, ZLINK_RECV_FLAGS_DONTWAIT));
            TEST_ASSERT_EQUAL_INT (ENOBUFS, errno);
            TEST_ASSERT_EQUAL_UINT64 (10, topic_length);
            TEST_ASSERT_EACH_EQUAL_UINT8 (0xa5, output, sizeof (output));
            TEST_ASSERT_EACH_EQUAL_UINT8 ('k', topic, sizeof (topic));
            TEST_ASSERT_EQUAL_INT (
              ZLINK_RECV_OK,
              ::zlink_subscribe (sub, &source, topic, sizeof (topic), &topic_length,
                                  output, 5, &part_count, ZLINK_RECV_FLAGS_DONTWAIT));
            TEST_ASSERT_NULL (source);
            TEST_ASSERT_EQUAL_MEMORY ("topic-data", topic, 10);
            TEST_ASSERT_EQUAL_UINT8 ('k', topic[10]);
            TEST_ASSERT_EQUAL_UINT64 (count, part_count);
            assert_parts (output, payloads, count);
            zlink_multipart_close (output, count);
        }
        // Exercise the terminal payload path directly into uninitialized
        // slots as well as the buffered capacity-retry path above.
        zlink_msg_t single;
        init_part (&single, payloads[0]);
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK, ::zlink_publish (pub, "topic-data", &single, 1,
                                           ZLINK_SEND_FLAGS_NONE));
        assert_consumed (&single, 1);
        wait_event (sub);
        memset (output, 0xa5, sizeof (output));
        char topic[32];
        size_t topic_length = 0;
        size_t part_count = 0;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_RECV_OK,
          ::zlink_subscribe (sub, NULL, topic, sizeof (topic), &topic_length,
                              output, 5, &part_count, ZLINK_RECV_FLAGS_DONTWAIT));
        TEST_ASSERT_EQUAL_UINT64 (1, part_count);
        TEST_ASSERT_EQUAL_UINT64 (10, topic_length);
        TEST_ASSERT_EQUAL_MEMORY ("topic-data", topic, topic_length);
        assert_parts (output, payloads, 1);
        zlink_multipart_close (output, part_count);
        test_context_socket_close_zero_linger (sub);
        test_context_socket_close_zero_linger (pub);
    }
}

void test_whole_subscribe_validation_and_part_interleave ()
{
    void *pub = make_socket (ZLINK_SOCKET_XPUB, "pub");
    void *sub = make_socket (ZLINK_SOCKET_SUB, "sub");
    connect_sockets (sub, pub, "inproc://whole-subscribe-interleave");
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_set_subscription (sub, "topic"));
    subscribe_event (pub, false, 1, "topic");
    zlink_msg_t parts[5];
    init_parts (parts, payloads, 5);
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK,
                           ::zlink_publish (pub, "topic", parts, 5,
                                            ZLINK_SEND_FLAGS_NONE));
    assert_consumed (parts, 5);
    wait_event (sub);
    char topic[32];
    size_t topic_length = 99;
    size_t count = 99;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_INVALID_HANDLE,
      ::zlink_subscribe (sub, NULL, topic, sizeof (topic), &topic_length,
                          parts, 5, NULL, ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EFAULT, errno);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_INVALID_HANDLE,
      ::zlink_subscribe (sub, NULL, NULL, 1, &topic_length, parts, 5, &count,
                          ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EFAULT, errno);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_INVALID_STATE,
      ::zlink_subscribe (sub, NULL, topic, sizeof (topic), &topic_length, parts,
                          5, &count, static_cast<zlink_recv_flags_t> (0x40)));
    TEST_ASSERT_EQUAL_INT (EINVAL, errno);
    TEST_ASSERT_EQUAL_UINT64 (99, topic_length);
    TEST_ASSERT_EQUAL_UINT64 (99, count);
    zlink_msg_t part;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&part));
    for (size_t i = 0; i < 5; ++i) {
        zlink_part_flag_t more = ZLINK_PART_FINAL;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_RECV_OK,
          zlink_subscribe_part (sub, NULL, topic, sizeof (topic), &topic_length,
                                &part, &more, ZLINK_RECV_FLAGS_DONTWAIT));
        assert_parts (&part, payloads + i, 1);
        if (i == 0) {
            TEST_ASSERT_EQUAL_INT (
              ZLINK_RECV_BUSY,
              ::zlink_subscribe (sub, NULL, topic, sizeof (topic), &topic_length,
                                  parts, 5, &count, ZLINK_RECV_FLAGS_DONTWAIT));
            TEST_ASSERT_EQUAL_INT (EBUSY, errno);
        }
    }
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&part));
    test_context_socket_close_zero_linger (sub);
    test_context_socket_close_zero_linger (pub);
}

void test_whole_xpub_recv_alias_and_capacity_retry ()
{
    void *pub = make_socket (ZLINK_SOCKET_XPUB, "pub");
    void *sub = make_socket (ZLINK_SOCKET_XSUB, "sub");
    connect_sockets (sub, pub, "inproc://whole-xpub-recv");
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_set_subscription (sub, "topic-long"));
    wait_event (pub);
    char topic[32];
    memset (topic, 'k', sizeof (topic));
    size_t length = 99;
    int subscribed = -1;
    const zlink_routing_id_t *source =
      reinterpret_cast<const zlink_routing_id_t *> (1);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_BUFFER_TOO_SMALL,
      ::zlink_xpub_recv (pub, &source, &subscribed, topic, 1, &length,
                         ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (ENOBUFS, errno);
    TEST_ASSERT_EQUAL_UINT64 (10, length);
    TEST_ASSERT_EQUAL_INT (-1, subscribed);
    TEST_ASSERT_EQUAL_PTR (reinterpret_cast<const void *> (1), source);
    TEST_ASSERT_EACH_EQUAL_UINT8 ('k', topic, sizeof (topic));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_xpub_recv_part (pub, &source, &subscribed, topic, sizeof (topic),
                            &length, ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (1, subscribed);
    TEST_ASSERT_NOT_NULL (source);
    TEST_ASSERT_EQUAL_UINT64 (3, source->size);
    TEST_ASSERT_EQUAL_MEMORY ("sub", source->data, 3);
    TEST_ASSERT_EQUAL_MEMORY ("topic-long", topic, 10);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_unset_subscription (sub, "topic-long"));
    subscribe_event (pub, false, 0, "topic-long");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_NO_DATA,
      ::zlink_xpub_recv (pub, NULL, &subscribed, topic, sizeof (topic), &length,
                         ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);
    test_context_socket_close_zero_linger (sub);
    test_context_socket_close_zero_linger (pub);
}

void test_whole_send_preserves_existing_part_sequence ()
{
    void *sender = make_socket (ZLINK_SOCKET_PAIR, "sender");
    void *receiver = make_socket (ZLINK_SOCKET_PAIR, "receiver");
    connect_sockets (sender, receiver, "inproc://whole-send-existing-prefix");
    zlink_msg_t part;
    init_part (&part, payloads[0]);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK, zlink_send_part (sender, &part, ZLINK_SEND_FLAGS_NONE,
                                       ZLINK_PART_MORE, NULL, NULL));
    assert_consumed (&part, 1);
    zlink_msg_t parts[3];
    init_parts (parts, payloads, 3);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_INVALID_STATE,
      ::zlink_send (sender, parts, 3, ZLINK_SEND_FLAGS_NONE, NULL, NULL));
    TEST_ASSERT_EQUAL_INT (EBUSY, errno);
    assert_consumed (parts, 3);
    init_part (&part, payloads[1]);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK, zlink_send_part (sender, &part, ZLINK_SEND_FLAGS_NONE,
                                       ZLINK_PART_FINAL, NULL, NULL));
    assert_consumed (&part, 1);
    receive_record (receiver, false, payloads, 2);
    assert_no_record (receiver, false);
    test_context_socket_close_zero_linger (sender);
    test_context_socket_close_zero_linger (receiver);
}

void test_whole_send_concurrent_callers_keep_record_boundaries ()
{
    void *sender = make_socket (ZLINK_SOCKET_PAIR, "sender");
    void *receiver = make_socket (ZLINK_SOCKET_PAIR, "receiver");
    connect_sockets (sender, receiver, "inproc://whole-send-concurrent");
    bool success[2] = {true, true};
    std::thread writers[2];
    for (int writer = 0; writer < 2; ++writer) {
        writers[writer] = std::thread ([&, writer] {
            for (int record = 0; record < 16; ++record) {
                const unsigned char bytes[] = {
                  static_cast<unsigned char> (writer),
                  static_cast<unsigned char> (record)};
                zlink_msg_t parts[3];
                for (size_t i = 0; i < 3; ++i) {
                    if (zlink_msg_init_size (&parts[i], sizeof (bytes)) != ZLINK_CONFIG_OK) {
                        success[writer] = false;
                        return;
                    }
                    memcpy (zlink_msg_data (&parts[i]), bytes, sizeof (bytes));
                }
                if (::zlink_send (sender, parts, 3, ZLINK_SEND_FLAGS_NONE, NULL, NULL)
                    != ZLINK_SUBMIT_OK)
                    success[writer] = false;
                for (size_t i = 0; i < 3; ++i) {
                    if (zlink_msg_size (&parts[i]) != 0)
                        success[writer] = false;
                    zlink_msg_close (&parts[i]);
                }
            }
        });
    }
    for (int i = 0; i < 2; ++i) {
        writers[i].join ();
        TEST_ASSERT_TRUE (success[i]);
    }
    bool seen[2][16] = {};
    for (int record = 0; record < 32; ++record) {
        wait_event (receiver);
        zlink_msg_t parts[3];
        memset (parts, 0xa5, sizeof (parts));
        size_t count = 0;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_RECV_OK, ::zlink_recv (receiver, NULL, parts, 3, &count,
                                      ZLINK_RECV_FLAGS_DONTWAIT));
        TEST_ASSERT_EQUAL_UINT64 (3, count);
        TEST_ASSERT_EQUAL_UINT64 (2, zlink_msg_size (&parts[0]));
        const unsigned char *bytes =
          static_cast<const unsigned char *> (zlink_msg_data (&parts[0]));
        TEST_ASSERT_TRUE (bytes[0] < 2 && bytes[1] < 16);
        TEST_ASSERT_FALSE (seen[bytes[0]][bytes[1]]);
        seen[bytes[0]][bytes[1]] = true;
        for (size_t i = 1; i < count; ++i) {
            TEST_ASSERT_EQUAL_UINT64 (2, zlink_msg_size (&parts[i]));
            TEST_ASSERT_EQUAL_MEMORY (bytes, zlink_msg_data (&parts[i]), 2);
        }
        zlink_multipart_close (parts, count);
    }
    assert_no_record (receiver, false);
    test_context_socket_close_zero_linger (sender);
    test_context_socket_close_zero_linger (receiver);
}

void test_whole_reply_backpressure_preserves_token_for_record_retry ()
{
    void *router = make_socket (ZLINK_SOCKET_ROUTER, "router");
    void *dealer = make_socket (ZLINK_SOCKET_DEALER, "dealer");
    set_hwm (router, ZLINK_OPT_SNDHWM, 1024);
    set_hwm (dealer, ZLINK_OPT_RCVHWM, 1024);
    set_option (router, ZLINK_OPT_SNDTIMEO, 0);
    connect_sockets (dealer, router, "inproc://whole-reply-backpressure");
    zlink_msg_t request;
    init_part (&request, payloads[0]);
    zlink_completion_id_t request_id = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      ::zlink_request (dealer, NULL, &request, 1, ZLINK_SEND_FLAGS_NONE,
                        wait_ms, NULL, &request_id));
    assert_consumed (&request, 1);
    const received_route_t route = receive_record (router, true, payloads, 1);
    const std::string filler[] = {std::string (256, 'f')};
    size_t accepted = 0;
    for (; accepted < 256; ++accepted) {
        zlink_msg_t part;
        init_part (&part, filler[0]);
        const zlink_submit_result_t result =
          ::zlink_send_rid (router, &route.source, &part, 1,
                            ZLINK_SEND_FLAGS_DONTWAIT, NULL, NULL);
        assert_consumed (&part, 1);
        if (result == ZLINK_SUBMIT_BACKPRESSURED)
            break;
        TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, result);
    }
    TEST_ASSERT_TRUE (accepted > 0 && accepted < 256);
    zlink_msg_t parts[5];
    init_parts (parts, payloads, 5);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_BACKPRESSURED,
      ::zlink_reply (router, &route.source, route.token, parts, 5));
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);
    assert_consumed (parts, 5);
    for (size_t i = 0; i < accepted; ++i)
        receive_record (dealer, false, filler, 1);
    assert_no_record (dealer, false);
    zlink_completion_t writable = receive_completion (router);
    TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_WRITABLE, writable.kind);
    zlink_completion_close (&writable);
    reply_record (router, route, 5);
    assert_reply_completion (dealer, request_id, NULL, 5);
    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
}

void test_whole_publish_backpressure_retries_entire_record ()
{
    void *pub = make_socket (ZLINK_SOCKET_XPUB, "pub");
    void *sub = make_socket (ZLINK_SOCKET_SUB, "sub");
    set_hwm (pub, ZLINK_OPT_SNDHWM, 1024);
    set_hwm (sub, ZLINK_OPT_RCVHWM, 1024);
    const int nodrop = 1;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK, zlink_set_pub_option (pub, ZLINK_PUB_OPT_NODROP,
                                             &nodrop, sizeof (nodrop)));
    connect_sockets (sub, pub, "inproc://whole-publish-backpressure");
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_set_subscription (sub, "topic"));
    subscribe_event (pub, false, 1, "topic");
    const std::string record[] = {std::string (128, 'a'), std::string (128, 'z')};
    size_t accepted = 0;
    for (; accepted < 256; ++accepted) {
        zlink_msg_t parts[2];
        init_parts (parts, record, 2);
        const zlink_submit_result_t result =
          ::zlink_publish (pub, "topic", parts, 2, ZLINK_SEND_FLAGS_DONTWAIT);
        const int error = errno;
        assert_consumed (parts, 2);
        if (result == ZLINK_SUBMIT_BACKPRESSURED) {
            TEST_ASSERT_EQUAL_INT (EAGAIN, error);
            break;
        }
        TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, result);
    }
    TEST_ASSERT_TRUE (accepted > 0 && accepted < 256);
    for (size_t i = 0; i < accepted; ++i) {
        wait_event (sub);
        zlink_msg_t parts[2];
        char topic[16];
        size_t length = 0;
        size_t count = 0;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_RECV_OK,
          ::zlink_subscribe (sub, NULL, topic, sizeof (topic), &length,
                              parts, 2, &count, ZLINK_RECV_FLAGS_DONTWAIT));
        TEST_ASSERT_EQUAL_UINT64 (2, count);
        TEST_ASSERT_EQUAL_UINT64 (5, length);
        TEST_ASSERT_EQUAL_MEMORY ("topic", topic, length);
        assert_parts (parts, record, count);
        zlink_multipart_close (parts, count);
    }
    zlink_msg_t parts[2];
    char topic[16];
    size_t length = 0;
    size_t count = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_NO_DATA,
      ::zlink_subscribe (sub, NULL, topic, sizeof (topic), &length,
                          parts, 2, &count, ZLINK_RECV_FLAGS_DONTWAIT));
    init_parts (parts, record, 2);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      ::zlink_publish (pub, "topic", parts, 2, ZLINK_SEND_FLAGS_DONTWAIT));
    assert_consumed (parts, 2);
    wait_event (sub);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      ::zlink_subscribe (sub, NULL, topic, sizeof (topic), &length,
                          parts, 2, &count, ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_UINT64 (2, count);
    assert_parts (parts, record, count);
    zlink_multipart_close (parts, count);
    test_context_socket_close_zero_linger (sub);
    test_context_socket_close_zero_linger (pub);
}

int main ()
{
    setup_test_environment ();
    UNITY_BEGIN ();
    RUN_TEST (test_whole_send_pair_dealer_multipart_and_single);
    RUN_TEST (test_whole_send_rid_multipart_and_single);
    RUN_TEST (test_whole_request_reply_dealer_and_router);
    RUN_TEST (test_whole_send_validation_consumes_all_slots);
    RUN_TEST (test_whole_send_failed_middle_discards_prefix_and_consumes_suffix);
    RUN_TEST (test_whole_send_backpressure_retries_entire_record);
    RUN_TEST (test_whole_publish_subscribe_capacity_retry_and_array_reuse);
    RUN_TEST (test_whole_subscribe_validation_and_part_interleave);
    RUN_TEST (test_whole_xpub_recv_alias_and_capacity_retry);
    RUN_TEST (test_whole_send_preserves_existing_part_sequence);
    RUN_TEST (test_whole_send_concurrent_callers_keep_record_boundaries);
    RUN_TEST (test_whole_reply_backpressure_preserves_token_for_record_retry);
    RUN_TEST (test_whole_publish_backpressure_retries_entire_record);
    return UNITY_END ();
}
