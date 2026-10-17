#pragma once
#include "contracts.hpp"
#include <fstream>
#include <filesystem>

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
    std::string Id, RedisEndpoint, KeyPrefix, MeshEndpoint, StreamEndpoint;
    static NodeConfig load (const std::filesystem::path &path)
    {
        const auto j = read_json (path);
        NodeConfig c;
        c.Id = j.at ("Id");
        c.RedisEndpoint = j.at ("RedisEndpoint");
        c.KeyPrefix = j.at ("KeyPrefix");
        c.MeshEndpoint = j.at ("MeshEndpoint");
        c.StreamEndpoint = j.at ("StreamEndpoint");
        if (c.Id.empty () || c.RedisEndpoint.empty () || c.KeyPrefix.empty ()
            || c.MeshEndpoint.empty () || c.StreamEndpoint.empty ())
            throw std::invalid_argument ("Node configuration fields must be nonempty");
        return c;
    }
};
}
