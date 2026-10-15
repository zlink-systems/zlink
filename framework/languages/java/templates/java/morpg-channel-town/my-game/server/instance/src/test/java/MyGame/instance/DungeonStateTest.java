package MyGame.instance;

import static org.junit.jupiter.api.Assertions.*;

import MyGame.*;

import org.junit.jupiter.api.Test;

import java.util.*;

class DungeonStateTest {
    @Test
    void preparationRejectsInvalidRosterAndRepeatedPreparation() {
        for (var request :
                List.of(
                        new PrepareDungeon("party", List.of()),
                        new PrepareDungeon("party", List.of("a", "a")),
                        new PrepareDungeon("party", List.of("")),
                        new PrepareDungeon("other", List.of("a")),
                        new PrepareDungeon("party", List.of("a", "b", "c", "d", "e")))) {
            var state = new DungeonState();
            assertThrows(
                    IllegalArgumentException.class, () -> state.prepare("dungeon-party", request));
            assertTrue(state.members.isEmpty());
        }
        var state = new DungeonState();
        state.prepare("dungeon-party", new PrepareDungeon("party", List.of("a", "b")));
        assertThrows(
                IllegalArgumentException.class,
                () -> state.prepare("dungeon-party", new PrepareDungeon("party", List.of("a"))));
        assertEquals(List.of("a", "b"), state.members);
    }

    @Test
    void relocationPreservesProgressAndRoster() throws Exception {
        var state = new DungeonState();
        state.prepare("dungeon-party", new PrepareDungeon("party", List.of("a", "b")));
        state.stage = 1;
        var restored =
                Json.MAPPER.readValue(Json.MAPPER.writeValueAsBytes(state), DungeonState.class);
        assertEquals(state.partyId, restored.partyId);
        assertEquals(state.members, restored.members);
        assertEquals(state.stage, restored.stage);
    }
}
