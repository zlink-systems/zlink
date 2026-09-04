#include <errno.h>
#include <stdint.h>
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
    MAX_FILL_ATTEMPTS = 512
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

static int configure_small_hwm (void *socket)
{
    const uint64_t hwm = 4u * ((uint64_t) PAYLOAD_SIZE + sizeof (zlink_msg_t));
    CHECK (zlink_set_option (socket, ZLINK_OPT_SNDHWM, &hwm, sizeof (hwm)) == ZLINK_CONFIG_OK);
    CHECK (zlink_set_option (socket, ZLINK_OPT_RCVHWM, &hwm, sizeof (hwm)) == ZLINK_CONFIG_OK);
    return 0;
}

static int wait_socket (void *socket, short events)
{
    zlink_pollitem_t item = {socket, 0, events, 0};
    zlink_config_result_t error = ZLINK_CONFIG_INTERNAL_ERROR;
    CHECK (zlink_poll (&item, 1, 5000, &error) == 1);
    CHECK (error == ZLINK_CONFIG_OK);
    CHECK ((item.revents & events) == events);
    return 0;
}

static int receive_part (void *dealer, const void *expected, size_t expected_size)
{
    CHECK (wait_socket (dealer, ZLINK_POLLIN) == 0);

    zlink_msg_t part;
    zlink_part_flag_t part_flag = ZLINK_PART_MORE;
    CHECK (zlink_msg_init (&part) == ZLINK_CONFIG_OK);
    CHECK (zlink_recv_part (dealer, NULL, &part, &part_flag, ZLINK_RECV_FLAGS_DONTWAIT)
           == ZLINK_RECV_OK);
    CHECK (part_flag == ZLINK_PART_FINAL);
    CHECK (zlink_msg_size (&part) == expected_size);
    CHECK (memcmp (zlink_msg_data (&part), expected, expected_size) == 0);
    CHECK (zlink_msg_close (&part) == ZLINK_CONFIG_OK);
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
    CHECK (errno == EAGAIN);
    CHECK (zlink_errno () == EAGAIN);
    zlink_completion_close (&completion);
    return 0;
}

static int fill_until_backpressured (void *router,
                                     const zlink_routing_id_t *target,
                                     const void *payload,
                                     size_t payload_size,
                                     void *user_context,
                                     size_t *accepted_out,
                                     zlink_completion_id_t *wait_token_out)
{
    size_t attempt;
    for (attempt = 0; attempt != MAX_FILL_ATTEMPTS; ++attempt) {
        zlink_msg_t part;
        zlink_completion_id_t completion_id = UINT64_MAX;
        CHECK (init_part (&part, payload, payload_size) == 0);

        errno = 0;
        const zlink_submit_result_t result =
          zlink_send_part_rid (router, target, &part, ZLINK_SEND_FLAGS_DONTWAIT,
                               ZLINK_PART_FINAL, user_context, &completion_id);
        const int submit_errno = zlink_errno ();
        const int system_errno = errno;
        CHECK (check_part_consumed (&part) == 0);

        if (result == ZLINK_SUBMIT_BACKPRESSURED) {
            CHECK (submit_errno == EAGAIN);
            CHECK (system_errno == EAGAIN);
            CHECK (completion_id != 0);
            CHECK (attempt != 0);
            *accepted_out = attempt;
            *wait_token_out = completion_id;
            return 0;
        }

        CHECK (result == ZLINK_SUBMIT_OK);
        CHECK (completion_id == 0);
    }

    return __LINE__;
}

/* Same fill, but the caller declines the token handle. Core still reserves the
   wait token for the refused attempt. */
static int fill_until_backpressured_null_id (void *router,
                                             const zlink_routing_id_t *target,
                                             const void *payload,
                                             size_t payload_size,
                                             size_t *accepted_out)
{
    size_t attempt;
    for (attempt = 0; attempt != MAX_FILL_ATTEMPTS; ++attempt) {
        zlink_msg_t part;
        CHECK (init_part (&part, payload, payload_size) == 0);

        errno = 0;
        const zlink_submit_result_t result =
          zlink_send_part_rid (router, target, &part, ZLINK_SEND_FLAGS_DONTWAIT,
                               ZLINK_PART_FINAL, NULL, NULL);
        const int submit_errno = zlink_errno ();
        CHECK (check_part_consumed (&part) == 0);

        if (result == ZLINK_SUBMIT_BACKPRESSURED) {
            CHECK (submit_errno == EAGAIN);
            CHECK (attempt != 0);
            *accepted_out = attempt;
            return 0;
        }
        CHECK (result == ZLINK_SUBMIT_OK);
    }

    return __LINE__;
}

/* Pull the queue to NO_DATA and require exactly one WRITABLE record. */
static int receive_single_writable (void *router, zlink_completion_t *record_out)
{
    int seen = 0;
    for (;;) {
        zlink_completion_t completion;
        memset (&completion, 0, sizeof (completion));
        completion.struct_size = sizeof (completion);
        errno = 0;
        const zlink_recv_result_t result =
          zlink_completion_recv (router, &completion, ZLINK_RECV_FLAGS_DONTWAIT);
        if (result == ZLINK_RECV_NO_DATA) {
            CHECK (errno == EAGAIN);
            zlink_completion_close (&completion);
            break;
        }
        CHECK (result == ZLINK_RECV_OK);
        CHECK (!seen);
        CHECK (completion.kind == ZLINK_COMPLETION_WRITABLE);
        *record_out = completion;
        seen = 1;
        zlink_completion_close (&completion);
    }
    CHECK (seen);
    return 0;
}

int main (void)
{
    static const char dealer_name[] = "c-dontwait-backpressure-peer";
    static const char endpoint[] = "inproc://c-dontwait-backpressure-contract";
    static const char prime_payload[] = "route-prime";
    unsigned char logical_payload[PAYLOAD_SIZE];
    memset (logical_payload, 'r', sizeof (logical_payload));
    memcpy (logical_payload, "c-dontwait-exact-retry", strlen ("c-dontwait-exact-retry"));

    void *ctx = zlink_ctx_new ();
    CHECK (ctx != NULL);
    void *router = zlink_socket (ctx, ZLINK_SOCKET_ROUTER);
    void *dealer = zlink_socket (ctx, ZLINK_SOCKET_DEALER);
    CHECK (router != NULL);
    CHECK (dealer != NULL);

    const int zero = 0;
    const int mandatory = 1;
    const int five_seconds = 5000;
    CHECK (zlink_set_option (router, ZLINK_OPT_LINGER, &zero, sizeof (zero)) == ZLINK_CONFIG_OK);
    CHECK (zlink_set_option (dealer, ZLINK_OPT_LINGER, &zero, sizeof (zero)) == ZLINK_CONFIG_OK);
    CHECK (zlink_set_option (dealer, ZLINK_OPT_SNDTIMEO, &five_seconds,
                             sizeof (five_seconds)) == ZLINK_CONFIG_OK);
    CHECK (zlink_set_router_option (router, ZLINK_ROUTER_OPT_MANDATORY, &mandatory,
                                    sizeof (mandatory)) == ZLINK_CONFIG_OK);
    CHECK (zlink_set_routing_id (dealer, dealer_name, strlen (dealer_name)) == ZLINK_CONFIG_OK);
    CHECK (configure_small_hwm (router) == 0);
    CHECK (configure_small_hwm (dealer) == 0);

    CHECK (zlink_bind (router, endpoint) == ZLINK_BIND_OK);
    CHECK (zlink_connect (dealer, endpoint) == ZLINK_CONNECT_OK);

    /* The bounded blocking prime synchronizes the connection and registers the
       DEALER routing id without a settle delay. */
    zlink_msg_t prime;
    zlink_completion_id_t prime_id = UINT64_MAX;
    CHECK (init_part (&prime, prime_payload, strlen (prime_payload)) == 0);
    CHECK (zlink_send_part (dealer, &prime, ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL, NULL,
                            &prime_id) == ZLINK_SUBMIT_OK);
    CHECK (prime_id == 0);
    CHECK (check_part_consumed (&prime) == 0);

    CHECK (wait_socket (router, ZLINK_POLLIN) == 0);
    const zlink_routing_id_t *source_rid = NULL;
    zlink_reply_token_t reply_token = UINT64_MAX;
    zlink_msg_t received_prime;
    zlink_part_flag_t prime_flag = ZLINK_PART_MORE;
    CHECK (zlink_msg_init (&received_prime) == ZLINK_CONFIG_OK);
    CHECK (zlink_router_recv_part (router, &source_rid, &reply_token, &received_prime,
                                   &prime_flag, ZLINK_RECV_FLAGS_DONTWAIT) == ZLINK_RECV_OK);
    CHECK (source_rid != NULL);
    CHECK (source_rid->size == strlen (dealer_name));
    CHECK (memcmp (source_rid->data, dealer_name, strlen (dealer_name)) == 0);
    CHECK (reply_token == 0);
    CHECK (prime_flag == ZLINK_PART_FINAL);
    CHECK (zlink_msg_close (&received_prime) == ZLINK_CONFIG_OK);

    zlink_routing_id_t target;
    memset (&target, 0, sizeof (target));
    CHECK (strlen (dealer_name) <= sizeof (target.data));
    target.size = (uint8_t) strlen (dealer_name);
    memcpy (target.data, dealer_name, target.size);

    int poller_context = 71;
    void *poller = zlink_poller_new ();
    CHECK (poller != NULL);
    const short retry_events = (short) (ZLINK_POLLOUT | ZLINK_POLLCOMPLETION);
    CHECK (zlink_poller_add (poller, router, &poller_context, retry_events) == ZLINK_CONFIG_OK);

    int operation_context = 72;
    size_t accepted = 0;
    zlink_completion_id_t wait_token = 0;
    CHECK (fill_until_backpressured (router, &target, logical_payload,
                                     sizeof (logical_payload), &operation_context, &accepted,
                                     &wait_token) == 0);
    CHECK (accepted != 0);
    CHECK (wait_token != 0);

    /* Neither admitted SENDs nor a newly issued wait token complete here. */
    CHECK (check_no_completion (router) == 0);
    zlink_poller_event_t event;
    zlink_config_result_t poller_error = ZLINK_CONFIG_INTERNAL_ERROR;
    memset (&event, 0, sizeof (event));
    CHECK (zlink_poller_wait (poller, &event, 1, 0, &poller_error) == 0);
    CHECK (poller_error == ZLINK_CONFIG_OK);

    size_t index;
    for (index = 0; index != accepted; ++index)
        CHECK (receive_part (dealer, logical_payload, sizeof (logical_payload)) == 0);

    /* Core retained no payload; restoring credit must not deliver the refused packet. */
    zlink_pollitem_t before_retry = {dealer, 0, ZLINK_POLLIN, 0};
    zlink_config_result_t before_retry_error = ZLINK_CONFIG_INTERNAL_ERROR;
    CHECK (zlink_poll (&before_retry, 1, 20, &before_retry_error) == 0);
    CHECK (before_retry_error == ZLINK_CONFIG_OK);
    CHECK (before_retry.revents == 0);

    memset (&event, 0, sizeof (event));
    poller_error = ZLINK_CONFIG_INTERNAL_ERROR;
    CHECK (zlink_poller_wait (poller, &event, 1, 5000, &poller_error) == 1);
    CHECK (poller_error == ZLINK_CONFIG_OK);
    CHECK (event.source_kind == ZLINK_POLLER_SOURCE_SOCKET);
    CHECK (event.socket == router);
    CHECK (event.user_data == &poller_context);
    CHECK ((event.events & retry_events) == retry_events);

    int matching_writable_seen = 0;
    for (;;) {
        zlink_completion_t completion;
        memset (&completion, 0, sizeof (completion));
        completion.struct_size = sizeof (completion);
        errno = 0;
        const zlink_recv_result_t result =
          zlink_completion_recv (router, &completion, ZLINK_RECV_FLAGS_DONTWAIT);
        if (result == ZLINK_RECV_NO_DATA) {
            CHECK (errno == EAGAIN);
            CHECK (zlink_errno () == EAGAIN);
            zlink_completion_close (&completion);
            break;
        }

        CHECK (result == ZLINK_RECV_OK);
        CHECK (!matching_writable_seen);
        CHECK (completion.kind == ZLINK_COMPLETION_WRITABLE);
        CHECK (completion.completion_id == wait_token);
        CHECK (completion.user_context == &operation_context);
        CHECK (completion.peer_rid.size == target.size);
        CHECK (memcmp (completion.peer_rid.data, target.data, target.size) == 0);
        CHECK (completion.send_result == ZLINK_SEND_ADMITTED);
        CHECK (completion.send_terminal_errno == 0);
        matching_writable_seen = 1;
        zlink_completion_close (&completion);
    }
    CHECK (matching_writable_seen);

    /* Core retained only the wait token and context. Recreate the same packet
       and make one fresh DONTWAIT admission attempt. */
    zlink_msg_t retry;
    zlink_completion_id_t retry_id = UINT64_MAX;
    CHECK (init_part (&retry, logical_payload, sizeof (logical_payload)) == 0);
    CHECK (zlink_send_part_rid (router, &target, &retry, ZLINK_SEND_FLAGS_DONTWAIT,
                                ZLINK_PART_FINAL, NULL, &retry_id) == ZLINK_SUBMIT_OK);
    CHECK (retry_id == 0);
    CHECK (check_part_consumed (&retry) == 0);
    CHECK (receive_part (dealer, logical_payload, sizeof (logical_payload)) == 0);

    CHECK (check_no_completion (router) == 0);
    zlink_pollitem_t no_duplicate = {dealer, 0, ZLINK_POLLIN, 0};
    zlink_config_result_t no_duplicate_error = ZLINK_CONFIG_INTERNAL_ERROR;
    CHECK (zlink_poll (&no_duplicate, 1, 20, &no_duplicate_error) == 0);
    CHECK (no_duplicate_error == ZLINK_CONFIG_OK);
    CHECK (no_duplicate.revents == 0);

    /* Declining the token handle does not decline the token: the refused
       attempt still publishes one WRITABLE (nonzero ID, NULL context, same
       RID) on this queue, so REQUEST completion consumers that share the
       queue must classify records by kind instead of failing on it. */
    size_t accepted_without_handle = 0;
    CHECK (fill_until_backpressured_null_id (router, &target, logical_payload,
                                             sizeof (logical_payload),
                                             &accepted_without_handle) == 0);
    CHECK (accepted_without_handle != 0);
    CHECK (check_no_completion (router) == 0);
    for (index = 0; index != accepted_without_handle; ++index)
        CHECK (receive_part (dealer, logical_payload, sizeof (logical_payload)) == 0);
    memset (&event, 0, sizeof (event));
    poller_error = ZLINK_CONFIG_INTERNAL_ERROR;
    CHECK (zlink_poller_wait (poller, &event, 1, 5000, &poller_error) == 1);
    CHECK (poller_error == ZLINK_CONFIG_OK);
    CHECK ((event.events & ZLINK_POLLCOMPLETION) == ZLINK_POLLCOMPLETION);
    zlink_completion_t anonymous_writable;
    memset (&anonymous_writable, 0, sizeof (anonymous_writable));
    CHECK (receive_single_writable (router, &anonymous_writable) == 0);
    CHECK (anonymous_writable.completion_id != 0);
    CHECK (anonymous_writable.completion_id != wait_token);
    CHECK (anonymous_writable.user_context == NULL);
    CHECK (anonymous_writable.peer_rid.size == target.size);
    CHECK (memcmp (anonymous_writable.peer_rid.data, target.data, target.size) == 0);
    CHECK (anonymous_writable.send_result == ZLINK_SEND_ADMITTED);
    CHECK (anonymous_writable.send_terminal_errno == 0);
    CHECK (check_no_completion (router) == 0);

    /* Explicit target removal retires a live token as TERMINAL/ENOENT with the
       same token, context and RID; a waiter must observe it as a failure
       instead of waiting for credit that can no longer arrive. */
    size_t accepted_before_removal = 0;
    zlink_completion_id_t removal_token = 0;
    CHECK (fill_until_backpressured (router, &target, logical_payload,
                                     sizeof (logical_payload), &operation_context,
                                     &accepted_before_removal, &removal_token) == 0);
    CHECK (removal_token != 0);
    CHECK (removal_token != wait_token);
    CHECK (check_no_completion (router) == 0);
    CHECK (zlink_disconnect_rid (router, &target) == ZLINK_CONNECT_OK);
    memset (&event, 0, sizeof (event));
    poller_error = ZLINK_CONFIG_INTERNAL_ERROR;
    CHECK (zlink_poller_wait (poller, &event, 1, 5000, &poller_error) == 1);
    CHECK (poller_error == ZLINK_CONFIG_OK);
    CHECK ((event.events & ZLINK_POLLCOMPLETION) == ZLINK_POLLCOMPLETION);
    zlink_completion_t terminal_writable;
    memset (&terminal_writable, 0, sizeof (terminal_writable));
    CHECK (receive_single_writable (router, &terminal_writable) == 0);
    CHECK (terminal_writable.completion_id == removal_token);
    CHECK (terminal_writable.user_context == &operation_context);
    CHECK (terminal_writable.peer_rid.size == target.size);
    CHECK (memcmp (terminal_writable.peer_rid.data, target.data, target.size) == 0);
    CHECK (terminal_writable.send_result == ZLINK_SEND_TERMINAL);
    CHECK (terminal_writable.send_terminal_errno == ENOENT);
    CHECK (check_no_completion (router) == 0);

    CHECK (zlink_poller_remove (poller, router) == ZLINK_CONFIG_OK);
    CHECK (zlink_poller_destroy (&poller) == ZLINK_CLOSE_OK);
    CHECK (poller == NULL);
    CHECK (zlink_close (dealer) == ZLINK_CLOSE_OK);
    CHECK (zlink_close (router) == ZLINK_CLOSE_OK);
    CHECK (zlink_ctx_term (ctx) == ZLINK_CLOSE_OK);
    return 0;
}
