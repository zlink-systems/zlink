using Systems.Zlink;
using Xunit;
using Zlink.Framework.Contracts.Channels;
using Zlink.Framework.Contracts.Streams;
using ZoneWorld.Server.Ops.Application.Ops;
using ZoneWorld.Server.Ops.Infrastructure.ZLink.Sessions;
using ZoneWorld.Shared.Contracts;

namespace Zlink.Framework.SampleRegressionTests;

public sealed class ZoneWorldOpsConsoleRegistryTests
{
    [Fact]
    public async Task StaleConsoleFailureDoesNotBlockHealthyConsoleAndIsRemoved()
    {
        var registry = new OpsConsoleRegistry();
        var stale = new TestSessionContext("stale", failSend: true);
        var healthy = new TestSessionContext("healthy", failSend: false);
        registry.Add(stale);
        registry.Add(healthy);

        await registry.BroadcastAsync(Status("node-a"), CancellationToken.None);
        Assert.Equal(1, healthy.SendCount);
        await registry.BroadcastAsync(Status("node-a"), CancellationToken.None);
        Assert.Equal(2, healthy.SendCount);
        Assert.Equal(1, stale.SendCount);
    }

    [Fact]
    public async Task LateDisconnectCannotRemoveReplacementWithTheSameSessionId()
    {
        var registry = new OpsConsoleRegistry();
        var previous = new TestSessionContext("console", failSend: false);
        var replacement = new TestSessionContext("console", failSend: false);
        registry.Add(previous);
        registry.Add(replacement);

        registry.Remove(previous);
        await registry.BroadcastAsync(Status("node-b"), CancellationToken.None);

        Assert.Equal(0, previous.SendCount);
        Assert.Equal(1, replacement.SendCount);
    }

    [Fact]
    public async Task NodeStateReplayRunsAfterWatchReplyRatherThanConnectionLifecycle()
    {
        var registry = new OpsConsoleRegistry();
        var console = new TestSessionContext("console", failSend: false);
        registry.Add(console);

        await registry.BroadcastAsync(Status("node-c"), CancellationToken.None);
        await registry.ReplayNodesAsync(console, CancellationToken.None);

        Assert.Equal(2, console.SendCount);
    }

    [Fact]
    public async Task ReportCorrelatesAConnectionWhenTheRuntimePeerEventArrivedFirst()
    {
        var registry = new NodeRegistry();
        var routingId = RoutingId.From("replacement-node-rid");

        await registry.ApplyLiveRoutingIdsAsync(
            new HashSet<string> { routingId.ToString() },
            CancellationToken.None);

        var correlated = await registry.ApplyReportAsync(
            new ReportNodeStatusMsg(
                NodeIds.East,
                [ZoneIds.NorthEast, ZoneIds.SouthEast],
                PlayerCount: 0,
                Maintenance: false),
            routingId,
            CancellationToken.None);

        Assert.True(correlated);
        var node = Assert.Single(registry.Snapshot());
        Assert.Equal(NodeIds.East, node.NodeId);
        Assert.True(node.Registered);
        Assert.True(node.Connected);

        var duplicate = await registry.ApplyReportAsync(
            new ReportNodeStatusMsg(
                NodeIds.East,
                [ZoneIds.NorthEast, ZoneIds.SouthEast],
                PlayerCount: 1,
                Maintenance: false),
            routingId,
            CancellationToken.None);

        Assert.False(duplicate);
    }

    [Fact]
    public async Task ReportCorrelatesAConnectionWhenALogicalNodePublishesANewRoutingId()
    {
        var registry = new NodeRegistry();
        var previousRoutingId = RoutingId.From("previous-node-rid");
        var replacementRoutingId = RoutingId.From("replacement-node-rid");
        var zones = new[] { ZoneIds.NorthEast, ZoneIds.SouthEast };

        await registry.ApplyLiveRoutingIdsAsync(
            new HashSet<string> { previousRoutingId.ToString() },
            CancellationToken.None);
        Assert.True(await registry.ApplyReportAsync(
            new ReportNodeStatusMsg(NodeIds.East, zones, 0, false),
            previousRoutingId,
            CancellationToken.None));

        // The old lease can remain visible while the replacement publishes its
        // new descriptor, so both RIDs are live at this observation boundary.
        await registry.ApplyLiveRoutingIdsAsync(
            new HashSet<string>
            {
                previousRoutingId.ToString(),
                replacementRoutingId.ToString()
            },
            CancellationToken.None);

        var correlated = await registry.ApplyReportAsync(
            new ReportNodeStatusMsg(NodeIds.East, zones, 1, false),
            replacementRoutingId,
            CancellationToken.None);

        Assert.True(correlated);
        Assert.True(Assert.Single(registry.Snapshot()).Connected);
    }

    private static NodeStatusNotify Status(string nodeId) =>
        new(nodeId, Registered: true, Connected: true, Maintenance: false, [], PlayerCount: 0);

    private sealed class TestSessionContext(string sessionId, bool failSend) : IZLinkSessionContext
    {
        private readonly TestSessionClient _client = new(failSend);

        public string SessionId { get; } = sessionId;
        public RoutingId? RoutingId => null;
        public string? LocalAddr => null;
        public string? RemoteAddr => null;
        public IZLinkSessionClient Client => _client;
        public IZLinkSessionActors Actors => null!;
        public IZLinkSessionHandlerRegistry Handlers => null!;
        public int SendCount => _client.SendCount;
        public ValueTask CloseAsync() => ValueTask.CompletedTask;
    }

    private sealed class TestSessionClient(bool failSend) : IZLinkSessionClient
    {
        public int SendCount { get; private set; }

        public IZLinkSessionSendCall Send<TMessage>(TMessage message)
        {
            _ = message;
            return new TestSendCall(() =>
            {
                SendCount++;
                if (failSend) throw new InvalidOperationException("session transport unavailable");
            });
        }

        public IZLinkSessionReplyCall Reply<TMessage>(TMessage message) => throw new NotSupportedException();
    }

    private sealed class TestSendCall(Action submit) : IZLinkSessionSendCall
    {
        public IZLinkSessionSendCall Metadata(string key, string value) => this;
        public IZLinkSessionSendCall Metadata(ZLinkMessageMetadata metadata) => this;
        public IZLinkSessionSendCall Compress() => this;
        public ValueTask Async(CancellationToken cancellationToken = default)
        {
            cancellationToken.ThrowIfCancellationRequested();
            submit();
            return ValueTask.CompletedTask;
        }
    }
}
