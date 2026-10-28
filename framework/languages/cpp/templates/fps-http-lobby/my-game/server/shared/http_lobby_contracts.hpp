#pragma once
#include "contracts.hpp"
#include <zlink/json.hpp>

namespace MyGame
{
struct LobbyNames
{
    static constexpr auto match_path = "/match";
};

struct MatchRequest
{
    std::string playerId{};
    std::string mode = Names::default_mode;
};

struct MatchResponse
{
    std::string roomId{};
    std::string sessionEndpoint{};
    bool operator== (const MatchResponse &) const = default;
};

struct JoinRoomRequest
{
    static constexpr auto packet_name = "JoinRoomRequest";
    std::string playerId{};
    std::string roomId{};
};

struct PrepareRoom
{
    static constexpr auto packet_name = "PrepareRoom";
    std::string matchId{};
    std::vector<std::string> members{};
};

struct RoomPrepared
{
    static constexpr auto packet_name = "RoomPrepared";
    std::string matchId{};
};

struct HttpNodeReady
{
    std::string id{};
    std::string meshEndpoint{};
    std::optional<std::string> streamEndpoint{};
    std::optional<std::string> httpEndpoint{};
};

}

namespace zlink::json
{
template <> struct dto_traits<::MyGame::MatchRequest>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("playerId", &::MyGame::MatchRequest::playerId),
      zlink::json::field ("mode", &::MyGame::MatchRequest::mode)
    };
};

template <> struct dto_traits<::MyGame::MatchResponse>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("roomId", &::MyGame::MatchResponse::roomId),
      zlink::json::field ("sessionEndpoint", &::MyGame::MatchResponse::sessionEndpoint)
    };
};

template <> struct dto_traits<::MyGame::JoinRoomRequest>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("playerId", &::MyGame::JoinRoomRequest::playerId),
      zlink::json::field ("roomId", &::MyGame::JoinRoomRequest::roomId)
    };
};

template <> struct dto_traits<::MyGame::PrepareRoom>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("matchId", &::MyGame::PrepareRoom::matchId),
      zlink::json::field ("members", &::MyGame::PrepareRoom::members)
    };
};

template <> struct dto_traits<::MyGame::RoomPrepared>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("matchId", &::MyGame::RoomPrepared::matchId)
    };
};

template <> struct dto_traits<::MyGame::HttpNodeReady>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("id", &::MyGame::HttpNodeReady::id),
      zlink::json::field ("meshEndpoint", &::MyGame::HttpNodeReady::meshEndpoint),
      zlink::json::field ("streamEndpoint", &::MyGame::HttpNodeReady::streamEndpoint),
      zlink::json::field ("httpEndpoint", &::MyGame::HttpNodeReady::httpEndpoint)
    };
};
} // namespace zlink::json
