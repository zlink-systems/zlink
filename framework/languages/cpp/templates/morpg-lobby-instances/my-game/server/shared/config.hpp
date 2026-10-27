#pragma once
#include "contracts.hpp"
#include <filesystem>
#include <fstream>
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
    int DungeonCapacity = 0;
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
        c.DungeonCapacity = j.value ("DungeonCapacity", 0);
        if (c.DungeonCapacity < 0)
            throw std::invalid_argument ("Negative DungeonCapacity");
        if (c.Id.empty () || c.RedisEndpoint.empty () || c.KeyPrefix.empty ()
            || c.MeshEndpoint.empty ())
            throw std::invalid_argument ("Node configuration fields must be nonempty");
        return c;
    }
};
}
