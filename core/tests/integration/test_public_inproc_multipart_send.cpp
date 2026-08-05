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

void test_public_inproc_router_send_envelope_blocking ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);

    const char routing_id[] = "D2";
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer, routing_id, sizeof (routing_id) - 1));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (router, "inproc://public_inproc_router_send_envelope_blocking"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (dealer, "inproc://public_inproc_router_send_envelope_blocking"));
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
    const char reply_payload[] = "pong";
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&reply_parts[0], source_rid->size));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&reply_parts[1], sizeof (reply_payload) - 1));
    memcpy (zlink_msg_data (&reply_parts[0]), source_rid->data, source_rid->size);
    memcpy (zlink_msg_data (&reply_parts[1]), reply_payload, sizeof (reply_payload) - 1);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_send (router, reply_parts, 2, 0));
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&reply_parts[0]));
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&reply_parts[1]));

    zlink_msg_t *reply_recv = NULL;
    size_t reply_count = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_recv (dealer, NULL, &reply_recv, &reply_count, 0));
    TEST_ASSERT_EQUAL_UINT64 (1, reply_count);
    TEST_ASSERT_EQUAL_UINT64 (sizeof (reply_payload) - 1, zlink_msg_size (&reply_recv[0]));
    TEST_ASSERT_EQUAL_MEMORY (reply_payload, zlink_msg_data (&reply_recv[0]),
                              sizeof (reply_payload) - 1);
    zlink_multipart_close (reply_recv, reply_count);
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

    zlink_msg_t *received = NULL;
    size_t part_count = 0;
    const zlink_routing_id_t *source_rid_a = NULL;
    uint64_t request_seq_a = 0;
    recv_router_until_message (router, &source_rid_a, &request_seq_a, &received, &part_count);
    TEST_ASSERT_NOT_NULL (source_rid_a);
    TEST_ASSERT_EQUAL_UINT64 (0, request_seq_a);
    TEST_ASSERT_EQUAL_UINT64 (sizeof (routing_id) - 1, source_rid_a->size);
    TEST_ASSERT_EQUAL_MEMORY (routing_id, source_rid_a->data, sizeof (routing_id) - 1);
    TEST_ASSERT_EQUAL_UINT64 (2, part_count);
    TEST_ASSERT_TRUE (test_msg_has_more (&received[0]));
    TEST_ASSERT_FALSE (test_msg_has_more (&received[1]));
    TEST_ASSERT_EQUAL_UINT64 (sizeof (head) - 1, zlink_msg_size (&received[0]));
    TEST_ASSERT_EQUAL_MEMORY (head, zlink_msg_data (&received[0]), sizeof (head) - 1);
    TEST_ASSERT_EQUAL_UINT64 (sizeof (body) - 1, zlink_msg_size (&received[1]));
    TEST_ASSERT_EQUAL_MEMORY (body, zlink_msg_data (&received[1]), sizeof (body) - 1);
    zlink_multipart_close (received, part_count);

    zlink_msg_t single;
    const char payload[] = "ping";
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&single, sizeof (payload) - 1));
    memcpy (zlink_msg_data (&single), payload, sizeof (payload) - 1);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_send (dealer, &single, 1, 0));

    const zlink_routing_id_t *source_rid_c = NULL;
    uint64_t request_seq_c = 0;
    recv_router_until_message (router, &source_rid_c, &request_seq_c, &received, &part_count);
    TEST_ASSERT_NOT_NULL (source_rid_c);
    TEST_ASSERT_EQUAL_UINT64 (0, request_seq_c);
    TEST_ASSERT_EQUAL_UINT64 (sizeof (routing_id) - 1, source_rid_c->size);
    TEST_ASSERT_EQUAL_MEMORY (routing_id, source_rid_c->data, sizeof (routing_id) - 1);
    TEST_ASSERT_EQUAL_UINT64 (1, part_count);
    TEST_ASSERT_FALSE (test_msg_has_more (&received[0]));
    TEST_ASSERT_EQUAL_UINT64 (sizeof (payload) - 1, zlink_msg_size (&received[0]));
    TEST_ASSERT_EQUAL_MEMORY (payload, zlink_msg_data (&received[0]), sizeof (payload) - 1);
    zlink_multipart_close (received, part_count);
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
    RUN_TEST (test_public_inproc_dealer_recv_single_after_multipart_reset);
    RUN_TEST (test_public_inproc_pair_send_failure_consumes_all_parts);
    RUN_TEST (test_public_inproc_pair_send_is_safe_from_multiple_threads);
    RUN_TEST (test_public_inproc_dealer_send_is_safe_from_multiple_threads);
    RUN_TEST (test_public_inproc_router_send_rid_blocking);
    RUN_TEST (test_public_inproc_router_send_envelope_blocking);
    RUN_TEST (test_public_inproc_router_send_rid_multipart_blocking);
    RUN_TEST (test_public_inproc_router_recv_multipart_with_source_rid_blocking);
    RUN_TEST (test_public_inproc_router_recv_keeps_source_rid_across_reset);
    RUN_TEST (test_public_inproc_data_payload_matching_envelope_stays_data);
    const int rc = UNITY_END ();
    fflush (NULL);
    std::_Exit (rc);
}
