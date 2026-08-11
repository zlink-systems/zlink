/* SPDX-License-Identifier: MPL-2.0 */

#include "addon_request_callbacks.h"
#include "addon_message_parts.h"
#include "addon_message_values.h"
#include "addon_tsfn_slots.h"

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

struct request_completion_dispatcher_t
{
    request_completion_dispatcher_t ()
        : socket (NULL), env (NULL), tsfn (NULL), active (0), closing (false),
          scheduled (false)
    {
    }

    void *socket;
    napi_env env;
    napi_threadsafe_function tsfn;
    std::atomic<size_t> active;
    std::atomic<bool> closing;
    std::mutex queue_mu;
    std::deque<void *> queue;
    bool scheduled;
};

struct request_js_state_t
{
    request_js_state_t () : env (NULL), dispatcher (NULL), handler (NULL) {}

    napi_env env;
    request_completion_dispatcher_t *dispatcher;
    napi_ref handler;
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
    if (env && state->handler)
        (void) napi_delete_reference (env, state->handler);
    delete state;
    if (dispatcher && dispatcher->active.fetch_sub (1) == 1
        && dispatcher->closing.load ())
        (void) napi_release_threadsafe_function (dispatcher->tsfn, napi_tsfn_release);
}

void request_dispatcher_finalize (napi_env env,
                                  void *finalize_data,
                                  void *finalize_hint)
{
    (void) env;
    (void) finalize_hint;
    delete static_cast<request_completion_dispatcher_t *> (finalize_data);
}

void request_tsfn_call_js (napi_env env, napi_value js_cb, void *context, void *data)
{
    (void) js_cb;
    (void) data;
    request_completion_dispatcher_t *dispatcher =
      static_cast<request_completion_dispatcher_t *> (context);
    if (!dispatcher)
        return;

    std::vector<request_result_js_payload_t *> batch;
    bool reschedule = false;
    {
        std::lock_guard<std::mutex> lock (dispatcher->queue_mu);
        const size_t count = std::min<size_t> (64, dispatcher->queue.size ());
        batch.reserve (count);
        for (size_t index = 0; index < count; ++index) {
            batch.push_back (static_cast<request_result_js_payload_t *> (
              dispatcher->queue.front ()));
            dispatcher->queue.pop_front ();
        }
        reschedule = !dispatcher->queue.empty ();
        dispatcher->scheduled = reschedule;
    }

    for (size_t index = 0; index < batch.size (); ++index) {
        std::unique_ptr<request_result_js_payload_t> payload (batch[index]);
        request_js_state_t *state = payload->state;
        if (!env || !state || !state->handler) {
            release_request_state (NULL, state);
            continue;
        }
        napi_value handler;
        if (napi_get_reference_value (env, state->handler, &handler) != napi_ok) {
            release_request_state (env, state);
            continue;
        }

        napi_value argv[2];
        napi_create_int32 (env, payload->errnum, &argv[0]);
        if (payload->errnum != 0) {
            napi_get_null (env, &argv[1]);
        } else {
            napi_value parts_array;
            napi_create_array_with_length (env, payload->parts.size (), &parts_array);
            for (size_t part_index = 0; part_index < payload->parts.size (); ++part_index) {
                napi_value part_buf = create_message_data_buffer (
                  env, &payload->parts[part_index]);
                if (!part_buf)
                    break;
                napi_set_element (env, parts_array, static_cast<uint32_t> (part_index),
                                  part_buf);
            }
            argv[1] = parts_array;
        }

        napi_value recv;
        napi_value this_arg;
        napi_get_undefined (env, &this_arg);
        (void) napi_call_function (env, this_arg, handler, 2, argv, &recv);
        release_request_state (env, state);
    }

    if (reschedule
        && napi_call_threadsafe_function (dispatcher->tsfn, dispatcher,
                                          napi_tsfn_nonblocking)
             != napi_ok) {
        std::lock_guard<std::mutex> lock (dispatcher->queue_mu);
        dispatcher->scheduled = false;
    }
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
create_core_request_js_state (napi_env env, void *socket, napi_value handler)
{
    request_completion_dispatcher_t *dispatcher = request_dispatcher (env, socket);
    if (!dispatcher)
        return NULL;
    request_js_state_t *state = new request_js_state_t ();
    state->env = env;
    state->dispatcher = dispatcher;
    if (napi_create_reference (env, handler, 1, &state->handler) != napi_ok) {
        delete state;
        napi_throw_error (env, NULL, "request callback reference setup failed");
        return NULL;
    }
    dispatcher->active.fetch_add (1);
    return state;
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
    bool schedule = false;
    {
        std::lock_guard<std::mutex> lock (dispatcher->queue_mu);
        dispatcher->queue.push_back (payload.get ());
        payload.release ();
        if (!dispatcher->scheduled) {
            dispatcher->scheduled = true;
            schedule = true;
        }
    }
    if (schedule
        && napi_call_threadsafe_function (dispatcher->tsfn, dispatcher,
                                          napi_tsfn_nonblocking)
             != napi_ok) {
        std::lock_guard<std::mutex> lock (dispatcher->queue_mu);
        dispatcher->scheduled = false;
    }
}
