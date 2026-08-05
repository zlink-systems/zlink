// SPDX-License-Identifier: MPL-2.0

using Systems.Zlink.Runtime.Sockets.Internal;

namespace Systems.Zlink;

// Continuation of Received; the type summary lives on the primary partial in
// Received.cs.
public sealed partial class Received : IDisposable
{
    internal void ResetForReuse()
    {
        if (_singlePart != null)
        {
            _singlePart.DisposeNativeOwned();
            _singlePart = null;
        }

        if (_parts != null)
        {
            _parts.Dispose();
            _parts = null;
        }

        _routingId = null;
        _routingIdSnapshot = default;
        _metadata = null;
        _sendSingleHandler = null;
        _sendHandler = null;
        _sendKernel = null;
        _sendRoutingIdSnapshot = default;
        MessageType = ReceivedMessageType.Raw;
        _closed = false;
    }

    internal void PopulateSinglePart(Message singlePart)
    {
        ResetForReuse();
        _singlePart = singlePart;
    }

    internal void PopulateMultipart(MultipartMessageCollection parts)
    {
        ResetForReuse();
        _parts = parts;
    }

    internal void PopulateMessageEnvelope(Message[] parts,
        ReceivedMessageType messageType, ulong? requestSeq,
        ReceivedReplyHandler? replyHandler = null)
    {
        ResetForReuse();
        _parts = MultipartMessageCollection.FromMessages(parts);
        MessageType = messageType;
        _metadata = ReceivedMetadata.Create(requestSeq, replyHandler);
    }

    internal void PopulateMessageEnvelopeSingle(Message singlePart,
        ReceivedMessageType messageType, ulong? requestSeq,
        ReceivedReplyHandler? replyHandler = null)
    {
        ResetForReuse();
        _singlePart = singlePart;
        MessageType = messageType;
        _metadata = ReceivedMetadata.Create(requestSeq, replyHandler);
    }

    internal void PopulateRoutedSinglePart(Message singlePart,
        RoutingIdSnapshot routingId, ulong? requestSeq,
        ReceivedReplyHandler? replyHandler,
        ReceivedSendHandler? sendHandler = null,
        ReceivedSendSingleHandler? sendSingleHandler = null,
        SocketKernel? sendKernel = null)
    {
        ResetForReuse();
        _singlePart = singlePart;
        _routingIdSnapshot = routingId;
        MessageType = requestSeq.HasValue || replyHandler is not null
            ? ReceivedMessageType.Request
            : ReceivedMessageType.Raw;
        _metadata = ReceivedMetadata.Create(requestSeq, replyHandler);
        _sendSingleHandler = sendSingleHandler;
        _sendHandler = sendHandler;
        SetSendContext(sendKernel, routingId);
    }

    internal void PopulateRoutedMultipart(MultipartMessageCollection parts,
        RoutingIdSnapshot routingId, ulong? requestSeq,
        ReceivedReplyHandler? replyHandler,
        ReceivedSendHandler? sendHandler = null,
        ReceivedSendSingleHandler? sendSingleHandler = null,
        SocketKernel? sendKernel = null)
    {
        ResetForReuse();
        _parts = parts;
        _routingIdSnapshot = routingId;
        MessageType = requestSeq.HasValue || replyHandler is not null
            ? ReceivedMessageType.Request
            : ReceivedMessageType.Raw;
        _metadata = ReceivedMetadata.Create(requestSeq, replyHandler);
        _sendSingleHandler = sendSingleHandler;
        _sendHandler = sendHandler;
        SetSendContext(sendKernel, routingId);
    }

    internal void SetSendHandler(ReceivedSendHandler? sendHandler,
        ReceivedSendSingleHandler? sendSingleHandler = null)
    {
        _sendKernel = null;
        _sendRoutingIdSnapshot = default;
        _sendSingleHandler = sendSingleHandler;
        _sendHandler = sendHandler;
    }

    internal void SetSendContext(SocketKernel? sendKernel,
        RoutingIdSnapshot routingId)
    {
        _sendKernel = sendKernel;
        _sendRoutingIdSnapshot = routingId;
    }
}
