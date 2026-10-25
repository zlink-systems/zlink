package MyGame.e2e;

import MyGame.*;

import java.util.*;
import java.util.function.Predicate;
import java.util.concurrent.TimeUnit;

public final class Scenarios {
    private Scenarios() {}

    private static final double WALL_CENTER_Y = (Rules.WALL_MIN_Y + Rules.WALL_MAX_Y) / 2;
    private static final String MISSING_PLAYER = "missing-player";

    private static void check(boolean value, String message) {
        Structure.require(value, message);
    }

    private static boolean sameView(RoomView left, RoomView right) {
        return new RoomView(
                        right.roomId(),
                        left.nodeId(),
                        right.phase(),
                        right.winner(),
                        right.players())
                .equals(left);
    }

    private static void same(RoomView before, RoomView after) {
        check(sameView(before, after), "Room state changed: " + before + " -> " + after);
    }

    private static RoomView match(Client a, Client b) throws Exception {
        a.queue();
        b.queue();
        var found = a.next(MatchFound.class);
        check(found.equals(b.next(MatchFound.class)), "Players matched different rooms");
        Predicate<RoomView> full = view -> view.players().size() == Rules.PLAYERS_PER_ROOM;
        var view = a.next(RoomView.class, full);
        check(
                view.equals(b.next(RoomView.class, full))
                        && view.roomId().equals(found.roomId()),
                "Initial room views differ");
        check(
                view.phase().equals(Rules.PLAYING_PHASE)
                        && view.winner() == null
                        && view.players().stream().allMatch(
                                player -> player.hp() == Rules.INITIAL_HP
                                        && player.lastSequence() == 0)
                        && view.players().stream().anyMatch(
                                player -> player.playerId().equals(a.playerId))
                        && view.players().stream().anyMatch(
                                player -> player.playerId().equals(b.playerId))
                        && view.players().stream().anyMatch(
                                player -> player.x() == Rules.FIRST_SPAWN_X
                                        && player.y() == Rules.FIRST_SPAWN_Y)
                        && view.players().stream().anyMatch(
                                player -> player.x() == Rules.SECOND_SPAWN_X
                                        && player.y() == Rules.SECOND_SPAWN_Y),
                "Initial room state differs: " + view);
        return view;
    }

    private static PlayerView player(RoomView view, Client client) {
        return view.players().stream()
                .filter(candidate -> candidate.playerId().equals(client.playerId))
                .findFirst()
                .orElseThrow();
    }

    private static long nextSequence(Client client) throws Exception {
        return player(client.view(), client).lastSequence() + 1;
    }

    private static RoomView moveBy(
            Client a,
            Client b,
            Client mover,
            double dx,
            double dy,
            long sequence,
            RoomView priorView,
            boolean requireMoveBudget)
            throws Exception {
        var before = priorView == null ? mover.view() : priorView;
        var current = player(before, mover);
        var targetX = current.x() + dx;
        var targetY = current.y() + dy;
        Predicate<RoomView> moved = view -> view.players().stream()
                .anyMatch(candidate -> candidate.playerId().equals(mover.playerId)
                        && candidate.x() == targetX
                        && candidate.y() == targetY
                        && candidate.lastSequence() == sequence);
        var request = new MoveRequest(dx, dy, sequence);
        var moveBudgetRejected = false;
        while (true) {
            mover.send(request);
            var result = mover.nextActionResult(moved);
            if (result instanceof ActionRejected rejection) {
                check(!moveBudgetRejected, "Move was rejected again after RetryAfterMs");
                moveBudgetRejected = true;
                check(
                        rejection.reason().equals(Rules.MOVE_BUDGET)
                                && rejection.retryAfterMs() > 0,
                        "Unexpected movement rejection: " + rejection.reason());
                same(before, mover.view());
                TimeUnit.MILLISECONDS.sleep(rejection.retryAfterMs());
                continue;
            }

            var updated = (RoomView) result;
            var peer = mover == a ? b : a;
            check(
                    updated.equals(peer.next(RoomView.class, moved)),
                    "Movement views differ");
            check(
                    !requireMoveBudget || moveBudgetRejected,
                    "Movement budget rejection was not observed");
            return updated;
        }
    }

    private static RoomView walk(
            Client a, Client b, Client mover, double destinationX, double destinationY)
            throws Exception {
        var view = mover.view();
        var current = player(view, mover);
        while (current.x() != destinationX || current.y() != destinationY) {
            var remainingX = destinationX - current.x();
            var dx = remainingX == 0
                    ? 0
                    : Math.max(-Rules.MAX_STEP, Math.min(Rules.MAX_STEP, remainingX));
            var remainingY = destinationY - current.y();
            var dy = dx != 0 || remainingY == 0
                    ? 0
                    : Math.max(-Rules.MAX_STEP, Math.min(Rules.MAX_STEP, remainingY));
            view = moveBy(
                    a,
                    b,
                    mover,
                    dx,
                    dy,
                    current.lastSequence() + 1,
                    view,
                    false);
            current = player(view, mover);
        }
        return view;
    }

    private static RoomView hit(
            Client a,
            Client b,
            long sequence,
            int hp,
            boolean allowFireRetry)
            throws Exception {
        Predicate<RoomView> hit = view -> view.players().stream()
                .anyMatch(candidate -> candidate.playerId().equals(b.playerId)
                        && candidate.hp() == hp)
                && (hp != 0
                        || (view.phase().equals(Rules.FINISHED_PHASE)
                                && a.playerId.equals(view.winner())));
        var before = a.view();
        var request = new ShootRequest(b.playerId, sequence);
        var fireTooSoonRejected = false;
        while (true) {
            a.send(request);
            var result = a.nextActionResult(hit);
            if (result instanceof ActionRejected rejection) {
                check(!fireTooSoonRejected, "Shot was rejected again after RetryAfterMs");
                fireTooSoonRejected = true;
                check(
                        rejection.reason().equals(Rules.FIRE_TOO_SOON)
                                && rejection.retryAfterMs() > 0,
                        "Unexpected shot rejection: " + rejection.reason());
                same(before, a.view());
                check(allowFireRetry, "Shot was rejected after the explicit RetryAfterMs wait");
                TimeUnit.MILLISECONDS.sleep(rejection.retryAfterMs());
                before = a.view();
                continue;
            }

            var updated = (RoomView) result;
            check(updated.equals(b.next(RoomView.class, hit)), "Hit views differ");
            return updated;
        }
    }

    private static ActionRejected rejectAction(
            Client player, Object request, String reason) throws Exception {
        var before = player.view();
        player.send(request);
        var response = player.next(ActionRejected.class);
        check(response.reason().equals(reason), "Expected " + reason + "; received " + response.reason());
        var timeLimited = reason.equals(Rules.MOVE_BUDGET) || reason.equals(Rules.FIRE_TOO_SOON);
        check(
                timeLimited ? response.retryAfterMs() > 0 : response.retryAfterMs() == 0,
                "Unexpected RetryAfterMs for " + reason);
        same(before, player.view());
        return response;
    }

    private static void rejectShot(
            Client a, Client b, String reason, String target) throws Exception {
        rejectAction(a, new ShootRequest(target == null ? b.playerId : target, nextSequence(a)), reason);
    }

    private static void input(Client p1, Client p2) throws Exception {
        hit(p1, p2, 1, Rules.INITIAL_HP - Rules.SHOT_DAMAGE, true);
        var fireDelay = rejectAction(
                p1,
                new ShootRequest(p2.playerId, 2),
                Rules.FIRE_TOO_SOON);
        TimeUnit.MILLISECONDS.sleep(fireDelay.retryAfterMs());
        hit(p1, p2, 2, Rules.INITIAL_HP - 2 * Rules.SHOT_DAMAGE, false);

        var initial = p1.view();
        var firstSpawn = initial.players().stream()
                .filter(candidate -> candidate.x() == Rules.FIRST_SPAWN_X
                        && candidate.y() == Rules.FIRST_SPAWN_Y)
                .findFirst()
                .orElseThrow();
        var mover = firstSpawn.playerId().equals(p1.playerId) ? p1 : p2;
        var movementStart = player(initial, mover).lastSequence() + 1;
        var budgetSequence = movementStart + 1;
        var firstMove = moveBy(p1, p2, mover, 1, 0, movementStart, initial, false);
        moveBy(p1, p2, mover, 1, 0, budgetSequence, firstMove, true);
        rejectAction(mover, new MoveRequest(0, 0, budgetSequence), Rules.STALE_SEQUENCE);
        rejectAction(mover, new MoveRequest(0, 0, movementStart), Rules.STALE_SEQUENCE);
        var stepTooLargeSequence = budgetSequence + 1;
        rejectAction(
                mover,
                new MoveRequest(Rules.MAX_STEP + 0.01, 0, stepTooLargeSequence),
                Rules.STEP_TOO_LARGE);
        rejectAction(
                mover,
                new MoveRequest(0, 0, Rules.MAX_SAFE_INTEGER + 1),
                Rules.INTEGER_OUT_OF_RANGE);

        var outsideStart = walk(p1, p2, mover, 1, Rules.FIRST_SPAWN_Y);
        var outsideSequence = player(outsideStart, mover).lastSequence() + 1;
        rejectAction(
                mover,
                new MoveRequest(-Rules.MAX_STEP, 0, outsideSequence),
                Rules.OUTSIDE_MAP);

        var beforeWall = walk(p1, p2, mover, Rules.WALL_X - Rules.MAX_STEP, WALL_CENTER_Y);
        var wallSequence = player(beforeWall, mover).lastSequence() + 1;
        rejectAction(
                mover,
                new MoveRequest(Rules.MAX_STEP, 0, wallSequence),
                Rules.MOVE_BLOCKED_BY_WALL);

        moveBy(p1, p2, mover, -0.1, 0, Rules.MAX_SAFE_INTEGER, null, false);
        check(
                player(mover.view(), mover).lastSequence() == Rules.MAX_SAFE_INTEGER,
                "Maximum safe sequence was not accepted");
        rejectAction(
                mover,
                new MoveRequest(0, 0, Rules.MAX_SAFE_INTEGER + 1),
                Rules.INTEGER_OUT_OF_RANGE);
    }

    private static void newMatch(Nodes nodes, String replacement) throws Exception {
        var suffix = UUID.randomUUID().toString();
        try (var a = new Client(nodes.endpoint, "new-a-" + suffix);
                var b = new Client(nodes.endpoint, "new-b-" + suffix)) {
            a.connect();
            b.connect();
            check(match(a, b).nodeId().equals(replacement), "Replacement did not accept the new room");
        }
    }

    static void run(Scenario scenario, Nodes nodes) throws Exception {
        var suffix = UUID.randomUUID().toString();
        try (var a = new Client(nodes.endpoint, "p1-" + suffix)) {
            var b = new Client(nodes.endpoint, "p2-" + suffix);
            boolean bClosed = false;
            Throwable primary = null;
            try {
                a.connect();
                b.connect();
                var initial = match(a, b);
                if (scenario == Scenario.INPUT) {
                    input(a, b);
                    check(a.disconnects.get() == 0 && a.connector.isConnected(), "First player disconnected");
                    check(b.disconnects.get() == 0 && b.connector.isConnected(), "Second player disconnected");
                    return;
                }

                var session1 = a.sessionInfo();
                var session2 = b.sessionInfo();
                check(
                        session1.actorId().equals(a.playerId)
                                && session2.actorId().equals(b.playerId),
                        "Actor identity differs");
                moveBy(a, b, a, 0, 0, 1, initial, false);
                moveBy(a, b, b, 0, 0, 1, initial, false);
                var before = hit(a, b, 2, Rules.INITIAL_HP - Rules.SHOT_DAMAGE, true);
                switch (scenario) {
                    case HIT -> {
                        rejectAction(a, new RoomAssignment(before.roomId()), Rules.PACKET_NOT_ALLOWED);
                        walk(a, b, a, Rules.FIRST_SPAWN_X, Rules.FIRST_SPAWN_Y);
                        walk(a, b, b, Rules.WALL_X + Rules.MAX_STEP, WALL_CENTER_Y);
                        walk(a, b, a, Rules.WALL_X - Rules.MAX_STEP, WALL_CENTER_Y);
                        rejectShot(a, b, Rules.BLOCKED_BY_WALL, null);
                        walk(a, b, a, Rules.FIRST_SPAWN_X, Rules.FIRST_SPAWN_Y);
                        walk(a, b, b, Rules.MAP_MAX_X, Rules.MAP_MAX_Y);
                        rejectShot(a, b, Rules.OUT_OF_RANGE, null);
                        rejectShot(a, b, Rules.NO_TARGET, MISSING_PLAYER);
                        walk(a, b, a, Rules.FIRST_SPAWN_X, Rules.FIRST_SPAWN_Y);
                        walk(a, b, b, Rules.SECOND_SPAWN_X, Rules.SECOND_SPAWN_Y);
                        var remaining = Rules.INITIAL_HP / Rules.SHOT_DAMAGE - 1;
                        for (var i = 0; i < remaining; i++)
                            before = hit(
                                    a,
                                    b,
                                    nextSequence(a),
                                    Rules.INITIAL_HP - Rules.SHOT_DAMAGE * (2 + i),
                                    true);
                        check(
                                a.next(MatchEnded.class).winner().equals(a.playerId)
                                        && b.next(MatchEnded.class).winner().equals(a.playerId)
                                        && before.phase().equals(Rules.FINISHED_PHASE)
                                        && before.winner().equals(a.playerId),
                                "Winner differs");
                    }
                    case WALL -> {
                        walk(a, b, a, Rules.WALL_X - Rules.MAX_STEP, WALL_CENTER_Y);
                        walk(a, b, b, Rules.WALL_X + Rules.MAX_STEP, WALL_CENTER_Y);
                        var beforeWall = a.view();
                        var targetBefore = player(beforeWall, b);
                        rejectAction(
                                a,
                                new MoveRequest(Rules.MAX_STEP, 0, nextSequence(a)),
                                Rules.MOVE_BLOCKED_BY_WALL);
                        rejectShot(a, b, Rules.BLOCKED_BY_WALL, null);
                        var afterWall = a.view();
                        same(beforeWall, afterWall);
                        check(player(afterWall, b).hp() == targetBefore.hp(), "Wall-blocked shot changed HP");

                        walk(a, b, a, Rules.FIRST_SPAWN_X, Rules.FIRST_SPAWN_Y);
                        walk(a, b, b, Rules.SECOND_SPAWN_X, Rules.SECOND_SPAWN_Y);
                        var expectedHp = player(a.view(), b).hp() - Rules.SHOT_DAMAGE;
                        var clearShot = hit(a, b, nextSequence(a), expectedHp, true);
                        check(player(clearShot, b).hp() == expectedHp, "Wall behind target blocked a clear shot");
                    }
                    case RECONNECT -> {
                        bClosed = true;
                        b.close();
                        try (var again = new Client(nodes.endpoint, b.playerId)) {
                            again.connect();
                            again.queue();
                            var restored = again.next(RoomView.class);
                            var identity = again.sessionInfo();
                            check(
                                    !identity.sessionId().equals(session2.sessionId())
                                            && identity.actorId().equals(session2.actorId())
                                            && identity.objectGeneration() == session2.objectGeneration(),
                                    "Reconnect changed Actor identity");
                            same(before, restored);
                            check(
                                    restored.phase().equals(Rules.PLAYING_PHASE)
                                            && restored.winner() == null
                                            && player(restored, again).lastSequence() == 1,
                                    "Reconnect did not restore the active phase and last sequence");
                            moveBy(again, a, again, 0, 0, 2, restored, false);
                            var finished = hit(a, again, 3, Rules.INITIAL_HP - 2 * Rules.SHOT_DAMAGE, true);
                            finished = hit(a, again, 4, Rules.INITIAL_HP - 3 * Rules.SHOT_DAMAGE, true);
                            finished = hit(a, again, 5, 0, true);
                            check(
                                    finished.phase().equals(Rules.FINISHED_PHASE)
                                            && finished.winner().equals(a.playerId),
                                    "Room view did not restore the finished phase and winner");
                            check(
                                    a.next(MatchEnded.class).winner().equals(a.playerId)
                                            && again.next(MatchEnded.class).winner().equals(a.playerId),
                                    "Finished match did not notify both players");
                            try (var finishedReconnect = new Client(nodes.endpoint, again.playerId)) {
                                finishedReconnect.connect();
                                finishedReconnect.queue();
                                var restoredFinished = finishedReconnect.next(
                                        RoomView.class,
                                        view -> view.phase().equals(Rules.FINISHED_PHASE));
                                var finishedIdentity = finishedReconnect.sessionInfo();
                                check(
                                        !finishedIdentity.sessionId().equals(identity.sessionId())
                                                && finishedIdentity.actorId().equals(identity.actorId())
                                                && finishedIdentity.objectGeneration() == identity.objectGeneration(),
                                        "Finished reconnect changed Actor identity");
                                same(finished, restoredFinished);
                                check(
                                        restoredFinished.winner().equals(a.playerId)
                                                && player(restoredFinished, finishedReconnect).lastSequence() == 2,
                                        "Finished reconnect did not restore winner and last sequence");
                            }
                        }
                    }
                    case UPDATE -> {
                        nodes.drain(before.nodeId());
                        var afterA = a.view();
                        var afterB = b.view();
                        check(
                                !afterA.nodeId().equals(before.nodeId())
                                        && afterB.nodeId().equals(afterA.nodeId()),
                                "Room did not relocate");
                        same(before, afterA);
                        same(before, afterB);
                        check(
                                a.sessionInfo().equals(session1)
                                        && b.sessionInfo().equals(session2),
                                "Update changed binding");
                        nodes.replace(before.nodeId());
                        newMatch(nodes, before.nodeId());
                        hit(a, b, 3, Rules.INITIAL_HP - Rules.SHOT_DAMAGE * 2, true);
                        check(
                                a.sessionInfo().equals(session1)
                                        && b.sessionInfo().equals(session2),
                                "Replacement changed binding");
                    }
                    case INPUT -> throw new IllegalStateException("Input scenario handled earlier");
                }
                check(
                        a.disconnects.get() == 0 && a.connector.isConnected(),
                        "First player disconnected");
                if (!bClosed)
                    check(
                            b.disconnects.get() == 0 && b.connector.isConnected(),
                            "Second player disconnected");
            } catch (Exception | Error error) {
                primary = error;
                throw error;
            } finally {
                if (!bClosed)
                    try {
                        b.close();
                    } catch (Exception error) {
                        if (primary != null) primary.addSuppressed(error);
                        else throw error;
                    }
            }
        }
    }
}
