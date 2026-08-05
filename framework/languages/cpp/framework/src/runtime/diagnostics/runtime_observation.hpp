/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework/contracts/monitoring/framework_runtime.hpp>

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace zlink::framework::observation_detail
{

class observation_dispatchable_t
{
  public:
    virtual ~observation_dispatchable_t () = default;

    // Delivers at most one queued item. True keeps the item scheduled.
    virtual bool dispatch_one () noexcept = 0;
};

class observation_dispatcher_t final
{
  public:
    static observation_dispatcher_t &instance ()
    {
        static observation_dispatcher_t dispatcher;
        return dispatcher;
    }

    observation_dispatcher_t (const observation_dispatcher_t &) = delete;
    observation_dispatcher_t &operator= (
      const observation_dispatcher_t &) = delete;

    void schedule (std::shared_ptr<observation_dispatchable_t> item)
    {
        {
            std::lock_guard lock (_mutex);
            if (_stopping)
                return;
            _ready.push_back (std::move (item));
        }
        _changed.notify_one ();
    }

  private:
    observation_dispatcher_t () : _worker ([this] { run (); }) {}

    ~observation_dispatcher_t ()
    {
        {
            std::lock_guard lock (_mutex);
            _stopping = true;
        }
        _changed.notify_all ();
        if (_worker.joinable ())
            _worker.join ();
    }

    void run () noexcept
    {
        for (;;) {
            std::shared_ptr<observation_dispatchable_t> item;
            {
                std::unique_lock lock (_mutex);
                _changed.wait (
                  lock, [&] { return _stopping || !_ready.empty (); });
                if (_stopping && _ready.empty ())
                    return;
                item = std::move (_ready.front ());
                _ready.pop_front ();
            }

            bool again = false;
            try {
                again = item->dispatch_one ();
            }
            catch (...) {
                // An observer must not terminate the shared dispatcher.
            }
            if (again)
                schedule (std::move (item));
        }
    }

    std::mutex _mutex;
    std::condition_variable _changed;
    std::deque<std::shared_ptr<observation_dispatchable_t>> _ready;
    bool _stopping = false;
    std::thread _worker;
};

template <typename TStatus>
class runtime_observer_state_t final :
    public observation_dispatchable_t,
    public std::enable_shared_from_this<runtime_observer_state_t<TStatus>>
{
  public:
    using callback_t =
      std::function<void (const observed_status_t<TStatus> &)>;

    runtime_observer_state_t (std::size_t capacity, callback_t callback) :
        _capacity (std::max<std::size_t> (1, capacity)),
        _callback (std::move (callback))
    {
    }

    ~runtime_observer_state_t () override { close (); }

    void enqueue (TStatus status, bool terminal = false) noexcept
    {
        bool schedule = false;
        try {
            {
                std::lock_guard lock (_mutex);
                if (_closed)
                    return;

                if (terminal) {
                    const auto intermediate = std::find_if (
                      _pending.begin (), _pending.end (),
                      [] (const queued_status_t &item) {
                          return !item.terminal;
                      });
                    if (_pending.size () == _capacity) {
                        if (intermediate != _pending.end ()) {
                            _pending.erase (intermediate);
                            saturating_increment (_loss.coalesced_count);
                        }
                        else {
                            _pending.pop_front ();
                            saturating_increment (
                              _loss.discarded_terminal_count);
                        }
                    }
                    _pending.push_back (
                      queued_status_t{std::move (status), true});
                }
                else {
                    const auto intermediate = std::find_if (
                      _pending.begin (), _pending.end (),
                      [] (const queued_status_t &item) {
                          return !item.terminal;
                      });
                    if (intermediate != _pending.end ()) {
                        intermediate->status = std::move (status);
                        saturating_increment (_loss.coalesced_count);
                    }
                    else if (_pending.size () < _capacity) {
                        _pending.push_back (
                          queued_status_t{std::move (status), false});
                    }
                    else {
                        saturating_increment (_loss.coalesced_count);
                    }
                }

                if (!_scheduled) {
                    _scheduled = true;
                    schedule = true;
                }
            }
            if (schedule)
                observation_dispatcher_t::instance ().schedule (
                  this->shared_from_this ());
        }
        catch (...) {
            if (schedule) {
                std::lock_guard lock (_mutex);
                _scheduled = false;
            }
        }
    }

    void close () noexcept
    {
        std::lock_guard lock (_mutex);
        _closed = true;
        _pending.clear ();
        _scheduled = false;
    }

    bool dispatch_one () noexcept override
    {
        std::optional<queued_status_t> next;
        observed_status_t<TStatus> delivered;
        {
            std::lock_guard lock (_mutex);
            if (_closed || _pending.empty ()) {
                _scheduled = false;
                return false;
            }
            next.emplace (std::move (_pending.front ()));
            _pending.pop_front ();
            delivered.status = std::move (next->status);
            delivered.loss = _loss;
        }

        try {
            _callback (delivered);
        }
        catch (...) {
            close ();
            return false;
        }

        std::lock_guard lock (_mutex);
        if (_closed || _pending.empty ()) {
            _scheduled = false;
            return false;
        }
        return true;
    }

  private:
    struct queued_status_t
    {
        TStatus status;
        bool terminal = false;
    };

    static void saturating_increment (std::uint64_t &value) noexcept
    {
        constexpr auto maximum = static_cast<std::uint64_t> (
          std::numeric_limits<std::int64_t>::max ());
        if (value < maximum)
            ++value;
    }

    std::size_t _capacity;
    callback_t _callback;
    std::mutex _mutex;
    std::deque<queued_status_t> _pending;
    observation_loss_t _loss;
    bool _closed = false;
    bool _scheduled = false;
};

} // namespace zlink::framework::observation_detail
