// SPDX-License-Identifier: MPL-2.0

using Systems.Zlink.Runtime.Sockets.Internal;

namespace Systems.Zlink;

public sealed partial class Received
{
    private bool _closed;
    private ReceivedMetadata? _metadata;
    private MultipartMessageCollection? _parts;
    private RoutingId? _routingId;
    private RoutingIdSnapshot _routingIdSnapshot;
    private ReceivedSendHandler? _sendHandler;
    private SocketKernel? _sendKernel;
    private RoutingIdSnapshot _sendRoutingIdSnapshot;
    private ReceivedSendSingleHandler? _sendSingleHandler;
    private Message? _singlePart;

    private Received()
    {
    }

    internal Received(RoutingId? routingId, Message[] parts,
        ulong? requestSeq = null, ReceivedReplyHandler? replyHandler = null)
        : this(routingId, MultipartMessageCollection.FromMessages(parts),
            requestSeq, replyHandler)
    {
    }

    internal Received(RoutingId? routingId, MultipartMessageCollection parts,
        ulong? requestSeq = null, ReceivedReplyHandler? replyHandler = null)
    {
        _routingId = routingId;
        _metadata = ReceivedMetadata.Create(requestSeq, replyHandler);
        _parts = parts ?? MultipartMessageCollection.FromMessages(Array.Empty<Message>());
    }

    internal Received(byte[]? routingIdBytes, Message[] parts,
        ulong? requestSeq = null, ReceivedReplyHandler? replyHandler = null)
        : this(routingIdBytes, MultipartMessageCollection.FromMessages(parts),
            requestSeq, replyHandler)
    {
    }

    internal Received(byte[]? routingIdBytes, MultipartMessageCollection parts,
        ulong? requestSeq = null, ReceivedReplyHandler? replyHandler = null)
    {
        _routingIdSnapshot = RoutingIdSnapshot.FromBytes(routingIdBytes);
        _metadata = ReceivedMetadata.Create(requestSeq, replyHandler);
        _parts = parts ?? MultipartMessageCollection.FromMessages(Array.Empty<Message>());
    }

    internal Received(RoutingId? routingId, Message singlePart,
        ulong? requestSeq = null, ReceivedReplyHandler? replyHandler = null)
    {
        _routingId = routingId;
        _metadata = ReceivedMetadata.Create(requestSeq, replyHandler);
        _singlePart = singlePart ?? throw new ArgumentNullException(nameof(singlePart));
    }

    internal Received(byte[]? routingIdBytes, Message singlePart,
        ulong? requestSeq = null, ReceivedReplyHandler? replyHandler = null)
    {
        _routingIdSnapshot = RoutingIdSnapshot.FromBytes(routingIdBytes);
        _metadata = ReceivedMetadata.Create(requestSeq, replyHandler);
        _singlePart = singlePart ?? throw new ArgumentNullException(nameof(singlePart));
    }

    internal Received(RoutingIdSnapshot routingId,
        Message singlePart, ulong? requestSeq = null,
        ReceivedReplyHandler? replyHandler = null)
    {
        _routingIdSnapshot = routingId;
        _metadata = ReceivedMetadata.Create(requestSeq, replyHandler);
        _singlePart = singlePart ?? throw new ArgumentNullException(nameof(singlePart));
    }

    internal Received(RoutingIdSnapshot routingId,
        MultipartMessageCollection parts, ulong? requestSeq = null,
        ReceivedReplyHandler? replyHandler = null)
    {
        _routingIdSnapshot = routingId;
        _metadata = ReceivedMetadata.Create(requestSeq, replyHandler);
        _parts = parts ?? MultipartMessageCollection.FromMessages(Array.Empty<Message>());
    }

    internal int Count => _singlePart != null ? 1 : PartsCollection.Count;

    internal Message this[int index]
    {
        get
        {
            if (_singlePart != null)
            {
                if (index == 0)
                    return _singlePart;
                throw new ArgumentOutOfRangeException(nameof(index));
            }

            return PartsCollection[index];
        }
    }

    private MultipartMessageCollection PartsCollection
    {
        get
        {
            if (_parts != null)
                return _parts;
            if (_singlePart == null)
                return _parts = MultipartMessageCollection.FromMessages(Array.Empty<Message>());
            var part = _singlePart;
            _singlePart = null;
            return _parts = MultipartMessageCollection.FromSingle(part);
        }
    }

    private void DisposeCore()
    {
        if (_closed)
            return;
        _closed = true;
        if (_singlePart != null)
        {
            _singlePart.DisposeNativeOwned();
            _singlePart = null;
            return;
        }

        _parts?.Dispose();
    }

    internal IReadOnlyList<Message> TakePartsOwnership()
    {
        if (_singlePart != null)
        {
            var part = _singlePart;
            _singlePart = null;
            _closed = true;
            return new SingleMessageReadOnlyList(part);
        }

        return PartsCollection.TakeMessages();
    }

    internal IEnumerator<Message> GetEnumerator()
    {
        return PartsCollection.GetEnumerator();
    }

    private sealed class ReceivedMetadata
    {
        private ReceivedMetadata(ulong? requestSeq,
            ReceivedReplyHandler? replyHandler)
        {
            RequestSeq = requestSeq;
            ReplyHandler = replyHandler;
        }

        internal ulong? RequestSeq { get; }

        internal ReceivedReplyHandler? ReplyHandler { get; }

        internal static ReceivedMetadata? Create(ulong? requestSeq,
            ReceivedReplyHandler? replyHandler)
        {
            return requestSeq.HasValue || replyHandler != null
                ? new ReceivedMetadata(requestSeq, replyHandler)
                : null;
        }
    }

}
