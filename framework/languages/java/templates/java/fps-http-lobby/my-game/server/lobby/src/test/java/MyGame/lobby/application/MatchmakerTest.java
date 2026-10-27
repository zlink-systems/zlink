package MyGame.lobby.application;

import static org.junit.jupiter.api.Assertions.*;

import MyGame.*;
import MyGame.lobby.infrastructure.HttpLobby;

import org.junit.jupiter.api.Test;

import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.UUID;
import java.util.concurrent.*;

final class MatchmakerTest {
    private static final int HTTP_TIMEOUT_MILLIS = 5000;
    private static final String SESSION_ENDPOINT = "tcp://localhost:1";

    @Test
    void cancellationDoesNotChangePairedRequests() {
        var prepared = new CompletableFuture<MatchResponse>();
        try (var matches = new Matchmaker((roomId, members) -> prepared)) {
            var request = new MatchRequest("a", Names.DEFAULT_MODE);
            var first = matches.match(request).toCompletableFuture();
            var second = matches.match(new MatchRequest("b")).toCompletableFuture();
            assertFalse(matches.cancelWaiting(request, first));
            var assignment = new MatchResponse("room-0123456789abcdef0123456789abcdef", SESSION_ENDPOINT);
            prepared.complete(assignment);
            assertEquals(assignment, first.join());
            assertEquals(assignment, second.join());
        }
    }

    @Test
    void staleCancellationDoesNotRemoveReplacement() {
        try (var matches = new Matchmaker((roomId, members) ->
                CompletableFuture.completedFuture(new MatchResponse(roomId, SESSION_ENDPOINT)))) {
            var request = new MatchRequest("a");
            var first = matches.match(request).toCompletableFuture();
            assertTrue(matches.cancelWaiting(request, first));
            assertTrue(first.isCancelled());
            var replacement = matches.match(request).toCompletableFuture();
            assertFalse(matches.cancelWaiting(request, first));
            assertFalse(replacement.isDone());
            var second = matches.match(new MatchRequest("b")).toCompletableFuture();
            assertEquals(replacement.join(), second.join());
        }
    }

    @Test
    void disconnectedWaiterCanRequestAgainAndMatch() throws Exception {
        var config =
                new NodeConfig(
                        "lobby", "", "", "", null, "http://127.0.0.1:0", SESSION_ENDPOINT, Integer.MAX_VALUE);
        try (var matches = new Matchmaker((roomId, members) ->
                        CompletableFuture.completedFuture(new MatchResponse(roomId, config.sessionEndpoint())));
                var lobby = new HttpLobby(config, matches)) {
            var uri = URI.create(lobby.endpoint());
            try (var abandoned = request(uri, "a")) {
                abandoned.shutdownOutput();
                assertEquals(-1, abandoned.getInputStream().read(), "Server must close the disconnected waiter's connection");
            }
            try (var first = request(uri, "a"); var second = request(uri, "b")) {
                var a = new String(first.getInputStream().readAllBytes(), StandardCharsets.UTF_8);
                var b = new String(second.getInputStream().readAllBytes(), StandardCharsets.UTF_8);
                assertTrue(a.startsWith("HTTP/1.1 200"), a);
                assertTrue(b.startsWith("HTTP/1.1 200"), b);
                assertEquals(
                        Json.MAPPER.readTree(a.substring(a.indexOf("\r\n\r\n") + 4)),
                        Json.MAPPER.readTree(b.substring(b.indexOf("\r\n\r\n") + 4)));
                assertTrue(a.contains(Names.ROOM_PREFIX), a);
            }
        }
    }

    @Test
    void bothResponsesWaitForPreparedRoom() {
        var prepared = new CompletableFuture<MatchResponse>();
        String[] generatedRoomId = {null};
        try (var matches = new Matchmaker((roomId, members) -> {
            generatedRoomId[0] = roomId;
            assertEquals(2, members.size());
            return prepared;
        })) {
            var first = matches.match(new MatchRequest("a")).toCompletableFuture();
            assertFalse(first.isDone());
            assertThrows(IllegalArgumentException.class, () -> matches.match(new MatchRequest("a")));
            var second = matches.match(new MatchRequest("b")).toCompletableFuture();
            assertFalse(first.isDone());
            assertFalse(second.isDone());
            assertNotNull(generatedRoomId[0]);
            var assignment = new MatchResponse(generatedRoomId[0], SESSION_ENDPOINT);
            prepared.complete(assignment);
            assertEquals(assignment, first.join());
            assertEquals(assignment, second.join());
        }
    }

    @Test
    void modesStaySeparateAndShutdownFailsWaiters() {
        var matches = new Matchmaker((roomId, members) ->
                CompletableFuture.completedFuture(new MatchResponse(roomId, SESSION_ENDPOINT)));
        var a = matches.match(new MatchRequest("a", "first")).toCompletableFuture();
        var b = matches.match(new MatchRequest("b", "second")).toCompletableFuture();
        assertFalse(a.isDone());
        assertFalse(b.isDone());
        matches.close();
        assertTrue(a.isCompletedExceptionally());
        assertTrue(b.isCompletedExceptionally());
        assertThrows(IllegalStateException.class, () -> matches.match(new MatchRequest("c", "first")));
    }

    @Test
    void preparationFailureReachesBothPlayers() {
        try (var matches = new Matchmaker((roomId, members) ->
                CompletableFuture.failedFuture(new IllegalStateException("Room rejected")))) {
            var first = matches.match(new MatchRequest("a")).toCompletableFuture();
            var second = matches.match(new MatchRequest("b")).toCompletableFuture();
            assertTrue(first.isCompletedExceptionally());
            assertTrue(second.isCompletedExceptionally());
            assertEquals(
                    "Room rejected",
                    assertThrows(CompletionException.class, first::join).getCause().getMessage());
        }
    }

    @Test
    void invalidRequestNeverEntersQueue() {
        try (var matches = new Matchmaker((roomId, members) -> {
            throw new AssertionError("No pair expected");
        })) {
            assertThrows(IllegalArgumentException.class, () -> matches.match(null));
            assertThrows(IllegalArgumentException.class, () -> matches.match(new MatchRequest("", Names.DEFAULT_MODE)));
            assertThrows(IllegalArgumentException.class, () -> matches.match(new MatchRequest("a", "")));
        }
    }

    private static java.net.Socket request(URI uri, String player) throws Exception {
        var socket = new java.net.Socket(uri.getHost(), uri.getPort());
        socket.setSoTimeout(HTTP_TIMEOUT_MILLIS);
        var body = Json.MAPPER.writeValueAsBytes(new MatchRequest(player));
        var headers =
                "POST " + Names.MATCH_PATH + " HTTP/1.1\r\nHost: localhost\r\nContent-Length: "
                        + body.length + "\r\nConnection: close\r\n\r\n";
        socket.getOutputStream().write(headers.getBytes(StandardCharsets.US_ASCII));
        socket.getOutputStream().write(body);
        socket.getOutputStream().flush();
        return socket;
    }
}
