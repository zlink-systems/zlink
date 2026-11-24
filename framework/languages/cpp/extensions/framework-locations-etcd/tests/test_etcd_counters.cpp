/* SPDX-License-Identifier: Elastic-2.0 */
#include "fake_etcd.hpp"

#include <gtest/gtest.h>

#include <atomic>

namespace
{
using namespace zlink::framework;
using namespace zlink::framework::etcd;
using namespace etcd_test;
using namespace std::chrono_literals;

TEST (EtcdCounterTransaction, AddRereadsAfterRevisionComparisonFails)
{
    fake_etcd_t backend;
    backend.record ("capacity", "1");
    etcd_location_store_t store (backend.options ());

    std::atomic<int> applies{0};
    backend.txn_fault = [&] (auto *, const auto &request, auto *response) {
        if (applying (request) && applies++ == 0)
            backend.record ("capacity", "5");
        return backend.transact (request, response);
    };
    const auto result = store.write ({{}, {store_add_t{{"capacity"}, 1}}}).result ();
    ASSERT_TRUE (std::holds_alternative<store_write_applied_t> (result.value ()));
    EXPECT_EQ (applies.load (), 2);
    const auto payload = backend.get (key ("test", 'p', "capacity"));
    ASSERT_TRUE (payload);
    EXPECT_EQ (payload->value (), "6");
}

TEST (EtcdCounterTransaction, AddTreatsValueExpiringDuringCommitAsMissing)
{
    fake_etcd_t backend;
    backend.record ("capacity", "2", 1001);
    etcd_location_store_t store (backend.options ());

    std::atomic<int> applies{0};
    backend.txn_fault = [&] (auto *, const auto &request, auto *response) {
        if (applying (request) && applies++ == 0)
            backend.clock (1001);
        return backend.transact (request, response);
    };
    const auto result = store.write ({{}, {store_add_t{{"capacity"}, 1}}}).result ();
    const auto *applied = std::get_if<store_write_applied_t> (&result.value ());
    ASSERT_NE (applied, nullptr);
    EXPECT_EQ (applied->store_now, std::chrono::system_clock::time_point (1001ms));
    EXPECT_EQ (applies.load (), 2);
    const auto payload = backend.get (key ("test", 'p', "capacity"));
    ASSERT_TRUE (payload);
    EXPECT_EQ (payload->value (), "1");
}
} // namespace
