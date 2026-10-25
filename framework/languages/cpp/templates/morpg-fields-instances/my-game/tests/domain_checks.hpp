#pragma once
#include "server/field/domain.hpp"
#include "server/field/party-domain.hpp"
#include "server/instance/domain.hpp"
#include <iostream>
#include <limits>

namespace MyGame::DomainChecks
{
inline int run ()
{
    int count = 0;
    auto check = [&] (bool condition, const std::string &message) {
        if (!condition)
            throw std::runtime_error ("Domain: " + message);
        ++count;
    };
    const auto has_reason = [] (const auto &rejection, const auto &reason) {
        return rejection && rejection->reason == reason;
    };
    std::int64_t now = 1000;
    FieldPositionDomain domain ("field-1", [&] { return now; });
    domain.add_player ("p", EnterField{50, 0});
    const std::vector<std::pair<MoveRequest, std::string>> invalid_moves{
      {{std::numeric_limits<double>::quiet_NaN (), 1}, Rejection::non_finite},
      {{std::numeric_limits<double>::infinity (), 1}, Rejection::non_finite},
      {{-std::numeric_limits<double>::infinity (), 1}, Rejection::non_finite},
      {{10.01, 1}, Rejection::step_too_large},
      {{0, Names::max_safe_integer + 1}, Rejection::integer_out_of_range},
      {{0, -1}, Rejection::stale_sequence},
      {{0, 0}, Rejection::stale_sequence}};
    for (const auto &[request, reason] : invalid_moves) {
        const auto before = domain.state ("p");
        const auto rejected = domain.move ("p", request);
        check (rejected.rejection && rejected.rejection->reason == reason
                 && rejected.rejection->retryAfterMs == 0,
               "movement rejection " + reason);
        check (domain.state ("p") == before, "movement rejection changed state");
    }
    check (domain.move ("p", {10, 1}).accepted, "first move budget");
    const auto state = domain.state ("p");
    const auto budget = domain.move ("p", {10, 2});
    check (budget.rejection && budget.rejection->reason == Rejection::move_budget
             && budget.rejection->retryAfterMs == 200,
           "budget retry");
    check (domain.state ("p") == state, "budget rejection changed state");
    now += budget.rejection->retryAfterMs;
    check (domain.move ("p", {10, 2}).accepted, "retry after server delay");
    now += 25;
    const auto snapshot = nlohmann::json (domain.capture ()).get<FieldPositionSnapshot> ();
    FieldPositionDomain restored ("field-1", [&] { return now + 10'000; });
    restored.restore (snapshot);
    check (restored.state ("p") == domain.state ("p"), "relative time snapshot");
    check (restored.move ("p", {0, Names::max_safe_integer}).accepted, "safe integer upper bound");
    const auto range_before_stale = restored.move ("p", {0, Names::max_safe_integer + 1});
    check (range_before_stale.rejection
             && range_before_stale.rejection->reason == Rejection::integer_out_of_range,
           "range before stale");
    const auto maximum_is_stale = restored.move ("p", {0, Names::max_safe_integer});
    check (maximum_is_stale.rejection
             && maximum_is_stale.rejection->reason == Rejection::stale_sequence,
           "maximum sequence stale");
    domain.add_player ("edge", EnterField{0, 0});
    const auto minimum = domain.move ("edge", {-1, 1});
    check (has_reason (minimum.rejection, Rejection::outside_world), "world minimum");
    FieldPositionDomain last ("field-4", [&] { return now; });
    last.add_player ("edge", EnterField{399, 0});
    const auto maximum = last.move ("edge", {1, 1});
    check (has_reason (maximum.rejection, Rejection::outside_world), "world maximum");
    last.add_player ("fraction", EnterField{399.5, 0});
    check (last.move ("fraction", {0, 1}).accepted, "fractional world edge");
    check (has_reason (domain.queue_arrival ("x", EnterField{100, 0}), Rejection::outside_world),
           "arrival field");
    check (has_reason (domain.queue_arrival ("x", EnterField{1, Names::max_safe_integer + 1}),
                       Rejection::integer_out_of_range),
           "arrival sequence");
    check (has_reason (domain.queue_arrival ("x", EnterField{1, 0, 201}),
                       Rejection::integer_out_of_range),
           "arrival remaining time");

    for (const auto phase : {RunPhase::Forming, RunPhase::Preparing, RunPhase::Entering,
                             RunPhase::Active, RunPhase::Completed}) {
        PartyRun party;
        party.members = {"leader", "member"};
        party.phase = phase;
        party.runId = phase == RunPhase::Forming ? std::nullopt
                                                : std::optional<std::string> ("run-" + std::string (32, 'a'));
        const auto before = nlohmann::json (party);
        const auto non_leader_rejection = party.enter ("member");
        check (has_reason (non_leader_rejection, Rejection::not_leader)
                 && nlohmann::json (party) == before,
               "nonleader immutable");
        const auto old_run = party.runId;
        check (!party.enter ("leader"), "leader entry");
        check (party.runId && PartyRun::valid_id (*party.runId, "run-"), "run ID");
        check ((phase == RunPhase::Forming || phase == RunPhase::Completed)
                 ? party.runId != old_run && party.phase == RunPhase::Preparing
                 : party.runId == old_run && party.phase == phase,
               "entry transition");
        PartyRun leaving;
        leaving.members = {"leader", "member"};
        leaving.phase = phase;
        leaving.leave ("member");
        check (leaving.members.size () == (phase == RunPhase::Forming ? 1U : 2U), "leave");
        PartyRun transition;
        transition.members = {"leader"};
        transition.phase = phase;
        transition.runId = "run";
        check (static_cast<bool> (transition.prepared ("run"))
                 == !(phase == RunPhase::Preparing || phase == RunPhase::Entering),
               "prepared transition");
        transition.phase = phase;
        transition.entered = {"leader"};
        check (static_cast<bool> (transition.started ("run"))
                 == !(phase == RunPhase::Entering || phase == RunPhase::Active),
               "started transition");
        transition.phase = phase;
        check (static_cast<bool> (transition.completed ("run"))
                 == !(phase == RunPhase::Active || phase == RunPhase::Completed),
               "completed transition");
        transition.phase = phase;
        const auto wrong_prepare = transition.prepared ("wrong");
        const auto wrong_start = transition.started ("wrong");
        const auto wrong_complete = transition.completed ("wrong");
        check (has_reason (wrong_prepare, Rejection::run_mismatch)
                 && has_reason (wrong_start, Rejection::run_mismatch)
                 && has_reason (wrong_complete, Rejection::run_mismatch),
               "mismatched transitions");
        DungeonRun dungeon;
        dungeon.phase = phase;
        check (static_cast<bool> (dungeon.advance ()) == (phase != RunPhase::Active),
               "advance phase");
    }

    PartyRun run;
    check (!run.join ("leader") && !run.join ("member"), "forming membership");
    check (has_reason (run.join ("member"), Rejection::invalid_party), "duplicate member");
    PartyRun full_party;
    check (!full_party.join ("p1") && !full_party.join ("p2") && !full_party.join ("p3")
             && !full_party.join ("p4")
             && has_reason (full_party.join ("p5"), Rejection::invalid_party),
           "party capacity");
    check (!run.enter ("leader"), "party enter");
    const auto run_id = *run.runId;
    check (has_reason (run.join ("late"), Rejection::invalid_party), "membership frozen");
    check (!run.prepared (run_id), "party prepared");
    check (!run.arrived (run_id, "leader") && !run.arrived (run_id, "member"),
           "party arrivals");
    check (!run.started (run_id) && !run.completed (run_id), "complete state machine");
    const auto restored_party = nlohmann::json (run).get<PartyRun> ();
    check (nlohmann::json (restored_party) == nlohmann::json (run), "party snapshot");
    check (!run.enter ("leader") && run.runId != run_id && run.phase == RunPhase::Preparing,
           "fresh run after completion");

    DungeonRun prepared;
    const auto party_id = Names::new_id ("party-");
    const PrepareDungeon prepare{run_id, party_id, {"leader", "member"}};
    check (PartyRun::valid_id (party_id, "party-"), "party ID");
    DungeonRun duplicate_roster;
    check (has_reason (duplicate_roster.prepare (
                         PrepareDungeon{run_id, party_id, {"leader", "leader"}}),
                       Rejection::invalid_party),
           "duplicate dungeon member");
    DungeonRun blank_roster;
    check (has_reason (blank_roster.prepare (PrepareDungeon{run_id, party_id, {" "}}),
                       Rejection::invalid_party),
           "blank dungeon member");
    DungeonRun oversized_roster;
    check (has_reason (oversized_roster.prepare (
                         PrepareDungeon{run_id, party_id, {"p1", "p2", "p3", "p4", "p5"}}),
                       Rejection::invalid_party),
           "dungeon roster capacity");
    check (!prepared.prepare (prepare) && !prepared.prepare (prepare), "idempotent preparation");
    check (prepared.can_join ("leader", run_id), "prepared member admitted");
    check (!prepared.can_join ("outsider", run_id), "foreign member admitted");
    check (!prepared.can_join ("leader", "other"), "foreign run admitted");
    const auto prepared_state = nlohmann::json (prepared);
    const auto wrong_run = prepared.prepare (PrepareDungeon{"other", party_id, {"leader", "member"}});
    const auto wrong_members = prepared.prepare (PrepareDungeon{run_id, party_id, {"other"}});
    check (wrong_run && wrong_run->reason == Rejection::run_mismatch && wrong_members
             && wrong_members->reason == Rejection::run_mismatch
             && nlohmann::json (prepared) == prepared_state,
           "preparation mismatch immutable");
    prepared.arrived ("leader");
    const auto partial_advance = prepared.advance ();
    check (prepared.phase == RunPhase::Entering
             && has_reason (partial_advance, Rejection::run_not_active),
           "partial arrival");
    prepared.arrived ("member");
    bool completed = false;
    for (int stage = 1; stage <= Names::final_stage; ++stage)
        completed = !prepared.advance () && prepared.stage == stage;
    check (completed, "stages");
    const auto completed_advance = prepared.advance ();
    check (prepared.phase == RunPhase::Completed
             && has_reason (completed_advance, Rejection::run_not_active),
           "final stage");
    const auto restored_dungeon = nlohmann::json (prepared).get<DungeonRun> ();
    check (nlohmann::json (restored_dungeon) == nlohmann::json (prepared), "dungeon snapshot");
    const auto view_json = nlohmann::json (run.view (party_id)).dump ();
    check (view_json.find ("\"phase\":\"Preparing\"") != std::string::npos,
           "enum name JSON");
    const auto nullable_json = nlohmann::json (PartyRun{}.view (party_id)).dump ();
    check (nullable_json.find ("\"runId\":null") != std::string::npos, "nullable JSON");
    return count;
}
}
