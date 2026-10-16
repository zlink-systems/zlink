#pragma once
#include "server/shared/contracts.hpp"
#include <algorithm>
#include <set>
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
        const std::set<std::string> unique (request.members.begin (), request.members.end ());
        if (!partyId.empty () || !Names::valid_id (request.partyId)
            || Names::dungeon_id (request.partyId) != id || request.members.empty ()
            || request.members.size () > Names::maximum_party_members
            || unique.size () != request.members.size ()
            || std::any_of (request.members.begin (), request.members.end (),
                            [] (const auto &member) { return !Names::valid_id (member); }))
            throw std::invalid_argument ("Invalid dungeon party preparation");
        partyId = request.partyId;
        members = request.members;
    }
    bool accepts (std::string_view actor, const std::string &id, const JoinDungeon &join) const
    {
        return join.dungeonId == id && join.partyId == partyId
               && std::find (members.begin (), members.end (), actor) != members.end ();
    }
    void advance ()
    {
        if (stage == std::numeric_limits<int>::max ())
            throw std::overflow_error ("Dungeon stage exhausted");
        ++stage;
    }
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (DungeonState, partyId, members, stage)
};
}
