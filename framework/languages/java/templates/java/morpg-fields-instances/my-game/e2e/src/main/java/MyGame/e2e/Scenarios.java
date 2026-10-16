package MyGame.e2e;

import MyGame.*;

import java.util.*;

public final class Scenarios implements AutoCloseable {
    private final Nodes nodes;
    private final List<Client> clients = new ArrayList<>();
    private Client leader, member;
    private DungeonView advanced;

    Scenarios(Nodes nodes) {
        this.nodes = nodes;
    }

    private static void require(boolean value, String message) {
        Structure.require(value, message);
    }

    private Client create(String id) {
        var client = new Client(nodes.endpoint, id);
        clients.add(client);
        client.connect();
        return client;
    }

    private static void sameActor(SessionInfo before, SessionInfo after) {
        require(
                before.actorId().equals(after.actorId())
                        && before.objectGeneration() == after.objectGeneration(),
                "Actor identity changed");
    }

    private static void sameDungeon(DungeonView before, DungeonView after) {
        require(
                before.dungeonId().equals(after.dungeonId())
                        && before.partyId().equals(after.partyId())
                        && before.members().equals(after.members())
                        && before.stage() == after.stage(),
                "Dungeon state changed");
    }

    private static boolean party(FieldView view, String id, List<String> members) {
        return view.parties().stream()
                .anyMatch(p -> p.partyId().equals(id) && p.members().equals(members));
    }

    private DungeonView enter(Client p1, Client p2, String partyId) throws Exception {
        p1.send(new CreatePartyRequest(partyId));
        p1.next(FieldView.class, v -> party(v, partyId, List.of(p1.playerId)));
        var members = List.of(p1.playerId, p2.playerId);
        p2.send(new JoinPartyRequest(partyId));
        p2.next(FieldView.class, v -> party(v, partyId, members));
        p1.next(FieldView.class, v -> party(v, partyId, members));
        p1.send(new EnterDungeonRequest());
        var a = p1.next(DungeonView.class, v -> v.partyId().equals(partyId));
        var b = p2.next(DungeonView.class, v -> v.partyId().equals(partyId));
        sameDungeon(a, b);
        require(
                a.dungeonId().equals(Rules.dungeonId(partyId))
                        && a.nodeId().equals(b.nodeId())
                        && a.stage() == 0
                        && a.members().equals(members),
                "Party did not enter the same fresh dungeon");
        // Query both current Spots before submitting the next stage transition.
        sameDungeon(a, p1.dungeon());
        sameDungeon(b, p2.dungeon());
        return a;
    }

    private void prepare() throws Exception {
        if (advanced != null) return;
        leader = create("dungeon-p1");
        member = create("dungeon-p2");
        leader.join(250);
        member.join(250);
        var s1 = leader.sessionInfo();
        var s2 = member.sessionInfo();
        var initial = enter(leader, member, "party-1");
        leader.send(new AdvanceRequest());
        advanced = leader.next(DungeonView.class, v -> v.stage() == 1);
        sameDungeon(advanced, member.next(DungeonView.class, v -> v.stage() == 1));
        require(initial.nodeId().equals(advanced.nodeId()), "Advance changed host");
        require(
                s1.equals(leader.sessionInfo()) && s2.equals(member.sessionInfo()),
                "Dungeon entry changed Session");
    }

    void run(Scenario scenario) throws Exception {
        if (scenario == Scenario.FIELD) {
            var player = create("p1");
            var observer = create("border-p2");
            var observed = observer.join(199);
            var origin = player.join(198);
            require(
                    origin.fieldId().equals("field-2")
                            && origin.nodeId().equals("fieldA")
                            && observed.nodeId().equals("fieldA"),
                    "Field crossing origin mismatch");
            var session = player.sessionInfo();
            var after = player.move(200, 1);
            require(
                    after.fieldId().equals("field-3")
                            && after.nodeId().equals("fieldB")
                            && after.x() == 200
                            && after.sequence() == 1,
                    "Field crossing destination mismatch");
            var border =
                    observer.next(
                            BorderVisible.class,
                            v -> v.playerId().equals(player.playerId) && v.sequence() == 1);
            require(
                    border.observerFieldId().equals("field-2")
                            && border.sourceFieldId().equals("field-3")
                            && border.x() == 200,
                    "Border publication mismatch");
            require(
                    session.equals(player.sessionInfo())
                            && player.disconnects.get() == 0
                            && observer.disconnects.get() == 0,
                    "Field crossing interrupted connection");
            return;
        }
        prepare();
        switch (scenario) {
            case DUNGEON -> {
                sameDungeon(advanced, leader.dungeon());
                sameDungeon(advanced, member.dungeon());
            }
            case RECONNECT -> {
                var before = member.sessionInfo();
                member.close();
                clients.remove(member);
                member = create(before.actorId());
                member.send(new JoinWorldRequest(member.playerId, 250));
                var restored = member.next(DungeonView.class);
                sameDungeon(advanced, restored);
                require(advanced.nodeId().equals(restored.nodeId()), "Reconnect changed host");
                var after = member.sessionInfo();
                sameActor(before, after);
                require(
                        !before.sessionId().equals(after.sessionId()),
                        "Reconnect reused physical Session");
            }
            case UPDATE -> {
                var s1 = leader.sessionInfo();
                var s2 = member.sessionInfo();
                nodes.drain(advanced.nodeId());
                var relocated = leader.dungeon(advanced.nodeId());
                var other = member.dungeon(advanced.nodeId());
                sameDungeon(advanced, relocated);
                sameDungeon(relocated, other);
                require(
                        !relocated.nodeId().equals(advanced.nodeId())
                                && relocated.nodeId().equals(other.nodeId()),
                        "Dungeon did not relocate together");
                require(
                        s1.equals(leader.sessionInfo()) && s2.equals(member.sessionInfo()),
                        "Host relocation changed Session");
                nodes.replace(advanced.nodeId());
                sameDungeon(relocated, leader.dungeon());
                sameDungeon(relocated, member.dungeon());
                var p3 = create("replacement-p3");
                var p4 = create("replacement-p4");
                p3.join(250);
                p4.join(250);
                var fresh = enter(p3, p4, "party-2");
                require(
                        fresh.nodeId().equals(advanced.nodeId()),
                        "Replacement did not accept new work");
                require(
                        s1.equals(leader.sessionInfo()) && s2.equals(member.sessionInfo()),
                        "Replacement interrupted Session");
            }
            default -> throw new IllegalStateException("Unexpected scenario");
        }
        for (var client : clients)
            require(client.disconnects.get() == 0, "Client disconnected: " + client.playerId);
    }

    public void close() throws Exception {
        Exception failure = null;
        for (var client : clients.reversed()) {
            try {
                client.close();
            } catch (Exception error) {
                if (failure == null) failure = error;
                else failure.addSuppressed(error);
            }
        }
        if (failure != null) throw failure;
    }
}
