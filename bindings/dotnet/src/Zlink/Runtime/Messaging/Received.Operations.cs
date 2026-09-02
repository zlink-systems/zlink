// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

// Continuation of Received; the type summary lives on the primary partial in
// Received.cs.
public sealed partial class Received : IDisposable
{
    internal ReceivedSendContext CaptureSendContext() =>
        new(_sendKernel, _sendRoutingIdSnapshot);

    internal ReceivedReplyContext CaptureReplyContext()
    {
        if (_replyToken is null || _sendKernel is null)
            throw new ZlinkSubmitException(SubmitResult.InvalidArgument,
                (int)ErrorCode.EInval);
        var target = _sendRoutingIdSnapshot.ToRoutingId();
        if (!target.HasValue)
            throw new ZlinkSubmitException(SubmitResult.InvalidArgument,
                (int)ErrorCode.EInval);
        return new ReceivedReplyContext(_sendKernel, target.Value, _replyToken);
    }

}

internal sealed class ReceivedReplyContext(
    Runtime.Sockets.Internal.SocketKernel kernel,
    RoutingId target,
    ReplyToken replyToken)
{
    internal void Reply(IReadOnlyList<Message> parts)
    {
        kernel.SendReplyCore(target, replyToken, parts);
    }
}

internal sealed class ReceivedSendContext(
    Runtime.Sockets.Internal.SocketKernel? sendKernel,
    RoutingIdSnapshot routingId)
{
    internal Task SendAsyncCore(
        OperationMessageBuffer parts,
        CancellationToken cancellationToken)
    {
        if (sendKernel == null)
            throw new ZlinkSubmitException(SubmitResult.InvalidArgument,
                (int)ErrorCode.EInval);
        var target = routingId.ToRoutingId();
        if (!target.HasValue)
            throw new ZlinkSubmitException(SubmitResult.InvalidArgument,
                (int)ErrorCode.EInval);
        return parts.IsSingle
            ? sendKernel.Completion.SendAsync(target.Value,
                new SingleMessageReadOnlyList(parts.Single),
                cancellationToken)
            : sendKernel.Completion.SendAsync(target.Value, parts.Parts,
                cancellationToken);
    }

    internal void SendCore(IReadOnlyList<Message> parts)
    {
        ArgumentNullException.ThrowIfNull(parts);
        if (sendKernel == null)
            throw new ZlinkSubmitException(SubmitResult.InvalidArgument,
                (int)ErrorCode.EInval);
        var target = routingId.ToRoutingId();
        if (!target.HasValue)
            throw new ZlinkSubmitException(SubmitResult.InvalidArgument,
                (int)ErrorCode.EInval);
        sendKernel.Completion.Send(target.Value, parts);
    }
}
