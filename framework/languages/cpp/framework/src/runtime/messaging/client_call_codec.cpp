/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include <zlink/framework/detail/binary_text_codec.hpp>

#include "runtime/messaging/client_call_codec.hpp"

#include <atomic>
#include <charconv>
#include <ctime>

namespace zlink::framework::runtime::messaging
{

namespace
{

std::string next_correlation_id ()
{
    static std::atomic_uint64_t next{1};
    std::uint64_t value = next.fetch_add (1, std::memory_order_relaxed);
    // Cheap uint->hex (no ostringstream): runs per outbound envelope.
    char buffer[17];
    int index = static_cast<int> (sizeof (buffer));
    buffer[--index] = '\0';
    do {
        buffer[--index] = zlink::framework::detail::lowercase_hex_digits[value & 0xfu];
        value >>= 4u;
    } while (value != 0);
    return std::string (buffer + index);
}

std::string format_utc_deadline (std::chrono::system_clock::time_point deadline)
{
    const auto seconds = std::chrono::time_point_cast<std::chrono::seconds> (deadline);
    const std::time_t time = std::chrono::system_clock::to_time_t (seconds);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s (&tm, &time);
#else
    gmtime_r (&time, &tm);
#endif
    char output[64];
    const auto size = std::strftime (output, sizeof (output), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return std::string (output, size);
}

} // namespace

std::optional<std::chrono::system_clock::time_point> parse_utc_deadline (std::string_view value)
{
    if (value.size () < 20 || value[4] != '-' || value[7] != '-' || value[10] != 'T'
        || value[13] != ':' || value[16] != ':')
        return std::nullopt;
    const auto number = [value] (std::size_t start, std::size_t size) -> std::optional<unsigned> {
        unsigned parsed = 0;
        const auto first = value.data () + start;
        const auto last = first + size;
        const auto result = std::from_chars (first, last, parsed);
        if (result.ec != std::errc{} || result.ptr != last)
            return std::nullopt;
        return parsed;
    };
    const auto year = number (0, 4);
    const auto month = number (5, 2);
    const auto day = number (8, 2);
    const auto hour = number (11, 2);
    const auto minute = number (14, 2);
    const auto second = number (17, 2);
    if (!year || !month || !day || !hour || !minute || !second || *hour > 23 || *minute > 59
        || *second > 59)
        return std::nullopt;
    const auto date =
      std::chrono::year_month_day{std::chrono::year{static_cast<int> (*year)},
                                  std::chrono::month{*month}, std::chrono::day{*day}};
    if (!date.ok ())
        return std::nullopt;
    std::size_t cursor = 19;
    std::uint32_t fraction = 0;
    unsigned fraction_digits = 0;
    if (value[cursor] == '.') {
        ++cursor;
        while (cursor < value.size () && value[cursor] >= '0' && value[cursor] <= '9') {
            if (fraction_digits < 9) {
                fraction = fraction * 10 + static_cast<unsigned> (value[cursor] - '0');
                ++fraction_digits;
            }
            ++cursor;
        }
        if (fraction_digits == 0)
            return std::nullopt;
        while (fraction_digits < 9) {
            fraction *= 10;
            ++fraction_digits;
        }
    }
    std::chrono::minutes offset{0};
    if (cursor + 1 == value.size () && value[cursor] == 'Z') {
        // UTC designator.
    } else if (cursor + 6 == value.size () && (value[cursor] == '+' || value[cursor] == '-')
               && value[cursor + 3] == ':') {
        const auto offset_hour = number (cursor + 1, 2);
        const auto offset_minute = number (cursor + 4, 2);
        if (!offset_hour || !offset_minute || *offset_hour > 23 || *offset_minute > 59)
            return std::nullopt;
        offset = std::chrono::minutes{*offset_hour * 60 + *offset_minute};
        if (value[cursor] == '-')
            offset = -offset;
    } else {
        return std::nullopt;
    }
    // system_clock::duration is coarser than nanoseconds on MSVC and libc++.
    return std::chrono::floor<std::chrono::system_clock::duration> (
      std::chrono::sys_days{date} + std::chrono::hours{*hour} + std::chrono::minutes{*minute}
      + std::chrono::seconds{*second} + std::chrono::nanoseconds{fraction} - offset);
}

envelope_header_t client_call_codec_t::create_envelope (message_kind_t kind,
                                                        std::string channel_name,
                                                        std::string message_name,
                                                        std::chrono::milliseconds timeout,
                                                        std::optional<std::string> topic,
                                                        std::optional<std::string> source) const
{
    envelope_header_t header;
    header.kind = kind;
    header.channel_name = std::move (channel_name);
    header.message_name = std::move (message_name);
    header.content_type = envelope_codec_t::default_content_type;
    header.correlation_id = next_correlation_id ();
    if (timeout.count () > 0) {
        header.deadline = format_utc_deadline (std::chrono::system_clock::now () + timeout);
    }
    header.topic = std::move (topic);
    header.source = std::move (source);
    return header;
}

} // namespace zlink::framework::runtime::messaging
