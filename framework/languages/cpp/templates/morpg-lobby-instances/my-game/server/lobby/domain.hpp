#pragma once
#include "server/shared/contracts.hpp"
#include <zlink/json.hpp>
#include <vector>
namespace MyGame
{
struct PartyState
{
    std::string partyId;
    std::vector<std::string> members;
    std::vector<std::string> ready, entered;
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
        std::erase (ready, id);
    }
    PartyView view () const
    {
        PartyView result{partyId, members, {}, phase, runId};
        for (const auto &id : members)
            result.ready.push_back (std::binary_search (ready.begin (), ready.end (), id));
        return result;
    }
    std::string entry_rejection (const std::string &id) const
    {
        if (members.empty () || members.front () != id)
            return Rejection::not_leader;
        for (const auto &member : members)
            if (!std::binary_search (ready.begin (), ready.end (), member))
                return Rejection::not_ready;
        return {};
    }
    void mark_ready(const std::string &id) {
        if (phase == RunPhase::Forming && std::ranges::find(members, id) != members.end())
            insert_sorted_unique (ready, id);
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
        insert_sorted_unique (entered, player); return {};
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
}

namespace zlink::json
{
template <> struct dto_traits<::MyGame::PartyState>
{
    static constexpr auto fields = std::tuple{
      zlink::json::field ("partyId", &::MyGame::PartyState::partyId),
      zlink::json::field ("members", &::MyGame::PartyState::members),
      zlink::json::field ("ready", &::MyGame::PartyState::ready),
      zlink::json::field ("entered", &::MyGame::PartyState::entered),
      zlink::json::field ("phase", &::MyGame::PartyState::phase),
      zlink::json::field ("runId", &::MyGame::PartyState::runId)
    };
};
} // namespace zlink::json
