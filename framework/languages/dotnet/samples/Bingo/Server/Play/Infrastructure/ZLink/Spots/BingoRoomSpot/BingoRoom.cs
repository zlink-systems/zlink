using Bingo.Server.Configuration;
using Bingo.Server.Play.Domain.Bingo;
using Bingo.Server.Play.Infrastructure.ZLink.Actors;
using Bingo.Server.Play.Infrastructure.ZLink.Spots.BingoRoomSpot.Notifications;
using Bingo.Shared.Contracts;
using Microsoft.Extensions.Logging;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;

namespace Bingo.Server.Play.Infrastructure.ZLink.Spots.BingoRoomSpot;

internal sealed class BingoRoom(
    IZLinkSpotContext context,
    BingoRoomEventMapper eventMapper,
    BingoNotificationPublisher notifications,
    ILogger<BingoRoom> logger) : IZLinkSpot<PlayerActor>
{
    private static readonly BingoRoomSettings DefaultSettings = BingoRoomSettings.Create(BingoSampleModes.TwoPlayer, 0);

    private readonly Dictionary<string, PlayerActor> _actors = new(StringComparer.Ordinal);
    private readonly Dictionary<string, PendingJoin> _pendingJoins = new(StringComparer.Ordinal);
    private bool _cleanupStarted;
    private BingoRoomGame? _game = new(context.SpotId, DefaultSettings);
    private PlayerActor? _observerActor;
    private BingoRoomSettings _settings = DefaultSettings;

    internal bool IsReadyToDraw => _game?.IsReadyToDraw == true;

    public IZLinkSpotContext Context { get; } = context;

    internal (BingoRoomSettings Settings, BingoRoomState State) CaptureRelocationState()
    {
        return (_settings, _game?.Snapshot() ?? new BingoRoomState
        {
            RoomId = Context.SpotId,
            Status = BingoRoomStatus.Running
        });
    }

    internal void RestoreRelocationState(
        BingoRoomSettings settings,
        BingoRoomState state)
    {
        _settings = settings;
        _game = settings.IsObserver
            ? null
            : BingoRoomGame.Restore(Context.SpotId, settings, state);
    }

    public ValueTask OnRelocationReadyCompletedAsync(
        ZLinkSpotRelocationReadyCompletion completion,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        logger.LogInformation(
            "bingo room: relocation-ready completed. room={RoomId}, outcome={Outcome}",
            Context.SpotId,
            completion.Outcome);
        return ValueTask.CompletedTask;
    }

    public ValueTask OnClosingAsync(
        ZLinkSpotClosingContext context,
        CancellationToken cleanupCancellationToken)
    {
        _ = context;
        cleanupCancellationToken.ThrowIfCancellationRequested();
        return ValueTask.CompletedTask;
    }

    public async ValueTask OnJoinedActorAsync(
        PlayerActor actor,
        CancellationToken cancellationToken)
    {
        if (!_pendingJoins.TryGetValue(actor.ActorId, out var pending)) return;

        var join = pending.Request;
        if (join.ObserveOnly)
        {
            _pendingJoins.Remove(actor.ActorId);
            actor.SetDisplayName(join.DisplayName);
            actor.JoinRoom(join.RoomId);
            _observerActor = actor;
        }
        else
        {
            GetPlayerRecordRes record;
            try
            {
                // Yield releases the Spot execution turn while the API owns the player record lookup.
                record = await Context.Outbound
                    .RequestToChannel(
                        SampleNames.ApiChannel,
                        new GetPlayerRecordReq { ActorId = actor.ActorId })
                    .Yield<GetPlayerRecordRes>(cancellationToken);
            }
            catch
            {
                _pendingJoins.Remove(actor.ActorId);
                throw;
            }

            if (!_pendingJoins.TryGetValue(actor.ActorId, out var resumed)
                || !ReferenceEquals(resumed, pending)
                || _game is null
                || !_game.CanAcceptPlayer())
            {
                _pendingJoins.Remove(actor.ActorId);
                await Context.LeaveActorAsync(actor, cancellationToken);
                return;
            }

            _pendingJoins.Remove(actor.ActorId);
            actor.SetDisplayName(join.DisplayName);
            actor.JoinRoom(join.RoomId);
            _actors[actor.ActorId] = actor;
            var change = RequireGame().JoinPlayer(
                actor.ActorId,
                join.DisplayName,
                record.Wins,
                record.Losses);
            await PublishAsync(change, cancellationToken);
            logger.LogInformation(
                "bingo room: player record loaded. room={RoomId}, actor={ActorId}, wins={Wins}, losses={Losses}",
                Context.SpotId,
                actor.ActorId,
                record.Wins,
                record.Losses);
        }

        logger.LogInformation(
            "bingo room: actor joined. room={RoomId}, actor={ActorId}",
            Context.SpotId,
            actor.ActorId);

        // PublishAsync excludes the joining actor, so the destination room
        // notifies it after the actor has completed the join lifecycle.
        if (_game is not null && _game.Status == BingoRoomStatus.Running)
            await actor.Context.BoundSession
                .Send(new BingoGameStartedNotify { State = _game.Snapshot() })
                .Async(cancellationToken);

        if (_settings.IsObserver)
            Context.RelocationReady().Defer();
    }

    public async ValueTask OnLeaveActorAsync(
        PlayerActor actor,
        CancellationToken cancellationToken)
    {
        if (_actors.ContainsKey(actor.ActorId) && _game is not null)
        {
            var finalState = _game.Snapshot();
            // Capture the completed room result before Yield permits another Spot turn to run.
            var report = new ReportBingoResultReq
            {
                RoomId = finalState.RoomId,
                ActorId = actor.ActorId,
                Won = finalState.Winners.Contains(actor.ActorId),
                FinalDrawSeq = finalState.DrawSeq
            };
            var record = await Context.Outbound
                .RequestToChannel(SampleNames.ApiChannel, report)
                .Yield<ReportBingoResultRes>(cancellationToken);
            logger.LogInformation(
                "bingo room: result reported. room={RoomId}, actor={ActorId}, won={Won}, wins={Wins}, losses={Losses}",
                report.RoomId,
                report.ActorId,
                report.Won,
                record.Wins,
                record.Losses);
        }

        _actors.Remove(actor.ActorId);
        if (_observerActor is not null
            && string.Equals(_observerActor.ActorId, actor.ActorId, StringComparison.Ordinal))
            _observerActor = null;
        logger.LogInformation(
            "bingo room: actor left. room={RoomId}, actor={ActorId}",
            Context.SpotId,
            actor.ActorId);
        if (_actors.Count == 0 && _observerActor is null)
            _ = await Context.CloseAsync(cancellationToken);
    }

    public ValueTask OnDisconnectActorAsync(
        PlayerActor actor,
        CancellationToken cancellationToken)
    {
        actor.MarkDisconnected();
        logger.LogInformation(
            "bingo room: actor disconnected. room={RoomId}, actor={ActorId}",
            Context.SpotId,
            actor.ActorId);
        return ValueTask.CompletedTask;
    }

    public async ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(
        string actorId,
        ZLinkMessage request,
        CancellationToken cancellationToken)
    {
        var reply = await JoinAsync(actorId, request.Decode<BingoRoomJoinReq>(), cancellationToken);
        return ZLinkSpotActorJoinResult.Accept(reply);
    }

    public ValueTask<ZLinkSpotCreateResponse> OnCreateAsync(
        ZLinkMessage request,
        CancellationToken cancellationToken)
    {
        var settings = BingoRoomSettingsPayloadMapper.FromMessage(request, DefaultSettings);
        ApplySettings(settings);
        logger.LogInformation(
            "bingo room: created. room={RoomId}, roomName={RoomName}, mode={Mode}, purpose={Purpose}, observedRoom={ObservedRoomId}, requiredPlayers={RequiredPlayers}, maxDrawNumber={MaxDrawNumber}",
            Context.SpotId,
            settings.RoomName,
            settings.Mode,
            settings.Purpose,
            settings.ObservedRoomId ?? "-",
            settings.RequiredPlayers,
            settings.MaxDrawNumber);
        return ValueTask.FromResult(ZLinkSpotCreateResponse.Accept());
    }

    public ValueTask<BingoRoomJoinRes> JoinAsync(
        string actorId,
        BingoRoomJoinReq request,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        return request.ObserveOnly
            ? ValueTask.FromResult(JoinObserver(actorId, request))
            : ValueTask.FromResult(JoinPlayer(actorId, request));
    }

    internal BingoGameChange SubmitCard(string actorId, BingoCard card)
    {
        return RequireGame().SubmitCard(actorId, card);
    }

    internal BingoGameChange DrawNextNumber()
    {
        return RequireGame().DrawNextNumber();
    }

    internal void DeferRelocationAtRoundBoundary()
    {
        Context.RelocationReady().Defer();
    }

    internal async ValueTask PublishAsync(
        BingoGameChange change,
        CancellationToken cancellationToken)
    {
        if (change.Events.Count == 0) return;

        await notifications.PublishAsync(eventMapper.Map(change.Events, _actors), cancellationToken);
        if (change.State.Status == BingoRoomStatus.Finished && change.State.Winners.Count > 0)
        {
            logger.LogInformation(
                "bingo reward: publishing. room={RoomId}, actor={ActorId}, item={ItemId}, nodeRid={NodeRid}",
                change.State.RoomId,
                change.State.Winners[0],
                BingoRewardItems.GoldenDauberId,
                Context.NodeRid.ToString());
            await Context.Outbound.Publish(
                    SampleNames.RoomChannel,
                    SampleNames.RewardTopic,
                    new BingoRewardAcquiredEvent
                    {
                        RoomId = change.State.RoomId,
                        ActorId = change.State.Winners[0],
                        DrawSeq = change.State.DrawSeq,
                        ItemId = BingoRewardItems.GoldenDauberId,
                        ItemName = BingoRewardItems.GoldenDauberName,
                        Rarity = BingoRewardItems.LegendaryRarity
                    })
                .Async(cancellationToken);
            logger.LogInformation(
                "bingo reward: published. room={RoomId}, actor={ActorId}, item={ItemId}, nodeRid={NodeRid}",
                change.State.RoomId,
                change.State.Winners[0],
                BingoRewardItems.GoldenDauberId,
                Context.NodeRid.ToString());
        }
    }

    internal async ValueTask LeaveFinishedActorsAsync(CancellationToken cancellationToken)
    {
        if (_cleanupStarted || _game?.Status != BingoRoomStatus.Finished) return;

        _cleanupStarted = true;
        var actors = _actors.Values.ToArray();
        foreach (var actor in actors)
        {
            actor.MarkForDestroyAfterRoomLeave();
            logger.LogInformation(
                "bingo room: actor marked for destroy. room={RoomId}, actor={ActorId}",
                Context.SpotId,
                actor.ActorId);
        }
        foreach (var actor in actors)
            await Context.LeaveActorAsync(actor, cancellationToken);
    }

    public void ApplySettings(BingoRoomSettings settings)
    {
        _settings = settings;
        _game = settings.IsObserver ? null : new BingoRoomGame(Context.SpotId, settings);
    }

    internal void EnsureRoomId(string roomId)
    {
        if (!string.Equals(roomId, Context.SpotId, StringComparison.Ordinal))
            throw new InvalidOperationException($"Player is not submitting to this room. room={roomId}");
    }

    internal async ValueTask AnnounceRewardAsync(BingoRewardAcquiredEvent message, CancellationToken cancellationToken)
    {
        if (!_settings.IsObserver
            || _observerActor is null
            || !string.Equals(message.RoomId, _settings.ObservedRoomId, StringComparison.Ordinal))
        {
            logger.LogInformation(
                "bingo reward: ignored. room={RoomId}, actor={ActorId}, item={ItemId}, observer={IsObserver}, hasActor={HasActor}, observedRoom={ObservedRoomId}, nodeRid={NodeRid}",
                message.RoomId,
                message.ActorId,
                message.ItemId,
                _settings.IsObserver,
                _observerActor is not null,
                _settings.ObservedRoomId ?? "-",
                Context.NodeRid.ToString());
            return;
        }

        logger.LogInformation(
            "bingo reward: announcing. room={RoomId}, actor={ActorId}, item={ItemId}, observer={ObserverActorId}, nodeRid={NodeRid}",
            message.RoomId,
            message.ActorId,
            message.ItemId,
            _observerActor.ActorId,
            Context.NodeRid.ToString());
        await _observerActor.Context.BoundSession
            .Send(
                new BingoRewardAnnouncedNotify
                {
                    RoomId = message.RoomId,
                    ActorId = message.ActorId,
                    DrawSeq = message.DrawSeq,
                    ItemId = message.ItemId,
                    ItemName = message.ItemName,
                    Rarity = message.Rarity
            })
            .Async(cancellationToken);
        Context.RelocationReady().Defer();
    }

    internal async ValueTask<bool> StopObservingAsync(PlayerActor actor, string roomId,
        CancellationToken cancellationToken)
    {
        if (!_settings.IsObserver
            || _observerActor is null
            || !string.Equals(_observerActor.ActorId, actor.ActorId, StringComparison.Ordinal)
            || !string.Equals(_settings.ObservedRoomId, roomId, StringComparison.Ordinal))
            return false;

        _observerActor = null;
        await Context.LeaveActorAsync(actor, cancellationToken);
        logger.LogInformation(
            "bingo observer room: actor left. observedRoom={ObservedRoomId}, observer={ActorId}",
            roomId,
            actor.ActorId);
        return true;
    }

    private BingoRoomGame RequireGame()
    {
        return _game ?? throw new InvalidOperationException("Observer BingoRoom does not own game state.");
    }

    private BingoRoomJoinRes JoinObserver(
        string actorId,
        BingoRoomJoinReq request)
    {
        if (!_settings.IsObserver)
            throw new InvalidOperationException("Observe-only actor can join only an observer BingoRoom.");

        if (!string.Equals(request.RoomId, _settings.ObservedRoomId, StringComparison.Ordinal))
            throw new InvalidOperationException($"Observer room is not watching room '{request.RoomId}'.");
        _pendingJoins[actorId] = new PendingJoin(request);
        logger.LogInformation(
            "bingo observer room: actor joined. observedRoom={ObservedRoomId}, observer={ActorId}, nodeRid={NodeRid}",
            _settings.ObservedRoomId,
            actorId,
            Context.NodeRid.ToString());
        return new BingoRoomJoinRes
        {
            State = new BingoRoomState
            {
                RoomId = request.RoomId,
                Status = BingoRoomStatus.Running
            }
        };
    }

    private BingoRoomJoinRes JoinPlayer(
        string actorId,
        BingoRoomJoinReq request)
    {
        var game = RequireGame();
        if (_pendingJoins.TryGetValue(actorId, out var existing))
            return new BingoRoomJoinRes { State = PreviewPendingJoins(game) };

        if (!game.CanAcceptPlayer())
            throw new InvalidOperationException($"Room {Context.SpotId} cannot accept more players.");

        _pendingJoins[actorId] = new PendingJoin(request);
        var state = PreviewPendingJoins(game);
        logger.LogInformation(
            "bingo room: actor accepted. room={RoomId}, actor={ActorId}, status={Status}",
            request.RoomId,
            actorId,
            state.Status);
        logger.LogInformation(
            "bingo room: actor join reply ready. room={RoomId}, actor={ActorId}, status={Status}",
            request.RoomId,
            actorId,
            state.Status);
        return new BingoRoomJoinRes { State = state };
    }

    private BingoRoomState PreviewPendingJoins(BingoRoomGame game)
    {
        return game.PreviewPlayerJoins(
            _pendingJoins.Values
                .Where(static pending => !pending.Request.ObserveOnly)
                .Select(static pending => pending.Request));
    }

    private sealed record PendingJoin(BingoRoomJoinReq Request);
}
