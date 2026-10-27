#pragma once
#include "server/lobby/domain.hpp"
#include "server/instance/domain.hpp"
#include <regex>
#include <iostream>
namespace MyGame::E2E {
inline void domain_checks() {
    int count = 0;
    const auto check = [&](bool value, const std::string &name) { if (!value) throw std::runtime_error("Domain check failed: " + name); ++count; };
    const auto snap = [](const auto &v) { return nlohmann::json(v).dump(); };
    const auto party = [](RunPhase phase = RunPhase::Forming) {
        PartyState p; p.partyId = new_id("party-"); p.members = {"leader", "member"}; p.ready = {"leader", "member"}; p.phase = phase;
        if (phase != RunPhase::Forming) p.runId = new_id("run-"); return p;
    };
    check(std::regex_match(new_id("party-"), std::regex("party-[0-9a-f]{32}")), "party ID");
    check(std::regex_match(new_id("run-"), std::regex("run-[0-9a-f]{32}")), "run ID");
    check(Names::maximum_integer == 9007199254740991LL && Names::final_stage == 3, "bounds");
    for (const auto phase : {RunPhase::Forming, RunPhase::Preparing, RunPhase::Entering, RunPhase::Active, RunPhase::Completed}) {
        auto p = party(phase); auto before = snap(p);
        check(p.enter("member") == Rejection::not_leader && snap(p) == before, "non-leader unchanged");
        p.ready.erase("member"); before = snap(p);
        check(p.enter("leader") == Rejection::not_ready && snap(p) == before, "not-ready unchanged");
        p.ready.insert("member"); auto old = p.runId;
        check(p.enter("leader").empty(), "leader");
        check(phase == RunPhase::Forming || phase == RunPhase::Completed ? p.phase == RunPhase::Preparing && p.runId != old : p.phase == phase && p.runId == old, "entry");
        old = p.runId; check(p.enter("leader").empty() && p.runId == old, "repeated entry");
        p = party(phase); before = snap(p);
        check(p.prepared("wrong") == Rejection::run_mismatch && snap(p) == before, "prepared mismatch");
        check(p.prepared(p.runId.value_or("")).empty() == (phase == RunPhase::Preparing || phase == RunPhase::Entering), "prepared table");
        p = party(phase); before = snap(p);
        check(p.started("wrong") == Rejection::run_mismatch && snap(p) == before, "started mismatch");
        check(p.started(p.runId.value_or("")).empty() == (phase == RunPhase::Entering || phase == RunPhase::Active), "started table");
        p = party(phase); before = snap(p);
        check(p.completed("wrong") == Rejection::run_mismatch && snap(p) == before, "completed mismatch");
        check(p.completed(p.runId.value_or("")).empty() == (phase == RunPhase::Active || phase == RunPhase::Completed), "completed table");
        p = party(phase); p.leave("member");
        check((std::ranges::find(p.members, "member") != p.members.end()) == (phase != RunPhase::Forming), "leave");
        check(snap(nlohmann::json(p).get<PartyState>()) == snap(p), "party snapshot");
        DungeonState d; d.phase = phase; before = snap(d);
        check(d.advance().empty() == (phase == RunPhase::Active), "advance table");
        check(phase == RunPhase::Active || snap(d) == before, "rejected advance unchanged");
    }
    DungeonState d; const PrepareDungeon request{new_id("party-"), {"leader", "member"}, new_id("run-")};
    check(d.prepare(request).empty(), "prepare"); auto snapshot = snap(d);
    check(d.prepare(request).empty() && snap(d) == snapshot, "prepare idempotence");
    auto changed = request; changed.runId = new_id("run-");
    check(d.prepare(changed) == Rejection::run_mismatch && snap(d) == snapshot, "run mismatch");
    changed = request; changed.members = {"leader"};
    check(d.prepare(changed) == Rejection::run_mismatch && snap(d) == snapshot, "roster mismatch");
    changed = request; changed.partyId = new_id("party-");
    check(d.prepare(changed) == Rejection::run_mismatch && snap(d) == snapshot, "party mismatch");
    check(!d.admits("outsider", {Names::dungeon_id(request.runId), request.runId}, Names::dungeon_id(request.runId)), "admission roster");
    check(!d.join("outsider") && snap(d) == snapshot, "outsider unchanged");
    check(!d.join("leader") && d.phase == RunPhase::Entering, "partial");
    check(!d.join("leader") && d.phase == RunPhase::Entering, "duplicate");
    check(d.join("member") && d.phase == RunPhase::Active, "all arrived");
    check(!d.join("member") && d.phase == RunPhase::Active, "started idempotent");
    for (std::int64_t stage = 1; stage <= Names::final_stage; ++stage) check(d.advance().empty() && d.stage == stage && d.phase == (stage == Names::final_stage ? RunPhase::Completed : RunPhase::Active), "stage");
    snapshot = snap(d); check(d.advance() == Rejection::run_not_active && snap(d) == snapshot, "completed unchanged");
    check(snap(nlohmann::json(d).get<DungeonState>()) == snapshot, "dungeon snapshot");
    for (const auto stage : {std::int64_t(-1), Names::maximum_integer, Names::maximum_integer + 1, std::numeric_limits<std::int64_t>::max()}) {
        d = {}; d.stage = stage; d.phase = RunPhase::Active; snapshot = snap(d);
        check(d.advance() == Rejection::integer_out_of_range && snap(d) == snapshot, "integer boundary");
    }
    auto arrivals = party(RunPhase::Entering); auto arrival_snapshot = snap(arrivals);
    check(arrivals.arrived("wrong", "member") == Rejection::run_mismatch && snap(arrivals) == arrival_snapshot, "arrival run mismatch");
    check(arrivals.arrived(*arrivals.runId, "outsider") == Rejection::run_mismatch && snap(arrivals) == arrival_snapshot, "arrival roster mismatch");
    check(arrivals.arrived(*arrivals.runId, "member").empty() && arrivals.entered.contains("member"), "arrival recorded");
    arrival_snapshot = snap(arrivals);
    check(arrivals.arrived(*arrivals.runId, "member").empty() && snap(arrivals) == arrival_snapshot, "arrival idempotent");
    check(snap(nlohmann::json(arrivals).get<PartyState>()) == arrival_snapshot, "arrival snapshot");
    arrivals.phase = RunPhase::Completed;
    check(arrivals.enter("leader").empty() && arrivals.entered.empty(), "new run clears arrivals");
    const auto view = nlohmann::json(party().view());
    check(view.at("runId").is_null() && view.at("phase") == "Forming", "enum null JSON");
    std::cout << "Domain checks: " << count << "/" << count << " passed" << std::endl;
}
}
