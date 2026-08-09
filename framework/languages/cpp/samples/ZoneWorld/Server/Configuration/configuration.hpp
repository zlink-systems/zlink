/* SPDX-License-Identifier: FSL-1.1-ALv2 */
#pragma once

#include <cstdlib>
#include <stdexcept>
#include <string>

namespace zlink::samples::zoneworld
{

inline std::string env (const char *name, std::string fallback = {})
{
    const auto *value = std::getenv (name);
    return value == nullptr || *value == '\0' ? std::move (fallback) : std::string (value);
}

struct configuration_t
{
    std::string redis_endpoint = env ("ZONEWORLD_REDIS_ENDPOINT", "tcp://127.0.0.1:6379");
    std::string redis_key_prefix = env ("ZONEWORLD_REDIS_KEY_PREFIX", "zoneworld:cpp:");
    std::string node_id = env ("ZONEWORLD_NODE_ID", "zone-node-1");
    std::string mesh_endpoint = env ("ZONEWORLD_MESH_ENDPOINT", "tcp://127.0.0.1:35101");
    std::string peer_endpoint = env ("ZONEWORLD_PEER_ENDPOINT");
    std::string peer_endpoint_2 = env ("ZONEWORLD_PEER_ENDPOINT_2");
    std::string stream_endpoint = env ("ZONEWORLD_STREAM_ENDPOINT", "tcp://127.0.0.1:35201");
    std::string broadcast_endpoint = env ("ZONEWORLD_BROADCAST_ENDPOINT", "tcp://127.0.0.1:35301");
    std::string bootstrap_http_endpoint = env ("ZONEWORLD_BOOTSTRAP_HTTP_ENDPOINT", "http://127.0.0.1:35401");
    std::string log_dir = env ("ZONEWORLD_LOG_DIR", "framework/languages/cpp/samples/ZoneWorld/logs");
};

} // namespace zlink::samples::zoneworld
