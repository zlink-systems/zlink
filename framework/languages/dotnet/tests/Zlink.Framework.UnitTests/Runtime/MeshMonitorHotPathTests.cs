using System.Reflection;
using Zlink.Framework.Runtime.Execution;
using Zlink.Framework.Runtime.Service;

namespace Zlink.Framework.UnitTests;

public sealed class MeshMonitorHotPathTests
{
    [Fact]
    public async Task PublicationObservesCurrentMonitorsAndStateWithoutWaitingForAnotherTurn()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var node = new ZLinkManagedMeshNode(context, "monitor-hot-path");
        using var early = node.OpenMonitor(MeshMonitorEventMask.MessageSubmitted);
        node.PublishDraining();
        using var late = node.OpenMonitor(MeshMonitorEventMask.MessageSubmitted);
        var lane = (ZLinkStateLane)typeof(ZLinkManagedMeshNode)
            .GetField("_lane", BindingFlags.Instance | BindingFlags.NonPublic)!.GetValue(node)!;
        var entered = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        var release = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
        Assert.True(lane.TryPost(async () =>
        {
            entered.SetResult();
            await release.Task;
        }));
        await entered.Task.WaitAsync(TimeSpan.FromSeconds(3));
        Task? publication = null;
        try
        {
            publication = Task.Run(() => typeof(ZLinkManagedMeshNode)
                .GetMethod("Publish", BindingFlags.Instance | BindingFlags.NonPublic,
                    [typeof(MeshMonitorEventKind), typeof(RoutingId), typeof(string),
                        typeof(MeshOperationId), typeof(int), typeof(int)])!
                .Invoke(node, [MeshMonitorEventKind.MessageSubmitted, default(RoutingId),
                    "worker", default(MeshOperationId), 0, 0]));
            await publication.WaitAsync(TimeSpan.FromSeconds(3));

            foreach (var monitor in new[] { early, late })
            {
                var item = Assert.IsType<MeshMonitorEvent>(monitor.Recv(RecvFlags.DontWait));
                Assert.Equal(MeshMonitorEventKind.MessageSubmitted, item.Kind);
                Assert.Equal(MeshNodeState.Draining, item.MeshState);
                Assert.Equal("worker", item.ChannelName);
                Assert.Equal(1UL, monitor.Status().SubmittedMessages);
            }
        }
        finally
        {
            release.TrySetResult();
            if (publication is not null)
                await publication.WaitAsync(TimeSpan.FromSeconds(3));
        }
    }
}
