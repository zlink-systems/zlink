using System.Text.Json;
using MyGame.Shared;

namespace MyGame.E2E;

internal static class Scenarios
{
    private static readonly string[] Order = ["table", "reconnect", "update"];
    internal static IReadOnlyList<string> All => Order;
    private const int FirstBet = 100;
    private const int SecondBet = 50;
    private const int ContinuityBet = 10;

    public static string[] Select(string? requested)
    {
        var selected = requested?.Split(',', StringSplitOptions.TrimEntries) ?? Order;
        if (
            selected.Length == 0
            || selected.Distinct().Count() != selected.Length
            || selected.Any(id => !Order.Contains(id))
        )
            throw new ArgumentException("Select distinct scenarios from table,reconnect,update.");
        return Order.Where(selected.Contains).ToArray();
    }

    private static void Require(bool condition, string message)
    {
        if (!condition)
            throw new InvalidOperationException(message);
    }

    private static bool Equal<T>(T left, T right) =>
        JsonSerializer.Serialize(left) == JsonSerializer.Serialize(right);

    public static async Task RunAsync(
        string endpoint,
        TableRun? run,
        string[] selected,
        CancellationToken ct
    )
    {
        var passed = 0;
        try
        {
            foreach (var scenario in selected)
            {
                try
                {
                    await PlayAsync(endpoint, scenario, run, ct);
                    Console.WriteLine($"✓ {scenario}");
                    passed++;
                }
                catch
                {
                    Console.WriteLine($"✗ {scenario}");
                    throw;
                }
            }
        }
        finally
        {
            Console.WriteLine($"Scenarios: {passed}/{selected.Length} passed");
        }
    }

    private static async Task PlayAsync(
        string endpoint,
        string scenario,
        TableRun? run,
        CancellationToken ct,
        string? expectedNode = null
    )
    {
        var suffix = Guid.NewGuid().ToString("N");
        var table = "table-" + suffix;
        await using var p1 = await GameClient.ConnectAsync(endpoint, "p1-" + suffix, ct);
        try
        {
            await using var p2 = await GameClient.ConnectAsync(endpoint, "p2-" + suffix, ct);
            try
            {
                var joined1 = await p1.JoinAsync(table, ct);
                var joined2 = await p2.JoinAsync(table, ct);
                if (expectedNode is not null)
                    Require(
                        joined1.NodeId == expectedNode && joined2.NodeId == expectedNode,
                        "Replacement node did not host new work."
                    );
                Require(joined1.YourSeat == 0 && joined2.YourSeat == 1, "Initial seats differ.");
                Require(
                    joined2.Seats.Length == GameNames.MinimumPlayers
                        && joined2.Seats.All(seat => seat.Chips == GameNames.InitialChips),
                    "Initial chips differ."
                );
                var session1 = await p1.SessionAsync(ct);
                var session2 = await p2.SessionAsync(ct);
                var cards1 = p1.WaitAsync<HandDealt>(_ => true, ct);
                var cards2 = p2.WaitAsync<HandDealt>(_ => true, ct);
                var started1 = p1.WaitAsync<TableView>(
                    view => view.InHand && view.TurnSeat == 0,
                    ct
                );
                var started2 = p2.WaitAsync<TableView>(
                    view => view.InHand && view.TurnSeat == 0,
                    ct
                );
                await p1.SendAsync(new StartHandRequest(), ct);
                var hand1 = await cards1;
                var hand2 = await cards2;
                var before1 = await started1;
                var before2 = await started2;
                Require(
                    hand1.Cards.Length == GameNames.CardsPerHand
                        && hand2.Cards.Length == GameNames.CardsPerHand
                        && hand1.Cards.Concat(hand2.Cards).Distinct().Count()
                            == GameNames.CardsPerHand * GameNames.MinimumPlayers
                        && hand1
                            .Cards.Concat(hand2.Cards)
                            .All(card => card >= 0 && card < GameNames.DeckSize),
                    "Invalid dealt cards."
                );
                Require(
                    hand1.HandNumber == before1.HandNumber
                        && hand2.HandNumber == before1.HandNumber,
                    "Hand numbers differ."
                );
                if (scenario == "table")
                {
                    var rejected = p2.WaitAsync<ActionRejected>(_ => true, ct);
                    await p2.SendAsync(new BetRequest(hand2.HandNumber, SecondBet), ct);
                    Require(
                        (await rejected).Reason == Rejection.NotYourTurn,
                        "Wrong out-of-turn rejection."
                    );
                }
                var amount1 = scenario == "table" ? FirstBet : ContinuityBet;
                var turn1 = p1.WaitAsync<TableView>(
                    view => view.InHand && view.TurnSeat == 1 && view.Pot == amount1,
                    ct
                );
                var turn2 = p2.WaitAsync<TableView>(
                    view => view.InHand && view.TurnSeat == 1 && view.Pot == amount1,
                    ct
                );
                await p1.SendAsync(new BetRequest(hand1.HandNumber, amount1), ct);
                before1 = await turn1;
                before2 = await turn2;
                PrivateCards(p1, hand1);
                PrivateCards(p2, hand2);

                if (scenario == "reconnect")
                {
                    await p2.DisposeAsync();
                    await using var reconnected = await GameClient.ConnectAsync(
                        endpoint,
                        p2.PlayerId,
                        ct
                    );
                    try
                    {
                        var restored = await reconnected.JoinAsync(table, ct);
                        var restoredCards = await reconnected.WaitAsync<HandDealt>(_ => true, ct);
                        var restoredSession = await reconnected.SessionAsync(ct);
                        Require(
                            restoredSession.SessionId != session2.SessionId
                                && restoredSession.ActorId == session2.ActorId
                                && restoredSession.ObjectGeneration == session2.ObjectGeneration,
                            "Reconnect changed player session binding identity."
                        );
                        SameState(before2, restored);
                        Require(Equal(hand2, restoredCards), "Reconnect changed private cards.");
                        Require(
                            restored.Seats.Single(seat => seat.PlayerId == p2.PlayerId).Chips
                                == GameNames.InitialChips,
                            "Reconnect changed waiting player's chips."
                        );
                        await FinishAsync(p1, reconnected, hand1, hand2, amount1, ct);
                    }
                    catch (Exception failureBeforeCleanup)
                    {
                        Console.Error.WriteLine("Failure before cleanup: " + failureBeforeCleanup);
                        throw;
                    }
                }
                else
                {
                    if (scenario == "update")
                    {
                        await run!.DrainAsync(before1.NodeId, ct);
                        Require(
                            session1 == await p1.SessionAsync(ct)
                                && session2 == await p2.SessionAsync(ct),
                            "Relocation changed a player session binding."
                        );
                        var moved1 = await p1.ViewAsync(view => view.NodeId != before1.NodeId, ct);
                        var moved2 = await p2.ViewAsync(view => view.NodeId != before2.NodeId, ct);
                        SameState(before1, moved1);
                        SameState(before2, moved2);
                        Require(
                            Equal(hand1, await p1.WaitAsync<HandDealt>(_ => true, ct))
                                && Equal(hand2, await p2.WaitAsync<HandDealt>(_ => true, ct)),
                            "Relocation changed cards."
                        );
                    }
                    await FinishAsync(p1, p2, hand1, hand2, amount1, ct);
                    if (scenario == "update")
                    {
                        await run!.ReplaceAsync(before1.NodeId, ct);
                        await PlayAsync(endpoint, "table", null, ct, before1.NodeId);
                    }
                }
                Require(
                    !p1.Disconnected
                        && p1.Connector.IsConnected
                        && await p1.SessionAsync(ct) == session1,
                    "First player connection changed."
                );
                if (scenario != "reconnect")
                    Require(
                        !p2.Disconnected && p2.Connector.IsConnected,
                        "Second player disconnected."
                    );
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

    private static void SameState(TableView before, TableView after) =>
        Require(
            Equal(before with { NodeId = after.NodeId }, after),
            "Continuity changed table state."
        );

    private static void PrivateCards(GameClient client, HandDealt own)
    {
        foreach (var message in client.Received)
        {
            Require(message is not HandResult, "Cards were revealed before settlement.");
            if (message is HandDealt dealt)
                Require(Equal(dealt, own), "Another player's cards were disclosed.");
            if (message is TableView view)
                Require(
                    view.Seats.All(seat => seat.CardCount is 0 or GameNames.CardsPerHand),
                    "Invalid public card count."
                );
        }
    }

    private static async Task FinishAsync(
        GameClient p1,
        GameClient p2,
        HandDealt hand1,
        HandDealt hand2,
        int amount1,
        CancellationToken ct
    )
    {
        PrivateCards(p1, hand1);
        PrivateCards(p2, hand2);
        var result1 = p1.WaitAsync<HandResult>(_ => true, ct);
        var result2 = p2.WaitAsync<HandResult>(_ => true, ct);
        await p2.SendAsync(new BetRequest(hand2.HandNumber, SecondBet), ct);
        var result = await result1;
        Require(Equal(result, await result2), "Players received different settlements.");
        Require(
            result.HandNumber == hand1.HandNumber && result.Pot == amount1 + SecondBet,
            "Settlement pot differs."
        );
        Require(
            result.Hands.Length == GameNames.MinimumPlayers
                && Equal(
                    result.Hands.Single(hand => hand.PlayerId == p1.PlayerId).Cards,
                    hand1.Cards
                )
                && Equal(
                    result.Hands.Single(hand => hand.PlayerId == p2.PlayerId).Cards,
                    hand2.Cards
                ),
            "Revealed cards differ."
        );
        var scores = result
            .Hands.Select(hand =>
                (
                    hand.Seat,
                    Score: hand.Cards.Sum(card => card % GameNames.RankCount + GameNames.LowestRank)
                )
            )
            .ToArray();
        var winners = scores
            .Where(hand => hand.Score == scores.Max(value => value.Score))
            .Select(hand => hand.Seat)
            .Order()
            .ToArray();
        Require(Equal(winners, result.Winners), "Settlement winners differ.");
        foreach (var seat in result.Seats)
        {
            var expected =
                GameNames.InitialChips - (seat.PlayerId == p1.PlayerId ? amount1 : SecondBet);
            if (winners.Contains(seat.Seat))
                expected += result.Pot / winners.Length;
            if (seat.Seat == winners[0])
                expected += result.Pot % winners.Length;
            Require(seat.Chips == expected, "Settlement chips differ.");
        }
        Require(
            result.Seats.Sum(seat => seat.Chips)
                == GameNames.InitialChips * GameNames.MinimumPlayers,
            "Total chips changed."
        );
        foreach (var client in new[] { p1, p2 })
        {
            var settled = await client.WaitAsync<TableView>(
                view => !view.InHand && view.HandNumber == hand1.HandNumber,
                ct
            );
            Require(
                settled.Pot == 0
                    && settled.TurnSeat == GameNames.NoSeat
                    && Equal(settled.Seats, result.Seats),
                "Final public state differs from settlement."
            );
        }
    }
}
