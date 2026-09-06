#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <zlink.h>

#define CHECK(expr)                                                                                \
    do {                                                                                           \
        if (!(expr))                                                                               \
            return __LINE__;                                                                       \
    } while (0)

enum
{
    PAYLOAD_SIZE = 64,
    MAX_FILL_ATTEMPTS = 512,
    REPEAT_COUNT = 5,
    WAIT_MS = 5000
};

static int init_part (zlink_msg_t *part, const void *data, size_t size)
{
    CHECK (zlink_msg_init_size (part, size) == ZLINK_CONFIG_OK);
    if (size != 0)
        memcpy (zlink_msg_data (part), data, size);
    return 0;
}

static int check_part_consumed (zlink_msg_t *part)
{
    CHECK (zlink_msg_size (part) == 0);
    CHECK (zlink_msg_close (part) == ZLINK_CONFIG_OK);
    return 0;
}

static int configure_socket (void *socket, int small_hwm)
{
    const int zero = 0;
    const int receive_timeout = WAIT_MS;
    CHECK (zlink_set_option (socket, ZLINK_OPT_LINGER, &zero, sizeof (zero)) == ZLINK_CONFIG_OK);
    CHECK (zlink_set_option (socket, ZLINK_OPT_RCVTIMEO, &receive_timeout,
                             sizeof (receive_timeout)) == ZLINK_CONFIG_OK);
    if (small_hwm) {
        const uint64_t hwm = 4u * ((uint64_t) PAYLOAD_SIZE + sizeof (zlink_msg_t));
        CHECK (zlink_set_option (socket, ZLINK_OPT_SNDHWM, &hwm, sizeof (hwm))
               == ZLINK_CONFIG_OK);
        CHECK (zlink_set_option (socket, ZLINK_OPT_RCVHWM, &hwm, sizeof (hwm))
               == ZLINK_CONFIG_OK);
    }
    return 0;
}

static int make_endpoint (char *endpoint, size_t capacity, const char *scenario, int iteration)
{
    const int count = snprintf (endpoint, capacity, "inproc://c-request-%s-%d", scenario,
                                iteration);
    CHECK (count > 0);
    CHECK ((size_t) count < capacity);
    return 0;
}

static int make_rid (zlink_routing_id_t *rid, const char *text)
{
    const size_t size = strlen (text);
    CHECK (size <= sizeof (rid->data));
    memset (rid, 0, sizeof (*rid));
    rid->size = (uint8_t) size;
    memcpy (rid->data, text, size);
    return 0;
}

static int check_rid (const zlink_routing_id_t *actual, const zlink_routing_id_t *expected)
{
    CHECK (actual->size == expected->size);
    CHECK (memcmp (actual->data, expected->data, expected->size) == 0);
    return 0;
}

static int check_no_completion (void *socket)
{
    zlink_completion_t completion;
    memset (&completion, 0, sizeof (completion));
    completion.struct_size = sizeof (completion);
    errno = 0;
    CHECK (zlink_completion_recv (socket, &completion, ZLINK_RECV_FLAGS_DONTWAIT)
           == ZLINK_RECV_NO_DATA);
    CHECK (zlink_errno () == EAGAIN);
    zlink_completion_close (&completion);
    return 0;
}

static int wait_for_completion (void *poller, void *socket, void *poller_context)
{
    zlink_poller_event_t event;
    zlink_config_result_t error = ZLINK_CONFIG_INTERNAL_ERROR;
    memset (&event, 0, sizeof (event));
    CHECK (zlink_poller_wait (poller, &event, 1, WAIT_MS, &error) == 1);
    CHECK (error == ZLINK_CONFIG_OK);
    CHECK (event.source_kind == ZLINK_POLLER_SOURCE_SOCKET);
    CHECK (event.socket == socket);
    CHECK (event.user_data == poller_context);
    CHECK ((event.events & ZLINK_POLLCOMPLETION) != 0);
    return 0;
}

static int receive_completion (void *socket, zlink_completion_t *completion)
{
    memset (completion, 0, sizeof (*completion));
    completion->struct_size = sizeof (*completion);
    CHECK (zlink_completion_recv (socket, completion, ZLINK_RECV_FLAGS_DONTWAIT)
           == ZLINK_RECV_OK);
    return 0;
}

static int receive_request (void *router,
                            zlink_routing_id_t *source_out,
                            zlink_reply_token_t *token_out,
                            void *payload_out,
                            size_t payload_capacity,
                            size_t *payload_size_out)
{
    const zlink_routing_id_t *source = NULL;
    zlink_msg_t part;
    zlink_part_flag_t flag = ZLINK_PART_MORE;
    CHECK (zlink_msg_init (&part) == ZLINK_CONFIG_OK);
    CHECK (zlink_router_recv_part (router, &source, token_out, &part, &flag,
                                   ZLINK_RECV_FLAGS_NONE)
           == ZLINK_RECV_OK);
    CHECK (source != NULL);
    CHECK (flag == ZLINK_PART_FINAL);
    CHECK (zlink_msg_size (&part) <= payload_capacity);
    *source_out = *source;
    *payload_size_out = zlink_msg_size (&part);
    if (*payload_size_out != 0)
        memcpy (payload_out, zlink_msg_data (&part), *payload_size_out);
    CHECK (zlink_msg_close (&part) == ZLINK_CONFIG_OK);
    return 0;
}

static int reply_request (void *router,
                          const zlink_routing_id_t *source,
                          zlink_reply_token_t token,
                          const void *payload,
                          size_t payload_size)
{
    zlink_msg_t reply;
    CHECK (init_part (&reply, payload, payload_size) == 0);
    CHECK (zlink_reply_part (router, source, token, &reply, ZLINK_PART_FINAL)
           == ZLINK_SUBMIT_OK);
    CHECK (check_part_consumed (&reply) == 0);
    return 0;
}

static int prime_route (void *sender, void *receiver, const zlink_routing_id_t *target)
{
    static const char payload[] = "route-prime";
    unsigned char received[PAYLOAD_SIZE];
    size_t received_size = 0;
    zlink_routing_id_t source;
    zlink_reply_token_t token = UINT64_MAX;
    zlink_completion_id_t id = UINT64_MAX;
    zlink_msg_t part;
    CHECK (init_part (&part, payload, sizeof (payload) - 1) == 0);
    const zlink_submit_result_t result =
      target ? zlink_send_part_rid (sender, target, &part, ZLINK_SEND_FLAGS_NONE,
                                    ZLINK_PART_FINAL, NULL, &id)
             : zlink_send_part (sender, &part, ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL, NULL,
                                &id);
    CHECK (result == ZLINK_SUBMIT_OK);
    CHECK (id == 0);
    CHECK (check_part_consumed (&part) == 0);
    CHECK (receive_request (receiver, &source, &token, received, sizeof (received),
                            &received_size)
           == 0);
    CHECK (token == 0);
    CHECK (received_size == sizeof (payload) - 1);
    CHECK (memcmp (received, payload, received_size) == 0);
    return 0;
}

static int fill_requests (void *sender,
                          const zlink_routing_id_t *target,
                          const void *payload,
                          void *context,
                          size_t *accepted_out,
                          zlink_completion_id_t *wait_token_out)
{
    size_t attempt;
    for (attempt = 0; attempt != MAX_FILL_ATTEMPTS; ++attempt) {
        zlink_msg_t part;
        zlink_completion_id_t id = 0;
        CHECK (init_part (&part, payload, PAYLOAD_SIZE) == 0);
        errno = 0;
        const zlink_submit_result_t result = zlink_request_part (
          sender, target, &part, ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL, 120000,
          context, &id);
        CHECK (check_part_consumed (&part) == 0);
        if (result == ZLINK_SUBMIT_BACKPRESSURED) {
            CHECK (zlink_errno () == EAGAIN);
            CHECK (id != 0);
            CHECK (attempt != 0);
            *accepted_out = attempt;
            *wait_token_out = id;
            return 0;
        }
        CHECK (result == ZLINK_SUBMIT_OK);
        CHECK (id != 0);
    }
    return __LINE__;
}

static int test_hwm_retry (int iteration)
{
    char endpoint[96];
    unsigned char logical_payload[PAYLOAD_SIZE];
    unsigned char received[PAYLOAD_SIZE];
    size_t received_size = 0;
    zlink_routing_id_t source;
    zlink_reply_token_t reply_token = 0;
    memset (logical_payload, 'q', sizeof (logical_payload));
    memcpy (logical_payload, "request-writable-exact-retry",
            strlen ("request-writable-exact-retry"));
    CHECK (make_endpoint (endpoint, sizeof (endpoint), "hwm", iteration) == 0);

    void *ctx = zlink_ctx_new ();
    CHECK (ctx != NULL);
    void *router = zlink_socket (ctx, ZLINK_SOCKET_ROUTER);
    void *dealer = zlink_socket (ctx, ZLINK_SOCKET_DEALER);
    CHECK (router != NULL);
    CHECK (dealer != NULL);
    CHECK (configure_socket (router, 1) == 0);
    CHECK (configure_socket (dealer, 1) == 0);
    CHECK (zlink_bind (router, endpoint) == ZLINK_BIND_OK);
    CHECK (zlink_connect (dealer, endpoint) == ZLINK_CONNECT_OK);
    CHECK (prime_route (dealer, router, NULL) == 0);

    int poller_context = 101;
    void *poller = zlink_poller_new ();
    CHECK (poller != NULL);
    CHECK (zlink_poller_add (poller, dealer, &poller_context,
                             (short) (ZLINK_POLLOUT | ZLINK_POLLCOMPLETION))
           == ZLINK_CONFIG_OK);

    int request_context = 102;
    size_t accepted = 0;
    zlink_completion_id_t wait_token = 0;
    CHECK (fill_requests (dealer, NULL, logical_payload, &request_context, &accepted,
                          &wait_token)
           == 0);
    CHECK (check_no_completion (dealer) == 0);

    size_t index;
    for (index = 0; index != accepted; ++index) {
        CHECK (receive_request (router, &source, &reply_token, received, sizeof (received),
                                &received_size)
               == 0);
        CHECK (reply_token != 0);
        CHECK (received_size == sizeof (logical_payload));
        CHECK (memcmp (received, logical_payload, received_size) == 0);
        CHECK (reply_request (router, &source, reply_token, "filler-reply", 12) == 0);
    }

    size_t completed_fillers = 0;
    int saw_writable = 0;
    while (completed_fillers != accepted || !saw_writable) {
        CHECK (wait_for_completion (poller, dealer, &poller_context) == 0);
        zlink_completion_t completion;
        CHECK (receive_completion (dealer, &completion) == 0);
        if (completion.kind == ZLINK_COMPLETION_WRITABLE) {
            CHECK (!saw_writable);
            CHECK (completion.completion_id == wait_token);
            CHECK (completion.user_context == &request_context);
            CHECK (completion.peer_rid.size == 0);
            CHECK (completion.send_result == ZLINK_SEND_ADMITTED);
            CHECK (completion.send_terminal_errno == 0);
            saw_writable = 1;
        } else {
            CHECK (completion.kind == ZLINK_COMPLETION_REQUEST);
            CHECK (completion.completion_id != 0);
            CHECK (completion.completion_id != wait_token);
            CHECK (completion.user_context == &request_context);
            CHECK (completion.request_result == ZLINK_REQUEST_OK);
            CHECK (completion.reply_part_count == 1);
            CHECK (zlink_msg_size (&completion.reply_parts[0]) == 12);
            CHECK (memcmp (zlink_msg_data (&completion.reply_parts[0]), "filler-reply", 12)
                   == 0);
            ++completed_fillers;
        }
        zlink_completion_close (&completion);
    }
    CHECK (check_no_completion (dealer) == 0);

    zlink_msg_t retry;
    zlink_completion_id_t request_id = 0;
    CHECK (init_part (&retry, logical_payload, sizeof (logical_payload)) == 0);
    CHECK (zlink_request_part (dealer, NULL, &retry, ZLINK_SEND_FLAGS_DONTWAIT,
                               ZLINK_PART_FINAL, 120000, &request_context, &request_id)
           == ZLINK_SUBMIT_OK);
    CHECK (request_id != 0);
    CHECK (request_id != wait_token);
    CHECK (check_part_consumed (&retry) == 0);
    CHECK (receive_request (router, &source, &reply_token, received, sizeof (received),
                            &received_size)
           == 0);
    CHECK (received_size == sizeof (logical_payload));
    CHECK (memcmp (received, logical_payload, received_size) == 0);
    CHECK (reply_request (router, &source, reply_token, "reply", 5) == 0);

    CHECK (wait_for_completion (poller, dealer, &poller_context) == 0);
    zlink_completion_t retry_completion;
    CHECK (receive_completion (dealer, &retry_completion) == 0);
    CHECK (retry_completion.kind == ZLINK_COMPLETION_REQUEST);
    CHECK (retry_completion.completion_id == request_id);
    CHECK (retry_completion.user_context == &request_context);
    CHECK (retry_completion.request_result == ZLINK_REQUEST_OK);
    CHECK (retry_completion.reply_part_count == 1);
    CHECK (zlink_msg_size (&retry_completion.reply_parts[0]) == 5);
    CHECK (memcmp (zlink_msg_data (&retry_completion.reply_parts[0]), "reply", 5) == 0);
    zlink_completion_close (&retry_completion);
    CHECK (check_no_completion (dealer) == 0);

    CHECK (zlink_poller_destroy (&poller) == ZLINK_CLOSE_OK);
    CHECK (zlink_close (dealer) == ZLINK_CLOSE_OK);
    CHECK (zlink_close (router) == ZLINK_CLOSE_OK);
    CHECK (zlink_ctx_term (ctx) == ZLINK_CLOSE_OK);
    return 0;
}

static int test_connect_before_bind_and_mixed_tokens (int iteration)
{
    char endpoint[96];
    CHECK (make_endpoint (endpoint, sizeof (endpoint), "connect", iteration) == 0);
    void *ctx = zlink_ctx_new ();
    CHECK (ctx != NULL);
    void *dealer = zlink_socket (ctx, ZLINK_SOCKET_DEALER);
    void *router = zlink_socket (ctx, ZLINK_SOCKET_ROUTER);
    CHECK (dealer != NULL);
    CHECK (router != NULL);
    CHECK (configure_socket (dealer, 0) == 0);
    CHECK (configure_socket (router, 0) == 0);
    const int immediate = 1;
    const uint64_t one = 1;
    CHECK (zlink_set_option (dealer, ZLINK_OPT_IMMEDIATE, &immediate, sizeof (immediate))
           == ZLINK_CONFIG_OK);
    CHECK (zlink_set_option (dealer, ZLINK_OPT_PENDING_MAX_MSGS, &one, sizeof (one))
           == ZLINK_CONFIG_OK);
    CHECK (zlink_set_option (dealer, ZLINK_OPT_PENDING_MAX_BYTES, &one, sizeof (one))
           == ZLINK_CONFIG_OK);
    uint64_t stored = 0;
    size_t stored_size = sizeof (stored);
    CHECK (zlink_get_option (dealer, ZLINK_OPT_PENDING_MAX_MSGS, &stored, &stored_size)
           == ZLINK_CONFIG_OK);
    CHECK (stored == one);
    CHECK (stored_size == sizeof (stored));
    CHECK (zlink_connect (dealer, endpoint) == ZLINK_CONNECT_OK);

    int request_context = 201;
    zlink_completion_id_t request_wait_token = 0;
    zlink_msg_t request;
    CHECK (init_part (&request, "connect-before-bind-request", 27) == 0);
    errno = 0;
    CHECK (zlink_request_part (dealer, NULL, &request, ZLINK_SEND_FLAGS_DONTWAIT,
                               ZLINK_PART_FINAL, 1, &request_context, &request_wait_token)
           == ZLINK_SUBMIT_BACKPRESSURED);
    CHECK (zlink_errno () == EAGAIN);
    CHECK (request_wait_token != 0);
    CHECK (check_part_consumed (&request) == 0);

    int send_context = 202;
    zlink_completion_id_t send_wait_token = 0;
    zlink_msg_t send;
    CHECK (init_part (&send, "mixed-send", 10) == 0);
    errno = 0;
    CHECK (zlink_send_part (dealer, &send, ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL,
                            &send_context, &send_wait_token)
           == ZLINK_SUBMIT_BACKPRESSURED);
    CHECK (zlink_errno () == EAGAIN);
    CHECK (send_wait_token != 0);
    CHECK (send_wait_token != request_wait_token);
    CHECK (check_part_consumed (&send) == 0);
    CHECK (check_no_completion (dealer) == 0);

    int poller_context = 203;
    void *poller = zlink_poller_new ();
    CHECK (poller != NULL);
    CHECK (zlink_poller_add (poller, dealer, &poller_context,
                             (short) (ZLINK_POLLOUT | ZLINK_POLLCOMPLETION))
           == ZLINK_CONFIG_OK);
    CHECK (zlink_bind (router, endpoint) == ZLINK_BIND_OK);
    CHECK (wait_for_completion (poller, dealer, &poller_context) == 0);

    int saw_request = 0;
    int saw_send = 0;
    int index;
    for (index = 0; index != 2; ++index) {
        zlink_completion_t writable;
        CHECK (receive_completion (dealer, &writable) == 0);
        CHECK (writable.kind == ZLINK_COMPLETION_WRITABLE);
        CHECK (writable.send_result == ZLINK_SEND_ADMITTED);
        CHECK (writable.send_terminal_errno == 0);
        CHECK (writable.peer_rid.size == 0);
        if (writable.completion_id == request_wait_token) {
            CHECK (writable.user_context == &request_context);
            saw_request = 1;
        } else {
            CHECK (writable.completion_id == send_wait_token);
            CHECK (writable.user_context == &send_context);
            saw_send = 1;
        }
        zlink_completion_close (&writable);
    }
    CHECK (saw_request);
    CHECK (saw_send);
    CHECK (check_no_completion (dealer) == 0);

    zlink_completion_id_t request_id = 0;
    CHECK (init_part (&request, "connect-before-bind-request", 27) == 0);
    CHECK (zlink_request_part (dealer, NULL, &request, ZLINK_SEND_FLAGS_DONTWAIT,
                               ZLINK_PART_FINAL, WAIT_MS, &request_context, &request_id)
           == ZLINK_SUBMIT_OK);
    CHECK (request_id != 0);
    CHECK (check_part_consumed (&request) == 0);

    unsigned char received[64];
    size_t received_size = 0;
    zlink_routing_id_t source;
    zlink_reply_token_t reply_token = 0;
    CHECK (receive_request (router, &source, &reply_token, received, sizeof (received),
                            &received_size)
           == 0);
    CHECK (received_size == 27);
    CHECK (memcmp (received, "connect-before-bind-request", 27) == 0);
    CHECK (reply_request (router, &source, reply_token, "mixed-reply", 11) == 0);
    CHECK (wait_for_completion (poller, dealer, &poller_context) == 0);
    zlink_completion_t completion;
    CHECK (receive_completion (dealer, &completion) == 0);
    CHECK (completion.kind == ZLINK_COMPLETION_REQUEST);
    CHECK (completion.completion_id == request_id);
    CHECK (completion.user_context == &request_context);
    CHECK (completion.request_result == ZLINK_REQUEST_OK);
    zlink_completion_close (&completion);
    CHECK (check_no_completion (dealer) == 0);

    CHECK (zlink_poller_destroy (&poller) == ZLINK_CLOSE_OK);
    CHECK (zlink_close (dealer) == ZLINK_CLOSE_OK);
    CHECK (zlink_close (router) == ZLINK_CLOSE_OK);
    CHECK (zlink_ctx_term (ctx) == ZLINK_CLOSE_OK);
    return 0;
}

static int test_close_reclaims_token (int iteration)
{
    char endpoint[96];
    CHECK (make_endpoint (endpoint, sizeof (endpoint), "close", iteration) == 0);
    void *ctx = zlink_ctx_new ();
    CHECK (ctx != NULL);
    void *dealer = zlink_socket (ctx, ZLINK_SOCKET_DEALER);
    CHECK (dealer != NULL);
    CHECK (configure_socket (dealer, 0) == 0);
    const int immediate = 1;
    CHECK (zlink_set_option (dealer, ZLINK_OPT_IMMEDIATE, &immediate, sizeof (immediate))
           == ZLINK_CONFIG_OK);
    CHECK (zlink_connect (dealer, endpoint) == ZLINK_CONNECT_OK);
    int request_context = 301;
    zlink_completion_id_t wait_token = 0;
    zlink_msg_t request;
    CHECK (init_part (&request, "close-token", 11) == 0);
    CHECK (zlink_request_part (dealer, NULL, &request, ZLINK_SEND_FLAGS_DONTWAIT,
                               ZLINK_PART_FINAL, 1, &request_context, &wait_token)
           == ZLINK_SUBMIT_BACKPRESSURED);
    CHECK (wait_token != 0);
    CHECK (check_part_consumed (&request) == 0);
    CHECK (zlink_close (dealer) == ZLINK_CLOSE_OK);
    CHECK (zlink_ctx_term (ctx) == ZLINK_CLOSE_OK);
    return 0;
}

static int test_terminal_writable (int iteration)
{
    char endpoint[96];
    char server_name[48];
    CHECK (make_endpoint (endpoint, sizeof (endpoint), "terminal", iteration) == 0);
    CHECK (snprintf (server_name, sizeof (server_name), "c-request-server-%d", iteration) > 0);
    zlink_routing_id_t target;
    CHECK (make_rid (&target, server_name) == 0);

    void *ctx = zlink_ctx_new ();
    CHECK (ctx != NULL);
    void *server = zlink_socket (ctx, ZLINK_SOCKET_ROUTER);
    void *client = zlink_socket (ctx, ZLINK_SOCKET_ROUTER);
    CHECK (server != NULL);
    CHECK (client != NULL);
    CHECK (configure_socket (server, 1) == 0);
    CHECK (configure_socket (client, 1) == 0);
    CHECK (zlink_set_routing_id (server, server_name, strlen (server_name)) == ZLINK_CONFIG_OK);
    CHECK (zlink_set_router_option (client, ZLINK_ROUTER_OPT_CONNECT_ROUTING_ID,
                                    server_name, strlen (server_name)) == ZLINK_CONFIG_OK);
    CHECK (zlink_bind (server, endpoint) == ZLINK_BIND_OK);
    CHECK (zlink_connect (client, endpoint) == ZLINK_CONNECT_OK);
    CHECK (prime_route (client, server, &target) == 0);

    unsigned char payload[PAYLOAD_SIZE];
    memset (payload, 't', sizeof (payload));
    int request_context = 401;
    size_t accepted = 0;
    zlink_completion_id_t wait_token = 0;
    CHECK (fill_requests (client, &target, payload, &request_context, &accepted, &wait_token)
           == 0);
    CHECK (accepted != 0);
    CHECK (zlink_disconnect_rid (client, &target) == ZLINK_CONNECT_OK);

    int poller_context = 402;
    void *poller = zlink_poller_new ();
    CHECK (poller != NULL);
    CHECK (zlink_poller_add (poller, client, &poller_context, ZLINK_POLLCOMPLETION)
           == ZLINK_CONFIG_OK);
    /* Explicit removal ends everything queued for the target (socket README
       completion table): the wait token gets its terminal WRITABLE and every
       accepted REQUEST completes with NOT_FOUND. The order is not fixed. */
    size_t terminals = 0;
    size_t not_found = 0;
    while (terminals + not_found < accepted + 1) {
        CHECK (wait_for_completion (poller, client, &poller_context) == 0);
        zlink_completion_t completion;
        while (receive_completion (client, &completion) == 0) {
            if (completion.kind == ZLINK_COMPLETION_WRITABLE) {
                CHECK (completion.completion_id == wait_token);
                CHECK (completion.user_context == &request_context);
                CHECK (completion.send_result == ZLINK_SEND_TERMINAL);
                CHECK (completion.send_terminal_errno == ENOENT);
                CHECK (check_rid (&completion.peer_rid, &target) == 0);
                ++terminals;
            } else {
                CHECK (completion.kind == ZLINK_COMPLETION_REQUEST);
                CHECK (completion.completion_id != wait_token);
                CHECK (completion.user_context == &request_context);
                CHECK (completion.request_result == ZLINK_REQUEST_NOT_FOUND);
                CHECK (check_rid (&completion.peer_rid, &target) == 0);
                ++not_found;
            }
            zlink_completion_close (&completion);
        }
    }
    CHECK (terminals == 1);
    CHECK (not_found == accepted);
    CHECK (check_no_completion (client) == 0);

    CHECK (zlink_poller_destroy (&poller) == ZLINK_CLOSE_OK);
    CHECK (zlink_close (client) == ZLINK_CLOSE_OK);
    CHECK (zlink_close (server) == ZLINK_CLOSE_OK);
    CHECK (zlink_ctx_term (ctx) == ZLINK_CLOSE_OK);
    return 0;
}

int main (void)
{
    int iteration;
    for (iteration = 0; iteration != REPEAT_COUNT; ++iteration) {
        CHECK (test_hwm_retry (iteration) == 0);
        CHECK (test_connect_before_bind_and_mixed_tokens (iteration) == 0);
        CHECK (test_close_reclaims_token (iteration) == 0);
        CHECK (test_terminal_writable (iteration) == 0);
    }
    return 0;
}
