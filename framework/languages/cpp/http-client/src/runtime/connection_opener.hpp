/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "runtime/connection_pool.hpp"
#include "runtime/http_client_runtime.hpp"
#include "runtime/url.hpp"

#include <chrono>
#include <memory>

namespace zlink::http_client::detail
{

std::unique_ptr<pooled_connection_t>
open_connection (const http_client_options_t &options,
                 const hop_target_t &hop,
                 std::chrono::milliseconds timeout);

} // namespace zlink::http_client::detail
