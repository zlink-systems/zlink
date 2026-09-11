// SPDX-License-Identifier: MPL-2.0

using System.Buffers;
using System.Text;
using Systems.Zlink.Runtime.Native;

namespace Systems.Zlink.Runtime.Sockets.Internal;

internal sealed partial class SocketKernel
{
    private bool ReceiveBasicParts(int flags,
        out Message? singlePart, out MultipartMessageCollection? parts,
        bool allowNoData = false)
    {
        return ReceiveParts(flags, false, null, out _, out _, out _,
            out singlePart, out parts, allowNoData);
    }

    private bool ReceiveRoutedParts(int flags,
        out RoutingIdSnapshot routingId, out ulong replyToken,
        out Message? singlePart, out MultipartMessageCollection? parts,
        bool allowNoData = false)
    {
        return ReceiveParts(flags, true, null, out routingId, out replyToken,
            out _, out singlePart, out parts, allowNoData);
    }

    private unsafe bool ReceiveParts(int flags, bool captureRoutingId,
        TopicMessage? subscription, out RoutingIdSnapshot routingId,
        out ulong replyToken, out int topicLength, out Message? singlePart,
        out MultipartMessageCollection? parts, bool allowNoData)
    {
        var nativeParts = ArrayPool<ZlinkMsg>.Shared.Rent(
            NativeMessageParts.StackPartLimit);
        var receivedParts = Array.Empty<Message>();
        var nativeCount = 0;
        var adoptedCount = 0;
        routingId = default;
        replyToken = 0;
        topicLength = 0;
        singlePart = null;
        parts = null;
        try
        {
            var topicBuffer = subscription?.GetWritableTopicBuffer(TopicBufferSize);
            while (true)
            {
                IntPtr sourceRoutingId;
                nuint count;
                nuint nativeTopicLength = 0;
                int rc;
                if (subscription != null)
                {
                    if ((flags & DontWaitFlag) != 0)
                    {
                        fixed (byte* topicId = topicBuffer)
                            rc = NativeMethods.zlink_subscribe_dont_wait(Handle,
                                out sourceRoutingId, topicId,
                                (nuint)topicBuffer!.Length, out nativeTopicLength,
                                ref nativeParts[0], (nuint)nativeParts.Length,
                                out count, flags);
                    }
                    else
                        rc = NativeMethods.zlink_subscribe(Handle,
                            out sourceRoutingId, topicBuffer!,
                            (nuint)topicBuffer!.Length, out nativeTopicLength,
                            ref nativeParts[0], (nuint)nativeParts.Length,
                            out count, flags);
                }
                else if (captureRoutingId && _policy.UsesRouterRoutedReceiveEnvelope)
                    rc = NativeMethods.zlink_router_recv(Handle,
                        out sourceRoutingId, out replyToken, ref nativeParts[0],
                        (nuint)nativeParts.Length, out count, flags);
                else
                    rc = NativeMethods.zlink_recv(Handle, out sourceRoutingId,
                        ref nativeParts[0], (nuint)nativeParts.Length, out count,
                        flags);

                if ((RecvResult)rc == RecvResult.BufferTooSmall)
                {
                    // Core preserves the entire record and writes only required
                    // lengths. None of the native slots are owned on this path.
                    var growParts = count > (nuint)nativeParts.Length;
                    var growTopic = subscription != null
                        && nativeTopicLength > (nuint)topicBuffer!.Length;
                    if (!growParts && !growTopic)
                        throw new ZlinkRecvException(RecvResult.InternalError);
                    if (growParts)
                    {
                        var expanded = ArrayPool<ZlinkMsg>.Shared.Rent(checked((int)count));
                        ArrayPool<ZlinkMsg>.Shared.Return(nativeParts);
                        nativeParts = expanded;
                    }
                    if (growTopic)
                        topicBuffer = subscription!.GetWritableTopicBuffer(
                            checked((int)nativeTopicLength));
                    continue;
                }
                if (rc != 0)
                {
                    if (allowNoData && (RecvResult)rc is RecvResult.NoData or RecvResult.Busy)
                        return false;
                    throw new ZlinkRecvException((RecvResult)rc,
                        NativeMethods.zlink_errno());
                }

                if (count == 0 || count > (nuint)nativeParts.Length)
                    throw new ZlinkRecvException(RecvResult.InternalError);
                nativeCount = (int)count;
                // Basic receive deliberately avoids allocating or copying RID metadata.
                if (captureRoutingId || subscription != null)
                    routingId = RoutingIdSnapshot.FromPointer(sourceRoutingId);
                topicLength = checked((int)nativeTopicLength);
                break;
            }

            if (nativeCount == 1)
            {
                if (subscription != null)
                {
                    singlePart = subscription.PrepareReusableSinglePart();
                    singlePart.ReplaceNativeOwned(ref nativeParts[0]);
                }
                else
                    singlePart = Message.AdoptNativeFromPool(ref nativeParts[0]);
                adoptedCount = 1;
                return true;
            }

            receivedParts = ArrayPool<Message>.Shared.Rent(nativeCount);
            for (; adoptedCount < nativeCount; adoptedCount++)
                receivedParts[adoptedCount] = Message.AdoptNativeFromPool(
                    ref nativeParts[adoptedCount]);
            parts = MultipartMessageCollection.FromMessages(receivedParts,
                nativeCount, rented: true);
            receivedParts = Array.Empty<Message>();
            return true;
        }
        catch
        {
            for (var i = 0; i < adoptedCount && i < receivedParts.Length; i++)
                receivedParts[i].Dispose();
            singlePart?.Dispose();
            throw;
        }
        finally
        {
            // Adoption clears the source slot; only the unadopted suffix still
            // belongs to this receive. Never close uninitialized capacity slots.
            if (adoptedCount < nativeCount)
                fixed (ZlinkMsg* remaining = &nativeParts[adoptedCount])
                    NativeMethods.zlink_multipart_close((IntPtr)remaining,
                        (nuint)(nativeCount - adoptedCount));
            ArrayPool<ZlinkMsg>.Shared.Return(nativeParts);
            if (receivedParts.Length != 0)
                ArrayPool<Message>.Shared.Return(receivedParts, clearArray: true);
        }
    }

    private static unsafe byte[]? CopyRoutingIdBytes(IntPtr routingIdPtr)
    {
        if (routingIdPtr == IntPtr.Zero)
            return null;

        return NativeHelpers.ReadRoutingId(ref *(ZlinkRoutingId*)routingIdPtr);
    }

    private static string DecodeTopic(byte[] topicBuffer, nuint topicLength)
    {
        var boundedLength = topicLength > (nuint)topicBuffer.Length
            ? topicBuffer.Length
            : (int)topicLength;
        return boundedLength == 0
            ? string.Empty
            : Encoding.UTF8.GetString(topicBuffer, 0, boundedLength);
    }
}
