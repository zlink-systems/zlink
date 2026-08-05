/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <zlink/framework.hpp>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace zlink::framework::e2e::registry_messaging::provider
{

inline std::vector<std::string> split_csv (const std::string &text)
{
    std::vector<std::string> result;
    std::stringstream input (text);
    std::string item;
    while (std::getline (input, item, ',')) {
        if (!item.empty ()) {
            result.push_back (item);
        }
    }
    return result;
}

inline std::optional<int> parse_optional_int (const configuration_section_t &section,
                                             const char *name)
{
    const auto value = section.get (name);
    if (!value || value->empty ()) {
        return std::nullopt;
    }
    return std::stoi (*value);
}

struct provider_options_t
{
    std::string rid;
    std::string instance_id;
    std::string api_endpoint;
    std::string route_endpoint;
    std::string http_endpoint;
    std::string redis_endpoint;
    std::string redis_key_prefix;
    std::vector<std::string> route_peers;
    std::string log_dir;
    std::optional<int> server_weight;
    static provider_options_t bind (const configuration_section_t &section)
    {
        const auto rid = section.get ("rid").value_or ("api-a");
        return {.rid = rid,
                .instance_id = section.get ("instanceId").value_or (rid),
                .api_endpoint = section.get ("apiEndpoint").value_or (""),
                .route_endpoint = section.get ("routeEndpoint").value_or (""),
                .http_endpoint = section.get ("httpEndpoint").value_or (""),
                .redis_endpoint = section.require ("redis.endpoint"),
                .redis_key_prefix = section.require ("redis.keyPrefix"),
                .route_peers = split_csv (section.get ("routePeers").value_or ("")),
                .log_dir = section.require ("logDir"),
                .server_weight = parse_optional_int (section, "serverWeight")};
    }
};

} // namespace zlink::framework::e2e::registry_messaging::provider
