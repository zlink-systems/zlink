/* SPDX-License-Identifier: MPL-2.0 */

#pragma once

#include "addon_common_api.h"

struct request_js_state_t;

request_js_state_t *create_core_request_js_state (napi_env env, napi_value handler);
void abort_request_js_state (request_js_state_t *state);

void request_reply_callback_trampoline (zlink_request_result_t errnum,
                                        zlink_msg_t *parts,
                                        size_t part_count,
                                        void *userdata);
