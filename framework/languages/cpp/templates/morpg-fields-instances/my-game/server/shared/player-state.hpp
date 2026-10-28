#pragma once
#include "contracts.hpp"
#include <zlink/json.hpp>
namespace MyGame
{
struct PlayerState
{
    double x = 0;
    std::int64_t sequence = 0;
    std::optional<std::string> partyId;
    void apply (const EnterField &arrival)
    {
        x = arrival.x;
        sequence = arrival.sequence;
    }
};
}

namespace zlink::json
{
template <> struct dto_traits<::MyGame::PlayerState>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("x", &::MyGame::PlayerState::x),
      zlink::json::field ("sequence", &::MyGame::PlayerState::sequence),
      zlink::json::field ("partyId", &::MyGame::PlayerState::partyId)
    };
};
} // namespace zlink::json
