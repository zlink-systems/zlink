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

// Everything the admission state tracks per routed target. One record replaces
// the parallel queue/wake-version/ready-set maps the state used to key by the
// same target, so admitting a send touches one node instead of four.
struct routed_target_state_t
{
    std::deque<uint64_t> queue;
    uint64_t wake_version = 0;
    // Terminal events are counted, not just applied to registered records, so
    // a caller-thread attempt that runs without a pending record can still see
    // the terminal it raced.
    uint64_t terminal_epoch = 0;
    int terminal_error = 0;
    size_t inline_attempts = 0;
    bool ready_marked = false;

    bool idle () const noexcept
    {
        return queue.empty () && inline_attempts == 0 && !ready_marked;
    }
};

using routed_targets_t = std::map<routed_target_key_t, routed_target_state_t>;
using routed_target_iterator_t = routed_targets_t::iterator;

// Target records outlive their work so a steady send stream reuses one node
// and its queue storage. Only a socket that churns through more distinct
// transport pairs than this keeps idle records around long enough to prune.
constexpr size_t k_retained_target_records = 32;



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
    routed_target_iterator_t target{};
    state_t state = state_t::ready;
    uint64_t observed_wake = 0;
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max ();
    std::shared_ptr<routed_admission_request_t> request;
    bool forced_terminal = false;
    submit_result_t forced_result = submit_result_t::internal_error;
    int forced_error = 0;
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

    // Admits one routed request.
    //
    // A request whose target has nothing queued ahead of it is attempted on
    // the caller thread before any pending record exists. Core accepts that
    // attempt in the uncongested case, so the request reaches its terminal
    // without ever allocating a pending record, a deadline entry or a queue
    // slot; only a request that has to wait is materialised.
    routed_admission_ticket_t enqueue (const zlink_routed_submit_target_t &target_,
                                       std::shared_ptr<routed_admission_request_t> request_)
    {
        if (!request_)
            throw submit_error_t (submit_result_t::invalid_argument, EINVAL);

        routed_target_iterator_t target;
        uint64_t observed_wake = 0;
        uint64_t observed_terminal = 0;
        bool admit_inline = false;
        std::shared_ptr<pending_operation_t> queued;
        {
            std::lock_guard<std::mutex> lock (_mutex);
            if (_closed)
                throw submit_error_t (submit_result_t::terminated, ETERM);
            target = ensure_target_locked (target_);
            if (target->second.queue.empty () && target->second.inline_attempts == 0) {
                // Reserve the target for the caller thread. A concurrent
                // enqueue for the same target sees the reservation and queues
                // behind it, so the target's FIFO stream is preserved.
                ++target->second.inline_attempts;
                ++_active_pumps;
                observed_wake = target->second.wake_version;
                observed_terminal = target->second.terminal_epoch;
                admit_inline = true;
            } else {
                queued = std::make_shared<pending_operation_t> ();
                queued->id = ++_next_id;
                queued->target = target;
                queued->request = std::move (request_);
                queued->state = pending_operation_t::state_t::queued;
                target->second.queue.push_back (queued->id);
                _operations.emplace (queued->id, queued);
                refresh_schedule_locked ();
            }
        }
        if (!admit_inline) {
            arm_deadline (queued);
            return routed_admission_ticket_t (weak_from_this (), queued->id);
        }
        return admit_on_caller_thread (target, std::move (request_), observed_wake,
                                       observed_terminal);
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
        invoke_terminal (*operation->request, submit_result_t::terminated, ECANCELED);
        return true;
    }

    void on_event (const zlink_routed_send_ready_event_t &event_) noexcept
    {
        std::lock_guard<std::mutex> lock (_mutex);
        if (_closed)
            return;
        const routed_target_key_t key = target_key (zlink_routed_submit_target_t{
          event_.peer_rid, event_.transport_pair_id, event_.transport_pair_generation});
        bool changed = false;
        const auto target = _targets.find (key);
        if (event_.state == ZLINK_ROUTED_SEND_TERMINAL) {
            if (target != _targets.end ()) {
                ++target->second.terminal_epoch;
                target->second.terminal_error = event_.terminal_errno;
                for (const uint64_t id : target->second.queue) {
                    const auto entry = _operations.find (id);
                    if (entry == _operations.end ())
                        continue;
                    entry->second->forced_terminal = true;
                    entry->second->forced_result =
                      terminal_submit_result (event_.terminal_errno);
                    entry->second->forced_error = event_.terminal_errno;
                }
                if (!target->second.queue.empty ()) {
                    const auto front = _operations.find (target->second.queue.front ());
                    if (front != _operations.end ()
                        && front->second->state != pending_operation_t::state_t::in_flight) {
                        front->second->state = pending_operation_t::state_t::ready;
                        unregister_deadline_locked (*front->second);
                        mark_target_ready_locked (target);
                    }
                    changed = true;
                }
            }
        } else {
            if (target != _targets.end ())
                changed = wake_target_locked (target);
            // A DEALER operation can be accepted before Core has a
            // concrete transport-pair target.  Such work is keyed by the
            // empty target and any writable routed edge is its exact
            // signal to select a current pair and retry once.
            const auto generic = _targets.find (routed_target_key_t{});
            if (generic != _targets.end () && generic != target)
                changed = wake_target_locked (generic) || changed;
        }
        if (changed)
            refresh_schedule_locked ();
    }

    void on_generic_ready () noexcept
    {
        std::lock_guard<std::mutex> lock (_mutex);
        if (_closed)
            return;
        const auto generic = _targets.find (routed_target_key_t{});
        if (generic == _targets.end ())
            return;
        (void) wake_target_locked (generic);
        refresh_schedule_locked ();
    }

    void pump (uint64_t generation_) noexcept
    {
        struct terminal_action_t
        {
            std::shared_ptr<routed_admission_request_t> request;
            submit_result_t result;
            int error;
        };
        std::vector<std::shared_ptr<pending_operation_t>> attempts;
        std::vector<std::shared_ptr<routed_admission_request_t>> accepted_actions;
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
                const routed_target_iterator_t target = _ready_targets.front ();
                _ready_targets.pop_front ();
                target->second.ready_marked = false;
                if (target->second.queue.empty ())
                    continue;
                const auto found = _operations.find (target->second.queue.front ());
                if (found == _operations.end ()
                    || found->second->state != pending_operation_t::state_t::ready)
                    continue;
                if (found->second->forced_terminal) {
                    terminal_actions.push_back ({found->second->request,
                                                 found->second->forced_result,
                                                 found->second->forced_error});
                    remove_operation_locked (found->second);
                    continue;
                }
                found->second->state = pending_operation_t::state_t::in_flight;
                found->second->observed_wake = target->second.wake_version;
                unregister_deadline_locked (*found->second);
                attempts.push_back (found->second);
            }
        }

        for (const auto &operation : attempts) {
            routed_attempt_result_t result = run_attempt (*operation->request);

            std::shared_ptr<routed_admission_request_t> accepted;
            std::shared_ptr<routed_admission_request_t> terminal;
            {
                std::lock_guard<std::mutex> lock (_mutex);
                const auto found = _operations.find (operation->id);
                if (found == _operations.end ())
                    continue;
                if (_closed) {
                    terminal = operation->request;
                    remove_operation_locked (operation);
                    result = {submit_result_t::terminated, ETERM};
                } else if (operation->forced_terminal) {
                    terminal = operation->request;
                    remove_operation_locked (operation);
                    result = {operation->forced_result, operation->forced_error};
                } else if (result.result == submit_result_t::ok) {
                    accepted = operation->request;
                    remove_operation_locked (operation);
                } else if (result.result == submit_result_t::backpressured) {
                    const auto now = std::chrono::steady_clock::now ();
                    if (operation->deadline
                          != std::chrono::steady_clock::time_point::max ()
                        && now >= operation->deadline) {
                        terminal = operation->request;
                        remove_operation_locked (operation);
                        result.error = ETIMEDOUT;
                    } else if (operation->target->second.wake_version
                               > operation->observed_wake) {
                        operation->state = pending_operation_t::state_t::ready;
                        mark_target_ready_locked (operation->target);
                    } else {
                        operation->state = pending_operation_t::state_t::waiting;
                        register_deadline_locked (*operation);
                    }
                } else {
                    terminal = operation->request;
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
            invoke_accepted (*accepted);
        for (auto &terminal : terminal_actions)
            invoke_terminal (*terminal.request, terminal.result, terminal.error);
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
            _targets.clear ();
            _ready_targets.clear ();
            _deadlines.clear ();
            _scheduled = false;
            ++_schedule_generation;
        }
        for (const auto &operation : pending)
            invoke_terminal (*operation->request, submit_result_t::terminated, ETERM);
    }

  private:
    // Runs the first attempt for a reserved target on the caller thread.
    routed_admission_ticket_t admit_on_caller_thread (
      routed_target_iterator_t target_,
      std::shared_ptr<routed_admission_request_t> request_,
      uint64_t observed_wake_,
      uint64_t observed_terminal_)
    {
        routed_admission_request_t &request = *request_;
        routed_attempt_result_t result = run_attempt (request);

        // Resolving the deadline is the request's own work and may call into
        // Core, so it runs outside the admission lock -- and only for a
        // request that did not get through on its first attempt.
        std::chrono::steady_clock::time_point deadline =
          std::chrono::steady_clock::time_point::max ();
        bool deadline_failed = false;
        if (result.result == submit_result_t::backpressured) {
            try {
                deadline = request.deadline ();
            }
            catch (...) {
                deadline_failed = true;
            }
        }

        std::shared_ptr<pending_operation_t> parked;
        bool accepted = false;
        bool has_terminal = false;
        {
            std::lock_guard<std::mutex> lock (_mutex);
            --target_->second.inline_attempts;
            if (_closed) {
                result = {submit_result_t::terminated, ETERM};
                has_terminal = true;
            } else if (target_->second.terminal_epoch != observed_terminal_) {
                const int error = target_->second.terminal_error;
                result = {terminal_submit_result (error), error};
                has_terminal = true;
            } else if (result.result == submit_result_t::ok) {
                accepted = true;
            } else if (result.result == submit_result_t::backpressured) {
                const auto now = std::chrono::steady_clock::now ();
                const bool expired =
                  deadline_failed
                  || (deadline != std::chrono::steady_clock::time_point::max ()
                      && now >= deadline);
                if (expired) {
                    result.error = ETIMEDOUT;
                    has_terminal = true;
                } else {
                    parked = park_locked (target_, std::move (request_),
                                          deadline, observed_wake_);
                }
            } else {
                has_terminal = true;
            }
            promote_front_locked (target_);
            prune_target_locked (target_);
            refresh_schedule_locked ();
            --_active_pumps;
            _quiesced.notify_all ();
        }
        if (accepted)
            invoke_accepted (request);
        else if (has_terminal)
            invoke_terminal (request, result.result, result.error);
        if (parked)
            return routed_admission_ticket_t (weak_from_this (), parked->id);
        return {};
    }

    // Materialises the pending record for a request whose caller-thread
    // attempt was backpressured. The request keeps the front of its target
    // queue: anything enqueued during the attempt was queued behind it.
    std::shared_ptr<pending_operation_t> park_locked (
      routed_target_iterator_t target_,
      std::shared_ptr<routed_admission_request_t> request_,
      std::chrono::steady_clock::time_point deadline_,
      uint64_t observed_wake_)
    {
        std::shared_ptr<pending_operation_t> operation =
          std::make_shared<pending_operation_t> ();
        operation->id = ++_next_id;
        operation->target = target_;
        operation->request = std::move (request_);
        operation->deadline = deadline_;
        operation->observed_wake = observed_wake_;
        const bool woken = target_->second.wake_version > observed_wake_;
        operation->state = woken ? pending_operation_t::state_t::ready
                                 : pending_operation_t::state_t::waiting;
        target_->second.queue.push_front (operation->id);
        _operations.emplace (operation->id, operation);
        if (woken)
            mark_target_ready_locked (target_);
        else
            register_deadline_locked (*operation);
        return operation;
    }

    // Arms the deadline of a request that was queued behind other work. The
    // request resolves its own timeout, which can call into Core, so this
    // runs outside the admission lock and re-checks the record.
    void arm_deadline (const std::shared_ptr<pending_operation_t> &operation_) noexcept
    {
        std::chrono::steady_clock::time_point deadline;
        try {
            deadline = operation_->request->deadline ();
        }
        catch (...) {
            deadline = std::chrono::steady_clock::now ();
        }
        if (deadline == std::chrono::steady_clock::time_point::max ())
            return;
        std::lock_guard<std::mutex> lock (_mutex);
        if (_closed || _operations.find (operation_->id) == _operations.end ())
            return;
        if (operation_->deadline != std::chrono::steady_clock::time_point::max ())
            return;
        operation_->deadline = deadline;
        if (operation_->state != pending_operation_t::state_t::in_flight)
            register_deadline_locked (*operation_);
        refresh_schedule_locked ();
    }

    routed_target_iterator_t ensure_target_locked (
      const zlink_routed_submit_target_t &target_)
    {
        routed_target_key_t key = target_key (target_);
        const auto found = _targets.lower_bound (key);
        if (found != _targets.end () && !(key < found->first))
            return found;
        return _targets.emplace_hint (found, std::move (key), routed_target_state_t{});
    }

    // Records a writable edge for one target and releases its front record if
    // that record was waiting for exactly this signal.
    bool wake_target_locked (routed_target_iterator_t target_)
    {
        if (target_->second.queue.empty () && target_->second.inline_attempts == 0)
            return false;
        ++target_->second.wake_version;
        if (target_->second.queue.empty ())
            return false;
        const auto front = _operations.find (target_->second.queue.front ());
        if (front == _operations.end ()
            || front->second->state != pending_operation_t::state_t::waiting)
            return false;
        front->second->state = pending_operation_t::state_t::ready;
        unregister_deadline_locked (*front->second);
        mark_target_ready_locked (target_);
        return true;
    }

    void mark_target_ready_locked (routed_target_iterator_t target_)
    {
        if (target_->second.ready_marked)
            return;
        target_->second.ready_marked = true;
        _ready_targets.push_back (target_);
    }

    void prune_target_locked (routed_target_iterator_t target_)
    {
        if (_targets.size () <= k_retained_target_records || !target_->second.idle ())
            return;
        _targets.erase (target_);
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

    void promote_front_locked (routed_target_iterator_t target_)
    {
        if (target_->second.queue.empty ())
            return;
        const auto operation = _operations.find (target_->second.queue.front ());
        if (operation == _operations.end ())
            return;
        if (operation->second->state == pending_operation_t::state_t::queued)
            operation->second->state = pending_operation_t::state_t::ready;
        if (operation->second->state == pending_operation_t::state_t::ready)
            mark_target_ready_locked (target_);
    }

    void remove_operation_locked (const std::shared_ptr<pending_operation_t> &operation_)
    {
        unregister_deadline_locked (*operation_);
        const routed_target_iterator_t target = operation_->target;
        std::deque<uint64_t> &queue = target->second.queue;
        const auto id = std::find (queue.begin (), queue.end (), operation_->id);
        const bool was_front = id == queue.begin ();
        if (id != queue.end ())
            queue.erase (id);
        if (queue.empty ())
            prune_target_locked (target);
        else if (was_front)
            promote_front_locked (target);
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
            std::shared_ptr<routed_admission_request_t> request = operation->second->request;
            remove_operation_locked (operation->second);
            terminal_actions_.push_back (
              {std::move (request), submit_result_t::backpressured, ETIMEDOUT});
        }
    }

    void refresh_schedule_locked ()
    {
        if (_closed)
            return;
        const bool ready = !_ready_targets.empty ();
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

    static routed_attempt_result_t run_attempt (routed_admission_request_t &request_) noexcept
    {
        try {
            return request_.attempt ();
        }
        catch (const submit_error_t &error) {
            return {error.result (), error.internal_errno ()};
        }
        catch (...) {
            return {submit_result_t::internal_error, EIO};
        }
    }

    static void invoke_accepted (routed_admission_request_t &request_) noexcept
    {
        try {
            request_.accepted ();
        }
        catch (...) {
        }
    }

    static void invoke_terminal (routed_admission_request_t &request_,
                                 submit_result_t result_,
                                 int error_) noexcept
    {
        try {
            request_.terminal (result_, error_);
        }
        catch (...) {
        }
    }

    std::mutex _mutex;
    std::condition_variable _quiesced;
    std::map<uint64_t, std::shared_ptr<pending_operation_t>> _operations;
    routed_targets_t _targets;
    std::deque<routed_target_iterator_t> _ready_targets;
    std::multimap<std::chrono::steady_clock::time_point, uint64_t> _deadlines;
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
                          std::shared_ptr<routed_admission_request_t> request_)
{
    if (!owner_)
        throw submit_error_t (submit_result_t::invalid_state, EINVAL);
    return owner_->enqueue (target_, std::move (request_));
}

} // namespace detail
} // namespace zlink
