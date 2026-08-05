using SpotService.Server.Play.Spots;
using SpotService.Shared;
using Systems.Zlink;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;

namespace SpotService.Server.Play.Handlers;

[ZLinkHandlerGroup("client")]
internal sealed class ChannelEchoHandler(EvidenceStore evidence)
    : IZLinkRequestHandler<ChannelEchoReq, ChannelEchoRes>
{
    public ValueTask<ChannelEchoRes> HandleAsync(
        ChannelEchoReq request,
        IZLinkMessageContext context,
        CancellationToken cancellationToken)
    {
        _ = context;
        cancellationToken.ThrowIfCancellationRequested();
        evidence.Add($"channel-echo|value={request.Value}");
        return ValueTask.FromResult(new ChannelEchoRes($"echo-{request.Value}"));
    }
}

[ZLinkHandlerGroup("client")]
internal sealed class ChannelNotifyHandler(EvidenceStore evidence)
    : IZLinkSendHandler<ChannelNotify>
{
    public ValueTask HandleAsync(
        ChannelNotify message,
        IZLinkMessageContext context,
        CancellationToken cancellationToken)
    {
        _ = context;
        cancellationToken.ThrowIfCancellationRequested();
        evidence.Add($"channel-notify|marker={message.Marker}");
        return ValueTask.CompletedTask;
    }
}

[ZLinkSpotActorRequestHandler("ActorPingReq")]
internal sealed class EntryActorPingHandler(EvidenceStore evidence)
    : IZLinkEntrySpotActorRequestHandler<ScenarioEntrySpot, ScenarioActor, ActorPingReq, ActorPingRes>
{
    public ValueTask<ActorPingRes> HandleAsync(
        ScenarioEntrySpot entrySpot,
        ScenarioActor actor,
        IZLinkMessageContext context,
        ActorPingReq request,
        CancellationToken cancellationToken)
    {
        _ = context;
        cancellationToken.ThrowIfCancellationRequested();
        actor.Seen++;
        evidence.Add(
            $"actor-ping|rid={entrySpot.Context.NodeRid}|actor={actor.ActorId}"
            + $"|spot={entrySpot.Context.SpotId}|value={request.Value}|seen={actor.Seen}");
        return ValueTask.FromResult(new ActorPingRes(
            actor.ActorId,
            entrySpot.Context.NodeRid.ToString(),
            entrySpot.Context.SpotId.ToString(),
            request.Value,
            actor.Seen));
    }
}

[ZLinkSpotActorRequestHandler("SlowActorPingReq")]
internal sealed class EntrySlowActorPingHandler(EvidenceStore evidence)
    : IZLinkEntrySpotActorRequestHandler<ScenarioEntrySpot, ScenarioActor, SlowActorPingReq, ActorPingRes>
{
    public async ValueTask<ActorPingRes> HandleAsync(
        ScenarioEntrySpot entrySpot,
        ScenarioActor actor,
        IZLinkMessageContext context,
        SlowActorPingReq request,
        CancellationToken cancellationToken)
    {
        _ = context;
        evidence.Add(
            $"actor-slow-ping-started|rid={entrySpot.Context.NodeRid}|actor={actor.ActorId}"
            + $"|spot={entrySpot.Context.SpotId}|value={request.Value}");
        await Task.Delay(TimeSpan.FromMilliseconds(request.DelayMs), cancellationToken);
        actor.Seen++;
        evidence.Add(
            $"actor-slow-ping|rid={entrySpot.Context.NodeRid}|actor={actor.ActorId}"
            + $"|spot={entrySpot.Context.SpotId}|value={request.Value}|seen={actor.Seen}");
        return new ActorPingRes(
            actor.ActorId,
            entrySpot.Context.NodeRid.ToString(),
            entrySpot.Context.SpotId.ToString(),
            request.Value,
            actor.Seen);
    }
}

[ZLinkSpotActorRequestHandler("UserActorPingReq")]
internal sealed class EntryUserActorPingHandler(EvidenceStore evidence)
    : IZLinkEntrySpotActorRequestHandler<ScenarioEntrySpot, ScenarioActor, ActorPingReq, ActorPingRes>
{
    public ValueTask<ActorPingRes> HandleAsync(
        ScenarioEntrySpot entrySpot,
        ScenarioActor actor,
        IZLinkMessageContext context,
        ActorPingReq request,
        CancellationToken cancellationToken)
    {
        _ = context;
        cancellationToken.ThrowIfCancellationRequested();
        actor.Seen++;
        evidence.Add(
            $"actor-ping|rid={entrySpot.Context.NodeRid}|actor={actor.ActorId}"
            + $"|spot={actor.DisplayName}|value={request.Value}|seen={actor.Seen}");
        return ValueTask.FromResult(new ActorPingRes(
            actor.ActorId,
            entrySpot.Context.NodeRid.ToString(),
            actor.DisplayName,
            request.Value,
            actor.Seen));
    }
}

[ZLinkSpotActorRequestHandler("UserActorPingReq")]
internal sealed class UserActorPingHandler(EvidenceStore evidence)
    : IZLinkSpotActorRequestHandler<ScenarioUserSpot, ScenarioActor, ActorPingReq, ActorPingRes>
{
    public ValueTask<ActorPingRes> HandleAsync(
        ScenarioUserSpot spot,
        ScenarioActor actor,
        IZLinkMessageContext context,
        ActorPingReq request,
        CancellationToken cancellationToken)
    {
        _ = context;
        cancellationToken.ThrowIfCancellationRequested();
        actor.Seen++;
        evidence.Add(
            $"actor-ping|rid={spot.Context.NodeRid}|actor={actor.ActorId}"
            + $"|spot={spot.Context.SpotId}|value={request.Value}|seen={actor.Seen}");
        return ValueTask.FromResult(new ActorPingRes(
            actor.ActorId,
            spot.Context.NodeRid.ToString(),
            spot.Context.SpotId.ToString(),
            request.Value,
            actor.Seen));
    }
}

[ZLinkSpotActorRequestHandler("JoinUserSpotActorReq")]
internal sealed class EntryUserSpotActorJoinHandler
    : IZLinkEntrySpotActorRequestHandler<ScenarioEntrySpot, ScenarioActor, JoinUserSpotActorReq, JoinUserSpotActorRes>
{
    public ValueTask<JoinUserSpotActorRes> HandleAsync(
        ScenarioEntrySpot entrySpot,
        ScenarioActor actor,
        IZLinkMessageContext context,
        JoinUserSpotActorReq request,
        CancellationToken cancellationToken)
    {
        _ = entrySpot;
        _ = context;
        cancellationToken.ThrowIfCancellationRequested();
        if (!string.Equals(request.ActorId, actor.ActorId, StringComparison.Ordinal))
            throw new InvalidOperationException("Join request actor does not match dispatched actor.");

        actor.Context.JoinSpot(request.SpotRid, ZLinkMessage.Empty).Defer();
        return ValueTask.FromResult(new JoinUserSpotActorRes(
            request.SpotRid,
            actor.ActorId,
            true,
            actor.Context.ObjectGeneration));
    }
}

[ZLinkSpotActorRequestHandler("JoinAdmittedUserSpotActorReq")]
internal sealed class EntryAdmittedUserSpotActorJoinHandler
    : IZLinkEntrySpotActorRequestHandler<ScenarioEntrySpot, ScenarioActor, JoinAdmittedUserSpotActorReq,
        JoinAdmittedUserSpotActorRes>
{
    public ValueTask<JoinAdmittedUserSpotActorRes> HandleAsync(
        ScenarioEntrySpot entrySpot,
        ScenarioActor actor,
        IZLinkMessageContext context,
        JoinAdmittedUserSpotActorReq request,
        CancellationToken cancellationToken)
    {
        _ = entrySpot;
        _ = context;
        cancellationToken.ThrowIfCancellationRequested();
        if (!string.Equals(request.ActorId, actor.ActorId, StringComparison.Ordinal))
            throw new InvalidOperationException("Admission join request actor does not match dispatched actor.");

        actor.Context.JoinSpot(request.SpotRid, ZLinkMessage.From(request)).Defer();
        return ValueTask.FromResult(new JoinAdmittedUserSpotActorRes(
            request.SpotRid,
            actor.ActorId,
            request.Allow,
            actor.Context.ObjectGeneration,
            request.Allow ? string.Empty : "ActorJoinRejected"));
    }
}

[ZLinkSpotActorRequestHandler("LeaveReq")]
internal sealed class EntryActorLeaveHandler(EvidenceStore evidence)
    : IZLinkEntrySpotActorRequestHandler<ScenarioEntrySpot, ScenarioActor, LeaveReq, LeaveRes>
{
    public ValueTask<LeaveRes> HandleAsync(
        ScenarioEntrySpot entrySpot,
        ScenarioActor actor,
        IZLinkMessageContext context,
        LeaveReq request,
        CancellationToken cancellationToken)
    {
        _ = context;
        cancellationToken.ThrowIfCancellationRequested();
        if (!string.Equals(request.ActorId, actor.ActorId, StringComparison.Ordinal))
            throw new InvalidOperationException("Leave request actor does not match dispatched actor.");

        evidence.Add(
            $"spot-actor-left|rid={entrySpot.Context.NodeRid}|spot={actor.DisplayName}|actor={actor.ActorId}");
        return ValueTask.FromResult(new LeaveRes(actor.ActorId, true));
    }
}

[ZLinkSpotActorRequestHandler("LeaveReq")]
internal sealed class UserActorLeaveHandler
    : IZLinkSpotActorRequestHandler<ScenarioUserSpot, ScenarioActor, LeaveReq, LeaveRes>
{
    public async ValueTask<LeaveRes> HandleAsync(
        ScenarioUserSpot spot,
        ScenarioActor actor,
        IZLinkMessageContext context,
        LeaveReq request,
        CancellationToken cancellationToken)
    {
        _ = context;
        if (!string.Equals(request.ActorId, actor.ActorId, StringComparison.Ordinal))
            throw new InvalidOperationException("Leave request actor does not match dispatched actor.");

        await spot.Context.LeaveActorAsync(actor, cancellationToken);
        return new LeaveRes(actor.ActorId, true);
    }
}

[ZLinkSpotActorRequestHandler("SnapshotReq")]
internal sealed class EntryActorSnapshotHandler
    : IZLinkEntrySpotActorRequestHandler<ScenarioEntrySpot, ScenarioActor, SnapshotReq, SnapshotRes>
{
    public ValueTask<SnapshotRes> HandleAsync(
        ScenarioEntrySpot entrySpot,
        ScenarioActor actor,
        IZLinkMessageContext context,
        SnapshotReq request,
        CancellationToken cancellationToken)
    {
        _ = entrySpot;
        _ = context;
        cancellationToken.ThrowIfCancellationRequested();
        if (!string.Equals(request.ActorId, actor.ActorId, StringComparison.Ordinal))
            throw new InvalidOperationException("Snapshot request actor does not match dispatched actor.");

        return ValueTask.FromResult(new SnapshotRes(actor.ActorId, actor.Seen));
    }
}

[ZLinkSpotActorRequestHandler("DestroyActorReq")]
internal sealed class EntryActorDestroyHandler(EvidenceStore evidence)
    : IZLinkEntrySpotActorRequestHandler<ScenarioEntrySpot, ScenarioActor, DestroyActorReq, DestroyActorRes>
{
    public async ValueTask<DestroyActorRes> HandleAsync(
        ScenarioEntrySpot entrySpot,
        ScenarioActor actor,
        IZLinkMessageContext context,
        DestroyActorReq request,
        CancellationToken cancellationToken)
    {
        _ = context;
        if (!string.Equals(request.ActorId, actor.ActorId, StringComparison.Ordinal))
            throw new InvalidOperationException("Destroy request actor does not match dispatched actor.");

        await entrySpot.Context.RunCpuWorker(_ => true).Async(cancellationToken);
        await entrySpot.Context.DestroyActorAsync(actor, cancellationToken);
        evidence.Add($"actor-destroyed|rid={evidence.Rid}|actor={actor.ActorId}");
        return new DestroyActorRes(actor.ActorId, true);
    }
}

[ZLinkSpotActorRequestHandler("ActorPushReq")]
internal sealed class ActorPushHandler
    : IZLinkEntrySpotActorRequestHandler<ScenarioEntrySpot, ScenarioActor, ActorPushReq, ActorPingRes>
{
    public ActorPushHandler(EvidenceStore evidence)
    {
        Evidence = evidence;
    }

    private EvidenceStore Evidence { get; }

    public async ValueTask<ActorPingRes> HandleAsync(
        ScenarioEntrySpot entrySpot,
        ScenarioActor actor,
        IZLinkMessageContext context,
        ActorPushReq request,
        CancellationToken cancellationToken)
    {
        actor.Seen++;
        Evidence.Add(
            $"actor-push|rid={entrySpot.Context.NodeRid}|actor={actor.ActorId}"
            + $"|spot={entrySpot.Context.SpotId}|value={request.Value}|seen={actor.Seen}");
        await actor.Context.BoundSession.Send(new ActorPushNotify(actor.ActorId, request.Value, actor.Seen))
            .Async(cancellationToken);
        return new ActorPingRes(
            actor.ActorId,
            entrySpot.Context.NodeRid.ToString(),
            entrySpot.Context.SpotId.ToString(),
            request.Value,
            actor.Seen);
    }
}

[ZLinkSpotActorRequestHandler("UserActorPushReq")]
internal sealed class EntryUserActorPushHandler
    : IZLinkEntrySpotActorRequestHandler<ScenarioEntrySpot, ScenarioActor, ActorPushReq, ActorPingRes>
{
    public async ValueTask<ActorPingRes> HandleAsync(
        ScenarioEntrySpot entrySpot,
        ScenarioActor actor,
        IZLinkMessageContext context,
        ActorPushReq request,
        CancellationToken cancellationToken)
    {
        _ = context;
        cancellationToken.ThrowIfCancellationRequested();
        actor.Seen++;
        await actor.Context.BoundSession.Send(new ActorPushNotify(actor.ActorId, request.Value, actor.Seen))
            .Async(cancellationToken);
        return new ActorPingRes(
            actor.ActorId,
            entrySpot.Context.NodeRid.ToString(),
            actor.DisplayName,
            request.Value,
            actor.Seen);
    }
}

[ZLinkSpotActorRequestHandler("UserActorPushReq")]
internal sealed class UserActorPushHandler
    : IZLinkSpotActorRequestHandler<ScenarioUserSpot, ScenarioActor, ActorPushReq, ActorPingRes>
{
    public async ValueTask<ActorPingRes> HandleAsync(
        ScenarioUserSpot spot,
        ScenarioActor actor,
        IZLinkMessageContext context,
        ActorPushReq request,
        CancellationToken cancellationToken)
    {
        _ = context;
        actor.Seen++;
        await actor.Context.BoundSession.Send(new ActorPushNotify(actor.ActorId, request.Value, actor.Seen))
            .Async(cancellationToken);
        return new ActorPingRes(
            actor.ActorId,
            spot.Context.NodeRid.ToString(),
            spot.Context.SpotId.ToString(),
            request.Value,
            actor.Seen);
    }
}

[ZLinkSpotActorRequestHandler("ComplexActorReq")]
internal sealed class ComplexActorHandler(EvidenceStore evidence)
    : IZLinkEntrySpotActorRequestHandler<ScenarioEntrySpot, ScenarioActor, ComplexActorReq, ComplexActorRes>
{
    public ValueTask<ComplexActorRes> HandleAsync(
        ScenarioEntrySpot entrySpot,
        ScenarioActor actor,
        IZLinkMessageContext context,
        ComplexActorReq request,
        CancellationToken cancellationToken)
    {
        _ = entrySpot;
        _ = context;
        cancellationToken.ThrowIfCancellationRequested();
        actor.DisplayName = request.DisplayName;
        evidence.Add(
            $"actor-complex|rid={evidence.Rid}|actor={actor.ActorId}|name={request.DisplayName}"
            + $"|level={request.Level}|tags={string.Join(",", request.Tags)}"
            + $"|attrs={string.Join(",", request.Attributes.OrderBy(static pair => pair.Key).Select(static pair => $"{pair.Key}:{pair.Value}"))}");
        return ValueTask.FromResult(new ComplexActorRes(
            actor.ActorId,
            request.DisplayName,
            request.Level,
            request.Tags,
            request.Attributes));
    }
}
