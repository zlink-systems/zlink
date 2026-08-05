using Bingo.Shared.Contracts;
using Systems.Zlink.Stream.Connector.Contracts;

namespace Bingo.Client;

internal sealed class BingoClientScenario
{
    // End-to-end client story:
    // 1. Connect player 1, authenticate, and create a waiting two-player bingo room.
    // 2. Subscribe an observer to room events through the room's global identifier.
    // 3. Connect player 2, join the same room, and verify join/start pushes.
    // 4. Submit both bingo cards and wait for server-driven number draws.
    // 5. Verify the finished game state, winner, marked cards, and matching client results.
    // 6. Verify the observer receives the reward announcement, then stop observing.
    public async ValueTask RunAsync(
        IZlinkStreamConnector client1,
        IZlinkStreamConnector client2,
        IZlinkStreamConnector observer,
        CancellationToken cancellationToken = default)
    {
        // Client 1 connects, authenticates, and creates the waiting room.
        await client1.Connect.Async(cancellationToken);
        var client1Auth = await client1.Request(new AuthenticateReq { AccessToken = BingoSamplePlayers.Player1 })
            .Async<AuthenticateRes>(cancellationToken);

        ZlinkStreamAssert.Ensure(client1Auth.ActorId == BingoSamplePlayers.Player1, "Assertion failed: client1Auth.ActorId == BingoSamplePlayers.Player1");

        var client1MatchRes = await MatchAsync(
            client1,
            BingoSampleModes.TwoPlayer,
            cancellationToken);

        ZlinkStreamAssert.Ensure(client1MatchRes.State.Status == BingoRoomStatuses.WaitingForPlayers, "Assertion failed: client1MatchRes.State.Status == BingoRoomStatuses.WaitingForPlayers");
        ZlinkStreamAssert.Ensure(client1MatchRes.State.HostActorId == client1Auth.ActorId, "Assertion failed: client1MatchRes.State.HostActorId == client1Auth.ActorId");
        await client1.ExpectNone<PlayerJoinedNotify>()
            .Within(TimeSpan.FromMilliseconds(250))
            .Async(cancellationToken);

        await observer.Connect.Async(cancellationToken);
        var observerAuth = await observer.Request(new AuthenticateReq { AccessToken = BingoSamplePlayers.Observer })
            .Async<AuthenticateRes>(cancellationToken);
        ZlinkStreamAssert.Ensure(observerAuth.ActorId == BingoSamplePlayers.Observer, "Assertion failed: observerAuth.ActorId == BingoSamplePlayers.Observer");

        var observed = await ObserveAsync(
            observer,
            client1MatchRes.RoomId,
            cancellationToken);
        ZlinkStreamAssert.Ensure(observed.Subscribed, "Assertion failed: observed.Subscribed");

        // Register before the room can finish so the reward push cannot race past the observer.
        var rewardTask = observer.WaitFor<BingoRewardAnnouncedNotify>()
            .Where(message => message.Payload.RoomId == client1MatchRes.RoomId)
            .Async(cancellationToken).AsTask();

        // Client 2 connects, authenticates, and joins the same room.
        await client2.Connect.Async(cancellationToken);

        var client2Auth = await client2.Request(new AuthenticateReq { AccessToken = BingoSamplePlayers.Player2 })
            .Async<AuthenticateRes>(cancellationToken);

        ZlinkStreamAssert.Ensure(client2Auth.ActorId == BingoSamplePlayers.Player2, "Assertion failed: client2Auth.ActorId == BingoSamplePlayers.Player2");
        ZlinkStreamAssert.Ensure(client2Auth.ActorId != client1Auth.ActorId, "Assertion failed: client2Auth.ActorId != client1Auth.ActorId");

        var client2MatchRes = await MatchAsync(
            client2,
            BingoSampleModes.TwoPlayer,
            cancellationToken);

        ZlinkStreamAssert.Ensure(client2MatchRes.RoomId == client1MatchRes.RoomId, "Assertion failed: client2MatchRes.RoomId == client1MatchRes.RoomId");
        ZlinkStreamAssert.Ensure(client2MatchRes.State.Status == BingoRoomStatuses.Running, "Assertion failed: client2MatchRes.State.Status == BingoRoomStatuses.Running");

        // Joining another player is delivered as a push to existing room members.
        var client1SawClient2Join = await client1.WaitFor<PlayerJoinedNotify>()
            .Where(message => message.Payload.ActorId == client2Auth.ActorId)
            .Async(cancellationToken);

        ZlinkStreamAssert.Ensure(client1SawClient2Join.Payload.ActorId == client2Auth.ActorId, "Assertion failed: client1SawClient2Join.Payload.ActorId == client2Auth.ActorId");
        ZlinkStreamAssert.Ensure(
            client1SawClient2Join.Payload.State.Players.All(
                static player => player.Wins == 0 && player.Losses == 0), "Assertion failed: client1SawClient2Join.Payload.State.Players.All( static player => player.Wins == 0 && player.Losses == 0)");
        await client2.ExpectNone<PlayerJoinedNotify>()
            .Within(TimeSpan.FromMilliseconds(250))
            .Async(cancellationToken);

        // The room starts automatically after both players have joined.
        var client1StartedTask = client1.WaitFor<BingoGameStartedNotify>().Async(cancellationToken).AsTask();
        var client2StartedTask = client2.WaitFor<BingoGameStartedNotify>().Async(cancellationToken).AsTask();
        await Task.WhenAll(client1StartedTask, client2StartedTask);

        var client1Started = await client1StartedTask;
        ZlinkStreamAssert.Ensure(client1Started.Payload.State.Status == BingoRoomStatuses.Running, "Assertion failed: client1Started.Payload.State.Status == BingoRoomStatuses.Running");
        var client2Started = await client2StartedTask;
        ZlinkStreamAssert.Ensure(client2Started.Payload.State.Status == BingoRoomStatuses.Running, "Assertion failed: client2Started.Payload.State.Status == BingoRoomStatuses.Running");

        // Each client submits its bingo card; the server starts drawing after both cards arrive.
        var client2Card = await client2
            .Request(new SubmitBingoCardReq
            {
                RoomId = client2MatchRes.RoomId,
                Card = { BingoClientCards.Player2 }
            })
            .Async<SubmitBingoCardRes>(cancellationToken);
        ZlinkStreamAssert.Ensure(client2Card.State.Status == BingoRoomStatuses.Running, "Assertion failed: client2Card.State.Status == BingoRoomStatuses.Running");

        var client1Card = await client1
            .Request(new SubmitBingoCardReq
            {
                RoomId = client1MatchRes.RoomId,
                Card = { BingoClientCards.Player1 }
            })
            .Async<SubmitBingoCardRes>(cancellationToken);
        ZlinkStreamAssert.Ensure(client1Card.State.Status == BingoRoomStatuses.Running, "Assertion failed: client1Card.State.Status == BingoRoomStatuses.Running");
        // The second response is the first state captured after both submissions, so it must
        // contain both complete cards rather than only acknowledging the last request.
        ZlinkStreamAssert.Ensure(client1Card.State.Players.Count == 2, "Assertion failed: client1Card.State.Players.Count == 2");
        ZlinkStreamAssert.Ensure(client1Card.State.Players.All(static player => player.Card.Count == 9), "Assertion failed: client1Card.State.Players.All(static player => player.Card.Count == 9)");

        var drawnNumbers = new List<BingoNumberDrawnNotify>();
        // Number drawing is server-driven; clients only wait for draw notifications.
        while (true)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var expectedSeq = drawnNumbers.Count + 1;
            var client1DrawnTask = client1.WaitFor<BingoNumberDrawnNotify>()
                .Where(message => message.Payload.DrawSeq == expectedSeq)
                .Async(cancellationToken).AsTask();
            var client2DrawnTask = client2.WaitFor<BingoNumberDrawnNotify>()
                .Where(message => message.Payload.DrawSeq == expectedSeq)
                .Async(cancellationToken).AsTask();
            await Task.WhenAll(client1DrawnTask, client2DrawnTask);

            var client1Drawn = await client1DrawnTask;
            var client2Drawn = await client2DrawnTask;
            drawnNumbers.Add(client1Drawn.Payload);
            ZlinkStreamAssert.Ensure(client1Drawn.Payload.DrawSeq == expectedSeq, "Assertion failed: client1Drawn.Payload.DrawSeq == expectedSeq");
            ZlinkStreamAssert.Ensure(client2Drawn.Payload.DrawSeq == expectedSeq, "Assertion failed: client2Drawn.Payload.DrawSeq == expectedSeq");
            ZlinkStreamAssert.Ensure(client2Drawn.Payload.DrawSeq == client1Drawn.Payload.DrawSeq, "Assertion failed: client2Drawn.Payload.DrawSeq == client1Drawn.Payload.DrawSeq");
            ZlinkStreamAssert.Ensure(client2Drawn.Payload.Number == client1Drawn.Payload.Number, "Assertion failed: client2Drawn.Payload.Number == client1Drawn.Payload.Number");
            // Both clients must observe the same room snapshot for this draw, not merely the
            // same sequence number and drawn number.
            ZlinkStreamAssert.Ensure(client2Drawn.Payload.State.Equals(client1Drawn.Payload.State), "Assertion failed: client2Drawn.Payload.State.Equals(client1Drawn.Payload.State)");

            if (client1Drawn.Payload.State.Status == BingoRoomStatuses.Finished) break;
        }

        ZlinkStreamAssert.Ensure(drawnNumbers.Count > 0, "Assertion failed: drawnNumbers.Count > 0");
        ZlinkStreamAssert.Ensure(drawnNumbers[^1].State.Status == BingoRoomStatuses.Finished, "Assertion failed: drawnNumbers[^1].State.Status == BingoRoomStatuses.Finished");

        // The final result is pushed to both clients when the server detects bingo.
        var client1ResultTask = client1.WaitFor<BingoGameEndedNotify>()
            .Where(message => message.Payload.State.Status == BingoRoomStatuses.Finished)
            .Async(cancellationToken).AsTask();
        var client2ResultTask = client2.WaitFor<BingoGameEndedNotify>()
            .Where(message => message.Payload.State.Status == BingoRoomStatuses.Finished)
            .Async(cancellationToken).AsTask();
        await Task.WhenAll(client1ResultTask, client2ResultTask);

        var client1Result = await client1ResultTask;
        ZlinkStreamAssert.Ensure(client1Result.Payload.State.Status == BingoRoomStatuses.Finished, "Assertion failed: client1Result.Payload.State.Status == BingoRoomStatuses.Finished");
        var client2Result = await client2ResultTask;
        ZlinkStreamAssert.Ensure(client2Result.Payload.State.Status == BingoRoomStatuses.Finished, "Assertion failed: client2Result.Payload.State.Status == BingoRoomStatuses.Finished");
        ZlinkStreamAssert.Ensure(
            client2Result.Payload.State.DrawnNumbers.SequenceEqual(client1Result.Payload.State.DrawnNumbers), "Assertion failed: client2Result.Payload.State.DrawnNumbers.SequenceEqual(client1Result.Payload.State.DrawnNumbers)");
        ZlinkStreamAssert.Ensure(
            client2Result.Payload.State.Winners.SequenceEqual(client1Result.Payload.State.Winners), "Assertion failed: client2Result.Payload.State.Winners.SequenceEqual(client1Result.Payload.State.Winners)");
        ZlinkStreamAssert.Ensure(
            client2Result.Payload.State.Players.Select(static player => player.ActorId)
                .SequenceEqual(client1Result.Payload.State.Players.Select(static player => player.ActorId)), "Assertion failed: client2Result.Payload.State.Players.Select(static player => player.ActorId) .SequenceEqual(client1Result.Payload.State.Players.Select(static player => player.ActorId))");

        ZlinkStreamAssert.Ensure(
            client1Result.Payload.State.DrawnNumbers.SequenceEqual(drawnNumbers.Select(static draw => draw.Number)), "Assertion failed: client1Result.Payload.State.DrawnNumbers.SequenceEqual(drawnNumbers.Select(static draw => draw.Number))");
        ZlinkStreamAssert.Ensure(
            client1Result.Payload.State.Winners.SequenceEqual([client1Auth.ActorId]), "Assertion failed: client1Result.Payload.State.Winners.SequenceEqual([client1Auth.ActorId])");
        ZlinkStreamAssert.Ensure(
            client1Result.Payload.State.Players.All(static player => player.Card.Count == 9), "Assertion failed: client1Result.Payload.State.Players.All(static player => player.Card.Count == 9)");
        ZlinkStreamAssert.Ensure(
            client1Result.Payload.State.Players.All(static player => player.Marks[4]), "Assertion failed: client1Result.Payload.State.Players.All(static player => player.Marks[4])");

        var reward = await rewardTask;
        ZlinkStreamAssert.Ensure(reward.Payload.ActorId == client1Auth.ActorId, "Assertion failed: reward.Payload.ActorId == client1Auth.ActorId");
        ZlinkStreamAssert.Ensure(reward.Payload.DrawSeq == client1Result.Payload.State.DrawSeq, "Assertion failed: reward.Payload.DrawSeq == client1Result.Payload.State.DrawSeq");
        ZlinkStreamAssert.Ensure(reward.Payload.ItemId == BingoRewardItems.GoldenDauberId, "Assertion failed: reward.Payload.ItemId == BingoRewardItems.GoldenDauberId");
        ZlinkStreamAssert.Ensure(reward.Payload.ItemName == BingoRewardItems.GoldenDauberName, "Assertion failed: reward.Payload.ItemName == BingoRewardItems.GoldenDauberName");
        ZlinkStreamAssert.Ensure(reward.Payload.Rarity == BingoRewardItems.LegendaryRarity, "Assertion failed: reward.Payload.Rarity == BingoRewardItems.LegendaryRarity");

        var stopped = await observer.Request(new StopObservingBingoEventsReq { RoomId = client1MatchRes.RoomId })
            .Async<StopObservingBingoEventsRes>(cancellationToken);
        ZlinkStreamAssert.Ensure(stopped.Stopped, "Assertion failed: stopped.Stopped");
    }

    private static async ValueTask<MatchBingoRes> MatchAsync(
        IZlinkStreamConnector connector,
        string mode,
        CancellationToken cancellationToken)
    {
        var completion = connector.WaitFor<MatchBingoRes>()
            .Async(cancellationToken);
        await connector.Send(new MatchBingoReq { Mode = mode })
            .Async(cancellationToken);
        return (await completion).Payload;
    }

    private static async ValueTask<ObserveBingoEventsRes> ObserveAsync(
        IZlinkStreamConnector connector,
        string roomId,
        CancellationToken cancellationToken)
    {
        var completion = connector.WaitFor<ObserveBingoEventsRes>()
            .Async(cancellationToken);
        await connector.Send(new ObserveBingoEventsReq { RoomId = roomId })
            .Async(cancellationToken);
        return (await completion).Payload;
    }
}

internal static class BingoClientCards
{
    public static readonly int[] Player1 = [1, 2, 3, 4, 0, 6, 7, 8, 9];

    public static readonly int[] Player2 = [10, 11, 12, 13, 0, 14, 4, 5, 6];
}
