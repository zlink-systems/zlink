// SPDX-License-Identifier: MPL-2.0

using System.Runtime.CompilerServices;
using System.Text;
using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink.Runtime.Sockets.Internal;

internal sealed partial class SocketKernel
{
    // HOT PATH: basic message receive intentionally discards source routing
    // metadata. Routed receive helpers own that information; do not restore a
    // routing-id allocation or copy to this path.
    private bool ReceiveBasicParts(int flags, out Message? singlePart,
        out MultipartMessageCollection? parts, bool allowNoData = false)
    {
        var nativeParts = Array.Empty<ZlinkMsg>();
        var nativePartCount = 0;
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
                var initialized = true;
                var rc = (flags & DontWaitFlag) != 0
                    ? NativeMethods.zlink_recv_part_nowait(Handle,
                        out _, ref part, out var hasMore,
                        flags)
                    : NativeMethods.zlink_recv_part(Handle,
                        out _, ref part, out hasMore, flags);
                if (rc != 0)
                {
                    if (initialized)
                        NativeMethods.zlink_msg_close(ref part);
                    var errno = NativeMethods.zlink_errno();
                    if (allowNoData && nativePartCount == 0
                                    && ZlinkException.MapErrorCode(errno) is ErrorCode.EAgain
                                        or ErrorCode.EBusy)
                        return false;
                    throw ZlinkException.CreateRecvException(errno);
                }

                initialized = false;
                if (hasMore == 0 && nativePartCount == 0)
                {
                    // Pool-aware adoption: in routed echo workloads the
                    // Message wrapper lifetime is bounded by the caller's
                    // using-scope. Recycling these instances eliminates a
                    // per-message heap allocation and Gen 0 GC pressure.
                    singlePart = Message.AdoptNativeFromPool(ref part);
                    return true;
                }

                AppendNativePart(ref nativeParts, ref nativePartCount, ref part);
                if (hasMore == 0)
                    break;
            }

            parts = MultipartMessageCollection.FromNativeParts(nativeParts,
                nativePartCount);
            return true;
        }
        catch
        {
            CloseNativeParts(nativeParts, nativePartCount);
            singlePart?.Dispose();
            throw;
        }
    }

    private bool ReceiveRoutedParts(int flags,
        out RoutingIdSnapshot routingId,
        out ulong requestSeq, out Message? singlePart,
        out MultipartMessageCollection? parts, bool allowNoData = false)
    {
        routingId = default;
        requestSeq = 0;
        singlePart = null;
        parts = null;
        if (_policy.UsesRouterRoutedReceiveEnvelope)
            return ReceiveRouterParts(flags, out routingId,
                out requestSeq, out singlePart,
                out parts, allowNoData);

        var nativeParts = Array.Empty<ZlinkMsg>();
        var nativePartCount = 0;
        try
        {
            while (true)
            {
                ZlinkMsg part = default;
                var initRc = NativeMethods.zlink_msg_init(ref part);
                if (initRc != 0)
                    throw ZlinkException.CreateRecvException(
                        NativeMethods.zlink_errno());
                var initialized = true;
                int rc;
                IntPtr sourceNodeRid;
                int basicHasMore;
                rc = (flags & DontWaitFlag) != 0
                    ? NativeMethods.zlink_recv_part_nowait(Handle,
                        out sourceNodeRid, ref part, out basicHasMore,
                        flags)
                    : NativeMethods.zlink_recv_part(Handle, out sourceNodeRid,
                        ref part, out basicHasMore, flags);
                if (rc != 0)
                {
                    if (initialized)
                        NativeMethods.zlink_msg_close(ref part);
                    var errno = NativeMethods.zlink_errno();
                    if (allowNoData && nativePartCount == 0
                                    && ZlinkException.MapErrorCode(errno) is ErrorCode.EAgain
                                        or ErrorCode.EBusy)
                        return false;
                    throw ZlinkException.CreateRecvException(errno);
                }

                initialized = false;
                if (!routingId.HasValue)
                    routingId = RoutingIdSnapshot.FromPointer(sourceNodeRid);
                if (basicHasMore == 0 && nativePartCount == 0)
                {
                    singlePart = Message.AdoptNativeFromPool(ref part);
                    return true;
                }

                AppendNativePart(ref nativeParts, ref nativePartCount, ref part);
                if (basicHasMore == 0)
                    break;
            }

            parts = MultipartMessageCollection.FromNativeParts(nativeParts,
                nativePartCount);
            return true;
        }
        catch
        {
            CloseNativeParts(nativeParts, nativePartCount);
            singlePart?.Dispose();
            throw;
        }
    }

    private bool ReceiveRouterParts(int flags,
        out RoutingIdSnapshot routingId,
        out ulong requestSeq, out Message? singlePart,
        out MultipartMessageCollection? parts, bool allowNoData)
    {
        var nativeParts = Array.Empty<ZlinkMsg>();
        var nativePartCount = 0;
        routingId = default;
        requestSeq = 0;
        singlePart = null;
        parts = null;
        try
        {
            while (true)
            {
                ZlinkMsg part = default;
                var initRc = NativeMethods.zlink_msg_init(ref part);
                if (initRc != 0)
                    throw ZlinkException.CreateRecvException(
                        NativeMethods.zlink_errno());
                var initialized = true;
                // DONT_WAIT-only variant: avoid blocking while still allowing
                // managed free callbacks during native message handling.
                IntPtr sourceNodeRid;
                ulong receivedRequestSeq;
                int hasMore;
                var rc = (flags & DontWaitFlag) != 0
                    ? NativeMethods.zlink_router_recv_part_nowait(Handle,
                        out sourceNodeRid, out receivedRequestSeq, ref part,
                        out hasMore, flags)
                    : NativeMethods.zlink_router_recv_part(Handle,
                        out sourceNodeRid, out receivedRequestSeq, ref part,
                        out hasMore, flags);
                if (rc != 0)
                {
                    if (initialized)
                        NativeMethods.zlink_msg_close(ref part);
                    var errno = NativeMethods.zlink_errno();
                    if (allowNoData && nativePartCount == 0
                                    && ZlinkException.MapErrorCode(errno) is ErrorCode.EAgain
                                        or ErrorCode.EBusy)
                        return false;

                    throw ZlinkException.CreateRecvException(errno);
                }

                initialized = false;
                if (nativePartCount == 0)
                {
                    routingId = RoutingIdSnapshot.FromPointer(sourceNodeRid);
                    requestSeq = receivedRequestSeq;
                }

                if (hasMore == 0 && nativePartCount == 0)
                {
                    // Pool-aware adoption: in routed echo workloads the
                    // Message wrapper lifetime is bounded by the caller's
                    // using-scope. Recycling these instances eliminates a
                    // per-message heap allocation and Gen 0 GC pressure.
                    singlePart = Message.AdoptNativeFromPool(ref part);
                    return true;
                }

                AppendNativePart(ref nativeParts, ref nativePartCount, ref part);
                if (hasMore == 0)
                    break;
            }

            parts = MultipartMessageCollection.FromNativeParts(nativeParts,
                nativePartCount);
            return true;
        }
        catch
        {
            CloseNativeParts(nativeParts, nativePartCount);
            singlePart?.Dispose();
            throw;
        }
    }

    private bool ReceiveSubscribedParts(int flags,
        byte[] topicBuffer, out RoutingIdSnapshot routingId, out int topicLength,
        out Message? singlePart, out MultipartMessageCollection? parts,
        bool allowNoData = false)
    {
        var nativeParts = Array.Empty<ZlinkMsg>();
        var nativePartCount = 0;
        routingId = default;
        topicLength = 0;
        singlePart = null;
        parts = null;
        try
        {
            while (true)
            {
                ZlinkMsg part = default;
                var initRc = NativeMethods.zlink_msg_init(ref part);
                if (initRc != 0)
                    throw ZlinkException.CreateRecvException(
                        NativeMethods.zlink_errno());
                var initialized = true;
                var rc = NativeMethods.zlink_subscribe_part(Handle,
                    out var sourceRoutingId, topicBuffer,
                    (nuint)topicBuffer.Length, out var nativeTopicLength, ref part,
                    out var hasMore, flags);
                if (rc != 0)
                {
                    if (initialized)
                        NativeMethods.zlink_msg_close(ref part);
                    var errno = NativeMethods.zlink_errno();
                    if (allowNoData && nativePartCount == 0
                                    && ZlinkException.MapErrorCode(errno) is ErrorCode.EAgain
                                        or ErrorCode.EBusy)
                        return false;
                    throw ZlinkException.CreateRecvException(errno);
                }

                initialized = false;
                if (nativePartCount == 0)
                {
                    routingId = RoutingIdSnapshot.FromPointer(sourceRoutingId);
                    topicLength = checked((int)nativeTopicLength);
                }

                if (hasMore == 0 && nativePartCount == 0)
                {
                    singlePart = Message.AdoptNativeFromPool(ref part);
                    return true;
                }

                AppendNativePart(ref nativeParts, ref nativePartCount, ref part);
                if (hasMore == 0)
                    break;
            }

            parts = MultipartMessageCollection.FromNativeParts(nativeParts,
                nativePartCount);
            return true;
        }
        catch
        {
            CloseNativeParts(nativeParts, nativePartCount);
            singlePart?.Dispose();
            throw;
        }
    }

    private static ZlinkMsg MoveStoredPart(ref ZlinkMsg source)
    {
        ZlinkMsg stored = default;
        var initRc = NativeMethods.zlink_msg_init(ref stored);
        if (initRc != 0)
            throw ZlinkException.CreateRecvException(NativeMethods.zlink_errno());
        try
        {
            var rc = NativeMethods.zlink_msg_move(ref stored, ref source);
            if (rc != 0)
                throw ZlinkException.CreateConfigException(NativeMethods.zlink_errno());
            return stored;
        }
        catch
        {
            NativeMethods.zlink_msg_close(ref stored);
            throw;
        }
    }

    private static void AppendNativePart(ref ZlinkMsg[] nativeParts,
        ref int count, ref ZlinkMsg source)
    {
        if (count == nativeParts.Length) Array.Resize(ref nativeParts, count == 0 ? 4 : count * 2);

        nativeParts[count++] = MoveStoredPart(ref source);
    }

    private static void CloseNativeParts(ZlinkMsg[] parts, int count)
    {
        for (var i = 0; i < count; i++)
            NativeMethods.zlink_msg_close(ref parts[i]);
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
