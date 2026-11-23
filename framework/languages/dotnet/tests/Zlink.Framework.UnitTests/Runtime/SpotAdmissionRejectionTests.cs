using Microsoft.Extensions.DependencyInjection;
using Xunit.Abstractions;
using Zlink.Framework.AspNetCore;
using Zlink.Framework.Contracts.Messaging;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Execution;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests.Runtime;

public sealed class SpotAdmissionRejectionTests(ITestOutputHelper output)
{
    private const string Packet = "SpotAdmissionProbe";
    private const string ProbeFlow = "0193cd00-6000-7e00-a000-000000000001";
    private const string Correlation = "admission-request";
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

    [Theory]
    [InlineData("local Spot manager")]
    [InlineData("Spot context")]
    [InlineData("remote Spot manager (command 48)")]
    public async Task CommittedExplicitCloseReportsItsSourceOnlyOnTheFlow(string source)
    {
        using var flow = StartFlow();
        await using var host = await SpotCloseHost.StartAsync();
        EnableDiagnostics(host);
        var spot = await host.CreateSpotAsync();
        await using var peer =
            source == "remote Spot manager (command 48)"
                ? await SpotCloseHost.StartAsync(host.Store)
                : null;
        if (peer is not null)
        {
            using var ready = new CancellationTokenSource(Wait);
            await Task.WhenAll(
                peer.WaitForPeerAsync(host, ready.Token),
                host.WaitForPeerAsync(peer, ready.Token)
            );
        }
        host.State.HoldOnClosing = true;
        var close = StartCloseAsync();
        try
        {
            await host.State.OnClosingEntered.Task.WaitAsync(Wait);

            var result = await RequestDirectAsync(
                host,
                spot.NodeRid,
                spot.SpotId,
                spot.ObjectGeneration
            );
            var rejection = WaitForFlow(flow, "ExplicitClose");
            Assert.Equal("not_found", result.ErrorCode);
            Assert.Equal("SPOT is closing.", result.ErrorMessage);
            Assert.Contains(spot.SpotId, rejection);
            Assert.Contains($"Close reason: ExplicitClose via {source}.", rejection);
            output.WriteLine(rejection);
        }
        finally
        {
            host.State.ReleaseOnClosing.TrySetResult();
            Assert.True(await close.WaitAsync(Wait));
        }

        async Task<bool> StartCloseAsync()
        {
            if (source == "Spot context")
            {
                host.State.HandlerMode = "closeAndReturn";
                Assert.Equal("handlerReply", await host.RequestAsync(spot.SpotId));
                return await host.State.ContextCloseTask!;
            }
            return await (peer ?? host).Manager.CloseAsync(spot);
        }
    }

    [Fact]
    public async Task CancellationWhileDrainingStartedCallsIsRecordedOnTheRejectedRequestFlow()
    {
        using var flow = StartFlow();
        await using var host = await SpotCloseHost.StartAsync();
        EnableDiagnostics(host);
        ZLinkSpotActivation? instance = null;
        host.State.OnInstanceInitializing = context => instance = (ZLinkSpotActivation)context;
        const string spotId = "close-admission-cancel";
        var initial = await host.RequestInstanceAsync(spotId);
        var activation = Assert.IsAssignableFrom<ZLinkSpotActivation>(instance);
        var entered = new TaskCompletionSource<Task<bool>>(
            TaskCreationOptions.RunContinuationsAsynchronously
        );
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var started = activation
            ._serial.ExecuteAsync(
                (_, _, ct) =>
                {
                    entered.SetResult(
                        ((IZLinkInstanceSpotContext)activation).CloseAsync().AsTask()
                    );
                    return ZLinkSerialTurn.Current!.YieldFrameworkCallAsync(
                        _ => new ValueTask(release.Task),
                        ct
                    );
                },
                0,
                CancellationToken.None
            )
            .AsTask();
        Task<bool>? close = null;
        try
        {
            close = await entered.Task.WaitAsync(Wait);
            TelemetryReceipt.Wait(() => activation.HasCommittedClose);
            var result = await RequestDirectAsync(
                host,
                activation.NodeRid,
                spotId,
                initial.Generation
            );
            var rejection = WaitForFlow(flow, "ExplicitClose");
            Assert.Equal("not_found", result.ErrorCode);
            Assert.Equal(0, host.State.OnClosingCalls);
            Assert.False(close.IsCompleted);
            Assert.Contains("Close reason: ExplicitClose via Spot context.", rejection);
            output.WriteLine(rejection);

            activation.CancelActiveOperations();
            release.TrySetResult();
            var failure = await Assert.ThrowsAnyAsync<OperationCanceledException>(() =>
                close.WaitAsync(Wait)
            );
            Assert.Equal(0, host.State.OnClosingCalls);
            output.WriteLine(failure.ToString());
            var failedClose = WaitForFlow(flow, "CanceledException");
            Assert.Contains(spotId, failedClose);
            output.WriteLine(failedClose);
        }
        finally
        {
            release.TrySetResult();
            await Record.ExceptionAsync(() => started.WaitAsync(Wait));
            if (close is not null)
                await Record.ExceptionAsync(() => close.WaitAsync(Wait));
        }
    }

    private TestHostMessageFlowListener StartFlow()
    {
        var directory =
            Environment.GetEnvironmentVariable("ZLINK_TEST_FLOW_DIR") ?? Path.GetTempPath();
        var path = Path.Combine(directory, $"spot-admission-{Guid.NewGuid():N}.flow");
        output.WriteLine($"Message flow file: {path}");
        return new TestHostMessageFlowListener(path);
    }

    private static void EnableDiagnostics(SpotCloseHost host) =>
        host.Runtime.Registration.DispatchOptions.Diagnostics.SetLevel(
            ZLinkDiagnosticsLevel.Normal
        );

    private static string WaitForFlow(TestHostMessageFlowListener flow, string marker)
    {
        bool Matches(string line) =>
            line.Contains("event=zlink.dispatch_error")
            && line.Contains($"packet={Packet}")
            && line.Contains($"flow={ProbeFlow}")
            && line.Contains(Correlation)
            && line.Contains(marker);
        TelemetryReceipt.Wait(() => flow.ReadLines().Any(Matches));
        return Assert.Single(flow.ReadLines(), Matches);
    }

    private static async Task<ZLinkEnvelopeHeader> RequestDirectAsync(
        SpotCloseHost host,
        RoutingId nodeRid,
        string spotId,
        ulong generation
    )
    {
        // A direct request reaches the receiving Spot even when the Store says Closing.
        var node = host.Runtime.GetSpotNodeRuntime(SpotCloseHost.MeshName).Node.RequestOperations;
        var header = CreateHeader(SpotCloseHost.MeshName) with
        {
            FlowId = ProbeFlow,
            FlowOrigin = ZLinkFlowOrigin.Inbound,
        };
        var parts = ZLinkEnvelopeCodec.EncodeParts(header, null, null, null);
        try
        {
            using var reply = await node.RequestToSpotDirectAsync(
                string.Empty,
                nodeRid,
                spotId,
                generation,
                parts,
                SendFlags.None,
                default,
                Wait,
                CancellationToken.None
            );
            return ZLinkEnvelopeCodec.DecodeHeader(reply.Parts, validateFlow: false);
        }
        finally
        {
            ZLinkMessageParts.DisposeAll(parts);
        }
    }

    private static ZLinkEnvelopeHeader CreateHeader(string channel) =>
        new(
            ZLinkMessageKind.Request,
            channel,
            Packet,
            ZLinkEnvelopeCodec.DefaultContentType,
            Correlation,
            null,
            null,
            null,
            null
        );
}
