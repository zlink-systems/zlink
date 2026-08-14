/* SPDX-License-Identifier: MPL-2.0 */

#ifndef __ZLINK_SIGNALER_HPP_INCLUDED__
#define __ZLINK_SIGNALER_HPP_INCLUDED__

#ifdef HAVE_FORK
#include <unistd.h>
#endif

#include "utils/fd.hpp"
#include "utils/macros.hpp"

#include <atomic>

namespace zlink
{
//  This is a cross-platform equivalent to signal_fd. By default there can be
//  at most one signal in the signaler at any given moment. The bool
//  constructor enables coalescing for a shared notification signaler (and an
//  event-only implementation on Windows).

class signaler_t
{
  public:
    signaler_t ();
    explicit signaler_t (bool event_only_);
    ~signaler_t ();

    // Returns the socket/file descriptor
    // May return retired_fd if the signaler could not be initialized.
    fd_t get_fd () const;
#ifdef ZLINK_HAVE_WINDOWS
    HANDLE get_handle () const;
    void reset_event ();
#endif
    void send ();
    int wait (int timeout_) const;
    void recv ();
    int recv_failable ();

    bool valid () const;

#ifdef HAVE_FORK
    // close the file descriptors in a forked child process so that they
    // do not interfere with the context in the parent process.
    void forked ();
#endif

  private:
    //  Underlying write & read file descriptor
    //  Will be -1 if an error occurred during initialization, e.g. we
    //  exceeded the number of available handles
    fd_t _w;
    fd_t _r;

#ifdef ZLINK_HAVE_WINDOWS
    //  Avoid a nonblocking Winsock recv when no wakeup is pending.
    std::atomic<bool> _signaled;
    bool _event_only;
    HANDLE _event;
#else
    std::atomic<bool> _signaled;
    bool _coalescing;
#endif

#ifdef HAVE_FORK
    // the process that created this context. Used to detect forking.
    pid_t pid;
    // idempotent close of file descriptors that is safe to use by destructor
    // and forked().
    void close_internal ();
#endif

    ZLINK_NON_COPYABLE_NOR_MOVABLE (signaler_t)
};
}

#endif
