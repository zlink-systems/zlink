// SPDX-License-Identifier: MPL-2.0

using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink.Runtime.Sockets.Internal;

internal sealed partial class SocketKernel : IDisposable
{
    private const int TopicBufferSize = 4096;
    private const int DontWaitFlag = 1;
    private readonly SocketCallbackRegistry _callbacks = new();
    private readonly object _completionControlRegistrationSync = new();
    private readonly object _streamModeGate = new();

    private readonly SocketHandle _handle;
    private readonly SocketOptionAccessor _options;
    private readonly SocketTypePolicy _policy;
    private string? _publishTopicCacheKey;
    private byte[]? _publishTopicCacheUtf8;
    private bool _streamAttached;
    private int _streamReceiveMode;

    public SocketKernel(Context context, SocketType type)
    {
        _handle = new SocketHandle(context, type);
        _options = new SocketOptionAccessor(_handle);
        _policy = new SocketTypePolicy(type);
    }

    public SocketKernel(IntPtr handle, bool own)
    {
        _handle = new SocketHandle(handle, own);
        _options = new SocketOptionAccessor(_handle);
        _policy = new SocketTypePolicy(
            SocketOptionAccessor.ReadSocketType(_handle.DangerousGetHandle()));
    }

    public IntPtr Handle => _handle.DangerousGetHandle();
    public SocketType Type => _policy.SocketType;

    public bool ReceiveSubscriptionEvent(SubscriptionEvent result,
        RecvFlags flags = RecvFlags.None)
    {
        EnsureSupports(nameof(ReceiveSubscriptionEvent),
            SocketTypePolicy.SocketCapability.SubscriptionEvents);
        if (result == null)
            throw new ArgumentNullException(nameof(result));
        return ReceiveSubscriptionEventInto(result, (int)flags);
    }

    private void SendReplyCore(RoutingId routingId,
        ulong requestSeq, IReadOnlyList<Message> parts)
    {
        var nativeRoutingId = routingId.ToNative();
        var cloned = RequestReplySupport.CloneParts(parts);
        try
        {
            RequestReplySupport.SubmitClonedParts(cloned,
                (ref ZlinkMsg nativePart,
                    NativeMethods.ZlinkPartFlag partFlag) =>
                    NativeMethods.zlink_router_reply_part(Handle,
                        ref nativeRoutingId, requestSeq, ref nativePart,
                        partFlag));
        }
        catch
        {
            RequestReplySupport.DisposeParts(cloned);
            throw;
        }
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

    internal static bool TrySendOrThrow(SendResult result)
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
