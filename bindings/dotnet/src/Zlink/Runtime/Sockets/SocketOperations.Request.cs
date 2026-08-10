// SPDX-License-Identifier: MPL-2.0

using System.Runtime.CompilerServices;

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

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public RequestSubmitOperation Message(Message message)
    {
        AddMessage(message);
        return this;
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public RequestSubmitOperation Timeout(TimeSpan timeout)
    {
        EnsureNotSubmitted();
        _timeout = timeout;
        return this;
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
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

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public bool Submit(RequestCallback callback)
    {
        if (callback == null)
            throw new ArgumentNullException(nameof(callback));
        EnsureReady();
        _submission.MarkSubmittedAfterValidation();
        // Keep single-part submission on the same ownership and callback path
        // without creating the temporary one-item list used by multi-part
        // submission.
        return _parts.IsSingle
            ? _socket.RequestCallbackCore(_parts.Single, callback, _flags,
                _timeout)
            : _socket.RequestCallbackCore(_parts.Parts, callback, _flags,
                _timeout);
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private void AddMessage(Message message)
    {
        EnsureNotSubmitted();
        _parts.Add(message);
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

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public RequestSubmitOperation Message(Message message)
    {
        AddMessage(message);
        return this;
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public RequestSubmitOperation Timeout(TimeSpan timeout)
    {
        EnsureNotSubmitted();
        _timeout = timeout;
        return this;
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
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

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public bool Submit(RequestCallback callback)
    {
        if (callback == null)
            throw new ArgumentNullException(nameof(callback));
        EnsureReady();
        _submission.MarkSubmittedAfterValidation();
        // Keep single-part submission on the same ownership and callback path
        // without creating the temporary one-item list used by multi-part
        // submission.
        return _parts.IsSingle
            ? SubmitSingleCore(_parts.Single, callback, _flags, _timeout)
            : SubmitCore(_parts.Parts, callback, _flags, _timeout);
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

    protected abstract bool SubmitSingleCore(
        Message part,
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

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    protected override Task<IReadOnlyList<Message>> AsyncCore(
        IReadOnlyList<Message> parts,
        TimeSpan timeout,
        CancellationToken ct)
    {
        return _socket.RequestCore(_peerRid, parts, timeout, ct);
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    protected override bool SubmitCore(
        IReadOnlyList<Message> parts,
        RequestCallback callback,
        SendFlags flags,
        TimeSpan timeout)
    {
        return _socket.RequestCallbackCore(_peerRid, parts, callback, flags,
            timeout);
    }

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    protected override bool SubmitSingleCore(
        Message part,
        RequestCallback callback,
        SendFlags flags,
        TimeSpan timeout)
    {
        return _socket.RequestCallbackCore(_peerRid, part, callback, flags,
            timeout);
    }
}
