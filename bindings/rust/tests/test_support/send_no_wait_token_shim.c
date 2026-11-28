// SPDX-License-Identifier: MPL-2.0

#include <errno.h>
#include "zlink.h"

zlink_submit_result_t zlink_send(
    void *socket, zlink_msg_t *parts, size_t part_count,
    zlink_send_flags_t flags, void *user_context,
    zlink_completion_id_t *completion_id)
{
    (void)socket;
    (void)flags;
    (void)user_context;
    zlink_multipart_close(parts, part_count);
    *completion_id = 0;
    errno = EAGAIN;
    return ZLINK_SUBMIT_BACKPRESSURED;
}
