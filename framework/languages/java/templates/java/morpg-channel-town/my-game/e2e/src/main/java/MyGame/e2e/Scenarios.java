package MyGame.e2e;

import MyGame.*;

import java.util.*;

public final class Scenarios {
    private Scenarios() {}

    private static void equal(Object expected, Object actual) {
        Structure.require(
                Objects.equals(expected, actual),
                "Expected " + expected + ", got " + actual);
    }

    private static void sameState(DungeonView expected, DungeonView actual) {
        Structure.require(Client.sameDungeonState(expected, actual), "Dungeon state changed");
    }

    private static PartyView party(TownView view, List<String> members) {
        return view.parties().stream()
                .filter(item -> item.members().equals(members))
                .findFirst()
                .orElseThrow(() -> new IllegalStateException("Party not found: " + members));
    }

    private static PartyView party(TownView view, String partyId) {
        return view.parties().stream()
                .filter(item -> item.partyId().equals(partyId))
                .findFirst()
                .orElseThrow(() -> new IllegalStateException("Party not found: " + partyId));
    }

    private static TownView queryTown(Client client) throws Exception {
        client.clear();
        client.send(new ViewRequest());
        return client.next(TownView.class);
    }

    private static DungeonView enter(Client leader, Client member) throws Exception {
        return enter(leader, member, Names.CHANNEL_COUNT);
    }

    private static DungeonView enter(Client leader, Client member, long channel) throws Exception {
        leader.connect(channel);
        leader.next(TownView.class, view -> view.townId().equals(Names.townId(channel)));
        member.connect(channel);
        var together = leader.next(
                TownView.class,
                view -> view.players().containsAll(List.of(leader.playerId, member.playerId)));
        equal(
                together,
                member.next(
                        TownView.class,
                        view -> view.players().containsAll(List.of(leader.playerId, member.playerId))));

        leader.send(new CreatePartyRequest());
        var created = leader.next(
                TownView.class,
                view -> view.parties().stream()
                        .anyMatch(item -> item.members().equals(List.of(leader.playerId))));
        var leaderParty = party(created, List.of(leader.playerId));
        String partyId = leaderParty.partyId();
        Structure.require(Names.validId(partyId, "party-"), "Party ID format");

        member.send(new JoinPartyRequest(partyId));
        var expectedParty = new PartyView(
                partyId,
                List.of(leader.playerId, member.playerId),
                RunPhase.Forming,
                null);
        var joined = leader.next(
                TownView.class,
                view -> view.parties().contains(expectedParty));
        equal(joined, member.next(TownView.class, view -> view.parties().contains(expectedParty)));

        member.send(new EnterDungeonRequest());
        equal(
                new ActionRejected(Rules.NOT_LEADER),
                member.next(ActionRejected.class, rejected -> Rules.NOT_LEADER.equals(rejected.reason())));
        var unchanged = queryTown(member);
        var forming = party(unchanged, List.of(leader.playerId, member.playerId));
        equal(RunPhase.Forming, forming.phase());
        equal(null, forming.runId());

        member.send(new JoinDungeon("forged", "forged"));
        equal(
                new ActionRejected(Rules.PACKET_NOT_ALLOWED),
                member.next(
                        ActionRejected.class,
                        rejected -> Rules.PACKET_NOT_ALLOWED.equals(rejected.reason())));
        equal(unchanged, queryTown(member));

        leader.send(new EnterDungeonRequest());
        leader.send(new EnterDungeonRequest());
        var leaderView = leader.next(
                DungeonView.class,
                view -> view.partyId().equals(partyId) && view.phase() == RunPhase.Active);
        var memberView = member.next(
                DungeonView.class,
                view -> view.partyId().equals(partyId) && view.phase() == RunPhase.Active);
        equal(leaderView, memberView);
        equal(Names.dungeonId(leaderView.runId()), leaderView.dungeonId());
        Structure.require(Names.validId(leaderView.runId(), "run-"), "Run ID format");
        equal(0L, leaderView.stage());
        equal(List.of(leader.playerId, member.playerId), leaderView.members());
        sameState(leaderView, leader.view(leaderView, false));
        sameState(leaderView, member.view(leaderView, false));
        return leaderView;
    }

    private static DungeonView secondParty(String endpoint, DungeonView first) throws Exception {
        try (var p3 = new Client(endpoint, "p3"); var p4 = new Client(endpoint, "p4")) {
            var fresh = enter(p3, p4, 1);
            Structure.require(
                    !fresh.dungeonId().equals(first.dungeonId())
                            && !fresh.runId().equals(first.runId())
                            && !fresh.partyId().equals(first.partyId()),
                    "Two channel parties shared a run identity");
            return fresh;
        }
    }

    private static DungeonView advance(Client leader, Client member, long stage) throws Exception {
        leader.clear();
        member.clear();
        leader.send(new AdvanceRequest());
        var view = leader.next(DungeonView.class, current -> current.stage() == stage);
        equal(view, member.next(DungeonView.class, current -> current.stage() == stage));
        return view;
    }

    private static void channel(Client player, String endpoint) throws Exception {
        player.connector.connect().submit().toCompletableFuture().join();
        player.send(new JoinTownRequest(player.playerId, 1));
        var initial = player.next(TownView.class, view -> view.townId().equals(Names.townId(1)));
        equal("townA", initial.nodeId());
        var before = player.sessionInfo();

        try (var observer = new Client(endpoint, "channel-observer")) {
            observer.connect(1);
            observer.next(
                    TownView.class,
                    view -> view.players().containsAll(List.of(player.playerId, observer.playerId)));
            player.send(new CreatePartyRequest());
            var forming = player.next(
                    TownView.class,
                    view -> view.parties().stream()
                            .anyMatch(item -> item.members().equals(List.of(player.playerId))));
            String partyId = party(forming, List.of(player.playerId)).partyId();
            Structure.require(Names.validId(partyId, "party-"), "Party ID format");

            player.send(new ChangeChannelRequest(Names.MAXIMUM_INTEGER + 1));
            equal(
                    new ActionRejected(Rules.INTEGER_OUT_OF_RANGE),
                    player.next(
                            ActionRejected.class,
                            rejected -> Rules.INTEGER_OUT_OF_RANGE.equals(rejected.reason())));
            equal(forming, queryTown(player));

            observer.send(new ViewRequest());
            observer.next(
                    TownView.class,
                    view -> view.parties().stream()
                            .anyMatch(item -> item.partyId().equals(partyId)));
            observer.clear();
            observer.send(new ViewRequest());
            observer.next(TownView.class);
            player.send(new ChangeChannelRequest(2));
            var changed = player.next(
                    TownView.class,
                    view -> view.townId().equals(Names.townId(2)));
            var sourceLeft = observer.next(
                    TownView.class,
                    view -> !view.players().contains(player.playerId)
                            && view.parties().stream().noneMatch(item -> item.partyId().equals(partyId)));
            equal("townB", changed.nodeId());
            Structure.require(changed.players().contains(player.playerId), "Player missing from town-2");
            Structure.require(
                    changed.parties().stream().noneMatch(item -> item.partyId().equals(partyId)),
                    "Forming party followed channel change");
            Structure.require(
                    sourceLeft.parties().stream().noneMatch(item -> item.partyId().equals(partyId)),
                    "Source party remained after channel change");
            equal(before, player.sessionInfo());
            equal(0, player.disconnects.get());
        }
    }

    public static void run(Scenario scenario, Nodes nodes) throws Exception {
        String id = UUID.randomUUID().toString();
        try (var a = new Client(nodes.endpoint, id + "-p1");
                var b = new Client(nodes.endpoint, id + "-p2")) {
            if (scenario == Scenario.CHANNEL) {
                channel(a, nodes.endpoint);
                return;
            }

            var initial = enter(a, b);
            var sa = a.sessionInfo();
            var sb = b.sessionInfo();
            equal(a.playerId, sa.actorId());
            equal(b.playerId, sb.actorId());

            if (scenario == Scenario.DUNGEON) {
                a.send(new ChangeChannelRequest(1));
                equal(
                        new ActionRejected(Rules.RUN_IN_PROGRESS),
                        a.next(
                                ActionRejected.class,
                                rejected -> Rules.RUN_IN_PROGRESS.equals(rejected.reason())));
                sameState(initial, a.view(initial, false));
            }

            var before = advance(a, b, 1);
            equal(initial.nodeId(), before.nodeId());
            equal(sa, a.sessionInfo());
            equal(sb, b.sessionInfo());

            switch (scenario) {
                case DUNGEON -> {
                    for (long stage = 2; stage <= Names.FINAL_STAGE; stage++)
                        before = advance(a, b, stage);
                    equal(RunPhase.Completed, before.phase());

                    var completedState = before;
                    a.clear();
                    a.send(new ViewRequest());
                    var completedDungeon = a.next(
                            DungeonView.class,
                            view -> Client.sameDungeonState(completedState, view));
                    var completedParty = a.next(
                            TownView.class,
                            view -> view.parties().stream()
                                    .anyMatch(item -> item.partyId().equals(completedState.partyId())
                                            && item.phase() == RunPhase.Completed
                                            && Objects.equals(item.runId(), completedState.runId())));
                    equal(completedState, completedDungeon);
                    equal(RunPhase.Completed, party(completedParty, completedState.partyId()).phase());
                    a.send(new AdvanceRequest());
                    equal(
                            new ActionRejected(Rules.RUN_NOT_ACTIVE),
                            a.next(
                                    ActionRejected.class,
                                    rejected -> Rules.RUN_NOT_ACTIVE.equals(rejected.reason())));
                    sameState(before, a.view(before, false));
                    secondParty(nodes.endpoint, before);
                }
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
                    var after = a.view(before, true);
                    equal(after, b.view(before, true));
                    sameState(before, after);
                    Structure.require(
                            !before.nodeId().equals(after.nodeId()),
                            "Dungeon did not relocate");
                    nodes.replace(before.nodeId());
                    var fresh = secondParty(nodes.endpoint, before);
                    Structure.require(
                            fresh.nodeId().equals(before.nodeId())
                                    && !fresh.dungeonId().equals(before.dungeonId())
                                    && !fresh.runId().equals(before.runId()),
                            "Replacement instance did not accept a distinct run");
                    advance(a, b, 2);
                    equal(sa, a.sessionInfo());
                    equal(sb, b.sessionInfo());
                }
                case CHANNEL -> throw new IllegalStateException("Channel already handled");
            }
            equal(0, a.disconnects.get());
            if (scenario != Scenario.RECONNECT) equal(0, b.disconnects.get());
        }
    }
}
