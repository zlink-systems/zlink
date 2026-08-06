/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#pragma once

#include "../Support/client_support.hpp"
#include "mon_c1_dispatch_failure_scenario.hpp"

#include <iostream>
#include <string>

namespace zlink::framework::e2e::runtime_monitoring::client
{

inline void run_mon_a4_availability_transition_scenario (
  const client_options_t &options)
{
    const auto evidence = wait_evidence_count_at_least (
      options.service_url,
      "identifier=zlink.runtime.mesh_node.peer_changed", 4,
      std::chrono::seconds (15));
    std::uint64_t previous_sequence = 0;
    int svc_b_events = 0;
    for (const auto &line : evidence) {
        if (!contains (
              line,
              "identifier=zlink.runtime.mesh_node.peer_changed")
            || !contains (line, "routing=svc-b"))
            continue;
        const auto sequence = evidence_sequence (line);
        ensure (sequence > previous_sequence,
                "MON-A4 peer events are not sequence ordered");
        previous_sequence = sequence;
        ++svc_b_events;
    }
    ensure (svc_b_events >= 4,
            "MON-A4 normal/crash replacement peer events are incomplete");
    const auto snapshot = runtime_snapshot (options.service_url);
    int svc_b_ready = 0;
    for (const auto &peer : snapshot.at ("peers")) {
        if (peer.at ("rid").get<std::string> () != "svc-b"
            || peer.at ("state").get<std::string> () != "ready")
            continue;
        ++svc_b_ready;
    }
    ensure (svc_b_ready == 1,
            "MON-A4 old svc-b lifetime remained ready after replacement");
    bool channel_ready = false;
    for (const auto &channel : snapshot.at ("channels")) {
        if (channel.at ("name").get<std::string> () == route_mesh_channel) {
            channel_ready =
              channel.at ("isReady").get<bool> ()
              && channel.at ("readyTargetCount").get<std::uint64_t> () >= 2;
        }
    }
    ensure (channel_ready,
            "MON-A4 recovered channel readiness did not converge");
    if (options.scenario == "mon-a4a")
        std::cout << "scenario MON-A4A passed\n";
    else if (options.scenario == "mon-a4b")
        std::cout << "scenario MON-A4B passed\n";
    else if (options.scenario == "all")
        std::cout << "scenario MON-A4A passed\nscenario MON-A4B passed\n";
    else
        std::cout << "scenario MON-A4 passed\n";
}

} // namespace zlink::framework::e2e::runtime_monitoring::client
