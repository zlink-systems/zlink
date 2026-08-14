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
    private bool ReceiveBasicParts(int flags, bool retainCredit,
        out Message? singlePart,
        out MultipartMessageCollection? parts,
        out HwmBudgetLeaseOwner? hwmBudgetLeases,
        bool allowNoData = false)
    {
        var nativeParts = Array.Empty<ZlinkMsg>();
        var nativePartCount = 0;
        singlePart = null;
        parts = null;
        hwmBudgetLeases = null;
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
                    var lease = IntPtr.Zero;
                    int hasMore;
                    var rc = retainCredit
                        ? NativeMethods.zlink_recv_part_with_hwm_budget_lease(
                            Handle, out _, ref part, out lease, out hasMore,
                            flags)
                        : (flags & DontWaitFlag) != 0
                            ? NativeMethods.zlink_recv_part_nowait(Handle,
                                out _, ref part, out hasMore, flags)
                            : NativeMethods.zlink_recv_part(Handle, out _,
                                ref part, out hasMore, flags);
                    if (rc != 0)
                    {
                        HwmBudgetLeaseOwner.ReleaseUnowned(ref lease);
                        var errno = NativeMethods.zlink_errno();
                        if (allowNoData && nativePartCount == 0
                                        && ZlinkException.MapErrorCode(errno) is ErrorCode.EAgain
                                            or ErrorCode.EBusy)
                            return false;
                        throw ZlinkException.CreateRecvException(errno);
                    }

                    if (retainCredit)
                        HwmBudgetLeaseOwner.Adopt(ref hwmBudgetLeases,
                            ref lease);
                    if (hasMore == 0 && nativePartCount == 0)
                    {
                        // Pool-aware adoption: in routed echo workloads the
                        // Message wrapper lifetime is bounded by the caller's
                        // using-scope. Recycling these instances eliminates a
                        // per-message heap allocation and Gen 0 GC pressure.
                        singlePart = Message.AdoptNativeFromPool(ref part);
                        ownsNativePart = false;
                        return true;
                    }

                    AppendNativePart(ref nativeParts, ref nativePartCount,
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

            parts = MultipartMessageCollection.FromNativeParts(nativeParts,
                nativePartCount);
            return true;
        }
        catch
        {
            CloseNativeParts(nativeParts, nativePartCount);
            singlePart?.Dispose();
            hwmBudgetLeases?.Dispose();
            hwmBudgetLeases = null;
            throw;
        }
    }

    private bool ReceiveRoutedParts(int flags, bool retainCredit,
        out RoutingIdSnapshot routingId,
        out ulong requestSeq, out Message? singlePart,
        out MultipartMessageCollection? parts,
        out HwmBudgetLeaseOwner? hwmBudgetLeases,
        bool allowNoData = false)
    {
        routingId = default;
        requestSeq = 0;
        singlePart = null;
        parts = null;
        hwmBudgetLeases = null;
        if (_policy.UsesRouterRoutedReceiveEnvelope)
            return ReceiveRouterParts(flags, retainCredit, out routingId,
                out requestSeq, out singlePart,
                out parts, out hwmBudgetLeases, allowNoData);

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
                var ownsNativePart = true;
                try
                {
                    var lease = IntPtr.Zero;
                    IntPtr sourceNodeRid;
                    int basicHasMore;
                    var rc = retainCredit
                        ? NativeMethods.zlink_recv_part_with_hwm_budget_lease(
                            Handle, out sourceNodeRid, ref part, out lease,
                            out basicHasMore, flags)
                        : (flags & DontWaitFlag) != 0
                            ? NativeMethods.zlink_recv_part_nowait(Handle,
                                out sourceNodeRid, ref part,
                                out basicHasMore, flags)
                            : NativeMethods.zlink_recv_part(Handle,
                                out sourceNodeRid, ref part,
                                out basicHasMore, flags);
                    if (rc != 0)
                    {
                        HwmBudgetLeaseOwner.ReleaseUnowned(ref lease);
                        var errno = NativeMethods.zlink_errno();
                        if (allowNoData && nativePartCount == 0
                                        && ZlinkException.MapErrorCode(errno) is ErrorCode.EAgain
                                            or ErrorCode.EBusy)
                            return false;
                        throw ZlinkException.CreateRecvException(errno);
                    }

                    if (retainCredit)
                        HwmBudgetLeaseOwner.Adopt(ref hwmBudgetLeases,
                            ref lease);
                    if (!routingId.HasValue)
                        routingId = RoutingIdSnapshot.FromPointer(sourceNodeRid);
                    if (basicHasMore == 0 && nativePartCount == 0)
                    {
                        singlePart = Message.AdoptNativeFromPool(ref part);
                        ownsNativePart = false;
                        return true;
                    }

                    AppendNativePart(ref nativeParts, ref nativePartCount,
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

            parts = MultipartMessageCollection.FromNativeParts(nativeParts,
                nativePartCount);
            return true;
        }
        catch
        {
            CloseNativeParts(nativeParts, nativePartCount);
            singlePart?.Dispose();
            hwmBudgetLeases?.Dispose();
            hwmBudgetLeases = null;
            throw;
        }
    }

    private bool ReceiveRouterParts(int flags, bool retainCredit,
        out RoutingIdSnapshot routingId,
        out ulong requestSeq, out Message? singlePart,
        out MultipartMessageCollection? parts,
        out HwmBudgetLeaseOwner? hwmBudgetLeases, bool allowNoData)
    {
        var nativeParts = Array.Empty<ZlinkMsg>();
        var nativePartCount = 0;
        routingId = default;
        requestSeq = 0;
        singlePart = null;
        parts = null;
        hwmBudgetLeases = null;
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
                    var lease = IntPtr.Zero;
                    IntPtr sourceNodeRid;
                    ulong receivedRequestSeq;
                    int hasMore;
                    var rc = retainCredit
                        ? NativeMethods
                            .zlink_router_recv_part_v2_with_hwm_budget_lease(
                                Handle, out sourceNodeRid,
                                out receivedRequestSeq, out _, out _, ref part,
                                out lease, out hasMore, flags)
                        : (flags & DontWaitFlag) != 0
                            ? NativeMethods.zlink_router_recv_part_nowait(
                                Handle, out sourceNodeRid,
                                out receivedRequestSeq, ref part, out hasMore,
                                flags)
                            : NativeMethods.zlink_router_recv_part(Handle,
                                out sourceNodeRid, out receivedRequestSeq,
                                ref part, out hasMore, flags);
                    if (rc != 0)
                    {
                        HwmBudgetLeaseOwner.ReleaseUnowned(ref lease);
                        var errno = NativeMethods.zlink_errno();
                        if (allowNoData && nativePartCount == 0
                                        && ZlinkException.MapErrorCode(errno) is ErrorCode.EAgain
                                            or ErrorCode.EBusy)
                            return false;

                        throw ZlinkException.CreateRecvException(errno);
                    }

                    if (retainCredit)
                        HwmBudgetLeaseOwner.Adopt(ref hwmBudgetLeases,
                            ref lease);
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
                        ownsNativePart = false;
                        return true;
                    }

                    AppendNativePart(ref nativeParts, ref nativePartCount,
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

            parts = MultipartMessageCollection.FromNativeParts(nativeParts,
                nativePartCount);
            return true;
        }
        catch
        {
            CloseNativeParts(nativeParts, nativePartCount);
            singlePart?.Dispose();
            hwmBudgetLeases?.Dispose();
            hwmBudgetLeases = null;
            throw;
        }
    }

    private unsafe bool ReceiveSubscribedParts(int flags, bool retainCredit,
        byte[] topicBuffer, Message reusableSinglePart,
        out RoutingIdSnapshot routingId, out int topicLength,
        out Message? singlePart, out MultipartMessageCollection? parts,
        out HwmBudgetLeaseOwner? hwmBudgetLeases,
        bool allowNoData = false)
    {
        var nativeParts = Array.Empty<ZlinkMsg>();
        var nativePartCount = 0;
        routingId = default;
        topicLength = 0;
        singlePart = null;
        parts = null;
        hwmBudgetLeases = null;
        try
        {
            while (true)
            {
                var firstPart = nativePartCount == 0;
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
                var lease = IntPtr.Zero;
                try
                {
                    IntPtr sourceRoutingId;
                    nuint nativeTopicLength;
                    int hasMore;
                    int rc;
                    if (retainCredit)
                    {
                        rc = firstPart
                            ? NativeMethods
                                .zlink_subscribe_part_with_hwm_budget_lease(
                                    Handle, out sourceRoutingId, topicBuffer,
                                    (nuint)topicBuffer.Length,
                                    out nativeTopicLength,
                                    ref reusableSinglePart.Handle, out lease,
                                    out hasMore, flags)
                            : NativeMethods
                                .zlink_subscribe_part_with_hwm_budget_lease(
                                    Handle, out sourceRoutingId, topicBuffer,
                                    (nuint)topicBuffer.Length,
                                    out nativeTopicLength, ref part, out lease,
                                    out hasMore, flags);
                    }
                    else if (allowNoData)
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
                        HwmBudgetLeaseOwner.ReleaseUnowned(ref lease);
                        if (firstPart)
                            reusableSinglePart.CloseAfterFailedNativeReceive();
                        var errno = NativeMethods.zlink_errno();
                        if (allowNoData && nativePartCount == 0
                                        && ZlinkException.MapErrorCode(errno) is ErrorCode.EAgain
                                            or ErrorCode.EBusy)
                            return false;
                        throw ZlinkException.CreateRecvException(errno);
                    }

                    if (retainCredit)
                        HwmBudgetLeaseOwner.Adopt(ref hwmBudgetLeases,
                            ref lease);
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
                        AppendNativePart(ref nativeParts, ref nativePartCount,
                            ref reusableSinglePart.Handle);
                    else
                    {
                        AppendNativePart(ref nativeParts, ref nativePartCount,
                            ref part);
                        ownsNativePart = false;
                    }
                    if (hasMore == 0)
                        break;
                }
                finally
                {
                    HwmBudgetLeaseOwner.ReleaseUnowned(ref lease);
                    if (ownsNativePart)
                        NativeMethods.zlink_msg_close(ref part);
                }
            }

            parts = MultipartMessageCollection.FromNativeParts(nativeParts,
                nativePartCount);
            return true;
        }
        catch
        {
            CloseNativeParts(nativeParts, nativePartCount);
            singlePart?.Dispose();
            hwmBudgetLeases?.Dispose();
            hwmBudgetLeases = null;
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
