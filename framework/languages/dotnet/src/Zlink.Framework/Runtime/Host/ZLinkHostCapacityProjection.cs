using Zlink.Framework.Runtime.Execution;

namespace Zlink.Framework.Runtime.Host;

/// <summary>
/// Combines the Core HWM port and Application Job Queue aggregate into one
/// host-capacity epoch. It owns reset ordering; exporters only read this view.
/// </summary>
internal sealed class ZLinkHostCapacityProjection
{
    private readonly IZLinkBackendRuntimeContext _context;
    private readonly ZLinkInboundDispatchOptionsModel _configuration;
    private readonly ZLinkApplicationJobQueue _applicationJobQueue;
    private readonly ZLinkStateLane _lane = new();
    private ulong _measurementEpoch;

    internal ZLinkHostCapacityProjection(
        IZLinkBackendRuntimeContext context,
        ZLinkInboundDispatchOptionsModel configuration,
        ZLinkApplicationJobQueue applicationJobQueue)
    {
        _context = context;
        _configuration = configuration;
        _applicationJobQueue = applicationJobQueue;
    }

    internal ZLinkHostCapacityStatus GetStatus()
    {
        return AwaitStateLane(_lane.RunAsync(() =>
        {
            var core = _context.GetCoreHwmBudgetSnapshot();
            return new ZLinkHostCapacityStatus(
                _measurementEpoch,
                MapCoreStatus(core, _configuration),
                _applicationJobQueue.GetStatus());
        }));
    }

    internal void ResetMetrics()
    {
        AwaitStateLane(_lane.RunAsync(() =>
        {
            _context.ResetCoreHwmBudgetMetrics();
            _applicationJobQueue.ResetMetrics();
            _measurementEpoch = checked(_measurementEpoch + 1);
        }));
    }

    internal static ZLinkCoreHwmStatus MapCoreStatus(
        CoreHwmBudgetSnapshot snapshot,
        ZLinkInboundDispatchOptionsModel configuration) =>
        new(
            configuration.CoreHwmMemoryLimitBytes,
            configuration.CoreHwmBudgetBytes,
            configuration.CoreHwmProfile,
            snapshot.EffectiveCoreBudgetBytes,
            snapshot.TotalAppliedHwmBytes,
            snapshot.CoreQueueAccountedBytes,
            snapshot.ApplicationAccountedBytes,
            snapshot.CurrentAccountedBytes,
            snapshot.ProvisionalAccountedBytes,
            snapshot.PeakAccountedBytes,
            snapshot.CompletionCurrentAccountedBytes,
            snapshot.CompletionPeakAccountedBytes,
            snapshot.CompletionPendingMessageCount,
            snapshot.TotalMessagingAccountedBytes,
            snapshot.MonitorQueueAppliedHwmBytes,
            snapshot.MonitorQueueAccountedBytes,
            snapshot.TotalInstanceAppliedHwmBytes,
            snapshot.TotalInstanceAccountedBytes,
            snapshot.BlockedRatioPpm,
            snapshot.ActiveDirectionalQueueCount,
            snapshot.ActiveCompletionDirectionalQueueCount,
            snapshot.ActiveSendQueueCount,
            snapshot.ActiveReceiveQueueCount,
            snapshot.OutstandingApplicationLeaseCount,
            snapshot.RetiredQueueCount,
            snapshot.DeferredOriginCreditBytes);

    private static T AwaitStateLane<T>(ValueTask<T> operation) =>
        operation.GetAwaiter().GetResult();

    private static void AwaitStateLane(ValueTask operation) =>
        operation.GetAwaiter().GetResult();
}
