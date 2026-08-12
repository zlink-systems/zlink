using RuntimeMonitoring.Server.Service.Support;
using RuntimeMonitoring.Shared;
using Zlink.Framework.Contracts.Actors;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Handlers;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Contracts.Spots;
using Zlink.Framework.Contracts.Timers;

namespace RuntimeMonitoring.Server.Service.Handlers;

internal sealed class ProfileRequestHandler(
    EvidenceStore evidence,
    ApplicationDispatchGate? applicationGate = null)
    : IZLinkRequestHandler<ProfileReq, ProfileRes>
{
    public async ValueTask<ProfileRes> HandleAsync(
        ProfileReq request,
        IZLinkMessageContext context,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        evidence.Add($"profile-request|rid={evidence.Rid}|marker={request.Marker}|value={request.Value}");
        if (string.Equals(
                request.Value,
                "application-gate",
                StringComparison.Ordinal))
        {
            evidence.Add(
                $"application-gate-enter|rid={evidence.Rid}|marker={request.Marker}");
            await (applicationGate
                   ?? throw new InvalidOperationException(
                       "Application dispatch gate is not configured."))
                .WaitAsync(cancellationToken);
            evidence.Add(
                $"application-gate-exit|rid={evidence.Rid}|marker={request.Marker}");
        }
        return new ProfileRes(
            $"profile:{request.Value}",
            evidence.Rid,
            request.Marker);
    }
}

internal sealed class MonitoringActor(
    string actorId,
    IZLinkActorContext context) : IZLinkActor
{
    public string ActorId { get; } = actorId;

    public IZLinkActorContext Context { get; } = context;
}

internal sealed class MonitoringActorFactory : IZLinkActorFactory<MonitoringActor>
{
    public ValueTask<MonitoringActor> CreateAsync(
        IZLinkActorContext context,
        CancellationToken cancellationToken = default)
    {
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.FromResult(new MonitoringActor(context.ActorId, context));
    }
}

internal sealed class MonitoringEntrySpot(IZLinkEntrySpotContext context)
    : IZLinkEntrySpot<MonitoringActor>
{
    public IZLinkEntrySpotContext Context { get; } = context;

    public ValueTask<ZLinkActorCreateResponse> OnCreateActorAsync(
        MonitoringActor actor,
        ZLinkMessage createRequest,
        CancellationToken cancellationToken)
    {
        _ = actor;
        _ = createRequest;
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.FromResult(ZLinkActorCreateResponse.Accept());
    }

    public ValueTask<ZLinkSpotActorJoinResult> OnActorJoinAsync(
        string actorId,
        ZLinkMessage request,
        CancellationToken cancellationToken)
    {
        _ = actorId;
        _ = request;
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.FromResult(ZLinkSpotActorJoinResult.Accept());
    }

    public ValueTask OnJoinedActorAsync(
        MonitoringActor actor,
        CancellationToken cancellationToken)
    {
        _ = actor;
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.CompletedTask;
    }

    public ValueTask OnLeaveActorAsync(
        MonitoringActor actor,
        CancellationToken cancellationToken)
    {
        _ = actor;
        cancellationToken.ThrowIfCancellationRequested();
        return ValueTask.CompletedTask;
    }

    public async ValueTask OnInitializeAsync(CancellationToken cancellationToken)
    {
        await Context.AddTimer<FailingTimerHandler>(
            "failing",
            TimeSpan.FromMilliseconds(50),
            new ZLinkTimerOptions { StopOnUnhandledException = false },
            cancellationToken);
        await Context.AddTimer<FailingTimerHandler>(
            "stopping",
            TimeSpan.FromMilliseconds(50),
            new ZLinkTimerOptions { StopOnUnhandledException = true },
            cancellationToken);
    }
}

internal sealed class FailingTimerHandler : IZLinkSpotTimerHandler<MonitoringEntrySpot>
{
    public ValueTask HandleAsync(
        MonitoringEntrySpot spot,
        ZLinkTimerTick tick,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        throw new InvalidOperationException("monitoring timer failure");
    }
}

internal sealed class MonitoringSubjectSpot(IZLinkSpotContext context)
    : IZLinkSpot
{
    public IZLinkSpotContext Context { get; } = context;

    public void Configure()
    {
        Context.Handlers.AddSubscribe<MonitoringSubjectHandler>(
            RuntimeMonitoringNames.SpotChannel,
            "monitor.dynamic");
        Context.Handlers.AddSubscribe<MonitoringSubjectHandler>(
            RuntimeMonitoringNames.SpotChannel,
            "monitor.blocker");
    }

}

internal sealed class MonitoringSubjectHandler(
    EvidenceStore evidence,
    ApplicationDispatchGate gate)
    : IZLinkSpotSubscriptionHandler<MonitoringSubjectSpot, ProfileEvent>
{
    public async ValueTask HandleAsync(
        MonitoringSubjectSpot spot,
        ProfileEvent message,
        ZLinkPublishMessageContext context,
        CancellationToken cancellationToken)
    {
        _ = context;
        if (spot.Context.SpotId == "monitor-blocked")
        {
            evidence.Add(
                $"application-gate-enter|spot={spot.Context.SpotId}|topic={context.Topic}");
            await gate.WaitAsync(cancellationToken).ConfigureAwait(false);
        }
        cancellationToken.ThrowIfCancellationRequested();
        evidence.Add(
            $"logical-publish|spot={spot.Context.SpotId}|topic={context.Topic}|marker={message.Marker}");
    }
}
