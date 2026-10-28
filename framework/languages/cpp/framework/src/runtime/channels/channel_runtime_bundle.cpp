/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

#include "runtime/channels/channel_runtime_bundle.hpp"

#include <utility>

namespace zlink::framework::detail
{

void channel_runtime_bundle_t::attach_manual_connections (
  std::function<void (const std::string &)> connect,
  std::function<void (const std::string &)> disconnect)
{
    std::lock_guard lock (_mutex);
    for (const auto &endpoint : _manual_connections) {
        connect (endpoint);
    }
    _connect = std::move (connect);
    _disconnect = std::move (disconnect);
}

bool channel_runtime_bundle_t::has_manual_connections () const
{
    std::lock_guard lock (_mutex);
    return !_manual_connections.empty ();
}

bool channel_runtime_bundle_t::try_add_manual_connection (std::string endpoint)
{
    std::lock_guard lock (_mutex);
    auto [it, inserted] = _manual_connections.insert (std::move (endpoint));
    if (!inserted) {
        return false;
    }
    try {
        if (_connect) {
            _connect (*it);
        }
    }
    catch (...) {
        _manual_connections.erase (it);
        throw;
    }
    return true;
}

void channel_runtime_bundle_t::remove_manual_connection (const std::string &endpoint)
{
    std::lock_guard lock (_mutex);
    const auto found = _manual_connections.find (endpoint);
    if (found == _manual_connections.end ()) {
        return;
    }
    if (_disconnect) {
        _disconnect (*found);
    }
    _manual_connections.erase (found);
    if (_next_manual_connection >= _manual_connections.size ()) {
        _next_manual_connection = 0;
    }
}

bool channel_runtime_bundle_t::contains_manual_connection (const std::string &endpoint) const
{
    std::lock_guard lock (_mutex);
    return _manual_connections.find (endpoint) != _manual_connections.end ();
}

std::vector<std::string> channel_runtime_bundle_t::list_manual_connections () const
{
    std::lock_guard lock (_mutex);
    return std::vector<std::string> (_manual_connections.begin (), _manual_connections.end ());
}

std::optional<std::string> channel_runtime_bundle_t::next_manual_connection ()
{
    std::lock_guard lock (_mutex);
    if (_manual_connections.empty ()) {
        return std::nullopt;
    }
    auto endpoints =
      std::vector<std::string> (_manual_connections.begin (), _manual_connections.end ());
    if (_next_manual_connection >= endpoints.size ()) {
        _next_manual_connection = 0;
    }
    auto endpoint = endpoints[_next_manual_connection];
    _next_manual_connection = (_next_manual_connection + 1) % endpoints.size ();
    return endpoint;
}

bool channel_runtime_bundle_t::try_enter_receive () noexcept
{
    bool expected = false;
    return _receive_active.compare_exchange_strong (expected, true);
}

void channel_runtime_bundle_t::leave_receive () noexcept
{
    _receive_active.store (false);
}

bool channel_runtime_bundle_t::receive_active () const noexcept
{
    return _receive_active.load ();
}

} // namespace zlink::framework::detail
