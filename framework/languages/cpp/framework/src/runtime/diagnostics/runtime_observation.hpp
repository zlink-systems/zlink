/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/monitoring/framework_runtime.hpp>

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

namespace zlink::framework::observation_detail
{

bool schedule_runtime_observation_work (std::function<void ()> work) noexcept;

inline void increment_runtime_observation_loss (
  std::uint64_t &value) noexcept
{
    constexpr auto maximum = static_cast<std::uint64_t> (
      std::numeric_limits<std::int64_t>::max ());
    if (value < maximum)
        ++value;
}

template <typename TStatus>
class runtime_observer_state_t final :
    public std::enable_shared_from_this<runtime_observer_state_t<TStatus>>
{
  public:
    using callback_t =
      std::function<void (const observed_status_t<TStatus> &)>;

    runtime_observer_state_t (std::size_t terminal_capacity,
                              callback_t callback) :
        _terminal_capacity (std::max<std::size_t> (1, terminal_capacity)),
        _callback (std::move (callback))
    {
    }

    ~runtime_observer_state_t () { close (); }

    void start () noexcept
    {
        std::lock_guard lock (_mutex);
        if (!_closed)
            _started = true;
    }

    void enqueue (std::string source_key,
                  TStatus status,
                  bool terminal = false) noexcept
    {
        bool schedule = false;
        try {
            {
                std::lock_guard lock (_mutex);
                if (_closed || !_started || source_key.empty ())
                    return;

                if (terminal)
                    retain_terminal_locked (std::move (source_key),
                                            std::move (status));
                else
                    retain_intermediate_locked (std::move (source_key),
                                                std::move (status));

                if (!_scheduled && has_pending_locked ()) {
                    _scheduled = true;
                    schedule = true;
                }
            }
            if (schedule)
                schedule_delivery (this->shared_from_this ());
        }
        catch (...) {
            fail_closed ();
        }
    }

    void close () noexcept
    {
        std::unique_lock lock (_mutex);
        _closed = true;
        _intermediate_by_source.clear ();
        _terminal_fifo.clear ();
        _retained_terminal_count_by_source.clear ();

        if (_callback_active
            && _callback_thread == std::this_thread::get_id ())
            return;

        _idle.wait (lock, [this] {
            return !_scheduled && !_callback_active;
        });
    }

  private:
    struct queued_status_t
    {
        std::string source_key;
        TStatus status;
        bool terminal = false;
    };

    bool has_pending_locked () const noexcept
    {
        return !_terminal_fifo.empty ()
               || !_intermediate_by_source.empty ();
    }

    void retain_intermediate_locked (std::string source_key,
                                     TStatus status)
    {
        const auto terminal =
          _retained_terminal_count_by_source.find (source_key);
        if (terminal != _retained_terminal_count_by_source.end ()
            && terminal->second != 0) {
            increment_runtime_observation_loss (_loss.coalesced_count);
            return;
        }

        auto existing = _intermediate_by_source.find (source_key);
        if (existing != _intermediate_by_source.end ()) {
            existing->second.status = std::move (status);
            increment_runtime_observation_loss (_loss.coalesced_count);
            return;
        }

        queued_status_t queued{
          source_key, std::move (status), false};
        _intermediate_by_source.emplace (
          std::move (source_key), std::move (queued));
    }

    void retain_terminal_locked (std::string source_key,
                                  TStatus status)
    {
        if (_intermediate_by_source.erase (source_key) != 0)
            increment_runtime_observation_loss (_loss.coalesced_count);

        if (_terminal_fifo.size () == _terminal_capacity) {
            release_terminal_source_locked (
              _terminal_fifo.front ().source_key);
            _terminal_fifo.pop_front ();
            increment_runtime_observation_loss (
              _loss.discarded_terminal_count);
        }

        ++_retained_terminal_count_by_source[source_key];
        _terminal_fifo.push_back (
          queued_status_t{std::move (source_key),
                          std::move (status),
                          true});
    }

    void release_terminal_source_locked (const std::string &source_key)
    {
        const auto found =
          _retained_terminal_count_by_source.find (source_key);
        if (found == _retained_terminal_count_by_source.end ())
            return;
        if (found->second > 1) {
            --found->second;
            return;
        }
        _retained_terminal_count_by_source.erase (found);
    }

    std::optional<queued_status_t> take_next_locked ()
    {
        if (!_terminal_fifo.empty ()) {
            auto next = std::move (_terminal_fifo.front ());
            _terminal_fifo.pop_front ();
            release_terminal_source_locked (next.source_key);
            return next;
        }
        if (_intermediate_by_source.empty ())
            return std::nullopt;
        auto current = _intermediate_by_source.begin ();
        auto next = std::move (current->second);
        _intermediate_by_source.erase (current);
        return next;
    }

    static void schedule_delivery (
      std::shared_ptr<runtime_observer_state_t> state) noexcept
    {
        if (schedule_runtime_observation_work (
              [state] { state->deliver_one (); }))
            return;
        state->fail_closed ();
    }

    void deliver_one () noexcept
    {
        std::optional<queued_status_t> next;
        std::optional<observed_status_t<TStatus>> delivered;
        {
            std::lock_guard lock (_mutex);
            if (_closed) {
                _scheduled = false;
                _idle.notify_all ();
                return;
            }
            next = take_next_locked ();
            if (!next) {
                _scheduled = false;
                _idle.notify_all ();
                return;
            }
            delivered.emplace (observed_status_t<TStatus>{
              std::move (next->status), _loss});
            _callback_active = true;
            _callback_thread = std::this_thread::get_id ();
        }

        bool callback_failed = false;
        try {
            _callback (*delivered);
        }
        catch (...) {
            callback_failed = true;
        }

        bool reschedule = false;
        {
            std::lock_guard lock (_mutex);
            _callback_active = false;
            _callback_thread = {};
            if (callback_failed)
                _closed = true;
            if (_closed) {
                _intermediate_by_source.clear ();
                _terminal_fifo.clear ();
                _retained_terminal_count_by_source.clear ();
                _scheduled = false;
            }
            else if (has_pending_locked ()) {
                reschedule = true;
            }
            else {
                _scheduled = false;
            }
            _idle.notify_all ();
        }

        if (reschedule)
            schedule_delivery (this->shared_from_this ());
    }

    void fail_closed () noexcept
    {
        std::lock_guard lock (_mutex);
        _closed = true;
        _scheduled = false;
        _intermediate_by_source.clear ();
        _terminal_fifo.clear ();
        _retained_terminal_count_by_source.clear ();
        _idle.notify_all ();
    }

    std::size_t _terminal_capacity;
    callback_t _callback;
    std::mutex _mutex;
    std::map<std::string, queued_status_t> _intermediate_by_source;
    std::deque<queued_status_t> _terminal_fifo;
    std::unordered_map<std::string, std::size_t>
      _retained_terminal_count_by_source;
    observation_loss_t _loss;
    bool _started = false;
    bool _closed = false;
    bool _scheduled = false;
    bool _callback_active = false;
    std::thread::id _callback_thread;
    std::condition_variable _idle;
};

} // namespace zlink::framework::observation_detail
