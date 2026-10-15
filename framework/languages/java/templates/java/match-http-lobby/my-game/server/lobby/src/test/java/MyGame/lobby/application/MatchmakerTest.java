package MyGame.lobby.application;

import static org.junit.jupiter.api.Assertions.*;

import MyGame.*;

import org.junit.jupiter.api.Test;

import java.util.concurrent.*;

public class MatchmakerTest {
    private static final int HTTP_TIMEOUT_MILLIS = 5000;

    @Test
    void cancellationDoesNotChangePairedRequests() {
        var prepared = new CompletableFuture<MatchResponse>();
        try (var matches = new Matchmaker(id -> prepared)) {
            var request = new MatchRequest("a", Names.DEFAULT_MODE);
            var first = matches.match(request).toCompletableFuture();
            var second =
                    matches.match(new MatchRequest("b", Names.DEFAULT_MODE)).toCompletableFuture();
            assertFalse(matches.cancelWaiting(request, first));
            var assignment = new MatchResponse(Names.ROOM_PREFIX + "1", "tcp://localhost:1");
            prepared.complete(assignment);
            assertEquals(assignment, first.join());
            assertEquals(assignment, second.join());
        }
    }

    @Test
    void staleCancellationDoesNotRemoveReplacement() {
        try (var matches =
                new Matchmaker(
                        id ->
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
        }
    }

    @Test
    void disconnectedWaiterCanRequestAgainAndMatch() throws Exception {
        var config =
                new NodeConfig(
                        "lobby", "", "", "", null, "http://127.0.0.1:0", "tcp://localhost:1");
        try (var matches =
                        new Matchmaker(
                                id ->
                                        CompletableFuture.completedFuture(
                                                new MatchResponse(id, config.sessionEndpoint())));
                var lobby = new MyGame.lobby.infrastructure.HttpLobby(config, matches)) {
            var uri = java.net.URI.create(lobby.endpoint());
            try (var abandoned = request(uri, "a")) {
                abandoned.shutdownOutput();
                assertEquals(
                        -1,
                        abandoned.getInputStream().read(),
                        "Server must close the disconnected waiter's connection");
            }
            try (var first = request(uri, "a");
                    var second = request(uri, "b")) {
                var a =
                        new String(
                                first.getInputStream().readAllBytes(),
                                java.nio.charset.StandardCharsets.UTF_8);
                var b =
                        new String(
                                second.getInputStream().readAllBytes(),
                                java.nio.charset.StandardCharsets.UTF_8);
                assertTrue(a.startsWith("HTTP/1.1 200"), a);
                assertTrue(b.startsWith("HTTP/1.1 200"), b);
                assertEquals(
                        Json.MAPPER.readTree(a.substring(a.indexOf("\r\n\r\n") + 4)),
                        Json.MAPPER.readTree(b.substring(b.indexOf("\r\n\r\n") + 4)));
                assertTrue(a.contains(Names.ROOM_PREFIX + "1"), a);
            }
        }
    }

    private static java.net.Socket request(java.net.URI uri, String player) throws Exception {
        var socket = new java.net.Socket(uri.getHost(), uri.getPort());
        socket.setSoTimeout(HTTP_TIMEOUT_MILLIS);
        var body = Json.MAPPER.writeValueAsBytes(new MatchRequest(player, Names.DEFAULT_MODE));
        var headers =
                "POST "
                        + Names.MATCH_PATH
                        + " HTTP/1.1\r\nHost: localhost\r\nContent-Length: "
                        + body.length
                        + "\r\nConnection: close\r\n\r\n";
        socket.getOutputStream()
                .write(headers.getBytes(java.nio.charset.StandardCharsets.US_ASCII));
        socket.getOutputStream().write(body);
        socket.getOutputStream().flush();
        return socket;
    }

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
