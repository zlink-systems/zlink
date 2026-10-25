#pragma once
#include "contracts.hpp"
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
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (PlayerState, x, sequence, partyId)
};
}
