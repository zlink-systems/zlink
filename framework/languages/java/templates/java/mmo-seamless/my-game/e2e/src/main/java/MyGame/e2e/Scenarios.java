package MyGame.e2e;

import MyGame.*;

import java.util.*;
import java.util.concurrent.*;

public final class Scenarios {
    private Scenarios() {}

    private static void require(boolean value, String message) {
        Structure.require(value, message);
    }

    private static void sameActor(SessionInfo before, SessionInfo after) {
        require(before.actorId().equals(after.actorId()), "Actor ID changed");
        require(before.objectGeneration() == after.objectGeneration(), "Actor generation changed");
    }

    private static Client create(Nodes nodes, List<Client> clients, String id) {
        var client = new Client(nodes.endpoint, id);
        clients.add(client);
        client.connect();
        return client;
    }

    public static void run(Scenario scenario, Nodes nodes) throws Exception {
        var clients = new ArrayList<Client>();
        Throwable primary = null;
        try {
            var client = create(nodes, clients, scenario.id() + "-" + UUID.randomUUID());
            switch (scenario) {
                case CROSS -> {
                    var before = client.join(294);
                    var session = client.sessionInfo();
                    require(
                            before.zoneId().equals("zone-3") && before.nodeId().equals("zoneA"),
                            "Cross origin mismatch");
                    var step = client.moveRejected(11, 1);
                    require(step.reason().equals("step-too-large"), "Expected step-too-large rejection");
                    require(client.state().equals(before), "Step rejection changed state");
                    var responses = client.moveWithImmediateBudgetRejection(2, 1, 10, 2);
                    var first = responses.accepted();
                    var budget = responses.rejected();
                    require(budget.reason().equals("move-budget"), "Expected move-budget rejection");
                    require(budget.retryAfterMs() > 0, "Move budget rejection omitted RetryAfterMs");
                    require(client.state().equals(first), "Budget rejection changed state");
                    var after = client.move(10, 2);
                    require(
                            after.zoneId().equals("zone-4")
                                    && after.nodeId().equals("zoneB")
                                    && after.x() == 306
                                    && after.sequence() == 2,
                            "Cross destination mismatch");
                    sameActor(session, client.sessionInfo());
                    require(session.equals(client.sessionInfo()), "Cross changed Session");
                    var packet = client.packetRejected(
                            new EnterZone(new Position(307), 3, 0));
                    require(packet.retryAfterMs() == 0, "Internal packet rejection has a retry delay");
                    require(client.state().equals(after), "Session rejection changed state");
                    var edge = create(nodes, clients, "edge-" + UUID.randomUUID());
                    var edgeBefore = edge.join(Rules.ZONE_WIDTH * (double) Rules.ZONE_COUNT - 1);
                    var outside = edge.moveRejected(1, 1);
                    require(outside.reason().equals("outside-world"), "Expected outside-world rejection");
                    require(edge.state().equals(edgeBefore), "World boundary rejection changed state");
                    require(client.move(1, 3).nodeId().equals("zoneB"), "Movement did not continue");
                    sameActor(session, client.sessionInfo());
                    require(
                            client.dungeon("dungeon-" + UUID.randomUUID())
                                    .nodeId()
                                    .equals("instance"),
                            "Dungeon host mismatch");
                }
                case BORDER -> {
                    var observer = create(nodes, clients, "observer-" + UUID.randomUUID());
                    var observed = observer.join(300);
                    var origin = client.join(299);
                    require(
                            observed.nodeId().equals("zoneB") && origin.nodeId().equals("zoneA"),
                            "Border origin mismatch");
                    var appeared = observer.next(PlayerAppeared.class, v -> v.playerId().equals(client.playerId));
                    require(
                            appeared.position().x() == 299,
                            "Border appearance mismatch");
                    var state = client.move(-1, 1);
                    var moved = observer.next(
                            PlayerMoved.class,
                            v -> v.playerId().equals(client.playerId) && v.sequence() == 1);
                    require(moved.position().x() == 298, "Border movement mismatch");
                    client.move(2, 2);
                    var left = observer.next(
                            PlayerLeft.class,
                            v -> v.playerId().equals(client.playerId)
                                    && v.reason() == PlayerLeftReason.LEFT_ZONE);
                    require(left.reason() == PlayerLeftReason.LEFT_ZONE, "Zone departure mismatch");
                    var entered = observer.next(
                            PlayerAppeared.class,
                            v -> v.playerId().equals(client.playerId) && v.position().x() == 300);
                    require(entered.position().x() == 300, "Target Zone appearance mismatch");
                    client.move(1, 3);
                    var continued = observer.next(
                            PlayerMoved.class,
                            v -> v.playerId().equals(client.playerId) && v.sequence() == 3);
                    require(continued.position().x() == 301, "Target Zone movement mismatch");
                    client.close();
                    clients.remove(client);
                    var disconnected = observer.next(
                            PlayerLeft.class,
                            v -> v.playerId().equals(client.playerId)
                                    && v.reason() == PlayerLeftReason.DISCONNECTED);
                    require(disconnected.reason() == PlayerLeftReason.DISCONNECTED,
                            "Disconnect visibility mismatch");
                    require(observer.disconnects.get() == 0, "Observer disconnected");
                }
                case RECONNECT -> {
                    client.join(299);
                    var before = client.move(1, 1);
                    var session = client.sessionInfo();
                    client.close();
                    clients.remove(client);
                    var reconnect = create(nodes, clients, client.playerId);
                    var after = reconnect.join(0);
                    require(before.equals(after), "Reconnect overwrote authoritative world state");
                    var rebound = reconnect.sessionInfo();
                    sameActor(session, rebound);
                    require(
                            !session.sessionId().equals(rebound.sessionId()),
                            "Reconnect reused physical Session");
                    reconnect.move(1, 2);
                    sameActor(session, reconnect.sessionInfo());
                    require(reconnect.disconnects.get() == 0, "Reconnected client disconnected");
                }
                case UPDATE -> {
                    var before = client.join(299);
                    var session = client.sessionInfo();
                    require(before.nodeId().equals("zoneA"), "Update origin mismatch");
                    WorldState state = client.move(-1, 1);
                    try (var executor = Executors.newSingleThreadExecutor()) {
                        var drain =
                                executor.submit(
                                        () -> {
                                            nodes.drain(before.nodeId());
                                            return null;
                                        });
                        long sequence = 1;
                        long expires = System.nanoTime() + Control.SAFE_DEADLINE.toNanos();
                        do {
                            require(System.nanoTime() < expires, "Update deadline exceeded");
                            if (drain.isDone()) drain.get();
                            sequence++;
                            double target = sequence % 2 == 0 ? 299 : 298;
                            state = client.move(target - state.x(), sequence);
                            sameActor(session, client.sessionInfo());
                            require(
                                    state.sequence() == sequence && client.disconnects.get() == 0,
                                    "Drain interrupted gameplay");
                        } while (!drain.isDone() || !state.nodeId().equals("zoneB"));
                        drain.get();
                        require(state.zoneId().equals("zone-3"), "Drain changed logical Zone");
                        nodes.replace(before.nodeId());
                        var after = client.move(299 - state.x(), sequence + 1);
                        require(
                                after.nodeId().equals("zoneB")
                                        && client.sessionInfo().equals(session)
                                        && client.disconnects.get() == 0,
                                "Replacement interrupted relocated player");
                        require(
                                client.dungeon("after-update-" + UUID.randomUUID())
                                        .nodeId()
                                        .equals("instance"),
                                "Dungeon unavailable after replacement");
                        var fresh = create(nodes, clients, "replacement-" + UUID.randomUUID());
                        require(
                                fresh.join(Control.REPLACEMENT_X).nodeId().equals(before.nodeId()),
                                "Replacement admission failed");
                        var identity = fresh.sessionInfo();
                        require(
                                fresh.move(1, 1)
                                        .nodeId()
                                        .equals(before.nodeId()),
                                "Replacement movement failed");
                        sameActor(identity, fresh.sessionInfo());
                        require(fresh.disconnects.get() == 0, "Replacement client disconnected");
                    }
                }
            }
            for (var activeClient : clients)
                require(activeClient.disconnects.get() == 0, "Client disconnected");
        } catch (Exception | Error error) {
            primary = error;
            throw error;
        } finally {
            Exception failure = null;
            for (var client : clients.reversed())
                try {
                    client.close();
                } catch (Exception error) {
                    if (failure == null) failure = error;
                    else failure.addSuppressed(error);
                }
            if (failure != null) {
                if (primary != null) primary.addSuppressed(failure);
                else throw failure;
            }
        }
    }
}
