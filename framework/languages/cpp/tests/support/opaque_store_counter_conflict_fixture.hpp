/* SPDX-License-Identifier: Elastic-2.0 */

#pragma once

#include "runtime/locations/in_memory_store_providers.hpp"
#include <functional>

namespace zlink::framework::test::opaque_store
{
using namespace runtime;

class counter_conflict_store_t final : public location_store_t
{
  public:
    task_t<store_read_result_t> read (store_key_t key) override
    {
        return inner.read (std::move (key));
    }
    task_t<store_scan_result_t> scan (store_scan_request_t request) override
    {
        return inner.scan (std::move (request));
    }
    task_t<store_write_result_t> write (store_write_request_t request) override
    {
        if (conflict_key) {
            // Awaited: the repository may resume on the inner store's lane.
            const auto key = std::exchange (conflict_key, std::nullopt).value ();
            const auto current = std::get<store_found_t> (co_await inner.read (key));
            store_write_request_t concurrent{{},
                                             {store_put_t{key, current.value.bytes, std::nullopt}}};
            (void) co_await inner.write (std::move (concurrent));
        }
        if (remaining_conflicts != 0) {
            --remaining_conflicts;
            ++rejected;
            if (on_conflict) {
                auto action = std::move (on_conflict);
                co_await action ();
            }
            co_return store_write_result_t{
              store_write_conflict_t{std::chrono::system_clock::now ()}};
        }
        co_return co_await inner.write (std::move (request));
    }

    in_memory_location_store_t inner;
    std::optional<store_key_t> conflict_key;
    std::size_t remaining_conflicts = 0;
    std::size_t rejected = 0;
    std::function<task_t<void> ()> on_conflict;
};
}
