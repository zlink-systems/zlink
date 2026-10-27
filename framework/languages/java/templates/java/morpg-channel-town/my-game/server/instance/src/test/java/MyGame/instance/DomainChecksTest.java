package MyGame.instance;

import static org.junit.jupiter.api.Assertions.assertTrue;

import MyGame.*;

import org.junit.jupiter.api.Test;

import java.util.*;

class DomainChecksTest {
    private static final String RUN_ID = "run-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

    private static final class Checks {
        int count;

        void check(boolean value, String name) {
            assertTrue(value, "Domain: " + name);
            count++;
        }
    }

    @Test
    void domainBoundariesAndTransitionsMatchDotnet() throws Exception {
        var checks = new Checks();

        checks.check(
                ChannelRules.validate(Names.MAXIMUM_INTEGER + 1) == Rules.INTEGER_OUT_OF_RANGE,
                "channel above maximum");
        checks.check(
                ChannelRules.validate(Names.MAXIMUM_INTEGER) == Rules.INVALID_CHANNEL,
                "channel maximum");
        checks.check(
                ChannelRules.validate(-Names.MAXIMUM_INTEGER - 1) == Rules.INTEGER_OUT_OF_RANGE,
                "channel below minimum");
        checks.check(ChannelRules.validate(0) == Rules.INVALID_CHANNEL, "channel zero");
        checks.check(
                ChannelRules.validate(1) == null && ChannelRules.validate(2) == null,
                "valid channels");
        checks.check(ChannelRules.validate(3) == Rules.INVALID_CHANNEL, "channel above count");

        for (var phase : RunPhase.values()) {
            for (var action : List.of("enter", "prepared", "started", "completed")) {
                var sample = new PartyRun();
                sample.members = new ArrayList<>(List.of("leader", "member"));
                sample.phase = phase;
                sample.runId = phase == RunPhase.Forming ? null : RUN_ID;
                var snapshot = Json.MAPPER.writeValueAsString(sample);
                String result = switch (action) {
                    case "enter" -> sample.enter("leader");
                    case "prepared" -> sample.prepared(RUN_ID);
                    case "started" -> sample.started(RUN_ID);
                    default -> sample.completed(RUN_ID);
                };
                boolean allowed = switch (action) {
                    case "enter" -> true;
                    case "prepared" -> phase == RunPhase.Preparing || phase == RunPhase.Entering;
                    case "started" -> phase == RunPhase.Entering || phase == RunPhase.Active;
                    default -> phase == RunPhase.Active || phase == RunPhase.Completed;
                };
                checks.check(
                        allowed
                                ? result == null
                                : Rules.RUN_MISMATCH.equals(result)
                                        && Json.MAPPER.writeValueAsString(sample).equals(snapshot),
                        "transition table " + phase + "/" + action);
            }
            var original = new PartyRun();
            original.members = new ArrayList<>(List.of("leader", "member"));
            original.phase = phase;
            original.runId = phase == RunPhase.Forming ? null : RUN_ID;
            var encoded = Json.MAPPER.writeValueAsString(original);
            checks.check(
                    Json.MAPPER.writeValueAsString(
                                    Json.MAPPER.readValue(encoded, PartyRun.class))
                            .equals(encoded),
                    "snapshot " + phase);
        }

        var party = new PartyRun();
        party.members = new ArrayList<>(List.of("leader", "member"));
        checks.check(Names.validId(party.partyId, "party-"), "party ID");
        checks.check(
                party.phase == RunPhase.Forming && party.runId == null,
                "forming");
        checks.check(party.changeChannel() == null, "forming channel");
        var before = Json.MAPPER.writeValueAsString(party);
        checks.check(
                Rules.NOT_LEADER.equals(party.enter("member"))
                        && before.equals(Json.MAPPER.writeValueAsString(party)),
                "nonleader unchanged");
        checks.check(Rules.RUN_MISMATCH.equals(party.started("unknown")), "forming start rejected");
        checks.check(Rules.RUN_MISMATCH.equals(party.completed("unknown")), "forming completion rejected");
        checks.check(Rules.RUN_MISMATCH.equals(party.prepared("unknown")), "forming preparation rejected");
        checks.check(
                party.enter("leader") == null && party.phase == RunPhase.Preparing,
                "prepare transition");
        String run = party.runId;
        checks.check(Names.validId(run, "run-"), "run ID");
        checks.check(
                party.enter("leader") == null && party.runId.equals(run),
                "preparing retry");
        checks.check(Rules.RUN_IN_PROGRESS.equals(party.changeChannel()), "preparing channel");
        party.leave("member");
        checks.check(party.members.contains("member"), "preparing retains member");
        checks.check(Rules.RUN_MISMATCH.equals(party.started(run)), "preparing start rejected");
        checks.check(Rules.RUN_MISMATCH.equals(party.completed(run)), "preparing completion rejected");
        checks.check(Rules.RUN_MISMATCH.equals(party.prepared("other")), "wrong preparation");
        checks.check(
                party.prepared(run) == null && party.phase == RunPhase.Entering,
                "entering transition");
        checks.check(party.prepared(run) == null, "prepared retry");
        checks.check(
                party.enter("leader") == null && party.runId.equals(run),
                "entering retry");
        checks.check(Rules.RUN_MISMATCH.equals(party.completed(run)), "entering completion rejected");
        checks.check(Rules.RUN_IN_PROGRESS.equals(party.changeChannel()), "entering channel");
        party.leave("member");
        checks.check(party.members.size() == 2, "entering retains member");
        checks.check(
                party.started(run) == null && party.phase == RunPhase.Active,
                "active transition");
        checks.check(party.started(run) == null, "started retry");
        checks.check(
                party.enter("leader") == null && party.runId.equals(run),
                "active enter unchanged");
        checks.check(Rules.RUN_MISMATCH.equals(party.prepared(run)), "active prepare rejected");
        checks.check(Rules.RUN_MISMATCH.equals(party.completed("other")), "wrong completion");
        checks.check(Rules.RUN_IN_PROGRESS.equals(party.changeChannel()), "active channel");
        party.leave("member");
        checks.check(party.members.size() == 2, "active retains member");
        checks.check(
                party.completed(run) == null && party.phase == RunPhase.Completed,
                "completed transition");
        checks.check(party.completed(run) == null, "completed retry");
        checks.check(
                Rules.RUN_MISMATCH.equals(party.prepared(run))
                        && Rules.RUN_MISMATCH.equals(party.started(run)),
                "completed rejects old transitions");
        checks.check(Rules.RUN_IN_PROGRESS.equals(party.changeChannel()), "completed channel");
        party.leave("member");
        checks.check(party.members.size() == 2, "completed retains member");
        checks.check(
                Json.MAPPER.writeValueAsString(
                                Json.MAPPER.readValue(
                                        Json.MAPPER.writeValueAsString(party), PartyRun.class))
                        .equals(Json.MAPPER.writeValueAsString(party)),
                "party snapshot");
        checks.check(
                party.enter("leader") == null
                        && !party.runId.equals(run)
                        && party.phase == RunPhase.Preparing,
                "new completed run");
        var forming = new PartyRun();
        forming.members.add("leader");
        forming.leave("leader");
        checks.check(forming.members.isEmpty(), "forming leave");

        var dungeon = new DungeonState();
        String dungeonId = Names.dungeonId(run);
        var request = new PrepareDungeon(run, party.partyId, "town-2", List.of("leader", "member"));
        checks.check(
                Rules.RUN_NOT_ACTIVE.equals(dungeon.advance()) && dungeon.stage == 0,
                "unprepared advance");
        checks.check(dungeon.prepare(dungeonId, request) == null, "prepare dungeon");
        before = Json.MAPPER.writeValueAsString(dungeon);
        checks.check(
                dungeon.prepare(dungeonId, request) == null
                        && before.equals(Json.MAPPER.writeValueAsString(dungeon)),
                "idempotent prepare");
        var wrongRun = new PrepareDungeon(
                "run-bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb",
                party.partyId,
                "town-2",
                request.members());
        checks.check(
                Rules.RUN_MISMATCH.equals(dungeon.prepare(dungeonId, wrongRun))
                        && before.equals(Json.MAPPER.writeValueAsString(dungeon)),
                "wrong run unchanged");
        checks.check(
                Rules.RUN_MISMATCH.equals(
                                dungeon.prepare(
                                        dungeonId,
                                        new PrepareDungeon(
                                                run,
                                                party.partyId,
                                                "town-2",
                                                List.of("member", "leader"))))
                        && before.equals(Json.MAPPER.writeValueAsString(dungeon)),
                "wrong members unchanged");
        checks.check(
                Rules.RUN_MISMATCH.equals(
                                dungeon.prepare(
                                        dungeonId,
                                        new PrepareDungeon(
                                                run,
                                                party.partyId,
                                                "town-1",
                                                request.members())))
                        && before.equals(Json.MAPPER.writeValueAsString(dungeon)),
                "wrong owner unchanged");
        checks.check(
                Rules.RUN_NOT_ACTIVE.equals(dungeon.advance())
                        && before.equals(Json.MAPPER.writeValueAsString(dungeon)),
                "entering advance unchanged");
        checks.check(
                !dungeon.accepts("other", dungeonId, new JoinDungeon(dungeonId, run)),
                "nonmember admission");
        checks.check(
                !dungeon.accepts("leader", dungeonId, new JoinDungeon(dungeonId, "other")),
                "wrong run admission");
        checks.check(
                dungeon.accepts("leader", dungeonId, new JoinDungeon(dungeonId, run)),
                "member admission");
        checks.check(!dungeon.join("other") && dungeon.joined.isEmpty(), "unknown join unchanged");
        checks.check(
                !dungeon.join("leader") && dungeon.phase == RunPhase.Entering,
                "partial arrival");
        checks.check(
                !dungeon.join("leader") && dungeon.joined.size() == 1,
                "duplicate arrival");
        checks.check(
                dungeon.join("member") && dungeon.phase == RunPhase.Active,
                "all arrive");
        checks.check(dungeon.advance() == null && dungeon.stage == 1, "advance one");
        checks.check(dungeon.advance() == null && dungeon.stage == 2, "advance two");
        checks.check(
                dungeon.advance() == null
                        && dungeon.stage == Names.FINAL_STAGE
                        && dungeon.phase == RunPhase.Completed,
                "final stage");
        before = Json.MAPPER.writeValueAsString(dungeon);
        checks.check(
                Rules.RUN_NOT_ACTIVE.equals(dungeon.advance())
                        && before.equals(Json.MAPPER.writeValueAsString(dungeon)),
                "completed advance unchanged");
        checks.check(
                Json.MAPPER.writeValueAsString(
                                Json.MAPPER.readValue(before, DungeonState.class))
                        .equals(before),
                "dungeon snapshot");
        dungeon.stage = Names.MAXIMUM_INTEGER;
        before = Json.MAPPER.writeValueAsString(dungeon);
        checks.check(
                Rules.INTEGER_OUT_OF_RANGE.equals(dungeon.advance())
                        && before.equals(Json.MAPPER.writeValueAsString(dungeon)),
                "integer boundary unchanged");
        dungeon.stage = Names.MAXIMUM_INTEGER + 1;
        checks.check(Rules.INTEGER_OUT_OF_RANGE.equals(dungeon.advance()), "integer above bound");
        checks.check(
                Json.MAPPER.writeValueAsString(RunPhase.Active).equals("\"Active\""),
                "enum name");
        checks.check(
                Json.MAPPER.writeValueAsString(
                                new PartyView("party-id", List.of(), RunPhase.Active, null))
                        .equals(
                                "{\"partyId\":\"party-id\",\"members\":[],\"phase\":\"Active\",\"runId\":null}"),
                "camelCase JSON and nullable run");
        checks.check(
                Json.MAPPER.writeValueAsString(new PartyRun()).contains("\"runId\":null"),
                "nullable run");
        checks.check(
                new ActionRejected(Rules.RUN_NOT_ACTIVE).retryAfterMs() == 0,
                "rejection time");
        System.out.println("Domain checks: " + checks.count + "/" + checks.count + " passed");
    }
}
