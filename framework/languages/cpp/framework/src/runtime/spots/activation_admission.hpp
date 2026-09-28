/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/errors/error.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>

namespace zlink::framework::detail
{

// The MeshNode's single activation admission record (MeshNode §5.1 "Pending activation").
// Actor creation, User Spot creation, Instance Spot cold activation and a relocation
// target's Restore each hold one admission, keyed by the object, from the moment the
// target MeshNode receives the operation until it reaches Ready or target commit, or is
// rejected, fails or is cleaned up. Entry Spot and Actor Join never enter. Runtime
// monitoring §5 reads the current value and the headroom from here, and the limit is
// enforced here.
class activation_admission_t : public std::enable_shared_from_this<activation_admission_t>
{
  public:
    static constexpr std::int32_t default_limit = 128;

    // The limit is fixed before the MeshNode starts (MeshNode §5.1).
    void set_limit (std::int32_t limit)
    {
        if (limit <= 0)
            throw framework_exception_t (framework_error_kind_t::not_configured,
                                         "Activation concurrency limit must be positive");
        std::lock_guard lock (_mutex);
        _limit = limit;
    }

    std::int32_t limit () const
    {
        std::lock_guard lock (_mutex);
        return _limit;
    }

    std::uint32_t active () const
    {
        std::lock_guard lock (_mutex);
        return static_cast<std::uint32_t> (_admitted.size ());
    }

    bool has_headroom () const
    {
        std::lock_guard lock (_mutex);
        return _admitted.size () < static_cast<std::size_t> (_limit);
    }

    bool holds (const std::string &key) const
    {
        std::lock_guard lock (_mutex);
        return _admitted.contains (key);
    }

    // Admits the operation for `key` when the limit has room. An object has at most one
    // operation in flight on this MeshNode, so a key already held stays admitted once.
    bool try_enter (const std::string &key)
    {
        std::lock_guard lock (_mutex);
        if (_admitted.contains (key))
            return true;
        if (_admitted.size () >= static_cast<std::size_t> (_limit))
            return false;
        return _admitted.insert (key).second;
    }

    // Ends the admission for `key`. Ending an admission that is not held is a no-op, so
    // every terminal path of an operation may end it.
    void leave (const std::string &key) noexcept
    {
        std::lock_guard lock (_mutex);
        _admitted.erase (key);
    }

    // Admits the operation for `key` for the lifetime of the returned handle, or throws
    // `unavailable` when the limit has no room.
    std::shared_ptr<void> enter_scoped (std::string key)
    {
        if (!try_enter (key))
            throw framework_exception_t (framework_error_kind_t::unavailable,
                                         "Object activation concurrency limit was reached");
        auto self = shared_from_this ();
        return std::shared_ptr<void> (
          nullptr, [self, key = std::move (key)] (void *) { self->leave (key); });
    }

    static std::string actor_key (const std::string &actor_id) { return "actor:" + actor_id; }
    static std::string spot_key (const std::string &spot_id) { return "spot:" + spot_id; }

  private:
    mutable std::mutex _mutex;
    std::int32_t _limit = default_limit;
    std::set<std::string> _admitted;
};

} // namespace zlink::framework::detail
