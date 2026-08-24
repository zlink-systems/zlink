/* SPDX-License-Identifier: MPL-2.0 */

#include <zlink/Contracts/Sockets/socket_contracts.hpp>

#include <Runtime/Sockets/socket_access.hpp>

#include <zlink.h>

namespace zlink
{

void proxy (socket_t &frontend_, socket_t &backend_)
{
    detail::throw_if_failed<config_error_t> (static_cast<config_result_t> (
      zlink_proxy (detail::native_handle (frontend_), detail::native_handle (backend_), nullptr)));
}

void proxy (socket_t &frontend_, socket_t &backend_, socket_t &capture_)
{
    detail::throw_if_failed<config_error_t> (static_cast<config_result_t> (
      zlink_proxy (detail::native_handle (frontend_), detail::native_handle (backend_),
                   detail::native_handle (capture_))));
}

} // namespace zlink
