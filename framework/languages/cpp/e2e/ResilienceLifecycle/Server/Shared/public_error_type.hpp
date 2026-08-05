/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework.hpp>

#include <string>
#include <system_error>

namespace zlink::framework::e2e::resilience_lifecycle
{

template <typename T> std::string public_error_type (const result_t<T> &result)
{
    if (const auto *error = result.error (); error != nullptr
        && error->code () == std::make_error_code (std::errc::timed_out)) {
        return "TimeoutException";
    }
    switch (result.error_kind ()) {
        case framework_error_kind_t::unavailable:
            return "Unavailable";
        case framework_error_kind_t::not_found:
            return "NotFound";
        case framework_error_kind_t::rejected:
            return "Rejected";
        case framework_error_kind_t::internal_failure:
            return "InternalFailure";
        default:
            return "UnexpectedFrameworkError";
    }
}

} // namespace zlink::framework::e2e::resilience_lifecycle
