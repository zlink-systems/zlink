using System.Text.Json;
using MyGame.Lobby.Application;
using MyGame.Match.Domain;
using MyGame.Shared;

namespace MyGame.E2E;

internal static class LobbyDomainChecks
{
    public static async Task RunAsync()
    {
        var count = 0;
        void Check(bool condition, string name)
        {
            if (!condition) throw new InvalidOperationException("HTTP domain boundary failed: " + name);
            count++;
        }
        static bool Equal<T>(T a, T b) => JsonSerializer.Serialize(a) == JsonSerializer.Serialize(b);
        bool Rejects(Action action)
        {
            try { action(); return false; }
            catch (InvalidDataException) { return true; }
            catch (InvalidOperationException) { return true; }
        }
        var roster = new RoomRoster();
        Check(!roster.CanAdmit("a") && !roster.CanAdmit("outsider"), "unprepared Room rejects admission");
        Check(Rejects(() => roster.Prepare("room", new("other", new[] { "a", "b" }))), "wrong match identity");
        Check(Rejects(() => roster.Prepare("room", new("room", new[] { "a", "a" }))), "duplicate roster member");
        Check(Rejects(() => roster.Prepare("room", new("room", new[] { "a" }))), "incomplete roster");
        var members = new[] { "a", "b" };
        Check(roster.Prepare("room", new("room", members)).MatchId == "room", "preparation acknowledges identity");
        members[0] = "outsider";
        Check(roster.CanAdmit("a") && roster.CanAdmit("b") && !roster.CanAdmit("outsider"), "roster owns member copy");
        var snapshot = roster.Capture();
        roster.Prepare("room", new("room", new[] { "b", "a" }));
        Check(Equal(snapshot, roster.Capture()), "same preparation is idempotent");
        Check(Rejects(() => roster.Prepare("room", new("room", new[] { "a", "outsider" })))
            && Equal(snapshot, roster.Capture()), "prepared roster cannot be replaced");
        var game = new RoomState(() => 1000);
        game.Admit("a"); game.Admit("b");
        game.Move("a", new(new(0, 0, -1), 1));
        game.Shoot("a", new(new(4, 0, 1), 2));
        var envelope = new HttpRoomSnapshot(game.Capture(), roster.Capture());
        var restored = JsonSerializer.Deserialize<HttpRoomSnapshot>(JsonSerializer.Serialize(envelope))!;
        var restoredRoster = new RoomRoster(); restoredRoster.Restore(restored.Roster);
        var restoredGame = new RoomState(() => 900000); restoredGame.Restore(restored.Game);
        Check(restoredRoster.CanAdmit("a") && restoredRoster.CanAdmit("b")
            && !restoredRoster.CanAdmit("outsider") && Equal(game.View(), restoredGame.View()),
            "relocation envelope retains roster, HP, position and sequence");
        Check(Rejects(() => restoredRoster.Prepare("room", new("room", new[] { "a", "outsider" }))),
            "relocated roster cannot be replaced");
        Check(Rejects(() => new RoomRoster().Restore(new(null, new[] { "a" }))), "unprepared snapshot with members");
        Check(JsonSerializer.Deserialize<MatchRequest>("{\"PlayerId\":\"default\"}")!.Mode == GameNames.DefaultMode,
            "omitted HTTP mode defaults to duel");

        var provisioner = new RecordingProvisioner();
        var firstProcess = new Matchmaker(provisioner);
        var rankedA = firstProcess.MatchAsync(new("ra", "ranked"), default);
        var casualA = firstProcess.MatchAsync(new("ca", "casual"), default);
        var rankedB = firstProcess.MatchAsync(new("rb", "ranked"), default);
        Check(!casualA.IsCompleted && await rankedA == await rankedB,
            "interleaved modes pair only matching members");
        var casualB = firstProcess.MatchAsync(new("cb", "casual"), default);
        Check(await casualA == await casualB && (await casualA).RoomId != (await rankedA).RoomId,
            "different modes have distinct Rooms");
        Check(provisioner.Calls[0].Members.ToHashSet().SetEquals(new[] { "ra", "rb" })
            && provisioner.Calls[1].Members.ToHashSet().SetEquals(new[] { "ca", "cb" }),
            "provisioner receives each matched roster");
        var restarted = new Matchmaker(provisioner);
        var newA = restarted.MatchAsync(new("new-a"), default);
        var newB = restarted.MatchAsync(new("new-b"), default);
        var newRoom = await newA;
        Check(newRoom == await newB && newRoom.RoomId != (await rankedA).RoomId
            && newRoom.RoomId != (await casualA).RoomId, "restart cannot reuse counter-based Room identity");
        Check(provisioner.Calls.All(call => call.RoomId.StartsWith(GameNames.RoomPrefix, StringComparison.Ordinal)
            && Guid.TryParseExact(call.RoomId[GameNames.RoomPrefix.Length..], "N", out _)),
            "Room ID contains a generated GUID");
        var waiting = new RecordingProvisioner { Completion = new(TaskCreationOptions.RunContinuationsAsynchronously) };
        var held = new Matchmaker(waiting);
        var heldA = held.MatchAsync(new("held-a"), default);
        var heldB = held.MatchAsync(new("held-b"), default);
        Check(!heldA.IsCompleted && !heldB.IsCompleted && waiting.Calls.Count == 1,
            "HTTP assignments wait for Room preparation completion");
        waiting.Completion.SetResult(new(waiting.Calls[0].RoomId, RecordingProvisioner.SessionEndpoint));
        Check(await heldA == await heldB, "prepared assignment releases both HTTP responses");
        Console.WriteLine($"HTTP domain boundaries: {count}/{count} passed");
    }

    private sealed class RecordingProvisioner : IRoomProvisioner
    {
        internal const string SessionEndpoint = "ws://127.0.0.1:1";
        internal List<(string RoomId, string[] Members)> Calls { get; } = [];
        internal TaskCompletionSource<MatchResponse>? Completion { get; init; }
        public Task<MatchResponse> PrepareAsync(string roomId, string[] members, CancellationToken ct)
        {
            Calls.Add((roomId, members));
            return Completion?.Task ?? Task.FromResult(new MatchResponse(roomId, SessionEndpoint));
        }
    }
}
