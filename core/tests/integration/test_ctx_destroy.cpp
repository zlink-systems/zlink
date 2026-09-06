/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"


#include <unity.h>

#include <string.h>
#include <stdlib.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include <atomic>
bool should_run_ctx_destroy_test (const char *name_)
{
    const char *const selected = getenv ("ZLINK_TEST_CASE");
    return !selected || !*selected || strcmp (selected, name_) == 0;
}
void setUp ()
{
}
void tearDown ()
{
}
static void receiver (void *socket_)
{
    char buffer[16];
    int rc = zlink_recv (socket_, &buffer, sizeof (buffer), 0);
    TEST_ASSERT_EQUAL_INT (-1, rc);
    TEST_ASSERT_EQUAL_INT (ETERM, errno);
}
struct concurrent_shutdown_start_gate_t
{
    concurrent_shutdown_start_gate_t () : ready (0), go (false) {}

    std::mutex sync;
    std::condition_variable changed;
    int ready;
    bool go;
};

void test_ctx_shutdown_releases_concurrent_receive_and_send_after_backpressure ()
{
    const size_t message_size = 64;
    const uint64_t hwm =
      4u * (static_cast<uint64_t> (message_size) + sizeof (zlink_msg_t));
    const int infinite_timeout = -1;
    const int zero_linger = 0;
    const char *const endpoint =
      "inproc://ctx-shutdown-concurrent-blockers";

    void *ctx = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx);
    void *socket = zlink_socket (ctx, ZLINK_SOCKET_PAIR);
    void *peer = zlink_socket (ctx, ZLINK_SOCKET_PAIR);
    TEST_ASSERT_NOT_NULL (socket);
    TEST_ASSERT_NOT_NULL (peer);

    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (socket, ZLINK_OPT_SNDHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (socket, ZLINK_OPT_RCVHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (peer, ZLINK_OPT_SNDHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (peer, ZLINK_OPT_RCVHWM, &hwm, sizeof (hwm)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (socket, ZLINK_OPT_SNDTIMEO, &infinite_timeout,
                        sizeof (infinite_timeout)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (socket, ZLINK_OPT_RCVTIMEO, &infinite_timeout,
                        sizeof (infinite_timeout)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (socket, ZLINK_OPT_LINGER, &zero_linger,
                        sizeof (zero_linger)));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK,
      zlink_set_option (peer, ZLINK_OPT_LINGER, &zero_linger,
                        sizeof (zero_linger)));
    TEST_ASSERT_EQUAL_INT (ZLINK_BIND_OK, zlink_bind (peer, endpoint));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONNECT_OK, zlink_connect (socket, endpoint));

    char payload[message_size];
    char received_probe[message_size];
    memset (payload, 'q', sizeof (payload));
    memset (received_probe, 0, sizeof (received_probe));

    // A blocking transfer establishes the inproc pipe before the HWM fill.
    TEST_ASSERT_EQUAL_INT (
      static_cast<int> (message_size),
      zlink_send (socket, payload, sizeof (payload), 0));
    TEST_ASSERT_EQUAL_INT (
      static_cast<int> (message_size),
      zlink_recv (peer, received_probe, sizeof (received_probe), 0));

    int queued = 0;
    while (queued < 512
           && zlink_send (socket, payload, sizeof (payload), ZLINK_DONTWAIT)
                == static_cast<int> (message_size))
        ++queued;
    TEST_ASSERT_GREATER_THAN_INT (0, queued);
    TEST_ASSERT_LESS_THAN_INT (512, queued);
    TEST_ASSERT_EQUAL_INT (EAGAIN, errno);

    zlink_msg_t recv_part;
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_init (&recv_part));
    zlink_msg_t send_part;
    TEST_ASSERT_EQUAL_INT (
      ZLINK_CONFIG_OK, zlink_msg_init_size (&send_part, message_size));
    memset (zlink_msg_data (&send_part), 's', message_size);
    zlink_part_flag_t recv_part_flag = ZLINK_PART_MORE;
    zlink_recv_result_t recv_result = ZLINK_RECV_INTERNAL_ERROR;
    int recv_errno = 0;
    concurrent_shutdown_start_gate_t start_gate;
    const auto wait_for_start = [&start_gate] {
        std::unique_lock<std::mutex> lock (start_gate.sync);
        ++start_gate.ready;
        start_gate.changed.notify_all ();
        start_gate.changed.wait (lock, [&start_gate] { return start_gate.go; });
    };
    std::thread receiver_thread ([&] {
        wait_for_start ();
        errno = 0;
        recv_result = zlink_recv_part (socket, NULL, &recv_part,
                                       &recv_part_flag,
                                       ZLINK_RECV_FLAGS_NONE);
        recv_errno = zlink_errno ();
    });

    zlink_submit_result_t send_result = ZLINK_SUBMIT_INTERNAL_ERROR;
    int send_errno = 0;
    zlink_completion_id_t completion_id = UINT64_MAX;
    std::thread sender_thread ([&] {
        wait_for_start ();
        errno = 0;
        send_result = zlink_send_part (
          socket, &send_part, ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL,
          NULL, &completion_id);
        send_errno = zlink_errno ();
    });

    bool workers_ready = false;
    {
        // Public C API exposes the established HWM refusal, but not whether a
        // particular blocking call has parked. Release both calls together so
        // shutdown races their entry without relying on a settling sleep.
        std::unique_lock<std::mutex> lock (start_gate.sync);
        workers_ready = start_gate.changed.wait_for (
          lock, std::chrono::seconds (3),
          [&start_gate] { return start_gate.ready == 2; });
        start_gate.go = true;
        start_gate.changed.notify_all ();
    }

    const zlink_close_result_t shutdown_result = zlink_ctx_shutdown (ctx);
    receiver_thread.join ();
    sender_thread.join ();

    TEST_ASSERT_TRUE (workers_ready);
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, shutdown_result);
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_TERMINATED, recv_result);
    TEST_ASSERT_EQUAL_INT (ETERM, recv_errno);
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&recv_part));
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_MORE, recv_part_flag);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&recv_part));
    TEST_ASSERT_EQUAL_INT (ZLINK_SUBMIT_TERMINATED, send_result);
    TEST_ASSERT_EQUAL_INT (ETERM, send_errno);
    TEST_ASSERT_EQUAL_UINT64 (0, completion_id);
    TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&send_part));
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_OK, zlink_msg_close (&send_part));

    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_close (socket));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_close (peer));
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, zlink_ctx_term (ctx));
}

void test_ctx_destroy ()
{
    //  Set up our context and sockets
    void *ctx = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx);

    void *socket = zlink_socket (ctx, ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (socket);

    // Close the socket
    TEST_ASSERT_SUCCESS_ERRNO (zlink_close (socket));

    // Destroy the context
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (ctx));
}

void test_ctx_term_with_open_socket_monitors ()
{
    void *ctx = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx);

    void *router = zlink_socket (ctx, ZLINK_SOCKET_ROUTER);
    void *dealer = zlink_socket (ctx, ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (router);
    TEST_ASSERT_NOT_NULL (dealer);

    zlink_socket_monitor_open_options_t options;
    memset (&options, 0, sizeof (options));
    options.events = ZLINK_EVENT_CONNECTION_READY;
    void *router_monitor = zlink_socket_monitor_open (router, &options);
    void *dealer_monitor = zlink_socket_monitor_open (dealer, &options);
    TEST_ASSERT_NOT_NULL (router_monitor);
    TEST_ASSERT_NOT_NULL (dealer_monitor);

    // The application may close the source sockets before it consumes the raw
    // monitor handles. Context termination must detach the source monitor
    // tasks before it reaps those still-open monitor sockets.
    TEST_ASSERT_SUCCESS_ERRNO (zlink_close (dealer));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_close (router));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_shutdown (ctx));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_monitor_close (&dealer_monitor));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_monitor_close (&router_monitor));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (ctx));
}

void test_router_router_connection_ready ()
{
    void *ctx = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx);

    void *server = zlink_socket (ctx, ZLINK_SOCKET_ROUTER);
    void *client = zlink_socket (ctx, ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_NOT_NULL (server);
    TEST_ASSERT_NOT_NULL (client);

    const int zero = 0;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (server, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (client, ZLINK_OPT_LINGER, &zero, sizeof (zero)));

    const char server_id[] = "SRV01";
    const char client_id[] = "CLT01";
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_routing_id (server, server_id, sizeof (server_id) - 1));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_routing_id (client, client_id, sizeof (client_id) - 1));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_router_option (
      client, ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID, server_id,
      sizeof (server_id) - 1));

    zlink_socket_monitor_open_options_t monitor_options;
    memset (&monitor_options, 0, sizeof (monitor_options));
    monitor_options.events = ZLINK_EVENT_CONNECTION_READY;
    void *monitor = zlink_socket_monitor_open (server, &monitor_options);
    TEST_ASSERT_NOT_NULL (monitor);

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (server, endpoint, sizeof (endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint));

    bool ready = false;
    zlink_monitor_event_t event;
    for (int attempt = 0; attempt < 30 && !ready; ++attempt) {
        zlink_pollitem_t items[] = {
          {monitor, 0, ZLINK_POLLIN, 0}, {server, 0, ZLINK_POLLIN, 0}};
        if (zlink_poll (items, 2, 100, NULL) <= 0
            || (items[0].revents & ZLINK_POLLIN) == 0)
            continue;

        while (zlink_socket_monitor_recv (
                 monitor, &event, ZLINK_RECV_FLAGS_DONTWAIT)
               == ZLINK_RECV_OK) {
            if (event.event == ZLINK_EVENT_CONNECTION_READY) {
                ready = true;
                break;
            }
        }
    }

    TEST_ASSERT_TRUE (ready);
    TEST_ASSERT_EQUAL_UINT64 (sizeof (client_id) - 1, event.routing_id.size);
    TEST_ASSERT_EQUAL_MEMORY (client_id, event.routing_id.data, event.routing_id.size);
    TEST_ASSERT_NOT_EQUAL (0, event.connection_id);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_monitor_close (&monitor));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_disconnect (client, endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_close (client));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_close (server));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (ctx));
}

void test_ctx_shutdown ()
{
    //  Set up our context and sockets
    void *ctx = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx);

    void *socket = zlink_socket (ctx, ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (socket);

    // Spawn a thread to receive on socket
    void *receiver_thread = zlink_thread_start (&receiver, socket);

    // Wait for thread to start up and block
    msleep (SETTLE_TIME);

    // Shutdown context, if we used destroy here we would deadlock.
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_shutdown (ctx));

    // Wait for thread to finish
    zlink_thread_join (receiver_thread);

    // Close the socket.
    TEST_ASSERT_SUCCESS_ERRNO (zlink_close (socket));

    // Destroy the context, will now not hang as we have closed the socket.
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (ctx));
}

void test_ctx_shutdown_socket_opened_after ()
{
    //  Set up our context.
    void *ctx = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx);

    // Open a socket to start context, and close it immediately again.
    void *socket = zlink_socket (ctx, ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (socket);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_close (socket));

    // Shutdown context.
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_shutdown (ctx));

    // Opening socket should now fail.
    TEST_ASSERT_NULL (zlink_socket (ctx, ZLINK_SOCKET_DEALER));
    TEST_ASSERT_FAILURE_ERRNO (ETERM, -1);

    // Destroy the context.
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (ctx));
}

void test_ctx_shutdown_only_socket_opened_after ()
{
    //  Set up our context.
    void *ctx = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx);

    // Shutdown context.
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_shutdown (ctx));

    // Opening socket should now fail.
    TEST_ASSERT_NULL (zlink_socket (ctx, ZLINK_SOCKET_DEALER));
    TEST_ASSERT_FAILURE_ERRNO (ETERM, -1);

    // Destroy the context.
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (ctx));
}

void test_monitor_and_hwm_update_race_peer_close ()
{
    void *ctx_handle = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (ctx_handle);
    void *server = zlink_socket (ctx_handle, ZLINK_SOCKET_PAIR);
    void *client = zlink_socket (ctx_handle, ZLINK_SOCKET_PAIR);
    TEST_ASSERT_NOT_NULL (server);
    TEST_ASSERT_NOT_NULL (client);

    const int zero = 0;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (server, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (client, ZLINK_OPT_LINGER, &zero, sizeof (zero)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (server, "inproc://peer-snapshot-race"));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, "inproc://peer-snapshot-race"));

    const unsigned char byte = 0x2a;
    unsigned char received = 0;
    TEST_ASSERT_EQUAL_INT (1, zlink_send (client, &byte, sizeof (byte), 0));
    TEST_ASSERT_EQUAL_INT (1, zlink_recv (server, &received, sizeof (received), 0));

    zlink_socket_monitor_open_options_t monitor_options;
    memset (&monitor_options, 0, sizeof (monitor_options));
    void *monitor = zlink_socket_monitor_open (server, &monitor_options);
    TEST_ASSERT_NOT_NULL (monitor);

    const int iterations = 2000;
    std::atomic<int> ready (0);
    std::atomic<bool> start (false);
    std::atomic<int> option_failures (0);
    std::atomic<int> monitor_failures (0);
    std::thread update_hwm ([&] {
        ready.fetch_add (1, std::memory_order_release);
        while (!start.load (std::memory_order_acquire))
            std::this_thread::yield ();
        for (int i = 0; i != iterations; ++i) {
            const uint64_t hwm = (i & 1) == 0 ? 4096 : 8192;
            if (zlink_set_option (server, ZLINK_OPT_SNDHWM, &hwm,
                                  sizeof (hwm)) != ZLINK_CONFIG_OK
                || zlink_set_option (server, ZLINK_OPT_RCVHWM, &hwm,
                                     sizeof (hwm)) != ZLINK_CONFIG_OK)
                option_failures.fetch_add (1, std::memory_order_relaxed);
        }
    });
    std::thread read_monitor ([&] {
        ready.fetch_add (1, std::memory_order_release);
        while (!start.load (std::memory_order_acquire))
            std::this_thread::yield ();
        for (int i = 0; i != iterations; ++i) {
            zlink_monitor_status_t status;
            memset (&status, 0, sizeof (status));
            if (zlink_monitor_status (monitor, &status) != ZLINK_CONFIG_OK)
                monitor_failures.fetch_add (1, std::memory_order_relaxed);
        }
    });

    while (ready.load (std::memory_order_acquire) != 2)
        std::this_thread::yield ();
    start.store (true, std::memory_order_release);
    std::this_thread::yield ();
    TEST_ASSERT_SUCCESS_ERRNO (zlink_close (client));
    update_hwm.join ();
    read_monitor.join ();

    TEST_ASSERT_EQUAL_INT (0, option_failures.load (std::memory_order_relaxed));
    TEST_ASSERT_EQUAL_INT (0, monitor_failures.load (std::memory_order_relaxed));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_monitor_close (&monitor));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_close (server));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (ctx_handle));
}

void test_zlink_ctx_term_null_fails ()
{
    int rc = zlink_ctx_term (NULL);
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_INVALID_HANDLE, rc);
    TEST_ASSERT_EQUAL_INT (EFAULT, errno);
}

void test_zlink_term_null_fails ()
{
    int rc = zlink_ctx_term (NULL);
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_INVALID_HANDLE, rc);
    TEST_ASSERT_EQUAL_INT (EFAULT, errno);
}

void test_zlink_ctx_shutdown_null_fails ()
{
    int rc = zlink_ctx_shutdown (NULL);
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_INVALID_HANDLE, rc);
    TEST_ASSERT_EQUAL_INT (EFAULT, errno);
}
int main ()
{
    setup_test_environment ();
    UNITY_BEGIN ();
#define RUN_CTX_DESTROY_TEST(test_) \
    do { if (should_run_ctx_destroy_test (#test_)) RUN_TEST (test_); } while (false)
    RUN_CTX_DESTROY_TEST (test_ctx_destroy);
    RUN_CTX_DESTROY_TEST (test_ctx_term_with_open_socket_monitors);
    RUN_CTX_DESTROY_TEST (test_router_router_connection_ready);
    RUN_CTX_DESTROY_TEST (test_ctx_shutdown);
    RUN_CTX_DESTROY_TEST (
      test_ctx_shutdown_releases_concurrent_receive_and_send_after_backpressure);
    RUN_CTX_DESTROY_TEST (test_ctx_shutdown_socket_opened_after);
    RUN_CTX_DESTROY_TEST (test_ctx_shutdown_only_socket_opened_after);
    RUN_CTX_DESTROY_TEST (test_monitor_and_hwm_update_race_peer_close);
    RUN_CTX_DESTROY_TEST (test_zlink_ctx_term_null_fails);
    RUN_CTX_DESTROY_TEST (test_zlink_term_null_fails);
    RUN_CTX_DESTROY_TEST (test_zlink_ctx_shutdown_null_fails);
#undef RUN_CTX_DESTROY_TEST
    return UNITY_END ();
}
