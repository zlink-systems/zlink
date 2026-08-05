using System.Diagnostics;
using Microsoft.Extensions.DependencyInjection;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Codecs;
using Zlink.Framework.Runtime.Dispatch;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class SpotRouteDispatcherTests
{
    [Fact]
    public async Task MalformedSendPayload_IsDropped_WithoutBlockingTheNextMessage()
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
        var probe = new DispatchProbe();
        using var services = new ServiceCollection()
            .AddSingleton(probe)
            .BuildServiceProvider();
        await using var handlerInstances = new ZLinkScopedHandlerInstanceOwner(services);
        var spot = new TestSpot();
        var packets = new ZLinkSpotPacketRegistry();
        packets.Add(typeof(TestHandler));
        packets.Bind(spot);
        var codecs = new ZLinkCodecRegistryBuilder();
        var invoker = new ZLinkSpotHandlerInvoker(handlerInstances, spot, codecs, null);
        var options = new ZLinkDispatchOptionsModel();
        options.Diagnostics.SetLevel(ZLinkDiagnosticsLevel.Normal);
        _ = new ZLinkDiagnosticsRuntimeService(options.Diagnostics);
        var dispatcher = new ZLinkSpotRouteDispatcher(
            "route",
            "target-spot",
            packets,
            () => invoker,
            codecs,
            new ZLinkDispatchErrorReporter(options));

        var malformed = Encode(new TestMessage("ignored"));
        malformed[1].Dispose();
        malformed = [malformed[0], Message.From("{")];
        var valid = Encode(new TestMessage("next"));

        try
        {
            await dispatcher.DispatchAsync(CreateRoutedReceived(malformed), CancellationToken.None);
            await dispatcher.DispatchAsync(CreateRoutedReceived(valid), CancellationToken.None);
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(malformed);
            ZLinkMessageParts.DisposeAll(valid);
        }

        Assert.Equal(["next"], probe.Values);
        var spotTraces = activities.Where(activity =>
            activity.OperationName == "zlink.message_flow"
            && Equals(
                activity.GetTagItem("surface")?.ToString(),
                "SpotRoute"));
        Assert.NotEmpty(spotTraces);
        Assert.All(spotTraces, activity =>
            Assert.Equal(
                "target-spot",
                activity.GetTagItem("spot_id")));
    }

    private static IReadOnlyList<Message> Encode(TestMessage message)
    {
        return ZLinkEnvelopeCodec.EncodeParts(
            new ZLinkEnvelopeHeader(
                ZLinkMessageKind.Command,
                "route",
                nameof(TestMessage),
                ZLinkEnvelopeCodec.DefaultContentType,
                null,
                null,
                null,
                null,
                null),
            message,
            typeof(TestMessage),
            null);
    }

    // RouteMesh 10.0.0 delivers per-spot route records to the dispatcher as a
    // framework-owned ZLinkBackendRouteReceived (the pump drains Core claims into
    // this record). The record owns a private copy of the parts; the dispatcher
    // disposes it via `using (received)`, so the caller's originals stay valid.
    private static ZLinkBackendRouteReceived CreateRoutedReceived(IReadOnlyList<Message> parts)
    {
        var owned = parts
            .Select(static part => Message.From(part.AsReadOnlySpan()))
            .ToArray();
        return new ZLinkBackendRouteReceived(
            owned,
            sourceNodeRid: RoutingId.From("route-source"),
            spotId: "route-target",
            requestSeq: null,
            reply: null);
    }

    private sealed record TestMessage(string Value);

    private sealed class DispatchProbe
    {
        public List<string> Values { get; } = [];
    }

    private sealed class TestSpot : IZLinkSpot
    {
        public IZLinkSpotContext Context => throw new NotSupportedException();
    }

    private sealed class TestHandler(DispatchProbe probe)
        : IZLinkSpotPacketHandler<TestSpot, TestMessage>
    {
        public ValueTask HandleAsync(
            TestSpot spot,
            TestMessage message,
            CancellationToken cancellationToken)
        {
            _ = spot;
            cancellationToken.ThrowIfCancellationRequested();
            probe.Values.Add(message.Value);
            return ValueTask.CompletedTask;
        }
    }
}
