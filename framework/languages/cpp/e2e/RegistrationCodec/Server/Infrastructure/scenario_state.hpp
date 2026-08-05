/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../../Shared/registration_codec_contracts.hpp"

#include <atomic>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace zlink::framework::e2e::registration_codec::server
{

inline std::atomic<int> next_dependency_id{1};
inline std::atomic<int> scoped_dependency_destroyed{0};

class scenario_state_t
{
  public:
    void record (std::string marker, std::string value)
    {
        std::lock_guard lock (_mutex);
        _entries.push_back ({std::move (marker), std::move (value)});
    }

    evidence_snapshot_t snapshot () const
    {
        std::lock_guard lock (_mutex);
        return {.entries = _entries};
    }

  private:
    mutable std::mutex _mutex;
    std::vector<evidence_entry_t> _entries;
};

class singleton_dependency_t
{
  public:
    singleton_dependency_t () : id (next_dependency_id.fetch_add (1)) {}

    int id;
};

class scoped_dependency_t
{
  public:
    scoped_dependency_t () : id (next_dependency_id.fetch_add (1)) {}

    ~scoped_dependency_t () { scoped_dependency_destroyed.fetch_add (1); }

    int id;
};

class filter_order_state_t
{
  public:
    void reset ()
    {
        std::lock_guard lock (_mutex);
        _order.clear ();
    }

    void add (std::string value)
    {
        std::lock_guard lock (_mutex);
        _order.push_back (std::move (value));
    }

    std::vector<std::string> snapshot () const
    {
        std::lock_guard lock (_mutex);
        return _order;
    }

  private:
    mutable std::mutex _mutex;
    std::vector<std::string> _order;
};

} // namespace zlink::framework::e2e::registration_codec::server
