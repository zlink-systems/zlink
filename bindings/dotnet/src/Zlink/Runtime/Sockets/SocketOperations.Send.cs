// SPDX-License-Identifier: MPL-2.0

using System.Runtime.CompilerServices;

namespace Systems.Zlink;

internal sealed class MessageSocketSendOperation : RoutedSendOperation,
    RoutedSendSubmitOperation
{
    private readonly MessageSocketBase _socket;
    private OperationMessageBuffer _parts;
    private OperationSubmissionGuard _submission;

    internal MessageSocketSendOperation(MessageSocketBase socket)
    {
        _socket = socket;
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public RoutedSendSubmitOperation Message(Message message)
    {
        EnsureNotSubmitted();
        _parts.Add(message);
        return this;
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public void Submit(SendFlags flags)
    {
        EnsureReady();
        _submission.MarkSubmittedAfterValidation();
        if (_parts.IsSingle)
            _socket.Kernel.SendMessageUnchecked(_parts.Single, flags);
        else
            _socket.Kernel.Send(_parts.Parts, flags);
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public Task Async(CancellationToken ct = default)
    {
        EnsureReady();
        _submission.MarkSubmittedAfterValidation();
        return _parts.IsSingle
            ? _socket.Kernel.SendCompletion.SendSingleAsync(null,
                _parts.Single, ct)
            : _socket.Kernel.SendCompletion.SendAsync(null, _parts.Parts, ct);
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private void EnsureReady()
    {
        EnsureNotSubmitted();
        _parts.EnsureNotEmpty();
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private void EnsureNotSubmitted()
    {
        _submission.EnsureNotSubmitted();
    }
}

internal sealed class PublisherTrySendOperation : SendOperation,
    SendSubmitOperation
{
    private readonly PublisherSocketBase _socket;
    private readonly string _topic;
    private SendFlags _flags;
    private OperationMessageBuffer _parts;
    private OperationSubmissionGuard _submission;

    internal PublisherTrySendOperation(PublisherSocketBase socket,
        string topic)
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
        var flags = _flags | SendFlags.DontWait;
        return _parts.IsSingle
            ? _socket.PublishCore(_topic, _parts.Single, flags)
            : _socket.PublishCore(_topic, _parts.Parts, flags);
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private void EnsureReady()
    {
        EnsureNotSubmitted();
        _parts.EnsureNotEmpty();
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private void EnsureNotSubmitted()
    {
        _submission.EnsureNotSubmitted();
    }
}

/// <summary>
///     The synchronous PUB/XPUB publish terminal. Lossy PUB semantics mean the
///     publisher never waits at the high-water mark, so there is no pending
///     record, no admission queue and no asynchronous completion here.
/// </summary>
internal sealed class PublisherPublishOperation : PublishOperation,
    PublishSubmitOperation
{
    private readonly PublisherSocketBase _socket;
    private readonly string _topic;
    private SendFlags _flags;
    private OperationMessageBuffer _parts;
    private OperationSubmissionGuard _submission;

    internal PublisherPublishOperation(PublisherSocketBase socket,
        string topic)
    {
        _socket = socket;
        _topic = topic;
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public PublishSubmitOperation Message(Message message)
    {
        EnsureNotSubmitted();
        _parts.Add(message);
        return this;
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public PublishSubmitOperation Flags(SendFlags flags)
    {
        EnsureNotSubmitted();
        _flags = flags;
        return this;
    }

    public void Submit()
    {
        EnsureNotSubmitted();
        _parts.EnsureNotEmpty();
        _submission.MarkSubmittedAfterValidation();
        // A publisher never waits at the high-water mark: the default contract
        // drops the full subscriber's copy, and NODROP must surface the refusal
        // on the spot rather than parking on SNDTIMEO.
        var flags = _flags | SendFlags.DontWait;
        var accepted = _parts.IsSingle
            ? _socket.PublishCore(_topic, _parts.Single, flags)
            : _socket.PublishCore(_topic, _parts.Parts, flags);
        if (!accepted)
            throw ZlinkException.CreateSubmitException(
                (int)ErrorCode.EAgain);
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private void EnsureNotSubmitted()
    {
        _submission.EnsureNotSubmitted();
    }
}

internal sealed class StreamSendOperation : SendOperation,
    SendSubmitOperation
{
    private readonly RoutingId _routingId;
    private readonly RoutedMessageSocketBase _socket;
    private SendFlags _flags;
    private OperationMessageBuffer _parts;
    private OperationSubmissionGuard _submission;

    internal StreamSendOperation(RoutedMessageSocketBase socket,
        RoutingId routingId)
    {
        _socket = socket;
        _routingId = routingId;
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

internal sealed class RoutedAsyncSendOperation : RoutedSendOperation,
    RoutedSendSubmitOperation
{
    private readonly RoutingId? _routingId;
    private readonly SocketBase _socket;
    private OperationMessageBuffer _parts;
    private OperationSubmissionGuard _submission;

    internal RoutedAsyncSendOperation(DealerSocket socket)
    {
        _socket = socket;
    }

    internal RoutedAsyncSendOperation(RouterSocket socket,
        RoutingId routingId)
    {
        _socket = socket;
        _routingId = routingId;
    }

    internal RoutedAsyncSendOperation(StreamSocket socket,
        RoutingId routingId)
    {
        _socket = socket;
        _routingId = routingId;
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public RoutedSendSubmitOperation Message(Message message)
    {
        EnsureNotSubmitted();
        _parts.Add(message);
        return this;
    }

    public Task Async(CancellationToken ct = default)
    {
        EnsureNotSubmitted();
        _parts.EnsureNotEmpty();
        _submission.MarkSubmittedAfterValidation();
        if (_parts.IsSingle)
            return _socket.Kernel.SendCompletion.SendSingleAsync(
                _routingId, _parts.Single, ct);
        return _socket.Kernel.SendCompletion.SendAsync(_routingId,
            _parts.Parts, ct);
    }

    public void Submit(SendFlags flags)
    {
        EnsureNotSubmitted();
        _parts.EnsureNotEmpty();
        _submission.MarkSubmittedAfterValidation();
        if (_routingId.HasValue)
        {
            if (_parts.IsSingle)
                _socket.Kernel.Send(_routingId.Value, _parts.Single, flags);
            else
                _socket.Kernel.Send(_routingId.Value, _parts.Parts, flags);
            return;
        }

        if (_parts.IsSingle)
            _socket.Kernel.Send(_parts.Single, flags);
        else
            _socket.Kernel.Send(_parts.Parts, flags);
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private void EnsureNotSubmitted()
    {
        _submission.EnsureNotSubmitted();
    }
}

internal sealed class ReceivedSendOperationImpl : RoutedSendOperation,
    RoutedSendSubmitOperation
{
    private readonly ReceivedSendContext _context;
    private OperationMessageBuffer _parts;
    private OperationSubmissionGuard _submission;

    internal ReceivedSendOperationImpl(ReceivedSendContext context)
    {
        _context = context;
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public RoutedSendSubmitOperation Message(Message message)
    {
        EnsureNotSubmitted();
        _parts.Add(message);
        return this;
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public void Submit(SendFlags flags)
    {
        EnsureReady();
        _submission.MarkSubmittedAfterValidation();
        bool sent = _parts.IsSingle
            ? _context.SendCore(_parts.Single, flags)
            : _context.SendCore(_parts.Parts, flags);
        if (!sent)
            throw new ZlinkSubmitException(SubmitResult.Backpressured,
                (int)ErrorCode.EAgain);
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public Task Async(CancellationToken ct = default)
    {
        EnsureReady();
        _submission.MarkSubmittedAfterValidation();
        return _context.SendAsyncCore(_parts, ct);
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private void EnsureReady()
    {
        EnsureNotSubmitted();
        _parts.EnsureNotEmpty();
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private void EnsureNotSubmitted()
    {
        _submission.EnsureNotSubmitted();
    }
}
