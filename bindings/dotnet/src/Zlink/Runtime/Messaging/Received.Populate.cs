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
        _replyToken = null;
        _sendKernel = null;
        _sendRoutingIdSnapshot = default;
        MessageType = ReceivedMessageType.Raw;
        _closed = false;
    }

    internal void PopulateSinglePart(Message singlePart)
    {
        _singlePart = singlePart;
    }

    internal void PopulateMultipart(MultipartMessageCollection parts)
    {
        _parts = parts;
    }

    internal void PopulateRoutedSinglePart(Message singlePart,
        RoutingIdSnapshot routingId, ReplyToken? replyToken,
        SocketKernel? sendKernel = null)
    {
        _singlePart = singlePart;
        _routingIdSnapshot = routingId;
        MessageType = replyToken is not null
            ? ReceivedMessageType.Request
            : ReceivedMessageType.Raw;
        _replyToken = replyToken;
        SetSendContext(sendKernel, routingId);
    }

    internal void PopulateRoutedMultipart(MultipartMessageCollection parts,
        RoutingIdSnapshot routingId, ReplyToken? replyToken,
        SocketKernel? sendKernel = null)
    {
        _parts = parts;
        _routingIdSnapshot = routingId;
        MessageType = replyToken is not null
            ? ReceivedMessageType.Request
            : ReceivedMessageType.Raw;
        _replyToken = replyToken;
        SetSendContext(sendKernel, routingId);
    }

    internal void SetSendContext(SocketKernel? sendKernel,
        RoutingIdSnapshot routingId)
    {
        _sendKernel = sendKernel;
        _sendRoutingIdSnapshot = routingId;
    }
}
