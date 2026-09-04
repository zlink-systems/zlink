/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"
#include "api/socket/socket_api_internal.hpp"
#include "core/flow_state_frame.hpp"
#include "core/object.hpp"
#include "core/pipe.hpp"
#include "sockets/internal/dist.hpp"
#include "sockets/internal/lb.hpp"
#include "sockets/dealer/dealer.hpp"

#include <unity.h>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

void setUp ()
{
    setup_test_context ();
}

void tearDown ()
{
    teardown_test_context ();
}

namespace
{
class pipe_cleanup_sink_t : public zlink::i_pipe_events
{
  public:
    pipe_cleanup_sink_t () : terminated_count (0), write_activated_count (0) {}

    void read_activated (zlink::pipe_t *) ZLINK_OVERRIDE {}
    void write_activated (zlink::pipe_t *) ZLINK_OVERRIDE
    {
        ++write_activated_count;
    }
    void hiccuped (zlink::pipe_t *) ZLINK_OVERRIDE {}
    void pipe_peer_terminated (zlink::pipe_t *) ZLINK_OVERRIDE {}
    void pipe_terminated (zlink::pipe_t *) ZLINK_OVERRIDE
    {
        ++terminated_count;
    }

    int terminated_count;
    int write_activated_count;
};

void *create_sync_socket (int type_)
{
    void *socket = zlink_socket (get_test_context (), static_cast<zlink_socket_type_t> (type_));
    TEST_ASSERT_NOT_NULL (socket);
    return socket;
}

void close_sync_socket (void *socket_)
{
    TEST_ASSERT_SUCCESS_ERRNO (zlink_close (socket_));
}

zlink_auto_hwm_budget_snapshot_t read_budget_snapshot ()
{
    zlink_auto_hwm_budget_snapshot_t snapshot;
    memset (&snapshot, 0, sizeof (snapshot));
    snapshot.abi_version = ZLINK_AUTO_HWM_BUDGET_SNAPSHOT_ABI_V1;
    snapshot.struct_size = sizeof (snapshot);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_ctx_get_auto_hwm_budget_snapshot (get_test_context (), &snapshot));
    return snapshot;
}
}

void test_router_multiple_dealers_tcp ()
{
    void *router = create_sync_socket (ZLINK_SOCKET_ROUTER);
    void *dealer1 = create_sync_socket (ZLINK_SOCKET_DEALER);
    void *dealer2 = create_sync_socket (ZLINK_SOCKET_DEALER);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer1, "D1", 2));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer2, "D2", 2));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (router, "tcp://127.0.0.1:*"));

    char endpoint[MAX_SOCKET_STRING];
    size_t len = sizeof (endpoint);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_get_option (router, ZLINK_OPT_LAST_ENDPOINT, endpoint, &len));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer1, endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer2, endpoint));

    msleep (SETTLE_TIME);

    // Both dealers send messages
    send_string_expect_success (dealer1, "from_dealer1", 0);
    recv_string_expect_success (router, "D1", 0);
    recv_string_expect_success (router, "from_dealer1", 0);
    send_string_expect_success (dealer2, "from_dealer2", 0);
    recv_string_expect_success (router, "D2", 0);
    recv_string_expect_success (router, "from_dealer2", 0);

    // Router can reply to specific dealer
    TEST_ASSERT_SUCCESS_ERRNO (zlink_send (router, "D1", 2, ZLINK_SNDMORE));
    send_string_expect_success (router, "reply_to_d1", 0);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_send (router, "D2", 2, ZLINK_SNDMORE));
    send_string_expect_success (router, "reply_to_d2", 0);

    // Dealers receive their specific replies
    recv_string_expect_success (dealer1, "reply_to_d1", 0);
    recv_string_expect_success (dealer2, "reply_to_d2", 0);

    close_sync_socket (dealer2);
    close_sync_socket (dealer1);
    close_sync_socket (router);
}

void test_router_multiple_dealers_ipc ()
{
#if defined(ZLINK_HAVE_IPC)
    void *router = create_sync_socket (ZLINK_SOCKET_ROUTER);
    void *dealer1 = create_sync_socket (ZLINK_SOCKET_DEALER);
    void *dealer2 = create_sync_socket (ZLINK_SOCKET_DEALER);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer1, "D1", 2));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer2, "D2", 2));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (router, "ipc://*"));

    char endpoint[MAX_SOCKET_STRING];
    size_t len = sizeof (endpoint);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_get_option (router, ZLINK_OPT_LAST_ENDPOINT, endpoint, &len));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer1, endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer2, endpoint));

    msleep (SETTLE_TIME);

    // Both dealers send messages
    send_string_expect_success (dealer1, "from_dealer1", 0);
    recv_string_expect_success (router, "D1", 0);
    recv_string_expect_success (router, "from_dealer1", 0);
    send_string_expect_success (dealer2, "from_dealer2", 0);
    recv_string_expect_success (router, "D2", 0);
    recv_string_expect_success (router, "from_dealer2", 0);

    // Router replies to specific dealers
    TEST_ASSERT_SUCCESS_ERRNO (zlink_send (router, "D1", 2, ZLINK_SNDMORE));
    send_string_expect_success (router, "reply_to_d1", 0);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_send (router, "D2", 2, ZLINK_SNDMORE));
    send_string_expect_success (router, "reply_to_d2", 0);

    recv_string_expect_success (dealer1, "reply_to_d1", 0);
    recv_string_expect_success (dealer2, "reply_to_d2", 0);

    close_sync_socket (dealer2);
    close_sync_socket (dealer1);
    close_sync_socket (router);
#else
    TEST_IGNORE_MESSAGE ("IPC not supported on this platform");
#endif
}

void test_router_multiple_dealers_inproc ()
{
    void *router = create_sync_socket (ZLINK_SOCKET_ROUTER);
    void *dealer1 = create_sync_socket (ZLINK_SOCKET_DEALER);
    void *dealer2 = create_sync_socket (ZLINK_SOCKET_DEALER);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer1, "D1", 2));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_routing_id (dealer2, "D2", 2));

    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (router, "inproc://test_router_multi_dealers"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer1, "inproc://test_router_multi_dealers"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer2, "inproc://test_router_multi_dealers"));

    // Both dealers send messages
    send_string_expect_success (dealer1, "from_dealer1", 0);
    recv_string_expect_success (router, "D1", 0);
    recv_string_expect_success (router, "from_dealer1", 0);
    send_string_expect_success (dealer2, "from_dealer2", 0);
    recv_string_expect_success (router, "D2", 0);
    recv_string_expect_success (router, "from_dealer2", 0);

    // Router replies to specific dealers
    TEST_ASSERT_SUCCESS_ERRNO (zlink_send (router, "D1", 2, ZLINK_SNDMORE));
    send_string_expect_success (router, "reply_to_d1", 0);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_send (router, "D2", 2, ZLINK_SNDMORE));
    send_string_expect_success (router, "reply_to_d2", 0);

    recv_string_expect_success (dealer1, "reply_to_d1", 0);
    recv_string_expect_success (dealer2, "reply_to_d2", 0);

    close_sync_socket (dealer2);
    close_sync_socket (dealer1);
    close_sync_socket (router);
}

void test_weighted_dealer_preserves_peer_weight_after_backpressure ()
{
    void *dealer = create_sync_socket (ZLINK_SOCKET_DEALER);
    void *router1 = create_sync_socket (ZLINK_SOCKET_ROUTER);
    void *router2 = create_sync_socket (ZLINK_SOCKET_ROUTER);

    const uint64_t hwm = 64u + sizeof (zlink_msg_t);
    const int timeout = 0;
    const int weight1 = 25;
    const int weight2 = 100;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (dealer, ZLINK_OPT_SNDHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (router1, ZLINK_OPT_RCVHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (router2, ZLINK_OPT_RCVHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (dealer, ZLINK_OPT_SNDTIMEO, &timeout, sizeof (timeout)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_router_option (
        router1, ZLINK_ROUTER_OPT_WEIGHT, &weight1, sizeof (weight1)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_router_option (
        router2, ZLINK_ROUTER_OPT_WEIGHT, &weight2, sizeof (weight2)));

    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (router1, "inproc://weighted-backpressure-1"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (router2, "inproc://weighted-backpressure-2"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (dealer, "inproc://weighted-backpressure-1"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (dealer, "inproc://weighted-backpressure-2"));
    msleep (SETTLE_TIME);

    bool backpressured = false;
    for (int i = 0; i < 1000; ++i) {
        if (zlink_send (dealer, "fill", 4, ZLINK_DONTWAIT) == -1) {
            TEST_ASSERT_TRUE (errno == EAGAIN || errno == ECONNREFUSED);
            backpressured = true;
            break;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE (
      backpressured, "weighted dealer did not reach the backpressure path");

    char buffer[16];
    while (zlink_recv (router1, buffer, sizeof (buffer), ZLINK_DONTWAIT) >= 0) {
    }
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);
    while (zlink_recv (router2, buffer, sizeof (buffer), ZLINK_DONTWAIT) >= 0) {
    }
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);
    msleep (SETTLE_TIME);
    while (zlink_recv (router1, buffer, sizeof (buffer), ZLINK_DONTWAIT) >= 0) {
    }
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);
    while (zlink_recv (router2, buffer, sizeof (buffer), ZLINK_DONTWAIT) >= 0) {
    }
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);

    int router1_received = 0;
    int router2_received = 0;
    for (int i = 0; i < 40; ++i) {
        int send_rc = -1;
        for (int attempt = 0; attempt < 1000 && send_rc == -1; ++attempt) {
            send_rc = zlink_send (dealer, "next", 4, ZLINK_DONTWAIT);
            if (send_rc == -1)
                msleep (1);
        }
        TEST_ASSERT_EQUAL_INT (4, send_rc);

        bool received = false;
        for (int attempt = 0; attempt < 1000 && !received; ++attempt) {
            void *routers[] = {router1, router2};
            int *counts[] = {&router1_received, &router2_received};
            for (size_t router_index = 0; router_index < 2; ++router_index) {
                const int rid_size =
                  zlink_recv (routers[router_index], buffer, sizeof (buffer), ZLINK_DONTWAIT);
                if (rid_size < 0)
                    continue;
                TEST_ASSERT_GREATER_THAN_INT (0, rid_size);
                TEST_ASSERT_EQUAL_INT (
                  4, zlink_recv (routers[router_index], buffer, sizeof (buffer), 0));
                TEST_ASSERT_EQUAL_MEMORY ("next", buffer, 4);
                *counts[router_index] += 1;
                received = true;
                break;
            }
            if (!received)
                msleep (1);
        }
        TEST_ASSERT_TRUE (received);
    }

    TEST_ASSERT_GREATER_THAN_INT (
      0, router1_received);
    TEST_ASSERT_GREATER_THAN_INT (
      0, router2_received);

    close_sync_socket (router2);
    close_sync_socket (router1);
    close_sync_socket (dealer);
}

int recv_one_weighted_router_index (void *router1_, void *router2_)
{
    char buffer[32];
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (3);
    while (std::chrono::steady_clock::now () < deadline) {
        void *routers[] = {router1_, router2_};
        for (size_t i = 0; i < 2; ++i) {
            const int rid_size =
              zlink_recv (routers[i], buffer, sizeof (buffer), ZLINK_DONTWAIT);
            if (rid_size < 0)
                continue;
            TEST_ASSERT_GREATER_THAN_INT (0, rid_size);
            TEST_ASSERT_EQUAL_INT (
              1, zlink_recv (routers[i], buffer, sizeof (buffer), 0));
            TEST_ASSERT_EQUAL_MEMORY ("x", buffer, 1);
            return static_cast<int> (i);
        }
        msleep (1);
    }
    return -1;
}

void process_socket_control_commands (void *socket_)
{
    int events = 0;
    size_t events_size = sizeof (events);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_option (socket_, ZLINK_OPT_EVENTS, &events, &events_size));
}

bool wait_for_unpaired_peer_weights (void *dealer_, void *router1_,
                                     void *router2_, uint32_t first_,
                                     uint32_t second_)
{
    zlink::dealer_t *const dealer = static_cast<zlink::dealer_t *> (
      as_socket_handle (dealer_).socket);
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (3);
    while (std::chrono::steady_clock::now () < deadline) {
        process_socket_control_commands (router1_);
        process_socket_control_commands (router2_);
        process_socket_control_commands (dealer_);
        if (dealer->test_peer_weight_count (first_) == 1
            && dealer->test_peer_weight_count (second_) == 1)
            return true;
        msleep (1);
    }
    return false;
}

void test_unpaired_inproc_peer_weight_is_not_application_data ()
{
    void *dealer = create_sync_socket (ZLINK_SOCKET_DEALER);
    void *router1 = create_sync_socket (ZLINK_SOCKET_ROUTER);
    void *router2 = create_sync_socket (ZLINK_SOCKET_ROUTER);
    const int weight1 = 1;
    const int weight2 = 3;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_router_option (router1, ZLINK_ROUTER_OPT_WEIGHT, &weight1,
                               sizeof (weight1)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_router_option (router2, ZLINK_ROUTER_OPT_WEIGHT, &weight2,
                               sizeof (weight2)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (router1, "inproc://weighted-owner-control-1"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (router2, "inproc://weighted-owner-control-2"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (dealer, "inproc://weighted-owner-control-1"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (dealer, "inproc://weighted-owner-control-2"));

    TEST_ASSERT_TRUE (
      wait_for_unpaired_peer_weights (dealer, router1, router2, 1, 3));
    char raw[32];
    TEST_ASSERT_EQUAL_INT (
      -1, zlink_recv (dealer, raw, sizeof (raw), ZLINK_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());

    int counts[2] = {0, 0};
    for (int i = 0; i < 40; ++i) {
        send_string_expect_success (dealer, "x", 0);
        const int selected = recv_one_weighted_router_index (router1, router2);
        TEST_ASSERT_TRUE (selected == 0 || selected == 1);
        ++counts[selected];
    }
    TEST_ASSERT_EQUAL_INT (10, counts[0]);
    TEST_ASSERT_EQUAL_INT (30, counts[1]);

    //  Zero removes one peer from selection immediately. The update still
    //  travels only as an owner command, so a raw receive remains empty.
    const int zero = 0;
    const int one = 1;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_router_option (router1, ZLINK_ROUTER_OPT_WEIGHT, &zero,
                               sizeof (zero)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_router_option (router2, ZLINK_ROUTER_OPT_WEIGHT, &one,
                               sizeof (one)));
    TEST_ASSERT_TRUE (
      wait_for_unpaired_peer_weights (dealer, router1, router2, 0, 1));
    for (int i = 0; i < 12; ++i) {
        send_string_expect_success (dealer, "x", 0);
        TEST_ASSERT_EQUAL_INT (
          1, recv_one_weighted_router_index (router1, router2));
    }
    TEST_ASSERT_EQUAL_INT (
      -1, zlink_recv (dealer, raw, sizeof (raw), ZLINK_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());

    close_sync_socket (router2);
    close_sync_socket (router1);
    close_sync_socket (dealer);
}

void test_peer_control_does_not_complete_open_application_multipart ()
{
    void *owner_handle = create_sync_socket (ZLINK_SOCKET_PAIR);
    zlink::object_t *owner = static_cast<zlink::object_t *> (
      as_socket_handle (owner_handle).socket);
    zlink::object_t *parents[] = {owner, owner};
    const uint64_t hwms[] = {4096, 4096};
    const bool conflate[] = {false, false};
    zlink::pipe_t *pipes[2];
    TEST_ASSERT_SUCCESS_ERRNO (zlink::pipepair (
      parents, pipes, hwms, conflate, true,
      zlink::transport_lane_application, zlink::auto_hwm_role_none, false,
      zlink::physical_queue_class_application, 0));
    pipes[0]->set_transport_pair (zlink::transport_lane_application, 17, 1);
    pipes[1]->set_transport_pair (zlink::transport_lane_application, 17, 1);
    pipes[0]->set_transport_lane_count (1);
    pipes[1]->set_transport_lane_count (1);
    pipes[0]->set_peer_socket_type (ZLINK_CORE_SOCKET_ROUTER);
    pipes[1]->set_peer_socket_type (ZLINK_CORE_SOCKET_DEALER);
    pipes[0]->set_max_message_bytes (12);

    pipe_cleanup_sink_t cleanup_sink;
    pipes[0]->set_event_sink (&cleanup_sink);
    pipes[1]->set_event_sink (&cleanup_sink);

    zlink::msg_t first;
    TEST_ASSERT_SUCCESS_ERRNO (first.init_size (6));
    memset (first.data (), 'a', first.size ());
    first.set_flags (zlink::msg_t::more);
    TEST_ASSERT_TRUE (pipes[0]->write (&first));

    TEST_ASSERT_TRUE (pipes[0]->write_flow_state_control_and_flush (
      zlink::flow_state::receive_flow_paused, 1));
    static const unsigned char weight_command[] = {
      'W', 'E', 'I', 'G', 'H', 'T', 0, 0, 0, 7};
    TEST_ASSERT_TRUE (pipes[0]->write_peer_weight_control_and_flush (7));
    TEST_ASSERT_TRUE (pipes[0]->write_flow_state_control_and_flush (
      zlink::flow_state::receive_flow_running, 2));

    const uint64_t control_bytes =
      2 * sizeof (zlink::msg_t) + sizeof (weight_command)
      + zlink::flow_state::frame_size;
    TEST_ASSERT_EQUAL_UINT64 (0, pipes[0]->get_msgs_written ());
    TEST_ASSERT_EQUAL_UINT64 (0, pipes[0]->get_bytes_written ());

    zlink::msg_t received;
    TEST_ASSERT_SUCCESS_ERRNO (received.init ());
    // A terminal control must not move ypipe's commit boundary over an open
    // Application multipart. Neither the prefix nor WEIGHT is visible yet.
    TEST_ASSERT_FALSE (pipes[1]->check_read ());
    TEST_ASSERT_FALSE (pipes[1]->read (&received));
    TEST_ASSERT_SUCCESS_ERRNO (received.close ());
    TEST_ASSERT_EQUAL_UINT64 (0, pipes[1]->get_msgs_read ());
    TEST_ASSERT_EQUAL_UINT64 (0, pipes[1]->get_bytes_read ());

    zlink::msg_t final;
    TEST_ASSERT_SUCCESS_ERRNO (final.init_size (6));
    memset (final.data (), 'b', final.size ());
    TEST_ASSERT_TRUE (pipes[0]->write_and_flush (&final));

    TEST_ASSERT_SUCCESS_ERRNO (received.init ());
    TEST_ASSERT_TRUE (pipes[1]->read (&received));
    TEST_ASSERT_TRUE ((received.flags () & zlink::msg_t::more) != 0);
    TEST_ASSERT_EQUAL_UINT64 (6, received.size ());
    TEST_ASSERT_SUCCESS_ERRNO (received.close ());
    TEST_ASSERT_EQUAL_UINT64 (0, pipes[1]->get_msgs_read ());
    TEST_ASSERT_EQUAL_UINT64 (0, pipes[1]->get_bytes_read ());

    TEST_ASSERT_SUCCESS_ERRNO (received.init ());
    TEST_ASSERT_TRUE (pipes[1]->read (&received));
    TEST_ASSERT_FALSE ((received.flags ()
                        & (zlink::msg_t::more | zlink::msg_t::command))
                       != 0);
    TEST_ASSERT_EQUAL_UINT64 (6, received.size ());
    TEST_ASSERT_SUCCESS_ERRNO (received.close ());
    TEST_ASSERT_EQUAL_UINT64 (1, pipes[1]->get_msgs_read ());

    TEST_ASSERT_SUCCESS_ERRNO (received.init ());
    TEST_ASSERT_TRUE (pipes[1]->read (&received));
    TEST_ASSERT_TRUE ((received.flags () & zlink::msg_t::command) != 0);
    TEST_ASSERT_EQUAL_MEMORY (
      weight_command, received.data (), sizeof (weight_command));
    TEST_ASSERT_SUCCESS_ERRNO (received.close ());

    TEST_ASSERT_SUCCESS_ERRNO (received.init ());
    TEST_ASSERT_TRUE (pipes[1]->read (&received));
    zlink::flow_state::frame_t flow_frame;
    TEST_ASSERT_EQUAL_INT (
      zlink::flow_state::decode_ok,
      zlink::flow_state::decode_frame (received, &flow_frame));
    TEST_ASSERT_EQUAL_UINT8 (zlink::flow_state::receive_flow_running,
                             flow_frame.state);
    TEST_ASSERT_EQUAL_UINT64 (2, flow_frame.epoch);
    TEST_ASSERT_SUCCESS_ERRNO (received.close ());
    TEST_ASSERT_FALSE (pipes[1]->check_read ());

    const uint64_t application_bytes =
      2 * sizeof (zlink::msg_t) + first.size () + final.size ();
    TEST_ASSERT_EQUAL_UINT64 (3, pipes[0]->get_msgs_written ());
    TEST_ASSERT_EQUAL_UINT64 (
      control_bytes + application_bytes, pipes[0]->get_bytes_written ());
    TEST_ASSERT_EQUAL_UINT64 (3, pipes[1]->get_msgs_read ());
    TEST_ASSERT_EQUAL_UINT64 (
      control_bytes + application_bytes, pipes[1]->get_bytes_read ());

    // Rollback removes the unpublished Application prefix, then publishes the
    // deferred absolute control at a standalone boundary. A later fresh record
    // must not inherit any bytes or MORE state from the rolled-back prefix.
    zlink::msg_t rolled_back_prefix;
    TEST_ASSERT_SUCCESS_ERRNO (rolled_back_prefix.init_size (4));
    memset (rolled_back_prefix.data (), 'x', rolled_back_prefix.size ());
    rolled_back_prefix.set_flags (zlink::msg_t::more);
    TEST_ASSERT_TRUE (pipes[0]->write (&rolled_back_prefix));
    TEST_ASSERT_TRUE (pipes[0]->write_peer_weight_control_and_flush (9));
    TEST_ASSERT_FALSE (pipes[1]->check_read ());
    pipes[0]->rollback ();

    static const unsigned char rollback_weight_command[] = {
      'W', 'E', 'I', 'G', 'H', 'T', 0, 0, 0, 9};
    TEST_ASSERT_SUCCESS_ERRNO (received.init ());
    TEST_ASSERT_TRUE (pipes[1]->read (&received));
    TEST_ASSERT_TRUE ((received.flags () & zlink::msg_t::command) != 0);
    TEST_ASSERT_EQUAL_MEMORY (rollback_weight_command, received.data (),
                              sizeof (rollback_weight_command));
    TEST_ASSERT_SUCCESS_ERRNO (received.close ());

    zlink::msg_t fresh;
    TEST_ASSERT_SUCCESS_ERRNO (fresh.init_size (3));
    memcpy (fresh.data (), "new", fresh.size ());
    TEST_ASSERT_TRUE (pipes[0]->write_and_flush (&fresh));
    TEST_ASSERT_SUCCESS_ERRNO (received.init ());
    TEST_ASSERT_TRUE (pipes[1]->read (&received));
    TEST_ASSERT_FALSE ((received.flags ()
                        & (zlink::msg_t::more | zlink::msg_t::command))
                       != 0);
    TEST_ASSERT_EQUAL_MEMORY ("new", received.data (), fresh.size ());
    TEST_ASSERT_SUCCESS_ERRNO (received.close ());
    TEST_ASSERT_FALSE (pipes[1]->check_read ());

    // MAXMSGSIZE is Application admission. A fixed, validated internal WEIGHT
    // still crosses the same session pipe when the Application limit is one.
    pipes[0]->set_max_message_bytes (1);
    TEST_ASSERT_TRUE (pipes[0]->write_peer_weight_control_and_flush (11));
    static const unsigned char small_limit_weight_command[] = {
      'W', 'E', 'I', 'G', 'H', 'T', 0, 0, 0, 11};
    TEST_ASSERT_SUCCESS_ERRNO (received.init ());
    TEST_ASSERT_TRUE (pipes[1]->read (&received));
    TEST_ASSERT_EQUAL_MEMORY (small_limit_weight_command, received.data (),
                              sizeof (small_limit_weight_command));
    TEST_ASSERT_SUCCESS_ERRNO (received.close ());

    TEST_ASSERT_SUCCESS_ERRNO (first.close ());
    TEST_ASSERT_SUCCESS_ERRNO (final.close ());
    TEST_ASSERT_SUCCESS_ERRNO (rolled_back_prefix.close ());
    TEST_ASSERT_SUCCESS_ERRNO (fresh.close ());
    pipes[0]->terminate (false);
    pipes[1]->terminate (false);
    int events = 0;
    size_t events_size = sizeof (events);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_option (owner_handle, ZLINK_OPT_EVENTS, &events,
                        &events_size));
    TEST_ASSERT_EQUAL_INT (2, cleanup_sink.terminated_count);
    close_sync_socket (owner_handle);
}

void test_connection_guarded_write_rejects_stale_generation ()
{
    void *owner_handle = create_sync_socket (ZLINK_SOCKET_PAIR);
    zlink::object_t *owner = static_cast<zlink::object_t *> (
      as_socket_handle (owner_handle).socket);
    zlink::object_t *parents[] = {owner, owner};
    const uint64_t hwms[] = {4096, 4096};
    const bool conflate[] = {false, false};
    zlink::pipe_t *pipes[2];
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink::pipepair (parents, pipes, hwms, conflate));

    pipe_cleanup_sink_t cleanup_sink;
    pipes[0]->set_event_sink (&cleanup_sink);
    pipes[1]->set_event_sink (&cleanup_sink);
    pipes[0]->set_transport_connection_id (41);
    pipes[1]->set_transport_connection_id (41);

    zlink::msg_t payload;
    TEST_ASSERT_SUCCESS_ERRNO (payload.init_size (3));
    memcpy (payload.data (), "one", payload.size ());
    zlink::pipe_message_admission_t admission =
      zlink::pipe_message_admission_invalid;
    TEST_ASSERT_TRUE (pipes[0]->write_and_flush_if_transport_connection (
      &payload, 41, &admission));
    TEST_ASSERT_EQUAL_INT (zlink::pipe_message_admission_ready, admission);

    zlink::msg_t received;
    TEST_ASSERT_SUCCESS_ERRNO (received.init ());
    TEST_ASSERT_TRUE (pipes[1]->read (&received));
    TEST_ASSERT_EQUAL_MEMORY ("one", received.data (), received.size ());
    TEST_ASSERT_SUCCESS_ERRNO (received.close ());

    pipes[1]->set_transport_connection_id (42);
    admission = zlink::pipe_message_admission_invalid;
    TEST_ASSERT_FALSE (pipes[0]->write_and_flush_if_transport_connection (
      &payload, 41, &admission));
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);
    TEST_ASSERT_EQUAL_INT (zlink::pipe_message_admission_inactive, admission);
    TEST_ASSERT_FALSE (pipes[1]->check_read ());

    admission = zlink::pipe_message_admission_invalid;
    TEST_ASSERT_TRUE (pipes[0]->write_and_flush_if_transport_connection (
      &payload, 42, &admission));
    TEST_ASSERT_SUCCESS_ERRNO (received.init ());
    TEST_ASSERT_TRUE (pipes[1]->read (&received));
    TEST_ASSERT_SUCCESS_ERRNO (received.close ());

    pipes[1]->clear_transport_connection_id_before_peer_writes ();
    admission = zlink::pipe_message_admission_invalid;
    TEST_ASSERT_FALSE (pipes[0]->write_and_flush_if_transport_connection (
      &payload, 42, &admission));
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);
    TEST_ASSERT_EQUAL_INT (zlink::pipe_message_admission_inactive, admission);
    TEST_ASSERT_FALSE (pipes[1]->check_read ());

    TEST_ASSERT_SUCCESS_ERRNO (payload.close ());
    pipes[0]->terminate (false);
    pipes[1]->terminate (false);
    int events = 0;
    size_t events_size = sizeof (events);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_option (owner_handle, ZLINK_OPT_EVENTS, &events,
                        &events_size));
    TEST_ASSERT_EQUAL_INT (2, cleanup_sink.terminated_count);
    close_sync_socket (owner_handle);
}

void test_peer_control_slots_reject_non_dealer_router_pipe ()
{
    const int ordinary_peer_types[] = {
      ZLINK_CORE_SOCKET_PAIR, ZLINK_CORE_SOCKET_PUB,
      ZLINK_CORE_SOCKET_SUB,  ZLINK_CORE_SOCKET_XPUB,
      ZLINK_CORE_SOCKET_XSUB, ZLINK_CORE_SOCKET_STREAM};

    for (size_t session_index = 0; session_index != 2; ++session_index) {
        const bool session_pipe = session_index != 0;
        for (size_t type_index = 0;
             type_index
             != sizeof (ordinary_peer_types) / sizeof (ordinary_peer_types[0]);
             ++type_index) {
            void *owner_handle = create_sync_socket (ZLINK_SOCKET_PAIR);
            zlink::object_t *owner = static_cast<zlink::object_t *> (
              as_socket_handle (owner_handle).socket);
            zlink::object_t *parents[] = {owner, owner};
            const uint64_t hwms[] = {4096, 4096};
            const bool conflates[] = {false, false};
            zlink::pipe_t *pipes[2];
            TEST_ASSERT_SUCCESS_ERRNO (zlink::pipepair (
              parents, pipes, hwms, conflates, session_pipe,
              zlink::transport_lane_application, zlink::auto_hwm_role_none,
              false, zlink::physical_queue_class_application,
              session_pipe ? 0 : -1));

            pipe_cleanup_sink_t cleanup_sink;
            pipes[0]->set_event_sink (&cleanup_sink);
            pipes[1]->set_event_sink (&cleanup_sink);

            // Even malformed future plumbing that assigns pair-like identity
            // to an ordinary socket must not enable D/R boundary controls.
            // Cover both network/session and inproc owner-command flush paths.
            pipes[0]->set_transport_pair (
              zlink::transport_lane_application, 23, 1);
            pipes[1]->set_transport_pair (
              zlink::transport_lane_application, 23, 1);
            pipes[0]->set_transport_lane_count (1);
            pipes[1]->set_transport_lane_count (1);
            pipes[0]->set_peer_socket_type (
              ordinary_peer_types[type_index]);
            pipes[1]->set_peer_socket_type (
              ordinary_peer_types[type_index]);

            zlink::msg_t first;
            TEST_ASSERT_SUCCESS_ERRNO (first.init_size (3));
            memcpy (first.data (), "ord", first.size ());
            first.set_flags (zlink::msg_t::more);
            TEST_ASSERT_TRUE (pipes[0]->write (&first));

            errno = 0;
            TEST_ASSERT_FALSE (
              pipes[0]->write_peer_weight_control_and_flush (100));
            TEST_ASSERT_EQUAL_INT (EINVAL, errno);
            errno = 0;
            TEST_ASSERT_FALSE (pipes[0]->write_flow_state_control_and_flush (
              zlink::flow_state::receive_flow_paused, 1));
            TEST_ASSERT_EQUAL_INT (EINVAL, errno);

            zlink::msg_t final;
            TEST_ASSERT_SUCCESS_ERRNO (final.init_size (3));
            memcpy (final.data (), "ary", final.size ());
            TEST_ASSERT_TRUE (pipes[0]->write_and_flush (&final));

            zlink::msg_t received;
            TEST_ASSERT_SUCCESS_ERRNO (received.init ());
            TEST_ASSERT_TRUE (pipes[1]->read (&received));
            TEST_ASSERT_TRUE ((received.flags () & zlink::msg_t::more) != 0);
            TEST_ASSERT_FALSE (
              (received.flags () & zlink::msg_t::command) != 0);
            TEST_ASSERT_EQUAL_MEMORY ("ord", received.data (),
                                      received.size ());
            TEST_ASSERT_SUCCESS_ERRNO (received.close ());

            TEST_ASSERT_SUCCESS_ERRNO (received.init ());
            TEST_ASSERT_TRUE (pipes[1]->read (&received));
            TEST_ASSERT_FALSE (
              (received.flags () & (zlink::msg_t::more | zlink::msg_t::command))
              != 0);
            TEST_ASSERT_EQUAL_MEMORY ("ary", received.data (),
                                      received.size ());
            TEST_ASSERT_SUCCESS_ERRNO (received.close ());
            TEST_ASSERT_FALSE (pipes[1]->check_read ());
            TEST_ASSERT_EQUAL_UINT64 (1, pipes[0]->get_msgs_written ());

            TEST_ASSERT_SUCCESS_ERRNO (first.close ());
            TEST_ASSERT_SUCCESS_ERRNO (final.close ());
            pipes[0]->terminate (false);
            pipes[1]->terminate (false);
            int events = 0;
            size_t events_size = sizeof (events);
            TEST_ASSERT_SUCCESS_ERRNO (
              zlink_get_option (owner_handle, ZLINK_OPT_EVENTS, &events,
                                &events_size));
            TEST_ASSERT_EQUAL_INT (2, cleanup_sink.terminated_count);
            close_sync_socket (owner_handle);
        }
    }
}

void test_weighted_lb_reactivation_keeps_configured_weight ()
{
    void *owner_handle = create_sync_socket (ZLINK_SOCKET_PAIR);
    zlink::object_t *owner = static_cast<zlink::object_t *> (
      as_socket_handle (owner_handle).socket);
    zlink::object_t *parents[] = {owner, owner};
    const uint64_t hwms[] = {1, 1};
    const bool conflate[] = {false, false};
    zlink::pipe_t *first_pair[2];
    zlink::pipe_t *second_pair[2];
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink::pipepair (parents, first_pair, hwms, conflate));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink::pipepair (parents, second_pair, hwms, conflate));

    pipe_cleanup_sink_t cleanup_sink;
    first_pair[0]->set_event_sink (&cleanup_sink);
    first_pair[1]->set_event_sink (&cleanup_sink);
    second_pair[0]->set_event_sink (&cleanup_sink);
    second_pair[1]->set_event_sink (&cleanup_sink);

    zlink::lb_t lb;
    lb.attach (first_pair[0]);
    lb.attach (second_pair[0]);
    lb.set_weight (first_pair[0], 25);
    lb.set_weight (second_pair[0], 100);

    zlink::msg_t message;
    TEST_ASSERT_SUCCESS_ERRNO (message.init_size (1));
    bool backpressured = false;
    for (int i = 0; i < 16; ++i) {
        if (lb.send (&message) != 0) {
            backpressured = true;
            break;
        }
    }
    TEST_ASSERT_TRUE (backpressured);

    // A failed write changes only writability. The configured routing policy
    // must remain available when the pipe reports fresh write credit.
    TEST_ASSERT_EQUAL_UINT32 (25, lb.weight (first_pair[0]));
    TEST_ASSERT_EQUAL_UINT32 (100, lb.weight (second_pair[0]));
    first_pair[0]->refresh_write_credit (1, first_pair[0]->get_bytes_written ());
    second_pair[0]->refresh_write_credit (1, second_pair[0]->get_bytes_written ());
    lb.activated (first_pair[0]);
    lb.activated (second_pair[0]);
    TEST_ASSERT_SUCCESS_ERRNO (lb.send (&message));

    TEST_ASSERT_SUCCESS_ERRNO (message.close ());
    lb.pipe_terminated (first_pair[0]);
    lb.pipe_terminated (second_pair[0]);

    // Removing a pipe from lb_t drops only the scheduler reference. Complete
    // the normal peer handshake so command references and both ypipes are
    // released by the same protocol used by runtime-owned pipes.
    first_pair[0]->terminate (false);
    first_pair[1]->terminate (false);
    second_pair[0]->terminate (false);
    second_pair[1]->terminate (false);
    int events = 0;
    size_t events_size = sizeof (events);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_option (owner_handle, ZLINK_OPT_EVENTS, &events, &events_size));
    TEST_ASSERT_EQUAL_INT (4, cleanup_sink.terminated_count);
    close_sync_socket (owner_handle);
}

void test_weight_zero_between_parts_preserves_selected_message ()
{
    void *owner_handle = create_sync_socket (ZLINK_SOCKET_PAIR);
    zlink::object_t *owner = static_cast<zlink::object_t *> (
      as_socket_handle (owner_handle).socket);
    zlink::object_t *parents[] = {owner, owner};
    const uint64_t hwms[] = {4096, 4096};
    const bool conflate[] = {false, false};
    zlink::pipe_t *first_pair[2];
    zlink::pipe_t *second_pair[2];
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink::pipepair (parents, first_pair, hwms, conflate));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink::pipepair (parents, second_pair, hwms, conflate));

    pipe_cleanup_sink_t cleanup_sink;
    first_pair[0]->set_event_sink (&cleanup_sink);
    first_pair[1]->set_event_sink (&cleanup_sink);
    second_pair[0]->set_event_sink (&cleanup_sink);
    second_pair[1]->set_event_sink (&cleanup_sink);

    zlink::lb_t lb;
    lb.attach (first_pair[0]);
    lb.attach (second_pair[0]);

    zlink::msg_t prefix;
    TEST_ASSERT_SUCCESS_ERRNO (prefix.init_size (1));
    *static_cast<unsigned char *> (prefix.data ()) = 0x41;
    prefix.set_flags (zlink::msg_t::more);
    zlink::pipe_t *selected = NULL;
    TEST_ASSERT_SUCCESS_ERRNO (lb.sendpipe (&prefix, &selected));
    TEST_ASSERT_NOT_NULL (selected);

    // The absolute policy change affects the next message. It must not turn
    // an already accepted prefix into a dropped/corrupt partial record.
    lb.set_weight (selected, 0);
    zlink::msg_t final;
    TEST_ASSERT_SUCCESS_ERRNO (final.init_size (1));
    *static_cast<unsigned char *> (final.data ()) = 0x42;
    zlink::pipe_t *final_selected = NULL;
    TEST_ASSERT_SUCCESS_ERRNO (lb.sendpipe (&final, &final_selected));
    TEST_ASSERT_EQUAL_PTR (selected, final_selected);

    zlink::pipe_t *selected_reader =
      selected == first_pair[0] ? first_pair[1] : second_pair[1];
    zlink::pipe_t *other_writer =
      selected == first_pair[0] ? second_pair[0] : first_pair[0];
    zlink::pipe_t *other_reader =
      selected == first_pair[0] ? second_pair[1] : first_pair[1];
    zlink::msg_t received;
    TEST_ASSERT_SUCCESS_ERRNO (received.init ());
    TEST_ASSERT_TRUE (selected_reader->read (&received));
    TEST_ASSERT_TRUE ((received.flags () & zlink::msg_t::more) != 0);
    TEST_ASSERT_EQUAL_UINT8 (
      0x41, *static_cast<unsigned char *> (received.data ()));
    TEST_ASSERT_SUCCESS_ERRNO (received.close ());
    TEST_ASSERT_SUCCESS_ERRNO (received.init ());
    TEST_ASSERT_TRUE (selected_reader->read (&received));
    TEST_ASSERT_FALSE ((received.flags () & zlink::msg_t::more) != 0);
    TEST_ASSERT_EQUAL_UINT8 (
      0x42, *static_cast<unsigned char *> (received.data ()));
    TEST_ASSERT_SUCCESS_ERRNO (received.close ());
    TEST_ASSERT_FALSE (selected_reader->check_read ());

    zlink::msg_t next;
    TEST_ASSERT_SUCCESS_ERRNO (next.init_size (1));
    *static_cast<unsigned char *> (next.data ()) = 0x43;
    zlink::pipe_t *next_selected = NULL;
    TEST_ASSERT_SUCCESS_ERRNO (lb.sendpipe (&next, &next_selected));
    TEST_ASSERT_EQUAL_PTR (other_writer, next_selected);
    TEST_ASSERT_SUCCESS_ERRNO (received.init ());
    TEST_ASSERT_TRUE (other_reader->read (&received));
    TEST_ASSERT_EQUAL_UINT8 (
      0x43, *static_cast<unsigned char *> (received.data ()));
    TEST_ASSERT_SUCCESS_ERRNO (received.close ());

    TEST_ASSERT_SUCCESS_ERRNO (prefix.close ());
    TEST_ASSERT_SUCCESS_ERRNO (final.close ());
    TEST_ASSERT_SUCCESS_ERRNO (next.close ());
    lb.pipe_terminated (first_pair[0]);
    lb.pipe_terminated (second_pair[0]);
    first_pair[0]->terminate (false);
    first_pair[1]->terminate (false);
    second_pair[0]->terminate (false);
    second_pair[1]->terminate (false);
    int events = 0;
    size_t events_size = sizeof (events);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_option (owner_handle, ZLINK_OPT_EVENTS, &events, &events_size));
    TEST_ASSERT_EQUAL_INT (4, cleanup_sink.terminated_count);
    close_sync_socket (owner_handle);
}

void test_single_pipe_lb_rolls_back_byte_hwm_rejected_multipart ()
{
    void *owner_handle = create_sync_socket (ZLINK_SOCKET_PAIR);
    zlink::object_t *owner = static_cast<zlink::object_t *> (
      as_socket_handle (owner_handle).socket);
    zlink::object_t *parents[] = {owner, owner};
    const uint64_t frame_bytes = sizeof (zlink::msg_t) + 1;
    const uint64_t hwms[] = {frame_bytes * 2, frame_bytes * 2};
    const bool conflate[] = {false, false};
    zlink::pipe_t *pipes[2];
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink::pipepair (parents, pipes, hwms, conflate));

    pipe_cleanup_sink_t cleanup_sink;
    pipes[0]->set_event_sink (&cleanup_sink);
    pipes[1]->set_event_sink (&cleanup_sink);

    zlink::lb_t lb;
    lb.attach (pipes[0]);

    zlink::msg_t filler;
    TEST_ASSERT_SUCCESS_ERRNO (filler.init_size (1));
    *static_cast<unsigned char *> (filler.data ()) = 0x11;
    TEST_ASSERT_SUCCESS_ERRNO (lb.send (&filler));

    zlink::msg_t first_part;
    TEST_ASSERT_SUCCESS_ERRNO (first_part.init_size (1));
    *static_cast<unsigned char *> (first_part.data ()) = 0x22;
    first_part.set_flags (zlink::msg_t::more);
    TEST_ASSERT_SUCCESS_ERRNO (lb.send (&first_part));

    zlink::msg_t rejected_final;
    TEST_ASSERT_SUCCESS_ERRNO (rejected_final.init_size (1));
    *static_cast<unsigned char *> (rejected_final.data ()) = 0x33;
    TEST_ASSERT_EQUAL_INT (-2, lb.send (&rejected_final));
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);
    lb.rollback ();

    zlink::msg_t received;
    TEST_ASSERT_SUCCESS_ERRNO (received.init ());
    TEST_ASSERT_TRUE (pipes[1]->read (&received));
    TEST_ASSERT_EQUAL_UINT8 (
      0x11, *static_cast<unsigned char *> (received.data ()));
    TEST_ASSERT_FALSE ((received.flags () & zlink::msg_t::more) != 0);
    TEST_ASSERT_SUCCESS_ERRNO (received.close ());

    pipes[0]->refresh_write_credit (
      pipes[1]->get_msgs_read (), pipes[1]->get_bytes_read ());
    lb.activated (pipes[0]);

    zlink::msg_t after_failure;
    TEST_ASSERT_SUCCESS_ERRNO (after_failure.init_size (1));
    *static_cast<unsigned char *> (after_failure.data ()) = 0x44;
    TEST_ASSERT_SUCCESS_ERRNO (lb.send (&after_failure));

    TEST_ASSERT_SUCCESS_ERRNO (received.init ());
    TEST_ASSERT_TRUE (pipes[1]->read (&received));
    TEST_ASSERT_EQUAL_UINT8 (
      0x44, *static_cast<unsigned char *> (received.data ()));
    TEST_ASSERT_FALSE ((received.flags () & zlink::msg_t::more) != 0);
    TEST_ASSERT_SUCCESS_ERRNO (received.close ());
    TEST_ASSERT_FALSE (pipes[1]->check_read ());

    TEST_ASSERT_SUCCESS_ERRNO (filler.close ());
    TEST_ASSERT_SUCCESS_ERRNO (first_part.close ());
    TEST_ASSERT_SUCCESS_ERRNO (rejected_final.close ());
    TEST_ASSERT_SUCCESS_ERRNO (after_failure.close ());
    lb.pipe_terminated (pipes[0]);
    pipes[0]->terminate (false);
    pipes[1]->terminate (false);
    int events = 0;
    size_t events_size = sizeof (events);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_option (owner_handle, ZLINK_OPT_EVENTS, &events, &events_size));
    TEST_ASSERT_EQUAL_INT (2, cleanup_sink.terminated_count);
    close_sync_socket (owner_handle);
}

void test_single_pipe_dist_rolls_back_byte_hwm_rejected_multipart ()
{
    void *owner_handle = create_sync_socket (ZLINK_SOCKET_PAIR);
    zlink::object_t *owner = static_cast<zlink::object_t *> (
      as_socket_handle (owner_handle).socket);
    zlink::object_t *parents[] = {owner, owner};
    const uint64_t frame_bytes = sizeof (zlink::msg_t) + 1;
    const uint64_t hwms[] = {frame_bytes * 2, frame_bytes * 2};
    const bool conflate[] = {false, false};
    zlink::pipe_t *pipes[2];
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink::pipepair (parents, pipes, hwms, conflate));

    pipe_cleanup_sink_t cleanup_sink;
    pipes[0]->set_event_sink (&cleanup_sink);
    pipes[1]->set_event_sink (&cleanup_sink);

    zlink::dist_t dist;
    dist.attach (pipes[0]);

    zlink::msg_t filler;
    TEST_ASSERT_SUCCESS_ERRNO (filler.init_size (1));
    *static_cast<unsigned char *> (filler.data ()) = 0x11;
    TEST_ASSERT_SUCCESS_ERRNO (dist.send_to_all (&filler));
    const uint64_t committed_before_failure = pipes[0]->get_bytes_written ();

    zlink::msg_t first_part;
    TEST_ASSERT_SUCCESS_ERRNO (first_part.init_size (1));
    *static_cast<unsigned char *> (first_part.data ()) = 0x22;
    first_part.set_flags (zlink::msg_t::more);
    TEST_ASSERT_SUCCESS_ERRNO (dist.send_to_all (&first_part));

    zlink::msg_t rejected_final;
    TEST_ASSERT_SUCCESS_ERRNO (rejected_final.init_size (1));
    *static_cast<unsigned char *> (rejected_final.data ()) = 0x33;
    TEST_ASSERT_SUCCESS_ERRNO (dist.send_to_all (&rejected_final));
    TEST_ASSERT_EQUAL_UINT64 (
      committed_before_failure, pipes[0]->get_bytes_written ());

    zlink::msg_t received;
    TEST_ASSERT_SUCCESS_ERRNO (received.init ());
    TEST_ASSERT_TRUE (pipes[1]->read (&received));
    TEST_ASSERT_EQUAL_UINT8 (
      0x11, *static_cast<unsigned char *> (received.data ()));
    TEST_ASSERT_FALSE ((received.flags () & zlink::msg_t::more) != 0);
    TEST_ASSERT_SUCCESS_ERRNO (received.close ());
    TEST_ASSERT_FALSE (pipes[1]->check_read ());

    pipes[0]->refresh_write_credit (
      pipes[1]->get_msgs_read (), pipes[1]->get_bytes_read ());
    dist.activated (pipes[0]);

    zlink::msg_t after_failure;
    TEST_ASSERT_SUCCESS_ERRNO (after_failure.init_size (1));
    *static_cast<unsigned char *> (after_failure.data ()) = 0x44;
    TEST_ASSERT_SUCCESS_ERRNO (dist.send_to_all (&after_failure));

    TEST_ASSERT_SUCCESS_ERRNO (received.init ());
    TEST_ASSERT_TRUE (pipes[1]->read (&received));
    TEST_ASSERT_EQUAL_UINT8 (
      0x44, *static_cast<unsigned char *> (received.data ()));
    TEST_ASSERT_FALSE ((received.flags () & zlink::msg_t::more) != 0);
    TEST_ASSERT_SUCCESS_ERRNO (received.close ());
    TEST_ASSERT_FALSE (pipes[1]->check_read ());

    TEST_ASSERT_SUCCESS_ERRNO (filler.close ());
    TEST_ASSERT_SUCCESS_ERRNO (first_part.close ());
    TEST_ASSERT_SUCCESS_ERRNO (rejected_final.close ());
    TEST_ASSERT_SUCCESS_ERRNO (after_failure.close ());
    dist.pipe_terminated (pipes[0]);
    pipes[0]->terminate (false);
    pipes[1]->terminate (false);
    int events = 0;
    size_t events_size = sizeof (events);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_option (owner_handle, ZLINK_OPT_EVENTS, &events, &events_size));
    TEST_ASSERT_EQUAL_INT (2, cleanup_sink.terminated_count);
    close_sync_socket (owner_handle);
}

void test_pipe_rejects_multipart_before_partial_bytes_exceed_hwm ()
{
    void *owner_handle = create_sync_socket (ZLINK_SOCKET_PAIR);
    zlink::object_t *owner = static_cast<zlink::object_t *> (
      as_socket_handle (owner_handle).socket);
    zlink::object_t *parents[] = {owner, owner};
    const uint64_t frame_bytes = sizeof (zlink::msg_t) + 1;
    const uint64_t hwms[] = {frame_bytes * 3, frame_bytes * 3};
    const bool conflate[] = {false, false};
    zlink::pipe_t *pipes[2];
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink::pipepair (parents, pipes, hwms, conflate));

    pipe_cleanup_sink_t cleanup_sink;
    pipes[0]->set_event_sink (&cleanup_sink);
    pipes[1]->set_event_sink (&cleanup_sink);

    zlink::msg_t filler;
    TEST_ASSERT_SUCCESS_ERRNO (filler.init_size (1));
    TEST_ASSERT_TRUE (pipes[0]->write_and_flush (&filler));
    const uint64_t committed_before_failure = pipes[0]->get_bytes_written ();

    zlink::msg_t first_part;
    TEST_ASSERT_SUCCESS_ERRNO (first_part.init_size (1));
    first_part.set_flags (zlink::msg_t::more);
    TEST_ASSERT_TRUE (pipes[0]->write (&first_part));

    zlink::msg_t second_part;
    TEST_ASSERT_SUCCESS_ERRNO (second_part.init_size (1));
    second_part.set_flags (zlink::msg_t::more);
    TEST_ASSERT_TRUE (pipes[0]->write (&second_part));

    zlink::msg_t rejected_more;
    TEST_ASSERT_SUCCESS_ERRNO (rejected_more.init_size (1));
    rejected_more.set_flags (zlink::msg_t::more);
    TEST_ASSERT_FALSE (pipes[0]->write (&rejected_more));
    TEST_ASSERT_EQUAL_UINT64 (
      committed_before_failure, pipes[0]->get_bytes_written ());
    pipes[0]->rollback ();

    zlink::msg_t received;
    TEST_ASSERT_SUCCESS_ERRNO (received.init ());
    TEST_ASSERT_TRUE (pipes[1]->read (&received));
    TEST_ASSERT_FALSE ((received.flags () & zlink::msg_t::more) != 0);
    TEST_ASSERT_SUCCESS_ERRNO (received.close ());
    TEST_ASSERT_FALSE (pipes[1]->check_read ());

    pipes[0]->refresh_write_credit (
      pipes[1]->get_msgs_read (), pipes[1]->get_bytes_read ());

    zlink::msg_t after_failure;
    TEST_ASSERT_SUCCESS_ERRNO (after_failure.init_size (1));
    TEST_ASSERT_TRUE (pipes[0]->write_and_flush (&after_failure));
    TEST_ASSERT_SUCCESS_ERRNO (received.init ());
    TEST_ASSERT_TRUE (pipes[1]->read (&received));
    TEST_ASSERT_FALSE ((received.flags () & zlink::msg_t::more) != 0);
    TEST_ASSERT_SUCCESS_ERRNO (received.close ());
    TEST_ASSERT_FALSE (pipes[1]->check_read ());

    TEST_ASSERT_SUCCESS_ERRNO (filler.close ());
    TEST_ASSERT_SUCCESS_ERRNO (first_part.close ());
    TEST_ASSERT_SUCCESS_ERRNO (second_part.close ());
    TEST_ASSERT_SUCCESS_ERRNO (rejected_more.close ());
    TEST_ASSERT_SUCCESS_ERRNO (after_failure.close ());
    pipes[0]->terminate (false);
    pipes[1]->terminate (false);
    int events = 0;
    size_t events_size = sizeof (events);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_option (owner_handle, ZLINK_OPT_EVENTS, &events, &events_size));
    TEST_ASSERT_EQUAL_INT (2, cleanup_sink.terminated_count);
    close_sync_socket (owner_handle);
}

void test_empty_pipe_incomplete_multipart_stops_at_max_message_size ()
{
    void *owner_handle = create_sync_socket (ZLINK_SOCKET_PAIR);
    zlink::object_t *owner = static_cast<zlink::object_t *> (
      as_socket_handle (owner_handle).socket);
    zlink::object_t *parents[] = {owner, owner};
    const uint64_t frame_bytes = sizeof (zlink::msg_t) + 1;
    const uint64_t hwms[] = {frame_bytes * 3, frame_bytes * 3};
    const bool conflate[] = {false, false};
    zlink::pipe_t *pipes[2];
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink::pipepair (parents, pipes, hwms, conflate));
    pipes[0]->set_max_message_bytes (3);

    pipe_cleanup_sink_t cleanup_sink;
    pipes[0]->set_event_sink (&cleanup_sink);
    pipes[1]->set_event_sink (&cleanup_sink);

    zlink::msg_t frames[4];
    for (size_t i = 0; i < 4; ++i) {
        TEST_ASSERT_SUCCESS_ERRNO (frames[i].init_size (1));
        frames[i].set_flags (zlink::msg_t::more);
    }
    TEST_ASSERT_TRUE (pipes[0]->write (&frames[0]));
    TEST_ASSERT_TRUE (pipes[0]->write (&frames[1]));
    TEST_ASSERT_TRUE (pipes[0]->write (&frames[2]));
    TEST_ASSERT_FALSE (pipes[0]->write (&frames[3]));
    TEST_ASSERT_EQUAL_INT (EMSGSIZE, errno);
    TEST_ASSERT_FALSE (pipes[1]->check_read ());
    pipes[0]->rollback ();

    for (size_t i = 0; i < 4; ++i)
        TEST_ASSERT_SUCCESS_ERRNO (frames[i].close ());
    pipes[0]->terminate (false);
    pipes[1]->terminate (false);
    int events = 0;
    size_t events_size = sizeof (events);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_option (owner_handle, ZLINK_OPT_EVENTS, &events, &events_size));
    TEST_ASSERT_EQUAL_INT (2, cleanup_sink.terminated_count);
    close_sync_socket (owner_handle);
}

void test_empty_pipe_oversize_exception_applies_only_to_complete_message ()
{
    void *owner_handle = create_sync_socket (ZLINK_SOCKET_PAIR);
    zlink::object_t *owner = static_cast<zlink::object_t *> (
      as_socket_handle (owner_handle).socket);
    zlink::object_t *parents[] = {owner, owner};
    const uint64_t frame_bytes = sizeof (zlink::msg_t) + 1;
    const uint64_t hwms[] = {frame_bytes * 2, frame_bytes * 2};
    const bool conflate[] = {false, false};
    zlink::pipe_t *pipes[2];
    TEST_ASSERT_SUCCESS_ERRNO (zlink::pipepair (parents, pipes, hwms, conflate));
    pipes[0]->set_max_message_bytes (5);

    pipe_cleanup_sink_t cleanup_sink;
    pipes[0]->set_event_sink (&cleanup_sink);
    pipes[1]->set_event_sink (&cleanup_sink);

    zlink::msg_t frames[3];
    for (size_t i = 0; i < 3; ++i) {
        TEST_ASSERT_SUCCESS_ERRNO (frames[i].init_size (1));
        frames[i].set_flags (zlink::msg_t::more);
    }
    TEST_ASSERT_TRUE (pipes[0]->write (&frames[0]));
    TEST_ASSERT_TRUE (pipes[0]->write (&frames[1]));
    TEST_ASSERT_FALSE (pipes[0]->write (&frames[2]));
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);
    TEST_ASSERT_FALSE (pipes[1]->check_read ());
    pipes[0]->rollback ();

    for (size_t i = 0; i < 3; ++i)
        TEST_ASSERT_SUCCESS_ERRNO (frames[i].close ());

    zlink::msg_t complete;
    TEST_ASSERT_SUCCESS_ERRNO (complete.init_size (5));
    TEST_ASSERT_TRUE (pipes[0]->write_and_flush (&complete));
    zlink::msg_t received;
    TEST_ASSERT_SUCCESS_ERRNO (received.init ());
    TEST_ASSERT_TRUE (pipes[1]->read (&received));
    TEST_ASSERT_FALSE ((received.flags () & zlink::msg_t::more) != 0);
    TEST_ASSERT_SUCCESS_ERRNO (received.close ());
    TEST_ASSERT_SUCCESS_ERRNO (complete.close ());

    pipes[0]->terminate (false);
    pipes[1]->terminate (false);
    int events = 0;
    size_t events_size = sizeof (events);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_option (owner_handle, ZLINK_OPT_EVENTS, &events, &events_size));
    TEST_ASSERT_EQUAL_INT (2, cleanup_sink.terminated_count);
    close_sync_socket (owner_handle);
}

void test_drained_pipe_oversize_multipart_uses_fresh_peer_credit ()
{
    void *owner_handle = create_sync_socket (ZLINK_SOCKET_PAIR);
    zlink::object_t *owner = static_cast<zlink::object_t *> (
      as_socket_handle (owner_handle).socket);
    zlink::object_t *parents[] = {owner, owner};
    const uint64_t hwm = 4096;
    const uint64_t hwms[] = {hwm, hwm};
    const bool conflate[] = {false, false};
    zlink::pipe_t *pipes[2];
    TEST_ASSERT_SUCCESS_ERRNO (zlink::pipepair (parents, pipes, hwms, conflate));

    pipe_cleanup_sink_t cleanup_sink;
    pipes[0]->set_event_sink (&cleanup_sink);
    pipes[1]->set_event_sink (&cleanup_sink);

    //  Drain several small multipart messages without crossing the reader LWM.
    //  The completed-read snapshot is current, but no credit command updates the
    //  writer's cached value.
    for (size_t i = 0; i < 5; ++i) {
        zlink::msg_t first;
        zlink::msg_t final;
        TEST_ASSERT_SUCCESS_ERRNO (first.init_size (1));
        TEST_ASSERT_SUCCESS_ERRNO (final.init_size (1));
        first.set_flags (zlink::msg_t::more);
        TEST_ASSERT_TRUE (pipes[0]->write (&first));
        TEST_ASSERT_TRUE (pipes[0]->write_and_flush (&final));

        zlink::msg_t received;
        TEST_ASSERT_SUCCESS_ERRNO (received.init ());
        TEST_ASSERT_TRUE (pipes[1]->read (&received));
        TEST_ASSERT_TRUE ((received.flags () & zlink::msg_t::more) != 0);
        TEST_ASSERT_SUCCESS_ERRNO (received.close ());
        TEST_ASSERT_SUCCESS_ERRNO (received.init ());
        TEST_ASSERT_TRUE (pipes[1]->read (&received));
        TEST_ASSERT_FALSE ((received.flags () & zlink::msg_t::more) != 0);
        TEST_ASSERT_SUCCESS_ERRNO (received.close ());
        TEST_ASSERT_FALSE (pipes[1]->check_read ());

        TEST_ASSERT_SUCCESS_ERRNO (first.close ());
        TEST_ASSERT_SUCCESS_ERRNO (final.close ());
    }

    zlink::msg_t first;
    zlink::msg_t oversize_final;
    TEST_ASSERT_SUCCESS_ERRNO (first.init_size (1));
    TEST_ASSERT_SUCCESS_ERRNO (oversize_final.init_size (hwm));
    first.set_flags (zlink::msg_t::more);
    TEST_ASSERT_TRUE (pipes[0]->write (&first));
    TEST_ASSERT_TRUE (pipes[0]->write_and_flush (&oversize_final));

    zlink::msg_t received;
    TEST_ASSERT_SUCCESS_ERRNO (received.init ());
    TEST_ASSERT_TRUE (pipes[1]->read (&received));
    TEST_ASSERT_TRUE ((received.flags () & zlink::msg_t::more) != 0);
    TEST_ASSERT_SUCCESS_ERRNO (received.close ());
    TEST_ASSERT_SUCCESS_ERRNO (received.init ());
    TEST_ASSERT_TRUE (pipes[1]->read (&received));
    TEST_ASSERT_FALSE ((received.flags () & zlink::msg_t::more) != 0);
    TEST_ASSERT_EQUAL_UINT64 (hwm, received.size ());
    TEST_ASSERT_SUCCESS_ERRNO (received.close ());

    TEST_ASSERT_SUCCESS_ERRNO (first.close ());
    pipes[0]->terminate (false);
    pipes[1]->terminate (false);
    int events = 0;
    size_t events_size = sizeof (events);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_option (owner_handle, ZLINK_OPT_EVENTS, &events, &events_size));
    TEST_ASSERT_EQUAL_INT (2, cleanup_sink.terminated_count);
    close_sync_socket (owner_handle);
}

void test_physical_queue_snapshot_accounts_multipart_once ()
{
    void *owner_handle = create_sync_socket (ZLINK_SOCKET_PAIR);
    zlink::object_t *owner = static_cast<zlink::object_t *> (
      as_socket_handle (owner_handle).socket);
    zlink::object_t *parents[] = {owner, owner};
    const uint64_t hwms[] = {4096, 4096};
    const bool conflate[] = {false, false};
    zlink::pipe_t *pipes[2];
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink::pipepair (parents, pipes, hwms, conflate));
    pipes[0]->set_transport_pair (zlink::transport_lane_application, 1, 1);
    pipes[1]->set_transport_pair (zlink::transport_lane_application, 1, 1);

    pipe_cleanup_sink_t cleanup_sink;
    pipes[0]->set_event_sink (&cleanup_sink);
    pipes[1]->set_event_sink (&cleanup_sink);

    zlink::msg_t first;
    TEST_ASSERT_SUCCESS_ERRNO (first.init_size (5));
    first.set_flags (zlink::msg_t::more);
    TEST_ASSERT_TRUE (pipes[0]->write (&first));
    const zlink_auto_hwm_budget_snapshot_t provisional = read_budget_snapshot ();
    TEST_ASSERT_GREATER_THAN_UINT64 (0,
                                     provisional.provisional_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (provisional.provisional_accounted_bytes,
                              provisional.current_accounted_bytes);
    pipes[0]->rollback ();
    const zlink_auto_hwm_budget_snapshot_t rolled_back = read_budget_snapshot ();
    TEST_ASSERT_EQUAL_UINT64 (0, rolled_back.provisional_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (0, rolled_back.current_accounted_bytes);
    TEST_ASSERT_TRUE (pipes[0]->write (&first));

    zlink::msg_t final;
    TEST_ASSERT_SUCCESS_ERRNO (final.init_size (7));
    TEST_ASSERT_TRUE (pipes[0]->write_and_flush (&final));
    const zlink_auto_hwm_budget_snapshot_t committed = read_budget_snapshot ();
    TEST_ASSERT_EQUAL_UINT64 (0, committed.provisional_accounted_bytes);
    TEST_ASSERT_GREATER_THAN_UINT64 (provisional.current_accounted_bytes,
                                     committed.current_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (committed.current_accounted_bytes,
                              committed.core_queue_accounted_bytes);

    zlink::msg_t received;
    TEST_ASSERT_SUCCESS_ERRNO (received.init ());
    TEST_ASSERT_TRUE (pipes[1]->read (&received));
    TEST_ASSERT_TRUE ((received.flags () & zlink::msg_t::more) != 0);
    TEST_ASSERT_SUCCESS_ERRNO (received.close ());
    const zlink_auto_hwm_budget_snapshot_t partial = read_budget_snapshot ();
    TEST_ASSERT_GREATER_THAN_UINT64 (0, partial.current_accounted_bytes);
    TEST_ASSERT_LESS_THAN_UINT64 (committed.current_accounted_bytes,
                                  partial.current_accounted_bytes);

    TEST_ASSERT_SUCCESS_ERRNO (received.init ());
    TEST_ASSERT_TRUE (pipes[1]->read (&received));
    TEST_ASSERT_FALSE ((received.flags () & zlink::msg_t::more) != 0);
    TEST_ASSERT_SUCCESS_ERRNO (received.close ());
    const zlink_auto_hwm_budget_snapshot_t drained = read_budget_snapshot ();
    TEST_ASSERT_EQUAL_UINT64 (0, drained.current_accounted_bytes);
    TEST_ASSERT_GREATER_THAN_UINT64 (0, drained.peak_accounted_bytes);

    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_ctx_reset_auto_hwm_budget_metrics (get_test_context ()));
    const zlink_auto_hwm_budget_snapshot_t reset = read_budget_snapshot ();
    TEST_ASSERT_EQUAL_UINT64 (0, reset.peak_accounted_bytes);

    TEST_ASSERT_SUCCESS_ERRNO (first.close ());
    TEST_ASSERT_SUCCESS_ERRNO (final.close ());
    pipes[0]->terminate (false);
    pipes[1]->terminate (false);
    int events = 0;
    size_t events_size = sizeof (events);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_option (owner_handle, ZLINK_OPT_EVENTS, &events, &events_size));
    TEST_ASSERT_EQUAL_INT (2, cleanup_sink.terminated_count);
    close_sync_socket (owner_handle);
}

void test_physical_queue_deferred_shrink_applies_on_drain ()
{
    void *owner_handle = create_sync_socket (ZLINK_SOCKET_PAIR);
    zlink::object_t *owner = static_cast<zlink::object_t *> (
      as_socket_handle (owner_handle).socket);
    zlink::object_t *parents[] = {owner, owner};
    const uint64_t initial_hwm = 4096;
    const uint64_t hwms[] = {initial_hwm, initial_hwm};
    const bool conflate[] = {false, false};
    zlink::pipe_t *pipes[2];
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink::pipepair (parents, pipes, hwms, conflate));
    pipes[0]->set_transport_pair (zlink::transport_lane_application, 1, 1);
    pipes[1]->set_transport_pair (zlink::transport_lane_application, 1, 1);

    pipe_cleanup_sink_t cleanup_sink;
    pipes[0]->set_event_sink (&cleanup_sink);
    pipes[1]->set_event_sink (&cleanup_sink);

    zlink::msg_t first;
    TEST_ASSERT_SUCCESS_ERRNO (first.init_size (256));
    TEST_ASSERT_TRUE (pipes[0]->write_and_flush (&first));

    const uint64_t shrink_target = 64;
    pipes[0]->set_hwms (shrink_target, shrink_target);
    TEST_ASSERT_EQUAL_UINT64 (shrink_target, pipes[0]->planned_out_hwm ());
    TEST_ASSERT_EQUAL_UINT64 (initial_hwm, pipes[0]->applied_out_hwm ());

    zlink::msg_t blocked;
    TEST_ASSERT_SUCCESS_ERRNO (blocked.init_size (1));
    TEST_ASSERT_FALSE (pipes[0]->write_and_flush (&blocked));

    zlink::msg_t received;
    TEST_ASSERT_SUCCESS_ERRNO (received.init ());
    TEST_ASSERT_TRUE (pipes[1]->read (&received));
    TEST_ASSERT_SUCCESS_ERRNO (received.close ());
    TEST_ASSERT_EQUAL_UINT64 (shrink_target, pipes[0]->applied_out_hwm ());

    TEST_ASSERT_SUCCESS_ERRNO (blocked.close ());
    pipes[0]->terminate (false);
    pipes[1]->terminate (false);
    int events = 0;
    size_t events_size = sizeof (events);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_option (owner_handle, ZLINK_OPT_EVENTS, &events, &events_size));
    TEST_ASSERT_EQUAL_INT (2, cleanup_sink.terminated_count);
    close_sync_socket (owner_handle);
}

void test_deferred_shrink_wakes_writer_at_planned_lwm ()
{
    void *owner_handle = create_sync_socket (ZLINK_SOCKET_PAIR);
    zlink::object_t *owner = static_cast<zlink::object_t *> (
      as_socket_handle (owner_handle).socket);
    zlink::object_t *parents[] = {owner, owner};
    const uint64_t frame_bytes = sizeof (zlink::msg_t) + 1;
    const uint64_t initial_hwm = frame_bytes * 8;
    const uint64_t shrink_target = frame_bytes * 4;
    const uint64_t hwms[] = {initial_hwm, initial_hwm};
    const bool conflate[] = {false, false};
    zlink::pipe_t *pipes[2];
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink::pipepair (parents, pipes, hwms, conflate));

    pipe_cleanup_sink_t cleanup_sink;
    pipes[0]->set_event_sink (&cleanup_sink);
    pipes[1]->set_event_sink (&cleanup_sink);

    for (size_t i = 0; i != 8; ++i) {
        zlink::msg_t frame;
        TEST_ASSERT_SUCCESS_ERRNO (frame.init_size (1));
        TEST_ASSERT_TRUE (pipes[0]->write_and_flush (&frame));
        TEST_ASSERT_SUCCESS_ERRNO (frame.close ());
    }
    TEST_ASSERT_EQUAL_UINT64 (initial_hwm, pipes[0]->get_bytes_written ());

    //  Shrink only the reader's inbound target. The writer still has the
    //  original 8C admission window, so the planned 2C credit wake can make
    //  it writable while six frames remain in flight.
    pipes[1]->set_hwms (shrink_target, initial_hwm);
    TEST_ASSERT_EQUAL_UINT64 (shrink_target, pipes[0]->planned_out_hwm ());
    TEST_ASSERT_EQUAL_UINT64 (initial_hwm, pipes[0]->applied_out_hwm ());
    TEST_ASSERT_EQUAL_UINT64 (shrink_target, pipes[1]->planned_in_hwm ());
    TEST_ASSERT_EQUAL_UINT64 (initial_hwm, pipes[1]->applied_in_hwm ());

    zlink::msg_t blocked;
    TEST_ASSERT_SUCCESS_ERRNO (blocked.init_size (1));
    TEST_ASSERT_FALSE (pipes[0]->write_and_flush (&blocked));

    for (size_t i = 0; i != 2; ++i) {
        zlink::msg_t received;
        TEST_ASSERT_SUCCESS_ERRNO (received.init ());
        TEST_ASSERT_TRUE (pipes[1]->read (&received));
        TEST_ASSERT_SUCCESS_ERRNO (received.close ());
    }
    //  Two frame-charges reach planned LWM (2C), while the queue still has
    //  six frames. The old applied-LWM calculation (4C) would not activate.
    TEST_ASSERT_TRUE (pipes[1]->check_read ());
    process_socket_control_commands (owner_handle);
    TEST_ASSERT_EQUAL_INT (1, cleanup_sink.write_activated_count);

    for (size_t i = 2; i != 6; ++i) {
        zlink::msg_t received;
        TEST_ASSERT_SUCCESS_ERRNO (received.init ());
        TEST_ASSERT_TRUE (pipes[1]->read (&received));
        TEST_ASSERT_SUCCESS_ERRNO (received.close ());
    }
    TEST_ASSERT_TRUE (pipes[1]->check_read ());

    TEST_ASSERT_SUCCESS_ERRNO (blocked.close ());
    pipes[0]->terminate (false);
    pipes[1]->terminate (false);
    int events = 0;
    size_t events_size = sizeof (events);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_option (owner_handle, ZLINK_OPT_EVENTS, &events, &events_size));
    TEST_ASSERT_EQUAL_INT (2, cleanup_sink.terminated_count);
    close_sync_socket (owner_handle);
}

void test_prefetched_batch_tail_does_not_wake_blocked_writer_before_lwm ()
{
    void *owner_handle = create_sync_socket (ZLINK_SOCKET_PAIR);
    zlink::object_t *owner = static_cast<zlink::object_t *> (
      as_socket_handle (owner_handle).socket);
    zlink::object_t *parents[] = {owner, owner};
    const uint64_t frame_bytes = sizeof (zlink::msg_t) + 1;
    const uint64_t hwm = frame_bytes * 8;
    const uint64_t hwms[] = {hwm, hwm};
    const bool conflate[] = {false, false};
    zlink::pipe_t *pipes[2];
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink::pipepair (parents, pipes, hwms, conflate));

    pipe_cleanup_sink_t cleanup_sink;
    pipes[0]->set_event_sink (&cleanup_sink);
    pipes[1]->set_event_sink (&cleanup_sink);

    // Prefetch the first published batch, then publish the rest of the HWM
    // window as a second batch. Reading the first frame reaches the prefetched
    // batch tail, but the pipe is not drained and credit is still below LWM.
    zlink::msg_t frame;
    TEST_ASSERT_SUCCESS_ERRNO (frame.init_size (1));
    TEST_ASSERT_TRUE (pipes[0]->write_and_flush (&frame));
    TEST_ASSERT_SUCCESS_ERRNO (frame.close ());
    TEST_ASSERT_TRUE (pipes[1]->check_read ());

    for (size_t i = 1; i != 8; ++i) {
        TEST_ASSERT_SUCCESS_ERRNO (frame.init_size (1));
        TEST_ASSERT_TRUE (pipes[0]->write_and_flush (&frame));
        TEST_ASSERT_SUCCESS_ERRNO (frame.close ());
    }

    zlink::msg_t blocked;
    TEST_ASSERT_SUCCESS_ERRNO (blocked.init_size (1));
    TEST_ASSERT_FALSE (pipes[0]->write_and_flush (&blocked));

    zlink::msg_t received;
    TEST_ASSERT_SUCCESS_ERRNO (received.init ());
    TEST_ASSERT_TRUE (pipes[1]->read (&received));
    TEST_ASSERT_SUCCESS_ERRNO (received.close ());
    process_socket_control_commands (owner_handle);
    TEST_ASSERT_EQUAL_INT (0, cleanup_sink.write_activated_count);

    // The fourth consumed frame reaches the normal LWM and produces exactly
    // one writer activation while the second published batch remains readable.
    for (size_t i = 1; i != 4; ++i) {
        TEST_ASSERT_SUCCESS_ERRNO (received.init ());
        TEST_ASSERT_TRUE (pipes[1]->read (&received));
        TEST_ASSERT_SUCCESS_ERRNO (received.close ());
    }
    process_socket_control_commands (owner_handle);
    TEST_ASSERT_EQUAL_INT (1, cleanup_sink.write_activated_count);
    TEST_ASSERT_TRUE (pipes[1]->check_read ());

    TEST_ASSERT_SUCCESS_ERRNO (blocked.close ());
    pipes[0]->terminate (false);
    pipes[1]->terminate (false);
    int events = 0;
    size_t events_size = sizeof (events);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_option (owner_handle, ZLINK_OPT_EVENTS, &events, &events_size));
    TEST_ASSERT_EQUAL_INT (2, cleanup_sink.terminated_count);
    close_sync_socket (owner_handle);
}

void test_completion_pipe_does_not_apply_hwm_admission ()
{
    void *owner_handle = create_sync_socket (ZLINK_SOCKET_PAIR);
    zlink::object_t *owner = static_cast<zlink::object_t *> (
      as_socket_handle (owner_handle).socket);
    zlink::object_t *parents[] = {owner, owner};
    const uint64_t configured_hwm = 64u * 1024u;
    const uint64_t hwms[] = {configured_hwm, configured_hwm};
    const bool conflate[] = {false, false};
    zlink::pipe_t *pipes[2];
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink::pipepair (parents, pipes, hwms, conflate, false,
                       zlink::transport_lane_completion));
    pipes[0]->set_transport_pair (zlink::transport_lane_completion, 1, 1);
    pipes[1]->set_transport_pair (zlink::transport_lane_completion, 1, 1);
    pipes[0]->set_hwms (configured_hwm, configured_hwm);

    pipe_cleanup_sink_t cleanup_sink;
    pipes[0]->set_event_sink (&cleanup_sink);
    pipes[1]->set_event_sink (&cleanup_sink);

    size_t admitted = 0;
    for (; admitted < 8; ++admitted) {
        zlink::msg_t frame;
        TEST_ASSERT_SUCCESS_ERRNO (frame.init_size (64u * 1024u));
        const bool written = pipes[0]->write_and_flush (&frame);
        if (!written) {
            TEST_ASSERT_SUCCESS_ERRNO (frame.close ());
            break;
        }
    }
    TEST_ASSERT_EQUAL_UINT64 (8, admitted);

    for (size_t i = 0; i < admitted; ++i) {
        zlink::msg_t received;
        TEST_ASSERT_SUCCESS_ERRNO (received.init ());
        TEST_ASSERT_TRUE (pipes[1]->read (&received));
        TEST_ASSERT_SUCCESS_ERRNO (received.close ());
    }

    pipes[0]->terminate (false);
    pipes[1]->terminate (false);
    int events = 0;
    size_t events_size = sizeof (events);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_option (owner_handle, ZLINK_OPT_EVENTS, &events, &events_size));
    TEST_ASSERT_EQUAL_INT (2, cleanup_sink.terminated_count);
    close_sync_socket (owner_handle);
}

void test_session_completion_control_balances_registry_charge ()
{
    void *owner_handle = create_sync_socket (ZLINK_SOCKET_PAIR);
    zlink::object_t *owner = static_cast<zlink::object_t *> (
      as_socket_handle (owner_handle).socket);
    zlink::object_t *parents[] = {owner, owner};
    const uint64_t hwms[] = {0, 0};
    const bool conflate[] = {false, false};
    zlink::pipe_t *pipes[2];
    TEST_ASSERT_SUCCESS_ERRNO (zlink::pipepair (
      parents, pipes, hwms, conflate, true,
      zlink::transport_lane_completion, zlink::auto_hwm_role_none, false,
      zlink::physical_queue_class_completion, 0));
    pipes[0]->set_transport_pair (zlink::transport_lane_completion, 1, 1);
    pipes[1]->set_transport_pair (zlink::transport_lane_completion, 1, 1);
    pipes[0]->set_transport_lane_count (2);
    pipes[1]->set_transport_lane_count (2);
    pipes[0]->set_peer_socket_type (ZLINK_CORE_SOCKET_ROUTER);
    pipes[1]->set_peer_socket_type (ZLINK_CORE_SOCKET_ROUTER);

    pipe_cleanup_sink_t cleanup_sink;
    pipes[0]->set_event_sink (&cleanup_sink);
    pipes[1]->set_event_sink (&cleanup_sink);

    const bool control_written =
      pipes[0]->write_flow_state_control_and_flush (
        zlink::flow_state::receive_flow_paused, 1);
    const uint64_t expected_charge =
      sizeof (zlink::msg_t) + zlink::flow_state::frame_size;
    const zlink_auto_hwm_budget_snapshot_t queued = read_budget_snapshot ();

    zlink::msg_t received;
    TEST_ASSERT_SUCCESS_ERRNO (received.init ());
    const bool frame_received = pipes[1]->read (&received);
    zlink::flow_state::frame_t flow_frame;
    const zlink::flow_state::decode_result_t decode_result =
      frame_received
        ? zlink::flow_state::decode_frame (received, &flow_frame)
        : zlink::flow_state::decode_not_flow_frame;
    TEST_ASSERT_SUCCESS_ERRNO (received.close ());

    const zlink_auto_hwm_budget_snapshot_t drained = read_budget_snapshot ();

    pipes[0]->terminate (false);
    pipes[1]->terminate (false);
    int events = 0;
    size_t events_size = sizeof (events);
    const int events_rc =
      zlink_get_option (owner_handle, ZLINK_OPT_EVENTS, &events, &events_size);
    const int terminated_count = cleanup_sink.terminated_count;
    close_sync_socket (owner_handle);

    TEST_ASSERT_TRUE (control_written);
    TEST_ASSERT_EQUAL_UINT64 (
      expected_charge, queued.completion_current_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (1, queued.completion_pending_message_count);
    TEST_ASSERT_TRUE (frame_received);
    TEST_ASSERT_EQUAL_INT (zlink::flow_state::decode_ok, decode_result);
    TEST_ASSERT_EQUAL_UINT8 (zlink::flow_state::receive_flow_paused,
                             flow_frame.state);
    TEST_ASSERT_EQUAL_UINT64 (1, flow_frame.epoch);
    TEST_ASSERT_EQUAL_UINT64 (0,
                              drained.completion_current_accounted_bytes);
    TEST_ASSERT_EQUAL_UINT64 (0, drained.completion_pending_message_count);
    TEST_ASSERT_EQUAL_INT (0, events_rc);
    TEST_ASSERT_EQUAL_INT (2, terminated_count);
}

void test_conflate_replacement_releases_physical_queue_charge ()
{
    void *owner_handle = create_sync_socket (ZLINK_SOCKET_PAIR);
    zlink::object_t *owner = static_cast<zlink::object_t *> (
      as_socket_handle (owner_handle).socket);
    zlink::object_t *parents[] = {owner, owner};
    const uint64_t hwms[] = {0, 0};
    const bool conflate[] = {true, true};
    zlink::pipe_t *pipes[2];
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink::pipepair (parents, pipes, hwms, conflate));
    pipes[0]->set_transport_pair (zlink::transport_lane_application, 1, 1);
    pipes[1]->set_transport_pair (zlink::transport_lane_application, 1, 1);

    pipe_cleanup_sink_t cleanup_sink;
    pipes[0]->set_event_sink (&cleanup_sink);
    pipes[1]->set_event_sink (&cleanup_sink);

    zlink::msg_t first;
    TEST_ASSERT_SUCCESS_ERRNO (first.init_size (5));
    TEST_ASSERT_TRUE (pipes[0]->write_and_flush (&first));
    const uint64_t first_accounted =
      read_budget_snapshot ().current_accounted_bytes;
    TEST_ASSERT_GREATER_THAN_UINT64 (0, first_accounted);

    zlink::msg_t replacement;
    TEST_ASSERT_SUCCESS_ERRNO (replacement.init_size (19));
    TEST_ASSERT_TRUE (pipes[0]->write_and_flush (&replacement));
    const uint64_t replacement_accounted =
      read_budget_snapshot ().current_accounted_bytes;
    TEST_ASSERT_GREATER_THAN_UINT64 (first_accounted, replacement_accounted);
    TEST_ASSERT_EQUAL_UINT64 (sizeof (zlink::msg_t) + 19,
                              replacement_accounted);

    zlink::msg_t received;
    TEST_ASSERT_SUCCESS_ERRNO (received.init ());
    TEST_ASSERT_TRUE (pipes[1]->read (&received));
    TEST_ASSERT_EQUAL_UINT64 (19, received.size ());
    TEST_ASSERT_SUCCESS_ERRNO (received.close ());
    TEST_ASSERT_EQUAL_UINT64 (0,
                              read_budget_snapshot ().current_accounted_bytes);

    TEST_ASSERT_SUCCESS_ERRNO (first.close ());
    TEST_ASSERT_SUCCESS_ERRNO (replacement.close ());
    pipes[0]->terminate (false);
    pipes[1]->terminate (false);
    int events = 0;
    size_t events_size = sizeof (events);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_option (owner_handle, ZLINK_OPT_EVENTS, &events, &events_size));
    TEST_ASSERT_EQUAL_INT (2, cleanup_sink.terminated_count);
    close_sync_socket (owner_handle);
}

namespace
{
const uint64_t weighted_selection_hwm = 64u * 1024u * 1024u;

//  Owns the pipe pairs a weighted-selection scenario attaches to lb_t and
//  performs the peer handshake teardown the runtime would normally drive.
class weighted_selection_harness_t
{
  public:
    weighted_selection_harness_t () : _owner_handle (create_sync_socket (ZLINK_SOCKET_PAIR)) {}

    zlink::pipe_t *add_peer (zlink::lb_t &lb_,
                             const char *routing_id_,
                             uint32_t weight_,
                             uint64_t hwm_ = weighted_selection_hwm)
    {
        zlink::object_t *owner = static_cast<zlink::object_t *> (
          as_socket_handle (_owner_handle).socket);
        zlink::object_t *parents[] = {owner, owner};
        const uint64_t hwms[] = {hwm_, hwm_};
        const bool conflate[] = {false, false};
        zlink::pipe_t *pair[2];
        TEST_ASSERT_SUCCESS_ERRNO (zlink::pipepair (parents, pair, hwms, conflate));
        pair[0]->set_event_sink (&_sink);
        pair[1]->set_event_sink (&_sink);
        _endpoints.push_back (pair[0]);
        _endpoints.push_back (pair[1]);

        if (routing_id_) {
            pair[0]->set_peer_routing_id (reinterpret_cast<const unsigned char *> (routing_id_),
                                          strlen (routing_id_));
        }

        lb_.attach (pair[0]);
        lb_.set_weight (pair[0], weight_);
        _attached.push_back (pair[0]);
        return pair[0];
    }

    //  Detaches a peer from the candidate set without terminating it, the way
    //  a disconnect reaches lb_t.
    void detach_peer (zlink::lb_t &lb_, zlink::pipe_t *pipe_)
    {
        lb_.pipe_terminated (pipe_);
        for (size_t i = 0; i < _attached.size (); ++i) {
            if (_attached[i] == pipe_) {
                _attached.erase (_attached.begin () + static_cast<ptrdiff_t> (i));
                break;
            }
        }
    }

    void teardown (zlink::lb_t &lb_)
    {
        for (size_t i = 0; i < _attached.size (); ++i)
            lb_.pipe_terminated (_attached[i]);
        _attached.clear ();

        for (size_t i = 0; i < _endpoints.size (); ++i)
            _endpoints[i]->terminate (false);

        int events = 0;
        size_t events_size = sizeof (events);
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_get_option (_owner_handle, ZLINK_OPT_EVENTS, &events, &events_size));
        TEST_ASSERT_EQUAL_INT (static_cast<int> (_endpoints.size ()), _sink.terminated_count);
        _endpoints.clear ();
        close_sync_socket (_owner_handle);
        _owner_handle = NULL;
    }

  private:
    void *_owner_handle;
    pipe_cleanup_sink_t _sink;
    std::vector<zlink::pipe_t *> _endpoints;
    std::vector<zlink::pipe_t *> _attached;
};

//  Submits one single-part message and reports the pipe the selection
//  procedure picked, or NULL when the submit was refused.
zlink::pipe_t *submit_one (zlink::lb_t &lb_)
{
    zlink::msg_t message;
    TEST_ASSERT_SUCCESS_ERRNO (message.init_size (1));
    zlink::pipe_t *selected = NULL;
    const int rc = lb_.sendpipe (&message, &selected);
    TEST_ASSERT_SUCCESS_ERRNO (message.close ());
    return rc == 0 ? selected : NULL;
}

//  Renders a selection run as a string so a whole sequence can be compared in
//  one assertion. Unknown pipes render as '?'.
std::string selection_sequence (zlink::lb_t &lb_,
                                size_t count_,
                                zlink::pipe_t *const *pipes_,
                                const char *labels_,
                                size_t pipe_count_)
{
    std::string sequence;
    for (size_t i = 0; i < count_; ++i) {
        zlink::pipe_t *selected = submit_one (lb_);
        char label = '-';
        for (size_t p = 0; p < pipe_count_; ++p) {
            if (pipes_[p] == selected) {
                label = labels_[p];
                break;
            }
        }
        if (label == '-' && selected != NULL)
            label = '?';
        sequence.push_back (label);
    }
    return sequence;
}
}

void test_weighted_selection_spreads_consecutive_picks ()
{
    zlink::lb_t lb;
    weighted_selection_harness_t harness;
    zlink::pipe_t *pipes[2];
    pipes[0] = harness.add_peer (lb, "A", 100);
    pipes[1] = harness.add_peer (lb, "B", 300);

    //  A 1:3 ratio must not hand three consecutive messages to the same peer.
    TEST_ASSERT_EQUAL_STRING ("BABB", selection_sequence (lb, 4, pipes, "AB", 2).c_str ());

    harness.teardown (lb);
}

void test_equal_weights_alternate_through_the_same_procedure ()
{
    zlink::lb_t lb;
    weighted_selection_harness_t harness;
    zlink::pipe_t *pipes[2];
    pipes[0] = harness.add_peer (lb, "A", 100);
    pipes[1] = harness.add_peer (lb, "B", 100);

    //  Equal weights are not a separate code path. The procedure alternates on
    //  its own and starts with the lower identifier.
    TEST_ASSERT_EQUAL_STRING ("ABABAB", selection_sequence (lb, 6, pipes, "AB", 2).c_str ());

    harness.teardown (lb);
}

void test_routed_target_selection_commits_once_before_exact_submit ()
{
    zlink::lb_t lb;
    weighted_selection_harness_t harness;
    zlink::pipe_t *a = harness.add_peer (lb, "A", 100);
    zlink::pipe_t *b = harness.add_peer (lb, "B", 100);

    // Routed target selection is the weighted commit boundary even though it
    // does not reserve pipe credit. Consecutive async snapshots therefore
    // alternate before either exact payload is submitted.
    zlink::pipe_t *first = NULL;
    zlink::pipe_t *second = NULL;
    TEST_ASSERT_SUCCESS_ERRNO (lb.select_connected_pipe (&first));
    TEST_ASSERT_SUCCESS_ERRNO (lb.select_connected_pipe (&second));
    TEST_ASSERT_EQUAL_PTR (a, first);
    TEST_ASSERT_EQUAL_PTR (b, second);

    // Exact submit consumes the already chosen target, not another scheduler
    // step. Abandoning the second snapshot and submitting the first must leave
    // the next selection at A; a second commit in sendpipe_to would choose B.
    zlink::msg_t message;
    TEST_ASSERT_SUCCESS_ERRNO (message.init_size (1));
    TEST_ASSERT_SUCCESS_ERRNO (lb.sendpipe_to (first, &message));
    zlink::pipe_t *third = NULL;
    TEST_ASSERT_SUCCESS_ERRNO (lb.select_connected_pipe (&third));
    TEST_ASSERT_EQUAL_PTR (a, third);
    TEST_ASSERT_SUCCESS_ERRNO (message.close ());

    harness.teardown (lb);
}

void test_weighted_selection_ignores_attach_order ()
{
    zlink::lb_t lb;
    weighted_selection_harness_t harness;
    //  Same peers, reversed attach order. The identifier decides the order,
    //  not the connect sequence.
    zlink::pipe_t *b = harness.add_peer (lb, "B", 300);
    zlink::pipe_t *a = harness.add_peer (lb, "A", 100);
    zlink::pipe_t *pipes[2] = {a, b};

    TEST_ASSERT_EQUAL_STRING ("BABB", selection_sequence (lb, 4, pipes, "AB", 2).c_str ());

    harness.teardown (lb);
}

void test_weighted_selection_keeps_ratio_across_pipe_changes ()
{
    zlink::lb_t lb;
    weighted_selection_harness_t harness;
    zlink::pipe_t *pipes[2];
    pipes[0] = harness.add_peer (lb, "A", 100);
    pipes[1] = harness.add_peer (lb, "B", 300);

    TEST_ASSERT_EQUAL_STRING ("BA", selection_sequence (lb, 2, pipes, "AB", 2).c_str ());

    //  A third peer joins and leaves again without being used. The running
    //  values of the peers that stay must survive the candidate-set change.
    zlink::pipe_t *transient = harness.add_peer (lb, "C", 100);
    harness.detach_peer (lb, transient);

    TEST_ASSERT_EQUAL_STRING ("BB", selection_sequence (lb, 2, pipes, "AB", 2).c_str ());

    //  The full period repeats, so the ratio is unchanged.
    TEST_ASSERT_EQUAL_STRING ("BABB", selection_sequence (lb, 4, pipes, "AB", 2).c_str ());

    harness.teardown (lb);
}

void test_weighted_selection_converges_to_wide_range_ratio ()
{
    zlink::lb_t lb;
    weighted_selection_harness_t harness;
    zlink::pipe_t *pipes[2];
    pipes[0] = harness.add_peer (lb, "A", 5000);
    pipes[1] = harness.add_peer (lb, "B", 10000);

    const std::string sequence = selection_sequence (lb, 300, pipes, "AB", 2);
    size_t first_count = 0;
    size_t second_count = 0;
    for (size_t i = 0; i < sequence.size (); ++i) {
        if (sequence[i] == 'A')
            ++first_count;
        else if (sequence[i] == 'B')
            ++second_count;
    }

    TEST_ASSERT_EQUAL_UINT (100, first_count);
    TEST_ASSERT_EQUAL_UINT (200, second_count);

    harness.teardown (lb);
}

void test_write_failure_restores_candidate_after_recovery ()
{
    zlink::lb_t lb;
    weighted_selection_harness_t harness;
    zlink::pipe_t *pipes[2];
    //  The second peer accepts a single frame before it runs out of credit.
    const uint64_t single_frame_hwm = sizeof (zlink::msg_t) + 1;
    pipes[0] = harness.add_peer (lb, "A", 100);
    pipes[1] = harness.add_peer (lb, "B", 100, single_frame_hwm);

    TEST_ASSERT_EQUAL_STRING ("AB", selection_sequence (lb, 2, pipes, "AB", 2).c_str ());

    //  B has no credit left. The failed write must not consume the message
    //  and must not take A out of the candidate set.
    TEST_ASSERT_EQUAL_STRING ("AA", selection_sequence (lb, 2, pipes, "AB", 2).c_str ());

    //  Fresh write credit returns B to the candidate set.
    pipes[1]->refresh_write_credit (1, pipes[1]->get_bytes_written ());
    lb.activated (pipes[1]);
    const std::string after_recovery = selection_sequence (lb, 2, pipes, "AB", 2);
    TEST_ASSERT_TRUE_MESSAGE (after_recovery.find ('B') != std::string::npos,
                              "recovered pipe did not return to the candidate set");

    harness.teardown (lb);
}

void test_dist_message_preflight_consumes_published_credit_before_owner_wake ()
{
    void *writer_owner_handle = create_sync_socket (ZLINK_SOCKET_PAIR);
    void *reader_owner_handle = create_sync_socket (ZLINK_SOCKET_PAIR);
    zlink::object_t *writer_owner = static_cast<zlink::object_t *> (
      as_socket_handle (writer_owner_handle).socket);
    zlink::object_t *reader_owner = static_cast<zlink::object_t *> (
      as_socket_handle (reader_owner_handle).socket);
    TEST_ASSERT_NOT_EQUAL (writer_owner->get_tid (), reader_owner->get_tid ());
    zlink::object_t *parents[] = {writer_owner, reader_owner};
    const uint64_t frame_bytes = sizeof (zlink::msg_t) + 1;
    const uint64_t hwms[] = {frame_bytes, frame_bytes};
    const bool conflate[] = {false, false};
    zlink::pipe_t *pipes[2];
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink::pipepair (parents, pipes, hwms, conflate));

    class activating_sink_t : public zlink::i_pipe_events
    {
      public:
        activating_sink_t (zlink::dist_t *dist_, zlink::pipe_t *writer_) :
            dist (dist_), writer (writer_), write_activated_count (0),
            terminated_count (0)
        {
        }

        void read_activated (zlink::pipe_t *) ZLINK_OVERRIDE {}
        void write_activated (zlink::pipe_t *pipe_) ZLINK_OVERRIDE
        {
            ++write_activated_count;
            if (pipe_ == writer)
                dist->activated (pipe_);
        }
        void hiccuped (zlink::pipe_t *) ZLINK_OVERRIDE {}
        void pipe_peer_terminated (zlink::pipe_t *) ZLINK_OVERRIDE {}
        void pipe_terminated (zlink::pipe_t *) ZLINK_OVERRIDE
        {
            ++terminated_count;
        }

        zlink::dist_t *dist;
        zlink::pipe_t *writer;
        int write_activated_count;
        int terminated_count;
    };

    zlink::dist_t dist;
    activating_sink_t sink (&dist, pipes[0]);
    pipes[0]->set_event_sink (&sink);
    pipes[1]->set_event_sink (&sink);
    dist.attach (pipes[0]);

    zlink::msg_t first;
    TEST_ASSERT_SUCCESS_ERRNO (first.init_size (1));
    TEST_ASSERT_SUCCESS_ERRNO (dist.send_to_all (&first));
    TEST_ASSERT_SUCCESS_ERRNO (first.close ());

    zlink::msg_t blocked;
    TEST_ASSERT_SUCCESS_ERRNO (blocked.init_size (1));
    const zlink::pipe_message_admission_t blocked_admission =
      dist.check_hwm (&blocked);

    zlink::msg_t received;
    TEST_ASSERT_SUCCESS_ERRNO (received.init ());
    TEST_ASSERT_TRUE (pipes[1]->read (&received));
    TEST_ASSERT_SUCCESS_ERRNO (received.close ());

    // The peer has published enough byte credit and queued activate_write,
    // but the writer owner has not processed that command yet. dist_t still
    // owns active membership, so its message-aware preflight can recover
    // immediately without waiting for an otherwise redundant mailbox turn.
    const zlink::pipe_message_admission_t recovered_admission =
      dist.check_hwm (&blocked);
    const bool sent_before_owner_wake =
      recovered_admission == zlink::pipe_message_admission_ready;
    if (sent_before_owner_wake)
        TEST_ASSERT_SUCCESS_ERRNO (dist.send_to_matching (&blocked));
    TEST_ASSERT_SUCCESS_ERRNO (blocked.close ());

    bool received_before_owner_wake = false;
    TEST_ASSERT_SUCCESS_ERRNO (received.init ());
    if (sent_before_owner_wake) {
        received_before_owner_wake = pipes[1]->read (&received);
        if (received_before_owner_wake)
            TEST_ASSERT_SUCCESS_ERRNO (received.close ());
    }
    if (!received_before_owner_wake)
        TEST_ASSERT_SUCCESS_ERRNO (received.close ());

    process_socket_control_commands (writer_owner_handle);
    process_socket_control_commands (writer_owner_handle);
    const int activation_count = sink.write_activated_count;

    dist.pipe_terminated (pipes[0]);
    pipes[0]->terminate (false);
    pipes[1]->terminate (false);
    for (int i = 0; i != 3; ++i) {
        process_socket_control_commands (writer_owner_handle);
        process_socket_control_commands (reader_owner_handle);
    }
    TEST_ASSERT_EQUAL_INT (2, sink.terminated_count);
    close_sync_socket (reader_owner_handle);
    close_sync_socket (writer_owner_handle);

    TEST_ASSERT_EQUAL_INT (zlink::pipe_message_admission_hwm_full,
                           blocked_admission);
    TEST_ASSERT_EQUAL_INT (zlink::pipe_message_admission_ready,
                           recovered_admission);
    TEST_ASSERT_TRUE (received_before_owner_wake);
    TEST_ASSERT_EQUAL_INT (0, activation_count);
}

void test_passive_hwm_probe_does_not_consume_write_activation ()
{
    void *writer_owner_handle = create_sync_socket (ZLINK_SOCKET_PAIR);
    void *reader_owner_handle = create_sync_socket (ZLINK_SOCKET_PAIR);
    zlink::object_t *writer_owner = static_cast<zlink::object_t *> (
      as_socket_handle (writer_owner_handle).socket);
    zlink::object_t *reader_owner = static_cast<zlink::object_t *> (
      as_socket_handle (reader_owner_handle).socket);
    TEST_ASSERT_NOT_EQUAL (writer_owner->get_tid (), reader_owner->get_tid ());
    zlink::object_t *parents[] = {writer_owner, reader_owner};
    const uint64_t frame_bytes = sizeof (zlink::msg_t) + 1;
    const uint64_t hwms[] = {frame_bytes, frame_bytes};
    const bool conflate[] = {false, false};
    zlink::pipe_t *pipes[2];
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink::pipepair (parents, pipes, hwms, conflate));

    class activating_sink_t : public zlink::i_pipe_events
    {
      public:
        explicit activating_sink_t (zlink::lb_t *lb_) :
            lb (lb_), write_activated_count (0), terminated_count (0)
        {
        }

        void read_activated (zlink::pipe_t *) ZLINK_OVERRIDE {}
        void write_activated (zlink::pipe_t *pipe_) ZLINK_OVERRIDE
        {
            ++write_activated_count;
            lb->activated (pipe_);
        }
        void hiccuped (zlink::pipe_t *) ZLINK_OVERRIDE {}
        void pipe_peer_terminated (zlink::pipe_t *) ZLINK_OVERRIDE {}
        void pipe_terminated (zlink::pipe_t *) ZLINK_OVERRIDE
        {
            ++terminated_count;
        }

        zlink::lb_t *lb;
        int write_activated_count;
        int terminated_count;
    };

    zlink::lb_t lb;
    activating_sink_t sink (&lb);
    pipes[0]->set_event_sink (&sink);
    pipes[1]->set_event_sink (&sink);
    lb.attach (pipes[0]);

    zlink::msg_t first;
    TEST_ASSERT_SUCCESS_ERRNO (first.init_size (1));
    TEST_ASSERT_SUCCESS_ERRNO (lb.send (&first));
    TEST_ASSERT_SUCCESS_ERRNO (first.close ());

    zlink::msg_t blocked;
    TEST_ASSERT_SUCCESS_ERRNO (blocked.init_size (1));
    zlink::pipe_message_admission_t admission =
      zlink::pipe_message_admission_invalid;
    TEST_ASSERT_EQUAL_INT (-1, lb.send (&blocked, &admission));
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);
    TEST_ASSERT_EQUAL_INT (zlink::pipe_message_admission_hwm_full, admission);
    TEST_ASSERT_SUCCESS_ERRNO (blocked.close ());

    zlink::msg_t received;
    TEST_ASSERT_SUCCESS_ERRNO (received.init ());
    TEST_ASSERT_TRUE (pipes[1]->read (&received));
    TEST_ASSERT_SUCCESS_ERRNO (received.close ());

    // Model the interval after peer credit publication and before the owner
    // processes its matching activate_write command. This passive all-pipes
    // classification probe must not make the pipe active behind lb_t's back.
    zlink::msg_t probe;
    TEST_ASSERT_SUCCESS_ERRNO (probe.init_size (1));
    admission = zlink::pipe_message_admission_invalid;
    const int probe_rc = lb.send (&probe, &admission);
    const int probe_errno = errno;
    const zlink::pipe_message_admission_t probe_admission = admission;
    TEST_ASSERT_SUCCESS_ERRNO (probe.close ());

    process_socket_control_commands (writer_owner_handle);
    process_socket_control_commands (writer_owner_handle);
    const int activation_count = sink.write_activated_count;

    zlink::msg_t recovered;
    TEST_ASSERT_SUCCESS_ERRNO (recovered.init_size (1));
    const int recovered_rc = lb.send (&recovered);
    TEST_ASSERT_SUCCESS_ERRNO (recovered.close ());

    lb.pipe_terminated (pipes[0]);
    pipes[0]->terminate (false);
    pipes[1]->terminate (false);
    for (int i = 0; i != 3; ++i) {
        process_socket_control_commands (writer_owner_handle);
        process_socket_control_commands (reader_owner_handle);
    }
    TEST_ASSERT_EQUAL_INT (2, sink.terminated_count);
    close_sync_socket (reader_owner_handle);
    close_sync_socket (writer_owner_handle);

    TEST_ASSERT_EQUAL_INT (-1, probe_rc);
    TEST_ASSERT_EQUAL_INT (EAGAIN, probe_errno);
    TEST_ASSERT_EQUAL_INT (zlink::pipe_message_admission_hwm_full,
                           probe_admission);
    TEST_ASSERT_EQUAL_INT (1, activation_count);
    TEST_ASSERT_EQUAL_INT (0, recovered_rc);
}

int main ()
{
    setup_test_environment ();

    UNITY_BEGIN ();
    RUN_TEST (test_router_multiple_dealers_tcp);
    RUN_TEST (test_router_multiple_dealers_ipc);
    RUN_TEST (test_router_multiple_dealers_inproc);
    RUN_TEST (test_weighted_dealer_preserves_peer_weight_after_backpressure);
    RUN_TEST (test_unpaired_inproc_peer_weight_is_not_application_data);
    RUN_TEST (
      test_peer_control_does_not_complete_open_application_multipart);
    RUN_TEST (test_connection_guarded_write_rejects_stale_generation);
    RUN_TEST (test_peer_control_slots_reject_non_dealer_router_pipe);
    RUN_TEST (test_weighted_lb_reactivation_keeps_configured_weight);
    RUN_TEST (test_weight_zero_between_parts_preserves_selected_message);
    RUN_TEST (test_single_pipe_lb_rolls_back_byte_hwm_rejected_multipart);
    RUN_TEST (test_single_pipe_dist_rolls_back_byte_hwm_rejected_multipart);
    RUN_TEST (test_pipe_rejects_multipart_before_partial_bytes_exceed_hwm);
    RUN_TEST (test_empty_pipe_incomplete_multipart_stops_at_max_message_size);
    RUN_TEST (test_empty_pipe_oversize_exception_applies_only_to_complete_message);
    RUN_TEST (test_drained_pipe_oversize_multipart_uses_fresh_peer_credit);
    RUN_TEST (test_physical_queue_snapshot_accounts_multipart_once);
    RUN_TEST (test_physical_queue_deferred_shrink_applies_on_drain);
    RUN_TEST (test_deferred_shrink_wakes_writer_at_planned_lwm);
    RUN_TEST (
      test_prefetched_batch_tail_does_not_wake_blocked_writer_before_lwm);
    RUN_TEST (test_completion_pipe_does_not_apply_hwm_admission);
    RUN_TEST (test_session_completion_control_balances_registry_charge);
    RUN_TEST (test_conflate_replacement_releases_physical_queue_charge);
    RUN_TEST (test_weighted_selection_spreads_consecutive_picks);
    RUN_TEST (test_equal_weights_alternate_through_the_same_procedure);
    RUN_TEST (
      test_routed_target_selection_commits_once_before_exact_submit);
    RUN_TEST (test_weighted_selection_ignores_attach_order);
    RUN_TEST (test_weighted_selection_keeps_ratio_across_pipe_changes);
    RUN_TEST (test_weighted_selection_converges_to_wide_range_ratio);
    RUN_TEST (test_write_failure_restores_candidate_after_recovery);
    RUN_TEST (
      test_dist_message_preflight_consumes_published_credit_before_owner_wake);
    RUN_TEST (test_passive_hwm_probe_does_not_consume_write_activation);
    return UNITY_END ();
}
