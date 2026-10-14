package MyGame.lobby.application;

import static org.junit.jupiter.api.Assertions.*;

import MyGame.*;

import org.junit.jupiter.api.Test;

import java.util.concurrent.*;

public class MatchmakerTest {
    @Test
    void bothResponsesWaitForPreparedRoom() {
        var prepared = new CompletableFuture<MatchResponse>();
        try (var matches =
                new Matchmaker(
                        id -> {
                            assertEquals(Names.ROOM_PREFIX + "1", id);
                            return prepared;
                        })) {
            var first =
                    matches.match(new MatchRequest("a", Names.DEFAULT_MODE)).toCompletableFuture();
            assertFalse(first.isDone());
            assertThrows(
                    IllegalArgumentException.class,
                    () -> matches.match(new MatchRequest("a", Names.DEFAULT_MODE)));
            var second =
                    matches.match(new MatchRequest("b", Names.DEFAULT_MODE)).toCompletableFuture();
            assertFalse(first.isDone());
            assertFalse(second.isDone());
            var assignment = new MatchResponse(Names.ROOM_PREFIX + "1", "tcp://localhost:1");
            prepared.complete(assignment);
            assertEquals(assignment, first.join());
            assertEquals(assignment, second.join());
        }
    }

    @Test
    void modesStaySeparateAndShutdownFailsWaiters() {
        var matches =
                new Matchmaker(
                        id ->
                                CompletableFuture.completedFuture(
                                        new MatchResponse(id, "tcp://localhost:1")));
        var a = matches.match(new MatchRequest("a", "first")).toCompletableFuture();
        var b = matches.match(new MatchRequest("b", "second")).toCompletableFuture();
        assertFalse(a.isDone());
        assertFalse(b.isDone());
        matches.close();
        assertTrue(a.isCompletedExceptionally());
        assertTrue(b.isCompletedExceptionally());
        assertThrows(
                IllegalStateException.class, () -> matches.match(new MatchRequest("c", "first")));
    }

    @Test
    void preparationFailureReachesBothPlayers() {
        try (var matches =
                new Matchmaker(
                        id ->
                                CompletableFuture.failedFuture(
                                        new IllegalStateException("Room rejected")))) {
            var first =
                    matches.match(new MatchRequest("a", Names.DEFAULT_MODE)).toCompletableFuture();
            var second =
                    matches.match(new MatchRequest("b", Names.DEFAULT_MODE)).toCompletableFuture();
            assertTrue(first.isCompletedExceptionally());
            assertTrue(second.isCompletedExceptionally());
            assertEquals(
                    "Room rejected",
                    assertThrows(CompletionException.class, first::join).getCause().getMessage());
        }
    }

    @Test
    void invalidRequestNeverEntersQueue() {
        try (var matches =
                new Matchmaker(
                        id -> {
                            throw new AssertionError("No pair expected");
                        })) {
            assertThrows(IllegalArgumentException.class, () -> matches.match(null));
            assertThrows(
                    IllegalArgumentException.class,
                    () -> matches.match(new MatchRequest("", Names.DEFAULT_MODE)));
            assertThrows(
                    IllegalArgumentException.class, () -> matches.match(new MatchRequest("a", "")));
        }
    }
}
