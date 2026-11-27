using System.Text.Json;
using System.Text.Json.Serialization;
using MyGame.Shared;

namespace MyGame.E2E;

internal static class NetworkSyncScenario
{
    public static async Task PlayAsync(string endpoint, RunOutcome outcome, CancellationToken ct)
    {
        var suffix = Guid.NewGuid().ToString("N");
        await using var first = await GameClient.ConnectAsync(outcome, endpoint, "p1-" + suffix, ct);
        await using var second = await GameClient.ConnectAsync(outcome, endpoint, "p2-" + suffix, ct);
        first.Record<NetworkSyncSnapshotV1>();
        second.Record<NetworkSyncSnapshotV1>();
        await first.QueueAsync(ct);
        await second.QueueAsync(ct);
        var match = await first.WaitAsync<MatchFound>(_ => true, ct);
        var otherMatch = await second.WaitAsync<MatchFound>(_ => true, ct);
        Require(match.RoomId == otherMatch.RoomId, "Players joined different rooms.");
        var baseline = await first.WaitAsync<NetworkSyncSnapshotV1>(
            s => s.Anchor.MatchId == match.RoomId && s.State.Phase == MatchPhase.Playing, ct);
        Require(baseline.State.Players.Length == GameNames.PlayersPerRoom
            && baseline.State.Players.All(p => p.Hp == GameNames.InitialHp && p.LastSequence == 0),
            "The initial full state did not contain both players at their starting state.");

        var legacy = await first.ActionAsync(new MoveRequest(new Vec3(1, 0, 0), 1), ct);
        Require(legacy is ActionRejected { Reason: Rejection.PacketNotAllowed },
            "The sync profile allowed a legacy movement event.");

        var original = baseline.State.Players.Single(p => p.PlayerId == first.PlayerId);
        var opponent = baseline.State.Players.Single(p => p.PlayerId == second.PlayerId);
        var aim = opponent.Position - original.Position;
        // Leave time for a feedback snapshot before the target tick on a live server.
        var target = baseline.Anchor.Tick + 15;
        await first.SendAsync(new NetworkSyncInputV1(
            match.RoomId, baseline.Anchor.Epoch, target, 1, baseline.Anchor.Tick,
            new Vec3(1, 0, 0), [new Action(0, aim)]), ct);
        var beforeTick = await first.WaitAsync<NetworkSyncSnapshotV1>(
            s => s.InputFeedback?.TargetTick == target, ct);
        Require(beforeTick.InputFeedback?.Disposition == InputDisposition.Accepted,
            "Target input was not accepted.");
        Require(beforeTick.Anchor.Epoch == baseline.Anchor.Epoch && beforeTick.Anchor.Tick < target
            && Equal(beforeTick.State, baseline.State), "Game state changed before the target tick.");

        bool IsBeforeTarget(NetworkSyncSnapshotV1 snapshot) =>
            snapshot.Anchor.Epoch == baseline.Anchor.Epoch && snapshot.Anchor.Tick < target;

        void RequireUnchangedBeforeTarget(NetworkSyncSnapshotV1 snapshot)
        {
            if (IsBeforeTarget(snapshot) && snapshot.State.Phase == MatchPhase.Playing)
                Require(Equal(snapshot.State, baseline.State), "Game state changed before the target tick.");
        }

        var applied = await first.WaitAsync<NetworkSyncSnapshotV1>(
            s =>
            {
                RequireUnchangedBeforeTarget(s);
                return s.LastAppliedInputTick == target;
            }, ct);
        var peer = await second.WaitAsync<NetworkSyncSnapshotV1>(
            s =>
            {
                RequireUnchangedBeforeTarget(s);
                return s.Anchor == applied.Anchor;
            }, ct);
        Require(applied.Anchor.Epoch == baseline.Anchor.Epoch && applied.Anchor.Tick >= target,
            "Applied input was published before its target tick.");
        var moved = applied.State.Players.Single(p => p.PlayerId == first.PlayerId);
        Require(moved.Position.X > original.Position.X, "The target input did not move the player.");
        var hit = applied.State.Players.Single(p => p.PlayerId == second.PlayerId);
        Require(hit.Hp == opponent.Hp - GameNames.ShotDamage, "The target input did not fire one shot.");
        Require(Equal(applied.State, peer.State), "Clients received different full states for the same tick.");
        Console.WriteLine($"network-sync: accepted before {target}, identical full state at tick {applied.Anchor.Tick}");
    }

    private static bool Equal(RoomView a, RoomView b) =>
        JsonSerializer.Serialize(a) == JsonSerializer.Serialize(b);

    private static void Require(bool condition, string message)
    {
        if (!condition)
            throw new InvalidOperationException(message);
    }

    // Typed wire fixtures for selecting an exact tick and comparing server snapshots.
    private sealed record NetworkSyncInputV1(
        [property: JsonPropertyName("match_id")] string MatchId,
        ulong Epoch,
        [property: JsonPropertyName("target_tick")] ulong TargetTick,
        [property: JsonPropertyName("t1_us")] ulong T1Us,
        [property: JsonPropertyName("last_snapshot_tick")] ulong? LastSnapshotTick,
        Vec3 Input,
        Action[] Actions);

    private sealed record Action(
        [property: JsonPropertyName("action_id")] ulong ActionId, Vec3 Payload);

    private sealed record NetworkSyncSnapshotV1(
        Anchor Anchor,
        RoomView State,
        [property: JsonPropertyName("last_applied_input_tick")] ulong? LastAppliedInputTick,
        [property: JsonPropertyName("input_feedback")] Feedback? InputFeedback);

    private sealed record Anchor(
        [property: JsonPropertyName("match_id")] string MatchId, ulong Epoch, ulong Tick);

    private sealed record Feedback(
        [property: JsonPropertyName("target_tick")] ulong TargetTick, InputDisposition Disposition);

    private enum InputDisposition { Accepted, Late, Duplicate, Rejected }
}
