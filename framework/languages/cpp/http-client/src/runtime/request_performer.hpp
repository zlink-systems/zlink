/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "runtime/http_client_runtime.hpp"

namespace zlink::http_client::detail
{

zlink::framework::result_t<raw_http_response_t> perform_once (const http_client_options_t &options,
                                                              cookie_jar_t &cookie_jar,
                                                              connection_pool_t &pool,
                                                              const http_request_t &request);

} // namespace zlink::http_client::detail
