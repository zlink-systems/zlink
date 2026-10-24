package MyGame.e2e;

import MyGame.*;

import java.util.*;

public final class Scenarios {
    private Scenarios() {}

    private static void equal(Object a, Object b) {
        Structure.require(Objects.equals(a, b), "Expected " + a + ", got " + b);
    }

    private static void sameState(DungeonView a, DungeonView b) {
        equal(a.dungeonId(), b.dungeonId());
        equal(a.partyId(), b.partyId());
        equal(a.members(), b.members());
        equal(a.stage(), b.stage());
    }

    private static DungeonView enter(Client a, Client b, String party) throws Exception {
        a.connect();
        a.next(LobbyView.class);
        b.connect();
        b.next(LobbyView.class);
        a.clear();
        b.clear();
        a.send(new CreatePartyRequest(party));
        equal(new PartyView(party, List.of(a.playerId), List.of(false)), a.next(PartyView.class));
        a.clear();
        b.send(new JoinPartyRequest(party));
        var joined = a.next(PartyView.class);
        equal(joined, b.next(PartyView.class));
        equal(new PartyView(party, List.of(a.playerId, b.playerId), List.of(false, false)), joined);
        a.send(new EnterDungeonRequest());
        equal(Rules.NOT_READY, a.next(ActionRejected.class).reason());
        a.clear();
        b.clear();
        a.send(new ReadyRequest());
        var ready = a.next(PartyView.class);
        equal(ready, b.next(PartyView.class));
        equal(List.of(true, false), ready.ready());
        a.clear();
        b.clear();
        b.send(new ReadyRequest());
        ready = a.next(PartyView.class);
        equal(ready, b.next(PartyView.class));
        equal(List.of(true, true), ready.ready());
        a.clear();
        b.clear();
        a.send(new EnterDungeonRequest());
        var dungeon = a.next(DungeonView.class);
        equal(dungeon, b.next(DungeonView.class));
        equal(Names.DUNGEON_PREFIX + party, dungeon.dungeonId());
        equal(party, dungeon.partyId());
        equal(List.of(a.playerId, b.playerId), dungeon.members());
        equal(0, dungeon.stage());
        sameState(dungeon, a.view());
        sameState(dungeon, b.view());
        return dungeon;
    }

    private static DungeonView advance(Client a, Client b, int stage) throws Exception {
        a.clear();
        b.clear();
        a.send(new AdvanceRequest());
        var view = a.next(DungeonView.class);
        equal(view, b.next(DungeonView.class));
        equal(stage, view.stage());
        return view;
    }

    public static void run(Scenario scenario, Nodes nodes) throws Exception {
        String id = UUID.randomUUID().toString();
        try (var a = new Client(nodes.endpoint, id + "-p1");
                var b = new Client(nodes.endpoint, id + "-p2")) {
            var initial = enter(a, b, id);
            var sa = a.sessionInfo();
            var sb = b.sessionInfo();
            equal(a.playerId, sa.actorId());
            equal(b.playerId, sb.actorId());
            var before = advance(a, b, 1);
            equal(initial.nodeId(), before.nodeId());
            equal(sa, a.sessionInfo());
            equal(sb, b.sessionInfo());
            switch (scenario) {
                case PARTY -> {}
                case RECONNECT -> {
                    b.close();
                    try (var again = new Client(nodes.endpoint, b.playerId)) {
                        again.connect();
                        var restored = again.next(DungeonView.class);
                        sameState(before, restored);
                        equal(before.nodeId(), restored.nodeId());
                        var identity = again.sessionInfo();
                        equal(sb.actorId(), identity.actorId());
                        equal(sb.objectGeneration(), identity.objectGeneration());
                        Structure.require(
                                !sb.sessionId().equals(identity.sessionId()),
                                "Reconnect reused Session");
                        advance(a, again, 2);
                        equal(0, again.disconnects.get());
                    }
                }
                case UPDATE -> {
                    nodes.drain(before.nodeId());
                    equal(sa, a.sessionInfo());
                    equal(sb, b.sessionInfo());
                    var after = a.view();
                    equal(after, b.view());
                    sameState(before, after);
                    Structure.require(
                            !before.nodeId().equals(after.nodeId()), "Dungeon did not relocate");
                    nodes.replace(before.nodeId());
                    try (var c = new Client(nodes.endpoint, id + "-new1");
                            var d = new Client(nodes.endpoint, id + "-new2")) {
                        var fresh = enter(c, d, id + "-replacement");
                        equal(before.nodeId(), fresh.nodeId());
                    }
                    advance(a, b, 2);
                    equal(sa, a.sessionInfo());
                    equal(sb, b.sessionInfo());
                }
            }
            equal(0, a.disconnects.get());
            if (scenario != Scenario.RECONNECT) equal(0, b.disconnects.get());
        }
    }
}
