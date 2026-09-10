#include <errno.h>
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
    size_t part_count = 0;
    CHECK (zlink_recv (receiver, &rid, &empty_part, 1, &part_count,
                       ZLINK_RECV_FLAGS_DONTWAIT)
           == ZLINK_RECV_NO_DATA);

    zlink_msg_t outbound;
    CHECK (make_part (&outbound, "hello-c") == 0);
    CHECK (zlink_send (sender, &outbound, 1, ZLINK_SEND_FLAGS_NONE, NULL, NULL)
           == ZLINK_SUBMIT_OK);

    zlink_msg_t inbound;
    CHECK (zlink_recv (receiver, &rid, &inbound, 1, &part_count, ZLINK_RECV_FLAGS_NONE)
           == ZLINK_RECV_OK);
    CHECK (part_count == 1);
    CHECK (zlink_msg_size (&inbound) == strlen ("hello-c"));
    CHECK (memcmp (zlink_msg_data (&inbound), "hello-c", strlen ("hello-c")) == 0);
    zlink_multipart_close (&inbound, part_count);

    zlink_msg_t multipart[2];
    CHECK (make_part (&multipart[0], "first") == 0);
    CHECK (make_part (&multipart[1], "second") == 0);
    CHECK (zlink_send (sender, multipart, 2, ZLINK_SEND_FLAGS_NONE, NULL, NULL)
           == ZLINK_SUBMIT_OK);
    zlink_multipart_close (multipart, 2);

    zlink_msg_t preserved;
    CHECK (make_part (&preserved, "preserved") == 0);
    part_count = 0;
    errno = 0;
    CHECK (zlink_recv (receiver, &rid, &preserved, 1, &part_count,
                       ZLINK_RECV_FLAGS_NONE)
           == ZLINK_RECV_BUFFER_TOO_SMALL);
    CHECK (zlink_errno () == ENOBUFS);
    CHECK (part_count == 2);
    CHECK (zlink_msg_size (&preserved) == strlen ("preserved"));
    CHECK (memcmp (zlink_msg_data (&preserved), "preserved", strlen ("preserved")) == 0);
    CHECK (zlink_msg_close (&preserved) == ZLINK_CONFIG_OK);

    zlink_msg_t received_multipart[2];
    CHECK (zlink_recv (receiver, &rid, received_multipart, 2, &part_count,
                       ZLINK_RECV_FLAGS_NONE)
           == ZLINK_RECV_OK);
    CHECK (part_count == 2);
    CHECK (zlink_msg_size (&received_multipart[0]) == strlen ("first"));
    CHECK (memcmp (zlink_msg_data (&received_multipart[0]), "first", strlen ("first")) == 0);
    CHECK (zlink_msg_size (&received_multipart[1]) == strlen ("second"));
    CHECK (memcmp (zlink_msg_data (&received_multipart[1]), "second", strlen ("second")) == 0);
    zlink_multipart_close (received_multipart, part_count);

    CHECK (zlink_close (sender) == ZLINK_CLOSE_OK);
    CHECK (zlink_close (receiver) == ZLINK_CLOSE_OK);

    CHECK (zlink_ctx_term (ctx) == ZLINK_CLOSE_OK);
    return 0;
}
