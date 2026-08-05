/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "../Support/scenario_context.hpp"

namespace
{

/* SF-A2: the client owns this scenario's public requests and assertions. */
inline void run_sf_a2_scenario (const options_t &options)
{
    const auto status = sf_client::get_status (options.consumer_url);
    sf_client::ensure (!status.watch_enabled, "SF-A2 consumer unexpectedly reports watch enabled");
    sf_client::wait_peers (
      options.consumer_url,
      [] (const auto &peers) {
          return sf_client::has_rid (peers, "api-a") && sf_client::has_rid (peers, "api-b")
                 && !sf_client::has_rid (peers, "api-c");
      },
      options.polling * 8 + options.heartbeat, "SF-A2 initial providers were not ready");

    sf_client::ensure (!options.provider_c_start_file.empty (),
                       "SF-A2 provider start signal path is required");
    std::ofstream (options.provider_c_start_file) << "start\n";
    sf_client::wait_peers (
      options.consumer_url, [] (const auto &peers) { return sf_client::has_rid (peers, "api-c"); },
      options.polling * 8 + options.heartbeat,
      "SF-A2 added provider did not appear through polling");

    bool provider_c_served = false;
    const auto routing_deadline =
      std::chrono::steady_clock::now () + options.polling * 8 + options.heartbeat;
    for (int attempt = 0; std::chrono::steady_clock::now () < routing_deadline; ++attempt) {
        const auto reply = sf_client::request_profile (options.consumer_url,
                                                       "sf-a2-added-" + std::to_string (attempt));
        sf_client::ensure (reply.value == "profile:fast", "SF-A2 added provider request failed");
        if (reply.provider_rid == "api-c") {
            provider_c_served = true;
            break;
        }
    }
    sf_client::ensure (provider_c_served, "SF-A2 added provider never served traffic");

    sf_client::post_empty (options.provider_c_url, "/shutdown");
    sf_client::wait_down (options.provider_c_url);
    sf_client::wait_peers (
      options.consumer_url, [] (const auto &peers) { return !sf_client::has_rid (peers, "api-c"); },
      options.polling * 8 + options.heartbeat,
      "SF-A2 removed provider did not disappear through polling");
    for (int attempt = 0; attempt < 8; ++attempt) {
        const auto reply = sf_client::request_profile (options.consumer_url,
                                                       "sf-a2-removed-" + std::to_string (attempt));
        sf_client::ensure (reply.provider_rid != "api-c",
                           "SF-A2 removed provider still served traffic");
    }
    std::cout << "scenario SF-A2 passed\n";
}

} // namespace
