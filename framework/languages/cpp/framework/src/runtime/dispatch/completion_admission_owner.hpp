/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>

namespace zlink::framework::runtime
{

struct completion_admission_snapshot_t
{
    std::uint64_t pending_completion_sends = 0;
    std::uint64_t completion_send_limit = 0;
};

class completion_admission_owner_t :
    public std::enable_shared_from_this<completion_admission_owner_t>
{
  public:
    class permit_t
    {
      public:
        permit_t () = default;
        explicit permit_t (
          std::shared_ptr<completion_admission_owner_t> owner) :
            _owner (std::move (owner))
        {
        }
        ~permit_t () { release (); }
        permit_t (permit_t &&other) noexcept :
            _owner (std::move (other._owner))
        {
        }
        permit_t &operator= (permit_t &&other) noexcept
        {
            if (this != &other) {
                release ();
                _owner = std::move (other._owner);
            }
            return *this;
        }
        permit_t (const permit_t &) = delete;
        permit_t &operator= (const permit_t &) = delete;

        explicit operator bool () const noexcept
        {
            return static_cast<bool> (_owner);
        }

      private:
        void release () noexcept
        {
            if (_owner) {
                _owner->release ();
                _owner.reset ();
            }
        }

        std::shared_ptr<completion_admission_owner_t> _owner;
    };

    explicit completion_admission_owner_t (
      std::uint64_t completion_send_limit) :
        _limit (completion_send_limit)
    {
        if (_limit == 0)
            throw std::invalid_argument (
              "completion send limit must be positive");
    }

    permit_t acquire ()
    {
        std::unique_lock lock (_mutex);
        ++_pending;
        _changed.wait (lock, [this] {
            return _stopped || _active < _limit;
        });
        if (_stopped) {
            --_pending;
            return {};
        }
        ++_active;
        return permit_t (shared_from_this ());
    }

    completion_admission_snapshot_t snapshot () const noexcept
    {
        std::lock_guard lock (_mutex);
        return {_pending, _limit};
    }

    void stop () noexcept
    {
        {
            std::lock_guard lock (_mutex);
            _stopped = true;
        }
        _changed.notify_all ();
    }

  private:
    void release () noexcept
    {
        {
            std::lock_guard lock (_mutex);
            if (_active != 0)
                --_active;
            if (_pending != 0)
                --_pending;
        }
        _changed.notify_one ();
    }

    mutable std::mutex _mutex;
    std::condition_variable _changed;
    std::uint64_t _limit = 0;
    std::uint64_t _pending = 0;
    std::uint64_t _active = 0;
    bool _stopped = false;
};

} // namespace zlink::framework::runtime
