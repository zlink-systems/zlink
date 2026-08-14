/* SPDX-License-Identifier: MPL-2.0 */
#include "publish_admission_state.hpp"
#include "async_operation_state.hpp"

#include <Runtime/Sockets/socket_callback_state.hpp>
#include <zlink/Contracts/Errors/errors.hpp>
#include <zlink.h>

#include <algorithm>
#include <cerrno>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <queue>
#include <thread>
#include <utility>

namespace zlink::detail
{
namespace
{
class publish_admission_reactor_t
{
  public:
    publish_admission_reactor_t ()
    {
        ensure_async_continuation_dispatcher ();
        _thread = std::thread ([this] { run (); });
    }
    ~publish_admission_reactor_t ()
    {
        {
            std::lock_guard lock (_mutex);
            _stopping = true;
        }
        _wake.notify_all ();
        _thread.join ();
    }
    void post (std::function<void ()> work,
               std::chrono::steady_clock::time_point due =
                 std::chrono::steady_clock::now ())
    {
        {
            std::lock_guard lock (_mutex);
            _work.push ({due, _next++, std::move (work)});
        }
        _wake.notify_one ();
    }
  private:
    struct entry_t {
        std::chrono::steady_clock::time_point due;
        uint64_t sequence;
        std::function<void ()> work;
        bool operator< (const entry_t &other) const noexcept
        {
            return due != other.due ? due > other.due : sequence > other.sequence;
        }
    };
    void run ()
    {
        std::unique_lock lock (_mutex);
        while (!_stopping) {
            if (_work.empty ()) {
                _wake.wait (lock, [this] { return _stopping || !_work.empty (); });
                continue;
            }
            const auto due = _work.top ().due;
            if (_wake.wait_until (lock, due, [this, due] {
                    return _stopping || _work.empty () || _work.top ().due < due;
                }))
                continue;
            auto work = std::move (_work.top ().work);
            _work.pop ();
            lock.unlock ();
            try { work (); } catch (...) {}
            lock.lock ();
        }
    }
    std::mutex _mutex;
    std::condition_variable _wake;
    std::priority_queue<entry_t> _work;
    std::thread _thread;
    uint64_t _next = 0;
    bool _stopping = false;
};

publish_admission_reactor_t &reactor ()
{
    static publish_admission_reactor_t value;
    return value;
}

bool terminal_result (submit_result_t result) noexcept
{
    return result != submit_result_t::backpressured;
}
} // namespace

class publish_admission_state_t :
  public std::enable_shared_from_this<publish_admission_state_t>
{
  public:
    publish_admission_ticket_t enqueue (publish_attempt_fn_t attempt,
                                        publish_accepted_fn_t accepted,
                                        publish_terminal_fn_t terminal,
                                        std::chrono::steady_clock::time_point deadline)
    {
        auto operation = std::make_shared<operation_t> ();
        {
            std::lock_guard lock (_mutex);
            if (_closed)
                throw submit_error_t (submit_result_t::terminated, ETERM);
            operation->id = ++_next_id;
            operation->attempt = std::move (attempt);
            operation->accepted = std::move (accepted);
            operation->terminal = std::move (terminal);
            operation->deadline = deadline;
            _operations.emplace (operation->id, operation);
        }
        attempt_one (operation, false);
        return {weak_from_this (), operation->id};
    }

    void ready ()
    {
        uint64_t epoch = 0;
        std::size_t count = 0;
        {
            std::lock_guard lock (_mutex);
            if (_closed)
                return;
            epoch = ++_ready_epoch;
            count = _pending.size ();
        }
        if (count == 0)
            return;
        const auto self = shared_from_this ();
        reactor ().post ([self, epoch, count] { self->pump_epoch (epoch, count); });
    }

    bool cancel (uint64_t id) noexcept
    {
        publish_terminal_fn_t terminal;
        {
            std::lock_guard lock (_mutex);
            const auto found = _operations.find (id);
            if (found == _operations.end ())
                return false;
            terminal = std::move (found->second->terminal);
            erase_locked (found->second);
        }
        invoke_terminal (terminal, submit_result_t::terminated, ECANCELED);
        return true;
    }

    void shutdown () noexcept
    {
        std::vector<publish_terminal_fn_t> terminal;
        {
            std::lock_guard lock (_mutex);
            if (_closed)
                return;
            _closed = true;
            for (auto &[_, operation] : _operations)
                terminal.push_back (std::move (operation->terminal));
            _operations.clear ();
            _pending.clear ();
        }
        for (auto &completion : terminal)
            invoke_terminal (completion, submit_result_t::terminated, ETERM);
    }

  private:
    struct operation_t {
        uint64_t id = 0;
        uint64_t observed_epoch = 0;
        bool pending = false;
        std::chrono::steady_clock::time_point deadline;
        publish_attempt_fn_t attempt;
        publish_accepted_fn_t accepted;
        publish_terminal_fn_t terminal;
    };

    void pump_epoch (uint64_t epoch, std::size_t count)
    {
        while (count-- != 0) {
            std::shared_ptr<operation_t> operation;
            {
                std::lock_guard lock (_mutex);
                if (_closed || _pending.empty ())
                    return;
                operation = _pending.front ();
                _pending.pop_front ();
                operation->pending = false;
                if (_operations.find (operation->id) == _operations.end ())
                    continue;
                if (operation->observed_epoch == epoch) {
                    queue_locked (operation);
                    continue;
                }
                operation->observed_epoch = epoch;
            }
            attempt_one (operation, true);
        }
    }

    void attempt_one (const std::shared_ptr<operation_t> &operation,
                      bool from_ready)
    {
        if (from_ready
            && operation->deadline != std::chrono::steady_clock::time_point::max ()
            && std::chrono::steady_clock::now () >= operation->deadline) {
            finish (operation, submit_result_t::backpressured, ETIMEDOUT);
            return;
        }
        publish_attempt_result_t result;
        try { result = operation->attempt (); }
        catch (...) { finish (operation, submit_result_t::internal_error, errno); return; }
        if (result.result == submit_result_t::ok) {
            finish (operation, result.result, result.error);
            return;
        }
        if (terminal_result (result.result)) {
            finish (operation, result.result, result.error);
            return;
        }
        {
            std::lock_guard lock (_mutex);
            if (_closed || _operations.find (operation->id) == _operations.end ())
                return;
            if (from_ready)
                operation->observed_epoch = _ready_epoch;
            queue_locked (operation);
        }
        schedule_deadline (operation);
    }

    void schedule_deadline (const std::shared_ptr<operation_t> &operation)
    {
        if (operation->deadline == std::chrono::steady_clock::time_point::max ())
            return;
        const auto weak = weak_from_this ();
        const uint64_t id = operation->id;
        reactor ().post ([weak, id] {
            if (const auto self = weak.lock ())
                self->expire (id);
        }, operation->deadline);
    }

    void expire (uint64_t id)
    {
        std::shared_ptr<operation_t> operation;
        {
            std::lock_guard lock (_mutex);
            const auto found = _operations.find (id);
            if (found == _operations.end ()
                || found->second->deadline > std::chrono::steady_clock::now ())
                return;
            operation = found->second;
        }
        finish (operation, submit_result_t::backpressured, ETIMEDOUT);
    }

    void finish (const std::shared_ptr<operation_t> &operation,
                 submit_result_t result, int error)
    {
        publish_accepted_fn_t accepted;
        publish_terminal_fn_t terminal;
        {
            std::lock_guard lock (_mutex);
            const auto found = _operations.find (operation->id);
            if (found == _operations.end ())
                return;
            if (result == submit_result_t::ok)
                accepted = std::move (operation->accepted);
            else
                terminal = std::move (operation->terminal);
            erase_locked (operation);
        }
        if (accepted) { try { accepted (); } catch (...) {} }
        else invoke_terminal (terminal, result, error);
    }

    void queue_locked (const std::shared_ptr<operation_t> &operation)
    {
        if (!operation->pending) {
            operation->pending = true;
            _pending.push_back (operation);
        }
    }
    void erase_locked (const std::shared_ptr<operation_t> &operation)
    {
        _operations.erase (operation->id);
        if (operation->pending) {
            _pending.erase (std::remove (_pending.begin (), _pending.end (), operation),
                            _pending.end ());
            operation->pending = false;
        }
    }
    static void invoke_terminal (publish_terminal_fn_t &terminal,
                                 submit_result_t result, int error) noexcept
    {
        try { if (terminal) terminal (result, error); } catch (...) {}
    }

    std::mutex _mutex;
    std::map<uint64_t, std::shared_ptr<operation_t>> _operations;
    std::deque<std::shared_ptr<operation_t>> _pending;
    uint64_t _next_id = 0;
    uint64_t _ready_epoch = 0;
    bool _closed = false;
    friend class publish_admission_ticket_t;
};

bool publish_admission_ticket_t::cancel () const noexcept
{
    const auto owner = _owner.lock ();
    return owner && _id != 0 ? owner->cancel (_id) : false;
}

std::shared_ptr<publish_admission_state_t>
ensure_publish_admission_state (socket_callback_state_t &callbacks)
{
    std::lock_guard lock (callbacks.publish_admission_mutex);
    if (!callbacks.publish_admission)
        callbacks.publish_admission = std::make_shared<publish_admission_state_t> ();
    return callbacks.publish_admission;
}

void shutdown_publish_admission_state (socket_callback_state_t &callbacks) noexcept
{
    std::shared_ptr<publish_admission_state_t> state;
    {
        std::lock_guard lock (callbacks.publish_admission_mutex);
        state = std::move (callbacks.publish_admission);
    }
    if (state)
        state->shutdown ();
}

void notify_publish_admission_ready (socket_callback_state_t &callbacks) noexcept
{
    std::shared_ptr<publish_admission_state_t> state;
    {
        std::lock_guard lock (callbacks.publish_admission_mutex);
        state = callbacks.publish_admission;
    }
    if (state)
        state->ready ();
}

publish_admission_ticket_t enqueue_publish_admission (
  const std::shared_ptr<publish_admission_state_t> &owner,
  publish_attempt_fn_t attempt, publish_accepted_fn_t accepted,
  publish_terminal_fn_t terminal, std::chrono::steady_clock::time_point deadline)
{
    if (!owner)
        throw submit_error_t (submit_result_t::invalid_state, EINVAL);
    return owner->enqueue (std::move (attempt), std::move (accepted),
                           std::move (terminal), deadline);
}
} // namespace zlink::detail
