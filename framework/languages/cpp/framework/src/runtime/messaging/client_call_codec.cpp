/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

#include <zlink/framework/detail/binary_text_codec.hpp>

#include "runtime/messaging/client_call_codec.hpp"

#include <atomic>
#include <ctime>

namespace zlink::framework::runtime::messaging
{

namespace
{

std::string next_correlation_id ()
{
    static std::atomic_uint64_t next{1};
    std::uint64_t value = next.fetch_add (1, std::memory_order_relaxed);
    // Cheap uint->hex (no ostringstream): runs per outbound request.
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

envelope_header_t client_call_codec_t::create_envelope (message_kind_t kind,
                                                        std::string channel_name,
                                                        std::string message_name,
                                                        std::chrono::milliseconds timeout,
                                                        std::optional<std::string> topic,
                                                        std::optional<std::string> source,
                                                        std::string content_type) const
{
    // One prvalue: a named local could be moved on return (no NRVO), and a
    // moved std::map allocates a new head node in MSVC's STL.
    return envelope_header_t{
      .kind = kind,
      .channel_name = std::move (channel_name),
      .message_name = std::move (message_name),
      .content_type = std::move (content_type),
      .correlation_id = kind == message_kind_t::request ? next_correlation_id () : std::string{},
      .deadline = timeout.count () > 0 ? std::optional<std::string>{format_utc_deadline (
                                           std::chrono::system_clock::now () + timeout)}
                                       : std::nullopt,
      .topic = std::move (topic),
      .source = std::move (source)};
}

} // namespace zlink::framework::runtime::messaging
