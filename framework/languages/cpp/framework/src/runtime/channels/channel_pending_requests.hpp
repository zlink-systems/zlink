/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace zlink::framework::detail
{

class channel_pending_requests_t
{
  public:
    std::uint64_t next_request_seq ();
    bool register_request (std::uint64_t request_seq, std::string channel_name);
    std::optional<std::string> remove (std::uint64_t request_seq);
    bool contains (std::uint64_t request_seq) const;
    void clear () noexcept;
    std::size_t count () const noexcept;

  private:
    std::uint64_t _next_request_seq = 1;
    std::map<std::uint64_t, std::string> _pending;
};

} // namespace zlink::framework::detail
