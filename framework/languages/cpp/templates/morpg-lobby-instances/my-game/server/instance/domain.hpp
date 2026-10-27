#pragma once
#include "server/shared/contracts.hpp"
#include <set>
namespace MyGame {
struct DungeonState {
    std::string partyId;
    std::vector<std::string> members;
    std::set<std::string> joined;
    std::int64_t stage = 0;
    std::optional<std::string> runId;
    RunPhase phase = RunPhase::Preparing;
    std::string prepare(const PrepareDungeon &r) {
        if (runId) return *runId == r.runId && partyId == r.partyId && members == r.members ? "" : Rejection::run_mismatch;
        const std::set<std::string> unique(r.members.begin(), r.members.end());
        if (!nonblank(r.runId) || !nonblank(r.partyId) || r.members.empty() || r.members.size() > Names::maximum_members || unique.size() != r.members.size() || std::ranges::any_of(r.members, [](const auto &id) { return !nonblank(id); })) return Rejection::run_mismatch;
        runId = r.runId; partyId = r.partyId; members = r.members; phase = RunPhase::Entering; return {};
    }
    bool admits(std::string_view id, const JoinDungeon &r, std::string_view spot) const {
        return r.dungeonId == spot && runId == r.runId && std::ranges::find(members, id) != members.end();
    }
    bool join(const std::string &id) {
        if (std::ranges::find(members, id) == members.end()) return false;
        joined.insert(id);
        if (phase != RunPhase::Entering || joined.size() != members.size()) return false;
        phase = RunPhase::Active; return true;
    }
    std::string advance() {
        if (stage >= Names::maximum_integer || stage < 0) return Rejection::integer_out_of_range;
        if (phase != RunPhase::Active) return Rejection::run_not_active;
        ++stage; if (stage == Names::final_stage) phase = RunPhase::Completed; return {};
    }
};

inline void to_json(nlohmann::json &j, const DungeonState& v) {
    j["partyId"] = v.partyId;
    j["members"] = v.members;
    j["joined"] = v.joined;
    j["stage"] = v.stage;
    j["runId"] = v.runId ? nlohmann::json(*v.runId) : nlohmann::json(nullptr);
    j["phase"] = v.phase;
}
inline void from_json(const nlohmann::json &j, DungeonState& v) {
    j.at("partyId").get_to(v.partyId);
    j.at("members").get_to(v.members);
    j.at("joined").get_to(v.joined);
    j.at("stage").get_to(v.stage);
    if (j.at("runId").is_null()) v.runId.reset(); else v.runId = j.at("runId").get<std::string>();
    j.at("phase").get_to(v.phase);
}
}
