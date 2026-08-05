using RuntimeMonitoring.Server.Service.Support;
using RuntimeMonitoring.Shared;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Handlers;
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

internal sealed class MonitoringEntrySpot(IZLinkEntrySpotContext context) : IZLinkEntrySpot
{
    public IZLinkEntrySpotContext Context { get; } = context;

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

internal sealed class MonitoringSubjectSpot(IZLinkSpotContext context) : IZLinkSpot
{
    public IZLinkSpotContext Context { get; } = context;

    public void Configure()
    {
        Context.Handlers.AddSubscribe<MonitoringSubjectHandler>(
            RuntimeMonitoringNames.SpotChannel,
            "monitor.dynamic");
    }
}

internal sealed class MonitoringSubjectHandler(EvidenceStore evidence)
    : IZLinkSpotSubscriptionHandler<MonitoringSubjectSpot, ProfileReq>
{
    public ValueTask HandleAsync(
        MonitoringSubjectSpot spot,
        ProfileReq message,
        ZLinkPublishMessageContext context,
        CancellationToken cancellationToken)
    {
        _ = context;
        cancellationToken.ThrowIfCancellationRequested();
        evidence.Add(
            $"logical-publish|topic=monitor.dynamic|marker={message.Marker}");
        return ValueTask.CompletedTask;
    }
}
