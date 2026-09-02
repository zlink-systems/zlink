// SPDX-License-Identifier: MPL-2.0

using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink.Runtime.Sockets.Internal;

internal sealed partial class SocketKernel : IDisposable
{
    private const int TopicBufferSize = 4096;
    private const int DontWaitFlag = 1;
    private readonly SocketHandle _handle;
    private readonly SocketOptionAccessor _options;
    private readonly SocketTypePolicy _policy;
    private readonly CompletionOwner? _completion;
    private readonly object _replyTokenOwner = new();

    public SocketKernel(Context context, SocketType type)
    {
        _handle = new SocketHandle(context, type);
        _options = new SocketOptionAccessor(_handle);
        _policy = new SocketTypePolicy(type);
        _completion = CreateCompletionOwner(type);
    }

    public IntPtr Handle => _handle.DangerousGetHandle();
    public SocketType Type => _policy.SocketType;
    /// <summary>The socket-local pull-completion owner.</summary>
    internal CompletionOwner Completion
    {
        get
        {
            if (Type is not (SocketType.Pair or SocketType.Dealer
                or SocketType.Router or SocketType.Stream))
                throw new NotSupportedException(
                    "Asynchronous send requires a PAIR, DEALER, ROUTER, or STREAM socket.");

            return _completion!;
        }
    }

    internal object ReplyTokenOwner => _replyTokenOwner;

    private CompletionOwner? CreateCompletionOwner(SocketType type)
    {
        return type is SocketType.Pair or SocketType.Dealer
            or SocketType.Router or SocketType.Stream
            ? new CompletionOwner(Handle, type)
            : null;
    }

    internal Task<IReadOnlyList<Message>> RequestAsync(
        RoutingId? routerRoutingId, IReadOnlyList<Message> parts,
        uint timeoutMs, CancellationToken cancellationToken)
    {
        return Completion.RequestAsync(routerRoutingId, parts, timeoutMs,
            cancellationToken);
    }

    internal IReadOnlyList<Message> Request(RoutingId? routerRoutingId,
        IReadOnlyList<Message> parts, uint timeoutMs)
    {
        return Completion.Request(routerRoutingId, parts, timeoutMs);
    }

    public bool ReceiveSubscriptionEvent(SubscriptionEvent result,
        RecvFlags flags = RecvFlags.None)
    {
        EnsureSupports(nameof(ReceiveSubscriptionEvent),
            SocketTypePolicy.SocketCapability.SubscriptionEvents);
        if (result == null)
            throw new ArgumentNullException(nameof(result));
        return ReceiveSubscriptionEventInto(result, (int)flags);
    }

    internal void SendReplyCore(RoutingId routingId,
        ReplyToken replyToken, IReadOnlyList<Message> parts)
    {
        if (!replyToken.IsOwnedBy(_replyTokenOwner))
            throw new ZlinkSubmitException(SubmitResult.InvalidArgument,
                (int)ErrorCode.EInval);
        Completion.Reply(routingId, replyToken, parts);
    }

    private void SendPartsWithFlags(IReadOnlyList<Message> parts, int flags)
    {
        Message[]? copied = null;
        SendCore(NativeMessageParts.AsSpan(parts, ref copied), flags,
            nameof(parts));
    }

    private SendResult SendPartsNoWaitResult(IReadOnlyList<Message> parts)
    {
        Message[]? copied = null;
        return SendNoWaitResultCore(NativeMessageParts.AsSpan(parts, ref copied),
            nameof(parts));
    }

    private void SendRoutedSingleWithFlags(string routingId, Message message,
        int flags)
    {
        var encoded = RoutingIdCodec.FromPublicString(routingId,
            nameof(routingId));
        var nativeRoutingId = NativeHelpers.WriteRoutingId(encoded);
        SendSingleCore(ref nativeRoutingId, message, flags);
    }

    private SendResult SendRoutedSingleNoWaitResult(string routingId,
        Message message)
    {
        var encoded = RoutingIdCodec.FromPublicString(routingId,
            nameof(routingId));
        var nativeRoutingId = NativeHelpers.WriteRoutingId(encoded);
        return SendSingleNoWaitResultCore(ref nativeRoutingId, message);
    }

    private void SendRoutedPartsWithFlags(string routingId,
        IReadOnlyList<Message> parts, int flags)
    {
        var encoded = RoutingIdCodec.FromPublicString(routingId,
            nameof(routingId));
        var nativeRoutingId = NativeHelpers.WriteRoutingId(encoded);

        Message[]? copied = null;
        SendCore(ref nativeRoutingId, NativeMessageParts.AsSpan(parts, ref copied),
            flags, nameof(parts));
    }

    private SendResult SendRoutedPartsNoWaitResult(string routingId,
        IReadOnlyList<Message> parts)
    {
        var encoded = RoutingIdCodec.FromPublicString(routingId,
            nameof(routingId));
        var nativeRoutingId = NativeHelpers.WriteRoutingId(encoded);

        Message[]? copied = null;
        return SendNoWaitResultCore(ref nativeRoutingId,
            NativeMessageParts.AsSpan(parts, ref copied), nameof(parts));
    }

    private void PublishPartsWithFlags(string topic, IReadOnlyList<Message> parts,
        int flags)
    {
        Message[]? copied = null;
        PublishCore(topic, NativeMessageParts.AsSpan(parts, ref copied), flags,
            nameof(parts));
    }

    private SendResult PublishNoWaitParts(string topic,
        IReadOnlyList<Message> parts)
    {
        Message[]? copied = null;
        return PublishNoWaitCore(topic, NativeMessageParts.AsSpan(parts, ref copied),
            nameof(parts));
    }

    private static RoutingId ParsePublicRoutingId(string value)
    {
        const string hexPrefix = "hex:";
        return value.StartsWith(hexPrefix, StringComparison.Ordinal)
            ? RoutingId.FromHex(value.Substring(hexPrefix.Length))
            : RoutingId.From(value);
    }

    internal static bool InterpretNoWaitResult(SendResult result)
    {
        return result switch
        {
            SendResult.Sent => true,
            SendResult.Backpressured => false,
            _ => throw CreateNoWaitSendException(result)
        };
    }

    private static ZlinkSubmitException CreateNoWaitSendException(
        SendResult result)
    {
        return result switch
        {
            SendResult.Backpressured =>
                ZlinkException.CreateSubmitException((int)ErrorCode.EAgain),
            SendResult.NotReady =>
                ZlinkException.CreateSubmitException((int)ErrorCode.ENotConn),
            _ => ZlinkException.CreateSubmitException((int)ErrorCode.EInval)
        };
    }

    private void EnsureSupports(string memberName,
        SocketTypePolicy.SocketCapability capability)
    {
        _policy.EnsureSupportsMember(memberName, capability);
    }

    private void EnsureOptionSupported(SocketOption option)
    {
        _policy.EnsureOptionSupported(option);
    }
}
