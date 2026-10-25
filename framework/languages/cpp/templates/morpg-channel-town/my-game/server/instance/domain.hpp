#pragma once
#include "server/shared/contracts.hpp"
#include "server/shared/domain.hpp"
#include <algorithm>
#include <set>
#include <string_view>

namespace MyGame
{
struct DungeonState
{
    std::string partyId;
    std::string runId;
    std::string ownerSpotId;
    std::vector<std::string> members;
    std::set<std::string> joined;
    RunPhase phase = RunPhase::Forming;
    std::int64_t stage = 0;

    std::optional<std::string> prepare (const std::string &dungeon_id,
                                        const PrepareDungeon &request)
    {
        if (!runId.empty ())
            return runId == request.runId && partyId == request.partyId
                             && ownerSpotId == request.ownerSpotId && members == request.members
                     ? std::nullopt
                     : std::optional<std::string> ("run-mismatch");
        const std::set<std::string> distinct (request.members.begin (), request.members.end ());
        if (!valid_id (request.runId, "run-") || !valid_id (request.partyId, "party-")
            || blank (request.ownerSpotId) || Names::dungeon_id (request.runId) != dungeon_id
            || request.members.empty () || request.members.size () > Names::maximum_party_members
            || distinct.size () != request.members.size ()
            || std::any_of (request.members.begin (), request.members.end (), blank))
            return "run-mismatch";
        runId = request.runId;
        partyId = request.partyId;
        ownerSpotId = request.ownerSpotId;
        members = request.members;
        phase = RunPhase::Entering;
        return std::nullopt;
    }
    static bool valid_id (std::string_view value, std::string_view prefix)
    {
        return value.size () == prefix.size () + 32 && value.starts_with (prefix)
               && std::all_of (value.begin () + static_cast<std::ptrdiff_t> (prefix.size ()),
                               value.end (),
                               [] (char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
    }
    bool accepts (std::string_view actor, const std::string &dungeon_id,
                  const JoinDungeon &request) const
    {
        return request.dungeonId == dungeon_id && request.runId == runId
               && std::find (members.begin (), members.end (), actor) != members.end ();
    }
    bool join (const std::string &actor)
    {
        if (std::find (members.begin (), members.end (), actor) == members.end ())
            return false;
        joined.insert (actor);
        if (phase != RunPhase::Entering || joined.size () != members.size ())
            return false;
        phase = RunPhase::Active;
        return true;
    }
    std::optional<std::string> advance ()
    {
        if (stage < 0 || stage >= Names::maximum_integer)
            return "integer-out-of-range";
        if (phase != RunPhase::Active)
            return "run-not-active";
        ++stage;
        if (stage == Names::final_stage)
            phase = RunPhase::Completed;
        return std::nullopt;
    }
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (DungeonState, partyId, runId, ownerSpotId, members, joined,
                                   phase, stage)
};
}
