#pragma once
#include "server/shared/contracts.hpp"
#include "server/shared/domain.hpp"
#include <algorithm>
#include <set>
#include <string_view>
#include <limits>

namespace MyGame
{
struct DungeonState
{
    std::string partyId;
    std::vector<std::string> members;
    int stage = 0;
    void prepare (const std::string &id, const PrepareDungeon &request)
    {
        const std::set<std::string> distinct (request.members.begin (), request.members.end ());
        if (!partyId.empty () || blank (request.partyId) || request.members.empty ()
            || request.members.size () > Names::maximum_party_members
            || distinct.size () != request.members.size ()
            || std::any_of (request.members.begin (), request.members.end (), blank)
            || Names::dungeon_id (request.partyId) != id)
            throw std::invalid_argument ("Invalid dungeon party preparation");
        partyId = request.partyId;
        members = request.members;
    }
    bool accepts (std::string_view actor, const std::string &id, const JoinDungeon &request) const
    {
        return request.dungeonId == id && request.partyId == partyId
               && std::find (members.begin (), members.end (), actor) != members.end ();
    }
    void advance ()
    {
        if (stage == std::numeric_limits<int>::max ())
            throw std::overflow_error ("Dungeon stage overflow");
        ++stage;
    }
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (DungeonState, partyId, members, stage)
};
}
