#pragma once
#include "server/shared/contracts.hpp"
#include <map>

namespace MyGame
{
struct PlayerState
{
    int x = 0, sequence = 0;
    void apply (const EnterZone &arrival)
    {
        x = arrival.x;
        sequence = arrival.sequence;
    }
    void validate (const MoveRequest &request) const
    {
        if (request.sequence != sequence + 1)
            throw std::invalid_argument ("Move sequence must increase by one");
        (void) Names::zone_id (request.x);
    }
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (PlayerState, x, sequence)
};
// Visibility is an application projection; PlayerState owns movement.
struct ZoneState
{
    std::map<std::string, EnterZone> visiblePlayers, arrivals;
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (ZoneState, visiblePlayers, arrivals)
};
}
