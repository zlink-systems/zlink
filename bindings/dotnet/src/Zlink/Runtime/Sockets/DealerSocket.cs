// SPDX-License-Identifier: MPL-2.0

using System.Runtime.CompilerServices;
using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink;

internal sealed class DealerSocket : ReceivingMessageSocketBase, IDealerSocket
{
    private static readonly TimeSpan DefaultRequestTimeout = TimeSpan.FromSeconds(5);

    public DealerSocket(Context context)
        : base(context, SocketType.Dealer)
    {
        Options = new DealerSocketOptions(this);
    }

    public RoutedSendOperation Send()
    {
        return new RoutedAsyncSendOperation(this);
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
        return RecvCore(result, flags);
    }

    private bool RecvCore(Received result, RecvFlags flags)
    {
        if (result == null)
            throw new ArgumentNullException(nameof(result));
        result.PrepareForReceive();

        List<Message>? parts = null;
        var messageType = ReceivedMessageType.Raw;
        ulong requestSeq = 0;
        var firstPart = true;
        var transferred = false;

        try
        {
            while (true)
            {
                Unsafe.SkipInit(out ZlinkMsg nativePart);
                var initRc = NativeMethods.zlink_msg_init(ref nativePart);
                if (initRc != 0)
                    throw ZlinkException.CreateRecvException(
                        NativeMethods.zlink_errno());

                var ownsNativePart = true;
                var recvFlags = firstPart ? (int)flags : 0;
                try
                {
                    byte nativeMessageType;
                    ulong nativeRequestSeq;
                    NativeMethods.ZlinkPartFlag hasMore;
                    var rc = NativeMethods.zlink_dealer_recv_part(Handle,
                        out nativeMessageType, out nativeRequestSeq,
                        ref nativePart, out hasMore, recvFlags);

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

                    parts ??= new List<Message>(4);
                    if (parts.Count == parts.Capacity)
                        parts.Capacity = checked(parts.Count * 2);
                    var adoptedPart = Message.AdoptNative(ref nativePart);
                    parts.Add(adoptedPart);
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
            {
                if (parts != null)
                    RequestReplySupport.DisposeParts(parts);
            }
            throw;
        }
    }

    internal Task<IReadOnlyList<Message>> RequestCore(
        IReadOnlyList<Message> parts, TimeSpan timeout,
        CancellationToken ct = default)
    {
        var timeoutMs = RequestReplySupport.NormalizeRequestTimeout(
            timeout,
            DefaultRequestTimeout);
        return Kernel.RequestAsync(null, parts, timeoutMs, ct);
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
