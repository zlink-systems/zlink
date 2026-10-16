using MyGame.Shared;

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
                && before.Stage == after.Stage
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
        await leader.SendAsync<FieldView, CreatePartyRequest>(
            new(party),
            view =>
                view.Parties.Any(item =>
                    item.PartyId == party && item.Members.SequenceEqual(new[] { leader.PlayerId })
                ),
            ct
        );
        var leaderParty = leader.ReceiveAsync<FieldView>(
            view => view.Parties.Any(item => item.PartyId == party && item.Members.Length == 2),
            ct
        );
        await member.SendAsync<FieldView, JoinPartyRequest>(
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
                await using var player = await GameClient.ConnectAsync(endpoint, "p1", ct);
                try
                {
                    await using var observer = await GameClient.ConnectAsync(
                        endpoint,
                        "border-p2",
                        ct
                    );
                    try
                    {
                        var observed = await observer.JoinAsync(199, ct);
                        var origin = await player.JoinAsync(198, ct);
                        Require(
                            origin.FieldId == "field-2"
                                && origin.NodeId == "fieldA"
                                && observed.NodeId == "fieldA",
                            "Field crossing must start on fieldA."
                        );
                        var before = await player.SessionAsync(ct);
                        var visible = observer.ReceiveAsync<BorderVisible>(
                            view => view.PlayerId == player.PlayerId && view.Sequence == 1,
                            ct
                        );
                        var moved = await player.SendAsync<WorldState, MoveRequest>(
                            new(200, 1),
                            view => view.FieldId == "field-3" && view.Sequence == 1,
                            ct
                        );
                        Require(
                            moved.NodeId == "fieldB" && moved.X == 200,
                            "Field relocation failed."
                        );
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
                        Console.WriteLine("✓ field");
                        passed++;
                    }
                    catch (Exception failureBeforeCleanup)
                    {
                        Console.Error.WriteLine("Failure before cleanup: " + failureBeforeCleanup);
                        throw;
                    }
                }
                catch (Exception failureBeforeCleanup)
                {
                    Console.Error.WriteLine("Failure before cleanup: " + failureBeforeCleanup);
                    throw;
                }
            }
            if (selected.Any(id => id != "field"))
            {
                current = selected.First(id => id != "field");
                await using var p1 = await GameClient.ConnectAsync(endpoint, "dungeon-p1", ct);
                try
                {
                    await p1.JoinAsync(250, ct);
                    var p2 = await GameClient.ConnectAsync(endpoint, "dungeon-p2", ct);
                    try
                    {
                        try
                        {
                            await p2.JoinAsync(250, ct);
                            var before1 = await p1.SessionAsync(ct);
                            var before2 = await p2.SessionAsync(ct);
                            var dungeon = await EnterAsync(p1, p2, "party-1", ct);
                            var secondStage = p2.ReceiveAsync<DungeonView>(
                                view => view.Stage == 1,
                                ct
                            );
                            var advanced = await p1.SendAsync<DungeonView, AdvanceRequest>(
                                new(),
                                view => view.Stage == 1,
                                ct
                            );
                            SameDungeon(advanced, await secondStage);
                            Require(
                                advanced.NodeId == dungeon.NodeId,
                                "Advance changed the dungeon host."
                            );
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
                                p2 = await GameClient.ConnectAsync(endpoint, "dungeon-p2", ct);
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
                                    endpoint,
                                    "p3",
                                    ct
                                );
                                try
                                {
                                    await p3.JoinAsync(250, ct);
                                    await using var p4 = await GameClient.ConnectAsync(
                                        endpoint,
                                        "p4",
                                        ct
                                    );
                                    try
                                    {
                                        await p4.JoinAsync(250, ct);
                                        var fresh = await EnterAsync(p3, p4, "party-2", ct);
                                        Require(
                                            fresh.NodeId == advanced.NodeId,
                                            "Replacement instance did not accept new work."
                                        );
                                        Console.WriteLine("✓ update");
                                        passed++;
                                    }
                                    catch (Exception failureBeforeCleanup)
                                    {
                                        Console.Error.WriteLine(
                                            "Failure before cleanup: " + failureBeforeCleanup
                                        );
                                        throw;
                                    }
                                }
                                catch (Exception failureBeforeCleanup)
                                {
                                    Console.Error.WriteLine(
                                        "Failure before cleanup: " + failureBeforeCleanup
                                    );
                                    throw;
                                }
                            }
                        }
                        catch (Exception failureBeforeCleanup)
                        {
                            Console.Error.WriteLine(
                                "Failure before cleanup: " + failureBeforeCleanup
                            );
                            throw;
                        }
                    }
                    finally
                    {
                        await p2.DisposeAsync();
                    }
                }
                catch (Exception failureBeforeCleanup)
                {
                    Console.Error.WriteLine("Failure before cleanup: " + failureBeforeCleanup);
                    throw;
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
