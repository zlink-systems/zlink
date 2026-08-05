/* SPDX-License-Identifier: MPL-2.0 */

#include "runtime/configuration/endpoint_connections.hpp"

#include <zlink/framework/contracts/errors/error.hpp>

#include <algorithm>
#include <cctype>
#include <functional>
#include <mutex>
#include <utility>

namespace zlink::framework
{

namespace detail
{

class endpoint_connections_state_t
{
  public:
    mutable std::mutex mutex;
    std::vector<std::string> endpoints;
    std::function<void (const std::string &)> live_connect;
    std::function<void (const std::string &)> live_disconnect;
    bool frozen = false;
};

namespace
{
void require_endpoint (const std::string &endpoint)
{
    const auto non_blank = std::any_of (endpoint.begin (), endpoint.end (),
                                        [] (unsigned char value) { return !std::isspace (value); });
    if (endpoint.empty () || !non_blank) {
        throw framework_exception_t (framework_error_kind_t::protocol_error,
                                     "endpoint connection requires a non-blank endpoint");
    }
}

void require_manual (const endpoint_connections_state_t &state)
{
    if (state.frozen) {
        throw framework_exception_t (
          framework_error_kind_t::protocol_error,
          "endpoint connections are frozen: the role acquires peers by discovery");
    }
}
} // namespace

void endpoint_connections_runtime_t::attach (
  endpoint_connections_t &connections,
  std::function<void (const std::string &)> connect,
  std::function<void (const std::string &)> disconnect)
{
    auto &state = *connections._state;
    const std::lock_guard lock (state.mutex);
    for (const auto &endpoint : state.endpoints) {
        if (connect) {
            connect (endpoint);
        }
    }
    state.live_connect = std::move (connect);
    state.live_disconnect = std::move (disconnect);
}

void endpoint_connections_runtime_t::freeze (endpoint_connections_t &connections)
{
    auto &state = *connections._state;
    const std::lock_guard lock (state.mutex);
    state.frozen = true;
    state.endpoints.clear ();
}

void endpoint_connections_runtime_t::thaw (endpoint_connections_t &connections)
{
    auto &state = *connections._state;
    const std::lock_guard lock (state.mutex);
    state.frozen = false;
}

std::vector<std::string>
endpoint_connections_runtime_t::snapshot (const endpoint_connections_t &connections)
{
    const auto &state = *connections._state;
    const std::lock_guard lock (state.mutex);
    return state.endpoints;
}

} // namespace detail

endpoint_connections_t::endpoint_connections_t () :
    _state (std::make_shared<detail::endpoint_connections_state_t> ())
{
}

void endpoint_connections_t::connect (std::string endpoint)
{
    detail::require_endpoint (endpoint);
    const std::lock_guard lock (_state->mutex);
    detail::require_manual (*_state);
    if (std::find (_state->endpoints.begin (), _state->endpoints.end (), endpoint)
        != _state->endpoints.end ()) {
        return;
    }
    if (_state->live_connect) {
        _state->live_connect (endpoint);
    }
    _state->endpoints.push_back (std::move (endpoint));
}

void endpoint_connections_t::disconnect (std::string endpoint)
{
    detail::require_endpoint (endpoint);
    const std::lock_guard lock (_state->mutex);
    detail::require_manual (*_state);
    const auto found = std::find (_state->endpoints.begin (), _state->endpoints.end (), endpoint);
    if (found == _state->endpoints.end ()) {
        return;
    }
    if (_state->live_disconnect) {
        _state->live_disconnect (endpoint);
    }
    _state->endpoints.erase (found);
}

std::vector<std::string> endpoint_connections_t::list_connections () const
{
    const std::lock_guard lock (_state->mutex);
    return _state->endpoints;
}

} // namespace zlink::framework
