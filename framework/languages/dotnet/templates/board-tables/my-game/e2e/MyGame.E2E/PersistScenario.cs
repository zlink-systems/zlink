using MyGame.Shared;

namespace MyGame.E2E;

internal static partial class Scenarios
{
    private static async Task VerifyPersistenceAsync(
        string root,
        ServerEndpoints endpoints,
        string tableId,
        SeatView balance,
        RunOutcome outcome,
        RunLog log,
        CancellationToken ct
    )
    {
        Require(
            balance.Chips != GameNames.InitialChips,
            "The settled hand did not change the balance."
        );
        await using var fresh = new TableRun(
            root,
            1,
            outcome,
            Path.Combine(log.DirectoryPath, "persistence-probe"),
            persistenceSource: endpoints
        );
        await fresh.StartAsync(ct);
        await using (
            var stale = await GameClient.ConnectAsync(outcome, fresh.Endpoint, balance.PlayerId, ct)
        )
        {
            await stale.SendJoinAsync(tableId, reconnect: true, ct);
            Require(
                await stale.WaitAsync<ActionRejected>(_ => true, ct)
                    == new ActionRejected(Rejection.NotSeated, 0),
                "Reconnect accepted a seat missing from the fresh deployment."
            );
        }

        await using var player = await GameClient.ConnectAsync(
            outcome,
            fresh.Endpoint,
            balance.PlayerId,
            ct
        );
        var view = await player.JoinAsync(tableId, ct);
        var seat = view.Seats.Single(seat => seat.Seat == view.YourSeat);
        Require(
            (await player.SessionAsync(ct)).ActorId == balance.PlayerId
                && seat.PlayerId == balance.PlayerId,
            "The recreated Actor or seat has the wrong player ID."
        );
        Require(
            seat.Chips == balance.Chips,
            "The recreated Actor did not load the settled balance."
        );
    }
}
