/* SPDX-License-Identifier: MPL-2.0 */

#include "addon_request_callbacks.h"
#include "addon_message_parts.h"
#include "addon_message_values.h"
#include "addon_tsfn_slots.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

struct request_completion_dispatcher_t
{
    request_completion_dispatcher_t ()
        : socket (NULL), env (NULL), tsfn (NULL), handler (NULL), active (0), closing (false)
    {
    }

    void *socket;
    napi_env env;
    napi_threadsafe_function tsfn;
    napi_ref handler;
    std::atomic<size_t> active;
    std::atomic<bool> closing;
};

struct request_js_state_t
{
    request_js_state_t () : env (NULL), dispatcher (NULL), token (0) {}

    napi_env env;
    request_completion_dispatcher_t *dispatcher;
    uint64_t token;
};

namespace
{

std::mutex g_request_dispatchers_mu;
std::unordered_map<void *, request_completion_dispatcher_t *> g_request_dispatchers;

struct request_result_js_payload_t
{
    request_result_js_payload_t () : state (NULL), errnum (0), part_count (0) {}

    ~request_result_js_payload_t ()
    {
        if (part_count > 0)
            close_recv_parts (parts.data (), part_count);
    }

    request_js_state_t *state;
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

void release_request_state (napi_env env, request_js_state_t *state)
{
    if (!state)
        return;
    request_completion_dispatcher_t *dispatcher = state->dispatcher;
    delete state;
    if (dispatcher && dispatcher->active.fetch_sub (1) == 1
        && dispatcher->closing.load ())
        (void) napi_release_threadsafe_function (dispatcher->tsfn, napi_tsfn_release);
}

void request_dispatcher_finalize (napi_env env,
                                  void *finalize_data,
                                  void *finalize_hint)
{
    (void) finalize_hint;
    request_completion_dispatcher_t *dispatcher =
      static_cast<request_completion_dispatcher_t *> (finalize_data);
    if (env && dispatcher && dispatcher->handler)
        (void) napi_delete_reference (env, dispatcher->handler);
    delete dispatcher;
}

void request_tsfn_call_js (napi_env env, napi_value js_cb, void *context, void *data)
{
    (void) js_cb;
    request_completion_dispatcher_t *dispatcher =
      static_cast<request_completion_dispatcher_t *> (context);
    std::unique_ptr<request_result_js_payload_t> payload (
      static_cast<request_result_js_payload_t *> (data));
    if (!dispatcher || !payload)
        return;

    request_js_state_t *state = payload->state;
    if (!env || !state || !dispatcher->handler) {
        release_request_state (NULL, state);
        return;
    }

    napi_value completion;
    napi_create_object (env, &completion);
    napi_value token;
    napi_create_bigint_uint64 (env, state->token, &token);
    napi_set_named_property (env, completion, "token", token);
    napi_value result;
    napi_create_int32 (env, payload->errnum, &result);
    napi_set_named_property (env, completion, "result", result);
    if (payload->errnum != 0) {
        napi_value none;
        napi_get_null (env, &none);
        napi_set_named_property (env, completion, "parts", none);
    } else {
        napi_value parts_array;
        napi_create_array_with_length (env, payload->parts.size (), &parts_array);
        for (size_t part_index = 0; part_index < payload->parts.size (); ++part_index) {
            napi_value part_buf = create_received_message_buffer (
              env, &payload->parts[part_index]);
            if (!part_buf) {
                release_request_state (env, state);
                return;
            }
            napi_set_element (env, parts_array, static_cast<uint32_t> (part_index), part_buf);
        }
        napi_set_named_property (env, completion, "parts", parts_array);
    }

    napi_value handler;
    napi_value this_arg;
    napi_value recv;
    napi_get_reference_value (env, dispatcher->handler, &handler);
    napi_get_undefined (env, &this_arg);
    napi_call_function (env, this_arg, handler, 1, &completion, &recv);
    release_request_state (env, state);
}

request_completion_dispatcher_t *request_dispatcher (napi_env env, void *socket)
{
    std::lock_guard<std::mutex> lock (g_request_dispatchers_mu);
    std::unordered_map<void *, request_completion_dispatcher_t *>::iterator existing =
      g_request_dispatchers.find (socket);
    if (existing != g_request_dispatchers.end ())
        return existing->second;

    request_completion_dispatcher_t *dispatcher = new request_completion_dispatcher_t ();
    dispatcher->socket = socket;
    dispatcher->env = env;
    napi_value resource_name;
    napi_create_string_utf8 (env, "zlink-request-completion-dispatcher", NAPI_AUTO_LENGTH,
                             &resource_name);
    if (napi_create_threadsafe_function (
          env, NULL, NULL, resource_name, 0, 1, dispatcher,
          request_dispatcher_finalize, dispatcher, request_tsfn_call_js,
          &dispatcher->tsfn)
        != napi_ok) {
        delete dispatcher;
        napi_throw_error (env, NULL, "request completion dispatcher setup failed");
        return NULL;
    }
    (void) napi_unref_threadsafe_function (env, dispatcher->tsfn);
    g_request_dispatchers[socket] = dispatcher;
    return dispatcher;
}

} // namespace

request_js_state_t *
create_core_request_js_state (napi_env env, void *socket, uint64_t token)
{
    request_completion_dispatcher_t *dispatcher = request_dispatcher (env, socket);
    if (!dispatcher)
        return NULL;
    request_js_state_t *state = new request_js_state_t ();
    state->env = env;
    state->dispatcher = dispatcher;
    state->token = token;
    dispatcher->active.fetch_add (1);
    return state;
}

bool set_socket_request_completion_handler (napi_env env, void *socket, napi_value handler)
{
    request_completion_dispatcher_t *dispatcher = request_dispatcher (env, socket);
    if (!dispatcher)
        return false;
    if (dispatcher->handler)
        (void) napi_delete_reference (env, dispatcher->handler);
    if (napi_create_reference (env, handler, 1, &dispatcher->handler) != napi_ok) {
        napi_throw_error (env, NULL, "request completion handler setup failed");
        return false;
    }
    return true;
}

void abort_request_js_state (request_js_state_t *state)
{
    if (!state)
        return;
    release_request_state (state->env, state);
}

void release_socket_request_dispatcher (void *socket)
{
    request_completion_dispatcher_t *dispatcher = NULL;
    {
        std::lock_guard<std::mutex> lock (g_request_dispatchers_mu);
        std::unordered_map<void *, request_completion_dispatcher_t *>::iterator entry =
          g_request_dispatchers.find (socket);
        if (entry == g_request_dispatchers.end ())
            return;
        dispatcher = entry->second;
        g_request_dispatchers.erase (entry);
    }
    dispatcher->closing.store (true);
    if (dispatcher->active.load () == 0)
        (void) napi_release_threadsafe_function (dispatcher->tsfn, napi_tsfn_release);
}

void request_reply_callback_trampoline (zlink_request_result_t errnum,
                                        zlink_msg_t *parts,
                                        size_t part_count,
                                        void *userdata)
{
    request_js_state_t *state = static_cast<request_js_state_t *> (userdata);
    if (!state || !state->dispatcher || !state->dispatcher->tsfn) {
        close_recv_parts (parts, part_count);
        return;
    }

    std::unique_ptr<request_result_js_payload_t> payload (new request_result_js_payload_t ());
    payload->errnum = errnum;
    if (errnum == 0 && !move_recv_parts_to_payload (parts, part_count, payload.get ()))
        payload->errnum = ZLINK_REQUEST_INTERNAL_ERROR;
    close_recv_parts (parts, part_count);

    payload->state = state;
    request_completion_dispatcher_t *dispatcher = state->dispatcher;
    if (napi_call_threadsafe_function (
          dispatcher->tsfn, payload.get (), napi_tsfn_nonblocking)
        == napi_ok)
        payload.release ();
    else
        release_request_state (NULL, state);
}
