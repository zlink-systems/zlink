/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#include "runtime/stateful/stateful_object_runtime.hpp"
#include "runtime/spots/actor_transfer_coordinator.hpp"
#include <gtest/gtest.h>
#include <atomic>
#include <cstdlib>
#include <new>
#include <string>

namespace
{
std::atomic<std::size_t> fail_size{0};
std::atomic<int> injected{0};
constexpr std::size_t inventory_count = 257;
}

void *operator new (std::size_t size)
{
    auto expected = fail_size.load ();
    if (expected != 0 && size >= expected && fail_size.compare_exchange_strong (expected, 0)) {
        ++injected;
        throw std::bad_alloc ();
    }
    if (auto *memory = std::malloc (size == 0 ? 1 : size))
        return memory;
    throw std::bad_alloc ();
}
void *operator new[] (std::size_t size)
{
    return ::operator new (size);
}
void operator delete (void *memory) noexcept
{
    std::free (memory);
}
void operator delete[] (void *memory) noexcept
{
    std::free (memory);
}
void operator delete (void *memory, std::size_t) noexcept
{
    std::free (memory);
}
void operator delete[] (void *memory, std::size_t) noexcept
{
    std::free (memory);
}

namespace zlink::framework
{
TEST (MaintenanceFailure, CompletionSnapshotFailureReleasesSourceSeal)
{
    detail::actor_transfer_coordinator_t transfers;
    for (std::size_t index = 0; index < inventory_count; ++index)
        ASSERT_TRUE (transfers.try_reserve_source (std::to_string (index)));
    injected = 0;
    fail_size = inventory_count * sizeof (std::string);
    const auto result = transfers.try_seal_maintenance_inventory ().result ();
    fail_size = 0;
    ASSERT_EQ (injected.load (), 1);
    ASSERT_FALSE (result);
    // state_lane maps std::exception to the existing InternalFailure contract.
    EXPECT_EQ (result.error_kind (), framework_error_kind_t::internal_failure);
    ASSERT_NE (result.error (), nullptr);
    EXPECT_STREQ (result.error ()->what (), std::bad_alloc ().what ());
    EXPECT_TRUE (transfers.try_reserve_source ("after-failure"));
    for (std::size_t index = 0; index < inventory_count; ++index)
        transfers.cancel_move (std::to_string (index));
    transfers.cancel_move ("after-failure");
}

TEST (MaintenanceFailure, NativeInventoryFailureReleasesSourceSeal)
{
    namespace st = runtime::stateful;
    st::stateful_object_runtime_t objects;
    for (std::size_t index = 0; index < inventory_count; ++index) {
        const st::object_ref_t object{
          st::object_kind_t::actor, std::to_string (index), 1, 1, "mesh", "source"};
        const auto created = objects.begin_reserved_object (object, "actor", {});
        ASSERT_EQ (created.error, st::stateful_error_t::none);
        ASSERT_EQ (objects.commit_create (created.attempt), st::stateful_error_t::none);
    }
    injected = 0;
    fail_size = inventory_count * sizeof (st::object_inventory_t);
    const auto result = objects.try_begin_maintenance_inventory ().result ();
    fail_size = 0;
    ASSERT_EQ (injected.load (), 1);
    ASSERT_FALSE (result);
    // state_lane maps std::exception to the existing InternalFailure contract.
    EXPECT_EQ (result.error_kind (), framework_error_kind_t::internal_failure);
    ASSERT_NE (result.error (), nullptr);
    EXPECT_STREQ (result.error ()->what (), std::bad_alloc ().what ());
    EXPECT_TRUE (objects.actor_transfers ()->try_reserve_source ("after-failure"));
    objects.actor_transfers ()->cancel_move ("after-failure");
    const auto next = objects.try_begin_maintenance_inventory ().result ();
    ASSERT_TRUE (next);
    ASSERT_TRUE (next.value ());
    EXPECT_EQ (next.value ()->size (), inventory_count);
    objects.end_maintenance_inventory ().result ().value ();
}
}
