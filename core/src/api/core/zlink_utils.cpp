/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "utils/macros.hpp"
#include "utils/clock.hpp"
#include "utils/err.hpp"
#include "core/thread.hpp"
#include "utils/atomic_counter.hpp"
#include "utils/atomic_ptr.hpp"
#include "utils/random.hpp"
#include <assert.h>
#include <new>

#if !defined ZLINK_HAVE_WINDOWS
#include <unistd.h>
#endif

void zlink_sleep (int seconds_)
{
#if defined ZLINK_HAVE_WINDOWS
    Sleep (seconds_ * 1000);
#else
    sleep (seconds_);
#endif
}

void *zlink_stopwatch_start ()
{
    uint64_t *watch = static_cast<uint64_t *> (malloc (sizeof (uint64_t)));
    alloc_assert (watch);
    *watch = zlink::clock_t::now_us ();
    return static_cast<void *> (watch);
}

unsigned long zlink_stopwatch_intermediate (void *watch_)
{
    const uint64_t end = zlink::clock_t::now_us ();
    const uint64_t start = *static_cast<uint64_t *> (watch_);
    return static_cast<unsigned long> (end - start);
}

unsigned long zlink_stopwatch_stop (void *watch_)
{
    const unsigned long res = zlink_stopwatch_intermediate (watch_);
    free (watch_);
    return res;
}

void *zlink_thread_start (zlink_thread_fn *func_, void *arg_)
{
    zlink::thread_t *thread = new (std::nothrow) zlink::thread_t;
    alloc_assert (thread);
    thread->start (func_, arg_, "ZLINKapp");
    return thread;
}

void zlink_thread_join (void *thread_)
{
    zlink::thread_t *p_thread = static_cast<zlink::thread_t *> (thread_);
    p_thread->stop ();
    LIBZLINK_DELETE (p_thread);
}

//  --------------------------------------------------------------------------
//  Initialize a new atomic counter, which is set to zero

void *zlink_atomic_counter_new (void)
{
    zlink::atomic_counter_t *counter = new (std::nothrow) zlink::atomic_counter_t;
    alloc_assert (counter);
    return counter;
}

//  Se the value of the atomic counter

void zlink_atomic_counter_set (void *counter_, int value_)
{
    (static_cast<zlink::atomic_counter_t *> (counter_))->set (value_);
}

//  Increment the atomic counter, and return the old value

int zlink_atomic_counter_inc (void *counter_)
{
    return (static_cast<zlink::atomic_counter_t *> (counter_))->add (1);
}

//  Decrement the atomic counter and return 1 (if counter >= 1), or
//  0 if counter hit zero.

int zlink_atomic_counter_dec (void *counter_)
{
    return (static_cast<zlink::atomic_counter_t *> (counter_))->sub (1) ? 1 : 0;
}

//  Return actual value of atomic counter

int zlink_atomic_counter_value (void *counter_)
{
    return (static_cast<zlink::atomic_counter_t *> (counter_))->get ();
}

//  Destroy atomic counter, and set reference to NULL

void zlink_atomic_counter_destroy (void **counter_p_)
{
    delete (static_cast<zlink::atomic_counter_t *> (*counter_p_));
    *counter_p_ = NULL;
}
