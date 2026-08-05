/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include <zlink/framework/contracts/configuration/transport.hpp>
#include <zlink/framework/contracts/errors/error.hpp>

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

    const auto scheme = uri.substr (0, scheme_end);
    if (scheme == "tcp") {
        return {transport_scheme_t::tcp, std::move (uri)};
    }
    if (scheme == "ipc") {
        return {transport_scheme_t::ipc, std::move (uri)};
    }
    if (scheme == "tls") {
        return {transport_scheme_t::tls, std::move (uri)};
    }
    if (scheme == "ws") {
        return {transport_scheme_t::websocket, std::move (uri)};
    }
    if (scheme == "wss") {
        return {transport_scheme_t::websocket_tls, std::move (uri)};
    }

    throw framework_exception_t (framework_error_kind_t::protocol_error,
                                 "unsupported endpoint URI scheme");
}

} // namespace zlink::framework
