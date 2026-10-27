#pragma once
#include "server/shared/contracts.hpp"
#include <algorithm>
#include <set>
namespace MyGame
{
struct PartyState
{
    std::string partyId;
    std::vector<std::string> members;
    std::set<std::string> ready, entered;
    RunPhase phase = RunPhase::Forming;
    std::optional<std::string> runId;
    bool join (const std::string &id, const PartyJoin &request)
    {
        if (phase != RunPhase::Forming || !nonblank (request.partyId) || std::ranges::find (members, id) != members.end ()
            || members.size () >= Names::maximum_members
            || (request.create ? !members.empty ()
                               : partyId != request.partyId || members.empty ()))
            return false;
        partyId = request.partyId;
        members.push_back (id);
        return true;
    }
    void leave (const std::string &id)
    {
        if (phase != RunPhase::Forming) return;
        std::erase (members, id);
        ready.erase (id);
    }
    PartyView view () const
    {
        PartyView result{partyId, members, {}, phase, runId};
        for (const auto &id : members)
            result.ready.push_back (ready.contains (id));
        return result;
    }
    std::string entry_rejection (const std::string &id) const
    {
        if (members.empty () || members.front () != id)
            return Rejection::not_leader;
        for (const auto &member : members)
            if (!ready.contains (member))
                return Rejection::not_ready;
        return {};
    }
    void mark_ready(const std::string &id) {
        if (phase == RunPhase::Forming && std::ranges::find(members, id) != members.end()) ready.insert(id);
    }
    std::string enter(const std::string &id) {
        const auto reason = entry_rejection(id); if (!reason.empty()) return reason;
        if (phase == RunPhase::Forming || phase == RunPhase::Completed) {
            runId = new_id("run-"); phase = RunPhase::Preparing; entered.clear();
        }
        return {};
    }
    std::string arrived(const std::string &run, const std::string &player) {
        if (runId != run || phase == RunPhase::Forming || std::ranges::find(members, player) == members.end()) return Rejection::run_mismatch;
        entered.insert(player); return {};
    }
    std::string prepared(const std::string &id) {
        if (runId != id || (phase != RunPhase::Preparing && phase != RunPhase::Entering)) return Rejection::run_mismatch;
        phase = RunPhase::Entering; return {};
    }
    std::string started(const std::string &id) {
        if (runId != id || (phase != RunPhase::Entering && phase != RunPhase::Active)) return Rejection::run_mismatch;
        phase = RunPhase::Active; return {};
    }
    std::string completed(const std::string &id) {
        if (runId != id || (phase != RunPhase::Active && phase != RunPhase::Completed)) return Rejection::run_mismatch;
        phase = RunPhase::Completed; return {};
    }
};

inline void to_json(nlohmann::json &j, const PartyState& v) {
    j["partyId"] = v.partyId;
    j["members"] = v.members;
    j["ready"] = v.ready;
    j["entered"] = v.entered;
    j["phase"] = v.phase;
    j["runId"] = v.runId ? nlohmann::json(*v.runId) : nlohmann::json(nullptr);
}
inline void from_json(const nlohmann::json &j, PartyState& v) {
    j.at("partyId").get_to(v.partyId);
    j.at("members").get_to(v.members);
    j.at("ready").get_to(v.ready);
    j.at("entered").get_to(v.entered);
    j.at("phase").get_to(v.phase);
    if (j.at("runId").is_null()) v.runId.reset(); else v.runId = j.at("runId").get<std::string>();
}
}
