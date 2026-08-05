using System.Reflection;

using Zlink.Framework.Runtime.Backend.Contracts;
using Zlink.Framework.Runtime.Spots;

namespace Zlink.Framework.UnitTests;

public sealed class SpotMessageFollowSubmitTests
{
    [Fact]
    public async Task OneWayMessageFollow_RetriesBackpressureWithoutChangingOperationIdentity()
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
        var pending = transport.SendMessageFollowToSpotAsync(
            RoutingId.From("target-node"),
            "room-1",
            7,
            operationId,
            11,
            13,
            17,
            1,
            [Message.From("payload")],
            CancellationToken.None).AsTask();

        Assert.False(pending.IsCompleted);
        Assert.Equal(1, proxy.AttemptCount);

        proxy.SignalSendReady();

        var result = await pending.WaitAsync(TimeSpan.FromSeconds(5));
        Assert.Equal(ZLinkOneWaySubmitStatus.Submitted, result.Status);
        Assert.Equal(2, proxy.AttemptCount);
        Assert.Equal([operationId, operationId], proxy.OperationIds);
        Assert.Equal([SendFlags.DontWait, SendFlags.DontWait], proxy.Flags);
        Assert.Equal(1, proxy.AcceptedCount);
    }

    private interface IMessageFollowBackendSpot :
        IZLinkBackendSpot,
        IZLinkBackendSpotMessageFollower;

    private class MessageFollowSpotProxy : DispatchProxy
    {
        private Action? _sendReady;

        internal int AttemptCount { get; private set; }

        internal int AcceptedCount { get; private set; }

        internal List<MeshOperationId> OperationIds { get; } = [];

        internal List<SendFlags> Flags { get; } = [];

        internal void SignalSendReady() => _sendReady?.Invoke();

        protected override object? Invoke(MethodInfo? targetMethod, object?[]? args)
        {
            ArgumentNullException.ThrowIfNull(targetMethod);
            return targetMethod.Name switch
            {
                nameof(IZLinkBackendSpot.OnSendReady) => RegisterSendReady(args),
                nameof(IZLinkBackendSpotMessageFollower.MessageFollowSendToSpot) =>
                    SubmitMessageFollow(args),
                nameof(IAsyncDisposable.DisposeAsync) => ValueTask.CompletedTask,
                _ => throw new NotSupportedException(targetMethod.Name)
            };
        }

        private object? RegisterSendReady(object?[]? args)
        {
            _sendReady = (Action)args![0]!;
            return null;
        }

        private SubmitResult SubmitMessageFollow(object?[]? args)
        {
            AttemptCount++;
            OperationIds.Add((MeshOperationId)args![3]!);
            Flags.Add((SendFlags)args[9]!);
            if (AttemptCount == 1)
                return SubmitResult.Backpressured;

            AcceptedCount++;
            return SubmitResult.Ok;
        }
    }
}
