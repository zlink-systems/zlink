using System.Reflection;
using System.Text;

using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests;

public sealed class SpotMessageFollowSubmitTests
{
    [Fact]
    public async Task OneWayMessageFollow_AwaitsBindingOwnedAdmissionOnceWithExactFence()
    {
        var spot = DispatchProxy.Create<
            IMessageFollowBackendSpot,
            MessageFollowSpotProxy>();
        var proxy = (MessageFollowSpotProxy)(object)spot;
        var operationId = new MeshOperationId(41, 146);

        await using var transport = new ZLinkSpotOutboundTransport(
            spot,
            TimeSpan.FromSeconds(2),
            CancellationToken.None);
        var first = Message.From("payload-1");
        var second = Message.From("payload-2");
        var pending = transport.SendMessageFollowToSpotAsync(
            RoutingId.From("target-node"),
            "room-1",
            7,
            operationId,
            11,
            13,
            17,
            1,
            [first, second],
            CancellationToken.None).AsTask();

        Assert.False(pending.IsCompleted);
        Assert.Equal(1, proxy.InvocationCount);
        Assert.False(proxy.ReadyHandlerRegistered);
        Assert.Equal(operationId, proxy.OperationId);
        Assert.Equal((ulong)11, proxy.TargetNodeGeneration);
        Assert.Equal((ulong)13, proxy.AuthorityOwnerGeneration);
        Assert.Equal((ulong)17, proxy.OwnerLeaseGeneration);
        Assert.Equal((byte)1, proxy.MessageFollowHopCount);
        Assert.Equal(
            new[] { "payload-1", "payload-2" },
            proxy.Payloads);

        proxy.CompleteAdmission();

        var result = await pending.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal(ZLinkOneWaySubmitStatus.Submitted, result.Status);
        Assert.Equal(1, proxy.InvocationCount);
        Assert.Throws<ObjectDisposedException>(() => _ = first.Size);
        Assert.Throws<ObjectDisposedException>(() => _ = second.Size);
    }

    [Fact]
    public async Task OneWayMessageFollow_MapsBindingAdmissionTimeoutOnce()
    {
        var spot = DispatchProxy.Create<
            IMessageFollowBackendSpot,
            MessageFollowSpotProxy>();
        var proxy = (MessageFollowSpotProxy)(object)spot;
        var message = Message.From("timeout");

        await using var transport = new ZLinkSpotOutboundTransport(
            spot,
            TimeSpan.FromMilliseconds(25),
            CancellationToken.None);
        var result = await transport.SendMessageFollowToSpotAsync(
                RoutingId.From("target-node"),
                "room-1",
                7,
                new MeshOperationId(41, 146),
                11,
                13,
                17,
                1,
                [message],
                CancellationToken.None)
            .AsTask()
            .WaitAsync(TimeSpan.FromSeconds(5));

        Assert.Equal(ZLinkOneWaySubmitStatus.TimedOut, result.Status);
        Assert.Equal(1, proxy.InvocationCount);
        Assert.True(proxy.TerminalToken.IsCancellationRequested);
        Assert.Throws<ObjectDisposedException>(() => _ = message.Size);
    }

    [Fact]
    public async Task OneWayMessageFollow_MapsRuntimeStopButPropagatesCallerCancellation()
    {
        var shutdownSpot = DispatchProxy.Create<
            IMessageFollowBackendSpot,
            MessageFollowSpotProxy>();
        var shutdownProxy = (MessageFollowSpotProxy)(object)shutdownSpot;
        using var stop = new CancellationTokenSource();
        var shutdownMessage = Message.From("shutdown");
        await using (var transport = new ZLinkSpotOutboundTransport(
                         shutdownSpot,
                         TimeSpan.FromSeconds(5),
                         stop.Token))
        {
            var pending = transport.SendMessageFollowToSpotAsync(
                RoutingId.From("target-node"),
                "room-1",
                7,
                new MeshOperationId(41, 146),
                11,
                13,
                17,
                1,
                [shutdownMessage],
                CancellationToken.None).AsTask();
            stop.Cancel();
            var result = await pending.WaitAsync(TimeSpan.FromSeconds(5));
            Assert.Equal(ZLinkOneWaySubmitStatus.Shutdown, result.Status);
        }
        Assert.Equal(1, shutdownProxy.InvocationCount);
        Assert.Throws<ObjectDisposedException>(() => _ = shutdownMessage.Size);

        var cancelledSpot = DispatchProxy.Create<
            IMessageFollowBackendSpot,
            MessageFollowSpotProxy>();
        var cancelledProxy = (MessageFollowSpotProxy)(object)cancelledSpot;
        using var caller = new CancellationTokenSource();
        var cancelledMessage = Message.From("cancelled");
        await using var cancelledTransport = new ZLinkSpotOutboundTransport(
            cancelledSpot,
            TimeSpan.FromSeconds(5),
            CancellationToken.None);
        var cancelled = cancelledTransport.SendMessageFollowToSpotAsync(
            RoutingId.From("target-node"),
            "room-1",
            7,
            new MeshOperationId(41, 146),
            11,
            13,
            17,
            1,
            [cancelledMessage],
            caller.Token).AsTask();
        caller.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(
            () => cancelled.WaitAsync(TimeSpan.FromSeconds(5)));
        Assert.Equal(1, cancelledProxy.InvocationCount);
        Assert.Throws<ObjectDisposedException>(() => _ = cancelledMessage.Size);
    }

    private interface IMessageFollowBackendSpot :
        IZLinkBackendSpot,
        IZLinkBackendSpotMessageFollower;

    private class MessageFollowSpotProxy : DispatchProxy
    {
        private readonly TaskCompletionSource _admission = new(
            TaskCreationOptions.RunContinuationsAsynchronously);

        internal int InvocationCount { get; private set; }

        internal bool ReadyHandlerRegistered { get; private set; }

        internal MeshOperationId OperationId { get; private set; }

        internal ulong TargetNodeGeneration { get; private set; }

        internal ulong AuthorityOwnerGeneration { get; private set; }

        internal ulong OwnerLeaseGeneration { get; private set; }

        internal byte MessageFollowHopCount { get; private set; }

        internal string[] Payloads { get; private set; } = [];

        internal CancellationToken TerminalToken { get; private set; }

        internal void CompleteAdmission() => _admission.TrySetResult();

        protected override object? Invoke(MethodInfo? targetMethod, object?[]? args)
        {
            ArgumentNullException.ThrowIfNull(targetMethod);
            return targetMethod.Name switch
            {
                nameof(IZLinkBackendSpot.OnSendReady) => RegisterSendReady(),
                nameof(IZLinkBackendSpotMessageFollower.MessageFollowSendToSpotAsync) =>
                    SubmitMessageFollow(args),
                nameof(IAsyncDisposable.DisposeAsync) => ValueTask.CompletedTask,
                _ => throw new NotSupportedException(targetMethod.Name)
            };
        }

        private object? RegisterSendReady()
        {
            ReadyHandlerRegistered = true;
            return null;
        }

        private ValueTask SubmitMessageFollow(object?[]? args)
        {
            InvocationCount++;
            OperationId = (MeshOperationId)args![3]!;
            TargetNodeGeneration = (ulong)args[4]!;
            AuthorityOwnerGeneration = (ulong)args[5]!;
            OwnerLeaseGeneration = (ulong)args[6]!;
            MessageFollowHopCount = (byte)args[7]!;
            Payloads = ((IReadOnlyList<Message>)args[8]!)
                .Select(static part =>
                    Encoding.UTF8.GetString(part.AsReadOnlySpan()))
                .ToArray();
            TerminalToken = (CancellationToken)args[10]!;
            TerminalToken.Register(
                () => _admission.TrySetCanceled(TerminalToken));
            return new ValueTask(_admission.Task);
        }
    }
}
