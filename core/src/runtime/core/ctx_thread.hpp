/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_CTX_THREAD_HPP_INCLUDED__
#define __ZLINK_CTX_THREAD_HPP_INCLUDED__

#include <set>
#include <string>

#include "core/thread.hpp"
#include "utils/mutex.hpp"

namespace zlink
{
class thread_ctx_t
{
  public:
    thread_ctx_t ();

    //  Start a new thread with proper scheduling parameters.
    void
    start_thread (thread_t &thread_, thread_fn *tfn_, void *arg_, const char *name_ = NULL) const;

    int set (int option_, const void *optval_, size_t optvallen_);
    int get (int option_, void *optval_, size_t *optvallen_);

  private:
    //  Synchronisation of access to thread options.
    mutable recursive_mutex_t _opt_sync;

    //  Thread parameters.
    int _thread_priority;
    int _thread_sched_policy;
    std::set<int> _thread_affinity_cpus;
    std::string _thread_name_prefix;
};
}

#endif
