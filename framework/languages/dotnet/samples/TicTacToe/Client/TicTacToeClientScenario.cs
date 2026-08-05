using Microsoft.Extensions.Logging;
using Systems.Zlink.Stream.Connector.Contracts;
using TicTacToe.Shared.Contracts;
using Zlink.HttpClient;

namespace TicTacToe.Client;

public sealed class TicTacToeClientScenario(ILogger logger)
{
    // End-to-end client story:
    // 1. Create a game through HTTP and choose separate stream endpoints for host, guest, and observer.
    // 2. Connect the host and authenticate as player X.
    // 3. Connect the observer and subscribe to win milestone notifications.
    // 4. Join the host to the empty room, then connect the guest and verify join/start pushes.
    // 5. Play a deterministic move sequence where player X wins the top row.
    // 6. Verify the guest sees the final state and the observer receives the win milestone.
    public async ValueTask RunAsync(
        TicTacToeClientOptions options,
        CancellationToken cancellationToken = default)
    {
        using var api = ZLinkHttpClient.Create(options.ApiUrl.ToString())
            .Timeout(options.HttpTimeout)
            .Build();
        var room = await api.Post("/games")
            .Body(new CreateGameHttpReq(options.GameName))
            .Fetch<CreateGameHttpRes>(cancellationToken);

        ZlinkStreamAssert.Ensure(!string.IsNullOrWhiteSpace(room.RoomId), "Assertion failed: !string.IsNullOrWhiteSpace(room.RoomId)");
        ZlinkStreamAssert.Ensure(room.PlayEndpoints.Count >= 2, "Assertion failed: room.PlayEndpoints.Count >= 2");
        ZlinkStreamAssert.Ensure(room.PlayNodes.Count == room.PlayEndpoints.Count, "Assertion failed: room.PlayNodes.Count == room.PlayEndpoints.Count");
        ZlinkStreamAssert.Ensure(room.RequiredLevel == 3, "Assertion failed: room.RequiredLevel == 3");
        ZlinkStreamAssert.Ensure(room.GameName == options.GameName, "Assertion failed: room.GameName == options.GameName");

        var hostPlayEndpoint = room.PlayEndpoints[0];
        var guestPlayEndpoint = room.PlayEndpoints[1];
        var observerPlayEndpoint = guestPlayEndpoint;
        var observerPlayNode = room.PlayNodes.Single(node =>
            string.Equals(node.StreamEndpoint, observerPlayEndpoint, StringComparison.Ordinal));

        await using var client1 =
            TicTacToeClientConnections.CreateStreamClient(hostPlayEndpoint, options, "host", logger);
        await using var client2 =
            TicTacToeClientConnections.CreateStreamClient(guestPlayEndpoint, options, "guest", logger);
        await using var observer =
            TicTacToeClientConnections.CreateStreamClient(observerPlayEndpoint, options, "observer", logger);

        // Client 1 connects, authenticates as player X, and joins the empty room.
        await client1.Connect.Async(cancellationToken);

        var client1Authentication = await client1.Request(new AuthenticateReq(options.XActorId))
            .Async<AuthenticateRes>(cancellationToken);
        ZlinkStreamAssert.Ensure(client1Authentication.Player.ActorId == options.XActorId, "Assertion failed: client1Authentication.Player.ActorId == options.XActorId");
        ZlinkStreamAssert.Ensure(client1Authentication.Player.Level >= room.RequiredLevel, "Assertion failed: client1Authentication.Player.Level >= room.RequiredLevel");
        ZlinkStreamAssert.Ensure(client1Authentication.Player.Wins == 99, "Assertion failed: client1Authentication.Player.Wins == 99");

        await observer.Connect.Async(cancellationToken);
        var observerAuthentication = await observer.Request(new AuthenticateReq(options.ObserverActorId))
            .Async<AuthenticateRes>(cancellationToken);
        ZlinkStreamAssert.Ensure(observerAuthentication.Player.ActorId == options.ObserverActorId, "Assertion failed: observerAuthentication.Player.ActorId == options.ObserverActorId");
        var observerSubscription =
            await observer.Request(new ObserveMilestoneReq()).Async<ObserveMilestoneRes>(cancellationToken);
        ZlinkStreamAssert.Ensure(observerSubscription.Subscribed, "Assertion failed: observerSubscription.Subscribed");

        var client1Join = await JoinGameAsync(client1, room.RoomId, cancellationToken);
        ZlinkStreamAssert.Ensure(client1Join.State.RoomId == room.RoomId, "Assertion failed: client1Join.State.RoomId == room.RoomId");
        ZlinkStreamAssert.Ensure(client1Join.State.Status == TicTacToeGameStatuses.WaitingForPlayers, "Assertion failed: client1Join.State.Status == TicTacToeGameStatuses.WaitingForPlayers");
        ZlinkStreamAssert.Ensure(client1Join.State.XActorId == options.XActorId, "Assertion failed: client1Join.State.XActorId == options.XActorId");
        await client1.ExpectNone<PlayerJoinedNotify>()
            .Within(TimeSpan.FromMilliseconds(250))
            .Async(cancellationToken);

        // Client 2 connects, authenticates as player O, and joins the same room.
        await client2.Connect.Async(cancellationToken);

        var client2Authentication = await client2.Request(new AuthenticateReq(options.OActorId))
            .Async<AuthenticateRes>(cancellationToken);
        ZlinkStreamAssert.Ensure(client2Authentication.Player.ActorId == options.OActorId, "Assertion failed: client2Authentication.Player.ActorId == options.OActorId");
        ZlinkStreamAssert.Ensure(client2Authentication.Player.Level >= room.RequiredLevel, "Assertion failed: client2Authentication.Player.Level >= room.RequiredLevel");
        ZlinkStreamAssert.Ensure(client2Authentication.Player.ActorId != client1Authentication.Player.ActorId, "Assertion failed: client2Authentication.Player.ActorId != client1Authentication.Player.ActorId");

        var client2Join = await JoinGameAsync(client2, room.RoomId, cancellationToken);
        ZlinkStreamAssert.Ensure(client2Join.State.RoomId == room.RoomId, "Assertion failed: client2Join.State.RoomId == room.RoomId");
        ZlinkStreamAssert.Ensure(client2Join.State.Status == TicTacToeGameStatuses.InProgress, "Assertion failed: client2Join.State.Status == TicTacToeGameStatuses.InProgress");
        ZlinkStreamAssert.Ensure(client2Join.State.OActorId == options.OActorId, "Assertion failed: client2Join.State.OActorId == options.OActorId");

        // Existing room members receive push packets when another player joins.
        var client1SawClient2Join = await client1.WaitFor<PlayerJoinedNotify>()
            .Where(message => message.Payload.ActorId == options.OActorId)
            .Async(cancellationToken);
        ZlinkStreamAssert.Ensure(client1SawClient2Join.Payload.ActorId == options.OActorId, "Assertion failed: client1SawClient2Join.Payload.ActorId == options.OActorId");
        ZlinkStreamAssert.Ensure(client1SawClient2Join.Payload.DisplayName == client2Authentication.Player.DisplayName, "Assertion failed: client1SawClient2Join.Payload.DisplayName == client2Authentication.Player.DisplayName");
        ZlinkStreamAssert.Ensure(client1SawClient2Join.Payload.Level == client2Authentication.Player.Level, "Assertion failed: client1SawClient2Join.Payload.Level == client2Authentication.Player.Level");
        ZlinkStreamAssert.Ensure(client1SawClient2Join.Payload.Mark == TicTacToeMarks.O, "Assertion failed: client1SawClient2Join.Payload.Mark == TicTacToeMarks.O");
        ZlinkStreamAssert.Ensure(client1SawClient2Join.Payload.RoomId == room.RoomId, "Assertion failed: client1SawClient2Join.Payload.RoomId == room.RoomId");
        ZlinkStreamAssert.Ensure(client1SawClient2Join.Payload.State.Status == TicTacToeGameStatuses.InProgress, "Assertion failed: client1SawClient2Join.Payload.State.Status == TicTacToeGameStatuses.InProgress");
        await client2.ExpectNone<PlayerJoinedNotify>()
            .Within(TimeSpan.FromMilliseconds(250))
            .Async(cancellationToken);

        var client1SawGameStart = await client1.WaitFor<GameStateNotify>()
            .Where(message => message.Payload.State.Status == TicTacToeGameStatuses.InProgress
                              && message.Payload.State.OActorId == options.OActorId)
            .Async(cancellationToken);

        ZlinkStreamAssert.Ensure(client1SawGameStart.Payload.State.Status == TicTacToeGameStatuses.InProgress, "Assertion failed: client1SawGameStart.Payload.State.Status == TicTacToeGameStatuses.InProgress");
        ZlinkStreamAssert.Ensure(client1SawGameStart.Payload.State.OActorId == options.OActorId, "Assertion failed: client1SawGameStart.Payload.State.OActorId == options.OActorId");
        ZlinkStreamAssert.Ensure(client1SawGameStart.Payload.State.NextTurn == TicTacToeMarks.X, "Assertion failed: client1SawGameStart.Payload.State.NextTurn == TicTacToeMarks.X");

        // The move sequence is deterministic: client 1 completes the top row.
        var client1Move1 = await client1.Request(new PlaceMarkReq(0)).Async<PlaceMarkRes>(cancellationToken);
        ZlinkStreamAssert.Ensure(client1Move1.State.Board == "X........", "Assertion failed: client1Move1.State.Board == \"X........\"");
        ZlinkStreamAssert.Ensure(client1Move1.State.NextTurn == TicTacToeMarks.O, "Assertion failed: client1Move1.State.NextTurn == TicTacToeMarks.O");
        ZlinkStreamAssert.Ensure(client1Move1.State.LastMoveActorId == options.XActorId, "Assertion failed: client1Move1.State.LastMoveActorId == options.XActorId");
        ZlinkStreamAssert.Ensure(client1Move1.State.LastMoveCell == 0, "Assertion failed: client1Move1.State.LastMoveCell == 0");

        var client2SawClient1Move1 = await client2.WaitFor<GameStateNotify>()
            .Where(message => message.Payload.State.LastMoveActorId == options.XActorId
                              && message.Payload.State.LastMoveCell == 0)
            .Async(cancellationToken);
        ZlinkStreamAssert.Ensure(client2SawClient1Move1.Payload.State.LastMoveActorId == options.XActorId, "Assertion failed: client2SawClient1Move1.Payload.State.LastMoveActorId == options.XActorId");
        ZlinkStreamAssert.Ensure(client2SawClient1Move1.Payload.State.LastMoveCell == 0, "Assertion failed: client2SawClient1Move1.Payload.State.LastMoveCell == 0");
        ZlinkStreamAssert.Ensure(client2SawClient1Move1.Payload.State.Board == client1Move1.State.Board, "Assertion failed: client2SawClient1Move1.Payload.State.Board == client1Move1.State.Board");

        var client2Move1 = await client2.Request(new PlaceMarkReq(3)).Async<PlaceMarkRes>(cancellationToken);
        ZlinkStreamAssert.Ensure(client2Move1.State.Board == "X..O.....", "Assertion failed: client2Move1.State.Board == \"X..O.....\"");
        ZlinkStreamAssert.Ensure(client2Move1.State.NextTurn == TicTacToeMarks.X, "Assertion failed: client2Move1.State.NextTurn == TicTacToeMarks.X");
        ZlinkStreamAssert.Ensure(client2Move1.State.LastMoveActorId == options.OActorId, "Assertion failed: client2Move1.State.LastMoveActorId == options.OActorId");
        ZlinkStreamAssert.Ensure(client2Move1.State.LastMoveCell == 3, "Assertion failed: client2Move1.State.LastMoveCell == 3");

        var client1SawClient2Move1 = await client1.WaitFor<GameStateNotify>()
            .Where(message => message.Payload.State.LastMoveActorId == options.OActorId
                              && message.Payload.State.LastMoveCell == 3)
            .Async(cancellationToken);
        ZlinkStreamAssert.Ensure(client1SawClient2Move1.Payload.State.LastMoveActorId == options.OActorId, "Assertion failed: client1SawClient2Move1.Payload.State.LastMoveActorId == options.OActorId");
        ZlinkStreamAssert.Ensure(client1SawClient2Move1.Payload.State.LastMoveCell == 3, "Assertion failed: client1SawClient2Move1.Payload.State.LastMoveCell == 3");
        ZlinkStreamAssert.Ensure(client1SawClient2Move1.Payload.State.Board == client2Move1.State.Board, "Assertion failed: client1SawClient2Move1.Payload.State.Board == client2Move1.State.Board");

        var client1Move2 = await client1.Request(new PlaceMarkReq(1)).Async<PlaceMarkRes>(cancellationToken);
        ZlinkStreamAssert.Ensure(client1Move2.State.Board == "XX.O.....", "Assertion failed: client1Move2.State.Board == \"XX.O.....\"");
        ZlinkStreamAssert.Ensure(client1Move2.State.NextTurn == TicTacToeMarks.O, "Assertion failed: client1Move2.State.NextTurn == TicTacToeMarks.O");
        ZlinkStreamAssert.Ensure(client1Move2.State.LastMoveActorId == options.XActorId, "Assertion failed: client1Move2.State.LastMoveActorId == options.XActorId");
        ZlinkStreamAssert.Ensure(client1Move2.State.LastMoveCell == 1, "Assertion failed: client1Move2.State.LastMoveCell == 1");

        var client2SawClient1Move2 = await client2.WaitFor<GameStateNotify>()
            .Where(message => message.Payload.State.LastMoveActorId == options.XActorId
                              && message.Payload.State.LastMoveCell == 1)
            .Async(cancellationToken);
        ZlinkStreamAssert.Ensure(client2SawClient1Move2.Payload.State.LastMoveActorId == options.XActorId, "Assertion failed: client2SawClient1Move2.Payload.State.LastMoveActorId == options.XActorId");
        ZlinkStreamAssert.Ensure(client2SawClient1Move2.Payload.State.LastMoveCell == 1, "Assertion failed: client2SawClient1Move2.Payload.State.LastMoveCell == 1");
        ZlinkStreamAssert.Ensure(client2SawClient1Move2.Payload.State.Board == client1Move2.State.Board, "Assertion failed: client2SawClient1Move2.Payload.State.Board == client1Move2.State.Board");

        var client2Move2 = await client2.Request(new PlaceMarkReq(4)).Async<PlaceMarkRes>(cancellationToken);
        ZlinkStreamAssert.Ensure(client2Move2.State.Board == "XX.OO....", "Assertion failed: client2Move2.State.Board == \"XX.OO....\"");
        ZlinkStreamAssert.Ensure(client2Move2.State.NextTurn == TicTacToeMarks.X, "Assertion failed: client2Move2.State.NextTurn == TicTacToeMarks.X");
        ZlinkStreamAssert.Ensure(client2Move2.State.LastMoveActorId == options.OActorId, "Assertion failed: client2Move2.State.LastMoveActorId == options.OActorId");
        ZlinkStreamAssert.Ensure(client2Move2.State.LastMoveCell == 4, "Assertion failed: client2Move2.State.LastMoveCell == 4");

        var client1SawClient2Move2 = await client1.WaitFor<GameStateNotify>()
            .Where(message => message.Payload.State.LastMoveActorId == options.OActorId
                              && message.Payload.State.LastMoveCell == 4)
            .Async(cancellationToken);
        ZlinkStreamAssert.Ensure(client1SawClient2Move2.Payload.State.LastMoveActorId == options.OActorId, "Assertion failed: client1SawClient2Move2.Payload.State.LastMoveActorId == options.OActorId");
        ZlinkStreamAssert.Ensure(client1SawClient2Move2.Payload.State.LastMoveCell == 4, "Assertion failed: client1SawClient2Move2.Payload.State.LastMoveCell == 4");
        ZlinkStreamAssert.Ensure(client1SawClient2Move2.Payload.State.Board == client2Move2.State.Board, "Assertion failed: client1SawClient2Move2.Payload.State.Board == client2Move2.State.Board");

        var client1FinalMove = await client1.Request(new PlaceMarkReq(2)).Async<PlaceMarkRes>(cancellationToken);
        ZlinkStreamAssert.Ensure(client1FinalMove.State.Board == "XXXOO....", "Assertion failed: client1FinalMove.State.Board == \"XXXOO....\"");
        ZlinkStreamAssert.Ensure(client1FinalMove.State.Status == TicTacToeGameStatuses.Won, "Assertion failed: client1FinalMove.State.Status == TicTacToeGameStatuses.Won");
        ZlinkStreamAssert.Ensure(client1FinalMove.State.Winner == options.XActorId, "Assertion failed: client1FinalMove.State.Winner == options.XActorId");
        ZlinkStreamAssert.Ensure(client1FinalMove.State.LastMoveActorId == options.XActorId, "Assertion failed: client1FinalMove.State.LastMoveActorId == options.XActorId");
        ZlinkStreamAssert.Ensure(client1FinalMove.State.LastMoveCell == 2, "Assertion failed: client1FinalMove.State.LastMoveCell == 2");

        var client2SawFinal = await client2.WaitFor<GameStateNotify>()
            .Where(message => message.Payload.State.Status == TicTacToeGameStatuses.Won
                              && message.Payload.State.Winner == options.XActorId)
            .Async(cancellationToken);
        ZlinkStreamAssert.Ensure(client2SawFinal.Payload.State.Status == TicTacToeGameStatuses.Won, "Assertion failed: client2SawFinal.Payload.State.Status == TicTacToeGameStatuses.Won");
        ZlinkStreamAssert.Ensure(client2SawFinal.Payload.State.Winner == options.XActorId, "Assertion failed: client2SawFinal.Payload.State.Winner == options.XActorId");
        ZlinkStreamAssert.Ensure(client2SawFinal.Payload.State.Board == client1FinalMove.State.Board, "Assertion failed: client2SawFinal.Payload.State.Board == client1FinalMove.State.Board");

        var observerSawMilestone = await observer.WaitFor<WinMilestoneNotify>()
            .Where(message => message.Payload.ActorId == options.XActorId
                              && message.Payload.RoomId == room.RoomId)
            .Async(cancellationToken);
        ZlinkStreamAssert.Ensure(observerSawMilestone.Payload.DisplayName == client1Authentication.Player.DisplayName, "Assertion failed: observerSawMilestone.Payload.DisplayName == client1Authentication.Player.DisplayName");
        ZlinkStreamAssert.Ensure(observerSawMilestone.Payload.Wins == 100, "Assertion failed: observerSawMilestone.Payload.Wins == 100");
        logger.LogInformation(
            "observer-win-milestone=verified actor={0} wins={1}",
            observerSawMilestone.Payload.ActorId,
            observerSawMilestone.Payload.Wins);

        var client1Leave = await client1.Request(new LeaveGameMsg(room.RoomId))
            .Async<LeaveGameRes>(cancellationToken);
        ZlinkStreamAssert.Ensure(client1Leave.Completed, "Assertion failed: client1Leave.Completed");
        var client2Leave = await client2.Request(new LeaveGameMsg(room.RoomId))
            .Async<LeaveGameRes>(cancellationToken);
        ZlinkStreamAssert.Ensure(client2Leave.Completed, "Assertion failed: client2Leave.Completed");
    }

    private static async ValueTask<JoinGameRes> JoinGameAsync(
        IZlinkStreamConnector connector,
        string roomId,
        CancellationToken cancellationToken)
    {
        var completion = connector.WaitFor<JoinGameRes>()
            .Async(cancellationToken);
        await connector.Send(new JoinGameReq(roomId))
            .Async(cancellationToken);
        return (await completion).Payload;
    }
}
