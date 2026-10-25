using Systems.Zlink.Stream.Connector.Contracts;
using Systems.Zlink.Stream.Connector;
using MyGame.Shared;

namespace MyGame.E2E;

internal static class Scenarios
{
    private const string Party = "party";
    private const string Reconnect = "reconnect";
    private const string Update = "update";
    public static readonly string[] All = [Party, Reconnect, Update];

    public static string[] Select(string? value)
    {
        var selected = value is null
            ? All
            : value.Split(
                ',',
                StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries
            );
        if (
            selected.Length == 0
            || selected.Distinct().Count() != selected.Length
            || selected.Any(id => !All.Contains(id))
        )
            throw new ArgumentException("Select unique scenarios from: " + string.Join(',', All));
        return All.Where(selected.Contains).ToArray();
    }

    private static void Require(bool condition, string message)
    {
        if (!condition)
            throw new InvalidDataException(message);
    }

    private static void SameActor(SessionInfo before, SessionInfo after) =>
        Require(
            before.ActorId == after.ActorId && before.ObjectGeneration == after.ObjectGeneration,
            "Actor identity changed."
        );

    private static void SameDungeon(DungeonView before, DungeonView after) =>
        Require(
            before.DungeonId == after.DungeonId
                && before.PartyId == after.PartyId
                && before.Stage == after.Stage
                && before.RunId == after.RunId && before.Phase == after.Phase
                && before.Members.SequenceEqual(after.Members),
            "Dungeon state changed."
        );

    private static async Task<DungeonView> EnterAsync(
        GameClient leader,
        GameClient member,
        CancellationToken ct
    )
    {
        var unknown = await leader.SendAsync<ActionRejected, JoinPartyRequest>(new(GameNames.NewPartyId()), _ => true, ct);
        Require(unknown.Reason == Rejection.InvalidParty && unknown.RetryAfterMs == 0, "Unknown party allowed.");
        var created = await leader.SendAsync<PartyView, CreatePartyRequest>(
            new(),
            view =>
                view.Phase == RunPhase.Forming && view.RunId is null
                && view.Members.SequenceEqual(new[] { leader.PlayerId })
                && view.Ready.SequenceEqual(new[] { false }),
            ct
        );
        var party = created.PartyId;
        Require(System.Text.RegularExpressions.Regex.IsMatch(party, "^party-[0-9a-f]{32}$"), "Party ID format.");
        var leaderParty = leader.ReceiveAsync<PartyView>(
            view => view.PartyId == party && view.Members.Length == 2,
            ct
        );
        var joined = await member.SendAsync<PartyView, JoinPartyRequest>(
            new(party),
            view =>
                view.Phase == RunPhase.Forming && view.RunId is null
                && view.Members.SequenceEqual(new[] { leader.PlayerId, member.PlayerId }),
            ct
        );
        Require(
            joined.Ready.SequenceEqual(new[] { false, false }),
            "New party members must not be ready."
        );
        var leaderJoined = await leaderParty;
        Require(
            leaderJoined.Members.SequenceEqual(joined.Members)
                && leaderJoined.Ready.SequenceEqual(joined.Ready),
            "Party broadcast differs."
        );
        var rejected = await leader.SendAsync<ActionRejected, EnterDungeonRequest>(
            new(),
            _ => true,
            ct
        );
        Require(
            rejected.Reason == Rejection.NotReady,
            "Dungeon entry before readiness was not rejected."
        );
        var unchanged = await leader.SendAsync<PartyView, ViewRequest>(new(), _ => true, ct);
        Require(unchanged.Phase == RunPhase.Forming && unchanged.RunId is null
            && unchanged.Ready.SequenceEqual(joined.Ready) && unchanged.Members.SequenceEqual(joined.Members), "Rejected entry changed party.");
        var nonLeader = await member.SendAsync<ActionRejected, EnterDungeonRequest>(new(), _ => true, ct);
        Require(nonLeader.Reason == Rejection.NotLeader && nonLeader.RetryAfterMs == 0, "Non-leader entry rejection.");
        var nonLeaderState = await member.SendAsync<PartyView, ViewRequest>(new(), _ => true, ct);
        Require(nonLeaderState == unchanged || (nonLeaderState.Phase == unchanged.Phase && nonLeaderState.RunId == unchanged.RunId
            && nonLeaderState.Ready.SequenceEqual(unchanged.Ready) && nonLeaderState.Members.SequenceEqual(unchanged.Members)), "Non-leader changed party.");
        var denied = await member.SendAsync<ActionRejected, JoinDungeon>(new("dungeon-forged", "run-forged"), _ => true, ct);
        Require(denied.Reason == Rejection.PacketNotAllowed && denied.RetryAfterMs == 0, "Internal packet allowed.");
        var memberReady = member.ReceiveAsync<PartyView>(
            view => view.Ready.SequenceEqual(new[] { true, false }),
            ct
        );
        await leader.SendAsync<PartyView, ReadyRequest>(
            new(),
            view => view.Ready.SequenceEqual(new[] { true, false }),
            ct
        );
        await memberReady;
        var leaderReady = leader.ReceiveAsync<PartyView>(
            view => view.Ready.All(value => value),
            ct
        );
        await member.SendAsync<PartyView, ReadyRequest>(
            new(),
            view => view.Ready.All(value => value),
            ct
        );
        await leaderReady;
        var memberArrival = member.ReceiveAsync<DungeonView>(view => view.PartyId == party && view.Phase == RunPhase.Active, ct);
        var leaderArrival = leader.ReceiveAsync<DungeonView>(view => view.PartyId == party && view.Phase == RunPhase.Active, ct);
        await leader.Connector.Send(new EnterDungeonRequest()).Async(ct);
        await leader.Connector.Send(new EnterDungeonRequest()).Async(ct);
        var leaderView = await leaderArrival;
        var memberView = await memberArrival;
        SameDungeon(leaderView, memberView);
        Require(
            leaderView.DungeonId == GameNames.DungeonId(leaderView.RunId!)
                && leaderView.NodeId == memberView.NodeId
                && leaderView.Stage == 0 && leaderView.Phase == RunPhase.Active
                && System.Text.RegularExpressions.Regex.IsMatch(leaderView.RunId ?? "", "^run-[0-9a-f]{32}$")
                && leaderView.Members.SequenceEqual(new[] { leader.PlayerId, member.PlayerId }),
            "Party did not enter the same fresh dungeon."
        );
        // A reply from each current Spot confirms both deferred joins finished before Advance.
        SameDungeon(
            leaderView,
            await leader.DungeonAsync(
                view => view.Stage == leaderView.Stage && view.NodeId == leaderView.NodeId && view.Phase == leaderView.Phase && view.RunId == leaderView.RunId,
                ct
            )
        );
        SameDungeon(
            memberView,
            await member.DungeonAsync(
                view => view.Stage == memberView.Stage && view.NodeId == memberView.NodeId && view.Phase == memberView.Phase && view.RunId == memberView.RunId,
                ct
            )
        );
        return leaderView;
    }

    public static async Task RunAsync(
        string endpoint,
        WorldRun run,
        string[] selected,
        CancellationToken ct
    )
    {
        var passed = 0;
        string current = selected[0];
        try
        {
            current = selected[0];
            await using var p1 = await GameClient.ConnectAsync(run.Outcome, endpoint, "p1", ct);
            await p1.JoinAsync(ct);
            var p2 = await GameClient.ConnectAsync(run.Outcome, endpoint, "p2", ct);
            try
            {
                await p2.JoinAsync(ct);
                var before1 = await p1.SessionAsync(ct);
                var before2 = await p2.SessionAsync(ct);
                var dungeon = await EnterAsync(p1, p2, ct);
                var secondStage = p2.ReceiveAsync<DungeonView>(view => view.Stage == 1, ct);
                var advanced = await p1.SendAsync<DungeonView, AdvanceRequest>(
                    new(),
                    view => view.Stage == 1,
                    ct
                );
                SameDungeon(advanced, await secondStage);
                Require(advanced.NodeId == dungeon.NodeId, "Advance changed the dungeon host.");
                Require(
                    before1 == await p1.SessionAsync(ct) && before2 == await p2.SessionAsync(ct),
                    "Session identity changed while entering dungeon."
                );
                if (selected.Contains(Reconnect))
                {
                    current = Reconnect;
                    await p2.DisposeAsync();
                    p2 = await GameClient.ConnectAsync(run.Outcome, endpoint, "p2", ct);
                    var restored = await p2.SendAsync<DungeonView, JoinLobbyRequest>(
                        new("p2"),
                        _ => true,
                        ct
                    );
                    SameDungeon(advanced, restored);
                    Require(
                        restored.NodeId == advanced.NodeId,
                        "Reconnect changed the dungeon host."
                    );
                    var rebound = await p2.SessionAsync(ct);
                    SameActor(before2, rebound);
                    Require(rebound.SessionId != before2.SessionId, "Reconnect reused physical Session.");

                }
                if (selected.Contains(Update))
                {
                    current = Update;
                    var session1 = await p1.SessionAsync(ct);
                    var session2 = await p2.SessionAsync(ct);
                    var source = run.Instances[advanced.NodeId];
                    await source.CommandAsync(NodeProtocol.Drain);
                    await source.WaitLineAsync(NodeProtocol.RelocationRequested, ct);
                    await source.WaitLineAsync(NodeProtocol.Relocated, ct);
                    await source.WaitLineAsync(NodeProtocol.SafeToShutdown, ct);
                    var relocated = await p1.DungeonAsync(
                        view => view.Stage == advanced.Stage && view.NodeId != advanced.NodeId,
                        ct
                    );
                    SameDungeon(advanced, relocated);
                    var relocatedMember = await p2.DungeonAsync(
                        view => view.Stage == relocated.Stage && view.NodeId == relocated.NodeId,
                        ct
                    );
                    SameDungeon(relocated, relocatedMember);
                    Require(
                        relocatedMember.NodeId == relocated.NodeId
                            && relocated.NodeId != advanced.NodeId
                            && run.Instances.ContainsKey(relocated.NodeId),
                        "Dungeon did not move to the other instance."
                    );
                    Require(
                        session1 == await p1.SessionAsync(ct)
                            && session2 == await p2.SessionAsync(ct)
                            && !p1.Disconnected
                            && !p2.Disconnected,
                        "Session continuity failed during host relocation."
                    );
                    await run.ReplaceInstanceAsync(advanced.NodeId, ct);
                    await using var p3 = await GameClient.ConnectAsync(
                        run.Outcome,
                        endpoint,
                        "p3",
                        ct
                    );
                    await p3.JoinAsync(ct);
                    await using var p4 = await GameClient.ConnectAsync(
                        run.Outcome,
                        endpoint,
                        "p4",
                        ct
                    );
                    await p4.JoinAsync(ct);
                    var fresh = await EnterAsync(p3, p4, ct);
                    Require(fresh.DungeonId != advanced.DungeonId && fresh.RunId != advanced.RunId, "Two parties shared a run.");
                    Require(
                        fresh.NodeId == advanced.NodeId,
                        "Replacement instance did not accept new work."
                    );
                    var freshMember = p4.ReceiveAsync<DungeonView>(view => view.Stage == 1, ct);
                    var freshAdvanced = await p3.SendAsync<DungeonView, AdvanceRequest>(new(), view => view.Stage == 1, ct);
                    SameDungeon(freshAdvanced, await freshMember);


                }
                if (selected.Contains(Party))
                {
                    await using var p3 = await GameClient.ConnectAsync(run.Outcome, endpoint, "p3", ct);
                    await using var p4 = await GameClient.ConnectAsync(run.Outcome, endpoint, "p4", ct);
                    await p3.JoinAsync(ct); await p4.JoinAsync(ct);
                    var separate = await EnterAsync(p3, p4, ct);
                    Require(separate.DungeonId != advanced.DungeonId && separate.RunId != advanced.RunId, "Two parties shared a run.");
                }
                for (var stage = 2; stage <= GameNames.FinalStage; stage++)
                {
                    var nextStage = stage;
                    var observed = p2.ReceiveAsync<DungeonView>(view => view.Stage == nextStage, ct);
                    advanced = await p1.SendAsync<DungeonView, AdvanceRequest>(new(), view => view.Stage == nextStage, ct);
                    SameDungeon(advanced, await observed);
                }
                Require(advanced.Phase == RunPhase.Completed, "Final stage did not complete run.");
                var completedRejected = await p1.SendAsync<ActionRejected, AdvanceRequest>(new(), _ => true, ct);
                Require(completedRejected.Reason == Rejection.RunNotActive && completedRejected.RetryAfterMs == 0, "Completed advance allowed.");
                SameDungeon(advanced, await p1.DungeonAsync(view => view.Phase == advanced.Phase && view.Stage == advanced.Stage, ct));
                Console.WriteLine($"✓ {selected[0]}");
                passed++;
            }
            finally
            {
                await p2.DisposeAsync();
            }
        }
        catch
        {
            Console.WriteLine($"✗ {current}");
            throw;
        }
        finally
        {
            Console.WriteLine($"Scenarios: {passed}/{selected.Length} passed");
        }
    }
}
