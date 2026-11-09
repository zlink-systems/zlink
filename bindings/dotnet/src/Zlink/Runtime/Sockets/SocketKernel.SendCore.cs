// SPDX-License-Identifier: MPL-2.0

using System.Runtime.CompilerServices;
using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink.Runtime.Sockets.Internal;

internal sealed partial class SocketKernel
{
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private void SendSingleCore(Message message, int flags)
    {
        var submitter = new SendSingleSubmitter(_handle, flags);
        var rc = SinglePartSubmit.Submit(message, ref submitter);
        if (rc != 0)
            throw ZlinkException.CreateSubmitException((SubmitResult)rc);
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private SendResult SendSingleResultCore(Message message, int flags)
    {
        var submitter = new SendSingleSubmitter(_handle, flags);
        return MapSendResult(SinglePartSubmit.Submit(message, ref submitter));
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private SendResult SendSingleNoWaitResultCore(Message message)
    {
        var submitter = new SendSingleNoWaitSubmitter(_handle);
        return MapSendResult(SinglePartSubmit.Submit(message, ref submitter));
    }

    private void PublishSingleCore(string topic, Message message,
        int flags)
    {
        PublishSingleCore(PublishTopicEncoding.GetNullTerminatedUtf8(topic),
            message, flags);
    }

    private unsafe void PublishSingleCore(byte[] topicUtf8, Message message,
        int flags)
    {
        fixed (byte* topicPtr = topicUtf8)
        {
            var submitter = new PublishSingleSubmitter(_handle, topicPtr, flags);
            var rc = SinglePartSubmit.Submit(message, ref submitter);
            if (rc != 0)
                throw ZlinkException.CreateSubmitException((SubmitResult)rc);
        }
    }

    private SendResult PublishNoWaitSingleCore(string topic, Message message)
    {
        return PublishNoWaitSingleCore(
            PublishTopicEncoding.GetNullTerminatedUtf8(topic), message);
    }

    private unsafe SendResult PublishNoWaitSingleCore(byte[] topicUtf8,
        Message message)
    {
        fixed (byte* topicPtr = topicUtf8)
        {
            var submitter = new PublishSingleSubmitter(_handle, topicPtr,
                DontWaitFlag);
            return MapSendResult(SinglePartSubmit.Submit(message, ref submitter));
        }
    }

    private byte[] GetValidatedPublishTopicUtf8(string topic, string paramName)
    {
        BoundaryValidation.ValidateTopicOrFilterUtf8(topic, paramName);
        return PublishTopicEncoding.GetNullTerminatedUtf8(topic);
    }

    private unsafe void SendSingleCore(ref ZlinkRoutingId routingId,
        Message message, int flags)
    {
        fixed (ZlinkRoutingId* routingIdPtr = &routingId)
        {
            var submitter = new TargetedSendSingleSubmitter(_handle, routingIdPtr,
                flags);
            var rc = SinglePartSubmit.Submit(message, ref submitter);
            if (rc != 0)
                throw ZlinkException.CreateSubmitException((SubmitResult)rc);
        }
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private unsafe SendResult SendSingleResultCore(ref ZlinkRoutingId routingId,
        Message message, int flags)
    {
        fixed (ZlinkRoutingId* routingIdPtr = &routingId)
        {
            var submitter = new TargetedSendSingleSubmitter(_handle, routingIdPtr,
                flags);
            return MapSendResult(SinglePartSubmit.Submit(message, ref submitter));
        }
    }

    private unsafe SendResult SendSingleNoWaitResultCore(
        ref ZlinkRoutingId routingId, Message message)
    {
        fixed (ZlinkRoutingId* routingIdPtr = &routingId)
        {
            var submitter = new TargetedSendSingleNoWaitSubmitter(_handle,
                routingIdPtr);
            return MapSendResult(SinglePartSubmit.Submit(message, ref submitter));
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

    private unsafe readonly struct SendSingleSubmitter
        : INativeSinglePartSubmitter<SendSingleSubmitter>
    {
        private readonly SocketHandle _handle;
        private readonly int _flags;

        internal SendSingleSubmitter(SocketHandle handle, int flags)
        {
            _handle = handle;
            _flags = flags;
        }

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public static int Submit(ref SendSingleSubmitter submitter,
            ref ZlinkMsg nativePart)
        {
            using var call = submitter._handle.Reserve(submit: true);
            return NativeMethods.zlink_send(call.Handle,
                ref nativePart, 1, submitter._flags);
        }
    }

    private unsafe readonly struct SendSingleNoWaitSubmitter
        : INativeSinglePartSubmitter<SendSingleNoWaitSubmitter>
    {
        private readonly SocketHandle _handle;

        internal SendSingleNoWaitSubmitter(SocketHandle handle)
        {
            _handle = handle;
        }

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public static int Submit(ref SendSingleNoWaitSubmitter submitter,
            ref ZlinkMsg nativePart)
        {
            using var call = submitter._handle.Reserve(submit: true);
            // DONT_WAIT-only critical variant: contractually non-blocking.
            return NativeMethods.zlink_send_nowait(call.Handle,
                ref nativePart, 1, DontWaitFlag);
        }
    }

    private unsafe readonly struct PublishSingleSubmitter
        : INativeSinglePartSubmitter<PublishSingleSubmitter>
    {
        private readonly SocketHandle _handle;
        private readonly byte* _topicPtr;
        private readonly int _flags;

        internal PublishSingleSubmitter(SocketHandle handle, byte* topicPtr,
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
            using var call = submitter._handle.Reserve(submit: true);
            return NativeMethods.zlink_publish_utf8(call.Handle,
                submitter._topicPtr, ref nativePart, 1, submitter._flags);
        }
    }

    private unsafe readonly struct TargetedSendSingleSubmitter
        : INativeSinglePartSubmitter<TargetedSendSingleSubmitter>
    {
        private readonly SocketHandle _handle;
        private readonly ZlinkRoutingId* _routingId;
        private readonly int _flags;

        internal TargetedSendSingleSubmitter(SocketHandle handle,
            ZlinkRoutingId* routingId, int flags)
        {
            _handle = handle;
            _routingId = routingId;
            _flags = flags;
        }

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public static int Submit(ref TargetedSendSingleSubmitter submitter,
            ref ZlinkMsg nativePart)
        {
            using var call = submitter._handle.Reserve(submit: true);
            return (submitter._flags & DontWaitFlag) != 0
                ? NativeMethods.zlink_send_rid_nowait(call.Handle,
                    ref *submitter._routingId, ref nativePart, 1, submitter._flags)
                : NativeMethods.zlink_send_rid(call.Handle,
                    ref *submitter._routingId, ref nativePart, 1,
                    submitter._flags);
        }
    }

    private unsafe readonly struct TargetedSendSingleNoWaitSubmitter
        : INativeSinglePartSubmitter<TargetedSendSingleNoWaitSubmitter>
    {
        private readonly SocketHandle _handle;
        private readonly ZlinkRoutingId* _routingId;

        internal TargetedSendSingleNoWaitSubmitter(SocketHandle handle,
            ZlinkRoutingId* routingId)
        {
            _handle = handle;
            _routingId = routingId;
        }

        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public static int Submit(ref TargetedSendSingleNoWaitSubmitter submitter,
            ref ZlinkMsg nativePart)
        {
            using var call = submitter._handle.Reserve(submit: true);
            return NativeMethods.zlink_send_rid_nowait(call.Handle,
                ref *submitter._routingId, ref nativePart, 1, DontWaitFlag);
        }
    }
}
