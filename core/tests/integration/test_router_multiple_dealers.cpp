/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"
#include "core/object.hpp"
#include "core/pipe.hpp"
#include "sockets/internal/dist.hpp"
#include "sockets/internal/lb.hpp"

#include <unity.h>
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
    pipe_cleanup_sink_t () : terminated_count (0) {}

    void read_activated (zlink::pipe_t *) ZLINK_OVERRIDE {}
    void write_activated (zlink::pipe_t *) ZLINK_OVERRIDE {}
    void hiccuped (zlink::pipe_t *) ZLINK_OVERRIDE {}
    void pipe_peer_terminated (zlink::pipe_t *) ZLINK_OVERRIDE {}
    void pipe_terminated (zlink::pipe_t *) ZLINK_OVERRIDE
    {
        ++terminated_count;
    }

    int terminated_count;
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

void test_weighted_lb_reactivation_keeps_configured_weight ()
{
    void *owner_handle = create_sync_socket (ZLINK_SOCKET_PAIR);
    zlink::object_t *owner = static_cast<zlink::object_t *> (owner_handle);
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

void test_single_pipe_lb_rolls_back_byte_hwm_rejected_multipart ()
{
    void *owner_handle = create_sync_socket (ZLINK_SOCKET_PAIR);
    zlink::object_t *owner = static_cast<zlink::object_t *> (owner_handle);
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
    zlink::object_t *owner = static_cast<zlink::object_t *> (owner_handle);
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
    zlink::object_t *owner = static_cast<zlink::object_t *> (owner_handle);
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
    zlink::object_t *owner = static_cast<zlink::object_t *> (owner_handle);
    zlink::object_t *parents[] = {owner, owner};
    const uint64_t frame_bytes = sizeof (zlink::msg_t) + 1;
    const uint64_t hwms[] = {frame_bytes * 3, frame_bytes * 3};
    const bool conflate[] = {false, false};
    zlink::pipe_t *pipes[2];
    TEST_ASSERT_SUCCESS_ERRNO (zlink::pipepair (parents, pipes, hwms, conflate));
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
    zlink::object_t *owner = static_cast<zlink::object_t *> (owner_handle);
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

void test_completion_pipe_hwm_is_capped_by_internal_lane_policy ()
{
    void *owner_handle = create_sync_socket (ZLINK_SOCKET_PAIR);
    zlink::object_t *owner = static_cast<zlink::object_t *> (owner_handle);
    zlink::object_t *parents[] = {owner, owner};
    const uint64_t configured_hwm = 4u * 1024u * 1024u;
    const uint64_t hwms[] = {configured_hwm, configured_hwm};
    const bool conflate[] = {false, false};
    zlink::pipe_t *pipes[2];
    TEST_ASSERT_SUCCESS_ERRNO (zlink::pipepair (parents, pipes, hwms, conflate));
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
    TEST_ASSERT_TRUE (admitted > 0);
    TEST_ASSERT_TRUE (admitted < 8);

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
        zlink::object_t *owner = static_cast<zlink::object_t *> (_owner_handle);
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

int main ()
{
    setup_test_environment ();

    UNITY_BEGIN ();
    RUN_TEST (test_router_multiple_dealers_tcp);
    RUN_TEST (test_router_multiple_dealers_ipc);
    RUN_TEST (test_router_multiple_dealers_inproc);
    RUN_TEST (test_weighted_dealer_preserves_peer_weight_after_backpressure);
    RUN_TEST (test_weighted_lb_reactivation_keeps_configured_weight);
    RUN_TEST (test_single_pipe_lb_rolls_back_byte_hwm_rejected_multipart);
    RUN_TEST (test_single_pipe_dist_rolls_back_byte_hwm_rejected_multipart);
    RUN_TEST (test_pipe_rejects_multipart_before_partial_bytes_exceed_hwm);
    RUN_TEST (test_empty_pipe_incomplete_multipart_stops_at_max_message_size);
    RUN_TEST (test_empty_pipe_oversize_exception_applies_only_to_complete_message);
    RUN_TEST (test_completion_pipe_hwm_is_capped_by_internal_lane_policy);
    RUN_TEST (test_weighted_selection_spreads_consecutive_picks);
    RUN_TEST (test_equal_weights_alternate_through_the_same_procedure);
    RUN_TEST (test_weighted_selection_ignores_attach_order);
    RUN_TEST (test_weighted_selection_keeps_ratio_across_pipe_changes);
    RUN_TEST (test_weighted_selection_converges_to_wide_range_ratio);
    RUN_TEST (test_write_failure_restores_candidate_after_recovery);
    return UNITY_END ();
}
