/* SPDX-License-Identifier: Elastic-2.0 */

#include "../support/actor_maintenance_join_host_fixture.hpp"
#include "../support/actor_maintenance_join_packets.hpp"
#include "runtime/locations/in_memory_store_providers.hpp"
#include <zlink/framework.hpp>
#include <zlink/framework/detail/test_creation_probe.hpp>
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>

namespace zlink::framework::test::actor_maintenance_join
{
namespace
{
enum class placement_t
{
    available,
    unavailable,
    held
};

class entry_selection_store_t final : public fw::location_store_t
{
  public:
    fw::task_t<fw::store_read_result_t> read (fw::store_key_t key) override
    {
        return _inner.read (std::move (key));
    }

    fw::task_t<fw::store_write_result_t> write (fw::store_write_request_t request) override
    {
        return _inner.write (std::move (request));
    }

    fw::task_t<fw::store_scan_result_t> scan (fw::store_scan_request_t request) override
    {
        using namespace std::string_view_literals;
        if (hold.load () && request.prefix.starts_with ("mesh-node\0"sv)) {
            selecting.complete (fw::result_t<void>::success ());
            co_await release.task ();
        }
        co_return co_await _inner.scan (std::move (request));
    }

    std::atomic<bool> hold{false};
    fw::task_completion_source_t<void> selecting;
    fw::task_completion_source_t<void> release;

  private:
    fw::runtime::in_memory_location_store_t _inner;
};

class DeferredEntryJoinCompletion : public testing::TestWithParam<placement_t>
{
};

TEST_P (DeferredEntryJoinCompletion, CompletesOnceAfterApplicationAccepts)
{
    const auto placement = GetParam ();
    const auto timeout = placement == placement_t::held ? 300ms : wait_budget;
    auto evidence = std::make_shared<observations_t> ();
    auto store = std::make_shared<entry_selection_store_t> ();
    host_t host;
    struct release_selection_t
    {
        entry_selection_store_t &store;
        ~release_selection_t () { store.release.complete (fw::result_t<void>::success ()); }
    } release_selection{*store};
    configure (host, evidence, first_rid, store,
               std::make_shared<fw::runtime::in_memory_relocation_store_t> ());
    host.start ();
    ASSERT_TRUE (ready (host));
    auto services = host.app.advanced ().services ().build_provider ();
    ASSERT_TRUE (services.get_required<fw::spot_manager_t> ()
                   .get_or_create (fw::spot_id_t (local_spot), room_type)
                   .async ()
                   .result ());
    ASSERT_TRUE (services.get_required<fw::actor_manager_t> ()
                   .get_or_create (fw::actor_id_t (actor_id), actor_type)
                   .async ()
                   .result ());
    auto &client = services.get_required<fw::actor_client_t> ();
    ASSERT_TRUE (move_to (client, *evidence, local_spot, 1));
    if (placement == placement_t::unavailable)
        ASSERT_TRUE (set_published_placement_weight (services, 0));
    store->hold = placement == placement_t::held;
#if defined(ZLINK_FRAMEWORK_TEST_CREATION_PROBE)
    auto &submissions = fw::detail::test_creation_probe::local_entry_join_submissions;
    const auto submissions_before = submissions.load (std::memory_order_relaxed);
#endif

    const auto accepted =
      client.request (fw::actor_id_t (actor_id), entry_join_t{static_cast<int> (timeout.count ())})
        .async<probe_t> ()
        .result_for (wait_budget);
    ASSERT_TRUE (accepted && *accepted);
    if (placement == placement_t::held) {
        ASSERT_TRUE (store->selecting.task ().result_for (wait_budget));
        std::this_thread::sleep_for (timeout);
        store->release.complete (fw::result_t<void>::success ());
    }
    const auto after = client.request (fw::actor_id_t (actor_id), probe_t{instance_probe})
                         .async<probe_t> ()
                         .result_for (wait_budget);
    ASSERT_TRUE (after && *after);
    EXPECT_EQ (after->value ().value, accepted->value ().value);

    std::lock_guard lock (evidence->mutex);
    EXPECT_EQ (evidence->entry_joined, placement == placement_t::available ? 1 : 0);
#if defined(ZLINK_FRAMEWORK_TEST_CREATION_PROBE)
    EXPECT_EQ (submissions.load (std::memory_order_relaxed) - submissions_before,
               placement == placement_t::available ? 1u : 0u);
#endif
    ASSERT_EQ (evidence->completions.size (), 2u);
    const auto &completion = evidence->completions.back ();
    if (placement == placement_t::available) {
        const auto *joined = std::get_if<fw::actor_join_accepted_t> (&completion);
        ASSERT_NE (joined, nullptr);
        EXPECT_EQ (joined->actor.actor_id ().value (), actor_id);
    } else {
        const auto *failed = std::get_if<fw::actor_join_failed_t> (&completion);
        ASSERT_NE (failed, nullptr);
        EXPECT_EQ (failed->error_kind, placement == placement_t::held
                                         ? fw::framework_error_kind_t::deadline_exceeded
                                         : fw::framework_error_kind_t::unavailable);
        EXPECT_TRUE (failed->operation_id_high != 0 || failed->operation_id_low != 0);
        EXPECT_EQ (evidence->completion_spots.back (), fw::spot_id_t (local_spot));
    }
}

INSTANTIATE_TEST_SUITE_P (EntrySelection,
                          DeferredEntryJoinCompletion,
                          testing::Values (placement_t::unavailable,
                                           placement_t::held,
                                           placement_t::available),
                          [] (const auto &info) {
                              switch (info.param) {
                                  case placement_t::unavailable:
                                      return "NoCompatibleTarget";
                                  case placement_t::held:
                                      return "SelectionExceedsDeadline";
                                  case placement_t::available:
                                      return "CompatibleTarget";
                              }
                              return "Unknown";
                          });
}
} // namespace zlink::framework::test::actor_maintenance_join
