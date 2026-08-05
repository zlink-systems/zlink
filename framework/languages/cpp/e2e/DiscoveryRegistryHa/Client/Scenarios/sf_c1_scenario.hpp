/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/scenario_context.hpp"

namespace
{

/* SF-C1: the client owns this scenario's public requests and assertions. */
inline void run_sf_c1_scenario (const options_t &options)
{
    sf_client::post_empty (options.provider_b_url, "/admin/crash");
    sf_client::wait_down (options.provider_b_url);
    sf_client::wait_peers (
      options.consumer_url, [] (const auto &peers) { return !sf_client::has_rid (peers, "api-b"); },
      stale_peer_timeout (options), "SF-C1 api-b row did not expire");
    std::this_thread::sleep_for (options.polling * 4);
    for (int i = 0; i < 8; ++i) {
        auto reply =
          sf_client::request_profile (options.consumer_url, "sf-c1-" + std::to_string (i));
        sf_client::ensure (reply.provider_rid == "api-a", "SF-C1 routed to dead provider");
    }
    std::cout << "scenario SF-C1 passed\n";
}

} // namespace
