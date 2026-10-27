package MyGame.e2e;

import MyGame.*;
import MyGame.Geometry.Vec3;

import java.util.*;
import java.util.function.Predicate;

public final class Scenarios {
    private Scenarios() {}

    static void run(Scenario scenario, Nodes nodes) throws Exception {
        if (scenario == Scenario.MATCHMAKING) {
            matchmaking(nodes);
            return;
        }
        String suffix = UUID.randomUUID().toString().replace("-", "");
        try (var a = new Client(nodes.endpoint, "p1-" + suffix);
                var b = new Client(nodes.endpoint, "p2-" + suffix)) {
            a.connect();
            b.connect();
            var initial = pair(a, b);
            var sessionA = a.sessionInfo();
            var sessionB = b.sessionInfo();
            check(sessionA.actorId().equals(a.playerId) && sessionB.actorId().equals(b.playerId), "Actor identity differs");
            switch (scenario) {
                case HIT -> hitScenario(a, b);
                case WALL -> wallScenario(a, b);
                case INPUT -> inputScenario(a, b);
                case RECONNECT -> reconnectScenario(a, b, sessionA, sessionB);
                case UPDATE -> updateScenario(a, b, nodes, initial, sessionA, sessionB);
                case MATCHMAKING -> throw new IllegalStateException("Matchmaking scenario was handled separately");
            }
            check(a.disconnects.get() == 0 && a.connector.isConnected(), "First player disconnected");
            if (scenario != Scenario.RECONNECT)
                check(b.disconnects.get() == 0 && b.connector.isConnected(), "Second player disconnected");
        }
    }

    private static void matchmaking(Nodes nodes) throws Exception {
        String suffix = UUID.randomUUID().toString().replace("-", "");
        try (var rankedA = new Client(nodes.endpoint, "ranked-a-" + suffix);
                var casualA = new Client(nodes.endpoint, "casual-a-" + suffix);
                var rankedB = new Client(nodes.endpoint, "ranked-b-" + suffix);
                var casualB = new Client(nodes.endpoint, "casual-b-" + suffix)) {
            for (var client : List.of(rankedA, casualA, rankedB, casualB)) client.connect();
            rankedA.queue("ranked");
            casualA.queue("casual");
            rankedB.queue("ranked");
            casualB.queue("casual");
            var rankedRoom = rankedA.next(MatchFound.class);
            var casualRoom = casualA.next(MatchFound.class);
            check(rankedRoom.roomId().equals(rankedB.next(MatchFound.class).roomId())
                    && casualRoom.roomId().equals(casualB.next(MatchFound.class).roomId())
                    && !rankedRoom.roomId().equals(casualRoom.roomId()), "Different modes were not separated");
            Predicate<RoomView> full = view -> view.players().size() == Rules.PLAYERS_PER_ROOM;
            var ranked = rankedA.next(RoomView.class, full);
            var casual = casualA.next(RoomView.class, full);
            var rankedOther = rankedB.next(RoomView.class, full);
            var casualOther = casualB.next(RoomView.class, full);
            check(ranked.equals(rankedOther) && casual.equals(casualOther)
                    && new HashSet<>(ranked.players().stream().map(PlayerView::playerId).toList())
                            .equals(Set.of(rankedA.playerId, rankedB.playerId))
                    && new HashSet<>(casual.players().stream().map(PlayerView::playerId).toList())
                            .equals(Set.of(casualA.playerId, casualB.playerId))
                    && ranked.phase() == MatchPhase.Playing && casual.phase() == MatchPhase.Playing
                    && ranked.players().stream().allMatch(player -> player.hp() == Rules.INITIAL_HP && player.lastSequence() == 0)
                    && casual.players().stream().allMatch(player -> player.hp() == Rules.INITIAL_HP && player.lastSequence() == 0),
                    "Matched Room members or initial state differ");
            var rejected = rankedA;
            rejected.send(new RoomAssignment(casualRoom.roomId()));
            var packetResult = rejected.next(ActionRejected.class);
            check(packetResult.reason().equals(Rejection.PACKET_NOT_ALLOWED), "Session accepted an internal RoomAssignment");
            same(ranked, view(rankedA));
            check(List.of(rankedA, rankedB, casualA, casualB).stream()
                    .allMatch(client -> client.disconnects.get() == 0 && client.connector.isConnected()),
                    "Matchmaking disconnected a client");
        }
    }

    private static RoomView pair(Client a, Client b) throws Exception {
        a.queue();
        b.queue();
        var foundA = a.next(MatchFound.class);
        var foundB = b.next(MatchFound.class);
        check(foundA.roomId().equals(foundB.roomId()), "Players matched different Rooms");
        Predicate<RoomView> full = room -> room.players().size() == Rules.PLAYERS_PER_ROOM;
        var first = a.next(RoomView.class, full);
        var second = b.next(RoomView.class, full);
        check(first.equals(second) && first.roomId().equals(foundA.roomId())
                && first.phase() == MatchPhase.Playing
                && first.players().stream().allMatch(player -> player.hp() == Rules.INITIAL_HP && player.lastSequence() == 0),
                "Initial room state differs");
        return first;
    }

    private static void hitScenario(Client a, Client b) throws Exception {
        hit(a, b, 75);
        shoot(a, new Vec3(0, 1, 0), ShotOutcome.MISS);
        check(player(view(b), b).hp() == 75, "An upward miss caused damage");
        walk(a, new Vec3(5, 0, 18));
        walk(a, new Vec3(5, 0, 65));
        walk(b, new Vec3(52, 0, 65));
        walk(b, new Vec3(95, 0, 65));
        shoot(a, aim(a, b), ShotOutcome.MISS);
        check(player(view(b), b).hp() == 75, "An out-of-range ray caused damage");
        walk(a, new Vec3(20, 0, 65));
        hit(a, b, 50);
        hit(a, b, 25);
        hit(a, b, 0);
        ended(a, b);
        reject(a, new MoveRequest(new Vec3(0, 0, 1), next(view(a), a)), Rejection.MATCH_FINISHED);
        reject(a, new ShootRequest(new Vec3(1, 0, 0), next(view(a), a)), Rejection.MATCH_FINISHED);
    }

    private static void wallScenario(Client a, Client b) throws Exception {
        walk(a, new Vec3(48, 0, 22));
        walk(b, new Vec3(52, 0, 22));
        var before = view(b);
        shoot(a, aim(a, b), ShotOutcome.BLOCKED_BY_WALL);
        check(player(view(b), b).hp() == player(before, b).hp(), "A wall-blocked ray caused damage");
        walk(a, Geometry.ArenaMap.SPAWNS.get(0));
        walk(b, Geometry.ArenaMap.SPAWNS.get(1));
        hit(a, b, 75);
        System.out.println("wall: intervening wall blocks; wall behind target permits hit");
    }

    private static void inputScenario(Client a, Client b) throws Exception {
        var beforeBudget = view(a);
        var moveSequence = next(beforeBudget, a);
        var acceptedMoveRequest = new MoveRequest(new Vec3(0, 0, -2), moveSequence);
        var move = new MoveRequest(new Vec3(0, 0, -2), moveSequence + 1);
        a.send(acceptedMoveRequest);
        a.send(move);
        var acceptedMove = a.next(MoveResult.class);
        check(acceptedMove.sequence() == moveSequence,
                "Expected accepted movement before movement-budget rejection");
        var budget = a.next(ActionRejected.class);
        check(budget.reason().equals(Rejection.MOVE_BUDGET) && budget.retryAfterMs() > 0,
                "Expected movement-budget rejection");
        var afterBudget = view(a);
        var beforeBudgetPlayer = player(beforeBudget, a);
        var afterBudgetPlayer = player(afterBudget, a);
        check(afterBudgetPlayer.position().equals(acceptedMove.position())
                        && afterBudgetPlayer.lastSequence() == moveSequence,
                "Movement-budget rejection changed the accepted movement position or sequence");
        check(beforeBudgetPlayer.hp() == afterBudgetPlayer.hp()
                        && player(beforeBudget, b).hp() == player(afterBudget, b).hp(),
                "Movement-budget rejection changed player HP");
        Thread.sleep(budget.retryAfterMs());
        var retriedMove = a.sendAndAwait(move, value -> value instanceof ActionRejected || value instanceof MoveResult);
        check(retriedMove instanceof MoveResult moveRetry && moveRetry.sequence() == move.sequence(),
                "Server movement retry delay was insufficient");

        var current = view(a);
        long sequence = player(current, a).lastSequence();
        reject(a, new MoveRequest(new Vec3(0, 0, 1), sequence), Rejection.STALE_SEQUENCE);
        reject(a, new ShootRequest(new Vec3(1, 0, 0), sequence - 1), Rejection.STALE_SEQUENCE);
        reject(a, new MoveRequest(new Vec3(Rules.MAX_STEP + 1, 0, 0), sequence + 1), Rejection.STEP_TOO_LARGE);
        reject(a, new MoveRequest(new Vec3(0, 1, 0), sequence + 1), Rejection.VERTICAL_MOVE);
        reject(a, new MoveRequest(new Vec3(0, -1, 0), sequence + 1), Rejection.VERTICAL_MOVE);
        walk(a, new Vec3(player(current, a).position().x(), 0, 0));
        sequence = player(view(a), a).lastSequence();
        reject(a, new MoveRequest(new Vec3(0, 0, -1), sequence + 1), Rejection.OUTSIDE_MAP);
        reject(a, new ShootRequest(new Vec3(0, 0, 0), sequence + 1), Rejection.INVALID_AIM);
        walk(a, new Vec3(48, 0, 22));
        sequence = next(view(a), a);
        reject(a, new MoveRequest(new Vec3(2, 0, 0), sequence), Rejection.MOVE_BLOCKED_BY_WALL);
        var beforeFire = view(a);
        var shotSequence = next(beforeFire, a);
        var acceptedShotRequest = new ShootRequest(new Vec3(0, 1, 0), shotSequence);
        var shot = new ShootRequest(new Vec3(0, 1, 0), shotSequence + 1);
        a.send(acceptedShotRequest);
        a.send(shot);
        var acceptedShot = a.next(ShotResult.class);
        check(acceptedShot.sequence() == shotSequence && acceptedShot.outcome().equals(ShotOutcome.MISS),
                "Expected accepted miss before fire-too-soon rejection");
        var fire = a.next(ShotRejected.class);
        check(fire.reason().equals(Rejection.FIRE_TOO_SOON) && fire.retryAfterMs() > 0,
                "Expected fire-interval rejection");
        var afterFire = view(a);
        var beforeFirePlayer = player(beforeFire, a);
        var afterFirePlayer = player(afterFire, a);
        check(afterFirePlayer.position().equals(beforeFirePlayer.position())
                        && afterFirePlayer.lastSequence() == shotSequence,
                "Fire-too-soon rejection changed the accepted shot position or sequence");
        check(beforeFirePlayer.hp() == afterFirePlayer.hp()
                        && player(beforeFire, b).hp() == player(afterFire, b).hp(),
                "Fire-too-soon rejection changed player HP");
        Thread.sleep(fire.retryAfterMs());
        var retriedShot = a.sendAndAwait(shot, value -> value instanceof ShotRejected || value instanceof ShotResult);
        check(retriedShot instanceof ShotResult shotRetry
                        && shotRetry.sequence() == shot.sequence()
                        && shotRetry.outcome().equals(ShotOutcome.MISS),
                "Server fire retry delay was insufficient");
        check(player(view(b), b).hp() == Rules.INITIAL_HP, "Input checks damaged target");
        System.out.println("input: move-budget and fire-too-soon retry delays accepted unchanged requests");
    }

    private static void reconnectScenario(Client a, Client b, SessionInfo sessionA, SessionInfo sessionB) throws Exception {
        walk(b, new Vec3(54, 0, 18));
        hit(a, b, 75);
        var beforeReconnect = view(b);
        b.close();
        try (var again = new Client(a.endpoint, b.playerId)) {
            again.connect();
            again.queue();
            same(beforeReconnect, again.next(RoomView.class));
            var identity = again.sessionInfo();
            check(!identity.sessionId().equals(sessionB.sessionId())
                    && identity.actorId().equals(sessionB.actorId())
                    && identity.objectGeneration() == sessionB.objectGeneration(), "Reconnect changed Actor identity");
            move(again, new Vec3(0, 0, 1));
            hit(a, again, 50);
            hit(a, again, 25);
            hit(a, again, 0);
            ended(a, again);
            check(again.disconnects.get() == 0 && again.connector.isConnected(), "Reconnected player disconnected");
        }
        try (var finished = new Client(a.endpoint, b.playerId)) {
            finished.connect();
            finished.queue();
            var restored = finished.next(RoomView.class);
            check(restored.phase() == MatchPhase.Finished && restored.winner().equals(a.playerId)
                    && player(restored, finished).hp() == 0, "Finished reconnect lost winner or HP");
        }
        check(a.sessionInfo().equals(sessionA), "Reconnect changed the first Session");
    }

    private static void updateScenario(
            Client a, Client b, Nodes nodes, RoomView initial, SessionInfo sessionA, SessionInfo sessionB)
            throws Exception {
        move(a, new Vec3(0, 0, -1));
        hit(a, b, 75);
        var before = view(a);
        nodes.drain(before.nodeId());
        check(a.sessionInfo().equals(sessionA) && b.sessionInfo().equals(sessionB), "Update changed session binding");
        var relocatedA = view(a);
        var relocatedB = view(b);
        check(!relocatedA.nodeId().equals(before.nodeId())
                        && !relocatedB.nodeId().equals(before.nodeId()),
                "Room did not relocate");
        same(before, relocatedA);
        same(before, relocatedB);
        nodes.replace(before.nodeId());
        newMatch(nodes, before.nodeId());
        hit(a, b, 50);
        check(a.sessionInfo().equals(sessionA) && b.sessionInfo().equals(sessionB), "Replacement changed session binding");
    }

    private static void newMatch(Nodes nodes, String replacement) throws Exception {
        String suffix = UUID.randomUUID().toString().replace("-", "");
        try (var a = new Client(nodes.endpoint, "new-a-" + suffix);
                var b = new Client(nodes.endpoint, "new-b-" + suffix)) {
            a.connect();
            b.connect();
            check(pair(a, b).nodeId().equals(replacement), "Replacement did not accept the new Room");
        }
    }

    private static void move(Client client, Vec3 delta) throws Exception {
        var before = view(client);
        var request = new MoveRequest(delta, next(before, client));
        while (true) {
            Object result = client.sendAndAwait(request, value -> value instanceof MoveResult || value instanceof ActionRejected);
            if (result instanceof ActionRejected rejected && rejected.reason().equals(Rejection.MOVE_BUDGET)
                    && rejected.retryAfterMs() > 0) {
                same(before, view(client));
                Thread.sleep(rejected.retryAfterMs());
                continue;
            }
            check(result instanceof MoveResult, "Movement was rejected: " + result);
            var moved = (MoveResult) result;
            check(moved.sequence() == request.sequence()
                    && moved.position().equals(player(before, client).position().plus(delta)), "Movement result differs");
            var after = view(client);
            check(player(after, client).lastSequence() == request.sequence(), "Movement lost sequence");
            check(after.players().stream().allMatch(player -> player.position().y() == 0), "Movement changed feet height");
            return;
        }
    }

    private static void walk(Client client, Vec3 destination) throws Exception {
        var position = player(view(client), client).position();
        while (!position.equals(destination)) {
            var remaining = destination.minus(position);
            Vec3 delta =
                    remaining.x() != 0
                            ? new Vec3(clamp(remaining.x()), 0, 0)
                            : remaining.y() != 0
                                    ? new Vec3(0, clamp(remaining.y()), 0)
                                    : new Vec3(0, 0, clamp(remaining.z()));
            move(client, delta);
            position = player(view(client), client).position();
        }
    }

    private static double clamp(double value) {
        return Math.max(-Rules.MAX_STEP, Math.min(value, Rules.MAX_STEP));
    }

    private static Vec3 aim(Client shooter, Client target) throws Exception {
        var room = view(shooter);
        return player(room, target).position().minus(player(room, shooter).position());
    }

    private static void shoot(Client shooter, Vec3 direction, String outcome) throws Exception {
        var before = view(shooter);
        var request = new ShootRequest(direction, next(before, shooter));
        while (true) {
            Object response = shooter.sendAndAwait(request, value -> value instanceof ShotResult || value instanceof ShotRejected);
            if (response instanceof ShotRejected rejected && rejected.reason().equals(Rejection.FIRE_TOO_SOON)
                    && rejected.retryAfterMs() > 0) {
                same(before, view(shooter));
                Thread.sleep(rejected.retryAfterMs());
                continue;
            }
            check(response instanceof ShotResult result && result.outcome().equals(outcome)
                    && result.sequence() == request.sequence(), "Expected " + outcome + ", received " + response);
            return;
        }
    }

    private static void hit(Client shooter, Client target, int hp) throws Exception {
        shoot(shooter, aim(shooter, target), ShotOutcome.HIT);
        var first = view(shooter);
        var second = view(target);
        check(first.equals(second) && player(first, target).hp() == hp, "Hit views or damage differ");
    }

    private static void ended(Client a, Client b) throws Exception {
        check(a.next(MatchEnded.class).winner().equals(a.playerId)
                && b.next(MatchEnded.class).winner().equals(a.playerId), "Wrong match winner");
        var room = view(a);
        check(room.phase() == MatchPhase.Finished && room.winner().equals(a.playerId), "Finished state differs");
    }

    private static void reject(Client client, Object request, String reason) throws Exception {
        var before = view(client);
        client.send(request);
        Object result = client.sendAndAwait(null, value -> value instanceof ActionRejected || value instanceof ShotRejected);
        boolean expected =
                result instanceof ActionRejected action
                        && action.retryAfterMs() == 0
                        && action.reason().equals(reason)
                || result instanceof ShotRejected shot
                        && shot.retryAfterMs() == 0
                        && shot.reason().equals(reason);
        check(expected, "Expected rejection " + reason + ", received " + result);
        same(before, view(client));
        System.out.println("input rejected: " + reason + "; state unchanged");
    }

    private static RoomView view(Client client) throws Exception {
        return client.view();
    }

    private static PlayerView player(RoomView room, Client client) {
        return room.players().stream()
                .filter(player -> player.playerId().equals(client.playerId))
                .findFirst()
                .orElseThrow();
    }

    private static long next(RoomView room, Client client) {
        return player(room, client).lastSequence() + 1;
    }

    private static void same(RoomView before, RoomView after) {
        check(new RoomView(after.roomId(), before.nodeId(), after.players(), after.phase(), after.winner()).equals(before),
                "Room state changed: " + before + " -> " + after);
    }

    private static void check(boolean condition, String message) {
        Structure.require(condition, message);
    }
}
