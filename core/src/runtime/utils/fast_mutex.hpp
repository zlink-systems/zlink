/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_FAST_MUTEX_HPP_INCLUDED__
#define __ZLINK_FAST_MUTEX_HPP_INCLUDED__

#include "utils/err.hpp"
#include "utils/macros.hpp"

#if defined(ZLINK_HAVE_WINDOWS) && !defined(ZLINK_USE_CV_IMPL_PTHREADS)

#include "utils/mutex.hpp"

namespace zlink
{
typedef mutex_t fast_mutex_t;
}

#elif defined ZLINK_HAVE_VXWORKS

#include "utils/mutex.hpp"

namespace zlink
{
typedef mutex_t fast_mutex_t;
}

#else

#include <pthread.h>
#include <errno.h>

namespace zlink
{
class fast_mutex_t
{
  public:
    inline fast_mutex_t ()
    {
        int rc = pthread_mutexattr_init (&_attr);
        posix_assert (rc);

        rc = pthread_mutexattr_settype (&_attr, PTHREAD_MUTEX_RECURSIVE);
        posix_assert (rc);

        rc = pthread_mutex_init (&_mutex, &_attr);
        posix_assert (rc);
    }

    inline ~fast_mutex_t ()
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

  private:
    pthread_mutex_t _mutex;
    pthread_mutexattr_t _attr;

    ZLINK_NON_COPYABLE_NOR_MOVABLE (fast_mutex_t)
};
}

#endif

namespace zlink
{
struct scoped_fast_lock_t
{
    scoped_fast_lock_t (fast_mutex_t &mutex_) : _mutex (mutex_) { _mutex.lock (); }

    ~scoped_fast_lock_t () { _mutex.unlock (); }

  private:
    fast_mutex_t &_mutex;

    ZLINK_NON_COPYABLE_NOR_MOVABLE (scoped_fast_lock_t)
};

struct scoped_optional_fast_lock_t
{
    scoped_optional_fast_lock_t (fast_mutex_t *mutex_) : _mutex (mutex_)
    {
        if (_mutex != NULL)
            _mutex->lock ();
    }

    ~scoped_optional_fast_lock_t ()
    {
        if (_mutex != NULL)
            _mutex->unlock ();
    }

  private:
    fast_mutex_t *_mutex;

    ZLINK_NON_COPYABLE_NOR_MOVABLE (scoped_optional_fast_lock_t)
};
}

#endif
