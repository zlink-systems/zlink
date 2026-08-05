// SPDX-License-Identifier: MPL-2.0

using System.Runtime.CompilerServices;

namespace Systems.Zlink;

internal sealed class MessageSocketSendOperation : SendOperation,
    SendSubmitOperation
{
    private readonly MessageSocketBase _socket;
    private SendFlags _flags;
    private OperationMessageBuffer _parts;
    private OperationSubmissionGuard _submission;

    internal MessageSocketSendOperation(MessageSocketBase socket)
    {
        _socket = socket;
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public SendSubmitOperation Message(Message message)
    {
        EnsureNotSubmitted();
        _parts.Add(message);
        return this;
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public SendSubmitOperation Flags(SendFlags flags)
    {
        EnsureNotSubmitted();
        _flags = flags;
        return this;
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public bool Submit()
    {
        EnsureReady();
        _submission.MarkSubmittedAfterValidation();
        return _parts.IsSingle
            ? _socket.SendCore(_parts.Single, _flags)
            : _socket.SendCore(_parts.Parts, _flags);
    }

    private void EnsureReady()
    {
        EnsureNotSubmitted();
        _parts.EnsureNotEmpty();
    }

    private void EnsureNotSubmitted()
    {
        _submission.EnsureNotSubmitted();
    }
}

internal sealed class PublisherSendOperation : SendOperation, SendSubmitOperation
{
    private readonly PublisherSocketBase _socket;
    private readonly string _topic;
    private SendFlags _flags;
    private OperationMessageBuffer _parts;
    private OperationSubmissionGuard _submission;

    internal PublisherSendOperation(PublisherSocketBase socket, string topic)
    {
        _socket = socket;
        _topic = topic;
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public SendSubmitOperation Message(Message message)
    {
        EnsureNotSubmitted();
        _parts.Add(message);
        return this;
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public SendSubmitOperation Flags(SendFlags flags)
    {
        EnsureNotSubmitted();
        _flags = flags;
        return this;
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public bool Submit()
    {
        EnsureReady();
        _submission.MarkSubmittedAfterValidation();
        return _parts.IsSingle
            ? _socket.PublishCore(_topic, _parts.Single, _flags)
            : _socket.PublishCore(_topic, _parts.Parts, _flags);
    }

    private void EnsureReady()
    {
        EnsureNotSubmitted();
        _parts.EnsureNotEmpty();
    }

    private void EnsureNotSubmitted()
    {
        _submission.EnsureNotSubmitted();
    }
}

internal sealed class RoutedSendOperation : SendOperation, SendSubmitOperation
{
    private readonly RoutingId _routingId;
    private readonly RoutedMessageSocketBase _socket;
    private SendFlags _flags;
    private OperationMessageBuffer _parts;
    private OperationSubmissionGuard _submission;

    internal RoutedSendOperation(RoutedMessageSocketBase socket,
        RoutingId routingId)
    {
        _socket = socket;
        _routingId = routingId;
    }

    public SendSubmitOperation Message(Message message)
    {
        EnsureNotSubmitted();
        _parts.Add(message);
        return this;
    }

    public SendSubmitOperation Flags(SendFlags flags)
    {
        EnsureNotSubmitted();
        _flags = flags;
        return this;
    }

    public bool Submit()
    {
        EnsureReady();
        _submission.MarkSubmittedAfterValidation();
        return _parts.IsSingle
            ? _socket.SendRoutedCore(_routingId, _parts.Single, _flags)
            : _socket.SendRoutedCore(_routingId, _parts.Parts, _flags);
    }

    private void EnsureReady()
    {
        EnsureNotSubmitted();
        _parts.EnsureNotEmpty();
    }

    private void EnsureNotSubmitted()
    {
        _submission.EnsureNotSubmitted();
    }
}

internal sealed class StreamSendOperation : SendOperation, SendSubmitOperation
{
    private readonly RoutingId? _routingId;
    private readonly StreamSocket _socket;
    private SendFlags _flags;
    private OperationMessageBuffer _parts;
    private OperationSubmissionGuard _submission;

    internal StreamSendOperation(StreamSocket socket, RoutingId routingId)
    {
        _socket = socket;
        _routingId = routingId;
    }

    public SendSubmitOperation Message(Message message)
    {
        EnsureNotSubmitted();
        _parts.Add(message);
        return this;
    }

    public SendSubmitOperation Flags(SendFlags flags)
    {
        EnsureNotSubmitted();
        _flags = flags;
        return this;
    }

    public bool Submit()
    {
        EnsureReady();
        _submission.MarkSubmittedAfterValidation();
        return _parts.IsSingle
            ? _socket.SendRoutedCore(_routingId!.Value, _parts.Single, _flags)
            : _socket.SendRoutedCore(_routingId!.Value, _parts.Parts, _flags);
    }

    private void EnsureReady()
    {
        EnsureNotSubmitted();
        _parts.EnsureNotEmpty();
    }

    private void EnsureNotSubmitted()
    {
        _submission.EnsureNotSubmitted();
    }
}

internal sealed class ReceivedSendOperationImpl : SendOperation,
    SendSubmitOperation
{
    private readonly Received _received;
    private SendFlags _flags;
    private OperationMessageBuffer _parts;
    private OperationSubmissionGuard _submission;

    internal ReceivedSendOperationImpl(Received received)
    {
        _received = received;
    }

    public SendSubmitOperation Message(Message message)
    {
        EnsureNotSubmitted();
        _parts.Add(message);
        return this;
    }

    public SendSubmitOperation Flags(SendFlags flags)
    {
        EnsureNotSubmitted();
        _flags = flags;
        return this;
    }

    public bool Submit()
    {
        EnsureReady();
        _submission.MarkSubmittedAfterValidation();
        return _parts.IsSingle
            ? _received.SendCore(_parts.Single, _flags)
            : _received.SendCore(_parts.Parts, _flags);
    }

    private void EnsureReady()
    {
        EnsureNotSubmitted();
        _parts.EnsureNotEmpty();
    }

    private void EnsureNotSubmitted()
    {
        _submission.EnsureNotSubmitted();
    }
}