/* SPDX-License-Identifier: Elastic-2.0 */

#pragma once

#include "runtime/locations/in_memory_store_providers.hpp"
#include "runtime/locations/location_records.hpp"
#include <array>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>

namespace zlink::framework::test::opaque_store
{
using namespace std::chrono_literals;
using namespace zlink::framework;
using namespace zlink::framework::runtime;

class startup_review_store_t final : public location_store_t
{
  public:
    task_t<store_read_result_t> read (store_key_t key) override
    {
        if (before_read)
            co_await before_read (key);
        if (unavailable && key.value.starts_with ("owner-lease"))
            co_return result_t<store_read_result_t>::failure (framework_error_kind_t::unavailable,
                                                              "startup owner read unavailable");
        co_return co_await inner.read (std::move (key));
    }
    task_t<store_write_result_t> write (store_write_request_t request) override
    {
        for (const auto &mutation : request.mutations) {
            const auto *put = std::get_if<store_put_t> (&mutation);
            if (!put)
                continue;
            if (unavailable && put->key.value.starts_with ("owner-lease"))
                co_return result_t<store_write_result_t>::failure (
                  framework_error_kind_t::unavailable, "startup owner write unavailable");
            if (hold_claims && put->key.value.starts_with ("mesh-node")) {
                unsigned index;
                {
                    const std::lock_guard lock (mutex);
                    index = held++;
                    changed.notify_all ();
                }
                if (index < releases.size ())
                    co_await releases[index].task ();
            }
        }
        auto result = co_await inner.write (std::move (request));
        if (std::holds_alternative<store_write_conflict_t> (result))
            ++conflicts;
        co_return result;
    }
    task_t<store_scan_result_t> scan (store_scan_request_t request) override
    {
        if (before_scan)
            co_await before_scan (request);
        co_return co_await inner.scan (std::move (request));
    }
    bool wait_held ()
    {
        std::unique_lock lock (mutex);
        return changed.wait_for (lock, 5s, [&] { return held >= 2; });
    }
    void release_all ()
    {
        for (auto &release : releases)
            release.complete (result_t<void>::success ());
    }
    in_memory_location_store_t inner;
    std::atomic_bool unavailable{false};
    bool hold_claims = false;
    std::array<task_completion_source_t<void>, 2> releases;
    std::atomic_uint conflicts{0};
    std::function<task_t<void> (const store_key_t &)> before_read;
    std::function<task_t<void> (const store_scan_request_t &)> before_scan;

  private:
    std::mutex mutex;
    std::condition_variable changed;
    unsigned held = 0;
};
}
