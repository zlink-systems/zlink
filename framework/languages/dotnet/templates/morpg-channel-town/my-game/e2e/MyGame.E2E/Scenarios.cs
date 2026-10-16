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
        await leader.SendAsync<TownView, CreatePartyRequest>(
            new(party),
            view =>
                view.Parties.Any(item =>
                    item.PartyId == party && item.Members.SequenceEqual(new[] { leader.PlayerId })
                ),
            ct
        );
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
        var memberArrival = member.ReceiveAsync<DungeonView>(view => view.PartyId == party, ct);
        var leaderView = await leader.SendAsync<DungeonView, EnterDungeonRequest>(
            new(),
            view => view.PartyId == party,
            ct
        );
        var memberView = await memberArrival;
        SameDungeon(leaderView, memberView);
        Require(
            leaderView.DungeonId == GameNames.DungeonId(party)
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
                await using var player = await GameClient.ConnectAsync(endpoint, "p1", ct);
                var town = await player.JoinAsync(1, ct);
                Require(town.NodeId == "townA", "Initial town was not hosted on townA.");
                var before = await player.SessionAsync(ct);
                var changed = await player.SendAsync<TownView, ChangeChannelRequest>(
                    new(2),
                    view => view.TownId == GameNames.TownId(2),
                    ct
                );
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
                await using var p1 = await GameClient.ConnectAsync(endpoint, "p1", ct);
                await p1.JoinAsync(2, ct);
                var p2 = await GameClient.ConnectAsync(endpoint, "p2", ct);
                try
                {
                    await p2.JoinAsync(2, ct);
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
                    Require(
                        before1 == await p1.SessionAsync(ct)
                            && before2 == await p2.SessionAsync(ct),
                        "Session identity changed while entering dungeon."
                    );
                    if (selected.Contains("dungeon"))
                    {
                        Console.WriteLine("✓ dungeon");
                        passed++;
                    }
                    if (selected.Contains("reconnect"))
                    {
                        current = "reconnect";
                        await p2.DisposeAsync();
                        p2 = await GameClient.ConnectAsync(endpoint, "p2", ct);
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
                        await using var p3 = await GameClient.ConnectAsync(endpoint, "p3", ct);
                        await p3.JoinAsync(2, ct);
                        await using var p4 = await GameClient.ConnectAsync(endpoint, "p4", ct);
                        await p4.JoinAsync(2, ct);
                        var fresh = await EnterAsync(p3, p4, "party-2", ct);
                        Require(
                            fresh.NodeId == advanced.NodeId,
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
