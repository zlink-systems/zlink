/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include "client_support.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <future>
#include <iostream>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <vector>

namespace sf_client = zlink::framework::e2e::store_failure::client;
namespace sf = zlink::framework::e2e::store_failure;

namespace
{

struct options_t
{
    std::string scenario;
    std::string consumer_url;
    std::string provider_a_url;
    std::string provider_b_url;
    std::string provider_c_url;
    std::string provider_c_start_file;
    std::string redis_proxy_admin_url;
    std::string redis_container;
    std::string provider_a_endpoint;
    std::string provider_b_endpoint;
    std::string replacement_provider_url;
    std::string replacement_provider_endpoint;
    std::chrono::milliseconds heartbeat;
    std::chrono::milliseconds lease_ttl;
    std::chrono::milliseconds polling;
    std::chrono::milliseconds grace;
};

options_t read_options (int argc, char **argv)
{
    std::string path;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        constexpr std::string_view prefix = "--config=";
        if (argument.rfind (prefix, 0) != 0) {
            throw std::runtime_error ("unknown DiscoveryRegistryHa client option: " + argument);
        }
        path = argument.substr (prefix.size ());
    }
    if (path.empty ()) {
        throw std::runtime_error ("DiscoveryRegistryHa client requires --config=<path>");
    }
    std::ifstream input (path);
    if (!input) {
        throw std::runtime_error ("cannot open DiscoveryRegistryHa client config: " + path);
    }
    const auto root = nlohmann::json::parse (input).at ("e2e");
    const auto &location = root.at ("location");
    return {.scenario = root.at ("scenario").get<std::string> (),
            .consumer_url = root.at ("consumerUrl").get<std::string> (),
            .provider_a_url = root.at ("providerAUrl").get<std::string> (),
            .provider_b_url = root.at ("providerBUrl").get<std::string> (),
            .provider_c_url = root.at ("providerCUrl").get<std::string> (),
            .provider_c_start_file = root.at ("providerCStartFile").get<std::string> (),
            .redis_proxy_admin_url = root.at ("redisProxyAdminUrl").get<std::string> (),
            .redis_container = root.at ("redisContainer").get<std::string> (),
            .provider_a_endpoint = root.at ("providerAEndpoint").get<std::string> (),
            .provider_b_endpoint = root.at ("providerBEndpoint").get<std::string> (),
            .replacement_provider_url = root.at ("replacementProviderUrl").get<std::string> (),
            .replacement_provider_endpoint =
              root.at ("replacementProviderEndpoint").get<std::string> (),
            .heartbeat = std::chrono::milliseconds (location.at ("heartbeatMs").get<int> ()),
            .lease_ttl = std::chrono::milliseconds (location.at ("leaseTtlMs").get<int> ()),
            .polling = std::chrono::milliseconds (location.at ("pollingMs").get<int> ()),
            .grace = std::chrono::milliseconds (location.at ("graceMs").get<int> ())};
}

std::chrono::milliseconds stale_peer_timeout (const options_t &options)
{
    return options.lease_ttl * 6 + options.polling * 12 + options.heartbeat * 4;
}

void warm_provider_connections (const options_t &options, const std::string &scenario)
{
    std::set<std::string> served;
    for (int index = 0; index < 20 && served.size () < 2; ++index) {
        const auto reply = sf_client::request_profile (
          options.consumer_url,
          sf_client::unique_marker (scenario + "-warm-" + std::to_string (index)));
        served.insert (reply.provider_rid);
    }
    sf_client::ensure (served.contains ("api-a") && served.contains ("api-b"),
                       scenario + " did not warm both provider connections");
}

double percentile_ms (std::vector<double> values, double percentile)
{
    std::sort (values.begin (), values.end ());
    const auto index = static_cast<std::size_t> (
      std::max (0.0, std::ceil (percentile * static_cast<double> (values.size ())) - 1.0));
    return values[std::min (index, values.size () - 1)];
}

double measure_peer_query_ms (const options_t &options)
{
    const auto started = std::chrono::steady_clock::now ();
    auto peers = sf_client::get_json<std::vector<sf::peer_row_res_t>> (
      options.consumer_url, "/query/peers", std::chrono::seconds (15));
    const auto elapsed = std::chrono::steady_clock::now () - started;
    sf_client::ensure (sf_client::has_rid (peers, "api-a") && sf_client::has_rid (peers, "api-b"),
                       "SF-E1 delayed peer query did not return both providers");
    return std::chrono::duration<double, std::milli> (elapsed).count ();
}

struct tolerant_traffic_t
{
    std::vector<sf::profile_res_t> replies;
    std::chrono::milliseconds max_success_gap{0};
};

tolerant_traffic_t drive_tolerant_requests (const options_t &options,
                                            std::chrono::milliseconds window)
{
    tolerant_traffic_t result;
    const auto deadline = std::chrono::steady_clock::now () + window;
    auto last_success = std::chrono::steady_clock::now ();
    for (int index = 0; std::chrono::steady_clock::now () < deadline; ++index) {
        try {
            auto reply = sf_client::request_profile (
              options.consumer_url, sf_client::unique_marker ("sf-d2-" + std::to_string (index)));
            const auto now = std::chrono::steady_clock::now ();
            result.max_success_gap =
              std::max (result.max_success_gap,
                        std::chrono::duration_cast<std::chrono::milliseconds> (now - last_success));
            last_success = now;
            result.replies.push_back (std::move (reply));
        }
        catch (...) {
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (150));
    }
    result.max_success_gap =
      std::max (result.max_success_gap, std::chrono::duration_cast<std::chrono::milliseconds> (
                                          std::chrono::steady_clock::now () - last_success));
    sf_client::ensure (!result.replies.empty (), "SF-D2 produced no successful traffic");
    sf_client::ensure (result.max_success_gap < options.lease_ttl * 2,
                       "SF-D2 max_success_gap exceeded dead-transport tolerance");
    return result;
}

std::vector<double>
measure_requests (const std::string &consumer_url, const std::string &marker_prefix, int count)
{
    std::vector<double> timings;
    timings.reserve (static_cast<std::size_t> (count));
    for (int i = 0; i < count; ++i) {
        const auto started = std::chrono::steady_clock::now ();
        auto reply = sf_client::request_profile (
          consumer_url, marker_prefix + "-" + std::to_string (i), std::chrono::seconds (3));
        const auto elapsed = std::chrono::steady_clock::now () - started;
        sf_client::ensure (reply.value == "profile:fast",
                           "SF-E1 request returned unexpected value");
        timings.push_back (std::chrono::duration<double, std::milli> (elapsed).count ());
    }
    return timings;
}

} // namespace
