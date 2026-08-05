using System.Diagnostics;
using Zlink.Framework.Runtime.Diagnostics;
using Zlink.Framework.Runtime.Dispatch;

namespace Zlink.Framework.UnitTests;

public sealed class MessageFlowTracerTests
{
    [Fact]
    public void SpotFlowUsesSpotIdTagInsteadOfRoutingIdTag()
    {
        Activity? stopped = null;
        using var listener = new ActivityListener
        {
            ShouldListenTo = source => source.Name == ZLinkTelemetry.ActivitySourceName,
            Sample = (ref ActivityCreationOptions<ActivityContext> _) =>
                ActivitySamplingResult.AllDataAndRecorded,
            ActivityStopped = activity => stopped = activity
        };
        ActivitySource.AddActivityListener(listener);
        var options = new ZLinkDiagnosticsOptionsModel();
        options.SetLevel(ZLinkDiagnosticsLevel.Normal);
        _ = new ZLinkDiagnosticsRuntimeService(options);

        ZLinkTelemetry.TraceFlowEvent(
            "received",
            new ZLinkMessageFlowEvent(
                ZLinkMessageFlowOutcome.Received,
                ZLinkDispatchErrorSurface.SpotRoute,
                ZLinkDispatchMessageKind.Send,
                SpotId: "spot-1"),
            "dispatch",
            "accepted");

        Assert.NotNull(stopped);
        Assert.Contains(stopped!.TagObjects, tag => tag.Key == "spot_id"
                                                    && Equals(tag.Value, "spot-1"));
        Assert.DoesNotContain(stopped.TagObjects, tag => tag.Key == "zlink.spot.rid");
    }
}
