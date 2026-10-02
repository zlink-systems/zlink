/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/configuration/logging.hpp>

#include <array>
#include <cstddef>
#include <exception>
#include <optional>
#include <regex>
#include <typeinfo>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace zlink::framework::detail
{

class diagnostic_event_sink_t
{
  public:
    static constexpr std::size_t error_message_max_length = 512;

    struct exception_summary_t
    {
        std::string type;
        std::string message;
    };

    static exception_summary_t exception_summary (const std::exception_ptr &exception)
    {
        if (!exception)
            return {};
        exception_summary_t summary;
        try {
            std::rethrow_exception (exception);
        }
        catch (const std::exception &error) {
            summary.type = typeid (error).name ();
            summary.message = error.what ();
        }
        catch (...) {
            summary.type = "non-standard exception";
            summary.message = "non-standard exception";
        }
        const auto line_end = summary.message.find_first_of ("\r\n");
        if (line_end != std::string::npos)
            summary.message.resize (line_end);
        static const std::array<std::pair<std::regex, std::string>, 4> credential_patterns{
          std::pair{std::regex (R"(Authorization\s*:\s*(?:(?:Bearer|Basic)\s+)?[^\s,;]+)",
                                std::regex_constants::icase),
                    std::string ("Authorization: <redacted>")},
          std::pair{std::regex (R"(Bearer\s+[^\s,;]+)", std::regex_constants::icase),
                    std::string ("Bearer <redacted>")},
          std::pair{std::regex (R"(password\s*=\s*[^\s,;]+)", std::regex_constants::icase),
                    std::string ("password=<redacted>")},
          std::pair{std::regex (R"(token\s*=\s*[^\s,;]+)", std::regex_constants::icase),
                    std::string ("token=<redacted>")}};
        for (const auto &[pattern, replacement] : credential_patterns) {
            summary.message = std::regex_replace (summary.message, pattern, replacement);
        }
        if (summary.message.size () > error_message_max_length)
            summary.message.resize (error_message_max_length);
        return summary;
    }

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
