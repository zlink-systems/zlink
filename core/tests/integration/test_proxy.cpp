/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"
#include "api/socket/request_reply_protocol_internal.hpp"
#include "api/socket/socket_api_internal.hpp"
#include "sockets/proxy/proxy.hpp"

#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>

#define CONTENT_SIZE 13
#define CONTENT_SIZE_MAX 32
#define ROUTING_ID_SIZE 10
#define ROUTING_ID_SIZE_MAX 32
#define QT_WORKERS 5
#define QT_CLIENTS 3
#define is_verbose 0

struct thread_data
{
    int id;
};

struct proxy_thread_data
{
    proxy_thread_data (void *frontend_, void *backend_, void *capture_) :
        frontend (frontend_), backend (backend_), capture (capture_),
        result (ZLINK_CONFIG_OK), error (0), done (false)
    {
    }

    void *frontend;
    void *backend;
    void *capture;
    zlink_config_result_t result;
    int error;
    std::atomic<bool> done;
};

void *g_clients_pkts_out = NULL;
void *g_workers_pkts_out = NULL;
void *control_context = NULL;
bool g_test_context_active = false;
std::atomic<bool> g_server_endpoints_ready (false);
std::atomic<int> g_endpoint_clients_connected (0);
std::atomic<int> g_clients_ready (0);

void setUp ()
{
    setup_test_context ();
    g_test_context_active = true;
    g_server_endpoints_ready.store (false, std::memory_order_release);
    g_endpoint_clients_connected.store (0, std::memory_order_release);
    g_clients_ready.store (0, std::memory_order_release);
    zlink::test_reset_proxy_state ();
}

void tearDown ()
{
    // Unity still invokes tearDown after a TEST_ASSERT longjmp, while C++
    // automatic destructors on the interrupted stack are not guaranteed.
    zlink::test_reset_proxy_state ();
    if (g_test_context_active) {
        teardown_test_context ();
        g_test_context_active = false;
    }
    g_server_endpoints_ready.store (false, std::memory_order_release);
    g_endpoint_clients_connected.store (0, std::memory_order_release);
    g_clients_ready.store (0, std::memory_order_release);
}

static void metadata_proxy_task (void *arg_)
{
    proxy_thread_data *const data = static_cast<proxy_thread_data *> (arg_);
    data->result = zlink_proxy (data->frontend, data->backend, data->capture);
    data->error = zlink_errno ();
    data->done.store (true, std::memory_order_release);
}

static void assert_raw_dealer_part (void *socket_,
                                    const char *expected_,
                                    zlink_part_flag_t expected_more_)
{
    zlink_part_flag_t has_more = ZLINK_PART_FINAL;
    zlink_msg_t part;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&part));

    zlink_recv_result_t result = ZLINK_RECV_NO_DATA;
    for (int attempt = 0; attempt < 1000 && result == ZLINK_RECV_NO_DATA;
         ++attempt) {
        result = zlink_recv_part (socket_, NULL, &part, &has_more,
                                  ZLINK_RECV_FLAGS_DONTWAIT);
        if (result == ZLINK_RECV_NO_DATA)
            msleep (1);
    }

    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, result);
    TEST_ASSERT_EQUAL_INT (expected_more_, has_more);
    TEST_ASSERT_EQUAL_STRING_LEN (
      expected_, static_cast<const char *> (zlink_msg_data (&part)),
      strlen (expected_));

    unsigned char retained_kind = 0xff;
    uint64_t retained_sequence = UINT64_MAX;
    TEST_ASSERT_FALSE (
      reinterpret_cast<zlink::msg_t *> (&part)
        ->get_request_reply_metadata (&retained_kind, &retained_sequence));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&part));
}

static bool wait_for_proxy_exit (proxy_thread_data *data_)
{
    for (int attempt = 0; attempt != 1000; ++attempt) {
        if (data_->done.load (std::memory_order_acquire))
            return true;
        msleep (1);
    }
    return false;
}

static void assert_raw_pair_part (void *socket_, const char *expected_)
{
    zlink_msg_t part;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&part));
    zlink_part_flag_t has_more = ZLINK_PART_MORE;
    zlink_recv_result_t result = ZLINK_RECV_NO_DATA;
    for (int attempt = 0; attempt != 1000
                          && result == ZLINK_RECV_NO_DATA;
         ++attempt) {
        result = zlink_recv_part (
          socket_, NULL, &part, &has_more, ZLINK_RECV_FLAGS_DONTWAIT);
        if (result == ZLINK_RECV_NO_DATA)
            msleep (1);
    }
    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, result);
    TEST_ASSERT_EQUAL_INT (ZLINK_PART_FINAL, has_more);
    TEST_ASSERT_EQUAL_STRING_LEN (
      expected_, static_cast<const char *> (zlink_msg_data (&part)),
      strlen (expected_));
    TEST_ASSERT_EQUAL_UINT64 (strlen (expected_), zlink_msg_size (&part));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&part));
}


// Asynchronous client-to-server (DEALER to ROUTER) - pure libzlink
//
// While this example runs in a single process, that is to make
// it easier to start and stop the example. Each task may have its own
// context and conceptually acts as a separate process. To have this
// behaviour, it is necessary to replace the inproc transport of the
// control socket by a tcp transport.

// This is our client task
// It connects to the server, and then sends a request once per second
// It collects responses as they arrive, and it prints them out. We will
// run several client tasks in parallel, each with a different random ID.

static void client_task (void *db_)
{
    const thread_data *const databag = static_cast<const thread_data *> (db_);
    // Endpoint socket gets random port to avoid test failing when port in use
    void *endpoint = zlink_socket (get_test_context (), ZLINK_SOCKET_PAIR);
    TEST_ASSERT_NOT_NULL (endpoint);
    int linger = 0;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (endpoint, ZLINK_OPT_LINGER, &linger, sizeof (linger)));
    const int endpoint_timeout = 10000;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (endpoint, ZLINK_OPT_RCVTIMEO, &endpoint_timeout,
                        sizeof (endpoint_timeout)));
    char endpoint_source[256];
    snprintf (endpoint_source, 256 * sizeof (char), "inproc://endpoint%d", databag->id);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (endpoint, endpoint_source));
    g_endpoint_clients_connected.fetch_add (1, std::memory_order_release);
    char *my_endpoint = s_recv (endpoint);
    TEST_ASSERT_NOT_NULL (my_endpoint);

    void *client = zlink_socket (get_test_context (), ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (client);

    // Control socket receives terminate command from main over inproc
    void *control = zlink_socket (control_context, ZLINK_SOCKET_SUB);
    TEST_ASSERT_NOT_NULL (control);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (control, ""));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (control, ZLINK_OPT_LINGER, &linger, sizeof (linger)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (control, "inproc://control"));

    char content[CONTENT_SIZE_MAX] = {};
    // Set random routing id to make tracing easier
    char routing_id[ROUTING_ID_SIZE] = {};
    snprintf (routing_id, ROUTING_ID_SIZE * sizeof (char), "%04X-%04X", rand () % 0xFFFF,
              rand () % 0xFFFF);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_routing_id (client, routing_id,
                            ROUTING_ID_SIZE)); // includes '\0' as an helper for printf
    linger = 0;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (client, ZLINK_OPT_LINGER, &linger, sizeof (linger)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (client, my_endpoint));
    g_clients_ready.fetch_add (1, std::memory_order_release);

    zlink_pollitem_t items[] = {{client, 0, ZLINK_POLLIN, 0}, {control, 0, ZLINK_POLLIN, 0}};
    int request_nbr = 0;
    bool run = true;
    bool keep_sending = true;
    while (run) {
        // Tick once per 200 ms, pulling in arriving messages
        int centitick;
        for (centitick = 0; centitick < 20; centitick++) {
            zlink_poll (items, 2, 10, NULL);
            if (items[0].revents & ZLINK_POLLIN) {
                zlink_msg_t msg;
                TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&msg));
                int rc = TEST_ASSERT_SUCCESS_ERRNO (test_recv_single_msg (&msg, client, 0));
                TEST_ASSERT_EQUAL_INT (CONTENT_SIZE, rc);
                memcpy (content, zlink_msg_data (&msg), static_cast<size_t> (CONTENT_SIZE));
                content[CONTENT_SIZE] = '\0';
                if (is_verbose)
                    printf ("client receive - routing_id = %s    content = %s\n", routing_id,
                            content);
                //  Check that message is still the same
                TEST_ASSERT_EQUAL_STRING_LEN ("request #", content, 9);
                TEST_ASSERT_FALSE (test_msg_has_more (&msg));
                TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&msg));
            }
            if (items[1].revents & ZLINK_POLLIN) {
                int rc = zlink_recv (control, content, CONTENT_SIZE_MAX, 0);

                if (rc > 0) {
                    content[rc] = 0; // NULL-terminate the command string
                    if (is_verbose)
                        printf ("client receive - routing_id = %s    command = %s\n", routing_id,
                                content);
                    if (memcmp (content, "TERMINATE", 9) == 0) {
                        run = false;
                        break;
                    }
                    if (memcmp (content, "STOP", 4) == 0) {
                        keep_sending = false;
                        break;
                    }
                }
            }
        }

        if (keep_sending) {
            snprintf (content, CONTENT_SIZE_MAX * sizeof (char), "request #%03d",
                      ++request_nbr); // CONTENT_SIZE
            if (is_verbose)
                printf ("client send - routing_id = %s    request #%03d\n", routing_id,
                        request_nbr);
            zlink_atomic_counter_inc (g_clients_pkts_out);

            TEST_ASSERT_EQUAL_INT (CONTENT_SIZE, zlink_send (client, content, CONTENT_SIZE, 0));
        }
    }

    TEST_ASSERT_SUCCESS_ERRNO (zlink_close (client));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_close (control));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_close (endpoint));
    free (my_endpoint);
}

// This is our server task.
// It uses the multithreaded server model to deal requests out to a pool
// of workers and route replies back to clients. One worker can handle
// one request at a time but one client can talk to multiple workers at
// once.

static void server_worker (void * /*unused_*/);

void server_task (void * /*unused_*/)
{
    // Frontend socket talks to clients over TCP
    char my_endpoint[MAX_SOCKET_STRING];
    void *frontend = zlink_socket (get_test_context (), ZLINK_SOCKET_ROUTER);
    TEST_ASSERT_NOT_NULL (frontend);
    int linger = 0;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (frontend, ZLINK_OPT_LINGER, &linger, sizeof (linger)));
    bind_loopback_ipv4 (frontend, my_endpoint, sizeof my_endpoint);

    // Backend socket talks to workers over inproc
    void *backend = zlink_socket (get_test_context (), ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (backend);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (backend, ZLINK_OPT_LINGER, &linger, sizeof (linger)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (backend, "inproc://backend"));

    // Launch pool of worker threads, precise number is not critical
    int thread_nbr;
    void *threads[QT_WORKERS];
    for (thread_nbr = 0; thread_nbr < QT_WORKERS; thread_nbr++)
        threads[thread_nbr] = zlink_thread_start (&server_worker, NULL);

    // Endpoint socket sends random port to avoid test failing when port in use
    void *endpoint_receivers[QT_CLIENTS];
    char endpoint_source[256];
    for (int i = 0; i < QT_CLIENTS; ++i) {
        endpoint_receivers[i] = zlink_socket (get_test_context (), ZLINK_SOCKET_PAIR);
        TEST_ASSERT_NOT_NULL (endpoint_receivers[i]);
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_set_option (endpoint_receivers[i], ZLINK_OPT_LINGER, &linger, sizeof (linger)));
        const int endpoint_timeout = 10000;
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_set_option (endpoint_receivers[i], ZLINK_OPT_SNDTIMEO,
                            &endpoint_timeout, sizeof (endpoint_timeout)));
        snprintf (endpoint_source, 256 * sizeof (char), "inproc://endpoint%d", i);
        TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (endpoint_receivers[i], endpoint_source));
    }

    // Publish the bound endpoint sockets before clients are launched. Clients
    // then publish their completed inproc connects before this thread sends,
    // so neither side depends on the one-second default socket timeout.
    g_server_endpoints_ready.store (true, std::memory_order_release);
    const std::chrono::steady_clock::time_point endpoint_deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (10);
    while (g_endpoint_clients_connected.load (std::memory_order_acquire)
             != QT_CLIENTS
           && std::chrono::steady_clock::now () < endpoint_deadline)
        msleep (1);
    TEST_ASSERT_EQUAL_INT (
      QT_CLIENTS,
      g_endpoint_clients_connected.load (std::memory_order_acquire));

    for (int i = 0; i < QT_CLIENTS; ++i) {
        send_string_expect_success (endpoint_receivers[i], my_endpoint, 0);
    }

    // Connect backend to frontend via a proxy
    zlink_proxy (frontend, backend, NULL);

    for (thread_nbr = 0; thread_nbr < QT_WORKERS; thread_nbr++)
        zlink_thread_join (threads[thread_nbr]);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_close (frontend));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_close (backend));
    for (int i = 0; i < QT_CLIENTS; ++i) {
        TEST_ASSERT_SUCCESS_ERRNO (zlink_close (endpoint_receivers[i]));
    }
}

// Each worker task works on one request at a time and sends a random number
// of replies back, with random delays between replies:
// The comments in the first column, if suppressed, makes it a poller version

static void server_worker (void * /*unused_*/)
{
    void *worker = zlink_socket (get_test_context (), ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (worker);
    int linger = 0;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (worker, ZLINK_OPT_LINGER, &linger, sizeof (linger)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (worker, "inproc://backend"));

    // Control socket receives terminate command from main over inproc
    void *control = zlink_socket (control_context, ZLINK_SOCKET_SUB);
    TEST_ASSERT_NOT_NULL (control);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_set_subscription (control, ""));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (control, ZLINK_OPT_LINGER, &linger, sizeof (linger)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (control, "inproc://control"));

    char content[CONTENT_SIZE_MAX] = {};       // bigger than what we need to check that
    char routing_id[ROUTING_ID_SIZE_MAX] = {}; // the size received is the size sent

    bool run = true;
    bool keep_sending = true;
    while (run) {
        int rc = zlink_recv (control, content, CONTENT_SIZE_MAX,
                             ZLINK_DONTWAIT); // usually, rc == -1 (no message)
        if (rc > 0) {
            content[rc] = 0; // NULL-terminate the command string
            if (is_verbose)
                printf ("server_worker receives command = %s\n", content);
            if (memcmp (content, "TERMINATE", 9) == 0)
                run = false;
            if (memcmp (content, "STOP", 4) == 0)
                keep_sending = false;
        }
        // The DEALER socket gives us the reply envelope and message
        // if we don't poll, we have to use ZLINK_DONTWAIT, if we poll, we can block-receive with 0
        rc = zlink_recv (worker, routing_id, ROUTING_ID_SIZE_MAX, ZLINK_DONTWAIT);
        if (rc == ROUTING_ID_SIZE) {
            rc = zlink_recv (worker, content, CONTENT_SIZE_MAX, 0);
            TEST_ASSERT_EQUAL_INT (CONTENT_SIZE, rc);
            if (is_verbose)
                printf ("server receive - routing_id = %s    content = %s\n", routing_id, content);

            // Send 0..4 replies back
            if (keep_sending) {
                int reply, replies = rand () % 5;
                for (reply = 0; reply < replies; reply++) {
                    // Sleep for some fraction of a second
                    msleep (rand () % 10 + 1);

                    //  Send message from server to client
                    if (is_verbose)
                        printf ("server send - routing_id = %s    reply\n", routing_id);
                    zlink_atomic_counter_inc (g_workers_pkts_out);

                    rc = zlink_send (worker, routing_id, ROUTING_ID_SIZE, ZLINK_SNDMORE);
                    TEST_ASSERT_EQUAL_INT (ROUTING_ID_SIZE, rc);
                    rc = zlink_send (worker, content, CONTENT_SIZE, 0);
                    TEST_ASSERT_EQUAL_INT (CONTENT_SIZE, rc);
                }
            }
        }
    }
    TEST_ASSERT_SUCCESS_ERRNO (zlink_close (worker));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_close (control));
}

// The main thread simply starts several clients and a server, and then
// waits for the server to finish.

void test_proxy ()
{
    g_clients_pkts_out = zlink_atomic_counter_new ();
    g_workers_pkts_out = zlink_atomic_counter_new ();
    control_context = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (control_context);

    // Control socket receives terminate command from main over inproc
    void *control = zlink_socket (control_context, ZLINK_SOCKET_PUB);
    int linger = 0;
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (control, ZLINK_OPT_LINGER, &linger, sizeof (linger)));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (control, "inproc://control"));

    void *threads[QT_CLIENTS + 1];
    struct thread_data databags[QT_CLIENTS];
    threads[QT_CLIENTS] = zlink_thread_start (&server_task, NULL);

    const std::chrono::steady_clock::time_point endpoints_deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (10);
    while (!g_server_endpoints_ready.load (std::memory_order_acquire)
           && std::chrono::steady_clock::now () < endpoints_deadline)
        msleep (1);
    TEST_ASSERT_TRUE (
      g_server_endpoints_ready.load (std::memory_order_acquire));

    for (int i = 0; i < QT_CLIENTS; i++) {
        databags[i].id = i;
        threads[i] = zlink_thread_start (&client_task, &databags[i]);
    }

    const std::chrono::steady_clock::time_point ready_deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (10);
    while (g_clients_ready.load (std::memory_order_acquire) != QT_CLIENTS
           && std::chrono::steady_clock::now () < ready_deadline)
        msleep (1);
    TEST_ASSERT_EQUAL_INT (
      QT_CLIENTS, g_clients_ready.load (std::memory_order_acquire));

    msleep (500); // Run for 500 ms then quit

    if (is_verbose)
        printf ("stopping all clients and server workers\n");
    send_string_expect_success (control, "STOP", 0);

    msleep (500); // Wait for all clients and workers to STOP

    if (is_verbose)
        printf ("shutting down all clients and server workers\n");
    send_string_expect_success (control, "TERMINATE", 0);

    msleep (500); // Wait for all clients and workers to terminate

    teardown_test_context ();
    g_test_context_active = false;

    for (int i = 0; i < QT_CLIENTS + 1; i++)
        zlink_thread_join (threads[i]);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_close (control));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (control_context));
}

void test_proxy_and_capture_clear_request_reply_metadata ()
{
    void *const context = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (context);

    void *const frontend = zlink_socket (context, ZLINK_SOCKET_DEALER);
    void *const backend = zlink_socket (context, ZLINK_SOCKET_DEALER);
    void *const capture = zlink_socket (context, ZLINK_SOCKET_DEALER);
    void *const source = zlink_socket (context, ZLINK_SOCKET_DEALER);
    void *const sink = zlink_socket (context, ZLINK_SOCKET_DEALER);
    void *const capture_sink = zlink_socket (context, ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (frontend);
    TEST_ASSERT_NOT_NULL (backend);
    TEST_ASSERT_NOT_NULL (capture);
    TEST_ASSERT_NOT_NULL (source);
    TEST_ASSERT_NOT_NULL (sink);
    TEST_ASSERT_NOT_NULL (capture_sink);

    const int zero = 0;
    void *const sockets[] = {frontend, backend, capture, source, sink,
                             capture_sink};
    for (size_t i = 0; i < sizeof (sockets) / sizeof (sockets[0]); ++i) {
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_set_option (sockets[i], ZLINK_OPT_LINGER, &zero,
                            sizeof (zero)));
    }

    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (frontend, "inproc://proxy-metadata-frontend"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (backend, "inproc://proxy-metadata-backend"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (capture, "inproc://proxy-metadata-capture"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (source, "inproc://proxy-metadata-frontend"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (sink, "inproc://proxy-metadata-backend"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (capture_sink, "inproc://proxy-metadata-capture"));

    proxy_thread_data proxy_data (frontend, backend, capture);
    void *const proxy_thread =
      zlink_thread_start (&metadata_proxy_task, &proxy_data);
    TEST_ASSERT_NOT_NULL (proxy_thread);
    msleep (SETTLE_TIME);

    zlink_msg_t head;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&head, 4));
    memcpy (zlink_msg_data (&head), "head", 4);
    TEST_ASSERT_SUCCESS_ERRNO (
      reinterpret_cast<zlink::msg_t *> (&head)
        ->set_request_reply_metadata (zlink::request_reply::request_type,
                                      0x1122334455667788ULL));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (source, &head, ZLINK_SEND_FLAGS_NONE,
                       ZLINK_PART_MORE, NULL, NULL));

    zlink_msg_t tail;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&tail, 4));
    memcpy (zlink_msg_data (&tail), "tail", 4);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (source, &tail, ZLINK_SEND_FLAGS_NONE,
                       ZLINK_PART_FINAL, NULL, NULL));

    assert_raw_dealer_part (sink, "head", ZLINK_PART_MORE);
    assert_raw_dealer_part (sink, "tail", ZLINK_PART_FINAL);
    assert_raw_dealer_part (capture_sink, "head", ZLINK_PART_MORE);
    assert_raw_dealer_part (capture_sink, "tail", ZLINK_PART_FINAL);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_shutdown (context));
    zlink_thread_join (proxy_thread);
    TEST_ASSERT_TRUE (
      proxy_data.result == ZLINK_CONFIG_OK
      || (proxy_data.result == ZLINK_CONFIG_INTERNAL_ERROR
          && proxy_data.error == ETERM));

    for (size_t i = 0; i < sizeof (sockets) / sizeof (sockets[0]); ++i)
        TEST_ASSERT_SUCCESS_ERRNO (zlink_close (sockets[i]));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (context));
}

void test_proxy_rejects_request_reply_metadata_after_first_part ()
{
    void *const context = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (context);
    void *const frontend = zlink_socket (context, ZLINK_SOCKET_DEALER);
    void *const backend = zlink_socket (context, ZLINK_SOCKET_DEALER);
    void *const capture = zlink_socket (context, ZLINK_SOCKET_DEALER);
    void *const source = zlink_socket (context, ZLINK_SOCKET_DEALER);
    void *const sink = zlink_socket (context, ZLINK_SOCKET_DEALER);
    void *const capture_sink = zlink_socket (context, ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (frontend);
    TEST_ASSERT_NOT_NULL (backend);
    TEST_ASSERT_NOT_NULL (capture);
    TEST_ASSERT_NOT_NULL (source);
    TEST_ASSERT_NOT_NULL (sink);
    TEST_ASSERT_NOT_NULL (capture_sink);

    const int zero = 0;
    void *const sockets[] = {frontend, backend, capture, source, sink,
                             capture_sink};
    for (size_t i = 0; i < sizeof (sockets) / sizeof (sockets[0]); ++i) {
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_set_option (sockets[i], ZLINK_OPT_LINGER, &zero,
                            sizeof (zero)));
    }
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (frontend, "inproc://proxy-later-kind-frontend"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (backend, "inproc://proxy-later-kind-backend"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (capture, "inproc://proxy-later-kind-capture"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (source, "inproc://proxy-later-kind-frontend"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (sink, "inproc://proxy-later-kind-backend"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (capture_sink, "inproc://proxy-later-kind-capture"));

    proxy_thread_data proxy_data (frontend, backend, capture);
    void *const proxy_thread =
      zlink_thread_start (&metadata_proxy_task, &proxy_data);
    TEST_ASSERT_NOT_NULL (proxy_thread);
    msleep (SETTLE_TIME);

    zlink_msg_t head;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&head, 4));
    memcpy (zlink_msg_data (&head), "head", 4);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (source, &head, ZLINK_SEND_FLAGS_NONE,
                       ZLINK_PART_MORE, NULL, NULL));
    zlink_msg_t tail;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&tail, 4));
    memcpy (zlink_msg_data (&tail), "tail", 4);
    TEST_ASSERT_SUCCESS_ERRNO (
      reinterpret_cast<zlink::msg_t *> (&tail)
        ->set_request_reply_metadata (zlink::request_reply::reply_type, 45));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (source, &tail, ZLINK_SEND_FLAGS_NONE,
                       ZLINK_PART_FINAL, NULL, NULL));

    bool completed_before_shutdown = false;
    for (int attempt = 0; attempt < 1000; ++attempt) {
        if (proxy_data.done.load (std::memory_order_acquire)) {
            completed_before_shutdown = true;
            break;
        }
        msleep (1);
    }
    if (!completed_before_shutdown)
        TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_shutdown (context));
    zlink_thread_join (proxy_thread);
    TEST_ASSERT_TRUE_MESSAGE (
      completed_before_shutdown,
      "proxy did not reject malformed inproc multipart promptly");
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_INVALID_STATE, proxy_data.result);
    TEST_ASSERT_EQUAL_INT (EPROTO, proxy_data.error);

    void *const receivers[] = {sink, capture_sink};
    for (size_t i = 0; i < sizeof (receivers) / sizeof (receivers[0]); ++i) {
        zlink_msg_t part;
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&part));
        zlink_part_flag_t has_more = ZLINK_PART_FINAL;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_RECV_NO_DATA,
          zlink_recv_part (receivers[i], NULL, &part, &has_more,
                           ZLINK_RECV_FLAGS_DONTWAIT));
        TEST_ASSERT_EQUAL_INT (EAGAIN, zlink_errno ());
        TEST_ASSERT_EQUAL_UINT64 (0, zlink_msg_size (&part));
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&part));
    }

    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_shutdown (context));
    for (size_t i = 0; i < sizeof (sockets) / sizeof (sockets[0]); ++i)
        TEST_ASSERT_SUCCESS_ERRNO (zlink_close (sockets[i]));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (context));
}

void test_proxy_rolls_back_capture_after_destination_send_failure ()
{
    void *const context = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (context);
    void *const first_frontend = zlink_socket (context, ZLINK_SOCKET_PAIR);
    void *const second_frontend = zlink_socket (context, ZLINK_SOCKET_PAIR);
    void *const backend = zlink_socket (context, ZLINK_SOCKET_PAIR);
    void *const capture = zlink_socket (context, ZLINK_SOCKET_PAIR);
    void *const first_source = zlink_socket (context, ZLINK_SOCKET_PAIR);
    void *const second_source = zlink_socket (context, ZLINK_SOCKET_PAIR);
    void *const sink = zlink_socket (context, ZLINK_SOCKET_PAIR);
    void *const capture_sink = zlink_socket (context, ZLINK_SOCKET_PAIR);
    TEST_ASSERT_NOT_NULL (first_frontend);
    TEST_ASSERT_NOT_NULL (second_frontend);
    TEST_ASSERT_NOT_NULL (backend);
    TEST_ASSERT_NOT_NULL (capture);
    TEST_ASSERT_NOT_NULL (first_source);
    TEST_ASSERT_NOT_NULL (second_source);
    TEST_ASSERT_NOT_NULL (sink);
    TEST_ASSERT_NOT_NULL (capture_sink);

    const int zero = 0;
    void *const sockets[] = {first_frontend, second_frontend, backend, capture,
                             first_source, second_source, sink, capture_sink};
    for (size_t i = 0; i != sizeof (sockets) / sizeof (sockets[0]); ++i) {
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_set_option (sockets[i], ZLINK_OPT_LINGER, &zero,
                            sizeof (zero)));
    }

    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (first_frontend, "inproc://proxy-output-fail-first"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (second_frontend, "inproc://proxy-output-fail-second"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (backend, "inproc://proxy-output-fail-backend"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (capture, "inproc://proxy-output-fail-capture"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (first_source, "inproc://proxy-output-fail-first"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (second_source, "inproc://proxy-output-fail-second"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (sink, "inproc://proxy-output-fail-backend"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (capture_sink, "inproc://proxy-output-fail-capture"));

    proxy_thread_data first_proxy (first_frontend, backend, capture);
    zlink::test_fail_next_proxy_destination_send ();
    void *const first_proxy_thread =
      zlink_thread_start (&metadata_proxy_task, &first_proxy);
    TEST_ASSERT_NOT_NULL (first_proxy_thread);
    msleep (SETTLE_TIME);

    zlink_msg_t orphan_head;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&orphan_head, 6));
    memcpy (zlink_msg_data (&orphan_head), "orphan", 6);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (first_source, &orphan_head, ZLINK_SEND_FLAGS_NONE,
                       ZLINK_PART_MORE, NULL, NULL));
    zlink_msg_t orphan_tail;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&orphan_tail, 4));
    memcpy (zlink_msg_data (&orphan_tail), "tail", 4);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (first_source, &orphan_tail, ZLINK_SEND_FLAGS_NONE,
                       ZLINK_PART_FINAL, NULL, NULL));

    TEST_ASSERT_TRUE_MESSAGE (
      wait_for_proxy_exit (&first_proxy),
      "proxy did not report the injected destination send failure");
    zlink_thread_join (first_proxy_thread);
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_INTERNAL_ERROR, first_proxy.result);
    TEST_ASSERT_EQUAL_INT (EAGAIN, first_proxy.error);

    proxy_thread_data second_proxy (second_frontend, backend, capture);
    void *const second_proxy_thread =
      zlink_thread_start (&metadata_proxy_task, &second_proxy);
    TEST_ASSERT_NOT_NULL (second_proxy_thread);
    msleep (SETTLE_TIME);

    zlink_msg_t fresh;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&fresh, 5));
    memcpy (zlink_msg_data (&fresh), "fresh", 5);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (second_source, &fresh, ZLINK_SEND_FLAGS_NONE,
                       ZLINK_PART_FINAL, NULL, NULL));

    assert_raw_pair_part (sink, "fresh");
    assert_raw_pair_part (capture_sink, "fresh");

    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_shutdown (context));
    zlink_thread_join (second_proxy_thread);
    TEST_ASSERT_TRUE (
      second_proxy.result == ZLINK_CONFIG_OK
      || (second_proxy.result == ZLINK_CONFIG_INTERNAL_ERROR
          && second_proxy.error == ETERM));
    for (size_t i = 0; i != sizeof (sockets) / sizeof (sockets[0]); ++i)
        TEST_ASSERT_SUCCESS_ERRNO (zlink_close (sockets[i]));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (context));
}

int main (void)
{
    setup_test_environment (360);

    UNITY_BEGIN ();
    RUN_TEST (test_proxy_and_capture_clear_request_reply_metadata);
    RUN_TEST (test_proxy_rejects_request_reply_metadata_after_first_part);
    RUN_TEST (test_proxy_rolls_back_capture_after_destination_send_failure);
    RUN_TEST (test_proxy);
    return UNITY_END ();
}
