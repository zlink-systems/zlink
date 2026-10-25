package MyGame.match;

import static org.junit.jupiter.api.Assertions.*;

import MyGame.*;

import org.junit.jupiter.api.Test;

final class DomainTest {
    @Test
    void matchmakingPairsByModeAndPreservesQueuedPlayersAndRoomSequence() throws Exception {
        var state = new MatchmakingState();
        var first = new QueueRequest("p1");
        state.enqueue(first);
        state.enqueue(first);
        state.enqueue(new QueueRequest("p2", "squad"));
        assertTrue(state.pair().isEmpty());
        var restored =
                Json.MAPPER.readValue(Json.MAPPER.writeValueAsBytes(state), MatchmakingState.class);
        restored.enqueue(new QueueRequest("p3"));
        var pair = restored.pair();
        assertEquals(Rules.PLAYERS_PER_ROOM, pair.size());
        assertTrue(pair.stream().allMatch(player -> player.mode().equals(Names.DEFAULT_MODE)));
        assertEquals(Names.ROOM_PREFIX + 1, restored.allocateRoom());
        restored.remove(pair);
        restored =
                Json.MAPPER.readValue(Json.MAPPER.writeValueAsBytes(restored), MatchmakingState.class);
        assertEquals(Names.ROOM_PREFIX + 2, restored.allocateRoom());
        restored.enqueue(new QueueRequest("p4", "squad"));
        assertEquals("p2", restored.pair().getFirst().playerId());
    }
}
