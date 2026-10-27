package MyGame.instance;

import static org.junit.jupiter.api.Assertions.*;

import MyGame.*;

import org.junit.jupiter.api.Test;

import java.util.*;

class DungeonStateTest {
    private static final String RUN_ID = "run-0123456789abcdef0123456789abcdef";
    private static final String PARTY_ID = "party-0123456789abcdef0123456789abcdef";

    private static PrepareDungeon request(List<String> members) {
        return new PrepareDungeon(RUN_ID, PARTY_ID, "town-2", members);
    }

    @Test
    void preparationRejectsInvalidRosterAndRepeatedIdentityChanges() throws Exception {
        for (var invalid :
                List.of(
                        request(List.of()),
                        request(List.of("a", "a")),
                        request(List.of("")),
                        new PrepareDungeon("run-invalid", PARTY_ID, "town-2", List.of("a")),
                        new PrepareDungeon(RUN_ID, "party-invalid", "town-2", List.of("a")),
                        new PrepareDungeon(RUN_ID, PARTY_ID, " ", List.of("a")),
                        request(List.of("a", "b", "c", "d", "e")))) {
            var state = new DungeonState();
            assertEquals(Rules.RUN_MISMATCH, state.prepare(Names.dungeonId(RUN_ID), invalid));
            assertTrue(state.members.isEmpty());
            assertTrue(state.runId.isEmpty());
        }

        var state = new DungeonState();
        var initial = request(List.of("a", "b"));
        String dungeonId = Names.dungeonId(RUN_ID);
        assertNull(state.prepare(dungeonId, initial));
        var snapshot = Json.MAPPER.writeValueAsString(state);
        assertNull(state.prepare(dungeonId, initial));
        assertEquals(snapshot, Json.MAPPER.writeValueAsString(state));
        assertEquals(
                Rules.RUN_MISMATCH,
                state.prepare(
                        dungeonId,
                        new PrepareDungeon("run-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", PARTY_ID, "town-2", initial.members())));
        assertEquals(
                Rules.RUN_MISMATCH,
                state.prepare(
                        dungeonId,
                        new PrepareDungeon(RUN_ID, PARTY_ID, "town-2", List.of("b", "a"))));
        assertEquals(
                Rules.RUN_MISMATCH,
                state.prepare(
                        dungeonId,
                        new PrepareDungeon(RUN_ID, PARTY_ID, "town-1", initial.members())));
        assertEquals(snapshot, Json.MAPPER.writeValueAsString(state));
    }

    @Test
    void relocationPreservesRunOwnershipAndProgress() throws Exception {
        var state = new DungeonState();
        assertNull(state.prepare(Names.dungeonId(RUN_ID), request(List.of("a", "b"))));
        state.joined.add("a");
        state.joined.add("b");
        state.phase = RunPhase.Active;
        state.stage = 2;
        var restored =
                Json.MAPPER.readValue(Json.MAPPER.writeValueAsBytes(state), DungeonState.class);
        assertEquals(state.partyId, restored.partyId);
        assertEquals(state.runId, restored.runId);
        assertEquals(state.ownerSpotId, restored.ownerSpotId);
        assertEquals(state.members, restored.members);
        assertEquals(state.joined, restored.joined);
        assertEquals(state.phase, restored.phase);
        assertEquals(state.stage, restored.stage);
    }
}
