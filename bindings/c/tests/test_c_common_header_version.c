#include <zlink/common.h>

#define CHECK(expr)                                                                                \
    do {                                                                                           \
        if (!(expr))                                                                               \
            return __LINE__;                                                                       \
    } while (0)

int main (void)
{
    CHECK (ZLINK_VERSION_MAJOR == 0);
    CHECK (ZLINK_VERSION_MINOR == 15);
    CHECK (ZLINK_VERSION_PATCH == 1);
    CHECK (ZLINK_VERSION == ZLINK_MAKE_VERSION (0, 15, 1));
    return 0;
}
