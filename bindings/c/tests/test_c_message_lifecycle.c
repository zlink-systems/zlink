#include <stdlib.h>
#include <string.h>

#include <zlink.h>

#define CHECK(expr)                                                                                \
    do {                                                                                           \
        if (!(expr))                                                                               \
            return __LINE__;                                                                       \
    } while (0)

static int free_called = 0;

static void tracked_free (void *data, void *hint)
{
    if (data != hint)
        abort ();
    free (data);
    free_called = 1;
}

int main (void)
{
    zlink_msg_t empty;
    CHECK (zlink_msg_init (&empty) == ZLINK_CONFIG_OK);
    CHECK (zlink_msg_size (&empty) == 0);
    CHECK (zlink_msg_close (&empty) == ZLINK_CONFIG_OK);

    zlink_msg_t sized;
    CHECK (zlink_msg_init_size (&sized, 5) == ZLINK_CONFIG_OK);
    CHECK (zlink_msg_size (&sized) == 5);
    memcpy (zlink_msg_data (&sized), "hello", 5);
    CHECK (memcmp (zlink_msg_data (&sized), "hello", 5) == 0);

    zlink_msg_t copied;
    CHECK (zlink_msg_init (&copied) == ZLINK_CONFIG_OK);
    CHECK (zlink_msg_copy (&copied, &sized) == ZLINK_CONFIG_OK);
    CHECK (zlink_msg_size (&copied) == 5);
    CHECK (memcmp (zlink_msg_data (&copied), "hello", 5) == 0);

    CHECK (zlink_msg_close (&copied) == ZLINK_CONFIG_OK);
    CHECK (zlink_msg_close (&sized) == ZLINK_CONFIG_OK);

    char *borrowed = (char *) malloc (3);
    CHECK (borrowed != NULL);
    memcpy (borrowed, "abc", 3);

    zlink_msg_t external;
    CHECK (zlink_msg_init_data (&external, borrowed, 3, tracked_free, borrowed) == ZLINK_CONFIG_OK);
    CHECK (zlink_msg_size (&external) == 3);
    CHECK (memcmp (zlink_msg_data (&external), "abc", 3) == 0);
    CHECK (zlink_msg_close (&external) == ZLINK_CONFIG_OK);
    CHECK (free_called == 1);

    return 0;
}
