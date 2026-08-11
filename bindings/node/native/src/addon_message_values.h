/* SPDX-License-Identifier: MPL-2.0 */

#pragma once

#include "addon_common_api.h"

#include <atomic>
#include <new>

static constexpr size_t copied_message_buffer_max_size = 1024;

// A Message owns this frame. Its Buffer is only a JavaScript view of the
// zlink_msg_t storage; it is never the owning payload allocation.
struct native_message_frame_t
{
    native_message_frame_t () : references (1) {}
    zlink_msg_t message;
    std::atomic<uint32_t> references;
};

struct native_message_frame_handle_t
{
    explicit native_message_frame_handle_t (native_message_frame_t *frame_) : frame (frame_) {}
    native_message_frame_t *frame;
};

inline void retain_native_message_frame (native_message_frame_t *frame)
{
    frame->references.fetch_add (1, std::memory_order_relaxed);
}

inline void release_native_message_frame (native_message_frame_t *frame)
{
    if (!frame || frame->references.fetch_sub (1, std::memory_order_acq_rel) != 1)
        return;
    zlink_msg_close (&frame->message);
    delete frame;
}

inline native_message_frame_t *get_native_message_frame (napi_env env, napi_value value,
                                                          const char *error_message)
{
    native_message_frame_handle_t *handle = NULL;
    if (napi_get_value_external (env, value, reinterpret_cast<void **> (&handle)) != napi_ok
        || !handle || !handle->frame) {
        napi_throw_type_error (env, NULL, error_message);
        return NULL;
    }
    return handle->frame;
}

inline void finalize_native_message_frame (napi_env env, void *data, void *hint)
{
    (void) env;
    (void) hint;
    native_message_frame_handle_t *handle =
      static_cast<native_message_frame_handle_t *> (data);
    if (!handle)
        return;
    release_native_message_frame (handle->frame);
    delete handle;
}

inline void finalize_native_message_buffer (napi_env env, void *data, void *hint)
{
    (void) env;
    (void) data;
    release_native_message_frame (static_cast<native_message_frame_t *> (hint));
}

inline napi_value create_native_message_data_buffer (napi_env env, native_message_frame_t *frame)
{
    napi_value data;
    retain_native_message_frame (frame);
    const napi_status status = napi_create_external_buffer (
      env, zlink_msg_size (&frame->message), zlink_msg_data (&frame->message),
      finalize_native_message_buffer, frame, &data);
    if (status != napi_ok) {
        release_native_message_frame (frame);
        napi_throw_error (env, NULL, "native message buffer creation failed");
        return NULL;
    }
    return data;
}

inline napi_value create_native_message_frame_handle (napi_env env, native_message_frame_t *frame)
{
    native_message_frame_handle_t *handle =
      new (std::nothrow) native_message_frame_handle_t (frame);
    if (!handle) {
        napi_throw_error (env, NULL, "native message frame handle allocation failed");
        return NULL;
    }
    napi_value native_message;
    retain_native_message_frame (frame);
    const napi_status status = napi_create_external (
      env, handle, finalize_native_message_frame, NULL, &native_message);
    if (status != napi_ok) {
        release_native_message_frame (frame);
        delete handle;
        napi_throw_error (env, NULL, "native message frame creation failed");
        return NULL;
    }
    return native_message;
}

inline napi_value create_native_message_value (napi_env env, native_message_frame_t *frame)
{
    napi_value data = create_native_message_data_buffer (env, frame);
    if (!data) {
        release_native_message_frame (frame);
        return NULL;
    }
    napi_value native_message = create_native_message_frame_handle (env, frame);
    if (!native_message) {
        release_native_message_frame (frame);
        return NULL;
    }

    napi_value out;
    napi_create_object (env, &out);
    napi_set_named_property (env, out, "data", data);
    napi_set_named_property (env, out, "nativeMessage", native_message);
    release_native_message_frame (frame);
    return out;
}

inline napi_value move_message_to_native_value (napi_env env, zlink_msg_t *source)
{
    native_message_frame_t *frame = new (std::nothrow) native_message_frame_t;
    if (!frame) {
        napi_throw_error (env, NULL, "native message frame allocation failed");
        return NULL;
    }
    if (zlink_msg_init (&frame->message) != 0) {
        delete frame;
        return throw_last_error (env, "native message frame init failed");
    }
    if (zlink_msg_move (&frame->message, source) != 0) {
        zlink_msg_close (&frame->message);
        delete frame;
        return throw_last_error (env, "native message frame move failed");
    }
    return create_native_message_value (env, frame);
}

inline napi_value move_message_to_native_frame_value (napi_env env, zlink_msg_t *source)
{
    native_message_frame_t *frame = new (std::nothrow) native_message_frame_t;
    if (!frame) {
        napi_throw_error (env, NULL, "native message frame allocation failed");
        return NULL;
    }
    if (zlink_msg_init (&frame->message) != 0) {
        delete frame;
        return throw_last_error (env, "native message frame init failed");
    }
    if (zlink_msg_move (&frame->message, source) != 0) {
        zlink_msg_close (&frame->message);
        delete frame;
        return throw_last_error (env, "native message frame move failed");
    }
    napi_value native_message = create_native_message_frame_handle (env, frame);
    release_native_message_frame (frame);
    return native_message;
}

inline napi_value create_buffer_copy_or_empty (napi_env env, const void *data, size_t len)
{
    napi_value out;
    napi_create_buffer_copy (env, len, len == 0 ? NULL : data, NULL, &out);
    return out;
}

inline void finalize_external_msg_buffer (napi_env env, void *data, void *hint)
{
    (void) env;
    (void) data;
    zlink_msg_t *msg = static_cast<zlink_msg_t *> (hint);
    if (!msg)
        return;
    zlink_msg_close (msg);
    delete msg;
}

inline napi_value create_message_data_buffer (napi_env env, zlink_msg_t *msg)
{
    const size_t size = zlink_msg_size (msg);
    if (size == 0)
        return create_buffer_copy_or_empty (env, NULL, 0);
    if (size <= copied_message_buffer_max_size)
        return create_buffer_copy_or_empty (env, zlink_msg_data (msg), size);

    zlink_msg_t *owned = new (std::nothrow) zlink_msg_t;
    if (!owned) {
        napi_throw_error (env, NULL, "message buffer allocation failed");
        return NULL;
    }
    if (zlink_msg_init (owned) != 0) {
        delete owned;
        return throw_last_error (env, "message buffer init failed");
    }
    if (zlink_msg_move (owned, msg) != 0) {
        zlink_msg_close (owned);
        delete owned;
        return throw_last_error (env, "message buffer move failed");
    }

    napi_value data;
    napi_status status = napi_create_external_buffer (env, size, zlink_msg_data (owned),
                                                      finalize_external_msg_buffer, owned, &data);
    if (status != napi_ok) {
        zlink_msg_close (owned);
        delete owned;
        napi_throw_error (env, NULL, "message buffer creation failed");
        return NULL;
    }
    return data;
}

inline napi_value create_routing_id_value (napi_env env, const zlink_routing_id_t &rid)
{
    if (rid.size == 0) {
        napi_value none;
        napi_get_null (env, &none);
        return none;
    }
    return create_buffer_copy_or_empty (env, rid.data, rid.size);
}

enum message_snapshot_flags_t
{
    MESSAGE_SNAPSHOT_DEFAULT = 0,
    MESSAGE_SNAPSHOT_ALWAYS_REF_COUNT = 1 << 0,
    MESSAGE_SNAPSHOT_ALWAYS_PROPERTIES = 1 << 1
};

// The Core raw API exposes no message property API, so the binding derives its
// synthetic routed identity properties itself and avoids repeated native
// property probes on every data-only receive.
inline napi_value create_message_properties_snapshot (napi_env env,
                                                      const zlink_routing_id_t *routing_id,
                                                      zlink_msg_t *msg,
                                                      bool force)
{
    (void) msg;
    const bool has_routing_id_bytes = routing_id && routing_id->size > 0;

    if (!has_routing_id_bytes && !force)
        return NULL;

    napi_value props;
    napi_create_object (env, &props);

    if (has_routing_id_bytes) {
        napi_value out;
        napi_create_string_utf8 (env, reinterpret_cast<const char *> (routing_id->data),
                                 routing_id->size, &out);
        napi_set_named_property (env, props, "Routing-Id", out);
        napi_set_named_property (env, props, "Identity", out);
    }

    return props;
}

inline napi_value create_message_snapshot_value (napi_env env,
                                                 const zlink_routing_id_t *routing_id,
                                                 zlink_msg_t *msg,
                                                 int flags = MESSAGE_SNAPSHOT_DEFAULT)
{
    napi_value obj;
    napi_create_object (env, &obj);

    // A freshly received message is solely owned by the binding, so its
    // reference count is always 1 on the receive path and the "refCount" field
    // is omitted. Only query the library when a caller explicitly asks for the
    // count to be surfaced by an explicit diagnostic path.
    int refcnt = 1;
    if (flags & MESSAGE_SNAPSHOT_ALWAYS_REF_COUNT) {
        zlink_config_result_t refcnt_err = ZLINK_CONFIG_OK;
        refcnt = zlink_msg_refcnt (msg, &refcnt_err);
        if (refcnt_err != ZLINK_CONFIG_OK)
            return throw_last_error (env, "message refcnt failed");
    }

    napi_value native_message = move_message_to_native_frame_value (env, msg);
    if (!native_message)
        return NULL;

    napi_set_named_property (env, obj, "nativeMessage", native_message);
    if (refcnt != 1 || (flags & MESSAGE_SNAPSHOT_ALWAYS_REF_COUNT)) {
        napi_value ref_count;
        napi_create_int32 (env, refcnt, &ref_count);
        napi_set_named_property (env, obj, "refCount", ref_count);
    }
    napi_value props = create_message_properties_snapshot (
      env, routing_id, msg, (flags & MESSAGE_SNAPSHOT_ALWAYS_PROPERTIES) != 0);
    if (props)
        napi_set_named_property (env, obj, "properties", props);
    return obj;
}
