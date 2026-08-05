/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#pragma once

#include "mon_a1_socket_events_scenario.hpp"

#include <nlohmann/json.hpp>
#include <zlink/http_client.hpp>

#include <chrono>
#include <iostream>
#include <map>

namespace zlink::framework::e2e::runtime_monitoring::client
{

inline void run_mon_d1_failure_recovery_scenario (const client_options_t &options)
{
    auto http = zlink::http_client::client_t::create ()
                  .base_url (options.service_url)
                  .timeout (std::chrono::milliseconds (3000))
                  .build ();
    const auto validation = http.get ("/runtime/validation")
                              .submit<nlohmann::json> ()
                              .result ()
                              .value ()
                              .body;
    ensure (validation.at ("missingMeshRejected").get<bool> (),
            "MON-D1 missing MeshName was not rejected");
    ensure (validation.at ("missingObserverRejected").get<bool> (),
            "MON-D1 missing MeshName observer was not rejected");
    ensure (validation.at ("zeroCapacityRejected").get<bool> (),
            "MON-D1 zero observer capacity was not rejected");

    const auto events = wait_evidence_count_at_least (
      options.service_url,
      "identifier=zlink.runtime.mesh_node.peer_changed", 6,
      std::chrono::seconds (15));
    std::uint64_t previous = 0;
    int checked = 0;
    for (const auto &entry : events) {
        if (!contains (
              entry, "identifier=zlink.runtime.mesh_node.peer_changed"))
            continue;
        const auto marker = entry.find ("|sequence=");
        ensure (marker != std::string::npos,
                "MON-D1 event sequence missing");
        const auto sequence =
          std::stoull (entry.substr (marker + std::string ("|sequence=").size ()));
        ensure (sequence > previous,
                "MON-D1 event sequence is not strictly increasing");
        previous = sequence;
        ++checked;
    }
    ensure (checked >= 6, "MON-D1 repeated peer transition evidence missing");

    const auto snapshot = runtime_snapshot (options.service_url);
    ensure (!snapshot.at ("peers").empty (),
            "MON-D1 latest ready peer snapshot missing");
    std::map<std::string, int> ready_by_rid;
    for (const auto &peer : snapshot.at ("peers")) {
        const auto rid = peer.at ("rid").get<std::string> ();
        ready_by_rid.try_emplace (rid, 0);
        if (peer.at ("ready").get<bool> ())
            ++ready_by_rid[rid];
    }
    int ready_peers = 0;
    for (const auto &[rid, ready_count] : ready_by_rid) {
        ensure (ready_count <= 1,
                "MON-D1 latest ready generation is not unique for " + rid);
        ready_peers += ready_count;
    }
    ensure (ready_peers >= 2,
            "MON-D1 current ready peers did not recover");
    std::cout << "scenario MON-D1 passed\n";
}

} // namespace zlink::framework::e2e::runtime_monitoring::client
