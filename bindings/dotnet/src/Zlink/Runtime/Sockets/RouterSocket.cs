// SPDX-License-Identifier: MPL-2.0

using System.Runtime.InteropServices;
using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink;

internal sealed class RouterSocket : RoutedMessageSocketBase, IRouterSocket
{
    private static readonly TimeSpan DefaultRequestTimeout = TimeSpan.FromSeconds(5);

    private static readonly NativeMethods.ZlinkReplyHandlerDelegate RequestReplyHandler =
        OnRequestReply;

    private static readonly NativeMethods.ZlinkReplyHandlerDelegate DirectRequestReplyHandler =
        OnDirectRequestReply;

    private static readonly IntPtr RequestReplyHandlerPtr =
        Marshal.GetFunctionPointerForDelegate(RequestReplyHandler);

    private static readonly IntPtr DirectRequestReplyHandlerPtr =
        Marshal.GetFunctionPointerForDelegate(DirectRequestReplyHandler);

    public RouterSocket(Context context)
        : base(context, SocketType.Router)
    {
        Options = new RouterSocketOptions(this);
    }

    public new RouterSocketOptions Options { get; }

    public void SetRoutingId(RoutingId routingId)
    {
        Kernel.SetOption(SocketOptions.RoutingId, routingId.ToBytes());
    }

    public RoutingId GetRoutingId()
    {
        return RoutingId.From(Kernel.GetOption(SocketOptions.RoutingId));
    }

    public void Connect(string address)
    {
        SocketConnectionOperations.Connect(Kernel, address);
    }

    public void Disconnect(string address)
    {
        SocketConnectionOperations.Disconnect(Kernel, address);
    }

    public void DisconnectRid(RoutingId peerRid)
    {
        SocketConnectionOperations.DisconnectRid(Kernel, peerRid);
    }

    /// <summary>
    ///     Start a request to a specific peer (operation builder).
    /// </summary>
    public RequestOperation Request(RoutingId peerRid)
    {
        return new RouterPeerRequestOperation(this, peerRid);
    }

    /// <summary>
    ///     Start a reply (operation builder).
    /// </summary>
    public ReplyOperation Reply(RoutingId rid, ulong requestSeq)
    {
        return new RouterPeerReplyOperation(this, rid, requestSeq);
    }

    public bool TrySendCompletionControl(
        RoutingId peerRid,
        IReadOnlyList<Message> parts)
    {
        RequestReplySupport.EnsureParts(parts, nameof(parts));
        var nativeRoutingId = peerRid.ToNative();
        var cloned = RequestReplySupport.CloneParts(parts);
        try
        {
            for (var i = 0; i < cloned.Length; i++)
            {
                ZlinkMsg nativePart = default;
                cloned[i].MoveTo(ref nativePart);
                var rc = NativeMethods.zlink_router_completion_control_part(
                    Handle, ref nativeRoutingId, ref nativePart,
                    i + 1 < cloned.Length
                        ? NativeMethods.ZlinkPartFlag.More
                        : NativeMethods.ZlinkPartFlag.Final);
                if (rc == (int)SubmitResult.Ok)
                    continue;
                if (rc == (int)SubmitResult.Backpressured)
                    return false;
                throw ZlinkException.CreateSubmitException(
                    NativeMethods.zlink_errno());
            }

            return true;
        }
        finally
        {
            RequestReplySupport.DisposeParts(cloned);
        }
    }

    public void OnCompletionControl(CompletionControlHandler handler)
    {
        Kernel.CompletionControlHandler(handler);
    }

    internal async Task<IReadOnlyList<Message>> RequestCore(RoutingId peerRid,
        IReadOnlyList<Message> parts, TimeSpan timeout, CancellationToken ct)
    {
        var received = await RequestAsyncCore(peerRid, parts,
                timeout == TimeSpan.Zero ? DefaultRequestTimeout : timeout, ct)
            .ConfigureAwait(false);
        return received.Parts;
    }

    internal bool RequestCallbackCore(RoutingId peerRid,
        IReadOnlyList<Message> parts, RequestCallback callback,
        SendFlags flags, TimeSpan timeout)
    {
        if (callback == null)
            throw new ArgumentNullException(nameof(callback));
        RequestReplySupport.EnsureParts(parts, nameof(parts));
        var nativeRoutingId = peerRid.ToNative();
        var timeoutMs = RequestReplySupport.NormalizeRequestTimeout(
            timeout == TimeSpan.Zero ? TimeSpan.Zero : timeout,
            DefaultRequestTimeout);
        GCHandle handle = default;
        RequestCallbackCompletion? state = null;

        try
        {
            // Hot path: router request callback mirrors DealerSocket. Keep this
            // on direct native callback state so router-router request windows
            // do not pay TaskCompletionSource/continuation overhead.
            state = new RequestCallbackCompletion(callback);
            handle = GCHandle.Alloc(state, GCHandleType.Normal);
            var userData = GCHandle.ToIntPtr(handle);

            lock (SubmitGate)
            {
                RequestReplySupport.SubmitOwnedParts(parts,
                    (ref ZlinkMsg nativePart,
                            NativeMethods.ZlinkPartFlag partFlag) =>
                        NativeMethods.zlink_router_request_part(Handle,
                            ref nativeRoutingId, ref nativePart, (int)flags,
                            partFlag, timeoutMs, DirectRequestReplyHandlerPtr,
                            userData));
            }

            state.AttachProgress(RequestProgressPump.AttachSocketCallback(Handle));
            return true;
        }
        catch (ZlinkException error) when ((flags & SendFlags.DontWait) != 0)
        {
            state?.DisposeProgress();
            if (handle.IsAllocated)
                handle.Free();

            if (RequestReplySupport.MapSendNoWaitResult(error)
                == SendResult.Backpressured)
                return false;

            throw;
        }
        catch
        {
            state?.DisposeProgress();
            if (handle.IsAllocated)
                handle.Free();
            throw;
        }
    }

    internal void ReplyCore(RoutingId peerRid, ulong requestSeq,
        IReadOnlyList<Message> parts)
    {
        RequestReplySupport.EnsureParts(parts, nameof(parts));
        var nativeRoutingId = peerRid.ToNative();
        var cloned = RequestReplySupport.CloneParts(parts);
        lock (SubmitGate)
        {
            try
            {
                RequestReplySupport.SubmitClonedParts(cloned,
                    (ref ZlinkMsg nativePart,
                            NativeMethods.ZlinkPartFlag partFlag) =>
                        NativeMethods.zlink_router_reply_part(Handle,
                            ref nativeRoutingId, requestSeq, ref nativePart,
                            partFlag));
            }
            finally
            {
                RequestReplySupport.DisposeParts(cloned);
            }
        }
    }

    private Task<Received> RequestAsyncCore(RoutingId peerRid,
        IReadOnlyList<Message> parts, TimeSpan timeout, CancellationToken ct,
        int flags = 0)
    {
        RequestReplySupport.EnsureParts(parts, nameof(parts));
        var nativeRoutingId = peerRid.ToNative();
        var cloned = RequestReplySupport.CloneParts(parts);
        var timeoutMs = RequestReplySupport.NormalizeRequestTimeout(timeout,
            DefaultRequestTimeout);
        var completion = new TaskCompletionSource<Received>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        GCHandle handle = default;

        try
        {
            RequestCallState state = new(completion);
            handle = GCHandle.Alloc(state, GCHandleType.Normal);

            if (ct.CanBeCanceled)
                state.SetCancellationRegistration(
                    ct.Register(static userdata => { RequestCallState.CancelFromUserData(userdata); }, handle));

            var userData = GCHandle.ToIntPtr(handle);
            lock (SubmitGate)
            {
                RequestReplySupport.SubmitClonedParts(cloned,
                    (ref ZlinkMsg nativePart,
                            NativeMethods.ZlinkPartFlag partFlag) =>
                        NativeMethods.zlink_router_request_part(Handle,
                            ref nativeRoutingId, ref nativePart, flags, partFlag,
                            timeoutMs, RequestReplyHandlerPtr, userData));
            }

            return RequestProgressPump.AttachSocket(Handle, completion.Task);
        }
        catch
        {
            RequestReplySupport.DisposeParts(cloned);
            if (handle.IsAllocated)
                handle.Free();
            throw;
        }
    }

    private static void OnRequestReply(int result, IntPtr parts, nuint partCount,
        IntPtr userData)
    {
        RequestReplySupport.CompleteReceivedReply(result, parts, partCount,
            userData);
    }

    private static void OnDirectRequestReply(int result, IntPtr parts,
        nuint partCount, IntPtr userData)
    {
        CompleteDirectReply(result, parts, partCount, userData);
    }

    private static void CompleteDirectReply(int result, IntPtr parts,
        nuint partCount, IntPtr userData)
    {
        var handle = GCHandle.FromIntPtr(userData);
        var state = (RequestCallbackCompletion)handle.Target!;
        try
        {
            if (!state.TryStartCompletion())
                return;

            if (result != 0)
            {
                state.Invoke((RequestResult)result, Array.Empty<Message>());
                return;
            }

            var replyParts = Message.FromNativeVector(parts, partCount);
            parts = IntPtr.Zero;
            partCount = 0;
            state.Invoke(RequestResult.Ok, replyParts);
        }
        finally
        {
            if (parts != IntPtr.Zero)
                NativeMethods.zlink_multipart_close(parts, partCount);
            handle.Free();
        }
    }

}
