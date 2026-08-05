// SPDX-License-Identifier: MPL-2.0

using System.Buffers;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink;

internal static class RequestReplySupport
{
    private const int ErrnoEAgainWin = 35;
    private const int ErrnoEWouldBlockWin = 10035;

    internal static Message[] CloneParts(IReadOnlyList<Message> parts)
    {
        if (parts.Count == 0)
            throw new ArgumentException("parts must not be empty", nameof(parts));
        var cloned = new Message[parts.Count];
        for (var i = 0; i < parts.Count; i++)
            cloned[i] = parts[i].Copy();
        return cloned;
    }

    internal static void EnsureParts(IReadOnlyList<Message> parts,
        string paramName)
    {
        if (parts == null)
            throw new ArgumentNullException(paramName);
        if (parts.Count == 0)
            throw new ArgumentException("Parts must not be empty.", paramName);
    }

    internal static uint NormalizeTimeout(TimeSpan timeout)
    {
        return BoundaryValidation.EncodeTimeoutMilliseconds(timeout,
            nameof(timeout));
    }

    internal static uint NormalizeRequestTimeout(TimeSpan timeout,
        TimeSpan defaultTimeout)
    {
        var effective = timeout == TimeSpan.Zero
            ? defaultTimeout
            : timeout;
        return BoundaryValidation.EncodeTimeoutMilliseconds(effective,
            nameof(timeout));
    }

    internal static void DisposeParts(IEnumerable<Message> parts)
    {
        foreach (var part in parts)
            part.Dispose();
    }

    internal static void SubmitClonedParts(IReadOnlyList<Message> parts,
        NativePartSubmitter submit)
    {
        if (parts == null)
            throw new ArgumentNullException(nameof(parts));
        if (parts.Count == 0)
            throw new ArgumentException("parts must not be empty", nameof(parts));
        if (submit == null)
            throw new ArgumentNullException(nameof(submit));

        for (var i = 0; i < parts.Count; i++)
        {
            ZlinkMsg nativePart = default;
            parts[i].MoveTo(ref nativePart);
            var submitted = false;
            try
            {
                var rc = submit(ref nativePart, i + 1 < parts.Count
                    ? NativeMethods.ZlinkPartFlag.More
                    : NativeMethods.ZlinkPartFlag.Final);
                submitted = true;
                if (rc != 0)
                    throw ZlinkException.CreateSubmitException(
                        NativeMethods.zlink_errno());
            }
            finally
            {
                if (!submitted)
                    NativeMethods.zlink_msg_close(ref nativePart);
            }
        }
    }

    internal static void SubmitOwnedSinglePart(Message part,
        NativePartSubmitter submit)
    {
        if (part == null)
            throw new ArgumentNullException(nameof(part));
        if (submit == null)
            throw new ArgumentNullException(nameof(submit));

        var submitter = new DelegateSinglePartSubmitter(submit);
        var rc = SinglePartSubmit.Submit(part, ref submitter);
        if (rc != 0)
            throw ZlinkException.CreateSubmitException(NativeMethods.zlink_errno());
    }

    private readonly struct DelegateSinglePartSubmitter
        : INativeSinglePartSubmitter<DelegateSinglePartSubmitter>
    {
        private readonly NativePartSubmitter _submit;

        internal DelegateSinglePartSubmitter(NativePartSubmitter submit)
        {
            _submit = submit;
        }

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public static int Submit(ref DelegateSinglePartSubmitter submitter,
            ref ZlinkMsg nativePart)
        {
            return submitter._submit(ref nativePart,
                NativeMethods.ZlinkPartFlag.Final);
        }
    }

    internal static void SubmitOwnedParts(IReadOnlyList<Message> parts,
        NativePartSubmitter submit)
    {
        if (parts == null)
            throw new ArgumentNullException(nameof(parts));
        if (parts.Count == 0)
            throw new ArgumentException("parts must not be empty", nameof(parts));
        if (submit == null)
            throw new ArgumentNullException(nameof(submit));

        if (parts.Count == 1)
        {
            SubmitOwnedSinglePart(parts[0], submit);
            return;
        }

        Message[]? copiedParts = null;
        var sourceParts = NativeMessageParts.AsSpan(parts, ref copiedParts);
        ZlinkMsg[]? rentedNative = null;
        // Hot path: request/reply messages are normally one or two parts. Keep
        // native descriptors on the stack for that case and rent only for larger
        // multipart frames.
        var nativeParts = sourceParts.Length <= NativeMessageParts.StackPartLimit
            ? stackalloc ZlinkMsg[NativeMessageParts.StackPartLimit]
            : rentedNative = ArrayPool<ZlinkMsg>.Shared.Rent(sourceParts.Length);
        nativeParts = nativeParts[..sourceParts.Length];

        var built = 0;
        var submitted = 0;
        try
        {
            NativeMessageParts.MoveToNative(sourceParts, nativeParts,
                nameof(parts), ref built);
            for (var i = 0; i < built; i++)
            {
                var rc = submit(ref nativeParts[i], i + 1 < built
                    ? NativeMethods.ZlinkPartFlag.More
                    : NativeMethods.ZlinkPartFlag.Final);
                submitted = i + 1;
                if (rc == 0)
                    continue;

                throw ZlinkException.CreateSubmitException(
                    NativeMethods.zlink_errno());
            }
        }
        catch
        {
            NativeMessageParts.RestoreManaged(sourceParts, nativeParts,
                submitted, built - submitted);
            throw;
        }
        finally
        {
            if (rentedNative != null)
                ArrayPool<ZlinkMsg>.Shared.Return(rentedNative);
        }
    }

    internal static void AttachResultCallback(Func<Task<Received>> invoke,
        Action<RequestResult, Received?> callback)
    {
        if (callback == null)
            throw new ArgumentNullException(nameof(callback));
        var context = SynchronizationContext.Current;
        _ = invoke().ContinueWith(task =>
            {
                if (task.IsFaulted)
                {
                    var error = task.Exception?.GetBaseException()
                                ?? new ZlinkRequestException(RequestResult.ProtocolError);
                    if (error is ZlinkRequestException requestError)
                    {
                        DeliverCallback(context, () => callback(
                            (RequestResult)requestError.Code, null));
                        return;
                    }

                    if (error is ZlinkSubmitException submitError)
                    {
                        DeliverCallback(context, () => callback(
                            MapSubmitFailureResult(submitError), null));
                        return;
                    }

                    DeliverCallback(context, () => callback(
                        RequestResult.ProtocolError, null));
                    return;
                }

                if (task.IsCanceled)
                {
                    DeliverCallback(context, () => callback(
                        RequestResult.Terminated, null));
                    return;
                }

                DeliverCallback(context, () => callback(RequestResult.Ok,
                    task.Result));
            }, CancellationToken.None, TaskContinuationOptions.ExecuteSynchronously,
            TaskScheduler.Default);
    }

    internal static void CompleteReceivedReply(int result, IntPtr parts,
        nuint partCount, IntPtr userData)
    {
        var handle = GCHandle.FromIntPtr(userData);
        var state = (RequestCallState)handle.Target!;
        try
        {
            if (result != 0)
            {
                state.TrySetException(new ZlinkRequestException(
                    (RequestResult)result));
                return;
            }

            var replyParts = Message.FromNativeVector(parts, partCount);
            parts = IntPtr.Zero;
            partCount = 0;
            var received = new Received((RoutingId?)null, replyParts);
            if (!state.TrySetResult(received))
                DisposeParts(replyParts);
        }
        finally
        {
            if (parts != IntPtr.Zero)
                NativeMethods.zlink_multipart_close(parts, partCount);
            handle.Free();
        }
    }

    private static void DeliverCallback(SynchronizationContext? context,
        Action action)
    {
        if (context != null)
        {
            CallbackDelivery.Post(context, action);
            return;
        }

        try
        {
            action();
        }
        catch (Exception ex)
        {
            CallbackExceptionHub.Report(ex);
        }
    }

    internal static SendResult MapSendNoWaitResult(ZlinkException error)
    {
        var code = ZlinkException.MapErrorCode(error.NativeErrno);
        return code switch
        {
            ErrorCode.EAgain => SendResult.Backpressured,
            _ when error.NativeErrno == ErrnoEAgainWin ||
                   error.NativeErrno == ErrnoEWouldBlockWin =>
                SendResult.Backpressured,
            _ => throw error
        };
    }

    private static RequestResult MapSubmitFailureResult(ZlinkSubmitException error)
    {
        return error.Result switch
        {
            ZlinkSubmitException.ErrorCode.NotConnected => RequestResult.NotConnected,
            ZlinkSubmitException.ErrorCode.NotFound => RequestResult.NotFound,
            ZlinkSubmitException.ErrorCode.NotAdmitted
                or ZlinkSubmitException.ErrorCode.InvalidState => RequestResult.Rejected,
            ZlinkSubmitException.ErrorCode.Backpressured => RequestResult.Busy,
            ZlinkSubmitException.ErrorCode.InvalidArgument => RequestResult.InvalidArgument,
            ZlinkSubmitException.ErrorCode.NotSupported => RequestResult.NotSupported,
            ZlinkSubmitException.ErrorCode.Terminated => RequestResult.Terminated,
            _ => RequestResult.InternalError
        };
    }

    internal delegate int NativePartSubmitter(
        ref ZlinkMsg nativePart, NativeMethods.ZlinkPartFlag partFlag);
}
