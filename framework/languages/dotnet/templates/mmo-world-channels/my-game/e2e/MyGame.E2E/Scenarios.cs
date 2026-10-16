using System.Net.Http.Json;
using MyGame.Shared;

namespace MyGame.E2E;

internal static class Scenarios
{
    public static readonly string[] All = ["worlds", "channel", "reconnect", "update"];
    private const long EarnedGold = 10;
    private const int SecondChannel = 2;

    public static string[] Select(string? selected)
    {
        var requested =
            selected?.Split(
                ',',
                StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries
            ) ?? All;
        if (
            requested.Length == 0
            || requested.Distinct().Count() != requested.Length
            || requested.Except(All).Any()
        )
            throw new ArgumentException(
                "Select distinct scenarios from worlds,channel,reconnect,update."
            );
        return All.Where(requested.Contains).ToArray();
    }

    private static void Require(bool condition, string message)
    {
        if (!condition)
            throw new InvalidOperationException(message);
    }

    private static async Task WorldsAsync(WorldRun run, CancellationToken ct)
    {
        using var http = new HttpClient();
        var worlds =
            await http.GetFromJsonAsync<WorldInfo[]>(run.AccountsEndpoint + "/worlds", ct)
            ?? throw new InvalidDataException("World list is empty.");
        Require(
            worlds.Length == run.Endpoints.Count
                && worlds.Select(world => world.WorldId).Distinct().Count() == worlds.Length,
            "Accounts did not return distinct worlds."
        );
        foreach (var world in worlds)
            Require(
                world.Name.Length > 0 && run.Endpoints[world.WorldId] == world.SessionEndpoint,
                "Accounts endpoint differs from Session readiness."
            );
        await using var a = await GameClient.ConnectAsync(run.Endpoints["A"], "worlds-player", ct);
        var initial = await a.EnterAsync(ct);
        Require(initial.WorldId == "A" && initial.Gold == 0, "World A initial data differs.");
        Require(
            (await a.EarnAsync(EarnedGold, EarnedGold, ct)).Gold == EarnedGold,
            "World A earning was not persisted."
        );
        await using var b = await GameClient.ConnectAsync(run.Endpoints["B"], a.PlayerId, ct);
        var isolated = await b.EnterAsync(ct);
        Require(
            isolated.WorldId == "B" && isolated.Gold == 0 && isolated.NodeId == "channelB",
            "World B shares World A player data."
        );
        Require(
            (await a.ViewAsync(initial with { Gold = EarnedGold }, ct)).Gold == EarnedGold,
            "Entering World B changed World A data."
        );
    }

    private static async Task ChannelAsync(WorldRun run, CancellationToken ct)
    {
        await using var client = await GameClient.ConnectAsync(
            run.Endpoints["A"],
            "channel-player",
            ct
        );
        var initial = await client.EnterAsync(ct);
        Require(
            initial.Channel == GameNames.FirstChannel && initial.NodeId == "channelA1",
            "Initial Zone is not channelA1."
        );
        await client.EarnAsync(EarnedGold, EarnedGold, ct);
        var binding = await client.SessionAsync(ct);
        var moved = await client.ChangeAsync(SecondChannel, ct);
        Require(
            moved.WorldId == "A"
                && moved.Channel == SecondChannel
                && moved.NodeId == "channelA2"
                && moved.Gold == EarnedGold,
            "Channel relocation lost player state or selected the wrong owner."
        );
        Require(
            !client.Disconnected && await client.SessionAsync(ct) == binding,
            "Channel change replaced the Session or Actor binding."
        );
    }

    private static async Task ReconnectAsync(WorldRun run, CancellationToken ct)
    {
        SessionInfo before;
        PlayerView saved;
        const string player = "reconnect-player";
        await using (var client = await GameClient.ConnectAsync(run.Endpoints["A"], player, ct))
        {
            await client.EnterAsync(ct);
            await client.EarnAsync(EarnedGold, EarnedGold, ct);
            saved = await client.ChangeAsync(SecondChannel, ct);
            before = await client.SessionAsync(ct);
        }
        await using var reconnected = await GameClient.ConnectAsync(run.Endpoints["A"], player, ct);
        Require(await reconnected.EnterAsync(ct) == saved, "Reconnect lost channel or gold.");
        var after = await reconnected.SessionAsync(ct);
        Require(
            after.ActorId == before.ActorId
                && after.ObjectGeneration == before.ObjectGeneration
                && after.SessionId != before.SessionId,
            "Reconnect did not bind the original Actor to a new Session."
        );
    }

    private static async Task UpdateAsync(WorldRun run, CancellationToken ct)
    {
        await using var a = await GameClient.ConnectAsync(run.Endpoints["A"], "update-player", ct);
        await a.EnterAsync(ct);
        var before = await a.EarnAsync(EarnedGold, EarnedGold, ct);
        Require(before.NodeId == "channelA1", "Update subject was not on channelA1.");
        var binding = await a.SessionAsync(ct);
        await using var b = await GameClient.ConnectAsync(run.Endpoints["B"], a.PlayerId, ct);
        var worldB = await b.EnterAsync(ct);
        var bindingB = await b.SessionAsync(ct);
        var source = run.Channels["channelA1"];
        await source.CommandAsync(NodeProtocol.Drain);
        await source.WaitLineAsync(NodeProtocol.RelocationRequested, ct);
        await source.WaitLineAsync(NodeProtocol.Relocated, ct);
        await source.WaitLineAsync(NodeProtocol.SafeToShutdown, ct);
        var moved = await a.ViewAsync(before with { NodeId = "channelA2" }, ct);
        Require(
            moved == before with { NodeId = "channelA2" },
            "Host relocation lost Zone, Actor gold or world isolation."
        );
        Require(
            !a.Disconnected && await a.SessionAsync(ct) == binding,
            "Drain replaced the connected Session or Actor binding."
        );
        Require(
            !b.Disconnected
                && await b.ViewAsync(worldB, ct) == worldB
                && await b.SessionAsync(ct) == bindingB,
            "World B was affected by World A drain."
        );
        await run.ReplaceChannelAsync("channelA1", ct);
        Require(
            await a.ViewAsync(moved, ct) == moved && await a.SessionAsync(ct) == binding,
            "Replacement disturbed relocated state."
        );
        var replacement = run.Channels["channelA1"];
        await replacement.CommandAsync(NodeProtocol.CreateUpdateZone);
        await replacement.WaitLineAsync(NodeProtocol.UpdateZoneCreated, ct);
        await using var newcomer = await GameClient.ConnectAsync(
            run.Endpoints["A"],
            "replacement-player",
            ct
        );
        Require(
            await newcomer.EnterAsync(ct)
                == new PlayerView("A", GameNames.FirstChannel, "channelA2", 0),
            "Replacement deployment did not admit new player work."
        );
        Require(
            await newcomer.ChangeAsync(GameNames.UpdateChannel, ct)
                == new PlayerView("A", GameNames.UpdateChannel, "channelA1", 0),
            "New Zone was not placed on the replacement."
        );
        Require(
            await newcomer.EarnAsync(EarnedGold, EarnedGold, ct)
                == new PlayerView("A", GameNames.UpdateChannel, "channelA1", EarnedGold),
            "New work after replacement failed."
        );
    }

    public static async Task RunAsync(WorldRun run, string[] selected, CancellationToken ct)
    {
        var passed = 0;
        foreach (var scenario in selected)
        {
            try
            {
                switch (scenario)
                {
                    case "worlds":
                        await WorldsAsync(run, ct);
                        break;
                    case "channel":
                        await ChannelAsync(run, ct);
                        break;
                    case "reconnect":
                        await ReconnectAsync(run, ct);
                        break;
                    case "update":
                        await UpdateAsync(run, ct);
                        break;
                }
                passed++;
                Console.WriteLine($"✓ {scenario}");
            }
            catch (Exception error)
            {
                Console.WriteLine($"✗ {scenario}");
                Console.Error.WriteLine($"First failure ({scenario}): {error}");
                foreach (var skipped in selected.SkipWhile(id => id != scenario).Skip(1))
                    Console.WriteLine($"✗ {skipped} (not executed after first failure)");
                // Preserve the first failure instead of repeating a blocked admission path.
                break;
            }
        }
        Console.WriteLine($"Scenarios: {passed}/{selected.Length} passed");
        if (passed != selected.Length)
            throw new InvalidOperationException("Scenario verification failed.");
    }
}
