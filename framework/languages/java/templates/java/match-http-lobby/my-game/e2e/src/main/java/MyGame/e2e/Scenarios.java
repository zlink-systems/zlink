package MyGame.e2e;

import MyGame.*;

import java.net.*;
import java.net.http.*;
import java.time.Duration;
import java.util.*;

public final class Scenarios {
    private Scenarios() {}

    private static final double NEAR_X = 10,
            TARGET_X = 20,
            BEHIND_WALL_X = 60,
            FAR_X = 200,
            LANE_Y = 10;

    static void run(Scenario scenario, Nodes nodes) throws Exception {
        String suffix = UUID.randomUUID().toString();
        var assignment = match(nodes, "p1-" + suffix, "p2-" + suffix);
        try (var a = new Client(assignment.sessionEndpoint(), "p1-" + suffix, assignment.roomId());
                var b =
                        new Client(
                                assignment.sessionEndpoint(),
                                "p2-" + suffix,
                                assignment.roomId())) {
            a.connect();
            var initial = b.connect();
            Structure.require(
                    initial.players().size() == Rules.PLAYERS, "Incorrect initial player count");
            Structure.require(
                    initial.equals(
                            a.next(RoomView.class, v -> v.players().size() == Rules.PLAYERS)),
                    "Room views differ");
            Structure.require(
                    initial.roomId().equals(assignment.roomId())
                            && initial.players().stream().allMatch(p -> p.hp() == Rules.INITIAL_HP),
                    "Incorrect initial room");
            if (scenario == Scenario.LOBBY) {
                connected(a, b);
                return;
            }
            var identityA = a.sessionInfo();
            var identityB = b.sessionInfo();
            move(a, b, a, NEAR_X, LANE_Y, 1);
            move(a, b, b, TARGET_X, LANE_Y, 1);
            var before = hit(a, b, 2, Rules.INITIAL_HP - Rules.DAMAGE);
            switch (scenario) {
                case HIT -> {
                    move(a, b, b, BEHIND_WALL_X, LANE_Y, 2);
                    reject(a, b, 3, Rules.BLOCKED, b.playerId);
                    move(a, b, b, FAR_X, LANE_Y, 3);
                    reject(a, b, 4, Rules.OUT_OF_RANGE, b.playerId);
                    reject(a, b, 5, Rules.NO_TARGET, "missing-player");
                    move(a, b, b, TARGET_X, LANE_Y, 4);
                    for (long sequence = 6; sequence <= 8; sequence++)
                        hit(a, b, sequence, Rules.INITIAL_HP - Rules.DAMAGE * (int) (sequence - 4));
                    Structure.require(
                            a.next(MatchEnded.class).winner().equals(a.playerId)
                                    && b.next(MatchEnded.class).winner().equals(a.playerId),
                            "Wrong winner");
                }
                case RECONNECT -> {
                    b.close();
                    try (var again = new Client(nodes.endpoint, b.playerId, assignment.roomId())) {
                        sameState(before, again.connect());
                        var identity = again.sessionInfo();
                        Structure.require(
                                !identity.sessionId().equals(identityB.sessionId())
                                        && identity.actorId().equals(identityB.actorId())
                                        && identity.objectGeneration()
                                                == identityB.objectGeneration(),
                                "Actor identity changed");
                        a.clear();
                        again.clear();
                        hit(a, again, 3, Rules.INITIAL_HP - Rules.DAMAGE * 2);
                        connected(a, again);
                    }
                    return;
                }
                case UPDATE -> {
                    nodes.drain(before.nodeId());
                    Structure.require(
                            a.sessionInfo().equals(identityA) && b.sessionInfo().equals(identityB),
                            "Session binding changed");
                    sameState(before, view(a));
                    sameState(before, view(b));
                    Structure.require(
                            !view(a).nodeId().equals(before.nodeId()), "Room did not relocate");
                    nodes.replace(before.nodeId());
                    newMatch(nodes);
                    a.clear();
                    b.clear();
                    hit(a, b, 3, Rules.INITIAL_HP - Rules.DAMAGE * 2);
                    Structure.require(
                            a.sessionInfo().equals(identityA) && b.sessionInfo().equals(identityB),
                            "Replacement changed binding");
                }
                default -> throw new IllegalStateException("Unexpected scenario");
            }
            connected(a, b);
        }
    }

    private static MatchResponse match(Nodes nodes, String a, String b) throws Exception {
        try (var http = HttpClient.newHttpClient()) {
            var first = post(http, nodes.lobbyEndpoint, a);
            var second = post(http, nodes.lobbyEndpoint, b);
            var assignment = first.get();
            Structure.require(
                    assignment.equals(second.get())
                            && assignment.sessionEndpoint().equals(nodes.endpoint),
                    "HTTP assignment mismatch");
            return assignment;
        }
    }

    private static java.util.concurrent.CompletableFuture<MatchResponse> post(
            HttpClient http, String endpoint, String player) throws Exception {
        var request =
                HttpRequest.newBuilder(URI.create(endpoint + Names.MATCH_PATH))
                        .timeout(Duration.ofSeconds(Client.MESSAGE_SECONDS))
                        .header(HttpProtocol.CONTENT_TYPE, HttpProtocol.JSON)
                        .POST(
                                HttpRequest.BodyPublishers.ofByteArray(
                                        Json.MAPPER.writeValueAsBytes(
                                                new MatchRequest(player, Names.DEFAULT_MODE))))
                        .build();
        return http.sendAsync(request, HttpResponse.BodyHandlers.ofByteArray())
                .thenApply(
                        response -> {
                            Structure.require(
                                    response.statusCode() == HttpProtocol.OK,
                                    "HTTP status " + response.statusCode());
                            try {
                                return Json.MAPPER.readValue(response.body(), MatchResponse.class);
                            } catch (java.io.IOException error) {
                                throw new java.io.UncheckedIOException(error);
                            }
                        });
    }

    private static void newMatch(Nodes nodes) throws Exception {
        String suffix = UUID.randomUUID().toString(),
                pa = "new-a-" + suffix,
                pb = "new-b-" + suffix;
        var assignment = match(nodes, pa, pb);
        try (var a = new Client(assignment.sessionEndpoint(), pa, assignment.roomId());
                var b = new Client(assignment.sessionEndpoint(), pb, assignment.roomId())) {
            a.connect();
            var room = b.connect();
            Structure.require(
                    room.players().size() == Rules.PLAYERS, "Incorrect initial player count");
            Structure.require(
                    room.equals(a.next(RoomView.class, v -> v.players().size() == Rules.PLAYERS)),
                    "Room views differ");
            Structure.require(
                    room.roomId().equals(assignment.roomId()) && nodes.isLiveMatch(room.nodeId()),
                    "New Room is not on a live match node");
            connected(a, b);
        }
    }

    private static RoomView both(
            Client a, Client b, java.util.function.Predicate<RoomView> predicate) throws Exception {
        var view = a.next(RoomView.class, predicate);
        Structure.require(view.equals(b.next(RoomView.class, predicate)), "Room views differ");
        return view;
    }

    private static void move(Client a, Client b, Client mover, double x, double y, long sequence)
            throws Exception {
        a.clear();
        b.clear();
        mover.send(new MoveRequest(x, y, sequence));
        both(
                a,
                b,
                v ->
                        v.players().stream()
                                .anyMatch(
                                        p ->
                                                p.playerId().equals(mover.playerId)
                                                        && p.x() == x
                                                        && p.y() == y));
    }

    private static RoomView hit(Client a, Client b, long sequence, int hp) throws Exception {
        a.send(new ShootRequest(b.playerId, sequence));
        return both(
                a,
                b,
                v ->
                        v.players().stream()
                                .anyMatch(p -> p.playerId().equals(b.playerId) && p.hp() == hp));
    }

    private static RoomView view(Client a) throws Exception {
        a.clear();
        a.send(new RoomViewRequest());
        return a.next(RoomView.class);
    }

    private static void reject(Client a, Client b, long sequence, String reason, String target)
            throws Exception {
        var before = view(b);
        a.send(new ShootRequest(target, sequence));
        Structure.require(
                a.next(ShotRejected.class).reason().equals(reason), "Wrong shot rejection");
        sameState(before, view(b));
    }

    private static void sameState(RoomView a, RoomView b) {
        Structure.require(
                a.roomId().equals(b.roomId()) && a.players().equals(b.players()),
                "Room state changed");
    }

    private static void connected(Client a, Client b) {
        Structure.require(
                a.connector.isConnected()
                        && b.connector.isConnected()
                        && a.disconnects.get() == 0
                        && b.disconnects.get() == 0,
                "Client disconnected");
    }
}
