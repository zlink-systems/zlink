#include <string.h>

#include <zlink.h>

#define CHECK(expr)                                                                                \
    do {                                                                                           \
        if (!(expr))                                                                               \
            return __LINE__;                                                                       \
    } while (0)

static zlink_routing_id_t routing_id_of_size (size_t size)
{
    zlink_routing_id_t rid;
    memset (&rid, 0, sizeof (rid));
    rid.size = (uint8_t) size;
    memset (rid.data, 'r', size < sizeof (rid.data) ? size : sizeof (rid.data));
    return rid;
}

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

    void *router = zlink_socket (ctx, ZLINK_SOCKET_ROUTER);
    void *pair = zlink_socket (ctx, ZLINK_SOCKET_PAIR);
    CHECK (router != NULL);
    CHECK (pair != NULL);

    zlink_routing_id_t rid255 = routing_id_of_size (255);
    CHECK (zlink_set_routing_id (pair, rid255.data, rid255.size) == ZLINK_CONFIG_OK);
    CHECK (zlink_set_routing_id (pair, rid255.data, 256) == ZLINK_CONFIG_INVALID_ARGUMENT);

    char endpoint256[280];
    memset (endpoint256, 'e', sizeof (endpoint256));
    endpoint256[0] = 't';
    endpoint256[1] = 'c';
    endpoint256[2] = 'p';
    endpoint256[3] = ':';
    endpoint256[4] = '/';
    endpoint256[5] = '/';
    endpoint256[sizeof (endpoint256) - 1] = '\0';
    CHECK (zlink_bind (pair, endpoint256) == ZLINK_BIND_INVALID_ARGUMENT);

    zlink_msg_t failed;
    CHECK (make_part (&failed, "keep-owned") == 0);
    zlink_routing_id_t unknown = routing_id_of_size (7);
    CHECK (
      zlink_send_part_rid (router, &unknown, &failed, ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL)
      != ZLINK_SUBMIT_OK);
    CHECK (zlink_msg_close (&failed) == ZLINK_CONFIG_OK);

    zlink_msg_t pub_failed;
    CHECK (make_part (&pub_failed, "publish") == 0);
    CHECK (
      zlink_publish_part (pair, "topic", &pub_failed, ZLINK_SEND_FLAGS_DONTWAIT, ZLINK_PART_FINAL)
      == ZLINK_SUBMIT_NOT_SUPPORTED);
    CHECK (zlink_msg_close (&pub_failed) == ZLINK_CONFIG_OK);

    CHECK (zlink_close (router) == ZLINK_CLOSE_OK);
    CHECK (zlink_close (pair) == ZLINK_CLOSE_OK);
    CHECK (zlink_ctx_term (ctx) == ZLINK_CLOSE_OK);
    return 0;
}
