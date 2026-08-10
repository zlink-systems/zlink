// SPDX-License-Identifier: MPL-2.0

using System.Runtime.CompilerServices;
using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink.Runtime.Sockets.Internal;

internal sealed partial class SocketKernel
{
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private void SendSingleCore(Message message, int flags)
    {
        lock (SubmitGate)
        {
            var submitter = new SendSingleSubmitter(Handle, flags);
            var rc = SinglePartSubmit.Submit(message, ref submitter);
            if (rc != 0)
                throw ZlinkException.CreateSubmitException((SubmitResult)rc);
        }
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private SendResult SendSingleResultCore(Message message, int flags)
    {
        lock (SubmitGate)
        {
            var submitter = new SendSingleSubmitter(Handle, flags);
            return MapSendResult(SinglePartSubmit.Submit(message, ref submitter));
        }
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private SendResult SendSingleNoWaitResultCore(Message message)
    {
        lock (SubmitGate)
        {
            var submitter = new SendSingleNoWaitSubmitter(Handle);
            return MapSendResult(SinglePartSubmit.Submit(message, ref submitter));
        }
    }

    private void PublishSingleCore(string topic, Message message,
        int flags)
    {
        lock (SubmitGate)
            PublishSingleCoreUnlocked(GetPublishTopicUtf8Unlocked(topic),
                message, flags);
    }

    private unsafe void PublishSingleCore(byte[] topicUtf8, Message message,
        int flags)
    {
        lock (SubmitGate)
            PublishSingleCoreUnlocked(topicUtf8, message, flags);
    }

    private unsafe void PublishSingleCoreUnlocked(byte[] topicUtf8,
        Message message, int flags)
    {
        fixed (byte* topicPtr = topicUtf8)
        {
            var submitter = new PublishSingleSubmitter(Handle, topicPtr, flags);
            var rc = SinglePartSubmit.Submit(message, ref submitter);
            if (rc != 0)
                throw ZlinkException.CreateSubmitException((SubmitResult)rc);
        }
    }

    private SendResult PublishNoWaitSingleCore(string topic, Message message)
    {
        lock (SubmitGate)
            return PublishNoWaitSingleCoreUnlocked(
                GetPublishTopicUtf8Unlocked(topic), message);
    }

    private unsafe SendResult PublishNoWaitSingleCore(byte[] topicUtf8,
        Message message)
    {
        lock (SubmitGate)
            return PublishNoWaitSingleCoreUnlocked(topicUtf8, message);
    }

    private unsafe SendResult PublishNoWaitSingleCoreUnlocked(
        byte[] topicUtf8, Message message)
    {
        fixed (byte* topicPtr = topicUtf8)
        {
            var submitter = new PublishSingleSubmitter(Handle, topicPtr,
                DontWaitFlag);
            return MapSendResult(SinglePartSubmit.Submit(message, ref submitter));
        }
    }

    private byte[] GetPublishTopicUtf8Unlocked(string topic)
    {
        var cached = _publishTopicCacheUtf8;
        var cachedKey = _publishTopicCacheKey;
        if (cached != null
            && (ReferenceEquals(cachedKey, topic)
                || string.Equals(cachedKey, topic, StringComparison.Ordinal)))
            return cached;

        var encoded = PublishTopicEncoding.GetNullTerminatedUtf8(topic);
        _publishTopicCacheKey = topic;
        _publishTopicCacheUtf8 = encoded;
        return encoded;
    }

    private byte[] GetValidatedPublishTopicUtf8(string topic, string paramName)
    {
        lock (SubmitGate)
            return GetValidatedPublishTopicUtf8Unlocked(topic, paramName);
    }

    private byte[] GetValidatedPublishTopicUtf8Unlocked(string topic,
        string paramName)
    {
        var cached = _publishTopicCacheUtf8;
        var cachedKey = _publishTopicCacheKey;
        if (cached != null
            && (ReferenceEquals(cachedKey, topic)
                || string.Equals(cachedKey, topic, StringComparison.Ordinal)))
            return cached;

        BoundaryValidation.ValidateTopicOrFilterUtf8(topic, paramName);
        var encoded = PublishTopicEncoding.GetNullTerminatedUtf8(topic);
        _publishTopicCacheKey = topic;
        _publishTopicCacheUtf8 = encoded;
        return encoded;
    }

    private unsafe void SendSingleCore(ref ZlinkRoutingId routingId,
        Message message, int flags)
    {
        lock (SubmitGate)
        {
            fixed (ZlinkRoutingId* routingIdPtr = &routingId)
            {
                var submitter = new RoutedSendSingleSubmitter(Handle, routingIdPtr,
                    flags);
                var rc = SinglePartSubmit.Submit(message, ref submitter);
                if (rc != 0)
                    throw ZlinkException.CreateSubmitException((SubmitResult)rc);
            }
        }
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private unsafe SendResult SendSingleResultCore(ref ZlinkRoutingId routingId,
        Message message, int flags)
    {
        lock (SubmitGate)
        {
            fixed (ZlinkRoutingId* routingIdPtr = &routingId)
            {
                var submitter = new RoutedSendSingleSubmitter(Handle, routingIdPtr,
                    flags);
                return MapSendResult(SinglePartSubmit.Submit(message, ref submitter));
            }
        }
    }

    private unsafe SendResult SendSingleNoWaitResultCore(
        ref ZlinkRoutingId routingId, Message message)
    {
        lock (SubmitGate)
        {
            fixed (ZlinkRoutingId* routingIdPtr = &routingId)
            {
                var submitter = new RoutedSendSingleNoWaitSubmitter(Handle,
                    routingIdPtr);
                return MapSendResult(SinglePartSubmit.Submit(message, ref submitter));
            }
        }
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private static SendResult MapSendResult(int rc)
    {
        if (rc == 0)
            return SendResult.Sent;
        var result = (SubmitResult)rc;
        var mappedResult = SendResultErrno.TryMap(result);
        if (mappedResult == null)
            throw ZlinkException.CreateSubmitException(result);
        return mappedResult.Value;
    }

    private readonly struct SendSingleSubmitter
        : INativeSinglePartSubmitter<SendSingleSubmitter>
    {
        private readonly IntPtr _handle;
        private readonly int _flags;

        internal SendSingleSubmitter(IntPtr handle, int flags)
        {
            _handle = handle;
            _flags = flags;
        }

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public static int Submit(ref SendSingleSubmitter submitter,
            ref ZlinkMsg nativePart)
        {
            return NativeMethods.zlink_send_part(submitter._handle,
                ref nativePart, submitter._flags,
                NativeMethods.ZlinkPartFlag.Final);
        }
    }

    private readonly struct SendSingleNoWaitSubmitter
        : INativeSinglePartSubmitter<SendSingleNoWaitSubmitter>
    {
        private readonly IntPtr _handle;

        internal SendSingleNoWaitSubmitter(IntPtr handle)
        {
            _handle = handle;
        }

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public static int Submit(ref SendSingleNoWaitSubmitter submitter,
            ref ZlinkMsg nativePart)
        {
            // DONT_WAIT-only critical variant: contractually non-blocking.
            return NativeMethods.zlink_send_part_nowait(submitter._handle,
                ref nativePart, DontWaitFlag,
                NativeMethods.ZlinkPartFlag.Final);
        }
    }

    private unsafe readonly struct PublishSingleSubmitter
        : INativeSinglePartSubmitter<PublishSingleSubmitter>
    {
        private readonly IntPtr _handle;
        private readonly byte* _topicPtr;
        private readonly int _flags;

        internal PublishSingleSubmitter(IntPtr handle, byte* topicPtr,
            int flags)
        {
            _handle = handle;
            _topicPtr = topicPtr;
            _flags = flags;
        }

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public static int Submit(ref PublishSingleSubmitter submitter,
            ref ZlinkMsg nativePart)
        {
            return NativeMethods.zlink_publish_part_utf8(submitter._handle,
                submitter._topicPtr, ref nativePart, submitter._flags,
                NativeMethods.ZlinkPartFlag.Final);
        }
    }

    private unsafe readonly struct RoutedSendSingleSubmitter
        : INativeSinglePartSubmitter<RoutedSendSingleSubmitter>
    {
        private readonly IntPtr _handle;
        private readonly ZlinkRoutingId* _routingId;
        private readonly int _flags;

        internal RoutedSendSingleSubmitter(IntPtr handle,
            ZlinkRoutingId* routingId, int flags)
        {
            _handle = handle;
            _routingId = routingId;
            _flags = flags;
        }

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public static int Submit(ref RoutedSendSingleSubmitter submitter,
            ref ZlinkMsg nativePart)
        {
            return (submitter._flags & DontWaitFlag) != 0
                ? NativeMethods.zlink_send_part_rid_nowait(submitter._handle,
                    ref *submitter._routingId, ref nativePart, submitter._flags,
                    NativeMethods.ZlinkPartFlag.Final)
                : NativeMethods.zlink_send_part_rid(submitter._handle,
                    ref *submitter._routingId, ref nativePart,
                    submitter._flags, NativeMethods.ZlinkPartFlag.Final);
        }
    }

    private unsafe readonly struct RoutedSendSingleNoWaitSubmitter
        : INativeSinglePartSubmitter<RoutedSendSingleNoWaitSubmitter>
    {
        private readonly IntPtr _handle;
        private readonly ZlinkRoutingId* _routingId;

        internal RoutedSendSingleNoWaitSubmitter(IntPtr handle,
            ZlinkRoutingId* routingId)
        {
            _handle = handle;
            _routingId = routingId;
        }

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public static int Submit(ref RoutedSendSingleNoWaitSubmitter submitter,
            ref ZlinkMsg nativePart)
        {
            return NativeMethods.zlink_send_part_rid_nowait(submitter._handle,
                ref *submitter._routingId, ref nativePart, DontWaitFlag,
                NativeMethods.ZlinkPartFlag.Final);
        }
    }
}
