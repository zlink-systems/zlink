/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace zlink::framework::detail
{

class channel_runtime_bundle_t
{
  public:
    bool try_add_manual_connection (std::string endpoint);
    void remove_manual_connection (const std::string &endpoint);
    bool contains_manual_connection (const std::string &endpoint) const;
    std::vector<std::string> list_manual_connections () const;
    std::optional<std::string> next_manual_connection ();
    std::uint64_t connection_version () const;

    bool try_enter_receive () noexcept;
    void leave_receive () noexcept;
    bool receive_active () const noexcept;

  private:
    mutable std::mutex _mutex;
    std::set<std::string> _manual_connections;
    std::size_t _next_manual_connection = 0;
    std::uint64_t _connection_version = 0;
    std::atomic_bool _receive_active = false;
};

} // namespace zlink::framework::detail
