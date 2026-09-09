using System.Collections.Concurrent;
using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Backend.DotNet;
using Zlink.Framework.Runtime.Diagnostics;
using Zlink.Framework.Runtime.Dispatch;
using Zlink.Framework.Runtime.Execution;
using Zlink.Framework.Runtime.Service;

namespace Zlink.Framework.UnitTests;

public sealed class MeshApplicationWorkerTests
{
    [Fact]
    public async Task WorkersPersistAcrossBatches_AndDrainSuspendedHandlersOnShutdown()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        using var jobs = new ZLinkApplicationJobQueue(new(
            ZLinkApplicationJobQueueProfile.Balanced, 128, 2, 128));
        await using var node = new ZLinkManagedMeshNode(context, "workers", applicationJobQueue: jobs);
        var rid = RoutingId.From("worker-node");
        node.SetRoutingId(rid);
        node.Start();
        var failures = new Failures();
        var runner = new ZLinkRuntimeTaskRunner(failures, CancellationToken.None);
        var pump = new ZLinkMeshDispatchPump(node, new ZLinkMeshCompletionTable(), jobs);
        var firstStarted = Signal();
        var secondStarted = Signal();
        var release = Signal();
        var returned = Signal();
        ZLinkBackendRouteReceived? first = null;
        var count = 0;
        pump.SetNodeRouteHandler(async (records, ct) =>
        {
            Assert.True(runner.IsCurrentExecution);
            foreach (var received in records)
            {
                using (received)
                {
                    received.ApplicationJobAdmission?.ReleaseForHandlerStart();
                    if (Interlocked.Increment(ref count) == 1)
                    {
                        first = received;
                        firstStarted.TrySetResult();
                        await release.Task;
                        returned.TrySetResult();
                    }
                    else
                    {
                        secondStarted.TrySetResult();
                    }
                }
            }
        }, runner);
        pump.EnsureStarted();
        // Registrations are stable until cancellation: no per-batch runner task.
        var workers = runner.ActiveOnSupervisorLane.ToArray();
        try
        {
            using var payload = Message.From(new byte[4096]);
            Assert.Equal(SubmitResult.Ok, node.SendToNode(rid, [payload]));
            await firstStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.Equal(SubmitResult.Ok, node.SendToNode(rid, [payload]));
            await secondStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.Equal(workers.ToHashSet(), runner.ActiveOnSupervisorLane.ToHashSet());
            Assert.Equal(0UL, jobs.GetStatus().PermitsInUse);
            Assert.Equal(4096, first!.Parts[0].Size);

            var stopped = pump.DisposeAsync().AsTask();
            Assert.False(stopped.IsCompleted);
            release.TrySetResult();
            await stopped.WaitAsync(TimeSpan.FromSeconds(5));
            await returned.Task;
            Assert.Throws<ObjectDisposedException>(() => first.Parts[0].Size);
            await runner.StopAsync();
            Assert.Empty(failures.Errors);
        }
        finally
        {
            release.TrySetResult();
            await pump.DisposeAsync();
        }
    }

    [Fact]
    public async Task BlockedNodeHandler_DoesNotOccupyAnotherChannelOwner()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var source = new ZLinkManagedMeshNode(context, "workers");
        await using var target = new ZLinkManagedMeshNode(context, "workers");
        var suffix = Guid.NewGuid().ToString("N");
        var sourceRid = RoutingId.From("worker-source");
        var targetRid = RoutingId.From("worker-target");
        source.SetRoutingId(sourceRid);
        target.SetRoutingId(targetRid);
        source.SetBind($"inproc://worker-source-{suffix}");
        target.SetBind($"inproc://worker-target-{suffix}");
        target.AddChannel("independent");
        source.ConnectPeer($"inproc://worker-target-{suffix}", targetRid);
        var firstStarted = Signal();
        var secondStarted = Signal();
        using var release = new ManualResetEventSlim();
        await using var pump = new ZLinkMeshDispatchPump(target, new ZLinkMeshCompletionTable());
        pump.SetNodeRouteHandler((records, _) =>
        {
            foreach (var received in records)
            {
                using (received)
                {
                    if (received.ChannelName is null)
                    {
                        firstStarted.TrySetResult();
                        Assert.True(release.Wait(TimeSpan.FromSeconds(5)));
                    }
                    else
                        secondStarted.TrySetResult();
                }
            }
            return ValueTask.CompletedTask;
        });
        target.Start();
        source.Start();
        pump.EnsureStarted();
        try
        {
            var deadline = DateTime.UtcNow.AddSeconds(5);
            while (source.Status().AdmittedPeerCount != 1
                   || target.Status().AdmittedPeerCount != 1)
            {
                Assert.True(DateTime.UtcNow < deadline);
                await Task.Delay(1);
            }
            using var payload = Message.From(new byte[4096]);
            Assert.Equal(SubmitResult.Ok, source.SendToNode(targetRid, [payload]));
            await firstStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.Equal(SubmitResult.Ok,
                source.SendToChannel("independent", [payload], SendFlags.None, default));
            await secondStarted.Task.WaitAsync(TimeSpan.FromSeconds(5));
        }
        finally
        {
            release.Set();
        }
    }

    private static TaskCompletionSource Signal() =>
        new(TaskCreationOptions.RunContinuationsAsynchronously);

    private sealed class Failures : IZLinkRuntimeFailureReporter
    {
        internal ConcurrentQueue<Exception> Errors { get; } = new();
        public void ReportHandlerException(Exception exception) => Errors.Enqueue(exception);
        public void ReportUnhandledCallbackException(Exception exception) => Errors.Enqueue(exception);
        public void ReportRuntimeTaskException(string name, Exception exception) => Errors.Enqueue(exception);
    }
}
