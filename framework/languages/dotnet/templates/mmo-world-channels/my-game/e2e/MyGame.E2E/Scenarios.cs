using System.Net.Http.Json;
using MyGame.Shared;

namespace MyGame.E2E;

internal static class Scenarios
{
    public static readonly string[] All = ["worlds", "channel", "reconnect", "update"];
    private const long EarnedGold = GameRules.EarnAmount;
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
        await using var a = await GameClient.ConnectAsync(
            run.Outcome,
            run.Endpoints["A"],
            "worlds-player",
            ct
        );
        var initial = await a.EnterAsync(ct);
        Require(initial.WorldId == "A" && initial.Gold == 0, "World A initial data differs.");
        var acceptedEarn = a.SendAsync<PlayerView, EarnRequest>(
            new(),
            view => view.Gold == EarnedGold,
            ct
        );
        var rejectedEarn = a.SendAsync<ActionRejected, EarnRequest>(new(), _ => true, ct);
        Require((await acceptedEarn).Gold == EarnedGold, "World A earning was not persisted.");
        var rejected = await rejectedEarn;
        Require(
            rejected.Reason == Rejection.EarnTooSoon && rejected.RetryAfterMs > 0,
            "A repeated earn did not return its server cooldown."
        );
        Require(
            await a.ViewAsync(initial with { Gold = EarnedGold }, ct)
                == initial with { Gold = EarnedGold },
            "A rejected earn changed the gold balance."
        );
        await Task.Delay(TimeSpan.FromMilliseconds(rejected.RetryAfterMs), ct);
        var earnedTwice = EarnedGold * 2;
        Require(
            (await a.EarnAsync(earnedTwice, ct)).Gold == earnedTwice,
            "An earn was not accepted after the server cooldown."
        );
        await using var b = await GameClient.ConnectAsync(
            run.Outcome,
            run.Endpoints["B"],
            a.PlayerId,
            ct
        );
        var isolated = await b.EnterAsync(ct);
        Require(
            isolated.WorldId == "B" && isolated.Gold == 0 && isolated.NodeId == "channelB",
            "World B shares World A player data."
        );
        Require(
            (await a.ViewAsync(initial with { Gold = earnedTwice }, ct)).Gold == earnedTwice,
            "Entering World B changed World A data."
        );
    }

    private static async Task ChannelAsync(WorldRun run, CancellationToken ct)
    {
        await using var alice = await GameClient.ConnectAsync(
            run.Outcome,
            run.Endpoints["A"],
            "channel-player",
            ct
        );
        await using var charlie = await GameClient.ConnectAsync(
            run.Outcome,
            run.Endpoints["A"],
            "channel-mover",
            ct
        );
        var initial = await alice.EnterAsync(ct);
        Require(
            initial.Channel == GameNames.FirstChannel && initial.NodeId == "channelA1",
            "Initial Zone is not channelA1."
        );
        Task<ChannelView>? afterDisconnect = null;
        await using (var bob = await GameClient.ConnectAsync(
            run.Outcome,
            run.Endpoints["A"],
            "channel-observer",
            ct
        ))
        {
            var visibleToAlice = alice.ReceiveAsync<ChannelView>(
                view =>
                    view.ChannelId == GameNames.FirstChannel
                    && HasPlayers(view, alice.PlayerId, bob.PlayerId, charlie.PlayerId),
                ct
            );
            await bob.EnterAsync(ct);
            var visibleToBob = bob.ReceiveAsync<ChannelView>(
                view =>
                    view.ChannelId == GameNames.FirstChannel
                    && HasPlayers(view, alice.PlayerId, bob.PlayerId, charlie.PlayerId),
                ct
            );
            await charlie.EnterAsync(ct);
            var allowed = await visibleToAlice;
            var bobView = await visibleToBob;
            Require(
                allowed.ChannelId == GameNames.FirstChannel
                    && HasPlayers(allowed, alice.PlayerId, bob.PlayerId, charlie.PlayerId)
                    && bobView.ChannelId == GameNames.FirstChannel
                    && HasPlayers(bobView, alice.PlayerId, bob.PlayerId, charlie.PlayerId),
                "Players in the same channel were not visible."
            );
            var rejected = await alice.SendRejectedAsync(
                new PlayerView("A", GameNames.FirstChannel, "channelA1", 0),
                ct
            );
            Require(
                rejected == new ActionRejected(Rejection.PacketNotAllowed, 0),
                "Session relayed an internal packet."
            );
            var oversizedChannel = await alice.SendRejectedAsync(
                new ChangeChannelRequest(GameRules.MaxInteger + 1),
                ct
            );
            Require(
                oversizedChannel == new ActionRejected(Rejection.IntegerOutOfRange, 0),
                "Oversized channel request was not rejected."
            );
            var afterOversizedChannel = await alice.ChannelViewAsync(
                view =>
                    view.ChannelId == GameNames.FirstChannel
                    && HasPlayers(view, alice.PlayerId, bob.PlayerId, charlie.PlayerId),
                ct
            );
            Require(
                afterOversizedChannel.ChannelId == GameNames.FirstChannel
                    && HasPlayers(afterOversizedChannel, alice.PlayerId, bob.PlayerId, charlie.PlayerId),
                "Oversized channel request changed membership."
            );
            var movedFromFirstChannel = alice.ReceiveAsync<ChannelView>(
                view =>
                    view.ChannelId == GameNames.FirstChannel
                    && HasPlayers(view, alice.PlayerId, bob.PlayerId),
                ct
            );
            var movedCharlie = await charlie.ChangeAsync(SecondChannel, ct);
            Require(
                movedCharlie.Channel == SecondChannel && movedCharlie.NodeId == "channelA2",
                "Channel move did not complete on channelA2."
            );
            var firstChannel = await movedFromFirstChannel;
            var secondChannel = await charlie.ChannelViewAsync(
                view => view.ChannelId == SecondChannel && HasPlayers(view, charlie.PlayerId),
                ct
            );
            Require(
                HasPlayers(firstChannel, alice.PlayerId, bob.PlayerId)
                    && !firstChannel.PlayerIds.Contains(charlie.PlayerId, StringComparer.Ordinal),
                "A channel move left the player in the previous channel list."
            );
            Require(
                secondChannel.ChannelId == SecondChannel
                    && HasPlayers(secondChannel, charlie.PlayerId),
                "A player in another channel was visible."
            );
            afterDisconnect = alice.ReceiveAsync<ChannelView>(
                view => view.ChannelId == GameNames.FirstChannel && HasPlayers(view, alice.PlayerId),
                ct
            );
        }
        var afterDisconnectView = await afterDisconnect!;
        Require(
            HasPlayers(afterDisconnectView, alice.PlayerId),
            "A disconnected player remained in the channel list."
        );

        await alice.EarnAsync(EarnedGold, ct);
        var binding = await alice.SessionAsync(ct);
        var moved = await alice.ChangeAsync(SecondChannel, ct);
        Require(
            moved.WorldId == "A"
                && moved.Channel == SecondChannel
                && moved.NodeId == "channelA2"
                && moved.Gold == EarnedGold,
            "Channel relocation lost player state or selected the wrong owner."
        );
        Require(
            !alice.Disconnected && await alice.SessionAsync(ct) == binding,
            "Channel change replaced the Session or Actor binding."
        );
    }

    private static bool HasPlayers(ChannelView view, params string[] expected) =>
        view.PlayerIds
            .OrderBy(playerId => playerId, StringComparer.Ordinal)
            .SequenceEqual(expected.OrderBy(playerId => playerId, StringComparer.Ordinal));

    private static async Task ReconnectAsync(WorldRun run, CancellationToken ct)
    {
        SessionInfo before;
        PlayerView saved;
        const string player = "reconnect-player";
        await using (
            var client = await GameClient.ConnectAsync(run.Outcome, run.Endpoints["A"], player, ct)
        )
        {
            await client.EnterAsync(ct);
            await client.EarnAsync(EarnedGold, ct);
            saved = await client.ChangeAsync(SecondChannel, ct);
            before = await client.SessionAsync(ct);
        }
        await using var reconnected = await GameClient.ConnectAsync(
            run.Outcome,
            run.Endpoints["A"],
            player,
            ct
        );
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
        await using var a = await GameClient.ConnectAsync(
            run.Outcome,
            run.Endpoints["A"],
            "update-player",
            ct
        );
        var initialChannelView = a.ReceiveAsync<ChannelView>(
            view =>
                view.ChannelId == GameNames.FirstChannel && HasPlayers(view, a.PlayerId),
            ct
        );
        await a.EnterAsync(ct);
        await initialChannelView;
        var before = await a.EarnAsync(EarnedGold, ct);
        Require(before.NodeId == "channelA1", "Update subject was not on channelA1.");
        Require(
            HasPlayers(await a.ChannelViewAsync(ct), a.PlayerId),
            "Channel player list was not available before host relocation."
        );
        var binding = await a.SessionAsync(ct);
        await using var b = await GameClient.ConnectAsync(
            run.Outcome,
            run.Endpoints["B"],
            a.PlayerId,
            ct
        );
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
            HasPlayers(await a.ChannelViewAsync(ct), a.PlayerId),
            "Host relocation lost the Zone player list."
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
            run.Outcome,
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
            await newcomer.EarnAsync(EarnedGold, ct)
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
