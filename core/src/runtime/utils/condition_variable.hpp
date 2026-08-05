/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_CONDITON_VARIABLE_HPP_INCLUDED__
#define __ZLINK_CONDITON_VARIABLE_HPP_INCLUDED__

#include "utils/err.hpp"
#include "utils/mutex.hpp"

//  Condition variable class encapsulates OS mutex in a platform-independent way.

#if defined(ZLINK_USE_CV_IMPL_NONE)

namespace zlink
{
class condition_variable_t
{
  public:
    inline condition_variable_t () { zlink_assert (false); }

    inline int wait (mutex_t *mutex_, int timeout_)
    {
        zlink_assert (false);
        return -1;
    }

    inline void broadcast () { zlink_assert (false); }

    ZLINK_NON_COPYABLE_NOR_MOVABLE (condition_variable_t)
};
}

#elif defined(ZLINK_USE_CV_IMPL_WIN32API)

#include "utils/windows.hpp"

namespace zlink
{
class condition_variable_t
{
  public:
    inline condition_variable_t () { InitializeConditionVariable (&_cv); }

    inline int wait (mutex_t *mutex_, int timeout_)
    {
        int rc = SleepConditionVariableCS (&_cv, mutex_->get_cs (), timeout_);

        if (rc != 0)
            return 0;

        rc = GetLastError ();

        if (rc != ERROR_TIMEOUT)
            win_assert (rc);

        errno = EAGAIN;
        return -1;
    }

    inline void broadcast () { WakeAllConditionVariable (&_cv); }

  private:
    CONDITION_VARIABLE _cv;

    ZLINK_NON_COPYABLE_NOR_MOVABLE (condition_variable_t)
};
}

#elif defined(ZLINK_USE_CV_IMPL_STL11)

#include <condition_variable>

namespace zlink
{
class condition_variable_t
{
  public:
    condition_variable_t () ZLINK_DEFAULT;

    int wait (mutex_t *mutex_, int timeout_)
    {
        // this assumes that the mutex mutex_ has been locked by the caller
        int res = 0;
        if (timeout_ == -1) {
            _cv.wait (
              *mutex_); // unlock mtx and wait cv.notify_all(), lock mtx after cv.notify_all()
        } else if (_cv.wait_for (*mutex_, std::chrono::milliseconds (timeout_))
                   == std::cv_status::timeout) {
            // time expired
            errno = EAGAIN;
            res = -1;
        }
        return res;
    }

    void broadcast ()
    {
        // this assumes that the mutex associated with _cv has been locked by the caller
        _cv.notify_all ();
    }

  private:
    std::condition_variable_any _cv;

    ZLINK_NON_COPYABLE_NOR_MOVABLE (condition_variable_t)
};
}

#elif defined(ZLINK_USE_CV_IMPL_VXWORKS)

#include <sysLib.h>

namespace zlink
{
class condition_variable_t
{
  public:
    inline condition_variable_t () ZLINK_DEFAULT;

    inline ~condition_variable_t ()
    {
        scoped_lock_t l (_listenersMutex);
        for (size_t i = 0; i < _listeners.size (); i++) {
            semDelete (_listeners[i]);
        }
    }

    inline int wait (mutex_t *mutex_, int timeout_)
    {
        //Atomically releases lock, blocks the current executing thread,
        //and adds it to the list of threads waiting on *this. The thread
        //will be unblocked when broadcast() is executed.
        //It may also be unblocked spuriously. When unblocked, regardless
        //of the reason, lock is reacquired and wait exits.

        SEM_ID sem = semBCreate (SEM_Q_PRIORITY, SEM_EMPTY);
        {
            scoped_lock_t l (_listenersMutex);
            _listeners.push_back (sem);
        }
        mutex_->unlock ();

        int rc;
        if (timeout_ < 0)
            rc = semTake (sem, WAIT_FOREVER);
        else {
            int ticksPerSec = sysClkRateGet ();
            int timeoutTicks = (timeout_ * ticksPerSec) / 1000 + 1;
            rc = semTake (sem, timeoutTicks);
        }

        {
            scoped_lock_t l (_listenersMutex);
            // remove sem from listeners
            for (size_t i = 0; i < _listeners.size (); i++) {
                if (_listeners[i] == sem) {
                    _listeners.erase (_listeners.begin () + i);
                    break;
                }
            }
            semDelete (sem);
        }
        mutex_->lock ();

        if (rc == 0)
            return 0;

        if (rc == S_objLib_OBJ_TIMEOUT) {
            errno = EAGAIN;
            return -1;
        }

        return -1;
    }

    inline void broadcast ()
    {
        scoped_lock_t l (_listenersMutex);
        for (size_t i = 0; i < _listeners.size (); i++) {
            semGive (_listeners[i]);
        }
    }

  private:
    mutex_t _listenersMutex;
    std::vector<SEM_ID> _listeners;

    ZLINK_NON_COPYABLE_NOR_MOVABLE (condition_variable_t)
};
}

#elif defined(ZLINK_USE_CV_IMPL_PTHREADS)

#include <pthread.h>

namespace zlink
{
class condition_variable_t
{
  public:
    inline condition_variable_t ()
    {
        pthread_condattr_t attr;
        pthread_condattr_init (&attr);
#ifndef ZLINK_HAVE_OSX
        pthread_condattr_setclock (&attr, CLOCK_MONOTONIC);
#endif
        int rc = pthread_cond_init (&_cond, &attr);
        posix_assert (rc);
    }

    inline ~condition_variable_t ()
    {
        int rc = pthread_cond_destroy (&_cond);
        posix_assert (rc);
    }

    inline int wait (mutex_t *mutex_, int timeout_)
    {
        int rc;

        if (timeout_ != -1) {
            struct timespec timeout;

#ifdef ZLINK_HAVE_OSX
            timeout.tv_sec = 0;
            timeout.tv_nsec = 0;
#else
            rc = clock_gettime (CLOCK_MONOTONIC, &timeout);
            posix_assert (rc);
#endif

            timeout.tv_sec += timeout_ / 1000;
            timeout.tv_nsec += (timeout_ % 1000) * 1000000;

            if (timeout.tv_nsec >= 1000000000) {
                timeout.tv_sec++;
                timeout.tv_nsec -= 1000000000;
            }
#ifdef ZLINK_HAVE_OSX
            rc = pthread_cond_timedwait_relative_np (&_cond, mutex_->get_mutex (), &timeout);
#else
            rc = pthread_cond_timedwait (&_cond, mutex_->get_mutex (), &timeout);
#endif
        } else
            rc = pthread_cond_wait (&_cond, mutex_->get_mutex ());

        if (rc == 0)
            return 0;

        if (rc == ETIMEDOUT) {
            errno = EAGAIN;
            return -1;
        }

        posix_assert (rc);
        return -1;
    }

    inline void broadcast ()
    {
        int rc = pthread_cond_broadcast (&_cond);
        posix_assert (rc);
    }

  private:
    pthread_cond_t _cond;

    ZLINK_NON_COPYABLE_NOR_MOVABLE (condition_variable_t)
};
}

#endif

#endif
