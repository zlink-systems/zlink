#include <stddef.h>

#include <zlink.h>

#define CHECK(expr)                                                                                \
    do {                                                                                           \
        if (!(expr))                                                                               \
            return __LINE__;                                                                       \
    } while (0)

int main (void)
{
    CHECK (zlink_strerror (0) != NULL);

    void *ctx = zlink_ctx_new ();
    CHECK (ctx != NULL);
    CHECK (zlink_ctx_set (ctx, ZLINK_IO_THREADS, 1) == ZLINK_CONFIG_OK);
    zlink_config_result_t ctx_get_result = ZLINK_CONFIG_INTERNAL_ERROR;
    CHECK (zlink_ctx_get (ctx, ZLINK_IO_THREADS, &ctx_get_result) >= 1);
    CHECK (ctx_get_result == ZLINK_CONFIG_OK);

    void *pair = zlink_socket (ctx, ZLINK_SOCKET_PAIR);
    CHECK (pair != NULL);

    int linger = 0;
    CHECK (zlink_set_option (pair, ZLINK_OPT_LINGER, &linger, sizeof linger) == ZLINK_CONFIG_OK);

    int out_linger = -1;
    size_t out_size = sizeof out_linger;
    CHECK (zlink_get_option (pair, ZLINK_OPT_LINGER, &out_linger, &out_size) == ZLINK_CONFIG_OK);
    CHECK (out_size == sizeof out_linger);
    CHECK (out_linger == 0);

    CHECK (zlink_set_option (pair, ZLINK_OPT_LINGER, &linger, 1) == ZLINK_CONFIG_INVALID_ARGUMENT);
    CHECK (zlink_errno () != 0);

    void *invalid = zlink_socket (ctx, (zlink_socket_type_t) 0x7fff);
    CHECK (invalid == NULL);
    CHECK (zlink_errno () != 0);

    CHECK (zlink_close (pair) == ZLINK_CLOSE_OK);
    CHECK (zlink_ctx_term (ctx) == ZLINK_CLOSE_OK);
    return 0;
}
