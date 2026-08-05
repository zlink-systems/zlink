/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_MUTEX_HPP_INCLUDED__
#define __ZLINK_MUTEX_HPP_INCLUDED__

#include "utils/err.hpp"
#include "utils/macros.hpp"

//  Mutex class encapsulates OS mutex in a platform-independent way.

#if defined(ZLINK_HAVE_WINDOWS) && !defined(ZLINK_USE_CV_IMPL_PTHREADS)

#include "utils/windows.hpp"

namespace zlink
{
class mutex_t
{
  public:
    mutex_t () { InitializeCriticalSection (&_cs); }

    ~mutex_t () { DeleteCriticalSection (&_cs); }

    void lock () { EnterCriticalSection (&_cs); }

    bool try_lock () { return (TryEnterCriticalSection (&_cs)) ? true : false; }

    void unlock () { LeaveCriticalSection (&_cs); }

    CRITICAL_SECTION *get_cs () { return &_cs; }

  private:
    CRITICAL_SECTION _cs;

    ZLINK_NON_COPYABLE_NOR_MOVABLE (mutex_t)
};
}

#elif defined ZLINK_HAVE_VXWORKS

#include <vxWorks.h>
#include <semLib.h>

namespace zlink
{
class mutex_t
{
  public:
    inline mutex_t ()
    {
        _semId = semMCreate (SEM_Q_PRIORITY | SEM_INVERSION_SAFE | SEM_DELETE_SAFE);
    }

    inline ~mutex_t () { semDelete (_semId); }

    inline void lock () { semTake (_semId, WAIT_FOREVER); }

    inline bool try_lock ()
    {
        if (semTake (_semId, NO_WAIT) == OK) {
            return true;
        }
        return false;
    }

    inline void unlock () { semGive (_semId); }

  private:
    SEM_ID _semId;

    ZLINK_NON_COPYABLE_NOR_MOVABLE (mutex_t)
};
}

#else

#include <pthread.h>

namespace zlink
{
class mutex_t
{
  public:
    inline mutex_t ()
    {
        int rc = pthread_mutexattr_init (&_attr);
        posix_assert (rc);

        rc = pthread_mutexattr_settype (&_attr, PTHREAD_MUTEX_RECURSIVE);
        posix_assert (rc);

        rc = pthread_mutex_init (&_mutex, &_attr);
        posix_assert (rc);
    }

    inline ~mutex_t ()
    {
        int rc = pthread_mutex_destroy (&_mutex);
        posix_assert (rc);

        rc = pthread_mutexattr_destroy (&_attr);
        posix_assert (rc);
    }

    inline void lock ()
    {
        int rc = pthread_mutex_lock (&_mutex);
        posix_assert (rc);
    }

    inline bool try_lock ()
    {
        int rc = pthread_mutex_trylock (&_mutex);
        if (rc == EBUSY)
            return false;

        posix_assert (rc);
        return true;
    }

    inline void unlock ()
    {
        int rc = pthread_mutex_unlock (&_mutex);
        posix_assert (rc);
    }

    inline pthread_mutex_t *get_mutex () { return &_mutex; }

  private:
    pthread_mutex_t _mutex;
    pthread_mutexattr_t _attr;

    ZLINK_NON_COPYABLE_NOR_MOVABLE (mutex_t)
};
}

#endif


namespace zlink
{
struct scoped_lock_t
{
    scoped_lock_t (mutex_t &mutex_) : _mutex (mutex_) { _mutex.lock (); }

    ~scoped_lock_t () { _mutex.unlock (); }

  private:
    mutex_t &_mutex;

    ZLINK_NON_COPYABLE_NOR_MOVABLE (scoped_lock_t)
};


struct scoped_optional_lock_t
{
    scoped_optional_lock_t (mutex_t *mutex_) : _mutex (mutex_)
    {
        if (_mutex != NULL)
            _mutex->lock ();
    }

    ~scoped_optional_lock_t ()
    {
        if (_mutex != NULL)
            _mutex->unlock ();
    }

  private:
    mutex_t *_mutex;

    ZLINK_NON_COPYABLE_NOR_MOVABLE (scoped_optional_lock_t)
};
}

#endif
