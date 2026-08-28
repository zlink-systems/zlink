// SPDX-License-Identifier: MPL-2.0

using System.Runtime.CompilerServices;
using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink.Runtime.Sockets.Internal;

internal sealed partial class SocketKernel : IDisposable
{
    public void Send(Message message, SendFlags flags = SendFlags.None)
    {
        EnsureSupports(nameof(Send), SocketTypePolicy.SocketCapability.MessageSend);
        SendMessageUnchecked(message, flags);
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    internal void SendMessageUnchecked(Message message,
        SendFlags flags = SendFlags.None)
    {
        if (message == null)
            throw new ArgumentNullException(nameof(message));
        if (((int)flags & DontWaitFlag) != 0)
        {
            var result = SendSingleNoWaitResultCore(message);
            if (result != SendResult.Sent)
                throw CreateNoWaitSendException(result);
            return;
        }

        SendSingleCore(message, (int)flags);
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    internal SendResult SendMessageResultUnchecked(Message message, int flags)
    {
        if (message == null)
            throw new ArgumentNullException(nameof(message));
        if ((flags & DontWaitFlag) != 0)
            return SendSingleNoWaitResultCore(message);
        return SendSingleResultCore(message, flags);
    }

    public SendResult SendNoWaitResult(Message message)
    {
        EnsureSupports(nameof(SendNoWaitResult),
            SocketTypePolicy.SocketCapability.MessageSend);
        if (message == null)
            throw new ArgumentNullException(nameof(message));
        return SendSingleNoWaitResultCore(message);
    }

    public void Send(IReadOnlyList<Message> parts, SendFlags flags = SendFlags.None)
    {
        EnsureSupports(nameof(Send), SocketTypePolicy.SocketCapability.MessageSend);
        RequestReplySupport.EnsureParts(parts, nameof(parts));

        Message[]? copied = null;
        SendCore(NativeMessageParts.AsSpan(parts, ref copied), (int)flags,
            nameof(parts));
    }

    public SendResult SendNoWaitResult(IReadOnlyList<Message> parts)
    {
        EnsureSupports(nameof(SendNoWaitResult),
            SocketTypePolicy.SocketCapability.MessageSend);
        RequestReplySupport.EnsureParts(parts, nameof(parts));
        return SendPartsNoWaitResult(parts);
    }

    public void Send(string routingId, Message message, SendFlags flags = SendFlags.None)
    {
        EnsureSupports(nameof(Send), SocketTypePolicy.SocketCapability.RoutedSend);
        if (routingId == null)
            throw new ArgumentNullException(nameof(routingId));
        if (message == null)
            throw new ArgumentNullException(nameof(message));

        var encoded = RoutingIdCodec.FromPublicString(routingId,
            nameof(routingId));
        var nativeRoutingId = NativeHelpers.WriteRoutingId(encoded);
        if (((int)flags & DontWaitFlag) != 0)
        {
            var result = SendSingleNoWaitResultCore(ref nativeRoutingId,
                message);
            if (result != SendResult.Sent)
                throw CreateNoWaitSendException(result);
            return;
        }

        SendSingleCore(ref nativeRoutingId, message, (int)flags);
    }

    public void Send(RoutingId routingId, Message message,
        SendFlags flags = SendFlags.None)
    {
        EnsureSupports(nameof(Send), SocketTypePolicy.SocketCapability.RoutedSend);
        SendRoutedMessageUnchecked(routingId, message, flags);
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    internal void SendRoutedMessageUnchecked(RoutingId routingId,
        Message message, SendFlags flags = SendFlags.None)
    {
        if (message == null)
            throw new ArgumentNullException(nameof(message));
        ZlinkRoutingId fallback = default;
        ref var nativeRoutingId =
            ref routingId.ToNativeRef(ref fallback);
        if (((int)flags & DontWaitFlag) != 0)
        {
            var result = SendSingleNoWaitResultCore(ref nativeRoutingId,
                message);
            if (result != SendResult.Sent)
                throw CreateNoWaitSendException(result);
            return;
        }

        SendSingleCore(ref nativeRoutingId, message, (int)flags);
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    internal SendResult SendRoutedMessageResultUnchecked(RoutingId routingId,
        Message message, int flags)
    {
        if (message == null)
            throw new ArgumentNullException(nameof(message));
        // Take ref to the cached ZlinkRoutingId when available — avoids the
        // 256-byte struct copy that ToNative() does in the routed send hot
        // path (51 MB/s memcpy bandwidth at 200K msg/sec).
        ZlinkRoutingId fallback = default;
        ref var nativeRoutingId =
            ref routingId.ToNativeRef(ref fallback);
        return SendSingleResultCore(ref nativeRoutingId, message, flags);
    }

    public void Send(uint routingId, Message message,
        SendFlags flags = SendFlags.None)
    {
        EnsureSupports(nameof(Send), SocketTypePolicy.SocketCapability.RoutedSend);
        if (message == null)
            throw new ArgumentNullException(nameof(message));
        var nativeRoutingId = RoutingIdCodec.ToNative(routingId);
        if (((int)flags & DontWaitFlag) != 0)
        {
            var result = SendSingleNoWaitResultCore(ref nativeRoutingId,
                message);
            if (result != SendResult.Sent)
                throw CreateNoWaitSendException(result);
            return;
        }

        SendSingleCore(ref nativeRoutingId, message, (int)flags);
    }

    public SendResult SendNoWaitResult(string routingId, Message message)
    {
        EnsureSupports(nameof(SendNoWaitResult),
            SocketTypePolicy.SocketCapability.RoutedSend);
        if (routingId == null)
            throw new ArgumentNullException(nameof(routingId));
        if (message == null)
            throw new ArgumentNullException(nameof(message));
        return SendRoutedSingleNoWaitResult(routingId, message);
    }

    public SendResult SendNoWaitResult(RoutingId routingId, Message message)
    {
        EnsureSupports(nameof(SendNoWaitResult),
            SocketTypePolicy.SocketCapability.RoutedSend);
        if (message == null)
            throw new ArgumentNullException(nameof(message));
        ZlinkRoutingId fallback = default;
        ref var nativeRoutingId =
            ref routingId.ToNativeRef(ref fallback);
        return SendSingleNoWaitResultCore(ref nativeRoutingId, message);
    }

    public void Send(string routingId, IReadOnlyList<Message> parts,
        SendFlags flags = SendFlags.None)
    {
        EnsureSupports(nameof(Send), SocketTypePolicy.SocketCapability.RoutedSend);
        if (routingId == null)
            throw new ArgumentNullException(nameof(routingId));
        RequestReplySupport.EnsureParts(parts, nameof(parts));

        var encoded = RoutingIdCodec.FromPublicString(routingId,
            nameof(routingId));
        var nativeRoutingId = NativeHelpers.WriteRoutingId(encoded);

        Message[]? copied = null;
        SendCore(ref nativeRoutingId, NativeMessageParts.AsSpan(parts, ref copied),
            (int)flags, nameof(parts));
    }

    public void Send(RoutingId routingId, IReadOnlyList<Message> parts,
        SendFlags flags = SendFlags.None)
    {
        EnsureSupports(nameof(Send), SocketTypePolicy.SocketCapability.RoutedSend);
        RequestReplySupport.EnsureParts(parts, nameof(parts));

        ZlinkRoutingId fallback = default;
        ref var nativeRoutingId =
            ref routingId.ToNativeRef(ref fallback);

        Message[]? copied = null;
        SendCore(ref nativeRoutingId, NativeMessageParts.AsSpan(parts, ref copied),
            (int)flags, nameof(parts));
    }

    public SendResult SendNoWaitResult(string routingId, IReadOnlyList<Message> parts)
    {
        EnsureSupports(nameof(SendNoWaitResult),
            SocketTypePolicy.SocketCapability.RoutedSend);
        if (routingId == null)
            throw new ArgumentNullException(nameof(routingId));
        RequestReplySupport.EnsureParts(parts, nameof(parts));
        return SendRoutedPartsNoWaitResult(routingId, parts);
    }

    public SendResult SendNoWaitResult(RoutingId routingId, IReadOnlyList<Message> parts)
    {
        EnsureSupports(nameof(SendNoWaitResult),
            SocketTypePolicy.SocketCapability.RoutedSend);
        RequestReplySupport.EnsureParts(parts, nameof(parts));

        ZlinkRoutingId fallback = default;
        ref var nativeRoutingId =
            ref routingId.ToNativeRef(ref fallback);

        Message[]? copied = null;
        return SendNoWaitResultCore(ref nativeRoutingId,
            NativeMessageParts.AsSpan(parts, ref copied), nameof(parts));
    }

    public void Publish(string topic, Message message, SendFlags flags = SendFlags.None)
    {
        EnsureSupports(nameof(Publish), SocketTypePolicy.SocketCapability.Publish);
        if (message == null)
            throw new ArgumentNullException(nameof(message));
        var topicUtf8 = GetValidatedPublishTopicUtf8(topic, nameof(topic));
        PublishSingleCore(topicUtf8, message, (int)flags);
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    internal void PublishMessageUnchecked(string topic, Message message,
        SendFlags flags = SendFlags.None)
    {
        var topicUtf8 = GetValidatedPublishTopicUtf8(topic, nameof(topic));
        if (message == null)
            throw new ArgumentNullException(nameof(message));
        PublishSingleCore(topicUtf8, message, (int)flags);
    }

    internal SendResult PublishNoWaitResult(string topic, Message message)
    {
        EnsureSupports(nameof(PublishNoWaitResult),
            SocketTypePolicy.SocketCapability.Publish);
        if (message == null)
            throw new ArgumentNullException(nameof(message));
        var topicUtf8 = GetValidatedPublishTopicUtf8(topic, nameof(topic));
        return PublishNoWaitSingleCore(topicUtf8, message);
    }

    public void Publish(string topic, IReadOnlyList<Message> parts,
        SendFlags flags = SendFlags.None)
    {
        EnsureSupports(nameof(Publish), SocketTypePolicy.SocketCapability.Publish);
        BoundaryValidation.ValidateTopicOrFilterUtf8(topic, nameof(topic));
        RequestReplySupport.EnsureParts(parts, nameof(parts));

        Message[]? copied = null;
        PublishCore(topic, NativeMessageParts.AsSpan(parts, ref copied),
            (int)flags, nameof(parts));
    }

    internal SendResult PublishNoWaitResult(string topic, IReadOnlyList<Message> parts)
    {
        EnsureSupports(nameof(PublishNoWaitResult),
            SocketTypePolicy.SocketCapability.Publish);
        BoundaryValidation.ValidateTopicOrFilterUtf8(topic, nameof(topic));
        RequestReplySupport.EnsureParts(parts, nameof(parts));
        return PublishNoWaitParts(topic, parts);
    }

    public void SetSubscription(string topicOrPattern)
    {
        EnsureSupports(nameof(SetSubscription),
            SocketTypePolicy.SocketCapability.SubscriptionControl);
        BoundaryValidation.ValidateTopicOrFilterUtf8(topicOrPattern,
            nameof(topicOrPattern));

        var rc = NativeMethods.zlink_set_subscription(Handle, topicOrPattern);
        ZlinkException.ThrowConfigIfError(rc);
    }

    public void UnsetSubscription(string topicOrPattern)
    {
        EnsureSupports(nameof(UnsetSubscription),
            SocketTypePolicy.SocketCapability.SubscriptionControl);
        BoundaryValidation.ValidateTopicOrFilterUtf8(topicOrPattern,
            nameof(topicOrPattern));

        var rc = NativeMethods.zlink_unset_subscription(Handle, topicOrPattern);
        ZlinkException.ThrowConfigIfError(rc);
    }
}
