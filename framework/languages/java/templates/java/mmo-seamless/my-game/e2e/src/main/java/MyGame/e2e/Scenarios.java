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
        try {
            var client = create(nodes, clients, scenario.id() + "-" + UUID.randomUUID());
            switch (scenario) {
                case CROSS -> {
                    var before = client.join(299);
                    var session = client.sessionInfo();
                    require(
                            before.zoneId().equals("zone-3") && before.nodeId().equals("zoneA"),
                            "Cross origin mismatch");
                    var after = client.move(300, 1);
                    require(
                            after.zoneId().equals("zone-4")
                                    && after.nodeId().equals("zoneB")
                                    && after.x() == 300
                                    && after.sequence() == 1,
                            "Cross destination mismatch");
                    sameActor(session, client.sessionInfo());
                    require(session.equals(client.sessionInfo()), "Cross changed Session");
                    require(
                            client.move(301, 2).nodeId().equals("zoneB"),
                            "Movement did not continue");
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
                    client.move(298, 1);
                    var border =
                            observer.next(
                                    BorderVisible.class,
                                    v -> v.actorId().equals(client.playerId) && v.sequence() == 1);
                    require(
                            border.observerZoneId().equals("zone-4")
                                    && border.sourceZoneId().equals("zone-3")
                                    && border.x() == 298,
                            "Border visibility mismatch");
                    require(observer.disconnects.get() == 0, "Observer disconnected");
                }
                case RECONNECT -> {
                    client.join(299);
                    var before = client.move(300, 1);
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
                    reconnect.move(301, 2);
                    sameActor(session, reconnect.sessionInfo());
                    require(reconnect.disconnects.get() == 0, "Reconnected client disconnected");
                }
                case UPDATE -> {
                    var before = client.join(299);
                    var session = client.sessionInfo();
                    require(before.nodeId().equals("zoneA"), "Update origin mismatch");
                    client.move(298, 1);
                    try (var executor = Executors.newSingleThreadExecutor()) {
                        var drain =
                                executor.submit(
                                        () -> {
                                            nodes.drain(before.nodeId());
                                            return null;
                                        });
                        int sequence = 1;
                        WorldState state;
                        long expires = System.nanoTime() + Control.SAFE_DEADLINE.toNanos();
                        do {
                            require(System.nanoTime() < expires, "Update deadline exceeded");
                            if (drain.isDone()) drain.get();
                            sequence++;
                            state = client.move(sequence % 2 == 0 ? 299 : 298, sequence);
                            sameActor(session, client.sessionInfo());
                            require(
                                    state.sequence() == sequence && client.disconnects.get() == 0,
                                    "Drain interrupted gameplay");
                        } while (!drain.isDone() || !state.nodeId().equals("zoneB"));
                        drain.get();
                        require(state.zoneId().equals("zone-3"), "Drain changed logical Zone");
                        nodes.replace(before.nodeId());
                        var after = client.move(299, sequence + 1);
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
                                fresh.move(Control.REPLACEMENT_X + 1, 1)
                                        .nodeId()
                                        .equals(before.nodeId()),
                                "Replacement movement failed");
                        sameActor(identity, fresh.sessionInfo());
                        require(fresh.disconnects.get() == 0, "Replacement client disconnected");
                    }
                }
            }
            if (scenario != Scenario.RECONNECT)
                require(client.disconnects.get() == 0, "Client disconnected");
        } finally {
            Exception failure = null;
            for (var client : clients.reversed())
                try {
                    client.close();
                } catch (Exception error) {
                    if (failure == null) failure = error;
                    else failure.addSuppressed(error);
                }
            if (failure != null) throw failure;
        }
    }
}
