/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_CORE_CONTROL_RUNTIME_HPP_INCLUDED__
#define __ZLINK_CORE_CONTROL_RUNTIME_HPP_INCLUDED__

#include "utils/macros.hpp"
#include "utils/stdint.hpp"
#include "core/thread.hpp"
#include "utils/condition_variable.hpp"
#include "utils/mutex.hpp"

#include <map>
#include <string>

namespace zlink
{
class ctx_t;
typedef void (control_task_fn) (void *);

class control_runtime_t
{
  public:
    explicit control_runtime_t (ctx_t *ctx_, const char *thread_name_ = "core-ctrl");
    ~control_runtime_t ();

    bool start ();
    void stop ();

    uint64_t add_periodic_task (control_task_fn *fn_,
                                void *arg_,
                                uint32_t interval_ms_,
                                bool run_immediately_);
    int remove_task (uint64_t task_id_);
    int wakeup_task (uint64_t task_id_);

    bool is_current_thread () const;

  private:
    struct task_entry_t
    {
        task_entry_t () :
            id (0), fn (NULL), arg (NULL), interval_ms (0), next_run_ms (0), scheduled (false)
        {
        }

        uint64_t id;
        control_task_fn *fn;
        void *arg;
        uint32_t interval_ms;
        uint64_t next_run_ms;
        bool scheduled;
        std::multimap<uint64_t, uint64_t>::iterator schedule_it;
        //  The task's schedule slot is allocated exactly once, at add time.
        //  Descheduling extracts the node here and rescheduling moves it
        //  back, so wakeup and every periodic tick are allocation-free and
        //  cannot throw after the task exists.
        std::multimap<uint64_t, uint64_t>::node_type cached_node;
    };

    struct due_call_t
    {
        uint64_t task_id;
        control_task_fn *fn;
        void *arg;
    };

    static void run (void *arg_);
    void schedule_task_locked (task_entry_t *task_);
    void deschedule_task_locked (task_entry_t *task_);
    void loop ();

    ctx_t *_ctx;
    std::string _thread_name;
    thread_t _thread;
    mutex_t _sync;
    condition_variable_t _cv;
    std::map<uint64_t, task_entry_t> _tasks;
    std::multimap<uint64_t, uint64_t> _schedule;
    uint64_t _next_task_id;
    uint64_t _active_task_id;
    bool _running;
    bool _stopping;

    ZLINK_NON_COPYABLE_NOR_MOVABLE (control_runtime_t)
};
}

#endif
