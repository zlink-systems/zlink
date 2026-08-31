// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

// Continuation of Received; the type summary lives on the primary partial in
// Received.cs.
public sealed partial class Received : IDisposable
{
    internal ReceivedSendContext CaptureSendContext() =>
        new(
            _sendKernel,
            _sendRoutingIdSnapshot,
            _sendHandler,
            _sendSingleHandler,
            IsSinglePart ? _transportPairId : 0,
            IsSinglePart ? _transportPairGeneration : 0);

    internal ReceivedReplyHandler CaptureReplyHandler()
    {
        if (_metadata is not
            {
                RequestSeq: not null,
                ReplyHandler: { } replyHandler
            })
            throw new ZlinkSubmitException(SubmitResult.InvalidArgument,
                (int)ErrorCode.EInval);
        return replyHandler;
    }

}

internal sealed class ReceivedSendContext(
    Runtime.Sockets.Internal.SocketKernel? sendKernel,
    RoutingIdSnapshot routingId,
    ReceivedSendHandler? sendHandler,
    ReceivedSendSingleHandler? sendSingleHandler,
    ulong transportPairId,
    ulong transportPairGeneration)
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
            ? sendKernel.SendCompletion.SendSingleAsync(target.Value,
                parts.Single, cancellationToken, transportPairId,
                transportPairGeneration)
            : sendKernel.SendCompletion.SendAsync(target.Value, parts.Parts,
                cancellationToken, transportPairId,
                transportPairGeneration);
    }

    internal bool SendCore(Message part, SendFlags flags = SendFlags.None)
    {
        ArgumentNullException.ThrowIfNull(part);
        if (sendKernel != null)
            return sendKernel.SendReceivedSingle(
                routingId,
                transportPairId,
                transportPairGeneration,
                part,
                flags);
        if (sendSingleHandler != null)
            return sendSingleHandler(part, flags);
        return SendCore(new SingleMessageReadOnlyList(part), flags);
    }

    internal bool SendCore(
        IReadOnlyList<Message> parts,
        SendFlags flags = SendFlags.None)
    {
        ArgumentNullException.ThrowIfNull(parts);
        if (parts.Count == 1)
            return SendCore(parts[0], flags);
        if (sendKernel != null)
            return sendKernel.SendReceivedParts(
                routingId,
                transportPairId,
                transportPairGeneration,
                parts,
                flags);
        if (sendHandler == null)
            throw new ZlinkSubmitException(SubmitResult.InvalidArgument,
                (int)ErrorCode.EInval);
        return sendHandler(parts, flags);
    }
}
