/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"
#include "../src/api/socket/socket_request_reply_internal.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <string>
#include <string.h>

SETUP_TEARDOWN_TESTCONTEXT

namespace
{
struct reply_probe_t
{
    reply_probe_t () :
        done (false), callback_count (0), result (ZLINK_REQUEST_PROTOCOL_ERROR), payload ()
    {
    }

    std::mutex mutex;
    std::condition_variable cv;
    bool done;
    int callback_count;
    zlink_request_result_t result;
    std::string payload;
};

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
        ++probe->callback_count;
        probe->result = result_;
        probe->payload = part_count_ > 0
                           ? std::string (static_cast<const char *> (zlink_msg_data (&parts_[0])),
                                          zlink_msg_size (&parts_[0]))
                           : std::string ();
    }
    probe->cv.notify_all ();
}

bool wait_for_reply (reply_probe_t *probe_, int timeout_ms_)
{
    std::unique_lock<std::mutex> lock (probe_->mutex);
    return probe_->cv.wait_for (lock, std::chrono::milliseconds (timeout_ms_),
                                [probe_] () { return probe_->done; });
}

bool wait_for_reply_with_router_progress (void *router_, reply_probe_t *probe_, int timeout_ms_)
{
    const auto deadline =
      std::chrono::steady_clock::now () + std::chrono::milliseconds (timeout_ms_);
    while (std::chrono::steady_clock::now () < deadline) {
        if (wait_for_reply (probe_, 10))
            return true;
        void *poller = zlink_poller_new ();
        if (poller) {
            if (zlink_poller_add (poller, router_, NULL, ZLINK_POLLCOMPLETION) == ZLINK_CONFIG_OK) {
                zlink_poller_event_t event;
                (void) zlink_poller_wait (poller, &event, 1, 0, NULL);
                (void) zlink_poller_remove (poller, router_);
            }
            (void) zlink_poller_destroy (&poller);
        }
    }

    return false;
}

void init_part (zlink_msg_t *part_, const char *text_)
{
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (part_, strlen (text_)));
    memcpy (zlink_msg_data (part_), text_, strlen (text_));
}

void set_router_id_and_connect_target (void *router_,
                                       const char *router_id_,
                                       const char *connect_target_id_)
{
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (router_, router_id_, strlen (router_id_)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_router_option (router_, ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID, connect_target_id_,
                               strlen (connect_target_id_)));
}

void wait_for_request_from_router (void *router_, const char *expected_payload_)
{
    const zlink_routing_id_t *source_rid = NULL;
    uint64_t request_seq = 0;
    zlink_msg_t *parts = NULL;
    size_t part_count = 0;
    const auto deadline = std::chrono::steady_clock::now () + std::chrono::milliseconds (3000);

    while (std::chrono::steady_clock::now () < deadline) {
        const zlink_recv_result_t rc =
          zlink_router_recv (router_, &source_rid, &request_seq, &parts,
                             &part_count, ZLINK_DONTWAIT);
        if (rc == ZLINK_RECV_OK) {
            TEST_ASSERT_NOT_NULL (source_rid);
            TEST_ASSERT_TRUE (request_seq != 0);
            TEST_ASSERT_EQUAL_UINT64 (1, part_count);
            TEST_ASSERT_EQUAL_UINT64 (strlen (expected_payload_), zlink_msg_size (&parts[0]));
            TEST_ASSERT_EQUAL_MEMORY (expected_payload_, zlink_msg_data (&parts[0]),
                                      strlen (expected_payload_));
            zlink_multipart_close (parts, part_count);
            return;
        }

        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA, rc);
        TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
        msleep (10);
    }

    TEST_FAIL_MESSAGE ("router request timed out");
}
}

void test_router_request_part_failure_discards_pending_sequence_and_allows_fresh_request ()
{
    void *client = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *server_a = test_context_socket (ZLINK_SOCKET_ROUTER);

    const int mandatory = 1;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_router_option (client, ZLINK_ROUTER_OPT_MANDATORY, &mandatory, sizeof (mandatory)));

    set_router_id_and_connect_target (client, "client", "srv-a");
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (server_a, "srv-a", 5));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (server_a, "inproc://helper-req-a"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, "inproc://helper-req-a"));

    msleep (SETTLE_TIME);

    zlink_routing_id_t peer_a;
    memset (&peer_a, 0, sizeof (peer_a));
    memcpy (peer_a.data, "srv-a", 5);
    peer_a.size = 5;

    reply_probe_t *failed_probe = new reply_probe_t ();
    zlink_msg_t first_part;
    init_part (&first_part, "multipart-first");
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_router_request_part (client, &peer_a, &first_part, static_cast<zlink_send_flags_t> (0),
                                 ZLINK_PART_MORE, 3000, &capture_reply, failed_probe));

    zlink_msg_t final_part;
    init_part (&final_part, "multipart-final");
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&final_part));
    const zlink_submit_result_t failed_rc =
      zlink_router_request_part (client, &peer_a, &final_part, static_cast<zlink_send_flags_t> (0),
                                 ZLINK_PART_FINAL, 3000, &capture_reply, failed_probe);
    TEST_ASSERT_TRUE (failed_rc != ZLINK_SUBMIT_OK);
    TEST_ASSERT_TRUE (zlink_errno () != 0);

    TEST_ASSERT_FALSE (wait_for_reply (failed_probe, 200));
    {
        std::lock_guard<std::mutex> lock (failed_probe->mutex);
        TEST_ASSERT_FALSE (failed_probe->done);
        TEST_ASSERT_EQUAL_INT (0, failed_probe->callback_count);
    }

    reply_probe_t *fresh_probe = new reply_probe_t ();
    zlink_msg_t fresh_part;
    init_part (&fresh_part, "fresh-request");
    TEST_ASSERT_SUCCESS_ERRNO (zlink_router_request (client, &peer_a, &fresh_part, 1,
                                                     &capture_reply, fresh_probe,
                                                     static_cast<zlink_send_flags_t> (0), 1));

    wait_for_request_from_router (server_a, "fresh-request");
    (void) wait_for_reply_with_router_progress (client, fresh_probe, 1000);
    {
        std::lock_guard<std::mutex> lock (fresh_probe->mutex);
        if (fresh_probe->done) {
            TEST_ASSERT_EQUAL_INT (1, fresh_probe->callback_count);
            TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_TIMED_OUT, fresh_probe->result);
        }
    }

    test_context_socket_close_zero_linger (server_a);
    test_context_socket_close_zero_linger (client);
}

int main (void)
{
    setup_test_environment (120);

    UNITY_BEGIN ();
    RUN_TEST (test_router_request_part_failure_discards_pending_sequence_and_allows_fresh_request);
    const int rc = UNITY_END ();
    fflush (NULL);
    std::_Exit (rc);
}
