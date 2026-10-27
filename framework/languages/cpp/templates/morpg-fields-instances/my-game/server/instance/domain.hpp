#pragma once
#include "server/shared/contracts.hpp"
#include <algorithm>
#include <set>

namespace MyGame
{
struct DungeonRun
{
    // partyId is the owning Party Spot ID used for progress requests.
    std::string partyId;
    std::string runId;
    std::vector<std::string> members;
    std::vector<std::string> entered;
    RunPhase phase = RunPhase::Preparing;
    int stage = 0;

    std::optional<ActionRejected> prepare (const PrepareDungeon &request)
    {
        if (!runId.empty ())
            return runId == request.runId && partyId == request.partyId
                           && members == request.members
                     ? std::nullopt
                     : std::optional<ActionRejected> (ActionRejected{Rejection::run_mismatch});
        const std::set<std::string> unique (request.members.begin (), request.members.end ());
        if (!Names::valid_id (request.partyId) || !Names::valid_id (request.runId)
            || request.members.empty () || request.members.size () > Names::maximum_party_members
            || unique.size () != request.members.size ()
            || std::any_of (request.members.begin (), request.members.end (),
                            [] (const auto &member) { return !Names::valid_id (member); }))
            return ActionRejected{Rejection::invalid_party};
        runId = request.runId;
        partyId = request.partyId;
        members = request.members;
        phase = RunPhase::Entering;
        return std::nullopt;
    }

    bool can_join (const std::string &actor_id, const std::string &id) const
    {
        return id == runId
               && std::find (members.begin (), members.end (), actor_id) != members.end ();
    }

    void arrived (const std::string &actor_id)
    {
        if (std::find (entered.begin (), entered.end (), actor_id) == entered.end ())
            entered.push_back (actor_id);
        if (entered.size () == members.size () && phase == RunPhase::Entering)
            phase = RunPhase::Active;
    }

    std::optional<ActionRejected> advance ()
    {
        if (phase != RunPhase::Active)
            return ActionRejected{Rejection::run_not_active};
        ++stage;
        if (stage == Names::final_stage)
            phase = RunPhase::Completed;
        return std::nullopt;
    }

    NLOHMANN_DEFINE_TYPE_INTRUSIVE (DungeonRun, partyId, runId, members, entered, phase, stage)
};
}
