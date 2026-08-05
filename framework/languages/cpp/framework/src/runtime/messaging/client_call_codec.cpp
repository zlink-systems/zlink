/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#include "runtime/messaging/client_call_codec.hpp"

#include <atomic>
#include <ctime>
#include <iomanip>

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
        buffer[--index] = "0123456789abcdef"[value & 0xfu];
        value >>= 4u;
    } while (value != 0);
    return std::string (buffer + index);
}

std::string format_utc_deadline (std::chrono::system_clock::time_point deadline)
{
    const auto seconds = std::chrono::time_point_cast<std::chrono::seconds> (deadline);
    const std::time_t time = std::chrono::system_clock::to_time_t (seconds);
    std::tm tm{};
    gmtime_r (&time, &tm);
    std::ostringstream output;
    output << std::put_time (&tm, "%Y-%m-%dT%H:%M:%SZ");
    return output.str ();
}

} // namespace

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
