// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

internal sealed class SocketSendOperation : SendOperation,
    SendSubmitOperation
{
    private readonly SocketBase _socket;
    private readonly RoutingId? _target;
    private OperationMessageBuffer _parts;
    private OperationSubmissionGuard _submission;

    internal SocketSendOperation(SocketBase socket, RoutingId? target = null)
    {
        _socket = socket;
        _target = target;
    }

    public SendSubmitOperation Message(Message message)
    {
        EnsureNotSubmitted();
        _parts.Add(message);
        return this;
    }

    public void Submit()
    {
        EnsureReady();
        _submission.MarkSubmittedAfterValidation();
        _socket.Kernel.Completion.Send(_target, _parts.Parts);
    }

    public Task Async(CancellationToken cancellationToken = default)
    {
        EnsureReady();
        _submission.MarkSubmittedAfterValidation();
        return _socket.Kernel.Completion.SendAsync(_target, _parts.Parts,
            cancellationToken);
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
    private readonly ReceivedSendContext _context;
    private OperationMessageBuffer _parts;
    private OperationSubmissionGuard _submission;

    internal ReceivedSendOperationImpl(ReceivedSendContext context)
    {
        _context = context;
    }

    public SendSubmitOperation Message(Message message)
    {
        EnsureNotSubmitted();
        _parts.Add(message);
        return this;
    }

    public void Submit()
    {
        EnsureReady();
        _submission.MarkSubmittedAfterValidation();
        _context.SendCore(_parts.Parts);
    }

    public Task Async(CancellationToken cancellationToken = default)
    {
        EnsureReady();
        _submission.MarkSubmittedAfterValidation();
        return _context.SendAsyncCore(_parts, cancellationToken);
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

internal sealed class PublisherTryPublishOperation : TryPublishOperation,
    TryPublishSubmitOperation
{
    private readonly PublisherSocketBase _socket;
    private readonly string _topic;
    private SendFlags _flags;
    private OperationMessageBuffer _parts;
    private OperationSubmissionGuard _submission;

    internal PublisherTryPublishOperation(PublisherSocketBase socket,
        string topic)
    {
        _socket = socket;
        _topic = topic;
    }

    public TryPublishSubmitOperation Message(Message message)
    {
        EnsureNotSubmitted();
        _parts.Add(message);
        return this;
    }

    public TryPublishSubmitOperation Flags(SendFlags flags)
    {
        EnsureNotSubmitted();
        _flags = flags;
        return this;
    }

    public bool Submit()
    {
        EnsureNotSubmitted();
        _parts.EnsureNotEmpty();
        _submission.MarkSubmittedAfterValidation();
        var flags = _flags | SendFlags.DontWait;
        return _parts.IsSingle
            ? _socket.PublishCore(_topic, _parts.Single, flags)
            : _socket.PublishCore(_topic, _parts.Parts, flags);
    }

    private void EnsureNotSubmitted()
    {
        _submission.EnsureNotSubmitted();
    }
}

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

    public PublishSubmitOperation Message(Message message)
    {
        EnsureNotSubmitted();
        _parts.Add(message);
        return this;
    }

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
        var flags = _flags | SendFlags.DontWait;
        var accepted = _parts.IsSingle
            ? _socket.PublishCore(_topic, _parts.Single, flags)
            : _socket.PublishCore(_topic, _parts.Parts, flags);
        if (!accepted)
            throw ZlinkException.CreateSubmitException((int)ErrorCode.EAgain);
    }

    private void EnsureNotSubmitted()
    {
        _submission.EnsureNotSubmitted();
    }
}
