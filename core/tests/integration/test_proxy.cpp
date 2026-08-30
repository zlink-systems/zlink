/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_monitoring.hpp"
#include "testutil_unity.hpp"
#include "api/socket/request_reply_protocol_internal.hpp"
#include "api/socket/socket_api_internal.hpp"
#include "api/socket/socket_request_reply_internal.hpp"
#include "protocol/wire.hpp"
#include "protocol/zmp_metadata.hpp"
#include "protocol/zmp_protocol.hpp"
#include "sockets/proxy/proxy.hpp"

#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

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

struct proxy_request_probe_t
{
    proxy_request_probe_t () : done (false), result (ZLINK_REQUEST_INTERNAL_ERROR),
                               callback_count (0), part_count (0)
    {
    }

    std::atomic<bool> done;
    zlink_request_result_t result;
    size_t callback_count;
    size_t part_count;
};

void *g_clients_pkts_out = NULL;
void *g_workers_pkts_out = NULL;
void *control_context = NULL;
bool g_test_context_active = false;

void setUp ()
{
    setup_test_context ();
    g_test_context_active = true;
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
}

class proxy_part_forwarded_hook_scope_t
{
  public:
    proxy_part_forwarded_hook_scope_t (
      zlink::proxy_part_forwarded_hook_fn hook_, void *userdata_) :
        _active (true)
    {
        zlink::test_set_proxy_part_forwarded_hook (hook_, userdata_);
    }

    ~proxy_part_forwarded_hook_scope_t () { reset (); }

    void reset ()
    {
        if (_active) {
            zlink::test_set_proxy_part_forwarded_hook (NULL, NULL);
            _active = false;
        }
    }

  private:
    proxy_part_forwarded_hook_scope_t (
      const proxy_part_forwarded_hook_scope_t &);
    proxy_part_forwarded_hook_scope_t &operator= (
      const proxy_part_forwarded_hook_scope_t &);

    bool _active;
};

static void metadata_proxy_task (void *arg_)
{
    proxy_thread_data *const data = static_cast<proxy_thread_data *> (arg_);
    data->result = zlink_proxy (data->frontend, data->backend, data->capture);
    data->error = zlink_errno ();
    data->done.store (true, std::memory_order_release);
}

static void mark_proxy_part_forwarded (void *userdata_)
{
    static_cast<std::atomic<bool> *> (userdata_)->store (
      true, std::memory_order_release);
}

static void capture_proxy_request_completion (zlink_request_result_t result_,
                                              zlink_msg_t *parts_,
                                              size_t part_count_,
                                              void *userdata_)
{
    proxy_request_probe_t *const probe =
      static_cast<proxy_request_probe_t *> (userdata_);
    if (!probe)
        return;
    probe->result = result_;
    probe->part_count = part_count_;
    ++probe->callback_count;
    if (parts_)
        zlink_multipart_close (parts_, part_count_);
    probe->done.store (true, std::memory_order_release);
}

static void assert_raw_dealer_part (void *socket_,
                                    const char *expected_,
                                    zlink_part_flag_t expected_more_)
{
    uint8_t message_type = 0xff;
    uint64_t request_seq = UINT64_MAX;
    zlink_part_flag_t has_more = ZLINK_PART_FINAL;
    zlink_msg_t part;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&part));

    zlink_recv_result_t result = ZLINK_RECV_NO_DATA;
    for (int attempt = 0; attempt < 1000 && result == ZLINK_RECV_NO_DATA;
         ++attempt) {
        result = zlink_dealer_recv_part (socket_, &message_type, &request_seq,
                                         &part, &has_more,
                                         ZLINK_RECV_FLAGS_DONTWAIT);
        if (result == ZLINK_RECV_NO_DATA)
            msleep (1);
    }

    TEST_ASSERT_EQUAL_INT (ZLINK_RECV_OK, result);
    TEST_ASSERT_EQUAL_UINT8 (ZLINK_DEALER_MESSAGE_RAW, message_type);
    TEST_ASSERT_EQUAL_UINT64 (0, request_seq);
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

static bool proxy_test_send_all (fd_t fd_, const unsigned char *data_,
                                 size_t size_)
{
    size_t offset = 0;
    while (offset < size_) {
#if defined ZLINK_HAVE_WINDOWS
        const int rc = send (
          fd_, reinterpret_cast<const char *> (data_ + offset),
          static_cast<int> (size_ - offset), 0);
#else
        const ssize_t rc = send (
          fd_, data_ + offset, size_ - offset, MSG_NOSIGNAL);
#endif
        if (rc <= 0)
            return false;
        offset += static_cast<size_t> (rc);
    }
    return true;
}

static bool proxy_test_send_zmp_frame (fd_t fd_, unsigned char flags_,
                                       const unsigned char *body_,
                                       size_t body_size_)
{
    unsigned char header[zlink::zmp_header_size];
    header[0] = zlink::zmp_magic;
    header[1] = zlink::zmp_version;
    header[2] = flags_;
    header[3] = zlink::zmp_kind_data;
    zlink::put_uint32 (header + 4, static_cast<uint32_t> (body_size_));
    return proxy_test_send_all (fd_, header, sizeof (header))
           && (body_size_ == 0
               || proxy_test_send_all (fd_, body_, body_size_));
}

static bool proxy_test_send_pair_handshake (fd_t fd_)
{
    const unsigned char hello[] = {
      zlink::zmp_control_hello, ZLINK_CORE_SOCKET_PAIR, 0};
    if (!proxy_test_send_zmp_frame (
          fd_, zlink::zmp_flag_control, hello, sizeof (hello)))
        return false;

    std::vector<unsigned char> ready;
    ready.push_back (zlink::zmp_control_ready);
    static const char socket_type[] = "PAIR";
    zlink::zmp_metadata::append_property (
      ready, "Socket-Type", socket_type, sizeof (socket_type) - 1);
    return proxy_test_send_zmp_frame (
      fd_, zlink::zmp_flag_control, &ready[0], ready.size ());
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
    char endpoint_source[256];
    snprintf (endpoint_source, 256 * sizeof (char), "inproc://endpoint%d", databag->id);
    TEST_ASSERT_SUCCESS_ERRNO (zlink_connect (endpoint, endpoint_source));
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
    void *threads[5];
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
        snprintf (endpoint_source, 256 * sizeof (char), "inproc://endpoint%d", i);
        TEST_ASSERT_SUCCESS_ERRNO (zlink_bind (endpoint_receivers[i], endpoint_source));
    }

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
    struct thread_data databags[QT_CLIENTS + 1];
    for (int i = 0; i < QT_CLIENTS; i++) {
        databags[i].id = i;
        threads[i] = zlink_thread_start (&client_task, &databags[i]);
    }
    threads[QT_CLIENTS] = zlink_thread_start (&server_task, NULL);
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
                       ZLINK_PART_MORE));

    zlink_msg_t tail;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&tail, 4));
    memcpy (zlink_msg_data (&tail), "tail", 4);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (source, &tail, ZLINK_SEND_FLAGS_NONE,
                       ZLINK_PART_FINAL));

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
                       ZLINK_PART_MORE));
    zlink_msg_t tail;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&tail, 4));
    memcpy (zlink_msg_data (&tail), "tail", 4);
    TEST_ASSERT_SUCCESS_ERRNO (
      reinterpret_cast<zlink::msg_t *> (&tail)
        ->set_request_reply_metadata (zlink::request_reply::reply_type, 45));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (source, &tail, ZLINK_SEND_FLAGS_NONE,
                       ZLINK_PART_FINAL));

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
        uint8_t message_type = 0xff;
        uint64_t request_seq = UINT64_MAX;
        zlink_msg_t part;
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&part));
        zlink_part_flag_t has_more = ZLINK_PART_FINAL;
        TEST_ASSERT_EQUAL_INT (
          ZLINK_RECV_NO_DATA,
          zlink_dealer_recv_part (
            receivers[i], &message_type, &request_seq, &part, &has_more,
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

void test_proxy_rolls_back_both_outputs_after_source_multipart_abort ()
{
    void *const context = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (context);
    void *const first_frontend = zlink_socket (context, ZLINK_SOCKET_PAIR);
    void *const second_frontend = zlink_socket (context, ZLINK_SOCKET_PAIR);
    void *const backend = zlink_socket (context, ZLINK_SOCKET_PAIR);
    void *const capture = zlink_socket (context, ZLINK_SOCKET_PAIR);
    void *const sink = zlink_socket (context, ZLINK_SOCKET_PAIR);
    void *const capture_sink = zlink_socket (context, ZLINK_SOCKET_PAIR);
    TEST_ASSERT_NOT_NULL (first_frontend);
    TEST_ASSERT_NOT_NULL (second_frontend);
    TEST_ASSERT_NOT_NULL (backend);
    TEST_ASSERT_NOT_NULL (capture);
    TEST_ASSERT_NOT_NULL (sink);
    TEST_ASSERT_NOT_NULL (capture_sink);

    const int zero = 0;
    void *const sockets[] = {first_frontend, second_frontend, backend,
                             capture, capture_sink, sink};
    for (size_t i = 0; i != sizeof (sockets) / sizeof (sockets[0]); ++i) {
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_set_option (sockets[i], ZLINK_OPT_LINGER, &zero,
                            sizeof (zero)));
    }

    zlink_socket_monitor_open_options_t monitor_options;
    memset (&monitor_options, 0, sizeof (monitor_options));
    monitor_options.events = ZLINK_EVENT_CONNECTION_READY;
    void *first_frontend_monitor =
      zlink_socket_monitor_open (first_frontend, &monitor_options);
    TEST_ASSERT_NOT_NULL (first_frontend_monitor);
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_set_option (first_frontend_monitor, ZLINK_OPT_LINGER, &zero,
                        sizeof (zero)));

    char first_endpoint[MAX_SOCKET_STRING];
    char second_endpoint[MAX_SOCKET_STRING];
    bind_loopback_ipv4 (
      first_frontend, first_endpoint, sizeof (first_endpoint));
    bind_loopback_ipv4 (
      second_frontend, second_endpoint, sizeof (second_endpoint));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (backend, "inproc://proxy-abort-backend"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (capture, "inproc://proxy-abort-capture"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (sink, "inproc://proxy-abort-backend"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (capture_sink, "inproc://proxy-abort-capture"));

    proxy_thread_data first_proxy (
      first_frontend, backend, capture);
    std::atomic<bool> orphan_forwarded (false);
    proxy_part_forwarded_hook_scope_t forwarded_hook_scope (
      &mark_proxy_part_forwarded, &orphan_forwarded);
    void *const first_proxy_thread =
      zlink_thread_start (&metadata_proxy_task, &first_proxy);
    TEST_ASSERT_NOT_NULL (first_proxy_thread);

    fd_t first_raw = connect_socket (
      first_endpoint, AF_INET, IPPROTO_TCP);
    const bool raw_connected = first_raw != retired_fd;
    const bool handshake_sent =
      raw_connected && proxy_test_send_pair_handshake (first_raw);
    const int ready_event =
      handshake_sent
        ? get_monitor_event_with_timeout (
            first_frontend_monitor, NULL, NULL, 5000)
        : -1;
    const zlink_close_result_t monitor_close_result =
      zlink_monitor_close (&first_frontend_monitor);
    static const unsigned char orphan[] = {'o', 'r', 'p', 'h', 'a', 'n'};
    const bool orphan_sent =
      monitor_close_result == ZLINK_CLOSE_OK
      && ready_event == ZLINK_EVENT_CONNECTION_READY
      && proxy_test_send_zmp_frame (
        first_raw, zlink::zmp_flag_more, orphan, sizeof (orphan));
    for (int attempt = 0;
         attempt != 1000
         && !orphan_forwarded.load (std::memory_order_acquire)
         && !first_proxy.done.load (std::memory_order_acquire);
         ++attempt)
        msleep (1);
    if (!orphan_sent
        || !orphan_forwarded.load (std::memory_order_acquire)) {
        if (raw_connected)
            close (first_raw);
        (void) zlink_ctx_shutdown (context);
        zlink_thread_join (first_proxy_thread);
        forwarded_hook_scope.reset ();
        const zlink_config_result_t proxy_result = first_proxy.result;
        const int proxy_error = first_proxy.error;
        for (size_t i = 0; i != sizeof (sockets) / sizeof (sockets[0]); ++i)
            (void) zlink_close (sockets[i]);
        (void) zlink_ctx_term (context);

        char failure[256];
        snprintf (failure, sizeof (failure),
                  "proxy prefix staging failed: handshake=%d ready_event=%d "
                  "monitor_close=%d orphan_sent=%d proxy_result=%d "
                  "proxy_errno=%d",
                  handshake_sent ? 1 : 0, ready_event,
                  static_cast<int> (monitor_close_result), orphan_sent ? 1 : 0,
                  static_cast<int> (proxy_result), proxy_error);
        TEST_FAIL_MESSAGE (failure);
    }
    close (first_raw);

    TEST_ASSERT_TRUE_MESSAGE (
      wait_for_proxy_exit (&first_proxy),
      "proxy did not observe the source multipart abort");
    zlink_thread_join (first_proxy_thread);
    forwarded_hook_scope.reset ();
    TEST_ASSERT_EQUAL_INT (ZLINK_CONFIG_INTERNAL_ERROR, first_proxy.result);
    TEST_ASSERT_EQUAL_INT (EAGAIN, first_proxy.error);

    // Reuse the same backend/capture destinations with a fresh source. If the
    // first forwarding attempt left its MORE prefixes staged, this final frame
    // is exposed as the tail of [orphan, fresh] on one or both outputs.
    proxy_thread_data second_proxy (
      second_frontend, backend, capture);
    void *const second_proxy_thread =
      zlink_thread_start (&metadata_proxy_task, &second_proxy);
    TEST_ASSERT_NOT_NULL (second_proxy_thread);

    fd_t second_raw = connect_socket (
      second_endpoint, AF_INET, IPPROTO_TCP);
    TEST_ASSERT_NOT_EQUAL (retired_fd, second_raw);
    TEST_ASSERT_TRUE (proxy_test_send_pair_handshake (second_raw));
    msleep (SETTLE_TIME);
    static const unsigned char fresh[] = {'f', 'r', 'e', 's', 'h'};
    TEST_ASSERT_TRUE (proxy_test_send_zmp_frame (
      second_raw, 0, fresh, sizeof (fresh)));

    assert_raw_pair_part (sink, "fresh");
    assert_raw_pair_part (capture_sink, "fresh");

    TEST_ASSERT_SUCCESS_ERRNO (zlink_ctx_shutdown (context));
    zlink_thread_join (second_proxy_thread);
    close (second_raw);
    TEST_ASSERT_TRUE (
      second_proxy.result == ZLINK_CONFIG_OK
      || (second_proxy.result == ZLINK_CONFIG_INTERNAL_ERROR
          && second_proxy.error == ETERM));
    for (size_t i = 0; i != sizeof (sockets) / sizeof (sockets[0]); ++i)
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
                       ZLINK_PART_MORE));
    zlink_msg_t orphan_tail;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&orphan_tail, 4));
    memcpy (zlink_msg_data (&orphan_tail), "tail", 4);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (first_source, &orphan_tail, ZLINK_SEND_FLAGS_NONE,
                       ZLINK_PART_FINAL));

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
                       ZLINK_PART_FINAL));

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

void test_proxy_does_not_bridge_request_completion_lane ()
{
    void *const context = zlink_ctx_new ();
    TEST_ASSERT_NOT_NULL (context);
    void *const frontend = zlink_socket (context, ZLINK_SOCKET_DEALER);
    void *const backend = zlink_socket (context, ZLINK_SOCKET_DEALER);
    void *const source = zlink_socket (context, ZLINK_SOCKET_DEALER);
    void *const far_side = zlink_socket (context, ZLINK_SOCKET_DEALER);
    TEST_ASSERT_NOT_NULL (frontend);
    TEST_ASSERT_NOT_NULL (backend);
    TEST_ASSERT_NOT_NULL (source);
    TEST_ASSERT_NOT_NULL (far_side);

    const int zero = 0;
    void *const sockets[] = {frontend, backend, source, far_side};
    for (size_t i = 0; i < sizeof (sockets) / sizeof (sockets[0]); ++i) {
        TEST_ASSERT_SUCCESS_ERRNO (
          zlink_set_option (sockets[i], ZLINK_OPT_LINGER, &zero,
                            sizeof (zero)));
    }
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (frontend, "inproc://proxy-no-completion-frontend"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_bind (backend, "inproc://proxy-no-completion-backend"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (source, "inproc://proxy-no-completion-frontend"));
    TEST_ASSERT_SUCCESS_ERRNO (
      zlink_connect (far_side, "inproc://proxy-no-completion-backend"));

    proxy_thread_data proxy_data (frontend, backend, NULL);
    void *const proxy_thread =
      zlink_thread_start (&metadata_proxy_task, &proxy_data);
    TEST_ASSERT_NOT_NULL (proxy_thread);
    msleep (SETTLE_TIME);

    proxy_request_probe_t completion;
    zlink_msg_t request;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&request, 7));
    memcpy (zlink_msg_data (&request), "request", 7);
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_dealer_request (source, &request, 1,
                            &capture_proxy_request_completion, &completion,
                            ZLINK_SEND_FLAGS_NONE, 150));

    const std::shared_ptr<
      zlink::socket_reqrep_internal::socket_request_reply_state_t>
      source_state =
        zlink::socket_reqrep_internal::find_request_reply_state (
          as_socket_handle (source));
    TEST_ASSERT_NOT_NULL (source_state.get ());
    uint64_t wire_sequence = 0;
    {
        std::lock_guard<std::mutex> lock (source_state->mutex);
        TEST_ASSERT_EQUAL_UINT64 (1, source_state->pending_requests.size ());
        wire_sequence = source_state->pending_requests.begin ()->second.identity.request_seq;
    }
    TEST_ASSERT_TRUE (wire_sequence != 0);

    assert_raw_dealer_part (far_side, "request", ZLINK_PART_FINAL);
    zlink_msg_t fake_reply;
    TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init_size (&fake_reply, 10));
    memcpy (zlink_msg_data (&fake_reply), "fake-reply", 10);
    TEST_ASSERT_SUCCESS_ERRNO (
      reinterpret_cast<zlink::msg_t *> (&fake_reply)
        ->set_request_reply_metadata (zlink::request_reply::reply_type,
                                      wire_sequence));
    TEST_ASSERT_EQUAL_INT (
      ZLINK_SUBMIT_OK,
      zlink_send_part (far_side, &fake_reply, ZLINK_SEND_FLAGS_NONE,
                       ZLINK_PART_FINAL));

    // The exact sequence returns only as ordinary application data. The
    // original request remains pending because a proxy never creates or
    // forwards the paired Completion lane.
    assert_raw_dealer_part (source, "fake-reply", ZLINK_PART_FINAL);
    TEST_ASSERT_FALSE (completion.done.load (std::memory_order_acquire));

    for (int attempt = 0;
         attempt < 1000
         && !completion.done.load (std::memory_order_acquire);
         ++attempt) {
        uint8_t message_type = 0;
        uint64_t request_seq = 0;
        zlink_part_flag_t has_more = ZLINK_PART_FINAL;
        zlink_msg_t part;
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_init (&part));
        (void) zlink_dealer_recv_part (
          source, &message_type, &request_seq, &part, &has_more,
          ZLINK_RECV_FLAGS_DONTWAIT);
        TEST_ASSERT_SUCCESS_ERRNO (zlink_msg_close (&part));
        msleep (1);
    }
    TEST_ASSERT_TRUE (completion.done.load (std::memory_order_acquire));
    TEST_ASSERT_EQUAL_INT (ZLINK_REQUEST_TIMED_OUT, completion.result);
    TEST_ASSERT_EQUAL_UINT64 (1, completion.callback_count);
    TEST_ASSERT_EQUAL_UINT64 (0, completion.part_count);

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

int main (void)
{
    setup_test_environment (360);

    UNITY_BEGIN ();
    RUN_TEST (test_proxy_and_capture_clear_request_reply_metadata);
    RUN_TEST (test_proxy_rejects_request_reply_metadata_after_first_part);
    RUN_TEST (test_proxy_rolls_back_both_outputs_after_source_multipart_abort);
    RUN_TEST (test_proxy_rolls_back_capture_after_destination_send_failure);
    RUN_TEST (test_proxy_does_not_bridge_request_completion_lane);
    RUN_TEST (test_proxy);
    return UNITY_END ();
}
