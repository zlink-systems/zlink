using System.Diagnostics;
using System.Reflection;
using Zlink.Framework.Runtime.Service;

namespace Zlink.Framework.UnitTests;

public sealed class MeshLivenessPhysicalCloseTests
{
    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task ExpiryRemovesTargetAndTerminatesPendingSend_ReceiveKeepsTarget(
        bool receive
    ) => await RunExpiryAsync(receive, observeProbeSubmission: false);

    [Fact]
    public async Task ReceivedRecordKeepsTargetAfterProbeSubmission() =>
        await RunExpiryAsync(receive: true, observeProbeSubmission: true);

    private static async Task RunExpiryAsync(bool receive, bool observeProbeSubmission)
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        context.Options.AutoHwmEnabled = false;
        await using var source = context.CreateRouterSocket();
        await using var target = context.CreateDealerSocket();
        await using var node = new ZLinkManagedMeshNode(context, "liveness-close");
        var rid = RoutingId.From("liveness-target");
        target.SetRoutingId(rid);
        const int recordBytes = 65536;
        target.Options.ReceiveHighWaterMark = recordBytes + 64;
        target.SetReceiveFlowState(ReceiveFlowState.Paused);
        source.Options.SendHighWaterMark = recordBytes + 64;
        var endpoint = $"inproc://liveness-close-{Guid.NewGuid():N}";
        source.Bind(endpoint);
        target.Connect(endpoint);
        using var completions = new TestCompletionPollerDriver(source);
        using var physicalMonitor = source.MonitorOpen(SocketEvent.Disconnected);
        var readyBy = Stopwatch.GetTimestamp() + 2 * Stopwatch.Frequency;
        while (source.RoutesSnapshot().Count == 0 && Stopwatch.GetTimestamp() < readyBy)
            await Task.Delay(1);
        var route = Assert.Single(source.RoutesSnapshot());
        var peer = new ZLinkMeshPeer(1)
        {
            RoutingId = rid,
            PhysicalRoutingId = rid,
            RouteGeneration = route.RouteGeneration,
            Admitted = true,
            State = MeshPeerState.Admitted,
            Liveness = new ZLinkServiceLiveness(0),
        };
        var peers = Field<Dictionary<RoutingId, ZLinkMeshPeer>>(node, "_peersByRid");
        peers.Add(rid, peer);
        SetField(node, "_socket", source);
        SetField(node, "_activeSocketGeneration", Field<ulong>(node, "_lifecycleGeneration"));
        using var monitor = node.OpenMonitor();
        try
        {
            var submitted = new List<Task>();
            for (var i = 0; i < 16; i++)
            {
                var submission = source
                    .Send(rid)
                    .Message(Message.From(new byte[recordBytes]))
                    .Async();
                submitted.Add(submission.Admitted);
            }
            var pending = submitted[^1];
            await Task.Delay(25);
            Assert.False(pending.IsCompleted, $"{pending.Status}: {pending.Exception}");
            var terminals = 0;
            var observed = pending.ContinueWith(_ => Interlocked.Increment(ref terminals));
            if (receive)
                peer.Liveness.RecordReceived(14 * Stopwatch.Frequency);
            Tick(node, 15 * Stopwatch.Frequency);
            if (observeProbeSubmission)
            {
                // Observe after the existing control-submit worker has run. Its
                // asynchronous Core admission may remain pending under backpressure.
                var scheduler = Field<ConcurrentExclusiveSchedulerPair>(
                    node,
                    "_routedSubmitScheduler"
                );
                await Task.Factory.StartNew(
                    () => Assert.True(peer.Admitted),
                    CancellationToken.None,
                    TaskCreationOptions.None,
                    scheduler.ExclusiveScheduler
                );
            }
            if (receive)
            {
                Assert.True(peer.Admitted);
                Assert.Single(source.RoutesSnapshot());
                Assert.False(pending.IsCompleted);
                Assert.Null(monitor.Recv(RecvFlags.DontWait));
                Assert.Null(physicalMonitor.Recv(RecvFlags.DontWait));
                source.DisconnectRid(rid);
            }
            else
            {
                Assert.False(peer.Admitted);
                Tick(node, 16 * Stopwatch.Frequency);
                Assert.Equal(
                    MeshMonitorEventKind.PeerClosed,
                    monitor.Recv(RecvFlags.DontWait)?.Kind
                );
                Assert.Null(monitor.Recv(RecvFlags.DontWait));
            }
            var error = await Assert.ThrowsAsync<ZlinkSubmitException>(() =>
                pending.WaitAsync(TimeSpan.FromSeconds(2))
            );
            Assert.Equal(ZlinkSubmitException.ErrorCode.NotFound, error.Result);
            await observed;
            Assert.Equal(1, terminals);
            Assert.Equal(1, ZlinkPoll.Poll([physicalMonitor], 2000));
            Assert.Equal(
                MonitorEventType.Disconnected,
                physicalMonitor.Recv(RecvFlags.DontWait)?.Event
            );
            Assert.Null(physicalMonitor.Recv(RecvFlags.DontWait));
        }
        finally
        {
            SetField(node, "_socket", null);
            SetField(node, "_activeSocketGeneration", 0UL);
        }
    }

    [Fact]
    public async Task RetiredDeadlineDoesNotCloseReplacementAdmission()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(context, "liveness-close");
        var retired = new ZLinkServiceLiveness(0);
        var peer = new ZLinkMeshPeer(1)
        {
            RoutingId = RoutingId.From("replacement"),
            RouteGeneration = 12,
            Admitted = true,
            State = MeshPeerState.Admitted,
            Liveness = new ZLinkServiceLiveness(14 * Stopwatch.Frequency),
        };
        Field<Dictionary<RoutingId, ZLinkMeshPeer>>(node, "_peersByRid").Add(peer.RoutingId, peer);
        Assert.True(retired.IsExpired(15 * Stopwatch.Frequency));
        Tick(node, 15 * Stopwatch.Frequency);
        Assert.True(peer.Admitted);
        Assert.Equal(12UL, peer.RouteGeneration);
    }

    private static void Tick(ZLinkManagedMeshNode node, long now) =>
        typeof(ZLinkManagedMeshNode)
            .GetMethod("ProcessInfrastructure", BindingFlags.Instance | BindingFlags.NonPublic)!
            .Invoke(node, [now]);

    private static T Field<T>(object owner, string name) =>
        (T)
            owner
                .GetType()
                .GetField(name, BindingFlags.Instance | BindingFlags.NonPublic)!
                .GetValue(owner)!;

    private static void SetField(object owner, string name, object? value) =>
        owner
            .GetType()
            .GetField(name, BindingFlags.Instance | BindingFlags.NonPublic)!
            .SetValue(owner, value);
}
