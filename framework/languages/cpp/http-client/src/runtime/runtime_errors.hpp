/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include <zlink/http_client/contracts/client.hpp>

#include <exception>
#include <string>

namespace zlink::http_client::detail
{

zlink::framework::framework_exception_t request_error (const std::string &message);
zlink::framework::result_t<raw_http_response_t> timeout_before_exchange ();
zlink::framework::result_t<raw_http_response_t> map_transport_exception (const std::exception &ex);
zlink::framework::result_t<raw_http_response_t> map_unexpected_exception (const std::exception &ex);

} // namespace zlink::http_client::detail
