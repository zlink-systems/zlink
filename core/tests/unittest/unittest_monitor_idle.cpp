/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil.hpp"
#include "testutil_unity.hpp"
#include "testutil_monitoring.hpp"
#include "api/monitoring/monitor_api_internal.hpp"
#include "core/control_runtime.hpp"
#include "core/ctx.hpp"
#include "sockets/pair/pair.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <atomic>
#include <climits>
#ifdef ZLINK_HAVE_WINDOWS
#include <io.h>
#include <fcntl.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif
#ifdef __linux__
#include <sys/resource.h>
#endif

namespace
{
const int test_wait_timeout_ms = 3000;
const int test_idle_observation_ms = 250;
const int test_no_further_callbacks_ms = 100;
const int test_idle_measurement_ms = 10000;
const char *const diagnostic_env = "ZLINK_MONITOR_TASK_DIAG";
FILE *diagnostics = NULL;
int saved_stderr = -1;
#ifdef ZLINK_HAVE_WINDOWS
int saved_stderr_mode = -1;
#endif
std::string saved_diagnostic_env;
bool had_diagnostic_env = false;
void *source = NULL;
void *monitor = NULL;
size_t diagnostic_record_bytes = 0;
int idle_ms = test_idle_observation_ms;
std::atomic<bool> fail_next_send (false);
#ifdef __linux__
std::atomic<bool> fail_next_message_init (false);
std::atomic<int> message_init_failure_errno (0);
#endif

void fail_send_once (void *)
{
    if (fail_next_send.exchange (false, std::memory_order_acq_rel))
        throw std::bad_alloc ();
}

void set_diagnostic_env (const char *value_)
{
#ifdef ZLINK_HAVE_WINDOWS
    TEST_ASSERT_EQUAL_INT (0, _putenv_s (diagnostic_env, value_ ? value_ : ""));
#else
    TEST_ASSERT_EQUAL_INT (
      0, value_ ? setenv (diagnostic_env, value_, 1) : unsetenv (diagnostic_env));
#endif
}

uint64_t callback_count ()
{
#ifdef ZLINK_HAVE_WINDOWS
    _lock_file (stderr);
#else
    flockfile (stderr);
#endif
    fflush (stderr);
#ifdef ZLINK_HAVE_WINDOWS
    const int64_t bytes = _filelengthi64 (_fileno (diagnostics));
    _unlock_file (stderr);
#else
    struct stat status;
    const int result = fstat (fileno (diagnostics), &status);
    funlockfile (stderr);
    TEST_ASSERT_EQUAL_INT (0, result);
    const int64_t bytes = status.st_size;
#endif
    TEST_ASSERT_TRUE (bytes >= 0);
    TEST_ASSERT_EQUAL_UINT64 (0, bytes % diagnostic_record_bytes);
    return static_cast<uint64_t> (bytes) / diagnostic_record_bytes;
}

void open_monitor (uint64_t hwm_events_ = 8)
{
    source = test_context_socket (ZLINK_SOCKET_PAIR);
    TEST_ASSERT_NOT_NULL (source);
    zlink_socket_monitor_open_options_t options;
    memset (&options, 0, sizeof (options));
    options.events = ZLINK_EVENT_LISTENING;
    options.monitor_hwm_bytes =
      socket_monitor_event_accounted_bytes () * hwm_events_;
    monitor = zlink_socket_monitor_open (source, &options);
    TEST_ASSERT_NOT_NULL (monitor);
    const socket_handle_t handle = as_socket_handle (source);
    char record[128];
    diagnostic_record_bytes = static_cast<size_t> (snprintf (
      record, sizeof (record), "raw-monitor-task source-socket=%p\n",
      static_cast<void *> (handle.socket)));
}

void emit_event (int value_)
{
    const socket_handle_t handle = as_socket_handle (source);
    handle.socket->event_listening (
      zlink::make_unconnected_bind_endpoint_pair ("inproc://monitor-idle"),
      static_cast<zlink::fd_t> (value_));
}

void receive_event (uint64_t value_)
{
    zlink_monitor_event_t event;
    TEST_ASSERT_TRUE (zlink_test_wait_until (test_wait_timeout_ms, [&] {
        return recv_monitor_event_from_socket (monitor, &event, ZLINK_DONTWAIT)
               == 0;
    }));
    TEST_ASSERT_EQUAL_UINT64 (ZLINK_EVENT_LISTENING, event.event);
    TEST_ASSERT_EQUAL_UINT64 (value_, event.value);
}

void barrier_callback (void *arg_)
{
    static_cast<std::atomic<bool> *> (arg_)->store (true, std::memory_order_release);
}

void control_barrier ()
{
    zlink::control_runtime_t *runtime =
      static_cast<zlink::ctx_t *> (get_test_context ())->control_runtime ();
    TEST_ASSERT_NOT_NULL (runtime);
    std::atomic<bool> completed (false);
    const uint64_t task = runtime->add_task (
      &barrier_callback, &completed, UINT_MAX, false);
    TEST_ASSERT_TRUE (task != 0);
    TEST_ASSERT_EQUAL_INT (0, runtime->schedule_task_after (task, 0));
    const bool ran = zlink_test_wait_until (test_wait_timeout_ms, [&] {
        return completed.load (std::memory_order_acquire);
    });
    TEST_ASSERT_EQUAL_INT (0, runtime->remove_task (task));
    TEST_ASSERT_TRUE (ran);
}

uint64_t worker_pending_bytes ()
{
    const socket_handle_t handle = as_socket_handle (source);
    return handle.socket->test_monitor_worker_pending_bytes ();
}

void wait_for_worker_drain ()
{
    TEST_ASSERT_TRUE (zlink_test_wait_until (test_wait_timeout_ms, [&] {
        return worker_pending_bytes () == 0;
    }));
    control_barrier ();
}

#ifdef __linux__
double cpu_seconds (const rusage &usage_)
{
    return usage_.ru_utime.tv_sec + usage_.ru_stime.tv_sec
           + (usage_.ru_utime.tv_usec + usage_.ru_stime.tv_usec) / 1000000.0;
}
#endif
}

#ifdef __linux__
extern "C" zlink_config_result_t __real_zlink_msg_init_size (zlink_msg_t *, size_t);

extern "C" zlink_config_result_t __wrap_zlink_msg_init_size (
  zlink_msg_t *message_, size_t size_)
{
    if (size_ == sizeof (socket_monitor_internal_event_t)
        && fail_next_message_init.exchange (false, std::memory_order_acq_rel)) {
        // The real initializer rejects the overflowing allocation and leaves
        // the message invalid, exactly as a failed malloc does.
        const zlink_config_result_t result =
          __real_zlink_msg_init_size (message_, SIZE_MAX);
        message_init_failure_errno.store (errno, std::memory_order_release);
        return result;
    }
    return __real_zlink_msg_init_size (message_, size_);
}
#endif

void setUp ()
{
    setup_test_context ();
    const char *previous = getenv (diagnostic_env);
    had_diagnostic_env = previous != NULL;
    saved_diagnostic_env = previous ? previous : "";
    diagnostics = tmpfile ();
    TEST_ASSERT_NOT_NULL (diagnostics);
    fflush (stderr);
#ifdef ZLINK_HAVE_WINDOWS
    saved_stderr = _dup (_fileno (stderr));
    TEST_ASSERT_TRUE (saved_stderr >= 0);
    saved_stderr_mode = _setmode (_fileno (stderr), _O_BINARY);
    TEST_ASSERT_EQUAL_INT (0, _dup2 (_fileno (diagnostics), _fileno (stderr)));
    TEST_ASSERT_TRUE (_setmode (_fileno (stderr), _O_BINARY) >= 0);
#else
    saved_stderr = dup (fileno (stderr));
    TEST_ASSERT_TRUE (saved_stderr >= 0);
    TEST_ASSERT_TRUE (dup2 (fileno (diagnostics), fileno (stderr)) >= 0);
#endif
    set_diagnostic_env ("1");
}

void tearDown ()
{
    zlink::test_set_pair_xsend_gate_hook (NULL, NULL);
#ifdef __linux__
    fail_next_message_init.store (false, std::memory_order_release);
#endif
    if (monitor)
        TEST_ASSERT_SUCCESS_ERRNO (zlink_monitor_close (&monitor));
    if (source) {
        test_context_socket_close_zero_linger (source);
        source = NULL;
    }
    teardown_test_context ();
    fflush (stderr);
#ifdef ZLINK_HAVE_WINDOWS
    _dup2 (saved_stderr, _fileno (stderr));
    _setmode (_fileno (stderr), saved_stderr_mode);
    _close (saved_stderr);
#else
    dup2 (saved_stderr, fileno (stderr));
    close (saved_stderr);
#endif
    fclose (diagnostics);
    diagnostics = NULL;
    set_diagnostic_env (had_diagnostic_env ? saved_diagnostic_env.c_str () : NULL);
}

void test_monitor_sleeps_when_empty_and_wakes_for_each_burst ()
{
    open_monitor ();
    emit_event (1);
    receive_event (1);
    control_barrier ();
    const uint64_t before = callback_count ();
#ifdef __linux__
    rusage usage_before;
    rusage usage_after;
    TEST_ASSERT_EQUAL_INT (0, getrusage (RUSAGE_SELF, &usage_before));
#endif
    void *watch = zlink_stopwatch_start ();
    msleep (idle_ms);
    const uint64_t after = callback_count ();
    const double elapsed_seconds = zlink_stopwatch_stop (watch) / 1000000.0;
#ifdef __linux__
    TEST_ASSERT_EQUAL_INT (0, getrusage (RUSAGE_SELF, &usage_after));
    const double process_cpu_seconds =
      cpu_seconds (usage_after) - cpu_seconds (usage_before);
    const long process_voluntary_switches =
      usage_after.ru_nvcsw - usage_before.ru_nvcsw;
    printf ("idle_ms=%d elapsed_seconds=%.6f source_callbacks=%llu source_callbacks_per_second=%.3f "
            "process_cpu_seconds=%.6f process_cpu_percent=%.4f "
            "process_voluntary_switches=%ld process_voluntary_switches_per_second=%.3f "
            "process_involuntary_switches=%ld\n",
            idle_ms, elapsed_seconds, static_cast<unsigned long long> (after - before),
            (after - before) / elapsed_seconds, process_cpu_seconds,
            process_cpu_seconds * 100.0 / elapsed_seconds,
            process_voluntary_switches,
            process_voluntary_switches / elapsed_seconds,
            usage_after.ru_nivcsw - usage_before.ru_nivcsw);
#else
    LIBZLINK_UNUSED (elapsed_seconds);
#endif
    TEST_ASSERT_EQUAL_UINT64_MESSAGE (
      before, after, "An empty monitor must not run periodic callbacks");
    for (int i = 2; i <= 5; ++i)
        emit_event (i);
    for (int i = 2; i <= 5; ++i)
        receive_event (i);
    TEST_ASSERT_TRUE (callback_count () > after);
}

void test_monitor_blocked_event_progresses_without_new_source_event ()
{
    open_monitor (1);
    emit_event (1);
    wait_for_worker_drain ();
    // The PAIR's one-record byte budget is occupied by the unread event.
    // The next record must retain worker ownership until credit is available.
    const uint64_t before = callback_count ();
    emit_event (2);
    TEST_ASSERT_TRUE (zlink_test_wait_until (test_wait_timeout_ms, [&] {
        return callback_count () > before;
    }));
    control_barrier ();
    TEST_ASSERT_EQUAL_UINT64 (
      socket_monitor_event_accounted_bytes (), worker_pending_bytes ());
    receive_event (1);
    receive_event (2);
    control_barrier ();
    const uint64_t drained = callback_count ();
    msleep (test_no_further_callbacks_ms);
    TEST_ASSERT_EQUAL_UINT64 (drained, callback_count ());
}

void test_monitor_source_close_keeps_published_records_ordered ()
{
    open_monitor ();
    for (int i = 1; i <= 4; ++i)
        emit_event (i);
    wait_for_worker_drain ();
    test_context_socket_close_zero_linger (source);
    source = NULL;
    for (int i = 1; i <= 4; ++i)
        receive_event (i);
    const uint64_t closed = callback_count ();
    msleep (test_no_further_callbacks_ms);
    TEST_ASSERT_EQUAL_UINT64 (closed, callback_count ());
}

void test_monitor_send_bad_alloc_keeps_record_and_rearms_delivery ()
{
    open_monitor ();
    fail_next_send.store (true, std::memory_order_release);
    zlink::test_set_pair_xsend_gate_hook (&fail_send_once, NULL);
    emit_event (1);
    TEST_ASSERT_TRUE (zlink_test_wait_until (test_wait_timeout_ms, [&] {
        return !fail_next_send.load (std::memory_order_acquire);
    }));
    receive_event (1);
    control_barrier ();
    TEST_ASSERT_EQUAL_UINT64 (0, worker_pending_bytes ());
}

#ifdef __linux__
void test_monitor_message_init_enomem_keeps_record_and_rearms_delivery ()
{
    open_monitor ();
    message_init_failure_errno.store (0, std::memory_order_relaxed);
    fail_next_message_init.store (true, std::memory_order_release);
    emit_event (1);
    receive_event (1);
    control_barrier ();
    TEST_ASSERT_EQUAL_INT (
      ENOMEM, message_init_failure_errno.load (std::memory_order_acquire));
    TEST_ASSERT_EQUAL_UINT64 (0, worker_pending_bytes ());
}
#endif

int main (int argc_, char **argv_)
{
    setup_test_environment ();
    UNITY_BEGIN ();
    if (argc_ == 2 && strcmp (argv_[1], "--idle-measure") == 0) {
        idle_ms = test_idle_measurement_ms;
        RUN_TEST (test_monitor_sleeps_when_empty_and_wakes_for_each_burst);
    } else {
        RUN_TEST (test_monitor_sleeps_when_empty_and_wakes_for_each_burst);
        RUN_TEST (test_monitor_blocked_event_progresses_without_new_source_event);
        RUN_TEST (test_monitor_source_close_keeps_published_records_ordered);
        RUN_TEST (test_monitor_send_bad_alloc_keeps_record_and_rearms_delivery);
#ifdef __linux__
        RUN_TEST (test_monitor_message_init_enomem_keeps_record_and_rearms_delivery);
#endif
    }
    return UNITY_END ();
}
