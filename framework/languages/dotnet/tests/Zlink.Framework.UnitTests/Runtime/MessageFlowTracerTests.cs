using System.Collections.Concurrent;
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
            [
                "received",
                "admitted",
                "dispatched",
                "completed",
                "replied",
                "sent",
                "reply_received",
                "backpressured",
                "dropped",
            ],
            Enum.GetValues<ZLinkMessageFlowOutcome>()
                .Select(ZLinkTraceFormat.OutcomeKey)
                .OrderBy(value =>
                    Array.IndexOf(
                        [
                            "received",
                            "admitted",
                            "dispatched",
                            "completed",
                            "replied",
                            "sent",
                            "reply_received",
                            "backpressured",
                            "dropped",
                        ],
                        value
                    )
                )
        );
        Assert.Equal(
            [
                "actor",
                "actor_relocation",
                "channel",
                "classic_fanout",
                "instance_spot",
                "node",
                "spot",
                "stream",
            ],
            Enum.GetValues<ZLinkDispatchErrorSurface>()
                .Select(ZLinkTraceFormat.SurfaceKey)
                .Distinct()
                .OrderBy(value => value)
        );
        Assert.Equal(
            ["control", "error", "request", "response", "send"],
            Enum.GetValues<ZLinkDispatchMessageKind>()
                .Select(ZLinkTraceFormat.MessageKindKey)
                .Distinct()
                .OrderBy(value => value)
        );
        Assert.Equal(
            ["backpressured", "cancelled", "dropped", "failed", "shutdown", "succeeded"],
            Enum.GetValues<ZLinkMessageFlowResult>()
                .Select(ZLinkTraceFormat.ResultKey)
                .OrderBy(value => value)
        );
        Assert.Equal(
            [
                "activation_rejected",
                "activation_timeout",
                "backpressure",
                "location_unavailable",
                "shutdown",
                "stale_target",
                "target_closed",
            ],
            Enum.GetValues<ZLinkMessageFlowReason>()
                .Select(value => ZLinkTraceFormat.MessageReasonKey(value)!)
                .OrderBy(value => value)
        );
        Assert.Equal(
            [
                "backpressure",
                "decode_error",
                "handler_exception",
                "invalid_frame",
                "no_handler",
                "reply_path_missing",
                "shutdown",
                "stale_target",
                "unexpected_reply",
            ],
            Enum.GetValues<ZLinkDispatchErrorReason>()
                .Select(ZLinkTraceFormat.DispatchReasonKey)
                .OrderBy(value => value)
        );
        Assert.Equal(
            ["drop", "fail_caller", "reply_error"],
            Enum.GetValues<ZLinkDispatchErrorAction>()
                .Select(ZLinkTraceFormat.DispatchActionKey)
                .OrderBy(value => value)
        );
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
            tracer.Trace(
                new ZLinkMessageFlowEvent(
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
                    Result: ZLinkMessageFlowResult.Cancelled
                )
                {
                    FlowId = "018f2b63-9d4a-7abc-8def-0123456789ab",
                    FlowOrigin = ZLinkFlowOrigin.Application,
                }
            );
            TelemetryReceipt.Wait(() => activities.Count == 1);
        }

        var activity = Assert.Single(activities);
        Assert.Equal("zlink.message_flow", activity.GetTagItem("event_id"));
        Assert.Equal("reply_received", activity.GetTagItem("phase"));
        Assert.Equal("channel", activity.GetTagItem("surface"));
        Assert.Equal("request", activity.GetTagItem("message_kind"));
        Assert.Equal("packet", activity.GetTagItem("packet_name"));
        Assert.Equal("018f2b63-9d4a-7abc-8def-0123456789ab", activity.GetTagItem("flow_id"));
        Assert.Equal("application", activity.GetTagItem("flow_origin"));
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

        tracer.Trace(
            new ZLinkMessageFlowEvent(
                ZLinkMessageFlowOutcome.Admitted,
                ZLinkDispatchErrorSurface.RouteMeshChannel,
                ZLinkDispatchMessageKind.Request,
                "packet",
                "channel",
                SourceRid: "source",
                MeshName: "mesh",
                TargetRid: "target"
            )
        );
        TelemetryReceipt.Wait(() => logger.CallCount == 1);

        Assert.StartsWith("zlink flow: ", logger.Message);
        Assert.Equal(
            //  본문 key는 관찰 스펙의 "Structured log 대체 표기"가 고정한다. 첫 key는
            //  `event`이며 telemetry attribute 이름 `event_id`와 다른 집합이다.
            [
                "event",
                "phase",
                "surface",
                "kind",
                "mesh",
                "channel",
                "channel_route",
                "source_rid",
                "target_rid",
                "packet",
                "outcome",
            ],
            logger.Fields.Select(field => field.Key)
        );
    }

    [Fact]
    public void DispatchErrorIsOneUnsampledRecordWithoutPseudoPhase()
    {
        const int errorMessageMaxLength = 512;
        var activities = CaptureActivities(out var listener);
        var message =
            "Authorization: Bearer auth Bearer standalone password=p token=t "
            + new string('x', errorMessageMaxLength + 1)
            + "\n   at Secret.Handler()";
        var expectedMessage = (
            "Authorization: <redacted> Bearer <redacted> password=<redacted> token=<redacted> "
            + new string('x', errorMessageMaxLength + 1)
        )[..errorMessageMaxLength];
        using (listener)
        {
            var options = new ZLinkDispatchOptionsModel();
            options.Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Errors);
            options.Diagnostics.SetSampleRate(0);
            var logger = new CapturingLogger();
            var reporter = new ZLinkDispatchErrorReporter(options, logger);
            InvalidOperationException exception;
            try
            {
                throw new InvalidOperationException(message);
            }
            catch (InvalidOperationException caught)
            {
                exception = caught;
            }

            reporter.Report(
                new ZLinkDispatchFailure(
                    ZLinkDispatchErrorSurface.ClassicFanout,
                    ZLinkDispatchMessageKind.Send,
                    ZLinkDispatchErrorReason.HandlerException,
                    ZLinkDispatchErrorAction.Drop,
                    "packet",
                    "channel",
                    Exception: exception
                )
            );
            TelemetryReceipt.Wait(() => activities.Count == 1);

            Assert.Contains(
                logger.Fields,
                field =>
                    field.Key == "error_type" && (string?)field.Value == "InvalidOperationException"
            );
            Assert.Contains(
                logger.Fields,
                field => field.Key == "error_message" && (string?)field.Value == expectedMessage
            );
            Assert.DoesNotContain("Bearer auth", logger.Message);
            Assert.DoesNotContain("Bearer standalone", logger.Message);
            Assert.DoesNotContain("password=p", logger.Message);
            Assert.DoesNotContain("token=t", logger.Message);
            Assert.DoesNotContain("Secret.Handler", logger.Message);
        }

        var activity = Assert.Single(activities);
        Assert.Equal("zlink.dispatch_error", activity.GetTagItem("event_id"));
        Assert.Null(activity.GetTagItem("phase"));
        Assert.Equal("classic_fanout", activity.GetTagItem("surface"));
        Assert.Equal("send", activity.GetTagItem("message_kind"));
        Assert.Equal("failed", activity.GetTagItem("outcome"));
        Assert.Equal("handler_exception", activity.GetTagItem("reason"));
        Assert.Equal("drop", activity.GetTagItem("action"));
        Assert.Equal("InvalidOperationException", activity.GetTagItem("error_type"));
        Assert.Equal(expectedMessage, activity.GetTagItem("error_message"));
        Assert.Null(activity.GetTagItem("channel_route_kind"));
    }

    [Fact]
    public void DispatchErrorKeepsEmptyMessageInTraceAndStructuredLog()
    {
        var activities = CaptureActivities(out var listener);
        var logger = new CapturingLogger();
        using (listener)
        {
            var options = new ZLinkDispatchOptionsModel();
            options.Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Errors);
            var reporter = new ZLinkDispatchErrorReporter(options, logger);

            reporter.Report(
                new ZLinkDispatchFailure(
                    ZLinkDispatchErrorSurface.ClassicFanout,
                    ZLinkDispatchMessageKind.Send,
                    ZLinkDispatchErrorReason.HandlerException,
                    ZLinkDispatchErrorAction.Drop,
                    PacketName: null,
                    ChannelName: null,
                    Exception: new InvalidOperationException(string.Empty)
                )
            );
            TelemetryReceipt.Wait(() => activities.Count == 1);
        }

        Assert.Contains(
            logger.Fields,
            field =>
                field.Key == "error_type" && (string?)field.Value == "InvalidOperationException"
        );
        var logMessage = Assert.Single(logger.Fields, field => field.Key == "error_message");
        Assert.Equal(string.Empty, (string?)logMessage.Value);
        var activity = Assert.Single(activities);
        Assert.Contains(
            activity.TagObjects,
            tag => tag.Key == "error_type" && (string?)tag.Value == "InvalidOperationException"
        );
        var traceMessage = Assert.Single(activity.TagObjects, tag => tag.Key == "error_message");
        Assert.Equal(string.Empty, (string?)traceMessage.Value);
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
            tracer.Trace(
                new ZLinkMessageFlowEvent(
                    ZLinkMessageFlowOutcome.Received,
                    ZLinkDispatchErrorSurface.Node,
                    ZLinkDispatchMessageKind.Send
                )
            );
        WaitForSamplingDelivery();

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
                    registration
                ),
                static _ => new HashSet<string>(StringComparer.Ordinal),
                new ZLinkDispatchErrorReporter(registration.DispatchOptions),
                registration.Codecs
            );
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
                "source"
            );
            using var message = new TopicMessage();

            await pipeline.DispatchAsync("channel", message, header, CancellationToken.None);
            TelemetryReceipt.Wait(() => activities.Count == 1);
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

    [Theory]
    [InlineData(ZLinkDiagnosticsLevel.Off, false)]
    [InlineData(ZLinkDiagnosticsLevel.Errors, true)]
    [InlineData(ZLinkDiagnosticsLevel.Normal, true)]
    public void StageFailureUsesExistingFlowGateAndPreservesDiagnosticDetails(
        ZLinkDiagnosticsLevel level,
        bool expected
    )
    {
        var activities = CaptureActivities(out var listener);
        var logger = new CapturingLogger();
        using (listener)
        {
            var options = new ZLinkDispatchOptionsModel();
            options.Diagnostics.SetLevel(level);
            options.Diagnostics.SetSampleRate(0);
            var tracer = new ZLinkMessageFlowTracer(options, logger);
            if (tracer.Enabled(ZLinkMessageFlowOutcome.Completed, ZLinkMessageFlowResult.Failed))
            {
                var error = ZLinkTraceFormat.DispatchErrorDetails(
                    new InvalidOperationException("restore failed token=private\nsecret stack")
                );
                tracer.Trace(
                    new ZLinkMessageFlowEvent(
                        ZLinkMessageFlowOutcome.Completed,
                        ZLinkDispatchErrorSurface.ActorRelocation,
                        ZLinkDispatchMessageKind.Control,
                        Result: ZLinkMessageFlowResult.Failed
                    )
                    {
                        Detail = "target_prepare_failed",
                        ErrorType = error.Type,
                        ErrorMessage = error.Message,
                    }
                );
                TelemetryReceipt.Wait(() => activities.Count == 1);
            }
        }
        if (!expected)
        {
            Assert.Empty(activities);
            Assert.Equal(0, logger.CallCount);
            return;
        }
        var activity = Assert.Single(activities);
        Assert.Equal("completed", activity.GetTagItem("phase"));
        Assert.Equal("failed", activity.GetTagItem("outcome"));
        Assert.Equal("target_prepare_failed", activity.GetTagItem("detail"));
        Assert.Equal(nameof(InvalidOperationException), activity.GetTagItem("error_type"));
        Assert.Equal("restore failed token=<redacted>", activity.GetTagItem("error_message"));
        Assert.Contains(
            logger.Fields,
            field => field.Key == "detail" && Equals(field.Value, "target_prepare_failed")
        );
        Assert.Contains(
            logger.Fields,
            field =>
                field.Key == "error_message"
                && Equals(field.Value, "restore failed token=<redacted>")
        );
    }

    [Theory]
    [InlineData(ZLinkDiagnosticsLevel.Errors, false, false)]
    [InlineData(ZLinkDiagnosticsLevel.Errors, true, false)]
    [InlineData(ZLinkDiagnosticsLevel.Normal, false, false)]
    [InlineData(ZLinkDiagnosticsLevel.Normal, true, false)]
    [InlineData(ZLinkDiagnosticsLevel.Errors, false, true)]
    [InlineData(ZLinkDiagnosticsLevel.Errors, true, true)]
    public void DispatchErrorFlowUsesProcessingLevel(
        ZLinkDiagnosticsLevel level,
        bool explicitFlow,
        bool changeLevelAfterGate
    )
    {
        var activities = CaptureActivities(out var listener);
        using (listener)
        {
            var outerOptions = new ZLinkDispatchOptionsModel();
            outerOptions.Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
            var outerTracer = new ZLinkMessageFlowTracer(outerOptions);
            using var scope = ZLinkFlowContext.Enter(
                null,
                null,
                outerTracer.CaptureEnabled,
                ZLinkFlowOrigin.Inbound
            );
            var ambient = Assert.IsType<ZLinkFlowValue>(ZLinkFlowContext.Current);
            const string explicitFlowId = "018f2b63-9d4a-7abc-8def-0123456789ab";
            Assert.NotEqual(explicitFlowId, ambient.FlowId);
            var options = new ZLinkDispatchOptionsModel();
            options.Diagnostics.SetLevel(
                changeLevelAfterGate ? ZLinkDiagnosticsLevel.Normal : level
            );
            options.Diagnostics.SetSampleRate(0);
            var logger = new CapturingLogger();
            var reporter = new ZLinkDispatchErrorReporter(options, logger);
            Assert.True(reporter.Enabled);
            if (changeLevelAfterGate)
                options.Diagnostics.SetLevel(level);

            reporter.Report(
                new ZLinkDispatchFailure(
                    ZLinkDispatchErrorSurface.SpotActor,
                    ZLinkDispatchMessageKind.Request,
                    ZLinkDispatchErrorReason.HandlerException,
                    ZLinkDispatchErrorAction.Drop,
                    "packet",
                    CorrelationId: "corr",
                    Exception: new InvalidOperationException("failed"),
                    FlowId: explicitFlow ? explicitFlowId : null,
                    FlowOrigin: explicitFlow ? ZLinkFlowOrigin.Application : null,
                    StreamSessionId: "session"
                )
            );
            TelemetryReceipt.Wait(() => activities.Count == 1);

            Assert.Equal(1, logger.CallCount);
            var activity = Assert.Single(activities);
            Assert.Equal("zlink.dispatch_error", activity.GetTagItem("event_id"));
            Assert.Equal("drop", activity.GetTagItem("action"));
            Assert.Equal("corr", activity.GetTagItem("correlation_id"));
            Assert.Equal("session", activity.GetTagItem("stream_session_id"));
            Assert.Equal("InvalidOperationException", activity.GetTagItem("error_type"));
            Assert.Equal("failed", activity.GetTagItem("error_message"));
            Assert.Contains(
                logger.Fields,
                field => field.Key == "corr" && Equals(field.Value, "corr")
            );
            Assert.Contains(
                logger.Fields,
                field => field.Key == "session" && Equals(field.Value, "session")
            );
            Assert.Contains(
                logger.Fields,
                field =>
                    field.Key == "error_type" && Equals(field.Value, "InvalidOperationException")
            );
            Assert.Contains(
                logger.Fields,
                field => field.Key == "error_message" && Equals(field.Value, "failed")
            );
            AssertObservedFlow(
                logger,
                activity,
                level == ZLinkDiagnosticsLevel.Normal
                    ? explicitFlow
                        ? explicitFlowId
                        : ambient.FlowId
                    : null,
                explicitFlow ? "application" : "inbound"
            );
        }
    }

    [Theory]
    [InlineData(ZLinkDiagnosticsLevel.Errors, false, false)]
    [InlineData(ZLinkDiagnosticsLevel.Errors, true, false)]
    [InlineData(ZLinkDiagnosticsLevel.Normal, false, false)]
    [InlineData(ZLinkDiagnosticsLevel.Normal, true, false)]
    [InlineData(ZLinkDiagnosticsLevel.Errors, false, true)]
    [InlineData(ZLinkDiagnosticsLevel.Errors, true, true)]
    public void FailedAndDroppedFlowUseProcessingLevel(
        ZLinkDiagnosticsLevel level,
        bool explicitFlow,
        bool changeLevelAfterGate
    )
    {
        var outerOptions = new ZLinkDispatchOptionsModel();
        outerOptions.Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
        var outerTracer = new ZLinkMessageFlowTracer(outerOptions, new CapturingLogger());
        using var scope = ZLinkFlowContext.Enter(
            null,
            null,
            outerTracer.CaptureEnabled,
            ZLinkFlowOrigin.Inbound
        );
        var ambient = Assert.IsType<ZLinkFlowValue>(ZLinkFlowContext.Current);
        const string explicitFlowId = "018f2b63-9d4a-7abc-8def-0123456789ab";
        Assert.NotEqual(explicitFlowId, ambient.FlowId);
        foreach (
            var outcome in new[]
            {
                ZLinkMessageFlowOutcome.Completed,
                ZLinkMessageFlowOutcome.Dropped,
            }
        )
        {
            var activities = CaptureActivities(out var listener);
            using (listener)
            {
                var options = new ZLinkDispatchOptionsModel();
                options.Diagnostics.SetLevel(
                    changeLevelAfterGate ? ZLinkDiagnosticsLevel.Normal : level
                );
                options.Diagnostics.SetSampleRate(0);
                var logger = new CapturingLogger();
                var tracer = new ZLinkMessageFlowTracer(options, logger);
                Assert.True(tracer.Enabled(outcome, ZLinkMessageFlowResult.Failed));
                if (changeLevelAfterGate)
                    options.Diagnostics.SetLevel(level);
                tracer.Trace(
                    new ZLinkMessageFlowEvent(
                        outcome,
                        ZLinkDispatchErrorSurface.SpotActor,
                        ZLinkDispatchMessageKind.Request,
                        PacketName: "packet",
                        CorrelationId: "corr",
                        Result: ZLinkMessageFlowResult.Failed
                    )
                    {
                        FlowId = explicitFlow ? explicitFlowId : string.Empty,
                        FlowOrigin = explicitFlow ? ZLinkFlowOrigin.Application : null,
                        Detail = "target_prepare_failed",
                        ErrorType = "InvalidOperationException",
                        ErrorMessage = "failed",
                        StreamSessionId = "session",
                    }
                );
                TelemetryReceipt.Wait(() => activities.Count == 1);

                Assert.Equal(1, logger.CallCount);
                var activity = Assert.Single(activities);
                Assert.Equal("zlink.message_flow", activity.GetTagItem("event_id"));
                Assert.Equal(ZLinkTraceFormat.OutcomeKey(outcome), activity.GetTagItem("phase"));
                Assert.Equal("failed", activity.GetTagItem("outcome"));
                Assert.Equal("corr", activity.GetTagItem("correlation_id"));
                Assert.Equal("session", activity.GetTagItem("stream_session_id"));
                Assert.Equal("target_prepare_failed", activity.GetTagItem("detail"));
                Assert.Equal("InvalidOperationException", activity.GetTagItem("error_type"));
                Assert.Equal("failed", activity.GetTagItem("error_message"));
                Assert.Contains(
                    logger.Fields,
                    field => field.Key == "corr" && Equals(field.Value, "corr")
                );
                Assert.Contains(
                    logger.Fields,
                    field => field.Key == "session" && Equals(field.Value, "session")
                );
                Assert.Contains(
                    logger.Fields,
                    field => field.Key == "detail" && Equals(field.Value, "target_prepare_failed")
                );
                Assert.Contains(
                    logger.Fields,
                    field =>
                        field.Key == "error_type"
                        && Equals(field.Value, "InvalidOperationException")
                );
                Assert.Contains(
                    logger.Fields,
                    field => field.Key == "error_message" && Equals(field.Value, "failed")
                );
                AssertObservedFlow(
                    logger,
                    activity,
                    level == ZLinkDiagnosticsLevel.Normal
                        ? explicitFlow
                            ? explicitFlowId
                            : ambient.FlowId
                        : null,
                    explicitFlow ? "application" : "inbound"
                );
            }
        }
    }

    [Fact]
    public void OffPreservesCallSiteConstructionGuardsInsideAmbientFlow()
    {
        using var scope = ZLinkFlowContext.Enter(null, null, true, ZLinkFlowOrigin.Inbound);
        var activities = CaptureActivities(out var listener);
        using (listener)
        {
            var options = new ZLinkDispatchOptionsModel();
            options.Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Off);
            var logger = new CapturingLogger();
            var reporter = new ZLinkDispatchErrorReporter(options, logger);
            var constructed = 0;
            if (reporter.Enabled)
            {
                constructed++;
                reporter.Report(
                    new ZLinkDispatchFailure(
                        ZLinkDispatchErrorSurface.SpotActor,
                        ZLinkDispatchMessageKind.Request,
                        ZLinkDispatchErrorReason.HandlerException,
                        ZLinkDispatchErrorAction.Drop,
                        "packet"
                    )
                );
            }
            if (
                reporter.Flow.Enabled(
                    ZLinkMessageFlowOutcome.Completed,
                    ZLinkMessageFlowResult.Failed
                )
            )
            {
                constructed++;
                reporter.Flow.Trace(
                    new ZLinkMessageFlowEvent(
                        ZLinkMessageFlowOutcome.Completed,
                        ZLinkDispatchErrorSurface.SpotActor,
                        ZLinkDispatchMessageKind.Request,
                        Result: ZLinkMessageFlowResult.Failed
                    )
                );
            }
            Assert.Equal(0, constructed);
            Assert.Equal(0, logger.CallCount);
            Assert.Empty(activities);
        }
    }

    private static void AssertObservedFlow(
        CapturingLogger logger,
        Activity activity,
        string? expectedFlowId,
        string expectedOrigin
    )
    {
        if (expectedFlowId is null)
        {
            Assert.DoesNotContain(logger.Fields, field => field.Key is "flow" or "origin");
            Assert.Null(activity.GetTagItem("flow_id"));
            Assert.Null(activity.GetTagItem("flow_origin"));
            return;
        }
        Assert.Equal(
            expectedFlowId,
            Assert.Single(logger.Fields, field => field.Key == "flow").Value
        );
        Assert.Equal(
            expectedOrigin,
            Assert.Single(logger.Fields, field => field.Key == "origin").Value
        );
        Assert.Equal(expectedFlowId, activity.GetTagItem("flow_id"));
        Assert.Equal(expectedOrigin, activity.GetTagItem("flow_origin"));
    }

    private static void WaitForSamplingDelivery() =>
        Assert.True(
            SpinWait.SpinUntil(
                () => ZLinkMessageFlowDelivery.Shared.State is { Pending: 0, Executing: false },
                TimeSpan.FromSeconds(5)
            )
        );

    private static ConcurrentQueue<Activity> CaptureActivities(out ActivityListener listener)
    {
        var activities = new ConcurrentQueue<Activity>();
        listener = new ActivityListener
        {
            ShouldListenTo = source => source.Name == ZLinkTelemetry.ActivitySourceName,
            Sample = (ref ActivityCreationOptions<ActivityContext> _) =>
                ActivitySamplingResult.AllDataAndRecorded,
            ActivityStopped = activities.Enqueue,
        };
        ActivitySource.AddActivityListener(listener);
        return activities;
    }

    private sealed class CapturingLogger : ILogger
    {
        private int _callCount;
        public int CallCount => Volatile.Read(ref _callCount);
        public string Message { get; private set; } = string.Empty;
        public IReadOnlyList<KeyValuePair<string, object?>> Fields { get; private set; } = [];

        public IDisposable? BeginScope<TState>(TState state)
            where TState : notnull => null;

        public bool IsEnabled(LogLevel logLevel) => true;

        public void Log<TState>(
            LogLevel logLevel,
            EventId eventId,
            TState state,
            Exception? exception,
            Func<TState, Exception?, string> formatter
        )
        {
            Message = formatter(state, exception);
            Fields = state is IEnumerable<KeyValuePair<string, object?>> fields
                ? fields.ToArray()
                : [];
            Interlocked.Increment(ref _callCount);
        }
    }
}
