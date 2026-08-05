// SPDX-License-Identifier: MPL-2.0

using System.Runtime.InteropServices;
using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink;

internal sealed class DealerSocket : MessageSocketBase, IDealerSocket
{
    private static readonly TimeSpan DefaultRequestTimeout = TimeSpan.FromSeconds(5);

    private static readonly NativeMethods.ZlinkReplyHandlerDelegate RequestReplyHandler =
        OnRequestReply;

    private static readonly IntPtr RequestReplyHandlerPtr =
        Marshal.GetFunctionPointerForDelegate(RequestReplyHandler);

    public DealerSocket(Context context)
        : base(context, SocketType.Dealer)
    {
        Options = new DealerSocketOptions(this);
    }

    public new DealerSocketOptions Options { get; }

    public void SetRoutingId(RoutingId routingId)
    {
        Kernel.SetOption(SocketOptions.RoutingId, routingId.ToBytes());
    }

    public RoutingId GetRoutingId()
    {
        return RoutingId.From(Kernel.GetOption(SocketOptions.RoutingId));
    }

    /// <summary>
    ///     Start a dealer request (operation builder).
    /// </summary>
    public RequestOperation Request()
    {
        return new DealerRequestOperation(this);
    }

    public override bool Recv(Received result, RecvFlags flags = RecvFlags.None)
    {
        if (result == null)
            throw new ArgumentNullException(nameof(result));

        List<Message>? parts = null;
        var messageType = ReceivedMessageType.Raw;
        ulong requestSeq = 0;
        var firstPart = true;
        var transferred = false;

        try
        {
            while (true)
            {
                ZlinkMsg nativePart = default;
                var initRc = NativeMethods.zlink_msg_init(ref nativePart);
                if (initRc != 0)
                    throw ZlinkException.CreateRecvException(
                        NativeMethods.zlink_errno());

                var ownsNativePart = true;
                var recvFlags = firstPart ? (int)flags : 0;
                try
                {
                    var rc = NativeMethods.zlink_dealer_recv_part(Handle,
                        out var nativeMessageType, out var nativeRequestSeq,
                        ref nativePart, out var hasMore,
                        recvFlags);

                    if (rc != 0)
                    {
                        var errno = NativeMethods.zlink_errno();
                        if (firstPart && (flags & RecvFlags.DontWait) != 0
                                      && ZlinkException.MapErrorCode(errno) is ErrorCode.EAgain
                                          or ErrorCode.EBusy)
                            return false;

                        throw ZlinkException.CreateRecvException(errno);
                    }

                    messageType = (ReceivedMessageType)nativeMessageType;
                    requestSeq = nativeRequestSeq;

                    if (hasMore == NativeMethods.ZlinkPartFlag.Final
                        && firstPart)
                    {
                        // HOT PATH: single-part DEALER receive backs the
                        // inproc DEALER_DEALER benchmark and ordinary
                        // message traffic. Keep this on caller-provided
                        // Received storage and avoid List<Message> /
                        // Message[] materialization; request/reply metadata
                        // still stays inside the binding contract.
                        var singlePart = Message.AdoptNativeFromPool(
                            ref nativePart);
                        ownsNativePart = false;
                        if (messageType == ReceivedMessageType.Raw
                            && requestSeq == 0)
                        {
                            result.PopulateSinglePart(singlePart);
                            transferred = true;
                            return true;
                        }

                        var singleReplyHandler =
                            messageType == ReceivedMessageType.Request
                            && requestSeq != 0
                                ? CreateReplyHandler(requestSeq)
                                : null;
                        result.PopulateMessageEnvelopeSingle(singlePart,
                            messageType, requestSeq == 0 ? null : requestSeq,
                            singleReplyHandler);
                        transferred = true;
                        return true;
                    }

                    parts ??= new List<Message>();
                    parts.Add(Message.AdoptNative(ref nativePart));
                    ownsNativePart = false;
                    firstPart = false;

                    if (hasMore == NativeMethods.ZlinkPartFlag.Final)
                        break;
                }
                finally
                {
                    if (ownsNativePart)
                        NativeMethods.zlink_msg_close(ref nativePart);
                }
            }

            var replyHandler =
                messageType == ReceivedMessageType.Request && requestSeq != 0
                    ? CreateReplyHandler(requestSeq)
                    : null;
            result.PopulateMessageEnvelope(parts!.ToArray(), messageType,
                requestSeq == 0 ? null : requestSeq, replyHandler);
            transferred = true;
            return true;
        }
        catch
        {
            if (!transferred)
                if (parts != null)
                    RequestReplySupport.DisposeParts(parts);
            throw;
        }
    }

    internal async Task<IReadOnlyList<Message>> RequestCore(
        IReadOnlyList<Message> parts, TimeSpan timeout,
        CancellationToken ct = default)
    {
        var received = await RequestAsyncCore(parts,
                timeout == TimeSpan.Zero ? DefaultRequestTimeout : timeout, ct)
            .ConfigureAwait(false);
        return received.Parts;
    }

    internal bool RequestCallbackCore(IReadOnlyList<Message> parts,
        RequestCallback callback, SendFlags flags = SendFlags.None,
        TimeSpan? timeout = null)
    {
        if (callback == null)
            throw new ArgumentNullException(nameof(callback));
        RequestReplySupport.EnsureParts(parts, nameof(parts));
        var timeoutMs = RequestReplySupport.NormalizeRequestTimeout(
            timeout ?? TimeSpan.Zero, DefaultRequestTimeout);
        GCHandle handle = default;
        RequestCallbackCompletion? state = null;

        try
        {
            // Hot path: callback request is used by windowed perf and framework
            // request pumps. Complete directly from native callback state instead
            // of building a TaskCompletionSource/continuation per request.
            state = new RequestCallbackCompletion(callback);
            handle = GCHandle.Alloc(state, GCHandleType.Normal);
            var userData = GCHandle.ToIntPtr(handle);

            lock (SubmitGate)
            {
                RequestReplySupport.SubmitOwnedParts(parts,
                    (ref ZlinkMsg nativePart,
                            NativeMethods.ZlinkPartFlag partFlag) =>
                        NativeMethods.zlink_dealer_request_part(Handle,
                            ref nativePart, (int)flags, partFlag, timeoutMs,
                            DirectRequestReplyHandlerPtr, userData));
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

    private Task<Received> RequestAsyncCore(IReadOnlyList<Message> parts,
        TimeSpan timeout, CancellationToken ct, int flags = 0)
    {
        if (parts == null)
            throw new ArgumentNullException(nameof(parts));

        var timeoutMs = RequestReplySupport.NormalizeRequestTimeout(timeout,
            DefaultRequestTimeout);
        var completion = new TaskCompletionSource<Received>(
            TaskCreationOptions.RunContinuationsAsynchronously);
        GCHandle handle = default;

        try
        {
            RequestCallState state = new(completion);
            handle = GCHandle.Alloc(state, GCHandleType.Normal);
            var userData = GCHandle.ToIntPtr(handle);

            if (ct.CanBeCanceled)
                state.SetCancellationRegistration(
                    ct.Register(static userdata => { RequestCallState.CancelFromUserData(userdata); }, handle));

            lock (SubmitGate)
            {
                RequestReplySupport.SubmitOwnedParts(parts,
                    (ref ZlinkMsg nativePart,
                            NativeMethods.ZlinkPartFlag partFlag) =>
                        NativeMethods.zlink_dealer_request_part(Handle,
                            ref nativePart, flags, partFlag, timeoutMs,
                            RequestReplyHandlerPtr, userData));
            }

            return RequestProgressPump.AttachSocket(Handle, completion.Task);
        }
        catch
        {
            if (handle.IsAllocated)
                handle.Free();
            throw;
        }
    }

    private ReceivedReplyHandler CreateReplyHandler(ulong requestSeq)
    {
        return replyParts => ReplyCore(requestSeq, replyParts);
    }

    private void ReplyCore(ulong requestSeq,
        IReadOnlyList<Message> parts)
    {
        if (parts == null)
            throw new ArgumentNullException(nameof(parts));

        var cloned = RequestReplySupport.CloneParts(parts);
        lock (SubmitGate)
        {
            try
            {
                RequestReplySupport.SubmitClonedParts(cloned,
                    (ref ZlinkMsg nativePart,
                            NativeMethods.ZlinkPartFlag partFlag) =>
                        NativeMethods.zlink_dealer_reply_part(Handle,
                            requestSeq, ref nativePart, partFlag));
            }
            finally
            {
                RequestReplySupport.DisposeParts(cloned);
            }
        }
    }

    private static void OnRequestReply(int result, IntPtr parts, nuint partCount,
        IntPtr userData)
    {
        RequestReplySupport.CompleteReceivedReply(result, parts, partCount,
            userData);
    }

    private static readonly NativeMethods.ZlinkReplyHandlerDelegate DirectRequestReplyHandler =
        OnDirectRequestReply;

    private static readonly IntPtr DirectRequestReplyHandlerPtr =
        Marshal.GetFunctionPointerForDelegate(DirectRequestReplyHandler);

    private static void OnDirectRequestReply(int result, IntPtr parts,
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
