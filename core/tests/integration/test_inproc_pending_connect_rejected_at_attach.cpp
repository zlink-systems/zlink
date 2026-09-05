/* SPDX-License-Identifier: MPL-2.0 */

// Connect-before-bind inproc connections are parked in the context registry
// and completed later by the binder (zlink_bind), by the connector's own
// close, or by an explicit zlink_disconnect. Completing one attaches the
// bind-side pipe half to the bind socket. That attach may retire the pipe at
// once: a REJECT ROUTER closes a duplicate routing id (socket spec section 4),
// and the internal PAIR helper that materializes an ownerless pending half
// accepts one pipe only. The registry must have staged both inproc routing-id
// preambles before either socket admits the pipe, exactly like the direct
// connect path does; writing the bind-side preamble after admission tripped
// `Assertion failed: written (pipe.cpp)` in send_routing_id.

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <chrono>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

// Every monitor opened by a case is closed here so a failed assertion cannot
// leave a monitor handle that blocks context termination.
namespace { std::vector<void *> monitors; }

void setUp () { setup_test_context (); }
void tearDown ()
{
    for (size_t i = 0; i != monitors.size (); ++i)
        (void) zlink_monitor_close (&monitors[i]);
    monitors.clear ();
    teardown_test_context ();
}

namespace
{
typedef std::chrono::steady_clock clock_type;
const int observation_timeout_ms = 3000;
const int request_timeout_ms = 1000;

void set_int (void *socket_, zlink_option_t option_, int value_)
{
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (socket_, option_, &value_, sizeof (value_)));
}

void set_probe (void *router_)
{
    const int probe = 1;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_router_option (
      router_, ZLINK_ROUTER_OPT_PROBE, &probe, sizeof (probe)));
}

void *router (const char *routing_id_, int reconnect_ivl_)
{
    void *const socket = test_context_socket (ZLINK_SOCKET_ROUTER);
    set_int (socket, ZLINK_OPT_LINGER, 0);
    set_int (socket, ZLINK_OPT_RECONNECT_IVL, reconnect_ivl_);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_routing_id (socket, routing_id_, strlen (routing_id_)));
    return socket;
}

void *monitor (void *socket_)
{
    zlink_socket_monitor_open_options_t options;
    memset (&options, 0, sizeof (options));
    options.events = ZLINK_EVENT_CONNECTION_READY | ZLINK_EVENT_DISCONNECTED
                     | ZLINK_EVENT_CLOSED;
    void *const monitor = zlink_socket_monitor_open (socket_, &options);
    TEST_ASSERT_NOT_NULL (monitor);
    monitors.push_back (monitor);
    return monitor;
}

// One receive pump pass of a ROUTER, as the Java mesh node performs between
// monitor reads: drains probe frames and the inproc routing-id preamble of a
// pipe whose peer closed it, and lets the socket process its own commands.
void pump (void *router_)
{
    for (;;) {
        zlink_msg_t msg;
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&msg));
        const zlink_routing_id_t *source = NULL;
        zlink_reply_token_t token = 0;
        zlink_part_flag_t more = ZLINK_PART_MORE;
        const zlink_recv_result_t rc = zlink_router_recv_part (
          router_, &source, &token, &msg, &more, ZLINK_RECV_FLAGS_DONTWAIT);
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&msg));
        if (rc == ZLINK_RECV_NO_DATA)
            return;
        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, rc);
    }
}

// Waits for the requested edge. Without pump_ no application call touches the
// socket; with pump_ the socket is drained like a framework receive loop.
uint64_t await_event (void *monitor_, uint64_t event_, uint64_t connection_,
                      void *pump_ = NULL)
{
    const clock_type::time_point deadline =
      clock_type::now () + std::chrono::milliseconds (observation_timeout_ms);
    while (clock_type::now () < deadline) {
        zlink_monitor_event_t event;
        memset (&event, 0, sizeof (event));
        const zlink_recv_result_t rc = zlink_socket_monitor_recv (
          monitor_, &event, ZLINK_RECV_FLAGS_DONTWAIT);
        if (rc == ZLINK_RECV_NO_DATA) {
            if (pump_)
                pump (pump_);
            msleep (1);
            continue;
        }
        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, rc);
        if (event.event != event_)
            continue;
        if (event_ == ZLINK_EVENT_CONNECTION_READY
            && !(event.flags & ZLINK_MONITOR_EVENT_FLAG_CONNECTION_READY_EDGE))
            continue;
        if (connection_ != 0 && event.connection_id != connection_)
            continue;
        TEST_ASSERT_NOT_EQUAL (0, event.connection_id);
        return event.connection_id;
    }
    char message[96];
    snprintf (message, sizeof (message),
              "monitor event %llu for connection %llu did not arrive",
              static_cast<unsigned long long> (event_),
              static_cast<unsigned long long> (connection_));
    TEST_FAIL_MESSAGE (message);
    return 0;
}

void round_trip (void *client_, const char *server_rid_, void *server_)
{
    zlink_routing_id_t target;
    memset (&target, 0, sizeof (target));
    target.size = static_cast<uint8_t> (strlen (server_rid_));
    memcpy (target.data, server_rid_, target.size);

    zlink_msg_t msg;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&msg, 4));
    memcpy (zlink_msg_data (&msg), "ping", 4);
    zlink_completion_id_t id = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_request_part (client_, &target, &msg, ZLINK_SEND_FLAGS_NONE,
                          ZLINK_PART_FINAL, request_timeout_ms, NULL, &id));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&msg));
    TEST_ASSERT_NOT_EQUAL (0, id);

    zlink_msg_t received;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&received));
    const zlink_routing_id_t *source = NULL;
    zlink_reply_token_t token = 0;
    zlink_part_flag_t more = ZLINK_PART_MORE;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_router_recv_part (server_, &source, &token, &received, &more,
                              ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_NOT_NULL (source);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, more);
    TEST_ASSERT_EQUAL_UINT64 (4, zlink_msg_size (&received));
    TEST_ASSERT_EQUAL_MEMORY ("ping", zlink_msg_data (&received), 4);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_reply_part (server_, source, token, &received, ZLINK_PART_FINAL));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&received));

    zlink_completion_t completion;
    memset (&completion, 0, sizeof (completion));
    completion.struct_size = sizeof (completion);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_completion_recv (client_, &completion, ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_REQUEST, completion.kind);
    TEST_ASSERT_EQUAL_UINT64 (id, completion.completion_id);
    TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, completion.request_result);
    zlink_completion_close (&completion);
}

// Two connect-before-bind attempts from one ROUTER are both parked. Closing
// (or explicitly disconnecting) the connector materializes them through one
// internal PAIR helper, which admits the first pipe and terminates the
// second at attach.
void run_two_pending_then_release (bool disconnect_)
{
    const char *const endpoint = disconnect_
                                   ? "inproc://pending-attach-disconnect"
                                   : "inproc://pending-attach-close";
    void *const client = router ("pending-client", 100);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint));
    if (disconnect_)
        TEST_ASSERT_SUCCESS_ERRNO (zlink_disconnect (client, endpoint));
    test_context_socket_close_zero_linger (client);
}

void test_two_pending_connects_then_close ()
{
    run_two_pending_then_release (false);
}

void test_two_pending_connects_then_disconnect ()
{
    run_two_pending_then_release (true);
}

// Two connectors park connections that carry the same routing id. The
// REJECT binder admits the first one at bind and closes the duplicate at once
// (section 4). The admitted pair carries requests; the rejected connector
// observes only its own pair's termination edge through its monitor.
void test_two_pending_same_rid_then_reject_bind ()
{
    const char *const endpoint = "inproc://pending-attach-reject";
    void *const admitted = router ("pending-client", -1);
    void *const duplicate = router ("pending-client", -1);
    void *const admitted_monitor = monitor (admitted);
    void *const duplicate_monitor = monitor (duplicate);
    set_int (admitted, ZLINK_OPT_RCVTIMEO, observation_timeout_ms);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (admitted, endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (duplicate, endpoint));

    void *const server = router ("pending-server", 100);
    set_int (server, ZLINK_OPT_RID_DUPLICATE_POLICY, ZLINK_RID_DUPLICATE_REJECT);
    set_int (server, ZLINK_OPT_RCVTIMEO, observation_timeout_ms);
    void *const server_monitor = monitor (server);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (server, endpoint));

    const uint64_t server_ready =
      await_event (server_monitor, ZLINK_EVENT_CONNECTION_READY, 0);
    const uint64_t admitted_ready =
      await_event (admitted_monitor, ZLINK_EVENT_CONNECTION_READY, 0);
    round_trip (admitted, "pending-server", server);
    // The duplicate is closed without a wire reason; its connector sees only
    // the pair end (READY marks transport establishment, not admission). The
    // rejected pipe still holds the binder's routing-id preamble, so the
    // connector's receive loop drains it before the termination completes.
    const uint64_t rejected = await_event (
      duplicate_monitor, ZLINK_EVENT_DISCONNECTED, 0, duplicate);
    round_trip (admitted, "pending-server", server);
    printf ("pending_reject server_ready=%llu admitted_ready=%llu rejected=%llu\n",
            static_cast<unsigned long long> (server_ready),
            static_cast<unsigned long long> (admitted_ready),
            static_cast<unsigned long long> (rejected));

    test_context_socket_close_zero_linger (duplicate);
    test_context_socket_close_zero_linger (admitted);
    test_context_socket_close_zero_linger (server);
}

// The Java framework sequence that surfaced the abort
// (ZLinkJavaRawMeshNodeM6ATest.observedInprocCloseDoesNotFenceDescriptorReplacement):
// two probe ROUTERs bound on inproc, local connects peer, peer closes, a
// replacement peer binds the same endpoint with the same routing id, local
// connects again without disconnecting, then everything closes. Every pipe
// the REJECT replacement closes re-arms the connector's inproc intent, so
// several attempts are parked once the replacement unbinds and the connector
// close materializes them all through one PAIR helper.
void test_mesh_peer_replacement_sequence ()
{
    const char *const local_endpoint = "inproc://pending-attach-mesh-local";
    const char *const peer_endpoint = "inproc://pending-attach-mesh-peer";
    void *const local = router ("mesh-local", 100);
    set_probe (local);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (local, local_endpoint));
    void *const local_monitor = monitor (local);

    void *peer = router ("mesh-peer", 100);
    set_probe (peer);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (peer, peer_endpoint));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (local, peer_endpoint));
    const uint64_t first =
      await_event (local_monitor, ZLINK_EVENT_CONNECTION_READY, 0);
    test_context_socket_close_zero_linger (peer);
    await_event (local_monitor, ZLINK_EVENT_DISCONNECTED, first);

    void *const replacement = router ("mesh-peer", 100);
    set_probe (replacement);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (replacement, peer_endpoint));
    // zlink_connect first processes the re-armed intent (admitted by the
    // replacement), then this explicit attempt duplicates "mesh-local" and is
    // rejected. At least two attempts therefore exist when the replacement
    // closes, and each termination re-arms an intent that now parks.
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (local, peer_endpoint));
    const uint64_t second =
      await_event (local_monitor, ZLINK_EVENT_CONNECTION_READY, 0);
    TEST_ASSERT_NOT_EQUAL (first, second);
    test_context_socket_close_zero_linger (replacement);

    // Pump local like the Java receive loop until two distinct attempts have
    // reported their termination, so their re-armed intents are parked.
    uint64_t closed[2] = {0, 0};
    size_t closed_count = 0;
    const clock_type::time_point deadline =
      clock_type::now () + std::chrono::milliseconds (observation_timeout_ms);
    while (closed_count != 2 && clock_type::now () < deadline) {
        zlink_monitor_event_t event;
        memset (&event, 0, sizeof (event));
        const zlink_recv_result_t rc = zlink_socket_monitor_recv (
          local_monitor, &event, ZLINK_RECV_FLAGS_DONTWAIT);
        if (rc == ZLINK_RECV_NO_DATA) {
            pump (local);
            msleep (1);
            continue;
        }
        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, rc);
        if (event.event != ZLINK_EVENT_DISCONNECTED
            || event.connection_id == first
            || (closed_count == 1 && event.connection_id == closed[0]))
            continue;
        closed[closed_count++] = event.connection_id;
    }
    TEST_ASSERT_EQUAL_UINT (2, closed_count);
    pump (local);
    printf ("mesh_sequence first=%llu second=%llu closed=%llu,%llu\n",
            static_cast<unsigned long long> (first),
            static_cast<unsigned long long> (second),
            static_cast<unsigned long long> (closed[0]),
            static_cast<unsigned long long> (closed[1]));

    test_context_socket_close_zero_linger (local);
}
}

int main ()
{
    setup_test_environment ();
    UNITY_BEGIN ();
#define RUN_SELECTED(test_) \
    do { \
        const char *selected = getenv ("ZLINK_TEST_CASE"); \
        if (!selected || !*selected || strcmp (selected, #test_) == 0) \
            RUN_TEST (test_); \
    } while (false)
    RUN_SELECTED (test_two_pending_connects_then_close);
    RUN_SELECTED (test_two_pending_connects_then_disconnect);
    RUN_SELECTED (test_two_pending_same_rid_then_reject_bind);
    RUN_SELECTED (test_mesh_peer_replacement_sequence);
#undef RUN_SELECTED
    return UNITY_END ();
}
