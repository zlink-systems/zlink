package MyGame.lobby.application;

import static org.junit.jupiter.api.Assertions.*;

import MyGame.*;
import MyGame.Geometry.Vec3;
import MyGame.match.domain.HttpRoomSnapshot;
import MyGame.match.domain.RoomRoster;
import MyGame.match.domain.RoomState;
import MyGame.match.domain.RoomState.RoomSnapshot;

import org.junit.jupiter.api.Test;

import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Set;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

final class LobbyDomainTest {
    @Test
    void matchesNineteenHttpLobbyBoundaries() throws Exception {
        int[] count = {0};
        var roster = new RoomRoster();
        check(!roster.canAdmit("a") && !roster.canAdmit("outsider"), "unprepared Room rejects admission", count);
        check(rejects(() -> roster.prepare("room", new PrepareRoom("other", List.of("a", "b")))), "wrong match identity", count);
        check(rejects(() -> roster.prepare("room", new PrepareRoom("room", List.of("a", "a")))), "duplicate roster member", count);
        check(rejects(() -> roster.prepare("room", new PrepareRoom("room", List.of("a")))), "incomplete roster", count);
        var members = new ArrayList<>(List.of("a", "b"));
        check(roster.prepare("room", new PrepareRoom("room", members)).matchId().equals("room"), "preparation acknowledges identity", count);
        members.set(0, "outsider");
        check(roster.canAdmit("a") && roster.canAdmit("b") && !roster.canAdmit("outsider"), "roster owns member copy", count);
        var rosterSnapshot = roster.capture();
        roster.prepare("room", new PrepareRoom("room", List.of("b", "a")));
        check(rosterSnapshot.equals(roster.capture()), "same preparation is idempotent", count);
        check(rejects(() -> roster.prepare("room", new PrepareRoom("room", List.of("a", "outsider"))))
                && rosterSnapshot.equals(roster.capture()), "prepared roster cannot be replaced", count);

        double[] now = {1000};
        var game = new RoomState(() -> now[0]);
        game.admit("a");
        game.admit("b");
        game.move("a", new MoveRequest(new Vec3(0, 0, -1), 1));
        game.shoot("a", new ShootRequest(new Vec3(4, 0, 1), 2));
        var envelope = new HttpRoomSnapshot(game.capture(), roster.capture());
        var restored = Json.MAPPER.readValue(Json.MAPPER.writeValueAsBytes(envelope), HttpRoomSnapshot.class);
        var restoredRoster = new RoomRoster();
        restoredRoster.restore(restored.roster());
        var restoredGame = new RoomState(() -> 900000);
        restoredGame.restore(restored.game());
        check(restoredRoster.canAdmit("a") && restoredRoster.canAdmit("b")
                && !restoredRoster.canAdmit("outsider") && game.view().equals(restoredGame.view()),
                "relocation envelope retains roster, HP, position and sequence", count);
        check(rejects(() -> restoredRoster.prepare("room", new PrepareRoom("room", List.of("a", "outsider")))),
                "relocated roster cannot be replaced", count);
        check(rejects(() -> new RoomRoster().restore(new RoomRoster.RosterSnapshot(null, List.of("a")))),
                "unprepared snapshot with members", count);
        check(Json.MAPPER.readValue("{\"playerId\":\"default\"}", MatchRequest.class)
                .mode().equals(Names.DEFAULT_MODE), "omitted HTTP mode defaults to duel", count);

        var provisioner = new RecordingProvisioner();
        try (var firstProcess = new Matchmaker(provisioner)) {
            var rankedA = firstProcess.match(new MatchRequest("ra", "ranked")).toCompletableFuture();
            var casualA = firstProcess.match(new MatchRequest("ca", "casual")).toCompletableFuture();
            var rankedB = firstProcess.match(new MatchRequest("rb", "ranked")).toCompletableFuture();
            check(!casualA.isDone() && rankedA.join().equals(rankedB.join()), "interleaved modes pair only matching members", count);
            var casualB = firstProcess.match(new MatchRequest("cb", "casual")).toCompletableFuture();
            check(casualA.join().equals(casualB.join())
                    && !casualA.join().roomId().equals(rankedA.join().roomId()), "different modes have distinct Rooms", count);
            check(asSet(provisioner.calls.get(0).members()).equals(Set.of("ra", "rb"))
                    && asSet(provisioner.calls.get(1).members()).equals(Set.of("ca", "cb")),
                    "provisioner receives each matched roster", count);

            try (var restarted = new Matchmaker(provisioner)) {
                var newA = restarted.match(new MatchRequest("new-a")).toCompletableFuture();
                var newB = restarted.match(new MatchRequest("new-b")).toCompletableFuture();
                var newRoom = newA.join();
                check(newRoom.equals(newB.join())
                        && !newRoom.roomId().equals(rankedA.join().roomId())
                        && !newRoom.roomId().equals(casualA.join().roomId()),
                        "restart cannot reuse counter-based Room identity", count);
            }
        }
        check(provisioner.calls.stream().allMatch(call -> validRoomId(call.roomId())), "Room ID contains a generated GUID", count);

        var waiting = new RecordingProvisioner();
        waiting.completion = new CompletableFuture<>();
        try (var held = new Matchmaker(waiting)) {
            var heldA = held.match(new MatchRequest("held-a")).toCompletableFuture();
            var heldB = held.match(new MatchRequest("held-b")).toCompletableFuture();
            check(!heldA.isDone() && !heldB.isDone() && waiting.calls.size() == 1,
                    "HTTP assignments wait for Room preparation completion", count);
            var assignment = new MatchResponse(waiting.calls.get(0).roomId(), RecordingProvisioner.SESSION_ENDPOINT);
            waiting.completion.complete(assignment);
            check(heldA.join().equals(heldB.join()), "prepared assignment releases both HTTP responses", count);
        }

        assertEquals(19, count[0], "HTTP domain boundary item count");
        System.out.println("HTTP domain boundaries: " + count[0] + "/" + count[0] + " passed");
    }

    private static boolean rejects(Runnable action) {
        try {
            action.run();
            return false;
        } catch (IllegalArgumentException | IllegalStateException error) {
            return true;
        }
    }

    private static HashSet<String> asSet(List<String> values) {
        return new HashSet<>(values);
    }

    private static boolean validRoomId(String roomId) {
        if (roomId == null || !roomId.startsWith(Names.ROOM_PREFIX)) return false;
        String hex = roomId.substring(Names.ROOM_PREFIX.length());
        if (!hex.matches("[0-9a-f]{32}")) return false;
        String uuid = hex.replaceFirst(
                "^([0-9a-f]{8})([0-9a-f]{4})([0-9a-f]{4})([0-9a-f]{4})([0-9a-f]{12})$",
                "$1-$2-$3-$4-$5");
        return UUID.fromString(uuid) != null;
    }

    private static void check(boolean condition, String name, int[] count) {
        assertTrue(condition, "HTTP domain boundary failed: " + name);
        count[0]++;
    }

    private static final class RecordingProvisioner implements RoomProvisioner {
        static final String SESSION_ENDPOINT = "tcp://127.0.0.1:1";
        private final List<Call> calls = new ArrayList<>();
        private CompletableFuture<MatchResponse> completion;

        @Override
        public CompletionStage<MatchResponse> prepare(String roomId, List<String> members) {
            calls.add(new Call(roomId, List.copyOf(members)));
            if (completion != null) return completion;
            return CompletableFuture.completedFuture(new MatchResponse(roomId, SESSION_ENDPOINT));
        }
    }

    private record Call(String roomId, List<String> members) {}
}
