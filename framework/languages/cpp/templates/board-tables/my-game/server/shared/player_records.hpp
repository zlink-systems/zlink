#pragma once
#include "contracts.hpp"

namespace MyGame
{
inline constexpr auto player_records_channel = "my-game-player-records";

struct LoadPlayer
{
    static constexpr auto packet_name = "LoadPlayer";
    std::string playerId;
};
struct PlayerRecord
{
    static constexpr auto packet_name = "PlayerRecord";
    std::string playerId;
    std::int64_t chips;
};
struct CommitRound
{
    static constexpr auto packet_name = "CommitRound";
    std::string tableId;
    std::int64_t handNumber;
    std::vector<PlayerRecord> results;
};
struct RoundCommitted
{
    static constexpr auto packet_name = "RoundCommitted";
    std::string tableId;
    std::int64_t handNumber;
};
}

namespace zlink::json
{
template <> struct dto_traits<::MyGame::LoadPlayer>
{
    using Dto = ::MyGame::LoadPlayer;
    static constexpr auto fields = std::tuple{field ("playerId", &Dto::playerId)};
};
template <> struct dto_traits<::MyGame::PlayerRecord>
{
    using Dto = ::MyGame::PlayerRecord;
    static constexpr auto fields =
      std::tuple{field ("playerId", &Dto::playerId), field ("chips", &Dto::chips)};
};
template <> struct dto_traits<::MyGame::CommitRound>
{
    using Dto = ::MyGame::CommitRound;
    static constexpr auto fields =
      std::tuple{field ("tableId", &Dto::tableId), field ("handNumber", &Dto::handNumber),
                 field ("results", &Dto::results)};
};
template <> struct dto_traits<::MyGame::RoundCommitted>
{
    using Dto = ::MyGame::RoundCommitted;
    static constexpr auto fields =
      std::tuple{field ("tableId", &Dto::tableId), field ("handNumber", &Dto::handNumber)};
};
}
