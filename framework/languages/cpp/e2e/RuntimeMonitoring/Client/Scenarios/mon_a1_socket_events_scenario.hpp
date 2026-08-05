/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#pragma once

#include "../Support/client_support.hpp"

#include <nlohmann/json.hpp>
#include <zlink/http_client.hpp>

#include <chrono>
#include <iostream>

namespace zlink::framework::e2e::runtime_monitoring::client
{

inline nlohmann::json runtime_snapshot (const std::string &base_url)
{
    auto http = zlink::http_client::client_t::create ()
                  .base_url (base_url)
                  .timeout (std::chrono::milliseconds (3000))
                  .build ();
    return http.get ("/runtime/snapshot")
      .submit<nlohmann::json> ()
      .result ()
      .value ()
      .body;
}

inline void run_mon_a1_socket_events_scenario (const client_options_t &options)
{
    auto http = zlink::http_client::client_t::create ()
                  .base_url (options.service_url)
                  .timeout (std::chrono::milliseconds (3000))
                  .build ();
    const auto observing = http.post ("/runtime/observe").submit_raw ().result ();
    ensure (observing && observing.value ().status < 400,
            "MON-A1 public runtime observer did not start");

    const auto first = runtime_snapshot (options.service_url);
    const auto second = runtime_snapshot (options.service_url);
    ensure (first.at ("meshName") == route_mesh_name,
            "MON-A1 MeshName mismatch");
    ensure (first.contains ("state") && first.contains ("isReady")
              && first.contains ("readyPeerCount")
              && first.contains ("peers") && first.contains ("channels")
              && first.contains ("placement"),
            "MON-A1 full snapshot fields missing");
    ensure (second.at ("sequence").get<std::uint64_t> ()
              > first.at ("sequence").get<std::uint64_t> (),
            "MON-A1 snapshot sequence did not advance");
    ensure (!second.at ("peers").empty (), "MON-A1 ready peer missing");
    ensure (!second.at ("channels").empty (), "MON-A1 channel snapshot missing");
    ensure (second.at ("channels").front ().at ("isReady").get<bool> (),
            "MON-A1 channel is not selectable");

    std::cout << "scenario MON-A1 passed\n";
}

} // namespace zlink::framework::e2e::runtime_monitoring::client
