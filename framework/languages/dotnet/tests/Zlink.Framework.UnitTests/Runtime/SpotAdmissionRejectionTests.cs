using Microsoft.Extensions.DependencyInjection;
using Xunit.Abstractions;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class SpotAdmissionRejectionTests(ITestOutputHelper output)
{
    private const string Packet = "SpotAdmissionProbe";
    private static readonly TimeSpan Wait = TimeSpan.FromSeconds(5);

    [Theory]
    [InlineData(
        (int)ZLinkAcceptedWorkAdmission.Accepted,
        "internal_failure",
        "Accepted SPOT work reached the rejection path."
    )]
    [InlineData((int)ZLinkAcceptedWorkAdmission.Closed, "shutting_down", "SPOT is closed.")]
    [InlineData(
        (int)ZLinkAcceptedWorkAdmission.RelocationMoving,
        "unavailable",
        "SPOT is relocating."
    )]
    [InlineData((int)ZLinkAcceptedWorkAdmission.Closing, "not_found", "SPOT is closing.")]
    public void RejectionReturnsTheAdmissionError(int admission, string errorCode, string message)
    {
        const string channel = "spot-admission";
        var services = new ServiceCollection();
        services.AddZLinkFramework(static _ => { });
        using var provider = services.BuildServiceProvider();
        var runtime = provider.GetRequiredService<ZLinkFrameworkRuntime>();
        ZLinkEnvelopeHeader? reply = null;
        var received = new ZLinkBackendRouteReceived(
            ZLinkEnvelopeCodec.EncodeParts(CreateHeader(channel), null, null, null),
            sourceNodeRid: RoutingId.From("admission-source"),
            spotId: "admission-spot",
            requestSeq: 1UL,
            reply: parts =>
            {
                reply = ZLinkEnvelopeCodec.DecodeHeader(parts, validateFlow: false);
                return SubmitResult.Ok;
            }
        );

        ZLinkSpotActivationDispatcher.RejectApplicationRouteForDrain(
            received,
            channel,
            (ZLinkAcceptedWorkAdmission)admission,
            runtime
        );

        Assert.NotNull(reply);
        output.WriteLine($"{admission}: {reply.ErrorCode}: {reply.ErrorMessage}");
        Assert.Equal(ZLinkMessageKind.Error, reply.Kind);
        Assert.Equal(errorCode, reply.ErrorCode);
        Assert.Equal(message, reply.ErrorMessage);
    }

    [Fact]
    public async Task CommittedExplicitCloseReportsTheReasonOnTheRejectedRequestFlow()
    {
        var directory =
            Environment.GetEnvironmentVariable("ZLINK_TEST_FLOW_DIR") ?? Path.GetTempPath();
        var path = Path.Combine(directory, $"spot-admission-{Guid.NewGuid():N}.flow");
        output.WriteLine($"Message flow file: {path}");
        using var flow = new TestHostMessageFlowListener(path);
        await using var host = await SpotCloseHost.StartAsync();
        host.Runtime.Registration.DispatchOptions.Diagnostics.SetLevel(
            ZLinkDiagnosticsLevel.Normal
        );
        var spot = await host.CreateSpotAsync();
        var node = host.Runtime.GetSpotNodeRuntime(SpotCloseHost.MeshName).Node.RequestOperations;
        var header = CreateHeader(SpotCloseHost.MeshName) with
        {
            FlowId = "0193cd00-6000-7e00-a000-000000000001",
            FlowOrigin = ZLinkFlowOrigin.Inbound,
        };
        var parts = ZLinkEnvelopeCodec.EncodeParts(header, null, null, null);

        host.State.HoldOnClosing = true;
        var close = host.CloseAsync(spot);
        try
        {
            await host.State.OnClosingEntered.Task.WaitAsync(Wait);

            // Address the committed owner directly so sender-side resolution cannot
            // complete the request before it reaches the receiver's admission path.
            using var reply = await node.RequestToSpotDirectAsync(
                string.Empty,
                spot.NodeRid,
                spot.SpotId,
                spot.ObjectGeneration,
                parts,
                SendFlags.None,
                default,
                Wait,
                CancellationToken.None
            );
            var result = ZLinkEnvelopeCodec.DecodeHeader(reply.Parts, validateFlow: false);
            Assert.Equal("not_found", result.ErrorCode);
            TelemetryReceipt.Wait(() =>
                flow.ReadLines().Any(line => line.Contains("ExplicitClose"))
            );
            var rejection = Assert.Single(
                flow.ReadLines(),
                line => line.Contains($"packet={Packet}") && line.Contains("ExplicitClose")
            );
            Assert.Contains("event=zlink.dispatch_error", rejection);
            Assert.Contains($"flow={header.FlowId}", rejection);
            Assert.Contains(header.CorrelationId!, rejection);
            Assert.Contains(spot.SpotId, rejection);
            Assert.Equal("SPOT is closing.", result.ErrorMessage);
            output.WriteLine(rejection);
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(parts);
            host.State.ReleaseOnClosing.TrySetResult();
            Assert.Equal(true, await close.WaitAsync(Wait));
        }
    }

    private static ZLinkEnvelopeHeader CreateHeader(string channel) =>
        new(
            ZLinkMessageKind.Request,
            channel,
            Packet,
            ZLinkEnvelopeCodec.DefaultContentType,
            "admission-request",
            null,
            null,
            null,
            null
        );
}
