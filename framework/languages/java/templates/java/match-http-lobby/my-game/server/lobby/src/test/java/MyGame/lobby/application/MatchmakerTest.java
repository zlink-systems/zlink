package MyGame.lobby.application;

import static org.junit.jupiter.api.Assertions.*;

import MyGame.*;

import org.junit.jupiter.api.Test;

import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicReference;

public class MatchmakerTest {
    private static final int HTTP_TIMEOUT_MILLIS = 5000;
    private static final String ROOM_ID = "room-[0-9a-f]{32}";

    @Test
    void cancellationDoesNotChangePairedRequests() {
        var prepared = new CompletableFuture<MatchResponse>();
        var preparedId = new AtomicReference<String>();
        try (var matches =
                new Matchmaker(
                        (id, members) -> {
                            assertTrue(id.matches(ROOM_ID), id);
                            preparedId.set(id);
                            return prepared;
                        })) {
            var request = new MatchRequest("a", Names.DEFAULT_MODE);
            var first = matches.match(request).toCompletableFuture();
            var second =
                    matches.match(new MatchRequest("b", Names.DEFAULT_MODE)).toCompletableFuture();
            assertFalse(matches.cancelWaiting(request, first));
            assertFalse(first.isDone());
            assertFalse(second.isDone());
            var assignment = new MatchResponse(preparedId.get(), "tcp://localhost:1");
            prepared.complete(assignment);
            assertEquals(assignment, first.join());
            assertEquals(assignment, second.join());
        }
    }

    @Test
    void staleCancellationDoesNotRemoveReplacement() {
        try (var matches =
                new Matchmaker(
                        (id, members) ->
                                CompletableFuture.completedFuture(
                                        new MatchResponse(id, "tcp://localhost:1")))) {
            var request = new MatchRequest("a", Names.DEFAULT_MODE);
            var first = matches.match(request).toCompletableFuture();
            assertTrue(matches.cancelWaiting(request, first));
            assertTrue(first.isCancelled());
            var replacement = matches.match(request).toCompletableFuture();
            assertFalse(matches.cancelWaiting(request, first));
            assertFalse(replacement.isDone());
            var second =
                    matches.match(new MatchRequest("b", Names.DEFAULT_MODE)).toCompletableFuture();
            assertEquals(replacement.join(), second.join());
            assertTrue(replacement.join().roomId().matches(ROOM_ID));
        }
    }

    @Test
    void disconnectedWaiterCanRequestAgainAndMatch() throws Exception {
        var config =
                new NodeConfig(
                        "lobby",
                        "",
                        "",
                        "",
                        null,
                        "http://127.0.0.1:0",
                        "tcp://localhost:1",
                        Integer.MAX_VALUE);
        try (var matches =
                        new Matchmaker(
                                (id, members) ->
                                        CompletableFuture.completedFuture(
                                                new MatchResponse(id, config.sessionEndpoint())));
                var lobby = new MyGame.lobby.infrastructure.HttpLobby(config, matches)) {
            var uri = java.net.URI.create(lobby.endpoint());
            try (var abandoned = request(uri, "a", false)) {
                abandoned.shutdownOutput();
                assertEquals(
                        -1,
                        abandoned.getInputStream().read(),
                        "Server must close the disconnected waiter's connection");
            }
            try (var first = request(uri, "a", true);
                    var second = request(uri, "b", false)) {
                var a =
                        new String(first.getInputStream().readAllBytes(), StandardCharsets.UTF_8);
                var b =
                        new String(second.getInputStream().readAllBytes(), StandardCharsets.UTF_8);
                assertTrue(a.startsWith("HTTP/1.1 200"), a);
                assertTrue(b.startsWith("HTTP/1.1 200"), b);
                var responseA = Json.MAPPER.readTree(a.substring(a.indexOf("\r\n\r\n") + 4));
                var responseB = Json.MAPPER.readTree(b.substring(b.indexOf("\r\n\r\n") + 4));
                assertEquals(responseA, responseB);
                assertTrue(responseA.path("roomId").asText().matches(ROOM_ID), responseA.toString());
            }
        }
    }

    private static java.net.Socket request(java.net.URI uri, String player, boolean omitMode)
            throws Exception {
        var socket = new java.net.Socket(uri.getHost(), uri.getPort());
        socket.setSoTimeout(HTTP_TIMEOUT_MILLIS);
        byte[] body =
                omitMode
                        ? Json.MAPPER.writeValueAsBytes(java.util.Map.of("playerId", player))
                        : Json.MAPPER.writeValueAsBytes(new MatchRequest(player, Names.DEFAULT_MODE));
        var headers =
                "POST "
                        + Names.MATCH_PATH
                        + " HTTP/1.1\r\nHost: localhost\r\nContent-Length: "
                        + body.length
                        + "\r\nConnection: close\r\n\r\n";
        socket.getOutputStream().write(headers.getBytes(StandardCharsets.US_ASCII));
        socket.getOutputStream().write(body);
        socket.getOutputStream().flush();
        return socket;
    }

    @Test
    void bothResponsesWaitForPreparedRoomAndCarryMatchedMembers() {
        var prepared = new CompletableFuture<MatchResponse>();
        var preparedId = new AtomicReference<String>();
        try (var matches =
                new Matchmaker(
                        (id, members) -> {
                            assertTrue(id.matches(ROOM_ID), id);
                            assertEquals(List.of("a", "b"), members);
                            preparedId.set(id);
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
            var assignment = new MatchResponse(preparedId.get(), "tcp://localhost:1");
            prepared.complete(assignment);
            assertEquals(assignment, first.join());
            assertEquals(assignment, second.join());
        }
    }

    @Test
    void omittedModePairsWithExplicitDuelAndRoomIdsAreFresh() {
        var ids = new CopyOnWriteArrayList<String>();
        try (var matches =
                new Matchmaker(
                        (id, members) -> {
                            ids.add(id);
                            return CompletableFuture.completedFuture(
                                    new MatchResponse(id, "tcp://localhost:1"));
                        })) {
            var first = matches.match(new MatchRequest("a", null)).toCompletableFuture();
            var second =
                    matches.match(new MatchRequest("b", Names.DEFAULT_MODE)).toCompletableFuture();
            assertEquals(first.join(), second.join());
            assertTrue(first.join().roomId().matches(ROOM_ID));

            var third =
                    matches.match(new MatchRequest("c", Names.DEFAULT_MODE)).toCompletableFuture();
            var fourth =
                    matches.match(new MatchRequest("d", Names.DEFAULT_MODE)).toCompletableFuture();
            assertEquals(third.join(), fourth.join());
            assertTrue(third.join().roomId().matches(ROOM_ID));
            assertNotEquals(first.join().roomId(), third.join().roomId());
            assertEquals(2, ids.size());
        }
    }

    @Test
    void modesStaySeparateAndShutdownFailsWaiters() {
        var matches =
                new Matchmaker(
                        (id, members) ->
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
                        (id, members) ->
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
                        (id, members) -> {
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
