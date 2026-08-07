/* SPDX-License-Identifier: MPL-2.0 */

#pragma once

#include <node_api.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <string>
#include <vector>

#include "zlink.h"

napi_value throw_last_error (napi_env env, const char *prefix);
std::string get_string (napi_env env, napi_value val);
const char *get_c_string_arg (
  napi_env env, napi_value val, char *stack_buf, size_t stack_buf_size, std::string *heap_buf);
bool build_msg_vector (napi_env env, napi_value arr, std::vector<zlink_msg_t> *out);
bool build_msg_vector_or_single (napi_env env, napi_value value, std::vector<zlink_msg_t> *out);
bool init_msg_from_value (napi_env env, napi_value value, zlink_msg_t *msg);
void close_msg_vector (std::vector<zlink_msg_t> &parts);
void release_socket_send_ready_handler_slot (void *socket);
void release_socket_completion_control_handler_slot (void *socket);
void release_socket_monitor_handler_slot (void *monitor);

// Generic napi object-property setters shared across the addon translation
// units (message snapshots, discovery summaries, ...). Defined here so each
// caller pulls in one copy instead of redefining them.
inline void set_uint32_property (napi_env env, napi_value obj, const char *name, uint32_t value)
{
    napi_value out;
    napi_create_uint32 (env, value, &out);
    napi_set_named_property (env, obj, name, out);
}

inline void set_int64_property (napi_env env, napi_value obj, const char *name, int64_t value)
{
    napi_value out;
    napi_create_int64 (env, value, &out);
    napi_set_named_property (env, obj, name, out);
}

inline void set_uint64_bigint_property (napi_env env, napi_value obj, const char *name,
                                        uint64_t value)
{
    napi_value out;
    napi_create_bigint_uint64 (env, value, &out);
    napi_set_named_property (env, obj, name, out);
}

inline void set_string_property (napi_env env, napi_value obj, const char *name, const char *value)
{
    napi_value out;
    napi_create_string_utf8 (env, value ? value : "", NAPI_AUTO_LENGTH, &out);
    napi_set_named_property (env, obj, name, out);
}
