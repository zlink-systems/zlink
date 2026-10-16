package MyGame.e2e;

import MyGame.*;

import java.util.*;
import java.util.function.Predicate;

public final class Scenarios {
    private Scenarios() {}

    private static final double NEAR_X = 10,
            TARGET_X = 20,
            WALL_BEHIND_X = 60,
            FAR_X = 200,
            LANE_Y = 10;
    private static final String MISSING_PLAYER = "missing-player";

    private static void check(boolean value, String message) {
        Structure.require(value, message);
    }

    private static void same(RoomView before, RoomView after) {
        check(
                new RoomView(after.roomId(), before.nodeId(), after.players()).equals(before),
                "Room state changed: " + before + " -> " + after);
    }

    private static RoomView match(Client a, Client b) throws Exception {
        a.queue();
        b.queue();
        var found = a.next(MatchFound.class);
        check(found.equals(b.next(MatchFound.class)), "Players matched different rooms");
        Predicate<RoomView> full = v -> v.players().size() == Rules.PLAYERS_PER_ROOM;
        var view = a.next(RoomView.class, full);
        check(
                view.equals(b.next(RoomView.class, full)) && view.roomId().equals(found.roomId()),
                "Initial room views differ");
        return view;
    }

    private static void move(Client a, Client b, Client mover, double x, long sequence)
            throws Exception {
        mover.send(new MoveRequest(x, LANE_Y, sequence));
        Predicate<RoomView> moved =
                v ->
                        v.players().stream()
                                .anyMatch(
                                        p ->
                                                p.playerId().equals(mover.playerId)
                                                        && p.x() == x
                                                        && p.y() == LANE_Y);
        check(
                a.next(RoomView.class, moved).equals(b.next(RoomView.class, moved)),
                "Movement views differ");
    }

    private static RoomView hit(Client a, Client b, long sequence, int hp) throws Exception {
        a.send(new ShootRequest(b.playerId, sequence));
        Predicate<RoomView> hit =
                v ->
                        v.players().stream()
                                .anyMatch(p -> p.playerId().equals(b.playerId) && p.hp() == hp);
        var view = a.next(RoomView.class, hit);
        check(view.equals(b.next(RoomView.class, hit)), "Hit views differ");
        return view;
    }

    private static void reject(Client a, Client b, long sequence, String reason, String target)
            throws Exception {
        var before = b.view();
        a.send(new ShootRequest(target, sequence));
        check(a.next(ShotRejected.class).reason().equals(reason), "Wrong shot rejection");
        same(before, b.view());
    }

    private static void newMatch(Nodes nodes, String replacement) throws Exception {
        var suffix = UUID.randomUUID().toString();
        try (var a = new Client(nodes.endpoint, "new-a-" + suffix);
                var b = new Client(nodes.endpoint, "new-b-" + suffix)) {
            a.connect();
            b.connect();
            check(
                    match(a, b).nodeId().equals(replacement),
                    "Replacement did not accept the new room");
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
                match(a, b);
                var session1 = a.sessionInfo();
                var session2 = b.sessionInfo();
                check(
                        session1.actorId().equals(a.playerId)
                                && session2.actorId().equals(b.playerId),
                        "Actor identity differs");
                move(a, b, a, NEAR_X, 1);
                move(a, b, b, TARGET_X, 1);
                var before = hit(a, b, 2, Rules.INITIAL_HP - Rules.SHOT_DAMAGE);
                switch (scenario) {
                    case HIT -> {
                        move(a, b, b, WALL_BEHIND_X, 2);
                        reject(a, b, 3, Rules.BLOCKED_BY_WALL, b.playerId);
                        move(a, b, b, FAR_X, 3);
                        reject(a, b, 4, Rules.OUT_OF_RANGE, b.playerId);
                        reject(a, b, 5, Rules.NO_TARGET, MISSING_PLAYER);
                        move(a, b, b, TARGET_X, 4);
                        int remaining = Rules.INITIAL_HP / Rules.SHOT_DAMAGE - 1;
                        for (int i = 0; i < remaining; i++)
                            hit(a, b, 6 + i, Rules.INITIAL_HP - Rules.SHOT_DAMAGE * (2 + i));
                        check(
                                a.next(MatchEnded.class).winner().equals(a.playerId)
                                        && b.next(MatchEnded.class).winner().equals(a.playerId),
                                "Winner differs");
                    }
                    case RECONNECT -> {
                        bClosed = true;
                        b.close();
                        try (var again = new Client(nodes.endpoint, b.playerId)) {
                            again.connect();
                            again.queue();
                            same(before, again.next(RoomView.class));
                            var identity = again.sessionInfo();
                            check(
                                    !identity.sessionId().equals(session2.sessionId())
                                            && identity.actorId().equals(session2.actorId())
                                            && identity.objectGeneration()
                                                    == session2.objectGeneration(),
                                    "Reconnect changed Actor identity");
                            hit(a, again, 3, Rules.INITIAL_HP - Rules.SHOT_DAMAGE * 2);
                            check(
                                    again.disconnects.get() == 0 && again.connector.isConnected(),
                                    "Reconnected player disconnected");
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
                        hit(a, b, 3, Rules.INITIAL_HP - Rules.SHOT_DAMAGE * 2);
                        check(
                                a.sessionInfo().equals(session1)
                                        && b.sessionInfo().equals(session2),
                                "Replacement changed binding");
                    }
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
