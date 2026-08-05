/* SPDX-License-Identifier: Apache-2.0 */

#include "runtime/connection_pool.hpp"

namespace zlink::http_client::detail
{
namespace
{
//  Servers close idle keep-alive connections on their own schedule; an aged entry is very
//  likely already dead, and handing it out only converts into a stale-reuse retry (or a
//  hard failure for non-idempotent methods). Evict lazily on acquire instead.
constexpr std::chrono::seconds idle_ttl (30);
} // namespace

std::unique_ptr<pooled_connection_t> connection_pool_t::acquire (const std::string &key)
{
    const std::lock_guard<std::mutex> lock (_mutex);
    auto found = _idle.find (key);
    if (found == _idle.end ()) {
        return nullptr;
    }

    const auto now = std::chrono::steady_clock::now ();
    auto &idle = found->second;
    while (!idle.empty ()) {
        auto entry = std::move (idle.back ());
        idle.pop_back ();
        if (now - entry.released_at <= idle_ttl) {
            return std::move (entry.connection);
        }
        //  Expired: drop and keep looking at the next (older entries are further front).
    }
    return nullptr;
}

void connection_pool_t::release (const std::string &key,
                                 std::unique_ptr<pooled_connection_t> connection)
{
    static constexpr std::size_t max_idle_per_key = 4;
    const std::lock_guard<std::mutex> lock (_mutex);
    auto &idle = _idle[key];
    if (idle.size () < max_idle_per_key) {
        idle.push_back ({std::move (connection), std::chrono::steady_clock::now ()});
    }
}

} // namespace zlink::http_client::detail
