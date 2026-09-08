/* SPDX-License-Identifier: MPL-2.0 */

//  D-B218 regression: on one STREAM socket a pull thread receives packets
//  while a different thread submits the echoes. Command application
//  (activate_read, attach, pipe termination) and the public receive path
//  mutate the same fair-queue state, so they must not run concurrently.
//  When they did, the fair queue lost its active partition and the last
//  frames never came back out of recv_packet even though the transport had
//  already delivered them.

#include "testutil.hpp"
#include "testutil_unity.hpp"

#include <boost/asio.hpp>

#include <atomic>
#include <memory>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

SETUP_TEARDOWN_TESTCONTEXT

namespace
{
namespace net = boost::asio;
typedef net::ip::tcp tcp;

//  Failure bound only: the test never waits for this to expire on success.
const int stall_timeout_ms = 30000;
const size_t header_size = 11;

struct echo_item_t
{
    zlink_routing_id_t rid;
    std::string wire;
};

struct harness_t
{
    harness_t () :
        stop (false), received (0), submitted (0), echoed (0),
        client_failures (0), submit_failures (0)
    {
    }

    std::mutex mutex;
    std::condition_variable condition;
    std::deque<echo_item_t> items;
    std::atomic<bool> stop;
    std::atomic<uint64_t> received;
    std::atomic<uint64_t> submitted;
    std::atomic<uint64_t> echoed;
    std::atomic<uint64_t> client_failures;
    std::atomic<uint64_t> submit_failures;
};

std::string packet (unsigned client_, unsigned sequence_, size_t body_size_)
{
    std::string bytes (6, '\0');
    bytes[0] = static_cast<char> (header_size >> 8);
    bytes[1] = static_cast<char> (header_size);
    for (int i = 0; i != 4; ++i)
        bytes[2 + i] = static_cast<char> (body_size_ >> (24 - 8 * i));
    for (size_t i = 0; i != header_size; ++i)
        bytes += static_cast<char> ((client_ * 17 + sequence_ * 3 + i) & 0xff);
    bytes.append (body_size_, static_cast<char> ((client_ + sequence_) & 0xff));
    return bytes;
}

//  Runs one composed asynchronous transfer to completion, bounded by the
//  stall timeout and by the harness stop flag. The io_context, the socket,
//  the operation, its cancellation and its destruction all live on this one
//  thread: that is the concurrency model Asio actually guarantees, and it is
//  why no other thread ever touches this socket. `close()` is explicitly not
//  thread-safe, so the failure path cancels here instead of being closed from
//  the outside.
template <typename Start>
bool run_bounded (harness_t *harness_, net::io_context &io_,
                  tcp::socket &socket_, const Start &start_)
{
    boost::system::error_code result = net::error::would_block;
    start_ ([&result](const boost::system::error_code &ec_, size_t) {
        result = ec_;
    });
    const std::chrono::steady_clock::time_point deadline =
      std::chrono::steady_clock::now ()
      + std::chrono::milliseconds (stall_timeout_ms);
    while (result == net::error::would_block) {
        //  Returns as soon as the transfer completes; the bound only exists
        //  so a stalled run reports instead of hanging.
        io_.restart ();
        io_.run_for (std::chrono::milliseconds (50));
        if (result != net::error::would_block)
            break;
        if (harness_->stop.load ()
            || std::chrono::steady_clock::now () >= deadline) {
            boost::system::error_code ignored;
            socket_.cancel (ignored);
            io_.restart ();
            io_.run ();
            return false;
        }
    }
    return !result;
}

bool write_all (harness_t *harness_, net::io_context &io_,
                tcp::socket &socket_, const char *data_, size_t size_)
{
    return run_bounded (harness_, io_, socket_,
                        [&](std::function<void (const boost::system::error_code &,
                                                size_t)> handler_) {
                            net::async_write (socket_,
                                              net::buffer (data_, size_),
                                              handler_);
                        });
}

bool read_all (harness_t *harness_, net::io_context &io_,
               tcp::socket &socket_, char *data_, size_t size_)
{
    return run_bounded (harness_, io_, socket_,
                        [&](std::function<void (const boost::system::error_code &,
                                                size_t)> handler_) {
                            net::async_read (socket_,
                                             net::buffer (data_, size_),
                                             handler_);
                        });
}

//  A raw client is enough: STREAM carries opaque bytes and the server side
//  only has to hand every frame back.
void client_thread (harness_t *harness_, unsigned id_, unsigned short port_,
                    unsigned frames_, size_t body_size_)
{
    //  The io_context is declared before the socket and destroyed after it,
    //  so the service that owns the socket implementation always outlives the
    //  socket. Neither object ever escapes this thread.
    net::io_context io;
    tcp::socket socket (io);
    boost::system::error_code ec;
    socket.connect (
      tcp::endpoint (net::ip::make_address ("127.0.0.1"), port_), ec);
    if (ec) {
        ++harness_->client_failures;
        return;
    }
    socket.set_option (tcp::no_delay (true), ec);
    for (unsigned sequence = 0;
         sequence != frames_ && !harness_->stop.load (); ++sequence) {
        const std::string expected = packet (id_, sequence, body_size_);
        std::string echo (expected.size (), '\0');
        if (!write_all (harness_, io, socket, expected.data (),
                        expected.size ())
            || !read_all (harness_, io, socket, &echo[0], echo.size ())
            || echo != expected) {
            ++harness_->client_failures;
            break;
        }
        ++harness_->echoed;
    }
    socket.close (ec);
}

//  Thread A: poller readiness plus a non-blocking packet pull.
void pull_thread (harness_t *harness_, void *server_, uint64_t expected_)
{
    void *poller = zlink_poller_new ();
    if (!poller
        || zlink_poller_add (poller, server_, server_, ZLINK_POLLIN)
             != ZLINK_CONFIG_OK) {
        harness_->stop.store (true);
        return;
    }
    while (!harness_->stop.load () && harness_->received.load () < expected_) {
        zlink_poller_event_t event;
        memset (&event, 0, sizeof (event));
        const int poll_rc = zlink_poller_wait (poller, &event, 1, 50, NULL);
        if (poll_rc < 0)
            break;
        if (poll_rc == 0 || !(event.events & ZLINK_POLLIN))
            continue;
        for (;;) {
            zlink_msg_t header;
            zlink_msg_t body;
            zlink_msg_init (&header);
            zlink_msg_init (&body);
            const zlink_routing_id_t *rid = NULL;
            const zlink_recv_result_t rc = zlink_stream_recv_packet (
              server_, &rid, &header, &body, ZLINK_RECV_FLAGS_DONTWAIT);
            if (rc != ZLINK_RECV_OK) {
                zlink_msg_close (&header);
                zlink_msg_close (&body);
                break;
            }
            echo_item_t item;
            item.rid = *rid;
            const size_t got_header = zlink_msg_size (&header);
            const size_t got_body = zlink_msg_size (&body);
            item.wire.assign (6, '\0');
            item.wire[0] = static_cast<char> (got_header >> 8);
            item.wire[1] = static_cast<char> (got_header);
            for (int i = 0; i != 4; ++i)
                item.wire[2 + i] =
                  static_cast<char> (got_body >> (24 - 8 * i));
            item.wire.append (
              static_cast<const char *> (zlink_msg_data (&header)),
              got_header);
            item.wire.append (
              static_cast<const char *> (zlink_msg_data (&body)), got_body);
            zlink_msg_close (&header);
            zlink_msg_close (&body);
            {
                std::lock_guard<std::mutex> lock (harness_->mutex);
                harness_->items.push_back (item);
            }
            ++harness_->received;
            harness_->condition.notify_one ();
        }
    }
    zlink_poller_destroy (&poller);
}

//  Thread B: a separate dispatcher submits the echo on the same socket.
void submit_thread (harness_t *harness_, void *server_, uint64_t expected_)
{
    while (!harness_->stop.load () && harness_->submitted.load () < expected_) {
        echo_item_t item;
        {
            std::unique_lock<std::mutex> lock (harness_->mutex);
            harness_->condition.wait_for (
              lock, std::chrono::milliseconds (50), [harness_] {
                  return harness_->stop.load () || !harness_->items.empty ();
              });
            if (harness_->items.empty ())
                continue;
            item = harness_->items.front ();
            harness_->items.pop_front ();
        }
        zlink_msg_t part;
        zlink_msg_init_size (&part, item.wire.size ());
        memcpy (zlink_msg_data (&part), item.wire.data (), item.wire.size ());
        const zlink_submit_result_t rc =
          zlink_send_part_rid (server_, &item.rid, &part,
                               ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL, NULL,
                               NULL);
        zlink_msg_close (&part);
        if (rc != ZLINK_SUBMIT_OK) {
            ++harness_->submit_failures;
            break;
        }
        ++harness_->submitted;
    }
}

void run_concurrent_pull_send (unsigned clients_, unsigned frames_,
                               uint64_t hwm_, size_t body_size_)
{
    void *server = test_context_socket (ZLINK_SOCKET_STREAM);
    const zlink_stream_recv_mode_t mode = ZLINK_STREAM_RECV_MODE_PACKET;
    const int linger = 0;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_stream_option (server, ZLINK_STREAM_OPT_RECV_MODE, &mode,
                               sizeof (mode)));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_set_option (server, ZLINK_OPT_RCVHWM, &hwm_,
                                             sizeof (hwm_)));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_set_option (server, ZLINK_OPT_SNDHWM, &hwm_,
                                             sizeof (hwm_)));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_set_option (server, ZLINK_OPT_LINGER, &linger,
                                             sizeof (linger)));
    //  Failure bound only: a stalled run must report, not hang, so the echo
    //  submit cannot park forever in a blocking send. A passing run never
    //  reaches this timeout.
    const int sndtimeo = stall_timeout_ms;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_set_option (server, ZLINK_OPT_SNDTIMEO,
                                             &sndtimeo, sizeof (sndtimeo)));
    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK,
                           zlink_bind (server, "tcp://127.0.0.1:*"));
    char endpoint[256];
    size_t endpoint_size = sizeof (endpoint);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK,
                           zlink_get_option (server, ZLINK_OPT_LAST_ENDPOINT,
                                             endpoint, &endpoint_size));
    const unsigned short port = static_cast<unsigned short> (
      strtoul (strrchr (endpoint, ':') + 1, NULL, 10));

    harness_t harness;
    const uint64_t expected = static_cast<uint64_t> (clients_) * frames_;
    std::thread pull (pull_thread, &harness, server, expected);
    std::thread submit (submit_thread, &harness, server, expected);
    std::vector<std::thread> clients;
    clients.reserve (clients_);
    for (unsigned i = 0; i != clients_; ++i)
        clients.push_back (std::thread (client_thread, &harness, i, port,
                                        frames_, body_size_));

    const auto deadline = std::chrono::steady_clock::now ()
                          + std::chrono::milliseconds (stall_timeout_ms);
    while (harness.echoed.load () < expected
           && harness.client_failures.load () == 0
           && harness.submit_failures.load () == 0
           && std::chrono::steady_clock::now () < deadline)
        std::this_thread::yield ();

    const uint64_t echoed = harness.echoed.load ();
    const uint64_t received = harness.received.load ();
    const uint64_t submitted = harness.submitted.load ();
    //  On the stall path a client is blocked in a read for an echo that will
    //  never arrive. It observes this flag on its own thread and cancels its
    //  own socket there; nothing outside that thread touches it.
    harness.stop.store (true);
    harness.condition.notify_all ();
    for (size_t i = 0; i != clients.size (); ++i)
        clients[i].join ();
    pull.join ();
    submit.join ();
    test_context_socket_close_zero_linger (server);

    if (echoed != expected) {
        char message[256];
        snprintf (message, sizeof (message),
                  "packet pull stalled: expected %llu, recv %llu, sent %llu, "
                  "echoed %llu, client failures %llu, submit failures %llu",
                  static_cast<unsigned long long> (expected),
                  static_cast<unsigned long long> (received),
                  static_cast<unsigned long long> (submitted),
                  static_cast<unsigned long long> (echoed),
                  static_cast<unsigned long long> (
                    harness.client_failures.load ()),
                  static_cast<unsigned long long> (
                    harness.submit_failures.load ()));
        TEST_FAIL_MESSAGE (message);
    }
}
}

//  Backpressure on the receive side is what used to strand the fair queue,
//  so the bounded-HWM case is the regression itself.
void test_stream_concurrent_pull_send_bounded_hwm ()
{
    run_concurrent_pull_send (100, 40, 4096, 64);
}

//  Same shape without a receive-side limit, so an unrelated regression in
//  the pump boundary cannot hide behind the HWM case.
void test_stream_concurrent_pull_send_unbounded_hwm ()
{
    run_concurrent_pull_send (40, 40, 0, 64);
}

int main ()
{
    setup_test_environment ();
    UNITY_BEGIN ();
    const char *selected = getenv ("ZLINK_TEST_CASE");
#define RUN_SELECTED(name)                                                     \
    if (!selected || strcmp (selected, #name) == 0)                            \
    RUN_TEST (name)
    RUN_SELECTED (test_stream_concurrent_pull_send_bounded_hwm);
    RUN_SELECTED (test_stream_concurrent_pull_send_unbounded_hwm);
#undef RUN_SELECTED
    return UNITY_END ();
}
