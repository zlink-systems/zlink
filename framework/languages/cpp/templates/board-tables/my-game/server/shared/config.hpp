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
    std::optional<std::string> StreamEndpoint, ApiEndpoint, PlayerRecordsKeyPrefix;
    int TableCapacity = 0;
    static NodeConfig load (const std::filesystem::path &path)
    {
        const auto j = read_json (path);
        NodeConfig c;
        c.Id = j.at ("Id");
        c.RedisEndpoint = j.at ("RedisEndpoint");
        c.KeyPrefix = j.at ("KeyPrefix");
        c.MeshEndpoint = j.value ("MeshEndpoint", "");
        if (j.contains ("ApiEndpoint"))
            c.ApiEndpoint = j.at ("ApiEndpoint").get<std::string> ();
        if (j.contains ("PlayerRecordsKeyPrefix"))
            c.PlayerRecordsKeyPrefix = j.at ("PlayerRecordsKeyPrefix").get<std::string> ();
        c.TableCapacity = j.value ("TableCapacity", c.TableCapacity);
        if (c.TableCapacity < 0)
            throw std::invalid_argument ("Table capacity must be nonnegative");
        if (j.contains ("StreamEndpoint") && !j.at ("StreamEndpoint").is_null ())
            c.StreamEndpoint = j.at ("StreamEndpoint").get<std::string> ();
        if (c.Id.empty () || c.RedisEndpoint.empty () || c.KeyPrefix.empty ()
            || (c.MeshEndpoint.empty () && !c.ApiEndpoint))
            throw std::invalid_argument ("Node configuration fields must be nonempty");
        return c;
    }
};
}
