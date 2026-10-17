#pragma once
#include "contracts.hpp"
#include <limits>
namespace MyGame
{
struct PlayerState
{
    int x = 0, sequence = 0;
    void apply (const EnterField &arrival)
    {
        x = arrival.x;
        sequence = arrival.sequence;
    }
    void validate (const MoveRequest &request) const
    {
        if (sequence == std::numeric_limits<int>::max () || request.sequence != sequence + 1)
            throw std::invalid_argument ("Move sequence must increase by one");
        (void) Names::field_id (request.x);
    }
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (PlayerState, x, sequence)
};
}
