/* SPDX-License-Identifier: MPL-2.0 */
#include "testutil.hpp"
#include "testutil_unity.hpp"
#include "api/socket/socket_api_internal.hpp"
#include "sockets/common/socket_base.hpp"
#include "core/ctx.hpp"
#include "core/pipe.hpp"
#include <vector>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

namespace zlink {
class session_termination_test_access_t {
  public:
    static bool quiesce (socket_base_t *socket_)
    {
        if (!socket_->stop_async_mailbox_processing (false))
            return false;
        socket_->wait_async_quiesced (3000);
        return !socket_->lifecycle_coordinator ().is_async_mailbox_active ()
               && !socket_->lifecycle_coordinator ().is_async_quiesce_pending ();
    }
    static std::vector<pipe_t *> retain_pipes (socket_base_t *socket_)
    {
        socket_public_api_lock_scope_t turn (socket_->lifecycle_coordinator ());
        std::vector<pipe_t *> pipes;
        socket_->snapshot_attached_pipes (&pipes);
        for (size_t i = 0; i != pipes.size (); ++i)
            zlink_assert (pipes[i]->retain_lifetime_ref ());
        return pipes;
    }
};
}
namespace {
struct drain_gate_t {
    zlink::socket_base_t *peer;
    std::atomic<bool> entered;
    std::atomic<bool> released;
    drain_gate_t () : peer (NULL), entered (false), released (false) {}
};
void delay_peer (zlink::socket_base_t *socket_, void *arg_)
{
    drain_gate_t &gate = *static_cast<drain_gate_t *> (arg_);
    if (socket_ != gate.peer)
        return;
    gate.entered.store (true);
    while (!gate.released.load ())
        std::this_thread::yield ();
}
template<class Predicate> bool wait_until (Predicate predicate_)
{
    const auto end = std::chrono::steady_clock::now () + std::chrono::seconds (3);
    while (!predicate_ () && std::chrono::steady_clock::now () < end)
        std::this_thread::yield ();
    return predicate_ ();
}
}
void setUp () { setup_test_context (); }
void tearDown () { teardown_test_context (); }

void test_unbind_releases_endpoint_before_peer_ack ()
{
    void *bound = test_context_socket (ZLINK_SOCKET_ROUTER);
    void *peer = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (bound, "bound", 5));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (peer, "peer", 4));
    const int timeout = 3000;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (peer, ZLINK_OPT_RCVTIMEO, &timeout, sizeof timeout));
    const char *endpoint = "inproc://unbind-peer-async";
    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK, zlink_bind (bound, endpoint));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (peer, endpoint));
    zlink_routing_id_t peer_rid = {};
    TEST_ASSERT_SUCCESS_ERRNO (zlink_get_routing_id (peer, &peer_rid));
    zlink_msg_t part;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&part, 1));
    zlink_completion_id_t id = 0;
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK, zlink_request (bound, &peer_rid, &part, 1, ZLINK_SEND_FLAGS_NONE, 3000, NULL, &id));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&part));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&part));
    const zlink_routing_id_t *source = NULL;
    zlink_reply_token_t token = 0;
    size_t more = 0;
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, zlink_router_recv (peer, &source, &token, &part, 1, &more, ZLINK_RECV_FLAGS_NONE));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&part));
    const zlink_routing_id_t source_rid = *source;
    drain_gate_t gate;
    { socket_handle_t handle = as_socket_handle (peer); gate.peer = handle.socket; }
    std::vector<zlink::pipe_t *> pipes =
      zlink::session_termination_test_access_t::retain_pipes (gate.peer);
    TEST_ASSERT_EQUAL_UINT (2, pipes.size ());
    {
        socket_handle_t handle = as_socket_handle (bound);
        TEST_ASSERT_TRUE (zlink::session_termination_test_access_t::quiesce (handle.socket));
    }
    void *replacement = test_context_socket (ZLINK_SOCKET_ROUTER);
    zlink::test_set_async_mailbox_before_drain_hook (delay_peer, &gate);
    const auto start = std::chrono::steady_clock::now ();
    const int unbind_rc = zlink_unbind (bound, endpoint);
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds> (
      std::chrono::steady_clock::now () - start).count ();
    // Do not process the peer from the application: only its requested owner
    // can finish the two-lane handshake after the gate opens.
    const bool entered = wait_until ([&] { return gate.entered.load (); });
    const int rebind_rc = zlink_bind (replacement, endpoint);
    bool blocked_nonterminal = true;
    for (size_t i = 0; i != pipes.size (); ++i)
        blocked_nonterminal = blocked_nonterminal
                              && !pipes[i]->has_completed_termination ();
    zlink_completion_t completion = {};
    completion.struct_size = sizeof completion;
    const int completion_rc = zlink_completion_recv (
      bound, &completion, ZLINK_RECV_FLAGS_DONTWAIT);
    gate.released.store (true);
    const bool terminated = wait_until ([&] {
        for (size_t i = 0; i != pipes.size (); ++i)
            if (!pipes[i]->has_completed_termination ())
                return false;
        return true;
    });
    zlink::test_set_async_mailbox_before_drain_hook (NULL, NULL);
    for (size_t i = 0; i != pipes.size (); ++i)
        pipes[i]->release_lifetime_ref ();
    printf ("peer lanes terminated=%d\n", terminated);
    printf ("peer blocked: entered=%d unbind=%d elapsed=%lld us rebind=%d\n", entered, unbind_rc, static_cast<long long> (elapsed), rebind_rc);
    TEST_ASSERT_TRUE (entered);
    TEST_ASSERT_TRUE (blocked_nonterminal);
    TEST_ASSERT_TRUE (terminated);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, unbind_rc);
    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK, rebind_rc);
    TEST_ASSERT_LESS_THAN_INT (100000, elapsed);
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, completion_rc);
    TEST_ASSERT_EQUAL_INT (ZLINK_COMPLETION_REQUEST, completion.kind);
    TEST_ASSERT_EQUAL_UINT64 (id, completion.completion_id);
    TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_NOT_FOUND, completion.request_result);
    zlink_completion_close (&completion);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&part, 1));
    zlink_completion_id_t rejected_id = 0;
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_NOT_CONNECTED, zlink_request (
      peer, &source_rid, &part, 1, ZLINK_SEND_FLAGS_DONTWAIT, 3000, NULL, &rejected_id));
    TEST_ASSERT_EQUAL_INT (EHOSTUNREACH, zlink_errno ());
    TEST_ASSERT_EQUAL_UINT64 (0, rejected_id);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&part));
    test_context_socket_close (peer);
    test_context_socket_close (bound);
    test_context_socket_close (replacement);
    TEST_ASSERT_SUCCESS_ERRNO (static_cast<zlink::ctx_t *> (get_test_context ())
                                ->wait_for_socket_count_at_most (0, 3000));
}
int main (int argc, char **argv)
{
    setup_test_environment ();
    (void) argc;
    (void) argv;
    UNITY_BEGIN ();
    RUN_TEST (test_unbind_releases_endpoint_before_peer_ack);
    return UNITY_END ();
}
