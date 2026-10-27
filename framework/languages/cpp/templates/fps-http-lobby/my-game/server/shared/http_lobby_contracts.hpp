#pragma once
#include "contracts.hpp"

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
    NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT (MatchRequest, playerId, mode)
};

struct MatchResponse
{
    std::string roomId{};
    std::string sessionEndpoint{};
    bool operator== (const MatchResponse &) const = default;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (MatchResponse, roomId, sessionEndpoint)
};

struct JoinRoomRequest
{
    static constexpr auto packet_name = "JoinRoomRequest";
    std::string playerId{};
    std::string roomId{};
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (JoinRoomRequest, playerId, roomId)
};

struct PrepareRoom
{
    static constexpr auto packet_name = "PrepareRoom";
    std::string matchId{};
    std::vector<std::string> members{};
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (PrepareRoom, matchId, members)
};

struct RoomPrepared
{
    static constexpr auto packet_name = "RoomPrepared";
    std::string matchId{};
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (RoomPrepared, matchId)
};

struct HttpNodeReady
{
    std::string id{};
    std::string meshEndpoint{};
    std::optional<std::string> streamEndpoint{};
    std::optional<std::string> httpEndpoint{};
};
inline void to_json (nlohmann::json &j, const HttpNodeReady &value)
{
    j = nlohmann::json::object ();
    j["id"] = value.id;
    j["meshEndpoint"] = value.meshEndpoint;
    j["streamEndpoint"] = detail::to_nullable_json (value.streamEndpoint);
    j["httpEndpoint"] = detail::to_nullable_json (value.httpEndpoint);
}
inline void from_json (const nlohmann::json &j, HttpNodeReady &value)
{
    j.at ("id").get_to (value.id);
    j.at ("meshEndpoint").get_to (value.meshEndpoint);
    detail::from_nullable_json (j.at ("streamEndpoint"), value.streamEndpoint);
    if (j.contains ("httpEndpoint"))
        detail::from_nullable_json (j.at ("httpEndpoint"), value.httpEndpoint);
}
}
