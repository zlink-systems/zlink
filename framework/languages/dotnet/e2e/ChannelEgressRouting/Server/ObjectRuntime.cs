using System.Buffers.Binary;
using ChannelEgressRouting.Shared;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;
using Zlink.Framework.Contracts.Timers;

namespace ChannelEgressRouting.Server;

internal static class ChannelObjectNames
{
    internal const string ActorType = "channel.player";
    internal const string SpotType = "channel.room";
}

internal sealed class ChannelActor(
    string actorId,
    IZLinkActorContext context) : IZLinkActor
{
    public string ActorId { get; } = actorId;
    public IZLinkActorContext Context { get; } = context;
    public int StateVersion { get; set; }
}

internal sealed class ChannelActorFactory
    : IZLinkActorFactory<ChannelActor>
{
    public ValueTask<ChannelActor> CreateAsync(
        IZLinkActorContext context,
        CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.FromResult(new ChannelActor(context.ActorId, context));
    }
}

internal sealed class ChannelActorRelocationAdapter
    : IZLinkActorRelocationAdapter<ChannelActor>
{
    public ValueTask<byte[]> CaptureAsync(
        ChannelActor actor,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var payload = new byte[sizeof(int)];
        BinaryPrimitives.WriteInt32LittleEndian(payload, actor.StateVersion);
        return ValueTask.FromResult(payload);
    }

    public ValueTask RestoreAsync(
        ChannelActor actor,
        ReadOnlyMemory<byte> payload,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        if (payload.Length != sizeof(int))
            throw new InvalidDataException("Actor relocation payload is invalid.");
        actor.StateVersion = BinaryPrimitives.ReadInt32LittleEndian(payload.Span);
        return ValueTask.CompletedTask;
    }
}

internal sealed class ChannelEntrySpot(
    IZLinkEntrySpotContext context) : IZLinkEntrySpot<ChannelActor>
{
    private int _timerStarted;

    public IZLinkEntrySpotContext Context { get; } = context;

    public ValueTask<ZLinkActorCreateResponse> OnCreateActorAsync(
        ChannelActor actor,
        ZLinkMessage createRequest,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        actor.StateVersion = 1;
        return ValueTask.FromResult(ZLinkActorCreateResponse.Accept());
    }

    public ValueTask OnJoinedActorAsync(
        ChannelActor actor,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.CompletedTask;
    }

    public ValueTask OnLeaveActorAsync(
        ChannelActor actor,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.CompletedTask;
    }

    internal bool TryStartTimer() =>
        Interlocked.CompareExchange(ref _timerStarted, 1, 0) == 0;
}

internal sealed class ChannelRoomSpot(
    IZLinkSpotContext context) : IZLinkSpot<ChannelActor>
{
    public IZLinkSpotContext Context { get; } = context;

    public void Configure()
    {
        Context.Handlers.AddSubscribe<ChannelLogicalMulticastHandler>(
            ChannelEgressNames.Play,
            "config12.logical");
    }

    public ValueTask<ZLinkSpotCreateResponse> OnCreateAsync(
        ZLinkMessage request,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.FromResult(ZLinkSpotCreateResponse.Accept());
    }

    public ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(
        string actorId,
        ZLinkMessage request,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.FromResult(ZLinkSpotActorJoinResult.Accept(request));
    }

    public ValueTask OnJoinedActorAsync(
        ChannelActor actor,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.CompletedTask;
    }

    public ValueTask OnLeaveActorAsync(
        ChannelActor actor,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.CompletedTask;
    }
}

internal sealed class ChannelLogicalMulticastHandler(
    EvidenceStore evidence)
    : IZLinkSpotSubscriptionHandler<
        ChannelRoomSpot,
        LogicalMulticastProbeEvent>
{
    public ValueTask HandleAsync(
        ChannelRoomSpot spot,
        LogicalMulticastProbeEvent message,
        ZLinkPublishMessageContext context,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        evidence.Add(
            $"logical-multicast|spot={spot.Context.SpotId}|topic={context.Topic}|id={message.Id}");
        return ValueTask.CompletedTask;
    }
}

internal sealed class ChannelRoomRelocationAdapter
    : IZLinkSpotRelocationAdapter<ChannelRoomSpot>
{
    public ValueTask<byte[]> CaptureAsync(
        ChannelRoomSpot spot,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.FromResult(Array.Empty<byte>());
    }

    public ValueTask RestoreAsync(
        ChannelRoomSpot spot,
        ReadOnlyMemory<byte> payload,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        if (!payload.IsEmpty)
            throw new InvalidDataException("Room relocation payload is invalid.");
        return ValueTask.CompletedTask;
    }
}

[ZLinkSpotActorRequestHandler(nameof(ChannelObjectProbeRequest))]
internal sealed class ChannelEntryActorProbeHandler
    : IZLinkEntrySpotActorRequestHandler<
        ChannelEntrySpot,
        ChannelActor,
        ChannelObjectProbeRequest,
        ChannelObjectProbeReply>
{
    public ValueTask<ChannelObjectProbeReply> HandleAsync(
        ChannelEntrySpot spot,
        ChannelActor actor,
        IZLinkMessageContext context,
        ChannelObjectProbeRequest request,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.FromResult(Reply(spot.Context, actor, request.Id));
    }

    internal static ChannelObjectProbeReply Reply(
        IZLinkSpotCommonContext spot,
        ChannelActor actor,
        string id) =>
        new(
            id,
            actor.ActorId,
            spot.SpotId,
            actor.StateVersion,
            spot.NodeRid.ToString());
}

[ZLinkSpotActorRequestHandler(nameof(ChannelObjectProbeRequest))]
internal sealed class ChannelRoomActorProbeHandler
    : IZLinkSpotActorRequestHandler<
        ChannelRoomSpot,
        ChannelActor,
        ChannelObjectProbeRequest,
        ChannelObjectProbeReply>
{
    public ValueTask<ChannelObjectProbeReply> HandleAsync(
        ChannelRoomSpot spot,
        ChannelActor actor,
        IZLinkMessageContext context,
        ChannelObjectProbeRequest request,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.FromResult(
            ChannelEntryActorProbeHandler.Reply(
                spot.Context,
                actor,
                request.Id));
    }
}

[ZLinkSpotRequestHandler(nameof(ChannelObjectProbeRequest))]
internal sealed class ChannelRoomProbeHandler
    : IZLinkSpotRequestHandler<
        ChannelRoomSpot,
        ChannelObjectProbeRequest,
        ChannelObjectProbeReply>
{
    public ValueTask<ChannelObjectProbeReply> HandleAsync(
        ChannelRoomSpot spot,
        ChannelObjectProbeRequest request,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.FromResult(new ChannelObjectProbeReply(
            request.Id,
            string.Empty,
            spot.Context.SpotId,
            0,
            spot.Context.NodeRid.ToString()));
    }
}

[ZLinkSpotActorRequestHandler(nameof(ChannelSpotWorkflowRequest))]
internal sealed class ChannelEntryWorkflowHandler(
    IZLinkRouteClient routes,
    EvidenceStore evidence)
    : IZLinkEntrySpotActorRequestHandler<
        ChannelEntrySpot,
        ChannelActor,
        ChannelSpotWorkflowRequest,
        ChannelSpotWorkflowReply>
{
    public async ValueTask<ChannelSpotWorkflowReply> HandleAsync(
        ChannelEntrySpot spot,
        ChannelActor actor,
        IZLinkMessageContext context,
        ChannelSpotWorkflowRequest request,
        CancellationToken cancellationToken)
    {
        evidence.Add($"spot-workflow|phase=started|id={request.Id}|version={actor.StateVersion}");
        var reply = await routes
            .RequestToChannel(
                ChannelEgressNames.Workflow,
                new ChannelProbeRequest($"{request.Id}-handler"))
            .Async<ChannelProbeReply>(cancellationToken);
        actor.StateVersion++;
        evidence.Add($"spot-workflow|phase=resumed|id={request.Id}|version={actor.StateVersion}");
        await spot.Context.AddTimer<ChannelEntryTimerHandler>(
            $"workflow-probe-{request.Id}",
            TimeSpan.FromMilliseconds(100),
            new ZLinkTimerOptions
            {
                OverrunPolicy = ZLinkTimerOverrunPolicy.DelayNextTick,
                StopOnUnhandledException = true
            },
            cancellationToken);
        return new ChannelSpotWorkflowReply(
            request.Id,
            actor.StateVersion,
            reply.Role);
    }
}

[ZLinkSpotActorRequestHandler(nameof(ChannelActorJoinRequest))]
internal sealed class ChannelActorJoinHandler
    : IZLinkEntrySpotActorRequestHandler<
        ChannelEntrySpot,
        ChannelActor,
        ChannelActorJoinRequest,
        ChannelActorJoinReply>
{
    public ValueTask<ChannelActorJoinReply> HandleAsync(
        ChannelEntrySpot spot,
        ChannelActor actor,
        IZLinkMessageContext context,
        ChannelActorJoinRequest request,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        actor.Context.JoinSpot(request.TargetSpotId, request)
            .Timeout(TimeSpan.FromSeconds(10))
            .Defer();
        return ValueTask.FromResult(new ChannelActorJoinReply(
            request.Id,
            actor.ActorId,
            request.TargetSpotId,
            true));
    }
}

[ZLinkSpotActorRequestHandler(nameof(ChannelBoundPushRequest))]
internal sealed class ChannelEntryBoundPushHandler
    : IZLinkEntrySpotActorRequestHandler<
        ChannelEntrySpot,
        ChannelActor,
        ChannelBoundPushRequest,
        ChannelBoundPushReply>
{
    public async ValueTask<ChannelBoundPushReply> HandleAsync(
        ChannelEntrySpot spot,
        ChannelActor actor,
        IZLinkMessageContext context,
        ChannelBoundPushRequest request,
        CancellationToken cancellationToken)
    {
        await SendAsync(spot.Context, actor, request, cancellationToken);
        return new ChannelBoundPushReply(request.Id, true);
    }

    internal static async ValueTask SendAsync(
        IZLinkSpotCommonContext spot,
        ChannelActor actor,
        ChannelBoundPushRequest request,
        CancellationToken cancellationToken)
    {
        await actor.Context.BoundSession.Send(
                new ChannelBoundPushNotification(
                    request.Id,
                    actor.ActorId,
                    spot.SpotId,
                    actor.StateVersion))
            .Async(cancellationToken);
    }
}

[ZLinkSpotActorRequestHandler(nameof(ChannelBoundPushRequest))]
internal sealed class ChannelRoomBoundPushHandler
    : IZLinkSpotActorRequestHandler<
        ChannelRoomSpot,
        ChannelActor,
        ChannelBoundPushRequest,
        ChannelBoundPushReply>
{
    public async ValueTask<ChannelBoundPushReply> HandleAsync(
        ChannelRoomSpot spot,
        ChannelActor actor,
        IZLinkMessageContext context,
        ChannelBoundPushRequest request,
        CancellationToken cancellationToken)
    {
        await ChannelEntryBoundPushHandler.SendAsync(
            spot.Context,
            actor,
            request,
            cancellationToken);
        return new ChannelBoundPushReply(request.Id, true);
    }
}

internal sealed class ChannelEntryTimerHandler(
    IZLinkRouteClient routes,
    EvidenceStore evidence)
    : IZLinkSpotTimerHandler<ChannelEntrySpot>
{
    public async ValueTask HandleAsync(
        ChannelEntrySpot spot,
        ZLinkTimerTick tick,
        CancellationToken cancellationToken)
    {
        if (!spot.TryStartTimer())
            return;
        evidence.Add($"spot-timer|phase=started|tick={tick.DeliveryIndex}");
        var reply = await routes
            .RequestToChannel(
                ChannelEgressNames.Workflow,
                new ChannelProbeRequest($"timer-{tick.DeliveryIndex}"))
            .Timeout(TimeSpan.FromSeconds(3))
            .Async<ChannelProbeReply>(cancellationToken);
        evidence.Add(
            $"spot-timer|phase=resumed|tick={tick.DeliveryIndex}|role={reply.Role}");
    }
}

internal sealed class ChannelNodeProbeHandler(
    RoleOptions options,
    EvidenceStore evidence)
    : IZLinkRouteRequestHandler<
        ChannelObjectProbeRequest,
        ChannelProbeReply>
{
    public ValueTask<ChannelProbeReply> HandleAsync(
        ChannelObjectProbeRequest request,
        ZLinkRouteMessageContext context,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        evidence.Add(
            $"node-direct|role={options.Role}|mesh={context.MeshName}|id={request.Id}");
        return ValueTask.FromResult(new ChannelProbeReply(
            request.Id,
            options.Role,
            context.MeshName ?? string.Empty,
            []));
    }
}
