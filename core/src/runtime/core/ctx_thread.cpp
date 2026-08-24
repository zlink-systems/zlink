/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"

#include "core/ctx_thread.hpp"
#include "core/thread.hpp"

#include <string.h>

zlink::thread_ctx_t::thread_ctx_t () :
    _thread_priority (ZLINK_THREAD_PRIORITY_DFLT),
    _thread_sched_policy (ZLINK_THREAD_SCHED_POLICY_DFLT)
{
}

void zlink::thread_ctx_t::start_thread (thread_t &thread_,
                                        thread_fn *tfn_,
                                        void *arg_,
                                        const char *name_) const
{
    thread_.setSchedulingParameters (_thread_priority, _thread_sched_policy, _thread_affinity_cpus);

    char namebuf[16] = "";
    snprintf (namebuf, sizeof (namebuf), "%s%sZLINKbg%s%s",
              _thread_name_prefix.empty () ? "" : _thread_name_prefix.c_str (),
              _thread_name_prefix.empty () ? "" : "/", name_ ? "/" : "", name_ ? name_ : "");
    thread_.start (tfn_, arg_, namebuf);
}

int zlink::thread_ctx_t::set (int option_, const void *optval_, size_t optvallen_)
{
    const bool is_int = (optvallen_ == sizeof (int));
    int value = 0;
    if (is_int)
        memcpy (&value, optval_, sizeof (int));

    switch (option_) {
        case ZLINK_THREAD_SCHED_POLICY:
            if (is_int && value >= 0) {
                scoped_lock_t locker (_opt_sync);
                _thread_sched_policy = value;
                return 0;
            }
            break;

        case ZLINK_THREAD_AFFINITY_CPU_ADD:
            if (is_int && value >= 0) {
                scoped_lock_t locker (_opt_sync);
                _thread_affinity_cpus.insert (value);
                return 0;
            }
            break;

        case ZLINK_THREAD_AFFINITY_CPU_REMOVE:
            if (is_int && value >= 0) {
                scoped_lock_t locker (_opt_sync);
                if (0 == _thread_affinity_cpus.erase (value)) {
                    errno = EINVAL;
                    return -1;
                }
                return 0;
            }
            break;

        case ZLINK_THREAD_PRIORITY:
            if (is_int && value >= 0) {
                scoped_lock_t locker (_opt_sync);
                _thread_priority = value;
                return 0;
            }
            break;

        case ZLINK_THREAD_NAME_PREFIX:
            if (optvallen_ > 0 && optvallen_ <= 16) {
                scoped_lock_t locker (_opt_sync);
                _thread_name_prefix.assign (static_cast<const char *> (optval_), optvallen_);
                return 0;
            }
            break;
    }

    errno = EINVAL;
    return -1;
}

int zlink::thread_ctx_t::get (int option_, void *optval_, size_t *optvallen_)
{
    const bool is_int = (*optvallen_ == sizeof (int));
    int *value = static_cast<int *> (optval_);

    switch (option_) {
        case ZLINK_THREAD_SCHED_POLICY:
            if (is_int) {
                scoped_lock_t locker (_opt_sync);
                *value = _thread_sched_policy;
                return 0;
            }
            break;

        case ZLINK_THREAD_NAME_PREFIX:
            {
                scoped_lock_t locker (_opt_sync);
                if (*optvallen_ < _thread_name_prefix.size ()) {
                    *optvallen_ = _thread_name_prefix.size ();
                    break;
                }
                memcpy (optval_, _thread_name_prefix.data (), _thread_name_prefix.size ());
                *optvallen_ = _thread_name_prefix.size ();
                return 0;
            }
    }

    errno = EINVAL;
    return -1;
}
