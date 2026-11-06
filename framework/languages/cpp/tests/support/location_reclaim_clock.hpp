/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */
#pragma once

#include "runtime/locations/location_repository.hpp"
#include <atomic>
#include <memory>

namespace zlink::framework
{
struct location_reclaim_test_access_t
{
    std::shared_ptr<std::atomic<std::int64_t>> elapsed =
      std::make_shared<std::atomic<std::int64_t>> (0);

    void bind (location_repository_t &repository, std::function<void ()> observed = {}) const
    {
        const std::lock_guard lock (repository._reclaim_mutex);
        repository._reclaim_now = [clock = elapsed, observed = std::move (observed)] {
            const auto now =
              std::chrono::steady_clock::time_point{} + std::chrono::milliseconds (clock->load ());
            if (observed)
                observed ();
            return now;
        };
    }

    void advance (std::chrono::milliseconds duration) const
    {
        elapsed->fetch_add (duration.count ());
    }

    static void restart_observer (location_repository_t &repository)
    {
        const std::lock_guard lock (repository._reclaim_mutex);
        repository._reclaim_observations.clear ();
    }

    static std::size_t observations (location_repository_t &repository)
    {
        const std::lock_guard lock (repository._reclaim_mutex);
        return repository._reclaim_observations.size ();
    }

    static bool observe (location_repository_t &repository,
                         const std::string &key,
                         const location_owner_token_t &owner,
                         bool live = false)
    {
        return repository.reclaim_wait_elapsed (key, owner, live);
    }

    static void missing (location_repository_t &repository, const std::string &key)
    {
        repository.retain_reclaim_observation (key);
    }
};
} // namespace zlink::framework
