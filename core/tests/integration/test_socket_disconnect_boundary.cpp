/* SPDX-License-Identifier: MPL-2.0 */
#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <chrono>
#include <stdio.h>
#include <string>
#include <string.h>

SETUP_TEARDOWN_TESTCONTEXT

namespace
{
typedef std::chrono::steady_clock clock_type;
const int request_timeout_ms = 1000;
const int observation_timeout_ms = 3000;

void set_int (void *socket_, zlink_option_t option_, int value_)
{
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (socket_, option_, &value_, sizeof (value_)));
}

void message (zlink_msg_t *msg_, char value_)
{
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (msg_, 1));
    memcpy (zlink_msg_data (msg_), &value_, 1);
}

zlink_submit_result_t submit (void *client_, char payload_,
                              zlink_completion_id_t *id_)
{
    zlink_msg_t msg;
    message (&msg, payload_);
    const zlink_submit_result_t rc = zlink_request_part (
      client_, NULL, &msg, ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL,
      request_timeout_ms, NULL, id_);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&msg));
    TEST_ASSERT_TRUE (rc == ZLINK_SUBMIT_OK || rc == ZLINK_SUBMIT_BACKPRESSURED);
    TEST_ASSERT_NOT_EQUAL (0, *id_);
    return rc;
}

zlink_completion_t completion (void *client_)
{
    zlink_completion_t result;
    memset (&result, 0, sizeof (result));
    result.struct_size = sizeof (result);
    const clock_type::time_point deadline =
      clock_type::now () + std::chrono::milliseconds (observation_timeout_ms);
    while (clock_type::now () < deadline) {
        const zlink_recv_result_t rc = zlink_completion_recv (
          client_, &result, ZLINK_RECV_FLAGS_DONTWAIT);
        if (rc == ZLINK_RECV_OK)
            return result;
        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA, rc);
        msleep (1);
    }
    TEST_FAIL_MESSAGE ("completion did not terminate");
    return result;
}

bool receive_request (void *server_, char expected_, bool reply_)
{
    zlink_msg_t msg;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&msg));
    const zlink_routing_id_t *source = NULL;
    zlink_reply_token_t token = 0;
    zlink_part_flag_t more = ZLINK_PART_MORE;
    const zlink_recv_result_t rc = zlink_router_recv_part (
      server_, &source, &token, &msg, &more, ZLINK_RECV_FLAGS_DONTWAIT);
    if (rc == ZLINK_RECV_NO_DATA) {
        zlink_msg_close (&msg);
        return false;
    }
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, rc);
    TEST_ASSERT_NOT_NULL (source);
    TEST_ASSERT_NOT_EQUAL (0, token);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, more);
    TEST_ASSERT_EQUAL_UINT64 (1, zlink_msg_size (&msg));
    TEST_ASSERT_EQUAL_MEMORY (&expected_, zlink_msg_data (&msg), 1);
    zlink_msg_close (&msg);
    if (reply_) {
        message (&msg, expected_);
        TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, zlink_reply_part (
          server_, source, token, &msg, ZLINK_PART_FINAL));
        zlink_msg_close (&msg);
    }
    return true;
}

void await_request (void *server_, char payload_, bool reply_)
{
    const clock_type::time_point deadline =
      clock_type::now () + std::chrono::milliseconds (observation_timeout_ms);
    while (clock_type::now () < deadline) {
        if (receive_request (server_, payload_, reply_))
            return;
        msleep (1);
    }
    TEST_FAIL_MESSAGE ("request not delivered");
}

uint64_t ready (void *monitor_, uint64_t old_id_)
{
    const clock_type::time_point deadline =
      clock_type::now () + std::chrono::milliseconds (observation_timeout_ms);
    while (clock_type::now () < deadline) {
        zlink_monitor_event_t event;
        memset (&event, 0, sizeof (event));
        const zlink_recv_result_t rc = zlink_socket_monitor_recv (
          monitor_, &event, ZLINK_RECV_FLAGS_DONTWAIT);
        if (rc == ZLINK_RECV_NO_DATA) {
            msleep (1);
            continue;
        }
        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, rc);
        if (event.event == ZLINK_EVENT_CONNECTION_READY
            && (event.flags & ZLINK_MONITOR_EVENT_FLAG_CONNECTION_READY_EDGE)
            && event.connection_id != old_id_) {
            TEST_ASSERT_NOT_EQUAL (0, event.connection_id);
            return event.connection_id;
        }
    }
    TEST_FAIL_MESSAGE ("READY requires no application poll");
    return 0;
}

void assert_reply (zlink_completion_t *result_, zlink_completion_id_t id_,
                    char value_)
{
    TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_REQUEST, result_->kind);
    TEST_ASSERT_EQUAL_UINT64 (id_, result_->completion_id);
    TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, result_->request_result);
    TEST_ASSERT_EQUAL_UINT64 (1, result_->reply_part_count);
    TEST_ASSERT_EQUAL_UINT64 (1, zlink_msg_size (&result_->reply_parts[0]));
    TEST_ASSERT_EQUAL_MEMORY (&value_, zlink_msg_data (&result_->reply_parts[0]), 1);
}

void run_boundary (const char *transport_, bool handover_)
{
    if (strcmp (transport_, "inproc") != 0 && !zlink_has (transport_)
        && strcmp (transport_, "tcp") != 0)
        TEST_IGNORE_MESSAGE ("transport unavailable");
    // socket spec section 4 (D-B112): REJECT closes the duplicate pipe without a
    // wire reason, so the connector sees a physical disconnect and an admitted
    // request keeps its timeout budget; every request must still end once.
    int immediate_ok = 0, backpressured = 0, reply_ok = 0, not_connected = 0;
    int timed_out = 0, replacement_disconnects = 0;
    for (int iteration = 0; iteration != 20; ++iteration) {
        void *server = test_context_socket (ZLINK_SOCKET_ROUTER);
        void *client = test_context_socket (ZLINK_SOCKET_DEALER);
        set_int (server, ZLINK_OPT_LINGER, 0);
        set_int (client, ZLINK_OPT_LINGER, 0);
        set_int (client, ZLINK_OPT_RECONNECT_IVL, 20);
        set_int (server, ZLINK_OPT_RID_DUPLICATE_POLICY,
                 handover_ ? ZLINK_RID_DUPLICATE_HANDOVER : ZLINK_RID_DUPLICATE_REJECT);
        TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (client, "boundary-client", 15));
        std::string endpoint;
        if (strcmp (transport_, "inproc") == 0)
            endpoint = "inproc://disconnect-boundary-" + std::to_string (iteration);
        else if (strcmp (transport_, "ipc") == 0)
            endpoint = "ipc://" + make_random_ipc_path ();
        else
            endpoint = std::string (transport_) + "://127.0.0.1:*";
        TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (server, endpoint.c_str ()));
        char actual[MAX_SOCKET_STRING];
        size_t size = sizeof (actual);
        TEST_ASSERT_SUCCESS_ERRNO (zlink_get_option (
          server, ZLINK_OPT_LAST_ENDPOINT, actual, &size));
        endpoint = actual;
        zlink_socket_monitor_open_options_t options;
        memset (&options, 0, sizeof (options));
        options.events = ZLINK_EVENT_CONNECTION_READY | ZLINK_EVENT_DISCONNECTED;
        void *monitor = zlink_socket_monitor_open (client, &options);
        TEST_ASSERT_NOT_NULL (monitor);
        TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint.c_str ()));
        const uint64_t old_id = ready (monitor, 0);
        zlink_completion_id_t id_a = 0;
        TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, submit (client, 'A', &id_a));
        await_request (server, 'A', true);
        zlink_completion_t result = completion (client);
        assert_reply (&result, id_a, 'A');
        zlink_completion_close (&result);

        zlink_completion_id_t id_c = 0;
        TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, submit (client, 'C', &id_c));
        await_request (server, 'C', false);
        TEST_ASSERT_SUCCESS_ERRNO (zlink_disconnect (client, endpoint.c_str ()));
        // No monitor read or application poll between removal and new submit.
        TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint.c_str ()));
        zlink_completion_id_t id_b = 0;
        zlink_submit_result_t submitted = submit (client, 'B', &id_b);
        immediate_ok += submitted == ZLINK_SUBMIT_OK;
        backpressured += submitted == ZLINK_SUBMIT_BACKPRESSURED;

        bool c_done = false, b_done = false, delivered = false;
        const clock_type::time_point deadline =
          clock_type::now () + std::chrono::milliseconds (observation_timeout_ms);
        while (clock_type::now () < deadline && (!c_done || !b_done)) {
            if (!delivered)
                delivered = receive_request (server, 'B', true);
            memset (&result, 0, sizeof (result));
            result.struct_size = sizeof (result);
            const zlink_recv_result_t rc = zlink_completion_recv (
              client, &result, ZLINK_RECV_FLAGS_DONTWAIT);
            if (rc == ZLINK_RECV_NO_DATA) {
                msleep (1);
                continue;
            }
            TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, rc);
            if (result.completion_id == id_c) {
                TEST_ASSERT_FALSE (c_done);
                TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_REQUEST, result.kind);
                TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_NOT_FOUND, result.request_result);
                c_done = true;
            } else {
                TEST_ASSERT_EQUAL_UINT64 (id_b, result.completion_id);
                if (result.kind == ZLINK_COMPLETION_WRITABLE) {
                    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_BACKPRESSURED, submitted);
                    TEST_ASSERT_EQUAL_INT (ZLINK_SEND_ADMITTED, result.send_result);
                    zlink_completion_close (&result);
                    // The public DONTWAIT contract consumes the payload and asks
                    // the caller to resubmit only after its WRITABLE completion.
                    submitted = submit (client, 'B', &id_b);
                    continue;
                }
                TEST_ASSERT_FALSE (b_done);
                TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_REQUEST, result.kind);
                if (result.request_result == ZLINK_REQUEST_OK) {
                    TEST_ASSERT_TRUE (delivered);
                    assert_reply (&result, id_b, 'B');
                    ++reply_ok;
                } else if (result.request_result == ZLINK_REQUEST_TIMED_OUT) {
                    ++timed_out;
                } else {
                    TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_NOT_CONNECTED, result.request_result);
                    ++not_connected;
                }
                b_done = true;
            }
            zlink_completion_close (&result);
        }
        if (!c_done || !b_done) {
            printf ("BOUNDARY_STALLED transport=%s policy=%s iteration=%d "
                    "submitted=%d c_done=%d b_done=%d delivered=%d\n",
                    transport_, handover_ ? "HANDOVER" : "REJECT", iteration,
                    static_cast<int> (submitted), c_done, b_done, delivered);
            TEST_ASSERT_SUCCESS_ERRNO (zlink_monitor_close (&monitor));
            test_context_socket_close_zero_linger (client);
            test_context_socket_close_zero_linger (server);
            TEST_ASSERT_TRUE (c_done);
            TEST_ASSERT_TRUE (b_done);
        }
        // Read the queued new READY only after requests have terminated. No
        // monitor consumption or zlink_poll call drives reconnection above.
        TEST_ASSERT_NOT_EQUAL (old_id, ready (monitor, old_id));
        for (;;) {
            zlink_monitor_event_t event;
            memset (&event, 0, sizeof (event));
            const zlink_recv_result_t rc = zlink_socket_monitor_recv (
              monitor, &event, ZLINK_RECV_FLAGS_DONTWAIT);
            if (rc == ZLINK_RECV_NO_DATA)
                break;
            TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, rc);
            if (event.event == ZLINK_EVENT_DISCONNECTED
                && event.connection_id != old_id)
                ++replacement_disconnects;
        }
        TEST_ASSERT_SUCCESS_ERRNO (zlink_monitor_close (&monitor));
        test_context_socket_close_zero_linger (client);
        test_context_socket_close_zero_linger (server);
    }
    printf ("BOUNDARY transport=%s policy=%s iterations=20 immediate_ok=%d "
            "backpressured=%d reply_ok=%d not_connected=%d timed_out=%d replacement_disconnects=%d c_not_found=20 new_ready=20\n",
            transport_, handover_ ? "HANDOVER" : "REJECT", immediate_ok,
            backpressured, reply_ok, not_connected, timed_out, replacement_disconnects);
    fflush (stdout);
    if (handover_) {
        TEST_ASSERT_EQUAL_INT_MESSAGE (0, timed_out, "B must not expire on a handover connection");
        TEST_ASSERT_EQUAL_INT (20, reply_ok);
    } else {
        // socket spec section 4 (D-B112): a rejected pipe closes without a wire
        // reason, so an admitted request keeps its timeout budget like any
        // transient disconnect; every B must still terminate exactly once.
        TEST_ASSERT_EQUAL_INT (20, reply_ok + timed_out + not_connected);
    }
}
}

#define BOUNDARY_TEST(transport, policy, handover) \
    void test_##transport##_##policy () { run_boundary (#transport, handover); }
BOUNDARY_TEST (tcp, handover, true)
BOUNDARY_TEST (tcp, reject, false)
BOUNDARY_TEST (ipc, handover, true)
BOUNDARY_TEST (ipc, reject, false)
BOUNDARY_TEST (inproc, handover, true)
BOUNDARY_TEST (inproc, reject, false)
BOUNDARY_TEST (ws, handover, true)
BOUNDARY_TEST (ws, reject, false)

int main ()
{
    setup_test_environment ();
    UNITY_BEGIN ();
#define RUN_BOUNDARY(name) \
    do { \
        const char *selected = getenv ("ZLINK_TEST_CASE"); \
        if (!selected || strcmp (selected, #name) == 0) \
            RUN_TEST (test_##name); \
    } while (0)
    RUN_BOUNDARY (tcp_handover);
    RUN_BOUNDARY (tcp_reject);
#if defined(ZLINK_HAVE_IPC) && !defined(ZLINK_HAVE_GNU)
    RUN_BOUNDARY (ipc_handover);
    RUN_BOUNDARY (ipc_reject);
#endif
    RUN_BOUNDARY (inproc_handover);
    RUN_BOUNDARY (inproc_reject);
    RUN_BOUNDARY (ws_handover);
    RUN_BOUNDARY (ws_reject);
    return UNITY_END ();
}
