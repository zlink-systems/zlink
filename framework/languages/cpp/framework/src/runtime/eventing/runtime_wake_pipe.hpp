/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <cstddef>

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace zlink::framework::runtime::eventing
{

/*
 * Provides an OS-level wake source for a runtime-owned poller. The owner
 * signals it from the stopping thread and drains it from the poller thread,
 * so poller close and socket destruction remain single-threaded.
 */
class runtime_wake_pipe_t final
{
  public:
    runtime_wake_pipe_t () = default;
    ~runtime_wake_pipe_t () noexcept { close (); }

    runtime_wake_pipe_t (const runtime_wake_pipe_t &) = delete;
    runtime_wake_pipe_t &operator= (const runtime_wake_pipe_t &) = delete;

    bool open () noexcept
    {
        if (_read_fd >= 0 || _write_fd >= 0)
            return _read_fd >= 0 && _write_fd >= 0;
        int fds[2] = {-1, -1};
#if defined(_WIN32)
        if (_pipe (fds, 4096, _O_BINARY) != 0)
            return false;
#else
        if (::pipe (fds) != 0)
            return false;
        for (const int fd : fds) {
            const int flags = ::fcntl (fd, F_GETFL, 0);
            if (flags < 0
                || ::fcntl (fd, F_SETFL, flags | O_NONBLOCK) != 0) {
                ::close (fds[0]);
                ::close (fds[1]);
                return false;
            }
        }
#endif
        _read_fd = fds[0];
        _write_fd = fds[1];
        return true;
    }

    int read_fd () const noexcept { return _read_fd; }

    void signal () const noexcept
    {
        if (_write_fd < 0)
            return;
        const char value = 1;
#if defined(_WIN32)
        const auto result = _write (_write_fd, &value, 1);
#else
        const auto result = ::write (_write_fd, &value, 1);
#endif
        static_cast<void> (result);
    }

    void drain () const noexcept
    {
        if (_read_fd < 0)
            return;
        char buffer[64];
        for (;;) {
#if defined(_WIN32)
            const int count = _read (_read_fd, buffer, sizeof (buffer));
#else
            const auto count = ::read (_read_fd, buffer, sizeof (buffer));
#endif
            if (count <= 0
                || count < static_cast<decltype (count)> (sizeof (buffer)))
                return;
        }
    }

    void close () noexcept
    {
        close_fd (_read_fd);
        close_fd (_write_fd);
    }

  private:
    static void close_fd (int &fd) noexcept
    {
        if (fd < 0)
            return;
#if defined(_WIN32)
        (void) _close (fd);
#else
        (void) ::close (fd);
#endif
        fd = -1;
    }

    int _read_fd = -1;
    int _write_fd = -1;
};

} // namespace zlink::framework::runtime::eventing
