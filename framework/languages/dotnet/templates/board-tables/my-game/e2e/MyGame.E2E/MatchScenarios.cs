using MyGame.Shared;

namespace MyGame.E2E;

internal static partial class Scenarios
{
    private static async Task MatchAsync(
        string endpoint,
        bool cancelFirst,
        RunOutcome outcome,
        CancellationToken ct
    )
    {
        if (cancelFirst)
            await CancelAsync(endpoint, outcome, ct);

        await using var a = await GameClient.ConnectAsync(outcome, endpoint, "match-a", ct);
        await using var b = await GameClient.ConnectAsync(outcome, endpoint, "match-b", ct);
        var queuedA = await a.FindAsync(ct);
        Require(queuedA == await a.FindAsync(ct), "Duplicate FindMatch created another ticket.");
        var queuedB = await b.FindAsync(ct);
        Require(
            !string.IsNullOrWhiteSpace(queuedA.TicketId)
                && !string.IsNullOrWhiteSpace(queuedB.TicketId)
                && queuedA.TicketId != queuedB.TicketId,
            "Match tickets differ from the expected distinct IDs."
        );
        var foundA = await a.WaitAsync<MatchFound>(_ => true, ct);
        var foundB = await b.WaitAsync<MatchFound>(_ => true, ct);
        Require(
            foundA.TicketId == queuedA.TicketId
                && foundB.TicketId == queuedB.TicketId
                && foundA.RoomId.StartsWith("room-", StringComparison.Ordinal)
                && foundA.RoomId == foundB.RoomId
                && foundA.Seat != foundB.Seat,
            "MatchFound tickets, room or seats differ."
        );
        var viewA = await a.WaitAsync<TableView>(_ => true, ct);
        var viewB = await b.WaitAsync<TableView>(_ => true, ct);
        CheckMatchDelivery(a, foundA, viewA);
        CheckMatchDelivery(b, foundB, viewB);

        if (cancelFirst)
            return;

        var (first, second) = foundA.Seat == 0 ? (a, b) : (b, a);
        var hand1 = first.WaitAsync<HandDealt>(_ => true, ct);
        var hand2 = second.WaitAsync<HandDealt>(_ => true, ct);
        await first.SendAsync(new StartHandRequest(), ct);
        var cards1 = await hand1;
        var cards2 = await hand2;
        Require(
            cards1.Cards.Concat(cards2.Cards).Distinct().Count()
                == GameNames.CardsPerHand * GameNames.MinimumPlayers,
            "Dealt cards overlap."
        );
        await first.SendAsync(new BetRequest(cards1.HandNumber, FirstBet), ct);
        await second.WaitAsync<TableView>(
            view => view.InHand && view.TurnSeat == 1 && view.Pot == FirstBet,
            ct
        );
        await FinishAsync(first, second, cards1, cards2, FirstBet, ct);
    }

    private static void CheckMatchDelivery(GameClient client, MatchFound found, TableView view)
    {
        Require(
            view.TableId == found.RoomId
                && view.YourSeat == found.Seat
                && view.Seats.Length == GameNames.MinimumPlayers
                && view.Seats.Single(seat => seat.Seat == view.YourSeat).PlayerId
                    == client.PlayerId,
            "Committed TableView differs from MatchFound."
        );
        var matchPackets = client
            .Received.Where(message => message is MatchQueued or MatchFound or TableView)
            .ToArray();
        Require(
            matchPackets.First() is MatchQueued
                && Array.IndexOf(matchPackets, found) < Array.IndexOf(matchPackets, view),
            $"Match notifications arrived out of order for {client.PlayerId}: "
                + string.Join(", ", matchPackets.Select(message => message.GetType().Name))
        );
    }

    private static async Task CancelAsync(string endpoint, RunOutcome outcome, CancellationToken ct)
    {
        await using var cancelled = await GameClient.ConnectAsync(outcome, endpoint, "cancel", ct);
        var queued = await cancelled.FindAsync(ct);
        await cancelled.SendAsync(new CancelMatch(queued.TicketId), ct);
        Require(
            await cancelled.WaitAsync<MatchCancelled>(_ => true, ct)
                == new MatchCancelled(queued.TicketId, MatchNames.Cancelled),
            "Cancellation ticket or reason differs."
        );
    }
}
