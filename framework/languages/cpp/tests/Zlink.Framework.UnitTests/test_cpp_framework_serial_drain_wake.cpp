/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "runtime/execution/serial_execution_queue.hpp"
#include "runtime/execution/serial_application_record.hpp"
#include "../support/manual_internal_scheduler.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <linux/futex.h>
#include <memory>
#include <mutex>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace
{
namespace rt = zlink::framework::runtime;
using namespace std::chrono_literals;
static_assert (sizeof (std::size_t) > sizeof (int), "requires an indirect atomic waiter pool");
constexpr auto observation_timeout = 2s;
constexpr unsigned child_watchdog_seconds = 10;

class event_t
{
  public:
    void set ()
    {
        std::lock_guard lock (_mutex);
        _set = true;
        _changed.notify_all ();
    }
    bool wait ()
    {
        std::unique_lock lock (_mutex);
        return _changed.wait_for (lock, observation_timeout, [&] { return _set; });
    }

  private:
    std::mutex _mutex;
    std::condition_variable _changed;
    bool _set = false;
};

struct native_wait_t
{
    event_t reached;
    event_t resume;
    const int *address = nullptr;
    int version = 0;

    void enter (const void *wait_address, int expected)
    {
        address = static_cast<const int *> (wait_address);
        version = expected;
        reached.set ();
        if (!resume.wait ()) {
            std::fputs ("native wait release timed out\n", stderr);
            std::_Exit (EXIT_FAILURE);
        }
    }
};

// pthread condition variables do not alter libstdc++'s atomic waiter pool.
thread_local native_wait_t *armed_wait = nullptr;

class rejected_scheduler_t final : public rt::offload_executor_t
{
  public:
    rejected_scheduler_t () : offload_executor_t (1, "rejected-drain-wake") {}
    bool try_submit_internal (std::function<void ()>) override { return false; }
};

struct rejected_capture_t
{
    event_t &destroying;
    event_t &release;

    ~rejected_capture_t ()
    {
        destroying.set ();
        if (!release.wait ()) {
            std::fputs ("publication destruction release timed out\n", stderr);
            std::_Exit (EXIT_FAILURE);
        }
    }
};

[[noreturn]] void rollback_case (rt::serial_work_lane_t lane)
{
    rejected_scheduler_t worker;
    auto queue = std::make_shared<rt::serial_execution_queue_t> (
      worker, rt::serial_execution_queue_options_t{},
      rt::serial_execution_queue_t::error_handler_t{}, rt::serial_lane_policy_t::spot_wide ());
    event_t destroying, release_capture, drained;
    native_wait_t native;
    std::atomic_bool accepted{true};
    std::atomic_int handlers{0};
    auto capture = std::make_shared<rejected_capture_t> (destroying, release_capture);
    std::thread producer ([&, capture = std::move (capture)] () mutable {
        accepted = queue->try_post (
          "rejected-publication", [&, capture = std::move (capture)] { ++handlers; },
          rt::serial_work_options_t{.lane = lane});
    });
    if (!destroying.wait () || queue->pending_count (lane) != 1) {
        std::fputs ("rollback did not retain the real publication count\n", stderr);
        std::_Exit (EXIT_FAILURE);
    }
    // The failed schedule's gate-state notify precedes this capture destructor.
    std::thread waiter ([&] {
        armed_wait = &native;
        queue->drain ();
        armed_wait = nullptr;
        drained.set ();
    });
    if (!native.reached.wait ()) {
        std::fputs ("drain did not reach the native atomic wait\n", stderr);
        std::_Exit (EXIT_FAILURE);
    }
    release_capture.set ();
    producer.join ();
    const bool rolled_back =
      !accepted && handlers == 0 && queue->pending_count () == 0 && queue->pending_bytes () == 0;
    const bool notified = __atomic_load_n (native.address, __ATOMIC_ACQUIRE) != native.version;
    native.resume.set ();
    const bool completed = drained.wait ();
    std::fprintf (stderr,
                  "rollback lane=%s real-count-zero=%d waiter-version-changed=%d drained=%d\n",
                  lane == rt::serial_work_lane_t::application ? "application" : "lifecycle",
                  rolled_back, notified, completed);
    // Each case runs in a child, so a failed real wait cannot retain test threads.
    std::_Exit (rolled_back && notified && completed ? EXIT_SUCCESS : EXIT_FAILURE);
}

[[noreturn]] void suffix_case (bool unpublished)
{
    zlink::framework::test::manual_internal_scheduler_t worker;
    auto queue = std::make_shared<rt::serial_execution_queue_t> (
      worker, rt::serial_execution_queue_options_t{},
      rt::serial_execution_queue_t::error_handler_t{}, rt::serial_lane_policy_t::spot_wide ());
    queue->set_application_start_policy ([] (const auto &) { return false; });
    worker.run_all ();
    auto record = std::make_shared<rt::serial_application_record_t> ();
    record->admission.take_over ();
    event_t start_drain, drained;
    native_wait_t native;
    std::atomic_int handlers{0};
    const auto publish = [&] {
        if (!queue->try_post (
              "exported-record", [&] { ++handlers; },
              rt::serial_work_options_t{.application_record = record})) {
            std::fputs ("suffix publication was rejected\n", stderr);
            std::_Exit (EXIT_FAILURE);
        }
    };
    if (!unpublished) {
        publish ();
        worker.run_all ();
    }
    std::thread waiter ([&] {
        if (!start_drain.wait ())
            std::_Exit (EXIT_FAILURE);
        armed_wait = &native;
        queue->drain ();
        armed_wait = nullptr;
        drained.set ();
    });
    const auto begin_wait = [&] {
        if (queue->pending_count () != 1)
            std::_Exit (EXIT_FAILURE);
        start_drain.set ();
        if (!native.reached.wait ()) {
            std::fputs ("suffix drain did not reach the native wait\n", stderr);
            std::_Exit (EXIT_FAILURE);
        }
    };
    bool retired = false;
    bool notified_before_retire = false;
    record->retire_invocation = [&] (auto outcome) {
        retired = static_cast<bool> (outcome);
        // Observe before gate-state notifications can collide with this waiter's pool.
        notified_before_retire =
          __atomic_load_n (native.address, __ATOMIC_ACQUIRE) != native.version;
    };
    rt::serial_execution_queue_t::producer_cut_t cut;
    if (unpublished) {
        cut = [&] (const auto &take) {
            // Publication occurs after the owner's import and before the producer cut.
            publish ();
            begin_wait ();
            take ();
            return true;
        };
    } else {
        begin_wait ();
    }
    auto suffix = queue->take_application_suffix ({}, std::move (cut));
    worker.run_all ();
    const auto &exported = suffix.result ().value ();
    const bool transferred = retired && handlers == 0 && exported.size () == 1
                             && exported.front () == record && queue->pending_count () == 0
                             && queue->pending_bytes () == 0;
    native.resume.set ();
    const bool completed = drained.wait ();
    std::fprintf (stderr, "suffix source=%s transferred=%d notified-before-retire=%d drained=%d\n",
                  unpublished ? "publication" : "owned-fifo", transferred, notified_before_retire,
                  completed);
    std::_Exit (transferred && notified_before_retire && completed ? EXIT_SUCCESS : EXIT_FAILURE);
}
} // namespace

extern "C" long __real_syscall (long number, ...);

extern "C" long __wrap_syscall (long number, ...)
{
    if (number != SYS_futex) {
        std::fprintf (stderr, "unexpected wrapped syscall: %ld\n", number);
        std::_Exit (EXIT_FAILURE);
    }
    va_list arguments;
    va_start (arguments, number);
    const auto *address = va_arg (arguments, const void *);
    const int operation = va_arg (arguments, int);
    const int value = va_arg (arguments, int);
    if (operation == FUTEX_WAIT || operation == FUTEX_WAIT_PRIVATE) {
        auto *timeout = va_arg (arguments, void *);
        va_end (arguments);
        if (auto *wait = std::exchange (armed_wait, nullptr))
            wait->enter (address, value);
        return __real_syscall (number, address, operation, value, timeout);
    }
    va_end (arguments);
    if (operation == FUTEX_WAKE || operation == FUTEX_WAKE_PRIVATE)
        return __real_syscall (number, address, operation, value);
    std::fprintf (stderr, "unexpected wrapped futex operation: %d\n", operation);
    std::_Exit (EXIT_FAILURE);
}

int main ()
{
    bool passed = true;
    enum class scenario_t
    {
        application_rollback,
        lifecycle_rollback,
        imported_suffix,
        unpublished_suffix
    };
    for (const auto scenario : {scenario_t::application_rollback, scenario_t::lifecycle_rollback,
                                scenario_t::imported_suffix, scenario_t::unpublished_suffix}) {
        const auto child = ::fork ();
        if (child == 0) {
            ::alarm (child_watchdog_seconds);
            switch (scenario) {
                case scenario_t::application_rollback:
                    rollback_case (rt::serial_work_lane_t::application);
                case scenario_t::lifecycle_rollback:
                    rollback_case (rt::serial_work_lane_t::lifecycle);
                case scenario_t::imported_suffix:
                    suffix_case (false);
                case scenario_t::unpublished_suffix:
                    suffix_case (true);
            }
        }
        if (child < 0) {
            std::perror ("fork");
            return EXIT_FAILURE;
        }
        int status = 0;
        pid_t waited;
        do {
            waited = ::waitpid (child, &status, 0);
        } while (waited < 0 && errno == EINTR);
        passed =
          waited == child && WIFEXITED (status) && WEXITSTATUS (status) == EXIT_SUCCESS && passed;
    }
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
