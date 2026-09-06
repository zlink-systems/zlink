/* SPDX-License-Identifier: MPL-2.0 */

//  Core flow-state frames, pair topology, epoch fencing and byte-credit rules.
//  Socket owners and pipes are admitted locally without a transport session.

#include "unittest_flow_state_testutil.hpp"
#include "contract_socket_pair_fixture.hpp"
#include "testutil_unity.hpp"

#include "../../src/runtime/core/flow_state_frame.hpp"
#include "../../src/runtime/core/msg.hpp"
#include "../../src/runtime/core/pipe.hpp"
#include "../../src/api/socket/request_reply_protocol_internal.hpp"
#include "../../src/runtime/sockets/common/socket_base.hpp"
#include "../../src/runtime/sockets/dealer/dealer.hpp"
#include "../../src/runtime/sockets/router/router.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string.h>
#include <string>
#include <thread>

SETUP_TEARDOWN_TESTCONTEXT

namespace zlink
{
class session_termination_test_access_t
{
  public:
    static void attach_socket_pipe (socket_base_t *socket_, pipe_t *pipe_)
    {
        socket_->attach_pipe (pipe_);
    }
};
}

namespace
{
const int k_running = zlink::flow_state::receive_flow_running;
const int k_paused = zlink::flow_state::receive_flow_paused;

class passive_flow_pipe_sink_t : public zlink::i_pipe_events
{
  public:
    void read_activated (zlink::pipe_t *) ZLINK_OVERRIDE {}
    void write_activated (zlink::pipe_t *) ZLINK_OVERRIDE {}
    void hiccuped (zlink::pipe_t *) ZLINK_OVERRIDE {}
    void pipe_peer_terminated (zlink::pipe_t *, bool) ZLINK_OVERRIDE {}
    void pipe_terminated (zlink::pipe_t *) ZLINK_OVERRIDE {}
};

zlink::socket_base_t *as_socket (void *socket_)
{
    socket_handle_t handle = as_socket_handle (socket_);
    return handle.socket;
}

void pump_socket_commands (void *socket_)
{
    (void) contract_socket_pair_t::pump_owner (as_socket (socket_));
}

bool deadline_expired (const std::chrono::steady_clock::time_point &deadline_)
{
    return std::chrono::steady_clock::now () >= deadline_;
}

std::chrono::steady_clock::time_point deadline_in_ms (int ms_)
{
    return std::chrono::steady_clock::now () + std::chrono::milliseconds (ms_);
}

void assert_dealer_router_single_lane (void *socket_,
                                       uint64_t pair_id_,
                                       uint64_t generation_)
{
    zlink::pipe_t *const application =
      as_socket (socket_)->test_pair_pipe (pair_id_, generation_, false);
    TEST_ASSERT_NOT_NULL (application);
    TEST_ASSERT_NULL (
      as_socket (socket_)->test_pair_pipe (pair_id_, generation_, true));
    TEST_ASSERT_EQUAL_UINT (1u, application->get_transport_lane_count ());
    TEST_ASSERT_EQUAL_INT (zlink::transport_lane_application,
                           application->get_transport_lane ());
    TEST_ASSERT_NULL (as_socket (socket_)->completion_pipe_for_transport_pair (
      pair_id_, generation_));
}

//  A locally admitted DEALER/ROUTER pair with explicit command ownership.
struct paired_fixture_t
{
    paired_fixture_t () : pair (NULL), dealer (NULL), router (NULL) {}

    void setup (uint64_t dealer_sndhwm_ = 0,
                int router_weight_ = 100,
                int dealer_weight_ = 100)
    {
        const int zero = 0;
        router = test_context_socket (ZLINK_SOCKET_ROUTER);
        dealer = test_context_socket (ZLINK_SOCKET_DEALER);
        TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
          router, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
          dealer, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_set_router_option (
          router, ZLINK_ROUTER_OPT_WEIGHT, &router_weight_, sizeof (router_weight_)));
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_set_dealer_option (
          dealer, ZLINK_DEALER_OPT_WEIGHT, &dealer_weight_, sizeof (dealer_weight_)));
        if (dealer_sndhwm_)
            TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
              dealer, ZLINK_OPT_SNDHWM, &dealer_sndhwm_, sizeof (dealer_sndhwm_)));
        pair = new contract_socket_pair_t (dealer, router, 1, 1, true,
                                            dealer_sndhwm_);
        pair_id = pair->pair_id;
        pair_generation = pair->generation;
        flow_internal_send_string (dealer, "hello", 0);
        char rid[256];
        const int rid_size = flow_internal_recv (router, rid, sizeof (rid), 0);
        TEST_ASSERT_GREATER_THAN_INT (0, rid_size);
        flow_internal_recv_string (router, "hello", 0);
        peer_rid.assign (rid, static_cast<size_t> (rid_size));
        pair->pump ();
        TEST_ASSERT_TRUE (resolve_dealer_target ());
        assert_dealer_router_single_lane (dealer, pair_id, pair_generation);
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

    bool resolve_reconnected_dealer_target (uint64_t old_pair_id_,
                                             uint64_t old_generation_)
    {
        zlink_routed_submit_target_t target;
        memset (&target, 0, sizeof (target));
        const std::chrono::steady_clock::time_point deadline =
          deadline_in_ms (4000);
        while (!deadline_expired (deadline)) {
            pump_socket_commands (dealer);
            pump_socket_commands (router);
            if (as_socket (dealer)->select_routed_submit_target (NULL, &target)
                  == 0
                && target.transport_pair_id != 0
                && (target.transport_pair_id != old_pair_id_
                    || target.transport_pair_generation != old_generation_)) {
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
        delete pair;
        pair = NULL;
        if (dealer)
            dealer = test_context_socket_close_zero_linger (dealer);
        if (router)
            router = test_context_socket_close_zero_linger (router);
    }

    void replace_pair ()
    {
        const uint64_t next_pair = pair_id + 1;
        const uint64_t next_generation = pair_generation + 1;
        pair->application[0]->terminate (false);
        pair->pump ();
        delete pair;
        pair = new contract_socket_pair_t (dealer, router, next_pair,
                                            next_generation);
        pair_id = pair->pair_id;
        pair_generation = pair->generation;
    }

    //  Delivers one hand-built frame to the DEALER through the topology-selected
    //  control source (Application for this count-1 pair), then drains the
    //  socket mailbox so the pipe applies it.
    bool inject (uint8_t version_, uint8_t state_, uint64_t epoch_)
    {
        zlink::pipe_t *control =
          as_socket (dealer)->test_pair_pipe (pair_id, pair_generation, false);
        TEST_ASSERT_NOT_NULL (control);

        zlink::flow_state::frame_t frame;
        frame.version = zlink::flow_state::frame_protocol_version;
        frame.state = state_;
        frame.epoch = epoch_;

        zlink::msg_t msg;
        TEST_ASSERT_EQUAL_INT (0, msg.init ());
        TEST_ASSERT_EQUAL_INT (0, zlink::flow_state::init_frame (&msg, frame));
        msg.set_transport_connection_id (
          control->get_transport_connection_id ());
        static_cast<unsigned char *> (
          msg.data ())[zlink::flow_state::frame_name_size] = version_;
        const bool consumed =
          as_socket (dealer)->consume_receive_flow_state_frame (control, msg);
        TEST_ASSERT_EQUAL_INT (0, msg.close ());
        return consumed;
    }

    bool inject_from_other_connection (uint8_t state_, uint64_t epoch_)
    {
        zlink::pipe_t *control =
          as_socket (dealer)->test_pair_pipe (pair_id, pair_generation, false);
        TEST_ASSERT_NOT_NULL (control);

        zlink::flow_state::frame_t frame;
        frame.state = state_;
        frame.epoch = epoch_;
        zlink::msg_t msg;
        TEST_ASSERT_EQUAL_INT (0, msg.init ());
        TEST_ASSERT_EQUAL_INT (0, zlink::flow_state::init_frame (&msg, frame));
        const uint64_t current = control->get_transport_connection_id ();
        TEST_ASSERT_TRUE (current != 0);
        msg.set_transport_connection_id (current == UINT64_MAX ? 1 : current + 1);
        const bool consumed =
          as_socket (dealer)->consume_receive_flow_state_frame (control, msg);
        TEST_ASSERT_EQUAL_INT (0, msg.close ());
        return consumed;
    }

    bool wait_for_applied_pause (bool expected_)
    {
        pair->pump ();
        return as_socket (dealer)->application_pipe_remote_flow_paused (
                 pair_id, pair_generation) == expected_;
    }

    contract_socket_pair_t *pair;
    void *dealer;
    void *router;
    std::string peer_rid;
    uint64_t pair_id;
    uint64_t pair_generation;
};

bool resolve_router_pair_identity (paired_fixture_t *fixture_,
                                   uint64_t *pair_id_out_,
                                   uint64_t *generation_out_)
{
    if (!fixture_ || !pair_id_out_ || !generation_out_)
        return false;
    if (!fixture_->peer_rid.empty ()
        && as_socket (fixture_->router)->test_pair_identity_for_peer (
             reinterpret_cast<const unsigned char *> (
               fixture_->peer_rid.data ()),
             fixture_->peer_rid.size (), pair_id_out_, generation_out_))
        return true;

    // Inproc associates both pipe halves at construction and uses the peer
    // socket instance as its local pairing key, so its two owners retain the
    // connector's local pair fence.
    if (as_socket (fixture_->router)->test_pair_pipe (
          fixture_->pair_id, fixture_->pair_generation, false)) {
        *pair_id_out_ = fixture_->pair_id;
        *generation_out_ = fixture_->pair_generation;
        return true;
    }
    return false;
}

bool wait_for_paired_peer_weights (paired_fixture_t *fixture_,
                                   uint32_t dealer_weight_,
                                   uint32_t router_weight_)
{
    if (!fixture_)
        return false;
    const std::chrono::steady_clock::time_point deadline = deadline_in_ms (3000);
    while (!deadline_expired (deadline)) {
        pump_socket_commands (fixture_->dealer);
        pump_socket_commands (fixture_->router);
        zlink::pipe_t *const dealer_application =
          as_socket (fixture_->dealer)
            ->test_pair_pipe (fixture_->pair_id, fixture_->pair_generation,
                              false);
        uint64_t router_pair_id = 0;
        uint64_t router_generation = 0;
        zlink::pipe_t *router_application = NULL;
        if (resolve_router_pair_identity (
              fixture_, &router_pair_id, &router_generation))
            router_application = as_socket (fixture_->router)->test_pair_pipe (
              router_pair_id, router_generation, false);
        if (dealer_application && router_application
            && static_cast<zlink::dealer_t *> (as_socket (fixture_->dealer))
                   ->test_peer_weight (dealer_application)
                 == dealer_weight_
            && static_cast<zlink::router_t *> (as_socket (fixture_->router))
                   ->test_peer_weight (router_application)
                 == router_weight_)
            return true;
        msleep (1);
    }
    return false;
}

bool dealer_send_nonblocking (void *dealer_, const char *payload_, int flags_)
{
    const int rc = flow_internal_send (dealer_, payload_,
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
        const int rc = flow_internal_recv (router_, buffer, sizeof (buffer), ZLINK_DONTWAIT);
        if (rc < 0)
            break;
        ++drained;
    }
    return drained;
}

//  PAIR, the PUB/SUB family and STREAM do not support receive-flow control. The
//  internal entry reports not-supported and their existing send behaviour is
//  unchanged.
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
    void *binder = test_context_socket (ZLINK_SOCKET_PAIR);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (binder, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    void *connecter = test_context_socket (ZLINK_SOCKET_PAIR);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (connecter, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    contract_socket_pair_t local_pair (connecter, binder);
    TEST_ASSERT_EQUAL_INT (
      -1, as_socket (binder)->set_local_receive_flow_state (k_paused));
    TEST_ASSERT_EQUAL_INT (ENOTSUP, errno);
    flow_internal_send_string (connecter, "unchanged", 0);
    flow_internal_recv_string (binder, "unchanged", 0);
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

//  End to end over the real count-1 Application control path: the receiver's
//  socket-wide state reaches the sender and blocks it, and RUNNING releases it.
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
    TEST_ASSERT_TRUE (wait_for_send_success (fixture.dealer, 5000));

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
      zlink::flow_state::frame_protocol_version, k_paused, 1));
    TEST_ASSERT_TRUE (fixture.wait_for_applied_pause (true));
    (void) drain_router (fixture.router);
    msleep (50);
    (void) drain_router (fixture.router);
    TEST_ASSERT_TRUE (stays_blocked (fixture.dealer, 150));

    //  Removing the remote cause as well makes the pipe writable again.
    TEST_ASSERT_TRUE (fixture.inject (
      zlink::flow_state::frame_protocol_version, k_running, 2));
    TEST_ASSERT_TRUE (fixture.wait_for_applied_pause (false));
    TEST_ASSERT_TRUE (wait_for_send_success (fixture.dealer, 5000));

    //  Now the other order: fill the HWM again and clear only the remote
    //  cause. The HWM cause still holds the pipe.
    TEST_ASSERT_TRUE (fixture.inject (
      zlink::flow_state::frame_protocol_version, k_paused, 3));
    TEST_ASSERT_TRUE (fixture.wait_for_applied_pause (true));
    TEST_ASSERT_TRUE (fixture.inject (
      zlink::flow_state::frame_protocol_version, k_running, 4));
    TEST_ASSERT_TRUE (fixture.wait_for_applied_pause (false));
    TEST_ASSERT_TRUE (wait_for_send_success (fixture.dealer, 5000));

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
      zlink::flow_state::frame_protocol_version, k_paused, 1));
    TEST_ASSERT_TRUE (fixture.wait_for_applied_pause (true));

    //  The started message finishes.
    TEST_ASSERT_TRUE (dealer_send_nonblocking (fixture.dealer, "part-two", 0));
    //  The next message is blocked.
    TEST_ASSERT_TRUE (stays_blocked (fixture.dealer, 100));

    //  The receiver still observes the complete two-part message.
    char rid[256];
    const int rid_size = flow_internal_recv (fixture.router, rid, sizeof (rid), 0);
    TEST_ASSERT_GREATER_THAN_INT (0, rid_size);
    flow_internal_recv_string (fixture.router, "part-one", 0);
    flow_internal_recv_string (fixture.router, "part-two", 0);

    fixture.teardown ();
}

//  Duplicate, reversed and unsupported-version frames are consumed and
//  ignored without changing the applied state. Physical-connection staleness
//  is fenced by the receiving pipe rather than a peer-supplied wire field.
void test_duplicate_and_stale_frames_are_ignored ()
{
    paired_fixture_t fixture;
    fixture.setup ();

    TEST_ASSERT_TRUE (fixture.inject (
      zlink::flow_state::frame_protocol_version, k_paused, 5));
    TEST_ASSERT_TRUE (fixture.wait_for_applied_pause (true));

    //  Same epoch again: idempotent, still PAUSED.
    TEST_ASSERT_TRUE (fixture.inject (
      zlink::flow_state::frame_protocol_version, k_paused, 5));
    //  Older epoch that would resume: ignored.
    TEST_ASSERT_TRUE (fixture.inject (
      zlink::flow_state::frame_protocol_version, k_running, 4));
    //  A newer epoch recorded by another physical connection is also ignored;
    //  connection identity is local metadata, not a FLOWSTATE wire field.
    TEST_ASSERT_TRUE (fixture.inject_from_other_connection (k_running, 99));
    //  Unsupported protocol version: consumed and rejected.
    TEST_ASSERT_TRUE (fixture.inject (99, k_running, 101));

    fixture.pair->pump ();
    TEST_ASSERT_TRUE (
      as_socket (fixture.dealer)->application_pipe_remote_flow_paused (
        fixture.pair_id, fixture.pair_generation));
    TEST_ASSERT_TRUE (stays_blocked (fixture.dealer, 100));

    //  A frame that really advances the epoch resumes the pipe.
    TEST_ASSERT_TRUE (fixture.inject (
      zlink::flow_state::frame_protocol_version, k_running, 102));
    TEST_ASSERT_TRUE (fixture.wait_for_applied_pause (false));
    TEST_ASSERT_TRUE (wait_for_send_success (fixture.dealer, 5000));

    fixture.teardown ();
}

//  A pair that becomes ready after the local state was stored - a first
//  connection or a reconnect - is synchronised with that stored state.
void test_new_and_reconnected_pairs_receive_the_latest_state ()
{
    const int zero = 0;
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (router, ZLINK_OPT_LINGER, &zero, sizeof (zero)));

    //  Stored before any pair exists.
    TEST_ASSERT_EQUAL_INT (
      0, as_socket (router)->set_local_receive_flow_state (k_paused));

    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (dealer, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    contract_socket_pair_t local_pair (dealer, router);

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
        (void) contract_socket_pair_t::pump_owner (as_socket (router));
        msleep (1);
    }
    TEST_ASSERT_TRUE (paused_seen);
    const uint64_t first_pair_id = target.transport_pair_id;

    //  Reconnect: the replacement pair is synchronised with the same stored
    //  state without any further call.
    local_pair.application[0]->terminate (false);
    local_pair.pump ();
    contract_socket_pair_t replacement_pair (dealer, router, 2, 2);

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
        (void) contract_socket_pair_t::pump_owner (as_socket (router));
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
        (void) contract_socket_pair_t::pump_owner (as_socket (router));
        (void) contract_socket_pair_t::pump_owner (as_socket (dealer));
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
        (void) contract_socket_pair_t::pump_owner (as_socket (socket_));
        if (as_socket (socket_)->application_pipe_remote_flow_paused (
              pair_id_, generation_)
            == expected_)
            return true;
        msleep (1);
    }
    return false;
}

//  Round 2, R5. Epoch edge cases: the first state a pipe ever sees, equality
//  and reversal, the top of the range, and the socket-side wraparound.
void test_flow_state_epoch_edge_cases ()
{
    paired_fixture_t fixture;
    fixture.setup ();

    const uint64_t high_epoch = UINT64_MAX / 2;

    //  A pipe with no epoch yet accepts whatever arrives first, however large.
    TEST_ASSERT_TRUE (
      as_socket (fixture.dealer)
        ->test_deliver_flow_state_command (fixture.pair_id,
                                           fixture.pair_generation, 1,
                                           high_epoch));
    TEST_ASSERT_TRUE (fixture.wait_for_applied_pause (true));

    //  Equal epoch, then a lower one: both ignored.
    TEST_ASSERT_TRUE (
      as_socket (fixture.dealer)
        ->test_deliver_flow_state_command (fixture.pair_id,
                                           fixture.pair_generation, 0,
                                           high_epoch));
    TEST_ASSERT_TRUE (
      as_socket (fixture.dealer)
        ->test_deliver_flow_state_command (fixture.pair_id,
                                           fixture.pair_generation, 0,
                                           high_epoch - 1));
    for (int i = 0; i < 100; ++i) {
        fixture.pair->pump ();
        msleep (1);
    }
    TEST_ASSERT_TRUE (
      as_socket (fixture.dealer)->application_pipe_remote_flow_paused (
        fixture.pair_id, fixture.pair_generation));

    //  The top of the range still advances.
    TEST_ASSERT_TRUE (
      as_socket (fixture.dealer)
        ->test_deliver_flow_state_command (fixture.pair_id,
                                           fixture.pair_generation, 0,
                                           UINT64_MAX));
    TEST_ASSERT_TRUE (fixture.wait_for_applied_pause (false));

    //  Past the top nothing advances any more within this generation. That is
    //  the deliberate trade-off: the sequence is monotonic per generation and a
    //  new generation starts a fresh pipe with a fresh sequence.
    TEST_ASSERT_TRUE (
      as_socket (fixture.dealer)
        ->test_deliver_flow_state_command (fixture.pair_id,
                                           fixture.pair_generation, 1, 1));
    for (int i = 0; i < 100; ++i) {
        fixture.pair->pump ();
        msleep (1);
    }
    TEST_ASSERT_FALSE (
      as_socket (fixture.dealer)->application_pipe_remote_flow_paused (
        fixture.pair_id, fixture.pair_generation));

    //  The socket-wide epoch must never produce 0 on wraparound: 0 is the
    //  "never set" marker and is refused by the frame contract, so wrapping
    //  into it would silence the socket's flow state for good.
    as_socket (fixture.router)->test_set_local_receive_flow_epoch (UINT64_MAX);
    TEST_ASSERT_EQUAL_INT (
      0, as_socket (fixture.router)->set_local_receive_flow_state (k_paused));
    TEST_ASSERT_TRUE (
      as_socket (fixture.router)->test_local_receive_flow_epoch () != 0);

    fixture.teardown ();
}

//  Round 2, R5. A replacement generation starts a fresh epoch sequence, so a
//  low epoch is accepted again on the new pair.
void test_generation_change_resets_the_epoch_sequence ()
{
    paired_fixture_t fixture;
    fixture.setup ();

    TEST_ASSERT_TRUE (
      as_socket (fixture.dealer)
        ->test_deliver_flow_state_command (fixture.pair_id,
                                           fixture.pair_generation, 1,
                                           UINT64_MAX));
    TEST_ASSERT_TRUE (fixture.wait_for_applied_pause (true));

    const uint64_t old_pair_id = fixture.pair_id;
    fixture.replace_pair ();

    zlink_routed_submit_target_t target;
    memset (&target, 0, sizeof (target));
    bool replaced = false;
    const std::chrono::steady_clock::time_point deadline = deadline_in_ms (4000);
    while (!deadline_expired (deadline)) {
        if (as_socket (fixture.dealer)->select_routed_submit_target (NULL, &target)
              == 0
            && target.transport_pair_id != 0
            && target.transport_pair_id != old_pair_id) {
            replaced = true;
            break;
        }
        (void) contract_socket_pair_t::pump_owner (as_socket (fixture.router));
        msleep (1);
    }
    TEST_ASSERT_TRUE (replaced);

    //  Epoch 1 on the replacement pair, far below the previous generation's
    //  last epoch, is accepted.
    TEST_ASSERT_TRUE (
      as_socket (fixture.dealer)
        ->test_deliver_flow_state_command (target.transport_pair_id,
                                           target.transport_pair_generation, 1,
                                           1));
    TEST_ASSERT_TRUE (wait_for_pipe_pause (fixture.dealer,
                                           target.transport_pair_id,
                                           target.transport_pair_generation,
                                           true));

    fixture.teardown ();
}

// A flow frame is valid only at a record boundary. If it appears after a reply
// part carrying MORE on the count-1 Application FIFO, the pair is malformed and
// the outstanding request cannot complete successfully.
void init_empty_completion (zlink_completion_t *completion_)
{
    memset (completion_, 0, sizeof (*completion_));
    completion_->struct_size = sizeof (*completion_);
}

void test_flow_frame_cannot_complete_a_truncated_reply ()
{
    const int zero = 0;
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (router, ZLINK_OPT_LINGER, &zero, sizeof (zero)));

    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (dealer, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    contract_socket_pair_t local_pair (dealer, router);

    zlink_msg_t request_part;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&request_part, 4));
    memcpy (zlink_msg_data (&request_part), "ping", 4);
    zlink_completion_id_t completion_id = 0;
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_OK,
                           zlink_request_part (
                             dealer, NULL, &request_part,
                             ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL, 1500,
                             NULL, &completion_id));
    TEST_ASSERT_TRUE (completion_id != 0);

    const zlink_routing_id_t *peer_rid = NULL;
    uint64_t request_seq = 0;
    zlink_msg_t *parts = NULL;
    size_t part_count = 0;
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK,
                           zlink_router_recv (router, &peer_rid, &request_seq,
                                              &parts, &part_count, 0));
    TEST_ASSERT_NOT_NULL (peer_rid);
    const zlink_routing_id_t rid_value = *peer_rid;
    zlink_multipart_close (parts, part_count);

    zlink_routed_submit_target_t target;
    memset (&target, 0, sizeof (target));
    TEST_ASSERT_EQUAL_INT (
      0, as_socket (router)->select_routed_submit_target (&rid_value, &target));
    TEST_ASSERT_TRUE (target.transport_pair_id != 0);

    zlink::pipe_t *router_control = as_socket (router)->test_pair_pipe (
      target.transport_pair_id, target.transport_pair_generation, false);
    TEST_ASSERT_NOT_NULL (router_control);
    TEST_ASSERT_EQUAL_UINT (1u, router_control->get_transport_lane_count ());
    TEST_ASSERT_EQUAL_INT (zlink::transport_lane_application,
                           router_control->get_transport_lane ());
    TEST_ASSERT_NULL (as_socket (router)->test_pair_pipe (
      target.transport_pair_id, target.transport_pair_generation, true));
    TEST_ASSERT_FALSE_MESSAGE (
      router_control->check_read (),
      "Application control source retained a routing-id preamble");

    zlink::msg_t reply_head;
    TEST_ASSERT_EQUAL_INT (0, reply_head.init_size (4));
    memcpy (reply_head.data (), "part", 4);
    TEST_ASSERT_EQUAL_INT (
      0, reply_head.set_request_reply_metadata (
           zlink::request_reply::reply_type, request_seq));
    reply_head.set_flags (zlink::msg_t::more);
    TEST_ASSERT_TRUE (router_control->write (&reply_head));
    TEST_ASSERT_EQUAL_INT (0, reply_head.init ());
    TEST_ASSERT_EQUAL_INT (0, reply_head.close ());

    zlink::flow_state::frame_t frame;
    frame.version = zlink::flow_state::frame_protocol_version;
    frame.state = k_paused;
    frame.epoch = 21;
    zlink::msg_t flow;
    TEST_ASSERT_EQUAL_INT (0, flow.init ());
    TEST_ASSERT_EQUAL_INT (0, zlink::flow_state::init_frame (&flow, frame));
    flow.set_transport_connection_id (
      router_control->get_transport_connection_id ());
    TEST_ASSERT_TRUE (router_control->write_and_flush (&flow));
    TEST_ASSERT_EQUAL_INT (0, flow.init ());
    TEST_ASSERT_EQUAL_INT (0, flow.close ());

    //  Drive the client's socket-local completion drain. The truncated reply
    //  must never be reported as a successful reply.
    zlink_completion_t completion;
    init_empty_completion (&completion);
    for (int i = 0; i < 400; ++i) {
        const zlink_recv_result_t recv_rc = zlink_completion_recv (
          dealer, &completion, ZLINK_RECV_FLAGS_DONTWAIT);
        if (recv_rc == ZLINK_RECV_OK)
            break;
        TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA, recv_rc);
        TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
        msleep (1);
    }
    if (completion.kind != 0)
        TEST_ASSERT_TRUE (completion.request_result != ZLINK_REQUEST_OK);
    zlink_completion_close (&completion);

    // A misplaced flow frame is not applied. The malformed count-1 pair is
    // removed instead, and teardown supplies the normal disconnect result for
    // any still-pending request.
    bool old_pair_removed = false;
    const std::chrono::steady_clock::time_point removal_deadline =
      deadline_in_ms (4000);
    while (!deadline_expired (removal_deadline)) {
        // Inproc termination is a two-socket command handshake. Progress both
        // socket owners so the peer's term request and the local ack are
        // applied before checking that the malformed pair was removed.
        pump_socket_commands (dealer);
        pump_socket_commands (router);
        if (!as_socket (dealer)->test_pair_pipe (
              target.transport_pair_id, target.transport_pair_generation,
              false)) {
            old_pair_removed = true;
            break;
        }
        msleep (1);
    }
    TEST_ASSERT_TRUE (old_pair_removed);

    test_context_socket_close_zero_linger (dealer);
    test_context_socket_close_zero_linger (router);
}

//  Pass 3, S4. Epoch 0 is the "never set" marker. The frame decoder already
//  refuses it, and the pipe command must refuse it too rather than treat it as
//  a reset that overrides whatever ordering the pipe has established.
void test_epoch_zero_is_refused_by_the_pipe_command ()
{
    paired_fixture_t fixture;
    fixture.setup ();

    TEST_ASSERT_TRUE (
      as_socket (fixture.dealer)
        ->test_deliver_flow_state_command (fixture.pair_id,
                                           fixture.pair_generation, 1, 0));
    for (int i = 0; i < 100; ++i) {
        fixture.pair->pump ();
        msleep (1);
    }
    TEST_ASSERT_FALSE (
      as_socket (fixture.dealer)->application_pipe_remote_flow_paused (
        fixture.pair_id, fixture.pair_generation));

    //  An ordinary epoch still applies, so the refusal is about 0 and not
    //  about the pipe having stopped accepting states.
    TEST_ASSERT_TRUE (
      as_socket (fixture.dealer)
        ->test_deliver_flow_state_command (fixture.pair_id,
                                           fixture.pair_generation, 1, 1));
    TEST_ASSERT_TRUE (fixture.wait_for_applied_pause (true));

    fixture.teardown ();
}

//  Exhausting the epoch closes the old pair. A newly admitted generation
//  starts a fresh sequence and receives the current absolute state. The
//  transport reconnect itself is exercised through the public integration API.
void test_epoch_wraparound_forces_a_new_connection_generation ()
{
    paired_fixture_t fixture;
    fixture.setup ();

    const uint64_t first_generation = fixture.pair_generation;
    as_socket (fixture.router)->test_set_local_receive_flow_epoch (UINT64_MAX);
    TEST_ASSERT_EQUAL_INT (
      0, as_socket (fixture.router)->set_local_receive_flow_state (k_paused));
    TEST_ASSERT_TRUE (
      as_socket (fixture.router)->test_local_receive_flow_epoch () != 0);

    fixture.pair->pump ();
    TEST_ASSERT_NULL (as_socket (fixture.dealer)->test_pair_pipe (
      fixture.pair_id, fixture.pair_generation, false));
    delete fixture.pair;
    fixture.pair = new contract_socket_pair_t (
      fixture.dealer, fixture.router, fixture.pair_id, first_generation + 1);

    zlink_routed_submit_target_t target;
    memset (&target, 0, sizeof (target));
    bool replaced_and_paused = false;
    const std::chrono::steady_clock::time_point deadline = deadline_in_ms (8000);
    while (!deadline_expired (deadline)) {
        if (as_socket (fixture.dealer)->select_routed_submit_target (NULL, &target)
              == 0
            && target.transport_pair_id != 0
            //  Reconnect keeps the pair id and advances the generation, which
            //  is exactly the fresh-generation path a receiver accepts
            //  unconditionally.
            && target.transport_pair_generation != first_generation
            && as_socket (fixture.dealer)->application_pipe_remote_flow_paused (
                 target.transport_pair_id, target.transport_pair_generation)) {
            replaced_and_paused = true;
            break;
        }
        (void) contract_socket_pair_t::pump_owner (as_socket (fixture.router));
        msleep (1);
    }
    TEST_ASSERT_TRUE (replaced_and_paused);

    fixture.teardown ();
}

//  A pair-ready resync is an absolute state and is not repeated until it
//  changes. If it overtakes peer registration, hold it by candidate connection
//  without accepting or reporting it, then promote only the topology-selected
//  connection that wins validation.
void test_flow_frame_before_registration_is_promoted_after_validation ()
{
    const int zero = 0;
    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (dealer, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    zlink::socket_base_t *const dealer_socket = as_socket (dealer);

    // Construct the exact state between transport validation and socket-owner
    // admission instead of racing the I/O thread against the socket mailbox.
    // READY and the following FLOWSTATE share a count-1 FIFO, but decoding the
    // latter and processing the former's bind command are allowed in either
    // order. This explicit pre-attach pipe makes the buffering branch stable.
    zlink::object_t *parents[2] = {dealer_socket, dealer_socket};
    zlink::pipe_t *pipes[2] = {NULL, NULL};
    const uint64_t hwms[2] = {4096, 4096};
    const bool conflates[2] = {false, false};
    TEST_ASSERT_SUCCESS_ERRNO (zlink::pipepair (
      parents, pipes, hwms, conflates, false,
      zlink::transport_lane_application));

    const uint64_t pair_id = 91;
    const uint64_t generation = 7;
    const unsigned char peer_identity[] = {'r', 'o', 'u', 't', 'e', 'r'};
    for (size_t i = 0; i < 2; ++i) {
        pipes[i]->set_transport_pair (zlink::transport_lane_application,
                                      pair_id, generation);
        pipes[i]->set_transport_lane_count (1);
    }
    pipes[0]->set_peer_socket_type (ZLINK_CORE_SOCKET_ROUTER);
    pipes[1]->set_peer_socket_type (ZLINK_CORE_SOCKET_DEALER);
    pipes[0]->set_peer_routing_id (peer_identity, sizeof (peer_identity));
    pipes[0]->set_transport_peer_identity (peer_identity,
                                           sizeof (peer_identity));
    pipes[0]->hold_writes_until_transport_pair_ready ();
    passive_flow_pipe_sink_t peer_sink;
    pipes[1]->set_event_sink (&peer_sink);

    const uint64_t source_connection_id =
      pipes[0]->get_transport_connection_id ();
    TEST_ASSERT_TRUE (source_connection_id != 0);
    zlink::flow_state::frame_t paused_frame;
    paused_frame.state = zlink::flow_state::receive_flow_paused;
    paused_frame.epoch = 1;
    zlink::msg_t paused_msg;
    TEST_ASSERT_SUCCESS_ERRNO (paused_msg.init ());
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink::flow_state::init_frame (&paused_msg, paused_frame));
    paused_msg.set_transport_connection_id (source_connection_id);
    TEST_ASSERT_TRUE (
      dealer_socket->consume_receive_flow_state_frame (pipes[0], paused_msg));
    TEST_ASSERT_SUCCESS_ERRNO (paused_msg.close ());

    bool buffered_paused = false;
    uint64_t buffered_epoch = 0;
    uint64_t buffered_pair_id = 0;
    uint64_t buffered_generation = 0;
    uint64_t buffered_source_connection_id = 0;
    TEST_ASSERT_TRUE (dealer_socket->test_pending_flow_buffered (
      &buffered_paused, &buffered_epoch, &buffered_pair_id,
      &buffered_generation, &buffered_source_connection_id));
    TEST_ASSERT_TRUE (buffered_paused);
    TEST_ASSERT_EQUAL_UINT64 (1, buffered_epoch);
    TEST_ASSERT_EQUAL_UINT64 (pair_id, buffered_pair_id);
    TEST_ASSERT_EQUAL_UINT64 (generation, buffered_generation);
    TEST_ASSERT_EQUAL_UINT64 (source_connection_id,
                              buffered_source_connection_id);
    TEST_ASSERT_FALSE (
      dealer_socket->test_pair_is_ready (pair_id, generation));
    TEST_ASSERT_FALSE (
      dealer_socket->remote_receive_flow_paused (pair_id, generation));

    //  A foreign candidate with a newer epoch cannot overwrite the real
    //  connection's one-shot PAUSE.
    const uint64_t foreign_connection_id =
      buffered_source_connection_id == UINT64_MAX
        ? 1
        : buffered_source_connection_id + 1;
    TEST_ASSERT_TRUE (dealer_socket->test_buffer_flow_frame (
      pair_id, generation, foreign_connection_id, false, buffered_epoch + 8));

    zlink::session_termination_test_access_t::attach_socket_pipe (
      dealer_socket, pipes[0]);
    TEST_ASSERT_TRUE (dealer_socket->test_pair_is_ready (pair_id, generation));
    TEST_ASSERT_TRUE (
      dealer_socket->remote_receive_flow_paused (pair_id, generation));
    TEST_ASSERT_TRUE (dealer_socket->application_pipe_remote_flow_paused (
      pair_id, generation));
    TEST_ASSERT_FALSE (
      dealer_socket->test_pending_flow_buffered (NULL, NULL));
    TEST_ASSERT_TRUE (stays_blocked (dealer, 100));

    //  The next state advances normally after promotion and releases sends.
    zlink::flow_state::frame_t running_frame;
    running_frame.state = zlink::flow_state::receive_flow_running;
    running_frame.epoch = 2;
    zlink::msg_t running_msg;
    TEST_ASSERT_SUCCESS_ERRNO (running_msg.init ());
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink::flow_state::init_frame (&running_msg, running_frame));
    running_msg.set_transport_connection_id (source_connection_id);
    TEST_ASSERT_TRUE (
      dealer_socket->consume_receive_flow_state_frame (pipes[0], running_msg));
    TEST_ASSERT_SUCCESS_ERRNO (running_msg.close ());
    bool converged = false;
    const std::chrono::steady_clock::time_point converge_deadline =
      deadline_in_ms (4000);
    while (!deadline_expired (converge_deadline)) {
        (void) dealer_socket->process_submit_commands ();
        if (!dealer_socket->application_pipe_remote_flow_paused (
              pair_id, generation)) {
            converged = true;
            break;
        }
        msleep (1);
    }
    TEST_ASSERT_TRUE (converged);
    TEST_ASSERT_TRUE (wait_for_send_success (dealer, 5000));

    zlink::msg_t delivered;
    TEST_ASSERT_SUCCESS_ERRNO (delivered.init ());
    TEST_ASSERT_TRUE (pipes[1]->read (&delivered));
    TEST_ASSERT_SUCCESS_ERRNO (delivered.close ());

    pipes[0]->terminate (false);
    pipes[1]->terminate (false);
    pump_socket_commands (dealer);
    test_context_socket_close_zero_linger (dealer);
}

//  Round 2, R1. A receiver that consumes a sub-LWM message while more data
//  remains publishes credit without sending an activation, because the byte
//  waiter is not armed yet. RESUME must not then decide from the writer's stale
//  cached credit: it would arm the waiter after the only qualifying read and
//  wait for an activation that nobody will ever send.
void test_resume_rereads_credit_published_before_the_waiter_was_armed ()
{
    paired_fixture_t fixture;
    fixture.setup (0);

    //  Measure one message's exact charge, then set the HWM to a whole number
    //  of them so the queue can be filled to exactly full without any send
    //  ever failing - a failing send is what would arm the byte waiter.
    const std::string payload (100, 'y');
    //  Let the handshake's own credit land first, so the measurement below is
    //  not disturbed by an activation arriving mid-sequence.
    for (int i = 0; i < 50; ++i) {
        fixture.pair->pump ();
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
      0, flow_internal_recv (fixture.router, rid, sizeof (rid), 0));
    flow_internal_recv_string (fixture.router, payload.c_str (), 0);
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
    TEST_ASSERT_TRUE (wait_for_send_success (fixture.dealer, 5000));

    fixture.teardown ();
}

//  Review finding 7. Peer readiness answered from the byte HWM alone, so a
//  route whose peer is PAUSED was still reported writable and the send that
//  followed the report failed.
void test_router_peer_state_reports_remote_pause ()
{
    const int zero = 0;

    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (router, ZLINK_OPT_LINGER, &zero, sizeof (zero)));

    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (dealer, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    contract_socket_pair_t local_pair (dealer, router);

    flow_internal_send_string (dealer, "hello", 0);
    char rid[256];
    const int rid_size = flow_internal_recv (router, rid, sizeof (rid), 0);
    TEST_ASSERT_GREATER_THAN_INT (0, rid_size);
    flow_internal_recv_string (router, "hello", 0);

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

// On a local count-1 pair the flow frame and reply share the Application pipe
// instead of being intercepted by a session. A standalone flow frame is
// consumed before the next reply kind is dispatched, and both effects remain
// observable without leaking either record through public receive.
void test_flow_frame_before_reply_is_consumed_on_a_local_pair ()
{
    paired_fixture_t fixture;
    fixture.setup (0);

    zlink_msg_t request_part;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&request_part, 4));
    memcpy (zlink_msg_data (&request_part), "ping", 4);
    zlink_completion_id_t completion_id = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_request_part (fixture.dealer, NULL, &request_part,
                          ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL, 1500,
                          NULL, &completion_id));
    TEST_ASSERT_TRUE (completion_id != 0);

    const zlink_routing_id_t *peer_rid = NULL;
    uint64_t request_seq = 0;
    zlink_msg_t *parts = NULL;
    size_t part_count = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_router_recv (fixture.router, &peer_rid, &request_seq, &parts,
                         &part_count, 0));
    TEST_ASSERT_NOT_NULL (peer_rid);
    TEST_ASSERT_TRUE (request_seq != 0);
    zlink_multipart_close (parts, part_count);

    zlink::pipe_t *router_control = as_socket (fixture.router)->test_pair_pipe (
      fixture.pair_id, fixture.pair_generation, false);
    TEST_ASSERT_NOT_NULL (router_control);
    TEST_ASSERT_EQUAL_UINT (1u, router_control->get_transport_lane_count ());
    TEST_ASSERT_EQUAL_INT (zlink::transport_lane_application,
                           router_control->get_transport_lane ());

    zlink::flow_state::frame_t frame;
    frame.version = zlink::flow_state::frame_protocol_version;
    frame.state = k_paused;
    frame.epoch = 11;
    zlink::msg_t flow;
    TEST_ASSERT_EQUAL_INT (0, flow.init ());
    TEST_ASSERT_EQUAL_INT (0, zlink::flow_state::init_frame (&flow, frame));
    flow.set_transport_connection_id (
      router_control->get_transport_connection_id ());
    TEST_ASSERT_TRUE (router_control->write_and_flush (&flow));
    TEST_ASSERT_EQUAL_INT (0, flow.init ());
    TEST_ASSERT_EQUAL_INT (0, flow.close ());

    zlink::msg_t reply;
    TEST_ASSERT_EQUAL_INT (0, reply.init_size (4));
    memcpy (reply.data (), "pong", 4);
    TEST_ASSERT_EQUAL_INT (
      0, reply.set_request_reply_metadata (
           zlink::request_reply::reply_type, request_seq));
    reply.set_transport_connection_id (
      router_control->get_transport_connection_id ());
    TEST_ASSERT_TRUE (router_control->write_and_flush (&reply));
    TEST_ASSERT_EQUAL_INT (0, reply.init ());
    TEST_ASSERT_EQUAL_INT (0, reply.close ());

    bool applied = false;
    bool completed = false;
    zlink_completion_t completion;
    init_empty_completion (&completion);
    const std::chrono::steady_clock::time_point deadline = deadline_in_ms (4000);
    while (!deadline_expired (deadline)) {
        const zlink_recv_result_t recv_rc = zlink_completion_recv (
          fixture.dealer, &completion, ZLINK_RECV_FLAGS_DONTWAIT);
        if (recv_rc == ZLINK_RECV_OK)
            completed = true;
        else {
            TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA, recv_rc);
            TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
        }
        if (as_socket (fixture.dealer)->application_pipe_remote_flow_paused (
              fixture.pair_id, fixture.pair_generation)) {
            applied = true;
        }
        if (applied && completed)
            break;
        msleep (1);
    }
    TEST_ASSERT_TRUE (applied);
    TEST_ASSERT_TRUE (completed);
    TEST_ASSERT_EQUAL_UINT64 (completion_id, completion.completion_id);
    TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, completion.request_result);
    zlink_completion_close (&completion);

    fixture.teardown ();
}

//  Peer-weight advertisements are Core control on the same count-1 Application
//  pipe. They must be consumed at a boundary without becoming public data or
//  interfering with socket-local reply completion.
void test_peer_weight_change_does_not_leak_to_public_receive ()
{
    paired_fixture_t fixture;
    fixture.setup (0);

    const int weight = 37;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_router_option (fixture.router, ZLINK_ROUTER_OPT_WEIGHT,
                               &weight, sizeof (weight)));

    zlink::pipe_t *dealer_control = as_socket (fixture.dealer)->test_pair_pipe (
      fixture.pair_id, fixture.pair_generation, false);
    TEST_ASSERT_NOT_NULL (dealer_control);
    TEST_ASSERT_EQUAL_UINT (1u, dealer_control->get_transport_lane_count ());
    TEST_ASSERT_EQUAL_INT (zlink::transport_lane_application,
                           dealer_control->get_transport_lane ());
    TEST_ASSERT_FALSE (dealer_control->check_read ());

    zlink_msg_t request;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&request, 4));
    memcpy (zlink_msg_data (&request), "ping", 4);
    zlink_completion_id_t completion_id = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_request_part (fixture.dealer, NULL, &request,
                          ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL, 1500,
                          NULL, &completion_id));
    TEST_ASSERT_TRUE (completion_id != 0);

    const zlink_routing_id_t *peer_rid = NULL;
    uint64_t request_seq = 0;
    zlink_msg_t *parts = NULL;
    size_t part_count = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_RECV_OK,
      zlink_router_recv (fixture.router, &peer_rid, &request_seq, &parts,
                         &part_count, 0));
    TEST_ASSERT_NOT_NULL (peer_rid);
    TEST_ASSERT_TRUE (request_seq != 0);
    const zlink_routing_id_t reply_rid = *peer_rid;
    zlink_multipart_close (parts, part_count);

    zlink_msg_t reply;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&reply, 4));
    memcpy (zlink_msg_data (&reply), "pong", 4);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_reply_part (fixture.router, &reply_rid, request_seq, &reply,
                        ZLINK_PART_FINAL));

    bool completed = false;
    zlink_completion_t completion;
    init_empty_completion (&completion);
    const std::chrono::steady_clock::time_point deadline = deadline_in_ms (4000);
    while (!deadline_expired (deadline)) {
        const zlink_recv_result_t recv_rc = zlink_completion_recv (
          fixture.dealer, &completion, ZLINK_RECV_FLAGS_DONTWAIT);
        if (recv_rc == ZLINK_RECV_OK)
            completed = true;
        else {
            TEST_ASSERT_EQUAL_INT (ZLINK_RECV_NO_DATA, recv_rc);
            TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
        }
        if (completed)
            break;
        msleep (1);
    }
    TEST_ASSERT_TRUE (completed);
    TEST_ASSERT_EQUAL_UINT64 (completion_id, completion.completion_id);
    TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_OK, completion.request_result);
    zlink_completion_close (&completion);
    TEST_ASSERT_NOT_NULL (
      as_socket (fixture.dealer)->test_pair_pipe (
        fixture.pair_id, fixture.pair_generation, false));

    fixture.teardown ();
}

void test_inproc_peer_weight_is_owner_control_in_both_directions ()
{
    paired_fixture_t fixture;
    fixture.setup (0, 37, 0);

    //  Values configured before bind/connect are replayed once the exact pair
    //  is ready. Zero is policy, not absence of policy.
    TEST_ASSERT_TRUE (wait_for_paired_peer_weights (&fixture, 37, 0));

    //  Drain the receive scheduler before observing the steady-state path.
    //  Each subsequent message reactivates the ROUTER pipe, but an already
    //  adopted route must not retry pair-ready/peer-weight publication.
    char raw[32];
    TEST_ASSERT_EQUAL_INT (-1, flow_internal_recv (fixture.router, raw, sizeof (raw),
                                           ZLINK_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
    const uint64_t send_attempts_before =
      zlink::socket_base_t::test_local_peer_weight_send_attempt_count ();
    for (int i = 0; i != 8; ++i) {
        flow_internal_send_string (fixture.dealer, "steady", 0);
        char rid[256];
        const int rid_size = flow_internal_recv (fixture.router, rid, sizeof (rid), 0);
        TEST_ASSERT_GREATER_THAN_INT (0, rid_size);
        TEST_ASSERT_EQUAL_MEMORY (fixture.peer_rid.data (), rid,
                                  fixture.peer_rid.size ());
        flow_internal_recv_string (fixture.router, "steady", 0);
        TEST_ASSERT_EQUAL_INT (-1, flow_internal_recv (fixture.router, raw,
                                               sizeof (raw), ZLINK_DONTWAIT));
        TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
    }
    pump_socket_commands (fixture.dealer);
    pump_socket_commands (fixture.router);
    TEST_ASSERT_EQUAL_UINT64 (
      send_attempts_before,
      zlink::socket_base_t::test_local_peer_weight_send_attempt_count ());

    flow_unit_monitor_probe_t dealer_probe;
    flow_unit_monitor_probe_t router_probe;
    void *dealer_monitor = open_flow_unit_monitor_probe (
      fixture.dealer, ZLINK_EVENT_PEER_WEIGHT_CHANGED, &dealer_probe);
    void *router_monitor = open_flow_unit_monitor_probe (
      fixture.router, ZLINK_EVENT_PEER_WEIGHT_CHANGED, &router_probe);

    const int router_weight = 0;
    const int dealer_weight = 83;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_router_option (fixture.router, ZLINK_ROUTER_OPT_WEIGHT,
                               &router_weight, sizeof (router_weight)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_dealer_option (fixture.dealer, ZLINK_DEALER_OPT_WEIGHT,
                               &dealer_weight, sizeof (dealer_weight)));
    TEST_ASSERT_TRUE (wait_for_paired_peer_weights (
      &fixture, static_cast<uint32_t> (router_weight),
      static_cast<uint32_t> (dealer_weight)));

    TEST_ASSERT_TRUE (flow_unit_monitor_has_count (&dealer_probe, 1, 3000));
    TEST_ASSERT_TRUE (flow_unit_monitor_has_count (&router_probe, 1, 3000));
    const zlink_monitor_event_t dealer_event =
      flow_unit_monitor_record_at (&dealer_probe, 0);
    const zlink_monitor_event_t router_event =
      flow_unit_monitor_record_at (&router_probe, 0);
    TEST_ASSERT_EQUAL_UINT64 (ZLINK_EVENT_PEER_WEIGHT_CHANGED,
                              dealer_event.event);
    TEST_ASSERT_EQUAL_UINT64 (static_cast<uint64_t> (router_weight),
                              dealer_event.value);
    TEST_ASSERT_TRUE (dealer_event.connection_id != 0);
    TEST_ASSERT_TRUE (dealer_event.routing_id.size > 0);
    TEST_ASSERT_EQUAL_UINT (ZLINK_MONITOR_TRANSPORT_LANE_APPLICATION,
                            dealer_event.transport_lane);
    TEST_ASSERT_EQUAL_UINT64 (ZLINK_EVENT_PEER_WEIGHT_CHANGED,
                              router_event.event);
    TEST_ASSERT_EQUAL_UINT64 (static_cast<uint64_t> (dealer_weight),
                              router_event.value);
    TEST_ASSERT_TRUE (router_event.connection_id != 0);
    TEST_ASSERT_TRUE (router_event.routing_id.size > 0);
    TEST_ASSERT_EQUAL_UINT (ZLINK_MONITOR_TRANSPORT_LANE_APPLICATION,
                            router_event.transport_lane);

    // Absolute policy is deduplicated at the sender and again at the
    // scheduler boundary. Reapplying the same values creates neither a wire
    // command nor a duplicate monitor transition.
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_router_option (fixture.router, ZLINK_ROUTER_OPT_WEIGHT,
                               &router_weight, sizeof (router_weight)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_dealer_option (fixture.dealer, ZLINK_DEALER_OPT_WEIGHT,
                               &dealer_weight, sizeof (dealer_weight)));
    pump_socket_commands (fixture.dealer);
    pump_socket_commands (fixture.router);
    TEST_ASSERT_TRUE (
      flow_unit_monitor_has_no_additional (&dealer_probe, 1, 200));
    TEST_ASSERT_TRUE (
      flow_unit_monitor_has_no_additional (&router_probe, 1, 200));

    //  Neither internal delivery path can create an application record.
    TEST_ASSERT_EQUAL_INT (-1, flow_internal_recv (fixture.dealer, raw, sizeof (raw),
                                           ZLINK_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
    TEST_ASSERT_EQUAL_INT (-1, flow_internal_recv (fixture.router, raw, sizeof (raw),
                                           ZLINK_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
    zlink::pipe_t *dealer_control = as_socket (fixture.dealer)->test_pair_pipe (
      fixture.pair_id, fixture.pair_generation, false);
    uint64_t router_pair_id = 0;
    uint64_t router_generation = 0;
    TEST_ASSERT_TRUE (resolve_router_pair_identity (
      &fixture, &router_pair_id, &router_generation));
    zlink::pipe_t *router_control = as_socket (fixture.router)->test_pair_pipe (
      router_pair_id, router_generation, false);
    TEST_ASSERT_NOT_NULL (dealer_control);
    TEST_ASSERT_NOT_NULL (router_control);
    TEST_ASSERT_EQUAL_UINT (1u, dealer_control->get_transport_lane_count ());
    TEST_ASSERT_EQUAL_UINT (1u, router_control->get_transport_lane_count ());
    TEST_ASSERT_NULL (as_socket (fixture.dealer)->test_pair_pipe (
      fixture.pair_id, fixture.pair_generation, true));
    TEST_ASSERT_NULL (as_socket (fixture.router)->test_pair_pipe (
      router_pair_id, router_generation, true));
    TEST_ASSERT_FALSE (dealer_control->check_read ());
    TEST_ASSERT_FALSE (router_control->check_read ());

    close_flow_unit_monitor_probe (&router_monitor, &router_probe);
    close_flow_unit_monitor_probe (&dealer_monitor, &dealer_probe);
    fixture.teardown ();
}

//  Public option writes and socket-thread route/dispatch reads intentionally
//  use different synchronization domains. Exercise that boundary repeatedly;
//  this is also a focused TSAN regression for the policy scalar itself.
void test_peer_weight_update_is_safe_for_async_readers ()
{
    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    std::atomic<bool> start (false);
    std::atomic<bool> stop (false);
    std::atomic<bool> invalid_value_seen (false);
    std::atomic<uint64_t> read_count (0);

    std::thread reader ([&] {
        while (!start.load (std::memory_order_acquire))
            std::this_thread::yield ();
        while (!stop.load (std::memory_order_acquire)) {
            const uint32_t weight = as_socket (router)->local_peer_weight ();
            if (weight != 0 && weight != 37 && weight != 100)
                invalid_value_seen.store (true, std::memory_order_relaxed);
            read_count.fetch_add (1, std::memory_order_relaxed);
        }
    });

    start.store (true, std::memory_order_release);
    while (read_count.load (std::memory_order_relaxed) == 0)
        std::this_thread::yield ();
    bool update_failed = false;
    for (int i = 0; i != 4096; ++i) {
        const int weight = (i & 1) ? 37 : 0;
        if (zlink_set_router_option (router, ZLINK_ROUTER_OPT_WEIGHT, &weight,
                                     sizeof (weight))
            != ZLINK_CONFIG_OK) {
            update_failed = true;
            break;
        }
        if ((i & 63) == 0)
            std::this_thread::yield ();
    }
    const int final_weight = 37;
    if (zlink_set_router_option (router, ZLINK_ROUTER_OPT_WEIGHT,
                                 &final_weight, sizeof (final_weight))
        != ZLINK_CONFIG_OK)
        update_failed = true;
    stop.store (true, std::memory_order_release);
    reader.join ();

    TEST_ASSERT_FALSE (update_failed);
    TEST_ASSERT_FALSE (invalid_value_seen.load (std::memory_order_relaxed));
    TEST_ASSERT_GREATER_THAN_UINT64 (0,
                                     read_count.load (
                                       std::memory_order_relaxed));
    TEST_ASSERT_EQUAL_UINT32 (37, as_socket (router)->local_peer_weight ());
    test_context_socket_close_zero_linger (router);
}

void test_pair_replacement_keeps_exact_peer_weight_state ()
{
    paired_fixture_t fixture;
    fixture.setup (0, 23, 0);
    TEST_ASSERT_TRUE (wait_for_paired_peer_weights (&fixture, 23, 0));

    const int router_weight = 71;
    const int dealer_weight = 19;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_router_option (fixture.router, ZLINK_ROUTER_OPT_WEIGHT,
                               &router_weight, sizeof (router_weight)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_dealer_option (fixture.dealer, ZLINK_DEALER_OPT_WEIGHT,
                               &dealer_weight, sizeof (dealer_weight)));
    TEST_ASSERT_TRUE (wait_for_paired_peer_weights (
      &fixture, static_cast<uint32_t> (router_weight),
      static_cast<uint32_t> (dealer_weight)));

    // This case reconnects before the old inbound route has necessarily
    // retired. Select the contract that admits that same-RID overlap; the
    // default duplicate-reject policy intentionally leaves the replacement
    // pair outside the ROUTER scheduler.
    const int handover = ZLINK_RID_DUPLICATE_HANDOVER;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (
      fixture.router, ZLINK_OPT_RID_DUPLICATE_POLICY, &handover,
      sizeof (handover)));
    const uint64_t old_pair_id = fixture.pair_id;
    const uint64_t old_generation = fixture.pair_generation;
    fixture.replace_pair ();
    TEST_ASSERT_TRUE (fixture.resolve_reconnected_dealer_target (
      old_pair_id, old_generation));
    TEST_ASSERT_TRUE (wait_for_paired_peer_weights (
      &fixture, static_cast<uint32_t> (router_weight),
      static_cast<uint32_t> (dealer_weight)));

    zlink::pipe_t *dealer_control = as_socket (fixture.dealer)->test_pair_pipe (
      fixture.pair_id, fixture.pair_generation, false);
    uint64_t router_pair_id = 0;
    uint64_t router_generation = 0;
    TEST_ASSERT_TRUE (resolve_router_pair_identity (
      &fixture, &router_pair_id, &router_generation));
    zlink::pipe_t *router_control = as_socket (fixture.router)->test_pair_pipe (
      router_pair_id, router_generation, false);
    TEST_ASSERT_NOT_NULL (dealer_control);
    TEST_ASSERT_NOT_NULL (router_control);
    TEST_ASSERT_EQUAL_UINT (1u, dealer_control->get_transport_lane_count ());
    TEST_ASSERT_EQUAL_UINT (1u, router_control->get_transport_lane_count ());
    TEST_ASSERT_NULL (as_socket (fixture.dealer)->test_pair_pipe (
      fixture.pair_id, fixture.pair_generation, true));
    TEST_ASSERT_NULL (as_socket (fixture.router)->test_pair_pipe (
      router_pair_id, router_generation, true));
    TEST_ASSERT_FALSE (dealer_control->check_read ());
    TEST_ASSERT_FALSE (router_control->check_read ());
    fixture.teardown ();
}

//  The negotiated lane count selects the only legal FLOWSTATE source. A count-1
//  DEALER-ROUTER pair accepts Application; a count-2 ROUTER-ROUTER pair rejects
//  Application and accepts Completion. Rejection must not consume the epoch.
void test_flow_frame_uses_count_selected_control_lane ()
{
    {
        paired_fixture_t fixture;
        fixture.setup ();

        zlink::pipe_t *const application =
          as_socket (fixture.dealer)
            ->test_pair_pipe (fixture.pair_id, fixture.pair_generation, false);
        TEST_ASSERT_NOT_NULL (application);
        TEST_ASSERT_EQUAL_UINT (1u, application->get_transport_lane_count ());
        TEST_ASSERT_NULL (as_socket (fixture.dealer)->test_pair_pipe (
          fixture.pair_id, fixture.pair_generation, true));

        zlink::flow_state::frame_t frame;
        frame.version = zlink::flow_state::frame_protocol_version;
        frame.state = k_paused;
        frame.epoch = 7;
        zlink::msg_t msg;
        TEST_ASSERT_EQUAL_INT (0, msg.init ());
        TEST_ASSERT_EQUAL_INT (0, zlink::flow_state::init_frame (&msg, frame));
        msg.set_transport_connection_id (
          application->get_transport_connection_id ());
        TEST_ASSERT_TRUE (
          as_socket (fixture.dealer)
            ->consume_receive_flow_state_frame (application, msg));
        TEST_ASSERT_EQUAL_INT (0, msg.close ());
        TEST_ASSERT_TRUE (fixture.wait_for_applied_pause (true));

        fixture.teardown ();
    }

    {
        const int zero = 0;
        const char first_rid_text[] = "flow-router-a";
        const char second_rid_text[] = "flow-router-b";
        void *first = test_context_socket (ZLINK_SOCKET_ROUTER);
        void *second = test_context_socket (ZLINK_SOCKET_ROUTER);
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_set_option (first, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_set_option (second, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (
          first, first_rid_text, sizeof (first_rid_text) - 1));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (
          second, second_rid_text, sizeof (second_rid_text) - 1));
        TEST_ASSERT_EQUAL_INT (
          ZLINK_CONFIG_OK,
          zlink_set_router_option (second,
                                   ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID,
                                   first_rid_text,
                                   sizeof (first_rid_text) - 1));
        contract_socket_pair_t local_pair (first, second);

        flow_internal_send_string (second, first_rid_text, ZLINK_SNDMORE);
        flow_internal_send_string (second, "prime", 0);
        flow_internal_recv_string (first, second_rid_text, 0);
        flow_internal_recv_string (first, "prime", 0);

        zlink_routing_id_t first_rid;
        TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                               zlink_get_routing_id (first, &first_rid));
        zlink_routed_submit_target_t target;
        memset (&target, 0, sizeof (target));
        bool resolved = false;
        const std::chrono::steady_clock::time_point deadline =
          deadline_in_ms (2000);
        while (!deadline_expired (deadline)) {
            if (as_socket (second)->select_routed_submit_target (&first_rid,
                                                                 &target)
                  == 0
                && target.transport_pair_id != 0) {
                resolved = true;
                break;
            }
            msleep (1);
        }
        TEST_ASSERT_TRUE (resolved);

        zlink::pipe_t *const application =
          as_socket (second)->test_pair_pipe (target.transport_pair_id,
                                              target.transport_pair_generation,
                                              false);
        zlink::pipe_t *const completion =
          as_socket (second)->test_pair_pipe (target.transport_pair_id,
                                              target.transport_pair_generation,
                                              true);
        TEST_ASSERT_NOT_NULL (application);
        TEST_ASSERT_NOT_NULL (completion);
        TEST_ASSERT_EQUAL_UINT (2u, application->get_transport_lane_count ());
        TEST_ASSERT_EQUAL_UINT (2u, completion->get_transport_lane_count ());
        TEST_ASSERT_EQUAL_INT (zlink::transport_lane_application,
                               application->get_transport_lane ());
        TEST_ASSERT_EQUAL_INT (zlink::transport_lane_completion,
                               completion->get_transport_lane ());
        TEST_ASSERT_EQUAL_PTR (
          completion,
          as_socket (second)->completion_pipe_for_transport_pair (
            target.transport_pair_id, target.transport_pair_generation));

        zlink::flow_state::frame_t frame;
        frame.version = zlink::flow_state::frame_protocol_version;
        frame.state = k_paused;
        frame.epoch = 7;
        zlink::msg_t application_msg;
        TEST_ASSERT_EQUAL_INT (0, application_msg.init ());
        TEST_ASSERT_EQUAL_INT (
          0, zlink::flow_state::init_frame (&application_msg, frame));
        application_msg.set_transport_connection_id (
          application->get_transport_connection_id ());
        TEST_ASSERT_TRUE (
          as_socket (second)->consume_receive_flow_state_frame (
            application, application_msg));
        TEST_ASSERT_EQUAL_INT (0, application_msg.close ());
        for (int i = 0; i != 50; ++i) {
            (void) contract_socket_pair_t::pump_owner (as_socket (second));
            msleep (1);
        }
        TEST_ASSERT_FALSE (as_socket (second)->remote_receive_flow_paused (
          target.transport_pair_id, target.transport_pair_generation));

        zlink::msg_t completion_msg;
        TEST_ASSERT_EQUAL_INT (0, completion_msg.init ());
        TEST_ASSERT_EQUAL_INT (
          0, zlink::flow_state::init_frame (&completion_msg, frame));
        completion_msg.set_transport_connection_id (
          completion->get_transport_connection_id ());
        TEST_ASSERT_TRUE (
          as_socket (second)->consume_receive_flow_state_frame (
            completion, completion_msg));
        TEST_ASSERT_EQUAL_INT (0, completion_msg.close ());
        TEST_ASSERT_TRUE (wait_for_pipe_pause (
          second, target.transport_pair_id, target.transport_pair_generation,
          true));

        test_context_socket_close_zero_linger (second);
        test_context_socket_close_zero_linger (first);
    }
}

//  Review finding 3. A classic ROUTER send accepts the routing-ID part without
//  writing it to the pipe, so the pipe sees no message in progress. A PAUSE
//  that lands between the routing-ID part and the first payload part must not
//  break the message that the socket has already accepted.
void test_router_routing_id_part_holds_message_atomicity_across_pause ()
{
    const int zero = 0;
    const int mandatory = 1;

    void *router = test_context_socket (ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (router, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_router_option (
      router, ZLINK_ROUTER_OPT_MANDATORY, &mandatory, sizeof (mandatory)));

    void *dealer = test_context_socket (ZLINK_SOCKET_DEALER);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (dealer, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    contract_socket_pair_t local_pair (dealer, router);

    flow_internal_send_string (dealer, "hello", 0);
    char rid[256];
    const int rid_size = flow_internal_recv (router, rid, sizeof (rid), 0);
    TEST_ASSERT_GREATER_THAN_INT (0, rid_size);
    flow_internal_recv_string (router, "hello", 0);

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
    TEST_ASSERT_EQUAL_INT (rid_size, flow_internal_send (router, rid, rid_size,
                                                 ZLINK_SNDMORE | ZLINK_DONTWAIT));

    TEST_ASSERT_TRUE (
      as_socket (router)->test_deliver_flow_state_command (
        target.transport_pair_id, target.transport_pair_generation, 1, 1));
    TEST_ASSERT_TRUE (wait_for_pipe_pause (router, target.transport_pair_id,
                                           target.transport_pair_generation,
                                           true));

    //  The started message must still complete.
    TEST_ASSERT_EQUAL_INT (7, flow_internal_send (router, "payload", 7, ZLINK_DONTWAIT));
    flow_internal_recv_string (dealer, "payload", 0);

    //  The next message is blocked, from its very first part.
    TEST_ASSERT_EQUAL_INT (
      -1, flow_internal_send (router, rid, rid_size, ZLINK_SNDMORE | ZLINK_DONTWAIT));
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
    fixture.setup (512);

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
      flow_internal_send (fixture.dealer, oversize.data (),
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
    TEST_ASSERT_TRUE (wait_for_send_success (fixture.dealer, 5000));

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
        fixture.pair->pump ();
        msleep (1);
    }

    TEST_ASSERT_FALSE (as_socket (fixture.dealer)->remote_receive_flow_paused (
      fixture.pair_id, fixture.pair_generation));
    TEST_ASSERT_FALSE (
      as_socket (fixture.dealer)->application_pipe_remote_flow_paused (
        fixture.pair_id, fixture.pair_generation));
    TEST_ASSERT_TRUE (wait_for_send_success (fixture.dealer, 5000));

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
          flow_internal_recv (fixture.dealer, buffer, sizeof (buffer), ZLINK_DONTWAIT);
        TEST_ASSERT_EQUAL_INT (-1, rc);
        TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
        msleep (5);
    }

    //  Ordinary traffic still flows in both directions and carries only the
    //  application's own payload.
    TEST_ASSERT_TRUE (wait_for_send_success (fixture.dealer, 5000));
    char rid[256];
    const int rid_size = flow_internal_recv (fixture.router, rid, sizeof (rid), 0);
    TEST_ASSERT_GREATER_THAN_INT (0, rid_size);
    flow_internal_recv_string (fixture.router, "payload", 0);

    TEST_ASSERT_EQUAL_INT (rid_size, flow_internal_send (fixture.router, rid, rid_size,
                                                 ZLINK_SNDMORE));
    flow_internal_send_string (fixture.router, "reply", 0);
    flow_internal_recv_string (fixture.dealer, "reply", 0);

    fixture.teardown ();
}
}

int main ()
{
    setup_test_environment ();
    const char *selected_case = getenv ("ZLINK_TEST_CASE");
#define RUN_FLOW_CASE(TEST_FN)                                                                    \
    do {                                                                                          \
        if (!selected_case || strcmp (selected_case, #TEST_FN) == 0)                             \
            RUN_TEST (TEST_FN);                                                                   \
    } while (false)

    UNITY_BEGIN ();
    RUN_FLOW_CASE (test_unsupported_socket_types_report_not_supported);
    RUN_FLOW_CASE (test_invalid_state_is_rejected);
    RUN_FLOW_CASE (test_remote_pause_blocks_sender_and_resume_releases_it);
    RUN_FLOW_CASE (test_local_hwm_and_remote_pause_are_independent);
    RUN_FLOW_CASE (test_pause_mid_multipart_preserves_atomicity);
    RUN_FLOW_CASE (test_duplicate_and_stale_frames_are_ignored);
    RUN_FLOW_CASE (test_new_and_reconnected_pairs_receive_the_latest_state);
    RUN_FLOW_CASE (test_flow_state_epoch_edge_cases);
    RUN_FLOW_CASE (test_generation_change_resets_the_epoch_sequence);
    RUN_FLOW_CASE (test_flow_frame_cannot_complete_a_truncated_reply);
    RUN_FLOW_CASE (test_epoch_zero_is_refused_by_the_pipe_command);
    RUN_FLOW_CASE (test_epoch_wraparound_forces_a_new_connection_generation);
    RUN_FLOW_CASE (test_flow_frame_before_registration_is_promoted_after_validation);
    RUN_FLOW_CASE (test_resume_rereads_credit_published_before_the_waiter_was_armed);
    RUN_FLOW_CASE (test_router_peer_state_reports_remote_pause);
    RUN_FLOW_CASE (test_flow_frame_before_reply_is_consumed_on_a_local_pair);
    RUN_FLOW_CASE (test_peer_weight_change_does_not_leak_to_public_receive);
    RUN_FLOW_CASE (test_inproc_peer_weight_is_owner_control_in_both_directions);
    RUN_FLOW_CASE (test_peer_weight_update_is_safe_for_async_readers);
    RUN_FLOW_CASE (test_pair_replacement_keeps_exact_peer_weight_state);
    RUN_FLOW_CASE (test_flow_frame_uses_count_selected_control_lane);
    RUN_FLOW_CASE (test_router_routing_id_part_holds_message_atomicity_across_pause);
    RUN_FLOW_CASE (test_resume_while_hwm_full_still_recovers_through_byte_credit);
    RUN_FLOW_CASE (test_stale_flow_state_command_cannot_override_a_newer_epoch);
    RUN_FLOW_CASE (test_no_application_recv_returns_a_flow_frame);
    const int rc = UNITY_END ();
#undef RUN_FLOW_CASE
    return rc;
}
