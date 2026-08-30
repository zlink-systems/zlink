/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include "api/socket/socket_api_internal.hpp"
#include "core/ctx.hpp"
#include "core/msg.hpp"
#include "core/pipe.hpp"
#include "protocol/zmp_protocol.hpp"
#include "sockets/common/socket_base.hpp"
#include "sockets/pubsub/xsub.hpp"

#include <unity.h>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace zlink
{
class session_termination_test_access_t
{
  public:
    static void attach_socket_pipe (socket_base_t *socket_, pipe_t *pipe_)
    {
        socket_->attach_pipe (pipe_);
    }

    static void terminate_socket_pipe (socket_base_t *socket_, pipe_t *pipe_)
    {
        socket_->xpipe_terminated (pipe_);
    }

    static void install_socket_msg_handler (
      socket_base_t *socket_, zlink_socket_msg_handler_fn handler_,
      void *userdata_)
    {
        std::lock_guard<std::recursive_mutex> lock (
          socket_->dispatch_runtime ().socket_msg_dispatch_sync);
        socket_->dispatch_runtime ().socket_msg_handler_userdata.store (
          userdata_, std::memory_order_release);
        socket_->dispatch_runtime ().socket_msg_handler.store (
          handler_, std::memory_order_release);
    }

    static void clear_socket_msg_handler (socket_base_t *socket_)
    {
        std::lock_guard<std::recursive_mutex> lock (
          socket_->dispatch_runtime ().socket_msg_dispatch_sync);
        socket_->dispatch_runtime ().socket_msg_handler.store (
          NULL, std::memory_order_release);
        socket_->dispatch_runtime ().socket_msg_handler_userdata.store (
          NULL, std::memory_order_release);
    }

    static int dispatch_socket_msg_part (socket_base_t *socket_,
                                         msg_t *msg_, pipe_t *pipe_)
    {
        std::lock_guard<std::recursive_mutex> lock (
          socket_->dispatch_runtime ().socket_msg_dispatch_sync);
        return socket_->xsocket_msg_dispatch (msg_, pipe_);
    }

    static size_t xsub_dispatch_state_creations (xsub_t *socket_)
    {
        std::lock_guard<std::recursive_mutex> lock (
          socket_->dispatch_runtime ().socket_msg_dispatch_sync);
        return socket_->_socket_dispatch_state_creations;
    }
};
}

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
class passive_pipe_sink_t : public zlink::i_pipe_events
{
  public:
    void read_activated (zlink::pipe_t *) ZLINK_OVERRIDE {}
    void write_activated (zlink::pipe_t *) ZLINK_OVERRIDE {}
    void hiccuped (zlink::pipe_t *) ZLINK_OVERRIDE {}
    void pipe_peer_terminated (zlink::pipe_t *) ZLINK_OVERRIDE {}
    void pipe_terminated (zlink::pipe_t *) ZLINK_OVERRIDE {}
};

void write_internal_pipe_part (zlink::pipe_t *pipe_,
                               const char *payload_,
                               bool more_)
{
    zlink::msg_t msg;
    const size_t size = std::strlen (payload_);
    TEST_ASSERT_SUCCESS_ERRNO (msg.init_size (size));
    memcpy (msg.data (), payload_, size);
    if (more_)
        msg.set_flags (zlink::msg_t::more);
    TEST_ASSERT_TRUE (pipe_->write (&msg));
    pipe_->flush ();
    TEST_ASSERT_SUCCESS_ERRNO (msg.close ());
}

struct socket_msg_record_probe_t
{
    std::vector<std::vector<std::string> > records;
    bool metadata_leaked;

    socket_msg_record_probe_t () : metadata_leaked (false) {}
};

void capture_socket_msg_record (const zlink_routing_id_t *,
                                zlink_msg_t *parts_,
                                size_t part_count_,
                                void *userdata_)
{
    socket_msg_record_probe_t *probe =
      static_cast<socket_msg_record_probe_t *> (userdata_);
    std::vector<std::string> record;
    for (size_t i = 0; i < part_count_; ++i) {
        zlink::msg_t *part = reinterpret_cast<zlink::msg_t *> (&parts_[i]);
        unsigned char kind = 0;
        uint64_t sequence = 0;
        probe->metadata_leaked =
          probe->metadata_leaked
          || part->get_request_reply_metadata (&kind, &sequence);
        record.push_back (std::string (
          static_cast<const char *> (part->data ()), part->size ()));
        TEST_ASSERT_SUCCESS_ERRNO (part->close ());
    }
    probe->records.push_back (record);
}

void dispatch_socket_msg_part (zlink::socket_base_t *socket_,
                               zlink::pipe_t *pipe_,
                               const char *payload_,
                               bool more_,
                               bool request_metadata_ = false)
{
    zlink::msg_t msg;
    const size_t payload_size = std::strlen (payload_);
    TEST_ASSERT_SUCCESS_ERRNO (msg.init_size (payload_size));
    memcpy (msg.data (), payload_, payload_size);
    if (more_)
        msg.set_flags (zlink::msg_t::more);
    if (request_metadata_)
        TEST_ASSERT_SUCCESS_ERRNO (msg.set_request_reply_metadata (
          zlink::zmp_kind_request, UINT64_C (41)));
    TEST_ASSERT_EQUAL_INT (
      1, zlink::session_termination_test_access_t::dispatch_socket_msg_part (
           socket_, &msg, pipe_));
    TEST_ASSERT_SUCCESS_ERRNO (msg.close ());
}

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

static void test_pubsub_filter_transport (const char *endpoint_)
{
    void *pub = create_sync_socket (ZLINK_SOCKET_PUB);
    void *sub = create_sync_socket (ZLINK_SOCKET_SUB);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (pub, endpoint_));

    char connect_endpoint[MAX_SOCKET_STRING];
    if (strncmp (endpoint_, "tcp://", 6) == 0 || strncmp (endpoint_, "ipc://", 6) == 0) {
        size_t len = sizeof (connect_endpoint);
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_get_option (pub, ZLINK_OPT_LAST_ENDPOINT, connect_endpoint, &len));
    } else {
        strcpy (connect_endpoint, endpoint_);
    }

    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (sub, connect_endpoint));

    // Subscribe only to "topicA"
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (sub, "topicA"));

    msleep (SETTLE_TIME);

    // Send messages with different topics
    send_string_expect_success (pub, "topicA hello", 0);
    send_string_expect_success (pub, "topicB world", 0);
    send_string_expect_success (pub, "topicA test", 0);

    // Should only receive topicA messages
    recv_string_expect_success (sub, "topicA hello", 0);
    recv_string_expect_success (sub, "topicA test", 0);

    close_sync_socket (sub);
    close_sync_socket (pub);
}

void test_pubsub_filter_tcp ()
{
    test_pubsub_filter_transport ("tcp://127.0.0.1:*");
}

void test_pubsub_xpub_xsub_inproc ()
{
    void *xpub = create_sync_socket (ZLINK_SOCKET_XPUB);
    void *xsub = create_sync_socket (ZLINK_SOCKET_XSUB);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (xpub, "inproc://test_xpub_xsub"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (xsub, "inproc://test_xpub_xsub"));

    // XSUB subscribe
    char sub_msg[] = {0x01, 0};
    TEST_ASSERT_SUCCESS_ERRNO (zlink_send (xsub, sub_msg, 1, 0));

    // Wait for subscription to propagate
    char sub_recv[16];
    const int sub_size =
      TEST_ASSERT_SUCCESS_ERRNO (zlink_recv (xpub, sub_recv, sizeof (sub_recv), 0));
    TEST_ASSERT_TRUE (sub_size >= 1);
    TEST_ASSERT_EQUAL_HEX8 (0x01, (unsigned char) sub_recv[0]);

    // Test message flow
    const char *msg = "xpub_xsub_test";
    send_string_expect_success (xpub, msg, 0);
    recv_string_expect_success (xsub, msg, 0);

    close_sync_socket (xsub);
    close_sync_socket (xpub);
}

void test_xsub_multipart_pipe_termination_does_not_join_next_peer_record ()
{
    void *xsub_handle = create_sync_socket (ZLINK_SOCKET_XSUB);
    socket_handle_t xsub_pin = as_socket_handle (xsub_handle);
    TEST_ASSERT_NOT_NULL (xsub_pin.socket);
    zlink::xsub_t *xsub = static_cast<zlink::xsub_t *> (xsub_pin.socket);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (xsub_handle, ""));

    zlink::object_t *parents[2] = {xsub, xsub};
    zlink::pipe_t *pipe_a[2] = {NULL, NULL};
    zlink::pipe_t *pipe_b[2] = {NULL, NULL};
    const uint64_t hwms[2] = {1024 * 1024, 1024 * 1024};
    const bool conflates[2] = {false, false};
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink::pipepair (parents, pipe_a, hwms, conflates, true));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink::pipepair (parents, pipe_b, hwms, conflates, true));

    passive_pipe_sink_t peer_a_sink;
    passive_pipe_sink_t peer_b_sink;
    pipe_a[1]->set_event_sink (&peer_a_sink);
    pipe_b[1]->set_event_sink (&peer_b_sink);

    write_internal_pipe_part (pipe_a[1], "topic-A", true);
    write_internal_pipe_part (pipe_a[1], "payload-A", false);
    write_internal_pipe_part (pipe_b[1], "topic-B", true);
    write_internal_pipe_part (pipe_b[1], "payload-B", false);
    zlink::session_termination_test_access_t::attach_socket_pipe (
      xsub, pipe_a[0]);
    zlink::session_termination_test_access_t::attach_socket_pipe (
      xsub, pipe_b[0]);

    zlink::msg_t frame;
    TEST_ASSERT_SUCCESS_ERRNO (frame.init ());
    TEST_ASSERT_SUCCESS_ERRNO (xsub->recv (&frame, ZLINK_DONTWAIT));
    TEST_ASSERT_EQUAL_UINT64 (7, frame.size ());
    TEST_ASSERT_EQUAL_MEMORY ("topic-A", frame.data (), frame.size ());
    TEST_ASSERT_TRUE ((frame.flags () & zlink::msg_t::more) != 0);

    zlink::session_termination_test_access_t::terminate_socket_pipe (
      xsub, pipe_a[0]);
    TEST_ASSERT_EQUAL_INT (-1, xsub->recv (&frame, ZLINK_DONTWAIT));
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);

    TEST_ASSERT_SUCCESS_ERRNO (xsub->recv (&frame, ZLINK_DONTWAIT));
    TEST_ASSERT_EQUAL_UINT64 (7, frame.size ());
    TEST_ASSERT_EQUAL_MEMORY ("topic-B", frame.data (), frame.size ());
    TEST_ASSERT_TRUE ((frame.flags () & zlink::msg_t::more) != 0);
    TEST_ASSERT_SUCCESS_ERRNO (xsub->recv (&frame, ZLINK_DONTWAIT));
    TEST_ASSERT_EQUAL_UINT64 (9, frame.size ());
    TEST_ASSERT_EQUAL_MEMORY ("payload-B", frame.data (), frame.size ());
    TEST_ASSERT_FALSE ((frame.flags () & zlink::msg_t::more) != 0);
    TEST_ASSERT_SUCCESS_ERRNO (frame.close ());

    // Complete the ordinary two-sided pipe handshakes. Calling the socket's
    // termination callback above models state removal only; it does not own
    // the pipe objects' lifecycle.
    pipe_a[0]->terminate (false);
    pipe_a[1]->terminate (false);
    pipe_b[0]->terminate (false);
    pipe_b[1]->terminate (false);
    int events = 0;
    size_t events_size = sizeof (events);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_option (xsub_handle, ZLINK_OPT_EVENTS, &events,
                        &events_size));

    xsub_pin = socket_handle_t ();
    close_sync_socket (xsub_handle);
    zlink::ctx_t *ctx =
      static_cast<zlink::ctx_t *> (get_test_context ());
    TEST_ASSERT_SUCCESS_ERRNO (
      ctx->wait_for_socket_count_at_most (0, 5000));
}

void test_xsub_socket_dispatch_keeps_interleaved_publishers_separate ()
{
    void *xsub_handle = create_sync_socket (ZLINK_SOCKET_XSUB);
    socket_handle_t xsub_pin = as_socket_handle (xsub_handle);
    TEST_ASSERT_NOT_NULL (xsub_pin.socket);
    zlink::xsub_t *xsub = static_cast<zlink::xsub_t *> (xsub_pin.socket);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (xsub_handle, ""));

    zlink::object_t *parents[2] = {xsub, xsub};
    zlink::pipe_t *pipe_a[2] = {NULL, NULL};
    zlink::pipe_t *pipe_b[2] = {NULL, NULL};
    const uint64_t hwms[2] = {1024 * 1024, 1024 * 1024};
    const bool conflates[2] = {false, false};
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink::pipepair (parents, pipe_a, hwms, conflates, true));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink::pipepair (parents, pipe_b, hwms, conflates, true));

    passive_pipe_sink_t peer_a_sink;
    passive_pipe_sink_t peer_b_sink;
    pipe_a[1]->set_event_sink (&peer_a_sink);
    pipe_b[1]->set_event_sink (&peer_b_sink);
    zlink::session_termination_test_access_t::attach_socket_pipe (
      xsub, pipe_a[0]);
    zlink::session_termination_test_access_t::attach_socket_pipe (
      xsub, pipe_b[0]);

    socket_msg_record_probe_t probe;
    zlink::session_termination_test_access_t::install_socket_msg_handler (
      xsub, &capture_socket_msg_record, &probe);

    // Session I/O dispatch is frame-granular. Keep A's multipart record open,
    // complete B's one-part record (whose valid first frame carries internal
    // request metadata), then finish A. Per-pipe assembly must neither splice
    // the records nor mistake B's first frame for A's continuation. B must use
    // the one-part path without creating a transient multipart state.
    dispatch_socket_msg_part (xsub, pipe_a[0], "topic-A", true);
    TEST_ASSERT_EQUAL_UINT64 (
      1, zlink::session_termination_test_access_t::
           xsub_dispatch_state_creations (xsub));
    dispatch_socket_msg_part (xsub, pipe_b[0], "topic-B", false, true);
    TEST_ASSERT_EQUAL_UINT64 (
      1, zlink::session_termination_test_access_t::
           xsub_dispatch_state_creations (xsub));
    dispatch_socket_msg_part (xsub, pipe_a[0], "payload-A", false);

    TEST_ASSERT_FALSE (probe.metadata_leaked);
    TEST_ASSERT_EQUAL_UINT64 (2, probe.records.size ());
    TEST_ASSERT_EQUAL_UINT64 (1, probe.records[0].size ());
    TEST_ASSERT_EQUAL_STRING ("topic-B", probe.records[0][0].c_str ());
    TEST_ASSERT_EQUAL_UINT64 (2, probe.records[1].size ());
    TEST_ASSERT_EQUAL_STRING ("topic-A", probe.records[1][0].c_str ());
    TEST_ASSERT_EQUAL_STRING ("payload-A", probe.records[1][1].c_str ());

    zlink::session_termination_test_access_t::clear_socket_msg_handler (xsub);
    pipe_a[0]->terminate (false);
    pipe_a[1]->terminate (false);
    pipe_b[0]->terminate (false);
    pipe_b[1]->terminate (false);
    int events = 0;
    size_t events_size = sizeof (events);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_option (xsub_handle, ZLINK_OPT_EVENTS, &events,
                        &events_size));

    xsub_pin = socket_handle_t ();
    close_sync_socket (xsub_handle);
    zlink::ctx_t *ctx =
      static_cast<zlink::ctx_t *> (get_test_context ());
    TEST_ASSERT_SUCCESS_ERRNO (
      ctx->wait_for_socket_count_at_most (0, 5000));
}

int main ()
{
    setup_test_environment ();

    UNITY_BEGIN ();
    RUN_TEST (test_pubsub_filter_tcp);
    RUN_TEST (test_pubsub_xpub_xsub_inproc);
    RUN_TEST (
      test_xsub_multipart_pipe_termination_does_not_join_next_peer_record);
    RUN_TEST (
      test_xsub_socket_dispatch_keeps_interleaved_publishers_separate);
    return UNITY_END ();
}
