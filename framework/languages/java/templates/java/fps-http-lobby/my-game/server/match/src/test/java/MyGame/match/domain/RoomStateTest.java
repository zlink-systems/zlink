package MyGame.match.domain;

import static org.junit.jupiter.api.Assertions.*;

import MyGame.*;
import MyGame.Geometry.Aabb;
import MyGame.Geometry.Vec3;
import MyGame.Geometry.ArenaMap;
import MyGame.match.domain.RoomState.CombatantSnapshot;
import MyGame.match.domain.RoomState.RoomSnapshot;

import org.junit.jupiter.api.Test;

import java.util.List;
import java.util.Objects;

final class RoomStateTest {
    @Test
    void matchesFortyFiveDomainBoundaries() throws Exception {
        int[] count = {0};
        var box = new Aabb(new Vec3(1, 1, 1), new Vec3(2, 2, 2));
        check(Objects.equals(box.intersect(new Vec3(0, 1, 1), new Vec3(1, 0, 0), 1), 1.0), "closed face and exact limit", count);
        check(Objects.equals(box.intersect(new Vec3(0, 2, 2), new Vec3(1, 0, 0), 3), 1.0), "parallel corner tangent", count);
        check(box.intersect(new Vec3(0, 2.001, 2), new Vec3(1, 0, 0), 3) == null, "parallel outside slab", count);
        check(Objects.equals(box.intersect(new Vec3(1.5, 1.5, 1.5), new Vec3(1, 0, 0), 3), 0.0), "inside origin", count);
        check(Objects.equals(box.intersect(new Vec3(3, 1, 1), new Vec3(-1, 0, 0), 1), 1.0), "negative direction", count);
        check(box.intersect(new Vec3(0, 1, 1), new Vec3(-1, 0, 0), 3) == null, "box behind ray", count);
        check(box.intersect(new Vec3(0, 1, 1), new Vec3(1, 0, 0), 0.999) == null, "range cutoff", count);
        check(new Vec3(Double.MIN_VALUE, 0, 0).length() == Double.MIN_VALUE, "subnormal movement length", count);
        var huge = new Vec3(Double.MAX_VALUE, Double.MAX_VALUE, 0).tryNormalize();
        check(huge.isPresent() && Math.abs(huge.get().length() - 1) < 1e-12, "large finite aim normalization", count);
        var tiny = new Vec3(Double.MIN_VALUE, 0, 0).tryNormalize();
        check(tiny.isPresent() && tiny.get().equals(new Vec3(1, 0, 0)), "subnormal aim normalization", count);
        check(new Vec3(Double.NaN, 0, 0).tryNormalize().isEmpty()
                && new Vec3(Double.POSITIVE_INFINITY, 0, 0).tryNormalize().isEmpty()
                && new Vec3(0, 0, 0).tryNormalize().isEmpty(), "invalid aim", count);
        for (var spawn : ArenaMap.SPAWNS)
            check(ArenaMap.BOUNDS.contains(spawn)
                    && ArenaMap.WALLS.stream().noneMatch(wall -> wall.contains(spawn)), "spawn outside walls and inside map", count);

        double[] now = {1000.0};
        var room = new RoomState(() -> now[0]);
        room.admit("a");
        room.admit("b");
        check(room.move("a", new MoveRequest(new Vec3(0, 0, -2), 1)) == null, "first movement full budget", count);
        var before = room.capture();
        var budget = room.move("a", new MoveRequest(new Vec3(0, 0, -2), 2));
        check(budget != null && budget.reason().equals(Rejection.MOVE_BUDGET) && budget.retryAfterMs() == 200
                && before.equals(room.capture()), "movement rejection leaves timing and sequence unchanged", count);
        var subnormal = room.move("a", new MoveRequest(new Vec3(Double.MIN_VALUE, 0, 0), 2));
        check(subnormal != null && subnormal.reason().equals(Rejection.MOVE_BUDGET)
                && subnormal.retryAfterMs() > 0 && before.equals(room.capture()), "nonzero subnormal movement with zero elapsed budget leaves state unchanged", count);
        now[0] += 100;
        check(room.move("a", new MoveRequest(new Vec3(0, 0, -1), 2)) == null, "partial elapsed movement budget", count);
        before = room.capture();
        check(Objects.equals(room.move("a", new MoveRequest(new Vec3(0, 0, -3), 3)), new ActionRejected(Rejection.STEP_TOO_LARGE))
                && before.equals(room.capture()), "oversize rejection invariant", count);
        check(Objects.equals(room.move("a", new MoveRequest(new Vec3(0, 1, 0), 3)), new ActionRejected(Rejection.VERTICAL_MOVE))
                && before.equals(room.capture()), "upward movement rejection invariant", count);
        check(Objects.equals(room.move("a", new MoveRequest(new Vec3(0, -1, 0), 3)), new ActionRejected(Rejection.VERTICAL_MOVE))
                && before.equals(room.capture()), "downward movement rejection invariant", count);
        var edge = positioned(new Vec3(0, 0, 65), new Vec3(80, 0, 65), () -> now[0]);
        var edgeBefore = edge.capture();
        check(Objects.equals(edge.move("a", new MoveRequest(new Vec3(-1, 0, 0), 1)), new ActionRejected(Rejection.OUTSIDE_MAP))
                && edgeBefore.equals(edge.capture()), "horizontal map boundary invariant", count);
        check(Objects.equals(room.move("a", new MoveRequest(new Vec3(Double.NaN, 0, 0), 3)), new ActionRejected(Rejection.NON_FINITE))
                && before.equals(room.capture()), "nonfinite movement invariant", count);
        check(room.shoot("a", new ShootRequest(new Vec3(0, 1, 0), 3)).result().outcome().equals(ShotOutcome.MISS), "miss is accepted", count);
        before = room.capture();
        var stale = room.shoot("a", new ShootRequest(new Vec3(1, 0, 0), 3));
        check(stale.rejected() != null && stale.rejected().reason().equals(Rejection.STALE_SEQUENCE)
                && before.equals(room.capture()), "shared stale sequence invariant", count);
        var invalid = room.shoot("a", new ShootRequest(new Vec3(0, 0, 0), 4));
        check(invalid.rejected() != null && invalid.rejected().reason().equals(Rejection.INVALID_AIM)
                && before.equals(room.capture()), "invalid aim leaves cooldown unchanged", count);
        var tooSoon = room.shoot("a", new ShootRequest(new Vec3(0, 1, 0), 4));
        check(tooSoon.rejected() != null && tooSoon.rejected().reason().equals(Rejection.FIRE_TOO_SOON)
                && tooSoon.rejected().retryAfterMs() == Rules.FIRE_INTERVAL_MS && before.equals(room.capture()), "fire interval invariant", count);
        now[0] += Rules.FIRE_INTERVAL_MS;
        check(room.shoot("a", new ShootRequest(new Vec3(0, 1, 0), 4)).result() != null, "exact fire interval", count);
        check(room.move("a", new MoveRequest(new Vec3(0, 0, -2), 5)) == null, "movement budget capped at max step", count);

        var tie = positioned(new Vec3(48, 0, 22), new Vec3(49.5, 0, 22), () -> now[0]);
        var tiedShot = tie.shoot("a", new ShootRequest(new Vec3(1, 0, 0), 1));
        check(tiedShot.result() != null && tiedShot.result().outcome().equals(ShotOutcome.BLOCKED_BY_WALL)
                && tie.view().get(1).hp() == 100, "equal wall and player distance selects wall", count);
        var behind = positioned(ArenaMap.SPAWNS.get(0), ArenaMap.SPAWNS.get(1), () -> now[0]);
        check(behind.shoot("a", new ShootRequest(new Vec3(1, 0, 0), 1)).result().outcome().equals(ShotOutcome.HIT)
                && behind.view().get(1).hp() == 75, "wall behind target does not block", count);
        var exactRange = positioned(new Vec3(0, 0, 65), new Vec3(80.5, 0, 65), () -> now[0]);
        check(exactRange.shoot("a", new ShootRequest(new Vec3(1, 0, 0), 1)).result().outcome().equals(ShotOutcome.HIT), "player face at exact range", count);
        var beyondRange = positioned(new Vec3(0, 0, 65), new Vec3(80.501, 0, 65), () -> now[0]);
        check(beyondRange.shoot("a", new ShootRequest(new Vec3(1, 0, 0), 1)).result().outcome().equals(ShotOutcome.MISS), "player face beyond range", count);
        var crossing = positioned(new Vec3(48, 0, 22), ArenaMap.SPAWNS.get(1), () -> now[0]);
        before = crossing.capture();
        check(Objects.equals(crossing.move("a", new MoveRequest(new Vec3(2, 0, 0), 1)), new ActionRejected(Rejection.MOVE_BLOCKED_BY_WALL))
                && before.equals(crossing.capture()), "movement endpoint inside wall", count);
        var touching = positioned(new Vec3(48, 0, 20), ArenaMap.SPAWNS.get(1), () -> now[0]);
        check(Objects.equals(touching.move("a", new MoveRequest(new Vec3(1, 0, 0), 1)), new ActionRejected(Rejection.MOVE_BLOCKED_BY_WALL)), "movement touches wall face", count);

        now[0] = 1000;
        var source = new RoomState(() -> now[0]);
        source.admit("a");
        source.admit("b");
        source.move("a", new MoveRequest(new Vec3(0, 0, -2), 1));
        source.shoot("a", new ShootRequest(new Vec3(0, 1, 0), 2));
        now[0] += 60;
        var snapshot = source.capture();
        check(snapshot.players().get(0).fireRemainingMs() == 190
                && snapshot.players().get(0).moveRemainingMs() == 140, "snapshot stores remaining milliseconds", count);
        check(snapshot.equals(source.capture()), "capture does not mutate live state", count);
        double[] destinationNow = {900000.0};
        var destination = new RoomState(() -> destinationNow[0]);
        var roundTrip = Json.MAPPER.readValue(Json.MAPPER.writeValueAsBytes(snapshot), RoomSnapshot.class);
        destination.restore(roundTrip);
        check(source.view().equals(destination.view()) && source.phase() == destination.phase(), "relocation keeps view under new clock origin", count);
        var relocatedMove = destination.move("a", new MoveRequest(new Vec3(0, 0, -2), 3));
        check(relocatedMove != null && relocatedMove.reason().equals(Rejection.MOVE_BUDGET)
                && relocatedMove.retryAfterMs() == 140, "relocated movement budget", count);
        var relocatedShot = destination.shoot("a", new ShootRequest(new Vec3(0, 1, 0), 3));
        check(relocatedShot.rejected() != null && relocatedShot.rejected().reason().equals(Rejection.FIRE_TOO_SOON)
                && relocatedShot.rejected().retryAfterMs() == 190, "relocated fire cooldown", count);
        destinationNow[0] += 140;
        check(destination.move("a", new MoveRequest(new Vec3(0, 0, -2), 3)) == null, "relocated budget exact boundary", count);
        destinationNow[0] += 50;
        check(destination.shoot("a", new ShootRequest(new Vec3(0, 1, 0), 4)).result() != null, "relocated cooldown exact boundary", count);
        var lethal = positioned(ArenaMap.SPAWNS.get(0), ArenaMap.SPAWNS.get(1), () -> now[0]);
        for (long sequence = 1; sequence <= 4; sequence++) {
            lethal.shoot("a", new ShootRequest(new Vec3(1, 0, 0), sequence));
            now[0] += Rules.FIRE_INTERVAL_MS;
        }
        check(lethal.phase() == MatchPhase.Finished && "a".equals(lethal.winner())
                && lethal.view().get(1).hp() == 0, "four hits finish match", count);
        before = lethal.capture();
        var finishedMove = lethal.move("a", new MoveRequest(new Vec3(0, 0, 1), 5));
        var finishedShot = lethal.shoot("a", new ShootRequest(new Vec3(1, 0, 0), 5));
        check(finishedMove != null && finishedMove.reason().equals(Rejection.MATCH_FINISHED)
                && finishedShot.rejected() != null && finishedShot.rejected().reason().equals(Rejection.MATCH_FINISHED)
                && before.equals(lethal.capture()), "finished actions leave state unchanged", count);
        var finished = new RoomState(() -> destinationNow[0]);
        finished.restore(before);
        check(finished.phase() == MatchPhase.Finished && "a".equals(finished.winner())
                && finished.view().equals(lethal.view()), "finished snapshot preserves winner and sequence", count);
        check(List.of(room, destination, finished).stream().flatMap(state -> state.view().stream())
                .allMatch(player -> player.position().y() == 0), "feet remain on ground through movement and relocation", count);

        assertEquals(45, count[0], "Domain boundary item count");
        System.out.println("Domain boundaries: " + count[0] + "/" + count[0] + " passed");
    }

    private static RoomState positioned(Vec3 a, Vec3 b, java.util.function.DoubleSupplier clock) {
        var state = new RoomState(clock);
        state.restore(
                new RoomSnapshot(
                        List.of(
                                new CombatantSnapshot("a", a, 100, 0, 0, 0),
                                new CombatantSnapshot("b", b, 100, 0, 0, 0)),
                        MatchPhase.Playing,
                        null));
        return state;
    }

    private static void check(boolean condition, String name, int[] count) {
        assertTrue(condition, "Domain boundary failed: " + name);
        count[0]++;
    }
}
