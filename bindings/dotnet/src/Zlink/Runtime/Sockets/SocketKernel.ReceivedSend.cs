// SPDX-License-Identifier: MPL-2.0

using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink.Runtime.Sockets.Internal;

internal sealed partial class SocketKernel : IDisposable
{
    internal bool SendReceivedSingle(
        RoutingIdSnapshot routingId,
        ulong transportPairId,
        ulong transportPairGeneration,
        Message part,
        SendFlags flags)
    {
        if (part == null)
            throw new ArgumentNullException(nameof(part));
        if (!routingId.HasValue)
            throw new ZlinkSubmitException(SubmitResult.InvalidArgument,
                (int)ErrorCode.EInval);

        ZlinkRoutingId nativeRoutingId = default;
        routingId.WriteNative(ref nativeRoutingId);
        if (transportPairId != 0 && transportPairGeneration != 0)
            return SendReceivedExact(
                ref nativeRoutingId,
                transportPairId,
                transportPairGeneration,
                new SingleMessageReadOnlyList(part),
                flags);
        if ((flags & SendFlags.DontWait) != 0)
            return SendSingleResultCore(ref nativeRoutingId, part,
                (int)flags) == SendResult.Sent;

        SendSingleCore(ref nativeRoutingId, part, (int)flags);
        return true;
    }

    internal bool SendReceivedParts(
        RoutingIdSnapshot routingId,
        ulong transportPairId,
        ulong transportPairGeneration,
        IReadOnlyList<Message> parts,
        SendFlags flags)
    {
        if (parts == null)
            throw new ArgumentNullException(nameof(parts));

        var target = routingId.ToRoutingId();
        if (!target.HasValue)
            throw new ZlinkSubmitException(SubmitResult.InvalidArgument,
                (int)ErrorCode.EInval);
        if (transportPairId != 0 && transportPairGeneration != 0)
        {
            var nativeRoutingId = target.Value.ToNative();
            return SendReceivedExact(
                ref nativeRoutingId,
                transportPairId,
                transportPairGeneration,
                parts,
                flags);
        }

        if ((flags & SendFlags.DontWait) != 0)
            return SendNoWaitResult(target.Value, parts) == SendResult.Sent;

        Send(target.Value, parts, flags);
        return true;
    }

    private bool SendReceivedExact(
        ref ZlinkRoutingId nativeRoutingId,
        ulong transportPairId,
        ulong transportPairGeneration,
        IReadOnlyList<Message> parts,
        SendFlags flags)
    {
        var exactRoutingId = nativeRoutingId;
        try
        {
            RequestReplySupport.SubmitOwnedParts(
                parts,
                (ref ZlinkMsg nativePart,
                    NativeMethods.ZlinkPartFlag partFlag) =>
                    NativeMethods.zlink_send_part_transport_pair(
                        Handle,
                        ref exactRoutingId,
                        transportPairId,
                        transportPairGeneration,
                        ref nativePart,
                        (int)flags,
                        partFlag));
            return true;
        }
        catch (ZlinkSubmitException submit)
            when ((flags & SendFlags.DontWait) != 0
                  && submit.Result
                  == ZlinkSubmitException.ErrorCode.Backpressured)
        {
            return false;
        }
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
