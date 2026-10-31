/* SPDX-License-Identifier: MPL-2.0 */

#include "testutil_unity.hpp"
#include "api/socket/socket_api_internal.hpp"
#include "core/control_runtime.hpp"
#include "core/ctx.hpp"
#include "sockets/pair/pair.hpp"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <new>
#include <thread>

void setUp ()
{
}
void tearDown ()
{
    zlink::test_set_pair_xsend_gate_hook (NULL, NULL);
}

namespace
{
const std::chrono::seconds observation_timeout (2);

struct control_barrier_t
{
    explicit control_barrier_t (zlink::control_runtime_t *runtime_) :
        runtime (runtime_), done (false), id (0)
    {
        id = runtime->add_periodic_task (arrive, this, 0, true);
    }
    static void arrive (void *arg_)
    {
        control_barrier_t *self = static_cast<control_barrier_t *> (arg_);
        std::lock_guard<std::mutex> lock (self->mutex);
        self->done = true;
        self->changed.notify_all ();
    }
    bool wait ()
    {
        bool observed;
        {
            std::unique_lock<std::mutex> lock (mutex);
            observed = changed.wait_for (lock, observation_timeout, [this] { return done; });
        }
        runtime->remove_task (id);
        return id != 0 && observed;
    }
    zlink::control_runtime_t *runtime;
    std::mutex mutex;
    std::condition_variable changed;
    bool done;
    uint64_t id;
};

struct send_gate_t
{
    explicit send_gate_t (bool throw_first_ = false) :
        calls (0), released (false), throw_first (throw_first_)
    {
        zlink::test_set_pair_xsend_gate_hook (on_send, this);
    }
    static void on_send (void *arg_)
    {
        send_gate_t *self = static_cast<send_gate_t *> (arg_);
        std::unique_lock<std::mutex> lock (self->mutex);
        ++self->calls;
        self->changed.notify_all ();
        if (self->calls != 1)
            return;
        self->changed.wait (lock, [self] { return self->released; });
        if (self->throw_first)
            throw std::bad_alloc ();
    }
    bool wait ()
    {
        std::unique_lock<std::mutex> lock (mutex);
        return changed.wait_for (lock, observation_timeout, [this] { return calls != 0; });
    }
    void release ()
    {
        std::lock_guard<std::mutex> lock (mutex);
        released = true;
        changed.notify_all ();
    }
    unsigned count ()
    {
        std::lock_guard<std::mutex> lock (mutex);
        return calls;
    }
    void uninstall () { zlink::test_set_pair_xsend_gate_hook (NULL, NULL); }
    std::mutex mutex;
    std::condition_variable changed;
    unsigned calls;
    bool released;
    bool throw_first;
};

struct monitor_fixture_t
{
    explicit monitor_fixture_t (uint64_t hwm_ = 0) :
        context (zlink_ctx_new ()),
        source (NULL),
        monitor (NULL),
        core (NULL),
        runtime (NULL),
        task_id (0),
        configured (false)
    {
        if (!context)
            return;
        source = zlink_socket (context, ZLINK_SOCKET_DEALER);
        if (!source)
            return;
        core = as_socket_handle (source).socket;
        const int zero = 0;
        if (zlink_set_option (source, ZLINK_OPT_LINGER, &zero, sizeof (zero)) != 0)
            return;
        zlink_socket_monitor_open_options_t options = {};
        options.events = ZLINK_EVENT_LISTENING;
        options.monitor_hwm_bytes = hwm_;
        monitor = zlink_socket_monitor_open (source, &options);
        if (!monitor)
            return;
        const int receive_timeout = 1000;
        if (zlink_set_option (monitor, ZLINK_OPT_RCVTIMEO, &receive_timeout,
                              sizeof (receive_timeout))
            != 0)
            return;
        runtime = static_cast<zlink::ctx_t *> (context)->control_runtime_if_started ();
        zlink::socket_monitor_runtime_t &worker = core->test_monitor_worker ();
        zlink::scoped_lock_t lock (worker.queue_sync);
        task_id = worker.task_id;
        configured = runtime != NULL && task_id != 0;
    }
    void emit (const char *address_)
    {
        core->event_listening (zlink::make_unconnected_bind_endpoint_pair (address_),
                               zlink::retired_fd);
    }
    bool receive (const char *address_)
    {
        zlink_monitor_event_t event = {};
        return zlink_socket_monitor_recv (monitor, &event, ZLINK_RECV_FLAGS_NONE) == ZLINK_RECV_OK
               && event.event == ZLINK_EVENT_LISTENING && strcmp (address_, event.local_addr) == 0;
    }
    bool barrier ()
    {
        control_barrier_t barrier (runtime);
        return barrier.wait ();
    }
    bool close ()
    {
        bool result = true;
        if (monitor)
            result = zlink_monitor_close (&monitor) == ZLINK_CLOSE_OK && result;
        if (source) {
            result = zlink_close (source) == ZLINK_CLOSE_OK && result;
            source = NULL;
        }
        if (context) {
            result = zlink_ctx_term (context) == ZLINK_CLOSE_OK && result;
            context = NULL;
        }
        return result;
    }
    void *context;
    void *source;
    void *monitor;
    zlink::socket_base_t *core;
    zlink::control_runtime_t *runtime;
    uint64_t task_id;
    bool configured;
};

bool wait_for_retirement (zlink::control_runtime_t *runtime_, uint64_t task_id_)
{
    const auto deadline = std::chrono::steady_clock::now () + observation_timeout;
    while (runtime_->test_task_exists (task_id_)) {
        if (std::chrono::steady_clock::now () >= deadline)
            return false;
        std::this_thread::yield ();
    }
    return true;
}
}

void test_monitor_idle_task_is_parked ()
{
    monitor_fixture_t fixture;
    TEST_ASSERT_TRUE (fixture.configured);
    const bool barrier = fixture.barrier ();
    const bool exists = fixture.runtime->test_task_exists (fixture.task_id);
    const bool scheduled = fixture.runtime->test_task_scheduled (fixture.task_id);
    const bool closed = fixture.close ();
    TEST_ASSERT_TRUE (barrier);
    TEST_ASSERT_TRUE (exists);
    TEST_ASSERT_FALSE_MESSAGE (scheduled, "idle monitor retained a periodic schedule");
    TEST_ASSERT_TRUE (closed);
}

void test_monitor_enqueue_wakes_and_delivers ()
{
    monitor_fixture_t fixture;
    TEST_ASSERT_TRUE (fixture.configured);
    fixture.emit ("inproc://wake-one");
    const bool received = fixture.receive ("inproc://wake-one");
    const bool barrier = fixture.barrier ();
    const bool scheduled = fixture.runtime->test_task_scheduled (fixture.task_id);
    const bool closed = fixture.close ();
    TEST_ASSERT_TRUE_MESSAGE (received, "admitted event did not wake the idle monitor task");
    TEST_ASSERT_TRUE (barrier);
    TEST_ASSERT_FALSE (scheduled);
    TEST_ASSERT_TRUE (closed);
}

void test_monitor_preserved_event_retries_without_new_enqueue ()
{
    monitor_fixture_t fixture (1);
    TEST_ASSERT_TRUE (fixture.configured);
    fixture.emit ("inproc://full-first");
    zlink_pollitem_t readable = {fixture.monitor, 0, ZLINK_POLLIN, 0};
    const bool first_readable =
      zlink_poll (&readable, 1, 1000, NULL) > 0 && (readable.revents & ZLINK_POLLIN) != 0;
    const bool first_delivered = fixture.barrier ();
    send_gate_t gate;
    fixture.emit ("inproc://full-second");
    const bool attempted = gate.wait ();
    // Queue this barrier before the blocked send can reserve its retry. Its
    // completion observes the failed attempt with the PAIR still full.
    control_barrier_t failed_send (fixture.runtime);
    gate.release ();
    const bool failed_attempt_finished = failed_send.wait ();
    size_t pending;
    {
        zlink::socket_monitor_runtime_t &worker = fixture.core->test_monitor_worker ();
        zlink::scoped_lock_t lock (worker.queue_sync);
        pending = worker.queue.size ();
    }
    const bool first_received = fixture.receive ("inproc://full-first");
    const bool second_received = fixture.receive ("inproc://full-second");
    const bool final_barrier = fixture.barrier ();
    gate.uninstall ();
    const bool closed = fixture.close ();
    TEST_ASSERT_TRUE (first_readable);
    TEST_ASSERT_TRUE (first_delivered);
    TEST_ASSERT_TRUE (attempted);
    TEST_ASSERT_TRUE (failed_attempt_finished);
    TEST_ASSERT_EQUAL_UINT (1, pending);
    TEST_ASSERT_TRUE (first_received);
    TEST_ASSERT_TRUE_MESSAGE (
      second_received, "preserved event stalled after consumer recovered without new enqueue");
    TEST_ASSERT_TRUE (final_barrier);
    TEST_ASSERT_TRUE (closed);
}

void test_monitor_callback_bad_alloc_preserves_event ()
{
    monitor_fixture_t fixture;
    TEST_ASSERT_TRUE (fixture.configured);
    send_gate_t gate (true);
    fixture.emit ("inproc://allocation-retry");
    const bool attempted = gate.wait ();
    gate.release ();
    const bool received = fixture.receive ("inproc://allocation-retry");
    const bool barrier = fixture.barrier ();
    const unsigned attempts = gate.count ();
    gate.uninstall ();
    const bool closed = fixture.close ();
    TEST_ASSERT_TRUE (attempted);
    TEST_ASSERT_TRUE_MESSAGE (received, "callback allocation failure stranded an admitted record");
    TEST_ASSERT_TRUE (barrier);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT (2, attempts);
    TEST_ASSERT_TRUE (closed);
}

void test_monitor_enqueue_during_callback_keeps_reschedule ()
{
    monitor_fixture_t fixture;
    TEST_ASSERT_TRUE (fixture.configured);
    send_gate_t gate;
    fixture.emit ("inproc://active-first");
    const bool attempted = gate.wait ();
    fixture.emit ("inproc://active-second");
    const bool rescheduled = fixture.runtime->test_task_scheduled (fixture.task_id);
    gate.release ();
    const bool first_received = fixture.receive ("inproc://active-first");
    const bool second_received = fixture.receive ("inproc://active-second");
    const bool barrier = fixture.barrier ();
    const bool parked = !fixture.runtime->test_task_scheduled (fixture.task_id);
    gate.uninstall ();
    const bool closed = fixture.close ();
    TEST_ASSERT_TRUE (attempted);
    TEST_ASSERT_TRUE_MESSAGE (rescheduled, "enqueue during callback lost its next execution");
    TEST_ASSERT_TRUE (first_received);
    TEST_ASSERT_TRUE (second_received);
    TEST_ASSERT_TRUE (barrier);
    TEST_ASSERT_TRUE (parked);
    TEST_ASSERT_TRUE (closed);
}

namespace
{
enum close_kind_t
{
    close_monitor,
    close_source,
    shutdown_context,
    terminate_context
};

void run_close_during_callback (close_kind_t kind_)
{
    monitor_fixture_t fixture;
    TEST_ASSERT_TRUE (fixture.configured);
    send_gate_t gate;
    fixture.emit ("inproc://close-active");
    const bool attempted = gate.wait ();
    fixture.emit ("inproc://close-pending");
    zlink_close_result_t result = ZLINK_CLOSE_INVALID_HANDLE;
    std::thread closer ([&] {
        if (kind_ == close_monitor)
            result = zlink_monitor_close (&fixture.monitor);
        else if (kind_ == close_source)
            result = zlink_close (fixture.source);
        else if (kind_ == shutdown_context)
            result = zlink_ctx_shutdown (fixture.context);
        else
            result = zlink_ctx_term (fixture.context);
    });
    const bool retired = wait_for_retirement (fixture.runtime, fixture.task_id);
    bool queue_lock_available = false;
    bool monitor_lock_available = false;
    bool retired_admission_rejected = false;
    if (retired && attempted) {
        zlink::socket_monitor_runtime_t &worker = fixture.core->test_monitor_worker ();
        queue_lock_available = worker.queue_sync.try_lock ();
        if (queue_lock_available)
            worker.queue_sync.unlock ();
        monitor_lock_available = worker.sync.try_lock ();
        if (monitor_lock_available)
            worker.sync.unlock ();
        if (queue_lock_available && monitor_lock_available) {
            size_t before;
            {
                zlink::scoped_lock_t lock (worker.queue_sync);
                before = worker.queue.size ();
            }
            zlink::socket_monitor_event_record_t late;
            worker.enqueue_worker_event (late, fixture.runtime);
            zlink::scoped_lock_t lock (worker.queue_sync);
            retired_admission_rejected =
              worker.queue_stop && worker.task_id == 0 && worker.queue.size () == before;
        }
    }
    gate.release ();
    if (kind_ == terminate_context) {
        // Term owns context destruction but still needs the caller to close
        // both public handles after it has stopped the monitor worker.
        if (fixture.monitor)
            zlink_monitor_close (&fixture.monitor);
        zlink_close (fixture.source);
        fixture.source = NULL;
    }
    closer.join ();
    gate.uninstall ();
    if (kind_ == close_source)
        fixture.source = NULL;
    if (kind_ == terminate_context)
        fixture.context = NULL;
    const bool closed = fixture.close ();
    TEST_ASSERT_TRUE (attempted);
    TEST_ASSERT_TRUE_MESSAGE (retired, "close did not retire the active monitor task");
    TEST_ASSERT_TRUE_MESSAGE (queue_lock_available, "callback join retained queue_sync");
    TEST_ASSERT_TRUE_MESSAGE (monitor_lock_available, "callback join retained monitor.sync");
    TEST_ASSERT_TRUE_MESSAGE (retired_admission_rejected,
                              "admission after retirement posted work to a retired task");
    TEST_ASSERT_EQUAL_INT (ZLINK_CLOSE_OK, result);
    TEST_ASSERT_TRUE (closed);
}
}

void test_monitor_close_joins_active_callback ()
{
    run_close_during_callback (close_monitor);
}
void test_monitor_source_close_joins_active_callback ()
{
    run_close_during_callback (close_source);
}
void test_monitor_context_shutdown_joins_active_callback ()
{
    run_close_during_callback (shutdown_context);
}
void test_monitor_context_term_joins_active_callback ()
{
    run_close_during_callback (terminate_context);
}

int main ()
{
    setup_test_environment ();
    const char *selected = getenv ("ZLINK_TEST_CASE");
    unsigned executed = 0;
#define RUN_MONITOR_CASE(FN)                                                                       \
    do {                                                                                           \
        if (!selected || strcmp (selected, #FN) == 0) {                                            \
            ++executed;                                                                            \
            RUN_TEST (FN);                                                                         \
        }                                                                                          \
    } while (false)
    UNITY_BEGIN ();
    RUN_MONITOR_CASE (test_monitor_idle_task_is_parked);
    RUN_MONITOR_CASE (test_monitor_enqueue_wakes_and_delivers);
    RUN_MONITOR_CASE (test_monitor_preserved_event_retries_without_new_enqueue);
    RUN_MONITOR_CASE (test_monitor_callback_bad_alloc_preserves_event);
    RUN_MONITOR_CASE (test_monitor_enqueue_during_callback_keeps_reschedule);
    RUN_MONITOR_CASE (test_monitor_close_joins_active_callback);
    RUN_MONITOR_CASE (test_monitor_source_close_joins_active_callback);
    RUN_MONITOR_CASE (test_monitor_context_shutdown_joins_active_callback);
    RUN_MONITOR_CASE (test_monitor_context_term_joins_active_callback);
    const int result = UNITY_END ();
    return executed != 0 ? result : 2;
}
