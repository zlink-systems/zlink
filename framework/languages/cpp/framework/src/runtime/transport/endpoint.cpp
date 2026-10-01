/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include <zlink/framework/contracts/configuration/transport.hpp>
#include <zlink/framework/contracts/errors/error.hpp>

#include "runtime/transport/endpoint_notation.hpp"

#include <utility>

namespace zlink::framework
{

transport_endpoint_t::transport_endpoint_t (transport_scheme_t scheme, std::string uri) :
    _scheme (scheme), _uri (std::move (uri))
{
}

transport_endpoint_t transport_endpoint_t::parse (std::string uri)
{
    const auto scheme_end = uri.find ("://");
    if (scheme_end == std::string::npos) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "endpoint URI has no scheme");
    }

    const auto scheme =
      runtime::transport::parse_transport_scheme (std::string_view (uri).substr (0, scheme_end));
    if (scheme)
        return {*scheme, std::move (uri)};

    throw framework_exception_t (framework_error_kind_t::protocol_error,
                                 "unsupported endpoint URI scheme");
}

} // namespace zlink::framework
