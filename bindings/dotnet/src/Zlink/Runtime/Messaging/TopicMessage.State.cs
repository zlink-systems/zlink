// SPDX-License-Identifier: MPL-2.0

using System.Text;

namespace Systems.Zlink;

public sealed partial class TopicMessage
{
    private int _closed;
    private MultipartMessageCollection? _parts;
    private RoutingId? _routingId;
    private RoutingIdSnapshot _routingIdSnapshot;
    private Message? _reusableSinglePart;
    private Message? _singlePart;
    private string? _topic = string.Empty;
    private byte[]? _topicBytes;
    private int _topicLength;
    private byte[]? _topicWriteBuffer;

    internal TopicMessage(RoutingId? routingId, string topic, Message[] parts)
        : this(routingId, topic,
            MultipartMessageCollection.FromMessages(parts))
    {
    }

    internal TopicMessage(RoutingId? routingId, string topic,
        MultipartMessageCollection parts)
    {
        Populate(routingId, topic, parts);
    }

    internal TopicMessage(RoutingId? routingId, string topic,
        Message singlePart)
    {
        PopulateSinglePart(routingId, topic, singlePart);
    }

    internal TopicMessage(RoutingIdSnapshot routingId, string topic,
        MultipartMessageCollection parts)
    {
        Populate(routingId, topic, parts);
    }

    internal TopicMessage(RoutingIdSnapshot routingId, string topic,
        Message singlePart)
    {
        PopulateSinglePart(routingId, topic, singlePart);
    }

    private MultipartMessageCollection PartsCollection
    {
        get
        {
            if (_parts != null)
                return _parts;
            if (_singlePart == null)
                return _parts = MultipartMessageCollection.FromMessages(
                    Array.Empty<Message>());
            var part = _singlePart;
            _singlePart = null;
            return _parts = MultipartMessageCollection.FromSingle(part);
        }
    }

    internal void Populate(RoutingId? routingId, string topic,
        MultipartMessageCollection parts)
    {
        ResetForReuse();
        SetTopic(topic);
        PopulatePartsCore(routingId, default, parts);
    }

    internal void PopulateSinglePart(RoutingId? routingId, string topic,
        Message singlePart)
    {
        if (singlePart == null)
            throw new ArgumentNullException(nameof(singlePart));
        ResetForIncomingSinglePart(singlePart);
        SetTopic(topic);
        PopulateSinglePartCore(routingId, default, singlePart);
    }

    internal void Populate(RoutingIdSnapshot routingId, string topic,
        MultipartMessageCollection parts)
    {
        ResetForReuse();
        SetTopic(topic);
        PopulatePartsCore(null, routingId, parts);
    }

    internal void PopulateSinglePart(RoutingIdSnapshot routingId, string topic,
        Message singlePart)
    {
        if (singlePart == null)
            throw new ArgumentNullException(nameof(singlePart));
        ResetForIncomingSinglePart(singlePart);
        SetTopic(topic);
        PopulateSinglePartCore(null, routingId, singlePart);
    }

    internal void Populate(RoutingId? routingId, byte[] topicBuffer,
        int topicLength, MultipartMessageCollection parts)
    {
        ResetForReuse();
        CopyTopic(topicBuffer, topicLength);
        PopulatePartsCore(routingId, default, parts);
    }

    internal void PopulateSinglePart(RoutingId? routingId, byte[] topicBuffer,
        int topicLength, Message singlePart)
    {
        if (singlePart == null)
            throw new ArgumentNullException(nameof(singlePart));
        ResetForIncomingSinglePart(singlePart);
        CopyTopic(topicBuffer, topicLength);
        PopulateSinglePartCore(routingId, default, singlePart);
    }

    internal byte[] GetWritableTopicBuffer(int minimumLength)
    {
        EnsureOpen();
        if (minimumLength < 0)
            throw new ArgumentOutOfRangeException(nameof(minimumLength));
        var topicWriteBuffer = _topicWriteBuffer;
        if (topicWriteBuffer == null || topicWriteBuffer.Length < minimumLength)
        {
            topicWriteBuffer = new byte[minimumLength];
            _topicWriteBuffer = topicWriteBuffer;
        }

        return topicWriteBuffer;
    }

    internal void PrepareForSubscribe()
    {
        ResetForReuse(false);
    }

    internal void PopulateFromWritableTopicBuffer(RoutingId? routingId,
        int topicLength, MultipartMessageCollection parts)
    {
        ResetForReuse(false);
        SetTopicFromWritableBuffer(topicLength);
        PopulatePartsCore(routingId, default, parts);
    }

    internal void PopulateSinglePartFromWritableTopicBuffer(
        RoutingId? routingId, int topicLength, Message singlePart)
    {
        if (singlePart == null)
            throw new ArgumentNullException(nameof(singlePart));
        ResetForIncomingSinglePart(singlePart, resetTopic: false);
        SetTopicFromWritableBuffer(topicLength);
        PopulateSinglePartCore(routingId, default, singlePart);
    }

    internal void PopulateFromWritableTopicBuffer(
        RoutingIdSnapshot routingId, int topicLength,
        MultipartMessageCollection parts)
    {
        ResetForReuse(false);
        SetTopicFromWritableBuffer(topicLength);
        PopulatePartsCore(null, routingId, parts);
    }

    internal void PopulateSinglePartFromWritableTopicBuffer(
        RoutingIdSnapshot routingId, int topicLength, Message singlePart)
    {
        if (singlePart == null)
            throw new ArgumentNullException(nameof(singlePart));
        ResetForIncomingSinglePart(singlePart, resetTopic: false);
        SetTopicFromWritableBuffer(topicLength);
        PopulateSinglePartCore(null, routingId, singlePart);
    }

    internal Message PrepareReusableSinglePart()
    {
        EnsureOpen();
        var candidate = _reusableSinglePart;
        if (candidate == null)
        {
            // HOT PATH: the wrapper remains private until a receive succeeds.
            // Rent only wrappers returned by Message.Dispose so a retained
            // public Message can never be repurposed for another receive.
            candidate = Message.RentForNativeReceive();
            _reusableSinglePart = candidate;
        }
        candidate.PrepareForNativeReceive();
        return candidate;
    }

    private void PopulatePartsCore(RoutingId? routingId,
        RoutingIdSnapshot routingIdSnapshot, MultipartMessageCollection? parts)
    {
        _routingId = routingId;
        _routingIdSnapshot = routingIdSnapshot;
        _parts = parts ?? MultipartMessageCollection.FromMessages(Array.Empty<Message>());
    }

    private void PopulateSinglePartCore(RoutingId? routingId,
        RoutingIdSnapshot routingIdSnapshot, Message singlePart)
    {
        _routingId = routingId;
        _routingIdSnapshot = routingIdSnapshot;
        _singlePart = singlePart;
    }

    private void ResetForReuse(bool resetTopic = true, bool reopen = true)
    {
        if (reopen)
            EnsureOpen();

        var retainedSinglePart = reopen && _parts == null
            && _reusableSinglePart == null ? _singlePart : null;
        if (_parts != null)
            _parts.Dispose();
        else if (!ReferenceEquals(_singlePart, retainedSinglePart))
            _singlePart?.Dispose();
        _parts = null;
        _singlePart = null;
        if (retainedSinglePart != null)
            _reusableSinglePart = retainedSinglePart;
        if (!reopen)
        {
            _reusableSinglePart?.Dispose();
            _reusableSinglePart = null;
        }
        _routingId = null;
        _routingIdSnapshot = default;
        if (resetTopic)
        {
            _topic = string.Empty;
            _topicLength = 0;
        }

        if (reopen)
            Volatile.Write(ref _closed, 0);
    }

    private void ResetForIncomingSinglePart(Message singlePart,
        bool resetTopic = true)
    {
        EnsureOpen();
        var previousSinglePart = _singlePart;
        var candidate = _reusableSinglePart;
        if (_parts != null)
            _parts.Dispose();
        else if (!ReferenceEquals(singlePart, candidate)
                 && !ReferenceEquals(previousSinglePart, singlePart))
            previousSinglePart?.Dispose();
        _parts = null;
        _singlePart = null;

        if (ReferenceEquals(singlePart, candidate))
        {
            // The incoming wrapper was private until native receive succeeded.
            // Keep the prior public wrapper as the next private candidate;
            // callers may not retain it after this result is overwritten.
            _reusableSinglePart = previousSinglePart;
        }
        _routingId = null;
        _routingIdSnapshot = default;
        if (resetTopic)
        {
            _topic = string.Empty;
            _topicLength = 0;
        }
    }

    private void EnsureOpen()
    {
        if (Volatile.Read(ref _closed) != 0)
            throw new ObjectDisposedException(nameof(TopicMessage));
    }

    private void DisposeCore()
    {
        if (Interlocked.Exchange(ref _closed, 1) != 0)
            return;

        ResetForReuse(reopen: false);
        _topicBytes = null;
        _topicWriteBuffer = null;
    }

    private void SetTopic(string? topic)
    {
        _topic = topic ?? string.Empty;
        _topicLength = 0;
    }

    private void CopyTopic(byte[] topicBuffer, int topicLength)
    {
        if (topicLength <= 0)
        {
            _topic = string.Empty;
            _topicLength = 0;
            return;
        }

        if ((uint)topicLength > (uint)topicBuffer.Length)
            throw new ArgumentOutOfRangeException(nameof(topicLength));

        var topicBytes = _topicBytes != null
                         && _topicBytes.Length >= topicLength
            ? _topicBytes
            : new byte[topicLength];
        Buffer.BlockCopy(topicBuffer, 0, topicBytes, 0, topicLength);
        _topicBytes = topicBytes;
        _topicLength = topicLength;
        _topic = null;
    }

    private void SetTopicFromWritableBuffer(int topicLength)
    {
        if (topicLength <= 0)
        {
            _topic = string.Empty;
            _topicLength = 0;
            return;
        }

        var topicWriteBuffer = _topicWriteBuffer;
        if (topicWriteBuffer == null
            || (uint)topicLength > (uint)topicWriteBuffer.Length)
            throw new ArgumentOutOfRangeException(nameof(topicLength));

        // HOT PATH: Subscribe writes into the reusable alternate buffer. Keep
        // the decoded string when those bytes equal the previous topic; stable
        // subscriptions must not allocate the same string for every message.
        // Otherwise leave decoding lazy for payload-only callers. The reset step
        // preserves the previous topic long enough to compare before swapping,
        // avoiding a transient empty topic assignment on every receive.
        var decodedTopic = _topic;
        if (decodedTopic == null
            || _topicBytes == null
            || _topicLength != topicLength
            || !topicWriteBuffer.AsSpan(0, topicLength).SequenceEqual(
                _topicBytes.AsSpan(0, topicLength)))
            decodedTopic = null;

        _topicWriteBuffer = _topicBytes;
        _topicBytes = topicWriteBuffer;
        _topicLength = topicLength;
        _topic = decodedTopic;
    }

    private string DecodeTopicBytes()
    {
        return _topicBytes == null || _topicLength == 0
            ? string.Empty
            : Encoding.UTF8.GetString(_topicBytes, 0, _topicLength);
    }
}
