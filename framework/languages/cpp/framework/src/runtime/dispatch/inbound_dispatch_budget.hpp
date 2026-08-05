/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>

namespace zlink::framework::runtime
{

struct inbound_dispatch_snapshot_t
{
    std::uint64_t application_hwm_bytes = 0;
    std::uint64_t pending_payload_bytes = 0;
    std::uint64_t queued_payload_bytes = 0;
    std::uint64_t active_payload_bytes = 0;
    bool application_receive_paused = false;
};

class inbound_dispatch_budget_t
{
  public:
    static constexpr std::size_t observation_shard_count = 16;

    explicit inbound_dispatch_budget_t (std::uint64_t application_hwm_bytes) :
        _application_hwm_bytes (application_hwm_bytes)
    {
    }

    bool can_start_application_receive () const noexcept
    {
        return can_receive ();
    }

    // Blocks an ingress loop until the host-wide payload budget admits another
    // complete application message or the caller requests shutdown. The
    // condition variable is signalled by terminal completion, so callers do
    // not need a polling delay or retry loop.
    bool wait_for_application_capacity (const std::function<bool ()> &stop_requested = {})
    {
        std::unique_lock lock (_wait_mutex);
        _capacity_changed.wait (lock, [&] {
            return can_receive () || (stop_requested && stop_requested ());
        });
        return can_receive () && !(stop_requested && stop_requested ());
    }

    void wake_waiters () noexcept { _capacity_changed.notify_all (); }

    void received (std::uint64_t payload_bytes)
    {
        const auto before = add_saturating (_pending_payload_bytes, payload_bytes);
        const auto after = before > std::numeric_limits<std::uint64_t>::max () - payload_bytes
                             ? std::numeric_limits<std::uint64_t>::max ()
                             : before + payload_bytes;
        add_saturating (observation ().received, payload_bytes);
        update_pause_state (before, after);
    }

    void handler_started (std::uint64_t payload_bytes)
    {
        add_saturating (observation ().active, payload_bytes);
    }

    void completed (std::uint64_t payload_bytes,
                    bool handler_started) noexcept
    {
        if (handler_started)
            subtract_saturating (observation ().active, payload_bytes);
        add_saturating (observation ().completed, payload_bytes);
        const auto pending = subtract_saturating (_pending_payload_bytes, payload_bytes);
        update_pause_state (pending.first, pending.second);
        _capacity_changed.notify_all ();
    }

    inbound_dispatch_snapshot_t snapshot () const noexcept
    {
        const auto pending = _pending_payload_bytes.load (std::memory_order_acquire);
        std::uint64_t active = 0;
        for (const auto &shard : _observations) {
            const auto shard_active = shard.active.load (std::memory_order_acquire);
            active = shard_active > std::numeric_limits<std::uint64_t>::max () - active
                       ? std::numeric_limits<std::uint64_t>::max ()
                       : active + shard_active;
        }
        active = std::min (active, pending);
        return {
          _application_hwm_bytes,
          pending,
          pending - active,
          active,
          _application_receive_paused.load (std::memory_order_acquire)};
    }

  private:
    struct alignas(64) observation_shard_t
    {
        std::atomic<std::uint64_t> received{0};
        std::atomic<std::uint64_t> completed{0};
        std::atomic<std::uint64_t> active{0};
    };

    bool can_receive () const noexcept
    {
        return _application_hwm_bytes == 0
               || (!_application_receive_paused.load (std::memory_order_acquire)
                   && _pending_payload_bytes.load (std::memory_order_acquire)
                        < _application_hwm_bytes);
    }

    observation_shard_t &observation () const noexcept
    {
        const auto hash = std::hash<std::thread::id>{} (std::this_thread::get_id ());
        return const_cast<observation_shard_t &> (
          _observations[hash % observation_shard_count]);
    }

    static std::uint64_t add_saturating (
      std::atomic<std::uint64_t> &value,
      std::uint64_t amount) noexcept
    {
        auto before = value.load (std::memory_order_relaxed);
        for (;;) {
            const auto after =
              before > std::numeric_limits<std::uint64_t>::max () - amount
                ? std::numeric_limits<std::uint64_t>::max ()
                : before + amount;
            if (value.compare_exchange_weak (before, after,
                                              std::memory_order_acq_rel,
                                              std::memory_order_relaxed))
                return before;
        }
    }

    static std::pair<std::uint64_t, std::uint64_t> subtract_saturating (
      std::atomic<std::uint64_t> &value,
      std::uint64_t amount) noexcept
    {
        auto before = value.load (std::memory_order_relaxed);
        for (;;) {
            const auto after = before >= amount ? before - amount : 0;
            if (value.compare_exchange_weak (before, after,
                                              std::memory_order_acq_rel,
                                              std::memory_order_relaxed))
                return {before, after};
        }
    }

    void update_pause_state (std::uint64_t before,
                             std::uint64_t after) noexcept
    {
        if (_application_hwm_bytes == 0
            || ((before < _application_hwm_bytes)
                == (after < _application_hwm_bytes)))
            return;

        // Only the HWM transition takes the mutex. The normal receive,
        // completion and observation paths remain atomic/sharded.
        std::lock_guard lock (_wait_mutex);
        const auto current = _pending_payload_bytes.load (std::memory_order_acquire);
        const bool paused = current >= _application_hwm_bytes;
        _application_receive_paused.store (paused, std::memory_order_release);
        if (!paused)
            _capacity_changed.notify_all ();
    }

    mutable std::array<observation_shard_t, observation_shard_count> _observations;
    std::atomic<std::uint64_t> _pending_payload_bytes{0};
    std::atomic<bool> _application_receive_paused{false};
    mutable std::mutex _wait_mutex;
    std::condition_variable _capacity_changed;
    std::uint64_t _application_hwm_bytes = 0;
};

} // namespace zlink::framework::runtime
