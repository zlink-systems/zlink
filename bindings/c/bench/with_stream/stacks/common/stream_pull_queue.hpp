// Shared MPSC hand-off queue for the "pull model" stream benchmark servers
// (asio_pull, cppserver_pull).
//
// zlink's STREAM socket is a pull-model socket: the I/O thread decodes a packet
// into a pipe, wakes the application thread, and the application thread calls
// recv/send.  The plain asio/cppserver echo servers instead echo inside the read
// completion handler on the I/O thread.  This queue provides the missing thread
// hop so the two structures can be compared.
//
// Wake mechanism (default: condvar; ZLINK_BENCH_PULL_WAKE=eventfd switches):
//   condvar  - mutex + condition_variable (futex wake).  Same single scheduler
//              wake-up as zlink's signaler but without the write()/read()
//              syscalls on an fd, so it is a *lower bound* on the hand-off cost.
//   eventfd  - mutex-protected queue plus an eventfd the worker blocks on,
//              which is exactly what zlink's mailbox/poller signaler does
//              (write(8B) on the I/O thread, blocking read on the app thread).
// Both are provided so the wake cost itself can be measured.

#ifndef ZLINK_BENCH_STREAM_PULL_QUEUE_HPP
#define ZLINK_BENCH_STREAM_PULL_QUEUE_HPP

#include <sys/eventfd.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <string>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <utility>
#include <vector>

namespace stream_echo
{

inline bool pull_wake_uses_eventfd ()
{
    const char *mode = std::getenv ("ZLINK_BENCH_PULL_WAKE");
    return mode != NULL && std::strcmp (mode, "eventfd") == 0;
}

inline const char *pull_wake_name ()
{
    return pull_wake_uses_eventfd () ? "eventfd" : "condvar";
}

template <class item_t> class pull_queue_t
{
  public:
    pull_queue_t () :
        mutex (),
        cond (),
        queue (),
        stopped (false),
        use_eventfd (pull_wake_uses_eventfd ()),
        wake_fd (-1)
    {
        if (use_eventfd)
            wake_fd = ::eventfd (0, 0);
    }

    ~pull_queue_t ()
    {
        if (wake_fd >= 0)
            ::close (wake_fd);
    }

    void push (item_t &&item)
    {
        {
            std::lock_guard<std::mutex> guard (mutex);
            queue.push_back (std::move (item));
        }
        signal ();
    }

    //  Blocks until at least one item is available, then drains everything that
    //  is queued (mirrors zlink's application-thread drain loop per wake).
    //  Returns false once stop() has been called and the queue is empty.
    bool drain (std::vector<item_t> &out)
    {
        out.clear ();
        if (!use_eventfd) {
            std::unique_lock<std::mutex> lock (mutex);
            while (queue.empty () && !stopped)
                cond.wait (lock);
            return take_locked (out);
        }

        for (;;) {
            {
                std::lock_guard<std::mutex> guard (mutex);
                if (!queue.empty () || stopped)
                    return take_locked (out);
            }
            uint64_t value = 0;
            const ssize_t rc = ::read (wake_fd, &value, sizeof (value));
            if (rc < 0 && errno != EINTR)
                return false;
        }
    }

    void stop ()
    {
        {
            std::lock_guard<std::mutex> guard (mutex);
            stopped = true;
        }
        if (use_eventfd)
            signal ();
        else
            cond.notify_all ();
    }

  private:
    //  Caller holds the lock (lock_guard or unique_lock).
    bool take_locked (std::vector<item_t> &out)
    {
        if (queue.empty ())
            return false;
        out.reserve (queue.size ());
        while (!queue.empty ()) {
            out.push_back (std::move (queue.front ()));
            queue.pop_front ();
        }
        return true;
    }

    void signal ()
    {
        if (!use_eventfd) {
            cond.notify_one ();
            return;
        }
        const uint64_t one = 1;
        const ssize_t rc = ::write (wake_fd, &one, sizeof (one));
        (void) rc;
    }

    std::mutex mutex;
    std::condition_variable cond;
    std::deque<item_t> queue;
    bool stopped;
    const bool use_eventfd;
    int wake_fd;
};

//  Proof that every echo really went through the worker thread: the io thread
//  bumps enqueued, the worker bumps processed (and off_worker if it ever runs
//  on another thread), and the counters are printed once at shutdown.
struct pull_stats_t
{
    std::atomic<long> enqueued;
    std::atomic<long> processed;
    std::atomic<long> off_worker;
    std::atomic<long> echo_posts;

    pull_stats_t () : enqueued (0), processed (0), off_worker (0), echo_posts (0) {}

    std::string line (const char *stack) const
    {
        char buf[512];
        std::snprintf (buf, sizeof (buf),
                       "PULL_STATS stack=%s wake=%s enqueued=%ld processed_by_worker=%ld "
                       "processed_off_worker=%ld echo_posts=%ld",
                       stack, pull_wake_name (), enqueued.load (std::memory_order_relaxed),
                       processed.load (std::memory_order_relaxed),
                       off_worker.load (std::memory_order_relaxed),
                       echo_posts.load (std::memory_order_relaxed));
        return std::string (buf);
    }
};

} // namespace stream_echo

#endif
