/* SPDX-License-Identifier: FSL-1.1-ALv2 */

#pragma once

#include "mon_a1_socket_events_scenario.hpp"

#include <zlink/http_client.hpp>

#include <chrono>
#include <iostream>
#include <thread>

namespace zlink::framework::e2e::runtime_monitoring::client
{

inline nlohmann::json channel_snapshot (const nlohmann::json &snapshot)
{
    for (const auto &channel : snapshot.at ("channels")) {
        if (channel.at ("name") == route_mesh_channel)
            return channel;
    }
    throw std::runtime_error ("RouteMesh channel snapshot is missing");
}

inline nlohmann::json wait_channel_weight (const std::string &base_url,
                                           int expected_weight)
{
    const auto deadline =
      std::chrono::steady_clock::now () + std::chrono::seconds (10);
    do {
        const auto channel = channel_snapshot (runtime_snapshot (base_url));
        const auto expected_ready = expected_weight > 0;
        if (channel.at ("isReady").get<bool> () == expected_ready
            && (expected_ready || channel.at ("readyTargetCount").get<std::uint64_t> () == 0))
            return channel;
        std::this_thread::sleep_for (std::chrono::milliseconds (50));
    } while (std::chrono::steady_clock::now () < deadline);
    throw std::runtime_error ("MON-A3 channel weight did not converge");
}

inline void run_mon_a3_spot_events_scenario (const client_options_t &options)
{
    auto filtered = zlink::http_client::client_t::create ()
                      .base_url (options.filtered_service_url)
                      .timeout (std::chrono::milliseconds (3000))
                      .build ();
    auto changed =
      filtered.post ("/admin/mesh-weight?weight=0").submit_raw ().result ();
    ensure (changed && changed.value ().status < 400,
            "MON-A3 weight 0 public runtime-options call failed");
    const auto zero = wait_channel_weight (options.filtered_service_url, 0);
    ensure (!zero.at ("isReady").get<bool> ()
              && zero.at ("readyTargetCount").get<std::uint64_t> () == 0,
            "MON-A3 local weight 0 readiness is inconsistent");

    changed =
      filtered.post ("/admin/mesh-weight?weight=100").submit_raw ().result ();
    ensure (changed && changed.value ().status < 400,
            "MON-A3 weight restore public runtime-options call failed");
    const auto restored =
      wait_channel_weight (options.filtered_service_url, 100);
    ensure (restored.at ("isReady").get<bool> (),
            "MON-A3 restored channel is not selectable");
    std::cout << "scenario MON-A3 passed\n";
}

} // namespace zlink::framework::e2e::runtime_monitoring::client
