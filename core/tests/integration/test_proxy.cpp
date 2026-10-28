/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"

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
    const char *kind;
    int id;
    int failure_line;
    int error_number;
    char failure_message[128];
};

struct server_data
{
    thread_data thread;
    thread_data workers[QT_WORKERS];
};

void *g_clients_pkts_out = NULL;
void *g_workers_pkts_out = NULL;
void *control_context = NULL;
bool g_test_context_active = false;
std::atomic<bool> g_server_endpoints_ready (false);
std::atomic<int> g_endpoint_clients_connected (0);
std::atomic<int> g_clients_ready (0);

static void initialize_thread_data (thread_data *data_, const char *kind_, int id_)
{
    data_->kind = kind_;
    data_->id = id_;
    data_->failure_line = 0;
    data_->error_number = 0;
    data_->failure_message[0] = '\0';
}

static bool check_thread (thread_data *data_, bool condition_, int line_,
                          const char *message_)
{
    if (condition_)
        return true;
    if (data_->failure_line == 0) {
        data_->failure_line = line_;
        data_->error_number = zlink_errno ();
        snprintf (data_->failure_message, sizeof data_->failure_message, "%s",
                  message_);
    }
    return false;
}

#define CHECK_THREAD(data_, condition_, message_) \
    check_thread ((data_), (condition_), __LINE__, (message_))

static void append_thread_failure (char *details_, size_t capacity_, int *count_,
                                   const thread_data *data_)
{
    if (data_->failure_line == 0)
        return;
    const size_t used = strlen (details_);
    if (used < capacity_)
        snprintf (details_ + used, capacity_ - used,
                  "%s%s id=%d line=%d errno=%d: %s", used ? "\n" : "",
                  data_->kind, data_->id, data_->failure_line,
                  data_->error_number, data_->failure_message);
    ++*count_;
}

void setUp ()
{
    setup_test_context ();
    g_test_context_active = true;
    g_server_endpoints_ready.store (false, std::memory_order_release);
    g_endpoint_clients_connected.store (0, std::memory_order_release);
    g_clients_ready.store (0, std::memory_order_release);
}

void tearDown ()
{
    // Unity still invokes tearDown after a TEST_ASSERT longjmp, while C++
    // automatic destructors on the interrupted stack are not guaranteed.
    if (g_test_context_active) {
        teardown_test_context ();
        g_test_context_active = false;
    }
    g_server_endpoints_ready.store (false, std::memory_order_release);
    g_endpoint_clients_connected.store (0, std::memory_order_release);
    g_clients_ready.store (0, std::memory_order_release);
}






static int receive_control (thread_data *thread_, void *socket_, char *data_, size_t capacity_,
                            zlink_recv_flags_t flags_)
{
    zlink_msg_t part;
    if (!CHECK_THREAD (thread_, zlink_msg_init (&part) == 0,
                       "initialize control message"))
        return -1;
    size_t more = 0;
    size_t topic_size = 0;
    const zlink_recv_result_t rc = zlink_subscribe (socket_, NULL, NULL, 0, &topic_size, &part, 1, &more, flags_);
    if (rc != ZLINK_RECV_OK) {
        CHECK_THREAD (thread_, zlink_msg_close (&part) == 0,
                      "close empty control message");
        return -1;
    }
    const size_t size = zlink_msg_size (&part);
    bool valid = true;
    if (!CHECK_THREAD (thread_, topic_size == 0, "control topic size"))
        valid = false;
    if (!CHECK_THREAD (thread_, more == 1, "control message part count"))
        valid = false;
    if (!CHECK_THREAD (thread_, size < capacity_, "control message size"))
        valid = false;
    else
        memcpy (data_, zlink_msg_data (&part), size);
    if (!CHECK_THREAD (thread_, zlink_msg_close (&part) == 0,
                       "close control message"))
        valid = false;
    if (!valid)
        return -1;
    return static_cast<int> (size);
}

static int send_worker_message (thread_data *thread_, void *socket_,
                                const void *data_, size_t size_, int flags_)
{
    zlink_msg_t message;
    if (!CHECK_THREAD (thread_, zlink_msg_init_size (&message, size_) == 0,
                       "initialize outgoing message"))
        return -1;
    if (size_ > 0 && data_ != NULL)
        memcpy (zlink_msg_data (&message), data_, size_);
    const int rc = test_send_single_msg (&message, socket_, flags_);
    if (!CHECK_THREAD (thread_, rc == static_cast<int> (size_),
                       "send string")) {
        zlink_msg_close (&message);
        return -1;
    }
    return rc;
}

static int send_worker_string (thread_data *thread_, void *socket_, const char *data_,
                               int flags_)
{
    return send_worker_message (thread_, socket_, data_,
                                data_ ? strlen (data_) : 0, flags_);
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
    thread_data *const databag = static_cast<thread_data *> (db_);
    void *endpoint = NULL;
    void *client = NULL;
    void *control = NULL;
    char *my_endpoint = NULL;

    const bool completed = [&]() -> bool {
        // Endpoint socket gets random port to avoid test failing when port in use
        endpoint = zlink_socket (get_test_context (), ZLINK_SOCKET_PAIR);
        if (!CHECK_THREAD (databag, endpoint != NULL, "create endpoint socket"))
            return false;
        int linger = 0;
        if (!CHECK_THREAD (
              databag,
              zlink_set_option (endpoint, ZLINK_OPT_LINGER, &linger,
                                sizeof (linger)) == 0,
              "set endpoint linger"))
            return false;
        const int endpoint_timeout = 10000;
        if (!CHECK_THREAD (
              databag,
              zlink_set_option (endpoint, ZLINK_OPT_RCVTIMEO, &endpoint_timeout,
                                sizeof (endpoint_timeout)) == 0,
              "set endpoint receive timeout"))
            return false;
        char endpoint_source[256];
        snprintf (endpoint_source, 256 * sizeof (char), "inproc://endpoint%d",
                  databag->id);
        if (!CHECK_THREAD (databag, zlink_connect (endpoint, endpoint_source) == 0,
                           "connect endpoint socket"))
            return false;
        g_endpoint_clients_connected.fetch_add (1, std::memory_order_release);
        my_endpoint = s_recv (endpoint);
        if (!CHECK_THREAD (databag, my_endpoint != NULL,
                           "receive endpoint address"))
            return false;

        client = zlink_socket (get_test_context (), ZLINK_SOCKET_DEALER);
        if (!CHECK_THREAD (databag, client != NULL, "create client socket"))
            return false;

        // Control socket receives terminate command from main over inproc
        control = zlink_socket (control_context, ZLINK_SOCKET_SUB);
        if (!CHECK_THREAD (databag, control != NULL, "create control socket"))
            return false;
        if (!CHECK_THREAD (databag, zlink_set_subscription (control, "") == 0,
                           "set control subscription"))
            return false;
        if (!CHECK_THREAD (
              databag,
              zlink_set_option (control, ZLINK_OPT_LINGER, &linger,
                                sizeof (linger)) == 0,
              "set control linger"))
            return false;
        if (!CHECK_THREAD (databag,
                           zlink_connect (control, "inproc://control") == 0,
                           "connect control socket"))
            return false;

        char content[CONTENT_SIZE_MAX] = {};
        // Set a client-specific routing id for tracing.
        char routing_id[ROUTING_ID_SIZE] = {};
        snprintf (routing_id, sizeof routing_id, "%04X-%04X", databag->id,
                  databag->id);
        if (!CHECK_THREAD (
              databag,
              zlink_set_routing_id (client, routing_id, ROUTING_ID_SIZE) == 0,
              "set client routing id"))
            return false;
        linger = 0;
        if (!CHECK_THREAD (
              databag,
              zlink_set_option (client, ZLINK_OPT_LINGER, &linger,
                                sizeof (linger)) == 0,
              "set client linger"))
            return false;
        if (!CHECK_THREAD (databag, zlink_connect (client, my_endpoint) == 0,
                           "connect client socket"))
            return false;
        g_clients_ready.fetch_add (1, std::memory_order_release);

        zlink_pollitem_t items[] = {{client, 0, ZLINK_POLLIN, 0},
                                    {control, 0, ZLINK_POLLIN, 0}};
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
                    if (!CHECK_THREAD (databag, zlink_msg_init (&msg) == 0,
                                       "initialize received message"))
                        return false;
                    size_t has_more = 0;
                    const int rc = test_recv_single_msg (&msg, client, 0, &has_more);
                    bool valid_size =
                      CHECK_THREAD (databag, rc == CONTENT_SIZE,
                                    "received message size");
                    if (rc >= CONTENT_SIZE)
                        memcpy (content, zlink_msg_data (&msg), CONTENT_SIZE);
                    const bool valid_more =
                      CHECK_THREAD (databag, has_more == 1,
                                    "received message part count");
                    const bool valid_close =
                      CHECK_THREAD (databag, zlink_msg_close (&msg) == 0,
                                    "close received message");
                    if (!valid_size || !valid_more || !valid_close)
                        return false;
                    content[CONTENT_SIZE] = '\0';
                    if (is_verbose)
                        printf ("client receive - routing_id = %s    content = %s\n",
                                routing_id, content);
                    // Check that the message still contains the request prefix.
                    if (!CHECK_THREAD (databag,
                                       memcmp (content, "request #", 9) == 0,
                                       "received request prefix"))
                        return false;
                }
                if (items[1].revents & ZLINK_POLLIN) {
                    const int rc = receive_control (
                      databag, control, content, CONTENT_SIZE_MAX,
                      ZLINK_RECV_FLAGS_NONE);
                    if (!CHECK_THREAD (databag, databag->failure_line == 0,
                                       "receive control command"))
                        return false;

                    if (rc > 0) {
                        content[rc] = 0; // NULL-terminate the command string
                        if (is_verbose)
                            printf ("client receive - routing_id = %s    command = %s\n",
                                    routing_id, content);
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
                snprintf (content, CONTENT_SIZE_MAX * sizeof (char),
                          "request #%03d", ++request_nbr); // CONTENT_SIZE
                if (is_verbose)
                    printf ("client send - routing_id = %s    request #%03d\n",
                            routing_id, request_nbr);
                zlink_atomic_counter_inc (g_clients_pkts_out);

                if (send_worker_message (databag, client, content, CONTENT_SIZE, 0)
                    != CONTENT_SIZE)
                    return false;
            }
        }
        return true;
    } ();
    (void) completed;

    if (client != NULL)
        CHECK_THREAD (databag, zlink_close (client) == 0, "close client socket");
    if (control != NULL)
        CHECK_THREAD (databag, zlink_close (control) == 0, "close control socket");
    if (endpoint != NULL)
        CHECK_THREAD (databag, zlink_close (endpoint) == 0, "close endpoint socket");
    free (my_endpoint);
}

// This is our server task.
// It uses the multithreaded server model to deal requests out to a pool
// of workers and route replies back to clients. One worker can handle
// one request at a time but one client can talk to multiple workers at
// once.

static void server_worker (void * /*unused_*/);

void server_task (void *db_)
{
    server_data *const databag = static_cast<server_data *> (db_);
    thread_data *const thread = &databag->thread;
    char my_endpoint[MAX_SOCKET_STRING] = {};
    void *frontend = NULL;
    void *backend = NULL;
    void *endpoint_receivers[QT_CLIENTS] = {};
    void *threads[QT_WORKERS] = {};
    int thread_count = 0;

    const bool completed = [&]() -> bool {
        // Frontend socket talks to clients over TCP
        frontend = zlink_socket (get_test_context (), ZLINK_SOCKET_ROUTER);
        if (!CHECK_THREAD (thread, frontend != NULL, "create frontend socket"))
            return false;
        int linger = 0;
        if (!CHECK_THREAD (
              thread,
              zlink_set_option (frontend, ZLINK_OPT_LINGER, &linger,
                                sizeof (linger)) == 0,
              "set frontend linger"))
            return false;
        int ipv6 = 0;
        if (!CHECK_THREAD (
              thread,
              zlink_set_option (frontend, ZLINK_OPT_IPV6, &ipv6,
                                sizeof (ipv6)) == 0,
              "set frontend address family"))
            return false;
        if (!CHECK_THREAD (
              thread,
              zlink_bind (frontend, "tcp://127.0.0.1:*") == 0,
              "bind frontend socket"))
            return false;
        size_t endpoint_size = sizeof my_endpoint;
        if (!CHECK_THREAD (
              thread,
              zlink_get_option (frontend, ZLINK_OPT_LAST_ENDPOINT,
                                my_endpoint, &endpoint_size) == 0,
              "get frontend endpoint"))
            return false;

        // Backend socket talks to workers over inproc
        backend = zlink_socket (get_test_context (), ZLINK_SOCKET_DEALER);
        if (!CHECK_THREAD (thread, backend != NULL, "create backend socket"))
            return false;
        if (!CHECK_THREAD (
              thread,
              zlink_set_option (backend, ZLINK_OPT_LINGER, &linger,
                                sizeof (linger)) == 0,
              "set backend linger"))
            return false;
        if (!CHECK_THREAD (thread, zlink_bind (backend, "inproc://backend") == 0,
                           "bind backend socket"))
            return false;

        // Launch pool of worker threads, precise number is not critical
        for (int i = 0; i < QT_WORKERS; ++i) {
            threads[i] = zlink_thread_start (&server_worker,
                                             &databag->workers[i]);
            if (!CHECK_THREAD (thread, threads[i] != NULL,
                               "start server worker"))
                return false;
            ++thread_count;
        }

        // Endpoint socket sends random port to avoid test failing when port in use
        char endpoint_source[256];
        for (int i = 0; i < QT_CLIENTS; ++i) {
            endpoint_receivers[i] =
              zlink_socket (get_test_context (), ZLINK_SOCKET_PAIR);
            if (!CHECK_THREAD (thread, endpoint_receivers[i] != NULL,
                               "create endpoint receiver"))
                return false;
            if (!CHECK_THREAD (
                  thread,
                  zlink_set_option (endpoint_receivers[i], ZLINK_OPT_LINGER,
                                    &linger, sizeof (linger)) == 0,
                  "set endpoint receiver linger"))
                return false;
            const int endpoint_timeout = 10000;
            if (!CHECK_THREAD (
                  thread,
                  zlink_set_option (endpoint_receivers[i], ZLINK_OPT_SNDTIMEO,
                                    &endpoint_timeout,
                                    sizeof (endpoint_timeout)) == 0,
                  "set endpoint receiver send timeout"))
                return false;
            snprintf (endpoint_source, 256 * sizeof (char),
                      "inproc://endpoint%d", i);
            if (!CHECK_THREAD (
                  thread,
                  zlink_bind (endpoint_receivers[i], endpoint_source) == 0,
                  "bind endpoint receiver"))
                return false;
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
        if (!CHECK_THREAD (
              thread,
              g_endpoint_clients_connected.load (std::memory_order_acquire)
                == QT_CLIENTS,
              "all client endpoints connected"))
            return false;

        for (int i = 0; i < QT_CLIENTS; ++i) {
            if (send_worker_string (thread, endpoint_receivers[i], my_endpoint, 0)
                < 0)
                return false;
        }

        // Connect backend to frontend via a proxy
        zlink_proxy (frontend, backend, NULL);
        return true;
    } ();
    (void) completed;

    for (int i = 0; i < thread_count; ++i)
        zlink_thread_join (threads[i]);

    if (frontend != NULL)
        CHECK_THREAD (thread, zlink_close (frontend) == 0,
                      "close frontend socket");
    if (backend != NULL)
        CHECK_THREAD (thread, zlink_close (backend) == 0,
                      "close backend socket");
    for (int i = 0; i < QT_CLIENTS; ++i) {
        if (endpoint_receivers[i] != NULL)
            CHECK_THREAD (thread, zlink_close (endpoint_receivers[i]) == 0,
                          "close endpoint receiver");
    }
}

// Each worker task works on one request at a time and sends a random number
// of replies back, with random delays between replies:
// The comments in the first column, if suppressed, makes it a poller version

static void server_worker (void *db_)
{
    thread_data *const thread = static_cast<thread_data *> (db_);
    void *worker = NULL;
    void *control = NULL;

    const bool completed = [&]() -> bool {
        worker = zlink_socket (get_test_context (), ZLINK_SOCKET_DEALER);
        if (!CHECK_THREAD (thread, worker != NULL, "create worker socket"))
            return false;
        int linger = 0;
        if (!CHECK_THREAD (
              thread,
              zlink_set_option (worker, ZLINK_OPT_LINGER, &linger,
                                sizeof (linger)) == 0,
              "set worker linger"))
            return false;
        if (!CHECK_THREAD (thread, zlink_connect (worker, "inproc://backend") == 0,
                           "connect worker socket"))
            return false;

        // Control socket receives terminate command from main over inproc
        control = zlink_socket (control_context, ZLINK_SOCKET_SUB);
        if (!CHECK_THREAD (thread, control != NULL, "create control socket"))
            return false;
        if (!CHECK_THREAD (thread, zlink_set_subscription (control, "") == 0,
                           "set control subscription"))
            return false;
        if (!CHECK_THREAD (
              thread,
              zlink_set_option (control, ZLINK_OPT_LINGER, &linger,
                                sizeof (linger)) == 0,
              "set control linger"))
            return false;
        if (!CHECK_THREAD (thread,
                           zlink_connect (control, "inproc://control") == 0,
                           "connect control socket"))
            return false;

        char content[CONTENT_SIZE_MAX] = {};       // bigger than what we need to check that
        char routing_id[ROUTING_ID_SIZE_MAX] = {}; // the size received is the size sent

        bool run = true;
        bool keep_sending = true;
        while (run) {
            const int rc = receive_control (
              thread, control, content, CONTENT_SIZE_MAX,
              ZLINK_RECV_FLAGS_DONTWAIT); // usually, rc == -1 (no message)
            if (!CHECK_THREAD (thread, thread->failure_line == 0,
                               "receive control command"))
                return false;
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
            zlink_msg_t incoming[2];
            size_t incoming_count = 0;
            const zlink_recv_result_t recv_result =
              zlink_recv (worker, NULL, incoming, 2, &incoming_count,
                          ZLINK_RECV_FLAGS_DONTWAIT);
            if (recv_result == ZLINK_RECV_OK) {
                bool valid = true;
                if (!CHECK_THREAD (thread, incoming_count == 2,
                                   "incoming message part count"))
                    valid = false;
                if (incoming_count == 2) {
                    if (!CHECK_THREAD (
                          thread,
                          zlink_msg_size (&incoming[0]) == ROUTING_ID_SIZE,
                          "incoming routing id size"))
                        valid = false;
                    if (!CHECK_THREAD (
                          thread,
                          zlink_msg_size (&incoming[1]) == CONTENT_SIZE,
                          "incoming content size"))
                        valid = false;
                }
                if (valid) {
                    memcpy (routing_id, zlink_msg_data (&incoming[0]),
                            ROUTING_ID_SIZE);
                    memcpy (content, zlink_msg_data (&incoming[1]),
                            CONTENT_SIZE);
                }
                zlink_multipart_close (incoming, incoming_count);
                if (!valid)
                    return false;
                if (is_verbose)
                    printf ("server receive - routing_id = %s    content = %s\n",
                            routing_id, content);

                // Send 0..4 replies back
                if (keep_sending) {
                    int reply, replies = rand () % 5;
                    for (reply = 0; reply < replies; reply++) {
                        // Sleep for some fraction of a second
                        msleep (rand () % 10 + 1);

                        // Send message from server to client
                        if (is_verbose)
                            printf ("server send - routing_id = %s    reply\n",
                                    routing_id);
                        zlink_atomic_counter_inc (g_workers_pkts_out);

                        zlink_msg_t reply_parts[2];
                        if (!CHECK_THREAD (
                              thread,
                              zlink_msg_init_size (&reply_parts[0],
                                                   ROUTING_ID_SIZE) == 0,
                              "initialize reply routing id"))
                            return false;
                        if (!CHECK_THREAD (
                              thread,
                              zlink_msg_init_size (&reply_parts[1],
                                                   CONTENT_SIZE) == 0,
                              "initialize reply content")) {
                            zlink_msg_close (&reply_parts[0]);
                            return false;
                        }
                        memcpy (zlink_msg_data (&reply_parts[0]), routing_id,
                                ROUTING_ID_SIZE);
                        memcpy (zlink_msg_data (&reply_parts[1]), content,
                                CONTENT_SIZE);
                        const zlink_submit_result_t send_result = zlink_send (
                          worker, reply_parts, 2, ZLINK_SEND_FLAGS_NONE, NULL,
                          NULL);
                        if (!CHECK_THREAD (
                              thread, send_result == ZLINK_SUBMIT_OK,
                              "send worker reply")) {
                            zlink_multipart_close (reply_parts, 2);
                            return false;
                        }
                    }
                }
            } else {
                if (!CHECK_THREAD (thread, recv_result == ZLINK_RECV_NO_DATA,
                                   "nonblocking receive result"))
                    return false;
                if (!CHECK_THREAD (thread, zlink_errno () == EAGAIN,
                                   "nonblocking receive errno"))
                    return false;
            }
        }
        return true;
    } ();
    (void) completed;

    if (worker != NULL)
        CHECK_THREAD (thread, zlink_close (worker) == 0, "close worker socket");
    if (control != NULL)
        CHECK_THREAD (thread, zlink_close (control) == 0, "close control socket");
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

    void *threads[QT_CLIENTS + 1] = {};
    thread_data main_result;
    initialize_thread_data (&main_result, "test_proxy", 0);
    server_data server_databag = {};
    initialize_thread_data (&server_databag.thread, "server", 0);
    for (int i = 0; i < QT_WORKERS; ++i)
        initialize_thread_data (&server_databag.workers[i], "server_worker", i);
    thread_data databags[QT_CLIENTS];
    threads[QT_CLIENTS] = zlink_thread_start (&server_task, &server_databag);
    CHECK_THREAD (&server_databag.thread, threads[QT_CLIENTS] != NULL,
                  "start server thread");

    const std::chrono::steady_clock::time_point endpoints_deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (10);
    while (!g_server_endpoints_ready.load (std::memory_order_acquire)
           && std::chrono::steady_clock::now () < endpoints_deadline)
        msleep (1);
    const bool endpoints_ready =
      g_server_endpoints_ready.load (std::memory_order_acquire);

    for (int i = 0; i < QT_CLIENTS; i++) {
        initialize_thread_data (&databags[i], "client", i);
        databags[i].id = i;
        threads[i] = zlink_thread_start (&client_task, &databags[i]);
        CHECK_THREAD (&databags[i], threads[i] != NULL, "start client thread");
    }

    const std::chrono::steady_clock::time_point ready_deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (10);
    while (g_clients_ready.load (std::memory_order_acquire) != QT_CLIENTS
           && std::chrono::steady_clock::now () < ready_deadline)
        msleep (1);
    const int clients_ready = g_clients_ready.load (std::memory_order_acquire);

    msleep (500); // Run for 500 ms then quit

    if (is_verbose)
        printf ("stopping all clients and server workers\n");
    send_published_string_expect_success (control, "", "STOP");

    msleep (500); // Wait for all clients and workers to STOP

    if (is_verbose)
        printf ("shutting down all clients and server workers\n");
    send_published_string_expect_success (control, "", "TERMINATE");

    msleep (500); // Wait for all clients and workers to terminate

    teardown_test_context ();
    g_test_context_active = false;

    for (int i = 0; i < QT_CLIENTS + 1; i++)
        if (threads[i] != NULL)
            zlink_thread_join (threads[i]);

    TEST_ASSERT_SUCCESS_ERRNO (zlink_close (control));
    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_term (control_context));

    CHECK_THREAD (&main_result, endpoints_ready,
                  "server endpoints did not become ready");
    CHECK_THREAD (&main_result, clients_ready == QT_CLIENTS,
                  "not all clients became ready");
    char failure_details[4096] = {};
    int failure_count = 0;
    append_thread_failure (failure_details, sizeof failure_details,
                          &failure_count, &main_result);
    append_thread_failure (failure_details, sizeof failure_details,
                          &failure_count, &server_databag.thread);
    for (int i = 0; i < QT_WORKERS; ++i)
        append_thread_failure (failure_details, sizeof failure_details,
                              &failure_count, &server_databag.workers[i]);
    for (int i = 0; i < QT_CLIENTS; ++i)
        append_thread_failure (failure_details, sizeof failure_details,
                              &failure_count, &databags[i]);
    TEST_ASSERT_EQUAL_INT_MESSAGE (0, failure_count, failure_details);
}




int main (void)
{
    setup_test_environment (360);

    UNITY_BEGIN ();
    RUN_TEST (test_proxy);
    return UNITY_END ();
}
