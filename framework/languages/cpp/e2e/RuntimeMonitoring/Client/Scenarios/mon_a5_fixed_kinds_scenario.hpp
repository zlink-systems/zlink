/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#pragma once

#include "../Support/client_support.hpp"

#include <iostream>

namespace zlink::framework::e2e::runtime_monitoring::client
{

inline void run_mon_a5_fixed_kinds_scenario (const client_options_t &options)
{
    const auto evidence = fetch_evidence (options.service_url);
    ensure (
      any_contains (
        evidence,
        "identifier=zlink.runtime.location.store_changed")
        && any_contains (evidence, "reason=degraded")
        && any_contains (evidence, "reason=ready"),
      "MON-A5 public location runtime transition evidence is incomplete");

    const auto snapshot = runtime_snapshot (options.service_url);
    ensure (snapshot.at ("location").at ("state").get<std::string> ()
              == "ready",
            "MON-A5 location runtime did not recover");
    ensure (
      snapshot.at ("location").at ("lastSuccessPresent").get<bool> ()
        && snapshot.at ("location").at ("lastFailurePresent").get<bool> (),
      "MON-A5 location success/failure timestamps are incomplete");
    int ready_peers = 0;
    for (const auto &peer : snapshot.at ("peers"))
        ready_peers += peer.at ("ready").get<bool> () ? 1 : 0;
    ensure (ready_peers >= 2,
            "MON-A5 admitted peers did not survive store-only failure");
    std::cout << "scenario MON-A5 passed\n";
}

} // namespace zlink::framework::e2e::runtime_monitoring::client
