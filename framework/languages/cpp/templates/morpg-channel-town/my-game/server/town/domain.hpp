#pragma once
#include "server/shared/contracts.hpp"
#include "server/shared/domain.hpp"
#include <algorithm>
#include <map>
#include <set>

namespace MyGame
{
struct PartyRun
{
    std::string partyId = server_id ("party-");
    std::vector<std::string> members;
    RunPhase phase = RunPhase::Forming;
    std::optional<std::string> runId;

    std::optional<std::string> enter (const std::string &player)
    {
        if (members.empty () || members.front () != player)
            return "not-leader";
        if (phase == RunPhase::Forming || phase == RunPhase::Completed) {
            runId = server_id ("run-");
            phase = RunPhase::Preparing;
        }
        return std::nullopt;
    }
    std::optional<std::string> prepared (const std::string &run_id)
    {
        if (!runId || *runId != run_id
            || (phase != RunPhase::Preparing && phase != RunPhase::Entering))
            return "run-mismatch";
        phase = RunPhase::Entering;
        return std::nullopt;
    }
    std::optional<std::string> started (const std::string &run_id)
    {
        if (!runId || *runId != run_id
            || (phase != RunPhase::Entering && phase != RunPhase::Active))
            return "run-mismatch";
        phase = RunPhase::Active;
        return std::nullopt;
    }
    std::optional<std::string> completed (const std::string &run_id)
    {
        if (!runId || *runId != run_id
            || (phase != RunPhase::Active && phase != RunPhase::Completed))
            return "run-mismatch";
        phase = RunPhase::Completed;
        return std::nullopt;
    }
    void leave (const std::string &player)
    {
        if (phase == RunPhase::Forming)
            std::erase (members, player);
    }
    std::optional<std::string> change_channel () const
    {
        return phase == RunPhase::Forming ? std::nullopt
                                          : std::optional<std::string> ("run-in-progress");
    }
    PartyView view () const { return {partyId, members, phase, runId}; }
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (PartyRun, partyId, members, phase, runId)
};

struct TownState
{
    std::set<std::string> players;
    std::map<std::string, PartyRun> parties;
    bool party (const std::string &id, const std::optional<std::string> &party_id, bool create)
    {
        const auto existing = party_id ? parties.find (*party_id) : parties.end ();
        if (!players.contains (id)
            || std::any_of (parties.begin (), parties.end (),
                            [&] (const auto &item) {
                                return std::find (item.second.members.begin (),
                                                  item.second.members.end (), id)
                                       != item.second.members.end ();
                            })
            || (!create && (!party_id || blank (*party_id) || existing == parties.end ()
                            || existing->second.phase != RunPhase::Forming
                            || existing->second.members.size () >= Names::maximum_party_members)))
            return false;
        if (create) {
            PartyRun party;
            party.members.push_back (id);
            parties.emplace (party.partyId, std::move (party));
        }
        else
            existing->second.members.push_back (id);
        return true;
    }
    void leave (const std::string &id)
    {
        players.erase (id);
        for (auto it = parties.begin (); it != parties.end ();) {
            it->second.leave (id);
            if (it->second.members.empty ())
                it = parties.erase (it);
            else
                ++it;
        }
    }
    NLOHMANN_DEFINE_TYPE_INTRUSIVE (TownState, players, parties)
};
}
