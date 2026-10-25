#pragma once
#include "server/shared/contracts.hpp"
#include <algorithm>
#include <set>

namespace MyGame
{
struct PartyRun
{
    std::vector<std::string> members;
    std::vector<std::string> entered;
    RunPhase phase = RunPhase::Forming;
    std::optional<std::string> runId;

    static bool valid_id (std::string_view id, std::string_view prefix)
    {
        if (id.size () != prefix.size () + 32 || !id.starts_with (prefix))
            return false;
        return std::all_of (id.begin () + static_cast<std::ptrdiff_t> (prefix.size ()),
                            id.end (),
                            [] (char value) {
                                return (value >= '0' && value <= '9')
                                       || (value >= 'a' && value <= 'f');
                            });
    }

    std::optional<ActionRejected> join (const std::string &id)
    {
        if (phase != RunPhase::Forming
            || std::find (members.begin (), members.end (), id) != members.end ()
            || members.size () >= Names::maximum_party_members || !Names::valid_id (id))
            return ActionRejected{Rejection::invalid_party};
        members.push_back (id);
        return std::nullopt;
    }

    void leave (const std::string &id)
    {
        if (phase == RunPhase::Forming)
            std::erase (members, id);
    }

    std::optional<ActionRejected> enter (const std::string &id)
    {
        if (members.empty () || members.front () != id)
            return ActionRejected{Rejection::not_leader};
        if (phase == RunPhase::Forming || phase == RunPhase::Completed) {
            runId = Names::new_id ("run-");
            entered.clear ();
            phase = RunPhase::Preparing;
        }
        return std::nullopt;
    }

    std::optional<ActionRejected> prepared (const std::string &id)
    {
        if (!runId || id != *runId
            || (phase != RunPhase::Preparing && phase != RunPhase::Entering))
            return ActionRejected{Rejection::run_mismatch};
        phase = RunPhase::Entering;
        return std::nullopt;
    }

    std::optional<ActionRejected> arrived (const std::string &id, const std::string &actor_id)
    {
        if (!runId || id != *runId
            || std::find (members.begin (), members.end (), actor_id) == members.end ()
            || (phase != RunPhase::Entering && phase != RunPhase::Active))
            return ActionRejected{Rejection::run_mismatch};
        if (std::find (entered.begin (), entered.end (), actor_id) == entered.end ())
            entered.push_back (actor_id);
        return std::nullopt;
    }

    std::optional<ActionRejected> started (const std::string &id)
    {
        if (!runId || id != *runId
            || (phase != RunPhase::Entering && phase != RunPhase::Active)
            || entered.size () != members.size ())
            return ActionRejected{Rejection::run_mismatch};
        phase = RunPhase::Active;
        return std::nullopt;
    }

    std::optional<ActionRejected> completed (const std::string &id)
    {
        if (!runId || id != *runId
            || (phase != RunPhase::Active && phase != RunPhase::Completed))
            return ActionRejected{Rejection::run_mismatch};
        phase = RunPhase::Completed;
        return std::nullopt;
    }

    PartyView view (const std::string &party_id) const
    {
        return {party_id, members, members.empty () ? std::string{} : members.front (), phase, runId};
    }

    NLOHMANN_DEFINE_TYPE_INTRUSIVE (PartyRun, members, entered, phase, runId)
};
}
