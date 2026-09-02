// SPDX-License-Identifier: MPL-2.0

using System.Buffers;
using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink.Runtime.Sockets.Internal;

internal sealed partial class SocketKernel
{
    private void SendCore(ReadOnlySpan<Message> parts, int flags,
        string paramName)
    {
        ZlinkRoutingId unused = default;
        SubmitMultipartCore(MultipartSubmitKind.Send, null, ref unused, parts,
            flags, paramName, false);
    }

    private SendResult SendNoWaitResultCore(ReadOnlySpan<Message> parts,
        string paramName)
    {
        ZlinkRoutingId unused = default;
        return SubmitMultipartCore(MultipartSubmitKind.Send, null, ref unused,
            parts, DontWaitFlag, paramName, true);
    }

    private void PublishCore(string topic, ReadOnlySpan<Message> parts,
        int flags, string paramName)
    {
        ZlinkRoutingId unused = default;
        SubmitMultipartCore(MultipartSubmitKind.Publish, topic, ref unused,
            parts, flags, paramName, false);
    }

    private SendResult PublishNoWaitCore(string topic,
        ReadOnlySpan<Message> parts, string paramName)
    {
        ZlinkRoutingId unused = default;
        return SubmitMultipartCore(MultipartSubmitKind.Publish, topic,
            ref unused, parts, DontWaitFlag, paramName, true);
    }

    private void SendCore(ref ZlinkRoutingId routingId,
        ReadOnlySpan<Message> parts, int flags, string paramName)
    {
        SubmitMultipartCore(MultipartSubmitKind.TargetedSend, null, ref routingId,
            parts, flags, paramName, false);
    }

    private SendResult SendNoWaitResultCore(ref ZlinkRoutingId routingId,
        ReadOnlySpan<Message> parts, string paramName)
    {
        return SubmitMultipartCore(MultipartSubmitKind.TargetedSend, null,
            ref routingId, parts, DontWaitFlag, paramName,
            true);
    }

    private unsafe SendResult SubmitMultipartCore(MultipartSubmitKind kind,
        string? topic, ref ZlinkRoutingId routingId, ReadOnlySpan<Message> parts,
        int flags, string paramName, bool mapNoWaitResult)
    {
        if (parts.Length == 1)
            return SubmitSinglePartCore(
                kind,
                topic,
                ref routingId,
                parts[0] ?? throw new ArgumentException(
                    "Parts must not contain null messages.", paramName),
                flags,
                mapNoWaitResult);

        ZlinkMsg[]? rented = null;
        var nativeParts = parts.Length <= NativeMessageParts.StackPartLimit
            ? stackalloc ZlinkMsg[NativeMessageParts.StackPartLimit]
            : rented = ArrayPool<ZlinkMsg>.Shared.Rent(parts.Length);
        nativeParts = nativeParts.Slice(0, parts.Length);

        var built = 0;
        var consumed = 0;
        try
        {
            NativeMessageParts.MoveToNative(parts, nativeParts, paramName,
                ref built);
            var publishTopicUtf8 = kind == MultipartSubmitKind.Publish
                ? PublishTopicEncoding.GetNullTerminatedUtf8(topic!)
                : null;
            for (var i = 0; i < built; i++)
            {
                var partFlag = i + 1 < built
                    ? NativeMethods.ZlinkPartFlag.More
                    : NativeMethods.ZlinkPartFlag.Final;
                int rc;
                if (kind == MultipartSubmitKind.Publish)
                    fixed (byte* topicPtr = publishTopicUtf8)
                    {
                        rc = NativeMethods.zlink_publish_part_utf8(Handle,
                            topicPtr, ref nativeParts[i], flags, partFlag);
                    }
                else
                    rc = kind switch
                    {
                        MultipartSubmitKind.Send => NativeMethods.zlink_send_part(
                            Handle, ref nativeParts[i], flags, partFlag),
                        MultipartSubmitKind.TargetedSend =>
                            NativeMethods.zlink_send_part_rid(Handle,
                                ref routingId, ref nativeParts[i], flags, partFlag),
                        _ => throw new InvalidOperationException()
                    };

                if (rc == 0)
                {
                    consumed = i + 1;
                    continue;
                }

                // STREAM is the sole synchronous send contract that retains
                // the submitted native part on EAGAIN. Every other result,
                // including EAGAIN on other socket families, consumes it.
                var retainCurrent = Type == SocketType.Stream
                    && rc == (int)SubmitResult.Backpressured;
                consumed = retainCurrent ? i : i + 1;

                if (mapNoWaitResult)
                {
                    var result = (SubmitResult)rc;
                    var sendResult = SendResultErrno.TryMap(result);
                    if (sendResult != null)
                    {
                        NativeMessageParts.RestoreManaged(parts, nativeParts,
                            consumed, built - consumed);
                        consumed = built;
                        return sendResult.Value;
                    }
                }

                throw ZlinkException.CreateSubmitException((SubmitResult)rc);
            }

            return SendResult.Sent;
        }
        catch
        {
            NativeMessageParts.RestoreManaged(parts, nativeParts, consumed,
                built - consumed);
            throw;
        }
        finally
        {
            if (rented != null)
                ArrayPool<ZlinkMsg>.Shared.Return(rented);
        }
    }

    private SendResult SubmitSinglePartCore(
        MultipartSubmitKind kind,
        string? topic,
        ref ZlinkRoutingId routingId,
        Message part,
        int flags,
        bool mapNoWaitResult)
    {
        return kind switch
        {
            MultipartSubmitKind.Send => mapNoWaitResult
                ? SendSingleResultCore(part, flags)
                : SubmitSingle(part, flags),
            MultipartSubmitKind.Publish => mapNoWaitResult
                ? PublishNoWaitSingleCore(topic!, part)
                : SubmitSinglePublish(topic!, part, flags),
            MultipartSubmitKind.TargetedSend => mapNoWaitResult
                ? SendSingleResultCore(ref routingId, part, flags)
                : SubmitSingle(ref routingId, part, flags),
            _ => throw new InvalidOperationException()
        };
    }

    private SendResult SubmitSingle(Message part, int flags)
    {
        SendSingleCore(part, flags);
        return SendResult.Sent;
    }

    private SendResult SubmitSingle(ref ZlinkRoutingId routingId, Message part,
        int flags)
    {
        SendSingleCore(ref routingId, part, flags);
        return SendResult.Sent;
    }

    private SendResult SubmitSinglePublish(string topic, Message part, int flags)
    {
        PublishSingleCore(topic, part, flags);
        return SendResult.Sent;
    }

    private enum MultipartSubmitKind
    {
        Send,
        Publish,
        TargetedSend
    }
}
