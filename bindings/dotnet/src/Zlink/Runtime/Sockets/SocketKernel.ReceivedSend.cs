// SPDX-License-Identifier: MPL-2.0

using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink.Runtime.Sockets.Internal;

internal sealed partial class SocketKernel : IDisposable
{
    internal bool SendReceivedSingle(RoutingIdSnapshot routingId,
        Message part, SendFlags flags)
    {
        if (part == null)
            throw new ArgumentNullException(nameof(part));
        if (!routingId.HasValue)
            throw new ZlinkSubmitException(SubmitResult.InvalidArgument,
                (int)ErrorCode.EInval);

        ZlinkRoutingId nativeRoutingId = default;
        routingId.WriteNative(ref nativeRoutingId);
        if ((flags & SendFlags.DontWait) != 0)
            return SendSingleResultCore(ref nativeRoutingId, part,
                (int)flags) == SendResult.Sent;

        SendSingleCore(ref nativeRoutingId, part, (int)flags);
        return true;
    }

    internal bool SendReceivedParts(RoutingIdSnapshot routingId,
        IReadOnlyList<Message> parts, SendFlags flags)
    {
        if (parts == null)
            throw new ArgumentNullException(nameof(parts));

        var target = routingId.ToRoutingId();
        if (!target.HasValue)
            throw new ZlinkSubmitException(SubmitResult.InvalidArgument,
                (int)ErrorCode.EInval);

        if ((flags & SendFlags.DontWait) != 0)
            return SendNoWaitResult(target.Value, parts) == SendResult.Sent;

        Send(target.Value, parts, flags);
        return true;
    }

    private ReceivedSendHandler? CreateRoutedSendHandler(
        RoutingIdSnapshot routingId)
    {
        var target = routingId.ToRoutingId();
        if (target == null)
            return null;

        return (sendParts, sendFlags) =>
        {
            if ((sendFlags & SendFlags.DontWait) != 0)
                return SendNoWaitResult(target.Value, sendParts) == SendResult.Sent;
            Send(target.Value, sendParts, sendFlags);
            return true;
        };
    }

    private ReceivedSendSingleHandler? CreateRoutedSendSingleHandler(
        RoutingIdSnapshot routingId)
    {
        if (!routingId.HasValue)
            return null;

        var target = routingId.ToRoutingId();
        if (!target.HasValue)
            return null;
        var targetRid = target.Value;
        return (sendPart, sendFlags) =>
        {
            if ((sendFlags & SendFlags.DontWait) != 0)
                return SendRoutedMessageResultUnchecked(targetRid, sendPart,
                           (int)sendFlags)
                       == SendResult.Sent;
            SendRoutedMessageUnchecked(targetRid, sendPart, sendFlags);
            return true;
        };
    }
}
