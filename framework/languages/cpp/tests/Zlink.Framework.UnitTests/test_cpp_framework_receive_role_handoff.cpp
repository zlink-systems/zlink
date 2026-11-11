/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include <gtest/gtest.h>
#include "same_owner_claim_controls.hpp"
#include "runtime/dispatch/offload_executor.hpp"
#include "runtime/mesh/service_mailbox.hpp"
#include "runtime/mesh/application_dispatch_terminal_owner.hpp"
#include "runtime/execution/serial_execution_queue.hpp"
#include "runtime/foundation/operation_registry.hpp"
#include <zlink/framework/contracts/channels/call.hpp>
#include "scope_exit.hpp"

#include <atomic>
#include <future>
#include <latch>
#include <array>
#include <algorithm>
#include <iostream>

#ifdef ZLINK_TEST_HANDOFF_ALLOC_FAIL
#include <pthread.h>
namespace
{
thread_local int allocation_failure_after = -1;
std::atomic<pthread_mutex_t *> observed_mutex{nullptr};
std::atomic_uint observed_attempts{0};
}
extern "C" int __real_pthread_mutex_trylock (pthread_mutex_t *);
extern "C" int __wrap_pthread_mutex_trylock (pthread_mutex_t *mutex)
{
    if (mutex == observed_mutex.load ())
        ++observed_attempts;
    return __real_pthread_mutex_trylock (mutex);
}
extern "C" void *real_allocate (std::size_t) __asm__ ("__real__Znwm");
extern "C" void *wrapped_allocate (std::size_t) __asm__ ("__wrap__Znwm");
extern "C" void *wrapped_allocate (std::size_t size)
{
    if (allocation_failure_after >= 0 && allocation_failure_after-- == 0) {
        allocation_failure_after = -1;
        throw std::bad_alloc ();
    }
    return real_allocate (size);
}
#endif

namespace rt = zlink::framework::runtime;
namespace mesh = rt::mesh;
using namespace std::chrono_literals;

namespace zlink::framework::runtime
{
struct offload_executor_test_access_t
{
    using worker_t = offload_executor_t::worker_t;
    static std::size_t idle (offload_executor_t &executor)
    {
        std::lock_guard lock (executor._mutex);
        return executor.idle_worker_count_locked ();
    }
    static bool receiving (offload_executor_t &executor)
    {
        std::lock_guard lock (executor._mutex);
        for (const auto &worker : executor._workers) {
            if (worker->thread.get_id () == std::this_thread::get_id ())
                return worker->assignment
                       && std::holds_alternative<std::shared_ptr<offload_executor_t::role_t>> (
                         worker->assignment->work);
        }
        return false;
    }
    static auto lock (offload_executor_t &executor) { return std::unique_lock (executor._mutex); }
    static std::size_t roles (offload_executor_t &executor)
    {
        std::lock_guard lock (executor._mutex);
        return executor._role_active;
    }
#ifdef ZLINK_TEST_HANDOFF_ALLOC_FAIL
    static void observe_attempts (offload_executor_t &executor)
    {
        observed_attempts = 0;
        observed_mutex = executor._mutex.native_handle ();
    }
#endif
    static bool stopping (offload_executor_t &executor)
    {
        std::lock_guard lock (executor._mutex);
        return executor._stopping;
    }
    // Slots without running threads let the test hold the successor before its
    // first instruction, and exercise reservation/stop/mutex boundaries exactly.
    struct placement_t
    {
        offload_executor_t executor{0, 1, 0ms};
        std::shared_ptr<worker_t> receiver = std::make_shared<worker_t> ();
        std::shared_ptr<worker_t> successor = std::make_shared<worker_t> ();
        explicit placement_t (std::shared_ptr<offload_executor_t::role_t> role)
        {
            receiver->assignment.emplace (
              offload_executor_t::work_item_t{std::move (role), std::nullopt});
            executor._active = executor._role_active = 1;
            executor.add_idle_locked (*successor);
        }
        ~placement_t ()
        {
            executor._idle_head = nullptr;
            executor._queue.clear ();
            receiver->assignment.reset ();
            successor->assignment.reset ();
            executor._active = executor._role_active = 0;
        }
        void queue (std::function<void ()> work = [] {})
        {
            executor._queue.push_back ({std::move (work), std::nullopt});
        }
        bool handoff () { return !!executor.try_handoff (*receiver); }
        void no_idle () { executor.remove_idle_locked (*successor); }
        void stop () { executor._stopping = true; }
        auto lock () { return std::unique_lock (executor._mutex); }
        std::size_t active () const { return executor._active; }
        std::size_t roles () const { return executor._role_active; }
        std::size_t queued () const { return executor._queue.size (); }
        bool role_preserved () const
        {
            return receiver->assignment
                   && std::holds_alternative<std::shared_ptr<offload_executor_t::role_t>> (
                     receiver->assignment->work);
        }
    };
};
}

namespace
{
using access_t = rt::offload_executor_test_access_t;
struct role_t final : rt::offload_executor_t::role_t
{
    std::function<bool ()> receive = [] { return false; };
    std::function<void ()> publication = [] {};
    bool run () override { return receive (); }
    bool has_publication = true;
    bool publish () override
    {
        publication ();
        return has_publication;
    }
};

template <typename Predicate> bool until (Predicate predicate)
{
    const auto end = std::chrono::steady_clock::now () + 5s;
    while (!predicate ()) {
        if (std::chrono::steady_clock::now () >= end)
            return false;
        std::this_thread::yield ();
    }
    return true;
}

TEST (ReceiveRoleHandoff, T01ExclusiveIdleAssignmentAndLocalQueueHead)
{
    rt::offload_executor_t executor (3);
    ASSERT_TRUE (until ([&] { return access_t::idle (executor) == 3; }));
    std::promise<void> entered, release;
    auto released = release.get_future ().share ();
    auto started = entered.get_future ();
    std::thread::id receiver, local;
    std::atomic_bool stop{false}, successor_started{false}, successor_was_receiver{false};
    unsigned rotations = 0;
    auto role = std::make_shared<role_t> ();
    role->receive = [&] {
        if (rotations++ == 0) {
            receiver = std::this_thread::get_id ();
            return true;
        }
        successor_was_receiver = access_t::receiving (executor);
        successor_started = std::this_thread::get_id () != receiver;
        while (!stop.load ())
            std::this_thread::yield ();
        return false;
    };
    role->publication = [&] {
        ASSERT_TRUE (executor.try_submit_internal ([&] {
            local = std::this_thread::get_id ();
            EXPECT_FALSE (access_t::receiving (executor));
            entered.set_value ();
            released.wait ();
        }));
    };
    ASSERT_TRUE (executor.start_role (role));
    const bool ran = started.wait_for (5s) == std::future_status::ready;
    const bool moved = until ([&] { return successor_started.load (); });
    EXPECT_TRUE (ran && moved && successor_was_receiver);
    EXPECT_EQ (receiver, local);
    EXPECT_EQ (3u, executor.live_worker_count ());
    EXPECT_EQ (3u, executor.owned_thread_count ());
    release.set_value ();
    stop = true;
    executor.drain ();
}

TEST (ReceiveRoleHandoff, T02NonblockingFailuresPreserveAssignments)
{
    auto role = std::make_shared<role_t> ();
    for (unsigned condition = 0; condition != 5; ++condition) {
        SCOPED_TRACE (condition);
        access_t::placement_t placement (role);
        if (condition != 4)
            placement.queue ();
        if (condition == 1 || condition == 2)
            placement.no_idle ();
        if (condition == 2)
            placement.successor->assignment.emplace ();
        if (condition == 3)
            placement.stop ();
        if (condition == 0) {
            auto lock = placement.lock ();
            auto attempt = std::async (std::launch::async, [&] { return placement.handoff (); });
            const auto returned_before_release = attempt.wait_for (100ms);
            lock.unlock ();
            EXPECT_EQ (std::future_status::ready, returned_before_release);
            EXPECT_FALSE (attempt.get ());
        } else {
            EXPECT_FALSE (placement.handoff ());
        }
        EXPECT_TRUE (placement.role_preserved ());
        EXPECT_EQ (1u, placement.roles ());
        EXPECT_EQ (1u, placement.active ());
        EXPECT_EQ (condition == 4 ? 0u : 1u, placement.queued ());
    }
}

TEST (ReceiveRoleHandoff, T02ReceiveSubmissionKeepsNormalDispatch)
{
    rt::offload_executor_t executor (2);
    ASSERT_TRUE (until ([&] { return access_t::idle (executor) == 2; }));
    std::promise<void> dispatched, finished;
    auto work = dispatched.get_future ();
    auto done = finished.get_future ();
    auto role = std::make_shared<role_t> ();
    role->receive = [&] {
        EXPECT_TRUE (access_t::receiving (executor));
        EXPECT_TRUE (executor.try_submit_internal ([&] {
            EXPECT_FALSE (access_t::receiving (executor));
            dispatched.set_value ();
        }));
        EXPECT_EQ (std::future_status::ready, work.wait_for (5s));
        finished.set_value ();
        return false;
    };
    ASSERT_TRUE (executor.start_role (role));
    EXPECT_EQ (std::future_status::ready, done.wait_for (5s));
    executor.drain ();
}

TEST (ReceiveRoleHandoff, T02EmptyRotationReturnsBeforeMutexRelease)
{
    rt::offload_executor_t executor (2);
    ASSERT_TRUE (until ([&] { return access_t::idle (executor) == 2; }));
    std::promise<void> publishing, release_publication, next_rotation;
    auto published = publishing.get_future ();
    auto release = release_publication.get_future ().share ();
    auto next = next_rotation.get_future ();
    bool first = true;
    auto role = std::make_shared<role_t> ();
    role->has_publication = false;
    role->receive = [&] {
        if (std::exchange (first, false))
            return true;
        next_rotation.set_value ();
        return false;
    };
    role->publication = [&] {
        publishing.set_value ();
        release.wait ();
    };
    ASSERT_TRUE (executor.start_role (role));
    ASSERT_EQ (std::future_status::ready, published.wait_for (5s));
    auto lock = access_t::lock (executor);
    release_publication.set_value ();
    const auto returned_before_release = next.wait_for (100ms);
    lock.unlock ();
    executor.drain ();
    EXPECT_EQ (std::future_status::ready, returned_before_release);
}

mesh::service_mailbox_record_t record (std::string owner, unsigned sequence);

TEST (ReceiveRoleHandoff, T03RotationHasOneReceiverAndOneAttempt)
{
    rt::offload_executor_t executor (3);
    ASSERT_TRUE (until ([&] { return access_t::idle (executor) == 3; }));
#ifdef ZLINK_TEST_HANDOFF_ALLOC_FAIL
    access_t::observe_attempts (executor);
    const zlink::framework::test::scope_exit_t reset_observer ([] { observed_mutex = nullptr; });
#endif
    mesh::service_mailbox_t mailbox;
    std::atomic_uint active{0}, maximum{0}, rotations{0}, publications{0}, started_records{0};
    unsigned handoffs = 0, failures = 0;
    std::thread::id previous_receiver;
    mesh::service_mailbox_t::application_ready_batch_t ready;
    mailbox.bind_application_dispatch ({}, [&] (const auto &owner) {
        ASSERT_TRUE (executor.try_submit_internal ([&, owner] {
            EXPECT_FALSE (access_t::receiving (executor));
            ASSERT_TRUE (mailbox.begin_application_drain (owner));
            auto claim = mailbox.try_claim_owner (mesh::service_mailbox_domain_t::application,
                                                  owner, 64, 65536);
            ASSERT_TRUE (claim);
            ASSERT_EQ (64u, claim->claimed_messages);
            for (unsigned i = 0; i != 64; ++i) {
                auto original = mailbox.take ({claim, i});
                ASSERT_TRUE (original);
                EXPECT_EQ (std::to_string (i), original->parts ().front ().to_string ());
                EXPECT_TRUE (original->transferred ());
                ++started_records;
            }
            mailbox.end_application_drain (owner);
        }));
    });
    auto role = std::make_shared<role_t> ();
    role->receive = [&] {
        const auto concurrent = ++active;
        maximum.store (std::max (maximum.load (), concurrent));
        EXPECT_TRUE (access_t::receiving (executor));
        EXPECT_EQ (1u, access_t::roles (executor));
        const auto current = std::this_thread::get_id ();
        if (previous_receiver != std::thread::id{}) {
            if (previous_receiver != current)
                ++handoffs;
            else
                ++failures;
        }
        previous_receiver = current;
        if (rotations == 33) {
            --active;
            return false;
        }
        const auto rotation = ++rotations;
        if (rotation == 17) {
            --active;
            return true; // An empty rotation makes no owner task.
        }
        mailbox.begin_application_receive_turn ();
        const auto owner = "node-" + std::to_string (rotation);
        for (unsigned i = 0; i != 64; ++i)
            EXPECT_TRUE (mailbox.try_enqueue (record (owner, i)));
        ready = mailbox.take_application_receive_turn ();
        --active;
        return true;
    };
    role->publication = [&] {
        ++publications;
        EXPECT_EQ (0u, active.load ());
        auto batch = std::move (ready);
        ready.reset ();
        mesh::service_mailbox_t::notify_application_ready (batch);
    };
    ASSERT_TRUE (executor.start_role (role));
    ASSERT_TRUE (until ([&] { return publications == 33; }));
    executor.drain ();
    EXPECT_EQ (1u, maximum.load ());
    EXPECT_EQ (rotations, publications);
    EXPECT_EQ (rotations, handoffs + failures);
#ifdef ZLINK_TEST_HANDOFF_ALLOC_FAIL
    EXPECT_EQ (rotations, observed_attempts.load ());
    EXPECT_EQ (observed_attempts.load (), handoffs + failures);
#endif
    EXPECT_EQ (0u, access_t::roles (executor));
    EXPECT_EQ (32u * 64, started_records.load ());
    EXPECT_EQ (0u, mailbox.pending_messages (mesh::service_mailbox_domain_t::application));
}

mesh::service_mailbox_record_t record (std::string owner, unsigned sequence)
{
    return mesh::service_mailbox_record_t::from_parts (
      std::move (owner), mesh::service_mailbox_domain_t::application,
      {zlink::message_t::from (std::to_string (sequence))});
}

void same_owner_negative_controls (const std::string &owner = "node")
{
    EXPECT_TRUE (zlink::framework::test::same_owner_claim_controls (owner));
}

using original_t = std::optional<mesh::service_mailbox_record_t>;
std::shared_ptr<mesh::service_mailbox_claim_t>
take_independent (mesh::service_mailbox_t &mailbox, const std::string &owner, original_t &original)
{
    return mailbox.try_claim_owner (mesh::service_mailbox_domain_t::application, owner, 64, 65536,
                                    [&] (const auto &claim) {
                                        original.emplace (std::move (*claim->records.front ()));
                                        claim->records.front ()->application.reset ();
                                    });
}

TEST (SameOwner, NodePreexistingBacklog)
{
    same_owner_negative_controls ("node");
}

TEST (SameOwner, ChannelPreexistingBacklog)
{
    same_owner_negative_controls ("channel:actor:raw-looking-name");
}

TEST (SameOwner, FifoTakeWithReverseEntry)
{
    for (const auto &owner : {"node", "channel:fifo"}) {
        mesh::service_mailbox_t mailbox;
        ASSERT_TRUE (mailbox.try_enqueue (record (owner, 0)));
        ASSERT_TRUE (mailbox.try_enqueue (record (owner, 1)));
        std::promise<void> taken, enter;
        auto did_take = taken.get_future ();
        auto may_enter = enter.get_future ();
        std::vector<unsigned> entries;
        std::mutex mutex;
        std::thread first ([&] {
            original_t a;
            auto claim = take_independent (mailbox, owner, a);
            EXPECT_TRUE (claim && a);
            if (a)
                EXPECT_EQ ("0", a->parts ().front ().to_string ());
            taken.set_value ();
            may_enter.wait ();
            std::lock_guard lock (mutex);
            entries.push_back (0);
            if (a)
                EXPECT_TRUE (a->transferred ());
        });
        const auto took = did_take.wait_for (5s);
        EXPECT_EQ (std::future_status::ready, took);
        original_t b;
        auto claim = take_independent (mailbox, owner, b);
        EXPECT_TRUE (claim && b);
        if (b) {
            EXPECT_EQ ("1", b->parts ().front ().to_string ());
            entries.push_back (1);
            EXPECT_TRUE (b->transferred ());
        }
        enter.set_value ();
        first.join ();
        EXPECT_EQ ((std::vector<unsigned>{1, 0}), entries);
    }
    same_owner_negative_controls ();
}

TEST (SameOwner, ReverseAndDuplicateSettlement)
{
    mesh::service_mailbox_t mailbox;
    auto a_record = record ("node", 0);
    a_record.correlation = 123;
    a_record.source_routing_id = {'a'};
    ASSERT_TRUE (mailbox.try_enqueue (std::move (a_record)));
    ASSERT_TRUE (mailbox.try_enqueue (record ("node", 1)));
    original_t a, b, c;
    auto a_claim = take_independent (mailbox, "node", a);
    auto b_claim = take_independent (mailbox, "node", b);
    ASSERT_TRUE (a_claim && b_claim && a && b);
    auto reply = a->reply_source ();
    EXPECT_TRUE (mailbox.release (*b_claim));
    EXPECT_FALSE (b->transferred ());
    EXPECT_FALSE (mailbox.release (*b_claim));
    EXPECT_EQ (1u, a_claim->outstanding ());
    EXPECT_EQ (123u, reply->correlation);
    EXPECT_EQ ((std::vector<std::uint8_t>{'a'}), reply->source_routing_id);
    ASSERT_TRUE (mailbox.try_enqueue (record ("node", 2)));
    auto c_claim = take_independent (mailbox, "node", c);
    ASSERT_TRUE (c_claim && c);
    EXPECT_TRUE (c->transferred ());
    EXPECT_TRUE (a->transferred ());
    EXPECT_FALSE (a->transferred ());
    EXPECT_FALSE (mailbox.release (*a_claim));
    EXPECT_EQ (0u, mailbox.pending_messages (mesh::service_mailbox_domain_t::application));
    same_owner_negative_controls ();
}

TEST (SameOwner, EnqueueAgainstLastSettlement)
{
    mesh::service_mailbox_t mailbox;
    std::atomic_uint ready{0};
    mailbox.bind_application_dispatch ({}, [&] (const auto &) { ++ready; });
    ASSERT_TRUE (mailbox.try_enqueue (record ("channel:race", 0)));
    ASSERT_TRUE (mailbox.begin_application_drain ("channel:race"));
    original_t a, b;
    auto claim = take_independent (mailbox, "channel:race", a);
    ASSERT_TRUE (claim && a);
    std::latch start (2);
    std::thread settlement ([&] {
        start.arrive_and_wait ();
        EXPECT_TRUE (a->transferred ());
        mailbox.end_application_drain ("channel:race");
    });
    start.arrive_and_wait ();
    EXPECT_TRUE (mailbox.try_enqueue (record ("channel:race", 1)));
    settlement.join ();
    EXPECT_EQ (2u, ready.load ());
    EXPECT_TRUE (mailbox.begin_application_drain ("channel:race"));
    auto next = take_independent (mailbox, "channel:race", b);
    ASSERT_TRUE (next && b);
    EXPECT_EQ ("1", b->parts ().front ().to_string ());
    EXPECT_TRUE (b->transferred ());
    mailbox.end_application_drain ("channel:race");
    same_owner_negative_controls ("channel:race");
}

TEST (SameOwner, CutAgainstEnqueueAndClaim)
{
    mesh::service_mailbox_t mailbox;
    for (unsigned i = 0; i != 3; ++i)
        ASSERT_TRUE (mailbox.try_enqueue (record ("node", i)));
    original_t a, b;
    auto first = take_independent (mailbox, "node", a);
    ASSERT_TRUE (first && a);
    std::latch start (3);
    std::shared_ptr<mesh::service_mailbox_claim_t> second;
    std::thread claimant ([&] {
        start.arrive_and_wait ();
        second = take_independent (mailbox, "node", b);
    });
    std::thread producer ([&] {
        start.arrive_and_wait ();
        EXPECT_TRUE (mailbox.try_enqueue (record ("node", 3)));
    });
    start.arrive_and_wait ();
    bool cut = false;
    auto originals = mailbox.cut_owners ({"node"}, {}, [&] { cut = true; });
    claimant.join ();
    producer.join ();
    ASSERT_TRUE (originals && cut);
    std::vector<unsigned> seen{0};
    if (b) {
        seen.push_back (std::stoul (b->parts ().front ().to_string ()));
        EXPECT_TRUE (b->transferred ());
    }
    for (auto &original : originals->front ()) {
        seen.push_back (std::stoul (original.parts ().front ().to_string ()));
        if (original.transferred)
            EXPECT_TRUE (original.transferred ());
    }
    for (;;) {
        original_t rest;
        auto next = take_independent (mailbox, "node", rest);
        if (!next)
            break;
        ASSERT_TRUE (rest);
        seen.push_back (std::stoul (rest->parts ().front ().to_string ()));
        EXPECT_TRUE (rest->transferred ());
    }
    std::sort (seen.begin (), seen.end ());
    EXPECT_EQ ((std::vector<unsigned>{0, 1, 2, 3}), seen);
    EXPECT_TRUE (a->transferred ());
    EXPECT_EQ (0u, mailbox.pending_messages (mesh::service_mailbox_domain_t::application));
    same_owner_negative_controls ();
}

TEST (SameOwner, RollbackAgainstEnqueue)
{
    for (const bool transfer : {false, true}) {
        SCOPED_TRACE (transfer);
        mesh::service_mailbox_t mailbox;
        ASSERT_TRUE (mailbox.try_enqueue (record ("node", 0)));
        original_t failed;
        std::latch locked (2);
        std::thread producer ([&] {
            locked.arrive_and_wait ();
            EXPECT_TRUE (mailbox.try_enqueue (record ("node", 1)));
        });
        EXPECT_THROW (
          mailbox.try_claim_owner (mesh::service_mailbox_domain_t::application, "node", 1, 65536,
                                   [&] (const auto &claim) {
                                       if (transfer) {
                                           failed.emplace (std::move (*claim->records.front ()));
                                           claim->records.front ()->application.reset ();
                                       }
                                       locked.arrive_and_wait ();
                                       throw std::bad_alloc ();
                                   }),
          std::bad_alloc);
        producer.join ();
        if (failed)
            EXPECT_FALSE (failed->transferred ());
        original_t b;
        auto next = take_independent (mailbox, "node", b);
        ASSERT_TRUE (next && b);
        EXPECT_EQ ("1", b->parts ().front ().to_string ());
        EXPECT_TRUE (b->transferred ());
        EXPECT_FALSE (mailbox.try_claim (mesh::service_mailbox_domain_t::application, 1, 65536));
    }
    same_owner_negative_controls ();
}

TEST (SameOwner, ShutdownAgainstEnqueueAndSettlement)
{
    mesh::service_mailbox_t mailbox;
    ASSERT_TRUE (mailbox.try_enqueue (record ("node", 0)));
    original_t a;
    auto claim = take_independent (mailbox, "node", a);
    ASSERT_TRUE (claim && a);
    std::latch start (3);
    bool accepted = false;
    std::thread producer ([&] {
        start.arrive_and_wait ();
        accepted = mailbox.try_enqueue (record ("node", 1));
    });
    std::thread settlement ([&] {
        start.arrive_and_wait ();
        EXPECT_TRUE (a->transferred ());
    });
    start.arrive_and_wait ();
    mailbox.close ();
    producer.join ();
    settlement.join ();
    EXPECT_FALSE (mailbox.try_enqueue (record ("node", 2)));
    original_t b;
    auto next = take_independent (mailbox, "node", b);
    EXPECT_EQ (accepted, static_cast<bool> (next));
    if (b)
        EXPECT_TRUE (b->transferred ());
    EXPECT_EQ (0u, mailbox.pending_messages (mesh::service_mailbox_domain_t::application));
    same_owner_negative_controls ();
}

TEST (SameOwner, ClaimAllocationFailureRepublishesUntakenOriginal)
{
#ifdef ZLINK_TEST_HANDOFF_ALLOC_FAIL
    for (int allocation = 0; allocation != 3; ++allocation) {
        SCOPED_TRACE (allocation);
        mesh::service_mailbox_t mailbox;
        unsigned publications = 0;
        mailbox.bind_application_dispatch ({}, [&] (const auto &) { ++publications; });
        mailbox.begin_application_receive_turn ();
        ASSERT_TRUE (mailbox.try_enqueue (record ("node", 0)));
        ASSERT_TRUE (mailbox.try_enqueue (record ("node", 1)));
        mailbox.end_application_receive_turn ();
        ASSERT_TRUE (mailbox.begin_application_drain ("node"));
        bool failed = false;
        original_t original;
        allocation_failure_after = allocation;
        try {
            (void) take_independent (mailbox, "node", original);
        }
        catch (const std::bad_alloc &) {
            failed = true;
        }
        allocation_failure_after = -1;
        EXPECT_TRUE (failed);
        EXPECT_FALSE (original);
        EXPECT_EQ (2u, mailbox.pending_messages (mesh::service_mailbox_domain_t::application));
        EXPECT_EQ (2u, publications);
        mailbox.end_application_drain ("node");
        ASSERT_TRUE (mailbox.begin_application_drain ("node"));
        for (unsigned sequence = 0; sequence != 2; ++sequence) {
            original_t next;
            auto claim = take_independent (mailbox, "node", next);
            ASSERT_TRUE (claim && next);
            EXPECT_EQ (std::to_string (sequence), next->parts ().front ().to_string ());
            EXPECT_TRUE (next->transferred ());
        }
        mailbox.end_application_drain ("node");
    }
    same_owner_negative_controls ();
#else
    GTEST_SKIP () << "allocation injection requires Linux/GNU linker wrapping";
#endif
}

TEST (SameOwner, RejectedReadyPublicationKeepsNextOriginal)
{
    mesh::service_mailbox_t mailbox;
    unsigned publications = 0;
    mailbox.bind_application_dispatch ({}, [&] (const auto &) {
        if (++publications == 2)
            throw std::bad_alloc ();
    });
    mailbox.begin_application_receive_turn ();
    ASSERT_TRUE (mailbox.try_enqueue (record ("node", 0)));
    ASSERT_TRUE (mailbox.try_enqueue (record ("node", 1)));
    mailbox.end_application_receive_turn ();
    ASSERT_TRUE (mailbox.begin_application_drain ("node"));
    original_t a, b;
    EXPECT_THROW (take_independent (mailbox, "node", a), std::bad_alloc);
    ASSERT_TRUE (a);
    EXPECT_FALSE (a->transferred ());
    EXPECT_EQ (1u, mailbox.pending_messages (mesh::service_mailbox_domain_t::application));
    mailbox.end_application_receive_turn ();
    EXPECT_EQ (3u, publications);
    ASSERT_TRUE (mailbox.begin_application_drain ("node"));
    auto next = take_independent (mailbox, "node", b);
    ASSERT_TRUE (next && b);
    EXPECT_EQ ("1", b->parts ().front ().to_string ());
    EXPECT_TRUE (b->transferred ());
    mailbox.end_application_drain ("node");
    same_owner_negative_controls ();
}

TEST (SameOwner, SerialOwnerBudgetsAndRetainedLifetime)
{
    for (const auto &owner : {"spot:serial", "actor:serial"}) {
        mesh::service_mailbox_t mailbox;
        for (unsigned i = 0; i != 4; ++i)
            ASSERT_TRUE (mailbox.try_enqueue (record (owner, i)));
        auto claim =
          mailbox.try_claim_owner (mesh::service_mailbox_domain_t::application, owner, 3, 65536);
        ASSERT_TRUE (claim);
        EXPECT_EQ (3u, claim->claimed_messages);
        EXPECT_FALSE (
          mailbox.try_claim_owner (mesh::service_mailbox_domain_t::application, owner, 1, 65536));
        std::array<original_t, 3> originals;
        for (unsigned i = 0; i != 3; ++i) {
            originals[i] = mailbox.take ({claim, i});
            ASSERT_TRUE (originals[i]);
            EXPECT_EQ (std::to_string (i), originals[i]->parts ().front ().to_string ());
        }
        EXPECT_TRUE (originals[2]->transferred ());
        EXPECT_TRUE (originals[0]->transferred ());
        EXPECT_FALSE (
          mailbox.try_claim_owner (mesh::service_mailbox_domain_t::application, owner, 1, 65536));
        EXPECT_TRUE (originals[1]->transferred ());
        auto last =
          mailbox.try_claim_owner (mesh::service_mailbox_domain_t::application, owner, 4, 1);
        ASSERT_TRUE (last);
        EXPECT_EQ (1u, last->claimed_messages); // First record may exceed the byte budget.
        EXPECT_TRUE (mailbox.release (*last));
    }
    mesh::service_mailbox_t infrastructure;
    for (unsigned i = 0; i != 2; ++i) {
        auto original = record ("node", i);
        original.domain = mesh::service_mailbox_domain_t::infrastructure;
        ASSERT_TRUE (infrastructure.try_enqueue (std::move (original)));
    }
    auto claim =
      infrastructure.try_claim (mesh::service_mailbox_domain_t::infrastructure, 2, 65536);
    ASSERT_TRUE (claim);
    EXPECT_EQ (2u, claim->claimed_messages);
    EXPECT_TRUE (infrastructure.release (*claim));
    same_owner_negative_controls ();
}

TEST (ReceiveRoleHandoff, T05OtherOwnerFifoAndExclusiveClaim)
{
    mesh::service_mailbox_t mailbox;
    mailbox.bind_application_dispatch ({}, [] (const auto &) {});
    for (unsigned i = 0; i != 64; ++i)
        ASSERT_TRUE (mailbox.try_enqueue (record ("spot:serial-control", i)));
    ASSERT_TRUE (mailbox.try_enqueue (record ("channel", 0)));
    ASSERT_TRUE (mailbox.begin_application_drain ("spot:serial-control"));
    const auto domain = mesh::service_mailbox_domain_t::application;
    auto a = mailbox.try_claim_owner (domain, "spot:serial-control", 64, 65536);
    ASSERT_TRUE (a);
    EXPECT_FALSE (mailbox.try_claim_owner (domain, "spot:serial-control", 64, 65536));
    EXPECT_FALSE (mailbox.begin_application_drain ("spot:serial-control"));
    EXPECT_TRUE (mailbox.begin_application_drain ("channel"));
    auto b = mailbox.try_claim_owner (domain, "channel", 1, 65536);
    ASSERT_TRUE (b);
    for (unsigned i = 0; i != 64; ++i) {
        auto original = mailbox.take ({a, i});
        ASSERT_TRUE (original);
        EXPECT_EQ (std::to_string (i), original->parts ().front ().to_string ());
        EXPECT_FALSE (mailbox.take ({a, i}));
        EXPECT_TRUE (original->transferred ());
        EXPECT_FALSE (original->transferred ());
    }
    auto independent = mailbox.take ({b, 0});
    ASSERT_TRUE (independent);
    EXPECT_TRUE (independent->transferred ());
    mailbox.end_application_drain ("channel");
    mailbox.end_application_drain ("spot:serial-control");
    EXPECT_EQ (0u, mailbox.pending_messages (domain));
}

TEST (ReceiveRoleHandoff, T05IndependentGatesDuringSynchronousHandler)
{
    for (const bool per_actor : {false, true}) {
        SCOPED_TRACE (per_actor);
        rt::offload_executor_t executor (4);
        ASSERT_TRUE (until ([&] { return access_t::idle (executor) == 4; }));
        const auto policy = per_actor ? rt::serial_lane_policy_t::per_actor_spot ()
                                      : rt::serial_lane_policy_t::spot_wide ();
        auto first_spot = std::make_shared<rt::serial_execution_queue_t> (
          executor, rt::serial_execution_queue_options_t{},
          rt::serial_execution_queue_t::error_handler_t{}, policy);
        auto second_spot = std::make_shared<rt::serial_execution_queue_t> (
          executor, rt::serial_execution_queue_options_t{},
          rt::serial_execution_queue_t::error_handler_t{}, policy);
        auto first_actor = std::make_shared<rt::serial_execution_queue_t> (executor);
        auto second_actor = std::make_shared<rt::serial_execution_queue_t> (executor);
        first_actor->attach_spot_gate (first_spot);
        second_actor->attach_spot_gate (second_spot);
        std::promise<void> entered, release, independent;
        auto started = entered.get_future ();
        auto released = release.get_future ().share ();
        auto progressed = independent.get_future ();
        std::atomic_uint same_gate{0}, other_gate{0};
        std::atomic_bool stop{false};
        bool first = true;
        auto role = std::make_shared<role_t> ();
        role->receive = [&] {
            if (std::exchange (first, false))
                return true;
            while (!stop.load ())
                std::this_thread::yield ();
            return false;
        };
        role->publication = [&] {
            first_actor->post ("blocked-handler", [&] {
                EXPECT_FALSE (access_t::receiving (executor));
                entered.set_value ();
                released.wait ();
            });
        };
        ASSERT_TRUE (executor.start_role (role));
        ASSERT_EQ (std::future_status::ready, started.wait_for (5s));
        first_actor->post ("same-gate", [&] { ++same_gate; });
        second_actor->post ("independent-gate", [&] {
            EXPECT_FALSE (access_t::receiving (executor));
            ++other_gate;
            independent.set_value ();
        });
        const auto independent_before_release = progressed.wait_for (5s);
        EXPECT_EQ (0u, same_gate.load ());
        release.set_value ();
        first_actor->drain ();
        second_actor->drain ();
        first_spot->drain ();
        second_spot->drain ();
        stop = true;
        executor.drain ();
        EXPECT_EQ (std::future_status::ready, independent_before_release);
        EXPECT_EQ (1u, same_gate.load ());
        EXPECT_EQ (1u, other_gate.load ());
    }
    same_owner_negative_controls ();
}

TEST (ReceiveRoleHandoff, T05SameNodeOwnerProgress)
{
    mesh::service_mailbox_t mailbox;
    ASSERT_TRUE (mailbox.try_enqueue (record ("node", 0)));
    original_t a, b;
    auto first = take_independent (mailbox, "node", a);
    ASSERT_TRUE (first && a);
    ASSERT_TRUE (mailbox.try_enqueue (record ("node", 1)));
    auto second = take_independent (mailbox, "node", b);
    ASSERT_TRUE (first && second && a && b);
    EXPECT_TRUE (b->transferred ());
    EXPECT_TRUE (a->transferred ());
    same_owner_negative_controls ();
}

TEST (ReceiveRoleHandoff, T06ReceiveAndPublicationReturnBeforePermitEntry)
{
    rt::offload_executor_t executor (3);
    ASSERT_TRUE (until ([&] { return access_t::idle (executor) == 3; }));
    auto jobs =
      std::make_shared<rt::application_job_queue_t> (rt::application_job_queue_configuration_t{
        zlink::framework::application_job_queue_profile_t::balanced, std::uint32_t{1}, 1, 1});
    auto permit = jobs->try_reserve_supply ();
    ASSERT_TRUE (permit);
    auto terminal = std::make_shared<rt::application_dispatch_terminal_owner_t> (
      nullptr, std::function<void ()>{}, rt::host::mailbox_callback_t{},
      rt::handler_entry_t (std::move (*permit)));
    std::atomic_uint receive_depth{0}, publication_depth{0}, entries{0};
    std::promise<void> entered;
    auto entry = entered.get_future ();
    bool first = true;
    auto role = std::make_shared<role_t> ();
    role->receive = [&] {
        if (!std::exchange (first, false))
            return false;
        ++receive_depth;
        zlink::framework::test::scope_exit_t leave ([&] { --receive_depth; });
        return true;
    };
    role->publication = [&] {
        ++publication_depth;
        zlink::framework::test::scope_exit_t leave ([&] { --publication_depth; });
        ASSERT_TRUE (executor.try_submit_internal ([&, terminal] {
            EXPECT_FALSE (access_t::receiving (executor));
            EXPECT_EQ (0u, receive_depth.load ());
            EXPECT_EQ (0u, publication_depth.load ());
            EXPECT_EQ (1u, jobs->snapshot ().permits_in_use);
            terminal->enter_handler ();
            ++entries;
            EXPECT_EQ (0u, jobs->snapshot ().permits_in_use);
            terminal->enter_handler ();
            entered.set_value ();
        }));
    };
    ASSERT_TRUE (executor.start_role (role));
    EXPECT_EQ (std::future_status::ready, entry.wait_for (5s));
    executor.drain ();
    EXPECT_EQ (1u, entries.load ());
    EXPECT_EQ (0u, jobs->snapshot ().permits_in_use);
}

TEST (ReceiveRoleHandoff, T07AwaitKeepsGateAndYieldReacquiresIt)
{
    namespace fw = zlink::framework;
    for (const bool per_actor : {false, true}) {
        SCOPED_TRACE (per_actor);
        rt::offload_executor_t executor (3);
        ASSERT_TRUE (until ([&] { return access_t::idle (executor) == 3; }));
        auto root = std::make_shared<rt::serial_execution_queue_t> (
          executor, rt::serial_execution_queue_options_t{},
          rt::serial_execution_queue_t::error_handler_t{},
          per_actor ? rt::serial_lane_policy_t::per_actor_spot ()
                    : rt::serial_lane_policy_t::spot_wide ());
        auto actor = std::make_shared<rt::serial_execution_queue_t> (executor);
        auto other = std::make_shared<rt::serial_execution_queue_t> (executor);
        actor->attach_spot_gate (root);
        other->attach_spot_gate (root);
        fw::task_completion_source_t<void> ordinary;
        fw::task_completion_source_t<int> reply;
        std::promise<void> suspended, yielding, finished;
        auto waiting = suspended.get_future ();
        auto yielded = yielding.get_future ();
        auto done = finished.get_future ();
        std::atomic_bool same_gate{false}, other_gate{false}, stop{false};
        std::optional<fw::task_t<void>> pending;
        const auto handler = [&] () -> fw::task_t<void> {
            auto original = fw::detail::capture_current_serial_turn ();
            EXPECT_TRUE (original);
            suspended.set_value ();
            co_await ordinary.task ();
            EXPECT_EQ (original, fw::detail::capture_current_serial_turn ());
            EXPECT_FALSE (same_gate.load ());
            fw::request_call_t<int> call ("handoff-yield", [&] (const auto &, auto, const auto &) {
                auto admitted = std::make_shared<fw::task_completion_source_t<void>> ();
                admitted->complete (fw::result_t<void>::success ());
                return fw::request_task_t<int> (reply.task (), std::move (admitted));
            });
            if (per_actor) {
                const auto rejected = call.yield ().result ();
                EXPECT_FALSE (rejected);
            } else {
                auto response = call.yield ();
                yielding.set_value ();
                EXPECT_EQ (17, co_await response);
                auto resumed = fw::detail::capture_current_serial_turn ();
                EXPECT_TRUE (resumed && !resumed->released ());
                EXPECT_NE (original, resumed);
            }
        };
        bool first = true;
        auto role = std::make_shared<role_t> ();
        role->receive = [&] {
            if (std::exchange (first, false))
                return true;
            while (!stop.load ())
                std::this_thread::yield ();
            return false;
        };
        role->publication = [&] {
            actor->post_async ("handler", [&] (auto complete) {
                EXPECT_FALSE (access_t::receiving (executor));
                pending.emplace (handler ());
                fw::detail::observe_task_terminal (*pending, [&, complete] (const auto &result) {
                    EXPECT_TRUE (result);
                    complete ([] {});
                    finished.set_value ();
                });
            });
        };
        ASSERT_TRUE (executor.start_role (role));
        ASSERT_EQ (std::future_status::ready, waiting.wait_for (5s));
        actor->post ("same-gate", [&] { same_gate = true; });
        other->post ("other-gate", [&] { other_gate = true; });
        // An independent executor item forms a scheduler barrier; both gate
        // submissions have been published before ordinary await is released.
        std::promise<void> barrier;
        auto observed = barrier.get_future ();
        ASSERT_TRUE (executor.try_submit_internal ([&] { barrier.set_value (); }));
        EXPECT_EQ (std::future_status::ready, observed.wait_for (5s));
        EXPECT_FALSE (same_gate.load ());
        if (per_actor)
            EXPECT_TRUE (until ([&] { return other_gate.load (); }));
        else
            EXPECT_FALSE (other_gate.load ());
        ordinary.complete (fw::result_t<void>::success ());
        if (!per_actor) {
            EXPECT_EQ (std::future_status::ready, yielded.wait_for (5s));
            EXPECT_TRUE (until ([&] { return other_gate.load (); }));
            // Yield releases the Spot gate, while the Actor claim stays held.
            EXPECT_FALSE (same_gate.load ());
            reply.complete (fw::result_t<int>::success (17));
        }
        EXPECT_EQ (std::future_status::ready, done.wait_for (5s));
        actor->drain ();
        other->drain ();
        root->drain ();
        EXPECT_TRUE (same_gate && other_gate);
        stop = true;
        executor.drain ();
    }
    same_owner_negative_controls ();
}

TEST (ReceiveRoleHandoff, T08IndependentCompletionWhileLocalCallbackBlocks)
{
    namespace fw = zlink::framework;
    fw::completion_owner_t owner;
    rt::foundation::operation_registry_t operations;
    rt::offload_executor_t executor (3);
    ASSERT_TRUE (until ([&] { return access_t::idle (executor) == 3; }));
    std::promise<void> entered, release, independent;
    auto entry = entered.get_future ();
    auto released = release.get_future ().share ();
    auto second = independent.get_future ();
    std::atomic_uint delivered{0};
    const rt::foundation::call_id_t a{11, 1}, b{11, 2};
    const auto deadline = rt::foundation::operation_registry_t::clock_t::time_point::max ();
    ASSERT_TRUE (operations.register_operation (a, deadline, [&] (auto, auto) {
        entered.set_value ();
        released.wait ();
        ++delivered;
    }));
    ASSERT_TRUE (operations.register_operation (b, deadline, [&] (auto, auto) {
        ++delivered;
        independent.set_value ();
    }));
    bool first = true;
    auto role = std::make_shared<role_t> ();
    role->receive = [&] { return std::exchange (first, false); };
    role->publication = [&] {
        ASSERT_TRUE (
          executor.try_submit_internal ([&] { EXPECT_TRUE (operations.complete (a, {})); }));
    };
    ASSERT_TRUE (executor.start_role (role));
    const auto a_entered = entry.wait_for (5s);
    EXPECT_EQ (std::future_status::ready, a_entered);
    ASSERT_TRUE (operations.complete (b, {}));
    const auto progressed = second.wait_for (5s);
    release.set_value ();
    executor.drain ();
    (void) operations.shutdown ();
    EXPECT_EQ (std::future_status::ready, progressed);
    EXPECT_EQ (2u, delivered.load ());
    EXPECT_FALSE (operations.complete (a, {}));
    EXPECT_FALSE (operations.complete (b, {}));
}

TEST (ReceiveRoleHandoff, T09ReservedRoleKeepsCreditUntilActualStop)
{
    auto role = std::make_shared<role_t> ();
    access_t::placement_t placement (role);
    placement.queue ();
    ASSERT_TRUE (placement.handoff ());
    EXPECT_EQ (2u, placement.active ());
    EXPECT_EQ (1u, placement.roles ());
    EXPECT_FALSE (placement.role_preserved ());
    EXPECT_TRUE (std::holds_alternative<std::shared_ptr<rt::offload_executor_t::role_t>> (
      placement.successor->assignment->work));
    placement.stop ();
    EXPECT_FALSE (placement.executor.drained ());
    // No successor thread runs, yet assignment already returned. No start ack.
    EXPECT_EQ (0u, placement.executor.live_worker_count ());
}

TEST (ReceiveRoleHandoff, T09StopDrainsCaptureAndSuspendedCredit)
{
    rt::offload_executor_t executor (3);
    ASSERT_TRUE (until ([&] { return access_t::idle (executor) == 3; }));
    std::promise<void> destroying, release;
    auto entered = destroying.get_future ();
    auto released = release.get_future ().share ();
    std::atomic_uint destroyed{0}, closed{0};
    std::atomic_bool stop{false};
    struct capture_t
    {
        std::promise<void> &entered;
        std::shared_future<void> release;
        std::atomic_uint &destroyed;
        ~capture_t ()
        {
            entered.set_value ();
            release.wait ();
            ++destroyed;
        }
    };
    auto capture = std::make_shared<capture_t> (destroying, released, destroyed);
    std::optional<rt::offload_executor_t::retained_work_t> suspended;
    bool first = true;
    auto role = std::make_shared<role_t> ();
    role->receive = [&] {
        if (std::exchange (first, false))
            return true;
        while (!stop.load ())
            std::this_thread::yield ();
        ++closed;
        return false;
    };
    role->publication = [&] {
        ASSERT_TRUE (executor.try_submit_internal ([&, held = std::move (capture)] {
            suspended.emplace (executor.retain_internal_work ());
        }));
    };
    ASSERT_TRUE (executor.start_role (role));
    EXPECT_EQ (std::future_status::ready, entered.wait_for (5s));
    auto draining = std::async (std::launch::async, [&] { executor.drain_application (); });
    EXPECT_NE (std::future_status::ready, draining.wait_for (100ms));
    release.set_value ();
    EXPECT_TRUE (until ([&] { return destroyed == 1; }));
    EXPECT_NE (std::future_status::ready, draining.wait_for (100ms));
    suspended.reset ();
    EXPECT_EQ (std::future_status::ready, draining.wait_for (5s));
    draining.get ();
    stop = true;
    executor.drain ();
    EXPECT_EQ (1u, closed.load ());
    EXPECT_EQ (1u, destroyed.load ());
}

TEST (ReceiveRoleHandoff, T09StopDuringPublicationStillDrainsQueue)
{
    rt::offload_executor_t executor (3);
    ASSERT_TRUE (until ([&] { return access_t::idle (executor) == 3; }));
    std::promise<void> publishing, release, application;
    auto published = publishing.get_future ();
    auto released = release.get_future ().share ();
    auto started = application.get_future ();
    std::atomic_bool stop{false};
    bool first = true;
    auto role = std::make_shared<role_t> ();
    role->receive = [&] {
        if (std::exchange (first, false))
            return true;
        while (!stop.load ())
            std::this_thread::yield ();
        return false;
    };
    role->publication = [&] {
        ASSERT_TRUE (executor.try_submit_internal ([&] {
            EXPECT_FALSE (access_t::receiving (executor));
            application.set_value ();
        }));
        publishing.set_value ();
        released.wait ();
    };
    ASSERT_TRUE (executor.start_role (role));
    ASSERT_EQ (std::future_status::ready, published.wait_for (5s));
    auto draining = std::async (std::launch::async, [&] { executor.drain_application (); });
    ASSERT_TRUE (until ([&] { return access_t::stopping (executor); }));
    const auto progressed = started.wait_for (5s);
    release.set_value ();
    // Keep a failing shutdown mutation observable without stranding the test
    // process: the original internal path can restart workers for queued work.
    if (progressed != std::future_status::ready)
        EXPECT_TRUE (executor.try_submit_internal ([] {}));
    EXPECT_EQ (std::future_status::ready, draining.wait_for (5s));
    draining.get ();
    stop = true;
    executor.drain ();
    EXPECT_EQ (std::future_status::ready, progressed);
}

TEST (ReceiveRoleHandoff, T09QueuedRolesDoNotDelayApplicationDrainOrReopenAdmission)
{
    rt::offload_executor_t executor (1);
    std::promise<void> entered;
    auto started = entered.get_future ();
    std::atomic_bool stop{false};
    std::atomic_uint closed{0}, applications{0};
    auto first = std::make_shared<role_t> ();
    first->receive = [&] {
        entered.set_value ();
        while (!stop.load ())
            std::this_thread::yield ();
        ++closed;
        return false;
    };
    auto queued = std::make_shared<role_t> ();
    queued->receive = [&] {
        ++closed;
        return false;
    };
    ASSERT_TRUE (executor.start_role (first));
    ASSERT_EQ (std::future_status::ready, started.wait_for (5s));
    ASSERT_TRUE (executor.start_role (queued));
    auto draining = std::async (std::launch::async, [&] { executor.drain_application (); });
    ASSERT_TRUE (until ([&] { return access_t::stopping (executor); }));
    const auto application_drained = draining.wait_for (100ms);
    const bool admitted = executor.try_submit_internal ([&] { ++applications; });
    stop = true;
    executor.drain ();
    draining.get ();
    EXPECT_EQ (std::future_status::ready, application_drained);
    EXPECT_FALSE (admitted);
    EXPECT_EQ (2u, closed.load ());
    EXPECT_EQ (0u, applications.load ());
}

TEST (ReceiveRoleHandoff, T10BatchPublicationIsPerOwnerAndKeepsQueueFifo)
{
    mesh::service_mailbox_t mailbox;
    unsigned published = 0;
    mailbox.bind_application_dispatch ({}, [&] (const auto &) { ++published; });
    mailbox.begin_application_receive_turn ();
    for (unsigned i = 0; i != 64; ++i)
        ASSERT_TRUE (mailbox.try_enqueue (record ("node", i)));
    EXPECT_EQ (0u, published);
    auto ready = mailbox.take_application_receive_turn ();
    ASSERT_TRUE (ready);
    ASSERT_EQ (1u, ready->first.size ());
    mesh::service_mailbox_t::notify_application_ready (ready);
    EXPECT_EQ (1u, published);
    auto role = std::make_shared<role_t> ();
    access_t::placement_t placement (role);
    std::vector<unsigned> order;
    placement.queue ([&] { order.push_back (0); });
    placement.queue ([&] { order.push_back (1); });
    ASSERT_TRUE (placement.handoff ());
    std::get<std::function<void ()>> (placement.receiver->assignment->work) ();
    EXPECT_EQ ((std::vector<unsigned>{0}), order);
    EXPECT_EQ (1u, placement.queued ());
}

TEST (ReceiveRoleHandoff, T11QueueAllocationFailurePreservesRecordAndNotification)
{
#ifdef ZLINK_TEST_HANDOFF_ALLOC_FAIL
    rt::offload_executor_t executor (1);
    std::promise<void> busy, release;
    auto entered = busy.get_future ();
    auto released = release.get_future ().share ();
    ASSERT_TRUE (executor.try_submit_internal ([&] {
        busy.set_value ();
        released.wait ();
    }));
    ASSERT_EQ (std::future_status::ready, entered.wait_for (5s));
    mesh::service_mailbox_t mailbox;
    std::atomic_uint calls{0};
    mailbox.bind_application_dispatch ({}, [&] (const auto &owner) {
        if (!executor.try_submit_internal ([&, owner] {
                ASSERT_TRUE (mailbox.begin_application_drain (owner));
                auto claim = mailbox.try_claim_owner (mesh::service_mailbox_domain_t::application,
                                                      owner, 1, 65536);
                ASSERT_TRUE (claim);
                auto original = mailbox.take ({claim, 0});
                ASSERT_TRUE (original);
                EXPECT_TRUE (original->transferred ());
                mailbox.end_application_drain (owner);
                ++calls;
            }))
            ADD_FAILURE () << "the active executor rejected admitted owner work";
    });
    const auto prepare = [&] (unsigned sequence) {
        const auto owner = "node-" + std::to_string (sequence);
        mailbox.begin_application_receive_turn ();
        EXPECT_TRUE (mailbox.try_enqueue (record (owner, sequence)));
        return mailbox.take_application_receive_turn ();
    };
    auto ready = prepare (0);
    allocation_failure_after = 0;
    EXPECT_THROW (mesh::service_mailbox_t::notify_application_ready (ready), std::bad_alloc);
    allocation_failure_after = -1;
    ASSERT_TRUE (ready);
    EXPECT_EQ ((std::list<std::string>{"node-0"}), ready->first);
    EXPECT_EQ (0u, calls.load ());
    mesh::service_mailbox_t::notify_application_ready (ready);
    // Callable construction allocates once. Find the next storage allocation
    // of the existing queue without relying on a library-specific block size.
    bool publication_failed = false;
    unsigned records = 1;
    for (; records != 256; ++records) {
        ready = prepare (records);
        allocation_failure_after = 1;
        try {
            mesh::service_mailbox_t::notify_application_ready (ready);
        }
        catch (const std::bad_alloc &) {
            publication_failed = true;
        }
        allocation_failure_after = -1;
        if (publication_failed) {
            ASSERT_TRUE (ready);
            EXPECT_EQ ((std::list<std::string>{"node-" + std::to_string (records)}), ready->first);
            ++records;
            break;
        }
    }
    EXPECT_TRUE (publication_failed);
    EXPECT_EQ (0u, calls.load ());
    EXPECT_EQ (records, mailbox.pending_messages (mesh::service_mailbox_domain_t::application));
    // The unaccepted notification still owns the same record. Publishing it
    // after the injected failure neither recreates a record nor a terminal.
    mesh::service_mailbox_t::notify_application_ready (ready);
    release.set_value ();
    executor.drain ();
    EXPECT_EQ (records, calls.load ());
    EXPECT_EQ (0u, mailbox.pending_messages (mesh::service_mailbox_domain_t::application));
#else
    GTEST_SKIP () << "allocation injection requires Linux/GNU linker wrapping";
#endif
}

TEST (ReceiveRoleHandoff, T11PublicationExceptionRetainsUnacceptedOwner)
{
    mesh::service_mailbox_t::application_ready_batch_t ready (std::in_place);
    ready->first = {"accepted", "unaccepted", "later"};
    std::vector<std::string> accepted;
    ready->second = [&] (const std::string &owner) {
        if (owner == "unaccepted")
            throw std::bad_alloc ();
        accepted.push_back (owner);
    };
    EXPECT_THROW (mesh::service_mailbox_t::notify_application_ready (ready), std::bad_alloc);
    EXPECT_EQ ((std::vector<std::string>{"accepted"}), accepted);
    EXPECT_EQ ((std::list<std::string>{"unaccepted", "later"}), ready->first);
}
} // namespace
