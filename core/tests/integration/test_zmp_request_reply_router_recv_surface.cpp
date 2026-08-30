/* SPDX-License-Identifier: MPL-2.0 */

#include "../testutil.hpp"
#include "../testutil_unity.hpp"
#include "api/socket/socket_api_internal.hpp"
#include "api/socket/request_reply_protocol_internal.hpp"
#include <string>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <future>
#include <mutex>

#include <unity.h>

SETUP_TEARDOWN_TESTCONTEXT

namespace
{
struct reply_probe_t
{
    std::mutex mutex;
    std::condition_variable cv;
    bool done;
    zlink_request_result_t result;
    size_t part_count;
    std::string payload;
    void *progress_handle;

    reply_probe_t () :
        done (false), result (ZLINK_REQUEST_PROTOCOL_ERROR), part_count (0), progress_handle (NULL)
    {
    }
};

struct router_recv_probe_t
{
    zlink_recv_result_t recv_rc;
    int recv_errno;
    bool source_rid_present;
    uint64_t request_seq;
    size_t part_count;
    std::string source_rid;
    std::string payload;
    zlink_submit_result_t reply_rc;
    int reply_errno;

    router_recv_probe_t () :
        recv_rc (ZLINK_RECV_INTERNAL_ERROR),
        recv_errno (0),
        source_rid_present (false),
        request_seq (0),
        part_count (0),
        reply_rc (ZLINK_SUBMIT_INTERNAL_ERROR),
        reply_errno (0)
    {
    }
};

std::string msg_to_string (const zlink_msg_t *msg_)
{
    return std::string (
      static_cast<const char *> (zlink_msg_data (const_cast<zlink_msg_t *> (msg_))),
      zlink_msg_size (const_cast<zlink_msg_t *> (msg_)));
}

void init_string_part (zlink_msg_t *part_, const char *text_)
{
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (part_, strlen (text_)));
    memcpy (zlink_msg_data (part_), text_, strlen (text_));
}

bool wait_for_reply (reply_probe_t *probe_)
{
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now () + std::chrono::milliseconds (SETTLE_TIME * 20);

    if (!probe_->progress_handle) {
        std::unique_lock<std::mutex> lock (probe_->mutex);
        return probe_->cv.wait_until (lock, deadline, [probe_] () { return probe_->done; });
    }

    while (std::chrono::steady_clock::now () < deadline) {
        {
            std::lock_guard<std::mutex> lock (probe_->mutex);
            if (probe_->done)
                return true;
        }

        const zlink_routing_id_t *source_rid = NULL;
        zlink_msg_t part;
        zlink_part_flag_t has_more = ZLINK_PART_FINAL;
        zlink_msg_init (&part);
        const zlink_recv_result_t rc = zlink_recv_part (probe_->progress_handle, &source_rid, &part,
                                                        &has_more, ZLINK_RECV_FLAGS_DONTWAIT);
        if (rc == ZLINK_RECV_OK) {
            zlink_msg_close (&part);
            while (has_more == ZLINK_PART_MORE) {
                zlink_msg_init (&part);
                if (zlink_recv_part (probe_->progress_handle, &source_rid, &part, &has_more,
                                     ZLINK_RECV_FLAGS_DONTWAIT)
                    != ZLINK_RECV_OK) {
                    zlink_msg_close (&part);
                    break;
                }
                zlink_msg_close (&part);
            }
        } else {
            zlink_msg_close (&part);
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }

    std::lock_guard<std::mutex> lock (probe_->mutex);
    return probe_->done;
}

void capture_reply (zlink_request_result_t result_,
                    zlink_msg_t *parts_,
                    size_t part_count_,
                    void *userdata_)
{
    reply_probe_t *probe = static_cast<reply_probe_t *> (userdata_);
    if (!probe)
        return;

    {
        std::lock_guard<std::mutex> lock (probe->mutex);
        probe->done = true;
        probe->result = result_;
        probe->part_count = part_count_;
        probe->payload = part_count_ > 0 ? msg_to_string (&parts_[0]) : std::string ();
    }
    probe->cv.notify_all ();
}

void assert_dealer_request_visible_through_router_recv (bool prime_recv_plane_)
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer, "dealer-recv", 11));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (router, "inproc://zmp-router-recv-surface"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer, "inproc://zmp-router-recv-surface"));
    msleep (SETTLE_TIME);

    if (prime_recv_plane_) {
        const zlink_routing_id_t *source_rid = NULL;
        uint64_t request_seq = 0;
        zlink_msg_t *parts = NULL;
        size_t part_count = 0;
        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA,
                               zlink_router_recv (router, &source_rid,
                                                  &request_seq, &parts, &part_count,
                                                  ZLINK_DONTWAIT));
        TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
    }

    zlink_msg_t request_part;
    zlink_msg_init (&request_part);
    init_string_part (&request_part, "dealer-request-recv");

    std::future<router_recv_probe_t> router_done = std::async (std::launch::async, [router] () {
        router_recv_probe_t result;
        const zlink_routing_id_t *source_rid = NULL;
        uint64_t request_seq = 0;
        zlink_msg_t *parts = NULL;
        size_t part_count = 0;
        result.recv_rc = zlink_router_recv (router, &source_rid, &request_seq,
                                            &parts, &part_count, 0);
        result.recv_errno = zlink_errno ();
        result.source_rid_present = source_rid != NULL;
        result.request_seq = request_seq;
        result.part_count = part_count;
        if (source_rid) {
            result.source_rid.assign (reinterpret_cast<const char *> (source_rid->data),
                                      source_rid->size);
        }
        if (parts && part_count > 0)
            result.payload = msg_to_string (&parts[0]);

        zlink_msg_t reply_part;
        zlink_msg_init (&reply_part);
        init_string_part (&reply_part, "router-reply-recv");
        if (result.recv_rc == ZLINK_RECV_OK && source_rid && request_seq != 0) {
            result.reply_rc = zlink_router_reply (router, source_rid, request_seq, &reply_part, 1);
            result.reply_errno = zlink_errno ();
        } else {
            zlink_msg_close (&reply_part);
        }
        if (parts)
            zlink_multipart_close (parts, part_count);
        return result;
    });

    reply_probe_t reply_probe;
    reply_probe.progress_handle = dealer;
    reply_probe.progress_handle = dealer;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_dealer_request (dealer, &request_part, 1, &capture_reply, &reply_probe, 0, 3000));
    TEST_ASSERT_TRUE (wait_for_reply (&reply_probe));
    const router_recv_probe_t router_result = router_done.get ();

    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, router_result.recv_rc);
    TEST_ASSERT_TRUE (router_result.source_rid_present);
    TEST_ASSERT_TRUE (router_result.request_seq != 0);
    TEST_ASSERT_EQUAL_UINT64 (1, router_result.part_count);
    TEST_ASSERT_EQUAL_STRING_LEN ("dealer-recv", router_result.source_rid.c_str (),
                                  router_result.source_rid.size ());
    TEST_ASSERT_EQUAL_STRING_LEN ("dealer-request-recv", router_result.payload.c_str (),
                                  router_result.payload.size ());
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, router_result.reply_rc);

    {
        std::lock_guard<std::mutex> lock (reply_probe.mutex);
        TEST_ASSERT_TRUE (reply_probe.done);
        TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, reply_probe.result);
        TEST_ASSERT_EQUAL_UINT64 (1, reply_probe.part_count);
        TEST_ASSERT_EQUAL_STRING_LEN ("router-reply-recv", reply_probe.payload.c_str (),
                                      reply_probe.payload.size ());
    }

    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
}

void test_raw_dealer_message_is_not_promoted_to_router_request ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer, "dealer-raw", 10));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (router, "inproc://zmp-router-recv-surface-raw"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (dealer, "inproc://zmp-router-recv-surface-raw"));
    msleep (SETTLE_TIME);

    zlink_msg_t raw;
    init_string_part (&raw, "ordinary-data");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (dealer, &raw, ZLINK_SEND_FLAGS_NONE,
                       ZLINK_PART_FINAL));

    const zlink_routing_id_t *source_rid = NULL;
    uint64_t request_seq = UINT64_MAX;
    zlink_msg_t *parts = NULL;
    size_t part_count = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_router_recv (router, &source_rid, &request_seq, &parts,
                         &part_count, ZLINK_RECV_FLAGS_NONE));

    TEST_ASSERT_NOT_NULL (source_rid);
    TEST_ASSERT_EQUAL_UINT64 (0, request_seq);
    TEST_ASSERT_EQUAL_UINT64 (1, part_count);
    TEST_ASSERT_EQUAL_STRING_LEN (
      "dealer-raw", reinterpret_cast<const char *> (source_rid->data),
      source_rid->size);
    TEST_ASSERT_EQUAL_STRING ("ordinary-data",
                              msg_to_string (&parts[0]).c_str ());

    unsigned char retained_kind = 0xff;
    uint64_t retained_sequence = UINT64_MAX;
    TEST_ASSERT_FALSE (
      reinterpret_cast<zlink::msg_t *> (&parts[0])
        ->get_request_reply_metadata (&retained_kind, &retained_sequence));
    zlink_multipart_close (parts, part_count);

    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
}

void test_router_raw_receive_surfaces_reject_metadata_after_first_part ()
{
    for (int aggregate = 0; aggregate <= 1; ++aggregate) {
        void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
        void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
        TEST_ASSERT_NOT_NULL (router);
        TEST_ASSERT_NOT_NULL (dealer);

        char endpoint[96];
        snprintf (endpoint, sizeof (endpoint),
                  "inproc://zmp-router-later-kind-%d", aggregate);
        char routing_id[32];
        snprintf (routing_id, sizeof (routing_id), "router-later-%d",
                  aggregate);
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_set_routing_id (dealer, routing_id, strlen (routing_id)));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (router, endpoint));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer, endpoint));
        msleep (SETTLE_TIME);

        zlink_msg_t first;
        init_string_part (&first, "ordinary-first");
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK,
          zlink_send_part (dealer, &first, ZLINK_SEND_FLAGS_NONE,
                           ZLINK_PART_MORE));
        zlink_msg_t later;
        init_string_part (&later, "metadata-later");
        TEST_ASSERT_SUCCESS_ERRNO (
          reinterpret_cast<zlink::msg_t *> (&later)
            ->set_request_reply_metadata (
              zlink::request_reply::reply_type, 700 + aggregate));
        TEST_ASSERT_EQUAL_INT (
          ZLINK_SUBMIT_OK,
          zlink_send_part (dealer, &later, ZLINK_SEND_FLAGS_NONE,
                           ZLINK_PART_FINAL));

        errno = 0;
        int observed_errno = 0;
        if (aggregate) {
            const zlink_routing_id_t *source_rid = NULL;
            uint64_t request_seq = UINT64_MAX;
            zlink_msg_t *parts = NULL;
            size_t part_count = 0;
            TEST_ASSERT_EQUAL_INT (
              ZLINK_RECV_INTERNAL_ERROR,
              zlink_router_recv (router, &source_rid, &request_seq, &parts,
                                 &part_count, ZLINK_RECV_FLAGS_NONE));
            observed_errno = errno;
            TEST_ASSERT_NULL (parts);
            TEST_ASSERT_EQUAL_UINT64 (0, part_count);
        } else {
            const zlink_routing_id_t *source_rid = NULL;
            uint64_t request_seq = UINT64_MAX;
            zlink_msg_t part;
            TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&part));
            zlink_part_flag_t has_more = ZLINK_PART_FINAL;
            TEST_ASSERT_EQUAL_INT (
              ZLINK_RECV_INTERNAL_ERROR,
              zlink_router_recv_part (
                router, &source_rid, &request_seq, &part, &has_more,
                ZLINK_RECV_FLAGS_NONE));
            observed_errno = errno;
            TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&part));
            TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&part));
        }
        TEST_ASSERT_EQUAL_INT (EPROTO, observed_errno);

        test_context_socket_close_zero_linger (dealer);
        test_context_socket_close_zero_linger (router);
    }
}

void test_dealer_request_is_visible_through_router_recv ()
{
    assert_dealer_request_visible_through_router_recv (true);
}

void test_dealer_request_is_visible_through_router_recv_without_priming ()
{
    assert_dealer_request_visible_through_router_recv (false);
}

void test_dealer_request_is_visible_through_nonblocking_router_recv_polling ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer, "dealer-poll", 11));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (router, "inproc://zmp-router-recv-surface-polling"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer, "inproc://zmp-router-recv-surface-polling"));
    msleep (SETTLE_TIME);

    zlink_msg_t request_part;
    zlink_msg_init (&request_part);
    init_string_part (&request_part, "dealer-request-poll");

    reply_probe_t reply_probe;
    reply_probe.progress_handle = dealer;
    reply_probe.progress_handle = dealer;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_dealer_request (dealer, &request_part, 1, &capture_reply, &reply_probe, 0, 3000));

    const zlink_routing_id_t *source_rid = NULL;
    uint64_t request_seq = 0;
    zlink_msg_t *parts = NULL;
    size_t part_count = 0;
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now () + std::chrono::milliseconds (SETTLE_TIME * 20);
    while (true) {
        const zlink_recv_result_t recv_rc = zlink_router_recv (
          router, &source_rid, &request_seq, &parts, &part_count, ZLINK_DONTWAIT);
        if (recv_rc == ZLINK_RECV_OK)
            break;

        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA, recv_rc);
        TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
        TEST_ASSERT_TRUE (std::chrono::steady_clock::now () < deadline);
        msleep (SETTLE_TIME);
    }

    TEST_ASSERT_NOT_NULL (source_rid);
    TEST_ASSERT_TRUE (request_seq != 0);
    TEST_ASSERT_EQUAL_UINT64 (1, part_count);
    TEST_ASSERT_EQUAL_STRING_LEN ("dealer-poll", reinterpret_cast<const char *> (source_rid->data),
                                  source_rid->size);
    const std::string request_payload = msg_to_string (&parts[0]);
    TEST_ASSERT_EQUAL_STRING_LEN ("dealer-request-poll", request_payload.c_str (),
                                  request_payload.size ());
    zlink_multipart_close (parts, part_count);

    zlink_msg_t reply_part;
    zlink_msg_init (&reply_part);
    init_string_part (&reply_part, "router-reply-poll");
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_router_reply (router, source_rid, request_seq, &reply_part, 1));

    TEST_ASSERT_TRUE (wait_for_reply (&reply_probe));
    {
        std::lock_guard<std::mutex> lock (reply_probe.mutex);
        TEST_ASSERT_TRUE (reply_probe.done);
        TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, reply_probe.result);
        TEST_ASSERT_EQUAL_UINT64 (1, reply_probe.part_count);
        TEST_ASSERT_EQUAL_STRING_LEN ("router-reply-poll", reply_probe.payload.c_str (),
                                      reply_probe.payload.size ());
    }

    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
}

void test_dealer_request_is_visible_through_first_blocking_router_recv ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer, "dealer-block", 12));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (router, "inproc://zmp-router-recv-surface-blocking"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer, "inproc://zmp-router-recv-surface-blocking"));
    msleep (SETTLE_TIME);

    zlink_msg_t request_part;
    zlink_msg_init (&request_part);
    init_string_part (&request_part, "dealer-request-block");

    reply_probe_t reply_probe;
    reply_probe.progress_handle = dealer;
    reply_probe.progress_handle = dealer;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_dealer_request (dealer, &request_part, 1, &capture_reply, &reply_probe, 0, 3000));

    const zlink_routing_id_t *source_rid = NULL;
    uint64_t request_seq = 0;
    zlink_msg_t *parts = NULL;
    size_t part_count = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_router_recv (router, &source_rid,
                                                  &request_seq, &parts, &part_count, 0));

    TEST_ASSERT_NOT_NULL (source_rid);
    TEST_ASSERT_TRUE (request_seq != 0);
    TEST_ASSERT_EQUAL_UINT64 (1, part_count);
    TEST_ASSERT_EQUAL_STRING_LEN ("dealer-block", reinterpret_cast<const char *> (source_rid->data),
                                  source_rid->size);
    const std::string request_payload = msg_to_string (&parts[0]);
    TEST_ASSERT_EQUAL_STRING_LEN ("dealer-request-block", request_payload.c_str (),
                                  request_payload.size ());
    zlink_multipart_close (parts, part_count);

    zlink_msg_t reply_part;
    zlink_msg_init (&reply_part);
    init_string_part (&reply_part, "router-reply-block");
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_router_reply (router, source_rid, request_seq, &reply_part, 1));

    TEST_ASSERT_TRUE (wait_for_reply (&reply_probe));
    {
        std::lock_guard<std::mutex> lock (reply_probe.mutex);
        TEST_ASSERT_TRUE (reply_probe.done);
        TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, reply_probe.result);
        TEST_ASSERT_EQUAL_UINT64 (1, reply_probe.part_count);
        TEST_ASSERT_EQUAL_STRING_LEN ("router-reply-block", reply_probe.payload.c_str (),
                                      reply_probe.payload.size ());
    }

    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
}

void test_dealer_request_is_visible_through_router_recv_over_tcp ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer);

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (router, endpoint, sizeof (endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer, "dealer-recv-tcp", 15));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer, endpoint));
    msleep (SETTLE_TIME * 5);

    zlink_msg_t request_part;
    zlink_msg_init (&request_part);
    init_string_part (&request_part, "dealer-request-recv-tcp");

    reply_probe_t reply_probe;
    reply_probe.progress_handle = dealer;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_dealer_request (dealer, &request_part, 1, &capture_reply,
                                                     &reply_probe, ZLINK_SEND_FLAGS_NONE, 5000));

    const zlink_routing_id_t *source_rid = NULL;
    uint64_t request_seq = 0;
    zlink_msg_t *parts = NULL;
    size_t part_count = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_router_recv (router, &source_rid,
                                                  &request_seq, &parts, &part_count, 0));

    TEST_ASSERT_NOT_NULL (source_rid);
    TEST_ASSERT_TRUE (request_seq != 0);
    TEST_ASSERT_EQUAL_UINT64 (1, part_count);
    TEST_ASSERT_EQUAL_STRING_LEN (
      "dealer-recv-tcp", reinterpret_cast<const char *> (source_rid->data), source_rid->size);
    const std::string request_payload = msg_to_string (&parts[0]);
    TEST_ASSERT_EQUAL_STRING_LEN ("dealer-request-recv-tcp", request_payload.c_str (),
                                  request_payload.size ());
    zlink_multipart_close (parts, part_count);

    zlink_msg_t reply_part;
    zlink_msg_init (&reply_part);
    init_string_part (&reply_part, "router-reply-recv-tcp");
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_router_reply (router, source_rid, request_seq, &reply_part, 1));

    TEST_ASSERT_TRUE (wait_for_reply (&reply_probe));
    {
        std::lock_guard<std::mutex> lock (reply_probe.mutex);
        TEST_ASSERT_TRUE (reply_probe.done);
        TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, reply_probe.result);
        TEST_ASSERT_EQUAL_UINT64 (1, reply_probe.part_count);
        TEST_ASSERT_EQUAL_STRING_LEN ("router-reply-recv-tcp", reply_probe.payload.c_str (),
                                      reply_probe.payload.size ());
    }

    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
}

void test_router_recv_part_v2_preserves_pair_identity_across_multipart ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer, "dealer-v2", 9));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (router, "inproc://zmp-router-recv-v2-multipart"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (dealer, "inproc://zmp-router-recv-v2-multipart"));
    msleep (SETTLE_TIME);

    const char *const expected_parts[] = {"v2-head", "v2-body", "v2-tail"};
    zlink_msg_t request_parts[3];
    for (size_t i = 0; i < 3; ++i) {
        zlink_msg_init (&request_parts[i]);
        init_string_part (&request_parts[i], expected_parts[i]);
    }
    reply_probe_t reply_probe;
    reply_probe.progress_handle = dealer;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_dealer_request (dealer, request_parts, 3, &capture_reply,
                            &reply_probe, ZLINK_SEND_FLAGS_NONE, 3000));

    uint64_t record_request_seq = 0;
    uint64_t record_pair_id = 0;
    uint64_t record_pair_generation = 0;
    zlink_routing_id_t reply_rid;
    memset (&reply_rid, 0, sizeof (reply_rid));
    for (size_t i = 0; i < 3; ++i) {
        const zlink_routing_id_t *source_rid = NULL;
        uint64_t request_seq = 0;
        uint64_t pair_id = 0;
        uint64_t pair_generation = 0;
        zlink_msg_t part;
        zlink_msg_init (&part);
        zlink_part_flag_t has_more = ZLINK_PART_FINAL;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_RECV_OK,
          zlink_router_recv_part_v2 (
            router, &source_rid, &request_seq, &pair_id, &pair_generation,
            &part, &has_more, ZLINK_RECV_FLAGS_NONE));
        TEST_ASSERT_NOT_NULL (source_rid);
        TEST_ASSERT_TRUE (request_seq != 0);
        TEST_ASSERT_TRUE (pair_id != 0);
        TEST_ASSERT_TRUE (pair_generation != 0);
        if (i == 0) {
            record_request_seq = request_seq;
            record_pair_id = pair_id;
            record_pair_generation = pair_generation;
            reply_rid = *source_rid;
        } else {
            TEST_ASSERT_EQUAL_UINT64 (record_request_seq, request_seq);
            TEST_ASSERT_EQUAL_UINT64 (record_pair_id, pair_id);
            TEST_ASSERT_EQUAL_UINT64 (record_pair_generation,
                                      pair_generation);
        }
        TEST_ASSERT_EQUAL_STRING_LEN (
          "dealer-v2", reinterpret_cast<const char *> (source_rid->data),
          source_rid->size);
        TEST_ASSERT_EQUAL_STRING (expected_parts[i],
                                  msg_to_string (&part).c_str ());
        TEST_ASSERT_EQUAL_INT (i + 1 == 3 ? ZLINK_PART_FINAL
                                          : ZLINK_PART_MORE,
                               has_more);
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&part));
    }

    zlink_msg_t reply;
    init_string_part (&reply, "v2-reply");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_router_reply (router, &reply_rid, record_request_seq, &reply, 1));
    TEST_ASSERT_TRUE (wait_for_reply (&reply_probe));
    {
        std::lock_guard<std::mutex> lock (reply_probe.mutex);
        TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, reply_probe.result);
        TEST_ASSERT_EQUAL_STRING ("v2-reply", reply_probe.payload.c_str ());
    }

    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
}

void test_router_recv_part_v2_snapshots_pair_before_source_pin_failure ()
{
    for (size_t part_count = 1; part_count <= 3; part_count += 2) {
        void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
        void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
        TEST_ASSERT_NOT_NULL (router);
        TEST_ASSERT_NOT_NULL (dealer);

        char endpoint[96];
        snprintf (endpoint, sizeof (endpoint),
                  "inproc://zmp-router-v2-pin-failure-%zu", part_count);
        char routing_id[32];
        snprintf (routing_id, sizeof (routing_id), "pin-failure-%zu",
                  part_count);
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_set_routing_id (dealer, routing_id, strlen (routing_id)));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (router, endpoint));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer, endpoint));
        msleep (SETTLE_TIME);

        for (size_t i = 0; i < part_count; ++i) {
            char payload[32];
            snprintf (payload, sizeof (payload), "ordinary-%zu", i);
            zlink_msg_t part;
            init_string_part (&part, payload);
            TEST_ASSERT_EQUAL_INT (
              ZLINK_SUBMIT_OK,
              zlink_send_part (
                dealer, &part, ZLINK_SEND_FLAGS_NONE,
                i + 1 == part_count ? ZLINK_PART_FINAL : ZLINK_PART_MORE));
        }

        socket_handle_t router_handle = as_socket_handle (router);
        TEST_ASSERT_NOT_NULL (router_handle.socket);
        router_handle.socket->test_fail_next_recv_pipe_pin ();

        uint64_t record_pair_id = 0;
        uint64_t record_pair_generation = 0;
        for (size_t i = 0; i < part_count; ++i) {
            const zlink_routing_id_t *source_rid = NULL;
            uint64_t request_seq = UINT64_MAX;
            uint64_t pair_id = 0;
            uint64_t pair_generation = 0;
            zlink_msg_t part;
            TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&part));
            zlink_part_flag_t has_more = ZLINK_PART_FINAL;
            TEST_ASSERT_EQUAL_INT (
              ZLINK_RECV_OK,
              zlink_router_recv_part_v2 (
                router, &source_rid, &request_seq, &pair_id,
                &pair_generation, &part, &has_more,
                ZLINK_RECV_FLAGS_NONE));
            TEST_ASSERT_NOT_NULL (source_rid);
            TEST_ASSERT_EQUAL_UINT64 (0, request_seq);
            TEST_ASSERT_TRUE (pair_id != 0);
            TEST_ASSERT_TRUE (pair_generation != 0);
            if (i == 0) {
                record_pair_id = pair_id;
                record_pair_generation = pair_generation;
            } else {
                TEST_ASSERT_EQUAL_UINT64 (record_pair_id, pair_id);
                TEST_ASSERT_EQUAL_UINT64 (record_pair_generation,
                                          pair_generation);
            }
            char expected[32];
            snprintf (expected, sizeof (expected), "ordinary-%zu", i);
            TEST_ASSERT_EQUAL_STRING (expected, msg_to_string (&part).c_str ());
            TEST_ASSERT_EQUAL_INT (
              i + 1 == part_count ? ZLINK_PART_FINAL : ZLINK_PART_MORE,
              has_more);
            TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&part));
        }

        TEST_ASSERT_FALSE (router_handle.socket->has_request_reply_state ());
        test_context_socket_close_zero_linger (dealer);
        test_context_socket_close_zero_linger (router);
    }
}
}

int main ()
{
    UNITY_BEGIN ();

    setup_test_environment ();

    RUN_TEST (test_dealer_request_is_visible_through_router_recv);
    RUN_TEST (test_raw_dealer_message_is_not_promoted_to_router_request);
    RUN_TEST (
      test_router_raw_receive_surfaces_reject_metadata_after_first_part);
    RUN_TEST (test_dealer_request_is_visible_through_router_recv_without_priming);
    RUN_TEST (test_dealer_request_is_visible_through_nonblocking_router_recv_polling);
    RUN_TEST (test_dealer_request_is_visible_through_first_blocking_router_recv);
    RUN_TEST (test_dealer_request_is_visible_through_router_recv_over_tcp);
    RUN_TEST (test_router_recv_part_v2_preserves_pair_identity_across_multipart);
    RUN_TEST (
      test_router_recv_part_v2_snapshots_pair_before_source_pin_failure);

    const int rc = UNITY_END ();
    fflush (NULL);
    std::_Exit (rc);
}
