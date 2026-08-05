/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/errors/result.hpp>

#include <cstdint>
#include <string>

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

class request_failure_mapper_t
{
  public:
    framework_exception_t completion_exception (request_result_t result,
                                                const std::string &operation_name) const;
    framework_exception_t error_header_exception (const std::string &error_code,
                                                  const std::string &error_message,
                                                  const std::string &operation_name) const;
    framework_exception_t reply_header_exception (
      std::uint32_t terminal_result,
      std::uint32_t failure_code,
      const std::string &operation_name) const;
};

} // namespace zlink::framework::runtime::messaging
