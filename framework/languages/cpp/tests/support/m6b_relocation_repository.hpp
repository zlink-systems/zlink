/* SPDX-License-Identifier: Elastic-2.0 */

#pragma once

#include "runtime/stateful/maintenance_runtime.hpp"
#include <map>
#include <string>
#include <utility>

namespace zlink::framework::test::m6b
{
namespace stateful = zlink::framework::runtime::stateful;

class memory_relocation_repository_t final : public stateful::relocation_store_port_t
{
  public:
    stateful::relocation_stored_t put (const std::vector<std::uint8_t> &payload,
                                       std::chrono::hours,
                                       std::chrono::steady_clock::time_point) override
    {
        std::lock_guard lock (_mutex);
        const auto reference = "instance-root-" + std::to_string (++_sequence);
        _roots.insert_or_assign (reference, payload);
        return {reference, stateful::maintenance_runtime_t::crc32c (payload)};
    }

    std::optional<std::vector<std::uint8_t>> get (const std::string &reference) override
    {
        std::lock_guard lock (_mutex);
        const auto found = _roots.find (reference);
        return found == _roots.end () ? std::optional<std::vector<std::uint8_t>>{}
                                      : std::make_optional (found->second);
    }

    void remove (const std::string &reference) override
    {
        std::lock_guard lock (_mutex);
        _roots.erase (reference);
    }

    std::size_t size () const
    {
        std::lock_guard lock (_mutex);
        return _roots.size ();
    }

  private:
    mutable std::mutex _mutex;
    std::uint64_t _sequence = 0;
    std::map<std::string, std::vector<std::uint8_t>> _roots;
};
}
