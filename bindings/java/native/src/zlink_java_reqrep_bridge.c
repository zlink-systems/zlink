#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <vector>

#include "zlink.h"

#ifdef __cplusplus
#define ZLINK_JAVA_EXPORT extern "C"
#else
#define ZLINK_JAVA_EXPORT
#endif
ZLINK_JAVA_EXPORT uintptr_t zlink_java_msg_data_addr (zlink_msg_t *msg)
{
    return (uintptr_t) zlink_msg_data (msg);
}

ZLINK_JAVA_EXPORT int zlink_java_send_u32 (
  void *socket, uint32_t routing_id, zlink_msg_t *parts, size_t part_count, int flags)
{
    zlink_routing_id_t rid;
    rid.size = 4;
    rid.data[0] = (uint8_t) (routing_id >> 24);
    rid.data[1] = (uint8_t) (routing_id >> 16);
    rid.data[2] = (uint8_t) (routing_id >> 8);
    rid.data[3] = (uint8_t) routing_id;
    if (parts == NULL || part_count == 0) {
        return (int) ZLINK_SUBMIT_INVALID_STATE;
    }

    for (size_t i = 0; i < part_count; ++i) {
        int rc = zlink_send_part_rid (socket, &rid, &parts[i], (zlink_send_flags_t) flags,
                                      i + 1u < part_count ? ZLINK_PART_MORE : ZLINK_PART_FINAL);
        if (rc != ZLINK_SUBMIT_OK) {
            return rc;
        }
    }
    return (int) ZLINK_SUBMIT_OK;
}
