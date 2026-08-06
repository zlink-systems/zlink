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

template <typename TStatus>
class runtime_observer_state_t final :
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

    ~runtime_observer_state_t () { close (); }

    void start ()
    {
        std::lock_guard lock (_mutex);
        if (_closed)
            return;
        if (_worker_started)
            return;

        auto self = this->shared_from_this ();
        _worker = std::thread ([self] { self->run (); });
        _worker_started = true;
    }

    void enqueue (TStatus status, bool terminal = false) noexcept
    {
        try {
            {
                std::lock_guard lock (_mutex);
                if (_closed || !_worker_started)
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

            }
            _changed.notify_one ();
        }
        catch (...) {
            std::lock_guard lock (_mutex);
            _closed = true;
            _pending.clear ();
        }
    }

    void close () noexcept
    {
        std::thread worker;
        {
            std::lock_guard lock (_mutex);
            _closed = true;
            _pending.clear ();
            _changed.notify_all ();
            if (_worker.joinable ()) {
                if (_worker.get_id () == std::this_thread::get_id ())
                    _worker.detach ();
                else
                    worker = std::move (_worker);
            }
        }
        if (worker.joinable ())
            worker.join ();
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

    void run () noexcept
    {
        for (;;) {
            std::optional<queued_status_t> next;
            std::optional<observed_status_t<TStatus>> delivered;
            {
                std::unique_lock lock (_mutex);
                _changed.wait (
                  lock, [&] { return _closed || !_pending.empty (); });
                if (_closed)
                    return;
                next.emplace (std::move (_pending.front ()));
                _pending.pop_front ();
                delivered.emplace (observed_status_t<TStatus>{
                  std::move (next->status), _loss});
            }

            try {
                _callback (*delivered);
            }
            catch (...) {
                std::lock_guard lock (_mutex);
                _closed = true;
                _pending.clear ();
                _changed.notify_all ();
                return;
            }
        }
    }

    std::size_t _capacity;
    callback_t _callback;
    std::mutex _mutex;
    std::deque<queued_status_t> _pending;
    observation_loss_t _loss;
    bool _closed = false;
    bool _worker_started = false;
    std::condition_variable _changed;
    std::thread _worker;
};

} // namespace zlink::framework::observation_detail
