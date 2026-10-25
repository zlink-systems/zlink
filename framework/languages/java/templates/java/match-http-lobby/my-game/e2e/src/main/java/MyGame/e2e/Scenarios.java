package MyGame.e2e;

import MyGame.*;

import java.net.*;
import java.net.http.*;
import java.time.Duration;
import java.util.*;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.TimeUnit;

public final class Scenarios {
    private Scenarios() {}
    private static final double WALL_CENTER_Y = (Rules.WALL_MIN_Y + Rules.WALL_MAX_Y) / 2;

    static void run(Scenario scenario, Nodes nodes) throws Exception {
        String suffix = UUID.randomUUID().toString();
        var assignment =
                match(
                        nodes.lobbyEndpoint,
                        "p1-" + suffix,
                        "p2-" + suffix,
                        scenario == Scenario.LOBBY);
        Structure.require(
                assignment.roomId().matches("room-[0-9a-f]{32}"), "Invalid Room ID format");
        Structure.require(
                assignment.sessionEndpoint().equals(nodes.endpoint),
                "HTTP assignment has the wrong Session endpoint");
        try (var a = new Client(nodes.endpoint, "p1-" + suffix, assignment.roomId());
                var b = new Client(nodes.endpoint, "p2-" + suffix, assignment.roomId())) {
            if (scenario == Scenario.LOBBY) {
                try (var outsider =
                        new Client(nodes.endpoint, "outsider-" + suffix, assignment.roomId())) {
                    outsider.beginJoin();
                    var rejected = outsider.next(ActionRejected.class);
                    Structure.require(
                            rejected.reason().equals(Rules.ADMISSION_REJECTED)
                                    && rejected.retryAfterMs() == 0,
                            "Empty prepared Room admitted an outsider");
                }
            }
            a.connect();
            var second = b.connect();
            var initial = a.next(RoomView.class, view -> view.players().size() == Rules.PLAYERS);
            Structure.require(initial.equals(second), "Room views differ");
            Structure.require(
                    initial.roomId().equals(assignment.roomId())
                            && initial.players().stream().allMatch(p -> p.hp() == Rules.INITIAL_HP)
                            && initial.players().stream().allMatch(p -> p.lastSequence() == 0)
                            && initial.phase().equals(Rules.PLAYING)
                            && initial.winner() == null,
                    "Incorrect initial Room view");
            Structure.require(
                    initial.players().get(0).x() == Rules.SPAWN_AX
                            && initial.players().get(0).y() == Rules.SPAWN_AY
                            && initial.players().get(1).x() == Rules.SPAWN_BX
                            && initial.players().get(1).y() == Rules.SPAWN_BY,
                    "Server spawn positions differ");

            if (scenario == Scenario.LOBBY) {
                lobby(a, b, nodes, assignment.roomId(), suffix);
                connected(a, b);
                return;
            }

            var identityA = a.sessionInfo();
            var identityB = b.sessionInfo();
            switch (scenario) {
                case HIT -> hitScenario(a, b);
                case WALL -> wallScenario(a, b);
                case INPUT -> inputScenario(a, b);
                case RECONNECT -> reconnectScenario(a, b, identityB, nodes.endpoint);
                case UPDATE -> updateScenario(a, b, nodes, assignment.roomId(), identityA, identityB);
                default -> throw new IllegalStateException("Unexpected scenario");
            }
            if (scenario != Scenario.RECONNECT) connected(a, b);
        }
    }

    private static void lobby(Client a, Client b, Nodes nodes, String roomId, String suffix)
            throws Exception {
        var before = view(b);
        a.clear();
        a.send(new PrepareRoom(roomId, List.of(a.playerId, b.playerId)));
        var packetRejected = a.next(ActionRejected.class);
        Structure.require(
                packetRejected.reason().equals(Rules.PACKET_NOT_ALLOWED)
                        && packetRejected.retryAfterMs() == 0,
                "Client PrepareRoom was not rejected");
        sameState(before, view(b));

        String endpoint = nodes.restartLobby();
        String newSuffix = UUID.randomUUID().toString();
        var next =
                match(
                        endpoint,
                        "after-restart-a-" + newSuffix,
                        "after-restart-b-" + newSuffix,
                        false);
        Structure.require(!next.roomId().equals(roomId), "Lobby restart reused the Room ID");
        try (var nextA =
                        new Client(nodes.endpoint, "after-restart-a-" + newSuffix, next.roomId());
                var nextB =
                        new Client(nodes.endpoint, "after-restart-b-" + newSuffix, next.roomId())) {
            nextA.connect();
            var nextView = nextB.connect();
            Structure.require(
                    nextView.roomId().equals(next.roomId())
                            && nextView.phase().equals(Rules.PLAYING),
                    "New Room after Lobby restart did not admit its members");
        }
    }

    private static void hitScenario(Client a, Client b) throws Exception {
        hit(a, b, 1, Rules.INITIAL_HP - Rules.DAMAGE);
        hit(a, b, 2, Rules.INITIAL_HP - Rules.DAMAGE * 2);
        moveTo(a, b, b, Rules.MAP_MAX, Rules.MAP_MAX - 10);
        rejectShot(a, b, 3, Rules.OUT_OF_RANGE, b.playerId);
        rejectShot(a, b, 3, Rules.NO_TARGET, "missing-player");
        moveTo(a, b, b, Rules.SPAWN_BX, Rules.SPAWN_BY);
        hit(a, b, 3, Rules.INITIAL_HP - Rules.DAMAGE * 3);
        var finished = hit(a, b, 4, 0);
        Structure.require(
                finished.phase().equals(Rules.FINISHED_PHASE)
                        && a.playerId.equals(finished.winner()),
                "Wrong winner or phase");
        Structure.require(
                a.next(MatchEnded.class).winner().equals(a.playerId)
                        && b.next(MatchEnded.class).winner().equals(a.playerId),
                "Winner event differs");
    }

    private static void wallScenario(Client a, Client b) throws Exception {
        double centerY = (Rules.WALL_MIN_Y + Rules.WALL_MAX_Y) / 2;
        moveTo(a, b, a, Rules.WALL_X - 2, centerY);
        moveTo(a, b, b, Rules.WALL_X + 2, centerY);
        var before = view(a);
        var targetBefore = player(before, b.playerId);
        rejectShot(a, b, player(before, a.playerId).lastSequence() + 1, Rules.BLOCKED, b.playerId);
        rejectShot(b, a, player(before, b.playerId).lastSequence() + 1, Rules.BLOCKED, a.playerId);
        var after = view(a);
        sameState(before, after);
        Structure.require(player(after, b.playerId).hp() == targetBefore.hp(),
                "Wall-blocked shots changed HP");

        moveTo(a, b, a, Rules.SPAWN_AX, Rules.SPAWN_AY);
        moveTo(a, b, b, Rules.SPAWN_BX, Rules.SPAWN_BY);
        var expectedHp = player(view(a), b.playerId).hp() - Rules.DAMAGE;
        var clearShot = hit(a, b, player(view(a), a.playerId).lastSequence() + 1, expectedHp);
        Structure.require(player(clearShot, b.playerId).hp() == expectedHp,
                "Clear shot behind the wall was rejected");
    }

    private static void inputScenario(Client p1, Client p2) throws Exception {
        var beforeMove = view(p1);
        var firstSpawn = beforeMove.players().stream()
                .filter(candidate -> candidate.x() == Rules.SPAWN_AX
                        && candidate.y() == Rules.SPAWN_AY)
                .findFirst()
                .orElseThrow();
        var mover = firstSpawn.playerId().equals(p1.playerId) ? p1 : p2;
        var other = mover == p1 ? p2 : p1;
        var source = player(beforeMove, mover.playerId);
        var target = player(beforeMove, other.playerId);
        var moveDelta = target.x() >= source.x() ? Rules.MAX_STEP : -Rules.MAX_STEP;
        var moveSequence = source.lastSequence() + 1;
        var acceptedMove = new MoveRequest(moveDelta, 0, moveSequence);
        var rejectedMove = new MoveRequest(moveDelta, 0, moveSequence + 1);
        mover.send(acceptedMove);
        mover.send(rejectedMove);
        var moveResult = mover.next(MoveResult.class);
        var moveRejection = mover.next(ActionRejected.class);
        Structure.require(moveResult.sequence() == moveSequence, "Accepted move result sequence differs");
        Structure.require(moveRejection.reason().equals(Rules.MOVE_BUDGET) && moveRejection.retryAfterMs() > 0,
                "Expected movement-budget rejection");
        var afterMoveA = view(mover);
        var afterMover = player(afterMoveA, mover.playerId);
        Structure.require(afterMover.x() == moveResult.x() && afterMover.y() == moveResult.y()
                        && afterMover.lastSequence() == moveSequence
                        && player(afterMoveA, other.playerId).hp() == target.hp(),
                "Movement-budget rejection changed the accepted movement state");
        Thread.sleep(moveRejection.retryAfterMs());
        mover.send(rejectedMove);
        var retriedMove = mover.next(MoveResult.class);
        Structure.require(retriedMove.sequence() == rejectedMove.sequence(), "Server movement retry delay was insufficient");
        var afterMoveB = view(mover);
        var afterRetryMove = player(afterMoveB, mover.playerId);
        Structure.require(afterRetryMove.lastSequence() == rejectedMove.sequence()
                        && afterRetryMove.x() == retriedMove.x()
                        && afterRetryMove.y() == retriedMove.y(),
                "Retried movement result differs from RoomView");

        var beforeShot = view(p1);
        var shotTarget = player(beforeShot, p2.playerId);
        var shotSequence = player(beforeShot, p1.playerId).lastSequence() + 1;
        var acceptedShot = new ShootRequest(p2.playerId, shotSequence);
        var rejectedShot = new ShootRequest(p2.playerId, shotSequence + 1);
        p1.send(acceptedShot);
        p1.send(rejectedShot);
        var shotResult = p1.next(ShotResult.class);
        var fireRejection = p1.next(ActionRejected.class);
        Structure.require(shotResult.sequence() == shotSequence && shotResult.outcome().equals("hit"),
                "Expected accepted hit before fire-too-soon rejection");
        Structure.require(fireRejection.reason().equals(Rules.FIRE_TOO_SOON) && fireRejection.retryAfterMs() > 0,
                "Expected fire-interval rejection");
        var afterShotA = view(p1);
        Structure.require(player(afterShotA, p2.playerId).hp() == shotTarget.hp() - Rules.DAMAGE
                        && player(afterShotA, p1.playerId).lastSequence() == shotSequence,
                "Fire-too-soon rejection changed the accepted shot state");
        Thread.sleep(fireRejection.retryAfterMs());
        p1.send(rejectedShot);
        var retriedShot = p1.next(ShotResult.class);
        Structure.require(retriedShot.sequence() == rejectedShot.sequence(), "Server fire retry delay was insufficient");
        var afterShotB = view(p1);
        Structure.require(player(afterShotB, p2.playerId).hp() == shotTarget.hp() - 2 * Rules.DAMAGE
                        && player(afterShotB, p1.playerId).lastSequence() == rejectedShot.sequence(),
                "Retried shot did not update the target state");

        long staleSequence = player(view(mover), mover.playerId).lastSequence();
        rejectAction(mover, new MoveRequest(0, 0, staleSequence), Rules.STALE_SEQUENCE);
        rejectAction(mover, new MoveRequest(0, 0, moveSequence), Rules.STALE_SEQUENCE);
        var stepTooLargeSequence = staleSequence + 1;
        rejectAction(mover,
                new MoveRequest(Rules.MAX_STEP + 0.01, 0, stepTooLargeSequence),
                Rules.STEP_TOO_LARGE);
        rejectAction(mover,
                new MoveRequest(0, 0, Rules.MAX_SAFE_INTEGER + 1),
                Rules.INTEGER_OUT_OF_RANGE);

        var outsideStart = moveTo(p1, p2, mover, 1, Rules.SPAWN_AY);
        var outsideSequence = player(outsideStart, mover.playerId).lastSequence() + 1;
        rejectAction(mover, new MoveRequest(-Rules.MAX_STEP, 0, outsideSequence), Rules.OUTSIDE_MAP);
        var beforeWall = moveTo(p1, p2, mover, Rules.WALL_X - Rules.MAX_STEP, WALL_CENTER_Y);
        var wallSequence = player(beforeWall, mover.playerId).lastSequence() + 1;
        rejectAction(mover, new MoveRequest(Rules.MAX_STEP, 0, wallSequence), Rules.MOVE_BLOCKED_BY_WALL);
        move(p1, p2, mover, -0.1, 0, Rules.MAX_SAFE_INTEGER);
        Structure.require(player(view(mover), mover.playerId).lastSequence() == Rules.MAX_SAFE_INTEGER,
                "Maximum safe sequence was not accepted");
        rejectAction(mover,
                new MoveRequest(0, 0, Rules.MAX_SAFE_INTEGER + 1),
                Rules.INTEGER_OUT_OF_RANGE);
    }

    private static void reconnectScenario(
            Client a, Client b, SessionInfo identityB, String endpoint)
            throws Exception {
        var beforeReconnect = hit(a, b, 1, Rules.INITIAL_HP - Rules.DAMAGE);
        b.close();
        try (var again = new Client(endpoint, b.playerId, b.roomId)) {
            var restoredPlaying = again.connect();
            sameState(beforeReconnect, restoredPlaying);
            var identity = again.sessionInfo();
            Structure.require(
                    !identity.sessionId().equals(identityB.sessionId())
                            && identity.actorId().equals(identityB.actorId())
                            && identity.objectGeneration() == identityB.objectGeneration(),
                    "Actor identity changed");
            a.clear();
            again.clear();
            hit(a, again, 2, Rules.INITIAL_HP - Rules.DAMAGE * 2);
            connected(a, again);

            var finished = hit(a, again, 3, Rules.INITIAL_HP - Rules.DAMAGE * 3);
            finished = hit(a, again, 4, 0);
            Structure.require(
                    finished.phase().equals(Rules.FINISHED_PHASE)
                            && a.playerId.equals(finished.winner()),
                    "Match did not finish before reconnect");
            var finishedIdentity = again.sessionInfo();
            again.close();
            try (var afterFinish = new Client(endpoint, b.playerId, b.roomId)) {
                var restoredFinished = afterFinish.connect();
                sameState(finished, restoredFinished);
                var identityAfterFinish = afterFinish.sessionInfo();
                Structure.require(
                        !identityAfterFinish.sessionId().equals(finishedIdentity.sessionId())
                                && identityAfterFinish.actorId().equals(identityB.actorId())
                                && identityAfterFinish.objectGeneration()
                                        == identityB.objectGeneration(),
                        "Finished Actor identity changed on reconnect");
                connected(a, afterFinish);
            }
        }
    }

    private static void updateScenario(
            Client a,
            Client b,
            Nodes nodes,
            String roomId,
            SessionInfo identityA,
            SessionInfo identityB)
            throws Exception {
        var before = hit(a, b, 1, Rules.INITIAL_HP - Rules.DAMAGE);
        nodes.drain(before.nodeId());
        Structure.require(
                a.sessionInfo().equals(identityA) && b.sessionInfo().equals(identityB),
                "Session binding changed");
        sameState(before, view(a));
        sameState(before, view(b));
        var relocated = view(a);
        Structure.require(!relocated.nodeId().equals(before.nodeId()), "Room did not relocate");
        try (var outsider =
                new Client(nodes.endpoint, "relocated-outsider-" + UUID.randomUUID(), roomId)) {
            outsider.beginJoin();
            var rejected = outsider.next(ActionRejected.class);
            Structure.require(
                    rejected.reason().equals(Rules.ADMISSION_REJECTED)
                            && rejected.retryAfterMs() == 0,
                    "Relocated roster did not reject outsider");
        }
        nodes.replace(before.nodeId());
        newMatch(nodes, before.nodeId());
        a.clear();
        b.clear();
        var current = player(view(a), a.playerId);
        var after = hit(a, b, current.lastSequence() + 1, Rules.INITIAL_HP - Rules.DAMAGE * 2);
        Structure.require(
                after.phase().equals(Rules.PLAYING)
                        && after.winner() == null
                        && a.sessionInfo().equals(identityA)
                        && b.sessionInfo().equals(identityB),
                "Replacement changed game state or Session binding");
    }

    private static MatchResponse match(String endpoint, String first, String second, boolean omitMode)
            throws Exception {
        try (var http = HttpClient.newHttpClient()) {
            var a = post(http, endpoint, first, omitMode);
            var b = post(http, endpoint, second, omitMode);
            var responseA = a.join();
            var responseB = b.join();
            Structure.require(responseA.equals(responseB), "HTTP assignments differ");
            return responseA;
        }
    }

    private static CompletableFuture<MatchResponse> post(
            HttpClient http, String endpoint, String player, boolean omitMode) throws Exception {
        String body =
                Json.MAPPER.writeValueAsString(
                        omitMode ? Map.of("playerId", player) : new MatchRequest(player, Names.DEFAULT_MODE));
        var request =
                HttpRequest.newBuilder(URI.create(endpoint + Names.MATCH_PATH))
                        .timeout(Duration.ofSeconds(Client.MESSAGE_SECONDS))
                        .header("Content-Type", "application/json")
                        .POST(HttpRequest.BodyPublishers.ofString(body))
                        .build();
        return http.sendAsync(request, HttpResponse.BodyHandlers.ofString())
                .thenApply(
                        response -> {
                            Structure.require(
                                    response.statusCode() == HttpProtocol.OK,
                                    "HTTP match failed: " + response.statusCode());
                            try {
                                return Json.MAPPER.readValue(response.body(), MatchResponse.class);
                            } catch (Exception error) {
                                throw new CompletionException(error);
                            }
                        });
    }

    private static void newMatch(Nodes nodes, String replacementId) throws Exception {
        String suffix = UUID.randomUUID().toString(),
                pa = "new-a-" + suffix,
                pb = "new-b-" + suffix;
        var assignment = match(nodes.lobbyEndpoint, pa, pb, false);
        Structure.require(
                assignment.sessionEndpoint().equals(nodes.endpoint)
                        && assignment.roomId().matches("room-[0-9a-f]{32}"),
                "Invalid replacement assignment");
        try (var a = new Client(nodes.endpoint, pa, assignment.roomId());
                var b = new Client(nodes.endpoint, pb, assignment.roomId())) {
            a.beginJoin();
            b.beginJoin();
            var firstView = a.next(RoomView.class, candidate -> candidate.players().size() == Rules.PLAYERS);
            var view = b.next(RoomView.class, candidate -> candidate.players().size() == Rules.PLAYERS);
            Structure.require(
                    firstView.equals(view)
                            && view.roomId().equals(assignment.roomId())
                            && view.phase().equals(Rules.PLAYING)
                            && view.players().size() == Rules.PLAYERS
                            && view.nodeId().equals(replacementId),
                    "Replacement Room did not admit its members");
        }
    }

    private static RoomView moveTo(
            Client a, Client b, Client mover, double targetX, double targetY) throws Exception {
        var current = view(mover);
        while (player(current, mover.playerId).x() != targetX) {
            var p = player(current, mover.playerId);
            double amount = Math.copySign(Math.min(Rules.MAX_STEP, Math.abs(targetX - p.x())), targetX - p.x());
            current = move(a, b, mover, amount, 0, p.lastSequence() + 1);
        }
        while (player(current, mover.playerId).y() != targetY) {
            var p = player(current, mover.playerId);
            double amount = Math.copySign(Math.min(Rules.MAX_STEP, Math.abs(targetY - p.y())), targetY - p.y());
            current = move(a, b, mover, 0, amount, p.lastSequence() + 1);
        }
        return current;
    }

    private static RoomView move(
            Client a, Client b, Client mover, double dx, double dy, long sequence) throws Exception {
        var before = view(mover);
        a.clear();
        b.clear();
        for (;;) {
            mover.send(new MoveRequest(dx, dy, sequence));
            Object result = mover.nextAny(ActionRejected.class, RoomView.class);
            if (result instanceof ActionRejected rejected) {
                Structure.require(
                        rejected.reason().equals(Rules.MOVE_BUDGET)
                                && rejected.retryAfterMs() > 0,
                        "Unexpected movement rejection: " + rejected);
                sameState(before, view(mover));
                TimeUnit.MILLISECONDS.sleep(rejected.retryAfterMs());
                continue;
            }
            var moved = (RoomView) result;
            var expected = player(before, mover.playerId);
            Structure.require(
                    player(moved, mover.playerId).x() == expected.x() + dx
                            && player(moved, mover.playerId).y() == expected.y() + dy,
                    "Server movement result differs");
            var other = mover == a ? b : a;
            Structure.require(moved.equals(other.next(RoomView.class, view ->
                    player(view, mover.playerId).x() == expected.x() + dx
                            && player(view, mover.playerId).y() == expected.y() + dy)),
                    "Room views differ after movement");
            return moved;
        }
    }

    private static RoomView hit(Client a, Client b, long sequence, int hp) throws Exception {
        var before = view(b);
        a.clear();
        b.clear();
        for (;;) {
            a.send(new ShootRequest(b.playerId, sequence));
            Object result = a.nextAny(ActionRejected.class, RoomView.class);
            if (result instanceof ActionRejected rejected) {
                Structure.require(
                        rejected.reason().equals(Rules.FIRE_TOO_SOON)
                                && rejected.retryAfterMs() > 0,
                        "Unexpected shot rejection: " + rejected);
                sameState(before, view(b));
                TimeUnit.MILLISECONDS.sleep(rejected.retryAfterMs());
                continue;
            }
            var viewA = (RoomView) result;
            var viewB =
                    b.next(
                            RoomView.class,
                            view -> player(view, b.playerId).hp() == hp);
            Structure.require(
                    viewA.equals(viewB) && player(viewA, b.playerId).hp() == hp,
                    "Room views differ after shot");
            return viewA;
        }
    }

    private static void rejectShot(Client a, Client b, long sequence, String reason, String target)
            throws Exception {
        var before = view(b);
        a.clear();
        b.clear();
        a.send(new ShootRequest(target, sequence));
        var rejected = a.next(ActionRejected.class);
        Structure.require(
                rejected.reason().equals(reason) && rejected.retryAfterMs() == 0,
                "Wrong shot rejection: " + rejected);
        sameState(before, view(b));
    }

    private static RoomView view(Client client) throws Exception {
        client.clear();
        client.send(new RoomViewRequest());
        return client.next(RoomView.class);
    }

    private static PlayerView player(RoomView view, String id) {
        return view.players().stream()
                .filter(player -> player.playerId().equals(id))
                .findFirst()
                .orElseThrow();
    }

    private static void sameState(RoomView a, RoomView b) {
        Structure.require(
                a.roomId().equals(b.roomId())
                        && a.players().equals(b.players())
                        && a.phase().equals(b.phase())
                        && Objects.equals(a.winner(), b.winner()),
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
    private static ActionRejected rejectAction(
            Client player, Object request, String reason) throws Exception {
        var before = view(player);
        player.send(request);
        var response = player.next(ActionRejected.class);
        Structure.require(response.reason().equals(reason), "Expected " + reason + "; received " + response.reason());
        var timeLimited = reason.equals(Rules.MOVE_BUDGET) || reason.equals(Rules.FIRE_TOO_SOON);
        Structure.require(
                timeLimited ? response.retryAfterMs() > 0 : response.retryAfterMs() == 0,
                "Unexpected RetryAfterMs for " + reason);
        sameState(before, view(player));
        return response;
    }

}
