/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "runtime/foundation/operation_registry.hpp"

#include <zlink/framework/contracts/errors/error.hpp>

#include <string>
#include <utility>

namespace zlink::framework::runtime::client_server
{

inline framework_exception_t client_server_operation_exception (
  foundation::operation_terminal_t terminal,
  std::string operation)
{
    switch (terminal) {
        case foundation::operation_terminal_t::timed_out:
            return detail::make_boundary_exception (
              detail::boundary_error_t::timed_out,
              std::move (operation) + " timed out");
        case foundation::operation_terminal_t::cancelled:
            return detail::make_boundary_exception (
              detail::boundary_error_t::cancelled,
              std::move (operation) + " was cancelled");
        case foundation::operation_terminal_t::transport_failed:
            return detail::make_boundary_exception (
              detail::boundary_error_t::disconnected,
              std::move (operation) + " lost its connection");
        case foundation::operation_terminal_t::shutdown:
            return detail::make_boundary_exception (
              detail::boundary_error_t::shutdown,
              std::move (operation) + " stopped because the runtime is shutting down");
        case foundation::operation_terminal_t::completed:
            break;
    }
    return framework_exception_t (
      framework_error_kind_t::internal_failure,
      std::move (operation) + " did not complete");
}

} // namespace zlink::framework::runtime::client_server
