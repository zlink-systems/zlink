using System.Diagnostics;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging;
using Zlink.Framework.Runtime.Channels;
using Zlink.Framework.Runtime.Diagnostics;
using Zlink.Framework.Runtime.Dispatch;

namespace Zlink.Framework.UnitTests;

public sealed class MessageFlowTracerTests
{
    [Fact]
    public void PhaseAndClosedValueVocabularyMatchesSpec26()
    {
        Assert.Equal(
            ["received", "admitted", "dispatched", "completed", "replied", "sent",
                "reply_received", "backpressured", "dropped"],
            Enum.GetValues<ZLinkMessageFlowOutcome>()
                .Select(ZLinkTraceFormat.OutcomeKey)
                .OrderBy(value => Array.IndexOf(
                    ["received", "admitted", "dispatched", "completed", "replied", "sent",
                        "reply_received", "backpressured", "dropped"], value)));
        Assert.Equal(
            ["actor", "actor_relocation", "channel", "classic_fanout", "instance_spot", "node",
                "spot", "stream"],
            Enum.GetValues<ZLinkDispatchErrorSurface>()
                .Select(ZLinkTraceFormat.SurfaceKey)
                .Distinct()
                .OrderBy(value => value));
        Assert.Equal(
            ["control", "error", "request", "response", "send"],
            Enum.GetValues<ZLinkDispatchMessageKind>()
                .Select(ZLinkTraceFormat.MessageKindKey)
                .Distinct()
                .OrderBy(value => value));
        Assert.Equal(
            ["backpressured", "cancelled", "dropped", "failed", "shutdown", "succeeded"],
            Enum.GetValues<ZLinkMessageFlowResult>()
                .Select(ZLinkTraceFormat.ResultKey)
                .OrderBy(value => value));
        Assert.Equal(
            ["activation_rejected", "activation_timeout", "backpressure", "location_unavailable",
                "shutdown", "stale_target", "target_closed"],
            Enum.GetValues<ZLinkMessageFlowReason>()
                .Select(value => ZLinkTraceFormat.MessageReasonKey(value)!)
                .OrderBy(value => value));
        Assert.Equal(
            ["backpressure", "decode_error", "handler_exception", "invalid_frame", "no_handler",
                "reply_path_missing", "shutdown", "stale_target", "unexpected_reply"],
            Enum.GetValues<ZLinkDispatchErrorReason>()
                .Select(ZLinkTraceFormat.DispatchReasonKey)
                .OrderBy(value => value));
        Assert.Equal(
            ["drop", "fail_caller", "reply_error"],
            Enum.GetValues<ZLinkDispatchErrorAction>()
                .Select(ZLinkTraceFormat.DispatchActionKey)
                .OrderBy(value => value));
    }

    [Fact]
    public void MessageFlowActivityUsesNormalizedSpecAttributes()
    {
        var activities = CaptureActivities(out var listener);
        using (listener)
        {
            var options = new ZLinkDispatchOptionsModel();
            options.Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Detailed);
            options.Diagnostics.IncludeMessageSizes(true);
            var tracer = new ZLinkMessageFlowTracer(options);
            tracer.Trace(new ZLinkMessageFlowEvent(
                ZLinkMessageFlowOutcome.ReplyReceived,
                ZLinkDispatchErrorSurface.RouteMeshChannel,
                ZLinkDispatchMessageKind.ActorRequest,
                "packet",
                "channel",
                "topic",
                "corr",
                "source",
                SpotId: "spot",
                ActorId: "actor",
                MessageSize: 17,
                MeshName: "mesh",
                TargetRid: "target",
                ServerRid: "server",
                InstanceSpotType: "type",
                ActivationState: "ready",
                DurationSeconds: 0.25,
                Result: ZLinkMessageFlowResult.Cancelled));
        }

        var activity = Assert.Single(activities);
        Assert.Equal("zlink.message_flow", activity.GetTagItem("event_id"));
        Assert.Equal("reply_received", activity.GetTagItem("phase"));
        Assert.Equal("channel", activity.GetTagItem("surface"));
        Assert.Equal("request", activity.GetTagItem("message_kind"));
        Assert.Equal("cancelled", activity.GetTagItem("outcome"));
        Assert.Equal("route_mesh", activity.GetTagItem("channel_route_kind"));
        Assert.Equal("mesh", activity.GetTagItem("mesh_name"));
        Assert.Equal("server", activity.GetTagItem("server_rid"));
        Assert.Equal("type", activity.GetTagItem("instance_spot_type"));
        Assert.Equal("ready", activity.GetTagItem("activation_state"));
        Assert.Equal(17L, activity.GetTagItem("message_size_bytes"));
        Assert.Equal(0.25, activity.GetTagItem("duration_seconds"));
    }

    [Fact]
    public void StructuredLogUsesExactSpecKeysAndPrefix()
    {
        var logger = new CapturingLogger();
        var options = new ZLinkDispatchOptionsModel();
        options.Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
        options.Diagnostics.SetSampleRate(1);
        var tracer = new ZLinkMessageFlowTracer(options, logger);

        tracer.Trace(new ZLinkMessageFlowEvent(
            ZLinkMessageFlowOutcome.Admitted,
            ZLinkDispatchErrorSurface.RouteMeshChannel,
            ZLinkDispatchMessageKind.Request,
            "packet",
            "channel",
            SourceRid: "source",
            MeshName: "mesh",
            TargetRid: "target"));

        Assert.StartsWith("zlink flow: ", logger.Message);
        Assert.Equal(
            ["event_id", "phase", "surface", "kind", "mesh", "channel", "channel_route",
                "source_rid", "target_rid", "packet", "outcome"],
            logger.Fields.Select(field => field.Key));
    }

    [Fact]
    public void DispatchErrorIsOneUnsampledRecordWithoutPseudoPhase()
    {
        var activities = CaptureActivities(out var listener);
        using (listener)
        {
            var options = new ZLinkDispatchOptionsModel();
            options.Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Errors);
            options.Diagnostics.SetSampleRate(0);
            var reporter = new ZLinkDispatchErrorReporter(options, new CapturingLogger());

            reporter.Report(new ZLinkDispatchFailure(
                ZLinkDispatchErrorSurface.ClassicFanout,
                ZLinkDispatchMessageKind.Send,
                ZLinkDispatchErrorReason.HandlerMissing,
                ZLinkDispatchErrorAction.Drop,
                "packet",
                "channel"));
        }

        var activity = Assert.Single(activities);
        Assert.Equal("zlink.dispatch_error", activity.GetTagItem("event_id"));
        Assert.Null(activity.GetTagItem("phase"));
        Assert.Equal("classic_fanout", activity.GetTagItem("surface"));
        Assert.Equal("send", activity.GetTagItem("message_kind"));
        Assert.Equal("failed", activity.GetTagItem("outcome"));
        Assert.Equal("no_handler", activity.GetTagItem("reason"));
        Assert.Equal("drop", activity.GetTagItem("action"));
        Assert.Null(activity.GetTagItem("channel_route_kind"));
    }

    [Fact]
    public void FlowlessSamplingUsesGenerationAndLocalSequence()
    {
        var logger = new CapturingLogger();
        var options = new ZLinkDispatchOptionsModel();
        options.Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
        options.Diagnostics.SetSampleRate(0.5);
        var tracer = new ZLinkMessageFlowTracer(options, logger);

        for (var index = 0; index < 64; index++)
            tracer.Trace(new ZLinkMessageFlowEvent(
                ZLinkMessageFlowOutcome.Received,
                ZLinkDispatchErrorSurface.Node,
                ZLinkDispatchMessageKind.Send));

        Assert.InRange(logger.CallCount, 1, 63);
    }

    [Fact]
    public async Task ClassicFanoutEmitsOnlyMissingHandlerDispatchError()
    {
        var activities = CaptureActivities(out var listener);
        using (listener)
        {
            var registration = new ZLinkFrameworkRegistration();
            registration.DispatchOptions.Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
            using var services = new ServiceCollection().BuildServiceProvider();
            var pipeline = new ZLinkChannelPublishDispatchPipeline(
                new ZLinkHandlerRegistry([]),
                new ZLinkHandlerDispatcher(
                    services.GetRequiredService<IServiceScopeFactory>(),
                    registration),
                static _ => new HashSet<string>(StringComparer.Ordinal),
                new ZLinkDispatchErrorReporter(registration.DispatchOptions),
                registration.Codecs);
            var header = new ZLinkEnvelopeHeader(
                ZLinkMessageKind.Publish,
                "channel",
                "MissingEvent",
                ZLinkEnvelopeCodec.DefaultContentType,
                null,
                null,
                "topic",
                null,
                null,
                "source");
            using var message = new TopicMessage();

            await pipeline.DispatchAsync(
                "channel",
                message,
                header,
                CancellationToken.None);
        }

        var activity = Assert.Single(activities);
        Assert.Equal("zlink.dispatch_error", activity.GetTagItem("event_id"));
        Assert.Equal("classic_fanout", activity.GetTagItem("surface"));
        Assert.Equal("send", activity.GetTagItem("message_kind"));
        Assert.Equal("no_handler", activity.GetTagItem("reason"));
        Assert.Equal("drop", activity.GetTagItem("action"));
        Assert.Null(activity.GetTagItem("phase"));
        Assert.Null(activity.GetTagItem("channel_route_kind"));
    }

    private static List<Activity> CaptureActivities(out ActivityListener listener)
    {
        var activities = new List<Activity>();
        listener = new ActivityListener
        {
            ShouldListenTo = source => source.Name == ZLinkTelemetry.ActivitySourceName,
            Sample = (ref ActivityCreationOptions<ActivityContext> _) =>
                ActivitySamplingResult.AllDataAndRecorded,
            ActivityStopped = activities.Add
        };
        ActivitySource.AddActivityListener(listener);
        return activities;
    }

    private sealed class CapturingLogger : ILogger
    {
        public int CallCount { get; private set; }
        public string Message { get; private set; } = string.Empty;
        public IReadOnlyList<KeyValuePair<string, object?>> Fields { get; private set; } = [];

        public IDisposable? BeginScope<TState>(TState state) where TState : notnull => null;
        public bool IsEnabled(LogLevel logLevel) => true;

        public void Log<TState>(
            LogLevel logLevel,
            EventId eventId,
            TState state,
            Exception? exception,
            Func<TState, Exception?, string> formatter)
        {
            CallCount++;
            Message = formatter(state, exception);
            Fields = state is IEnumerable<KeyValuePair<string, object?>> fields
                ? fields.ToArray()
                : [];
        }
    }
}
