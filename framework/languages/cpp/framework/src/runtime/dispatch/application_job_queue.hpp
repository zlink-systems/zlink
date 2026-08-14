/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/monitoring/framework_runtime.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace zlink::framework::runtime
{

struct application_job_queue_configuration_t
{
    application_job_queue_profile_t configured_profile =
      application_job_queue_profile_t::balanced;
    std::optional<std::uint32_t> configured_manual_max;
    std::uint32_t effective_processor_count = 1;
    std::uint32_t effective_max_queued_application_jobs = 1;
};

/* Host-instance aggregate for the shared Application Job Queue.  A permit
 * counts only while receive supply is reserved or a handler job is queued.
 * The permit is returned immediately before the first application handler
 * instruction; the handler's asynchronous lifetime is deliberately outside
 * this capacity boundary. */
class application_job_queue_t
{
  private:
    enum class permit_phase_t
    {
        reserved,
        queued,
        released
    };

    struct state_t;

    struct permit_state_t
    {
        std::weak_ptr<state_t> owner;
        permit_phase_t phase = permit_phase_t::reserved;
    };

    struct waiter_state_t;

  public:
    class permit_t
    {
      public:
        permit_t () = default;
        ~permit_t () { release_without_handler (); }

        permit_t (permit_t &&other) noexcept :
            _state (std::move (other._state))
        {
        }

        permit_t &operator= (permit_t &&other) noexcept
        {
            if (this != &other) {
                release_without_handler ();
                _state = std::move (other._state);
            }
            return *this;
        }

        permit_t (const permit_t &) = delete;
        permit_t &operator= (const permit_t &) = delete;

        explicit operator bool () const noexcept
        {
            return static_cast<bool> (_state);
        }

        void mark_queued ()
        {
            if (auto owner = owner_state ()) {
                std::lock_guard lock (owner->mutex);
                if (_state->phase != permit_phase_t::reserved)
                    return;
                --owner->reserved_supply_permits;
                ++owner->queued_application_jobs;
                _state->phase = permit_phase_t::queued;
            }
        }

        void release_for_handler_entry () noexcept { release (); }

        void release_without_handler () noexcept { release (); }

      private:
        friend class application_job_queue_t;

        explicit permit_t (std::shared_ptr<permit_state_t> state) :
            _state (std::move (state))
        {
        }

        std::shared_ptr<state_t> owner_state () const noexcept
        {
            return _state ? _state->owner.lock () : nullptr;
        }

        void release () noexcept
        {
            if (!_state)
                return;
            auto permit = std::exchange (_state, {});
            auto owner = permit->owner.lock ();
            if (!owner)
                return;
            application_job_queue_t::release_permit (
              std::move (owner), std::move (permit));
        }

        std::shared_ptr<permit_state_t> _state;
    };

    using supply_callback_t =
      std::function<void (std::optional<permit_t>)>;

    class waiter_t
    {
      public:
        waiter_t () = default;
        ~waiter_t () { (void) cancel (); }

        waiter_t (waiter_t &&other) noexcept :
            _owner (std::move (other._owner)),
            _waiter (std::move (other._waiter))
        {
        }

        waiter_t &operator= (waiter_t &&other) noexcept
        {
            if (this != &other) {
                (void) cancel ();
                _owner = std::move (other._owner);
                _waiter = std::move (other._waiter);
            }
            return *this;
        }

        waiter_t (const waiter_t &) = delete;
        waiter_t &operator= (const waiter_t &) = delete;

        bool cancel () noexcept
        {
            auto owner = _owner.lock ();
            auto waiter = std::exchange (_waiter, {});
            if (!owner || !waiter)
                return false;
            return application_job_queue_t::cancel_waiter (
              std::move (owner), std::move (waiter));
        }

      private:
        friend class application_job_queue_t;

        waiter_t (std::weak_ptr<state_t> owner,
                  std::shared_ptr<waiter_state_t> waiter) :
            _owner (std::move (owner)),
            _waiter (std::move (waiter))
        {
        }

        std::weak_ptr<state_t> _owner;
        std::shared_ptr<waiter_state_t> _waiter;
    };

    explicit application_job_queue_t (
      application_job_queue_configuration_t configuration) :
        _state (std::make_shared<state_t> (std::move (configuration)))
    {
    }

    application_job_queue_t (const application_job_queue_t &) = delete;
    application_job_queue_t &operator= (
      const application_job_queue_t &) = delete;

    std::optional<permit_t> try_reserve_supply ()
    {
        std::lock_guard lock (_state->mutex);
        if (_state->stopped || !_state->waiters.empty ()
            || _state->permits_in_use >= _state->configuration
                 .effective_max_queued_application_jobs) {
            return std::nullopt;
        }
        return reserve_locked (_state);
    }

    waiter_t wait_for_supply (supply_callback_t callback)
    {
        if (!callback)
            throw std::invalid_argument (
              "Application Job Queue waiter callback is required");

        auto waiter = std::make_shared<waiter_state_t> ();
        waiter->callback = std::move (callback);
        std::optional<permit_t> immediate;
        bool stopped = false;
        {
            std::lock_guard lock (_state->mutex);
            if (_state->stopped) {
                waiter->terminal = true;
                stopped = true;
            }
            else if (_state->waiters.empty ()
                     && _state->permits_in_use
                          < _state->configuration
                              .effective_max_queued_application_jobs) {
                waiter->terminal = true;
                immediate = reserve_locked (_state);
            }
            else {
                waiter->started_at = std::chrono::steady_clock::now ();
                waiter->measurement_epoch = _state->measurement_epoch;
                _state->waiters.push_back (waiter);
                ++_state->capacity_waiters;
                ++_state->capacity_wait_count;
            }
        }
        if (immediate)
            waiter->callback (std::move (immediate));
        else if (stopped)
            waiter->callback (std::nullopt);
        return waiter_t (_state, std::move (waiter));
    }

    std::optional<permit_t> wait_for_supply_blocking ()
    {
        struct blocking_state_t
        {
            std::mutex mutex;
            std::condition_variable changed;
            bool completed = false;
            std::optional<permit_t> permit;
        };
        auto state = std::make_shared<blocking_state_t> ();
        auto waiter = wait_for_supply (
          [state] (std::optional<permit_t> permit) mutable {
              {
                  std::lock_guard lock (state->mutex);
                  state->permit = std::move (permit);
                  state->completed = true;
              }
              state->changed.notify_all ();
          });
        std::unique_lock lock (state->mutex);
        state->changed.wait (lock, [&] { return state->completed; });
        return std::move (state->permit);
    }

    application_job_queue_status_t snapshot () const noexcept
    {
        std::lock_guard lock (_state->mutex);
        return {
          _state->configuration.configured_profile,
          _state->configuration.configured_manual_max,
          _state->configuration.effective_processor_count,
          _state->configuration.effective_max_queued_application_jobs,
          _state->reserved_supply_permits,
          _state->queued_application_jobs,
          _state->permits_in_use,
          _state->peak_permits_in_use,
          _state->capacity_waiters,
          _state->capacity_wait_count,
          std::chrono::nanoseconds (
            static_cast<std::chrono::nanoseconds::rep> (
              std::min<std::uint64_t> (
                _state->capacity_wait_duration_ns,
                static_cast<std::uint64_t> (
                  std::numeric_limits<
                    std::chrono::nanoseconds::rep>::max ())))) };
    }

    void reset_metrics () noexcept
    {
        std::lock_guard lock (_state->mutex);
        ++_state->measurement_epoch;
        _state->peak_permits_in_use = _state->permits_in_use;
        _state->capacity_wait_count = 0;
        _state->capacity_wait_duration_ns = 0;
    }

    void stop () noexcept
    {
        std::vector<supply_callback_t> callbacks;
        {
            std::lock_guard lock (_state->mutex);
            if (_state->stopped)
                return;
            _state->stopped = true;
            for (auto &waiter : _state->waiters) {
                if (!waiter || waiter->terminal)
                    continue;
                waiter->terminal = true;
                callbacks.push_back (std::move (waiter->callback));
            }
            _state->waiters.clear ();
            _state->capacity_waiters = 0;
        }
        for (auto &callback : callbacks) {
            try {
                callback (std::nullopt);
            }
            catch (...) {
            }
        }
    }

  private:
    struct waiter_state_t
    {
        supply_callback_t callback;
        std::chrono::steady_clock::time_point started_at{};
        std::uint64_t measurement_epoch = 0;
        bool terminal = false;
    };

    struct state_t
    {
        explicit state_t (
          application_job_queue_configuration_t configured) :
            configuration (std::move (configured))
        {
            if (configuration.effective_processor_count == 0
                || configuration.effective_max_queued_application_jobs == 0
                || configuration.effective_max_queued_application_jobs
                     > static_cast<std::uint32_t> (
                       std::numeric_limits<std::int32_t>::max ())) {
                throw std::invalid_argument (
                  "Application Job Queue effective capacity is invalid");
            }
        }

        mutable std::mutex mutex;
        application_job_queue_configuration_t configuration;
        std::deque<std::shared_ptr<waiter_state_t>> waiters;
        std::uint32_t reserved_supply_permits = 0;
        std::uint32_t queued_application_jobs = 0;
        std::uint32_t permits_in_use = 0;
        std::uint32_t peak_permits_in_use = 0;
        std::uint32_t capacity_waiters = 0;
        std::uint64_t capacity_wait_count = 0;
        std::uint64_t capacity_wait_duration_ns = 0;
        std::uint64_t measurement_epoch = 0;
        bool stopped = false;
    };

    static permit_t reserve_locked (const std::shared_ptr<state_t> &owner)
    {
        auto permit = std::make_shared<permit_state_t> ();
        permit->owner = owner;
        ++owner->reserved_supply_permits;
        ++owner->permits_in_use;
        owner->peak_permits_in_use =
          std::max (owner->peak_permits_in_use,
                    owner->permits_in_use);
        return permit_t (std::move (permit));
    }

    static void account_wait_locked (
      state_t &owner,
      const waiter_state_t &waiter) noexcept
    {
        if (waiter.measurement_epoch != owner.measurement_epoch)
            return;
        const auto elapsed = std::chrono::duration_cast<
          std::chrono::nanoseconds> (
          std::chrono::steady_clock::now () - waiter.started_at);
        const auto amount = elapsed.count () <= 0
                              ? std::uint64_t{0}
                              : static_cast<std::uint64_t> (
                                  elapsed.count ());
        owner.capacity_wait_duration_ns =
          amount > std::numeric_limits<std::uint64_t>::max ()
                     - owner.capacity_wait_duration_ns
            ? std::numeric_limits<std::uint64_t>::max ()
            : owner.capacity_wait_duration_ns + amount;
    }

    static std::shared_ptr<waiter_state_t>
    take_oldest_waiter_locked (state_t &owner) noexcept
    {
        while (!owner.waiters.empty ()) {
            auto waiter = std::move (owner.waiters.front ());
            owner.waiters.pop_front ();
            if (!waiter || waiter->terminal)
                continue;
            waiter->terminal = true;
            --owner.capacity_waiters;
            account_wait_locked (owner, *waiter);
            return waiter;
        }
        return {};
    }

    static void release_permit (
      std::shared_ptr<state_t> owner,
      std::shared_ptr<permit_state_t> permit) noexcept
    {
        std::shared_ptr<waiter_state_t> waiter;
        std::optional<permit_t> handoff;
        {
            std::lock_guard lock (owner->mutex);
            if (permit->phase == permit_phase_t::released)
                return;
            if (permit->phase == permit_phase_t::reserved)
                --owner->reserved_supply_permits;
            else
                --owner->queued_application_jobs;
            permit->phase = permit_phase_t::released;

            if (!owner->stopped)
                waiter = take_oldest_waiter_locked (*owner);
            if (waiter) {
                auto next = std::make_shared<permit_state_t> ();
                next->owner = owner;
                ++owner->reserved_supply_permits;
                handoff.emplace (permit_t (std::move (next)));
            }
            else {
                --owner->permits_in_use;
            }
        }
        if (waiter) {
            try {
                waiter->callback (std::move (handoff));
            }
            catch (...) {
                if (handoff)
                    handoff->release_without_handler ();
            }
        }
    }

    static bool cancel_waiter (
      std::shared_ptr<state_t> owner,
      std::shared_ptr<waiter_state_t> waiter) noexcept
    {
        supply_callback_t callback;
        {
            std::lock_guard lock (owner->mutex);
            if (waiter->terminal)
                return false;
            const auto found = std::find (
              owner->waiters.begin (), owner->waiters.end (), waiter);
            if (found == owner->waiters.end ())
                return false;
            owner->waiters.erase (found);
            waiter->terminal = true;
            --owner->capacity_waiters;
            account_wait_locked (*owner, *waiter);
            callback = std::move (waiter->callback);
        }
        try {
            callback (std::nullopt);
        }
        catch (...) {
        }
        return true;
    }

    std::shared_ptr<state_t> _state;
};

/* Event-loop adapter for the queue's cancellable FIFO reservation.  The
 * callback owns only shared state, so a queue handoff racing service shutdown
 * cannot reach a destroyed service object. */
class application_supply_slot_t final
{
  private:
    struct state_t
    {
        std::mutex mutex;
        std::function<void ()> wake;
        std::optional<application_job_queue_t::waiter_t> waiter;
        std::optional<application_job_queue_t::permit_t> permit;
        bool waiting = false;
        bool closed = false;
    };

  public:
    application_supply_slot_t (
      std::shared_ptr<application_job_queue_t> queue,
      std::function<void ()> wake) :
        _queue (std::move (queue)),
        _state (std::make_shared<state_t> ())
    {
        if (!_queue)
            throw std::invalid_argument (
              "Application supply slot requires the host queue");
        _state->wake = std::move (wake);
    }

    ~application_supply_slot_t () { close (); }

    application_supply_slot_t (const application_supply_slot_t &) = delete;
    application_supply_slot_t &operator= (
      const application_supply_slot_t &) = delete;

    void ensure_waiter ()
    {
        std::optional<application_job_queue_t::waiter_t> previous;
        {
            std::lock_guard lock (_state->mutex);
            if (_state->closed || _state->permit || _state->waiting)
                return;
            previous = std::move (_state->waiter);
            _state->waiting = true;
        }
        previous.reset ();

        const auto state = _state;
        auto waiter = _queue->wait_for_supply (
          [state] (std::optional<application_job_queue_t::permit_t> permit) {
              std::lock_guard lock (state->mutex);
              state->waiting = false;
              if (state->closed)
                  return;
              state->permit = std::move (permit);
              if (state->wake)
                  state->wake ();
          });
        {
            std::lock_guard lock (_state->mutex);
            if (_state->waiting && !_state->closed)
                _state->waiter.emplace (std::move (waiter));
        }
    }

    std::optional<application_job_queue_t::permit_t> take ()
    {
        std::optional<application_job_queue_t::waiter_t> completed;
        std::optional<application_job_queue_t::permit_t> permit;
        {
            std::lock_guard lock (_state->mutex);
            permit = std::move (_state->permit);
            completed = std::move (_state->waiter);
        }
        completed.reset ();
        return permit;
    }

    void close () noexcept
    {
        std::optional<application_job_queue_t::waiter_t> waiter;
        std::optional<application_job_queue_t::permit_t> permit;
        {
            std::lock_guard lock (_state->mutex);
            if (_state->closed)
                return;
            _state->closed = true;
            _state->waiting = false;
            _state->wake = {};
            waiter = std::move (_state->waiter);
            permit = std::move (_state->permit);
        }
        waiter.reset ();
        permit.reset ();
    }

  private:
    std::shared_ptr<application_job_queue_t> _queue;
    std::shared_ptr<state_t> _state;
};

} // namespace zlink::framework::runtime
