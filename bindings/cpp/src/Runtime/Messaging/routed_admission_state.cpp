/* SPDX-License-Identifier: MPL-2.0 */

#include "routed_admission_state.hpp"
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
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace zlink
{
namespace detail
{
namespace
{

struct routed_target_key_t
{
    std::string rid;
    uint64_t pair_id = 0;
    uint64_t generation = 0;

    bool operator< (const routed_target_key_t &other_) const noexcept
    {
        if (rid != other_.rid)
            return rid < other_.rid;
        if (pair_id != other_.pair_id)
            return pair_id < other_.pair_id;
        return generation < other_.generation;
    }
};

routed_target_key_t target_key (const zlink_routed_submit_target_t &target_)
{
    routed_target_key_t key;
    key.rid.assign (reinterpret_cast<const char *> (target_.peer_rid.data), target_.peer_rid.size);
    key.pair_id = target_.transport_pair_id;
    key.generation = target_.transport_pair_generation;
    return key;
}

submit_result_t terminal_submit_result (int error_) noexcept
{
    if (error_ == ETERM || error_ == ESHUTDOWN || error_ == ECANCELED)
        return submit_result_t::terminated;
    if (error_ == ENOENT || error_ == EHOSTUNREACH)
        return submit_result_t::not_found;
    return submit_result_t::not_connected;
}

struct pending_operation_t
{
    enum class state_t
    {
        queued,
        ready,
        waiting,
        in_flight
    };

    uint64_t id = 0;
    zlink_routed_submit_target_t target{};
    routed_target_key_t key;
    state_t state = state_t::ready;
    uint64_t observed_wake = 0;
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max ();
    routed_attempt_fn_t attempt;
    routed_accepted_fn_t accepted;
    routed_terminal_fn_t terminal;
    bool forced_terminal = false;
    submit_result_t forced_result = submit_result_t::internal_error;
    int forced_error = 0;
    bool attempt_before_expiry = false;
    bool deadline_registered = false;
};

class routed_admission_reactor_t
{
  public:
    routed_admission_reactor_t ();
    ~routed_admission_reactor_t ();

    void schedule (const std::shared_ptr<routed_admission_state_t> &state_,
                   std::chrono::steady_clock::time_point due_,
                   uint64_t generation_);
    void post (std::function<void ()> completion_);

  private:
    struct work_t
    {
        std::chrono::steady_clock::time_point due;
        uint64_t sequence = 0;
        uint64_t generation = 0;
        std::weak_ptr<routed_admission_state_t> state;
        std::function<void ()> completion;
    };

    struct later_t
    {
        bool operator() (const work_t &left_, const work_t &right_) const noexcept
        {
            if (left_.due != right_.due)
                return left_.due > right_.due;
            return left_.sequence > right_.sequence;
        }
    };

    void run ();

    std::mutex _mutex;
    std::condition_variable _changed;
    std::priority_queue<work_t, std::vector<work_t>, later_t> _work;
    uint64_t _next_sequence = 0;
    bool _stopping = false;
    std::thread _thread;
};

routed_admission_reactor_t &admission_reactor ()
{
    static routed_admission_reactor_t reactor;
    return reactor;
}

} // namespace

class routed_admission_state_t : public std::enable_shared_from_this<routed_admission_state_t>
{
  public:
    routed_admission_state_t () = default;

    routed_admission_ticket_t enqueue (const zlink_routed_submit_target_t &target_,
                                       routed_attempt_fn_t attempt_,
                                       routed_accepted_fn_t accepted_,
                                       routed_terminal_fn_t terminal_,
                                       std::chrono::steady_clock::time_point deadline_,
                                       bool attempt_before_expiry_)
    {
        std::shared_ptr<pending_operation_t> operation = std::make_shared<pending_operation_t> ();
        operation->target = target_;
        operation->key = target_key (target_);
        operation->deadline = deadline_;
        operation->attempt_before_expiry = attempt_before_expiry_;
        operation->attempt = std::move (attempt_);
        operation->accepted = std::move (accepted_);
        operation->terminal = std::move (terminal_);

        {
            std::lock_guard<std::mutex> lock (_mutex);
            if (_closed)
                throw submit_error_t (submit_result_t::terminated, ETERM);
            operation->id = ++_next_id;
            _operations.emplace (operation->id, operation);
            std::deque<uint64_t> &target_queue = _pending_by_target[operation->key];
            const bool first_for_target = target_queue.empty ();
            operation->state = first_for_target ? pending_operation_t::state_t::ready
                                                : pending_operation_t::state_t::queued;
            if (!first_for_target)
                operation->attempt_before_expiry = false;
            target_queue.push_back (operation->id);
            if (operation->state == pending_operation_t::state_t::ready)
                mark_target_ready_locked (operation->key);
            if (!operation->attempt_before_expiry)
                register_deadline_locked (*operation);
            refresh_schedule_locked ();
        }
        return routed_admission_ticket_t (shared_from_this (), operation->id);
    }

    bool cancel (uint64_t id_) noexcept
    {
        std::shared_ptr<pending_operation_t> operation;
        {
            std::lock_guard<std::mutex> lock (_mutex);
            const auto found = _operations.find (id_);
            if (found == _operations.end ())
                return false;
            operation = found->second;
            if (operation->state == pending_operation_t::state_t::in_flight) {
                operation->forced_terminal = true;
                operation->forced_result = submit_result_t::terminated;
                operation->forced_error = ECANCELED;
                return true;
            }
            remove_operation_locked (operation);
            refresh_schedule_locked ();
        }
        invoke_terminal (operation, submit_result_t::terminated, ECANCELED);
        return true;
    }

    void on_event (const zlink_routed_send_ready_event_t &event_) noexcept
    {
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock (_mutex);
            if (_closed)
                return;
            const routed_target_key_t key = target_key (zlink_routed_submit_target_t{
              event_.peer_rid, event_.transport_pair_id, event_.transport_pair_generation});
            if (event_.state == ZLINK_ROUTED_SEND_TERMINAL) {
                const auto queue = _pending_by_target.find (key);
                if (queue != _pending_by_target.end ()) {
                    for (const uint64_t id : queue->second) {
                        const auto entry = _operations.find (id);
                        if (entry == _operations.end ())
                            continue;
                        entry->second->forced_terminal = true;
                        entry->second->forced_result =
                          terminal_submit_result (event_.terminal_errno);
                        entry->second->forced_error = event_.terminal_errno;
                    }
                    const auto front = _operations.find (queue->second.front ());
                    if (front != _operations.end ()
                        && front->second->state != pending_operation_t::state_t::in_flight) {
                        front->second->state = pending_operation_t::state_t::ready;
                        unregister_deadline_locked (*front->second);
                        mark_target_ready_locked (key);
                    }
                    changed = true;
                }
            } else {
                const auto queue = _pending_by_target.find (key);
                if (queue != _pending_by_target.end () && !queue->second.empty ()) {
                    ++_wake_versions[key];
                    const auto front = _operations.find (queue->second.front ());
                    if (front != _operations.end ()
                        && front->second->state == pending_operation_t::state_t::waiting) {
                        front->second->state = pending_operation_t::state_t::ready;
                        unregister_deadline_locked (*front->second);
                        mark_target_ready_locked (key);
                        changed = true;
                    }
                }
                // A DEALER operation can be accepted before Core has a
                // concrete transport-pair target.  Such work is keyed by the
                // empty target and any writable routed edge is its exact
                // signal to select a current pair and retry once.
                const routed_target_key_t generic_key{};
                const auto generic = _pending_by_target.find (generic_key);
                if (generic != _pending_by_target.end ()
                    && !generic->second.empty ()) {
                    ++_wake_versions[generic_key];
                    const auto front =
                      _operations.find (generic->second.front ());
                    if (front != _operations.end ()
                        && front->second->state
                             == pending_operation_t::state_t::waiting) {
                        front->second->state =
                          pending_operation_t::state_t::ready;
                        unregister_deadline_locked (*front->second);
                        mark_target_ready_locked (generic_key);
                        changed = true;
                    }
                }
            }
            if (changed)
                refresh_schedule_locked ();
        }
    }

    void on_generic_ready () noexcept
    {
        std::lock_guard<std::mutex> lock (_mutex);
        if (_closed)
            return;
        const routed_target_key_t key{};
        const auto queue = _pending_by_target.find (key);
        if (queue == _pending_by_target.end () || queue->second.empty ())
            return;
        ++_wake_versions[key];
        const auto front = _operations.find (queue->second.front ());
        if (front != _operations.end ()
            && front->second->state
                 == pending_operation_t::state_t::waiting) {
            front->second->state = pending_operation_t::state_t::ready;
            unregister_deadline_locked (*front->second);
            mark_target_ready_locked (key);
        }
        refresh_schedule_locked ();
    }

    void pump (uint64_t generation_) noexcept
    {
        struct terminal_action_t
        {
            routed_terminal_fn_t callback;
            submit_result_t result;
            int error;
        };
        std::vector<std::shared_ptr<pending_operation_t>> attempts;
        std::vector<std::shared_ptr<pending_operation_t>> forced;
        std::vector<routed_accepted_fn_t> accepted_actions;
        std::vector<terminal_action_t> terminal_actions;
        {
            std::lock_guard<std::mutex> lock (_mutex);
            if (_closed || !_scheduled || generation_ != _schedule_generation)
                return;
            _scheduled = false;
            ++_active_pumps;
            const auto now = std::chrono::steady_clock::now ();
            expire_due_locked (now, terminal_actions);
            const size_t ready_target_count = _ready_targets.size ();
            for (size_t i = 0; i < ready_target_count; ++i) {
                const routed_target_key_t key = std::move (_ready_targets.front ());
                _ready_targets.pop_front ();
                _ready_set.erase (key);
                const auto queue = _pending_by_target.find (key);
                if (queue == _pending_by_target.end () || queue->second.empty ())
                    continue;
                const auto found = _operations.find (queue->second.front ());
                if (found == _operations.end ()
                    || found->second->state != pending_operation_t::state_t::ready)
                    continue;
                if (found->second->forced_terminal) {
                    forced.push_back (found->second);
                    remove_operation_locked (found->second);
                    continue;
                }
                found->second->state = pending_operation_t::state_t::in_flight;
                found->second->observed_wake = _wake_versions[found->second->key];
                unregister_deadline_locked (*found->second);
                attempts.push_back (found->second);
            }
        }

        for (const auto &operation : forced)
            terminal_actions.push_back (
              {std::move (operation->terminal), operation->forced_result, operation->forced_error});

        for (const auto &operation : attempts) {
            routed_attempt_result_t result;
            try {
                result = operation->attempt ();
            }
            catch (const submit_error_t &error) {
                result = {error.result (), error.internal_errno ()};
            }
            catch (...) {
                result = {submit_result_t::internal_error, EIO};
            }

            routed_accepted_fn_t accepted;
            routed_terminal_fn_t terminal;
            {
                std::lock_guard<std::mutex> lock (_mutex);
                const auto found = _operations.find (operation->id);
                if (found == _operations.end ())
                    continue;
                if (_closed) {
                    terminal = std::move (operation->terminal);
                    remove_operation_locked (operation);
                    result = {submit_result_t::terminated, ETERM};
                } else if (operation->forced_terminal) {
                    terminal = std::move (operation->terminal);
                    remove_operation_locked (operation);
                    result = {operation->forced_result, operation->forced_error};
                } else if (result.result == submit_result_t::ok) {
                    accepted = std::move (operation->accepted);
                    remove_operation_locked (operation);
                } else if (result.result == submit_result_t::backpressured) {
                    const auto now = std::chrono::steady_clock::now ();
                    if (operation->deadline
                          != std::chrono::steady_clock::time_point::max ()
                        && now >= operation->deadline) {
                        terminal = std::move (operation->terminal);
                        remove_operation_locked (operation);
                        result.error = ETIMEDOUT;
                    } else if (_wake_versions[operation->key]
                               > operation->observed_wake) {
                        operation->state = pending_operation_t::state_t::ready;
                        mark_target_ready_locked (operation->key);
                    } else {
                        operation->state = pending_operation_t::state_t::waiting;
                        register_deadline_locked (*operation);
                    }
                } else {
                    terminal = std::move (operation->terminal);
                    remove_operation_locked (operation);
                }
            }
            if (accepted)
                accepted_actions.push_back (std::move (accepted));
            if (terminal)
                terminal_actions.push_back ({std::move (terminal), result.result, result.error});
        }

        {
            std::lock_guard<std::mutex> lock (_mutex);
            refresh_schedule_locked ();
            --_active_pumps;
            _quiesced.notify_all ();
        }
        for (auto &accepted : accepted_actions)
            invoke_accepted (accepted);
        for (auto &terminal : terminal_actions)
            invoke_terminal (terminal.callback, terminal.result, terminal.error);
    }

    void shutdown () noexcept
    {
        std::vector<std::shared_ptr<pending_operation_t>> pending;
        {
            std::unique_lock<std::mutex> lock (_mutex);
            _closed = true;
            _quiesced.wait (lock, [this] { return _active_pumps == 0; });
            for (auto &entry : _operations)
                pending.push_back (std::move (entry.second));
            _operations.clear ();
            _pending_by_target.clear ();
            _ready_targets.clear ();
            _ready_set.clear ();
            _deadlines.clear ();
            _wake_versions.clear ();
            _scheduled = false;
            ++_schedule_generation;
        }
        for (const auto &operation : pending)
            invoke_terminal (operation, submit_result_t::terminated, ETERM);
    }

  private:
    void mark_target_ready_locked (const routed_target_key_t &key_)
    {
        if (_ready_set.insert (key_).second)
            _ready_targets.push_back (key_);
    }

    void unregister_deadline_locked (pending_operation_t &operation_)
    {
        if (!operation_.deadline_registered)
            return;
        const auto range = _deadlines.equal_range (operation_.deadline);
        for (auto it = range.first; it != range.second; ++it) {
            if (it->second == operation_.id) {
                _deadlines.erase (it);
                break;
            }
        }
        operation_.deadline_registered = false;
    }

    void register_deadline_locked (pending_operation_t &operation_)
    {
        if (operation_.deadline_registered
            || operation_.deadline == std::chrono::steady_clock::time_point::max ())
            return;
        _deadlines.emplace (operation_.deadline, operation_.id);
        operation_.deadline_registered = true;
    }

    void promote_front_locked (const routed_target_key_t &key_)
    {
        const auto queue = _pending_by_target.find (key_);
        if (queue == _pending_by_target.end () || queue->second.empty ())
            return;
        const auto operation = _operations.find (queue->second.front ());
        if (operation == _operations.end ())
            return;
        if (operation->second->state == pending_operation_t::state_t::queued)
            operation->second->state = pending_operation_t::state_t::ready;
        if (operation->second->state == pending_operation_t::state_t::ready)
            mark_target_ready_locked (key_);
    }

    void remove_operation_locked (const std::shared_ptr<pending_operation_t> &operation_)
    {
        unregister_deadline_locked (*operation_);
        const auto queue = _pending_by_target.find (operation_->key);
        if (queue != _pending_by_target.end ()) {
            auto id = std::find (queue->second.begin (), queue->second.end (), operation_->id);
            const bool was_front = id == queue->second.begin ();
            if (id != queue->second.end ())
                queue->second.erase (id);
            if (queue->second.empty ()) {
                _pending_by_target.erase (queue);
                _wake_versions.erase (operation_->key);
                _ready_set.erase (operation_->key);
            } else if (was_front) {
                promote_front_locked (operation_->key);
            }
        }
        _operations.erase (operation_->id);
    }

    template <typename TerminalActions>
    void expire_due_locked (std::chrono::steady_clock::time_point now_,
                            TerminalActions &terminal_actions_)
    {
        while (!_deadlines.empty () && _deadlines.begin ()->first <= now_) {
            const uint64_t id = _deadlines.begin ()->second;
            const auto operation = _operations.find (id);
            if (operation == _operations.end ()) {
                _deadlines.erase (_deadlines.begin ());
                continue;
            }
            operation->second->deadline_registered = false;
            _deadlines.erase (_deadlines.begin ());
            routed_terminal_fn_t terminal = std::move (operation->second->terminal);
            remove_operation_locked (operation->second);
            terminal_actions_.push_back (
              {std::move (terminal), submit_result_t::backpressured, ETIMEDOUT});
        }
    }

    void refresh_schedule_locked ()
    {
        if (_closed)
            return;
        const bool ready = !_ready_set.empty ();
        if (!ready && _deadlines.empty ()) {
            if (_scheduled) {
                _scheduled = false;
                ++_schedule_generation;
            }
            return;
        }
        const auto due = ready ? std::chrono::steady_clock::now ()
                               : _deadlines.begin ()->first;
        if (_scheduled) {
            if (ready && _scheduled_due <= due)
                return;
            if (!ready && _scheduled_due == due)
                return;
        }
        _scheduled = true;
        _scheduled_due = due;
        const uint64_t generation = ++_schedule_generation;
        admission_reactor ().schedule (shared_from_this (), due, generation);
    }

    static void invoke_accepted (routed_accepted_fn_t &accepted_) noexcept
    {
        try {
            accepted_ ();
        }
        catch (...) {
        }
    }

    static void invoke_terminal (const std::shared_ptr<pending_operation_t> &operation_,
                                 submit_result_t result_,
                                 int error_) noexcept
    {
        invoke_terminal (operation_->terminal, result_, error_);
    }

    static void
    invoke_terminal (routed_terminal_fn_t &terminal_, submit_result_t result_, int error_) noexcept
    {
        try {
            if (terminal_)
                terminal_ (result_, error_);
        }
        catch (...) {
        }
    }

    std::mutex _mutex;
    std::condition_variable _quiesced;
    std::map<uint64_t, std::shared_ptr<pending_operation_t>> _operations;
    std::map<routed_target_key_t, std::deque<uint64_t>> _pending_by_target;
    std::deque<routed_target_key_t> _ready_targets;
    std::set<routed_target_key_t> _ready_set;
    std::multimap<std::chrono::steady_clock::time_point, uint64_t> _deadlines;
    std::map<routed_target_key_t, uint64_t> _wake_versions;
    uint64_t _next_id = 0;
    size_t _active_pumps = 0;
    bool _closed = false;
    bool _scheduled = false;
    std::chrono::steady_clock::time_point _scheduled_due{};
    uint64_t _schedule_generation = 0;

    friend class routed_admission_reactor_t;
    friend class routed_admission_ticket_t;
};

namespace
{

routed_admission_reactor_t::routed_admission_reactor_t ()
{
    // Construct completion delivery first so static teardown stops this
    // physical admission producer before draining continuation work.
    ensure_async_continuation_dispatcher ();
    _thread = std::thread ([this] { run (); });
}

routed_admission_reactor_t::~routed_admission_reactor_t ()
{
    {
        std::lock_guard<std::mutex> lock (_mutex);
        _stopping = true;
    }
    _changed.notify_all ();
    if (_thread.joinable ())
        _thread.join ();
}

void routed_admission_reactor_t::schedule (const std::shared_ptr<routed_admission_state_t> &state_,
                                           std::chrono::steady_clock::time_point due_,
                                           uint64_t generation_)
{
    {
        std::lock_guard<std::mutex> lock (_mutex);
        if (_stopping)
            return;
        _work.push (work_t{due_, ++_next_sequence, generation_, state_, {}});
    }
    _changed.notify_one ();
}

void routed_admission_reactor_t::post (std::function<void ()> completion_)
{
    {
        std::lock_guard<std::mutex> lock (_mutex);
        if (_stopping)
            throw std::runtime_error ("routed admission reactor is stopping");
        _work.push (work_t{std::chrono::steady_clock::now (), ++_next_sequence,
                           0, {}, std::move (completion_)});
    }
    _changed.notify_one ();
}

void routed_admission_reactor_t::run ()
{
    std::unique_lock<std::mutex> lock (_mutex);
    while (!_stopping) {
        if (_work.empty ()) {
            _changed.wait (lock, [this] { return _stopping || !_work.empty (); });
            continue;
        }
        const auto due = _work.top ().due;
        if (_changed.wait_until (lock, due, [this, due] {
                return _stopping || _work.empty () || _work.top ().due < due;
            }))
            continue;
        if (_stopping || _work.empty ())
            continue;
        work_t work = _work.top ();
        _work.pop ();
        lock.unlock ();
        try {
            if (work.completion)
                work.completion ();
            else if (const auto state = work.state.lock ())
                state->pump (work.generation);
        }
        catch (...) {
        }
        lock.lock ();
    }
}

void routed_ready_trampoline (void *,
                              const zlink_routed_send_ready_event_t *event_,
                              void *userdata_)
{
    if (!event_ || !userdata_)
        return;
    socket_callback_state_t *callbacks = static_cast<socket_callback_state_t *> (userdata_);
    std::shared_ptr<routed_admission_state_t> state;
    {
        std::lock_guard<std::mutex> lock (callbacks->routed_admission_mutex);
        state = callbacks->routed_admission;
    }
    if (state)
        state->on_event (*event_);
}

} // namespace

bool routed_admission_ticket_t::cancel () const noexcept
{
    const auto owner = _owner.lock ();
    return owner && _id != 0 ? owner->cancel (_id) : false;
}

std::shared_ptr<routed_admission_state_t>
ensure_routed_admission_state (void *socket_, socket_callback_state_t &callbacks_)
{
    std::lock_guard<std::mutex> lock (callbacks_.routed_admission_mutex);
    if (callbacks_.routed_admission)
        return callbacks_.routed_admission;

    std::shared_ptr<routed_admission_state_t> state = std::make_shared<routed_admission_state_t> ();
    callbacks_.routed_admission = state;
    const zlink_handler_result_t rc =
      zlink_routed_send_ready_handler (socket_, &routed_ready_trampoline, &callbacks_);
    if (rc != ZLINK_HANDLER_OK) {
        callbacks_.routed_admission.reset ();
        throw handler_error_t (static_cast<handler_result_t> (rc), zlink_errno ());
    }
    return state;
}

void shutdown_routed_admission_state (socket_callback_state_t &callbacks_) noexcept
{
    std::shared_ptr<routed_admission_state_t> state;
    {
        std::lock_guard<std::mutex> lock (callbacks_.routed_admission_mutex);
        state = std::move (callbacks_.routed_admission);
    }
    if (state)
        state->shutdown ();
}

void notify_routed_admission_ready (
  socket_callback_state_t &callbacks_) noexcept
{
    std::shared_ptr<routed_admission_state_t> state;
    {
        std::lock_guard<std::mutex> lock (
          callbacks_.routed_admission_mutex);
        state = callbacks_.routed_admission;
    }
    if (state)
        state->on_generic_ready ();
}

void post_routed_completion (std::function<void ()> completion_)
{
    admission_reactor ().post (std::move (completion_));
}

zlink_routed_submit_target_t
select_routed_submit_target (void *socket_, const zlink_routing_id_t *router_rid_or_null_)
{
    zlink_routed_submit_target_t target{};
    const zlink_submit_result_t rc =
      zlink_select_routed_submit_target (socket_, router_rid_or_null_, &target);
    if (rc != ZLINK_SUBMIT_OK)
        throw submit_error_t (static_cast<submit_result_t> (rc), zlink_errno ());
    return target;
}

routed_admission_ticket_t
enqueue_routed_admission (const std::shared_ptr<routed_admission_state_t> &owner_,
                          const zlink_routed_submit_target_t &target_,
                          routed_attempt_fn_t attempt_,
                          routed_accepted_fn_t accepted_,
                          routed_terminal_fn_t terminal_,
                          std::chrono::steady_clock::time_point deadline_,
                          bool attempt_before_expiry_)
{
    if (!owner_)
        throw submit_error_t (submit_result_t::invalid_state, EINVAL);
    return owner_->enqueue (target_, std::move (attempt_), std::move (accepted_),
                            std::move (terminal_), deadline_, attempt_before_expiry_);
}

} // namespace detail
} // namespace zlink
