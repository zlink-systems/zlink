/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <string.h>
#include <thread>
#include <vector>

namespace
{
struct send_start_gate_t
{
    send_start_gate_t () : ready (0), go (false) {}

    std::mutex mutex;
    std::condition_variable cv;
    std::atomic<int> ready;
    bool go;
};

struct sender_probe_t
{
    sender_probe_t () :
        gate (NULL), socket (NULL), tag (0), count (0), failed (false), send_errno (0)
    {
    }

    send_start_gate_t *gate;
    void *socket;
    unsigned char tag;
    int count;
    std::atomic<bool> failed;
    std::atomic<int> send_errno;
};

void wait_and_send_messages (sender_probe_t *probe_)
{
    {
        std::unique_lock<std::mutex> lock (probe_->gate->mutex);
        probe_->gate->ready.fetch_add (1, std::memory_order_acq_rel);
        probe_->gate->cv.notify_all ();
        probe_->gate->cv.wait (lock, [&] () { return probe_->gate->go; });
    }

    for (int i = 0; i < probe_->count; ++i) {
        zlink_msg_t msg;
        const size_t payload_size = 2;
        if (zlink_msg_init_size (&msg, payload_size) != 0) {
            probe_->failed.store (true, std::memory_order_release);
            probe_->send_errno.store (errno, std::memory_order_release);
            return;
        }
        unsigned char *data = static_cast<unsigned char *> (zlink_msg_data (&msg));
        data[0] = probe_->tag;
        data[1] = static_cast<unsigned char> (i);
        if (zlink_send (probe_->socket, &msg, 1, 0) != 0) {
            probe_->failed.store (true, std::memory_order_release);
            probe_->send_errno.store (errno, std::memory_order_release);
            return;
        }
    }
}

void prime_router_recv_plane (void *router_)
{
    const zlink_routing_id_t *source_rid = NULL;
    uint64_t request_seq = 0;
    zlink_msg_t *parts = NULL;
    size_t part_count = 0;
    const zlink_recv_result_t rc = zlink_router_recv (
      router_, &source_rid, &request_seq, &parts, &part_count, ZLINK_DONTWAIT);
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA, rc);
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
}

void recv_router_until_message (void *router_,
                                const zlink_routing_id_t **source_rid_out_,
                                uint64_t *request_seq_out_,
                                zlink_msg_t **parts_out_,
                                size_t *part_count_out_)
{
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds (5);

    while (std::chrono::steady_clock::now () < deadline) {
        const zlink_recv_result_t rc =
          zlink_router_recv (router_, source_rid_out_, request_seq_out_,
                             parts_out_, part_count_out_, ZLINK_DONTWAIT);
        if (rc == ZLINK_RECV_OK) {
            return;
        }
        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA, rc);
        TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
        msleep (10);
    }

    TEST_FAIL_MESSAGE ("router recv timed out");
}

}

SETUP_TEARDOWN_TESTCONTEXT

void test_public_socket_timeout_defaults_and_override ()
{
    void *socket = test_context_socket (ZLINK_SOCKET_PAIR);
    TEST_ASSERT_NOT_NULL (socket);

    int timeout = 0;
    size_t timeout_size = sizeof (timeout);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_option (socket, ZLINK_OPT_SNDTIMEO, &timeout, &timeout_size));
    TEST_ASSERT_EQUAL_INT (static_cast<int> (sizeof (timeout)), static_cast<int> (timeout_size));
    TEST_ASSERT_EQUAL_INT (1000, timeout);

    timeout = 0;
    timeout_size = sizeof (timeout);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_option (socket, ZLINK_OPT_RCVTIMEO, &timeout, &timeout_size));
    TEST_ASSERT_EQUAL_INT (static_cast<int> (sizeof (timeout)), static_cast<int> (timeout_size));
    TEST_ASSERT_EQUAL_INT (1000, timeout);

    const int override_timeout = 77;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (socket, ZLINK_OPT_SNDTIMEO, &override_timeout, sizeof (override_timeout)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (socket, ZLINK_OPT_RCVTIMEO, &override_timeout, sizeof (override_timeout)));

    timeout = 0;
    timeout_size = sizeof (timeout);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_option (socket, ZLINK_OPT_SNDTIMEO, &timeout, &timeout_size));
    TEST_ASSERT_EQUAL_INT (override_timeout, timeout);

    timeout = 0;
    timeout_size = sizeof (timeout);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_option (socket, ZLINK_OPT_RCVTIMEO, &timeout, &timeout_size));
    TEST_ASSERT_EQUAL_INT (override_timeout, timeout);
}

void test_public_inproc_pair_send_single_part ()
{
    void *left = test_context_socket (ZLINK_SOCKET_PAIR);
    void *right = test_context_socket (ZLINK_SOCKET_PAIR);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (left, "inproc://public_inproc_pair_send_single_part"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (right, "inproc://public_inproc_pair_send_single_part"));

    int sndtimeo = 0;
    size_t sndtimeo_size = sizeof (sndtimeo);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_option (right, ZLINK_OPT_SNDTIMEO, &sndtimeo, &sndtimeo_size));
    TEST_ASSERT_EQUAL_INT (static_cast<int> (sizeof (sndtimeo)), static_cast<int> (sndtimeo_size));

    zlink_msg_t part;
    const char payload[] = "ping";
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&part, sizeof (payload) - 1));
    memcpy (zlink_msg_data (&part), payload, sizeof (payload) - 1);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_send (right, &part, 1, 0));

    zlink_msg_t *parts = NULL;
    size_t part_count = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_recv (left, NULL, &parts, &part_count, 0));
    TEST_ASSERT_EQUAL_UINT64 (1, part_count);
    TEST_ASSERT_EQUAL_UINT64 (sizeof (payload) - 1, zlink_msg_size (&parts[0]));
    TEST_ASSERT_EQUAL_MEMORY (payload, zlink_msg_data (&parts[0]), sizeof (payload) - 1);
    zlink_multipart_close (parts, part_count);
}

void test_public_inproc_pair_send_multipart_blocking ()
{
    void *left = test_context_socket (ZLINK_SOCKET_PAIR);
    void *right = test_context_socket (ZLINK_SOCKET_PAIR);

    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (left, "inproc://public_inproc_pair_send_multipart_blocking"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (right, "inproc://public_inproc_pair_send_multipart_blocking"));

    zlink_msg_t parts[2];
    const char header[] = "head";
    const char body[] = "body";
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&parts[0], sizeof (header) - 1));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&parts[1], sizeof (body) - 1));
    memcpy (zlink_msg_data (&parts[0]), header, sizeof (header) - 1);
    memcpy (zlink_msg_data (&parts[1]), body, sizeof (body) - 1);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_send (right, parts, 2, 0));

    zlink_msg_t *received = NULL;
    size_t part_count = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_recv (left, NULL, &received, &part_count, 0));
    TEST_ASSERT_EQUAL_UINT64 (2, part_count);
    TEST_ASSERT_EQUAL_UINT64 (sizeof (header) - 1, zlink_msg_size (&received[0]));
    TEST_ASSERT_EQUAL_MEMORY (header, zlink_msg_data (&received[0]), sizeof (header) - 1);
    TEST_ASSERT_EQUAL_UINT64 (sizeof (body) - 1, zlink_msg_size (&received[1]));
    TEST_ASSERT_EQUAL_MEMORY (body, zlink_msg_data (&received[1]), sizeof (body) - 1);
    zlink_multipart_close (received, part_count);
}

void test_public_inproc_pair_recv_single_after_multipart_reset ()
{
    void *left = test_context_socket (ZLINK_SOCKET_PAIR);
    void *right = test_context_socket (ZLINK_SOCKET_PAIR);

    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (left, "inproc://public_inproc_pair_recv_single_after_multipart"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (right, "inproc://public_inproc_pair_recv_single_after_multipart"));

    zlink_msg_t multipart[2];
    const char head[] = "head";
    const char body[] = "body";
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&multipart[0], sizeof (head) - 1));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&multipart[1], sizeof (body) - 1));
    memcpy (zlink_msg_data (&multipart[0]), head, sizeof (head) - 1);
    memcpy (zlink_msg_data (&multipart[1]), body, sizeof (body) - 1);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_send (right, multipart, 2, 0));

    zlink_msg_t *received = NULL;
    size_t part_count = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_recv (left, NULL, &received, &part_count, 0));
    TEST_ASSERT_EQUAL_UINT64 (2, part_count);
    zlink_multipart_close (received, part_count);

    zlink_msg_t single;
    const char payload[] = "ping";
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&single, sizeof (payload) - 1));
    memcpy (zlink_msg_data (&single), payload, sizeof (payload) - 1);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_send (right, &single, 1, 0));

    received = NULL;
    part_count = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_recv (left, NULL, &received, &part_count, 0));
    TEST_ASSERT_EQUAL_UINT64 (1, part_count);
    TEST_ASSERT_EQUAL_UINT64 (sizeof (payload) - 1, zlink_msg_size (&received[0]));
    TEST_ASSERT_EQUAL_MEMORY (payload, zlink_msg_data (&received[0]), sizeof (payload) - 1);
    zlink_multipart_close (received, part_count);
}

void test_public_inproc_dealer_send_single_part ()
{
    void *left = test_context_socket (ZLINK_SOCKET_DEALER);
    void *right = test_context_socket (ZLINK_SOCKET_DEALER);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (left, "inproc://public_inproc_dealer_send_single_part"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (right, "inproc://public_inproc_dealer_send_single_part"));

    zlink_msg_t part;
    const char payload[] = "ping";
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&part, sizeof (payload) - 1));
    memcpy (zlink_msg_data (&part), payload, sizeof (payload) - 1);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_send (right, &part, 1, 0));

    zlink_msg_t *parts = NULL;
    size_t part_count = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_recv (left, NULL, &parts, &part_count, 0));
    TEST_ASSERT_EQUAL_UINT64 (1, part_count);
    TEST_ASSERT_EQUAL_UINT64 (sizeof (payload) - 1, zlink_msg_size (&parts[0]));
    TEST_ASSERT_EQUAL_MEMORY (payload, zlink_msg_data (&parts[0]), sizeof (payload) - 1);
    zlink_multipart_close (parts, part_count);
}

void test_public_inproc_dealer_send_multipart_blocking ()
{
    void *left = test_context_socket (ZLINK_SOCKET_DEALER);
    void *right = test_context_socket (ZLINK_SOCKET_DEALER);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (left, "inproc://public_inproc_dealer_send_multipart"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (right, "inproc://public_inproc_dealer_send_multipart"));

    zlink_msg_t parts[2];
    const char header[] = "head";
    const char body[] = "body";
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&parts[0], sizeof (header) - 1));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&parts[1], sizeof (body) - 1));
    memcpy (zlink_msg_data (&parts[0]), header, sizeof (header) - 1);
    memcpy (zlink_msg_data (&parts[1]), body, sizeof (body) - 1);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_send (right, parts, 2, 0));

    zlink_msg_t *received = NULL;
    size_t part_count = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_recv (left, NULL, &received, &part_count, 0));
    TEST_ASSERT_EQUAL_UINT64 (2, part_count);
    TEST_ASSERT_EQUAL_UINT64 (sizeof (header) - 1, zlink_msg_size (&received[0]));
    TEST_ASSERT_EQUAL_MEMORY (header, zlink_msg_data (&received[0]), sizeof (header) - 1);
    TEST_ASSERT_EQUAL_UINT64 (sizeof (body) - 1, zlink_msg_size (&received[1]));
    TEST_ASSERT_EQUAL_MEMORY (body, zlink_msg_data (&received[1]), sizeof (body) - 1);
    zlink_multipart_close (received, part_count);
}

void test_dealer_multipart_size_failure_rolls_back_and_preserves_errno ()
{
    void *receiver = test_context_socket (ZLINK_SOCKET_DEALER);
    void *sender = test_context_socket (ZLINK_SOCKET_DEALER);
    const int64_t max_message_size = 5;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (receiver, ZLINK_OPT_MAXMSGSIZE, &max_message_size,
                        sizeof (max_message_size)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (receiver, "inproc://dealer-multipart-size-rollback"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (sender, "inproc://dealer-multipart-size-rollback"));
    msleep (SETTLE_TIME);

    zlink_msg_t first;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&first, 3));
    memcpy (zlink_msg_data (&first), "abc", 3);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (sender, &first, ZLINK_SEND_FLAGS_NONE,
                       ZLINK_PART_MORE, NULL, NULL));

    zlink_msg_t final_part;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&final_part, 3));
    memcpy (zlink_msg_data (&final_part), "def", 3);
    errno = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_INVALID_ARGUMENT,
      zlink_send_part (sender, &final_part, ZLINK_SEND_FLAGS_NONE,
                       ZLINK_PART_FINAL, NULL, NULL));
    TEST_ASSERT_EQUAL_INT (EMSGSIZE, errno);
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&final_part));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&final_part));

    zlink_msg_t *received = NULL;
    size_t part_count = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_NO_DATA,
      zlink_recv (receiver, NULL, &received, &part_count,
                  ZLINK_RECV_FLAGS_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);

    zlink_msg_t fresh;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&fresh, 1));
    *static_cast<char *> (zlink_msg_data (&fresh)) = 'z';
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (sender, &fresh, ZLINK_SEND_FLAGS_NONE,
                       ZLINK_PART_FINAL, NULL, NULL));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_recv (receiver, NULL, &received, &part_count,
                  ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_EQUAL_UINT64 (1, part_count);
    TEST_ASSERT_EQUAL_MEMORY ("z", zlink_msg_data (&received[0]), 1);
    zlink_multipart_close (received, part_count);
}

void test_public_inproc_dealer_recv_single_after_multipart_reset ()
{
    void *left = test_context_socket (ZLINK_SOCKET_DEALER);
    void *right = test_context_socket (ZLINK_SOCKET_DEALER);

    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (left, "inproc://public_inproc_dealer_recv_after_multipart"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (right, "inproc://public_inproc_dealer_recv_after_multipart"));

    zlink_msg_t multipart[2];
    const char head[] = "head";
    const char body[] = "body";
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&multipart[0], sizeof (head) - 1));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&multipart[1], sizeof (body) - 1));
    memcpy (zlink_msg_data (&multipart[0]), head, sizeof (head) - 1);
    memcpy (zlink_msg_data (&multipart[1]), body, sizeof (body) - 1);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_send (right, multipart, 2, 0));

    zlink_msg_t *received = NULL;
    size_t part_count = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_recv (left, NULL, &received, &part_count, 0));
    TEST_ASSERT_EQUAL_UINT64 (2, part_count);
    zlink_multipart_close (received, part_count);

    zlink_msg_t single;
    const char payload[] = "ping";
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&single, sizeof (payload) - 1));
    memcpy (zlink_msg_data (&single), payload, sizeof (payload) - 1);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_send (right, &single, 1, 0));

    received = NULL;
    part_count = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_recv (left, NULL, &received, &part_count, 0));
    TEST_ASSERT_EQUAL_UINT64 (1, part_count);
    TEST_ASSERT_EQUAL_UINT64 (sizeof (payload) - 1, zlink_msg_size (&received[0]));
    TEST_ASSERT_EQUAL_MEMORY (payload, zlink_msg_data (&received[0]), sizeof (payload) - 1);
    zlink_multipart_close (received, part_count);
}

void test_public_inproc_router_send_rid_blocking ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);

    const char routing_id[] = "D1";
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer, routing_id, sizeof (routing_id) - 1));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (router, "inproc://public_inproc_router_send_rid_blocking"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (dealer, "inproc://public_inproc_router_send_rid_blocking"));
    prime_router_recv_plane (router);
    msleep (50);

    zlink_msg_t outbound;
    const char payload[] = "ping";
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&outbound, sizeof (payload) - 1));
    memcpy (zlink_msg_data (&outbound), payload, sizeof (payload) - 1);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_send (dealer, &outbound, 1, 0));

    const zlink_routing_id_t *source_rid = NULL;
    zlink_msg_t *received = NULL;
    size_t part_count = 0;
    uint64_t request_seq = 0;
    recv_router_until_message (router, &source_rid, &request_seq, &received, &part_count);
    TEST_ASSERT_NOT_NULL (source_rid);
    TEST_ASSERT_EQUAL_UINT64 (0, request_seq);
    TEST_ASSERT_EQUAL_UINT64 (1, part_count);
    TEST_ASSERT_EQUAL_UINT64 (sizeof (routing_id) - 1, source_rid->size);
    TEST_ASSERT_EQUAL_MEMORY (routing_id, source_rid->data, sizeof (routing_id) - 1);
    zlink_multipart_close (received, part_count);

    zlink_msg_t reply;
    const char reply_payload[] = "pong";
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&reply, sizeof (reply_payload) - 1));
    memcpy (zlink_msg_data (&reply), reply_payload, sizeof (reply_payload) - 1);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_send_rid (router, source_rid, &reply, 1, 0));

    zlink_msg_t *reply_parts = NULL;
    size_t reply_count = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_recv (dealer, NULL, &reply_parts, &reply_count, 0));
    TEST_ASSERT_EQUAL_UINT64 (1, reply_count);
    TEST_ASSERT_EQUAL_UINT64 (sizeof (reply_payload) - 1, zlink_msg_size (&reply_parts[0]));
    TEST_ASSERT_EQUAL_MEMORY (reply_payload, zlink_msg_data (&reply_parts[0]),
                              sizeof (reply_payload) - 1);
    zlink_multipart_close (reply_parts, reply_count);
}


void test_public_inproc_router_send_rid_multipart_blocking ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);

    const char routing_id[] = "D3";
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer, routing_id, sizeof (routing_id) - 1));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (router, "inproc://public_inproc_router_send_rid_multipart_blocking"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (dealer, "inproc://public_inproc_router_send_rid_multipart_blocking"));
    prime_router_recv_plane (router);
    msleep (50);

    zlink_msg_t outbound;
    const char payload[] = "ping";
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&outbound, sizeof (payload) - 1));
    memcpy (zlink_msg_data (&outbound), payload, sizeof (payload) - 1);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_send (dealer, &outbound, 1, 0));

    const zlink_routing_id_t *source_rid = NULL;
    zlink_msg_t *received = NULL;
    size_t part_count = 0;
    uint64_t request_seq = 0;
    recv_router_until_message (router, &source_rid, &request_seq, &received, &part_count);
    TEST_ASSERT_NOT_NULL (source_rid);
    TEST_ASSERT_EQUAL_UINT64 (0, request_seq);
    TEST_ASSERT_EQUAL_UINT64 (1, part_count);
    TEST_ASSERT_EQUAL_UINT64 (sizeof (routing_id) - 1, source_rid->size);
    TEST_ASSERT_EQUAL_MEMORY (routing_id, source_rid->data, sizeof (routing_id) - 1);
    zlink_multipart_close (received, part_count);

    zlink_msg_t reply_parts[2];
    const char reply_head[] = "pong";
    const char reply_body[] = "tail";
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&reply_parts[0], sizeof (reply_head) - 1));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&reply_parts[1], sizeof (reply_body) - 1));
    memcpy (zlink_msg_data (&reply_parts[0]), reply_head, sizeof (reply_head) - 1);
    memcpy (zlink_msg_data (&reply_parts[1]), reply_body, sizeof (reply_body) - 1);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_send_rid (router, source_rid, reply_parts, 2, 0));
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&reply_parts[0]));
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&reply_parts[1]));

    zlink_msg_t *reply_recv = NULL;
    size_t reply_count = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_recv (dealer, NULL, &reply_recv, &reply_count, 0));
    TEST_ASSERT_EQUAL_UINT64 (2, reply_count);
    TEST_ASSERT_EQUAL_UINT64 (sizeof (reply_head) - 1, zlink_msg_size (&reply_recv[0]));
    TEST_ASSERT_EQUAL_MEMORY (reply_head, zlink_msg_data (&reply_recv[0]), sizeof (reply_head) - 1);
    TEST_ASSERT_EQUAL_UINT64 (sizeof (reply_body) - 1, zlink_msg_size (&reply_recv[1]));
    TEST_ASSERT_EQUAL_MEMORY (reply_body, zlink_msg_data (&reply_recv[1]), sizeof (reply_body) - 1);
    zlink_multipart_close (reply_recv, reply_count);
}

void test_public_inproc_router_recv_multipart_with_source_rid_blocking ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);

    const char routing_id[] = "D4";
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer, routing_id, sizeof (routing_id) - 1));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (router, "inproc://public_inproc_router_recv_rid_multipart_blocking"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (dealer, "inproc://public_inproc_router_recv_rid_multipart_blocking"));
    prime_router_recv_plane (router);
    msleep (50);

    zlink_msg_t outbound_parts[2];
    const char head[] = "head";
    const char body[] = "body";
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&outbound_parts[0], sizeof (head) - 1));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&outbound_parts[1], sizeof (body) - 1));
    memcpy (zlink_msg_data (&outbound_parts[0]), head, sizeof (head) - 1);
    memcpy (zlink_msg_data (&outbound_parts[1]), body, sizeof (body) - 1);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_send (dealer, outbound_parts, 2, 0));

    const zlink_routing_id_t *source_rid = NULL;
    zlink_msg_t *received = NULL;
    size_t part_count = 0;
    uint64_t request_seq = 0;
    recv_router_until_message (router, &source_rid, &request_seq, &received, &part_count);
    TEST_ASSERT_NOT_NULL (source_rid);
    TEST_ASSERT_EQUAL_UINT64 (0, request_seq);
    TEST_ASSERT_EQUAL_UINT64 (sizeof (routing_id) - 1, source_rid->size);
    TEST_ASSERT_EQUAL_MEMORY (routing_id, source_rid->data, sizeof (routing_id) - 1);
    TEST_ASSERT_EQUAL_UINT64 (2, part_count);
    TEST_ASSERT_EQUAL_UINT64 (sizeof (head) - 1, zlink_msg_size (&received[0]));
    TEST_ASSERT_EQUAL_MEMORY (head, zlink_msg_data (&received[0]), sizeof (head) - 1);
    TEST_ASSERT_EQUAL_UINT64 (sizeof (body) - 1, zlink_msg_size (&received[1]));
    TEST_ASSERT_EQUAL_MEMORY (body, zlink_msg_data (&received[1]), sizeof (body) - 1);
    zlink_multipart_close (received, part_count);
}

void test_public_inproc_router_recv_keeps_source_rid_across_reset ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);

    const char routing_id[] = "D5";
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer, routing_id, sizeof (routing_id) - 1));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (router, "inproc://public_inproc_router_msg_recv_rid_reset_blocking"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (dealer, "inproc://public_inproc_router_msg_recv_rid_reset_blocking"));
    prime_router_recv_plane (router);
    msleep (50);

    zlink_msg_t multipart[2];
    const char head[] = "head";
    const char body[] = "body";
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&multipart[0], sizeof (head) - 1));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&multipart[1], sizeof (body) - 1));
    memcpy (zlink_msg_data (&multipart[0]), head, sizeof (head) - 1);
    memcpy (zlink_msg_data (&multipart[1]), body, sizeof (body) - 1);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_send (dealer, multipart, 2, 0));

    zlink_msg_t received_storage[2];
    zlink_msg_t *received = received_storage;
    zlink_part_flag_t received_more[2];
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&received[0]));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&received[1]));
    const zlink_routing_id_t *source_rid_a = NULL;
    uint64_t request_seq_a = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_router_recv_part (
      router, &source_rid_a, &request_seq_a, &received[0], &received_more[0],
      ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_router_recv_part (
      router, &source_rid_a, &request_seq_a, &received[1], &received_more[1],
      ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_NOT_NULL (source_rid_a);
    TEST_ASSERT_EQUAL_UINT64 (0, request_seq_a);
    TEST_ASSERT_EQUAL_UINT64 (sizeof (routing_id) - 1, source_rid_a->size);
    TEST_ASSERT_EQUAL_MEMORY (routing_id, source_rid_a->data, sizeof (routing_id) - 1);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_MORE, received_more[0]);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, received_more[1]);
    TEST_ASSERT_EQUAL_UINT64 (sizeof (head) - 1, zlink_msg_size (&received[0]));
    TEST_ASSERT_EQUAL_MEMORY (head, zlink_msg_data (&received[0]), sizeof (head) - 1);
    TEST_ASSERT_EQUAL_UINT64 (sizeof (body) - 1, zlink_msg_size (&received[1]));
    TEST_ASSERT_EQUAL_MEMORY (body, zlink_msg_data (&received[1]), sizeof (body) - 1);
    zlink_multipart_close (received, 2);

    zlink_msg_t single;
    const char payload[] = "ping";
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&single, sizeof (payload) - 1));
    memcpy (zlink_msg_data (&single), payload, sizeof (payload) - 1);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_send (dealer, &single, 1, 0));

    const zlink_routing_id_t *source_rid_c = NULL;
    uint64_t request_seq_c = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&received[0]));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_router_recv_part (
      router, &source_rid_c, &request_seq_c, &received[0], &received_more[0],
      ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_NOT_NULL (source_rid_c);
    TEST_ASSERT_EQUAL_UINT64 (0, request_seq_c);
    TEST_ASSERT_EQUAL_UINT64 (sizeof (routing_id) - 1, source_rid_c->size);
    TEST_ASSERT_EQUAL_MEMORY (routing_id, source_rid_c->data, sizeof (routing_id) - 1);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, received_more[0]);
    TEST_ASSERT_EQUAL_UINT64 (sizeof (payload) - 1, zlink_msg_size (&received[0]));
    TEST_ASSERT_EQUAL_MEMORY (payload, zlink_msg_data (&received[0]), sizeof (payload) - 1);
    zlink_multipart_close (received, 1);
}

void test_public_inproc_data_payload_matching_envelope_stays_data ()
{
    void *left = test_context_socket (ZLINK_SOCKET_PAIR);
    void *right = test_context_socket (ZLINK_SOCKET_PAIR);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (left, "inproc://public_inproc_request_false_positive"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (right, "inproc://public_inproc_request_false_positive"));

    const unsigned char payload[] = {'Z', 'R', 'R', 'P', 1, 1, 0, 0, 0, 0, 0, 0, 0, 42};
    zlink_msg_t outbound;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&outbound, sizeof (payload)));
    memcpy (zlink_msg_data (&outbound), payload, sizeof (payload));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_send (right, &outbound, 1, 0));

    zlink_msg_t *received = NULL;
    size_t part_count = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_recv (left, NULL, &received, &part_count, 0));
    TEST_ASSERT_EQUAL_UINT64 (1, part_count);
    TEST_ASSERT_EQUAL_UINT64 (sizeof (payload), zlink_msg_size (&received[0]));
    TEST_ASSERT_EQUAL_MEMORY (payload, zlink_msg_data (&received[0]), sizeof (payload));
    zlink_multipart_close (received, part_count);
}

void test_public_inproc_pair_send_failure_consumes_all_parts ()
{
    void *right = test_context_socket (ZLINK_SOCKET_PAIR);

    zlink_msg_t parts[2];
    const char header[] = "head";
    const char body[] = "body";
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&parts[0], sizeof (header) - 1));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&parts[1], sizeof (body) - 1));
    memcpy (zlink_msg_data (&parts[0]), header, sizeof (header) - 1);
    memcpy (zlink_msg_data (&parts[1]), body, sizeof (body) - 1);

    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_BACKPRESSURED,
                           zlink_send (right, parts, 2, ZLINK_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&parts[0]));
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&parts[1]));
}

void test_public_inproc_pair_send_is_safe_from_multiple_threads ()
{
    void *left = test_context_socket (ZLINK_SOCKET_PAIR);
    void *right = test_context_socket (ZLINK_SOCKET_PAIR);

    // Admit only one tiny record before requiring receiver progress. Both
    // senders must therefore share the public command-owner handoff instead
    // of completing entirely in the pipe's initial capacity.
    const uint64_t hwm_bytes = 1;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (left, ZLINK_OPT_RCVHWM, &hwm_bytes,
                        sizeof (hwm_bytes)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (right, ZLINK_OPT_SNDHWM, &hwm_bytes,
                        sizeof (hwm_bytes)));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (left, "inproc://public_inproc_pair_concurrent_send"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (right, "inproc://public_inproc_pair_concurrent_send"));

    const int timeout_ms = 5000;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (left, ZLINK_OPT_RCVTIMEO, &timeout_ms, sizeof (timeout_ms)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (right, ZLINK_OPT_SNDTIMEO, &timeout_ms, sizeof (timeout_ms)));

    send_start_gate_t gate;
    const int per_sender = 64;
    sender_probe_t probe_a;
    probe_a.gate = &gate;
    probe_a.socket = right;
    probe_a.tag = static_cast<unsigned char> ('A');
    probe_a.count = per_sender;
    sender_probe_t probe_b;
    probe_b.gate = &gate;
    probe_b.socket = right;
    probe_b.tag = static_cast<unsigned char> ('B');
    probe_b.count = per_sender;
    std::thread sender_a (wait_and_send_messages, &probe_a);
    std::thread sender_b (wait_and_send_messages, &probe_b);

    {
        std::unique_lock<std::mutex> lock (gate.mutex);
        const bool ready = gate.cv.wait_for (lock, std::chrono::milliseconds (5000), [&] () {
            return gate.ready.load (std::memory_order_acquire) == 2;
        });
        TEST_ASSERT_TRUE (ready);
    }

    {
        std::lock_guard<std::mutex> lock (gate.mutex);
        gate.go = true;
    }
    gate.cv.notify_all ();

    int count_a = 0;
    int count_b = 0;
    for (int i = 0; i < per_sender * 2; ++i) {
        zlink_msg_t *parts = NULL;
        size_t part_count = 0;
        TEST_ASSERT_SUCCESS_ERRNO (zlink_recv (left, NULL, &parts, &part_count, 0));
        TEST_ASSERT_EQUAL_UINT64 (1, part_count);
        TEST_ASSERT_EQUAL_UINT64 (2, zlink_msg_size (&parts[0]));
        const unsigned char *data = static_cast<const unsigned char *> (zlink_msg_data (&parts[0]));
        if (data[0] == static_cast<unsigned char> ('A'))
            ++count_a;
        else if (data[0] == static_cast<unsigned char> ('B'))
            ++count_b;
        else
            TEST_FAIL_MESSAGE ("unexpected sender tag");
        zlink_multipart_close (parts, part_count);
    }

    sender_a.join ();
    sender_b.join ();

    TEST_ASSERT_FALSE (probe_a.failed.load (std::memory_order_acquire));
    TEST_ASSERT_FALSE (probe_b.failed.load (std::memory_order_acquire));
    TEST_ASSERT_EQUAL_INT (per_sender, count_a);
    TEST_ASSERT_EQUAL_INT (per_sender, count_b);
}

void test_public_inproc_dealer_send_is_safe_from_multiple_threads ()
{
    void *left = test_context_socket (ZLINK_SOCKET_DEALER);
    void *right = test_context_socket (ZLINK_SOCKET_DEALER);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (left, "inproc://public_inproc_dealer_concurrent_send"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (right, "inproc://public_inproc_dealer_concurrent_send"));

    const int timeout_ms = 5000;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (left, ZLINK_OPT_RCVTIMEO, &timeout_ms, sizeof (timeout_ms)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (right, ZLINK_OPT_SNDTIMEO, &timeout_ms, sizeof (timeout_ms)));

    send_start_gate_t gate;
    const int per_sender = 64;
    sender_probe_t probe_a;
    probe_a.gate = &gate;
    probe_a.socket = right;
    probe_a.tag = static_cast<unsigned char> ('A');
    probe_a.count = per_sender;
    sender_probe_t probe_b;
    probe_b.gate = &gate;
    probe_b.socket = right;
    probe_b.tag = static_cast<unsigned char> ('B');
    probe_b.count = per_sender;
    std::thread sender_a (wait_and_send_messages, &probe_a);
    std::thread sender_b (wait_and_send_messages, &probe_b);

    {
        std::unique_lock<std::mutex> lock (gate.mutex);
        const bool ready = gate.cv.wait_for (lock, std::chrono::milliseconds (5000), [&] () {
            return gate.ready.load (std::memory_order_acquire) == 2;
        });
        TEST_ASSERT_TRUE (ready);
    }

    {
        std::lock_guard<std::mutex> lock (gate.mutex);
        gate.go = true;
    }
    gate.cv.notify_all ();

    int count_a = 0;
    int count_b = 0;
    for (int i = 0; i < per_sender * 2; ++i) {
        zlink_msg_t *parts = NULL;
        size_t part_count = 0;
        TEST_ASSERT_SUCCESS_ERRNO (zlink_recv (left, NULL, &parts, &part_count, 0));
        TEST_ASSERT_EQUAL_UINT64 (1, part_count);
        TEST_ASSERT_EQUAL_UINT64 (2, zlink_msg_size (&parts[0]));
        const unsigned char *data = static_cast<const unsigned char *> (zlink_msg_data (&parts[0]));
        if (data[0] == static_cast<unsigned char> ('A'))
            ++count_a;
        else if (data[0] == static_cast<unsigned char> ('B'))
            ++count_b;
        else
            TEST_FAIL_MESSAGE ("unexpected sender tag");
        zlink_multipart_close (parts, part_count);
    }

    sender_a.join ();
    sender_b.join ();

    TEST_ASSERT_FALSE (probe_a.failed.load (std::memory_order_acquire));
    TEST_ASSERT_FALSE (probe_b.failed.load (std::memory_order_acquire));
    TEST_ASSERT_EQUAL_INT (per_sender, count_a);
    TEST_ASSERT_EQUAL_INT (per_sender, count_b);
}

void test_nonblocking_send_close_race_is_lifetime_safe ()
{
    const int attempts = 32;
    const int sender_count = 4;

    for (int attempt = 0; attempt < attempts; ++attempt) {
        void *socket = test_context_socket (ZLINK_SOCKET_PAIR);
        const int zero = 0;
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_set_option (socket, ZLINK_OPT_LINGER, &zero, sizeof (zero)));

        send_start_gate_t gate;
        std::atomic<bool> stop (false);
        std::atomic<int> message_init_errno (0);
        std::atomic<uint64_t> unexpected_submit_pair (0);
        std::atomic<size_t> unconsumed_part_size (0);
        std::vector<std::thread> senders;
        senders.reserve (sender_count);
        for (int sender = 0; sender < sender_count; ++sender) {
            senders.emplace_back ([&] {
                {
                    std::unique_lock<std::mutex> lock (gate.mutex);
                    gate.ready.fetch_add (1, std::memory_order_acq_rel);
                    gate.cv.notify_all ();
                    gate.cv.wait (lock, [&] () { return gate.go; });
                }

                uint64_t part_index = 0;
                while (!stop.load (std::memory_order_acquire)) {
                    zlink_msg_t part;
                    if (zlink_msg_init_size (&part, 64) != ZLINK_CONFIG_OK) {
                        const int init_errno = zlink_errno ();
                        const int observed_errno = init_errno != 0
                                                     ? init_errno
                                                     : EIO;
                        int expected_errno = 0;
                        message_init_errno.compare_exchange_strong (
                          expected_errno, observed_errno,
                          std::memory_order_acq_rel,
                          std::memory_order_acquire);
                        return;
                    }
                    memset (zlink_msg_data (&part), 0x5a, 64);
                    const zlink_part_flag_t part_flag =
                      (part_index++ & 1) == 0 ? ZLINK_PART_MORE
                                              : ZLINK_PART_FINAL;
                    const zlink_submit_result_t rc = zlink_send_part (
                      socket, &part, ZLINK_SEND_FLAGS_DONTWAIT,
                      part_flag, NULL, NULL);
                    const int err = zlink_errno ();
                    const size_t remaining_size = zlink_msg_size (&part);
                    if (remaining_size != 0) {
                        size_t expected_size = 0;
                        unconsumed_part_size.compare_exchange_strong (
                          expected_size, remaining_size,
                          std::memory_order_acq_rel,
                          std::memory_order_acquire);
                        zlink_msg_close (&part);
                        return;
                    }
                    if (rc == ZLINK_SUBMIT_OK
                        || (rc == ZLINK_SUBMIT_BACKPRESSURED && err == EAGAIN)
                        || (rc == ZLINK_SUBMIT_TERMINATED
                            && err == ESHUTDOWN))
                        continue;
                    const uint64_t observed_pair =
                      (static_cast<uint64_t> (static_cast<uint32_t> (rc))
                       << 32)
                      | static_cast<uint32_t> (err);
                    uint64_t expected_pair = 0;
                    unexpected_submit_pair.compare_exchange_strong (
                      expected_pair, observed_pair,
                      std::memory_order_acq_rel,
                      std::memory_order_acquire);
                    return;
                }
            });
        }

        zlink_close_result_t close_rc = ZLINK_CLOSE_INTERNAL_ERROR;
        int close_errno = 0;
        std::thread closer ([&] {
            {
                std::unique_lock<std::mutex> lock (gate.mutex);
                gate.ready.fetch_add (1, std::memory_order_acq_rel);
                gate.cv.notify_all ();
                gate.cv.wait (lock, [&] () { return gate.go; });
            }
            close_rc = zlink_close (socket);
            close_errno = zlink_errno ();
            stop.store (true, std::memory_order_release);
        });

        {
            std::unique_lock<std::mutex> lock (gate.mutex);
            const bool ready = gate.cv.wait_for (lock, std::chrono::milliseconds (5000), [&] () {
                return gate.ready.load (std::memory_order_acquire) == sender_count + 1;
            });
            TEST_ASSERT_TRUE (ready);
            gate.go = true;
        }
        gate.cv.notify_all ();

        closer.join ();
        stop.store (true, std::memory_order_release);
        for (std::vector<std::thread>::iterator it = senders.begin (); it != senders.end (); ++it)
            it->join ();

        TEST_ASSERT_EQUAL_INT (
          0, message_init_errno.load (std::memory_order_acquire));
        // High 32 bits are zlink_submit_result_t; low 32 bits are errno.
        TEST_ASSERT_EQUAL_HEX64 (
          0, unexpected_submit_pair.load (std::memory_order_acquire));
        TEST_ASSERT_EQUAL_UINT64 (
          0, unconsumed_part_size.load (std::memory_order_acquire));
        if (close_rc == ZLINK_CLOSE_OK) {
            test_context_socket_mark_closed (socket);
        } else {
            TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_BUSY, close_rc);
            TEST_ASSERT_EQUAL_INT (EBUSY, close_errno);
            TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_close (socket));
            test_context_socket_mark_closed (socket);
        }
    }
}

struct multipart_identity_payload_t
{
    uint32_t caller;
    uint32_t sequence;
    uint32_t part;
};

class cyclic_test_barrier_t
{
  public:
    explicit cyclic_test_barrier_t (int participants_) :
        _participants (participants_), _arrived (0), _generation (0)
    {
    }

    void wait ()
    {
        std::unique_lock<std::mutex> lock (_mutex);
        const int generation = _generation;
        if (++_arrived == _participants) {
            _arrived = 0;
            ++_generation;
            _cv.notify_all ();
            return;
        }
        _cv.wait (lock, [&] { return _generation != generation; });
    }

  private:
    const int _participants;
    int _arrived;
    int _generation;
    std::mutex _mutex;
    std::condition_variable _cv;
};

void run_four_caller_two_part_submit (
  void *sender_, void *receiver_, const zlink_routing_id_t *target_rid_,
  bool router_receive_)
{
    const int caller_count = 4;
    const int rounds = 100;
    cyclic_test_barrier_t more_barrier (caller_count);
    cyclic_test_barrier_t final_barrier (caller_count);
    std::atomic<int> failures (0);
    std::vector<std::thread> callers;
    callers.reserve (caller_count);
    for (int caller = 0; caller < caller_count; ++caller) {
        callers.emplace_back ([&, caller] {
            for (int sequence = 0; sequence < rounds; ++sequence) {
                multipart_identity_payload_t payload = {
                  static_cast<uint32_t> (caller),
                  static_cast<uint32_t> (sequence), 0};
                zlink_msg_t more;
                if (zlink_msg_init_size (&more, sizeof (payload)) != 0) {
                    failures.fetch_add (1, std::memory_order_relaxed);
                    more_barrier.wait ();
                    final_barrier.wait ();
                    continue;
                }
                memcpy (zlink_msg_data (&more), &payload, sizeof (payload));
                const zlink_submit_result_t more_result = target_rid_
                  ? zlink_send_part_rid (
                      sender_, target_rid_, &more, ZLINK_SEND_FLAGS_NONE,
                      ZLINK_PART_MORE, NULL, NULL)
                  : zlink_send_part (
                      sender_, &more, ZLINK_SEND_FLAGS_NONE,
                      ZLINK_PART_MORE, NULL, NULL);
                if (more_result != ZLINK_SUBMIT_OK)
                    failures.fetch_add (1, std::memory_order_relaxed);
                zlink_msg_close (&more);
                more_barrier.wait ();

                payload.part = 1;
                zlink_msg_t final_part;
                if (zlink_msg_init_size (&final_part, sizeof (payload)) != 0) {
                    failures.fetch_add (1, std::memory_order_relaxed);
                    final_barrier.wait ();
                    continue;
                }
                memcpy (zlink_msg_data (&final_part), &payload,
                        sizeof (payload));
                const zlink_submit_result_t final_result = target_rid_
                  ? zlink_send_part_rid (
                      sender_, target_rid_, &final_part,
                      ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL, NULL, NULL)
                  : zlink_send_part (
                      sender_, &final_part, ZLINK_SEND_FLAGS_NONE,
                      ZLINK_PART_FINAL, NULL, NULL);
                if (final_result != ZLINK_SUBMIT_OK)
                    failures.fetch_add (1, std::memory_order_relaxed);
                zlink_msg_close (&final_part);
                final_barrier.wait ();
            }
        });
    }

    bool seen[caller_count][rounds] = {};
    for (int record = 0; record < caller_count * rounds; ++record) {
        zlink_msg_t *parts = NULL;
        size_t part_count = 0;
        if (router_receive_) {
            const zlink_routing_id_t *source_rid = NULL;
            uint64_t request_seq = 0;
            recv_router_until_message (receiver_, &source_rid, &request_seq,
                                       &parts, &part_count);
        } else {
            TEST_ASSERT_EQUAL_INT (
              ZLINK_RECV_OK,
              zlink_recv (receiver_, NULL, &parts, &part_count,
                          ZLINK_RECV_FLAGS_NONE));
        }
        TEST_ASSERT_EQUAL_UINT64 (2, part_count);
        multipart_identity_payload_t first = {};
        multipart_identity_payload_t second = {};
        TEST_ASSERT_EQUAL_UINT64 (sizeof (first), zlink_msg_size (&parts[0]));
        TEST_ASSERT_EQUAL_UINT64 (sizeof (second), zlink_msg_size (&parts[1]));
        if (part_count == 2) {
            memcpy (&first, zlink_msg_data (&parts[0]), sizeof (first));
            memcpy (&second, zlink_msg_data (&parts[1]), sizeof (second));
            TEST_ASSERT_EQUAL_UINT32 (first.caller, second.caller);
            TEST_ASSERT_EQUAL_UINT32 (first.sequence, second.sequence);
            TEST_ASSERT_EQUAL_UINT32 (0, first.part);
            TEST_ASSERT_EQUAL_UINT32 (1, second.part);
            TEST_ASSERT_TRUE (first.caller < static_cast<uint32_t> (caller_count));
            TEST_ASSERT_TRUE (first.sequence < static_cast<uint32_t> (rounds));
            if (first.caller < static_cast<uint32_t> (caller_count)
                && first.sequence < static_cast<uint32_t> (rounds)) {
                TEST_ASSERT_FALSE (seen[first.caller][first.sequence]);
                seen[first.caller][first.sequence] = true;
            }
        }
        zlink_multipart_close (parts, part_count);
    }
    for (int caller = 0; caller != caller_count; ++caller)
        for (int sequence = 0; sequence != rounds; ++sequence)
            TEST_ASSERT_TRUE (seen[caller][sequence]);
    for (std::vector<std::thread>::iterator it = callers.begin ();
         it != callers.end (); ++it)
        it->join ();
    TEST_ASSERT_EQUAL_INT (0, failures.load (std::memory_order_relaxed));
}

void test_pair_four_callers_stage_two_parts_independently ()
{
    void *receiver = test_context_socket (ZLINK_SOCKET_PAIR);
    void *sender = test_context_socket (ZLINK_SOCKET_PAIR);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (receiver, "inproc://pair-four-caller-multipart"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (sender, "inproc://pair-four-caller-multipart"));
    run_four_caller_two_part_submit (sender, receiver, NULL, false);
}

void test_dealer_four_callers_stage_two_parts_independently ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (router, "inproc://dealer-four-caller-multipart"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (dealer, "inproc://dealer-four-caller-multipart"));
    prime_router_recv_plane (router);
    run_four_caller_two_part_submit (dealer, router, NULL, true);
}

void test_router_four_callers_stage_two_parts_independently ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    const char rid_text[] = "router-four-caller-peer";
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_routing_id (dealer, rid_text, sizeof (rid_text) - 1));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (router, "inproc://router-four-caller-multipart"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (dealer, "inproc://router-four-caller-multipart"));
    prime_router_recv_plane (router);
    zlink_msg_t probe;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&probe, 1));
    *static_cast<char *> (zlink_msg_data (&probe)) = 'p';
    TEST_ASSERT_SUCCESS_ERRNO (zlink_send (dealer, &probe, 1, 0));
    const zlink_routing_id_t *source_rid = NULL;
    uint64_t request_seq = 0;
    zlink_msg_t *parts = NULL;
    size_t part_count = 0;
    recv_router_until_message (router, &source_rid, &request_seq, &parts,
                               &part_count);
    zlink_routing_id_t target = *source_rid;
    zlink_multipart_close (parts, part_count);
    run_four_caller_two_part_submit (router, dealer, &target, false);
}

void test_router_two_callers_use_different_rids_concurrently ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealers[2] = {test_context_socket (ZLINK_SOCKET_DEALER),
                        test_context_socket (ZLINK_SOCKET_DEALER)};
    const char *const rid_text[2] = {"router-rid-a", "router-rid-b"};
    for (int i = 0; i != 2; ++i)
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_set_routing_id (dealers[i], rid_text[i], strlen (rid_text[i])));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (router, "inproc://router-two-different-rids"));
    for (int i = 0; i != 2; ++i)
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_connect (dealers[i], "inproc://router-two-different-rids"));
    prime_router_recv_plane (router);

    zlink_routing_id_t targets[2];
    for (int i = 0; i != 2; ++i) {
        zlink_msg_t probe;
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&probe, 1));
        *static_cast<unsigned char *> (zlink_msg_data (&probe)) =
          static_cast<unsigned char> ('0' + i);
        TEST_ASSERT_SUCCESS_ERRNO (zlink_send (dealers[i], &probe, 1, 0));
        const zlink_routing_id_t *source_rid = NULL;
        uint64_t request_seq = 0;
        zlink_msg_t *parts = NULL;
        size_t part_count = 0;
        recv_router_until_message (router, &source_rid, &request_seq, &parts,
                                   &part_count);
        TEST_ASSERT_NOT_NULL (source_rid);
        TEST_ASSERT_EQUAL_UINT64 (1, part_count);
        TEST_ASSERT_EQUAL_INT ('0' + i,
                               *static_cast<unsigned char *> (
                                 zlink_msg_data (&parts[0])));
        targets[i] = *source_rid;
        zlink_multipart_close (parts, part_count);
    }

    cyclic_test_barrier_t more_barrier (2);
    std::atomic<int> failures (0);
    std::thread callers[2];
    for (int caller = 0; caller != 2; ++caller) {
        callers[caller] = std::thread ([&, caller] {
            for (int part_index = 0; part_index != 2; ++part_index) {
                zlink_msg_t part;
                if (zlink_msg_init_size (&part, 1) != ZLINK_CONFIG_OK) {
                    failures.fetch_add (1, std::memory_order_relaxed);
                    if (part_index == 0)
                        more_barrier.wait ();
                    return;
                }
                *static_cast<unsigned char *> (zlink_msg_data (&part)) =
                  static_cast<unsigned char> ('A' + caller * 2 + part_index);
                if (zlink_send_part_rid (
                      router, &targets[caller], &part, ZLINK_SEND_FLAGS_NONE,
                      part_index == 0 ? ZLINK_PART_MORE : ZLINK_PART_FINAL,
                      NULL, NULL)
                    != ZLINK_SUBMIT_OK)
                    failures.fetch_add (1, std::memory_order_relaxed);
                zlink_msg_close (&part);
                if (part_index == 0)
                    more_barrier.wait ();
            }
        });
    }
    for (int caller = 0; caller != 2; ++caller) {
        zlink_msg_t *parts = NULL;
        size_t part_count = 0;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_RECV_OK,
          zlink_recv (dealers[caller], NULL, &parts, &part_count,
                      ZLINK_RECV_FLAGS_NONE));
        TEST_ASSERT_EQUAL_UINT64 (2, part_count);
        TEST_ASSERT_EQUAL_INT (
          'A' + caller * 2,
          *static_cast<unsigned char *> (zlink_msg_data (&parts[0])));
        TEST_ASSERT_EQUAL_INT (
          'A' + caller * 2 + 1,
          *static_cast<unsigned char *> (zlink_msg_data (&parts[1])));
        zlink_multipart_close (parts, part_count);
    }
    callers[0].join ();
    callers[1].join ();
    TEST_ASSERT_EQUAL_INT (0, failures.load (std::memory_order_relaxed));
}

int main (void)
{
    setup_test_environment ();

    UNITY_BEGIN ();
    RUN_TEST (test_public_socket_timeout_defaults_and_override);
    RUN_TEST (test_public_inproc_pair_send_single_part);
    RUN_TEST (test_public_inproc_pair_send_multipart_blocking);
    RUN_TEST (test_public_inproc_pair_recv_single_after_multipart_reset);
    RUN_TEST (test_public_inproc_dealer_send_single_part);
    RUN_TEST (test_public_inproc_dealer_send_multipart_blocking);
    RUN_TEST (test_dealer_multipart_size_failure_rolls_back_and_preserves_errno);
    RUN_TEST (test_public_inproc_dealer_recv_single_after_multipart_reset);
    RUN_TEST (test_public_inproc_pair_send_failure_consumes_all_parts);
    RUN_TEST (test_public_inproc_pair_send_is_safe_from_multiple_threads);
    RUN_TEST (test_public_inproc_dealer_send_is_safe_from_multiple_threads);
    RUN_TEST (test_nonblocking_send_close_race_is_lifetime_safe);
    RUN_TEST (test_pair_four_callers_stage_two_parts_independently);
    RUN_TEST (test_dealer_four_callers_stage_two_parts_independently);
    RUN_TEST (test_router_four_callers_stage_two_parts_independently);
    RUN_TEST (test_router_two_callers_use_different_rids_concurrently);
    RUN_TEST (test_public_inproc_router_send_rid_blocking);
    RUN_TEST (test_public_inproc_router_send_rid_multipart_blocking);
    RUN_TEST (test_public_inproc_router_recv_multipart_with_source_rid_blocking);
    RUN_TEST (test_public_inproc_router_recv_keeps_source_rid_across_reset);
    RUN_TEST (test_public_inproc_data_payload_matching_envelope_stays_data);
    const int rc = UNITY_END ();
    fflush (NULL);
    std::_Exit (rc);
}
