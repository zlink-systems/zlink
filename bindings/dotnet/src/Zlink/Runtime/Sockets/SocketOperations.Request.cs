// SPDX-License-Identifier: MPL-2.0

namespace Systems.Zlink;

internal sealed class DealerRequestOperation : RequestOperation,
    RequestSubmitOperation, RequestCallbackSubmitOperation
{
    private readonly DealerSocket _socket;
    private bool _callbackStage;
    private SendFlags _flags;
    private OperationMessageBuffer _parts;
    private OperationSubmissionGuard _submission;
    private TimeSpan _timeout;

    internal DealerRequestOperation(DealerSocket socket)
    {
        _socket = socket;
    }

    RequestCallbackSubmitOperation RequestCallbackSubmitOperation.Message(
        Message message)
    {
        AddMessage(message);
        return this;
    }

    RequestCallbackSubmitOperation RequestCallbackSubmitOperation.Timeout(
        TimeSpan timeout)
    {
        EnsureNotSubmitted();
        _timeout = timeout;
        return this;
    }

    RequestCallbackSubmitOperation RequestCallbackSubmitOperation.Flags(
        SendFlags flags)
    {
        EnsureNotSubmitted();
        _flags = flags;
        return this;
    }

    public RequestSubmitOperation Message(Message message)
    {
        AddMessage(message);
        return this;
    }

    public RequestSubmitOperation Timeout(TimeSpan timeout)
    {
        EnsureNotSubmitted();
        _timeout = timeout;
        return this;
    }

    public RequestCallbackSubmitOperation Flags(SendFlags flags)
    {
        EnsureNotSubmitted();
        _callbackStage = true;
        _flags = flags;
        return this;
    }

    public Task<IReadOnlyList<Message>> Async(
        CancellationToken ct = default)
    {
        EnsureReady();
        if (_callbackStage)
            throw new ZlinkConfigException(
                ZlinkConfigException.ErrorCode.InvalidState);
        _submission.MarkSubmittedAfterValidation();
        return _socket.RequestCore(_parts.Parts, _timeout, ct);
    }

    public bool Submit(RequestCallback callback)
    {
        if (callback == null)
            throw new ArgumentNullException(nameof(callback));
        EnsureReady();
        _submission.MarkSubmittedAfterValidation();
        return _socket.RequestCallbackCore(_parts.Parts, callback, _flags,
            _timeout);
    }

    private void AddMessage(Message message)
    {
        EnsureNotSubmitted();
        _parts.Add(message);
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

internal abstract class RouterRequestOperation : RequestOperation,
    RequestSubmitOperation, RequestCallbackSubmitOperation
{
    private bool _callbackStage;
    private SendFlags _flags;
    private OperationMessageBuffer _parts;
    private OperationSubmissionGuard _submission;
    private TimeSpan _timeout;

    RequestCallbackSubmitOperation RequestCallbackSubmitOperation.Message(
        Message message)
    {
        AddMessage(message);
        return this;
    }

    RequestCallbackSubmitOperation RequestCallbackSubmitOperation.Timeout(
        TimeSpan timeout)
    {
        EnsureNotSubmitted();
        _timeout = timeout;
        return this;
    }

    RequestCallbackSubmitOperation RequestCallbackSubmitOperation.Flags(
        SendFlags flags)
    {
        EnsureNotSubmitted();
        _flags = flags;
        return this;
    }

    public RequestSubmitOperation Message(Message message)
    {
        AddMessage(message);
        return this;
    }

    public RequestSubmitOperation Timeout(TimeSpan timeout)
    {
        EnsureNotSubmitted();
        _timeout = timeout;
        return this;
    }

    public RequestCallbackSubmitOperation Flags(SendFlags flags)
    {
        EnsureNotSubmitted();
        _callbackStage = true;
        _flags = flags;
        return this;
    }

    public Task<IReadOnlyList<Message>> Async(
        CancellationToken ct = default)
    {
        EnsureReady();
        if (_callbackStage)
            throw new ZlinkConfigException(
                ZlinkConfigException.ErrorCode.InvalidState);
        _submission.MarkSubmittedAfterValidation();
        return AsyncCore(_parts.Parts, _timeout, ct);
    }

    public bool Submit(RequestCallback callback)
    {
        if (callback == null)
            throw new ArgumentNullException(nameof(callback));
        EnsureReady();
        _submission.MarkSubmittedAfterValidation();
        return SubmitCore(_parts.Parts, callback, _flags, _timeout);
    }

    private void AddMessage(Message message)
    {
        EnsureNotSubmitted();
        _parts.Add(message);
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

    protected abstract Task<IReadOnlyList<Message>> AsyncCore(
        IReadOnlyList<Message> parts,
        TimeSpan timeout,
        CancellationToken ct);

    protected abstract bool SubmitCore(
        IReadOnlyList<Message> parts,
        RequestCallback callback,
        SendFlags flags,
        TimeSpan timeout);
}

internal sealed class RouterPeerRequestOperation : RouterRequestOperation
{
    private readonly RoutingId _peerRid;
    private readonly RouterSocket _socket;

    internal RouterPeerRequestOperation(
        RouterSocket socket,
        RoutingId peerRid)
    {
        _socket = socket;
        _peerRid = peerRid;
    }

    protected override Task<IReadOnlyList<Message>> AsyncCore(
        IReadOnlyList<Message> parts,
        TimeSpan timeout,
        CancellationToken ct)
    {
        return _socket.RequestCore(_peerRid, parts, timeout, ct);
    }

    protected override bool SubmitCore(
        IReadOnlyList<Message> parts,
        RequestCallback callback,
        SendFlags flags,
        TimeSpan timeout)
    {
        return _socket.RequestCallbackCore(_peerRid, parts, callback, flags,
            timeout);
    }
}
