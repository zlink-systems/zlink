/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/monitoring/framework_runtime.hpp>

#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/execution/state_lane.hpp"

#include <chrono>
#include <map>
#include <optional>
#include <string>

namespace zlink::framework::runtime
{

class listener_status_registry_t final
{
  public:
    void update (listener_kind_t kind,
                 std::string name,
                 std::string endpoint)
    {
        if (name.empty () || endpoint.empty ())
            return;
        _lane.run ([&] {
        listener_status_t status{
          kind,
          name,
          std::move (endpoint),
          std::chrono::system_clock::now ()};
        _listeners[kind].insert_or_assign (
          std::move (name), std::move (status));
        }).get ();
    }

    void remove (listener_kind_t kind, const std::string &name) noexcept
    {
        _lane.run ([&] {
        const auto found = _listeners.find (kind);
        if (found == _listeners.end ())
            return;
        found->second.erase (name);
        if (found->second.empty ())
            _listeners.erase (found);
        }).get ();
    }

    std::optional<listener_status_t> find (
      listener_kind_t kind,
      const std::string &name) const
    {
        return _lane.run ([&] () -> std::optional<listener_status_t> {
        const auto kind_found = _listeners.find (kind);
        if (kind_found == _listeners.end ())
            return std::nullopt;
        const auto found = kind_found->second.find (name);
        if (found == kind_found->second.end ())
            return std::nullopt;
        auto status = found->second;
        status.observed_at = std::chrono::system_clock::now ();
        return status;
        }).get ();
    }

  private:
    runtime::offload_executor_t _lane_executor;
    mutable runtime::state_lane_t _lane{_lane_executor};
    std::map<listener_kind_t, std::map<std::string, listener_status_t>>
      _listeners;
};

} // namespace zlink::framework::runtime
