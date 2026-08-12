// SPDX-License-Identifier: MPL-2.0

using System.Buffers;
using System.Runtime.CompilerServices;
using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink.Runtime.Sockets.Internal;

internal sealed partial class SocketKernel : IDisposable
{
    private bool SubscribeInto(TopicMessage result, int flags)
    {
        var topicBuffer = result.GetWritableTopicBuffer(TopicBufferSize);
        var allowNoData = (flags & DontWaitFlag) != 0;
        var candidate = result.PrepareReusableSinglePart();
        var received = ReceiveSubscribedParts(flags, topicBuffer, candidate,
            out var routingId, out var topicLength,
            out var singlePart, out var parts,
            allowNoData);
        if (!received)
            return false;
        if (singlePart != null)
        {
            result.PopulateSinglePartFromWritableTopicBuffer(routingId,
                topicLength, singlePart);
            return true;
        }

        if (parts == null || parts.Count == 0)
            throw ZlinkException.CreateRecvException((int)ErrorCode.EAgain);
        result.PopulateFromWritableTopicBuffer(routingId, topicLength, parts);
        return true;
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    internal bool SubscribeIntoSubscriber(TopicMessage result, int flags)
    {
        if (result == null)
            throw new ArgumentNullException(nameof(result));
        try
        {
            return SubscribeInto(result, flags);
        }
        catch (ZlinkException ex) when ((flags & DontWaitFlag) != 0
                                        && ZlinkException.MapErrorCode(ex.NativeErrno) is ErrorCode.EAgain
                                            or ErrorCode.EBusy)
        {
            return false;
        }
    }

    private bool ReceiveSubscriptionEventInto(SubscriptionEvent result,
        int flags)
    {
        var topicBuffer = ArrayPool<byte>.Shared.Rent(TopicBufferSize);
        try
        {
            var rc = NativeMethods.zlink_xpub_recv_part(Handle,
                out var sourceRoutingId, out var subscribedInt, topicBuffer,
                (nuint)topicBuffer.Length, out var topicLength, flags);
            if (rc != 0)
            {
                var errno = NativeMethods.zlink_errno();
                if ((flags & DontWaitFlag) != 0
                    && ZlinkException.MapErrorCode(errno) == ErrorCode.EAgain)
                    return false;
                throw ZlinkException.CreateRecvException(errno);
            }

            var routingIdBytes = CopyRoutingIdBytes(sourceRoutingId);
            var routingId = routingIdBytes == null
                ? null
                : RoutingId.FromOwnedOptionalBytes(routingIdBytes);
            var topic = DecodeTopic(topicBuffer, topicLength);
            result.Populate(routingId, topic, subscribedInt != 0);
            return true;
        }
        finally
        {
            ArrayPool<byte>.Shared.Return(topicBuffer);
        }
    }
}
