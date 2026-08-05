using TicTacToe.Server.Configuration;
using TicTacToe.Server.Play.Domain.TicTacToe;
using TicTacToe.Server.Play.Infrastructure.ZLink.Actors;
using TicTacToe.Server.Play.Infrastructure.ZLink.Spots.TicTacToeGameSpot.Handlers;
using TicTacToe.Shared.Contracts;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;
using Zlink.Framework.Contracts.Timers;

namespace TicTacToe.Server.Play.Infrastructure.ZLink.Spots.TicTacToeGameSpot;

internal sealed class TicTacToeGame(
    IZLinkSpotContext context,
    ILogger<TicTacToeGame> logger) : IZLinkSpot<PlayActor>
{
    private static readonly TimeSpan GameTickPeriod = TimeSpan.FromSeconds(1);
    private static readonly TimeSpan TurnTimeout = TimeSpan.FromSeconds(15);

    private readonly Dictionary<string, PlayActor> _actors = new(StringComparer.Ordinal);
    private readonly Dictionary<string, (string RoomId, PlayerInfo Player)> _pendingJoins = new(StringComparer.Ordinal);
    private readonly TicTacToeMatch _match = new(context.SpotId, TurnTimeout);
    private readonly string _roomId = context.SpotId;
    private string _gameName = SampleDefaults.GameName;
    private int _requiredLevel = SampleDefaults.RequiredLevel;
    private IZLinkTimer? _gameTick;

    public IZLinkSpotContext Context { get; } = context;

    public void Configure()
    {
        Context.Handlers.AddActorPacket<PlayActorPlaceMarkHandler, PlayActor>();
        Context.Handlers.AddActorPacket<PlayActorLeaveGameHandler, PlayActor>();
    }

    public async ValueTask OnJoinedActorAsync(
        PlayActor actor,
        CancellationToken cancellationToken)
    {
        if (_pendingJoins.Remove(actor.ActorId, out var join))
        {
            actor.JoinRoom(join.RoomId);
            actor.ApplyPlayer(join.Player);
            _actors[actor.ActorId] = actor;
            var state = _match.Snapshot();
            var mark = string.Equals(state.XActorId, actor.ActorId, StringComparison.Ordinal)
                ? TicTacToeMarks.X
                : TicTacToeMarks.O;
            await NotifyPlayerJoinedAsync(actor, mark, state, cancellationToken);
            await BroadcastAsync(state, actor.ActorId, cancellationToken);
        }

        logger.LogInformation(
            "game spot: actor joined. actor={ActorId}, roomId={RoomId}",
            actor.ActorId,
            _roomId);
    }

    public ValueTask OnLeaveActorAsync(
        PlayActor actor,
        CancellationToken cancellationToken)
    {
        logger.LogInformation(
            "game spot: actor left. actor={ActorId}, roomId={RoomId}",
            actor.ActorId,
            _roomId);
        _actors.Remove(actor.ActorId);
        return ValueTask.CompletedTask;
    }

    public ValueTask OnDisconnectActorAsync(
        PlayActor actor,
        CancellationToken cancellationToken)
    {
        actor.MarkDisconnected();
        logger.LogInformation(
            "game spot: actor disconnected. actor={ActorId}, roomId={RoomId}",
            actor.ActorId,
            _roomId);
        return ValueTask.CompletedTask;
    }

    public async ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(
        string actorId,
        ZLinkMessage request,
        CancellationToken cancellationToken)
    {
        var joinRequest = request.Decode<TicTacToeGameJoinReq>();
        var reply = await JoinPlayerAsync(actorId, joinRequest.RoomId, joinRequest.Player, cancellationToken);
        logger.LogInformation(
            "TicTacToeGame: actor join accepted. actor={ActorId}, roomId={RoomId}, mark={Mark}",
            actorId,
            joinRequest.RoomId,
            reply.State.XActorId == actorId ? TicTacToeMarks.X : TicTacToeMarks.O);

        return ZLinkSpotActorJoinResult.Accept(reply);
    }

    public ValueTask<ZLinkSpotCreateResponse> OnCreateAsync(
        ZLinkMessage request,
        CancellationToken cancellationToken)
    {
        var settings = request.Decode<TicTacToeGameCreateReq>();
        if (string.IsNullOrWhiteSpace(settings.GameName))
            return ValueTask.FromResult(
                ZLinkSpotCreateResponse.Reject("GameName is required."));
        if (settings.RequiredLevel < 0)
            return ValueTask.FromResult(
                ZLinkSpotCreateResponse.Reject("RequiredLevel must not be negative."));

        _gameName = settings.GameName;
        _requiredLevel = settings.RequiredLevel;
        logger.LogInformation(
            "game spot: created. roomId={RoomId}, game={GameName}, requiredLevel={RequiredLevel}",
            _roomId,
            _gameName,
            _requiredLevel);
        return ValueTask.FromResult(ZLinkSpotCreateResponse.Accept());
    }

    public async ValueTask OnInitializeAsync(CancellationToken cancellationToken)
    {
        _gameTick = await Context.AddTimer<TicTacToeGameTimerHandler>(
            "game-tick",
            GameTickPeriod,
            cancellationToken: cancellationToken);
    }

    public async ValueTask OnClosingAsync(
        ZLinkSpotClosingContext context,
        CancellationToken cleanupCancellationToken)
    {
        _ = context;
        cleanupCancellationToken.ThrowIfCancellationRequested();
        if (_gameTick is not null) await _gameTick.CancelAsync();
    }

    public ValueTask<TicTacToeGameJoinRes> JoinPlayerAsync(
        string actorId,
        string roomId,
        PlayerInfo player,
        CancellationToken cancellationToken)
    {
        if (!string.Equals(roomId, _roomId, StringComparison.Ordinal))
            throw new InvalidOperationException($"Actor requested join for a different room. roomId={roomId}");

        if (!string.Equals(player.ActorId, actorId, StringComparison.Ordinal))
            throw new InvalidOperationException(
                $"Join player '{player.ActorId}' does not match actor '{actorId}'.");

        if (player.Level < _requiredLevel)
            throw new InvalidOperationException(
                $"Player level {player.Level} is below required level {_requiredLevel}.");

        _pendingJoins[actorId] = (roomId, player);

        var change = _match.JoinPlayer(actorId, DateTimeOffset.UtcNow);

        return ValueTask.FromResult(new TicTacToeGameJoinRes(change.State));
    }

    public async ValueTask<PlaceMarkRes> PlaceMarkAsync(
        PlayActor actor,
        int cell,
        CancellationToken cancellationToken)
    {
        var before = _match.Snapshot();
        var change = _match.PlaceMark(actor.ActorId, cell, DateTimeOffset.UtcNow);
        await BroadcastAsync(change.State, actor.ActorId, cancellationToken);
        await PublishWinMilestoneAsync(actor, before, change.State, cancellationToken);
        return new PlaceMarkRes(change.State);
    }

    internal async ValueTask TickAsync(CancellationToken cancellationToken)
    {
        var change = _match.Tick(DateTimeOffset.UtcNow);
        if (!change.HasChanged) return;

        await BroadcastAsync(change.State, null, cancellationToken);
    }

    public async ValueTask LeaveGameAsync(
        PlayActor actor,
        string roomId,
        CancellationToken cancellationToken)
    {
        if (!string.Equals(roomId, _roomId, StringComparison.Ordinal))
            throw new InvalidOperationException($"Actor requested leave for a different room. roomId={roomId}");

        var state = _match.Snapshot();
        if (!IsTerminal(state)) throw new InvalidOperationException($"Game is not finished. status={state.Status}");

        if (!_actors.ContainsKey(actor.ActorId)) return;

        actor.MarkForDestroyAfterRoomLeave();
        await Context.LeaveActorAsync(actor, cancellationToken);
    }

    private async ValueTask BroadcastAsync(
        GameState state,
        string? excludedActorId,
        CancellationToken cancellationToken)
    {
        var message = new GameStateNotify(state);
        var recipients = _actors.Values
            .Where(actor => !string.Equals(actor.ActorId, excludedActorId, StringComparison.Ordinal))
            .ToArray();
        await SendSessionPushAsync(
            recipients,
            async actor =>
            {
                await actor.Context.BoundSession.Send(message).Async(cancellationToken);
            });
    }

    private async ValueTask NotifyPlayerJoinedAsync(
        PlayActor joinedActor,
        string mark,
        GameState state,
        CancellationToken cancellationToken)
    {
        var player = joinedActor.RequirePlayer();
        var message = new PlayerJoinedNotify(
            state.RoomId,
            joinedActor.ActorId,
            player.DisplayName,
            player.Level,
            mark,
            state);

        var recipients = _actors.Values
            .Where(actor => !string.Equals(actor.ActorId, joinedActor.ActorId, StringComparison.Ordinal))
            .ToArray();
        await SendSessionPushAsync(
            recipients,
            async actor =>
            {
                await actor.Context.BoundSession.Send(message).Async(cancellationToken);
            });
    }

    private async ValueTask PublishWinMilestoneAsync(
        PlayActor actor,
        GameState before,
        GameState after,
        CancellationToken cancellationToken)
    {
        if (string.Equals(before.Status, TicTacToeGameStatuses.Won, StringComparison.Ordinal)
            || !string.Equals(after.Status, TicTacToeGameStatuses.Won, StringComparison.Ordinal)
            || !string.Equals(after.Winner, actor.ActorId, StringComparison.Ordinal))
            return;

        var player = actor.RequirePlayer();
        var wins = player.Wins + 1;
        if (wins != 100) return;

        logger.LogInformation(
            "game spot: publishing win milestone. actor={ActorId}, roomId={RoomId}, wins={Wins}",
            player.ActorId,
            after.RoomId,
            wins);
        await Context.Outbound.Publish(
                SampleTopics.PlayerMilestoneChannel,
                SampleTopics.PlayerMilestone,
                new PlayerWinMilestoneEvent(after.RoomId, player.ActorId, player.DisplayName, wins))
            .Async(cancellationToken);
        logger.LogInformation(
            "game spot: milestone publish submitted. actor={ActorId}, roomId={RoomId}",
            player.ActorId,
            after.RoomId);
    }

    private static async ValueTask SendSessionPushAsync(
        IReadOnlyList<PlayActor> recipients,
        Func<PlayActor, ValueTask> send)
    {
        foreach (var recipient in recipients) await send(recipient);
    }

    private static bool IsTerminal(GameState state)
    {
        return string.Equals(state.Status, TicTacToeGameStatuses.Won, StringComparison.Ordinal)
               || string.Equals(state.Status, TicTacToeGameStatuses.Draw, StringComparison.Ordinal)
               || string.Equals(state.Status, TicTacToeGameStatuses.TurnTimedOut, StringComparison.Ordinal);
    }

}
