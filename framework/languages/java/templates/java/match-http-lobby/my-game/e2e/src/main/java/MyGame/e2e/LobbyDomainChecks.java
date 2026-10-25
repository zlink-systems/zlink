package MyGame.e2e;

import MyGame.*;
import MyGame.lobby.application.Matchmaker;
import MyGame.lobby.application.RoomProvisioner;
import MyGame.match.domain.RoomRoster;
import MyGame.match.domain.RoomState;
import MyGame.match.infrastructure.zlink.RoomTransferSnapshot;

import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicLong;

final class LobbyDomainChecks {
    private LobbyDomainChecks() {}

    static void run() throws Exception {
        int[] checks = {0};
        var roster = new RoomRoster();
        check(checks, !roster.canAdmit("a") && !roster.canAdmit("outsider"), "unprepared Room rejects admission");
        check(
                checks,
                rejects(() -> roster.prepare("room", new PrepareRoom("other", List.of("a", "b")))),
                "wrong match identity");
        check(
                checks,
                rejects(() -> roster.prepare("room", new PrepareRoom("room", List.of("a", "a")))),
                "duplicate roster member");
        check(
                checks,
                rejects(() -> roster.prepare("room", new PrepareRoom("room", List.of("a")))),
                "incomplete roster");

        var members = new ArrayList<>(List.of("a", "b"));
        check(
                checks,
                roster.prepare("room", new PrepareRoom("room", members)).equals(new RoomPrepared("room")),
                "preparation acknowledges identity");
        members.set(0, "outsider");
        check(
                checks,
                roster.canAdmit("a") && roster.canAdmit("b") && !roster.canAdmit("outsider"),
                "RoomRoster owns a copy of its member list");

        var rosterSnapshot = roster.capture();
        roster.prepare("room", new PrepareRoom("room", List.of("b", "a")));
        check(checks, rosterSnapshot.equals(roster.capture()), "same preparation is idempotent");
        check(
                checks,
                rejects(() -> roster.prepare("room", new PrepareRoom("room", List.of("a", "outsider"))))
                        && rosterSnapshot.equals(roster.capture()),
                "prepared roster cannot be replaced");

        var sourceClock = new AtomicLong(0);
        var game = new RoomState(sourceClock::get);
        game.admit("a");
        game.admit("b");
        game.move("a", new MoveRequest(1, 0, 1));
        game.shoot("a", new ShootRequest("b", 2));
        var envelope = new RoomTransferSnapshot(game.capture(), roster.capture());
        var encoded = Json.MAPPER.writeValueAsBytes(envelope);
        var envelopeJson = Json.MAPPER.readTree(encoded);
        var restoredEnvelope = Json.MAPPER.readValue(encoded, RoomTransferSnapshot.class);
        var restoredRoster = new RoomRoster();
        restoredRoster.restore(restoredEnvelope.roster());
        var destinationClock = new AtomicLong(900_000_000_000L);
        var restoredGame = new RoomState(destinationClock::get);
        restoredGame.restore(restoredEnvelope.game());
        check(
                checks,
                envelopeJson.has("game")
                        && envelopeJson.has("roster")
                        && restoredRoster.canAdmit("a")
                        && restoredRoster.canAdmit("b")
                        && !restoredRoster.canAdmit("outsider")
                        && game.view().equals(restoredGame.view())
                        && game.phase().equals(restoredGame.phase())
                        && Objects.equals(game.winner(), restoredGame.winner())
                        && game.view().getFirst().lastSequence() == 2,
                "relocation envelope retains roster, state and sequence");
        check(
                checks,
                rejects(() -> restoredRoster.prepare("room", new PrepareRoom("room", List.of("a", "outsider")))),
                "relocated roster cannot be replaced");
        check(
                checks,
                rejects(() -> new RoomRoster().restore(new RoomRoster.RosterSnapshot(null, List.of("a")))),
                "unprepared snapshot with members");

        var provisioner = new RecordingProvisioner();
        try (var matches = new Matchmaker(provisioner)) {
            var defaultA = matches.match(new MatchRequest("default-a", null)).toCompletableFuture();
            var defaultB = matches.match(new MatchRequest("default-b", Names.DEFAULT_MODE)).toCompletableFuture();
            check(checks, defaultA.join().equals(defaultB.join()), "omitted HTTP mode defaults to duel");

            var rankedA = matches.match(new MatchRequest("ra", "ranked")).toCompletableFuture();
            var casualA = matches.match(new MatchRequest("ca", "casual")).toCompletableFuture();
            var rankedB = matches.match(new MatchRequest("rb", "ranked")).toCompletableFuture();
            check(
                    checks,
                    !casualA.isDone() && rankedA.join().equals(rankedB.join()),
                    "interleaved modes pair only matching members");
            var casualB = matches.match(new MatchRequest("cb", "casual")).toCompletableFuture();
            check(
                    checks,
                    casualA.join().equals(casualB.join())
                            && !casualA.join().roomId().equals(rankedA.join().roomId()),
                    "different modes receive distinct Rooms");
            check(
                    checks,
                    provisioner.hasMembers(List.of("default-a", "default-b"))
                            && provisioner.hasMembers(List.of("ra", "rb"))
                            && provisioner.hasMembers(List.of("ca", "cb")),
                    "provisioner receives each matched roster");

            String priorRoom = rankedA.join().roomId();
            String casualRoom = casualA.join().roomId();
            try (var restarted = new Matchmaker(provisioner)) {
                var newA = restarted.match(new MatchRequest("new-a", null)).toCompletableFuture();
                var newB = restarted.match(new MatchRequest("new-b", null)).toCompletableFuture();
                String newRoom = newA.join().roomId();
                check(
                        checks,
                        newA.join().equals(newB.join())
                                && !newRoom.equals(priorRoom)
                                && !newRoom.equals(casualRoom),
                        "restarted lobby creates a new Room ID");
            }
        }
        check(
                checks,
                provisioner.calls.stream().allMatch(call -> call.roomId().matches("room-[0-9a-f]{32}")),
                "Room ID uses prefix and lowercase UUID");

        var pending = new RecordingProvisioner();
        pending.completion = new CompletableFuture<>();
        try (var matches = new Matchmaker(pending)) {
            var waitingA = matches.match(new MatchRequest("held-a", null)).toCompletableFuture();
            var waitingB = matches.match(new MatchRequest("held-b", null)).toCompletableFuture();
            check(
                    checks,
                    !waitingA.isDone() && !waitingB.isDone() && pending.calls.size() == 1,
                    "HTTP assignments wait for RoomPrepared");
            var call = pending.calls.getFirst();
            pending.completion.complete(new MatchResponse(call.roomId(), "ws://127.0.0.1:1"));
            check(checks, waitingA.join().equals(waitingB.join()), "preparation releases both HTTP responses");
        }

        if (checks[0] != 19) throw new IllegalStateException("Expected 19 HTTP domain boundaries, received " + checks[0]);
        System.out.println("HTTP domain boundaries: " + checks[0] + "/" + checks[0] + " passed");
    }

    private static boolean rejects(Runnable action) {
        try {
            action.run();
            return false;
        } catch (IllegalArgumentException | IllegalStateException expected) {
            return true;
        }
    }

    private static void check(int[] count, boolean valid, String name) {
        if (!valid) throw new IllegalStateException("HTTP domain check failed: " + name);
        count[0]++;
    }

    private static final class RecordingProvisioner implements RoomProvisioner {
        private final List<Call> calls = new CopyOnWriteArrayList<>();
        private CompletableFuture<MatchResponse> completion;

        @Override
        public CompletionStage<MatchResponse> prepare(String roomId, List<String> members) {
            calls.add(new Call(roomId, List.copyOf(members)));
            if (completion != null) return completion;
            return CompletableFuture.completedFuture(new MatchResponse(roomId, "tcp://localhost:1"));
        }

        private boolean hasMembers(List<String> expected) {
            var expectedSet = new HashSet<>(expected);
            return calls.stream().anyMatch(call -> new HashSet<>(call.members()).equals(expectedSet));
        }
    }

    private record Call(String roomId, List<String> members) {}
}
