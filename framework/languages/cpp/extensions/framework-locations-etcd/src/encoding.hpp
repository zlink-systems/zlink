/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once
#include <zlink/locations/etcd.hpp>
#include <zlink/framework/detail/utf8.hpp>
#include <string_view>

namespace zlink::framework::etcd::detail
{
inline constexpr std::uint64_t max_time = 9223372036854ULL;
inline constexpr std::size_t max_payload = 1048576;
inline constexpr std::size_t max_page_bytes = 4 * 1024 * 1024;
inline constexpr char store_failure_message[] =
  "The Location Store operation failed; its write outcome may be unknown.";

[[noreturn]] void store_failure ();
using zlink::framework::detail::is_valid_utf8;
void validate_key (std::string_view value, bool prefix = false);
void validate_write (const store_write_request_t &request);
std::string physical_key (std::string_view ns, char kind, std::string_view key = {});
std::string prefix_end (std::string value);
std::string encode_clock (std::uint64_t value);
std::uint64_t decode_clock (std::string_view value);
struct metadata_t
{
    bool has_expiry;
    std::uint64_t expires_at;
    std::uint32_t payload_length;
    bool expired (std::uint64_t now) const { return has_expiry && expires_at <= now; }
};
std::string encode_metadata (metadata_t value);
metadata_t decode_metadata (std::string_view value);
std::string encode_version (std::uint64_t cluster, std::int64_t revision);
std::string to_string (const std::vector<std::byte> &value);
std::vector<std::byte> to_bytes (std::string_view value);
std::chrono::system_clock::time_point from_ms (std::uint64_t value);
} // namespace zlink::framework::etcd::detail
