#pragma once
#include "world.hpp"
#include "server/instance/domain.hpp"
#include "server/town/domain.hpp"

namespace MyGame::E2E
{
using PartyRunState = MyGame::PartyRun;
inline void run_domain_checks ()
{
    int count = 0;
    const auto check = [&] (bool value, const char *name) {
        require (value, std::string ("Domain: ") + name);
        ++count;
    };
    const auto same_json = [] (const auto &left, const auto &right) {
        return nlohmann::json (left) == nlohmann::json (right);
    };
    check (ChannelRules::validate (Names::maximum_integer + 1) == "integer-out-of-range",
           "channel above maximum");
    check (ChannelRules::validate (Names::maximum_integer) == "invalid-channel",
           "channel maximum");
    check (ChannelRules::validate (-Names::maximum_integer - 1) == "integer-out-of-range",
           "channel below minimum");
    check (ChannelRules::validate (0) == "invalid-channel", "channel zero");
    check (!ChannelRules::validate (1) && !ChannelRules::validate (2), "valid channels");
    check (ChannelRules::validate (3) == "invalid-channel", "channel above count");

    for (const auto phase : {RunPhase::Forming, RunPhase::Preparing, RunPhase::Entering,
                             RunPhase::Active, RunPhase::Completed}) {
        const auto run_id = std::string ("run-") + std::string (32, 'a');
        for (const auto *action : {"enter", "prepared", "started", "completed"}) {
            PartyRunState sample;
            sample.members = {"leader", "member"};
            sample.phase = phase;
            if (phase != RunPhase::Forming)
                sample.runId = run_id;
            const auto snapshot = nlohmann::json (sample);
            std::optional<std::string> result;
            if (std::string_view (action) == "enter")
                result = sample.enter ("leader");
            else if (std::string_view (action) == "prepared")
                result = sample.prepared (run_id);
            else if (std::string_view (action) == "started")
                result = sample.started (run_id);
            else
                result = sample.completed (run_id);
            const bool allowed = std::string_view (action) == "enter"
                                 || (std::string_view (action) == "prepared"
                                     && (phase == RunPhase::Preparing || phase == RunPhase::Entering))
                                 || (std::string_view (action) == "started"
                                     && (phase == RunPhase::Entering || phase == RunPhase::Active))
                                 || (std::string_view (action) == "completed"
                                     && (phase == RunPhase::Active || phase == RunPhase::Completed));
            check (allowed ? !result
                           : result == "run-mismatch" && nlohmann::json (sample) == snapshot,
                   "transition table");
        }
        PartyRunState original;
        original.members = {"leader", "member"};
        original.phase = phase;
        if (phase != RunPhase::Forming)
            original.runId = run_id;
        const auto encoded = nlohmann::json (original);
        check (same_json (encoded.get<PartyRunState> (), original), "party phase snapshot");
    }

    PartyRunState party;
    party.members = {"leader", "member"};
    check (DungeonState::valid_id (party.partyId, "party-"), "party ID");
    check (party.phase == RunPhase::Forming && !party.runId, "forming");
    check (!party.change_channel (), "forming channel");
    auto before = nlohmann::json (party);
    check (party.enter ("member") == "not-leader" && before == nlohmann::json (party),
           "nonleader unchanged");
    check (party.started ("unknown") == "run-mismatch", "forming start rejected");
    check (party.completed ("unknown") == "run-mismatch", "forming completion rejected");
    check (party.prepared ("unknown") == "run-mismatch", "forming preparation rejected");
    check (!party.enter ("leader") && party.phase == RunPhase::Preparing, "prepare transition");
    const auto run_id = *party.runId;
    check (DungeonState::valid_id (run_id, "run-"), "run ID");
    check (!party.enter ("leader") && party.runId == run_id, "preparing retry");
    check (party.change_channel () == "run-in-progress", "preparing channel");
    party.leave ("member");
    check (std::find (party.members.begin (), party.members.end (), "member") != party.members.end (),
           "preparing retains member");
    check (party.started (run_id) == "run-mismatch", "preparing start rejected");
    check (party.completed (run_id) == "run-mismatch", "preparing completion rejected");
    check (party.prepared ("other") == "run-mismatch", "wrong preparation");
    check (!party.prepared (run_id) && party.phase == RunPhase::Entering, "entering transition");
    check (!party.prepared (run_id), "prepared retry");
    check (!party.enter ("leader") && party.runId == run_id, "entering retry");
    check (party.completed (run_id) == "run-mismatch", "entering completion rejected");
    check (party.change_channel () == "run-in-progress", "entering channel");
    party.leave ("member");
    check (party.members.size () == 2, "entering retains member");
    check (!party.started (run_id) && party.phase == RunPhase::Active, "active transition");
    check (!party.started (run_id), "started retry");
    check (!party.enter ("leader") && party.runId == run_id, "active enter unchanged");
    check (party.prepared (run_id) == "run-mismatch", "active prepare rejected");
    check (party.completed ("other") == "run-mismatch", "wrong completion");
    check (party.change_channel () == "run-in-progress", "active channel");
    party.leave ("member");
    check (party.members.size () == 2, "active retains member");
    check (!party.completed (run_id) && party.phase == RunPhase::Completed,
           "completed transition");
    check (!party.completed (run_id), "completed retry");
    check (party.prepared (run_id) == "run-mismatch"
             && party.started (run_id) == "run-mismatch",
           "completed rejects old transitions");
    check (party.change_channel () == "run-in-progress", "completed channel");
    party.leave ("member");
    check (party.members.size () == 2, "completed retains member");
    check (same_json (nlohmann::json (party).get<PartyRunState> (), party), "party snapshot");
    check (!party.enter ("leader") && party.runId != run_id
             && party.phase == RunPhase::Preparing,
           "new completed run");
    PartyRunState forming;
    forming.members = {"leader"};
    forming.leave ("leader");
    check (forming.members.empty (), "forming leave");

    DungeonState dungeon;
    const auto dungeon_id = Names::dungeon_id (run_id);
    const PrepareDungeon request{run_id, party.partyId, "town-2", {"leader", "member"}};
    check (dungeon.advance () == "run-not-active" && dungeon.stage == 0, "unprepared advance");
    check (!dungeon.prepare (dungeon_id, request), "prepare dungeon");
    before = nlohmann::json (dungeon);
    check (!dungeon.prepare (dungeon_id, request) && before == nlohmann::json (dungeon),
           "idempotent prepare");
    auto wrong_run = request;
    wrong_run.runId = *party.runId;
    check (dungeon.prepare (dungeon_id, wrong_run) == "run-mismatch"
             && before == nlohmann::json (dungeon),
           "wrong run unchanged");
    auto wrong_members = request;
    wrong_members.members = {"member", "leader"};
    check (dungeon.prepare (dungeon_id, wrong_members) == "run-mismatch"
             && before == nlohmann::json (dungeon),
           "wrong members unchanged");
    auto wrong_owner = request;
    wrong_owner.ownerSpotId = "town-1";
    check (dungeon.prepare (dungeon_id, wrong_owner) == "run-mismatch"
             && before == nlohmann::json (dungeon),
           "wrong owner unchanged");
    check (dungeon.advance () == "run-not-active" && before == nlohmann::json (dungeon),
           "entering advance unchanged");
    const JoinDungeon join{dungeon_id, run_id};
    check (!dungeon.accepts ("other", dungeon_id, join), "nonmember admission");
    check (!dungeon.accepts ("leader", dungeon_id, JoinDungeon{dungeon_id, "other"}),
           "wrong run admission");
    check (dungeon.accepts ("leader", dungeon_id, join), "member admission");
    check (!dungeon.join ("other") && dungeon.joined.empty (), "unknown join unchanged");
    check (!dungeon.join ("leader") && dungeon.phase == RunPhase::Entering, "partial arrival");
    check (!dungeon.join ("leader") && dungeon.joined.size () == 1, "duplicate arrival");
    check (dungeon.join ("member") && dungeon.phase == RunPhase::Active, "all arrive");
    check (!dungeon.advance () && dungeon.stage == 1, "advance one");
    check (!dungeon.advance () && dungeon.stage == 2, "advance two");
    check (!dungeon.advance () && dungeon.stage == Names::final_stage
             && dungeon.phase == RunPhase::Completed,
           "final stage");
    before = nlohmann::json (dungeon);
    check (dungeon.advance () == "run-not-active" && before == nlohmann::json (dungeon),
           "completed advance unchanged");
    check (same_json (nlohmann::json (dungeon).get<DungeonState> (), dungeon),
           "dungeon snapshot");
    dungeon.stage = Names::maximum_integer;
    before = nlohmann::json (dungeon);
    check (dungeon.advance () == "integer-out-of-range" && nlohmann::json (dungeon) == before,
           "integer boundary unchanged");
    dungeon.stage = Names::maximum_integer + 1;
    check (dungeon.advance () == "integer-out-of-range", "integer above bound");
    check (nlohmann::json (RunPhase::Active) == "Active", "enum name");
    check (nlohmann::json (PartyRunState{}).at ("runId").is_null (), "nullable run");
    check (ActionRejected{"run-not-active"}.retryAfterMs == 0, "rejection time");
    require (count == 91, "Domain check count differs from .NET baseline");
    std::cout << "Domain checks: " << count << "/" << count << " passed" << std::endl;
}
}
