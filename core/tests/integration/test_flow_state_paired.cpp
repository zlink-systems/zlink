/* SPDX-License-Identifier: MPL-2.0 */

//  Paired DEALER/ROUTER completion-lane flow state. The state frame, the
//  socket-wide local state and the remote-PAUSE send blocker are Core internal
//  in this step, so the tests drive the internal C++ surface directly.

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include "../../src/runtime/core/flow_state_frame.hpp"
#include "../../src/runtime/core/msg.hpp"
#include "../../src/runtime/core/pipe.hpp"
#include "../../src/runtime/sockets/common/socket_base.hpp"

#include <chrono>
#include <string.h>
#include <string>

SETUP_TEARDOWN_TESTCONTEXT

namespace
{
const int k_running = zlink::flow_state::receive_flow_running;
const int k_paused = zlink::flow_state::receive_flow_paused;

zlink::socket_base_t *as_socket (void *socket_)
{
    return static_cast<zlink::socket_base_t *> (socket_);
}

bool deadline_expired (const std::chrono::steady_clock::time_point &deadline_)
{
    return std::chrono::steady_clock::now () >= deadline_;
}

std::chrono::steady_clock::time_point deadline_in_ms (int ms_)
{
    return std::chrono::steady_clock::now () + std::chrono::milliseconds (ms_);
}

//  One connected DEALER (sender) and ROUTER (receiver) over TCP, with the
//  route already learned by the ROUTER.
struct paired_fixture_t
{
    paired_fixture_t () : dealer (NULL), router (NULL)
    {
        memset (endpoint, 0, sizeof (endpoint));
    }

    void setup (uint64_t dealer_sndhwm_ = 0, const char *inproc_name_ = NULL)
    {
        const int zero = 0;
        router = test_context_socket (ZLINK_SOCKET_ROUTER);
        TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
          router, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
        if (inproc_name_) {
            //  An inproc pair puts the peer's read cursor under the test's
            //  control: byte credit moves only when the ROUTER application
            //  actually dequeues, instead of when a session drains the pipe
            //  onto a socket.
            snprintf (endpoint, sizeof endpoint, "inproc://%s", inproc_name_);
            TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (router, endpoint));
        } else
            bind_loopback_ipv4 (router, endpoint, sizeof endpoint);

        dealer = test_context_socket (ZLINK_SOCKET_DEALER);
        TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
          dealer, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
        if (dealer_sndhwm_ != 0)
            TEST_ASSERT_SUCCESS_ERRNO (
              zlink_set_option (dealer, ZLINK_OPT_SNDHWM, &dealer_sndhwm_,
                                sizeof (dealer_sndhwm_)));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer, endpoint));

        //  One round trip so the ROUTER learns the route and both pairs are
        //  ready on both ends.
        send_string_expect_success (dealer, "hello", 0);
        char rid[256];
        const int rid_size = zlink_recv (router, rid, sizeof (rid), 0);
        TEST_ASSERT_GREATER_THAN_INT (0, rid_size);
        recv_string_expect_success (router, "hello", 0);
        peer_rid.assign (rid, static_cast<size_t> (rid_size));

        TEST_ASSERT_TRUE (resolve_dealer_target ());
    }

    bool resolve_dealer_target ()
    {
        zlink_routed_submit_target_t target;
        memset (&target, 0, sizeof (target));
        const std::chrono::steady_clock::time_point deadline =
          deadline_in_ms (2000);
        while (!deadline_expired (deadline)) {
            if (as_socket (dealer)->select_routed_submit_target (NULL, &target)
                  == 0
                && target.transport_pair_id != 0) {
                pair_id = target.transport_pair_id;
                pair_generation = target.transport_pair_generation;
                return true;
            }
            msleep (1);
        }
        return false;
    }

    void teardown ()
    {
        if (dealer)
            dealer = test_context_socket_close_zero_linger (dealer);
        if (router)
            router = test_context_socket_close_zero_linger (router);
    }

    //  Delivers one hand-built frame to the DEALER exactly as the completion
    //  lane would, then drains the socket mailbox so the pipe applies it.
    bool inject (uint8_t version_,
                 uint8_t state_,
                 uint64_t frame_pair_id_,
                 uint64_t frame_generation_,
                 uint64_t epoch_)
    {
        zlink::pipe_t *completion =
          as_socket (dealer)->completion_pipe_for_transport_pair (
            pair_id, pair_generation);
        TEST_ASSERT_NOT_NULL (completion);

        zlink::flow_state::frame_t frame;
        frame.version = zlink::flow_state::frame_protocol_version;
        frame.state = state_;
        frame.pair_id = frame_pair_id_;
        frame.generation = frame_generation_;
        frame.epoch = epoch_;

        zlink::msg_t msg;
        TEST_ASSERT_EQUAL_INT (0, msg.init ());
        TEST_ASSERT_EQUAL_INT (0, zlink::flow_state::init_frame (&msg, frame));
        static_cast<unsigned char *> (
          msg.data ())[zlink::flow_state::frame_name_size] = version_;
        const bool consumed =
          as_socket (dealer)->consume_receive_flow_state_frame (completion, msg);
        TEST_ASSERT_EQUAL_INT (0, msg.close ());
        return consumed;
    }

    bool wait_for_applied_pause (bool expected_)
    {
        const std::chrono::steady_clock::time_point deadline =
          deadline_in_ms (2000);
        while (!deadline_expired (deadline)) {
            (void) as_socket (dealer)->process_submit_commands ();
            if (as_socket (dealer)->application_pipe_remote_flow_paused (
                  pair_id, pair_generation)
                == expected_)
                return true;
            msleep (1);
        }
        return false;
    }

    void *dealer;
    void *router;
    char endpoint[MAX_SOCKET_STRING];
    std::string peer_rid;
    uint64_t pair_id;
    uint64_t pair_generation;
};

bool dealer_send_nonblocking (void *dealer_, const char *payload_, int flags_)
{
    const int rc = zlink_send (dealer_, payload_,
                               static_cast<int> (strlen (payload_)),
                               flags_ | ZLINK_DONTWAIT);
    return rc >= 0;
}

bool wait_for_send_success (void *dealer_, int budget_ms_)
{
    const std::chrono::steady_clock::time_point deadline =
      deadline_in_ms (budget_ms_);
    while (!deadline_expired (deadline)) {
        if (dealer_send_nonblocking (dealer_, "payload", 0))
            return true;
        msleep (1);
    }
    return false;
}

bool stays_blocked (void *dealer_, int window_ms_)
{
    const std::chrono::steady_clock::time_point deadline =
      deadline_in_ms (window_ms_);
    while (!deadline_expired (deadline)) {
        if (dealer_send_nonblocking (dealer_, "payload", 0))
            return false;
        TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
        msleep (1);
    }
    return true;
}

size_t drain_router (void *router_)
{
    size_t drained = 0;
    char buffer[4096];
    while (true) {
        const int rc = zlink_recv (router_, buffer, sizeof (buffer), ZLINK_DONTWAIT);
        if (rc < 0)
            break;
        ++drained;
    }
    return drained;
}

//  PAIR, the PUB/SUB family and STREAM have no completion lane. The internal
//  entry reports not-supported and their existing send behaviour is unchanged.
void test_unsupported_socket_types_report_not_supported ()
{
    const int types[] = {ZLINK_SOCKET_PAIR, ZLINK_SOCKET_PUB, ZLINK_SOCKET_SUB,
                         ZLINK_SOCKET_XPUB, ZLINK_SOCKET_XSUB,
                         ZLINK_SOCKET_STREAM};
    for (size_t i = 0; i < sizeof (types) / sizeof (types[0]); ++i) {
        void *socket = test_context_socket (types[i]);
        TEST_ASSERT_FALSE (
          zlink::socket_base_t::socket_type_supports_receive_flow_state (
            types[i]));
        TEST_ASSERT_EQUAL_INT (
          -1, as_socket (socket)->set_local_receive_flow_state (k_paused));
        TEST_ASSERT_EQUAL_INT (ENOTSUP, errno);
        TEST_ASSERT_EQUAL_INT (
          -1, as_socket (socket)->set_local_receive_flow_state (k_running));
        TEST_ASSERT_EQUAL_INT (ENOTSUP, errno);
        test_context_socket_close_zero_linger (socket);
    }

    //  The refused call leaves the existing send path untouched.
    const int zero = 0;
    char endpoint[MAX_SOCKET_STRING];
    void *binder = test_context_socket (ZLINK_SOCKET_PAIR);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (binder, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    bind_loopback_ipv4 (binder, endpoint, sizeof endpoint);
    void *connecter = test_context_socket (ZLINK_SOCKET_PAIR);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (connecter, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (connecter, endpoint));
    TEST_ASSERT_EQUAL_INT (
      -1, as_socket (binder)->set_local_receive_flow_state (k_paused));
    TEST_ASSERT_EQUAL_INT (ENOTSUP, errno);
    send_string_expect_success (connecter, "unchanged", 0);
    recv_string_expect_success (binder, "unchanged", 0);
    test_context_socket_close_zero_linger (connecter);
    test_context_socket_close_zero_linger (binder);
}

void test_invalid_state_is_rejected ()
{
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_EQUAL_INT (
      -1, as_socket (dealer)->set_local_receive_flow_state (2));
    TEST_ASSERT_EQUAL_INT (EINVAL, errno);
    TEST_ASSERT_EQUAL_INT (
      -1, as_socket (dealer)->set_local_receive_flow_state (-1));
    TEST_ASSERT_EQUAL_INT (EINVAL, errno);
    //  Repeating the current state succeeds and stays idempotent.
    TEST_ASSERT_EQUAL_INT (
      0, as_socket (dealer)->set_local_receive_flow_state (k_running));
    TEST_ASSERT_EQUAL_INT (
      0, as_socket (dealer)->set_local_receive_flow_state (k_running));
    TEST_ASSERT_EQUAL_INT (
      k_running, as_socket (dealer)->get_local_receive_flow_state ());
    TEST_ASSERT_EQUAL_INT (
      0, as_socket (dealer)->set_local_receive_flow_state (k_paused));
    TEST_ASSERT_EQUAL_INT (
      k_paused, as_socket (dealer)->get_local_receive_flow_state ());
    test_context_socket_close_zero_linger (dealer);
}

//  End to end over the real completion lane: the receiver's socket-wide state
//  reaches the sender and blocks it, and RUNNING releases it.
void test_remote_pause_blocks_sender_and_resume_releases_it ()
{
    paired_fixture_t fixture;
    fixture.setup ();

    TEST_ASSERT_EQUAL_INT (
      0, as_socket (fixture.router)->set_local_receive_flow_state (k_paused));
    TEST_ASSERT_TRUE (fixture.wait_for_applied_pause (true));
    TEST_ASSERT_TRUE (as_socket (fixture.dealer)->remote_receive_flow_paused (
      fixture.pair_id, fixture.pair_generation));
    TEST_ASSERT_TRUE (stays_blocked (fixture.dealer, 100));

    TEST_ASSERT_EQUAL_INT (
      0, as_socket (fixture.router)->set_local_receive_flow_state (k_running));
    TEST_ASSERT_TRUE (fixture.wait_for_applied_pause (false));
    TEST_ASSERT_TRUE (wait_for_send_success (fixture.dealer, 2000));

    fixture.teardown ();
}

//  Local byte HWM and remote PAUSE are independent causes: clearing either one
//  alone leaves the pipe unwritable.
void test_local_hwm_and_remote_pause_are_independent ()
{
    paired_fixture_t fixture;
    fixture.setup (2048);

    //  Fill the sender's byte HWM while the peer is still RUNNING.
    bool hwm_full = false;
    for (int i = 0; i < 10000 && !hwm_full; ++i) {
        if (!dealer_send_nonblocking (fixture.dealer, "payload", 0)) {
            TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
            hwm_full = true;
        }
    }
    TEST_ASSERT_TRUE (hwm_full);

    //  Add the remote-PAUSE cause on top of the HWM cause, then remove only
    //  the HWM cause by draining the receiver.
    TEST_ASSERT_TRUE (fixture.inject (
      zlink::flow_state::frame_protocol_version, k_paused, fixture.pair_id,
      fixture.pair_generation, 1));
    TEST_ASSERT_TRUE (fixture.wait_for_applied_pause (true));
    (void) drain_router (fixture.router);
    msleep (50);
    (void) drain_router (fixture.router);
    TEST_ASSERT_TRUE (stays_blocked (fixture.dealer, 150));

    //  Removing the remote cause as well makes the pipe writable again.
    TEST_ASSERT_TRUE (fixture.inject (
      zlink::flow_state::frame_protocol_version, k_running, fixture.pair_id,
      fixture.pair_generation, 2));
    TEST_ASSERT_TRUE (fixture.wait_for_applied_pause (false));
    TEST_ASSERT_TRUE (wait_for_send_success (fixture.dealer, 2000));

    //  Now the other order: fill the HWM again and clear only the remote
    //  cause. The HWM cause still holds the pipe.
    TEST_ASSERT_TRUE (fixture.inject (
      zlink::flow_state::frame_protocol_version, k_paused, fixture.pair_id,
      fixture.pair_generation, 3));
    TEST_ASSERT_TRUE (fixture.wait_for_applied_pause (true));
    TEST_ASSERT_TRUE (fixture.inject (
      zlink::flow_state::frame_protocol_version, k_running, fixture.pair_id,
      fixture.pair_generation, 4));
    TEST_ASSERT_TRUE (fixture.wait_for_applied_pause (false));
    TEST_ASSERT_TRUE (wait_for_send_success (fixture.dealer, 2000));

    fixture.teardown ();
}

//  A PAUSE that arrives inside a started multipart message does not break its
//  atomicity; it applies from the next message.
void test_pause_mid_multipart_preserves_atomicity ()
{
    paired_fixture_t fixture;
    fixture.setup ();

    TEST_ASSERT_TRUE (dealer_send_nonblocking (fixture.dealer, "part-one",
                                               ZLINK_SNDMORE));
    TEST_ASSERT_TRUE (fixture.inject (
      zlink::flow_state::frame_protocol_version, k_paused, fixture.pair_id,
      fixture.pair_generation, 1));
    TEST_ASSERT_TRUE (fixture.wait_for_applied_pause (true));

    //  The started message finishes.
    TEST_ASSERT_TRUE (dealer_send_nonblocking (fixture.dealer, "part-two", 0));
    //  The next message is blocked.
    TEST_ASSERT_TRUE (stays_blocked (fixture.dealer, 100));

    //  The receiver still observes the complete two-part message.
    char rid[256];
    const int rid_size = zlink_recv (fixture.router, rid, sizeof (rid), 0);
    TEST_ASSERT_GREATER_THAN_INT (0, rid_size);
    recv_string_expect_success (fixture.router, "part-one", 0);
    recv_string_expect_success (fixture.router, "part-two", 0);

    fixture.teardown ();
}

//  Duplicate, reversed, foreign-generation and unsupported-version frames are
//  consumed and ignored without changing the applied state.
void test_duplicate_and_stale_frames_are_ignored ()
{
    paired_fixture_t fixture;
    fixture.setup ();

    TEST_ASSERT_TRUE (fixture.inject (
      zlink::flow_state::frame_protocol_version, k_paused, fixture.pair_id,
      fixture.pair_generation, 5));
    TEST_ASSERT_TRUE (fixture.wait_for_applied_pause (true));

    //  Same epoch again: idempotent, still PAUSED.
    TEST_ASSERT_TRUE (fixture.inject (
      zlink::flow_state::frame_protocol_version, k_paused, fixture.pair_id,
      fixture.pair_generation, 5));
    //  Older epoch that would resume: ignored.
    TEST_ASSERT_TRUE (fixture.inject (
      zlink::flow_state::frame_protocol_version, k_running, fixture.pair_id,
      fixture.pair_generation, 4));
    //  Newer epoch on a previous connection generation: ignored.
    TEST_ASSERT_TRUE (fixture.inject (
      zlink::flow_state::frame_protocol_version, k_running, fixture.pair_id,
      fixture.pair_generation - 1, 99));
    //  Another pair's identity: ignored.
    TEST_ASSERT_TRUE (fixture.inject (
      zlink::flow_state::frame_protocol_version, k_running,
      fixture.pair_id ^ 0x5a5aULL, fixture.pair_generation, 100));
    //  Unsupported protocol version: consumed and rejected.
    TEST_ASSERT_TRUE (fixture.inject (99, k_running, fixture.pair_id,
                                      fixture.pair_generation, 101));

    (void) as_socket (fixture.dealer)->process_submit_commands ();
    TEST_ASSERT_TRUE (
      as_socket (fixture.dealer)->application_pipe_remote_flow_paused (
        fixture.pair_id, fixture.pair_generation));
    TEST_ASSERT_TRUE (stays_blocked (fixture.dealer, 100));

    //  A frame that really advances the epoch resumes the pipe.
    TEST_ASSERT_TRUE (fixture.inject (
      zlink::flow_state::frame_protocol_version, k_running, fixture.pair_id,
      fixture.pair_generation, 102));
    TEST_ASSERT_TRUE (fixture.wait_for_applied_pause (false));
    TEST_ASSERT_TRUE (wait_for_send_success (fixture.dealer, 2000));

    fixture.teardown ();
}

//  A pair that becomes ready after the local state was stored - a first
//  connection or a reconnect - is synchronised with that stored state.
void test_new_and_reconnected_pairs_receive_the_latest_state ()
{
    const int zero = 0;
    char endpoint[MAX_SOCKET_STRING];
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (router, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    bind_loopback_ipv4 (router, endpoint, sizeof endpoint);

    //  Stored before any pair exists.
    TEST_ASSERT_EQUAL_INT (
      0, as_socket (router)->set_local_receive_flow_state (k_paused));

    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (dealer, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer, endpoint));

    zlink_routed_submit_target_t target;
    memset (&target, 0, sizeof (target));
    bool paused_seen = false;
    const std::chrono::steady_clock::time_point deadline = deadline_in_ms (4000);
    while (!deadline_expired (deadline)) {
        if (as_socket (dealer)->select_routed_submit_target (NULL, &target) == 0
            && target.transport_pair_id != 0
            && as_socket (dealer)->application_pipe_remote_flow_paused (
                 target.transport_pair_id, target.transport_pair_generation)) {
            paused_seen = true;
            break;
        }
        //  An idle socket runs its own admission only when the application
        //  enters it. A real receiver is inside recv; drive the same edge here.
        (void) as_socket (router)->process_submit_commands ();
        msleep (1);
    }
    TEST_ASSERT_TRUE (paused_seen);
    const uint64_t first_pair_id = target.transport_pair_id;

    //  Reconnect: the replacement pair is synchronised with the same stored
    //  state without any further call.
    TEST_ASSERT_SUCCESS_ERRNO (zlink_disconnect (dealer, endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer, endpoint));

    bool reconnected_paused = false;
    const std::chrono::steady_clock::time_point reconnect_deadline =
      deadline_in_ms (4000);
    while (!deadline_expired (reconnect_deadline)) {
        memset (&target, 0, sizeof (target));
        if (as_socket (dealer)->select_routed_submit_target (NULL, &target) == 0
            && target.transport_pair_id != 0
            && target.transport_pair_id != first_pair_id
            && as_socket (dealer)->application_pipe_remote_flow_paused (
                 target.transport_pair_id, target.transport_pair_generation)) {
            reconnected_paused = true;
            break;
        }
        (void) as_socket (router)->process_submit_commands ();
        msleep (1);
    }
    TEST_ASSERT_TRUE (reconnected_paused);

    //  Releasing the stored state reaches the replacement pair too.
    TEST_ASSERT_EQUAL_INT (
      0, as_socket (router)->set_local_receive_flow_state (k_running));
    bool resumed = false;
    const std::chrono::steady_clock::time_point resume_deadline =
      deadline_in_ms (4000);
    while (!deadline_expired (resume_deadline)) {
        (void) as_socket (router)->process_submit_commands ();
        (void) as_socket (dealer)->process_submit_commands ();
        if (!as_socket (dealer)->application_pipe_remote_flow_paused (
              target.transport_pair_id, target.transport_pair_generation)) {
            resumed = true;
            break;
        }
        msleep (1);
    }
    TEST_ASSERT_TRUE (resumed);

    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
}

bool wait_for_pipe_pause (void *socket_,
                          uint64_t pair_id_,
                          uint64_t generation_,
                          bool expected_)
{
    const std::chrono::steady_clock::time_point deadline = deadline_in_ms (2000);
    while (!deadline_expired (deadline)) {
        (void) as_socket (socket_)->process_submit_commands ();
        if (as_socket (socket_)->application_pipe_remote_flow_paused (
              pair_id_, generation_)
            == expected_)
            return true;
        msleep (1);
    }
    return false;
}

//  Round 2, R1. A receiver that consumes a sub-LWM message while more data
//  remains publishes credit without sending an activation, because the byte
//  waiter is not armed yet. RESUME must not then decide from the writer's stale
//  cached credit: it would arm the waiter after the only qualifying read and
//  wait for an activation that nobody will ever send.
void test_resume_rereads_credit_published_before_the_waiter_was_armed ()
{
    paired_fixture_t fixture;
    fixture.setup (0, "flow_state_resume_stale_credit");

    //  Measure one message's exact charge, then set the HWM to a whole number
    //  of them so the queue can be filled to exactly full without any send
    //  ever failing - a failing send is what would arm the byte waiter.
    const std::string payload (100, 'y');
    //  Let the handshake's own credit land first, so the measurement below is
    //  not disturbed by an activation arriving mid-sequence.
    for (int i = 0; i < 50; ++i) {
        (void) as_socket (fixture.dealer)->process_submit_commands ();
        msleep (1);
    }

    uint64_t before = 0;
    uint64_t after = 0;
    TEST_ASSERT_TRUE (dealer_send_nonblocking (fixture.dealer, payload.c_str (), 0));
    TEST_ASSERT_TRUE (as_socket (fixture.dealer)
                        ->test_application_pipe_flow_probe (
                          fixture.pair_id, fixture.pair_generation, NULL, NULL,
                          NULL, NULL, &before));
    TEST_ASSERT_TRUE (dealer_send_nonblocking (fixture.dealer, payload.c_str (), 0));
    TEST_ASSERT_TRUE (as_socket (fixture.dealer)
                        ->test_application_pipe_flow_probe (
                          fixture.pair_id, fixture.pair_generation, NULL, NULL,
                          NULL, NULL, &after));
    TEST_ASSERT_GREATER_THAN_UINT64 (before, after);
    const uint64_t charge = after - before;

    //  Eight more messages land the queue on exactly the HWM, so no send ever
    //  fails and the byte waiter is never armed.
    const uint64_t remaining = 8;
    const uint64_t hwm = after + remaining * charge;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (fixture.dealer, ZLINK_OPT_SNDHWM, &hwm, sizeof (hwm)));
    for (uint64_t i = 0; i < remaining; ++i)
        TEST_ASSERT_TRUE (
          dealer_send_nonblocking (fixture.dealer, payload.c_str (), 0));

    bool out_active = false;
    bool hwm_full = false;
    bool remote_paused = false;
    bool byte_waiter = false;
    uint64_t in_flight = 0;
    TEST_ASSERT_TRUE (as_socket (fixture.dealer)
                        ->test_application_pipe_flow_probe (
                          fixture.pair_id, fixture.pair_generation, &out_active,
                          &hwm_full, &remote_paused, &byte_waiter, &in_flight));
    TEST_ASSERT_EQUAL_UINT64 (hwm, in_flight);
    TEST_ASSERT_TRUE (hwm_full);
    TEST_ASSERT_TRUE (out_active);
    TEST_ASSERT_FALSE (byte_waiter);

    TEST_ASSERT_TRUE (
      as_socket (fixture.dealer)
        ->test_deliver_flow_state_command (fixture.pair_id,
                                           fixture.pair_generation, 1, 1));
    TEST_ASSERT_TRUE (fixture.wait_for_applied_pause (true));
    //  Refused by the remote cause, so the byte cause is still unrecorded.
    TEST_ASSERT_FALSE (dealer_send_nonblocking (fixture.dealer, "payload", 0));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());

    //  Consume exactly one message. Its credit is below the LWM and no writer
    //  is registered as waiting, so no activation is sent - but the credit is
    //  published, and it is enough to clear the HWM.
    char rid[256];
    TEST_ASSERT_GREATER_THAN_INT (
      0, zlink_recv (fixture.router, rid, sizeof (rid), 0));
    recv_string_expect_success (fixture.router, payload.c_str (), 0);
    msleep (50);

    TEST_ASSERT_TRUE (as_socket (fixture.dealer)
                        ->test_application_pipe_flow_probe (
                          fixture.pair_id, fixture.pair_generation, &out_active,
                          &hwm_full, &remote_paused, &byte_waiter, &in_flight));
    //  The writer's cached view is unchanged: no activation arrived.
    TEST_ASSERT_EQUAL_UINT64 (hwm, in_flight);
    TEST_ASSERT_TRUE (hwm_full);
    TEST_ASSERT_FALSE (byte_waiter);

    TEST_ASSERT_TRUE (
      as_socket (fixture.dealer)
        ->test_deliver_flow_state_command (fixture.pair_id,
                                           fixture.pair_generation, 0, 2));
    TEST_ASSERT_TRUE (fixture.wait_for_applied_pause (false));

    //  Nothing else will read, so the route can only come back if RESUME
    //  looked at the credit the reader actually published.
    TEST_ASSERT_TRUE (wait_for_send_success (fixture.dealer, 2000));

    fixture.teardown ();
}

//  Review finding 7. Peer readiness answered from the byte HWM alone, so a
//  route whose peer is PAUSED was still reported writable and the send that
//  followed the report failed.
void test_router_peer_state_reports_remote_pause ()
{
    const int zero = 0;
    char endpoint[MAX_SOCKET_STRING];

    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (router, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    bind_loopback_ipv4 (router, endpoint, sizeof endpoint);

    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (dealer, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer, endpoint));

    send_string_expect_success (dealer, "hello", 0);
    char rid[256];
    const int rid_size = zlink_recv (router, rid, sizeof (rid), 0);
    TEST_ASSERT_GREATER_THAN_INT (0, rid_size);
    recv_string_expect_success (router, "hello", 0);

    zlink_routing_id_t peer_rid;
    memset (&peer_rid, 0, sizeof (peer_rid));
    peer_rid.size = static_cast<uint8_t> (rid_size);
    memcpy (peer_rid.data, rid, static_cast<size_t> (rid_size));

    zlink_routed_submit_target_t target;
    memset (&target, 0, sizeof (target));
    bool resolved = false;
    const std::chrono::steady_clock::time_point deadline = deadline_in_ms (2000);
    while (!deadline_expired (deadline)) {
        if (as_socket (router)->select_routed_submit_target (&peer_rid, &target)
              == 0
            && target.transport_pair_id != 0) {
            resolved = true;
            break;
        }
        msleep (1);
    }
    TEST_ASSERT_TRUE (resolved);

    //  Writable before the pause.
    TEST_ASSERT_EQUAL_INT (
      ZLINK_POLLOUT,
      as_socket (router)->get_peer_state (rid, static_cast<size_t> (rid_size))
        & ZLINK_POLLOUT);

    TEST_ASSERT_TRUE (
      as_socket (router)->test_deliver_flow_state_command (
        target.transport_pair_id, target.transport_pair_generation, 1, 1));
    TEST_ASSERT_TRUE (wait_for_pipe_pause (router, target.transport_pair_id,
                                           target.transport_pair_generation,
                                           true));

    //  Readiness has to agree with what a send would now do.
    TEST_ASSERT_EQUAL_INT (
      0, as_socket (router)->get_peer_state (rid, static_cast<size_t> (rid_size))
           & ZLINK_POLLOUT);

    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
}

//  Review finding 6. On a local pair the flow frame is queued on the completion
//  pipe instead of being intercepted by a session, so the completion drain has
//  to classify it. Classifying only a standalone first part let a FLOWSTATE
//  that follows the reply-envelope control parts be stored as reply payload -
//  which both hides the state and hands the frame to the application.
void test_flow_frame_after_envelope_parts_is_still_consumed_on_a_local_pair ()
{
    paired_fixture_t fixture;
    fixture.setup (0, "flow_state_misplaced_frame");

    zlink::pipe_t *router_completion =
      as_socket (fixture.router)
        ->completion_pipe_for_transport_pair (fixture.pair_id,
                                              fixture.pair_generation);
    TEST_ASSERT_NOT_NULL (router_completion);

    //  Four leading parts, exactly as many as a reply envelope carries, then
    //  the flow frame as the terminating part.
    for (int i = 0; i < 4; ++i) {
        zlink::msg_t part;
        TEST_ASSERT_EQUAL_INT (0, part.init_size (4));
        memcpy (part.data (), "ctrl", 4);
        part.set_flags (zlink::msg_t::more);
        TEST_ASSERT_TRUE (router_completion->write (&part));
        TEST_ASSERT_EQUAL_INT (0, part.init ());
        TEST_ASSERT_EQUAL_INT (0, part.close ());
    }

    zlink::flow_state::frame_t frame;
    frame.version = zlink::flow_state::frame_protocol_version;
    frame.state = k_paused;
    frame.pair_id = fixture.pair_id;
    frame.generation = fixture.pair_generation;
    frame.epoch = 11;
    zlink::msg_t flow;
    TEST_ASSERT_EQUAL_INT (0, flow.init ());
    TEST_ASSERT_EQUAL_INT (0, zlink::flow_state::init_frame (&flow, frame));
    TEST_ASSERT_TRUE (router_completion->write_and_flush (&flow));
    TEST_ASSERT_EQUAL_INT (0, flow.init ());
    TEST_ASSERT_EQUAL_INT (0, flow.close ());

    bool applied = false;
    const std::chrono::steady_clock::time_point deadline = deadline_in_ms (4000);
    while (!deadline_expired (deadline)) {
        uint32_t events = 0;
        (void) as_socket (fixture.dealer)
          ->get_events (ZLINK_POLLCOMPLETION, &events);
        (void) as_socket (fixture.dealer)->process_submit_commands ();
        if (as_socket (fixture.dealer)->application_pipe_remote_flow_paused (
              fixture.pair_id, fixture.pair_generation)) {
            applied = true;
            break;
        }
        msleep (1);
    }
    TEST_ASSERT_TRUE (applied);

    fixture.teardown ();
}

//  Review finding 5. Pair id and generation are the same on both lanes of a
//  pair, so they cannot on their own tell a completion-lane frame from an
//  application-lane one. A FLOWSTATE that arrives on the application lane must
//  be dropped: otherwise a peer could pause a route over the data lane and, by
//  advancing the epoch there, make the real completion-lane state unusable.
void test_flow_frame_on_the_application_lane_is_rejected ()
{
    paired_fixture_t fixture;
    fixture.setup ();

    zlink::pipe_t *application = as_socket (fixture.dealer)
                                   ->test_pair_pipe (fixture.pair_id,
                                                     fixture.pair_generation,
                                                     false);
    TEST_ASSERT_NOT_NULL (application);

    zlink::flow_state::frame_t frame;
    frame.version = zlink::flow_state::frame_protocol_version;
    frame.state = k_paused;
    frame.pair_id = fixture.pair_id;
    frame.generation = fixture.pair_generation;
    frame.epoch = 7;

    zlink::msg_t msg;
    TEST_ASSERT_EQUAL_INT (0, msg.init ());
    TEST_ASSERT_EQUAL_INT (0, zlink::flow_state::init_frame (&msg, frame));
    //  Consumed - it is a flow frame and must never reach another handler -
    //  but not applied.
    TEST_ASSERT_TRUE (
      as_socket (fixture.dealer)
        ->consume_receive_flow_state_frame (application, msg));
    TEST_ASSERT_EQUAL_INT (0, msg.close ());

    for (int i = 0; i < 100; ++i) {
        (void) as_socket (fixture.dealer)->process_submit_commands ();
        msleep (1);
    }
    TEST_ASSERT_FALSE (as_socket (fixture.dealer)->remote_receive_flow_paused (
      fixture.pair_id, fixture.pair_generation));
    TEST_ASSERT_FALSE (
      as_socket (fixture.dealer)->application_pipe_remote_flow_paused (
        fixture.pair_id, fixture.pair_generation));

    //  The rejected frame must not have consumed the epoch either: the same
    //  epoch arriving on the completion lane is still the real state.
    TEST_ASSERT_TRUE (fixture.inject (
      zlink::flow_state::frame_protocol_version, k_paused, fixture.pair_id,
      fixture.pair_generation, 7));
    TEST_ASSERT_TRUE (fixture.wait_for_applied_pause (true));

    fixture.teardown ();
}

//  Review finding 4. When a pair becomes ready and a PAUSE has already been
//  accepted for it, releasing the transport-pair hold must not publish a
//  writable edge first. Queueing the state and releasing the hold in the same
//  breath leaves a window in which the pair looks writable although its peer
//  has asked it to stop.
void test_ready_pair_with_pending_pause_publishes_no_writable_edge ()
{
    const int zero = 0;
    char endpoint[MAX_SOCKET_STRING];

    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (router, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    bind_loopback_ipv4 (router, endpoint, sizeof endpoint);
    TEST_ASSERT_EQUAL_INT (
      0, as_socket (router)->set_local_receive_flow_state (k_paused));

    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (dealer, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer, endpoint));

    //  Only the ROUTER is driven here. The DEALER's mailbox is left untouched
    //  so its transport I/O thread accepts the PAUSE frame before the socket
    //  thread ever runs the pair admission - the ordering in which the pair
    //  becomes ready while a PAUSE is already on record.
    for (int i = 0; i < 600; ++i) {
        (void) as_socket (router)->process_submit_commands ();
        msleep (1);
    }
    TEST_ASSERT_TRUE (
      as_socket (dealer)->test_flow_frame_accepted_before_pair_ready ());

    zlink_routed_submit_target_t target;
    memset (&target, 0, sizeof (target));
    bool paused_seen = false;
    const std::chrono::steady_clock::time_point deadline = deadline_in_ms (4000);
    while (!deadline_expired (deadline)) {
        if (as_socket (dealer)->select_routed_submit_target (NULL, &target) == 0
            && target.transport_pair_id != 0
            && as_socket (dealer)->application_pipe_remote_flow_paused (
                 target.transport_pair_id, target.transport_pair_generation)) {
            paused_seen = true;
            break;
        }
        (void) as_socket (router)->process_submit_commands ();
        msleep (1);
    }
    TEST_ASSERT_TRUE (paused_seen);

    //  The pair went straight from held to paused; no writable edge in between.
    TEST_ASSERT_EQUAL_UINT32 (
      0, as_socket (dealer)->test_transport_write_release_edges ());
    TEST_ASSERT_FALSE (dealer_send_nonblocking (dealer, "payload", 0));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());

    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
}

//  Review finding 3. A classic ROUTER send accepts the routing-ID part without
//  writing it to the pipe, so the pipe sees no message in progress. A PAUSE
//  that lands between the routing-ID part and the first payload part must not
//  break the message that the socket has already accepted.
void test_router_routing_id_part_holds_message_atomicity_across_pause ()
{
    const int zero = 0;
    const int mandatory = 1;
    char endpoint[MAX_SOCKET_STRING];

    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (router, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_router_option (
      router, ZLINK_ROUTER_OPT_MANDATORY, &mandatory, sizeof (mandatory)));
    bind_loopback_ipv4 (router, endpoint, sizeof endpoint);

    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (dealer, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer, endpoint));

    send_string_expect_success (dealer, "hello", 0);
    char rid[256];
    const int rid_size = zlink_recv (router, rid, sizeof (rid), 0);
    TEST_ASSERT_GREATER_THAN_INT (0, rid_size);
    recv_string_expect_success (router, "hello", 0);

    zlink_routing_id_t peer_rid;
    memset (&peer_rid, 0, sizeof (peer_rid));
    peer_rid.size = static_cast<uint8_t> (rid_size);
    memcpy (peer_rid.data, rid, static_cast<size_t> (rid_size));

    zlink_routed_submit_target_t target;
    memset (&target, 0, sizeof (target));
    bool resolved = false;
    const std::chrono::steady_clock::time_point deadline = deadline_in_ms (2000);
    while (!deadline_expired (deadline)) {
        if (as_socket (router)->select_routed_submit_target (&peer_rid, &target)
              == 0
            && target.transport_pair_id != 0) {
            resolved = true;
            break;
        }
        msleep (1);
    }
    TEST_ASSERT_TRUE (resolved);

    //  The socket accepts the routing-ID part: the message has started even
    //  though nothing has reached the pipe yet.
    TEST_ASSERT_EQUAL_INT (rid_size, zlink_send (router, rid, rid_size,
                                                 ZLINK_SNDMORE | ZLINK_DONTWAIT));

    TEST_ASSERT_TRUE (
      as_socket (router)->test_deliver_flow_state_command (
        target.transport_pair_id, target.transport_pair_generation, 1, 1));
    TEST_ASSERT_TRUE (wait_for_pipe_pause (router, target.transport_pair_id,
                                           target.transport_pair_generation,
                                           true));

    //  The started message must still complete.
    TEST_ASSERT_EQUAL_INT (7, zlink_send (router, "payload", 7, ZLINK_DONTWAIT));
    recv_string_expect_success (dealer, "payload", 0);

    //  The next message is blocked, from its very first part.
    TEST_ASSERT_EQUAL_INT (
      -1, zlink_send (router, rid, rid_size, ZLINK_SNDMORE | ZLINK_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());

    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
}

//  Review finding 2. The remote-pause cause is evaluated before the byte HWM,
//  so a send refused while PAUSED never records the HWM cause. If the queue is
//  over its HWM at that moment, RESUME alone cannot publish the writable edge
//  and the byte-credit path refuses to publish it either, because it only fires
//  when the HWM cause was recorded. The route would stay deactivated forever.
void test_resume_while_hwm_full_still_recovers_through_byte_credit ()
{
    paired_fixture_t fixture;
    //  A small HWM plus one message larger than it: the empty-pipe oversize
    //  exception admits that message, which leaves the queue over its HWM while
    //  the HWM cause has never been recorded.
    fixture.setup (512, "flow_state_resume_credit");

    bool out_active = false;
    bool hwm_full = false;
    bool remote_paused = false;
    TEST_ASSERT_TRUE (as_socket (fixture.dealer)
                        ->test_application_pipe_flow_probe (
                          fixture.pair_id, fixture.pair_generation,
                          &out_active, &hwm_full, &remote_paused));
    TEST_ASSERT_FALSE (hwm_full);

    const std::string oversize (4096, 'x');
    TEST_ASSERT_EQUAL_INT (
      static_cast<int> (oversize.size ()),
      zlink_send (fixture.dealer, oversize.data (),
                  static_cast<int> (oversize.size ()), ZLINK_DONTWAIT));

    //  Precondition of the defect: over the HWM, yet the HWM cause is unset.
    TEST_ASSERT_TRUE (as_socket (fixture.dealer)
                        ->test_application_pipe_flow_probe (
                          fixture.pair_id, fixture.pair_generation,
                          &out_active, &hwm_full, &remote_paused));
    TEST_ASSERT_TRUE (hwm_full);
    TEST_ASSERT_TRUE (out_active);

    TEST_ASSERT_TRUE (
      as_socket (fixture.dealer)
        ->test_deliver_flow_state_command (fixture.pair_id,
                                           fixture.pair_generation, 1, 1));
    TEST_ASSERT_TRUE (fixture.wait_for_applied_pause (true));

    //  This send is refused by the remote-pause cause and deactivates the
    //  route without recording the HWM cause.
    TEST_ASSERT_FALSE (dealer_send_nonblocking (fixture.dealer, "payload", 0));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());

    TEST_ASSERT_TRUE (
      as_socket (fixture.dealer)
        ->test_deliver_flow_state_command (fixture.pair_id,
                                           fixture.pair_generation, 0, 2));
    TEST_ASSERT_TRUE (fixture.wait_for_applied_pause (false));

    //  Draining returns the byte credit. The route has to come back.
    (void) drain_router (fixture.router);
    TEST_ASSERT_TRUE (wait_for_send_success (fixture.dealer, 2000));

    fixture.teardown ();
}

//  Review finding 1. attach_pipe replays the pair's stored state to the
//  application pipe, and a newer epoch can be accepted between the snapshot and
//  the queueing. The stale replay must not overwrite the newer state: without
//  an epoch on the pipe command the pipe stays PAUSED forever while the socket
//  record says RUNNING, and every later RUNNING frame is deduplicated away.
void test_stale_flow_state_command_cannot_override_a_newer_epoch ()
{
    paired_fixture_t fixture;
    fixture.setup ();

    //  Epoch 1 PAUSED, then epoch 2 RUNNING: the state the socket accepted.
    TEST_ASSERT_TRUE (
      as_socket (fixture.dealer)
        ->test_deliver_flow_state_command (fixture.pair_id,
                                           fixture.pair_generation, 1, 1));
    TEST_ASSERT_TRUE (
      as_socket (fixture.dealer)
        ->test_deliver_flow_state_command (fixture.pair_id,
                                           fixture.pair_generation, 0, 2));
    //  The attach replay carrying the older epoch 1 PAUSED arrives last.
    TEST_ASSERT_TRUE (
      as_socket (fixture.dealer)
        ->test_deliver_flow_state_command (fixture.pair_id,
                                           fixture.pair_generation, 1, 1));

    //  Drain every queued command before judging. Polling for the expected
    //  value would otherwise stop on the transient RUNNING that sits between
    //  the second and the third command.
    for (int i = 0; i < 300; ++i) {
        (void) as_socket (fixture.dealer)->process_submit_commands ();
        msleep (1);
    }

    TEST_ASSERT_FALSE (as_socket (fixture.dealer)->remote_receive_flow_paused (
      fixture.pair_id, fixture.pair_generation));
    TEST_ASSERT_FALSE (
      as_socket (fixture.dealer)->application_pipe_remote_flow_paused (
        fixture.pair_id, fixture.pair_generation));
    TEST_ASSERT_TRUE (wait_for_send_success (fixture.dealer, 2000));

    fixture.teardown ();
}

//  No application receive path ever returns a flow-state frame.
void test_no_application_recv_returns_a_flow_frame ()
{
    paired_fixture_t fixture;
    fixture.setup ();

    for (int i = 0; i < 4; ++i) {
        TEST_ASSERT_EQUAL_INT (
          0,
          as_socket (fixture.router)->set_local_receive_flow_state (k_paused));
        TEST_ASSERT_EQUAL_INT (
          0,
          as_socket (fixture.router)->set_local_receive_flow_state (k_running));
    }
    TEST_ASSERT_TRUE (fixture.wait_for_applied_pause (false));

    //  Nothing arrives on the DEALER's application receive path.
    char buffer[256];
    const std::chrono::steady_clock::time_point quiet = deadline_in_ms (200);
    while (!deadline_expired (quiet)) {
        const int rc =
          zlink_recv (fixture.dealer, buffer, sizeof (buffer), ZLINK_DONTWAIT);
        TEST_ASSERT_EQUAL_INT (-1, rc);
        TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
        msleep (5);
    }

    //  Ordinary traffic still flows in both directions and carries only the
    //  application's own payload.
    TEST_ASSERT_TRUE (wait_for_send_success (fixture.dealer, 2000));
    char rid[256];
    const int rid_size = zlink_recv (fixture.router, rid, sizeof (rid), 0);
    TEST_ASSERT_GREATER_THAN_INT (0, rid_size);
    recv_string_expect_success (fixture.router, "payload", 0);

    TEST_ASSERT_EQUAL_INT (rid_size, zlink_send (fixture.router, rid, rid_size,
                                                 ZLINK_SNDMORE));
    send_string_expect_success (fixture.router, "reply", 0);
    recv_string_expect_success (fixture.dealer, "reply", 0);

    fixture.teardown ();
}
}

int main ()
{
    setup_test_environment ();

    UNITY_BEGIN ();
    RUN_TEST (test_unsupported_socket_types_report_not_supported);
    RUN_TEST (test_invalid_state_is_rejected);
    RUN_TEST (test_remote_pause_blocks_sender_and_resume_releases_it);
    RUN_TEST (test_local_hwm_and_remote_pause_are_independent);
    RUN_TEST (test_pause_mid_multipart_preserves_atomicity);
    RUN_TEST (test_duplicate_and_stale_frames_are_ignored);
    RUN_TEST (test_new_and_reconnected_pairs_receive_the_latest_state);
    RUN_TEST (test_resume_rereads_credit_published_before_the_waiter_was_armed);
    RUN_TEST (test_router_peer_state_reports_remote_pause);
    RUN_TEST (test_flow_frame_after_envelope_parts_is_still_consumed_on_a_local_pair);
    RUN_TEST (test_flow_frame_on_the_application_lane_is_rejected);
    RUN_TEST (test_ready_pair_with_pending_pause_publishes_no_writable_edge);
    RUN_TEST (test_router_routing_id_part_holds_message_atomicity_across_pause);
    RUN_TEST (test_resume_while_hwm_full_still_recovers_through_byte_credit);
    RUN_TEST (test_stale_flow_state_command_cannot_override_a_newer_epoch);
    RUN_TEST (test_no_application_recv_returns_a_flow_frame);
    return UNITY_END ();
}
