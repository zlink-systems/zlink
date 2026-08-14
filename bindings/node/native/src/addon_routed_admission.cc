/* SPDX-License-Identifier: MPL-2.0 */

#include "addon_core_api.h"
#include "addon_message_parts.h"
#include "addon_message_values.h"
#include "addon_request_callbacks.h"
#include "addon_tsfn_slots.h"

#include <atomic>
#include <cstring>
#include <errno.h>
#include <memory>
#include <mutex>
#include <new>
#include <unordered_map>
#include <vector>

namespace
{

struct routed_ready_payload_t
{
    routed_ready_payload_t () : peer_rid (NULL), peer_rid_size (0) {}
    ~routed_ready_payload_t () { delete[] peer_rid; }

    unsigned char *peer_rid;
    size_t peer_rid_size;
    uint64_t pair_id;
    uint64_t pair_generation;
    int state;
    int terminal_errno;
};

struct routed_ready_state_t
{
    routed_ready_state_t ()
        : socket (NULL), tsfn (NULL), closing (false)
    {
    }

    void *socket;
    napi_threadsafe_function tsfn;
    std::atomic<bool> closing;
    std::mutex attempt_gate;
};

std::mutex g_ready_states_mu;
std::unordered_map<void *, routed_ready_state_t *> g_ready_states;

routed_ready_state_t *ready_state (void *socket)
{
    std::lock_guard<std::mutex> lock (g_ready_states_mu);
    std::unordered_map<void *, routed_ready_state_t *>::iterator entry =
      g_ready_states.find (socket);
    return entry == g_ready_states.end () ? NULL : entry->second;
}

bool get_uint64_bigint (napi_env env,
                        napi_value value,
                        uint64_t *out,
                        const char *name)
{
    bool lossless = false;
    if (napi_get_value_bigint_uint64 (env, value, out, &lossless) == napi_ok
        && lossless)
        return true;
    napi_throw_type_error (env, NULL, name);
    return false;
}

napi_value create_attempt_result (napi_env env, int result, int native_errno)
{
    napi_value out;
    napi_create_object (env, &out);
    napi_value value;
    napi_create_int32 (env, result, &value);
    napi_set_named_property (env, out, "result", value);
    napi_create_int32 (env, native_errno, &value);
    napi_set_named_property (env, out, "nativeErrno", value);
    return out;
}

napi_value create_target_result (napi_env env,
                                 int result,
                                 int native_errno,
                                 const zlink_routed_submit_target_t *target)
{
    napi_value out = create_attempt_result (env, result, native_errno);
    if (result != ZLINK_SUBMIT_OK || !target)
        return out;
    napi_value rid = create_routing_id_value (env, target->peer_rid);
    napi_set_named_property (env, out, "peerRid", rid);
    set_uint64_bigint_property (
      env, out, "transportPairId", target->transport_pair_id);
    set_uint64_bigint_property (
      env, out, "transportPairGeneration",
      target->transport_pair_generation);
    return out;
}

bool parse_exact_target (napi_env env,
                         napi_value *argv,
                         void **socket,
                         zlink_routed_submit_target_t *target)
{
    napi_get_value_external (env, argv[0], socket);
    if (!*socket || !parse_routing_id_value (env, argv[1], &target->peer_rid))
        return false;
    if (!get_uint64_bigint (
          env, argv[2], &target->transport_pair_id,
          "transportPairId must be a lossless bigint")
        || !get_uint64_bigint (
          env, argv[3], &target->transport_pair_generation,
          "transportPairGeneration must be a lossless bigint"))
        return false;
    if (target->transport_pair_id == 0
        || target->transport_pair_generation == 0) {
        napi_throw_range_error (
          env, NULL, "transport pair identity must be non-zero");
        return false;
    }
    return true;
}

void routed_ready_finalize (napi_env env,
                            void *finalize_data,
                            void *finalize_hint)
{
    (void) env;
    (void) finalize_hint;
    routed_ready_state_t *state =
      static_cast<routed_ready_state_t *> (finalize_data);
    if (!state)
        return;
    {
        std::lock_guard<std::mutex> lock (g_ready_states_mu);
        if (state->socket) {
            std::unordered_map<void *, routed_ready_state_t *>::iterator entry =
              g_ready_states.find (state->socket);
            if (entry != g_ready_states.end () && entry->second == state)
                g_ready_states.erase (entry);
        }
    }
    delete state;
}

void routed_ready_call_js (napi_env env,
                           napi_value js_callback,
                           void *context,
                           void *data)
{
    (void) context;
    std::unique_ptr<routed_ready_payload_t> payload (
      static_cast<routed_ready_payload_t *> (data));
    if (!env || !js_callback || !payload)
        return;

    napi_value event;
    napi_create_object (env, &event);
    napi_value rid;
    napi_create_buffer_copy (
      env, payload->peer_rid_size, payload->peer_rid, NULL, &rid);
    napi_set_named_property (env, event, "peerRid", rid);
    set_uint64_bigint_property (
      env, event, "transportPairId", payload->pair_id);
    set_uint64_bigint_property (
      env, event, "transportPairGeneration", payload->pair_generation);
    napi_value value;
    napi_create_int32 (env, payload->state, &value);
    napi_set_named_property (env, event, "state", value);
    napi_create_int32 (env, payload->terminal_errno, &value);
    napi_set_named_property (env, event, "terminalErrno", value);

    napi_value ignored;
    napi_value this_arg;
    napi_get_undefined (env, &this_arg);
    (void) napi_call_function (
      env, this_arg, js_callback, 1, &event, &ignored);
}

void routed_ready_callback (void *subject,
                            const zlink_routed_send_ready_event_t *event,
                            void *userdata)
{
    (void) subject;
    routed_ready_state_t *state = static_cast<routed_ready_state_t *> (userdata);
    if (!state || !event || state->closing.load (std::memory_order_acquire))
        return;

    napi_threadsafe_function tsfn = NULL;
    {
        std::lock_guard<std::mutex> lock (g_ready_states_mu);
        if (!state->closing.load (std::memory_order_relaxed))
            tsfn = state->tsfn;
    }
    if (!tsfn)
        return;

    std::unique_ptr<routed_ready_payload_t> payload (
      new (std::nothrow) routed_ready_payload_t ());
    if (!payload)
        return;
    if (event->peer_rid.size > 0) {
        payload->peer_rid =
          new (std::nothrow) unsigned char[event->peer_rid.size];
        if (!payload->peer_rid)
            return;
        payload->peer_rid_size = event->peer_rid.size;
        std::memcpy (
          payload->peer_rid, event->peer_rid.data, event->peer_rid.size);
    }
    payload->pair_id = event->transport_pair_id;
    payload->pair_generation = event->transport_pair_generation;
    payload->state = static_cast<int> (event->state);
    payload->terminal_errno = event->terminal_errno;
    if (napi_call_threadsafe_function (
          tsfn, payload.get (), napi_tsfn_nonblocking)
        == napi_ok)
        payload.release ();
}

int dealer_send_exact (void *dealer,
                       const zlink_routed_submit_target_t *target,
                       zlink_msg_t *parts,
                       size_t part_count)
{
    routed_ready_state_t *state = ready_state (dealer);
    if (!state) {
        errno = EFAULT;
        return ZLINK_SUBMIT_INVALID_HANDLE;
    }
    std::lock_guard<std::mutex> attempt (state->attempt_gate);
    return submit_msg_parts (
      parts, part_count,
      [dealer, target] (zlink_msg_t *part, zlink_part_flag_t part_flag, bool) {
          return zlink_dealer_send_transport_pair_part (
            dealer, target, part, ZLINK_SEND_FLAGS_DONTWAIT, part_flag);
      });
}

int router_send_exact (void *router,
                       const zlink_routed_submit_target_t *target,
                       zlink_msg_t *parts,
                       size_t part_count)
{
    routed_ready_state_t *state = ready_state (router);
    if (!state) {
        errno = EFAULT;
        return ZLINK_SUBMIT_INVALID_HANDLE;
    }
    std::lock_guard<std::mutex> attempt (state->attempt_gate);
    return submit_msg_parts (
      parts, part_count,
      [router, target] (zlink_msg_t *part, zlink_part_flag_t part_flag, bool) {
          return zlink_send_part_transport_pair (
            router, &target->peer_rid, target->transport_pair_id,
            target->transport_pair_generation, part,
            ZLINK_SEND_FLAGS_DONTWAIT, part_flag);
      });
}

int stream_send_exact (void *stream,
                       const zlink_routed_submit_target_t *target,
                       zlink_msg_t *parts,
                       size_t part_count)
{
    routed_ready_state_t *state = ready_state (stream);
    if (!state) {
        errno = EFAULT;
        return ZLINK_SUBMIT_INVALID_HANDLE;
    }
    std::lock_guard<std::mutex> attempt (state->attempt_gate);
    return submit_msg_parts (
      parts, part_count,
      [stream, target] (zlink_msg_t *part, zlink_part_flag_t part_flag, bool) {
          return zlink_send_part_transport_pair (
            stream, &target->peer_rid, target->transport_pair_id,
            target->transport_pair_generation, part,
            ZLINK_SEND_FLAGS_DONTWAIT, part_flag);
      });
}

int dealer_request_exact (void *dealer,
                          const zlink_routed_submit_target_t *target,
                          zlink_msg_t *parts,
                          size_t part_count,
                          uint32_t timeout_ms,
                          request_js_state_t *state)
{
    routed_ready_state_t *ready = ready_state (dealer);
    if (!ready) {
        errno = EFAULT;
        return ZLINK_SUBMIT_INVALID_HANDLE;
    }
    std::lock_guard<std::mutex> attempt (ready->attempt_gate);
    return submit_msg_parts (
      parts, part_count,
      [dealer, target, timeout_ms, state] (
        zlink_msg_t *part, zlink_part_flag_t part_flag, bool is_final) {
          return zlink_dealer_request_transport_pair_part (
            dealer, target, part, ZLINK_SEND_FLAGS_DONTWAIT, part_flag,
            is_final ? timeout_ms : 0u,
            is_final ? request_reply_callback_trampoline : NULL,
            is_final ? state : NULL);
      });
}

int router_request_exact (void *router,
                          const zlink_routed_submit_target_t *target,
                          zlink_msg_t *parts,
                          size_t part_count,
                          uint32_t timeout_ms,
                          request_js_state_t *state)
{
    routed_ready_state_t *ready = ready_state (router);
    if (!ready) {
        errno = EFAULT;
        return ZLINK_SUBMIT_INVALID_HANDLE;
    }
    std::lock_guard<std::mutex> attempt (ready->attempt_gate);
    return submit_msg_parts (
      parts, part_count,
      [router, target, timeout_ms, state] (
        zlink_msg_t *part, zlink_part_flag_t part_flag, bool is_final) {
          return zlink_router_request_transport_pair_part (
            router, &target->peer_rid, target->transport_pair_id,
            target->transport_pair_generation, part,
            ZLINK_SEND_FLAGS_DONTWAIT, part_flag,
            is_final ? timeout_ms : 0u,
            is_final ? request_reply_callback_trampoline : NULL,
            is_final ? state : NULL);
      });
}

napi_value routed_send_attempt (napi_env env,
                                napi_callback_info info,
                                int role)
{
    napi_value argv[5];
    size_t argc = 5;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    if (argc < 5) {
        napi_throw_type_error (
          env, NULL,
          "routed send attempt requires (socket, routingId, pairId, pairGeneration, parts)");
        return NULL;
    }
    void *socket = NULL;
    zlink_routed_submit_target_t target;
    if (!parse_exact_target (env, argv, &socket, &target))
        return NULL;
    std::vector<zlink_msg_t> parts;
    if (!build_msg_vector_or_single (env, argv[4], &parts))
        return NULL;
    const int result = role == 0
      ? dealer_send_exact (socket, &target, parts.data (), parts.size ())
      : role == 1
        ? router_send_exact (socket, &target, parts.data (), parts.size ())
        : stream_send_exact (socket, &target, parts.data (), parts.size ());
    parts.clear ();
    const int native_errno = result == ZLINK_SUBMIT_OK ? 0 : zlink_errno ();
    return create_attempt_result (env, result, native_errno);
}

napi_value routed_request_attempt (napi_env env,
                                   napi_callback_info info,
                                   bool dealer)
{
    napi_value argv[7];
    size_t argc = 7;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    if (argc < 7) {
        napi_throw_type_error (
          env, NULL,
          "routed request attempt requires (socket, routingId, pairId, pairGeneration, parts, token, timeoutMs)");
        return NULL;
    }
    void *socket = NULL;
    zlink_routed_submit_target_t target;
    if (!parse_exact_target (env, argv, &socket, &target))
        return NULL;
    std::vector<zlink_msg_t> parts;
    if (!build_msg_vector_or_single (env, argv[4], &parts))
        return NULL;
    uint64_t token = 0;
    if (!get_uint64_bigint (
          env, argv[5], &token, "request token must be a lossless bigint")) {
        close_msg_vector (parts);
        return NULL;
    }
    int32_t timeout_ms = 0;
    if (napi_get_value_int32 (env, argv[6], &timeout_ms) != napi_ok
        || timeout_ms <= 0) {
        close_msg_vector (parts);
        napi_throw_range_error (env, NULL, "request timeout must be positive");
        return NULL;
    }

    request_js_state_t *state =
      create_core_request_js_state (env, socket, token);
    if (!state) {
        close_msg_vector (parts);
        return NULL;
    }
    const int result = dealer
      ? dealer_request_exact (
          socket, &target, parts.data (), parts.size (),
          static_cast<uint32_t> (timeout_ms), state)
      : router_request_exact (
          socket, &target, parts.data (), parts.size (),
          static_cast<uint32_t> (timeout_ms), state);
    parts.clear ();
    const int native_errno = result == ZLINK_SUBMIT_OK ? 0 : zlink_errno ();
    if (result != ZLINK_SUBMIT_OK)
        abort_request_js_state (state);
    return create_attempt_result (env, result, native_errno);
}

} // namespace

napi_value socket_routed_send_ready_handler (napi_env env,
                                             napi_callback_info info)
{
    napi_value argv[2];
    size_t argc = 2;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    if (argc < 2) {
        napi_throw_type_error (
          env, NULL, "routed ready handler requires (socket, handler)");
        return NULL;
    }
    void *socket = NULL;
    napi_get_value_external (env, argv[0], &socket);
    napi_valuetype handler_type = napi_undefined;
    napi_typeof (env, argv[1], &handler_type);
    if (!socket || handler_type != napi_function) {
        napi_throw_type_error (env, NULL, "routed ready handler is invalid");
        return NULL;
    }

    routed_ready_state_t *state = new (std::nothrow) routed_ready_state_t ();
    if (!state) {
        napi_throw_error (env, NULL, "routed ready handler allocation failed");
        return NULL;
    }
    state->socket = socket;
    {
        std::lock_guard<std::mutex> lock (g_ready_states_mu);
        if (g_ready_states.find (socket) != g_ready_states.end ()) {
            delete state;
            napi_throw_error (env, NULL, "routed ready handler already attached");
            return NULL;
        }
        g_ready_states[socket] = state;
    }

    napi_threadsafe_function tsfn = NULL;
    if (!create_tsfn_slot_queue (
          env, argv[1], state, "zlink-routed-ready-handler",
          routed_ready_finalize, routed_ready_call_js,
          "routed ready handler callback queue setup failed", true, &tsfn)) {
        std::lock_guard<std::mutex> lock (g_ready_states_mu);
        g_ready_states.erase (socket);
        delete state;
        return NULL;
    }
    state->tsfn = tsfn;
    const zlink_handler_result_t result = zlink_routed_send_ready_handler (
      socket, routed_ready_callback, state);
    if (result != ZLINK_HANDLER_OK) {
        {
            std::lock_guard<std::mutex> lock (g_ready_states_mu);
            g_ready_states.erase (socket);
            state->socket = NULL;
            state->closing.store (true, std::memory_order_release);
        }
        (void) napi_release_threadsafe_function (tsfn, napi_tsfn_abort);
        return throw_last_error (env, "routed ready handler failed");
    }

    napi_value out;
    napi_get_undefined (env, &out);
    return out;
}

napi_value socket_routed_admission_close (napi_env env,
                                          napi_callback_info info)
{
    napi_value argv[1];
    size_t argc = 1;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *socket = NULL;
    if (argc >= 1)
        napi_get_value_external (env, argv[0], &socket);
    routed_ready_state_t *state = NULL;
    {
        std::lock_guard<std::mutex> lock (g_ready_states_mu);
        std::unordered_map<void *, routed_ready_state_t *>::iterator entry =
          g_ready_states.find (socket);
        if (entry != g_ready_states.end ()) {
            state = entry->second;
            g_ready_states.erase (entry);
            state->socket = NULL;
            state->closing.store (true, std::memory_order_release);
        }
    }
    if (state && state->tsfn)
        (void) napi_release_threadsafe_function (
          state->tsfn, napi_tsfn_abort);
    napi_value out;
    napi_get_undefined (env, &out);
    return out;
}

napi_value socket_routed_admission_begin_close (napi_env env,
                                                napi_callback_info info)
{
    napi_value argv[1];
    size_t argc = 1;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    void *socket = NULL;
    if (argc >= 1)
        napi_get_value_external (env, argv[0], &socket);
    {
        std::lock_guard<std::mutex> lock (g_ready_states_mu);
        std::unordered_map<void *, routed_ready_state_t *>::iterator entry =
          g_ready_states.find (socket);
        if (entry != g_ready_states.end ())
            entry->second->closing.store (true, std::memory_order_release);
    }
    napi_value out;
    napi_get_undefined (env, &out);
    return out;
}

napi_value socket_select_routed_submit_target (napi_env env,
                                               napi_callback_info info)
{
    napi_value argv[2];
    size_t argc = 2;
    napi_get_cb_info (env, info, &argc, argv, NULL, NULL);
    if (argc < 2) {
        napi_throw_type_error (
          env, NULL, "target selection requires (socket, routingIdOrNull)");
        return NULL;
    }
    void *socket = NULL;
    napi_get_value_external (env, argv[0], &socket);
    if (!socket) {
        napi_throw_type_error (env, NULL, "target selection socket is invalid");
        return NULL;
    }
    zlink_routing_id_t rid;
    const zlink_routing_id_t *rid_ptr = NULL;
    napi_value null_value;
    bool is_null = false;
    napi_get_null (env, &null_value);
    napi_strict_equals (env, argv[1], null_value, &is_null);
    if (!is_null) {
        if (!parse_routing_id_value (env, argv[1], &rid))
            return NULL;
        rid_ptr = &rid;
    }
    zlink_routed_submit_target_t target;
    const int result =
      zlink_select_routed_submit_target (socket, rid_ptr, &target);
    const int native_errno = result == ZLINK_SUBMIT_OK ? 0 : zlink_errno ();
    return create_target_result (
      env, result, native_errno,
      result == ZLINK_SUBMIT_OK ? &target : NULL);
}

napi_value dealer_routed_send_attempt (napi_env env,
                                       napi_callback_info info)
{
    return routed_send_attempt (env, info, 0);
}

napi_value router_routed_send_attempt (napi_env env,
                                       napi_callback_info info)
{
    return routed_send_attempt (env, info, 1);
}

napi_value stream_routed_send_attempt (napi_env env,
                                       napi_callback_info info)
{
    return routed_send_attempt (env, info, 2);
}

napi_value dealer_routed_request_attempt (napi_env env,
                                          napi_callback_info info)
{
    return routed_request_attempt (env, info, true);
}

napi_value router_routed_request_attempt (napi_env env,
                                          napi_callback_info info)
{
    return routed_request_attempt (env, info, false);
}
