/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include "api/socket/socket_api_internal.hpp"
#include "core/ctx.hpp"
#include "core/msg.hpp"
#include "core/pipe.hpp"
#include "sockets/common/socket_base.hpp"
#include "sockets/pubsub/xsub.hpp"

#include <unity.h>
#include <cstring>

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
    void pipe_peer_terminated (zlink::pipe_t *, bool) ZLINK_OVERRIDE {}
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

int main ()
{
    setup_test_environment ();
    UNITY_BEGIN ();
    RUN_TEST (test_xsub_multipart_pipe_termination_does_not_join_next_peer_record);
    return UNITY_END ();
}
