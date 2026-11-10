/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/dispatch/dispatch_limits.hpp"
#include "runtime/mesh/service_mailbox.hpp"
#include "runtime/execution/serial_execution_queue.hpp"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <future>
#include <latch>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>

namespace fw = zlink::framework;
namespace rt = fw::runtime;
namespace mesh = rt::mesh;
using namespace std::chrono_literals;

namespace zlink::framework::runtime
{
struct offload_executor_test_access_t
{
    static std::size_t items (offload_executor_t &executor)
    {
        std::lock_guard lock (executor._mutex);
        return executor._submitted_items;
    }
    static std::size_t entries (offload_executor_t &executor)
    {
        std::lock_guard lock (executor._mutex);
        return executor._queue.size ();
    }
    static bool stopping (offload_executor_t &executor)
    {
        std::lock_guard lock (executor._mutex);
        return executor._stopping;
    }
    static std::vector<std::condition_variable *> idle (offload_executor_t &executor)
    {
        std::lock_guard lock (executor._mutex);
        std::vector<std::condition_variable *> ready;
        for (const auto &worker : executor._workers)
            if (worker->phase == offload_executor_t::worker_phase_t::idle)
                ready.push_back (&worker->ready);
        return ready;
    }
};
}

namespace
{
std::atomic_bool count_wakes{false};
std::atomic_size_t wakes{0};
// Fixed-size executors keep these addresses stable until drain().
std::vector<std::condition_variable *> worker_conditions;
void require (bool condition, const char *message)
{
    if (!condition)
        throw std::runtime_error (message);
}
template <typename Predicate> void await (Predicate predicate)
{
    const auto deadline = std::chrono::steady_clock::now () + 5s;
    while (!predicate ()) {
        require (std::chrono::steady_clock::now () < deadline, "scheduler transition timed out");
        std::this_thread::yield ();
    }
}
template <typename T> void await (std::future<T> &future)
{
    require (future.wait_for (5s) == std::future_status::ready, "future timed out");
    future.get ();
}

struct source_t final : rt::offload_executor_t::drain_source_t
{
    using claim_t = mesh::service_mailbox_claim_t;
    explicit source_t (rt::offload_executor_t &executor,
                       mesh::service_mailbox_t &mailbox,
                       std::string owner = "node") :
        drain_source_t (&mailbox, std::move (owner)), executor (executor), mailbox (mailbox)
    {
    }
    std::size_t pending () const override { return mailbox.pending_application_records (owner); }
    record_t take () override
    {
        bool more;
        auto claim = mailbox.try_claim_application_record (owner, more, [&] (const auto &claimed) {
            taken.push_back (
              std::stoul (claimed->records.front ()->parts ().front ().to_string ()));
        });
        return {std::move (claim), terminal_serial ? false : more};
    }
    void run (record_t record) override
    {
        ++turns;
        auto claim = std::static_pointer_cast<claim_t> (std::move (record.value));
        const auto deadline = record.turn_started + rt::dispatch_limits::owner_time_budget;
        do {
            try {
                auto original = mailbox.take ({claim, 0});
                require (original.has_value (), "scheduled record lost its original");
                rt::handler_entry_t entry (original->application_permit
                                             ? std::move (*original->application_permit)
                                             : rt::application_job_queue_t::permit_t{});
                entry ();
                if (handle)
                    handle (std::stoul (original->parts ().front ().to_string ()));
                require (original->transferred (), "record did not settle");
                ++completed;
            }
            catch (...) {
                mailbox.release (*claim);
                throw;
            }
            claim.reset ();
            if (terminal_serial) {
                // The mutant publishes the next drain only after A's terminal.
                if (pending () != 0)
                    require (executor.try_submit_drain (shared_from_this ()),
                             "terminal-serial continuation was rejected");
                break;
            }
            if (std::chrono::steady_clock::now () >= deadline)
                break;
            if (one_item_per_record)
                break;
            auto next = executor.try_take_drain (shared_from_this ());
            claim = std::static_pointer_cast<claim_t> (std::move (next.value));
        } while (claim);
    }
    rt::offload_executor_t &executor;
    mesh::service_mailbox_t &mailbox;
    std::function<void (std::size_t)> handle;
    std::vector<std::size_t> taken; // written only under the mailbox mutex
    std::atomic_size_t completed{0}, turns{0};
    bool terminal_serial = false;
    bool one_item_per_record = false;
};

void enqueue (mesh::service_mailbox_t &mailbox,
              const std::string &owner,
              std::size_t id,
              const std::shared_ptr<rt::application_job_queue_t> &jobs = {})
{
    std::vector<zlink::message_t> parts;
    parts.push_back (zlink::message_t::from (std::to_string (id)));
    auto record = mesh::service_mailbox_record_t::from_parts (
      owner, mesh::service_mailbox_domain_t::application, std::move (parts));
    if (jobs) {
        auto permit = jobs->try_reserve_supply ();
        require (permit.has_value (), "test exhausted application permits");
        permit->mark_queued ();
        record.application_permit.emplace (std::move (*permit));
    }
    require (mailbox.try_enqueue (std::move (record)), "enqueue was rejected");
}

bool burst (std::string_view negative)
{
    constexpr std::size_t count = 1024, workers = 4;
    rt::offload_executor_t executor (workers);
    await ([&] { return rt::offload_executor_test_access_t::idle (executor).size () == workers; });
    worker_conditions = rt::offload_executor_test_access_t::idle (executor);
    mesh::service_mailbox_t mailbox;
    auto source = std::make_shared<source_t> (executor, mailbox);
    std::latch entered (workers);
    std::promise<void> release;
    auto released = release.get_future ().share ();
    source->one_item_per_record = negative == "items";
    source->handle = [&] (std::size_t id) {
        if (id < workers) {
            entered.count_down ();
            released.wait ();
        }
    };
    mailbox.begin_application_receive_turn ();
    for (std::size_t id = 0; id != count; ++id)
        enqueue (mailbox, "node", id);
    wakes = 0;
    count_wakes = true;
    if (negative == "items") {
        for (std::size_t id = 0; id != count; ++id)
            require (executor.try_submit_internal ([source] {
                auto record = source->take ();
                if (record.value)
                    source->run (std::move (record));
            }),
                     "negative item publication rejected");
    } else
        require (executor.try_submit_drain (source), "source publication rejected");
    // All four workers are held in handlers; subsequent publications must retain
    // one existing entry and cannot reserve these workers again.
    await ([&] { return entered.try_wait (); });
    for (std::size_t id = 1; id != count; ++id) {
        if (negative != "items")
            require (executor.try_submit_drain (source), "duplicate publication rejected");
        if (negative == "wakes")
            worker_conditions.front ()->notify_one ();
    }
    const auto queued = rt::offload_executor_test_access_t::entries (executor);
    release.set_value ();
    await ([&] { return source->completed.load () == count; });
    // Fence work capture destruction and the final persistent entry selection.
    await ([&] { return executor.drained (); });
    count_wakes = false;
    const auto items = rt::offload_executor_test_access_t::items (executor);
    const auto notifications = wakes.load ();
    executor.drain ();
#ifdef ZLINK_TEST_WRAP_NOTIFY_ONE
    require (notifications != 0, "worker wake instrumentation did not observe notifications");
#endif
    require (source->taken.size () == count, "take count differs");
    for (std::size_t id = 0; id != count; ++id)
        require (source->taken[id] == id, "record take order is not FIFO");
    std::fprintf (stderr,
                  "burst records=%zu items=%zu wakes=%zu queued=%zu turns=%zu bound=1/%zu\n", count,
                  items, notifications, queued, source->turns.load (), workers);
    return items == 1 && notifications <= workers && queued == 1;
}

bool steady_stream (bool notify_every_publication)
{
    constexpr std::size_t count = 1000, workers = 4;
    rt::offload_executor_t executor (workers);
    await ([&] { return rt::offload_executor_test_access_t::idle (executor).size () == workers; });
    worker_conditions = rt::offload_executor_test_access_t::idle (executor);
    mesh::service_mailbox_t mailbox;
    auto source = std::make_shared<source_t> (executor, mailbox);
    std::mutex participants_mutex;
    std::set<std::thread::id> participants;
    // Handlers hold until the last publication, so every publication after the
    // first W lands while all W workers run. Without the hold a descheduled
    // publisher lets the workers drain and go idle, and the next publication
    // then wakes an idle worker legitimately (timing, not the rule).
    std::promise<void> release;
    const auto released = release.get_future ().share ();
    source->handle = [&, released] (std::size_t) {
        {
            std::lock_guard lock (participants_mutex);
            participants.insert (std::this_thread::get_id ());
        }
        released.wait ();
    };
    mailbox.begin_application_receive_turn ();
    wakes = 0;
    count_wakes = true;
    for (std::size_t id = 0; id != count; ++id) {
        enqueue (mailbox, "node", id);
        require (executor.try_submit_drain (source), "stream publication rejected");
        if (notify_every_publication)
            worker_conditions[id % worker_conditions.size ()]->notify_one ();
        std::this_thread::sleep_for (50us);
    }
    release.set_value ();
    await ([&] { return source->completed.load () == count; });
    await ([&] { return executor.drained (); });
    count_wakes = false;
    const auto notifications = wakes.load ();
    const auto distinct_workers = participants.size ();
    executor.drain ();
    std::fprintf (stderr,
                  "steady_stream records=%zu worker_notifications=%zu notifications_per_record="
                  "%zu/%zu workers_participating=%zu workers_per_record=%zu/%zu bound=%zu\n",
                  count, notifications, notifications, count, distinct_workers, distinct_workers,
                  count, workers);
    return notifications <= workers && distinct_workers != 0 && distinct_workers <= workers;
}

bool independent (bool wait_for_terminal, const std::string &owner)
{
    rt::offload_executor_t executor (2);
    mesh::service_mailbox_t mailbox;
    auto source = std::make_shared<source_t> (executor, mailbox, owner);
    source->terminal_serial = wait_for_terminal;
    std::promise<void> first, second, release;
    auto a = first.get_future (), b = second.get_future ();
    auto released = release.get_future ().share ();
    std::thread::id a_thread, b_thread;
    source->handle = [&] (std::size_t id) {
        if (id == 0) {
            a_thread = std::this_thread::get_id ();
            first.set_value ();
            released.wait ();
        } else {
            b_thread = std::this_thread::get_id ();
            second.set_value ();
        }
    };
    mailbox.begin_application_receive_turn ();
    enqueue (mailbox, owner, 0);
    enqueue (mailbox, owner, 1);
    require (executor.try_submit_drain (source), "independent source rejected");
    await (a);
    const bool progressed = b.wait_for (5s) == std::future_status::ready;
    release.set_value ();
    executor.drain ();
    require (source->completed == 2, "independent dispatch lost a record");
    std::fprintf (stderr, "owner=%s B_before_A_terminal=%d different_worker=%d\n", owner.c_str (),
                  progressed, a_thread != b_thread);
    return progressed && a_thread != b_thread;
}

void shutdown (bool running)
{
    constexpr std::size_t count = 64;
    rt::offload_executor_t executor (1);
    mesh::service_mailbox_t mailbox;
    auto source = std::make_shared<source_t> (executor, mailbox);
    auto jobs =
      std::make_shared<rt::application_job_queue_t> (rt::application_job_queue_configuration_t{
        fw::application_job_queue_profile_t::balanced, std::uint32_t{count}, 1, count});
    std::promise<void> entered, release;
    auto started = entered.get_future ();
    auto released = release.get_future ().share ();
    if (!running) {
        require (executor.try_submit_internal ([&] {
            entered.set_value ();
            released.wait ();
        }),
                 "shutdown blocker rejected");
        await (started);
    } else
        source->handle = [&] (std::size_t id) {
            if (id == 0) {
                entered.set_value ();
                released.wait ();
            }
        };
    mailbox.begin_application_receive_turn ();
    for (std::size_t id = 0; id != count; ++id)
        enqueue (mailbox, "node", id, jobs);
    require (executor.try_submit_drain (source), "shutdown source rejected");
    if (running)
        await (started);
    auto draining = std::async (std::launch::async, [&] { executor.drain_application (); });
    await ([&] { return rt::offload_executor_test_access_t::stopping (executor); });
    const bool returned_early = draining.wait_for (0ms) == std::future_status::ready;
    const auto queued_permits = jobs->snapshot ().permits_in_use;
    release.set_value ();
    await (draining);
    executor.drain ();
    require (!returned_early, "shutdown returned before scheduled/running work");
    require (queued_permits == count - (running ? 1 : 0), "queued permits escaped shutdown");
    require (jobs->snapshot ().permits_in_use == 0, "shutdown retained permits");
    require (source->completed == count, "shutdown lost queued records");
    require (mailbox.pending_messages (mesh::service_mailbox_domain_t::application) == 0,
             "shutdown left mailbox records");
    std::fprintf (stderr, "shutdown running=%d records=%zu permits=0\n", running, count);
}

void last_take_publication_races ()
{
    for (const auto &owner : {std::string ("node"), std::string ("channel:test")}) {
        rt::offload_executor_t executor (2);
        mesh::service_mailbox_t mailbox;
        auto source = std::make_shared<source_t> (executor, mailbox, owner);
        mailbox.bind_application_dispatch ({}, [&] (const auto &) {
            require (executor.try_submit_drain (source), "racing source publication rejected");
        });
        for (std::size_t id = 0; id != 1000; ++id) {
            enqueue (mailbox, owner, id);
            if ((id & 1) == 0)
                await ([&] { return source->completed.load () == id + 1; });
        }
        await ([&] { return source->completed == 1000; });
        executor.drain ();
        require (source->taken.size () == 1000, "last-take race lost or duplicated records");
        for (std::size_t id = 0; id != 1000; ++id)
            require (source->taken[id] == id, "last-take race reordered takes");
    }
}

void preparation_failure_is_reported_outside_scheduler ()
{
    rt::offload_executor_t executor (1);
    struct failing_source_t final : rt::offload_executor_t::drain_source_t
    {
        explicit failing_source_t (rt::offload_executor_t &executor) :
            drain_source_t (&executor, "failing"), executor (executor)
        {
        }
        std::size_t pending () const override { return 1; }
        record_t take () override { throw std::runtime_error ("claim preparation"); }
        void run (record_t) override { throw std::logic_error ("unexpected run"); }
        void failed (std::exception_ptr error) override
        {
            require (error != nullptr, "preparation failure was lost");
            // Reentry would deadlock if failure delivery held the scheduler mutex.
            require (executor.try_submit_internal ([this] { delivered.set_value (); }),
                     "failure continuation was rejected");
        }
        rt::offload_executor_t &executor;
        std::promise<void> delivered;
    };
    auto source = std::make_shared<failing_source_t> (executor);
    auto delivered = source->delivered.get_future ();
    require (executor.try_submit_drain (source), "failing source was rejected");
    await (delivered);
    executor.drain ();
}

bool gated (bool actor, bool bypass)
{
    rt::offload_executor_t executor (2);
    rt::serial_execution_queue_t queue (executor, {}, {},
                                        actor ? rt::serial_lane_policy_t::actor_delivery ()
                                              : rt::serial_lane_policy_t::spot_wide ());
    std::promise<void> entered, second, release, sentinel;
    auto started = entered.get_future (), b = second.get_future (), fence = sentinel.get_future ();
    auto released = release.get_future ().share ();
    auto first = [&] {
        entered.set_value ();
        released.wait ();
    };
    auto next = [&] { second.set_value (); };
    require (queue.try_post ("A", first), "gate A rejected");
    await (started);
    if (bypass)
        require (executor.try_submit_internal (next), "negative gate B rejected");
    else
        require (queue.try_post ("B", next), "gate B rejected");
    // An independent worker must be available while the gated A runs.
    require (executor.try_submit_internal ([&] { sentinel.set_value (); }),
             "gate sentinel rejected");
    await (fence);
    // With A occupying one of two workers, B and the sentinel use the other
    // worker in FIFO order when the gate is bypassed. No absence timer is needed.
    const bool overlapped = b.wait_for (0ms) == std::future_status::ready;
    release.set_value ();
    await (b);
    queue.drain ();
    executor.drain ();
    std::fprintf (stderr, "%s_gate_overlap=%d\n", actor ? "actor" : "spot", overlapped);
    return !overlapped;
}
}

#ifdef ZLINK_TEST_WRAP_NOTIFY_ONE
extern "C" void __real__ZNSt18condition_variable10notify_oneEv (std::condition_variable *);
extern "C" void __wrap__ZNSt18condition_variable10notify_oneEv (std::condition_variable *condition)
{
    if (count_wakes.load ()
        && std::find (worker_conditions.begin (), worker_conditions.end (), condition)
             != worker_conditions.end ())
        ++wakes;
    __real__ZNSt18condition_variable10notify_oneEv (condition);
}
#endif

int main (int argc, char **argv)
{
    try {
        const std::string_view mode = argc > 1 ? argv[1] : "";
        if (mode == "--negative-items")
            return burst ("items") ? 0 : 1;
        if (mode == "--negative-wakes")
            return burst ("wakes") ? 0 : 1;
        if (mode == "--negative-stream-wakes")
            return steady_stream (true) ? 0 : 1;
        if (mode == "--negative-terminal")
            return independent (true, "node") ? 0 : 1;
        if (mode == "--negative-spot-gate")
            return gated (false, true) ? 0 : 1;
        if (mode == "--negative-actor-gate")
            return gated (true, true) ? 0 : 1;
        require (burst (""), "burst exceeded item/wake bound");
        require (steady_stream (false), "steady stream exceeded wake/worker bound");
        require (independent (false, "node"), "same Node waited for A terminal");
        require (independent (false, "channel:test"), "same Channel waited for A terminal");
        shutdown (false);
        shutdown (true);
        last_take_publication_races ();
        preparation_failure_is_reported_outside_scheduler ();
        require (gated (false, false), "Spot gate allowed overlapping handlers");
        require (gated (true, false), "Actor gate allowed overlapping handlers");
    }
    catch (const std::exception &error) {
        std::fprintf (stderr, "scheduler drain failed: %s\n", error.what ());
        return 2;
    }
}
