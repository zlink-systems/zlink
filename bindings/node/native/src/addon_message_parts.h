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

template <typename Submit>
inline int submit_msg_parts (zlink_msg_t *parts, size_t part_count, Submit submit)
{
    if (!parts || part_count == 0) {
        errno = EFAULT;
        return ZLINK_SUBMIT_INVALID_ARGUMENT;
    }

    for (size_t i = 0; i < part_count; ++i) {
        const bool is_final = i + 1u == part_count;
        const zlink_part_flag_t part_flag = is_final ? ZLINK_PART_FINAL : ZLINK_PART_MORE;
        int rc = submit (&parts[i], part_flag, is_final);
        if (rc != ZLINK_SUBMIT_OK) {
            // Core consumes the attempted native part even when the record is
            // rejected. Release the empty attempted slot and every untouched
            // later slot in this staging copy; the JavaScript Message owners
            // remain separate and untouched.
            zlink_multipart_close (parts, part_count);
            return rc;
        }
    }

    return ZLINK_SUBMIT_OK;
}
