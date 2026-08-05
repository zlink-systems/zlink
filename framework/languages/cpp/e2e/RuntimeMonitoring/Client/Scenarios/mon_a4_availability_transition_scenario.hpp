/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#pragma once

#include "../Support/client_support.hpp"
#include "mon_c1_dispatch_failure_scenario.hpp"

#include <cstdint>
#include <iostream>
#include <set>
#include <string>

namespace zlink::framework::e2e::runtime_monitoring::client
{

inline std::uint64_t mon_a4_generation (const std::string &line)
{
    const auto marker = line.find ("|generation=");
    if (marker == std::string::npos)
        return 0;
    return std::stoull (line.substr (marker + 12));
}

inline void run_mon_a4_availability_transition_scenario (
  const client_options_t &options)
{
    const auto evidence = wait_evidence_count_at_least (
      options.service_url,
      "identifier=zlink.runtime.mesh_node.peer_changed", 4,
      std::chrono::seconds (15));
    std::uint64_t previous_sequence = 0;
    std::set<std::uint64_t> generations;
    int svc_b_events = 0;
    for (const auto &line : evidence) {
        if (!contains (
              line,
              "identifier=zlink.runtime.mesh_node.peer_changed")
            || !contains (line, "peer-rid=svc-b"))
            continue;
        const auto sequence = evidence_sequence (line);
        ensure (sequence > previous_sequence,
                "MON-A4 peer events are not sequence ordered");
        previous_sequence = sequence;
        const auto generation = mon_a4_generation (line);
        if (generation != 0)
            generations.insert (generation);
        ++svc_b_events;
    }
    ensure (svc_b_events >= 4,
            "MON-A4 normal/crash replacement peer events are incomplete");
    ensure (generations.size () >= 2,
            "MON-A4 replacement did not expose a fresh lifecycle generation");

    const auto snapshot = runtime_snapshot (options.service_url);
    int svc_b_ready = 0;
    std::uint64_t ready_generation = 0;
    for (const auto &peer : snapshot.at ("peers")) {
        if (peer.at ("rid").get<std::string> () != "svc-b"
            || !peer.at ("ready").get<bool> ())
            continue;
        ++svc_b_ready;
        ready_generation = peer.at ("generation").get<std::uint64_t> ();
        ensure (!peer.at ("endpoint").get<std::string> ().empty (),
                "MON-A4 recovered peer endpoint is empty");
    }
    ensure (svc_b_ready == 1,
            "MON-A4 old svc-b lifetime remained ready after replacement");
    ensure (ready_generation == *generations.rbegin (),
            "MON-A4 snapshot did not retain the newest ready generation");

    bool channel_ready = false;
    for (const auto &channel : snapshot.at ("channels")) {
        if (channel.at ("name").get<std::string> () == route_mesh_channel) {
            channel_ready =
              channel.at ("selectable").get<bool> ()
              && channel.at ("readyMemberCount").get<std::uint64_t> () >= 2;
        }
    }
    ensure (channel_ready,
            "MON-A4 recovered channel readiness did not converge");
    std::cout << "scenario MON-A4 passed\n";
}

} // namespace zlink::framework::e2e::runtime_monitoring::client
