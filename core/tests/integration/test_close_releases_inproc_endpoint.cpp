/* SPDX-License-Identifier: MPL-2.0 */

// zlink_close releases every resource of the socket before ZLINK_CLOSE_OK
// returns (socket spec, zlink_close). A bound endpoint registration is such a
// resource: once close has returned, another socket in the same context must
// be able to bind the same endpoint. The release is the synchronous part of
// the public call that ends the bind, exactly as zlink_unbind already does it;
// it is not a deferred reaper step that the caller cannot observe.
//
// Public C API repro of the Java mesh node case (ZLinkJavaRawMeshNodeM6ATest):
// a ROUTER bound to an inproc endpoint with a connected ROUTER peer is closed,
// and a replacement ROUTER binds the same endpoint right away. Before the fix
// the bind failed with EADDRINUSE in most iterations under CPU load.

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

void *router (const char *routing_id_)
{
    void *const socket = test_context_socket (ZLINK_SOCKET_ROUTER);
    const int linger = 0;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (socket, ZLINK_OPT_LINGER, &linger, sizeof (linger)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_routing_id (socket, routing_id_, strlen (routing_id_)));
    return socket;
}

void *monitor (void *socket_)
{
    zlink_socket_monitor_open_options_t options;
    memset (&options, 0, sizeof (options));
    options.events = ZLINK_EVENT_CONNECTION_READY;
    void *const monitor = zlink_socket_monitor_open (socket_, &options);
    TEST_ASSERT_NOT_NULL (monitor);
    monitors.push_back (monitor);
    return monitor;
}

void close_monitor (void *monitor_)
{
    for (size_t i = 0; i != monitors.size (); ++i) {
        if (monitors[i] == monitor_) {
            monitors.erase (monitors.begin () + i);
            break;
        }
    }
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_monitor_close (&monitor_));
}

void await_connection_ready (void *monitor_)
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
            && (event.flags & ZLINK_MONITOR_EVENT_FLAG_CONNECTION_READY_EDGE))
            return;
    }
    TEST_FAIL_MESSAGE ("connection ready edge did not arrive");
}

// The replacement bind is the observable contract: it must succeed on the
// first call, without retry, once zlink_close has returned ZLINK_CLOSE_OK.
void bind_replacement (const std::string &endpoint_, int iteration_)
{
    void *const replacement = router ("peer");
    const int rc = zlink_bind (replacement, endpoint_.c_str ());
    if (rc != 0) {
        char message[256];
        snprintf (message, sizeof (message),
                  "iteration %d: bind of %s after close failed: %s (%d)",
                  iteration_, endpoint_.c_str (),
                  zlink_strerror (zlink_errno ()), zlink_errno ());
        TEST_FAIL_MESSAGE (message);
    }
    test_context_socket_close (replacement);
}

enum peer_state_t
{
    peer_connecting,
    peer_connection_ready
};

enum release_path_t
{
    release_by_close,
    release_by_unbind_then_close
};

// One close-then-rebind round on the given endpoint. bound_endpoint_ is the
// address passed to zlink_bind; the actual endpoint (a tcp wildcard port is
// resolved by bind) is what the peer connects to and the replacement rebinds.
void close_then_rebind (const char *bound_endpoint_,
                        peer_state_t peer_state_,
                        release_path_t release_path_,
                        int iteration_)
{
    void *const peer = router ("peer");
    char endpoint[256];
    test_bind (peer, bound_endpoint_, endpoint, sizeof (endpoint));

    void *const local = router ("local");
    void *local_monitor = NULL;
    if (peer_state_ == peer_connection_ready)
        local_monitor = monitor (local);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (local, endpoint));
    if (local_monitor) {
        await_connection_ready (local_monitor);
        close_monitor (local_monitor);
    }

    if (release_path_ == release_by_unbind_then_close)
        TEST_ASSERT_SUCCESS_ERRNO (zlink_unbind (peer, endpoint));
    test_context_socket_close (peer);

    bind_replacement (endpoint, iteration_);
    test_context_socket_close (local);
}

std::string inproc_endpoint (int iteration_)
{
    char endpoint[64];
    snprintf (endpoint, sizeof (endpoint), "inproc://close-rebind-%d",
              iteration_);
    return endpoint;
}
}

// The repro shape: the peer is closed while its connection may still be
// forming. Many iterations, because the defect was a race with the reaper.
void test_close_bound_router_releases_inproc_endpoint ()
{
    for (int i = 0; i != 300; ++i)
        close_then_rebind (inproc_endpoint (i).c_str (), peer_connecting,
                           release_by_close, i);
}

// The framework shape: the connection is ready (both lanes attached) when the
// bound peer is closed and a replacement binds the same endpoint.
void test_close_bound_router_with_ready_connection_releases_inproc_endpoint ()
{
    for (int i = 0; i != 50; ++i)
        close_then_rebind (inproc_endpoint (i).c_str (), peer_connection_ready,
                           release_by_close, i);
}

// zlink_unbind releases the registration before it returns; close of the
// already unbound socket must keep the endpoint available.
void test_unbind_then_close_releases_inproc_endpoint ()
{
    for (int i = 0; i != 10; ++i)
        close_then_rebind (inproc_endpoint (i).c_str (), peer_connecting,
                           release_by_unbind_then_close, i);
}

// Same contract for a transport listener: the resolved loopback endpoint of
// the closed ROUTER is bound again by the replacement.
void test_close_bound_router_releases_tcp_endpoint ()
{
    for (int i = 0; i != 30; ++i)
        close_then_rebind ("tcp://127.0.0.1:*", peer_connection_ready,
                           release_by_close, i);
}

int main ()
{
    setup_test_environment ();

    UNITY_BEGIN ();
    RUN_TEST (test_close_bound_router_releases_inproc_endpoint);
    RUN_TEST (
      test_close_bound_router_with_ready_connection_releases_inproc_endpoint);
    RUN_TEST (test_unbind_then_close_releases_inproc_endpoint);
    RUN_TEST (test_close_bound_router_releases_tcp_endpoint);
    return UNITY_END ();
}
