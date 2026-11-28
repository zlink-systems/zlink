/* SPDX-License-Identifier: Elastic-2.0 */
#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace zlink::framework::runtime::admission_queue
{

using bytes_t = std::vector<std::byte>;

struct connection_t
{
    bytes_t node_rid;
    std::uint64_t node_generation;
    std::string stream_name;
    bytes_t session_rid;

    auto operator<=> (const connection_t &) const = default;
};

std::string encode_base64url (std::span<const std::byte> bytes);
std::optional<bytes_t> decode_base64url (std::string_view encoded);

bool valid_stream_name (std::string_view name) noexcept;
std::uint64_t random_nonzero_u64 ();

inline constexpr std::size_t max_rid_bytes = 255;
inline constexpr std::uint64_t ticket_lifetime_ms = 30'000;

class ticket_signer_t
{
  public:
    ticket_signer_t (std::string_view current, std::optional<std::string> previous);

    std::string
    issue (const connection_t &connection, std::uint64_t epoch, std::uint64_t expiration) const;
    bool verify (const connection_t &connection,
                 std::string_view ticket,
                 std::uint64_t epoch,
                 std::uint64_t now) const;

  private:
    bytes_t _current;
    bytes_t _previous;
};

} // namespace zlink::framework::runtime::admission_queue
