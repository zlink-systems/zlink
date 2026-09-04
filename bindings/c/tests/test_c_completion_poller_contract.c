#include <string.h>

#include <zlink.h>

#define CHECK(expr)                                                                                \
    do {                                                                                           \
        if (!(expr))                                                                               \
            return __LINE__;                                                                       \
    } while (0)

static int make_part (zlink_msg_t *msg, const char *text)
{
    const size_t len = strlen (text);
    CHECK (zlink_msg_init_size (msg, len) == ZLINK_CONFIG_OK);
    memcpy (zlink_msg_data (msg), text, len);
    return 0;
}

static int wait_for_completion_ready (void *poller, zlink_poller_event_t *event)
{
    for (int attempt = 0; attempt < 40; ++attempt) {
        zlink_config_result_t error = ZLINK_CONFIG_OK;
        memset (event, 0, sizeof (*event));
        const int rc = zlink_poller_wait (poller, event, 1, 50, &error);
        CHECK (error == ZLINK_CONFIG_OK);
        if (rc == 1 && (event->events & ZLINK_POLLCOMPLETION) != 0)
            return 1;
        CHECK (rc == 0);
    }
    return 0;
}

int main (void)
{
    void *ctx = zlink_ctx_new ();
    CHECK (ctx != NULL);
    void *router = zlink_socket (ctx, ZLINK_SOCKET_ROUTER);
    void *dealer = zlink_socket (ctx, ZLINK_SOCKET_DEALER);
    CHECK (router != NULL);
    CHECK (dealer != NULL);

    CHECK (zlink_bind (router, "inproc://c-completion-poller-contract") == ZLINK_BIND_OK);
    CHECK (zlink_connect (dealer, "inproc://c-completion-poller-contract") == ZLINK_CONNECT_OK);

    void *poller = zlink_poller_new ();
    CHECK (poller != NULL);
    int poller_tag = 17;
    CHECK (zlink_poller_add (poller, dealer, &poller_tag, ZLINK_POLLCOMPLETION) == ZLINK_CONFIG_OK);

    int context_tag = 29;
    zlink_completion_id_t completion_id = 0;
    zlink_msg_t request;
    CHECK (make_part (&request, "ping") == 0);
    /* This test covers completion delivery, not pre-admission readiness. The
       blocking variant waits for admission and still completes asynchronously. */
    CHECK (zlink_request_part (dealer, NULL, &request, ZLINK_SEND_FLAGS_NONE,
                               ZLINK_PART_FINAL, 2000, &context_tag, &completion_id)
           == ZLINK_SUBMIT_OK);
    CHECK (completion_id != 0);

    const zlink_routing_id_t *source_rid = NULL;
    zlink_reply_token_t reply_token = 0;
    zlink_msg_t received;
    zlink_part_flag_t has_more = ZLINK_PART_MORE;
    CHECK (zlink_msg_init (&received) == ZLINK_CONFIG_OK);
    CHECK (zlink_router_recv_part (router, &source_rid, &reply_token, &received, &has_more,
                                   ZLINK_RECV_FLAGS_NONE)
           == ZLINK_RECV_OK);
    CHECK (source_rid != NULL);
    CHECK (reply_token != 0);
    CHECK (has_more == ZLINK_PART_FINAL);
    CHECK (zlink_msg_close (&received) == ZLINK_CONFIG_OK);

    zlink_msg_t reply;
    CHECK (make_part (&reply, "pong") == 0);
    CHECK (zlink_reply_part (router, source_rid, reply_token, &reply, ZLINK_PART_FINAL)
           == ZLINK_SUBMIT_OK);

    zlink_poller_event_t first;
    CHECK (wait_for_completion_ready (poller, &first) == 1);
    CHECK (first.socket == dealer);
    CHECK (first.user_data == &poller_tag);

    zlink_poller_event_t repeated;
    zlink_config_result_t repeated_error = ZLINK_CONFIG_OK;
    memset (&repeated, 0, sizeof (repeated));
    CHECK (zlink_poller_wait (poller, &repeated, 1, 0, &repeated_error) == 1);
    CHECK (repeated_error == ZLINK_CONFIG_OK);
    CHECK ((repeated.events & ZLINK_POLLCOMPLETION) != 0);

    zlink_completion_t completion;
    memset (&completion, 0, sizeof (completion));
    completion.struct_size = sizeof (completion);
    CHECK (zlink_completion_recv (dealer, &completion, ZLINK_RECV_FLAGS_DONTWAIT) == ZLINK_RECV_OK);
    CHECK (completion.kind == ZLINK_COMPLETION_REQUEST);
    CHECK (completion.completion_id == completion_id);
    CHECK (completion.user_context == &context_tag);
    CHECK (completion.request_result == ZLINK_REQUEST_OK);
    CHECK (completion.reply_part_count == 1);
    CHECK (zlink_msg_size (&completion.reply_parts[0]) == strlen ("pong"));
    CHECK (memcmp (zlink_msg_data (&completion.reply_parts[0]), "pong", strlen ("pong")) == 0);
    zlink_completion_close (&completion);
    CHECK (completion.struct_size == sizeof (completion));
    CHECK (completion.reply_parts == NULL);
    CHECK (completion.reply_part_count == 0);

    CHECK (zlink_poller_destroy (&poller) == ZLINK_CLOSE_OK);
    CHECK (zlink_close (dealer) == ZLINK_CLOSE_OK);
    CHECK (zlink_close (router) == ZLINK_CLOSE_OK);
    CHECK (zlink_ctx_term (ctx) == ZLINK_CLOSE_OK);
    return 0;
}
