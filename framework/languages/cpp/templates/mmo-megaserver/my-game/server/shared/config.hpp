#pragma once
#include "contracts.hpp"
#include <fstream>
#include <filesystem>
#include <optional>

namespace MyGame
{
inline nlohmann::json read_json (const std::filesystem::path &path)
{
    std::ifstream input (path);
    if (!input)
        throw std::runtime_error ("Cannot read " + path.string ());
    return nlohmann::json::parse (input);
}
struct NodeConfig
{
    std::string Id, RedisEndpoint, KeyPrefix, MeshEndpoint;
    std::optional<std::string> StreamEndpoint;
    int Capacity = Names::default_capacity;
    int RegionLimit = Names::default_region_limit;
    static NodeConfig load (const std::filesystem::path &path)
    {
        const auto j = read_json (path);
        NodeConfig c;
        c.Id = j.at ("Id");
        c.RedisEndpoint = j.at ("RedisEndpoint");
        c.KeyPrefix = j.at ("KeyPrefix");
        c.MeshEndpoint = j.at ("MeshEndpoint");
        if (j.contains ("StreamEndpoint") && !j.at ("StreamEndpoint").is_null ())
            c.StreamEndpoint = j.at ("StreamEndpoint").get<std::string> ();
        c.Capacity = j.value ("Capacity", Names::default_capacity);
        c.RegionLimit = j.value ("RegionLimit", Names::default_region_limit);
        if (c.Capacity <= 0 || c.RegionLimit <= 0)
            throw std::invalid_argument ("Capacity and RegionLimit must be positive");
        if (c.Id.empty () || c.RedisEndpoint.empty () || c.KeyPrefix.empty ()
            || c.MeshEndpoint.empty ())
            throw std::invalid_argument ("Node configuration fields must be nonempty");
        return c;
    }
};
}
