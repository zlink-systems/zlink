/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "core/control_runtime.hpp"

#include "core/ctx.hpp"
#include "utils/clock.hpp"

#include <limits.h>
#include <vector>

namespace zlink
{
control_runtime_t::control_runtime_t (ctx_t *ctx_, const char *thread_name_) :
    _ctx (ctx_),
    _thread_name (thread_name_ ? thread_name_ : "core-ctrl"),
    _next_task_id (1),
    _active_task_id (0),
    _running (false),
    _stopping (false)
{
    zlink_assert (_ctx);
}

control_runtime_t::~control_runtime_t ()
{
    stop ();
}

bool control_runtime_t::start ()
{
    scoped_lock_t lock (_sync);
    if (_running)
        return true;

    _stopping = false;
    _active_task_id = 0;
    _ctx->start_thread (_thread, run, this, _thread_name.c_str ());
    _running = true;
    return true;
}

void control_runtime_t::stop ()
{
    {
        scoped_lock_t lock (_sync);
        if (!_running)
            return;
        _stopping = true;
        _cv.broadcast ();
    }

    _thread.stop ();

    scoped_lock_t lock (_sync);
    _tasks.clear ();
    _schedule.clear ();
    _active_task_id = 0;
    _running = false;
    _stopping = false;
}

uint64_t control_runtime_t::add_periodic_task (control_task_fn *fn_,
                                                       void *arg_,
                                                       uint32_t interval_ms_,
                                                       bool run_immediately_)
{
    if (!fn_ || interval_ms_ == 0) {
        errno = EINVAL;
        return 0;
    }

    zlink::clock_t clock;
    const uint64_t now = clock.now_ms ();

    scoped_lock_t lock (_sync);
    if (!_running || _stopping) {
        errno = ETERM;
        return 0;
    }

    uint64_t task_id = _next_task_id++;
    if (task_id == 0)
        task_id = _next_task_id++;

    //  The whole add is one sealed transaction: every allocation (the task
    //  entry and its one schedule node) happens here, and any failure rolls
    //  back to the pre-call state with ENOMEM. After this returns, wakeup,
    //  reschedule and removal of the task are allocation-free.
    std::map<uint64_t, task_entry_t>::iterator task_it = _tasks.end ();
    try {
        task_it = _tasks.emplace (task_id, task_entry_t ()).first;
        task_entry_t &task = task_it->second;
        task.id = task_id;
        task.fn = fn_;
        task.arg = arg_;
        task.interval_ms = interval_ms_;
        task.next_run_ms = run_immediately_ ? now : now + interval_ms_;
        task.schedule_it =
          _schedule.insert (std::make_pair (task.next_run_ms, task.id));
        task.scheduled = true;
    }
    catch (const std::bad_alloc &) {
        if (task_it != _tasks.end ())
            _tasks.erase (task_it);
        errno = ENOMEM;
        return 0;
    }
    _cv.broadcast ();
    return task_id;
}

int control_runtime_t::remove_task (uint64_t task_id_)
{
    if (task_id_ == 0)
        return 0;

    scoped_lock_t lock (_sync);
    if (_thread.get_started () && _thread.is_current_thread () && _active_task_id == task_id_) {
        std::map<uint64_t, task_entry_t>::iterator it = _tasks.find (task_id_);
        if (it != _tasks.end ()) {
            deschedule_task_locked (&it->second);
            _tasks.erase (it);
        }
        return 0;
    }
    std::map<uint64_t, task_entry_t>::iterator it = _tasks.find (task_id_);
    if (it != _tasks.end ()) {
        deschedule_task_locked (&it->second);
        _tasks.erase (it);
    }
    while (_active_task_id == task_id_)
        _cv.wait (&_sync, -1);
    return 0;
}

int control_runtime_t::wakeup_task (uint64_t task_id_)
{
    if (task_id_ == 0)
        return 0;

    scoped_lock_t lock (_sync);
    std::map<uint64_t, task_entry_t>::iterator it = _tasks.find (task_id_);
    if (it == _tasks.end ()) {
        errno = EINVAL;
        return -1;
    }
    deschedule_task_locked (&it->second);
    it->second.next_run_ms = 0;
    schedule_task_locked (&it->second);
    _cv.broadcast ();
    return 0;
}

bool control_runtime_t::is_current_thread () const
{
    return _thread.get_started () && _thread.is_current_thread ();
}

void control_runtime_t::run (void *arg_)
{
    control_runtime_t *self = static_cast<control_runtime_t *> (arg_);
    //  The loop is allocation-free in steady state and callbacks are sealed
    //  individually; this last-resort seal commits the worker's terminal
    //  lifecycle so no waiter (remove_task, context teardown) blocks on a
    //  stale active ID and no new task is accepted by a dead worker.
    try {
        self->loop ();
    }
    catch (const std::bad_alloc &) {
        scoped_lock_t lock (self->_sync);
        self->_stopping = true;
        self->_active_task_id = 0;
        self->_cv.broadcast ();
    }
}

void control_runtime_t::schedule_task_locked (task_entry_t *task_)
{
    if (!task_)
        return;

    deschedule_task_locked (task_);
    //  Reuses the node allocated at add time: rekey and move it back in.
    //  Every caller deschedules an existing slot first (add inserts its
    //  initial node directly), so the cached node always exists and this
    //  path never allocates and never throws.
    zlink_assert (!task_->cached_node.empty ());
    task_->cached_node.key () = task_->next_run_ms;
    task_->schedule_it = _schedule.insert (std::move (task_->cached_node));
    task_->scheduled = true;
}

void control_runtime_t::deschedule_task_locked (task_entry_t *task_)
{
    if (!task_ || !task_->scheduled)
        return;

    //  Extract keeps the node allocated for the next schedule_task_locked.
    task_->cached_node = _schedule.extract (task_->schedule_it);
    task_->scheduled = false;
}

void control_runtime_t::loop ()
{
    zlink::clock_t clock;

    //  One due call per pass, and each reschedule reuses the task's cached
    //  schedule node: the steady-state loop performs no allocation, so no
    //  bad_alloc can unwind the worker thread and terminate the process.
    while (true) {
        due_call_t call;
        call.task_id = 0;
        call.fn = NULL;
        call.arg = NULL;

        {
            scoped_lock_t lock (_sync);
            while (call.task_id == 0 && !_stopping) {
                const uint64_t now = clock.now_ms ();

                while (!_schedule.empty ()) {
                    const std::multimap<uint64_t, uint64_t>::iterator next = _schedule.begin ();
                    if (next->first > now)
                        break;

                    const uint64_t task_id = next->second;
                    std::map<uint64_t, task_entry_t>::iterator task_it = _tasks.find (task_id);
                    if (task_it == _tasks.end ()) {
                        _schedule.erase (next);
                        continue;
                    }

                    task_entry_t &task = task_it->second;
                    task.cached_node = _schedule.extract (next);
                    task.scheduled = false;

                    task.next_run_ms = now + task.interval_ms;
                    schedule_task_locked (&task);

                    call.task_id = task.id;
                    call.fn = task.fn;
                    call.arg = task.arg;
                    break;
                }

                if (call.task_id != 0 || _stopping)
                    break;

                if (_schedule.empty ()) {
                    _cv.wait (&_sync, -1);
                } else {
                    const uint64_t next_deadline = _schedule.begin ()->first;
                    const int wait_ms =
                      next_deadline <= now
                        ? 0
                        : static_cast<int> (std::min<uint64_t> (next_deadline - now,
                                                                static_cast<uint64_t> (INT_MAX)));
                    _cv.wait (&_sync, wait_ms);
                }
            }

            if (_stopping)
                break;

            _active_task_id = call.task_id;
        }

        try {
            call.fn (call.arg);
        }
        catch (const std::bad_alloc &) {
            //  Policy: the failed tick is dropped, never the worker or the
            //  task. The task was rescheduled before the call, so it simply
            //  retries at its next interval. The epilogue below must still
            //  run or a concurrent remove_task()/ctx teardown waits forever
            //  on the stale active ID.
        }

        {
            scoped_lock_t lock (_sync);
            if (_active_task_id == call.task_id) {
                _active_task_id = 0;
                _cv.broadcast ();
            }
        }
    }

    scoped_lock_t lock (_sync);
    _active_task_id = 0;
    _cv.broadcast ();
}
}
