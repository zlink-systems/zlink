using System.Reflection;
using Zlink.Framework.Runtime.Dispatch;
using Zlink.Framework.Runtime.Service;

namespace Zlink.Framework.UnitTests;

public sealed class MeshPermitHandoffTests
{
    [Fact]
    public async Task RawIngress_ReservedPermitConsumptionAllowsSuccessorWaiter()
    {
        await using var context = Systems.Zlink.Zlink.CreateContext();
        await using var socket = context.CreateRouterSocket();
        socket.Bind($"inproc://permit-handoff-{Guid.NewGuid():N}");
        using var queue = new ZLinkApplicationJobQueue(new(
            ZLinkApplicationJobQueueProfile.Balanced, 1, 1, 1));
        await using var node = new ZLinkManagedMeshNode(
            context, "permit-handoff", applicationJobQueue: queue);
        using var stop = new CancellationTokenSource();
        SetField(node, "_socket", socket);

        // Arrange the producer's published state. Its reservation can be
        // consumed before the producer returns; only that consumer owns the
        // successful handoff flag, including registration of a successor.
        var reservation = await queue.AcquireAsync(stop.Token);
        SetField(node, "_reservedRawApplicationAdmission", reservation);
        SetField(node, "_rawApplicationAdmissionWaitActive", 1);
        ZLinkApplicationJobQueueLease? held = null;
        try
        {
            Drain(node, stop.Token);
            Assert.Equal(0UL, queue.GetStatus().PermitsInUse);
            held = await queue.AcquireAsync(stop.Token);

            Drain(node, stop.Token);
            Assert.Equal(1UL, queue.GetStatus().CapacityWaiters);
        }
        finally
        {
            stop.Cancel();
            held?.Dispose();
            SetField(node, "_socket", null);
        }
    }

    private static void Drain(ZLinkManagedMeshNode node, CancellationToken stop) =>
        node.GetType().GetMethod("DrainRawSocket",
                BindingFlags.Instance | BindingFlags.NonPublic)!
            .Invoke(node, [stop, new ZLinkApplicationJobQueueLease?[64]]);

    private static void SetField(object instance, string name, object? value) =>
        instance.GetType().GetField(name, BindingFlags.Instance | BindingFlags.NonPublic)!
            .SetValue(instance, value);
}
