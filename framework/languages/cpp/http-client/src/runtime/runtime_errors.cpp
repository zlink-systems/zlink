/* SPDX-License-Identifier: Apache-2.0 */

#include "runtime/runtime_errors.hpp"

namespace zlink::http_client::detail
{

zlink::framework::framework_exception_t request_error (const std::string &message)
{
    return zlink::framework::framework_exception_t (
      zlink::framework::framework_error_kind_t::internal_failure, message);
}

zlink::framework::result_t<raw_http_response_t> timeout_before_exchange ()
{
    return zlink::framework::detail::boundary_failure<raw_http_response_t> (zlink::framework::detail::boundary_error_t::timed_out,
      "HTTP request timed out before the scheduler started it");
}

//  Transport-layer failures (socket/TLS/wire errors) are retriable; anything else is an
//  unexpected error and must not be retried (retrying a programming error is never correct).
zlink::framework::result_t<raw_http_response_t> map_transport_exception (const std::exception &ex)
{
    return zlink::framework::result_t<raw_http_response_t>::failure (
      zlink::framework::framework_error_kind_t::unavailable, ex.what ());
}

zlink::framework::result_t<raw_http_response_t> map_unexpected_exception (const std::exception &ex)
{
    return zlink::framework::result_t<raw_http_response_t>::failure (
      zlink::framework::framework_error_kind_t::internal_failure, ex.what ());
}

} // namespace zlink::http_client::detail
