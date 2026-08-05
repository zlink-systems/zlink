using ObservabilityOps.Server.Play.Infrastructure;
using ObservabilityOps.Server.Play.Support;
using ObservabilityOps.Server.Support;
using ObservabilityOps.Shared;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;

namespace ObservabilityOps.Server.Play.Spots;

internal sealed class RoomSpot(
    IZLinkSpotContext context,
    EvidenceStore evidence,
    SpotClosingGate closingGate) : IZLinkSpot<PlayerActor>
{
    public IZLinkSpotContext Context { get; } = context;
    public DateTimeOffset? AutoCloseAfter { get; internal set; }
    public bool BlockClosing { get; internal set; }

    public ValueTask<ZLinkSpotCreateResponse> OnCreateAsync(ZLinkMessage request,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var create = request.Decode<CreateRoomReq>();
        if (string.Equals(create.Mode, "auto-close", StringComparison.Ordinal))
            AutoCloseAfter = DateTimeOffset.UtcNow + TimeSpan.FromSeconds(7);
        BlockClosing = string.Equals(
            create.Mode, "block-closing", StringComparison.Ordinal);
        evidence.Add($"room-created|room={Context.SpotId}|node={Context.NodeRid}");
        return ValueTask.FromResult(ZLinkSpotCreateResponse.Accept());
    }

    public ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(string actorId, ZLinkMessage request,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var joined = new JoinRoomRes(actorId, Context.SpotId.ToString(), Context.NodeRid.ToString());
        return ValueTask.FromResult(ZLinkSpotActorJoinResult.Accept(joined));
    }

    public ValueTask OnJoinedActorAsync(PlayerActor actor, CancellationToken cancellationToken)
    {
        actor.Player.JoinRoom(Context.SpotId.ToString());
        evidence.Add($"actor-joined|actor={actor.ActorId}|room={actor.Player.RoomRid}|node={Context.NodeRid}");
        return ValueTask.CompletedTask;
    }

    public ValueTask OnLeaveActorAsync(PlayerActor actor, CancellationToken cancellationToken) =>
        ValueTask.CompletedTask;

    public async ValueTask OnClosingAsync(
        ZLinkSpotClosingContext context,
        CancellationToken cleanupCancellationToken)
    {
        evidence.Add(
            $"spot-closing|kind=user|spot={Context.SpotId}"
            + $"|node={Context.NodeRid}|reason={context.Reason}"
            + $"|deadline={context.Deadline:O}");
        if (!BlockClosing) return;
        try
        {
            await closingGate.EnterAsync(cleanupCancellationToken);
            evidence.Add(
                $"spot-closing-released|spot={Context.SpotId}"
                + $"|node={Context.NodeRid}");
        }
        catch (OperationCanceledException)
            when (cleanupCancellationToken.IsCancellationRequested)
        {
            evidence.Add(
                $"spot-closing-cancelled|spot={Context.SpotId}"
                + $"|node={Context.NodeRid}");
            throw;
        }
    }
}
