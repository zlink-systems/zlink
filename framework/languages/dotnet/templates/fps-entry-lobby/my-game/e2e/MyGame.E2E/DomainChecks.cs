using System.Text.Json;
using MyGame.Match.Domain;
using MyGame.Shared;

namespace MyGame.E2E;

internal static class DomainChecks
{
    public static void Run()
    {
        var checkedCount = 0;
        void Check(bool condition, string name)
        {
            if (!condition) throw new InvalidOperationException("Domain boundary failed: " + name);
            checkedCount++;
        }
        static bool Equal<T>(T a, T b) => JsonSerializer.Serialize(a) == JsonSerializer.Serialize(b);
        var box = new Aabb(new(1, 1, 1), new(2, 2, 2));
        Check(box.Intersect(new(0, 1, 1), new(1, 0, 0), 1) == 1, "closed face and exact limit");
        Check(box.Intersect(new(0, 2, 2), new(1, 0, 0), 3) == 1, "parallel corner tangent");
        Check(box.Intersect(new(0, 2.001, 2), new(1, 0, 0), 3) is null, "parallel outside slab");
        Check(box.Intersect(new(1.5, 1.5, 1.5), new(1, 0, 0), 3) == 0, "inside origin");
        Check(box.Intersect(new(3, 1, 1), new(-1, 0, 0), 1) == 1, "negative direction");
        Check(box.Intersect(new(0, 1, 1), new(-1, 0, 0), 3) is null, "box behind ray");
        Check(box.Intersect(new(0, 1, 1), new(1, 0, 0), 0.999) is null, "range cutoff");
        Check(new Vec3(double.Epsilon, 0, 0).Length == double.Epsilon,
            "subnormal movement length");
        Check(new Vec3(double.MaxValue, double.MaxValue, 0).TryNormalize(out var huge)
            && Math.Abs(huge.Length - 1) < 1e-12, "large finite aim normalization");
        Check(new Vec3(double.Epsilon, 0, 0).TryNormalize(out var tiny) && tiny == new Vec3(1, 0, 0),
            "subnormal aim normalization");
        Check(!new Vec3(double.NaN, 0, 0).TryNormalize(out _)
            && !new Vec3(double.PositiveInfinity, 0, 0).TryNormalize(out _)
            && !new Vec3().TryNormalize(out _), "invalid aim");
        foreach (var spawn in ArenaMap.Spawns)
            Check(ArenaMap.Bounds.Contains(spawn) && !ArenaMap.Walls.Any(w => w.Contains(spawn)),
                "spawn outside walls and inside map");

        var now = 1000.0;
        var room = new RoomState(() => now);
        room.Admit("a"); room.Admit("b");
        Check(room.Move("a", new(new(0, 0, -2), 1)) is null, "first movement full budget");
        var before = room.Capture();
        var budget = room.Move("a", new(new(0, 0, -2), 2));
        Check(budget is { Reason: Rejection.MoveBudget, RetryAfterMs: 200 }
            && Equal(before, room.Capture()), "movement rejection leaves timing and sequence unchanged");
        Check(room.Move("a", new(new(double.Epsilon, 0, 0), 2))
            is { Reason: Rejection.MoveBudget, RetryAfterMs: > 0 } && Equal(before, room.Capture()),
            "nonzero subnormal movement with zero elapsed budget leaves state unchanged");
        now += 100;
        Check(room.Move("a", new(new(0, 0, -1), 2)) is null, "partial elapsed movement budget");
        before = room.Capture();
        Check(room.Move("a", new(new(0, 0, -3), 3)) is { Reason: Rejection.StepTooLarge }
            && Equal(before, room.Capture()), "oversize rejection invariant");
        Check(room.Move("a", new(new(0, 1, 0), 3)) is { Reason: Rejection.VerticalMove }
            && Equal(before, room.Capture()), "upward movement rejection invariant");
        Check(room.Move("a", new(new(0, -1, 0), 3)) is { Reason: Rejection.VerticalMove }
            && Equal(before, room.Capture()), "downward movement rejection invariant");
        var edge = Positioned(new(0, 0, 65), new(80, 0, 65));
        var edgeBefore = edge.Capture();
        Check(edge.Move("a", new(new(-1, 0, 0), 1)) is { Reason: Rejection.OutsideMap }
            && Equal(edgeBefore, edge.Capture()), "horizontal map boundary invariant");
        Check(room.Move("a", new(new(double.NaN, 0, 0), 3)) is { Reason: Rejection.NonFinite }
            && Equal(before, room.Capture()), "nonfinite movement invariant");
        Check(room.Shoot("a", new(new(0, 1, 0), 3)).Result is { Outcome: ShotOutcome.Miss },
            "miss is accepted");
        before = room.Capture();
        Check(room.Shoot("a", new(new(1, 0, 0), 3)).Rejected is { Reason: Rejection.StaleSequence }
            && Equal(before, room.Capture()), "shared stale sequence invariant");
        Check(room.Shoot("a", new(default, 4)).Rejected is { Reason: Rejection.InvalidAim }
            && Equal(before, room.Capture()), "invalid aim leaves cooldown unchanged");
        Check(room.Shoot("a", new(new(0, 1, 0), 4)).Rejected
            is { Reason: Rejection.FireTooSoon, RetryAfterMs: GameNames.FireIntervalMs }
            && Equal(before, room.Capture()), "fire interval invariant");
        now += GameNames.FireIntervalMs;
        Check(room.Shoot("a", new(new(0, 1, 0), 4)).Result is not null, "exact fire interval");
        Check(room.Move("a", new(new(0, 0, -2), 5)) is null, "movement budget capped at max step");

        RoomState Positioned(Vec3 a, Vec3 b)
        {
            var result = new RoomState(() => now);
            result.Restore(new(new[] {
                new CombatantSnapshot("a", a, 100, 0, 0, 0),
                new CombatantSnapshot("b", b, 100, 0, 0, 0),
            }, MatchPhase.Playing, null));
            return result;
        }
        var tie = Positioned(new(48, 0, 22), new(49.5, 0, 22));
        Check(tie.Shoot("a", new(new(1, 0, 0), 1)).Result is { Outcome: ShotOutcome.BlockedByWall }
            && tie.View()[1].Hp == 100, "equal wall and player distance selects wall");
        var behind = Positioned(ArenaMap.Spawns[0], ArenaMap.Spawns[1]);
        Check(behind.Shoot("a", new(new(1, 0, 0), 1)).Result is { Outcome: ShotOutcome.Hit }
            && behind.View()[1].Hp == 75, "wall behind target does not block");
        var exactRange = Positioned(new(0, 0, 65), new(80.5, 0, 65));
        Check(exactRange.Shoot("a", new(new(1, 0, 0), 1)).Result is { Outcome: ShotOutcome.Hit },
            "player face at exact range");
        var beyondRange = Positioned(new(0, 0, 65), new(80.501, 0, 65));
        Check(beyondRange.Shoot("a", new(new(1, 0, 0), 1)).Result is { Outcome: ShotOutcome.Miss },
            "player face beyond range");
        var crossing = Positioned(new(48, 0, 22), ArenaMap.Spawns[1]);
        before = crossing.Capture();
        Check(crossing.Move("a", new(new(2, 0, 0), 1)) is { Reason: Rejection.MoveBlockedByWall }
            && Equal(before, crossing.Capture()), "movement endpoint inside wall");
        var touching = Positioned(new(48, 0, 20), ArenaMap.Spawns[1]);
        Check(touching.Move("a", new(new(1, 0, 0), 1)) is { Reason: Rejection.MoveBlockedByWall },
            "movement touches wall face");

        now = 1000;
        var source = new RoomState(() => now);
        source.Admit("a"); source.Admit("b");
        source.Move("a", new(new(0, 0, -2), 1));
        source.Shoot("a", new(new(0, 1, 0), 2));
        now += 60;
        var snapshot = source.Capture();
        Check(snapshot.Players[0].FireRemainingMs == 190 && snapshot.Players[0].MoveRemainingMs == 140,
            "snapshot stores remaining milliseconds");
        Check(Equal(snapshot, source.Capture()), "capture does not mutate live state");
        var destinationNow = 900000.0;
        var destination = new RoomState(() => destinationNow);
        destination.Restore(JsonSerializer.Deserialize<RoomSnapshot>(JsonSerializer.Serialize(snapshot))!);
        Check(Equal(source.View(), destination.View()) && source.Phase == destination.Phase,
            "relocation keeps view under new clock origin");
        Check(destination.Move("a", new(new(0, 0, -2), 3))
            is { Reason: Rejection.MoveBudget, RetryAfterMs: 140 }, "relocated movement budget");
        Check(destination.Shoot("a", new(new(0, 1, 0), 3)).Rejected
            is { Reason: Rejection.FireTooSoon, RetryAfterMs: 190 }, "relocated fire cooldown");
        destinationNow += 140;
        Check(destination.Move("a", new(new(0, 0, -2), 3)) is null, "relocated budget exact boundary");
        destinationNow += 50;
        Check(destination.Shoot("a", new(new(0, 1, 0), 4)).Result is not null,
            "relocated cooldown exact boundary");
        var lethal = Positioned(ArenaMap.Spawns[0], ArenaMap.Spawns[1]);
        for (var sequence = 1L; sequence <= 4; sequence++)
        {
            lethal.Shoot("a", new(new(1, 0, 0), sequence));
            now += GameNames.FireIntervalMs;
        }
        Check(lethal.Phase == MatchPhase.Finished && lethal.Winner == "a" && lethal.View()[1].Hp == 0,
            "four hits finish match");
        before = lethal.Capture();
        Check(lethal.Move("a", new(new(0, 0, 1), 5)) is { Reason: Rejection.MatchFinished }
            && lethal.Shoot("a", new(new(1, 0, 0), 5)).Rejected is { Reason: Rejection.MatchFinished }
            && Equal(before, lethal.Capture()), "finished actions leave state unchanged");
        var finished = new RoomState(() => destinationNow);
        finished.Restore(before);
        Check(finished.Phase == MatchPhase.Finished && finished.Winner == "a"
            && Equal(finished.View(), lethal.View()), "finished snapshot preserves winner and sequence");
        Check(room.View().Concat(destination.View()).Concat(finished.View()).All(p => p.Position.Y == 0),
            "feet remain on ground through movement and relocation");
        Console.WriteLine($"Domain boundaries: {checkedCount}/{checkedCount} passed");
    }
}
