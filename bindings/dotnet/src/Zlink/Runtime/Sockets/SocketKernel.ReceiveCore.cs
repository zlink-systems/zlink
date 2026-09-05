// SPDX-License-Identifier: MPL-2.0

using System.Buffers;
using System.Runtime.CompilerServices;
using System.Text;
using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink.Runtime.Sockets.Internal;

internal sealed partial class SocketKernel
{
    // HOT PATH: basic message receive intentionally discards source routing
    // metadata. Routed receive helpers own that information; do not restore a
    // routing-id allocation or copy to this path.
    private bool ReceiveBasicParts(int flags,
        out Message? singlePart,
        out MultipartMessageCollection? parts,
        bool allowNoData = false)
    {
        var receivedParts = Array.Empty<Message>();
        var receivedPartCount = 0;
        singlePart = null;
        parts = null;
        try
        {
            while (true)
            {
                // HOT PATH: zlink_msg_init initializes the full opaque value
                // before recv or cleanup observes it.
                Unsafe.SkipInit(out ZlinkMsg part);
                var initRc = NativeMethods.zlink_msg_init(ref part);
                if (initRc != 0)
                    throw ZlinkException.CreateRecvException(
                        NativeMethods.zlink_errno());
                var ownsNativePart = true;
                try
                {
                    int hasMore;
                    var rc = (flags & DontWaitFlag) != 0
                        ? NativeMethods.zlink_recv_part_nowait(Handle, out _,
                            ref part, out hasMore, flags)
                        : NativeMethods.zlink_recv_part(Handle, out _,
                            ref part, out hasMore, flags);
                    if (rc != 0)
                    {
                        var errno = NativeMethods.zlink_errno();
                        if (allowNoData && receivedPartCount == 0
                                        && ZlinkException.MapErrorCode(errno) is ErrorCode.EAgain
                                            or ErrorCode.EBusy)
                            return false;
                        throw ZlinkException.CreateRecvException(errno);
                    }

                    if (hasMore == 0 && receivedPartCount == 0)
                    {
                        // Pool-aware adoption: in routed echo workloads the
                        // Message wrapper lifetime is bounded by the caller's
                        // using-scope. Recycling these instances eliminates a
                        // per-message heap allocation and Gen 0 GC pressure.
                        singlePart = Message.AdoptNativeFromPool(ref part);
                        ownsNativePart = false;
                        return true;
                    }

                    AppendNativePart(ref receivedParts, ref receivedPartCount,
                        ref part);
                    ownsNativePart = false;
                    if (hasMore == 0)
                        break;
                }
                finally
                {
                    if (ownsNativePart)
                        NativeMethods.zlink_msg_close(ref part);
                }
            }

            parts = MultipartMessageCollection.FromMessages(receivedParts,
                receivedPartCount, rented: true);
            receivedParts = Array.Empty<Message>();
            return true;
        }
        catch
        {
            DisposeReceivedParts(receivedParts, receivedPartCount);
            singlePart?.Dispose();
            throw;
        }
        finally
        {
            // The result takes the pooled storage on success; only a failed
            // assembly returns its storage here.
            if (receivedParts.Length != 0)
                ArrayPool<Message>.Shared.Return(receivedParts, clearArray: true);
        }
    }

    private bool ReceiveRoutedParts(int flags,
        out RoutingIdSnapshot routingId,
        out ulong replyToken, out Message? singlePart,
        out MultipartMessageCollection? parts,
        bool allowNoData = false)
    {
        routingId = default;
        replyToken = 0;
        singlePart = null;
        parts = null;
        if (_policy.UsesRouterRoutedReceiveEnvelope)
            return ReceiveRouterParts(flags, out routingId, out replyToken,
                out singlePart, out parts, allowNoData);

        var receivedParts = Array.Empty<Message>();
        var receivedPartCount = 0;
        try
        {
            while (true)
            {
                ZlinkMsg part = default;
                var initRc = NativeMethods.zlink_msg_init(ref part);
                if (initRc != 0)
                    throw ZlinkException.CreateRecvException(
                        NativeMethods.zlink_errno());
                var ownsNativePart = true;
                try
                {
                    IntPtr sourceNodeRid;
                    int basicHasMore;
                    var rc = (flags & DontWaitFlag) != 0
                        ? NativeMethods.zlink_recv_part_nowait(Handle,
                            out sourceNodeRid, ref part,
                            out basicHasMore, flags)
                        : NativeMethods.zlink_recv_part(Handle,
                            out sourceNodeRid, ref part,
                            out basicHasMore, flags);
                    if (rc != 0)
                    {
                        var errno = NativeMethods.zlink_errno();
                        if (allowNoData && receivedPartCount == 0
                                        && ZlinkException.MapErrorCode(errno) is ErrorCode.EAgain
                                            or ErrorCode.EBusy)
                            return false;
                        throw ZlinkException.CreateRecvException(errno);
                    }

                    if (!routingId.HasValue)
                        routingId = RoutingIdSnapshot.FromPointer(sourceNodeRid);
                    if (basicHasMore == 0 && receivedPartCount == 0)
                    {
                        singlePart = Message.AdoptNativeFromPool(ref part);
                        ownsNativePart = false;
                        return true;
                    }

                    AppendNativePart(ref receivedParts, ref receivedPartCount,
                        ref part);
                    ownsNativePart = false;
                    if (basicHasMore == 0)
                        break;
                }
                finally
                {
                    if (ownsNativePart)
                        NativeMethods.zlink_msg_close(ref part);
                }
            }

            parts = MultipartMessageCollection.FromMessages(receivedParts,
                receivedPartCount, rented: true);
            receivedParts = Array.Empty<Message>();
            return true;
        }
        catch
        {
            DisposeReceivedParts(receivedParts, receivedPartCount);
            singlePart?.Dispose();
            throw;
        }
        finally
        {
            // The result takes the pooled storage on success; only a failed
            // assembly returns its storage here.
            if (receivedParts.Length != 0)
                ArrayPool<Message>.Shared.Return(receivedParts, clearArray: true);
        }
    }

    private bool ReceiveRouterParts(int flags,
        out RoutingIdSnapshot routingId,
        out ulong replyToken, out Message? singlePart,
        out MultipartMessageCollection? parts,
        bool allowNoData)
    {
        var receivedParts = Array.Empty<Message>();
        var receivedPartCount = 0;
        routingId = default;
        replyToken = 0;
        singlePart = null;
        parts = null;
        try
        {
            while (true)
            {
                Unsafe.SkipInit(out ZlinkMsg part);
                var initRc = NativeMethods.zlink_msg_init(ref part);
                if (initRc != 0)
                    throw ZlinkException.CreateRecvException(
                        NativeMethods.zlink_errno());
                var ownsNativePart = true;
                try
                {
                    IntPtr sourceNodeRid;
                    ulong receivedReplyToken;
                    int hasMore;
                    var rc = (flags & DontWaitFlag) != 0
                        ? NativeMethods.zlink_router_recv_part_nowait(Handle,
                            out sourceNodeRid, out receivedReplyToken,
                            ref part, out hasMore, flags)
                        : NativeMethods.zlink_router_recv_part(Handle,
                            out sourceNodeRid, out receivedReplyToken,
                            ref part, out hasMore, flags);
                    if (rc != 0)
                    {
                        var errno = NativeMethods.zlink_errno();
                        if (allowNoData && receivedPartCount == 0
                                        && ZlinkException.MapErrorCode(errno) is ErrorCode.EAgain
                                            or ErrorCode.EBusy)
                            return false;

                        throw ZlinkException.CreateRecvException(errno);
                    }

                    if (receivedPartCount == 0)
                    {
                        routingId = RoutingIdSnapshot.FromPointer(sourceNodeRid);
                        replyToken = receivedReplyToken;
                    }

                    if (hasMore == 0 && receivedPartCount == 0)
                    {
                        // Pool-aware adoption: in routed echo workloads the
                        // Message wrapper lifetime is bounded by the caller's
                        // using-scope. Recycling these instances eliminates a
                        // per-message heap allocation and Gen 0 GC pressure.
                        singlePart = Message.AdoptNativeFromPool(ref part);
                        ownsNativePart = false;
                        return true;
                    }

                    AppendNativePart(ref receivedParts, ref receivedPartCount,
                        ref part);
                    ownsNativePart = false;
                    if (hasMore == 0)
                        break;
                }
                finally
                {
                    if (ownsNativePart)
                        NativeMethods.zlink_msg_close(ref part);
                }
            }

            parts = MultipartMessageCollection.FromMessages(receivedParts,
                receivedPartCount, rented: true);
            receivedParts = Array.Empty<Message>();
            return true;
        }
        catch
        {
            DisposeReceivedParts(receivedParts, receivedPartCount);
            singlePart?.Dispose();
            throw;
        }
        finally
        {
            // The result takes the pooled storage on success; only a failed
            // assembly returns its storage here.
            if (receivedParts.Length != 0)
                ArrayPool<Message>.Shared.Return(receivedParts, clearArray: true);
        }
    }

    private unsafe bool ReceiveSubscribedParts(int flags,
        byte[] topicBuffer, Message reusableSinglePart,
        out RoutingIdSnapshot routingId, out int topicLength,
        out Message? singlePart, out MultipartMessageCollection? parts,
        bool allowNoData = false)
    {
        var receivedParts = Array.Empty<Message>();
        var receivedPartCount = 0;
        routingId = default;
        topicLength = 0;
        singlePart = null;
        parts = null;
        try
        {
            while (true)
            {
                var firstPart = receivedPartCount == 0;
                ZlinkMsg part = default;
                var ownsNativePart = false;
                if (!firstPart)
                {
                    var initRc = NativeMethods.zlink_msg_init(ref part);
                    if (initRc != 0)
                        throw ZlinkException.CreateRecvException(
                            NativeMethods.zlink_errno());
                    ownsNativePart = true;
                }
                try
                {
                    IntPtr sourceRoutingId;
                    nuint nativeTopicLength;
                    int hasMore;
                    int rc;
                    if (allowNoData)
                    {
                        // HOT PATH: ready socket drains use DONT_WAIT, so this
                        // native call cannot wait for transport I/O or reenter
                        // managed code. Avoid a GC transition for each frame.
                        fixed (byte* topicId = topicBuffer)
                        {
                            rc = firstPart
                                ? NativeMethods.zlink_subscribe_part_dont_wait(
                                    Handle, out sourceRoutingId, topicId,
                                    (nuint)topicBuffer.Length,
                                    out nativeTopicLength,
                                    ref reusableSinglePart.Handle, out hasMore,
                                    flags)
                                : NativeMethods.zlink_subscribe_part_dont_wait(
                                    Handle, out sourceRoutingId, topicId,
                                    (nuint)topicBuffer.Length,
                                    out nativeTopicLength, ref part,
                                    out hasMore, flags);
                        }
                    }
                    else
                    {
                        rc = firstPart
                            ? NativeMethods.zlink_subscribe_part(Handle,
                                out sourceRoutingId, topicBuffer,
                                (nuint)topicBuffer.Length,
                                out nativeTopicLength,
                                ref reusableSinglePart.Handle, out hasMore,
                                flags)
                            : NativeMethods.zlink_subscribe_part(Handle,
                                out sourceRoutingId, topicBuffer,
                                (nuint)topicBuffer.Length,
                                out nativeTopicLength, ref part,
                                out hasMore, flags);
                    }
                    if (rc != 0)
                    {
                        if (firstPart)
                            reusableSinglePart.CloseAfterFailedNativeReceive();
                        var errno = NativeMethods.zlink_errno();
                        if (allowNoData && receivedPartCount == 0
                                        && ZlinkException.MapErrorCode(errno) is ErrorCode.EAgain
                                            or ErrorCode.EBusy)
                            return false;
                        throw ZlinkException.CreateRecvException(errno);
                    }

                    if (firstPart)
                    {
                        routingId = RoutingIdSnapshot.FromPointer(
                            sourceRoutingId);
                        topicLength = checked((int)nativeTopicLength);
                    }

                    if (hasMore == 0 && firstPart)
                    {
                        singlePart = reusableSinglePart;
                        return true;
                    }

                    if (firstPart)
                        AppendNativePart(ref receivedParts, ref receivedPartCount,
                            ref reusableSinglePart.Handle);
                    else
                    {
                        AppendNativePart(ref receivedParts, ref receivedPartCount,
                            ref part);
                        ownsNativePart = false;
                    }
                    if (hasMore == 0)
                        break;
                }
                finally
                {
                    if (ownsNativePart)
                        NativeMethods.zlink_msg_close(ref part);
                }
            }

            parts = MultipartMessageCollection.FromMessages(receivedParts,
                receivedPartCount, rented: true);
            receivedParts = Array.Empty<Message>();
            return true;
        }
        catch
        {
            DisposeReceivedParts(receivedParts, receivedPartCount);
            singlePart?.Dispose();
            throw;
        }
        finally
        {
            // The result takes the pooled storage on success; only a failed
            // assembly returns its storage here.
            if (receivedParts.Length != 0)
                ArrayPool<Message>.Shared.Return(receivedParts, clearArray: true);
        }
    }

    private static void AppendNativePart(ref Message[] receivedParts,
        ref int count, ref ZlinkMsg source)
    {
        if (count == receivedParts.Length)
        {
            var expanded = ArrayPool<Message>.Shared.Rent(
                count == 0 ? 4 : checked(count * 2));
            receivedParts.AsSpan(0, count).CopyTo(expanded);
            if (receivedParts.Length != 0)
                ArrayPool<Message>.Shared.Return(receivedParts, clearArray: true);
            receivedParts = expanded;
        }

        // Move once, directly into the public part's native header. There is
        // no intermediate native vector to initialize, move, and close again.
        receivedParts[count] = Message.MoveFromNative(ref source);
        count++;
    }

    private static void DisposeReceivedParts(Message[] parts, int count)
    {
        for (var i = 0; i < count; i++)
            parts[i].Dispose();
    }

    private static unsafe byte[]? CopyRoutingIdBytes(IntPtr routingIdPtr)
    {
        if (routingIdPtr == IntPtr.Zero)
            return null;

        return NativeHelpers.ReadRoutingId(ref *(ZlinkRoutingId*)routingIdPtr);
    }

    private static string DecodeTopic(byte[] topicBuffer, nuint topicLength)
    {
        var boundedLength = topicLength > (nuint)topicBuffer.Length
            ? topicBuffer.Length
            : (int)topicLength;
        return boundedLength == 0
            ? string.Empty
            : Encoding.UTF8.GetString(topicBuffer, 0, boundedLength);
    }
}
