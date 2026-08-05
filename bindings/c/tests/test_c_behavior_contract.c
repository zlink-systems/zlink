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

int main (void)
{
    void *ctx = zlink_ctx_new ();
    CHECK (ctx != NULL);

    void *sender = zlink_socket (ctx, ZLINK_SOCKET_PAIR);
    void *receiver = zlink_socket (ctx, ZLINK_SOCKET_PAIR);
    CHECK (sender != NULL);
    CHECK (receiver != NULL);

    const char *endpoint = "inproc://c-behavior-contract";
    CHECK (zlink_bind (receiver, endpoint) == ZLINK_BIND_OK);
    CHECK (zlink_connect (sender, endpoint) == ZLINK_CONNECT_OK);

    const zlink_routing_id_t *rid = NULL;
    zlink_msg_t empty_part;
    zlink_part_flag_t has_more = ZLINK_PART_FINAL;
    CHECK (zlink_recv_part (receiver, &rid, &empty_part, &has_more, ZLINK_RECV_FLAGS_DONTWAIT)
           == ZLINK_RECV_NO_DATA);

    zlink_msg_t outbound;
    CHECK (make_part (&outbound, "hello-c") == 0);
    CHECK (zlink_send_part (sender, &outbound, ZLINK_SEND_FLAGS_NONE, ZLINK_PART_FINAL)
           == ZLINK_SUBMIT_OK);

    zlink_msg_t inbound;
    CHECK (zlink_recv_part (receiver, &rid, &inbound, &has_more, ZLINK_RECV_FLAGS_NONE)
           == ZLINK_RECV_OK);
    CHECK (has_more == ZLINK_PART_FINAL);
    CHECK (zlink_msg_size (&inbound) == strlen ("hello-c"));
    CHECK (memcmp (zlink_msg_data (&inbound), "hello-c", strlen ("hello-c")) == 0);
    CHECK (zlink_msg_close (&inbound) == ZLINK_CONFIG_OK);

    CHECK (zlink_close (sender) == ZLINK_CLOSE_OK);
    CHECK (zlink_close (receiver) == ZLINK_CLOSE_OK);

    CHECK (zlink_ctx_term (ctx) == ZLINK_CLOSE_OK);
    return 0;
}
