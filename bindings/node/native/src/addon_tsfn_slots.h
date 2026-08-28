/* SPDX-License-Identifier: MPL-2.0 */

#pragma once

#include "addon_common_api.h"

#include <mutex>

template <typename Slot> inline Slot *find_free_tsfn_slot (Slot *slots, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        if (!slots[i].used)
            return &slots[i];
    }
    return NULL;
}

template <typename Slot, typename Subject>
inline Slot *
find_tsfn_slot_by_subject (Slot *slots, size_t count, Subject Slot::*member, Subject subject)
{
    for (size_t i = 0; i < count; ++i) {
        if (slots[i].used && slots[i].*member == subject)
            return &slots[i];
    }
    return NULL;
}

template <typename Slot> inline void reset_tsfn_slot_base (Slot *state)
{
    if (!state)
        return;
    state->used = false;
    state->env = NULL;
    state->tsfn = NULL;
}

template <typename Slot, typename Subject>
inline Slot *reserve_tsfn_subject_slot (napi_env env,
                                        std::mutex &mu,
                                        Slot *slots,
                                        size_t count,
                                        Subject Slot::*subject_member,
                                        Subject subject,
                                        const char *already_attached_error,
                                        const char *full_error,
                                        size_t *slot_index)
{
    std::lock_guard<std::mutex> lock (mu);
    if (find_tsfn_slot_by_subject (slots, count, subject_member, subject)) {
        napi_throw_error (env, NULL, already_attached_error);
        return NULL;
    }

    Slot *slot = find_free_tsfn_slot (slots, count);
    if (!slot) {
        napi_throw_error (env, NULL, full_error);
        return NULL;
    }
    if (slot_index)
        *slot_index = static_cast<size_t> (slot - slots);
    return slot;
}

inline bool create_tsfn_slot_queue (napi_env env,
                                    napi_value handler,
                                    void *state,
                                    const char *resource_name_text,
                                    napi_finalize finalize,
                                    napi_threadsafe_function_call_js call_js,
                                    const char *setup_error,
                                    bool unref,
                                    napi_threadsafe_function *out)
{
    napi_value resource_name;
    napi_create_string_utf8 (env, resource_name_text, NAPI_AUTO_LENGTH, &resource_name);
    napi_threadsafe_function tsfn = NULL;
    napi_status status = napi_create_threadsafe_function (
      env, handler, NULL, resource_name, 0, 1, state, finalize, state, call_js, &tsfn);
    if (status != napi_ok) {
        napi_throw_error (env, NULL, setup_error);
        return false;
    }
    if (unref)
        (void) napi_unref_threadsafe_function (env, tsfn);
    *out = tsfn;
    return true;
}

template <typename Slot, typename Subject>
inline void bind_tsfn_subject_slot_unsafe (Slot *slot,
                                           Subject Slot::*subject_member,
                                           Subject subject,
                                           napi_env env,
                                           napi_threadsafe_function tsfn)
{
    slot->used = true;
    slot->*subject_member = subject;
    slot->env = env;
    slot->tsfn = tsfn;
}
