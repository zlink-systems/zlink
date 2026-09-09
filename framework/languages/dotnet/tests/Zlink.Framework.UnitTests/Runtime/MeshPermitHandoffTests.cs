using System.Reflection;
using Zlink.Framework.Runtime.Dispatch;
using Zlink.Framework.Runtime.Service;

namespace Zlink.Framework.UnitTests;

public sealed class MeshPermitHandoffTests
{
    [Fact]
    public async Task RawIngress_CanWaitForNextPermitWhilePreviousProducerIsStillWaking()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var socket = context.CreateRouterSocket();
        socket.Bind($"inproc://permit-handoff-{Guid.NewGuid():N}");
        using var queue = new ZLinkApplicationJobQueue(new(
            ZLinkApplicationJobQueueProfile.Balanced, 1, 1, 1));
        await using var node = new ZLinkManagedMeshNode(
            context, "permit-handoff", applicationJobQueue: queue);
        using var stop = new CancellationTokenSource();
        using var wake = new BlockingFirstWakeTimer();
        SetField(node, "_socket", socket);
        SetField(node, "_ingressWake", wake);

        ZLinkApplicationJobQueueLease? held = await queue.AcquireAsync(stop.Token);
        // Arrange the existing waiter-registration boundary without starting
        // the node's receive thread; the actual producer and consumer run below.
        SetField(node, "_rawApplicationAdmissionWaitActive", 1);
        var producer = (Task)Invoke(node, "WaitForRawApplicationAdmissionAsync",
            queue, stop.Token)!;
        try
        {
            Assert.Equal(1UL, queue.GetStatus().CapacityWaiters);
            held.Dispose();
            held = null;
            await wake.Entered.Task.WaitAsync(TimeSpan.FromSeconds(5));

            // The producer has published its reservation and is still inside
            // WakeIngress. Consume it through the real receive path. An empty
            // ROUTER returns NO_DATA, returning that reservation to the queue.
            Drain(node, stop.Token);
            Assert.Equal(0UL, queue.GetStatus().PermitsInUse);
            held = await queue.AcquireAsync(stop.Token);

            // Another receive turn now needs a permit. The previous producer
            // must not prevent this successor from registering its own wait.
            Drain(node, stop.Token);
            Assert.Equal(1UL, queue.GetStatus().CapacityWaiters);

            wake.Release();
            await producer.WaitAsync(TimeSpan.FromSeconds(5));
            held.Dispose();
            held = null;
            await wake.NextWake.Task.WaitAsync(TimeSpan.FromSeconds(5));
            Drain(node, stop.Token);
            Assert.Equal(0UL, queue.GetStatus().PermitsInUse);
            Assert.Equal(0UL, queue.GetStatus().CapacityWaiters);
        }
        finally
        {
            stop.Cancel();
            wake.Release();
            await producer.WaitAsync(TimeSpan.FromSeconds(5));
            held?.Dispose();
            // The test owns these public binding objects; node shutdown still
            // owns any outstanding admission reservation.
            SetField(node, "_socket", null);
            SetField(node, "_ingressWake", null);
        }
    }

    private static void Drain(ZLinkManagedMeshNode node, CancellationToken stop) =>
        Invoke(node, "DrainRawSocket", stop,
            new ZLinkApplicationJobQueueLease?[64]);

    private static object? Invoke(object instance, string name, params object?[] arguments) =>
        instance.GetType().GetMethod(name, BindingFlags.Instance | BindingFlags.NonPublic)!
            .Invoke(instance, arguments);

    private static void SetField(object instance, string name, object? value) =>
        instance.GetType().GetField(name, BindingFlags.Instance | BindingFlags.NonPublic)!
            .SetValue(instance, value);

    private sealed class BlockingFirstWakeTimer : IZlinkTimer
    {
        private readonly ManualResetEventSlim _release = new();
        private int _starts;

        internal TaskCompletionSource Entered { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);
        internal TaskCompletionSource NextWake { get; } =
            new(TaskCreationOptions.RunContinuationsAsynchronously);

        public void Start(TimeSpan interval, ulong repeatCount)
        {
            if (Interlocked.Increment(ref _starts) != 1)
            {
                NextWake.TrySetResult();
                return;
            }
            Entered.TrySetResult();
            if (!_release.Wait(TimeSpan.FromSeconds(5)))
                throw new TimeoutException("The permit handoff test did not release its wake barrier.");
        }

        internal void Release() => _release.Set();
        public void Stop() { }
        public ulong? Recv(RecvFlags flags = RecvFlags.None) => null;
        public void Close() => Dispose();
        public void Dispose() => _release.Dispose();
        public ValueTask DisposeAsync()
        {
            Dispose();
            return ValueTask.CompletedTask;
        }
    }
}
