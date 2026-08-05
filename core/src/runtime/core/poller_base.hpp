/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_POLLER_BASE_HPP_INCLUDED__
#define __ZLINK_POLLER_BASE_HPP_INCLUDED__

#include <map>

#include "utils/clock.hpp"
#include "utils/atomic_counter.hpp"
#include "core/ctx_thread.hpp"

namespace zlink
{
struct i_poll_events;

// Base class for implementations of the poller concept (ASIO backend).
//
// For documentation of the public methods, see the description of the poller_t
// concept.
class poller_base_t
{
  public:
    poller_base_t () ZLINK_DEFAULT;
    virtual ~poller_base_t ();

    // Methods from the poller concept.
    int get_load () const;
    void add_timer (int timeout_, zlink::i_poll_events *sink_, int id_);
    void cancel_timer (zlink::i_poll_events *sink_, int id_);

  protected:
    //  Called by individual poller implementations to manage the load.
    void adjust_load (int amount_);

    //  Executes any timers that are due. Returns number of milliseconds
    //  to wait to match the next timer or 0 meaning "no timers".
    uint64_t execute_timers ();

  private:
    //  Clock instance private to this I/O thread.
    clock_t _clock;

    //  List of active timers.
    struct timer_info_t
    {
        zlink::i_poll_events *sink;
        int id;
    };
    typedef std::multimap<uint64_t, timer_info_t> timers_t;
    timers_t _timers;

    //  Load of the poller. Currently the number of file descriptors
    //  registered.
    atomic_counter_t _load;

    ZLINK_NON_COPYABLE_NOR_MOVABLE (poller_base_t)
};

//  Base class for a poller with a single worker thread.
class worker_poller_base_t : public poller_base_t
{
  public:
    worker_poller_base_t (const thread_ctx_t &ctx_);

    // Methods from the poller concept.
    void start (const char *name = NULL);

  protected:
    //  Checks whether the currently executing thread is the worker thread
    //  via an assertion.
    //  Should be called by the add_fd, removed_fd, set_*, reset_* functions
    //  to ensure correct usage.
    void check_thread () const;

    //  Stops the worker thread. Should be called from the destructor of the
    //  leaf class.
    void stop_worker ();

  private:
    //  Main worker thread routine.
    static void worker_routine (void *arg_);

    virtual void loop () = 0;

    // Reference to ZLINK context.
    const thread_ctx_t &_ctx;

    //  Handle of the physical thread doing the I/O work.
    thread_t _worker;
};
}

#endif
