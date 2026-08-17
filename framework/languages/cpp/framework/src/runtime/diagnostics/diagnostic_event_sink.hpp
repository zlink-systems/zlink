/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/configuration/logging.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace zlink::framework::detail
{

class diagnostic_event_sink_t
{
  public:
    static void append_field (std::vector<log_field_t> &fields, const char *key, std::string value)
    {
        fields.push_back (log_field_t{key, std::move (value)});
    }

    static void log_if_configured (const std::optional<logger_t<>> &logger,
                                   log_level_t level,
                                   std::string_view message,
                                   std::vector<log_field_t> fields) noexcept
    {
        if (logger)
            logger->log_with_fields (level, std::string (message), std::move (fields));
    }
};

} // namespace zlink::framework::detail
