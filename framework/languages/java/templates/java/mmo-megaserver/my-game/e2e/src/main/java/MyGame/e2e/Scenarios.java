package MyGame.e2e;

import MyGame.*;

import java.util.*;

public final class Scenarios {
    private Scenarios() {}

    private static final String FOREST = "forest";
    private static final int MOVED_X = 42;

    private static void require(boolean condition, String message) {
        Structure.require(condition, message);
    }

    private static void sameState(RegionView before, RegionView after) {
        require(
                before.regionId().equals(after.regionId())
                        && before.players().equals(after.players()),
                "Region state changed");
    }

    private static void sameActor(SessionInfo before, SessionInfo after) {
        require(
                before.actorId().equals(after.actorId())
                        && before.objectGeneration() == after.objectGeneration(),
                "Actor identity changed");
    }

    private static Client create(Nodes nodes, List<Client> clients, String id) {
        var client = new Client(nodes.endpoint, id);
        clients.add(client);
        client.connect();
        return client;
    }

    public static int run(List<Scenario> selected, Nodes nodes) throws Exception {
        var clients = new ArrayList<Client>();
        var completed = EnumSet.noneOf(Scenario.class);
        Throwable primary = null;
        try {
            var p1 = create(nodes, clients, "p1");
            var first = p1.join(FOREST);
            require(first.regionId().equals(Names.regionId(FOREST, 1)), "First Region mismatch");
            var p2 = create(nodes, clients, "p2");
            var together = p2.join(FOREST);
            require(
                    together.regionId().equals(first.regionId())
                            && together.players()
                                    .equals(
                                            List.of(
                                                    new PlayerPosition("p1", 0),
                                                    new PlayerPosition("p2", 0))),
                    "Players are not together");
            sameState(together, p1.next(RegionView.class, v -> v.players().size() == 2));
            var p3 = create(nodes, clients, "p3");
            var isolated = p3.join(FOREST);
            require(
                    isolated.regionId().equals(Names.regionId(FOREST, 2))
                            && isolated.players().equals(List.of(new PlayerPosition("p3", 0))),
                    "Capacity did not isolate p3");
            var session = p1.sessionInfo();
            var session2 = p2.sessionInfo();
            var moved = p1.move(MOVED_X, 1);
            sameState(
                    moved,
                    p2.next(
                            RegionView.class,
                            v ->
                                    v.players().stream()
                                            .anyMatch(
                                                    p ->
                                                            p.playerId().equals("p1")
                                                                    && p.x() == MOVED_X)));
            sameState(isolated, p3.view());
            require(
                    p3.history.stream()
                            .filter(RegionView.class::isInstance)
                            .map(RegionView.class::cast)
                            .allMatch(
                                    v ->
                                            v.players().stream()
                                                    .allMatch(p -> p.playerId().equals("p3"))),
                    "p3 received another Region's movement");
            for (var scenario : selected) {
                switch (scenario) {
                    case REGION -> {}
                    case DUNGEON -> {
                        var id = "dungeon-1";
                        var opened = p1.dungeon(id);
                        require(
                                opened.dungeonId().equals(id)
                                        && opened.nodeId().equals(Roles.INSTANCE),
                                "Dungeon host mismatch");
                    }
                    case RECONNECT -> {
                        p1.close();
                        clients.remove(p1);
                        p1 = create(nodes, clients, "p1");
                        var restored = p1.join(FOREST);
                        sameState(moved, restored);
                        require(
                                moved.nodeId().equals(restored.nodeId()),
                                "Reconnect changed Region host");
                        var identity = p1.sessionInfo();
                        sameActor(session, identity);
                        require(
                                !session.sessionId().equals(identity.sessionId()),
                                "Reconnect reused physical Session");
                        session = identity;
                    }
                    case UPDATE -> {
                        nodes.drain(moved.nodeId());
                        var relocated = p1.view();
                        sameState(moved, relocated);
                        sameState(relocated, p2.view());
                        require(
                                !relocated.nodeId().equals(moved.nodeId()),
                                "Region did not relocate");
                        require(
                                session.equals(p1.sessionInfo())
                                        && session2.equals(p2.sessionInfo()),
                                "Relocation changed Sessions");
                        require(
                                p1.disconnects.get() == 0 && p2.disconnects.get() == 0,
                                "Drain disconnected clients");
                        nodes.replace(moved.nodeId());
                        sameState(relocated, p1.view());
                        var fresh = create(nodes, clients, "p4");
                        var admitted = fresh.join("desert");
                        require(
                                admitted.nodeId().equals(moved.nodeId())
                                        && admitted.players()
                                                .equals(List.of(new PlayerPosition("p4", 0))),
                                "Replacement did not accept new Region");
                    }
                }
                for (var client : clients)
                    require(client.disconnects.get() == 0, "Client disconnected");
                completed.add(scenario);
                System.out.println("✓ " + scenario.id());
            }
        } catch (Exception error) {
            primary = error;
            error.printStackTrace(System.err);
        } catch (Error error) {
            primary = error;
            throw error;
        } finally {
            for (var scenario : selected)
                if (!completed.contains(scenario)) System.out.println("✗ " + scenario.id());
            Exception cleanup = null;
            for (var client : clients.reversed())
                try {
                    client.close();
                } catch (Exception error) {
                    if (cleanup == null) cleanup = error;
                    else cleanup.addSuppressed(error);
                }
            if (cleanup != null) {
                if (primary != null) {
                    primary.addSuppressed(cleanup);
                    cleanup.printStackTrace(System.err);
                } else throw cleanup;
            }
        }
        return completed.size();
    }
}
