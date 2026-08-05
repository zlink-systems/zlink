#include <zlink.h>

#define CHECK(expr)                                                                                \
    do {                                                                                           \
        if (!(expr))                                                                               \
            return __LINE__;                                                                       \
    } while (0)

int main (void)
{
    CHECK (ZLINK_VERSION_MAJOR == 11);
    CHECK (ZLINK_VERSION_MINOR == 0);
    CHECK (ZLINK_VERSION_PATCH == 0);
    CHECK (ZLINK_VERSION == ZLINK_MAKE_VERSION (11, 0, 0));

    CHECK (ZLINK_SOCKET_PAIR == 0x1001);
    CHECK (ZLINK_SOCKET_STREAM == 0x1008);
    CHECK (ZLINK_DONTWAIT == ZLINK_SEND_FLAGS_DONTWAIT);

    zlink_msg_t msg;
    CHECK (sizeof msg >= 64);

    int major = 0;
    int minor = 0;
    int patch = 0;
    zlink_version (&major, &minor, &patch);
    CHECK (major == ZLINK_VERSION_MAJOR);
    CHECK (minor == ZLINK_VERSION_MINOR);
    CHECK (patch == ZLINK_VERSION_PATCH);

    CHECK (zlink_send_part != NULL);
    CHECK (zlink_send_part_rid != NULL);
    CHECK (zlink_recv_part != NULL);
    CHECK (zlink_publish_part != NULL);
    CHECK (zlink_subscribe_part != NULL);
    return 0;
}
