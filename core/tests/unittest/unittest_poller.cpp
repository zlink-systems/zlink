/* SPDX-License-Identifier: MPL-2.0 */

#include "../testutil.hpp"

#include <poller.hpp>
#include <i_poll_events.hpp>
#include <utils/ip.hpp>

#include <boost/asio.hpp>
#include <unity.h>

void setUp ()
{
}
void tearDown ()
{
}

void test_create ()
{
    zlink::thread_ctx_t thread_ctx;
    zlink::poller_t poller (thread_ctx);
}

struct test_events_t : zlink::i_poll_events
{
    test_events_t (zlink::poller_t &poller_) : _poller (poller_) {}

    void in_event () ZLINK_OVERRIDE
    {
        _poller.rm_socket (_handle);
        _handle = (zlink::poller_t::handle_t) NULL;
        _poller.stop ();

        // this must only be incremented after rm_fd
        in_events.add (1);
    }


    void out_event () ZLINK_OVERRIDE
    {
        TEST_FAIL_MESSAGE ("test poller socket is not registered for write events");
    }


    void timer_event (int id_) ZLINK_OVERRIDE
    {
        LIBZLINK_UNUSED (id_);
        timer_events.add (1);
    }

    void set_handle (zlink::poller_t::handle_t handle_) { _handle = handle_; }

    zlink::atomic_counter_t in_events, timer_events;

  private:
    zlink::poller_t &_poller;
    zlink::poller_t::handle_t _handle;
};

void wait_in_events (test_events_t &events_)
{
    // zlink_stopwatch_intermediate reports microseconds. Keep the bound in
    // the same unit so the intended SETTLE_TIME * 40 timeout is preserved.
    const unsigned long event_timeout_us =
      static_cast<unsigned long> (SETTLE_TIME) * 40UL * 1000UL;
    void *watch = zlink_stopwatch_start ();
    while (events_.in_events.get () < 1) {
        msleep (1);
        TEST_ASSERT_LESS_OR_EQUAL_MESSAGE (event_timeout_us, zlink_stopwatch_intermediate (watch),
                                           "Timeout waiting for in event");
    }
    zlink_stopwatch_stop (watch);
}

void create_connected_tcp_pair (boost::asio::io_context &io_context_,
                                boost::asio::ip::tcp::socket *server_,
                                boost::asio::ip::tcp::socket *client_)
{
    boost::system::error_code ec;
    boost::asio::ip::tcp::acceptor acceptor (io_context_);
    acceptor.open (boost::asio::ip::tcp::v4 (), ec);
    TEST_ASSERT_EQUAL_INT (0, ec.value ());
    acceptor.set_option (boost::asio::ip::tcp::acceptor::reuse_address (true), ec);
    TEST_ASSERT_EQUAL_INT (0, ec.value ());
    acceptor.bind (boost::asio::ip::tcp::endpoint (boost::asio::ip::address_v4::loopback (), 0),
                   ec);
    TEST_ASSERT_EQUAL_INT (0, ec.value ());
    acceptor.listen (boost::asio::socket_base::max_listen_connections, ec);
    TEST_ASSERT_EQUAL_INT (0, ec.value ());

    const boost::asio::ip::tcp::endpoint endpoint = acceptor.local_endpoint (ec);
    TEST_ASSERT_EQUAL_INT (0, ec.value ());

    client_->connect (endpoint, ec);
    TEST_ASSERT_EQUAL_INT (0, ec.value ());
    acceptor.accept (*server_, ec);
    TEST_ASSERT_EQUAL_INT (0, ec.value ());
}

void send_tcp_signal (boost::asio::ip::tcp::socket *socket_)
{
    static const char msg[] = "test";
    boost::system::error_code ec;
    const std::size_t bytes =
      boost::asio::write (*socket_, boost::asio::buffer (msg, sizeof (msg)), ec);
    TEST_ASSERT_EQUAL_INT (0, ec.value ());
    TEST_ASSERT_EQUAL (sizeof (msg), bytes);
}

void test_add_fd_and_start_and_receive_data ()
{
    zlink::thread_ctx_t thread_ctx;
    zlink::poller_t poller (thread_ctx);

    boost::asio::io_context &io_context = poller.get_io_context ();
    boost::asio::ip::tcp::socket server (io_context);
    boost::asio::ip::tcp::socket client (io_context);
    create_connected_tcp_pair (io_context, &server, &client);

    test_events_t events (poller);

    zlink::poller_t::handle_t handle = poller.add_tcp_socket (&server, &events);
    events.set_handle (handle);
    poller.set_pollin (handle);
    poller.start ();

    send_tcp_signal (&client);

    wait_in_events (events);
}

int main (void)
{
    UNITY_BEGIN ();

    zlink::initialize_network ();
    setup_test_environment ();

    RUN_TEST (test_create);
    RUN_TEST (test_add_fd_and_start_and_receive_data);

    zlink::shutdown_network ();

    return UNITY_END ();
}
