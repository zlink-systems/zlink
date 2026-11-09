/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once
#include "encoding.hpp"
#include <array>

namespace zlink::framework::etcd::detail
{
struct snapshot_t
{
    std::uint64_t cluster;
    std::int64_t revision;
    std::uint64_t now;
    std::int64_t started;
    std::string last;
    bool expired () const;
};
std::int64_t monotonic_ms ();
class cursor_t
{
  public:
    cursor_t ();
    std::string
    encode (const snapshot_t &snapshot, std::string_view ns, std::string_view prefix) const;
    std::optional<snapshot_t>
    decode (std::string_view value, std::string_view ns, std::string_view prefix) const;

  private:
    std::string sign (std::string_view value) const;
    std::array<unsigned char, 32> _secret;
};
} // namespace zlink::framework::etcd::detail
