using System.Diagnostics;
using Microsoft.Extensions.Logging;
using Systems.Zlink.Stream.Connector.Runtime.Protocol;
using Zlink.Framework.Runtime.Diagnostics;
using Zlink.Framework.Runtime.Dispatch;

namespace Zlink.Framework.UnitTests;

public sealed class ExactMessageFlowRuntimeTests
{
    [Fact]
    public void RuntimeLevelSharesTheConfiguredLiveCell()
    {
        var options = new ZLinkDiagnosticsOptionsModel();
        options.SetLevel(ZLinkDiagnosticsLevel.Errors);
        options.LiveLevel = new ZLinkDiagnosticsLevelCell(options.ConfiguredLevel);
        var runtime = new ZLinkDiagnosticsRuntimeService(options);

        Assert.Equal(ZLinkDiagnosticsLevel.Errors, runtime.Level);

        runtime.Level = ZLinkDiagnosticsLevel.Detailed;

        Assert.Equal(ZLinkDiagnosticsLevel.Detailed, options.EffectiveLevel);
    }

    [Fact]
    public async Task RuntimeLevelAsyncAndSyncControlsCompleteBeforeObservation()
    {
        var options = new ZLinkDiagnosticsOptionsModel();
        options.LiveLevel = new ZLinkDiagnosticsLevelCell(options.ConfiguredLevel);
        IZLinkDiagnosticsRuntime runtime = new ZLinkDiagnosticsRuntimeService(options);

        await runtime.SetLevelAsync(ZLinkDiagnosticsLevel.Normal);
        Assert.Equal(ZLinkDiagnosticsLevel.Normal, runtime.Level);

        runtime.Level = ZLinkDiagnosticsLevel.Detailed;
        Assert.Equal(ZLinkDiagnosticsLevel.Detailed, options.EffectiveLevel);
    }

    [Fact]
    public void OffLevelSkipsDispatchLoggerAndActivityBeforeEventConstruction()
    {
        var activities = new List<Activity>();
        using var listener = new ActivityListener
        {
            ShouldListenTo = source => source.Name == ZLinkTelemetry.ActivitySourceName,
            Sample = (ref ActivityCreationOptions<ActivityContext> _) =>
                ActivitySamplingResult.AllDataAndRecorded,
            ActivityStopped = activities.Add
        };
        ActivitySource.AddActivityListener(listener);
        var options = new ZLinkDispatchOptionsModel();
        options.Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Off);
        _ = new ZLinkDiagnosticsRuntimeService(options.Diagnostics);
        var logger = new CountingLogger();
        var reporter = new ZLinkDispatchErrorReporter(options);
        var scope = new ZLinkDispatchFlowScope(
            ZLinkDispatchErrorSurface.Channel,
            reporter.Flow.CaptureEnabled,
            ZLinkDispatchMessageKind.Send,
            "packet");

        scope.HandlerMissing(
            reporter,
            ZLinkDispatchErrorAction.Drop);

        Assert.Equal(0, logger.CallCount);
        Assert.Empty(activities);
    }

    [Fact]
    public void DispatchTerminalTraceKeepsTheFlowCapturedAtReceiveBoundary()
    {
        var activities = new List<Activity>();
        using var listener = new ActivityListener
        {
            ShouldListenTo = source =>
                source.Name == ZLinkTelemetry.ActivitySourceName,
            Sample = (ref ActivityCreationOptions<ActivityContext> _) =>
                ActivitySamplingResult.AllDataAndRecorded,
            ActivityStopped = activities.Add
        };
        ActivitySource.AddActivityListener(listener);
        var options = new ZLinkDispatchOptionsModel();
        options.Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
        options.Diagnostics.SetSampleRate(1);
        _ = new ZLinkDiagnosticsRuntimeService(options.Diagnostics);
        var reporter = new ZLinkDispatchErrorReporter(options);
        var requestFlowId = ZlinkStreamFlowId.Create();

        ZLinkDispatchFlowScope scope;
        using (ZLinkFlowContext.Enter(
                   requestFlowId,
                   ZLinkFlowOrigin.Inbound,
                   captureEnabled: true,
                   ZLinkFlowOrigin.Inbound))
        {
            scope = new ZLinkDispatchFlowScope(
                ZLinkDispatchErrorSurface.SpotRoute,
                reporter.Flow.CaptureEnabled,
                ZLinkDispatchMessageKind.Request,
                "request",
                correlationId: "correlation-1",
                spotId: "spot-1");
            scope.Trace(reporter, ZLinkMessageFlowOutcome.Received);
        }

        using (ZLinkFlowContext.Enter(
                   ZlinkStreamFlowId.Create(),
                   ZLinkFlowOrigin.Application,
                   captureEnabled: true,
                   ZLinkFlowOrigin.Application))
            scope.Trace(reporter, ZLinkMessageFlowOutcome.Replied);

        var traces = activities
            .Where(activity =>
                activity.OperationName == "zlink.message_flow")
            .ToArray();
        Assert.Equal(2, traces.Length);
        Assert.All(traces, activity =>
            Assert.Equal(
                requestFlowId,
                activity.GetTagItem("flow_id")));
    }

    private sealed class CountingLogger : ILogger
    {
        public int CallCount { get; private set; }

        public IDisposable? BeginScope<TState>(TState state)
            where TState : notnull => null;

        public bool IsEnabled(LogLevel logLevel) => true;

        public void Log<TState>(
            LogLevel logLevel,
            EventId eventId,
            TState state,
            Exception? exception,
            Func<TState, Exception?, string> formatter) =>
            CallCount++;
    }
}
