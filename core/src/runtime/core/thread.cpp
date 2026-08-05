/* SPDX-License-Identifier: MPL-2.0 */

#include "utils/precompiled.hpp"
#include "utils/macros.hpp"
#include "core/thread.hpp"
#include "utils/err.hpp"

#ifdef ZLINK_HAVE_WINDOWS
#include <winnt.h>
#endif

#ifdef __MINGW32__
#include "pthread.h"
#endif

bool zlink::thread_t::get_started () const
{
    return _started;
}

#ifdef ZLINK_HAVE_WINDOWS

extern "C" {
#if defined _WIN32_WCE
static DWORD thread_routine (LPVOID arg_)
#else
static unsigned int __stdcall thread_routine (void *arg_)
#endif
{
    zlink::thread_t *self = static_cast<zlink::thread_t *> (arg_);
    self->applyThreadName ();
    self->_tfn (self->_arg);
    return 0;
}
}

void zlink::thread_t::start (thread_fn *tfn_, void *arg_, const char *name_)
{
    _tfn = tfn_;
    _arg = arg_;
    if (name_)
        strncpy (_name, name_, sizeof (_name) - 1);

    // set default stack size to 4MB to avoid std::map stack overflow on x64
    unsigned int stack = 0;
#if defined _WIN64
    stack = 0x400000;
#endif

#if defined _WIN32_WCE
    _descriptor = (HANDLE) CreateThread (NULL, stack, &::thread_routine, this, 0, &_thread_id);
#else
    _descriptor = (HANDLE) _beginthreadex (NULL, stack, &::thread_routine, this, 0, &_thread_id);
#endif
    win_assert (_descriptor != NULL);
    _started = true;
}

bool zlink::thread_t::is_current_thread () const
{
    return GetCurrentThreadId () == _thread_id;
}

void zlink::thread_t::stop ()
{
    if (_started) {
        const DWORD rc = WaitForSingleObject (_descriptor, INFINITE);
        win_assert (rc != WAIT_FAILED);
        const BOOL rc2 = CloseHandle (_descriptor);
        win_assert (rc2 != 0);
        _descriptor = NULL;
        _started = false;
    }
}

void zlink::thread_t::setSchedulingParameters (int priority_,
                                               int scheduling_policy_,
                                               const std::set<int> &affinity_cpus_)
{
    // Windows builds currently treat scheduling hints as unsupported.
    LIBZLINK_UNUSED (priority_);
    LIBZLINK_UNUSED (scheduling_policy_);
    LIBZLINK_UNUSED (affinity_cpus_);
}

void zlink::thread_t::applySchedulingParameters () // to be called in secondary thread context
{
    // No-op: Windows scheduling hints are not exposed by this abstraction.
}

#ifdef _MSC_VER

namespace
{
#pragma pack(push, 8)
struct thread_info_t
{
    DWORD _type;
    LPCSTR _name;
    DWORD _thread_id;
    DWORD _flags;
};
#pragma pack(pop)
}

#endif

void zlink::thread_t::applyThreadName () // to be called in secondary thread context
{
    if (!_name[0] || !IsDebuggerPresent ())
        return;

#ifdef _MSC_VER

    thread_info_t thread_info;
    thread_info._type = 0x1000;
    thread_info._name = _name;
    thread_info._thread_id = -1;
    thread_info._flags = 0;

    __try {
        const DWORD MS_VC_EXCEPTION = 0x406D1388;
        RaiseException (MS_VC_EXCEPTION, 0, sizeof (thread_info) / sizeof (ULONG_PTR),
                        (ULONG_PTR *) &thread_info);
    }
    __except (EXCEPTION_CONTINUE_EXECUTION) {
    }

#elif defined(__MINGW32__)

    int rc = pthread_setname_np (pthread_self (), _name);
    if (rc)
        return;

#else

        // No platform-specific thread-name API is available for this compiler.

#endif
}


#elif defined ZLINK_HAVE_VXWORKS

extern "C" {
static void *thread_routine (void *arg_)
{
    zlink::thread_t *self = (zlink::thread_t *) arg_;
    self->applySchedulingParameters ();
    self->_tfn (self->_arg);
    return NULL;
}
}

void zlink::thread_t::start (thread_fn *tfn_, void *arg_, const char *name_)
{
    LIBZLINK_UNUSED (name_);
    _tfn = tfn_;
    _arg = arg_;
    _descriptor = taskSpawn (NULL, DEFAULT_PRIORITY, DEFAULT_OPTIONS, DEFAULT_STACK_SIZE,
                             (FUNCPTR) thread_routine, (int) this, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    if (_descriptor != NULL || _descriptor > 0)
        _started = true;
}

void zlink::thread_t::stop ()
{
    if (_started) {
        while ((_descriptor != NULL || _descriptor > 0) && taskIdVerify (_descriptor) == 0) {
        }
        _descriptor = NULL;
        _started = false;
    }
}

bool zlink::thread_t::is_current_thread () const
{
    return taskIdSelf () == _descriptor;
}

void zlink::thread_t::setSchedulingParameters (int priority_,
                                               int schedulingPolicy_,
                                               const std::set<int> &affinity_cpus_)
{
    _thread_priority = priority_;
    _thread_sched_policy = schedulingPolicy_;
    _thread_affinity_cpus = affinity_cpus_;
}

void zlink::thread_t::applySchedulingParameters () // to be called in secondary thread context
{
    int priority = (_thread_priority >= 0 ? _thread_priority : DEFAULT_PRIORITY);
    priority = (priority < UCHAR_MAX ? priority : DEFAULT_PRIORITY);
    if (_descriptor != NULL || _descriptor > 0) {
        taskPrioritySet (_descriptor, priority);
    }
}

void zlink::thread_t::applyThreadName () // to be called in secondary thread context
{
    // No-op: VxWorks builds do not expose a portable thread-name hook here.
}

#else

#include <signal.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/resource.h>

extern "C" {
static void *thread_routine (void *arg_)
{
#if !defined ZLINK_HAVE_OPENVMS && !defined ZLINK_HAVE_ANDROID
    //  Following code will guarantee more predictable latencies as it'll
    //  disallow any signal handling in the I/O thread.
    sigset_t signal_set;
    int rc = sigfillset (&signal_set);
    errno_assert (rc == 0);
    rc = pthread_sigmask (SIG_BLOCK, &signal_set, NULL);
    posix_assert (rc);
#endif
    zlink::thread_t *self = (zlink::thread_t *) arg_;
    self->applySchedulingParameters ();
    self->applyThreadName ();
    self->_tfn (self->_arg);
    return NULL;
}
}

void zlink::thread_t::start (thread_fn *tfn_, void *arg_, const char *name_)
{
    _tfn = tfn_;
    _arg = arg_;
    if (name_)
        strncpy (_name, name_, sizeof (_name) - 1);
    int rc = pthread_create (&_descriptor, NULL, thread_routine, this);
    posix_assert (rc);
    _started = true;
}

void zlink::thread_t::stop ()
{
    if (_started) {
        int rc = pthread_join (_descriptor, NULL);
        if (rc != 0) {
            if (rc != ESRCH && rc != EINVAL)
                posix_assert (rc);
        }
        memset (&_descriptor, 0, sizeof (_descriptor));
        _started = false;
    }
}

bool zlink::thread_t::is_current_thread () const
{
    return bool (pthread_equal (pthread_self (), _descriptor));
}

void zlink::thread_t::setSchedulingParameters (int priority_,
                                               int scheduling_policy_,
                                               const std::set<int> &affinity_cpus_)
{
    _thread_priority = priority_;
    _thread_sched_policy = scheduling_policy_;
    _thread_affinity_cpus = affinity_cpus_;
}

void zlink::thread_t::applySchedulingParameters () // to be called in secondary thread context
{
#if defined _POSIX_THREAD_PRIORITY_SCHEDULING && _POSIX_THREAD_PRIORITY_SCHEDULING >= 0
    int policy = 0;
    struct sched_param param;

#if _POSIX_THREAD_PRIORITY_SCHEDULING == 0 && defined _SC_THREAD_PRIORITY_SCHEDULING
    if (sysconf (_SC_THREAD_PRIORITY_SCHEDULING) < 0) {
        return;
    }
#endif
    int rc = pthread_getschedparam (pthread_self (), &policy, &param);
    posix_assert (rc);

    if (_thread_sched_policy != ZLINK_THREAD_SCHED_POLICY_DFLT) {
        policy = _thread_sched_policy;
    }

    /* Quoting docs:
       "Linux allows the static priority range 1 to 99 for the SCHED_FIFO and
       SCHED_RR policies, and the priority 0 for the remaining policies."
       Other policies may use the "nice value" in place of the priority:
    */
    bool use_nice_instead_priority = (policy != SCHED_FIFO) && (policy != SCHED_RR);

    if (use_nice_instead_priority)
        param.sched_priority =
          0; // this is the only supported priority for most scheduling policies
    else if (_thread_priority != ZLINK_THREAD_PRIORITY_DFLT)
        param.sched_priority = _thread_priority; // user should provide a value between 1 and 99

#ifdef __NetBSD__
    if (policy == SCHED_OTHER)
        param.sched_priority = -1;
#endif

    rc = pthread_setschedparam (pthread_self (), policy, &param);

#if defined(__FreeBSD_kernel__) || defined(__FreeBSD__)
    // If this feature is unavailable at run-time, don't abort.
    if (rc == ENOSYS)
        return;
#endif

    posix_assert (rc);

#if !defined ZLINK_HAVE_VXWORKS
    if (use_nice_instead_priority && _thread_priority != ZLINK_THREAD_PRIORITY_DFLT
        && _thread_priority > 0) {
        // assume the user wants to decrease the thread's nice value
        // i.e., increase the chance of this thread being scheduled: try setting that to
        // maximum priority.
        rc = nice (-20);

        errno_assert (rc != -1);
        // IMPORTANT: EPERM is typically returned for unprivileged processes: that's because
        //            CAP_SYS_NICE capability is required or RLIMIT_NICE resource limit should be changed to avoid EPERM!
    }
#endif

#ifdef ZLINK_HAVE_PTHREAD_SET_AFFINITY
    if (!_thread_affinity_cpus.empty ()) {
        cpu_set_t cpuset;
        CPU_ZERO (&cpuset);
        for (std::set<int>::const_iterator it = _thread_affinity_cpus.begin (),
                                           end = _thread_affinity_cpus.end ();
             it != end; it++) {
            CPU_SET ((int) (*it), &cpuset);
        }
        rc = pthread_setaffinity_np (pthread_self (), sizeof (cpu_set_t), &cpuset);
        posix_assert (rc);
    }
#endif
#endif
}

void zlink::thread_t::applyThreadName () // to be called in secondary thread context
{
    /* The thread name is a cosmetic string, added to ease debugging of
 * multi-threaded applications. It is not a big issue if this value
 * can not be set for any reason (such as Permission denied in some
 * cases where the application changes its EUID, etc.) The value of
 * "int rc" is retained where available, to help debuggers stepping
 * through code to see its value - but otherwise it is ignored.
 */
    if (!_name[0])
        return;

        /* Fails with permission denied on Android 5/6 */
#if defined(ZLINK_HAVE_ANDROID)
    return;
#endif

#if defined(ZLINK_HAVE_PTHREAD_SETNAME_1)
    int rc = pthread_setname_np (_name);
    if (rc)
        return;
#elif defined(ZLINK_HAVE_PTHREAD_SETNAME_2)
    int rc = pthread_setname_np (pthread_self (), _name);
    if (rc)
        return;
#elif defined(ZLINK_HAVE_PTHREAD_SETNAME_3)
    int rc = pthread_setname_np (pthread_self (), _name, NULL);
    if (rc)
        return;
#elif defined(ZLINK_HAVE_PTHREAD_SET_NAME)
    pthread_set_name_np (pthread_self (), _name);
#endif
}

#endif
