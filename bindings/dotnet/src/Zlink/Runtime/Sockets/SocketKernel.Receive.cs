// SPDX-License-Identifier: MPL-2.0

using System.Runtime.CompilerServices;
using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink.Runtime.Sockets.Internal;

internal sealed partial class SocketKernel
{
    // HOT PATH: caller-provided Received storage lets the kernel rewrite
    // internal state on each successful receive without allocating a new
    // Received value per call.

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    internal bool ReceiveInto(Received result, int flags)
    {
        return ReceiveIntoCore(result, flags, false);
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    internal bool ReceiveRetainedInto(Received result, int flags)
    {
        return ReceiveIntoCore(result, flags, true);
    }

    private bool ReceiveIntoCore(Received result, int flags,
        bool retainCredit)
    {
        // The typed socket classes that expose receive on their public surface
        // already guarantee the capability. Skipping a per-call policy lookup
        // keeps the storage-reuse path direct.
        if (result == null)
            throw new ArgumentNullException(nameof(result));
        result.PrepareForReceive();
        return TryReceiveIntoMessageCore(result, flags, retainCredit);
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    internal bool ReceivePartInto(Message result, out bool hasMore,
        int flags)
    {
        if (result == null)
            throw new ArgumentNullException(nameof(result));

        ZlinkMsg part = default;
        var initRc = NativeMethods.zlink_msg_init(ref part);
        if (initRc != 0)
            throw ZlinkException.CreateRecvException(
                NativeMethods.zlink_errno());

        var initialized = true;
        try
        {
            var rc = (flags & DontWaitFlag) != 0
                ? NativeMethods.zlink_recv_part_nowait(Handle,
                    out var sourceRoutingId, ref part, out var more,
                    flags)
                : NativeMethods.zlink_recv_part(Handle,
                    out sourceRoutingId, ref part, out more, flags);

            if (rc != 0)
            {
                NativeMethods.zlink_msg_close(ref part);
                initialized = false;
                var errno = NativeMethods.zlink_errno();
                if ((flags & DontWaitFlag) != 0
                    && ZlinkException.MapErrorCode(errno) is ErrorCode.EAgain
                        or ErrorCode.EBusy)
                {
                    hasMore = false;
                    return false;
                }

                throw ZlinkException.CreateRecvException(errno);
            }

            initialized = false;
            hasMore = more != 0;
            result.ReplaceNativeOwned(ref part);
            return true;
        }
        catch
        {
            if (initialized)
                NativeMethods.zlink_msg_close(ref part);
            throw;
        }
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    internal bool ReceiveRoutedPartInto(Message result,
        out RoutingId? routingId, out bool hasMore, int flags)
    {
        if (result == null)
            throw new ArgumentNullException(nameof(result));

        routingId = null;
        ZlinkMsg part = default;
        var initRc = NativeMethods.zlink_msg_init(ref part);
        if (initRc != 0)
            throw ZlinkException.CreateRecvException(
                NativeMethods.zlink_errno());

        var initialized = true;
        try
        {
            int rc;
            int more;
            IntPtr sourceRoutingId;
            if (_policy.UsesRouterRoutedReceiveEnvelope)
            {
                ulong requestSeq;
                rc = (flags & DontWaitFlag) != 0
                    ? NativeMethods.zlink_router_recv_part_nowait(Handle,
                        out sourceRoutingId, out requestSeq, ref part,
                        out more, flags)
                    : NativeMethods.zlink_router_recv_part(Handle,
                        out sourceRoutingId, out requestSeq, ref part,
                        out more, flags);
            }
            else
            {
                rc = (flags & DontWaitFlag) != 0
                    ? NativeMethods.zlink_recv_part_nowait(Handle,
                        out sourceRoutingId, ref part, out more, flags)
                    : NativeMethods.zlink_recv_part(Handle,
                        out sourceRoutingId, ref part, out more, flags);
            }

            hasMore = more != 0;

            if (rc != 0)
            {
                NativeMethods.zlink_msg_close(ref part);
                initialized = false;
                var errno = NativeMethods.zlink_errno();
                if ((flags & DontWaitFlag) != 0
                    && ZlinkException.MapErrorCode(errno) is ErrorCode.EAgain
                        or ErrorCode.EBusy)
                {
                    hasMore = false;
                    return false;
                }

                throw ZlinkException.CreateRecvException(errno);
            }

            initialized = false;
            routingId = RoutingIdSnapshot.FromPointer(sourceRoutingId)
                .ToRoutingId();
            result.ReplaceNativeOwned(ref part);
            return true;
        }
        catch
        {
            if (initialized)
                NativeMethods.zlink_msg_close(ref part);
            throw;
        }
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    internal bool ReceiveRoutedInto(Received result, int flags)
    {
        return ReceiveRoutedIntoCore(result, flags, false);
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    internal bool ReceiveRoutedRetainedInto(Received result, int flags)
    {
        return ReceiveRoutedIntoCore(result, flags, true);
    }

    private bool ReceiveRoutedIntoCore(Received result, int flags,
        bool retainCredit)
    {
        if (result == null)
            throw new ArgumentNullException(nameof(result));
        result.PrepareForReceive();
        return TryReceiveIntoRoutedCore(result, flags, retainCredit);
    }

    private bool TryReceiveIntoMessageCore(Received result, int flags,
        bool retainCredit)
    {
        var allowNoData = (flags & DontWaitFlag) != 0;
        if (!ReceiveBasicParts(flags, retainCredit,
                out var singlePart, out var parts,
                out var hwmBudgetLeases,
                allowNoData))
            return false;
        try
        {
            if (singlePart != null)
                result.PopulateSinglePart(singlePart, hwmBudgetLeases);
            else
                result.PopulateMultipart(parts!, hwmBudgetLeases);
            return true;
        }
        catch
        {
            DisposeReceivedAssembly(singlePart, parts, hwmBudgetLeases);
            throw;
        }
    }

    private bool TryReceiveIntoRoutedCore(Received result, int flags,
        bool retainCredit)
    {
        var allowNoData = (flags & DontWaitFlag) != 0;
        if (!ReceiveRoutedParts(flags, retainCredit, out var routingId,
                out var requestSeq,
                out var singlePart, out var parts,
                out var hwmBudgetLeases,
                allowNoData))
            return false;
        try
        {
            PopulateRoutedReceivedInto(result, singlePart, parts, routingId,
                requestSeq, hwmBudgetLeases);
            return true;
        }
        catch
        {
            DisposeReceivedAssembly(singlePart, parts, hwmBudgetLeases);
            throw;
        }
    }

    private static void DisposeReceivedAssembly(Message? singlePart,
        MultipartMessageCollection? parts,
        HwmBudgetLeaseOwner? hwmBudgetLeases)
    {
        try
        {
            if (singlePart != null)
                singlePart.Dispose();
            else
                parts?.Dispose();
        }
        finally
        {
            hwmBudgetLeases?.Dispose();
        }
    }

    private void PopulateRoutedReceivedInto(Received result,
        Message? singlePart, MultipartMessageCollection? parts,
        RoutingIdSnapshot routingId, ulong requestSeq,
        HwmBudgetLeaseOwner? hwmBudgetLeases)
    {
        if (requestSeq == 0)
        {
            if (singlePart != null)
                result.PopulateRoutedSinglePart(singlePart, routingId,
                    null, null, sendKernel: this,
                    hwmBudgetLeases: hwmBudgetLeases);
            else
                result.PopulateRoutedMultipart(parts!, routingId,
                    null, null, sendKernel: this,
                    hwmBudgetLeases: hwmBudgetLeases);
            return;
        }

        // Request-reply context: capture the routing ids and request seq in
        // a reply handler closure so Received.Reply() can dispatch via the
        // kernel. This path allocates a RoutingId / byte[] / closure per
        // recv; non-request-reply routed traffic (the common router-router
        // / dealer-router echo case) skips this branch entirely.
        var routingIdBytes = routingId.ToByteArray();
        var replyRoutingId = routingIdBytes == null
            ? null
            : RoutingId.FromOwnedOptionalBytes(routingIdBytes);
        ReceivedReplyHandler replyHandler = replyParts =>
        {
            if (replyRoutingId is null)
                throw new ZlinkSubmitException(SubmitResult.InvalidArgument,
                    (int)ErrorCode.EInval);
            SendReplyCore(replyRoutingId.Value, requestSeq, replyParts);
        };

        if (singlePart != null)
            result.PopulateRoutedSinglePart(singlePart, routingId,
                requestSeq, replyHandler, CreateRoutedSendHandler(routingId),
                CreateRoutedSendSingleHandler(routingId),
                hwmBudgetLeases: hwmBudgetLeases);
        else
            result.PopulateRoutedMultipart(parts!, routingId,
                requestSeq, replyHandler, CreateRoutedSendHandler(routingId),
                CreateRoutedSendSingleHandler(routingId),
                hwmBudgetLeases: hwmBudgetLeases);
    }
}
