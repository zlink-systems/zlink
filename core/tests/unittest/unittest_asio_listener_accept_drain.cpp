/* SPDX-License-Identifier: MPL-2.0 */

#include "../testutil_unity.hpp"
#include "transports/asio/asio_listener_accept_policy.hpp"
#include <boost/asio.hpp>
#include <unity.h>

void setUp () {}
void tearDown () {}

namespace
{
// Exercise the production helper without a new runtime hook. Holding back
// completions models IOCP cancellation not yet being ready after close.
struct listener_t
{
    zlink::asio_listener_state_t state = zlink::asio_listener_state_t::accepting;
    size_t pending = 0;
    int linger = 0;
    int term_calls = 0;
    int observed_linger = 0;
    bool released = false;

    void release ()
    {
        released = true;
        if (state == zlink::asio_listener_state_t::accepting)
            state = zlink::asio_listener_state_t::endpoint_released;
    }

    void term (int linger_)
    {
        zlink::prepare_asio_listener_termination (
          linger_, &linger, &state, &pending, [this] () { release (); },
          [this] (int value_) { own_term (value_); });
    }

    void own_term (int linger_)
    {
        TEST_ASSERT_EQUAL_UINT (0, pending);
        ++term_calls;
        observed_linger = linger_;
    }

    bool complete (const std::shared_ptr<boost::asio::ip::tcp::socket> &socket_,
                   const boost::system::error_code &ec_)
    {
        return zlink::complete_asio_listener_accept (
          state, &pending, linger, socket_, ec_,
          [this] (int value_) { own_term (value_); });
    }
};

void test_delayed_completion_defers_termination ()
{
    listener_t listener;
    listener.pending = 2;
    listener.term (17);
    TEST_ASSERT_TRUE (listener.released);
    TEST_ASSERT_EQUAL_INT (17, listener.linger);
    TEST_ASSERT_EQUAL_UINT (2, listener.pending);
    TEST_ASSERT_EQUAL_INT_MESSAGE (0, listener.term_calls,
      "own termination entered with 2 accept completions outstanding");
    const std::shared_ptr<boost::asio::ip::tcp::socket> no_socket;
    TEST_ASSERT_TRUE (listener.complete (no_socket, boost::asio::error::operation_aborted));
    TEST_ASSERT_EQUAL_INT (0, listener.term_calls);
    TEST_ASSERT_TRUE (listener.complete (no_socket, boost::asio::error::operation_aborted));
    TEST_ASSERT_EQUAL_INT (1, listener.term_calls);
    TEST_ASSERT_EQUAL_INT (17, listener.observed_linger);
}

void test_zero_pending_terminates_immediately ()
{
    listener_t listener;
    listener.term (-1);
    TEST_ASSERT_EQUAL_INT (1, listener.term_calls);
    TEST_ASSERT_EQUAL_INT (-1, listener.observed_linger);
}

void test_release_before_term_cannot_destroy_listener ()
{
    listener_t listener;
    listener.pending = 1;
    listener.release ();
    const std::shared_ptr<boost::asio::ip::tcp::socket> no_socket;
    TEST_ASSERT_TRUE (listener.complete (no_socket, boost::asio::error::operation_aborted));
    TEST_ASSERT_EQUAL_INT (0, listener.term_calls);
    listener.term (23);
    TEST_ASSERT_EQUAL_INT (1, listener.term_calls);
    TEST_ASSERT_EQUAL_INT (23, listener.observed_linger);
}

void test_queued_success_closes_socket_before_termination ()
{
    boost::asio::io_context io;
    const auto socket = std::make_shared<boost::asio::ip::tcp::socket> (io);
    socket->open (boost::asio::ip::tcp::v4 ());
    listener_t listener;
    listener.pending = 1;
    listener.term (0);
    int term_calls = 0;
    TEST_ASSERT_TRUE (zlink::complete_asio_listener_accept (
      listener.state, &listener.pending, listener.linger, socket,
      boost::system::error_code (), [&] (int) {
          TEST_ASSERT_FALSE (socket->is_open ());
          ++term_calls;
      }));
    TEST_ASSERT_EQUAL_INT (1, term_calls);
}

void test_active_completion_keeps_socket_open ()
{
    boost::asio::io_context io;
    const auto socket = std::make_shared<boost::asio::ip::tcp::socket> (io);
    socket->open (boost::asio::ip::tcp::v4 ());
    listener_t listener;
    listener.pending = 1;
    TEST_ASSERT_FALSE (listener.complete (socket, boost::system::error_code ()));
    TEST_ASSERT_TRUE (socket->is_open ());
    TEST_ASSERT_EQUAL_UINT (0, listener.pending);
    TEST_ASSERT_EQUAL_INT (0, listener.term_calls);
}

void test_termination_does_not_dispatch_unrelated_handlers ()
{
    boost::asio::io_context io;
    bool dispatched = false;
    boost::asio::post (io, [&] () { dispatched = true; });
    listener_t listener;
    listener.pending = 1;
    listener.term (0);
    TEST_ASSERT_FALSE (dispatched);
    const std::shared_ptr<boost::asio::ip::tcp::socket> no_socket;
    listener.complete (no_socket, boost::asio::error::operation_aborted);
    TEST_ASSERT_EQUAL_UINT (1, io.poll ());
    TEST_ASSERT_TRUE (dispatched);
}

void test_real_accept_cancellation_drains_on_event_loop ()
{
    boost::asio::io_context io;
    boost::asio::ip::tcp::acceptor acceptor (
      io, {boost::asio::ip::address_v4::loopback (), 0});
    listener_t listener;
    for (int i = 0; i != 4; ++i) {
        const auto socket = std::make_shared<boost::asio::ip::tcp::socket> (io);
        ++listener.pending;
        acceptor.async_accept (*socket, [&, socket] (const boost::system::error_code &ec_) {
            TEST_ASSERT_TRUE (ec_ == boost::asio::error::operation_aborted);
            TEST_ASSERT_TRUE (listener.complete (socket, ec_));
        });
    }
    zlink::prepare_asio_listener_termination (
      31, &listener.linger, &listener.state, &listener.pending,
      [&] () { listener.release (); acceptor.close (); },
      [&] (int linger_) { listener.own_term (linger_); });
    TEST_ASSERT_EQUAL_INT (0, listener.term_calls);
    TEST_ASSERT_EQUAL_UINT (4, listener.pending);
#ifdef ZLINK_HAVE_WINDOWS
    // IOCP may queue cancellation later; run the existing event loop normally.
    TEST_ASSERT_EQUAL_UINT (4, io.run ());
#else
    // epoll cancellation is ready in this turn; one non-blocking poll suffices.
    TEST_ASSERT_EQUAL_UINT (4, io.poll ());
#endif
    TEST_ASSERT_EQUAL_UINT (0, listener.pending);
    TEST_ASSERT_EQUAL_INT (1, listener.term_calls);
    TEST_ASSERT_EQUAL_INT (31, listener.observed_linger);
}

void test_final_completion_can_delete_listener ()
{
    boost::asio::io_context io;
    const auto socket = std::make_shared<boost::asio::ip::tcp::socket> (io);
    socket->open (boost::asio::ip::tcp::v4 ());
    listener_t *listener = new listener_t;
    listener->pending = 1;
    listener->term (0);
    bool destroyed = false;
    TEST_ASSERT_TRUE (zlink::complete_asio_listener_accept (
      listener->state, &listener->pending, listener->linger, socket,
      boost::system::error_code (), [listener, &destroyed] (int) {
          TEST_ASSERT_EQUAL_UINT (0, listener->pending);
          delete listener;
          destroyed = true;
      }));
    TEST_ASSERT_TRUE (destroyed);
    TEST_ASSERT_FALSE (socket->is_open ());
}
}

int main (int argc, char **argv)
{
    setup_test_environment ();
    UNITY_BEGIN ();
    RUN_TEST (test_delayed_completion_defers_termination);
    RUN_TEST (test_zero_pending_terminates_immediately);
    RUN_TEST (test_release_before_term_cannot_destroy_listener);
    RUN_TEST (test_queued_success_closes_socket_before_termination);
    RUN_TEST (test_active_completion_keeps_socket_open);
    RUN_TEST (test_termination_does_not_dispatch_unrelated_handlers);
    RUN_TEST (test_real_accept_cancellation_drains_on_event_loop);
    RUN_TEST (test_final_completion_can_delete_listener);
    return UNITY_END ();
}
