package MyGame.e2e;

import MyGame.*;

import java.util.*;
import java.util.concurrent.TimeUnit;

public final class Scenarios implements AutoCloseable {
    private final Nodes nodes;
    private final Set<Scenario> selectedScenarios;
    private final List<Client> clients = new ArrayList<>();
    private Client leader, member;
    private DungeonView advanced;

    Scenarios(Nodes nodes, List<Scenario> selectedScenarios) {
        this.nodes = nodes;
        this.selectedScenarios = Set.copyOf(selectedScenarios);
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
        require(before.actorId().equals(after.actorId())
                && before.objectGeneration() == after.objectGeneration(),
                "Actor identity changed");
    }

    private static void sameDungeon(DungeonView before, DungeonView after) {
        require(before.dungeonId().equals(after.dungeonId())
                        && before.partyId().equals(after.partyId())
                        && before.members().equals(after.members())
                        && before.stage() == after.stage()
                        && before.runId().equals(after.runId())
                        && before.phase() == after.phase(),
                "Dungeon state changed");
    }

    private static boolean party(PartyView view, String id, List<String> members) {
        return view.partyId().equals(id) && view.members().equals(members);
    }

    private DungeonView enter(Client p1, Client p2) throws Exception {
        p1.send(new CreatePartyRequest());
        var created = p1.next(PartyView.class,
                view -> view.members().equals(List.of(p1.playerId))
                        && view.phase() == RunPhase.Forming);
        String partyId = created.partyId();
        require(MyGame.field.PartyRun.validId(partyId, "party-"),
                "Party ID is not server-issued");

        p2.send(new JoinPartyRequest(partyId));
        var members = List.of(p1.playerId, p2.playerId);
        var joined = p2.next(PartyView.class, view -> party(view, partyId, members));
        p1.next(PartyView.class, view -> party(view, partyId, members));

        p2.send(new EnterDungeonRequest());
        var nonleader = p2.next(ActionRejected.class,
                value -> value.reason().equals(Rejection.NOT_LEADER));
        require(nonleader.retryAfterMs() == 0, "Leadership rejection has a cooldown");
        p2.send(new ViewRequest());
        var unchanged = p2.next(PartyView.class, view -> view.partyId().equals(partyId));
        require(joined.members().equals(unchanged.members())
                        && joined.phase() == unchanged.phase()
                        && unchanged.runId() == null,
                "Rejected entry changed party state");

        p1.send(new EnterDungeonRequest());
        p1.send(new EnterDungeonRequest());
        var leaderView = p1.next(DungeonView.class,
                view -> view.partyId().equals(partyId) && view.phase() == RunPhase.Active);
        var memberView = p2.next(DungeonView.class,
                view -> view.partyId().equals(partyId) && view.phase() == RunPhase.Active);
        sameDungeon(leaderView, memberView);
        require(leaderView.dungeonId().equals(Rules.dungeonId(leaderView.runId()))
                        && leaderView.nodeId().equals(memberView.nodeId())
                        && leaderView.stage() == 0
                        && MyGame.field.PartyRun.validId(leaderView.runId(), "run-")
                        && leaderView.members().equals(members),
                "Party did not enter the same fresh dungeon");
        // Replies from both current Spots confirm deferred joins finished before Advance.
        sameDungeon(leaderView, p1.dungeon());
        sameDungeon(memberView, p2.dungeon());
        return leaderView;
    }

    private void prepare() throws Exception {
        if (advanced != null) return;
        leader = create("dungeon-p1");
        member = create("dungeon-p2");
        leader.join(250);
        member.join(250);
        var s1 = leader.sessionInfo();
        var s2 = member.sessionInfo();
        var initial = enter(leader, member);
        leader.send(new AdvanceRequest());
        advanced = leader.next(DungeonView.class, view -> view.stage() == 1);
        sameDungeon(advanced, member.next(DungeonView.class, view -> view.stage() == 1));
        require(initial.nodeId().equals(advanced.nodeId()), "Advance changed host");
        require(s1.equals(leader.sessionInfo()) && s2.equals(member.sessionInfo()),
                "Dungeon entry changed Session");
    }

    void run(Scenario scenario) throws Exception {
        if (scenario == Scenario.FIELD) {
            var player = create("p1");
            var observer = create("border-p2");
            var observed = observer.join(199);
            var origin = player.join(198);
            require(origin.fieldId().equals("field-2")
                            && origin.nodeId().equals("fieldA")
                            && observed.nodeId().equals("fieldA"),
                    "Field crossing origin mismatch");
            player.send(new JoinPartyRequest("party-" + "0".repeat(32)));
            require(player.next(ActionRejected.class,
                            value -> value.reason().equals(Rejection.INVALID_PARTY)
                                    && value.retryAfterMs() == 0) != null,
                    "Missing invalid-party rejection");
            player.send(new CreatePartyRequest());
            var party = player.next(PartyView.class,
                    view -> view.members().equals(List.of(player.playerId)));
            observer.send(new JoinPartyRequest(party.partyId()));
            var members = List.of(player.playerId, observer.playerId);
            observer.next(PartyView.class, view -> party(view, party.partyId(), members));
            player.next(PartyView.class, view -> party(view, party.partyId(), members));

            player.send(new JoinDungeon("internal", "internal"));
            player.next(ActionRejected.class,
                    value -> value.reason().equals(Rejection.PACKET_NOT_ALLOWED)
                            && value.retryAfterMs() == 0);
            var session = player.sessionInfo();
            player.send(new MoveRequest(2, 1));
            var after = player.next(WorldState.class, view -> view.sequence() == 1);
            require(after.fieldId().equals("field-3")
                            && after.nodeId().equals("fieldB")
                            && after.x() == 200
                            && after.sequence() == 1,
                    "Field crossing destination mismatch");
            var border = observer.next(BorderVisible.class,
                    view -> view.playerId().equals(player.playerId) && view.sequence() == 1);
            require(border.observerFieldId().equals("field-2")
                            && border.sourceFieldId().equals("field-3")
                            && border.x() == 200,
                    "Border publication mismatch");
            require(session.equals(player.sessionInfo())
                            && player.disconnects.get() == 0
                            && observer.disconnects.get() == 0,
                    "Field crossing interrupted connection");

            player.send(new ViewRequest());
            var playerParty = player.next(PartyView.class, view -> view.partyId().equals(party.partyId()));
            observer.send(new ViewRequest());
            var observerParty = observer.next(PartyView.class,
                    view -> view.partyId().equals(party.partyId()));
            require(playerParty.members().equals(members)
                            && observerParty.members().equals(members)
                            && playerParty.leaderId().equals(player.playerId)
                            && observerParty.leaderId().equals(player.playerId),
                    "Cross-field movement changed party membership or leadership");

            for (var entry : List.of(
                    new MoveCheck(new MoveRequest(11, 2), Rejection.STEP_TOO_LARGE),
                    new MoveCheck(new MoveRequest(0, Rules.MAX_SAFE_INTEGER + 1),
                            Rejection.INTEGER_OUT_OF_RANGE),
                    new MoveCheck(new MoveRequest(0, 1), Rejection.STALE_SEQUENCE))) {
                player.send(entry.request());
                var rejected = player.next(ActionRejected.class,
                        value -> value.reason().equals(entry.reason()) && value.retryAfterMs() == 0);
                require(rejected != null, "Missing movement rejection: " + entry.reason());
                player.send(new ViewRequest());
                var state = player.next(WorldState.class);
                require(state.x() == after.x() && state.sequence() == after.sequence(),
                        "Rejected movement changed state");
            }

            player.send(new MoveRequest(0, 2));
            player.send(new MoveRequest(10, 3));
            var accepted = player.next(WorldState.class, view -> view.sequence() == 2);
            var budget = player.next(ActionRejected.class,
                    view -> view.reason().equals(Rejection.MOVE_BUDGET));
            player.send(new ViewRequest());
            var budgetState = player.next(WorldState.class, view -> view.sequence() == 2);
            require(accepted.x() == after.x()
                            && budgetState.x() == after.x()
                            && budgetState.sequence() == 2
                            && budget.retryAfterMs() > 0,
                    "Movement budget rejection changed state: accepted=" + accepted
                            + ", rejection=" + budget + ", current=" + budgetState
                            + ", before=" + after);
            TimeUnit.MILLISECONDS.sleep(budget.retryAfterMs());
            var retried = player.move(10, 3);
            require(retried.x() == 210 && retried.sequence() == 3,
                    "Move after server retry delay failed");
            return;
        }

        prepare();
        switch (scenario) {
            case DUNGEON -> {
                leader.send(new EnterDungeonRequest());
                sameDungeon(advanced, leader.next(DungeonView.class,
                        view -> view.dungeonId().equals(advanced.dungeonId())));
                member.send(new CreatePartyRequest());
                member.next(ActionRejected.class,
                        value -> value.reason().equals(Rejection.RUN_IN_PROGRESS));
                member.send(new JoinPartyRequest("party-" + "0".repeat(32)));
                member.next(ActionRejected.class,
                        value -> value.reason().equals(Rejection.RUN_IN_PROGRESS));
                for (long stage = 2; stage <= Rules.FINAL_STAGE; stage++) {
                    long requestedStage = stage;
                    leader.send(new AdvanceRequest());
                    advanced = leader.next(DungeonView.class,
                            view -> view.stage() == requestedStage);
                    sameDungeon(advanced, member.next(DungeonView.class,
                            view -> view.stage() == requestedStage));
                }
                require(advanced.phase() == RunPhase.Completed, "Final stage did not complete run");
                var completedParty = leader.next(PartyView.class,
                        view -> view.phase() == RunPhase.Completed
                                && Objects.equals(view.runId(), advanced.runId()));
                require(completedParty.partyId().equals(advanced.partyId()),
                        "Completed run was not reported to its Party Spot");
                leader.send(new AdvanceRequest());
                leader.next(ActionRejected.class,
                        value -> value.reason().equals(Rejection.RUN_NOT_ACTIVE));
                sameDungeon(advanced, leader.dungeon());
                if (!selectedScenarios.contains(Scenario.UPDATE)) {
                    var otherLeader = create("other-leader");
                    var otherMember = create("other-member");
                    otherLeader.join(250);
                    otherMember.join(250);
                    var other = enter(otherLeader, otherMember);
                    require(!other.dungeonId().equals(advanced.dungeonId())
                                    && !other.runId().equals(advanced.runId()),
                            "Different parties shared a dungeon run");
                }
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
                require(!before.sessionId().equals(after.sessionId()),
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
                require(!relocated.nodeId().equals(advanced.nodeId())
                                && relocated.nodeId().equals(other.nodeId()),
                        "Dungeon did not relocate together");
                require(s1.equals(leader.sessionInfo()) && s2.equals(member.sessionInfo()),
                        "Host relocation changed Session");
                nodes.replace(advanced.nodeId());
                sameDungeon(relocated, leader.dungeon());
                sameDungeon(relocated, member.dungeon());
                var p3 = create("replacement-p3");
                var p4 = create("replacement-p4");
                p3.join(250);
                p4.join(250);
                var fresh = enter(p3, p4);
                require(fresh.nodeId().equals(advanced.nodeId())
                                && !fresh.dungeonId().equals(advanced.dungeonId())
                                && !fresh.runId().equals(advanced.runId()),
                        "Replacement did not accept new work");
                require(s1.equals(leader.sessionInfo()) && s2.equals(member.sessionInfo()),
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

    private record MoveCheck(MoveRequest request, String reason) {}
}
