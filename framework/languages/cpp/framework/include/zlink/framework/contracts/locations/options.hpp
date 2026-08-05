/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

namespace zlink::framework
{

struct location_options_t
{
    std::chrono::milliseconds owner_lease_renew_interval{5000};
    std::chrono::milliseconds owner_lease_ttl{15000};
    std::chrono::milliseconds polling_interval{1000};
    std::chrono::milliseconds store_failure_grace{30000};
    std::chrono::milliseconds owner_lease_fencing_margin{5000};
    std::chrono::milliseconds owner_lease_renew_timeout{3000};
    std::chrono::milliseconds route_cache_max_age{15000};
    std::chrono::milliseconds message_follow_duration{30000};
    std::size_t max_active_outbound_relocations = 64;
    std::size_t max_active_inbound_relocations = 64;
    std::size_t max_concurrent_relocation_captures = 8;
    std::size_t max_concurrent_relocation_restores = 8;
    std::uint64_t max_relocation_payload_in_flight_bytes = 268435456;

    // Internal paging bound. It is not part of the public configuration
    // contract, but remains here until store scans own their paging policy.
    int list_page_size = 1000;
    /* Spot mesh name to route mesh channel name, used when the two differ.
     * An unmapped mesh name is used as the route channel name as-is. */
    std::map<std::string, std::string> spot_router_channels;
};

} // namespace zlink::framework
