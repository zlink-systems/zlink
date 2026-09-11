/* SPDX-License-Identifier: MPL-2.0 */

#pragma once

#include "addon_common_api.h"

inline void copy_routing_id (zlink_routing_id_t *out, const zlink_routing_id_t *in)
{
    if (!out)
        return;
    if (in)
        memcpy (out, in, sizeof (*out));
    else
        memset (out, 0, sizeof (*out));
}

inline bool parse_routing_id_value (napi_env env,
                                    napi_value value,
                                    zlink_routing_id_t *routing_id)
{
    void *data = NULL;
    size_t len = 0;
    if (napi_get_buffer_info (env, value, &data, &len) != napi_ok) {
        napi_throw_type_error (env, NULL, "routingId must be Buffer");
        return false;
    }
    if (len == 0 || len > sizeof (routing_id->data)) {
        napi_throw_range_error (env, NULL, "routingId length must be 1..255 bytes");
        return false;
    }
    memset (routing_id, 0, sizeof (*routing_id));
    routing_id->size = static_cast<uint8_t> (len);
    memcpy (routing_id->data, data, len);
    return true;
}

inline void close_recv_parts (zlink_msg_t *parts, size_t part_count)
{
    if (!parts)
        return;
    zlink_multipart_close (parts, part_count);
}
