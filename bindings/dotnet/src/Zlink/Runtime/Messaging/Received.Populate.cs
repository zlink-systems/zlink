// SPDX-License-Identifier: MPL-2.0

using Systems.Zlink.Runtime.Sockets.Internal;

namespace Systems.Zlink;

// Continuation of Received; the type summary lives on the primary partial in
// Received.cs.
public sealed partial class Received : IDisposable
{
    internal void PrepareForReceive()
    {
        ResetForReuse();
    }

    internal void ResetForReuse()
    {
        try
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
        }
        finally
        {
            _hwmBudgetLeases?.Dispose();
            _hwmBudgetLeases = null;
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

    internal void PopulateSinglePart(Message singlePart,
        HwmBudgetLeaseOwner? hwmBudgetLeases = null)
    {
        _singlePart = singlePart;
        _hwmBudgetLeases = hwmBudgetLeases;
    }

    internal void PopulateMultipart(MultipartMessageCollection parts,
        HwmBudgetLeaseOwner? hwmBudgetLeases = null)
    {
        _parts = parts;
        _hwmBudgetLeases = hwmBudgetLeases;
    }

    internal void PopulateMessageEnvelope(Message[] parts,
        ReceivedMessageType messageType, ulong? requestSeq,
        ReceivedReplyHandler? replyHandler = null,
        HwmBudgetLeaseOwner? hwmBudgetLeases = null)
    {
        _parts = MultipartMessageCollection.FromMessages(parts);
        MessageType = messageType;
        _metadata = ReceivedMetadata.Create(requestSeq, replyHandler);
        _hwmBudgetLeases = hwmBudgetLeases;
    }

    internal void PopulateMessageEnvelopeSingle(Message singlePart,
        ReceivedMessageType messageType, ulong? requestSeq,
        ReceivedReplyHandler? replyHandler = null,
        HwmBudgetLeaseOwner? hwmBudgetLeases = null)
    {
        _singlePart = singlePart;
        MessageType = messageType;
        _metadata = ReceivedMetadata.Create(requestSeq, replyHandler);
        _hwmBudgetLeases = hwmBudgetLeases;
    }

    internal void PopulateRoutedSinglePart(Message singlePart,
        RoutingIdSnapshot routingId, ulong? requestSeq,
        ReceivedReplyHandler? replyHandler,
        ReceivedSendHandler? sendHandler = null,
        ReceivedSendSingleHandler? sendSingleHandler = null,
        SocketKernel? sendKernel = null,
        HwmBudgetLeaseOwner? hwmBudgetLeases = null)
    {
        _singlePart = singlePart;
        _routingIdSnapshot = routingId;
        MessageType = requestSeq.HasValue || replyHandler is not null
            ? ReceivedMessageType.Request
            : ReceivedMessageType.Raw;
        _metadata = ReceivedMetadata.Create(requestSeq, replyHandler);
        _sendSingleHandler = sendSingleHandler;
        _sendHandler = sendHandler;
        SetSendContext(sendKernel, routingId);
        _hwmBudgetLeases = hwmBudgetLeases;
    }

    internal void PopulateRoutedMultipart(MultipartMessageCollection parts,
        RoutingIdSnapshot routingId, ulong? requestSeq,
        ReceivedReplyHandler? replyHandler,
        ReceivedSendHandler? sendHandler = null,
        ReceivedSendSingleHandler? sendSingleHandler = null,
        SocketKernel? sendKernel = null,
        HwmBudgetLeaseOwner? hwmBudgetLeases = null)
    {
        _parts = parts;
        _routingIdSnapshot = routingId;
        MessageType = requestSeq.HasValue || replyHandler is not null
            ? ReceivedMessageType.Request
            : ReceivedMessageType.Raw;
        _metadata = ReceivedMetadata.Create(requestSeq, replyHandler);
        _sendSingleHandler = sendSingleHandler;
        _sendHandler = sendHandler;
        SetSendContext(sendKernel, routingId);
        _hwmBudgetLeases = hwmBudgetLeases;
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
