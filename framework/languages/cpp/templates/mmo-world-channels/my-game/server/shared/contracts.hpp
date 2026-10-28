#pragma once
#include <zlink/json.hpp>
#include <nlohmann/json.hpp>
#include <chrono>
#include <cstdint>
#include <optional>
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
    static constexpr int update_channel = 3;
    static std::string mesh (const std::string &world)
    {
        if (world == "A")
            return "my-game-world-a";
        if (world == "B")
            return "my-game-world-b";
        throw std::invalid_argument ("Unknown world");
    }
    static std::string zone_id (std::int64_t channel)
    {
        if (channel < first_channel)
            throw std::invalid_argument ("Invalid channel");
        return zone_prefix + std::to_string (channel);
    }
    static std::int64_t channel_of (const std::string &id)
    {
        if (!id.starts_with (zone_prefix))
            throw std::invalid_argument ("Invalid Zone ID");
        const auto channel = std::stoll (id.substr (std::string (zone_prefix).size ()));
        if (zone_id (channel) != id)
            throw std::invalid_argument ("Invalid Zone ID");
        return static_cast<std::int64_t> (channel);
    }
};
struct Limits
{
    static constexpr auto packet = std::chrono::seconds (10);
    static constexpr auto startup = std::chrono::seconds (30);
    static constexpr auto message_follow = std::chrono::seconds (30);
    static constexpr auto relocation = std::chrono::seconds (25);
    static constexpr auto safe = message_follow + relocation;
    static constexpr auto shutdown = std::chrono::seconds (10);
    static constexpr std::size_t observation_capacity = 64;
};
struct Control
{
    static constexpr auto ready = "READY ", drain = "drain", shutdown = "shutdown";
    static constexpr auto requested = "RELOCATION_REQUESTED", relocated = "RELOCATED";
    static constexpr auto safe = "SAFE_TO_SHUTDOWN", stopped = "STOPPED";
    static constexpr auto player_created = "PLAYER_CREATED ";
    static constexpr auto create_update_zone = "create-update-zone";
    static constexpr auto update_zone_created = "UPDATE_ZONE_CREATED";
};
struct EnterWorldRequest
{
    static constexpr auto packet_name = "EnterWorldRequest";
    std::string playerId;
};
struct ChangeChannelRequest
{
    static constexpr auto packet_name = "ChangeChannelRequest";
    std::int64_t channel = 0;
};
struct GameRules
{
    static constexpr std::int64_t max_integer = 9007199254740991;
    static constexpr std::int64_t earn_amount = 10;
    static constexpr std::int64_t earn_interval_ms = 1000;
};
struct Rejection
{
    static constexpr auto earn_too_soon = "earn-too-soon";
    static constexpr auto gold_limit = "gold-limit";
    static constexpr auto integer_out_of_range = "integer-out-of-range";
    static constexpr auto packet_not_allowed = "packet-not-allowed";
};
struct EarnRequest
{
    static constexpr auto packet_name = "EarnRequest";
};

struct ViewRequest
{
    static constexpr auto packet_name = "ViewRequest";
};

struct ChannelViewRequest
{
    static constexpr auto packet_name = "ChannelViewRequest";
};

struct ChannelView
{
    static constexpr auto packet_name = "ChannelView";
    std::int64_t channelId = 0;
    std::vector<std::string> playerIds;
};
struct ActionRejected
{
    static constexpr auto packet_name = "ActionRejected";
    std::string reason;
    std::int32_t retryAfterMs = 0;
};
struct PlayerView
{
    static constexpr auto packet_name = "PlayerView";
    std::string worldId;
    std::int64_t channel = 0;
    std::string nodeId;
    std::int64_t gold = 0;
    bool operator== (const PlayerView &) const = default;
};
struct SessionInfoRequest
{
    static constexpr auto packet_name = "SessionInfoRequest";
};

struct SessionInfo
{
    static constexpr auto packet_name = "SessionInfo";
    std::string sessionId, actorId;
    std::uint64_t objectGeneration = 0;
    bool operator== (const SessionInfo &) const = default;
};
struct WorldInfo
{
    std::string worldId, name, sessionEndpoint;
};
}

namespace zlink::json
{
template <> struct dto_traits<::MyGame::EnterWorldRequest>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("playerId", &::MyGame::EnterWorldRequest::playerId)
    };
};

template <> struct dto_traits<::MyGame::ChangeChannelRequest>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("channel", &::MyGame::ChangeChannelRequest::channel)
    };
};

template <> struct dto_traits<::MyGame::ChannelView>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("channelId", &::MyGame::ChannelView::channelId),
      zlink::json::field ("playerIds", &::MyGame::ChannelView::playerIds)
    };
};

template <> struct dto_traits<::MyGame::ActionRejected>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("reason", &::MyGame::ActionRejected::reason),
      zlink::json::field ("retryAfterMs", &::MyGame::ActionRejected::retryAfterMs)
    };
};

template <> struct dto_traits<::MyGame::PlayerView>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("worldId", &::MyGame::PlayerView::worldId),
      zlink::json::field ("channel", &::MyGame::PlayerView::channel),
      zlink::json::field ("nodeId", &::MyGame::PlayerView::nodeId),
      zlink::json::field ("gold", &::MyGame::PlayerView::gold)
    };
};

template <> struct dto_traits<::MyGame::SessionInfo>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("sessionId", &::MyGame::SessionInfo::sessionId),
      zlink::json::field ("actorId", &::MyGame::SessionInfo::actorId),
      zlink::json::field ("objectGeneration", &::MyGame::SessionInfo::objectGeneration)
    };
};

template <> struct dto_traits<::MyGame::WorldInfo>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("worldId", &::MyGame::WorldInfo::worldId),
      zlink::json::field ("name", &::MyGame::WorldInfo::name),
      zlink::json::field ("sessionEndpoint", &::MyGame::WorldInfo::sessionEndpoint)
    };
};

template <> struct dto_traits<::MyGame::EarnRequest>
{
    static constexpr auto fields = std::tuple{};
};

template <> struct dto_traits<::MyGame::ViewRequest>
{
    static constexpr auto fields = std::tuple{};
};

template <> struct dto_traits<::MyGame::ChannelViewRequest>
{
    static constexpr auto fields = std::tuple{};
};

template <> struct dto_traits<::MyGame::SessionInfoRequest>
{
    static constexpr auto fields = std::tuple{};
};
} // namespace zlink::json
