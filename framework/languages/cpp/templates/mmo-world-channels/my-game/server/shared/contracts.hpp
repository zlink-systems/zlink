#pragma once
#include <nlohmann/json.hpp>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace MyGame
{
struct Names
{
    static constexpr auto zone = "Zone", player = "Player", stream = "players";
    static constexpr auto zone_prefix = "field@", player_key = "player:", worlds_path = "/worlds";
    static constexpr int first_channel = 1;
    static std::string mesh (const std::string &world)
    {
        if (world == "A")
            return "my-game-world-a";
        if (world == "B")
            return "my-game-world-b";
        throw std::invalid_argument ("Unknown world");
    }
    static std::string zone_id (int channel)
    {
        if (channel < first_channel)
            throw std::invalid_argument ("Invalid channel");
        return zone_prefix + std::to_string (channel);
    }
    static int channel_of (const std::string &id)
    {
        if (!id.starts_with (zone_prefix))
            throw std::invalid_argument ("Invalid Zone ID");
        const auto channel = std::stoi (id.substr (std::string (zone_prefix).size ()));
        if (zone_id (channel) != id)
            throw std::invalid_argument ("Invalid Zone ID");
        return channel;
    }
};
struct Limits
{
    static constexpr auto packet = std::chrono::seconds (10);
    static constexpr auto startup = std::chrono::seconds (30);
    static constexpr auto relocation = std::chrono::seconds (25);
    static constexpr auto shutdown = std::chrono::seconds (10);
    static constexpr std::size_t observation_capacity = 64;
};
struct Control
{
    static constexpr auto ready = "READY ", drain = "drain", shutdown = "shutdown";
    static constexpr auto requested = "RELOCATION_REQUESTED", relocated = "RELOCATED";
    static constexpr auto safe = "SAFE_TO_SHUTDOWN", stopped = "STOPPED";
    static constexpr auto player_created = "PLAYER_CREATED ";
};
struct EnterWorldRequest
{
    static constexpr auto packet_name = "EnterWorldRequest";
    std::string playerId;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (EnterWorldRequest, playerId)
};
struct ChangeChannelRequest
{
    static constexpr auto packet_name = "ChangeChannelRequest";
    int channel = 0;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (ChangeChannelRequest, channel)
};
struct EarnRequest
{
    static constexpr auto packet_name = "EarnRequest";
    std::int64_t amount = 0;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (EarnRequest, amount)
};
struct ViewRequest
{
    static constexpr auto packet_name = "ViewRequest";
};
inline void to_json (nlohmann::json &j, const ViewRequest &)
{
    j = nlohmann::json::object ();
}
inline void from_json (const nlohmann::json &, ViewRequest &)
{
}
struct PlayerView
{
    static constexpr auto packet_name = "PlayerView";
    std::string worldId;
    int channel = 0;
    std::string nodeId;
    std::int64_t gold = 0;
    bool operator== (const PlayerView &) const = default;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (PlayerView, worldId, channel, nodeId, gold)
};
struct SessionInfoRequest
{
    static constexpr auto packet_name = "SessionInfoRequest";
};
inline void to_json (nlohmann::json &j, const SessionInfoRequest &)
{
    j = nlohmann::json::object ();
}
inline void from_json (const nlohmann::json &, SessionInfoRequest &)
{
}
struct SessionInfo
{
    static constexpr auto packet_name = "SessionInfo";
    std::string sessionId, actorId;
    std::uint64_t objectGeneration = 0;
    bool operator== (const SessionInfo &) const = default;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (SessionInfo, sessionId, actorId, objectGeneration)
};
struct WorldInfo
{
    std::string worldId, name, sessionEndpoint;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (WorldInfo, worldId, name, sessionEndpoint)
};
}
