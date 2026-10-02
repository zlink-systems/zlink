/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/errors/result.hpp>
#include <zlink/Contracts/Messaging/request_result.hpp>
#include "runtime/foundation/operation_terminal.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <optional>

namespace zlink::framework::runtime::messaging
{

enum class request_result_t
{
    timed_out,
    not_connected,
    not_found,
    rejected,
    conflict,
    busy,
    protocol_error,
    invalid_argument,
    invalid_state,
    not_supported,
    terminated,
    internal_error
};

struct request_wire_failure_t
{
    std::uint32_t terminal_result;
    std::uint32_t failure_code;
};

class request_failure_mapper_t
{
  public:
    std::optional<foundation::operation_terminal_t>
    transport_terminal (zlink::request_result_t terminal) const noexcept;
    std::uint32_t reply_failure_code (std::uint32_t terminal_result) const noexcept;
    framework_exception_t completion_exception (request_result_t result,
                                                const std::string &operation_name) const;
    framework_exception_t error_header_exception (const std::string &error_code,
                                                  const std::string &error_message,
                                                  const std::string &operation_name) const;
    framework_exception_t reply_header_exception (std::uint32_t terminal_result,
                                                  std::uint32_t failure_code,
                                                  const std::string &operation_name) const;
    // Encodes a target Framework failure; source/native completion uses its own mapper.
    // A missing value means the closed wire contract cannot represent this ErrorKind.
    std::optional<request_wire_failure_t> target_failure_reply (framework_error_kind_t kind) const;
};

} // namespace zlink::framework::runtime::messaging
