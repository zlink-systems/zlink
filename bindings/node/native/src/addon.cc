/* SPDX-License-Identifier: MPL-2.0 */

#include "addon_exports.h"
#include "addon_core_api.h"

static napi_value init (napi_env env, napi_value exports)
{
    if (!init_shared_context_env (env))
        return NULL;
    define_core_exports (env, exports);
    return exports;
}

NAPI_MODULE (NODE_GYP_MODULE_NAME, init)
