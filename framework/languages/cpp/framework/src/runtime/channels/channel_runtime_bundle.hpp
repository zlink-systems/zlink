/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include <atomic>
#include <functional>
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
    void attach_manual_connections (std::function<void (const std::string &)> connect,
                                    std::function<void (const std::string &)> disconnect);
    bool has_manual_connections () const;
    bool try_add_manual_connection (std::string endpoint);
    void remove_manual_connection (const std::string &endpoint);
    bool contains_manual_connection (const std::string &endpoint) const;
    std::vector<std::string> list_manual_connections () const;
    std::optional<std::string> next_manual_connection ();

    bool try_enter_receive () noexcept;
    void leave_receive () noexcept;
    bool receive_active () const noexcept;

  private:
    mutable std::mutex _mutex;
    std::set<std::string> _manual_connections;
    std::function<void (const std::string &)> _connect;
    std::function<void (const std::string &)> _disconnect;
    std::size_t _next_manual_connection = 0;
    std::atomic_bool _receive_active = false;
};

} // namespace zlink::framework::detail
