/* SPDX-License-Identifier: MPL-2.0 */

#include "addon_request_callbacks.h"
#include "addon_message_parts.h"
#include "addon_message_values.h"
#include "addon_tsfn_slots.h"

#include <memory>
#include <vector>

struct request_js_state_t
{
    request_js_state_t () : env (NULL), tsfn (NULL) {}

    napi_env env;
    napi_threadsafe_function tsfn;
};

namespace
{

struct request_result_js_payload_t
{
    request_result_js_payload_t () : errnum (0), part_count (0) {}

    ~request_result_js_payload_t ()
    {
        if (part_count > 0)
            close_recv_parts (parts.data (), part_count);
    }

    int errnum;
    std::vector<zlink_msg_t> parts;
    size_t part_count;
};

bool move_recv_parts_to_payload (zlink_msg_t *parts,
                                 size_t part_count,
                                 request_result_js_payload_t *payload)
{
    if (!payload)
        return false;
    payload->parts.resize (part_count);
    for (size_t i = 0; i < part_count; ++i) {
        if (zlink_msg_init (&payload->parts[i]) != 0)
            return false;
        payload->part_count = i + 1;
        if (zlink_msg_move (&payload->parts[i], &parts[i]) != 0)
            return false;
    }
    return true;
}

void request_tsfn_finalize (napi_env env, void *finalize_data, void *finalize_hint)
{
    (void) env;
    (void) finalize_hint;
    request_js_state_t *state = static_cast<request_js_state_t *> (finalize_data);
    delete state;
}

void request_tsfn_call_js (napi_env env, napi_value js_cb, void *context, void *data)
{
    (void) context;
    std::unique_ptr<request_result_js_payload_t> payload (
      static_cast<request_result_js_payload_t *> (data));
    if (!env || !js_cb || !payload)
        return;

    napi_value argv[2];
    napi_create_int32 (env, payload->errnum, &argv[0]);
    if (payload->errnum != 0) {
        napi_get_null (env, &argv[1]);
    } else {
        napi_value parts_array;
        napi_create_array_with_length (env, payload->parts.size (), &parts_array);
        for (size_t i = 0; i < payload->parts.size (); ++i) {
            napi_value part_buf = create_message_data_buffer (env, &payload->parts[i]);
            if (!part_buf)
                return;
            napi_set_element (env, parts_array, static_cast<uint32_t> (i), part_buf);
        }
        argv[1] = parts_array;
    }

    napi_value recv;
    napi_value this_arg;
    napi_get_undefined (env, &this_arg);
    (void) napi_call_function (env, this_arg, js_cb, 2, argv, &recv);
}

} // namespace

request_js_state_t *create_core_request_js_state (napi_env env, napi_value handler)
{
    request_js_state_t *state = new request_js_state_t ();
    state->env = env;

    napi_value resource_name;
    napi_create_string_utf8 (env, "zlink-request-reply-callback", NAPI_AUTO_LENGTH,
                             &resource_name);
    napi_threadsafe_function tsfn = NULL;
    napi_status status = napi_create_threadsafe_function (
      env, handler, NULL, resource_name, 0, 1, state, request_tsfn_finalize, state,
      request_tsfn_call_js, &tsfn);
    if (status != napi_ok) {
        delete state;
        napi_throw_error (env, NULL, "request callback setup failed");
        return NULL;
    }
    (void) napi_unref_threadsafe_function (env, tsfn);
    state->tsfn = tsfn;
    return state;
}

void abort_request_js_state (request_js_state_t *state)
{
    if (!state || !state->tsfn)
        return;
    (void) napi_release_threadsafe_function (state->tsfn, napi_tsfn_abort);
    state->tsfn = NULL;
}

void request_reply_callback_trampoline (zlink_request_result_t errnum,
                                        zlink_msg_t *parts,
                                        size_t part_count,
                                        void *userdata)
{
    request_js_state_t *state = static_cast<request_js_state_t *> (userdata);
    if (!state || !state->tsfn) {
        close_recv_parts (parts, part_count);
        return;
    }

    std::unique_ptr<request_result_js_payload_t> payload (new request_result_js_payload_t ());
    payload->errnum = errnum;
    if (errnum == 0 && !move_recv_parts_to_payload (parts, part_count, payload.get ()))
        payload->errnum = ZLINK_REQUEST_INTERNAL_ERROR;
    close_recv_parts (parts, part_count);

    if (napi_call_threadsafe_function (state->tsfn, payload.get (), napi_tsfn_nonblocking)
        == napi_ok)
        payload.release ();
    release_request_tsfn (state);
}
