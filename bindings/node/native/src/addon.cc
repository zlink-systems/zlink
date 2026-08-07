/* SPDX-License-Identifier: MPL-2.0 */

#include "addon_exports.h"

static napi_value init (napi_env env, napi_value exports)
{
    define_core_exports (env, exports);
    return exports;
}

NAPI_MODULE (NODE_GYP_MODULE_NAME, init)
