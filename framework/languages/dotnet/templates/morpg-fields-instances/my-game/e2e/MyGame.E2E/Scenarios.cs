using MyGame.Shared;
using Systems.Zlink.Stream.Connector.Contracts;

namespace MyGame.E2E;

internal static class Scenarios
{
    public static readonly string[] All = ["field", "dungeon", "reconnect", "update"];

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
                && before.Stage == after.Stage && before.RunId == after.RunId && before.Phase == after.Phase
                && before.Members.SequenceEqual(after.Members),
            "Dungeon state changed."
        );

    private static async Task<DungeonView> EnterAsync(
        GameClient leader,
        GameClient member,
        string party,
        CancellationToken ct
    )
    {
        var created = await leader.SendAsync<PartyView, CreatePartyRequest>(new(),
            view => view.Members.SequenceEqual(new[] { leader.PlayerId }), ct);
        party = created.PartyId;
        Require(MyGame.Field.PartyRun.ValidId(party, "party-"), "Party ID is not server-issued.");
        var leaderParty = leader.ReceiveAsync<PartyView>(view => view.Members.Length == 2, ct);
        var joined = await member.SendAsync<PartyView, JoinPartyRequest>(new(party),
            view => view.PartyId == party && view.Members.SequenceEqual(new[] { leader.PlayerId, member.PlayerId }), ct);
        await leaderParty;
        var rejected = await member.SendAsync<ActionRejected, EnterDungeonRequest>(new(),
            view => view.Reason == "not-leader", ct);
        Require(rejected.RetryAfterMs == 0, "Leadership rejection has a cooldown.");
        var unchanged = await member.SendAsync<PartyView, ViewRequest>(new(), _ => true, ct);
        Require(joined.Members.SequenceEqual(unchanged.Members) && joined.Phase == unchanged.Phase
            && unchanged.RunId is null, "Rejected entry changed party state.");
        var memberArrival = member.ReceiveAsync<DungeonView>(view => view.PartyId == party, ct);
        var arrival = leader.ReceiveAsync<DungeonView>(view => view.PartyId == party, ct);
        await leader.Connector.Send(new EnterDungeonRequest()).Async(ct);
        await leader.Connector.Send(new EnterDungeonRequest()).Async(ct);
        var leaderView = await arrival;
        var memberView = await memberArrival;
        SameDungeon(leaderView, memberView);
        Require(
            leaderView.DungeonId == GameNames.DungeonId(leaderView.RunId)
                && leaderView.NodeId == memberView.NodeId
                && leaderView.Stage == 0 && leaderView.Phase == RunPhase.Active
                && MyGame.Field.PartyRun.ValidId(leaderView.RunId, "run-")
                && leaderView.Members.SequenceEqual(new[] { leader.PlayerId, member.PlayerId }),
            "Party did not enter the same fresh dungeon."
        );
        // A reply from each current Spot confirms both deferred joins finished before Advance.
        SameDungeon(leaderView, await leader.DungeonAsync(ct));
        SameDungeon(memberView, await member.DungeonAsync(ct));
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
            if (selected.Contains("field"))
            {
                current = "field";
                await using var player = await GameClient.ConnectAsync(
                    run.Outcome,
                    endpoint,
                    "p1",
                    ct
                );
                await using var observer = await GameClient.ConnectAsync(
                    run.Outcome,
                    endpoint,
                    "border-p2",
                    ct
                );
                var observed = await observer.JoinAsync(199, ct);
                var origin = await player.JoinAsync(198, ct);
                Require(
                    origin.FieldId == "field-2"
                        && origin.NodeId == "fieldA"
                        && observed.NodeId == "fieldA",
                    "Field crossing must start on fieldA."
                );
                await player.SendAsync<ActionRejected, JoinPartyRequest>(new("party-" + new string('0', 32)),
                    view => view.Reason == "invalid-party" && view.RetryAfterMs == 0, ct);
                var party = await player.SendAsync<PartyView, CreatePartyRequest>(new(), _ => true, ct);
                await observer.SendAsync<PartyView, JoinPartyRequest>(new(party.PartyId),
                    view => view.Members.Length == 2, ct);
                await player.SendAsync<ActionRejected, JoinDungeon>(new("internal", "internal"),
                    view => view.Reason == "packet-not-allowed" && view.RetryAfterMs == 0, ct);
                var before = await player.SessionAsync(ct);
                var visible = observer.ReceiveAsync<BorderVisible>(
                    view => view.PlayerId == player.PlayerId && view.Sequence == 1,
                    ct
                );
                var moved = await player.SendAsync<WorldState, MoveRequest>(
                    new(2, 1),
                    view => view.FieldId == "field-3" && view.Sequence == 1,
                    ct
                );
                Require(moved.NodeId == "fieldB" && moved.X == 200, "Field relocation failed.");
                var border = await visible;
                Require(
                    border.ObserverFieldId == "field-2"
                        && border.SourceFieldId == "field-3"
                        && border.X == 200,
                    "Adjacent field did not receive the border publication."
                );
                Require(
                    before == await player.SessionAsync(ct) && !player.Disconnected,
                    "Session changed during field relocation."
                );
                var crossParty = await player.SendAsync<PartyView, ViewRequest>(new(), _ => true, ct);
                var otherParty = await observer.SendAsync<PartyView, ViewRequest>(new(), _ => true, ct);
                Require(crossParty.PartyId == party.PartyId && crossParty.Members.SequenceEqual(otherParty.Members)
                    && crossParty.Members.SequenceEqual(new[] { player.PlayerId, observer.PlayerId })
                    && crossParty.LeaderId == player.PlayerId && otherParty.LeaderId == player.PlayerId,
                    "Cross-field movement changed party membership or leadership.");
                foreach (var (request, reason) in new[] {
                    (new MoveRequest(11, 2), "step-too-large"),
                    (new MoveRequest(0, GameNames.MaxSafeInteger + 1), "integer-out-of-range"),
                    (new MoveRequest(0, 1), "stale-sequence") })
                {
                    await player.SendAsync<ActionRejected, MoveRequest>(request,
                        view => view.Reason == reason && view.RetryAfterMs == 0, ct);
                    var state = await player.SendAsync<WorldState, ViewRequest>(new(), _ => true, ct);
                    Require(state.X == moved.X && state.Sequence == moved.Sequence, "Rejected movement changed state.");
                }
                var accepted = player.ReceiveAsync<WorldState>(view => view.Sequence == 2, ct);
                var budgetRejection = player.ReceiveAsync<ActionRejected>(view => view.Reason == "move-budget", ct);
                await player.Connector.Send(new MoveRequest(0, 2)).Async(ct);
                await player.Connector.Send(new MoveRequest(10, 3)).Async(ct);
                await accepted;
                var budget = await budgetRejection;
                var budgetState = await player.SendAsync<WorldState, ViewRequest>(new(), _ => true, ct);
                Require(budgetState.X == moved.X && budgetState.Sequence == 2 && budget.RetryAfterMs > 0,
                    "Movement budget rejection changed state.");
                await Task.Delay(budget.RetryAfterMs, ct);
                await player.SendAsync<WorldState, MoveRequest>(new(10, 3), view => view.Sequence == 3, ct);
                Console.WriteLine("✓ field");
                passed++;
            }
            if (selected.Any(id => id != "field"))
            {
                current = selected.First(id => id != "field");
                await using var p1 = await GameClient.ConnectAsync(
                    run.Outcome,
                    endpoint,
                    "dungeon-p1",
                    ct
                );
                await p1.JoinAsync(250, ct);
                var p2 = await GameClient.ConnectAsync(run.Outcome, endpoint, "dungeon-p2", ct);
                try
                {
                    await p2.JoinAsync(250, ct);
                    var before1 = await p1.SessionAsync(ct);
                    var before2 = await p2.SessionAsync(ct);
                    var dungeon = await EnterAsync(p1, p2, "party-1", ct);
                    var secondStage = p2.ReceiveAsync<DungeonView>(view => view.Stage == 1, ct);
                    var advanced = await p1.SendAsync<DungeonView, AdvanceRequest>(
                        new(),
                        view => view.Stage == 1,
                        ct
                    );
                    SameDungeon(advanced, await secondStage);
                    Require(advanced.NodeId == dungeon.NodeId, "Advance changed the dungeon host.");
                    for (var stage = 2; stage <= GameNames.FinalStage; stage++)
                        advanced = await p1.SendAsync<DungeonView, AdvanceRequest>(new(), view => view.Stage == stage, ct);
                    Require(advanced.Phase == RunPhase.Completed, "Final stage did not complete run.");
                    await p1.SendAsync<ActionRejected, AdvanceRequest>(new(),
                        view => view.Reason == "run-not-active", ct);
                    SameDungeon(advanced, await p1.DungeonAsync(ct));
                    Require(
                        before1 == await p1.SessionAsync(ct)
                            && before2 == await p2.SessionAsync(ct),
                        "Session identity changed while entering dungeon."
                    );
                    if (selected.Contains("dungeon") && !selected.Contains("update"))
                    {
                        await using var otherLeader = await GameClient.ConnectAsync(run.Outcome, endpoint, "other-leader", ct);
                        await using var otherMember = await GameClient.ConnectAsync(run.Outcome, endpoint, "other-member", ct);
                        await otherLeader.JoinAsync(250, ct); await otherMember.JoinAsync(250, ct);
                        var other = await EnterAsync(otherLeader, otherMember, "", ct);
                        Require(other.DungeonId != dungeon.DungeonId && other.RunId != dungeon.RunId,
                            "Different parties shared a dungeon run.");
                    }
                    if (selected.Contains("dungeon"))
                    {
                        Console.WriteLine("✓ dungeon");
                        passed++;
                    }
                    if (selected.Contains("reconnect"))
                    {
                        current = "reconnect";
                        await p2.DisposeAsync();
                        p2 = await GameClient.ConnectAsync(run.Outcome, endpoint, "dungeon-p2", ct);
                        var restored = await p2.SendAsync<DungeonView, JoinWorldRequest>(
                            new("dungeon-p2", 250),
                            _ => true,
                            ct
                        );
                        SameDungeon(advanced, restored);
                        Require(
                            restored.NodeId == advanced.NodeId,
                            "Reconnect changed the dungeon host."
                        );
                        SameActor(before2, await p2.SessionAsync(ct));
                        Console.WriteLine("✓ reconnect");
                        passed++;
                    }
                    if (selected.Contains("update"))
                    {
                        current = "update";
                        var session1 = await p1.SessionAsync(ct);
                        var session2 = await p2.SessionAsync(ct);
                        var source = run.Instances[advanced.NodeId];
                        await source.CommandAsync(NodeProtocol.Drain);
                        await source.WaitLineAsync(NodeProtocol.RelocationRequested, ct);
                        await source.WaitLineAsync(NodeProtocol.Relocated, ct);
                        await source.WaitLineAsync(NodeProtocol.SafeToShutdown, ct);
                        var relocated = await p1.DungeonAsync(ct, advanced.NodeId);
                        SameDungeon(advanced, relocated);
                        var relocatedMember = await p2.DungeonAsync(ct, advanced.NodeId);
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
                        await p3.JoinAsync(250, ct);
                        await using var p4 = await GameClient.ConnectAsync(
                            run.Outcome,
                            endpoint,
                            "p4",
                            ct
                        );
                        await p4.JoinAsync(250, ct);
                        var fresh = await EnterAsync(p3, p4, "party-2", ct);
                        Require(
                            fresh.NodeId == advanced.NodeId && fresh.DungeonId != advanced.DungeonId
                                && fresh.RunId != advanced.RunId,
                            "Replacement instance did not accept new work."
                        );
                        Console.WriteLine("✓ update");
                        passed++;
                    }
                }
                finally
                {
                    await p2.DisposeAsync();
                }
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
