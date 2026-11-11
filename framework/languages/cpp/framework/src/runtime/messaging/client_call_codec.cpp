/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

#include <zlink/framework/detail/binary_text_codec.hpp>

#include "runtime/messaging/client_call_codec.hpp"

#include <atomic>
#include <array>

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


} // namespace

static std::array<char, 20> deadline_bytes (std::chrono::system_clock::time_point deadline)
{
    using namespace std::chrono;
    // Match the former time_point_cast/to_time_t truncation before splitting UTC fields.
    const auto seconds = time_point_cast<std::chrono::seconds> (deadline);
    const auto day = floor<days> (seconds);
    const year_month_day date (day);
    const hh_mm_ss clock (seconds - day);
    std::array<char, 20> output{'0', '0', '0', '0', '-', '0', '0', '-', '0', '0',
                                'T', '0', '0', ':', '0', '0', ':', '0', '0', 'Z'};
    const auto write = [&] (std::size_t offset, std::size_t width, unsigned value) {
        while (width != 0) {
            output[offset + --width] = static_cast<char> ('0' + value % 10);
            value /= 10;
        }
    };
    write (0, 4, static_cast<unsigned> (static_cast<int> (date.year ())));
    write (5, 2, static_cast<unsigned> (date.month ()));
    write (8, 2, static_cast<unsigned> (date.day ()));
    write (11, 2, static_cast<unsigned> (clock.hours ().count ()));
    write (14, 2, static_cast<unsigned> (clock.minutes ().count ()));
    write (17, 2, static_cast<unsigned> (clock.seconds ().count ()));
    return output;
}

std::string format_utc_deadline (std::chrono::system_clock::time_point deadline)
{
    const auto text = deadline_bytes (deadline);
    return std::string (text.data (), text.size ());
}

message_parts_t client_call_codec_t::encode_payload_parts (const envelope_header_t &header,
                                                           encoded_payload_t payload,
                                                           std::string_view content_type) const
{
    auto body = detail::encoded_payload_to_raw (std::move (payload));
    return message_parts_t (_codec.encode_header (header, content_type), std::move (body));
}

message_parts_t client_call_codec_t::encode_payload_parts (
  envelope_header_t &&header,
  encoded_payload_t payload,
  std::shared_ptr<const detail::request_type_plan_t> type,
  std::shared_ptr<const detail::prepared_header_fragment_t> channel) const
{
    auto body = detail::encoded_payload_to_raw (std::move (payload));
    return message_parts_t (_codec.encode_header (header, *type, *channel), std::move (body));
}

message_parts_t client_call_codec_t::encode_service_payload_parts (
  envelope_header_t &&header,
  encoded_payload_t payload,
  std::shared_ptr<const detail::request_type_plan_t> type,
  std::shared_ptr<const detail::prepared_header_fragment_t> channel) const
{
    auto body = detail::encoded_payload_to_raw (std::move (payload));
    return message_parts_t::from_envelope_encoder (_codec.prepare_service_payload (
      std::move (header), std::move (body), std::move (type), std::move (channel)));
}

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
      .deadline = timeout.count () > 0
                    ? std::optional<header_text_t>{header_text_t (
                        deadline_bytes (std::chrono::system_clock::now () + timeout))}
                    : std::nullopt,
      .topic = std::move (topic),
      .source = std::move (source)};
}

} // namespace zlink::framework::runtime::messaging
