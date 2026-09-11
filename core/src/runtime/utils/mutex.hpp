/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_MUTEX_HPP_INCLUDED__
#define __ZLINK_MUTEX_HPP_INCLUDED__

#include "utils/err.hpp"
#include "utils/macros.hpp"

//  Mutex class encapsulates OS mutex in a platform-independent way.
//
//  `mutex_t` is a plain (non-recursive) mutex: acquiring it twice on one
//  thread is a bug, not a supported mode. `recursive_mutex_t` is the explicit
//  opt-in for the few owners whose call graph re-enters its own critical
//  section; it derives from `mutex_t`, so every `mutex_t &`/`mutex_t *` API
//  (scoped locks, condition variables) accepts both without duplication.
//
//  Debug and sanitizer builds arm PTHREAD_MUTEX_ERRORCHECK on the plain type,
//  so a re-entrant acquisition fails with EDEADLK and aborts in posix_assert
//  instead of silently reintroducing recursion.

#if defined(ZLINK_MUTEX_FORCE_ERRORCHECK) || !defined(NDEBUG)                  \
  || defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
#define ZLINK_MUTEX_ERRORCHECK 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
#define ZLINK_MUTEX_ERRORCHECK 1
#endif
#endif

#if defined(ZLINK_HAVE_WINDOWS) && !defined(ZLINK_USE_CV_IMPL_PTHREADS)

#include "utils/windows.hpp"

namespace zlink
{
//  Windows gives the plain and recursive contracts distinct primitives.
//  SRWLOCK is non-recursive, while CRITICAL_SECTION provides the explicit
//  recursive opt-in used by recursive_mutex_t.
class mutex_t
{
  public:
    mutex_t () : _kind (plain_kind) { InitializeSRWLock (&_native.srwlock); }

    ~mutex_t ()
    {
        if (_kind == recursive_kind)
            DeleteCriticalSection (&_native.cs);
    }

    void lock ()
    {
        if (_kind == plain_kind)
            AcquireSRWLockExclusive (&_native.srwlock);
        else
            EnterCriticalSection (&_native.cs);
    }

    bool try_lock ()
    {
        return _kind == plain_kind
                 ? TryAcquireSRWLockExclusive (&_native.srwlock) != FALSE
                 : TryEnterCriticalSection (&_native.cs) != FALSE;
    }

    void unlock ()
    {
        if (_kind == plain_kind)
            ReleaseSRWLockExclusive (&_native.srwlock);
        else
            LeaveCriticalSection (&_native.cs);
    }

    bool uses_srwlock () const { return _kind == plain_kind; }
    SRWLOCK *get_srwlock () { return &_native.srwlock; }
    CRITICAL_SECTION *get_cs () { return &_native.cs; }

  protected:
    enum kind_t
    {
        plain_kind,
        recursive_kind
    };

    explicit mutex_t (kind_t kind_) : _kind (kind_)
    {
        if (_kind == plain_kind)
            InitializeSRWLock (&_native.srwlock);
        else
            InitializeCriticalSection (&_native.cs);
    }

  private:
    union native_t
    {
        SRWLOCK srwlock;
        CRITICAL_SECTION cs;
    } _native;
    const kind_t _kind;

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
#include <errno.h>

namespace zlink
{
class mutex_t
{
  public:
    inline mutex_t () { init (plain_kind); }

    inline ~mutex_t ()
    {
        int rc = pthread_mutex_destroy (&_mutex);
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

  protected:
    enum kind_t
    {
#if defined ZLINK_MUTEX_ERRORCHECK
        plain_kind = PTHREAD_MUTEX_ERRORCHECK,
#else
        plain_kind = PTHREAD_MUTEX_DEFAULT,
#endif
        recursive_kind = PTHREAD_MUTEX_RECURSIVE
    };

    explicit inline mutex_t (kind_t kind_) { init (kind_); }

  private:
    inline void init (kind_t kind_)
    {
        pthread_mutexattr_t attr;
        int rc = pthread_mutexattr_init (&attr);
        posix_assert (rc);

        rc = pthread_mutexattr_settype (&attr, static_cast<int> (kind_));
        posix_assert (rc);

        rc = pthread_mutex_init (&_mutex, &attr);
        posix_assert (rc);

        rc = pthread_mutexattr_destroy (&attr);
        posix_assert (rc);
    }

    pthread_mutex_t _mutex;

    ZLINK_NON_COPYABLE_NOR_MOVABLE (mutex_t)
};
}

#endif


namespace zlink
{
//  Same object, recursive acquisition allowed. Declare a member with this type
//  only where a re-entrant acquisition on one thread is proven to exist.
class recursive_mutex_t : public mutex_t
{
  public:
#if defined(ZLINK_HAVE_WINDOWS) && !defined(ZLINK_USE_CV_IMPL_PTHREADS)
    inline recursive_mutex_t () : mutex_t (recursive_kind) {}
#elif defined(ZLINK_HAVE_VXWORKS)
    //  The VxWorks primitive is already recursive.
    inline recursive_mutex_t () {}
#else
    inline recursive_mutex_t () : mutex_t (recursive_kind) {}
#endif

  private:
    ZLINK_NON_COPYABLE_NOR_MOVABLE (recursive_mutex_t)
};


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
