/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#pragma once

#include "mon_a1_socket_events_scenario.hpp"

#include <iostream>
#include <set>

namespace zlink::framework::e2e::runtime_monitoring::client
{

inline void run_mon_a2_location_events_scenario (const client_options_t &options)
{
    const auto snapshot = runtime_snapshot (options.service_url);
    ensure (!snapshot.at ("peers").empty (),
            "MON-A2 admitted peer snapshot missing");
    std::set<std::string> peer_ids;
    std::size_t ready_peers = 0;
    for (const auto &peer : snapshot.at ("peers")) {
        ensure (peer.contains ("rid") && peer.contains ("state")
                  && peer.contains ("unavailableReason"),
                "MON-A2 peer snapshot fields missing");
        if (peer.at ("state") == "ready")
            ++ready_peers;
        peer_ids.insert (peer.at ("rid").get<std::string> ());
    }
    ensure (peer_ids.size () == snapshot.at ("peers").size (),
            "MON-A2 duplicate peer identity remained in snapshot");
    ensure (ready_peers > 0, "MON-A2 current ready peer set is empty");
    std::cout << "scenario MON-A2 passed\n";
}

} // namespace zlink::framework::e2e::runtime_monitoring::client
