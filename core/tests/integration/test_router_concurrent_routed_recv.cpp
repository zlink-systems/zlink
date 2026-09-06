/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_monitoring.hpp"
#include "testutil_unity.hpp"


#include <atomic>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

SETUP_TEARDOWN_TESTCONTEXT
namespace
{
struct start_gate_t
{
    start_gate_t () : ready (0), go (false) {}

    std::mutex sync;
    std::condition_variable cv;
    int ready;
    bool go;
};

struct worker_result_t
{
    worker_result_t () : failed (false), errno_value (0) {}

    std::atomic<bool> failed;
    std::atomic<int> errno_value;
    std::string detail;
};

struct ready_monitor_state_t
{
    ready_monitor_state_t () : ready_count (0), error_code (0) {}

    std::mutex sync;
    std::condition_variable cv;
    int ready_count;
    int error_code;
};

struct ready_monitor_t
{
    ready_monitor_t () : monitor (NULL), state (NULL) {}

    void *monitor;
    ready_monitor_state_t *state;
    test_monitor_pull_dispatch_t dispatch;
};
void set_socket_timeouts (void *socket_)
{
    const int timeout_ms = 10000;
    const int linger_ms = 0;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (socket_, ZLINK_OPT_SNDTIMEO, &timeout_ms, sizeof (timeout_ms)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (socket_, ZLINK_OPT_RCVTIMEO, &timeout_ms, sizeof (timeout_ms)));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (socket_, ZLINK_OPT_LINGER, &linger_ms, sizeof (linger_ms)));
}

void monitor_ready_handler (const zlink_monitor_event_t *event_, void *userdata_)
{
    ready_monitor_state_t *state = static_cast<ready_monitor_state_t *> (userdata_);
    if (!state || !event_)
        return;

    {
        std::lock_guard<std::mutex> lock (state->sync);
        switch (event_->event) {
            case ZLINK_EVENT_CONNECTION_READY:
                state->ready_count = static_cast<int> (event_->value);
                break;
            case ZLINK_EVENT_BIND_FAILED:
            case ZLINK_EVENT_ACCEPT_FAILED:
            case ZLINK_EVENT_CLOSE_FAILED:
            case ZLINK_EVENT_HANDSHAKE_FAILED_NO_DETAIL:
            case ZLINK_EVENT_HANDSHAKE_FAILED_PROTOCOL:
            case ZLINK_EVENT_HANDSHAKE_FAILED_AUTH:
                if (state->error_code == 0)
                    state->error_code = event_->value > 0 ? static_cast<int> (event_->value) : EIO;
                break;
            default:
                break;
        }
    }

    state->cv.notify_all ();
}

bool open_ready_monitor (void *socket_, ready_monitor_t *out_)
{
    if (!socket_ || !out_) {
        errno = EINVAL;
        return false;
    }

    ready_monitor_state_t *state = new (std::nothrow) ready_monitor_state_t;
    if (!state) {
        errno = ENOMEM;
        return false;
    }

    zlink_socket_monitor_open_options_t opts;
    memset (&opts, 0, sizeof (opts));
    opts.events = ZLINK_EVENT_CONNECTION_READY | ZLINK_EVENT_BIND_FAILED | ZLINK_EVENT_ACCEPT_FAILED
                  | ZLINK_EVENT_CLOSE_FAILED | ZLINK_EVENT_HANDSHAKE_FAILED_NO_DETAIL
                  | ZLINK_EVENT_HANDSHAKE_FAILED_PROTOCOL | ZLINK_EVENT_HANDSHAKE_FAILED_AUTH;
    void *monitor = zlink_socket_monitor_open (socket_, &opts);
    if (!monitor) {
        delete state;
        return false;
    }

    if (!out_->dispatch.start (monitor, &monitor_ready_handler, state)) {
        (void) zlink_monitor_close (&monitor);
        delete state;
        return false;
    }

    const int zero = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (monitor, ZLINK_OPT_LINGER, &zero, sizeof (zero)));

    out_->monitor = monitor;
    out_->state = state;
    return true;
}

bool wait_ready_count (ready_monitor_t *monitor_, int expected_count_, int timeout_ms_)
{
    if (!monitor_ || !monitor_->state)
        return false;

    std::unique_lock<std::mutex> lock (monitor_->state->sync);
    return monitor_->state->cv.wait_for (lock, std::chrono::milliseconds (timeout_ms_),
                                         [monitor_, expected_count_] {
                                             return monitor_->state->ready_count >= expected_count_
                                                    || monitor_->state->error_code != 0;
                                         })
           && monitor_->state->error_code == 0 && monitor_->state->ready_count >= expected_count_;
}

bool wait_ready_count_equal (ready_monitor_t *monitor_,
                             int expected_count_,
                             int timeout_ms_)
{
    if (!monitor_ || !monitor_->state)
        return false;

    std::unique_lock<std::mutex> lock (monitor_->state->sync);
    return monitor_->state->cv.wait_for (
             lock, std::chrono::milliseconds (timeout_ms_),
             [monitor_, expected_count_] {
                 return monitor_->state->ready_count == expected_count_
                        || monitor_->state->error_code != 0;
             })
           && monitor_->state->error_code == 0
           && monitor_->state->ready_count == expected_count_;
}

void close_ready_monitor (ready_monitor_t *monitor_)
{
    if (!monitor_)
        return;
    monitor_->dispatch.stop ();
    if (monitor_->monitor)
        TEST_ASSERT_SUCCESS_ERRNO (zlink_monitor_close (&monitor_->monitor));
    delete monitor_->state;
    monitor_->state = NULL;
}

void mark_failure (worker_result_t *result_, const char *detail_)
{
    if (!result_)
        return;

    result_->failed.store (true);
    result_->errno_value.store (errno);
    if (detail_ && result_->detail.empty ())
        result_->detail = detail_;
}

bool wait_for_start_gate (start_gate_t *gate_, int timeout_ms_)
{
    std::unique_lock<std::mutex> lock (gate_->sync);
    gate_->ready++;
    gate_->cv.notify_all ();
    return gate_->cv.wait_for (lock, std::chrono::milliseconds (timeout_ms_),
                               [gate_] { return gate_->go; });
}

void open_single_part_message (zlink_msg_t *msg_, const char *payload_)
{
    const size_t size = std::strlen (payload_);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (msg_, size));
    memcpy (zlink_msg_data (msg_), payload_, size);
}

bool send_single_part (void *socket_, const zlink_routing_id_t *target_rid_, const char *payload_)
{
    zlink_msg_t msg;
    open_single_part_message (&msg, payload_);

    const int rc = target_rid_ ? zlink_send_rid (socket_, target_rid_, &msg, 1, 0)
                               : zlink_send (socket_, &msg, 1, 0);
    if (rc == 0)
        return true;

    zlink_msg_close (&msg);
    return false;
}

bool recv_single_part (void *socket_,
                       zlink_routing_id_t *source_rid_out_,
                       std::string *payload_out_)
{
    zlink_msg_t *parts = NULL;
    size_t part_count = 0;
    if (source_rid_out_) {
        const zlink_routing_id_t *peer_rid = NULL;
        uint64_t request_seq = 0;
        if (zlink_router_recv (socket_, &peer_rid, &request_seq, &parts,
                               &part_count, 0)
            != 0) {
            return false;
        }
        if (request_seq != 0) {
            if (parts)
                zlink_multipart_close (parts, part_count);
            errno = EPROTO;
            return false;
        }
        memset (source_rid_out_, 0, sizeof (*source_rid_out_));
        if (peer_rid)
            *source_rid_out_ = *peer_rid;
    } else if (zlink_recv (socket_, NULL, &parts, &part_count, 0) != 0) {
        return false;
    }

    if (part_count != 1) {
        zlink_multipart_close (parts, part_count);
        errno = EPROTO;
        return false;
    }

    if (payload_out_) {
        payload_out_->assign (static_cast<const char *> (zlink_msg_data (&parts[0])),
                              zlink_msg_size (&parts[0]));
    }

    zlink_multipart_close (parts, part_count);
    return true;
}

void router_recv_server_run (void *server_, int expected_messages_, worker_result_t *result_)
{
    for (int i = 0; i < expected_messages_; ++i) {
        zlink_routing_id_t source_rid;
        std::string payload;
        if (!recv_single_part (server_, &source_rid, &payload)) {
            mark_failure (result_, "server recv failed");
            return;
        }
        if (source_rid.size == 0 || payload.empty ()) {
            errno = EPROTO;
            mark_failure (result_, "server received invalid routed frame");
            return;
        }
    }
}

void router_client_run (void *client_,
                        const zlink_routing_id_t *server_rid_,
                        start_gate_t *gate_,
                        int iterations_,
                        worker_result_t *result_)
{
    if (!wait_for_start_gate (gate_, 5000)) {
        errno = ETIMEDOUT;
        mark_failure (result_, "client start gate timed out");
        return;
    }

    for (int i = 0; i < iterations_; ++i) {
        char payload[64];
        snprintf (payload, sizeof (payload), "payload-%04d", i);
        if (!send_single_part (client_, server_rid_, payload)) {
            mark_failure (result_, "client send_rid failed");
            return;
        }
    }
}

void router_send_during_peer_close_run (void *router_,
                                        const zlink_routing_id_t *peer_rid_,
                                        start_gate_t *gate_,
                                        worker_result_t *result_)
{
    if (!wait_for_start_gate (gate_, 5000)) {
        errno = ETIMEDOUT;
        mark_failure (result_, "router close-race sender start gate timed out");
        return;
    }

    for (int i = 0; i < 200; ++i) {
        char payload[64];
        snprintf (payload, sizeof (payload), "close-race-%04d", i);
        zlink_msg_t msg;
        open_single_part_message (&msg, payload);
        const int rc = zlink_send_rid (router_, peer_rid_, &msg, 1, ZLINK_DONTWAIT);
        if (rc == 0)
            continue;

        const int send_errno = errno;
        zlink_msg_close (&msg);
        if (send_errno == EAGAIN || send_errno == EHOSTUNREACH || send_errno == ENOTCONN
            || send_errno == ECONNREFUSED) {
            errno = 0;
            continue;
        }

        errno = send_errno;
        mark_failure (result_, "router send_rid during peer close failed unexpectedly");
        return;
    }
}
} // namespace
void test_router_router_concurrent_echo_does_not_corrupt_recv_queue ()
{
    // Keep this as a fast default-lane regression: enough concurrent
    // send/recv overlap to exercise the routed recv queue without turning the
    // test into a long stress workload.
    const int client_count = 2;
    const int iterations = 10;

    void *server = zlink_socket (get_test_context (), ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_NOT_NULL (server);
    set_socket_timeouts (server);

    ready_monitor_t server_monitor;
    TEST_ASSERT_TRUE (open_ready_monitor (server, &server_monitor));

    const char server_name[] = "SERVER";
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_routing_id (server, server_name, sizeof (server_name) - 1));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (server, "tcp://127.0.0.1:*"));

    char endpoint[MAX_SOCKET_STRING];
    size_t endpoint_size = sizeof (endpoint);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_get_option (server, ZLINK_OPT_LAST_ENDPOINT, endpoint, &endpoint_size));

    zlink_routing_id_t server_rid;
    memset (&server_rid, 0, sizeof (server_rid));
    server_rid.size = sizeof (server_name) - 1;
    memcpy (server_rid.data, server_name, server_rid.size);

    std::vector<void *> clients;
    std::vector<ready_monitor_t> client_monitors (static_cast<size_t> (client_count));
    clients.reserve (client_count);
    for (int i = 0; i < client_count; ++i) {
        void *client = zlink_socket (get_test_context (), ZLINK_SOCKET_ROUTER);
        TEST_ASSERT_NOT_NULL (client);
        set_socket_timeouts (client);
        TEST_ASSERT_TRUE (open_ready_monitor (client, &client_monitors[static_cast<size_t> (i)]));

        char client_name[32];
        snprintf (client_name, sizeof (client_name), "router-%02d", i);
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_set_routing_id (client, client_name, std::strlen (client_name)));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_set_router_option (
          client, ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID, server_name, sizeof (server_name) - 1));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, endpoint));
        clients.push_back (client);
    }

    TEST_ASSERT_TRUE (wait_ready_count (&server_monitor, client_count, 5000));
    for (int i = 0; i < client_count; ++i) {
        TEST_ASSERT_TRUE (wait_ready_count (&client_monitors[static_cast<size_t> (i)], 1, 5000));
    }

    worker_result_t server_result;
    std::thread server_thread (router_recv_server_run, server, client_count * iterations,
                               &server_result);

    start_gate_t gate;
    std::vector<worker_result_t> client_results (static_cast<size_t> (client_count));
    std::vector<std::thread> client_threads;
    client_threads.reserve (client_count);
    for (int i = 0; i < client_count; ++i) {
        client_threads.push_back (std::thread (router_client_run, clients[static_cast<size_t> (i)],
                                               &server_rid, &gate, iterations,
                                               &client_results[static_cast<size_t> (i)]));
    }

    {
        std::unique_lock<std::mutex> lock (gate.sync);
        const bool all_ready =
          gate.cv.wait_for (lock, std::chrono::milliseconds (5000),
                            [&gate, client_count] { return gate.ready == client_count; });
        TEST_ASSERT_TRUE_MESSAGE (all_ready, "router client start gate did not become ready");
        gate.go = true;
        gate.cv.notify_all ();
    }

    for (size_t i = 0; i < client_threads.size (); ++i)
        client_threads[i].join ();
    server_thread.join ();

    for (size_t i = 0; i < client_results.size (); ++i) {
        TEST_ASSERT_FALSE_MESSAGE (client_results[i].failed.load (),
                                   client_results[i].detail.c_str ());
        TEST_ASSERT_EQUAL_INT_MESSAGE (0, client_results[i].errno_value.load (),
                                       client_results[i].detail.c_str ());
    }
    TEST_ASSERT_FALSE_MESSAGE (server_result.failed.load (), server_result.detail.c_str ());
    TEST_ASSERT_EQUAL_INT_MESSAGE (0, server_result.errno_value.load (),
                                   server_result.detail.c_str ());

    for (size_t i = 0; i < client_monitors.size (); ++i)
        close_ready_monitor (&client_monitors[i]);
    close_ready_monitor (&server_monitor);
    for (size_t i = 0; i < clients.size (); ++i)
        close_zero_linger (clients[i]);
    close_zero_linger (server);
}

void test_router_routed_send_during_peer_close_does_not_touch_destroyed_pipe ()
{
    void *router = zlink_socket (get_test_context (), ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_NOT_NULL (router);
    set_socket_timeouts (router);

    const int zero = 0;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (router, ZLINK_OPT_SNDTIMEO, &zero, sizeof (zero)));
    const int mandatory = 1;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_router_option (router, ZLINK_ROUTER_OPT_MANDATORY, &mandatory, sizeof (mandatory)));

    ready_monitor_t router_monitor;
    TEST_ASSERT_TRUE (open_ready_monitor (router, &router_monitor));

    char endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (router, endpoint, sizeof (endpoint));

    for (int round = 0; round < 50; ++round) {
        void *dealer = zlink_socket (get_test_context (), ZLINK_SOCKET_DEALER);
        TEST_ASSERT_NOT_NULL (dealer);
        set_socket_timeouts (dealer);
        TEST_ASSERT_SUCCESS_ERRNO (zlink_set_option (dealer, ZLINK_OPT_LINGER, &zero, sizeof (zero)));

        char dealer_name[32];
        snprintf (dealer_name, sizeof (dealer_name), "close-race-%02d", round);
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_set_routing_id (dealer, dealer_name, std::strlen (dealer_name)));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (dealer, endpoint));

        TEST_ASSERT_TRUE (wait_ready_count (&router_monitor, 1, 5000));
        TEST_ASSERT_TRUE (send_single_part (dealer, NULL, "ready"));

        zlink_routing_id_t peer_rid;
        std::string payload;
        TEST_ASSERT_TRUE (recv_single_part (router, &peer_rid, &payload));
        TEST_ASSERT_EQUAL_STRING ("ready", payload.c_str ());

        start_gate_t gate;
        worker_result_t sender_result;
        std::thread sender (router_send_during_peer_close_run, router, &peer_rid, &gate,
                            &sender_result);

        {
            std::unique_lock<std::mutex> lock (gate.sync);
            const bool sender_ready =
              gate.cv.wait_for (lock, std::chrono::milliseconds (5000),
                                [&gate] { return gate.ready == 1; });
            TEST_ASSERT_TRUE_MESSAGE (sender_ready, "router close-race sender did not become ready");
            gate.go = true;
            gate.cv.notify_all ();
        }

        std::this_thread::sleep_for (std::chrono::milliseconds (1));
        close_zero_linger (dealer);
        sender.join ();

        TEST_ASSERT_FALSE_MESSAGE (sender_result.failed.load (), sender_result.detail.c_str ());
        TEST_ASSERT_EQUAL_INT_MESSAGE (0, sender_result.errno_value.load (),
                                       sender_result.detail.c_str ());
        TEST_ASSERT_TRUE (wait_ready_count_equal (&router_monitor, 0, 5000));
    }

    close_ready_monitor (&router_monitor);
    close_zero_linger (router);
}
int main ()
{
    setup_test_environment ();
    UNITY_BEGIN ();
    RUN_TEST (test_router_router_concurrent_echo_does_not_corrupt_recv_queue);
    RUN_TEST (test_router_routed_send_during_peer_close_does_not_touch_destroyed_pipe);
    return UNITY_END ();
}
