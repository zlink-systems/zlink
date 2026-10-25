using Systems.Zlink.Stream.Connector.Contracts;
using MyGame.Shared;

namespace MyGame.E2E;

internal static class Scenarios
{
    public static readonly string[] All = ["channel", "dungeon", "reconnect", "update"];

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
        Require(GameClient.SameDungeonState(before, after), "Dungeon state changed.");

    private static async Task<DungeonView> EnterAsync(
        GameClient leader,
        GameClient member,
        string party,
        CancellationToken ct
    )
    {
        var created = await leader.SendAsync<TownView, CreatePartyRequest>(new(),
            view => view.Parties.Any(item => item.Members.SequenceEqual(new[] { leader.PlayerId })), ct);
        party = created.Parties.Single(item => item.Members.SequenceEqual(new[] { leader.PlayerId })).PartyId;
        Require(DungeonState.ValidId(party, "party-"), "Party ID format.");
        var leaderParty = leader.ReceiveAsync<TownView>(
            view => view.Parties.Any(item => item.PartyId == party && item.Members.Length == 2),
            ct
        );
        await member.SendAsync<TownView, JoinPartyRequest>(
            new(party),
            view =>
                view.Parties.Any(item =>
                    item.PartyId == party
                    && item.Members.SequenceEqual(new[] { leader.PlayerId, member.PlayerId })
                ),
            ct
        );
        await leaderParty;
        var rejected = await member.SendAsync<ActionRejected, EnterDungeonRequest>(new(), _ => true, ct);
        Require(rejected == new ActionRejected("not-leader"), "Nonleader was not rejected.");
        var unchanged = await member.SendAsync<TownView, ViewRequest>(new(), _ => true, ct);
        Require(unchanged.Parties.Single(item => item.PartyId == party).Phase == RunPhase.Forming
            && unchanged.Parties.Single(item => item.PartyId == party).RunId is null, "Rejected entry changed party.");
        var internalRejection = await member.SendAsync<ActionRejected, JoinDungeon>(new("forged", "forged"), _ => true, ct);
        Require(internalRejection == new ActionRejected("packet-not-allowed"), "Session relayed internal packet.");
        var stillUnchanged = await member.SendAsync<TownView, ViewRequest>(new(), _ => true, ct);
        Require(System.Text.Json.JsonSerializer.Serialize(unchanged) == System.Text.Json.JsonSerializer.Serialize(stillUnchanged), "Internal rejection changed party: " + System.Text.Json.JsonSerializer.Serialize(unchanged) + " -> " + System.Text.Json.JsonSerializer.Serialize(stillUnchanged));
        var memberArrival = member.ReceiveAsync<DungeonView>(view => view.PartyId == party && view.Phase == RunPhase.Active, ct);
        var leaderArrival = leader.ReceiveAsync<DungeonView>(view => view.PartyId == party && view.Phase == RunPhase.Active, ct);
        await leader.Connector.Send(new EnterDungeonRequest()).Async(ct);
        await leader.Connector.Send(new EnterDungeonRequest()).Async(ct);
        var leaderView = await leaderArrival;
        var memberView = await memberArrival;
        SameDungeon(leaderView, memberView);
        Require(
            leaderView.DungeonId == GameNames.DungeonId(leaderView.RunId)
                && leaderView.NodeId == memberView.NodeId
                && leaderView.Stage == 0
                && leaderView.Members.SequenceEqual(new[] { leader.PlayerId, member.PlayerId }),
            "Party did not enter the same fresh dungeon."
        );
        // A reply from each current Spot confirms both deferred joins finished before Advance.
        SameDungeon(leaderView, await leader.DungeonAsync(leaderView, ct));
        SameDungeon(memberView, await member.DungeonAsync(memberView, ct));
        return leaderView;
    }

    private static async Task<DungeonView> SecondPartyAsync(string endpoint, WorldRun run, DungeonView first, CancellationToken ct)
    {
        await using var p3 = await GameClient.ConnectAsync(run.Outcome, endpoint, "p3", ct);
        await p3.JoinAsync(1, ct);
        await using var p4 = await GameClient.ConnectAsync(run.Outcome, endpoint, "p4", ct);
        await p4.JoinAsync(1, ct);
        var fresh = await EnterAsync(p3, p4, "", ct);
        Require(fresh.DungeonId != first.DungeonId && fresh.RunId != first.RunId
            && fresh.PartyId != first.PartyId, "Two town parties shared an identity.");
        return fresh;
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
            if (selected.Contains("channel"))
            {
                current = "channel";
                await using var player = await GameClient.ConnectAsync(
                    run.Outcome,
                    endpoint,
                    "p1",
                    ct
                );
                var town = await player.JoinAsync(1, ct);
                Require(town.NodeId == "townA", "Initial town was not hosted on townA.");
                await using var observer = await GameClient.ConnectAsync(run.Outcome, endpoint, "channel-observer", ct);
                await observer.JoinAsync(1, ct);
                var forming = await player.SendAsync<TownView, CreatePartyRequest>(new(),
                    view => view.Parties.Any(party => party.Members.Contains(player.PlayerId)), ct);
                var partyId = forming.Parties.Single(party => party.Members.Contains(player.PlayerId)).PartyId;
                var badChannel = await player.SendAsync<ActionRejected, ChangeChannelRequest>(new(GameNames.MaximumInteger + 1), _ => true, ct);
                Require(badChannel == new ActionRejected("integer-out-of-range"), "Channel range rejection.");
                var unchanged = await player.SendAsync<TownView, ViewRequest>(new(), _ => true, ct);
                Require(System.Text.Json.JsonSerializer.Serialize(forming) == System.Text.Json.JsonSerializer.Serialize(unchanged), "Rejected channel changed town.");
                var sourceLeft = observer.ReceiveAsync<TownView>(view => !view.Players.Contains(player.PlayerId) && view.Parties.All(party => party.PartyId != partyId), ct);
                var before = await player.SessionAsync(ct);
                var changed = await player.SendAsync<TownView, ChangeChannelRequest>(
                    new(2),
                    view => view.TownId == GameNames.TownId(2),
                    ct
                );
                await sourceLeft;
                Require(changed.Parties.All(party => party.PartyId != partyId), "Forming party followed channel change.");
                Require(
                    changed.NodeId == "townB" && changed.Players.Contains("p1"),
                    "Channel relocation failed."
                );
                Require(
                    before == await player.SessionAsync(ct) && !player.Disconnected,
                    "Session changed during channel relocation."
                );
                Console.WriteLine("✓ channel");
                passed++;
            }
            if (selected.Any(id => id != "channel"))
            {
                current = selected.First(id => id != "channel");
                await using var p1 = await GameClient.ConnectAsync(run.Outcome, endpoint, "p1", ct);
                await p1.JoinAsync(2, ct);
                var p2 = await GameClient.ConnectAsync(run.Outcome, endpoint, "p2", ct);
                try
                {
                    await p2.JoinAsync(2, ct);
                    var before1 = await p1.SessionAsync(ct);
                    var before2 = await p2.SessionAsync(ct);
                    var dungeon = await EnterAsync(p1, p2, "party-1", ct);
                    var channelRejected = await p1.SendAsync<ActionRejected, ChangeChannelRequest>(new(1), _ => true, ct);
                    Require(channelRejected == new ActionRejected("run-in-progress"), "Running member changed channel.");
                    SameDungeon(dungeon, await p1.DungeonAsync(dungeon, ct));
                    var secondStage = p2.ReceiveAsync<DungeonView>(view => view.Stage == 1, ct);
                    var advanced = await p1.SendAsync<DungeonView, AdvanceRequest>(
                        new(),
                        view => view.Stage == 1,
                        ct
                    );
                    SameDungeon(advanced, await secondStage);
                    Require(advanced.NodeId == dungeon.NodeId, "Advance changed the dungeon host.");
                    Require(
                        before1 == await p1.SessionAsync(ct)
                            && before2 == await p2.SessionAsync(ct),
                        "Session identity changed while entering dungeon."
                    );
                    for (var stage = 2L; stage <= GameNames.FinalStage; stage++)
                    {
                        var next = stage;
                        var observed = p2.ReceiveAsync<DungeonView>(view => view.Stage == next, ct);
                        advanced = await p1.SendAsync<DungeonView, AdvanceRequest>(new(), view => view.Stage == next, ct);
                        SameDungeon(advanced, await observed);
                    }
                    Require(advanced.Phase == RunPhase.Completed, "Final stage did not complete run.");
                    var completedPartyResult = p1.ReceiveAsync<TownView>(
                        view => view.Parties.Any(item => item.PartyId == advanced.PartyId
                            && item.Phase == RunPhase.Completed && item.RunId == advanced.RunId), ct);
                    await p1.Connector.Send(new ViewRequest()).Async(ct);
                    var completedParty = await completedPartyResult;
                    Require(completedParty.Parties.Single(item => item.PartyId == advanced.PartyId).Phase == RunPhase.Completed,
                        "Party owner did not complete the run.");
                    var completedRejection = await p1.SendAsync<ActionRejected, AdvanceRequest>(new(), _ => true, ct);
                    Require(completedRejection == new ActionRejected("run-not-active"), "Completed advance accepted.");
                    SameDungeon(advanced, await p1.DungeonAsync(advanced, ct));
                    if (selected.Contains("dungeon"))
                    {
                        if (!selected.Contains("update")) await SecondPartyAsync(endpoint, run, advanced, ct);
                        Console.WriteLine("✓ dungeon");
                        passed++;
                    }
                    if (selected.Contains("reconnect"))
                    {
                        current = "reconnect";
                        await p2.DisposeAsync();
                        p2 = await GameClient.ConnectAsync(run.Outcome, endpoint, "p2", ct);
                        var restored = await p2.SendAsync<DungeonView, JoinTownRequest>(
                            new("p2", 2),
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
                        var relocated = await p1.DungeonAsync(advanced, ct, moved: true);
                        SameDungeon(advanced, relocated);
                        var relocatedMember = await p2.DungeonAsync(advanced, ct, moved: true);
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
                        var fresh = await SecondPartyAsync(endpoint, run, advanced, ct);
                        Require(
                            fresh.NodeId == advanced.NodeId && fresh.DungeonId != advanced.DungeonId && fresh.RunId != advanced.RunId,
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
